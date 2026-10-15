// http_client.cpp —— HTTP 发起侧客户端的 send / open_stream 核心闭环（CHG-010 §6 / §12）。
//
// 落在这里的是「把回调式传输按 HTTP 线上次序编排成一条协程」：连接（http 直连 / https 先握手）、
// 写出 render_head() + 体、逐段喂接收侧装配器 ResponseAssembler 装配应答、按 Content-Encoding 还原消息体、
// 按 Location 跟随重定向、并施加整请求时限。协程出口的成败一律以 Result 表达，绝不抛异常穿过协程帧
// （§18-4）。编码链的文法与逐层解压不在本文件——那是 `detail/http_encoding.h` 的纯逻辑，本件只负责调用它，
// 并决定宣告什么、限到什么字节。
//
// 两条交付路（物化 `send` / 流式 `open_stream`）共用同一套编排骨架，故结构相似而**刻意不做抽象**：协程体
// 里的 `co_await` 无法从普通函数里发出，把「建连 + 写出」抽成辅助协程要么按值搬动那些类型擦除闭包、要么让
// 每一跳多背一帧，两种都比这几十行的重复更容易出错。四条真差异（体不物化、头部即判重定向、线上不设限、
// deflate 按包装头判定）全部落在各自看得见的地方，读一条即能对照另一条。
#include "aurora/net/http_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/net/detail/http_encoding.h"
#include "aurora/net/detail/http_progress.h"
#include "aurora/net/detail/http_wire.h"
#include "aurora/net/engine.h"
#include "aurora/net/http.h"
#include "aurora/net/tls.h"
#include "aurora/net/transport.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::http {
namespace {

/// @brief 单段读取的投喂缓冲字节数：足够让常见的头部 + 首块一次到位，又不无界占用栈帧。
constexpr std::size_t AURORA_READ_CHUNK = std::size_t{16} * 1024;

/// @brief 挂了进度回调时请求体的分块写出粒度：每块一次完成回调，上传进度与写侧背压都以块为节拍。
///
/// 取 64 KiB 是「一次系统调用值得发的量」与「进度不碎成每 16 KiB 一跳」之间的折中；它只影响本库内部的
/// 完成次数，线上字节序列与不分块时逐字节相同，故对端无从分辨。
constexpr std::size_t AURORA_WRITE_CHUNK = std::size_t{64} * 1024;

/// @brief 三档超时的档号：写进 `TimeoutState::fired`，收尾据此拼 `http-timeout` 的 detail。
enum class TimeoutStage : std::uint8_t {
    None,
    Connect,
    ReadIdle,
    Total,
};

/// @brief 三档看门狗共享的触发状态：第一个到点的档写 `fired`，其余档见其非 `None` 即不再触发。
///
/// 用 `shared_ptr` 让看门狗与引擎 `post_after` 闭包共享——闭包在 send 结束后仍可能到期，共享所有权保证
/// 那块内存活到闭包跑完，帧销毁后写它不再是悬垂访问。
struct TimeoutState {
    /// @brief 首先到点的档号（`None` = 尚未触发）。
    std::atomic<int> fired{static_cast<int>(TimeoutStage::None)};
};

/// @brief 一档超时的看门狗：构造登记引擎定时器，到点且本段仍在计时则记 `fired` 并取消令牌；析构即停表。
///
/// 引擎 `post_after` 无撤销口，故「停表」靠把共享的 `live` 置假——到期的闭包见 `live` 为假即 no-op，
/// 不会误伤该段结束之后的操作。`connect` 在拿到可用连接后停表、`read_idle` 每读到一次字节即重置。
class StageTimer {
  public:
    /// @brief 登记一档看门狗。
    /// @param window 该档时限；`nullopt` 或非正值即不登记（本段不设限）。
    /// @param stage 到点写入 `state->fired` 的档号。
    /// @param token 到点取消的令牌（在途操作以 net-cancelled 收尾）。
    /// @param state 三档共享的触发状态。
    StageTimer(std::optional<std::chrono::milliseconds> window, TimeoutStage stage, Cancellation token,
               const std::shared_ptr<TimeoutState> &state) {
        if (!window.has_value() || window->count() <= 0) {
            return;
        }
        live_ = std::make_shared<std::atomic<bool>>(true);
        // 引擎投递闭包经 std::function 类型擦除转入，命中本仓记录在案的「不应抛出」系统性假告警面；
        // 到期体只做原子写与取消，不抛。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        (void)Engine::instance().post_after(*window, [live = live_, stage, token, state]() mutable -> void {
            if (live->load() && state->fired.load() == static_cast<int>(TimeoutStage::None)) {
                state->fired.store(static_cast<int>(stage));
                token.cancel();
            }
        });
    }

    StageTimer(const StageTimer &) = delete;
    auto operator=(const StageTimer &) -> StageTimer & = delete;
    StageTimer(StageTimer &&) = delete;
    auto operator=(StageTimer &&) -> StageTimer & = delete;
    ~StageTimer() { stop(); }

    /// @brief 停表：令尚未到期的定时器闭包到点时 no-op（幂等）。
    auto stop() -> void {
        if (live_ != nullptr) {
            live_->store(false);
        }
    }

  private:
    std::shared_ptr<std::atomic<bool>> live_;  ///< 本段是否仍在计时；空 = 未登记（该档不设限）
};

