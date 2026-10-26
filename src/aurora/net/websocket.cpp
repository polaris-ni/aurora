// websocket.cpp —— 升级之后会话的编排实现（落码口径见 `detail/ws_session.h` 与公共头 `net/websocket.h`）。
//
// 本文件只做三件事：把 `WebSocket` 句柄的四个成员转进会话、把会话的三条环（读 / 写 / 心跳）跑成协程、
// 把分帧器交回的四种事实翻译成宿主的 sink 调用与收尾。协议判定本身在 `detail/ws_frame.cpp`，握手判定在
// `detail/ws_handshake.cpp`，发起侧那条腿在 `http_client.cpp`——这里不重复任何一处判据。
#include "aurora/net/websocket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/result.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/net/detail/ws_frame.h"
#include "aurora/net/detail/ws_session.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::detail {
namespace {

/// @brief 读环的单次读缓冲大小：与 HTTP 发起侧的 `AURORA_READ_CHUNK` 同值（同一份「一段不必大」的判断）。
constexpr std::size_t AURORA_WS_READ_CHUNK = std::size_t{16} * 1024;

/// @brief 一帧的线上字节数：载荷 + 两字节基头 + 最小档长度前缀 + 四字节掩码密钥。
///
/// 队列闸要的是「这条消息会占掉多少待发字节」，而那必须在**编帧之前**问得出——先编一帧再量长度等于为了
/// 判断一次越限而分配出 4 MiB。前缀档位与 `write_ws_frame()` 的取最小档规则逐字对应（口径见 `ws_frame.h`
/// 第三条），两处若分岔，闸就会放行一帧写不出去或拒掉一帧写得出去的字节数。
/// @param payload_bytes 载荷字节数。
/// @return 该帧的线上字节数。
[[nodiscard]] auto wire_frame_bytes(std::size_t payload_bytes) -> std::size_t {
    const std::size_t prefix = payload_bytes < 126U ? 0U : (payload_bytes <= 65535U ? 2U : 8U);
    return payload_bytes + 2U + prefix + AURORA_WS_MASK_KEY_BYTES;
}

/// @brief 一条消息折成几枚分片：空载荷也算一枚（那是一枚零长帧，协议上是条完整而空的消息）。
/// @param payload_bytes 载荷字节数。
/// @param frame_limit 单帧上限（非空载荷时由调用方保证非零）。
/// @return 分片枚数。
[[nodiscard]] auto frame_count(std::size_t payload_bytes, std::size_t frame_limit) -> std::size_t {
    if (payload_bytes == 0U || frame_limit == 0U) {
        return 1U;
    }
    return (payload_bytes + frame_limit - 1U) / frame_limit;
}

/// @brief 一条消息折成分片后的线上总字节数（闸用；不分配任何字节）。
/// @param payload_bytes 载荷字节数。
/// @param frames 分片枚数（`frame_count()` 的产出）。
/// @param frame_limit 单帧上限。
/// @return 总线上字节数（各片「载荷 + 帧头 + 掩码密钥」之和）。
[[nodiscard]] auto wire_message_bytes(std::size_t payload_bytes, std::size_t frames, std::size_t frame_limit)
    -> std::size_t {
    if (frames <= 1U) {
        return wire_frame_bytes(payload_bytes);
    }
    // 前 `frames - 1` 片恒等于上限，最后一片装剩下的；逐片相加而不是乘一个平均头长，是因为长度前缀会
    // 随片长换档，那 6 字节的差额正是「闸放行了却写不下」的来源。
    const std::size_t tail = payload_bytes - ((frames - 1U) * frame_limit);
    return ((frames - 1U) * wire_frame_bytes(frame_limit)) + wire_frame_bytes(tail);
}

/// @brief 会话级失败的文本形态（`ws-closed` 的 `{detail}` 槽）。
/// @param text ASCII 文本。
/// @return 待交出的错误。
[[nodiscard]] auto closed_error(std::string_view text) -> Error {
    return make_error(ErrorCode::WsClosed, ErrorParams{{"detail", std::string{text}}});
}

/// @brief 协议违例的文本形态（`ws-protocol-error` 的 `{detail}` 槽）。
/// @param text ASCII 文本。
/// @return 待交出的错误。
[[nodiscard]] auto protocol_error(std::string_view text) -> Error {
    return make_error(ErrorCode::WsProtocolError, ErrorParams{{"detail", std::string{text}}});
}

/// @brief 载荷越上限 / 队列越上限那两型 detail 的公共拼装。
/// @param label 被越过的旋钮名（ASCII，进 `{detail}` 槽）。
/// @param wanted 本次需要的量。
/// @param limit 旋钮当前值。
/// @return 待交出的错误。
[[nodiscard]] auto limit_error(std::string_view label, std::size_t wanted, std::size_t limit) -> Error {
    return make_error(ErrorCode::WsSendQueueFull,
                      ErrorParams{{"detail", std::string{label} + " needs " + std::to_string(wanted) + " while " +
                                                 std::to_string(limit) + " is the configured limit"}});
}

}  // namespace

