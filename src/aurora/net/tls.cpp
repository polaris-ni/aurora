// Aurora net — TLS 会话状态机。
//
// 本文件只做三件事：把 `detail::TlsBackend` 的字节推进接到 `StreamTransport` 的异步读写上、维护
// 「已解密待交付」与「待登记读请求」两条队列、保证每一条用户回调恰一次抵达（含取消、底层失败与
// 关闭这三种收尾）。加密本身一次都不在这里发生——那是后端的职责，见 `detail/tls_backend.h`。
//
// 锁纪律与 L1 同口径：会话状态由一枚互斥量串行化，**所有回调都转入 `Tasks` 后在锁外触发**。底层
// 流的完成回调可能就地转入本文件（L1 的就地完成路径），因此持锁期间绝不调用任何外部回调，也不在
// 持锁期间发起底层读写。
//
// 水位口径：本层不设第二道写水位。明文在 `write()` 就地封装成记录并交付底层流，此后待发字节数
// 与背压判定完全归 L1 的高水位（密文比明文大一个记录开销，因此本层的可发量只会更早收紧，不会更晚
// 越界）。读侧同理：只有在读请求等待时才从底层取字节，故待交付明文至多滞留一条记录。

#include "aurora/net/tls.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/detail/tls_backend.h"

namespace aurora::net {
namespace detail {

/// 会话阶段：握手进行中 / 可收发 / 已终止。
enum class TlsPhase : std::uint8_t {
    Handshaking,  ///< 握手未完成：底层流只归本状态机驱动
    Open,  ///< 已握手：应用记录可收发
    Closed,  ///< 已终止：不再有成功交付，未完成的请求都已收尾
};

/// 一枚排队的解密读请求（目标缓冲由调用方持有，须存活到对应回调抵达）。
struct TlsPendingRead {
    std::span<char> buffer;  ///< 调用方缓冲
    ReadDone on_done;  ///< 完成回调
};

/// 一次 TLS 会话的全部状态。
///
/// 后端与缓冲都由会话持有：`chunk` 是唯一的底层读缓冲（构造后不再改长度，因此在途读请求的 span
/// 始终有效），`plain` 是已解密但未交付的明文（只在有读请求等待时才增长）。
struct TlsState {
    std::mutex mutex;  ///< 会话唯一串行化点
    StreamTransport stream;  ///< 接管来的底层流
    std::unique_ptr<TlsBackend> backend;  ///< 平台后端
    Cancellation token;  ///< 本会话的取消令牌
    TlsPhase phase = TlsPhase::Handshaking;  ///< 当前阶段
    bool reading = false;  ///< 是否有一枚底层读在途
    bool peer_eof = false;  ///< 底层读到 0 字节（对端关闭写向）
    bool handshake_delivered = false;  ///< 握手回调是否已落地（恰一次判据）
    TlsHandshakeDone on_handshake;  ///< 握手完成通道（落地后转空）
    std::vector<char> chunk;  ///< 底层读缓冲（长度恒定）
    std::vector<char> plain;  ///< 已解密待交付明文
    std::deque<TlsPendingRead> reads;  ///< 待满足的读请求（按登记次序）
    std::optional<PeerCertificate> cert;  ///< 对端证书摘要（握手完成那一刻固化）
    std::optional<std::string> alpn;  ///< 协商出的 ALPN
    TlsContext context;  ///< 安全口径快照（pin 与危险开关在握手完成那一刻生效）
};

/// 实现侧工厂：唯一能以状态指针构造公共句柄的类型。
class TlsFactory {
  public:
    /// @brief 以既有会话状态装配公共句柄。
    /// @param state 已握手的会话状态。
    /// @return 持有该状态的 `TlsTransport`。
    [[nodiscard]] static auto from_state(std::shared_ptr<TlsState> state) -> TlsTransport {
        return TlsTransport{std::move(state)};
    }
};

}  // namespace detail

namespace {

using StatePtr = std::shared_ptr<detail::TlsState>;
using Tasks = std::vector<std::function<void()>>;

// 单条 TLS 记录的上限（16 KiB 正文）加上记录头与一次突发可能带来的余量：底层按自己的分块上限
// 投递，本层缓冲只需不在此处成为瓶颈。
constexpr std::size_t AURORA_TLS_CHUNK_BYTES = (std::size_t{16} * 1024) + 2048;

auto make_detail(ErrorCode code, std::string text) -> Error {
    return make_error(code, ErrorParams{{"detail", std::move(text)}});
}

auto cancelled_error() -> Error { return make_detail(ErrorCode::NetCancelled, "tls channel cancelled"); }

// 空句柄的失败形态：与 L1 的 `net-socket-failed` 同码同义（占位句柄没有可操作的连接）。
auto no_channel(std::string_view op) -> Error {
    return make_detail(ErrorCode::NetSocketFailed, "tls " + std::string{op} + " on an empty handle");
}

// 锁外触发全部待发动作（顺序即登记顺序）。
auto fire(Tasks &tasks) -> void {
    for (auto &task : tasks) {
        task();
    }
    tasks.clear();
}

// 前置声明：数据推进与读完成落点互相递归。
auto step_locked(const StatePtr &state, Tasks &tasks) -> void;
auto on_incoming(const StatePtr &state, Result<std::size_t> got) -> void;

// 把已解密的明文交付给排队中的读请求：只有「拿到字节」或「确定到此为止」才交付，绝不交付 0 字节
// 的空结果——0 字节在本层保留给 EOF 这一种含义（与 L1 一致）。
auto satisfy_reads_locked(detail::TlsState &state, Tasks &tasks) -> void {
    while (!state.reads.empty()) {
        auto &front = state.reads.front();
        const std::size_t delivered = std::min(front.buffer.size(), state.plain.size());
        const bool ended = state.peer_eof || state.phase == detail::TlsPhase::Closed;
        if (delivered == 0 && !ended) {
            return;  // 尚无字节可交付：等下一次解密或收尾
        }
        std::copy_n(state.plain.begin(), static_cast<std::ptrdiff_t>(delivered), front.buffer.data());
        state.plain.erase(state.plain.begin(), state.plain.begin() + static_cast<std::ptrdiff_t>(delivered));
        auto done = std::move(front.on_done);
        tasks.emplace_back([done, delivered]() { done(Result<std::size_t>{delivered}); });
        state.reads.pop_front();
    }
}

// 终止会话：未完成的读请求与握手回调一律以给定原因收尾一次，随后关闭底层流。
//
// `graceful_notify` 只在会话已握手时为真：先尽力发出 close_notify，再在它的完成回调里关流——反序
// 就等于从没发过，因为 `close()` 会丢弃尚未发出的待发字节。握手未完成时后端没有可加密记录的会话
// 状态，直接关流。
//
// 已交付握手后再来的错误只影响读队列（此时调用方手里已有句柄，握手回调不该也不能再响一次）。
auto terminate_locked(const StatePtr &state, Tasks &tasks, const Error &reason, bool graceful_notify) -> void {
    if (state->phase == detail::TlsPhase::Closed) {
        return;
    }
    std::optional<std::vector<char>> notify;
    if (graceful_notify && state->backend) {
        std::vector<char> records;
        if (auto result = state->backend->shutdown_records(records); result && !records.empty()) {
            notify = std::move(records);
        }
    }
    state->phase = detail::TlsPhase::Closed;
    auto reads = std::move(state->reads);
    state->plain.clear();
    for (auto &request : reads) {
        tasks.emplace_back([done = std::move(request.on_done), copy = reason]() { done(Result<std::size_t>{copy}); });
    }
    if (!state->handshake_delivered) {
        state->handshake_delivered = true;
        tasks.emplace_back(
            [done = std::move(state->on_handshake), copy = reason]() { done(Result<TlsTransport>{copy}); });
    }
    if (notify) {
        auto cipher = std::make_shared<std::vector<char>>(std::move(*notify));
        tasks.emplace_back([state, cipher]() {
            state->stream.write(std::span<const char>{*cipher}, [state](const Result<std::size_t> &sent) -> void {
                (void)sent;  // close_notify 的发出不可能有结论（对端已不再读），忽略即本体的设计
                state->stream.close();
            });
        });
    } else {
        tasks.emplace_back([state]() { state->stream.close(); });
    }
}

// 在底层流上登记一次读（同一时刻至多一枚在途：握手阶段本就串行，数据阶段由 `reading` 把关）。
auto arm_read_locked(const StatePtr &state, Tasks &tasks) -> void {
    if (state->reading || state->peer_eof || state->phase == detail::TlsPhase::Closed) {
        return;
    }
    state->reading = true;
    tasks.emplace_back([state]() {
        state->stream.read(std::span<char>{state->chunk.data(), state->chunk.size()},
                           [state](Result<std::size_t> got) -> void { on_incoming(state, std::move(got)); });
    });
}

// 握手完成那一刻的取证与加严判定：证书摘要、ALPN、SPKI pin（pin 判定不受危险开关影响）。
auto complete_handshake_locked(const StatePtr &state, Tasks &tasks) -> void {
    auto cert = state->backend->peer_certificate();
    if (!cert) {
        terminate_locked(state, tasks, cert.error(), false);
        return;
    }
    const auto &pinned = state->context.pinned_public_keys;
    if (!pinned.empty() && std::find(pinned.begin(), pinned.end(), cert.value().public_key_sha256) == pinned.end()) {
        terminate_locked(
            state, tasks,
            make_detail(ErrorCode::NetTlsPinMismatch,
                        "peer public key pin " + cert.value().public_key_sha256 + " is not in the configured pin set"),
            false);
        return;
    }
    state->cert = std::move(cert.value());
    state->alpn = state->backend->negotiated_alpn();
    state->phase = detail::TlsPhase::Open;
    state->handshake_delivered = true;
    // 句柄按 move-only 交付，因此装进共享指针过 `Tasks` 这一道拷贝要求的坎（回调恰一次，取出即空态）。
    auto channel = std::make_shared<TlsTransport>(detail::TlsFactory::from_state(state));
    tasks.emplace_back([done = std::move(state->on_handshake), channel]() mutable {
        done(Result<TlsTransport>{std::move(*channel)});
    });
}

// 推进会话直到「需要更多字节」或「出结果」为止；全程持锁，产出的动作经 `tasks` 在锁外执行。
auto step_locked(const StatePtr &state, Tasks &tasks) -> void {
    while (state->phase != detail::TlsPhase::Closed) {
        if (state->phase == detail::TlsPhase::Handshaking) {
            std::vector<char> outgoing;
            auto outcome = state->backend->handshake(outgoing);
            if (!outcome) {
                terminate_locked(state, tasks, outcome.error(), false);
                return;
            }
            if (!outgoing.empty()) {
                auto cipher = std::make_shared<std::vector<char>>(std::move(outgoing));
                tasks.emplace_back([state, cipher]() {
                    state->stream.write(std::span<const char>{*cipher},
                                        [state](const Result<std::size_t> &sent) -> void {
                                            Tasks more;
                                            {
                                                std::scoped_lock lock{state->mutex};
                                                if (state->phase != detail::TlsPhase::Closed) {
                                                    if (sent) {
                                                        step_locked(state, more);
                                                    } else {
                                                        terminate_locked(state, more, sent.error(), false);
                                                    }
                                                }
                                            }
                                            fire(more);
                                        });
                });
                return;
            }
            if (outcome.value()) {
                // 证书摘要、pin 判定在此生效；握手成功后后端缓冲可能已有明文字节（TLS 1.3 的会话票据
                // 即此形态），故不 return 而是继续走数据阶段。
                complete_handshake_locked(state, tasks);
                continue;
            }
            if (state->peer_eof) {
                terminate_locked(state, tasks,
                                 make_detail(ErrorCode::NetTlsHandshakeFailed, "peer closed the stream mid-handshake"),
                                 false);
                return;
            }
            arm_read_locked(state, tasks);
            return;
        }

        if (state->reads.empty()) {
            return;  // 没人等字节：不主动取，也不解密
        }
        std::vector<char> batch;
        std::vector<char> outgoing;
        auto records = state->backend->next_records(batch, outgoing);
        if (!records) {
            terminate_locked(state, tasks, records.error(), false);
            return;
        }
        state->plain.insert(state->plain.end(), batch.begin(), batch.end());
        if (records.value() == detail::TlsDataState::PeerClosed) {
            state->peer_eof = true;
        }
        satisfy_reads_locked(*state, tasks);
        if (!outgoing.empty()) {
            // 后端在数据阶段产出了握手字节（系统要求完成一轮重协商）：先外发，回来再继续推进。
            auto cipher = std::make_shared<std::vector<char>>(std::move(outgoing));
            tasks.emplace_back([state, cipher]() {
                state->stream.write(std::span<const char>{*cipher}, [state](const Result<std::size_t> &sent) -> void {
                    Tasks more;
                    {
                        std::scoped_lock lock{state->mutex};
                        if (state->phase != detail::TlsPhase::Closed) {
                            if (sent) {
                                step_locked(state, more);
                            } else {
                                terminate_locked(state, more, sent.error(), false);
                            }
                        }
                    }
                    fire(more);
                });
            });
            return;
        }
        if (records.value() == detail::TlsDataState::NeedMore) {
            arm_read_locked(state, tasks);
            return;
        }
    }
}

// 底层读完成的落点：字节交给后端缓冲，再推进会话。
//
// 收尾形态只有一条：所有分支都必须在锁作用域**内**只登记动作、在锁作用域**外**交付。分支里就地
// `return` 会跳过 `fire`，被丢弃的正是「读必须有结局」这批回调——现场表现为挂起的读永远不落地。
auto on_incoming(const StatePtr &state, Result<std::size_t> got) -> void {
    Tasks tasks;
    {
        std::scoped_lock lock{state->mutex};
        state->reading = false;
        if (state->phase == detail::TlsPhase::Closed) {
            return;
        }
        if (!got) {
            terminate_locked(state, tasks, got.error(), false);
        } else if (const auto count = got.value(); count == 0) {
            state->peer_eof = true;
            if (state->phase == detail::TlsPhase::Handshaking) {
                // 握手中途的对端消失不是「读到结尾」，而是会话从未成立：口径与超时/取消同形。
                terminate_locked(state, tasks,
                                 make_detail(ErrorCode::NetTlsHandshakeFailed, "peer closed the stream mid-handshake"),
                                 false);
            } else {
                satisfy_reads_locked(*state, tasks);
            }
        } else {
            state->backend->push_incoming(std::span<const char>{state->chunk.data(), count});
            step_locked(state, tasks);
        }
    }
    fire(tasks);
}

// 取消令牌的落点：以 `net-cancelled` 终止会话。
auto on_cancelled(const std::weak_ptr<detail::TlsState> &weaker) -> void {
    auto state = weaker.lock();
    if (!state) {
        return;
    }
    Tasks tasks;
    {
        std::scoped_lock lock{state->mutex};
        terminate_locked(state, tasks, cancelled_error(), false);
    }
    fire(tasks);
}

// 握手入参里本层就能判定的部分：目标名形态与版本界。
//
// 为什么要在这里就拦 IP 字面量：证书的主机名校验按 RFC 6125 只对 DNS 名有意义，静默跳过等于给
// 「连到 IP 就不校验名字」留了一条没人审查的后门。
auto validate_handshake_inputs(std::string_view server_name, const TlsContext &context) -> std::optional<Error> {
    if (server_name.empty()) {
        return make_detail(ErrorCode::NetEndpointInvalid, "tls handshake needs a non-empty server name");
    }
    if (Endpoint::from_ip(server_name, std::uint16_t{1})) {
        return make_detail(ErrorCode::NetEndpointInvalid,
                           "tls handshake needs a DNS name, not an IP literal: " + std::string{server_name});
    }
    if (context.max_version && *context.max_version < context.min_version) {
        return make_detail(ErrorCode::NetTlsVersionMismatch, "TlsContext upper version bound is below its lower bound");
    }
    // 客户端身份的两种形态各说各话：PKCS#12 是一整包（证书 + 私钥 + 可选链），PEM 是拆开的一对。同时置值
    // 就是「以谁为准」没有答案——猜一边等于悄悄丢掉调用方给的另一份材料，故当场拒。
    const bool has_pkcs12 = context.client_identity.has_value();
    const bool has_pem = context.client_certificate_pem.has_value() || context.client_private_key_pem.has_value();
    if (has_pkcs12 && has_pem) {
        return make_detail(ErrorCode::NetOptionConflict,
                           "client identity is given both as PKCS#12 and as a PEM pair: choose one form");
    }
    // PEM 只给一半时同样没有可发送的身份：只有证书则拿不出签名，只有私钥则对端无从知道这是谁的钥匙。
    // 报成「配置冲突」而不是让后端在握手半途报一句库内部的解析失败，是把错说在还没连线的时候。
    if (has_pem && !(context.client_certificate_pem.has_value() && context.client_private_key_pem.has_value())) {
        return make_detail(ErrorCode::NetOptionConflict,
                           "a PEM client identity needs both client_certificate_pem and client_private_key_pem");
    }
    return std::nullopt;
}

}  // namespace

auto TlsTransport::handshake(StreamTransport &&stream, std::string_view server_name, const TlsContext &context,
                             const Cancellation &cancellation, TlsHandshakeDone on_done) -> void {
    if (!on_done) {
        return;
    }
    auto state = std::make_shared<detail::TlsState>();
    state->stream = std::move(stream);
    state->on_handshake = std::move(on_done);
    state->chunk.resize(AURORA_TLS_CHUNK_BYTES);
    state->context = context;

    Tasks tasks;
    if (const auto invalid = validate_handshake_inputs(server_name, context); invalid) {
        terminate_locked(state, tasks, *invalid, false);
        fire(tasks);
        return;
    }
    if (context.allow_insecure_certificate) {
        // 危险开关一用就留痕：诊断是这一档唯一的现场证据，事后无法从连接本身还原「当初为什么没校验」。
        AURORA_LOG_WARN("tls", "certificate verification disabled for ", server_name,
                        " (allow_insecure_certificate: local testing only)");
    }

    auto backend = detail::make_tls_backend(context, server_name);
    if (!backend) {
        terminate_locked(state, tasks, backend.error(), false);
        fire(tasks);
        return;
    }
    state->backend = std::move(backend.value());

    // 取消链条：外部令牌触发会话令牌，会话令牌的钩子负责收尾（钩子在调用线程同步执行）。
    // 登记时若已取消则就地执行钩子——那正是「先取消后发起」这条竞态窗口的唯一收口。
    const std::weak_ptr<detail::TlsState> weaker{state};
    state->token.on_cancel([weaker]() { on_cancelled(weaker); });
    auto source = cancellation;  // 令牌是可共享的状态句柄，拷贝即加入同一状态（入参是 const 引用）
    source.on_cancel([token = state->token]() mutable { token.cancel(); });

    {
        std::scoped_lock lock{state->mutex};
        step_locked(state, tasks);
    }
    fire(tasks);
}

auto TlsTransport::peer_certificate() const -> Result<PeerCertificate> {
    // 已关闭即无摘要可交付：口径见 `net/tls.h`（「空态或已关闭为 `net-socket-failed`」）。摘要虽然在
    // 握手那一刻固化，但通道关掉后再让它读出一张「当前可信的对端证书」会诱导调用方拿它去复用连接。
    if (!is_open() || !state_->cert) {
        return no_channel("peer_certificate");
    }
    return *state_->cert;
}

auto TlsTransport::negotiated_alpn() const -> Result<std::string> {
    if (!state_) {
        return no_channel("negotiated_alpn");
    }
    if (!state_->alpn) {
        return make_detail(ErrorCode::NetOptionConflict, "no application protocol was negotiated");
    }
    return *state_->alpn;
}

auto TlsTransport::read(std::span<char> buffer, ReadDone on_done) -> void {
    if (!on_done) {
        return;
    }
    if (!state_) {
        on_done(Result<std::size_t>{no_channel("read")});
        return;
    }
    Tasks tasks;
    {
        std::scoped_lock lock{state_->mutex};
        if (state_->phase == detail::TlsPhase::Closed) {
            auto copy = make_detail(ErrorCode::NetSocketFailed, "tls channel is closed");
            tasks.emplace_back([done = std::move(on_done), copy]() { done(Result<std::size_t>{copy}); });
        } else if (buffer.empty()) {
            auto copy = make_detail(ErrorCode::NetBufferLimit, "read buffer is empty");
            tasks.emplace_back([done = std::move(on_done), copy]() { done(Result<std::size_t>{copy}); });
        } else {
            state_->reads.push_back(detail::TlsPendingRead{.buffer = buffer, .on_done = std::move(on_done)});
            step_locked(state_, tasks);
        }
    }
    fire(tasks);
}

auto TlsTransport::write(std::span<const char> data, WriteDone on_done) -> void {
    if (!on_done) {
        return;
    }
    if (!state_) {
        on_done(Result<std::size_t>{no_channel("write")});
        return;
    }
    if (data.empty()) {
        on_done(Result<std::size_t>{std::size_t{0}});  // 空写不产生任何记录与系统调用
        return;
    }
    const auto plaintext = data.size();
    Tasks tasks;
    {
        std::scoped_lock lock{state_->mutex};
        if (state_->phase != detail::TlsPhase::Open) {
            auto copy = make_detail(ErrorCode::NetSocketFailed, "tls channel is not open");
            tasks.emplace_back([done = std::move(on_done), copy]() { done(Result<std::size_t>{copy}); });
        } else {
            std::vector<char> cipher;
            if (auto wrapped = state_->backend->protect(data, cipher); !wrapped) {
                tasks.emplace_back(
                    [done = std::move(on_done), copy = wrapped.error()]() { done(Result<std::size_t>{copy}); });
            } else {
                auto batch = std::make_shared<std::vector<char>>(std::move(cipher));
                tasks.emplace_back([state = state_, batch, plaintext, done = std::move(on_done)]() {
                    state->stream.write(
                        std::span<const char>{*batch}, [done, plaintext](const Result<std::size_t> &sent) -> void {
                            if (sent) {
                                done(Result<std::size_t>{plaintext});  // 整批记录交付成功即整段明文已发出
                                return;
                            }
                            done(Result<std::size_t>{sent.error()});
                        });
                });
            }
        }
    }
    fire(tasks);
}

auto TlsTransport::close() -> void {
    if (!state_) {
        return;
    }
    Tasks tasks;
    {
        std::scoped_lock lock{state_->mutex};
        terminate_locked(state_, tasks, cancelled_error(), state_->phase == detail::TlsPhase::Open);
    }
    fire(tasks);
}

auto TlsTransport::pause_read() -> void {
    if (state_) {
        state_->stream.pause_read();
    }
}

auto TlsTransport::resume_read() -> void {
    if (!state_) {
        return;
    }
    // 先解底层流的暂停再推进会话：顺序反过来就是死锁形态——本层催出的那次读投递会被仍在生效的
    // 暂停挡回来，而挡回来的请求只有恢复后才有可能被满足，恢复却排在推进之后。
    state_->stream.resume_read();
    Tasks tasks;
    {
        std::scoped_lock lock{state_->mutex};
        step_locked(state_, tasks);
    }
    fire(tasks);
}

TlsTransport::TlsTransport(TlsTransport &&other) noexcept : state_(std::move(other.state_)) {}

// 移动赋值与析构都要先关掉自有通道：`close()` 的收尾链里有一次 Error 构造与若干转入 `Tasks` 的
// std::function 构造，理论上能 bad_alloc。这两个函数不能按建议加 noexcept 了事（抛出即 terminate），
// 也不能因抛出而跳过关停——句柄泄漏比栈展开更糟，析构期传出异常同样是 terminate，故按库侧析构关停
// 的既有口径逐点豁免（同 `net/transport.cpp` 的 `StreamTransport`）。
// NOLINTBEGIN(bugprone-exception-escape)
auto TlsTransport::operator=(TlsTransport &&other) noexcept -> TlsTransport & {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}

TlsTransport::~TlsTransport() { close(); }
// NOLINTEND(bugprone-exception-escape)

auto TlsTransport::is_open() const -> bool {
    return state_ && state_->phase == detail::TlsPhase::Open && state_->stream.is_open();
}

auto TlsTransport::read_paused() const -> bool { return state_ && state_->stream.read_paused(); }

auto TlsTransport::pending_write_bytes() const -> std::size_t {
    return state_ ? state_->stream.pending_write_bytes() : std::size_t{0};
}

auto TlsTransport::cancellation() const -> Cancellation { return state_ ? state_->token : Cancellation{}; }

}  // namespace aurora::net