/// @brief 该状态码是否为需要跟随的重定向。
[[nodiscard]] auto is_redirect(std::uint16_t status) -> bool {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/// @brief 按方法取对应工厂产出一枚指向 `url` 的请求（重定向换目标时复用）。
[[nodiscard]] auto make_request(Method method, std::string_view url) -> Request {
    switch (method) {
        case Method::Get:
            return Request::get(url);
        case Method::Head:
            return Request::head(url);
        case Method::Post:
            return Request::post(url);
        case Method::Put:
            return Request::put(url);
        case Method::Patch:
            return Request::patch(url);
        case Method::Delete:
            return Request::delete_(url);
    }
    return Request::get(url);
}

/// @brief 依 `Location` 与状态码算出重定向的下一跳请求（方法/体的改写按 RFC 9110 §15.4 惯例）。
[[nodiscard]] auto follow(const Request &current, const Response &response) -> Result<Request> {
    const auto location = response.header("Location");
    if (!location.has_value()) {
        return Result<Request>{make_error(ErrorCode::HttpProtocolError, "redirect response carried no Location")};
    }
    const auto &base = current.url();
    if (!base.has_value()) {
        return Result<Request>{
            make_error(ErrorCode::HttpProtocolError, "redirect from a target with no resolvable base")};
    }
    auto next = base->resolve(*location);
    if (!next) {
        return Result<Request>{next.error()};
    }
    const Url next_url = std::move(next.value());

    Method method = current.method();
    bool keep_body = true;
    const std::uint16_t status = response.status();
    if (status == 303) {
        if (method != Method::Head) {  // 303 把非 HEAD 一律改判 GET、弃体
            method = Method::Get;
            keep_body = false;
        }
    } else if (status == 301 || status == 302) {
        if (method == Method::Post) {  // 历史兼容：POST 的 301/302 转 GET、弃体；其余保方法
            method = Method::Get;
            keep_body = false;
        }
    }  // 307 / 308 严格保留方法与体

    Request redirected = make_request(method, next_url.to_string());
    for (const auto &entry : current.headers().entries()) {
        redirected.header(entry.name, entry.value);
    }
    if (keep_body && !current.body().empty()) {
        redirected.body(current.body());
    }
    return Result<Request>{std::move(redirected)};
}

/// @brief 缺省时补上 `Accept-Encoding` 宣告：宣告与解码能力同源，未编入能力时连宣告都不发。
///
/// 调用方已自带该字段（任意大小写拼写，`Headers` 的比较本就不敏感）时**原样保留**——协商是调用方的表达，
/// 本件只在缺省处补一枚与自己支持集等长的宣告，既不覆盖也不重复追加。
/// @param request 待补宣告的请求（就地修改）。
auto advertise_accept_encoding(Request &request) -> void {
    if (!detail::compression_available() || request.headers().contains("Accept-Encoding")) {
        return;
    }
    request.header("Accept-Encoding", detail::AURORA_HTTP_ACCEPT_ENCODING);
}

/// @brief 按应答的 `Content-Encoding` 链把已物化的消息体还原成原始字节（就地替换）。
///
/// 分帧解出的仍是编码字节，还原必须在物化之后、`Response` 交出之前完成，`Response::body()` 才是调用方可
/// 直接消费的字节。`BodyFraming::None`（HEAD / 1xx / 204 / 304）恒无体，也就不校验编码名——那类应答本来就
/// 没有可解的载荷，中间层却常照抄 `Content-Encoding`。字段缺席与 `identity` 都是恒等环节，直交原始字节。
/// @param head 已装配的头部块（提供字段原文）。
/// @param framing 已判定的消息体分帧形态。
/// @param body 已物化的线上字节；成功时换成还原后的字节，失败时保持原样不被半途写坏。
/// @param max_output_bytes 解压产物字节上限（`nullopt` = 不设限）；越限落 `http-body-too-large`。
/// @return 成功为 `Result<void>{}`；失败携 `http-content-encoding-unsupported` / `http-body-corrupt` /
///         `http-body-too-large` / `http-protocol-error` 之一。
auto materialize_content_encoding(const wire::ResponseHead &head, wire::BodyFraming framing, std::vector<char> &body,
                                  std::optional<std::uint64_t> max_output_bytes) -> Result<void> {
    if (framing == wire::BodyFraming::None) {
        return Result<void>{};
    }
    std::string list;
    for (const auto &line : head.headers.values("Content-Encoding")) {
        if (!list.empty()) {
            list += ',';
        }
        list += line;
    }
    if (list.empty()) {
        return Result<void>{};
    }
    const auto chain = detail::parse_content_encoding(list);
    if (!chain) {
        return Result<void>{chain.error()};
    }
    if (chain.value().empty()) {
        return Result<void>{};  // identity / 空值：没有环节要解
    }
    auto decoded = detail::decode_content_encoding(chain.value(), std::span<const char>{body}, max_output_bytes);
    if (!decoded) {
        return Result<void>{decoded.error()};
    }
    body = std::move(decoded.value());
    return Result<void>{};
}

/// @brief 从已装配的头部块取 `Content-Length` 的十进制值，供下载进度当「总量」。
///
/// 只在 `BodyFraming::Length` 形态下调用，故取首个同名值即可（分帧判定已由字节层把关，同名多值那种歧义
/// 形态在那一层就已被拒）。值畸形一律当「总量未知」而不是另造一枚错误：进度是观测口，它没有资格改变一次
/// 交换的成败判定。
/// @param head 已装配的头部块。
/// @return 解析出的字节数；无字段或非纯十进制时为 `nullopt`。
[[nodiscard]] auto announced_length(const wire::ResponseHead &head) -> std::optional<std::uint64_t> {
    for (const auto &line : head.headers.values("Content-Length")) {
        std::string_view text{line};
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
            text.remove_prefix(1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
            text.remove_suffix(1);
        }
        std::uint64_t value = 0;
        // 取 `[first, last)` 两枚指针即「带长度信息」的正规形态：`data()` 的返回值不假定以 NUL 结尾，
        // 尾指针由长度算出，判「整段都是数字」比的是尾指针而非某个哨兵。
        const char *const first = text.data();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars 需要 [first, last)
        const char *const last = first + text.size();
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec == std::errc{} && parsed.ptr == last) {
            return value;
        }
        return std::nullopt;  // 首个同名值即不可解析：不再往后猜
    }
    return std::nullopt;
}

}  // namespace

