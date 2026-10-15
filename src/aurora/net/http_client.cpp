// http_client.cpp —— HTTP 发起侧客户端的 send / open_stream 核心闭环。
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
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/net/detail/http_cache.h"
#include "aurora/net/detail/http_encoding.h"
#include "aurora/net/detail/http_progress.h"
#include "aurora/net/detail/http_request_id.h"
#include "aurora/net/detail/http_retry.h"
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
///
/// `Budget` 不属于单段看门狗：它是重试环的跨尝试墙（`RetryPolicy::budget`），由外层登记、由外层判读，
/// 单段收尾的 switch 里它恒不可达——列进来只为让那一处 switch 保持逐档穷尽、不靠 `default` 吞掉新档。
enum class TimeoutStage : std::uint8_t {
    None,
    Connect,
    ReadIdle,
    Total,
    Budget,
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

/// @brief 把链路标识按三级取定落到请求字段上：已写字段 → 请求级取值 → 客户端缺省补齐。
///
/// 只落在**原始请求**上，一次交换落一次：重试环的每一次尝试都从 `origin` 重新拷一份、重定向的每一跳又从
/// 当前请求复制字段，故这一枚 id 天然穿过全部跳与全部尝试不变——那正是它存在的理由（对端与中间层据同一串
/// 把一条逻辑交换的若干次物理往来对上）。落在逐尝试副本上会让第二次尝试换一个 id，把同一次重放在日志里裂成
/// 两条互不相干的交换。
/// @param request 待落标识的请求（就地修改）。
/// @param enabled `Client::request_id()` 的缺省开关：关掉后除非请求自带，否则一个字节都不多发。
auto stamp_request_id(Request &request, bool enabled) -> void {
    if (request.headers().contains(detail::AURORA_HTTP_REQUEST_ID)) {
        return;  // 调用方自己写的永远优先（与 `Accept-Encoding` 的缺省补齐同一纪律）
    }
    if (const auto &declared = request.request_id(); declared.has_value()) {
        if (!declared->empty()) {
            request.header(detail::AURORA_HTTP_REQUEST_ID, *declared);
        }
        return;  // 空串 = 这条请求明确不要标识，缺省补齐就此打住
    }
    if (enabled) {
        request.header(detail::AURORA_HTTP_REQUEST_ID, detail::make_request_id());
    }
}

/// @brief 起手把这次交换的身份记一行 debug：标识 + 方法 + 主机。
///
/// 恒不含鉴权字段与请求体——本行是给「拿 id 去对端日志找那条请求」用的，写得越少越不会被误当成凭据来源。
/// 只在字段真的落上了时记：关着缺省又没自带的请求，本行没有可报的事实。
/// @param request 已落标识的原始请求。
auto log_exchange_identity(const Request &request) -> void {
    const auto announced = request.headers().get(detail::AURORA_HTTP_REQUEST_ID);
    if (!announced.has_value()) {
        return;
    }
    std::string_view host;
    if (const auto &target = request.url(); target.has_value()) {
        host = target.value().host();
    }
    AURORA_LOG_DEBUG("net", "http exchange: id=", *announced, " method=", method_text(request.method()),
                     " host=", host);
}

/// @brief 一跳的建连事实：连到谁、以哪种形态连、请求行写成什么形态。
///
/// 三条发起侧口径在这枚小结构里一次定死：① 代理决策（`for_target` 的产出）先定；② 策略判定读的就是这一份
/// 决策，故「有没有经代理」在库里只有一个答案；③ 建连一律走 `await_connect_via` 这一条路，转发形态只是把
/// 「对端」换成代理本身、并把配置降为 `Direct`（代理已经站在门口，不必再「经代理」找它）。
struct HopRoute {
    ProxyConfig connect_with;  ///< 交给 `await_connect_via` 的配置（`Direct` = 对端就是直连目标或代理本身）
    std::string peer_host;  ///< 建连对端主机文本
    std::uint16_t peer_port{0};  ///< 建连对端端口（主机序）
    bool absolute_form{false};  ///< 请求行是否写成绝对形态（明文交转发代理时才是真）
};

/// @brief 生效策略的三级取定：客户端显式设过 → 进程级默认 → `NetPolicy{}` 那组保守缺省。
/// @param client_policy `Client::policy()` 的已设值。
/// @return 本次交换逐跳共用的那一份策略。
auto effective_policy(const std::optional<NetPolicy> &client_policy) -> NetPolicy {
    if (client_policy.has_value()) {
        return client_policy.value();
    }
    if (const auto from_process = default_policy(); from_process.has_value()) {
        return from_process.value();
    }
    return NetPolicy{};
}

/// @brief 定出本跳的路由：先按目标算代理决策，再过策略闸，最后折算对端与请求行形态。
///
/// 次序是刻意的：策略要看「本跳实际会经过哪个代理」（`require_proxy` 那一维读的正是这个），故在决策之后；
/// 而这一切都发生在任何 `co_await` 之前，被拒的那一跳一条连接都不发起、一个字节都不写出。
/// @param environment 客户端的代理集合。
/// @param tls 客户端的 TLS 口径（策略的版本下限维度拿它对表）。
/// @param policy 本次交换生效的策略（发起那一刻读定，逐跳共用同一份）。
/// @param target 本跳的目标 URL（已解析）。
/// @param port 本跳的目标端口。
/// @return 成功携路由；被策略拒绝携 `net-policy-violated`。
auto plan_route(const ProxyEnvironment &environment, const TlsContext &tls, const NetPolicy &policy, const Url &target,
                std::uint16_t port) -> Result<HopRoute> {
    HopRoute route;
    route.peer_host = std::string{target.host()};
    route.peer_port = port;
    const ProxyConfig decision = environment.for_target(route.peer_host, port, target.is_secure());
    if (const auto verdict = policy.evaluate(target, decision, tls); !verdict) {
        return Result<HopRoute>{verdict.error()};
    }
    // 明文遇上 HTTP 代理时按「转发」而非「隧道」交付：请求行写成绝对形态、对端换成代理本身。转发本就是转发
    // 代理的本职，而 `CONNECT` 到 80 常被拒止——那会把「这种用法代理不支持」伪装成连通性故障。SOCKS5 没有
    // 转发形态，`https://` 的载荷也不能让代理明文看见，两者一律建隧道、请求行照写 origin-form。
    if (decision.kind() == ProxyKind::HttpConnect && !target.is_secure()) {
        route.peer_host = decision.host();
        route.peer_port = decision.port();
        route.absolute_form = true;
        route.connect_with = ProxyConfig::direct();
    } else {
        route.connect_with = decision;
    }
    return Result<HopRoute>{std::move(route)};
}

/// @brief 起手策略闸：目标解析不了或没有可用端口时放行（那是 `render_head` / 发起处原有判据要报的错，
///        策略不抢它们的活、更不替畸形目标编一条），其余情形与逐跳判定同一条路。
/// @param environment 代理集合。
/// @param tls TLS 口径。
/// @param policy 生效策略。
/// @param request 起手请求（其 URL 可能已记解析失败）。
/// @return 成功为 `Result<void>{}`；被拒携 `net-policy-violated`。
auto gate_origin(const ProxyEnvironment &environment, const TlsContext &tls, const NetPolicy &policy,
                 const Request &request) -> Result<void> {
    const auto &slot = request.url();
    if (!slot.has_value()) {
        return Result<void>{};
    }
    const auto port = slot.value().effective_port();
    if (!port.has_value()) {
        return Result<void>{};
    }
    auto routed = plan_route(environment, tls, policy, slot.value(), *port);
    if (!routed) {
        return Result<void>{routed.error()};
    }
    return Result<void>{};
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

/// @brief 取应答里的 `Retry-After` 并折算为等待时长；字段缺失或解不出为空 `nullopt`（= 对端没给）。
[[nodiscard]] auto retry_after_of(const Response &response) -> std::optional<std::chrono::milliseconds> {
    const auto field = response.header("Retry-After");
    if (!field.has_value()) {
        return std::nullopt;
    }
    return detail::parse_retry_after(field.value(), std::chrono::system_clock::now());
}

/// @brief 起手问表的结论：可直接交付的新鲜副本，以及「这次是不是替表里的条目去重验证」。
struct CacheOpening {
    std::optional<Response> fresh;  ///< 新鲜副本：非空即本次交换根本不必发起
    bool revalidating{false};  ///< 已在请求上挂过两枚条件头
    std::string key;  ///< 建键用的目标 URL；空 = 本次没参与缓存
};

/// @brief 缓存起手式：命中新鲜副本即交出，命中陈旧副本则把两枚条件头挂到请求上（原地改）。
///
/// 只在「开关开着 + 本请求没 `no_cache()` + 方法可缓存 + URL 解析成功」四件同时成立时才问表，缺一即整段
/// 跳过——跳过的形态与缓存从未存在时逐字节相同（连一次内存查找都不做）。
///
/// 请求侧的 `Cache-Control` 在这里也有一票，与表内写入侧同口径：`no-store` 连查都不查（那条既不许存也不许
/// 复用），`no-cache` 则只挡「不经核对就交付」这一种命中，条目带着校验子时照样挂头去验证——那是
/// `RFC 9111 §5.2.1.5` 的字面语义，比 `Request::no_cache()` 的整段绕过弱一档，两者刻意不同。
/// @param cache 表；未构造时跳过。
/// @param options 本客户端的缓存口径。
/// @param request 待发起的请求（陈旧副本会往它上面挂 `If-None-Match` / `If-Modified-Since`）。
/// @return 见 `CacheOpening`。
[[nodiscard]] auto open_from_cache(const std::shared_ptr<detail::HttpResponseCache> &cache, const CacheOptions &options,
                                   Request &request) -> CacheOpening {
    CacheOpening out;
    if (!options.enabled || request.no_cache() || !detail::is_cacheable_method(request.method()) || !cache) {
        return out;
    }
    const auto target = request.url();
    if (!target.has_value()) {
        return out;  // URL 还没解析成功：那是 `render_head()` 要报的错，缓存不猜键
    }
    const auto directive = detail::parse_cache_control(detail::joined_field(request.headers(), "Cache-Control"));
    if (directive.no_store) {
        return out;
    }
    out.key = target.value().to_string();
    auto found = cache->lookup(request.method(), out.key, request.headers(), std::chrono::system_clock::now());
    if (found.fresh.has_value() && !directive.no_cache) {
        out.fresh = std::move(found.fresh);
        return out;
    }
    if (found.revalidate.has_value()) {
        // 调用方自己声明的条件头优先（与本件的 `Accept-Encoding` 同纪律：协商是调用方的表达，不覆盖）。
        if (found.revalidate->etag.has_value() && !request.headers().contains("If-None-Match")) {
            request.set_header("If-None-Match", found.revalidate.value().etag.value());
            out.revalidating = true;
        }
        if (found.revalidate->last_modified.has_value() && !request.headers().contains("If-Modified-Since")) {
            request.set_header("If-Modified-Since", found.revalidate.value().last_modified.value());
            out.revalidating = true;
        }
    }
    return out;
}

/// @brief 物化路径的收尾：304 换成合并后的已存应答，其余按规则进表。
///
/// 两枚判定都容错：合并不了（条目被逐出）就把对端说的那份 304 原样交出——编造一份体比交出真相更糟；
/// 存不进表（`no-store` / `Vary: *` / 不可存的方法与状态码）是常态而非故障，不改变交出的应答。
/// @param cache 表。
/// @param options 本客户端的缓存口径。
/// @param origin 本次交换的原始请求（键与 `Vary` 取值都从它身上取，重定向改写的是逐跳副本）。
/// @param key 起手算出的建键 URL；空 = 本次没参与缓存。
/// @param revalidating 起手是否挂过条件头。
/// @param outcome 本次交换的结局（就地改写：304 → 合并副本）。
auto settle_cache(const std::shared_ptr<detail::HttpResponseCache> &cache, const CacheOptions &options,
                  const Request &origin, const std::string &key, bool revalidating, Result<Response> &outcome) -> void {
    if (!options.enabled || origin.no_cache() || !detail::is_cacheable_method(origin.method()) || !cache ||
        key.empty() || !outcome) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    if (outcome.value().status() == 304) {
        // 调用方自己的条件请求拿到的 304 不在此列：本件不知道它对应表里哪一条，交出对端说的那份即可。
        if (!revalidating) {
            return;
        }
        auto merged = cache->revalidated(origin.method(), key, origin.headers(), outcome.value(), now);
        if (merged.has_value()) {
            outcome = Result<Response>{std::move(merged.value())};
        }
        return;
    }
    // 进不了表是常态而非故障（`no-store` / `Vary: *` / 不可存的方法与状态码 / 无有效期又无校验子），
    // 它不改变交出的应答，故这里只把判定本身当成一次分支。
    if (!cache->store(origin.method(), key, origin.headers(), outcome.value(), now)) {
        return;
    }
}

/// @brief 把一份已存应答按段交给流式宿主；交付次序与真从对端取回时一致。
/// @param sink 宿主（调用方持有）。
/// @param response 合并后要交出的那份（体来自表）。
/// @return 完成事实：`wire_bytes` 恒 0（本次没有线上字节），`delivered_bytes` 是实交量。
[[nodiscard]] auto deliver_cached_stream(DataSink &sink, const Response &response) -> StreamOutcome {
    Response head{response.status(), std::string{response.reason()}, response.headers(), {}, true};
    std::uint64_t delivered = 0;
    bool stopped = sink.on_head(head) == SinkFlow::Stop;
    if (!stopped) {
        const auto &body = response.body();
        for (std::size_t offset = 0; offset < body.size(); offset += AURORA_READ_CHUNK) {
            const std::size_t length = std::min<std::size_t>(AURORA_READ_CHUNK, body.size() - offset);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 分段窗口由 offset 与 length 界定
            const std::span<const char> segment{body.data() + offset, length};
            if (sink.on_data(segment) == SinkFlow::Stop) {
                stopped = true;
                break;
            }
            delivered += length;
        }
    }
    sink.on_end();
    return StreamOutcome{
        .response = std::move(head), .wire_bytes = 0, .delivered_bytes = delivered, .stopped_by_sink = stopped};
}

/// @brief 重试环套在流式宿主外的记录壳：只转发并记下「头部是否已经交出去」。
///
/// 为什么需要它：流式路径的重试只可能发生在 `on_head` 之前——首段字节一旦交给宿主就收不回来，重播等于让
/// 宿主重写已写下那段（大文件下载里那是不可逆的）。本壳不改变任何交付语义（三个入口逐字转发、返回值原样
/// 交回），只把那一个判定露给外层重试环。
class HeadGuardSink final : public DataSink {
  public:
    /// @brief 以真正的宿主壳构造。
    /// @param inner 调用方交来的接收侧宿主。
    explicit HeadGuardSink(DataSink &inner) : inner_(inner) {}

    HeadGuardSink(const HeadGuardSink &) = delete;
    auto operator=(const HeadGuardSink &) -> HeadGuardSink & = delete;
    HeadGuardSink(HeadGuardSink &&) = delete;
    auto operator=(HeadGuardSink &&) -> HeadGuardSink & = delete;
    // 基类有虚析构，故本壳也显式写一枚：只借用宿主、无自有资源，`= default` 即全部语义。
    ~HeadGuardSink() override = default;

    /// @brief 转发头部并登记「已交出」——此后本次交换不再具备重试资格。
    /// @param head 已装配的头部块。
    /// @return 宿主的答复，原样交回。
    auto on_head(const Response &head) -> SinkFlow override {
        handed_off_ = true;
        return inner_.on_head(head);
    }

    /// @brief 转发一段体字节。
    /// @param body 本段明文字节。
    /// @return 宿主的答复，原样交回。
    auto on_data(std::span<const char> body) -> SinkFlow override { return inner_.on_data(body); }

    /// @brief 转发收尾。
    auto on_end() -> void override { inner_.on_end(); }

    /// @brief 头部（或任何体字节）是否已交给宿主。
    /// @return 交出过为 true；本次交换不再可重试。
    [[nodiscard]] auto handed_off() const -> bool { return handed_off_; }

  private:
    DataSink &inner_;  ///< 真宿主（由调用方持有，本壳只借用）
    bool handed_off_{false};  ///< 是否已进入过 `on_head`
};

}  // namespace

Client::Client() : cache_{std::make_shared<detail::HttpResponseCache>(cache_options_)} {}

auto Client::cache(CacheOptions value) -> Client & {
    cache_options_ = value;
    // 口径同步进表：`max_entries` 是表自己的容量，改小时它按新上限逐出多余条目（不是留着等命中）。
    // 表为空（只在移动后的壳上 possible）时无处可同步，开关本身仍已生效，下次起手照旧跳过。
    if (cache_) {
        cache_->set_options(value);
    }
    return *this;
}

auto Client::send(Request request) const -> CoroTask<Result<Response>> {
    // 策略取「请求覆盖 → 客户端缺省」两级；形态闸在起手就过，越界值不进入任何一次尝试。
    const RetryPolicy policy = request.retry_policy().value_or(retry_);
    if (const auto invalid = detail::validate_retry_policy(policy)) {
        co_return Result<Response>{*invalid};
    }
    Request origin = std::move(request);
    // 链路标识在原始请求上落一次（口径见 `stamp_request_id`）：落在这里，重定向的每一跳与重试的每一次尝试
    // 都带着同一串出去，缓存键也据此在同一枚 id 上稳定命中。
    stamp_request_id(origin, request_id_);
    log_exchange_identity(origin);
    // 策略在发起那一刻读定：本次交换的每一跳与缓存命中都问这一份，之后改 `Client::policy()` 或进程级默认
    // 都不影响已在途的交换（判定用的是「出发时」的策略，不是「回投时」的策略）。
    const NetPolicy security = effective_policy(policy_);
    // 起手策略闸排在查表之前：命中缓存照样是一次交付，不能因为它就没被问过。
    if (const auto verdict = gate_origin(proxy_, tls_, security, origin); !verdict) {
        co_return Result<Response>{verdict.error()};
    }
    // 缓存起手式：命中新鲜副本时一条连接都不发起就交出——§12 承诺的「同一 URL 二次请求不触网」正是这一条。
    // 命中陈旧副本时把两枚条件头挂在**原始请求**上，故重试环的每一次尝试都带着同一份校验子重跑（挂在逐跳
    // 副本上的话，第二次尝试就成了「第一次验证、第二次裸打」）。
    CacheOpening opening = open_from_cache(cache_, cache_options_, origin);
    if (opening.fresh.has_value()) {
        co_return Result<Response>{std::move(*opening.fresh)};
    }
    const bool loop_open = detail::retry_enabled(policy) && detail::is_idempotent_method(origin.method());

    // 跨全部尝试（含退避等待）的墙登记在这一枚令牌上；每一次尝试各拿一份自己的令牌与 `total` 看门狗，
    // 故 `Timeouts::total` 天然逐尝试重新计时——这正是它与 `RetryPolicy::budget` 分家的落点。
    // 环不开时 `budget` 一并摘掉：一枚「跨全部尝试」的墙在没有第二次尝试时不该变成单段超时。
    Cancellation sequence;
    auto wall = std::make_shared<TimeoutState>();
    StageTimer budget_timer(loop_open ? policy.budget : std::optional<std::chrono::milliseconds>{},
                            TimeoutStage::Budget, sequence, wall);
    Result<Response> outcome{make_error(ErrorCode::HttpProtocolError, "send produced no outcome")};

    // 重试环与单次交换**同帧**：`CoroTask` 不是 awaitable（`state/coroutine.h` 的 promise 永不首挂也不终挂，
    // 结果只经 `is_done()` / `result()` 轮询），故「把一次尝试抽成子协程再 co_await」在本库无对应机制——
    // 这正是本文件开头「刻意不做抽象」那条约束在重试这一层的再确认。
    for (std::uint32_t attempt = 1;; ++attempt) {
        Cancellation token;
        // 墙到点即掐掉在途的这一次尝试（`Cancellation` 契约：钩子至多触发一次），随后本环据 `is_cancelled()` 收摊。
        sequence.on_cancel([token]() mutable { token.cancel(); });
        Request current = origin;  // 每次尝试都从原始请求重新发起：重定向与重放都会改写它
        advertise_accept_encoding(current);  // 缺省宣告与解码能力同源；调用方已自带则不动
        // 解压产物上限缺省跟随线上体上限：两枚旋钮各管一段字节，未显式收紧即「压缩不该让物化结果更多」。
        // 跟随的是「设了没有」而非数值——两枚都未设即恒不设限（`nullopt`）。
        const std::optional<std::uint64_t> decompressed_limit =
            max_decompressed_bytes_.has_value() ? max_decompressed_bytes_ : max_body_bytes_;
        const Timeouts timeouts = current.timeouts();
        const int limit = current.max_redirects();
        int hops = 0;
        Result<Response> one{make_error(ErrorCode::HttpProtocolError, "send produced no outcome")};
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
            // ── 路由先于渲染：请求行写成哪种形态，取决于本跳的对端是目标本身还是转发代理。
            const auto &url_slot = current.url();
            if (!url_slot.has_value()) {
                // 目标解析失败在工厂就已记进对象，这里借 render_head 把那条错误原样交出，不另编一句。
                auto undeliverable = current.render_head();
                one = Result<Response>{undeliverable.error()};
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
                one = Result<Response>{make_error(ErrorCode::HttpProtocolError, "target has no effective port")};
                break;
            }
            // 策略判定与代理决策同在这一处完成（`Direct` 与其余形态共用同一条建连路，见 `detail::await_connect_via`）：
            // 故「有没有经代理」「这跳能不能发」在库里只有一个答案，且被拒时还轮不到渲染与建连。
            auto routed = plan_route(proxy_, tls_, security, target, *port);
            if (!routed) {
                one = Result<Response>{routed.error()};
                break;
            }
            const HopRoute &route = routed.value();
            // ── 写出物化：请求头部 + 体（render_head 失败即字段畸形，一条连接都不发起、原样交出）。
            auto head = current.render_head(route.absolute_form);
            if (!head) {
                one = Result<Response>{head.error()};
                break;
            }
            // TLS 握手与证书校验的对象恒是目标主机（隧道之上再握手），代理看不见这些。
            const std::string host{target.host()};

            // ── 建连并统一读写口：明文 / TLS 两条流经捕获 shared_ptr 的类型擦除闭包收敛到同一对 awaiter。
            std::function<detail::CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
            std::function<detail::CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
            StageTimer connect_timer(timeouts.connect, TimeoutStage::Connect, token, tstate);
            if (target.is_secure()) {
                auto plain = co_await detail::await_connect_via(route.connect_with, route.peer_host, route.peer_port,
                                                                options, token);
                if (!plain) {
                    one = Result<Response>{plain.error()};
                    break;
                }
                auto secure = co_await detail::await_handshake(std::move(plain.value()), host, tls_, token);
                if (!secure) {
                    one = Result<Response>{secure.error()};
                    break;
                }
                auto holder = std::make_shared<TlsTransport>(std::move(secure.value()));
                read_one = [holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
                write_one = [holder](std::span<const char> data) { return detail::await_write(*holder, data); };
            } else {
                auto plain = co_await detail::await_connect_via(route.connect_with, route.peer_host, route.peer_port,
                                                                options, token);
                if (!plain) {
                    one = Result<Response>{plain.error()};
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
                    one = Result<Response>{written_head.error()};
                }
                while (!write_failed) {
                    const std::size_t take = std::min<std::size_t>(AURORA_WRITE_CHUNK, body_view.size() - uploaded);
                    auto written = co_await write_one(body_view.subspan(uploaded, take));
                    if (!written) {
                        one = Result<Response>{written.error()};
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
                    one = Result<Response>{written.error()};
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
                    announced_total = assembler.framing() == wire::BodyFraming::Length
                                          ? announced_length(assembler.head())
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
                    one = Result<Response>{read.error()};
                    halted = true;
                    break;
                }
                const std::size_t got = read.value();
                if (got == 0) {  // 对端 FIN：UntilClose 据此判全，其余形态提前收摊即协议错位
                    auto closed = assembler.peer_closed();
                    if (!closed) {
                        one = Result<Response>{closed.error()};
                        halted = true;
                        break;
                    }
                    if (!assembler.complete()) {
                        one = Result<Response>{
                            make_error(ErrorCode::HttpProtocolError, "peer closed before the body was complete")};
                        halted = true;
                        break;
                    }
                    break;
                }
                auto pushed = assembler.push(std::string_view{chunk.data(), got});
                if (!pushed) {
                    one = Result<Response>{pushed.error()};
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
                one = Result<Response>{decoded.error()};
                break;
            }
            one = Result<Response>{
                Response{response_head.status, response_head.reason, response_head.headers, std::move(body)}};

            // ── 重定向判定。
            if (!is_redirect(response_head.status) || limit == 0) {
                break;
            }
            if (++hops > limit) {
                one = Result<Response>{
                    make_error(ErrorCode::HttpRedirectLimit, "redirect chain exceeded the configured limit")};
                break;
            }
            auto next = follow(current, one.value());
            if (!next) {
                one = Result<Response>{next.error()};
                break;
            }
            current = std::move(next.value());
        }

        // 超时改道：失败若因某档看门狗取消了令牌，一律改报 http-timeout，并在 detail 标明是哪一档。
        if (!one) {
            switch (static_cast<TimeoutStage>(tstate->fired.load())) {
                case TimeoutStage::Connect:
                    one = Result<Response>{make_error(ErrorCode::HttpTimeout, "connect timeout")};
                    break;
                case TimeoutStage::ReadIdle:
                    one = Result<Response>{make_error(ErrorCode::HttpTimeout, "read-idle timeout")};
                    break;
                case TimeoutStage::Total:
                    one = Result<Response>{make_error(ErrorCode::HttpTimeout, "total timeout")};
                    break;
                case TimeoutStage::Budget:  // 跨尝试的墙登记在外层，本段的 `tstate` 永远写不进这一档
                case TimeoutStage::None:
                    break;
            }
        }

        // 进度收尾：两个方向的末值各发一次，不看闸号。成败由 `one` 表达，这一发只宣告「这一趟到此收摊」，
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

        // ── 重试裁决。非 2xx 是「成功的 Result」：状态码归 `should_retry_status` 判，尝试耗尽时把最后那次
        //    应答原样交出，绝不折成新错误；传输失败归 `should_retry_error` 判，耗尽时交出的同样是最后一枚
        //    底层错误。两枚时限旋钮的分工落在这里：`Timeouts::total` 逐尝试重新计时，`RetryPolicy::budget`
        //    是跨全部尝试与全部退避等待的一堵墙，到点即 `sequence.is_cancelled()`，本环就此收摊。
        const bool has_next = loop_open && attempt < policy.attempts && !sequence.is_cancelled();
        std::optional<std::chrono::milliseconds> wait;
        if (one) {
            if (!has_next || !detail::should_retry_status(one.value().status())) {
                outcome = std::move(one);
                break;
            }
            wait = detail::wait_delay(policy, attempt, retry_after_of(one.value()));
        } else {
            outcome = std::move(one);
            if (!has_next || !detail::should_retry_error(outcome.error())) {
                break;
            }
            wait = detail::wait_delay(policy, attempt, std::nullopt);
        }
        AURORA_LOG_INFO("net", "http retry: attempt ", attempt, " of ", policy.attempts, " failed, waiting ",
                        wait->count(), "ms before retry");
        co_await detail::await_sleep(*wait, sequence);
        if (sequence.is_cancelled()) {
            break;  // 墙在退避等待期间到点：剩余尝试不再发起
        }
    }

    // 被跨尝试的墙掐掉的尝试以 `net-cancelled` 形态抵达，那既不是对端说的也不是本库造的码——按 `total`
    // 档的先例改报 `http-timeout` 并在 detail 点名是 budget，使「超时」在两枚旋钮之间仍可分辨。
    if (!outcome && wall->fired.load() == static_cast<int>(TimeoutStage::Budget)) {
        outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "retry budget exhausted")};
    }
    // 收尾进表 / 换 304：一次交换至多写一次，写在重试环之外——中间那次尝试的应答不该进表。
    settle_cache(cache_, cache_options_, origin, opening.key, opening.revalidating, outcome);
    co_return outcome;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): Sink 由调用方持有，帧只借用
auto Client::open_stream(Request request, DataSink &sink) const -> CoroTask<Result<StreamOutcome>> {
    // 策略取「请求覆盖 → 客户端缺省」两级；形态闸在起手就过，越界值不进入任何一次尝试。
    const RetryPolicy policy = request.retry_policy().value_or(retry_);
    if (const auto invalid = detail::validate_retry_policy(policy)) {
        co_return Result<StreamOutcome>{*invalid};
    }
    // 本腿无流式语义（wasm 无 ReadableStream）时直接失败，绝不悄悄退化成物化路径：调用方选 `open_stream`
    // 要的就是「整条载荷不必同时存在于内存」，退化成交换一份 `Response` 是把它的内存预算和预期一起毁掉。
    if (!Engine::instance().capabilities().streaming) {
        co_return Result<StreamOutcome>{
            make_error(ErrorCode::NetPlatformUnavailable, "open_stream: this leg has no streaming response body")};
    }

    const bool loop_open = detail::retry_enabled(policy) && detail::is_idempotent_method(request.method());

    // 跨全部尝试（含退避等待）的墙登记在这一枚令牌上；每一次尝试各拿一份自己的令牌与 `total` 看门狗，
    // 故 `Timeouts::total` 天然逐尝试重新计时——这正是它与 `RetryPolicy::budget` 分家的落点。
    // 环不开时 `budget` 一并摘掉：一枚「跨全部尝试」的墙在没有第二次尝试时不该变成单段超时。
    Cancellation sequence;
    auto wall = std::make_shared<TimeoutState>();
    StageTimer budget_timer(loop_open ? policy.budget : std::optional<std::chrono::milliseconds>{},
                            TimeoutStage::Budget, sequence, wall);
    Request origin = std::move(request);
    // 链路标识与 `send` 同一处落一次：本入口的每一跳与每一次尝试都带同一串出去。
    stamp_request_id(origin, request_id_);
    log_exchange_identity(origin);
    // 策略读定与起手闸同 `send`：发起那一刻取一份，逐跳共用；闸排在查表之前，缓存命中同样是交付。
    const NetPolicy security = effective_policy(policy_);
    if (const auto verdict = gate_origin(proxy_, tls_, security, origin); !verdict) {
        co_return Result<StreamOutcome>{verdict.error()};
    }
    // 缓存起手式：命中新鲜副本时把已存的那份体按段交给同一个宿主，一条连接都不发起。交付次序与真取回时一致
    // （`on_head` → 若干次 `on_data` → `on_end`），`wire_bytes` 恒 0——本次确实没有线上字节，不是把计数漏掉了。
    //
    // 本入口**只读不写**：把流式载荷攒进表就意味着先把它攒进内存，那正是调用方选 `open_stream` 要避免的事。
    // 于是命中之外的另一条分岔也在这里定：陈旧条目挂上条件头去验证，拿到 304 仍要合并、仍要把已存那份体逐段
    // 交出去，但这次交换的结果不进表。
    const CacheOpening opening = open_from_cache(cache_, cache_options_, origin);
    if (opening.fresh.has_value()) {
        co_return Result<StreamOutcome>{deliver_cached_stream(sink, *opening.fresh)};
    }
    // 环关时没有壳可言：把调用方的宿主原样交下去，`on_head` / `on_data` / `on_end` 的调用对象与装环前完全
    // 同一个（少一层转发也就少一层「壳改变了宿主身份」的疑义）。环开着才套上记录头口的壳。
    HeadGuardSink guard{sink};  // 只借用：宿主由调用方持有，本壳随本帧存活到全部尝试结束
    DataSink &outlet = loop_open ? static_cast<DataSink &>(guard) : sink;
    Result<StreamOutcome> outcome{make_error(ErrorCode::HttpProtocolError, "open_stream produced no outcome")};

    // 重试环与单次交换**同帧**：`CoroTask` 不是 awaitable（`state/coroutine.h` 的 promise 永不首挂也不终挂，
    // 结果只经 `is_done()` / `result()` 轮询），故「把一次尝试抽成子协程再 co_await」在本库无对应机制。
    for (std::uint32_t attempt = 1;; ++attempt) {
        Cancellation token;
        // 墙到点即掐掉在途的这一次尝试（`Cancellation` 契约：钩子至多触发一次），随后本环据 `is_cancelled()` 收摊。
        sequence.on_cancel([token]() mutable { token.cancel(); });
        Request current = origin;
        advertise_accept_encoding(current);  // 缺省宣告与解码能力同源；调用方已自带则不动
        // 解压产物上限**只取 `max_decompressed_bytes()`**，不像 `send` 那样缺省跟随 `max_body_bytes()`：后者是
        // 物化路径的内存闸，流式路径不物化，拿它当解压上限会让「流式下载一个大压缩包」必须去调大一枚与内存无关
        // 的旋钮。两枚旋钮在流式路径各管各的：线上的一个都不设，还原的照本旋钮设。
        const std::optional<std::uint64_t> decompressed_limit = max_decompressed_bytes_;
        const Timeouts timeouts = current.timeouts();
        const int limit = current.max_redirects();
        int hops = 0;
        Result<StreamOutcome> one{make_error(ErrorCode::HttpProtocolError, "open_stream produced no outcome")};
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
            // ── 路由先于渲染：请求行形态取决于本跳的对端（与 `send` 同判据）。
            const auto &url_slot = current.url();
            if (!url_slot.has_value()) {
                auto undeliverable = current.render_head();
                one = Result<StreamOutcome>{undeliverable.error()};
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
                one = Result<StreamOutcome>{make_error(ErrorCode::HttpProtocolError, "target has no effective port")};
                break;
            }
            // 策略判定与代理决策同在这一处完成；被拒时连头部都没渲染，更没建连。
            auto routed = plan_route(proxy_, tls_, security, target, *port);
            if (!routed) {
                one = Result<StreamOutcome>{routed.error()};
                break;
            }
            const HopRoute &route = routed.value();
            // ── 写出物化：请求头部 + 体（与 `send` 同判据，含「挂了回调才分块写」那条）。
            auto head = current.render_head(route.absolute_form);
            if (!head) {
                one = Result<StreamOutcome>{head.error()};
                break;
            }
            // TLS 握手与证书校验的对象恒是目标主机（隧道之上再握手），代理看不见这些。
            const std::string host{target.host()};

            // ── 建连并统一读写口：明文 / TLS 两条流经捕获 shared_ptr 的类型擦除闭包收敛到同一对 awaiter。
            std::function<detail::CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
            std::function<detail::CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
            StageTimer connect_timer(timeouts.connect, TimeoutStage::Connect, token, tstate);
            if (target.is_secure()) {
                auto plain = co_await detail::await_connect_via(route.connect_with, route.peer_host, route.peer_port,
                                                                options, token);
                if (!plain) {
                    one = Result<StreamOutcome>{plain.error()};
                    break;
                }
                auto secure = co_await detail::await_handshake(std::move(plain.value()), host, tls_, token);
                if (!secure) {
                    one = Result<StreamOutcome>{secure.error()};
                    break;
                }
                auto holder = std::make_shared<TlsTransport>(std::move(secure.value()));
                read_one = [holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
                write_one = [holder](std::span<const char> data) { return detail::await_write(*holder, data); };
            } else {
                auto plain = co_await detail::await_connect_via(route.connect_with, route.peer_host, route.peer_port,
                                                                options, token);
                if (!plain) {
                    one = Result<StreamOutcome>{plain.error()};
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
                    one = Result<StreamOutcome>{written_head.error()};
                }
                while (!write_failed) {
                    const std::size_t take = std::min<std::size_t>(AURORA_WRITE_CHUNK, body_view.size() - uploaded);
                    auto written = co_await write_one(body_view.subspan(uploaded, take));
                    if (!written) {
                        one = Result<StreamOutcome>{written.error()};
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
                    one = Result<StreamOutcome>{written.error()};
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
            bool not_modified_hop = false;  // 本次交换替表里的条目重验证并拿到了 304

            // 产出出口：链末每交一段明文记一次交付量。宿主回 `Stop` 只中止交付，本库不替它决定成败。
            auto emit = [&outlet, &delivered_bytes](std::span<const char> bytes) -> Result<detail::EmitFlow> {
                delivered_bytes += bytes.size();
                const auto flow = outlet.on_data(bytes);
                return Result<detail::EmitFlow>{flow == SinkFlow::Stop ? detail::EmitFlow::Stop
                                                                       : detail::EmitFlow::Continue};
            };

            while (!assembler.complete()) {
                StageTimer read_timer(timeouts.read_idle, TimeoutStage::ReadIdle, token, tstate);
                auto read = co_await read_one(std::span<char>{chunk});
                read_timer.stop();
                if (!read) {
                    one = Result<StreamOutcome>{read.error()};
                    halted = true;
                    break;
                }
                const std::size_t got = read.value();
                if (got == 0) {  // 对端 FIN：UntilClose 据此判全，其余形态提前收摊即协议错位
                    auto closed = assembler.peer_closed();
                    if (!closed) {
                        one = Result<StreamOutcome>{closed.error()};
                        halted = true;
                        break;
                    }
                    if (!assembler.complete()) {
                        one = Result<StreamOutcome>{
                            make_error(ErrorCode::HttpProtocolError, "peer closed before the body was complete")};
                        halted = true;
                        break;
                    }
                    break;
                }
                auto pushed = assembler.push(std::string_view{chunk.data(), got});
                if (!pushed) {
                    one = Result<StreamOutcome>{pushed.error()};
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
                    announced_total = assembler.framing() == wire::BodyFraming::Length
                                          ? announced_length(assembler.head())
                                          : std::optional<std::uint64_t>{};
                    if (is_redirect(assembler.head().status) && limit != 0) {
                        // 重定向跳：头部一判定即弃该跳载荷、直接发起下一跳。一条连接一个跳，收摊由 holder 的
                        // 作用域结束完成；`send` 逐跳物化，本入口不为注定被丢弃的体继续读字节。
                        if (++hops > limit) {
                            one = Result<StreamOutcome>{make_error(ErrorCode::HttpRedirectLimit,
                                                                   "redirect chain exceeded the configured limit")};
                            halted = true;
                            break;
                        }
                        redirect_hop = true;
                        break;
                    }
                    if (assembler.head().status == 304 && opening.revalidating) {
                        // 条件请求拿到 304：那是本库替表里的条目问的，不该让宿主看见这枚中间状态——把新头并进
                        // 已存应答，连同它的体一起按段交出。此处既不建解码链也不调 `on_head`，交付留到环外。
                        not_modified_hop = true;
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
                                one = Result<StreamOutcome>{parsed.error()};
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
                        one = Result<StreamOutcome>{created.error()};
                        halted = true;
                        break;
                    }
                    decoder = std::move(created.value());

                    final_head =
                        Response{assembler.head().status, assembler.head().reason, assembler.head().headers, {}};
                    if (outlet.on_head(*final_head) == SinkFlow::Stop) {
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
                        one = Result<StreamOutcome>{emitted.error()};
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
            // 这一种形态。中途收摊（宿主 `Stop`）、弃体换跳（重定向）与 304 都不兜——前者不能再交付字节，后两者的
            // 体本就作废（重定向的是下一跳，304 的载荷在表里）。
            if (!redirect_hop && !not_modified_hop && !stopped_by_sink) {
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
                        one = Result<StreamOutcome>{emitted.error()};
                        break;
                    }
                    stopped_by_sink = emitted.value() == detail::EmitFlow::Stop;
                }
            }

            if (redirect_hop) {
                Response hop{assembler.head().status, assembler.head().reason, assembler.head().headers, {}};
                auto next = follow(current, hop);
                if (!next) {
                    one = Result<StreamOutcome>{next.error()};
                    break;
                }
                current = std::move(next.value());
                continue;
            }
            if (not_modified_hop) {
                // 合并路径：304 的头部并进表里那条，状态码与体仍取已存那份，再按段交给宿主。条目已被逐出
                // 就无从合并——那只能交出对端说的那份 304（它按定义没有体），编一份体比交出真相更糟。
                Response update{assembler.head().status, assembler.head().reason, assembler.head().headers, {}};
                std::optional<Response> merged;
                if (cache_) {
                    merged = cache_->revalidated(origin.method(), opening.key, origin.headers(), update,
                                                 std::chrono::system_clock::now());
                }
                if (merged.has_value()) {
                    one = Result<StreamOutcome>{deliver_cached_stream(outlet, merged.value())};
                    break;
                }
                final_head = Response{update.status(), std::string{update.reason()}, update.headers(), {}};
                stopped_by_sink = outlet.on_head(*final_head) == SinkFlow::Stop;
                outlet.on_end();
                one = Result<StreamOutcome>{StreamOutcome{.response = std::move(*final_head),
                                                          .wire_bytes = wire_bytes,
                                                          .delivered_bytes = delivered_bytes,
                                                          .stopped_by_sink = stopped_by_sink}};
                break;
            }
            if (!final_head.has_value()) {
                one = Result<StreamOutcome>{
                    make_error(ErrorCode::HttpProtocolError, "stream ended before the response head was settled")};
                break;
            }
            if (!stopped_by_sink) {
                // 收摊链：排出各环残留产物并逐环验证流完整终结。失败即字节断在半途，据此不调 `on_end`。
                auto settled = decoder ? decoder->finish() : Result<detail::EmitFlow>{detail::EmitFlow::Continue};
                if (!settled) {
                    one = Result<StreamOutcome>{settled.error()};
                    break;
                }
                // 收摊时回 `Stop` 同样是「宿主不要了」这一事实（此时线上字节已读完，只是末段交付被掐）。
                stopped_by_sink = settled.value() == detail::EmitFlow::Stop;
            }
            outlet.on_end();
            one = Result<StreamOutcome>{StreamOutcome{.response = std::move(*final_head),
                                                      .wire_bytes = wire_bytes,
                                                      .delivered_bytes = delivered_bytes,
                                                      .stopped_by_sink = stopped_by_sink}};
            break;
        }

        // 超时改道：与 `send` 同判据——失败若因某档看门狗取消了令牌，一律改报 http-timeout 并标明是哪一档。
        if (!one) {
            switch (static_cast<TimeoutStage>(tstate->fired.load())) {
                case TimeoutStage::Connect:
                    one = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "connect timeout")};
                    break;
                case TimeoutStage::ReadIdle:
                    one = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "read-idle timeout")};
                    break;
                case TimeoutStage::Total:
                    one = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "total timeout")};
                    break;
                case TimeoutStage::Budget:  // 跨尝试的墙登记在外层，本段的 `tstate` 永远写不进这一档
                case TimeoutStage::None:
                    break;
            }
        }