auto WsSession::report(const std::shared_ptr<SendSettlement> &settlement, Result<void> outcome) -> void {
    if (settlement == nullptr || settlement->reported) {
        return;
    }
    // 闸在结算对象上而不在会话里：一条消息的若干分片共用同一枚 `shared_ptr`，谁先到达谁报，其余一律静默。
    settlement->reported = true;
    if (settlement->on_sent) {
        settlement->on_sent(std::move(outcome));
    }
}

WsSession::WsSession(std::shared_ptr<StreamTransport> plain, std::shared_ptr<TlsTransport> secure, WebSocketSink &sink,
                     WebSocketOptions options, std::string subprotocol, std::string leftover)
    : plain_(std::move(plain)), secure_(std::move(secure)), sink_(sink), options_(std::move(options)),
      subprotocol_(std::move(subprotocol)),
      // `make_sinks(this)` 在成员初始化表里就叫：四个出口只由 `reader_.push()` 触发，而那是构造完成之后的事；
      // 此刻 `options_` 与 `sink_` 已按声明次序就位，闭包读到的不是半成品。`leftover_` 排在 `reader_` 之后是
      // 声明次序使然，它只在读环起跑时才有人读，构造期内无人问津。
      reader_(make_sinks(this), WsFrameLimits{.max_frame_bytes = options_.max_frame_bytes,
                                              .max_message_bytes = options_.max_message_bytes}),
      leftover_(std::move(leftover)) {}

WsSession::~WsSession() = default;

auto WsSession::start(const std::shared_ptr<WsSession> &self) -> void {
    if (already_started_) {
        return;  // 二次起跑会起两条读环：同一份重组缓冲被两处喂，帧序立刻不可信
    }
    already_started_ = true;
    // 两条环都是 fire-and-forget：`CoroTask` 的 promise 不首挂也不终挂，帧在挂起期间自活、收尾时自行销毁。
    // 交出去的那份 `shared_ptr` 就住在帧的参数里——这正是「句柄销毁了而会话还得活着把 close 发完」的落点。
    (void)read_loop(self);
    if (options_.heartbeat_interval.has_value() && options_.heartbeat_interval->count() > 0) {
        (void)heartbeat_loop(self);
    }
}