auto Client::send(Request request) const -> CoroTask<Result<Response>> {
    Cancellation token;
    Request current = std::move(request);
    advertise_accept_encoding(current);  // 缺省宣告与解码能力同源；调用方已自带则不动
    // 解压产物上限缺省跟随线上体上限：两枚旋钮各管一段字节，未显式收紧即「压缩不该让物化结果更多」。
    // 跟随的是「设了没有」而非数值——两枚都未设即恒不设限（`nullopt`）。
    const std::optional<std::uint64_t> decompressed_limit =
        max_decompressed_bytes_.has_value() ? max_decompressed_bytes_ : max_body_bytes_;
    const Timeouts timeouts = current.timeouts();
    const int limit = current.max_redirects();
    int hops = 0;
    Result<Response> outcome{make_error(ErrorCode::HttpProtocolError, "send produced no outcome")};
    const StreamOptions options;

    // 进度闸随协程帧存活；回调取自首跳请求，重定向后的各跳沿用同一份观测口（与 `timeouts` / 上限同纪律：
    // 发起侧选项在入口取一次，逐跳不重读）。下面几枚计数是**本跳**口径，每跳开头重置。
    detail::ProgressGate gate(current.on_progress(), progress_frame_source_);
    std::uint64_t wire_seen = 0;  // 本跳已 drain 的线上体字节（解压前口径）
    std::optional<std::uint64_t> announced_total;  // Length 分帧声明的体总量；其余形态未知
    std::uint64_t uploaded = 0;  // 本跳已写出的请求体字节
    std::uint64_t upload_total = 0;  // 本跳请求体总量（分块写时才有意义）
    bool head_seen = false;  // 本跳是否已见到头部（决定要不要报下载末值）
    bool upload_running = false;  // 本跳是否走了分块写路径
    bool upload_settled = false;  // 上传末值是否已在写环里发过（避免收尾重发同一事实）

    // 三档超时共享一枚触发状态：total 全程登记一次，connect / read_idle 在各自阶段前登记、结束即停表。
    auto tstate = std::make_shared<TimeoutState>();
    StageTimer total_timer(timeouts.total, TimeoutStage::Total, token, tstate);

    for (;;) {
        // ── 写出物化：请求头部 + 体（render_head 失败即 URL / 字段问题，原样交出）。
        auto head = current.render_head();
        if (!head) {
            outcome = Result<Response>{head.error()};
            break;
        }
        const auto &url_slot = current.url();  // render_head 成功即目标已解析
        if (!url_slot.has_value()) {
            outcome = Result<Response>{make_error(ErrorCode::HttpProtocolError, "target vanished after render_head")};
            break;
        }
        const Url &target = url_slot.value();
        // 本跳的进度计数重置：跨跳不累计，下一跳的总量在到达前无从得知。
        wire_seen = 0;
        announced_total.reset();
        uploaded = 0;
        upload_total = 0;
        head_seen = false;
        upload_running = false;
        upload_settled = false;

        const auto port = target.effective_port();
        if (!port.has_value()) {
            outcome = Result<Response>{make_error(ErrorCode::HttpProtocolError, "target has no effective port")};
            break;
        }
        const std::string host{target.host()};

        // ── 建连并统一读写口：明文 / TLS 两条流经捕获 shared_ptr 的类型擦除闭包收敛到同一对 awaiter。
        std::function<detail::CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
        std::function<detail::CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
        StageTimer connect_timer(timeouts.connect, TimeoutStage::Connect, token, tstate);
        if (target.is_secure()) {
            auto plain = co_await detail::await_connect(host, *port, options, token);
            if (!plain) {
                outcome = Result<Response>{plain.error()};
                break;
            }
            auto secure = co_await detail::await_handshake(std::move(plain.value()), host, tls_, token);
            if (!secure) {
                outcome = Result<Response>{secure.error()};
                break;
            }
            auto holder = std::make_shared<TlsTransport>(std::move(secure.value()));
            read_one = [holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
            write_one = [holder](std::span<const char> data) { return detail::await_write(*holder, data); };
        } else {
            auto plain = co_await detail::await_connect(host, *port, options, token);
            if (!plain) {
                outcome = Result<Response>{plain.error()};
                break;
            }
            auto holder = std::make_shared<StreamTransport>(std::move(plain.value()));
            read_one = [holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
            write_one = [holder](std::span<const char> data) { return detail::await_write(*holder, data); };
        }
        connect_timer.stop();  // 建连（含握手）完成，connect 档停表，不误伤后续读写

        // ── 发出请求。挂了进度回调且带请求体时才分块写出：分块只为给上传进度与写侧背压提供节拍，线上字节
        //    序列与一次写完逐字节相同，对端无从分辨。无回调恒一次写完，本切片的观测口对无需求者零改动。
        if (gate.armed() && !current.body().empty()) {
            const std::span<const char> body_view{current.body()};
            upload_total = body_view.size();
            upload_running = true;
            auto written_head = co_await write_one(std::span<const char>{head.value()});
            bool write_failed = !written_head;
            if (write_failed) {
                outcome = Result<Response>{written_head.error()};
            }
            while (!write_failed) {
                const std::size_t take = std::min<std::size_t>(AURORA_WRITE_CHUNK, body_view.size() - uploaded);
                auto written = co_await write_one(body_view.subspan(uploaded, take));
                if (!written) {
                    outcome = Result<Response>{written.error()};
                    write_failed = true;
                    break;
                }
                // 契约：`write` 成功即整段进入内核（`net/transport.h` 的「全部字节都进入内核或明确失败才算
                // 完成」），故计数按块长推进、不回读交付数——交付数在这里没有第二种可能的取值。
                uploaded += take;
                const bool settled = uploaded >= upload_total;
                gate.observe(Progress{.phase = ProgressPhase::Upload,
                                      .transferred_bytes = uploaded,
                                      .total_bytes = upload_total,
                                      .total_known = true,
                                      .finished = settled});
                gate.flush();
                upload_settled = upload_settled || settled;
                if (settled) {
                    break;
                }
            }
            if (write_failed) {
                break;
            }
        } else {
            std::string payload = head.value();
            payload.insert(payload.end(), current.body().begin(), current.body().end());
            auto written = co_await write_one(std::span<const char>{payload});
            if (!written) {
                outcome = Result<Response>{written.error()};
                break;
            }
        }

        // ── 逐段装配应答：喂 ResponseAssembler，按分帧物化体，直到 complete 或对端收摊。
        wire::ResponseAssembler assembler(max_body_bytes_, current.is_head());
        std::vector<char> body;
        std::array<char, AURORA_READ_CHUNK> chunk{};
        bool halted = false;
        // 下载进度的记账口：每次 `drain_body` 之后按「新到多少线上字节」记一笔，再冲一次闸。闸由每一次读
        // 恢复推动，故一次交付至多一发；总量只有 `Length` 形态给得出，chunked / until-close 一律报未知。
        auto note_download = [&]() -> void {
            if (!head_seen && assembler.head_ready()) {
                head_seen = true;
                announced_total = assembler.framing() == wire::BodyFraming::Length ? announced_length(assembler.head())
                                                                                   : std::optional<std::uint64_t>{};
            }
            if (body.size() > wire_seen) {
                wire_seen = body.size();
                gate.observe(Progress{.phase = ProgressPhase::Download,
                                      .transferred_bytes = wire_seen,
                                      .total_bytes = announced_total.value_or(0),
                                      .total_known = announced_total.has_value(),
                                      .finished = false});
            }
            gate.flush();
        };
        while (!assembler.complete()) {
            StageTimer read_timer(timeouts.read_idle, TimeoutStage::ReadIdle, token, tstate);
            auto read = co_await read_one(std::span<char>{chunk});
            read_timer.stop();  // 本段交付已落地，重置空闲计时：下一轮 read 重新起表
            if (!read) {
                outcome = Result<Response>{read.error()};
                halted = true;
                break;
            }
            const std::size_t got = read.value();
            if (got == 0) {  // 对端 FIN：UntilClose 据此判全，其余形态提前收摊即协议错位
                auto closed = assembler.peer_closed();
                if (!closed) {
                    outcome = Result<Response>{closed.error()};
                    halted = true;
                    break;
                }
                if (!assembler.complete()) {
                    outcome = Result<Response>{
                        make_error(ErrorCode::HttpProtocolError, "peer closed before the body was complete")};
                    halted = true;
                    break;
                }
                break;
            }
            auto pushed = assembler.push(std::string_view{chunk.data(), got});
            if (!pushed) {
                outcome = Result<Response>{pushed.error()};
                halted = true;
                break;
            }
            assembler.drain_body(body);
            note_download();
        }
        if (halted) {
            break;
        }
        assembler.drain_body(body);
        note_download();

        const auto &response_head = assembler.head();
        // 透明解压：分帧给出的仍是编码字节，还原完才交给 `Response`；失败与传输失败分道（字节已完整到达）。
        auto decoded = materialize_content_encoding(response_head, assembler.framing(), body, decompressed_limit);
        if (!decoded) {
            outcome = Result<Response>{decoded.error()};
            break;
        }
        outcome = Result<Response>{
            Response{response_head.status, response_head.reason, response_head.headers, std::move(body)}};

        // ── 重定向判定。
        if (!is_redirect(response_head.status) || limit == 0) {
            break;
        }
        if (++hops > limit) {
            outcome = Result<Response>{
                make_error(ErrorCode::HttpRedirectLimit, "redirect chain exceeded the configured limit")};
            break;
        }
        auto next = follow(current, outcome.value());
        if (!next) {
            outcome = Result<Response>{next.error()};
            break;
        }
        current = std::move(next.value());
    }

    // 超时改道：失败若因某档看门狗取消了令牌，一律改报 http-timeout，并在 detail 标明是哪一档。
    if (!outcome) {
        switch (static_cast<TimeoutStage>(tstate->fired.load())) {
            case TimeoutStage::Connect:
                outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "connect timeout")};
                break;
            case TimeoutStage::ReadIdle:
                outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "read-idle timeout")};
                break;
            case TimeoutStage::Total:
                outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "total timeout")};
                break;
            case TimeoutStage::None:
                break;
        }
    }

    // 进度收尾：两个方向的末值各发一次，不看闸号。成败由 `outcome` 表达，这一发只宣告「这一趟到此收摊」，
    // 让挂着回调的响应式状态落到确定值而不是停在一个半途数字上。写环里已发过上传末值的不重复发。
    if (gate.armed()) {
        if (upload_running && !upload_settled) {
            gate.observe(Progress{.phase = ProgressPhase::Upload,
                                  .transferred_bytes = uploaded,
                                  .total_bytes = upload_total,
                                  .total_known = true,
                                  .finished = true});
        }
        if (head_seen) {
            gate.observe(Progress{.phase = ProgressPhase::Download,
                                  .transferred_bytes = wire_seen,
                                  .total_bytes = announced_total.value_or(0),
                                  .total_known = announced_total.has_value(),
                                  .finished = true});
        }
    }
    gate.finish();

    co_return outcome;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): Sink 由调用方持有，帧只借用