        // 进度收尾：与 `send` 同判据，两方向末值各发一次、不看闸号；成败由 `one` 表达。
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

        // ── 重试裁决（与 `send` 同判据，另加两条流式专属资格）。非 2xx 仍是成功的 Result：尝试耗尽时把最后
        //    那次应答连同已交出的字节一起交出，绝不折成新错误。流式路径的资格在 `on_head` 那一次转发时即耗尽：
        //    首段字节交出去就收不回来，重播等于让宿主把已写下那段再写一遍；`stopped_by_sink` 是宿主自己收的摊，
        //    同样不重放——再打一次是替它改变决定。
        const bool has_next = loop_open && attempt < policy.attempts && !sequence.is_cancelled() && !guard.handed_off();
        std::optional<std::chrono::milliseconds> wait;
        if (one) {
            if (!has_next || one.value().stopped_by_sink ||
                !detail::should_retry_status(one.value().response.status())) {
                outcome = std::move(one);
                break;
            }
            wait = detail::wait_delay(policy, attempt, retry_after_of(one.value().response));
        } else {
            outcome = std::move(one);
            if (!has_next || !detail::should_retry_error(outcome.error())) {
                break;
            }
            wait = detail::wait_delay(policy, attempt, std::nullopt);
        }
        AURORA_LOG_INFO("net", "http stream retry: attempt ", attempt, " of ", policy.attempts, " failed, waiting ",
                        wait->count(), "ms before retry");
        co_await detail::await_sleep(*wait, sequence);
        if (sequence.is_cancelled()) {
            break;  // 墙在退避等待期间到点：剩余尝试不再发起
        }
    }

    // 被跨尝试的墙掐掉的尝试以 `net-cancelled` 形态抵达，那既不是对端说的也不是本库造的码——按 `total`
    // 档的先例改报 `http-timeout` 并在 detail 点名是 budget，使「超时」在两枚旋钮之间仍可分辨。
    if (!outcome && wall->fired.load() == static_cast<int>(TimeoutStage::Budget)) {
        outcome = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "retry budget exhausted")};
    }
    co_return outcome;
}
}  // namespace aurora::net::http