auto WsSession::send(WsMessageKind kind, std::string_view payload, WsSent on_sent) -> Result<void> {
    if (settled_) {
        return Result<void>{closed_error("the session has already closed")};
    }
    if (closing_) {
        return Result<void>{closed_error("this side already started the close handshake")};
    }
    if (kind == WsMessageKind::Text) {
        // 发出侧的 UTF-8 闸与读侧共用 `first_invalid_utf8()`：把对端一定会判成违例的字节写出去，等于亲手
        // 在一条已经建立的会话上踩一次刹车。
        if (const auto bad = first_invalid_utf8(payload); bad.has_value()) {
            return Result<void>{protocol_error("text payload is not valid UTF-8 at byte " + std::to_string(*bad))};
        }
    }
    if (payload.size() > options_.max_message_bytes) {
        return Result<void>{make_error(
            ErrorCode::NetBufferLimit,
            ErrorParams{{"detail", "message of " + std::to_string(payload.size()) + " bytes exceeds the configured " +
                                       std::to_string(options_.max_message_bytes) + " byte message limit"}})};
    }
    if (options_.max_frame_bytes == 0U && !payload.empty()) {
        // 单帧上限被收到 0 时任何非空载荷都无处安放。不静默放宽、也不退化成「一帧装下」——那会让旋钮说谎。
        return Result<void>{
            make_error(ErrorCode::NetBufferLimit,
                       ErrorParams{{"detail", "the frame limit is 0, so no non-empty payload can be framed"}})};
    }
    const std::size_t frames = frame_count(payload.size(), options_.max_frame_bytes);
    if (frames + queued_frames() > options_.max_queued_frames) {
        return Result<void>{
            limit_error("the send queue frame slots", frames + queued_frames(), options_.max_queued_frames)};
    }
    const std::size_t needed = wire_message_bytes(payload.size(), frames, options_.max_frame_bytes);
    if (needed + queued_bytes_ > options_.max_queued_bytes) {
        return Result<void>{limit_error("the send queue bytes", needed + queued_bytes_, options_.max_queued_bytes)};
    }
    const auto settlement = std::make_shared<SendSettlement>();
    settlement->on_sent = std::move(on_sent);
    settlement->frames_left = frames;

    const std::size_t first_index = queue_.size();
    std::size_t offset = 0;
    for (std::size_t index = 0; index < frames; ++index) {
        const std::size_t size = options_.max_frame_bytes == 0U
                                     ? payload.size()
                                     : std::min<std::size_t>(options_.max_frame_bytes, payload.size() - offset);
        const std::string_view piece = payload.substr(offset, size);
        const auto opcode =
            index == 0U ? (kind == WsMessageKind::Text ? WsOpcode::Text : WsOpcode::Binary) : WsOpcode::Continuation;
        const auto queued = queue_frame(opcode, index + 1U == frames, piece, settlement);
        if (!queued) {
            // 编帧失败（取不到熵）：本条消息已入队的分片当场撤下——留下半条消息等于让对端等一条永不收尾的
            // 分片序列，那比这一次发送失败严重得多。
            for (std::size_t retract = queue_.size(); retract > first_index; --retract) {
                queued_bytes_ -= queue_.back().bytes.size();
                queue_.pop_back();
            }
            return Result<void>{queued.error()};
        }
        offset += size;
    }
    kick_writer();
    return Result<void>{};
}

auto WsSession::close_wire(std::uint16_t code, std::string_view reason) -> Result<void> {
    if (settled_) {
        return Result<void>{closed_error("the session has already closed")};
    }
    if (closing_) {
        return Result<void>{closed_error("this side already started the close handshake")};
    }
    if (!is_close_code_on_wire(code)) {
        return Result<void>{protocol_error("close code " + std::to_string(code) + " cannot be carried on the wire")};
    }
    if (const auto bad = first_invalid_utf8(reason); bad.has_value()) {
        return Result<void>{protocol_error("close reason is not valid UTF-8 at byte " + std::to_string(*bad))};
    }
    if (reason.size() + 2U > AURORA_WS_MAX_CONTROL_BYTES) {
        return Result<void>{
            make_error(ErrorCode::WsProtocolError,
                       ErrorParams{{"detail", "close code plus reason is " + std::to_string(reason.size() + 2U) +
                                                  " bytes, above the 125 byte control frame limit"}})};
    }
    closing_ = true;
    const auto queued = queue_frame(WsOpcode::Close, true, close_frame_payload(code, reason), nullptr);
    if (!queued) {
        return Result<void>{queued.error()};
    }
    kick_writer();
    return Result<void>{};
}