auto Client::open_stream(Request request, DataSink &sink) const -> CoroTask<Result<StreamOutcome>> {
    // 本腿无流式语义（wasm 无 ReadableStream）时直接失败，绝不悄悄退化成物化路径：调用方选 `open_stream`
    // 要的就是「整条载荷不必同时存在于内存」，退化成交换一份 `Response` 是把它的内存预算和预期一起毁掉。
    if (!Engine::instance().capabilities().streaming) {
        co_return Result<StreamOutcome>{
            make_error(ErrorCode::NetPlatformUnavailable, "open_stream: this leg has no streaming response body")};
    }

    Cancellation token;
    Request current = std::move(request);
    advertise_accept_encoding(current);  // 缺省宣告与解码能力同源；调用方已自带则不动
    // 解压产物上限**只取 `max_decompressed_bytes()`**，不像 `send` 那样缺省跟随 `max_body_bytes()`：后者是
    // 物化路径的内存闸，流式路径不物化，拿它当解压上限会让「流式下载一个大压缩包」必须去调大一枚与内存无关
    // 的旋钮。两枚旋钮在流式路径各管各的：线上的一个都不设，还原的照本旋钮设。
    const std::optional<std::uint64_t> decompressed_limit = max_decompressed_bytes_;
    const Timeouts timeouts = current.timeouts();
    const int limit = current.max_redirects();
    int hops = 0;
    Result<StreamOutcome> outcome{make_error(ErrorCode::HttpProtocolError, "open_stream produced no outcome")};
    const StreamOptions options;

    detail::ProgressGate gate(current.on_progress(), progress_frame_source_);
    std::uint64_t wire_bytes = 0;  // 跨全部跳、分帧解出的线上字节（解压前口径）
    std::uint64_t delivered_bytes = 0;  // 跨全部跳交给 `DataSink::on_data` 的明文字节（解压后口径）
    bool stopped_by_sink = false;  // 宿主是否回过 `Stop`（成功结局的一种，见 net/http_stream.h）
    std::optional<Response> final_head;  // 最终一跳的头部块；重定向跳不进这里

    // 本跳的进度记账（跨跳不累计，与 `send` 同口径）：每跳开头重置。
    std::uint64_t wire_seen = 0;
    std::optional<std::uint64_t> announced_total;
    std::uint64_t uploaded = 0;
    std::uint64_t upload_total = 0;
    bool head_seen = false;
    bool upload_running = false;
    bool upload_settled = false;

    auto tstate = std::make_shared<TimeoutState>();
    StageTimer total_timer(timeouts.total, TimeoutStage::Total, token, tstate);

    for (;;) {
        // ── 写出物化：请求头部 + 体（与 `send` 同判据，含「挂了回调才分块写」那条）。
        auto head = current.render_head();
        if (!head) {
            outcome = Result<StreamOutcome>{head.error()};
            break;
        }
        const auto &url_slot = current.url();
        if (!url_slot.has_value()) {
            outcome =
                Result<StreamOutcome>{make_error(ErrorCode::HttpProtocolError, "target vanished after render_head")};
            break;
        }
        const Url &target = url_slot.value();
        wire_seen = 0;
        announced_total.reset();
        uploaded = 0;
        upload_total = 0;
        head_seen = false;
        upload_running = false;
        upload_settled = false;

        const auto port = target.effective_port();
        if (!port.has_value()) {
            outcome = Result<StreamOutcome>{make_error(ErrorCode::HttpProtocolError, "target has no effective port")};
            break;
        }
        const std::string host{target.host()};

        // ── 建连并统一读写口：明文 / TLS 两条流经捕获 shared_ptr 的类型擦除闭包收敛到同一对 awaiter。
        std::function<detail::CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
        std::function<detail::CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
        StageTimer connect_timer(timeouts.connect, TimeoutStage::Connect, token, tstate);
        if (target.is_secure()) {
            auto plain = co_await detail::await_connect(host, *port, options, token);
            if (!plain) {
                outcome = Result<StreamOutcome>{plain.error()};
                break;
            }
            auto secure = co_await detail::await_handshake(std::move(plain.value()), host, tls_, token);
            if (!secure) {
                outcome = Result<StreamOutcome>{secure.error()};
                break;
            }
            auto holder = std::make_shared<TlsTransport>(std::move(secure.value()));
            read_one = [holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
            write_one = [holder](std::span<const char> data) { return detail::await_write(*holder, data); };
        } else {
            auto plain = co_await detail::await_connect(host, *port, options, token);
            if (!plain) {
                outcome = Result<StreamOutcome>{plain.error()};
                break;
            }
            auto holder = std::make_shared<StreamTransport>(std::move(plain.value()));
            read_one = [holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
            write_one = [holder](std::span<const char> data) { return detail::await_write(*holder, data); };
        }
        connect_timer.stop();

        if (gate.armed() && !current.body().empty()) {
            const std::span<const char> body_view{current.body()};
            upload_total = body_view.size();
            upload_running = true;
            auto written_head = co_await write_one(std::span<const char>{head.value()});
            bool write_failed = !written_head;
            if (write_failed) {
                outcome = Result<StreamOutcome>{written_head.error()};
            }
            while (!write_failed) {
                const std::size_t take = std::min<std::size_t>(AURORA_WRITE_CHUNK, body_view.size() - uploaded);
                auto written = co_await write_one(body_view.subspan(uploaded, take));
                if (!written) {
                    outcome = Result<StreamOutcome>{written.error()};
                    write_failed = true;
                    break;
                }
                uploaded += take;
                const bool settled = uploaded >= upload_total;
                gate.observe(Progress{.phase = ProgressPhase::Upload,
                                      .transferred_bytes = uploaded,
                                      .total_bytes = upload_total,
                                      .total_known = true,
                                      .finished = settled});
                gate.flush();
                upload_settled = upload_settled || settled;
                if (settled) {
                    break;
                }
            }
            if (write_failed) {
                break;
            }
        } else {
            std::string payload = head.value();
            payload.insert(payload.end(), current.body().begin(), current.body().end());
            auto written = co_await write_one(std::span<const char>{payload});
            if (!written) {
                outcome = Result<StreamOutcome>{written.error()};
                break;
            }
        }

        // ── 逐段装配并交付：装配器**不设线上体上限**（物化路径的内存闸在这里没有对应的内存可闸），
        //    每段解出的明文喂进流式还原链，链末直交宿主。
        wire::ResponseAssembler assembler{std::nullopt, current.is_head()};
        std::vector<char> scratch;  // 本段刚解出的线上字节（每轮先清空，故峰值只有一段的量）
        std::array<char, AURORA_READ_CHUNK> chunk{};
        std::optional<detail::StreamingDecoder> decoder;  // 头部定形后才建链（建链要读 Content-Encoding）
        bool halted = false;
        bool redirect_hop = false;

        // 产出出口：链末每交一段明文记一次交付量。宿主回 `Stop` 只中止交付，本库不替它决定成败。
        auto emit = [&sink, &delivered_bytes](std::span<const char> bytes) -> Result<detail::EmitFlow> {
            delivered_bytes += bytes.size();
            const auto flow = sink.on_data(bytes);
            return Result<detail::EmitFlow>{flow == SinkFlow::Stop ? detail::EmitFlow::Stop
                                                                   : detail::EmitFlow::Continue};
        };

        while (!assembler.complete()) {
            StageTimer read_timer(timeouts.read_idle, TimeoutStage::ReadIdle, token, tstate);
            auto read = co_await read_one(std::span<char>{chunk});
            read_timer.stop();
            if (!read) {
                outcome = Result<StreamOutcome>{read.error()};
                halted = true;
                break;
            }
            const std::size_t got = read.value();
            if (got == 0) {  // 对端 FIN：UntilClose 据此判全，其余形态提前收摊即协议错位
                auto closed = assembler.peer_closed();
                if (!closed) {
                    outcome = Result<StreamOutcome>{closed.error()};
                    halted = true;
                    break;
                }
                if (!assembler.complete()) {
                    outcome = Result<StreamOutcome>{
                        make_error(ErrorCode::HttpProtocolError, "peer closed before the body was complete")};
                    halted = true;
                    break;
                }
                break;
            }
            auto pushed = assembler.push(std::string_view{chunk.data(), got});
            if (!pushed) {
                outcome = Result<StreamOutcome>{pushed.error()};
                halted = true;
                break;
            }
            scratch.clear();
            assembler.drain_body(scratch);
            if (!scratch.empty()) {
                // 线上字节先记账再判头部：宿主在 `on_head` 就收摊时，本批已到达的字节仍是到达过的事实。
                wire_bytes += scratch.size();
                wire_seen += scratch.size();
            }

            // 头部判定必须先于体交付：一次读常同时带来头部尾与体首段，而宿主的次序契约是 on_head → on_data。
            if (!head_seen && assembler.head_ready()) {
                head_seen = true;
                announced_total = assembler.framing() == wire::BodyFraming::Length ? announced_length(assembler.head())
                                                                                   : std::optional<std::uint64_t>{};
                if (is_redirect(assembler.head().status) && limit != 0) {
                    // 重定向跳：头部一判定即弃该跳载荷、直接发起下一跳。一条连接一个跳，收摊由 holder 的
                    // 作用域结束完成；`send` 逐跳物化，本入口不为注定被丢弃的体继续读字节。
                    if (++hops > limit) {
                        outcome = Result<StreamOutcome>{
                            make_error(ErrorCode::HttpRedirectLimit, "redirect chain exceeded the configured limit")};
                        halted = true;
                        break;
                    }
                    redirect_hop = true;
                    break;
                }
                std::vector<detail::Coding> chain;  // 应用次序；无体应答恒空（不校验编码名，与 `send` 同判据）
                if (assembler.framing() != wire::BodyFraming::None) {
                    std::string list;
                    for (const auto &line : assembler.head().headers.values("Content-Encoding")) {
                        if (!list.empty()) {
                            list += ',';
                        }
                        list += line;
                    }
                    if (!list.empty()) {
                        auto parsed = detail::parse_content_encoding(list);
                        if (!parsed) {
                            outcome = Result<StreamOutcome>{parsed.error()};
                            halted = true;
                            break;
                        }
                        chain = std::move(parsed.value());
                    }
                }
                // 空链也建（直通），交付口径就与有环节完全同一条路；未编入解压能力时在建链这一步就失败，
                // 不会让压缩字节流进宿主一半才被认出来。
                auto created = detail::StreamingDecoder::create(chain, decompressed_limit, emit);
                if (!created) {
                    outcome = Result<StreamOutcome>{created.error()};
                    halted = true;
                    break;
                }
                decoder = std::move(created.value());

                final_head = Response{assembler.head().status, assembler.head().reason, assembler.head().headers, {}};
                if (sink.on_head(*final_head) == SinkFlow::Stop) {
                    stopped_by_sink = true;
                    break;
                }
            }

            if (!scratch.empty()) {
                gate.observe(Progress{.phase = ProgressPhase::Download,
                                      .transferred_bytes = wire_seen,
                                      .total_bytes = announced_total.value_or(0),
                                      .total_known = announced_total.has_value(),
                                      .finished = false});
                gate.flush();
                auto emitted = decoder->push(std::span<const char>{scratch});
                if (!emitted) {
                    outcome = Result<StreamOutcome>{emitted.error()};
                    halted = true;
                    break;
                }
                if (emitted.value() == detail::EmitFlow::Stop) {
                    // 停读即收摊：链掐在半途，此后不得 `finish()`（那是宣告一条自己掐断的流「应当完整」）。
                    stopped_by_sink = true;
                    break;
                }
            }
        }
        if (halted) {
            break;
        }
        // 收尾排空：装配器在 `complete()` 那一轮已把明文交进 `pending_`，本步只兜住「最后一批还压在内部缓冲」
        // 这一种形态。中途收摊（宿主 `Stop`）与弃体换跳（重定向）都不兜——前者不能再交付字节，后者的体本就作废。
        if (!redirect_hop && !stopped_by_sink) {
            scratch.clear();
            assembler.drain_body(scratch);
            if (!scratch.empty() && decoder.has_value()) {
                wire_bytes += scratch.size();
                wire_seen += scratch.size();
                gate.observe(Progress{.phase = ProgressPhase::Download,
                                      .transferred_bytes = wire_seen,
                                      .total_bytes = announced_total.value_or(0),
                                      .total_known = announced_total.has_value(),
                                      .finished = false});
                gate.flush();
                auto emitted = decoder->push(std::span<const char>{scratch});
                if (!emitted) {
                    outcome = Result<StreamOutcome>{emitted.error()};
                    break;
                }
                stopped_by_sink = emitted.value() == detail::EmitFlow::Stop;
            }
        }

        if (redirect_hop) {
            Response hop{assembler.head().status, assembler.head().reason, assembler.head().headers, {}};
            auto next = follow(current, hop);
            if (!next) {
                outcome = Result<StreamOutcome>{next.error()};
                break;
            }
            current = std::move(next.value());
            continue;
        }
        if (!final_head.has_value()) {
            outcome = Result<StreamOutcome>{
                make_error(ErrorCode::HttpProtocolError, "stream ended before the response head was settled")};
            break;
        }
        if (!stopped_by_sink) {
            // 收摊链：排出各环残留产物并逐环验证流完整终结。失败即字节断在半途，据此不调 `on_end`。
            auto settled = decoder ? decoder->finish() : Result<detail::EmitFlow>{detail::EmitFlow::Continue};
            if (!settled) {
                outcome = Result<StreamOutcome>{settled.error()};
                break;
            }
            // 收摊时回 `Stop` 同样是「宿主不要了」这一事实（此时线上字节已读完，只是末段交付被掐）。
            stopped_by_sink = settled.value() == detail::EmitFlow::Stop;
        }
        sink.on_end();
        outcome = Result<StreamOutcome>{StreamOutcome{.response = std::move(*final_head),
                                                      .wire_bytes = wire_bytes,
                                                      .delivered_bytes = delivered_bytes,
                                                      .stopped_by_sink = stopped_by_sink}};
        break;
    }

    // 超时改道：与 `send` 同判据——失败若因某档看门狗取消了令牌，一律改报 http-timeout 并标明是哪一档。
    if (!outcome) {
        switch (static_cast<TimeoutStage>(tstate->fired.load())) {
            case TimeoutStage::Connect:
                outcome = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "connect timeout")};
                break;
            case TimeoutStage::ReadIdle:
                outcome = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "read-idle timeout")};
                break;
            case TimeoutStage::Total:
                outcome = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "total timeout")};
                break;
            case TimeoutStage::None:
                break;
        }
    }

    // 进度收尾：与 `send` 同判据，两方向末值各发一次、不看闸号；成败由 `outcome` 表达。
    if (gate.armed()) {
        if (upload_running && !upload_settled) {
            gate.observe(Progress{.phase = ProgressPhase::Upload,
                                  .transferred_bytes = uploaded,
                                  .total_bytes = upload_total,
                                  .total_known = true,
                                  .finished = true});
        }
        if (head_seen) {
            gate.observe(Progress{.phase = ProgressPhase::Download,
                                  .transferred_bytes = wire_seen,
                                  .total_bytes = announced_total.value_or(0),
                                  .total_known = announced_total.has_value(),
                                  .finished = true});
        }
    }
    gate.finish();

    co_return outcome;
}

}  // namespace aurora::net::http