auto WsSession::detach() -> void {
    if (detached_) {
        return;  // 幂等：移动赋值与重复销毁都只是空操作
    }
    detached_ = true;
    if (settled_) {
        abort_link();  // 收尾事实已经报过，这里只归还连接
        return;
    }
    if (!closing_) {
        closing_ = true;
        const auto queued = queue_frame(WsOpcode::Close, true, close_frame_payload(1000U, {}), nullptr);
        if (!queued) {
            abort_link();  // 连尽力投递都编不出来（取不到熵）：只掐，不假装道别过
            return;
        }
    }
    kick_writer();
    if (!writing_) {
        abort_link();  // 起不了写环（已定局）：没有「队列写空之后」可等
    }
}

auto WsSession::queue_frame(WsOpcode opcode, bool final, std::string_view payload,
                            std::shared_ptr<SendSettlement> settlement) -> Result<void> {
    std::string bytes;
    const auto written = write_ws_frame(bytes, opcode, final, payload);
    if (!written) {
        return Result<void>{written.error()};  // 失败即一字未动：调用方因此能按最简单的形状回滚
    }
    queued_bytes_ += bytes.size();
    queue_.push_back(QueuedFrame{.bytes = std::move(bytes), .settlement = std::move(settlement)});
    return Result<void>{};
}

auto WsSession::kick_writer() -> void {
    if (writing_ || settled_) {
        return;
    }
    // 检查与置位之间没有挂起点（`write_loop` 只在函数体内才 co_await），故「入队后必有人写」不存竞态窗口。
    writing_ = true;
    (void)write_loop(shared_from_this());
}

auto WsSession::abort_link() const -> void {
    if (plain_ != nullptr) {
        plain_->close();  // 幂等；在途读写各以 `net-cancelled` 收尾一次
    }
    if (secure_ != nullptr) {
        secure_->close();
    }
}

auto WsSession::settle(std::optional<Error> failure, std::optional<std::uint16_t> code, std::string reason,
                       bool by_peer) -> void {
    if (settled_) {
        return;  // 唯一终态出口：二次定局不重复报事实，也不重复掐连接
    }
    // 看守：`abort_link()` 让在途读写当场以失败收尾，而没装主投递器时那条恢复是**就地执行**的——于是本函数
    // 的后半段可能踩在一条已经把所有 `shared_ptr` 释放完的会话对象上。这份持有让它活到返回。
    const auto keeper = shared_from_this();
    settled_ = true;
    closing_ = true;
    pending_close_.reset();
    const auto queued_failure =
        failure.has_value() ? *failure : closed_error("the session closed before this message was written");
    for (auto &frame : queue_) {
        report(frame.settlement, Result<void>{queued_failure});
    }
    queue_.clear();
    queued_bytes_ = 0;
    abort_link();
    lifecycle_.cancel();  // 心跳环的在途等待据此当场放行，不必等定时器到点
    if (detached_) {
        return;  // 句柄已销毁：那个 sink 不再属于任何能被观察到的宿主
    }
    WsCloseInfo info;
    info.code = code;
    info.reason = std::move(reason);
    info.by_peer = by_peer;
    if (failure.has_value()) {
        info.failure = std::move(*failure);
    }
    sink_.on_closed(info);
}

auto WsSession::make_sinks(WsSession *self) -> WsFrameSinks {
    return WsFrameSinks{
        .on_message = [self](const WsParsedMessage &message) -> bool {
            if (self->detached_) {
                return false;  // 句柄销毁即停止派发；分帧与协议判定照走（见本文件头第二条）
            }
            const auto kind = message.opcode == WsOpcode::Text ? WsMessageKind::Text : WsMessageKind::Binary;
            return self->sink_.on_message(kind, std::string_view{message.payload}) == http::SinkFlow::Continue;
        },
        .on_ping =
            [self](std::string_view payload) {
                if (!self->detached_) {
                    self->sink_.on_ping(payload);
                }
                if (self->settled_ || self->detached_) {
                    return;  // 链路已在收摊：回不回 pong 都不再是宿主能观察的事实
                }
                // 恒自动回同载荷 pong：不回 ping 是协议违例，把这种违例交给宿主决定，等于让一次界面卡顿掐断
                // 一条连接（口径见公共头文件头第六条）。
                const auto echoed = self->queue_frame(WsOpcode::Pong, true, payload, nullptr);
                if (echoed) {
                    self->kick_writer();
                } else {
                    self->settle(echoed.error(), std::nullopt, {}, false);
                }
            },
        .on_pong =
            [self](std::string_view payload) {
                ++self->pongs_seen_;  // 心跳环醒来时问的就是这枚计数
                if (!self->detached_) {
                    self->sink_.on_pong(payload);
                }
            },
        .on_close =
            [self](const WsParsedClose &close) {
                if (!self->closing_) {
                    self->closing_ = true;
                    // 回声带上对端那枚码（对端没带才用 1000）：应答端镜像状态码是 `RFC 6455 §7.4.1` 的建议，
                    // 而对端看到的「我方如何理解这次收摊」正是这一枚。
                    const auto echoed = self->queue_frame(WsOpcode::Close, true,
                                                          close_frame_payload(close.code.value_or(1000U), {}), nullptr);
                    if (echoed) {
                        self->queue_.back().terminates = true;
                        // 定局押到回声写完那一刻：现在就 `settle()` 会连带把这枚 close 与内核缓冲一起丢掉，
                        // 对端只能把这次收摊记成异常断连、拿不到任何状态码（口径见本文件头第六条）。
                        self->pending_close_ =
                            WsCloseInfo{.code = close.code, .reason = close.reason, .by_peer = true, .failure = {}};
                        self->kick_writer();
                        return;
                    }
                    self->settle(echoed.error(), close.code, close.reason, true);
                    return;
                }
                // 本端已先发起关闭（`close()` 或析构）：对端这枚是回帧，收下事实即定局，不必再回一枚。
                // `by_peer` 交假：这次收摊是本端开启的，对端只是应答（公共头把该旗位定义为「是否由对端的 close
                // 帧开启」，把它写成真会让宿主把自己发起的收摊读成对端踢的）。
                self->settle(std::nullopt, close.code, close.reason, false);
            },
    };
}

auto WsSession::read_loop([[maybe_unused]] std::shared_ptr<WsSession> self) -> CoroTask<void> {
    // 明文 / TLS 两条流经捕获 shared_ptr 的类型擦除闭包收敛到同一枚 awaiter 型（口径同 `open_stream` 的
    // 「建连并统一读写口」）：闭包持有的是与 `plain_` / `secure_` 同一份连接状态，随本帧存活。
    std::function<CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
    if (secure_ != nullptr) {
        read_one = [holder = secure_](std::span<char> buffer) { return await_read(*holder, buffer); };
    } else {
        read_one = [holder = plain_](std::span<char> buffer) { return await_read(*holder, buffer); };
    }
    // 握手那次读多带回来的字节属于帧流（一次读不会刚好停在 `101\r\n\r\n` 的边界上）：先喂分帧器再开始在途
    // 读，否则那半条消息凭空消失。
    if (!leftover_.empty()) {
        auto seeded = reader_.push(leftover_);
        leftover_.clear();
        if (!seeded) {
            settle(seeded.error(), std::nullopt, {}, false);
            co_return;
        }
    }
    std::array<char, AURORA_WS_READ_CHUNK> chunk{};
    for (;;) {
        if (settled_ || pending_close_.has_value()) {
            co_return;  // 已定局，或已记下对端的 close、只等回声写出
        }
        auto got = co_await read_one(std::span<char>{chunk});
        if (!got) {
            // 传输失败（含本端 `abort_link()` 带来的 `net-cancelled`）：那条失败就是收尾事实。已交给宿主的
            // 消息不回滚——宿主看见过的字节由宿主处置（口径同 `open_stream` 的取消）。
            if (pending_close_.has_value()) {
                auto info = std::move(*pending_close_);
                settle(std::nullopt, info.code, std::move(info.reason), info.by_peer);
            } else {
                settle(got.error(), std::nullopt, {}, false);
            }
            co_return;
        }
        if (got.value() == 0U) {
            // 0 字节 = 对端关闭写向（不是错误）。它没说 close 就收摊，是本协议眼里的异常断连。
            const bool half_message = reader_.message_in_progress();
            (void)reader_.finish();
            if (pending_close_.has_value()) {
                auto info = std::move(*pending_close_);
                settle(std::nullopt, info.code, std::move(info.reason), info.by_peer);
                co_return;
            }
            settle(
                closed_error(half_message ? std::string_view{"the peer closed the connection without a close "
                                                             "frame, while a message was still being reassembled"}
                                          : std::string_view{"the peer closed the connection without a close frame"}),
                std::nullopt, {}, false);
            co_return;
        }
        auto fed = reader_.push(std::string_view{chunk.data(), got.value()});
        if (!fed) {
            settle(fed.error(), std::nullopt, {}, false);
            co_return;
        }
        // 喂入成功时 `on_close` 也可能已记下定局事实；下一轮循环开头据此退出。
    }
}

auto WsSession::write_loop([[maybe_unused]] std::shared_ptr<WsSession> self) -> CoroTask<void> {
    std::function<CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
    if (secure_ != nullptr) {
        write_one = [holder = secure_](std::span<const char> data) { return await_write(*holder, data); };
    } else {
        write_one = [holder = plain_](std::span<const char> data) { return await_write(*holder, data); };
    }
    for (;;) {
        if (settled_) {
            writing_ = false;  // `settle()` 已把队列连同落地回报一并冲掉，这里只交回「写环不在跑」
            co_return;
        }
        if (queue_.empty()) {
            writing_ = false;
            if (detached_) {
                abort_link();  // 尽力投递的那枚 close 已写出（或已写不出去）：现在才掐
            }
            co_return;
        }
        auto frame = std::move(queue_.front());
        queue_.erase(queue_.begin());
        queued_bytes_ -= frame.bytes.size();
        const bool terminating = frame.terminates;
        auto written = co_await write_one(std::span<const char>{frame.bytes});
        if (!written) {
            report(frame.settlement, Result<void>{written.error()});  // 在途那条拿到的是真实的写出失败
            if (pending_close_.has_value()) {
                auto info = std::move(*pending_close_);
                // 回声发不出去也不改口径：这次收摊由对端的 close 开启，写失败只是它的后果。
                settle(std::nullopt, info.code, std::move(info.reason), info.by_peer);
            } else {
                settle(written.error(), std::nullopt, {}, false);
            }
            writing_ = false;
            co_return;
        }
        if (frame.settlement != nullptr && frame.settlement->frames_left > 0U) {
            --frame.settlement->frames_left;
        }
        if (frame.settlement != nullptr && frame.settlement->frames_left == 0U) {
            report(frame.settlement, Result<void>{});  // 整条消息的最后一片落地，这才算「已写出」
        }
        if (terminating && pending_close_.has_value()) {
            auto info = std::move(*pending_close_);
            settle(std::nullopt, info.code, std::move(info.reason), info.by_peer);
            writing_ = false;
            co_return;
        }
    }
}

auto WsSession::heartbeat_loop([[maybe_unused]] std::shared_ptr<WsSession> self) -> CoroTask<void> {
    const auto interval = options_.heartbeat_interval.value_or(std::chrono::milliseconds{0});
    const auto pong_window = options_.pong_timeout.value_or(std::chrono::milliseconds{0});
    for (;;) {
        co_await await_sleep(interval, lifecycle_);
        if (settled_ || detached_) {
            co_return;
        }
        if (closing_) {
            co_return;  // 关闭交换已在路上，再发 ping 只会让对端多等一枚它并不需要的帧
        }
        const std::size_t seen_before = pongs_seen_;
        const auto queued = queue_frame(WsOpcode::Ping, true, {}, nullptr);
        if (!queued) {
            settle(queued.error(), std::nullopt, {}, false);
            co_return;
        }
        ++pings_sent_;
        kick_writer();
        if (pong_window.count() <= 0) {
            continue;  // 没给 pong 窗口 = 发 ping 只为让中间设备看到流量，不据它判失联
        }
        co_await await_sleep(pong_window, lifecycle_);
        if (settled_ || detached_) {
            co_return;
        }
        if (pongs_seen_ == seen_before) {
            // 本层不读任何时钟：到点与否由引擎定时器的次序给出，醒来发现这枚 ping 没有应答即失联。
            settle(closed_error("no pong arrived within the configured window after a ping"), std::nullopt, {}, false);
            co_return;
        }
    }
}

}  // namespace aurora::net::detail

namespace aurora::net {
namespace {

/// @brief 空句柄的统一失败：句柄不持会话时四个成员都交这一枚，而不是崩溃或未定义行为。
/// @return 待交出的错误。
[[nodiscard]] auto empty_handle_error() -> Error {
    return make_error(ErrorCode::WsClosed,
                      ErrorParams{{"detail", "this handle holds no session: the connection was never established"}});
}

}  // namespace

WebSocket::WebSocket(WebSocket &&other) noexcept : session_(std::move(other.session_)) {}

// 抛出面只有 `detach()` 一条：编那枚尽力投递的 close 帧要分配，取不到熵时还要造错误对象，除 bad_alloc 外无其
// 他来源。本函数标了 `noexcept`、析构函数隐式 `noexcept`，越出去就是 `terminate`；不 try/catch 是有意的——
// 把收摊失败吞掉，宿主看到的是一条「已经道别过」的连接而实际上没有（处置口径同
// `src/aurora/inspector/inspector_server.cpp` 的析构豁免）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto WebSocket::operator=(WebSocket &&other) noexcept -> WebSocket & {
    if (this != &other) {
        if (session_ != nullptr) {
            session_->detach();  // 先按析构语义收摊自己那份，再接管对方的
        }
        session_ = std::move(other.session_);
    }
    return *this;
}

// 同移动赋值：唯一的抛点是 `detach()` 里那枚 close 帧的分配，析构期没有可回报的通道，吞掉即撒谎。
// NOLINTNEXTLINE(bugprone-exception-escape)
WebSocket::~WebSocket() {
    if (session_ != nullptr) {
        session_->detach();
        session_.reset();
    }
}

auto WebSocket::is_open() const -> bool { return session_ != nullptr && session_->is_open(); }

auto WebSocket::subprotocol() const -> std::string_view {
    return session_ != nullptr ? session_->subprotocol() : std::string_view{};
}

auto WebSocket::queued_bytes() const -> std::size_t { return session_ != nullptr ? session_->queued_bytes() : 0U; }

auto WebSocket::send(std::string_view text, WsSent on_sent) const -> Result<void> {
    if (session_ == nullptr) {
        return Result<void>{empty_handle_error()};
    }
    return session_->send(WsMessageKind::Text, text, std::move(on_sent));
}

auto WebSocket::send_binary(std::span<const char> binary, WsSent on_sent) const -> Result<void> {
    if (session_ == nullptr) {
        return Result<void>{empty_handle_error()};
    }
    return session_->send(WsMessageKind::Binary, std::string_view{binary.data(), binary.size()}, std::move(on_sent));
}

auto WebSocket::close(std::uint16_t code, std::string_view reason) const -> Result<void> {
    if (session_ == nullptr) {
        return Result<void>{empty_handle_error()};
    }
    return session_->close_wire(code, reason);
}

}  // namespace aurora::net
