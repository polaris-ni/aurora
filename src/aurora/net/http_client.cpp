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
//
// 第三条发起腿 `connect_websocket` 也住在本文件，因为它要复用本文件私有的一半地基：策略三级取定、路由
// (`plan_route`) 与起手闸 (`gate_origin`)、链路标识与 Cookie 附着。那几件在库内各只有一份实现（「有没有经
// 代理」这个答案全库只有一个，见 `HopRoute` 的注释），搬到别处就得要么复制一份、要么把它们公开成库内接口。
// 它与另两腿**不共用骨架**：应答侧的装配器、重定向环、重试环、缓存表与分帧解码一概不进来（判据见
// `Client::connect_websocket()` 的文档），握手一结算就把连接交给 `detail::WsSession`，此后不再有交换可言。
#include "aurora/net/http_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_auth_wire.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/net/detail/http_body.h"
#include "aurora/net/detail/http_cache.h"
#include "aurora/net/detail/http_cookie_jar.h"
#include "aurora/net/detail/http_encoding.h"
#include "aurora/net/detail/http_fetch.h"
#include "aurora/net/detail/http_pool.h"
#include "aurora/net/detail/http_progress.h"
#include "aurora/net/detail/http_request_id.h"
#include "aurora/net/detail/http_retry.h"
#include "aurora/net/detail/http_wire.h"
#include "aurora/net/detail/random_draw.h"
#include "aurora/net/detail/ws_handshake.h"
#include "aurora/net/detail/ws_session.h"
#include "aurora/net/engine.h"
#include "aurora/net/http.h"
#include "aurora/net/tls.h"
#include "aurora/net/transport.h"
#include "aurora/net/websocket.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::http {
namespace {

/// @brief 单段读取的投喂缓冲字节数：足够让常见的头部 + 首块一次到位，又不无界占用栈帧。
constexpr std::size_t AURORA_READ_CHUNK = std::size_t{16} * 1024;

/// @brief 「先问再发体」（`Expect: 100-continue`）那一段等待的窗口时长。
///
/// 它是库内置常量而非 `Timeouts` 的任何一档，理由是它管的事与三档都不同：三档到点都是失败（取消在途操作、
/// 落 `http-timeout`），而本窗口到点是**成功路径的另一条腿**——多数中间层压根不回 `100`，盲发是 `RFC 9110
/// §10.1.1` 明确允许的形态。把它做成一档超时会把「没人应话」误报成超时，而它其实只是走了另一条腿。
/// 取 300 ms 是「诚实的服务器多半在这之内回话」与「不该让整条请求为一次大概无疾而终的问询买单」之间的
/// 折中；它只加在挂过 `expect_continue()` 且本次确有体的请求上。
constexpr std::chrono::milliseconds AURORA_EXPECT_WINDOW{300};

/// @brief 认证环两次轮询之间的让出时长。
///
/// 提供方的出口是协程，而 `CoroTask` 不可 `co_await`（`state/coroutine.h`：promise 永不首挂也不终挂，结果只经
/// `is_done()` / `result()` 轮询），故本环只能轮询。每次让出把控制权交回宿主事件循环，提供方内部那次 token
/// 换取的续体才投递得回来。内置件从不挂起，`is_done()` 当场为真，本间隔一次都用不上。
constexpr std::chrono::milliseconds AURORA_AUTH_POLL_INTERVAL{5};

/// @brief 一次问答的让出次数上限（× 上面的间隔 ≈ 25 秒）；到点落 `http-timeout` 并在 detail 点名提供方。
///
/// 这是库内置的兜底，不是第四枚超时旋钮：`Timeouts` 各档管的是「这条请求慢不慢」，而本环卡住的原因是「凭据
/// 提供方没回话」——那是另一件事，且现实里它几乎总在等一个永远不会回来的 token 端点。上限取得明显大于任何合理
/// 的换取耗时，为的是不误杀真在换取的实现方，同时不给整条协程留下静默悬挂的余地。
constexpr std::size_t AURORA_AUTH_POLL_CEILING = 5000;

/// @brief 连接池排队两次探测之间的让出时长。
///
/// 名额满时取空闲连接只能轮询等（空闲连接由并发的另一条交换归还），每次让出把控制权交回宿主事件循环，
/// 那条交换的归还才发生得进来。等待的墙是 `Timeouts::connect` 档（排队计入建连预算，见 `PoolOptions`
/// 头注释），到点即停——本间隔只是节奏，不是第二枚超时。
constexpr std::chrono::milliseconds AURORA_POOL_QUEUE_POLL{5};

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
    auto stop() const -> void {
        if (live_ != nullptr) {
            live_->store(false);
        }
    }

  private:
    std::shared_ptr<std::atomic<bool>> live_;  ///< 本段是否仍在计时；空 = 未登记（该档不设限）
};

/// @brief HTTP 字段名的比较口径：ASCII 大小写不敏感（`RFC 9110 §5.1`）。
///
/// `Headers` 内部同一枚判据是私有的（它的查找本就大小写不敏感），本处需要的是「拿着条目名逐条比」的那一侧，
/// 故就地补一枚等价的短函数——照抄那一份口径，避免出现两套字段名比较。
/// @param left 一侧名字。
/// @param right 另一侧名字。
/// @return 折叠大小写后逐字符相等时为 true。
[[nodiscard]] auto field_name_equal(std::string_view left, std::string_view right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](char unit) -> char {
            return (unit >= 'A' && unit <= 'Z') ? static_cast<char>(unit - 'A' + 'a') : unit;
        };
        if (fold(left.at(index)) != fold(right.at(index))) {
            return false;
        }
    }
    return true;
}

/// @brief 两个 URL 是否同一 origin（`RFC 9110 §7.2`：方案、主机、有效端口任一不同即换了域）。
/// @param left 出发地。
/// @param right 目的地。
/// @return 同一 origin 为 true。
[[nodiscard]] auto same_origin(const Url &left, const Url &right) -> bool {
    return left.scheme() == right.scheme() && left.host() == right.host() &&
           left.effective_port() == right.effective_port();
}

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
///
/// 保留体的那几条（307 / 308，以及非 POST 的 301/302/303）会把体连同三枚现地补字段的旋钮一起带到下一跳，
/// 并在带走流式体时先把它放回原点——放不回即落 `http-body-not-replayable`，因为下一跳注定要整份重发。
///
/// 换 origin 的那一跳剥掉两枚凭据字段（`Authorization` / `Cookie`），判据与口径见 `net/http_auth.h` 的
/// 「凭据纪律」条；同 origin 的重定向照原样带走，认证环补上的那枚也因此跟着后续每一跳。
/// @param current 本跳请求。
/// @param response 本跳拿到的重定向应答。
/// @return 下一跳请求；无 `Location`、目标解不出、或体不可重放时为对应错误。
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
    // 换 origin 就不带凭据（`RFC 9110 §15.4.4` 那条纪律，形态见 `net/http_auth.h`）：`Authorization` 是上一台
    // 机器给的口令，跟着 302 送到另一台机器上就是把凭据交给一个陌生对端。`Cookie` 同理——Jar 属 Session 切片，
    // 本阶段它只会以调用方自填的请求字段形态出现，那就一并剥掉。同 origin 的跳原样带走，认证环补上的那枚
    // 因此跟着后续每一跳与每一次重试。
    const bool leaves_origin = !same_origin(base.value(), next_url);
    for (const auto &entry : current.headers().entries()) {
        if (leaves_origin &&
            (field_name_equal(entry.name, "Authorization") || field_name_equal(entry.name, "Cookie"))) {
            continue;
        }
        redirected.header(entry.name, entry.value);
    }
    if (keep_body) {
        // 体的两种交法都跟着跳走：内存体是值拷贝，流式体是**同一枚源**（`Request` 只借引用，见 net/http.h）。
        // 源在这里必须回到原点——下一跳要把它整份重发一遍，接着半截往下发等于交付一份损坏的请求体。
        // `restart()` 的答案就是重放资格的唯一判据（同 `RetryPolicy` 那一闸），答「回不去」即这一跳跳不动：
        // 与其发出一份注定错误的体，不如把失败如实交回调用方。
        if (auto *source = current.body_source(); source != nullptr) {
            if (!source->restart()) {
                return Result<Request>{make_error(ErrorCode::HttpBodyNotReplayable,
                                                  "a streaming request body cannot be replayed for the next hop of a "
                                                  "redirect that preserves it")};
            }
            redirected.body(*source);
        } else if (!current.body().empty()) {
            redirected.body(current.body());
        }
        // 三枚旋钮都靠 `render_head()` 现地补成字段，故它们不在上面的字段拷贝里；不带过来，第二跳就会悄悄
        // 变成「整份资源 + 直接发体」——那是把调用方的请求改写成了另一件事。
        if (const auto &value = current.range(); value.has_value()) {
            redirected.range(*value);
            if (const auto &validator = current.if_range(); validator.has_value()) {
                redirected.if_range(*validator);
            }
        }
        redirected.expect_continue(current.expect_continue());
    }
    return Result<Request>{std::move(redirected)};
}

/// @brief 认证环的触发判据：401（源站要凭据）与 407（代理要凭据）。
///
/// 两枚状态码说的是同一件事的两侧——「这一跳要凭据」，差别只在问的人是源站还是代理，而那一侧由挑战自己带上
/// （`AuthChallenge::proxy`，取值就是「状态码是不是 407」）。
/// @param status 应答状态码。
/// @return 需要问一次凭据时为 true。
[[nodiscard]] auto is_auth_challenge(std::uint16_t status) -> bool { return status == 401 || status == 407; }

/// @brief 挑战字段名：源站问 `WWW-Authenticate`，代理问 `Proxy-Authenticate`。
/// @param proxy 挑战是否来自代理。
/// @return 字段名原文。
[[nodiscard]] auto auth_challenge_field(bool proxy) -> std::string_view {
    return proxy ? std::string_view{"Proxy-Authenticate"} : std::string_view{"WWW-Authenticate"};
}

/// @brief 答复字段名：源站补 `Authorization`，代理补 `Proxy-Authorization`。
/// @param proxy 挑战是否来自代理。
/// @return 字段名原文。
[[nodiscard]] auto auth_credential_field(bool proxy) -> std::string_view {
    return proxy ? std::string_view{"Proxy-Authorization"} : std::string_view{"Authorization"};
}

/// @brief 一次问答里，提供方的答复落成了哪种动作（口径见 `net/http_auth.h` 的五条裁决）。
enum class AuthReply : std::uint8_t {
    Declined,  ///< 答空，或代理挑战上没给出那枚字段：把对端说的那份 401 / 407 原样交出
    Origin,  ///< 源站挑战的答复已并回原始请求：原地重跑本次尝试
    Proxy,  ///< 代理挑战的答复已取出：原地重跑，且只写面向代理那一侧
    Malformed,  ///< 字段名或值里带着 CR/LF：那是写不上线的字段，本次交换就此落 `http-request-invalid`
};

/// @brief 一次交换的认证环状态（随协程帧存活，跨全部跳与全部尝试共用一份）。
struct AuthRing {
    std::shared_ptr<Authenticator> provider;  ///< 提供方（空 = 本环整环不存在）
    bool asked{false};  ///< 本次交换是否已问过：至多一次，答复如何都不问第二遍
    std::optional<std::string> proxy_answer;  ///< 代理答复原文：只写面向代理那一侧，绝不进隧道里的请求
};

/// @brief 装配交给提供方的那份挑战：应答里的 401 / 407 与被拒的 `CONNECT` 在此合一。
/// @param request 被拒的那一跳请求（方法与目标 URL 取自它）。
/// @param fields 挑战字段的全部值（不折叠，逐条各成一枚方案）。
/// @param status 触发本次问答的状态码。
/// @param host 问的那一侧的主机文本：源站挑战是目标主机，代理挑战是**代理**主机。
/// @return 挑战（一条方案都解析不出时 `mechanisms` 为空，仍照常问一次——「没得选」由实现方说，不由本库代答）。
[[nodiscard]] auto make_auth_challenge(const Request &request, const std::vector<std::string> &fields,
                                       std::uint16_t status, std::string_view host) -> AuthChallenge {
    AuthChallenge challenge;
    challenge.proxy = status == 407;
    challenge.status = status;
    challenge.host = std::string{host};
    challenge.method = request.method();
    if (const auto &target = request.url(); target.has_value()) {
        challenge.url = target->to_string();
    }
    challenge.mechanisms = detail::parse_auth_mechanisms(fields);
    return challenge;
}

/// @brief 把答复落成一次交换里的一处落位；源站答复并回**原始请求**，代理答复只留在环上。
///
/// 并回原始请求而不是当前那一跳，是因为重跑与后续每一跳都从 `origin` 重新起副本：只改副本的话，第二次尝试
/// 又变成裸打，缓存键也看不见这份凭据（同一份凭据必须读到自己的副本，见 `net/http_auth.h` 的凭据纪律条）。
/// @param challenge 本次问出去的挑战（决定落位与字段）。
/// @param decision 提供方的答复。
/// @param origin 本次交换的原始请求（源站答复就地并进去）。
/// @param ring 本交换的认证环状态（代理答复写在这里）。
/// @return 本环接下来该做的动作。
[[nodiscard]] auto place_auth_decision(const AuthChallenge &challenge, const AuthDecision &decision, Request &origin,
                                       AuthRing &ring) -> AuthReply {
    if (decision.empty()) {
        return AuthReply::Declined;
    }
    // CR/LF 放进字段值，写出去的就是两条字段乃至一条请求行：那是把「对端要凭据」变成「对端可以借这只手往这条
    // 连接里塞话」。当场拒掉，而不是等下一跳的 `render_head()` 才发现——那时字节已在线上了。
    for (const auto &entry : decision.headers.entries()) {
        if (entry.name.find_first_of("\r\n") != std::string::npos ||
            entry.value.find_first_of("\r\n") != std::string::npos) {
            return AuthReply::Malformed;
        }
    }
    if (challenge.proxy) {
        // 代理挑战只认那一枚字段：其余即便写了也不会被送到任何一侧，等于没答复。
        const auto answer = decision.headers.get(auth_credential_field(true));
        if (!answer.has_value()) {
            return AuthReply::Declined;
        }
        ring.proxy_answer = *answer;
        return AuthReply::Proxy;
    }
    for (const auto &entry : decision.headers.entries()) {
        origin.set_header(entry.name, entry.value);  // 同名以答复为准：覆盖调用方原先写在请求上的那枚
    }
    return AuthReply::Origin;
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
        // 取不到系统熵时这一条就是不带标识，而不是带一枚可猜的：请求照常发出，缺失由那一行 WARN 记下
        // （口径见 `detail/http_request_id.h` 第三条）。调用方自带的标识与 `Request::request_id()` 不经此处。
        if (auto id = detail::make_request_id(); id) {
            request.header(detail::AURORA_HTTP_REQUEST_ID, id.value());
        } else {
            detail::note_entropy_unavailable("X-Request-Id");
        }
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
    bool tunneled_via_proxy{false};  ///< 本跳是否经 HTTP 代理建 `CONNECT` 隧道（那句 407 属代理挑战，见下文）
    std::string proxy_label;  ///< 代理主机文本（不经代理即空）：代理挑战里「谁在问」的就是这一侧
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
        route.proxy_label = decision.host();
    } else {
        route.connect_with = decision;
        // 只有 HTTP 代理的隧道那一侧会有 `CONNECT` 应答可解析，也就只有它会回一句 407；SOCKS5 的子协商没有
        // 挑战形态（凭据只能预防性给出），故不进认证环（裁决见 `net/http_auth.h` 的代理挑战条）。
        if (decision.kind() == ProxyKind::HttpConnect) {
            route.tunneled_via_proxy = true;
            route.proxy_label = decision.host();
        }
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

/// @brief fetch 腿该从 fetch init 里剥掉的请求头（连接管理与内容编码管理字段，浏览器自理）。
///
/// `Host` 与 `Accept-Encoding` 由浏览器按目标与解码能力自算；`Content-Length` / `Transfer-Encoding`
/// 由 fetch 按体自理；`Connection` 是逐跳语义，fetch 交换没有「连接」可管。剥错一侧的后果是浏览器
/// 要么静默丢弃（forbidden header）要么按自己的值重发——不如起手就不给。
constexpr std::array<std::string_view, 5> AURORA_FETCH_STRIPPED_FIELDS{"host", "content-length", "transfer-encoding",
                                                                       "connection", "accept-encoding"};

/// @brief 判断一枚请求头是否属于 fetch 腿要剥掉的那族（字段名比较大小写不敏感）。
/// @param name 请求头名。
/// @return 应剥除为 true。
[[nodiscard]] auto fetch_stripped_field(std::string_view name) -> bool {
    return std::ranges::any_of(AURORA_FETCH_STRIPPED_FIELDS,
                               [name](std::string_view candidate) { return field_name_equal(name, candidate); });
}

/// @brief wasm fetch 腿的读写适配器（§16 第 37 项）：挂在 `send` 的「建连并统一读写口」分支上。
///
/// fetch 是整单语义——没有逐段写出、没有 100-continue 问询、没有逐段读。适配器把这套差异收在
/// 三个闭包的形态里：**写**只记账即成功（请求在装配规格时已整单备好）；**带窗口的读**恒交
/// 「窗口内无应答」（发起侧据此照常发体，浏览器不等 100）；**读**在首次调用时把整单交给浏览器
/// `fetch`，结局合成线上字节流逐段交出、排空后交 0（FIN 形态）——写出环 / 读环 / 物化 / 重定向
/// 与原生腿共用同一份代码，本类不解释任何协议语义。
/// @note Thread: single-threaded（随一次交换的协程帧使用）
/// @note Side-effects: 首读经 `fetch_exchange` 发起一次真实网络交换
class FetchExchangeAdapter {
  public:
    /// @brief 构造：装配 fetch 规格（URL / 方法 / 过闸请求头 / 已物化体 / 截止时刻）。
    /// @param request 本跳的请求（URL 已由发起侧验证；体是物化形态，流式体在发起侧已前置拒）。
    /// @param deadline 交换截止时长（`Timeouts::connect` 档折算；非正值 = 不设限）。
    /// @param token 本跳的取消令牌（send 的逐尝试令牌；`fetch_exchange` 借它的钩子槽打断在途
    ///        fetch——fetch 腿没有原生传输，该槽在本跳空闲）。
    FetchExchangeAdapter(const Request &request, std::chrono::milliseconds deadline, const Cancellation &token)
        : token_{token} {
        spec_.url = request.url().has_value() ? request.url().value().to_string() : std::string{};
        spec_.method_text = std::string{method_text(request.method())};
        for (const auto &field : request.headers().entries()) {
            if (!fetch_stripped_field(field.name)) {
                spec_.headers.emplace_back(field.name, field.value);
            }
        }
        spec_.body = request.body();
        spec_.deadline = deadline;
    }

    FetchExchangeAdapter(const FetchExchangeAdapter &) = delete;
    auto operator=(const FetchExchangeAdapter &) -> FetchExchangeAdapter & = delete;
    FetchExchangeAdapter(FetchExchangeAdapter &&) = delete;
    auto operator=(FetchExchangeAdapter &&) -> FetchExchangeAdapter & = delete;
    ~FetchExchangeAdapter() = default;

    /// @brief 写出：整单语义下无逐段落线，每一段记账即成功（字节数照报，写环的进度口径照走）。
    /// @param data 待写字节段。
    /// @return 可等待体，恒成功并报全段字节数。
    [[nodiscard]] auto write(std::span<const char> data) -> detail::CallbackAwaiter<std::size_t> {
        return detail::CallbackAwaiter<std::size_t>(
            [data](const detail::CallbackAwaiter<std::size_t>::Sink &on_done) -> void {
                on_done(Result<std::size_t>{data.size()});
            });
    }

    /// @brief 带窗口的读（`Expect: 100-continue` 问询段）：浏览器不等 100，窗口内恒无应答。
    /// @param capacity 读缓冲上限（整单语义下不消费，仅对齐闭包形态）。
    /// @param window 等待窗口（同上）。
    /// @return 可等待体，恒交 `WaitElapsed`（发起侧据此照常发体）。
    [[nodiscard]] auto read_within(std::size_t capacity, std::chrono::milliseconds window)
        -> detail::CallbackAwaiter<detail::WindowedRead> {
        (void)capacity;
        (void)window;
        return detail::CallbackAwaiter<detail::WindowedRead>(
            [](const detail::CallbackAwaiter<detail::WindowedRead>::Sink &on_done) -> void {
                on_done(Result<detail::WindowedRead>{
                    detail::WindowedRead{.outcome = detail::WindowedRead::Outcome::WaitElapsed}});
            });
    }

    /// @brief 读：首读整单交付，其后从已合成的线上字节流逐段服务，排空交 0。
    /// @param buffer 读缓冲。
    /// @return 可等待体，求得交付字节数（0 = 本跳字节已交完）或失败。
    [[nodiscard]] auto read(std::span<char> buffer) -> detail::CallbackAwaiter<std::size_t> {
        return detail::CallbackAwaiter<std::size_t>(
            // 发起器经 std::function 类型擦除转入触发的系统性假告警面；结局一律以 Result 交付、不抛。
            // NOLINTNEXTLINE(bugprone-exception-escape)
            [this, buffer](detail::CallbackAwaiter<std::size_t>::Sink on_done) mutable -> void {
                if (!fired_) {
                    fired_ = true;
                    // 内层急切协程驱动一次 fetch_exchange（`CallbackAwaiter` 需要 协程帧，而本成员不是
                    // 协程）；结局经投递口回到本闭包的调用方。适配器由 send 闭包的 shared_ptr 持有，
                    // 活过整个挂起期。
                    drive_first_read(buffer, std::move(on_done));
                    return;
                }
                on_done(serve_staged(buffer));  // 已交付过：直接从合成字节流服务（可能同步恢复发起协程）
            });
    }

  private:
    /// @brief 首读的内层驱动：等 fetch 结局 → 归一错误 → 合成线上字节流 → 交出首段。
    ///
    /// `CoroTask` 急切启动（构造即执行），本函数的返回值只作保活句柄；结局经投递口交回。
    /// @param buffer 读缓冲（首段交付目标）。
    /// @param on_done 读写环的投递口。
    auto drive_first_read(std::span<char> buffer, std::function<void(Result<std::size_t>)> on_done) -> CoroTask<void> {
        auto outcome = co_await detail::fetch_exchange(spec_, token_);
        if (!outcome) {
            on_done(Result<std::size_t>{outcome.error()});
            co_return;
        }
        switch (outcome.value().ending) {
            case detail::FetchOutcome::Ending::Completed: {
                auto staged = stage_response(outcome.value());
                if (!staged) {
                    on_done(Result<std::size_t>{staged.error()});
                    co_return;
                }
                break;
            }
            case detail::FetchOutcome::Ending::Aborted:
                // 打断的两个成因按令牌分辨：取消是调用方的决定，其余就是截止到点。
                on_done(Result<std::size_t>{make_error(
                    token_.is_cancelled() ? ErrorCode::NetCancelled : ErrorCode::HttpTimeout,
                    token_.is_cancelled() ? "the exchange was cancelled while the browser fetch was in flight"
                                          : "exchange timeout: the fetch deadline fired (browser abort)")});
                co_return;
            case detail::FetchOutcome::Ending::NetworkError:
                on_done(Result<std::size_t>{
                    make_error(ErrorCode::NetConnectFailed,
                               "fetch failed: the browser does not disclose the cause (network error, CORS preflight "
                               "refusal, or DNS resolution failure)")});
                co_return;
        }
        on_done(serve_staged(buffer));
    }

    /// @brief 把 fetch 结局合成「线上字节流」：状态行 + 过滤后的应答头 +（有体时的）体长 + 体。
    ///
    /// 浏览器已透明解码：`Content-Encoding` / `Transfer-Encoding` 剥除，`Content-Length` 以**合成
    /// 时刻的体长**为准（原值描述的是编码前的事实，与交出的字节不对应）。空体不补长度——装配器
    /// 落 `UntilClose` 分帧、适配器排空即交 0（FIN），同一形态在本跳收口。状态 0 = 浏览器不透明
    /// 应答（manual 重定向专属）：如实落 `net-platform-unavailable` 并点名限制，绝不编造一条重定向。
    /// @param outcome fetch 结局。
    /// @return 成功为 OK；不透明应答携 `net-platform-unavailable`。
    [[nodiscard]] auto stage_response(const detail::FetchOutcome &outcome) -> Result<void> {
        if (outcome.status == 0) {
            return Result<void>{
                make_error(ErrorCode::NetPlatformUnavailable,
                           "the redirect response is opaque on this leg (browser fetch with manual redirect): the "
                           "browser hides the redirect target, and redirects are never auto-followed here")};
        }
        std::string head = "HTTP/1.1 " + std::to_string(outcome.status);
        head += outcome.reason.empty() ? " \r\n" : " " + outcome.reason + "\r\n";
        std::size_t begin = 0;
        while (begin < outcome.headers_text.size()) {
            const auto end = outcome.headers_text.find('\n', begin);
            const auto line = std::string_view{outcome.headers_text}.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            begin = end == std::string::npos ? outcome.headers_text.size() : end + 1;
            const auto at = line.find(':');
            if (at == std::string_view::npos || at == 0) {
                continue;
            }
            const auto name = line.substr(0, at);
            if (field_name_equal(name, "content-encoding") || field_name_equal(name, "transfer-encoding") ||
                field_name_equal(name, "content-length")) {
                continue;  // 应答侧同族：编码已被浏览器还原，长度与分帧由合成口径自理
            }
            head += line;
            head += "\r\n";
        }
        if (!outcome.body.empty()) {
            head += "content-length: " + std::to_string(outcome.body.size()) + "\r\n";
        }
        head += "\r\n";
        staged_.assign(head.begin(), head.end());
        staged_.insert(staged_.end(), outcome.body.begin(), outcome.body.end());
        return Result<void>{};
    }

    /// @brief 从合成字节流服务一次读；排空后交 0（对端收摊形态）。
    /// @param buffer 读缓冲。
    /// @return 交付字节数（0 = 已交完）。
    [[nodiscard]] auto serve_staged(std::span<char> buffer) -> Result<std::size_t> {
        if (cursor_ >= staged_.size()) {
            return Result<std::size_t>{std::size_t{0}};
        }
        const std::size_t take = std::min(staged_.size() - cursor_, buffer.size());
        std::copy_n(staged_.begin() + static_cast<std::ptrdiff_t>(cursor_), take, buffer.begin());
        cursor_ += take;
        return Result<std::size_t>{take};
    }

    detail::FetchSpec spec_;  ///< fetch 交换规格（构造期装配）
    const Cancellation &token_;  ///< 本跳的取消令牌（引用逐尝试令牌：取消打断在途 fetch，见构造注释）
    std::vector<char> staged_;  ///< 合成的线上字节流（状态行 + 头 + 体）
    std::size_t cursor_ = 0;  ///< 已交付游标
    bool fired_ = false;  ///< 是否已发起过整单交换（首读标记）
};

/// @brief 由本跳的路由决策拼一枚池键（口径见 `detail::HttpPoolKey`：五枚在线事实 + 配置代次）。
/// @param route 本跳的路由（`plan_route()` 的产物，代理各元与请求行形态都在里面）。
/// @param secure 目标是否 `https://`（决定这条连接上有没有 TLS 会话）。
/// @param tls_generation 建连时刻的 TLS 配置代次。
/// @return 池键。
[[nodiscard]] auto make_pool_key(const HopRoute &route, bool secure, std::uint64_t tls_generation)
    -> detail::HttpPoolKey {
    detail::HttpPoolKey key;
    key.secure = secure;
    key.peer_host = route.peer_host;
    key.peer_port = route.peer_port;
    key.absolute_form = route.absolute_form;
    key.tunneled = route.tunneled_via_proxy;
    key.tls_generation = tls_generation;
    return key;
}

/// @brief 装配器开不开 leftover 容忍模式：只在池开着时开（裸 `Client` 的线上行为与池从未存在时逐字节相同）。
/// @param pool 池本体。
/// @param options 池口径。
/// @return 开池为真。
[[nodiscard]] auto pool_active(const std::shared_ptr<detail::HttpConnectionPool> &pool, const PoolOptions &options)
    -> bool {
    return options.enabled && pool != nullptr;
}

/// @brief 把 Jar 里适用于本跳目标的 cookie 附成 `Cookie` 请求字段。
///
/// 调用方自写的 `Cookie` 永远优先（与 `Accept-Encoding` / 条件头同一条纪律：协商是调用方的表达）；Jar 取
/// 出来是空串即什么也不做。附在原始请求副本与每一跳重建的副本上——缓存键因此自动看见 cookie
/// （凭据字段恒参与建键，见 `detail/http_cache.h`）。
/// @param request 就地附加的请求。
/// @param jar 会话的 Jar（空 = 裸 `Client`，不收不发）。
auto attach_cookies_from_jar(Request &request, const std::shared_ptr<detail::HttpCookieJar> &jar) -> void {
    if (jar == nullptr || request.headers().contains("Cookie")) {
        return;
    }
    const auto &slot = request.url();
    if (!slot.has_value()) {
        return;  // 目标还没解析成功：那是 `render_head()` 要报的错，Jar 不猜
    }
    auto header = jar->cookies_for(slot.value(), std::chrono::system_clock::now());
    if (!header.empty()) {
        request.header("Cookie", header);
    }
}

/// @brief 把本跳应答里的全部 `Set-Cookie` 收进 Jar（畸形的逐条丢弃；`Secure` 遇明文信道不存储）。
/// @param jar 会话的 Jar（空即整段跳过）。
/// @param target 本跳目标。
/// @param response_fields 本跳应答头（`Set-Cookie` 逐条取，不折叠）。
/// @param secure_channel 本跳是否走了 TLS。
auto harvest_set_cookies(const std::shared_ptr<detail::HttpCookieJar> &jar, const Url &target,
                         const Headers &response_fields, bool secure_channel) -> void {
    if (jar == nullptr) {
        return;
    }
    jar->store_from_response(target, response_fields, secure_channel, std::chrono::system_clock::now());
}

/// @brief 复用资格的后三判据（前两判据——完整读完、无 leftover——由调用方在装配器上问）。
///
/// `Connection: close` 由对端明示不想保活；`UntilClose` 分帧在体读全的那一刻连接就没了；
/// HTTP/1.0 缺省 close 语义（版本取自状态行，比从字段反推可靠）。`saw_fin` 是「本次交换见到过对端 FIN」。
/// @param head 本跳应答头。
/// @param framing 分帧形态。
/// @param saw_fin 交换中是否观察到对端关闭。
/// @return 五判据的后三枚全过为 true。
[[nodiscard]] auto response_allows_reuse(const wire::ResponseHead &head, wire::BodyFraming framing, bool saw_fin)
    -> bool {
    if (saw_fin || framing == wire::BodyFraming::UntilClose) {
        return false;
    }
    if (head.version_minor < 1) {
        return false;  // HTTP/1.0：缺省即关
    }
    for (const auto &value : head.headers.values("Connection")) {
        std::string lowered;
        lowered.reserve(value.size());
        for (const char c : value) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lowered.find("close") != std::string::npos) {
            return false;  // 对端明示关闭（token 列表里出现 `close`；`keep-alive` 与其余令牌不影响）
        }
    }
    return true;
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
/// 只在「开关开着 + 本请求没 `no_cache()` + 方法可缓存 + 没带字节范围 + URL 解析成功」五件同时成立时才问
/// 表，缺一即整段跳过——跳过的形态与缓存从未存在时逐字节相同（连一次内存查找都不做）。
///
/// 带 `range()` 的请求整段不参与缓存是刻意的：表按「方法 + URL」建键，同一 URL 的整份副本绝不能被拿来
/// 充当一个分段应答（那会把资源的前半截当成 `bytes=500-999` 的答案交出去），反向也一样——所以对端回来的
/// 那份 206 也不许进表。两半各有一道闸：起手这一段认出 `range()` 就把键留空，`settle_cache` 见键空即整段
/// 不收；即便范围是以裸 `Range` 头上来的（本件认不出，键照建、表照问），收尾时也由
/// `detail::is_cacheable_status()` 把 206 挡在表外。代价是范围请求一律不享缓存，以及裸头写法下前一半闸
/// 失效、整份副本会被当成对分段请求的答复交出——用 `range()` 而不是裸头是这层的约定。
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
    if (!options.enabled || request.no_cache() || request.range().has_value() ||
        !detail::is_cacheable_method(request.method()) || !cache) {
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

Client::Client()
    : cache_{std::make_shared<detail::HttpResponseCache>(cache_options_)},
      pool_{std::make_shared<detail::HttpConnectionPool>()}, tls_generation_{std::make_shared<std::uint64_t>(0)} {}

auto Client::cache(CacheOptions value) -> Client & {
    cache_options_ = value;
    // 口径同步进表：`max_entries` 是表自己的容量，改小时它按新上限逐出多余条目（不是留着等命中）。
    // 表为空（只在移动后的壳上 possible）时无处可同步，开关本身仍已生效，下次起手照旧跳过。
    if (cache_) {
        cache_->set_options(value);
    }
    return *this;
}

auto Client::pool(const PoolOptions &value) -> Client & {
    pool_options_ = value;
    // 关掉即清空：池的承诺是「开着时同键请求坐干净连接」，关着时每条空闲连接都是无人认领的负担，
    // 而且下一次开着时的首次取用也会被 sweep 淘汰掉它们——当场清比留给未来诚实。
    if (pool_ && !value.enabled) {
        pool_->clear();
    }
    return *this;
}

auto Client::send(Request request, Cancellation cancellation) const -> CoroTask<Result<Response>> {
    // 取消先于一切判定：令牌到手时已取消，策略闸、缓存表、连接一概不问（口径见本头「取消」条）。
    // 排在最前是为了让这条判据与「一次都不触网」同样可断言——缓存命中虽不触网，也不该被当成一次交付。
    if (cancellation.is_cancelled()) {
        co_return Result<Response>{make_error(ErrorCode::NetCancelled, "the exchange was cancelled before it started")};
    }
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
    // Jar 起手附着：把会话里适用于本目标的 cookie 附到原始请求副本上（排在查表之前，缓存键因此诚实——
    // `Cookie` 恒参与建键）。调用方自写的 `Cookie` 优先，Jar 不覆盖。裸 `Client` 没有 Jar，此处零作为。
    attach_cookies_from_jar(origin, cookie_jar_);
    // 策略在发起那一刻读定：本次交换的每一跳与缓存命中都问这一份，之后改 `Client::policy()` 或进程级默认
    // 都不影响已在途的交换（判定用的是「出发时」的策略，不是「回投时」的策略）。
    const NetPolicy security = effective_policy(policy_);
    // 起手策略闸排在查表之前：命中缓存照样是一次交付，不能因为它就没被问过。
    if (const auto verdict = gate_origin(proxy_, tls_, security, origin); !verdict) {
        co_return Result<Response>{verdict.error()};
    }
    // fetch 腿的整单语义不支持流式请求体：浏览器一次性交出整次交换，`BodySource` 的分段与回放口径
    // 无落点。如实落 `net-platform-unavailable` 并点名，不做悄悄一次性物化（§16 第 37 项）。
    if (detail::fetch_leg_active() && origin.body_source() != nullptr) {
        co_return Result<Response>{
            make_error(ErrorCode::NetPlatformUnavailable,
                       "send: a streaming request body is not available on this leg (browser fetch delivers the "
                       "whole exchange at once); materialize the body first")};
    }
    // 缓存起手式：命中新鲜副本时一条连接都不发起就交出——§12 承诺的「同一 URL 二次请求不触网」正是这一条。
    // 命中陈旧副本时把两枚条件头挂在**原始请求**上，故重试环的每一次尝试都带着同一份校验子重跑（挂在逐跳
    // 副本上的话，第二次尝试就成了「第一次验证、第二次裸打」）。
    CacheOpening opening = open_from_cache(cache_, cache_options_, origin);
    if (opening.fresh.has_value()) {
        co_return Result<Response>{std::move(*opening.fresh)};
    }
    // 重试环的资格有三道判据：策略开着、方法幂等、体可回放到原点。第三道只挂在流式体上——内存体天然能重发，
    // 而 `restart()` 是流式体重放资格的唯一判据（口径见 `BodySource`）：一次 `restart()` 在此问清，此后本交换
    // 再不问第二遍。半截体重发等于交付一份数据损坏的请求，且这个代价在失败现场无从判定，故本库不为此开旋钮。
    const bool replayable = origin.body_source() == nullptr || origin.body_source()->restart();
    const bool loop_open = detail::retry_enabled(policy) && detail::is_idempotent_method(origin.method()) && replayable;

    // 跨全部尝试（含退避等待）的墙登记在这一枚令牌上；每一次尝试各拿一份自己的令牌与 `total` 看门狗，
    // 故 `Timeouts::total` 天然逐尝试重新计时——这正是它与 `RetryPolicy::budget` 分家的落点。
    // 环不开时 `budget` 一并摘掉：一枚「跨全部尝试」的墙在没有第二次尝试时不该变成单段超时。
    Cancellation sequence;
    auto wall = std::make_shared<TimeoutState>();
    StageTimer budget_timer(loop_open ? policy.budget : std::optional<std::chrono::milliseconds>{},
                            TimeoutStage::Budget, sequence, wall);
    // 宿主交来的令牌接到本交换的「跨尝试序列」上：取消即掐掉在途那一次（逐尝试的 `token` 由 sequence 转发），
    // 且本环就此不再发起新的尝试。登记时若已取消会立即触发（`Cancellation` 的竞态契约），故起手检查到这里
    // 之间那点空隙不会留下「取消了却没人掐」的窗口。
    cancellation.on_cancel([sequence]() mutable { sequence.cancel(); });
    Result<Response> outcome{make_error(ErrorCode::HttpProtocolError, "send produced no outcome")};
    // 认证环的每次交换状态：提供方取自客户端（空即整环不存在）、问过没有、代理那侧的答复。源站那侧的答复直接
    // 并回 `origin`，故这里不留副本（口径见 `place_auth_decision`）。
    AuthRing ring{.provider = authenticator_};
    // 问出去的那份挑战存在交换作用域：提供方是协程出口，它的帧可能在本环放弃之后才被投递回来（见下面的契约条目）。
    std::optional<AuthChallenge> asked_challenge;
    // 本环要不要重跑本次尝试：递增 `attempt` 的动作被挪到重试环的尾巴上，正是为了让「问过凭据、重跑一次」
    // 这条路不吃 `RetryPolicy::attempts` 名额（那条裁决见 `net/http_auth.h`）。
    bool replay_for_credentials = false;

    // 重试环与单次交换**同帧**：`CoroTask` 不是 awaitable（`state/coroutine.h` 的 promise 永不首挂也不终挂，
    // 结果只经 `is_done()` / `result()` 轮询），故「把一次尝试抽成子协程再 co_await」在本库无对应机制——
    // 这正是本文件开头「刻意不做抽象」那条约束在重试这一层的再确认。
    for (std::uint32_t attempt = 1;;) {
        // 取消落在两次尝试之间（含起手检查之后的那点竞态）时，这里就是唯一的收摊口：一次都没打过即交「被取消」
        // 这份事实，打过的那次留下它自己的错误码——本环不替调用方改写已经发生的事。
        if (sequence.is_cancelled()) {
            if (attempt == 1 && !replay_for_credentials) {
                outcome =
                    Result<Response>{make_error(ErrorCode::NetCancelled, "the exchange was cancelled before it ran")};
            }
            break;
        }
        Cancellation token;
        // 墙到点即掐掉在途的这一次尝试（`Cancellation` 契约：钩子至多触发一次），随后本环据 `is_cancelled()` 收摊。
        sequence.on_cancel([token]() mutable { token.cancel(); });
        Request current = origin;  // 每次尝试都从原始请求重新发起：重定向与重放都会改写它
        if (attempt > 1 || replay_for_credentials) {
            // 上一次的这一趟把流式体读到了半途，本次须从原点重发。入口已判过资格的源在这里回退失败，说明它对
            // 「能否回到原点」给了两个答案——按不可重放收摊，绝不把半截体写上线。认证环的重跑与重试的重跑在
            // 这条判据上同构：都是「同一份请求再打一遍」，体自然也得再给一遍。
            if (auto *source = current.body_source(); source != nullptr && !source->restart()) {
                outcome = Result<Response>{make_error(
                    ErrorCode::HttpBodyNotReplayable,
                    "a streaming request body refused to return to its starting point before a retry attempt")};
                break;
            }
        }
        replay_for_credentials = false;
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
        // fetch 腿的整单语义（§16 第 37 项）：跳环里的建连分支据此换挂适配器；代理由浏览器代管，
        // 逐跳路由以空代理环境跑同一条 plan_route（策略闸照常判定，代理决策恒 Direct）。
        const bool fetch_leg = detail::fetch_leg_active();
        const ProxyEnvironment fetch_empty_proxy{};
        // 认证环要在本环的收尾处读「最近一跳路由向谁、那次 `CONNECT` 回了什么」，而这两件事实生在逐跳作用域里，
        // 故提到尝试级留一份副本（`negotiation` 每跳开头重置，只有走隧道那一跳会被传输层写入）。
        HopRoute hop;
        detail::ProxyNegotiation negotiation;
        bool stale_retried = false;  // 本尝试是否已对一条陈旧的池连接重试过（一尝试一次，保证收敛）
        bool queue_failed = false;  // 本尝试是否在排队上没等到（connect 档到点或取消）：收尾据此把 detail 说成「排队」

        // 进度闸随协程帧存活；回调取自首跳请求，重定向后的各跳沿用同一份观测口（与 `timeouts` / 上限同纪律：
        // 发起侧选项在入口取一次，逐跳不重读）。下面几枚计数是**本跳**口径，每跳开头重置。
        detail::ProgressGate gate(current.on_progress(), progress_frame_source_);
        std::uint64_t wire_seen = 0;  // 本跳已 drain 的线上体字节（解压前口径）
        std::optional<std::uint64_t> announced_total;  // Length 分帧声明的体总量；其余形态未知
        std::uint64_t uploaded = 0;  // 本跳已写出的请求体字节
        std::uint64_t upload_total = 0;  // 本跳请求体总量（长度给得出时才有意义）
        bool upload_total_known = false;  // 本跳的请求体总量是否给得出（分块形态给不出）
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
            upload_total_known = false;
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
            // fetch 腿以空代理环境问同一条路：代理由浏览器代管，策略闸（含明文闸）照常判定。
            auto routed = fetch_leg ? plan_route(fetch_empty_proxy, tls_, security, target, *port)
                                    : plan_route(proxy_, tls_, security, target, *port);
            if (!routed) {
                one = Result<Response>{routed.error()};
                break;
            }
            const HopRoute &route = routed.value();
            hop = route;
            negotiation = detail::ProxyNegotiation{};
            if (hop.tunneled_via_proxy) {
                // 答复回给 `CONNECT`：代理那句 407 说的是「这条隧道先付凭据」，故下一次建隧道时把答复写进
                // `Proxy-Authorization`（覆写优先级见 `detail::wire::make_connect_request`）。
                negotiation.proxy_authorization = ring.proxy_answer.value_or(std::string{});
            }
            // ── 写出物化：请求头部 + 体（render_head 失败即字段畸形，一条连接都不发起、原样交出）。
            //    代理答复只写面向代理那一侧：转发腿从一份带答复的副本渲染，`current` 本身不沾它——否则本尝试的
            //    下一跳换成直连或隧道时，那份代理凭据就被送到源站去了（口径见 `net/http_auth.h` 的代理挑战条）。
            auto outbound = std::optional<Request>{};
            const Request *on_wire = &current;
            if (route.absolute_form && ring.proxy_answer.has_value()) {
                outbound = current;
                outbound->set_header(auth_credential_field(true), *ring.proxy_answer);
                on_wire = &outbound.value();
            }
            auto head = on_wire->render_head(route.absolute_form);
            if (!head) {
                one = Result<Response>{head.error()};
                break;
            }
            // TLS 握手与证书校验的对象恒是目标主机（隧道之上再握手），代理看不见这些。
            const std::string host{target.host()};

            // ── 建连并统一读写口：明文 / TLS 两条流经捕获 shared_ptr 的类型擦除闭包收敛到同一对 awaiter。
            //    第三枚是「带窗口的读」，只服务 `Expect: 100-continue` 那一段问询。`late` 是那一段里被放弃
            //    那次读的落点：传输层按 FIFO 交付回调，故它后来到达的字节恒排在下一次读之前，装配前先清它
            //    （口径见 `detail::await_read_within`）。
            //    连接池开着时在「建连」之前多问一声池：命中即坐上那条空闲连接（跳过建连与握手）、无空闲但
            //    名额有余就照常建连、名额满就排队等——等的时间计入 connect 档（`StageTimer` 在本段之前登记），
            //    到点在 detail 点名「排队等不到」而不是「连不上」。
            //    fetch 腿没有连接可池化：池整段跳过（retaining_leftover 随之为假，装配器不开容忍模式——
            //    整单交付的应答不存在「完整之后又到字节」的形态，§16 第 37 项）。
            std::function<detail::CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
            std::function<detail::CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
            std::function<detail::CallbackAwaiter<detail::WindowedRead>(std::size_t, std::chrono::milliseconds)>
                read_within_one;
            auto late = std::make_shared<std::vector<char>>();
            StageTimer connect_timer(timeouts.connect, TimeoutStage::Connect, token, tstate);
            const bool retaining_leftover = pool_active(pool_, pool_options_) && !fetch_leg;
            detail::HttpPoolLease lease;  // 空卫 = 池关着，析构零作为；池开着时析构即恰好归还一次
            bool leased_from_pool = false;
            bool force_fresh_connect = false;  // 陈旧连接重试的那一轮不再信任空闲条目（保证一定换新）
            if (retaining_leftover) {
                const auto key =
                    make_pool_key(route, target.is_secure(), tls_generation_ != nullptr ? *tls_generation_ : 0);
                lease = detail::HttpPoolLease{*pool_, key, pool_options_};
                for (;;) {
                    auto got = pool_->acquire(key, pool_options_, std::chrono::steady_clock::now());
                    if (got.verdict == detail::HttpPoolAcquire::Ready && force_fresh_connect) {
                        // 陈旧事件之后不再信任空闲条目：逐条丢弃（关闭并让出名额），直到能开新连接。
                        // 每丢一条名额就让一枚，循环必然收敛到 `OpenNew` 或排队。
                        pool_->release(key, std::nullopt, got.born, pool_options_, std::chrono::steady_clock::now());
                        continue;
                    }
                    if (got.verdict == detail::HttpPoolAcquire::Ready) {
                        leased_from_pool = true;
                        lease.adopt(std::move(got.connection), got.born);
                        connect_timer.stop();  // 命中即停表：建连（含握手）这一段没有发生
                        break;
                    }
                    if (got.verdict == detail::HttpPoolAcquire::OpenNew) {
                        lease.adopt(std::move(got.connection), got.born);
                        break;
                    }
                    // 排队：让出宿主回合，空闲连接由并发的另一条交换归还；connect 档到点即停。
                    if (tstate->fired.load() != static_cast<int>(TimeoutStage::None)) {
                        one = Result<Response>{make_error(
                            ErrorCode::HttpTimeout,
                            "connect timeout: queued too long for an idle connection (pool per-host limit)")};
                        queue_failed = true;
                        break;
                    }
                    co_await detail::await_sleep(AURORA_POOL_QUEUE_POLL, token);
                    if (token.is_cancelled() && tstate->fired.load() == static_cast<int>(TimeoutStage::None)) {
                        one = Result<Response>{make_error(ErrorCode::NetCancelled,
                                                          "the exchange was cancelled while queued for a connection")};
                        queue_failed = true;
                        break;
                    }
                }
                if (queue_failed) {
                    break;  // 排队没等到：本跳收摊（错误已在 `one` 里，收尾的看门狗改道照常读它）
                }
            }
            std::shared_ptr<StreamTransport> plain_holder;
            std::shared_ptr<TlsTransport> secure_holder;
            if (fetch_leg) {
                // ── fetch 腿（§16 第 37 项）：本跳不建连、写出不落线——浏览器在首次读时整单交付。
                //    三个闭包挂在适配器上，写出环 / 读环 / 物化 / 重定向与原生腿共用同一份代码；
                //    `plan_route` 在本腿以空代理环境跑（路由恒 `Direct`，代理由浏览器代管）。
                auto adapter = std::make_shared<FetchExchangeAdapter>(
                    current, timeouts.connect.value_or(std::chrono::milliseconds{0}), token);
                read_one = [adapter](std::span<char> buffer) { return adapter->read(buffer); };
                write_one = [adapter](std::span<const char> data) { return adapter->write(data); };
                read_within_one = [adapter](std::size_t capacity, std::chrono::milliseconds window) {
                    return adapter->read_within(capacity, window);
                };
            } else {
                if (leased_from_pool) {
                    plain_holder = lease.connection().plain;
                    secure_holder = lease.connection().secure;
                } else if (target.is_secure()) {
                    auto plain = co_await detail::await_connect_via(route.connect_with, route.peer_host,
                                                                    route.peer_port, options, token,
                                                                    hop.tunneled_via_proxy ? &negotiation : nullptr);
                    if (!plain) {
                        one = Result<Response>{plain.error()};
                        break;
                    }
                    auto secure = co_await detail::await_handshake(std::move(plain.value()), host, tls_, token);
                    if (!secure) {
                        one = Result<Response>{secure.error()};
                        break;
                    }
                    secure_holder = std::make_shared<TlsTransport>(std::move(secure.value()));
                } else {
                    auto plain = co_await detail::await_connect_via(route.connect_with, route.peer_host,
                                                                    route.peer_port, options, token,
                                                                    hop.tunneled_via_proxy ? &negotiation : nullptr);
                    if (!plain) {
                        one = Result<Response>{plain.error()};
                        break;
                    }
                    plain_holder = std::make_shared<StreamTransport>(std::move(plain.value()));
                }
                if (!leased_from_pool) {
                    lease.adopt(detail::HttpPoolConnection{.plain = plain_holder, .secure = secure_holder},
                                lease.born());
                }
                if (secure_holder != nullptr) {
                    read_one = [holder = secure_holder](std::span<char> buffer) {
                        return detail::await_read(*holder, buffer);
                    };
                    write_one = [holder = secure_holder](std::span<const char> data) {
                        return detail::await_write(*holder, data);
                    };
                    read_within_one = [holder = secure_holder, late](std::size_t capacity,
                                                                     std::chrono::milliseconds window) {
                        return detail::await_read_within(*holder, capacity, window, late);
                    };
                } else {
                    read_one = [holder = plain_holder](std::span<char> buffer) {
                        return detail::await_read(*holder, buffer);
                    };
                    write_one = [holder = plain_holder](std::span<const char> data) {
                        return detail::await_write(*holder, data);
                    };
                    read_within_one = [holder = plain_holder, late](std::size_t capacity,
                                                                    std::chrono::milliseconds window) {
                        return detail::await_read_within(*holder, capacity, window, late);
                    };
                }
            }
            connect_timer.stop();  // 建连（含握手）完成，connect 档停表，不误伤后续读写

            // ── 发出请求。写出形态由 `detail::plan_request_body()` 裁决——那是 `render_head()` 已经用过的同一份
            //    事实，两处各问一次即不可能分岔（四条口径见 `detail/http_body.h`）。装配器与它的记账口先于写出
            //    就位，因为「先问再发体」要在发出请求体之前就把那条 1xx 喂进装配器。
            // ── 逐段装配应答的准备：喂 ResponseAssembler，按分帧物化体，直到 complete 或对端收摊。
            //    池开着时装配器开 leftover 容忍模式：完整之后再到的字节不再是「协议错」，而是存下来交复用判据
            //    （富余非空即放弃复用、当场关连接，本交换照常成功——口径见提案 §16 第 36 项第③款）。
            wire::ResponseAssembler assembler(max_body_bytes_, current.is_head(), retaining_leftover);
            std::vector<char> body;
            std::array<char, AURORA_READ_CHUNK> chunk{};
            bool halted = false;
            bool saw_fin = false;  // 本跳是否观察到对端 FIN（复用资格判据之一）
            // 陈旧连接重试的资格：坐的是池里的空闲连接、对端在空闲期把它关了（错误已见而头部未至）、
            // 方法幂等（重放安全与重试环同一白名单）、且本次尝试还没重试过。三条失败出口（快路写出、
            // 泵路写出、读环）各自问这一句，问中即 `continue` 跳循环换新连接重跑本跳。
            auto stale_retry_eligible = [&]() -> bool {
                return leased_from_pool && !stale_retried && !assembler.head_ready() &&
                       tstate->fired.load() == static_cast<int>(TimeoutStage::None) && !token.is_cancelled() &&
                       detail::is_idempotent_method(current.method());
            };
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
            // 把一段线上字节喂进装配器并记一次下载账；失败原样交出（体越上限 / 文法不合 / 完整后又来字节）。
            auto feed = [&](std::span<const char> bytes) -> Result<void> {
                if (bytes.empty()) {
                    return Result<void>{};
                }
                auto pushed = assembler.push(std::string_view{bytes.data(), bytes.size()});
                if (!pushed) {
                    return Result<void>{pushed.error()};
                }
                assembler.drain_body(body);
                note_download();
                return Result<void>{};
            };

            auto plan = detail::plan_request_body(current);
            if (!plan) {
                // `render_head()` 已按同一份判定在此前交出过失败，走到这里只可能是两处判据分岔；仍照畸形请求
                // 交出那枚错误，绝不留「头部里声明一套、线上写出另一套」的余地。
                one = Result<Response>{plan.error()};
                break;
            }
            const bool await_continue = current.expect_continue() && plan.value().has_body;
            // 快路成立的四件：没挂进度回调、不等 100、形态不是分块、体在内存。缺一即走泵路——快路的意义是
            // 「无需求者逐字节与本切片的观测口从未存在时同型」，泵路在线上等价但要多问源几次。
            const bool one_shot = !gate.armed() && !await_continue &&
                                  plan.value().framing != detail::RequestBodyFraming::Chunked &&
                                  current.body_source() == nullptr;
            bool send_body = true;  // 是否获准发体：不等 100 时恒真，等了却没等到回话才可能关死
            if (one_shot) {
                std::string payload = head.value();
                payload.insert(payload.end(), current.body().begin(), current.body().end());
                auto written = co_await write_one(std::span<const char>{payload});
                if (!written) {
                    if (stale_retry_eligible()) {
                        stale_retried = true;
                        // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): 协程帧上的跨 continue 读，分析器不可见
                        force_fresh_connect = true;
                        continue;  // 空闲期被对端关了：幂等方法换新连接重跑本跳（一个字节都没出去过）
                    }
                    one = Result<Response>{written.error()};
                    break;
                }
            } else {
                bool write_failed = false;
                auto written_head = co_await write_one(std::span<const char>{head.value()});
                if (!written_head) {
                    one = Result<Response>{written_head.error()};
                    write_failed = true;
                }
                if (!write_failed && await_continue) {
                    // 窗口里那条 1xx 不是最终应答：它排在最终应答之前被装配器收下，不改写后者的分帧与判定；
                    // 一条 101 在 `push()` 里当场落 `http-protocol-error`。故本处只问「对端回过话没有」。
                    auto handed = co_await read_within_one(AURORA_READ_CHUNK, AURORA_EXPECT_WINDOW);
                    const auto &probe = handed.value();  // awaiter 恒以成功 `Result` 交出结局，失败在载荷里
                    if (probe.outcome == detail::WindowedRead::Outcome::Failure) {
                        one = Result<Response>{*probe.error};
                        write_failed = true;
                    }
                    if (!write_failed) {
                        auto queued = feed(std::span<const char>{probe.bytes});
                        if (!queued) {
                            one = Result<Response>{queued.error()};
                            write_failed = true;
                        }
                    }
                    if (!write_failed) {
                        // 只有「对端把最终应答整个交出来了且它不是 1xx」才关死发体：那是 417（或任何直接作答的
                        // 状态码），体再发下去只是让对端多读一段它不打算读的字节。窗口里只到半条应答、或一条
                        // 1xx 与最终应答同批抵达时照发——前者无从判定对端的意思，后者正是本条问询要的答案。
                        send_body = !assembler.head_ready() || assembler.has_informational();
                    }
                }
                if (!write_failed && send_body) {
                    detail::RequestBodyPump pump{current, plan.value()};
                    // 总长只有定长形态给得出：分块形态是源自报「我说不清这体多大」，此时上传进度只能报未知。
                    const bool length_known =
                        plan.value().framing != detail::RequestBodyFraming::Chunked && plan.value().length.has_value();
                    upload_total_known = length_known;
                    // 声明了多长就得按多长收摊：`upload_total` 是写出侧的停止条件，与挂没挂进度回调无关。
                    // 只在挂了回调时赋值，会让无回调的定长流式体在第一段之后就自认写完（体只剩半截）。
                    upload_total = plan.value().length.value_or(0);
                    if (gate.armed() && pump.has_body()) {
                        upload_running = true;
                    }
                    while (!pump.done()) {
                        auto segment = pump.next_segment();
                        if (!segment) {
                            one = Result<Response>{segment.error()};  // 源读失败：原样上抛，不替它编原因
                            write_failed = true;
                            break;
                        }
                        if (segment.value().empty()) {
                            break;  // 已交完（分块终止块也已在上一轮随段交出），零长写没有语义
                        }
                        auto written = co_await write_one(std::span<const char>{segment.value()});
                        if (!written) {
                            one = Result<Response>{written.error()};
                            write_failed = true;
                            break;
                        }
                        // 契约：`write` 成功即整段进入内核（`net/transport.h` 的「全部字节都进入内核或明确失败
                        // 才算完成」），故计数按泵交出的正文推进、不回读交付数——交付数在这里没有第二种取值。
                        uploaded = pump.payload_bytes();
                        const bool settled = length_known ? uploaded >= upload_total : pump.done();
                        if (gate.armed()) {
                            gate.observe(Progress{.phase = ProgressPhase::Upload,
                                                  .transferred_bytes = uploaded,
                                                  .total_bytes = upload_total,
                                                  .total_known = length_known,
                                                  .finished = settled});
                            gate.flush();
                        }
                        upload_settled = upload_settled || settled;
                        if (settled) {
                            break;
                        }
                    }
                }
                if (write_failed) {
                    if (stale_retry_eligible()) {
                        stale_retried = true;
                        // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): 协程帧上的跨 continue 读，分析器不可见
                        force_fresh_connect = true;
                        continue;  // 头部都还没到就折了：幂等方法换新连接重跑本跳
                    }
                    break;
                }
            }
            while (!assembler.complete()) {
                StageTimer read_timer(timeouts.read_idle, TimeoutStage::ReadIdle, token, tstate);
                auto read = co_await read_one(std::span<char>{chunk});
                read_timer.stop();  // 本段交付已落地，重置空闲计时：下一轮 read 重新起表
                if (!read) {
                    one = Result<Response>{read.error()};
                    halted = true;
                    break;
                }
                // 被放弃那次「带窗口的读」若后来到过字节，它们排在本次读之前（传输层的 FIFO 口径），故先兑现
                // 它的线上次序再判本轮结局——结局即便是「对端已关」也一样：那批字节可能正是最后一段应答，而
                // 关闭事实总是可以在一条已完整的报文之后再观察到的。队列只可能被填一次，喂完即清。
                if (!late->empty()) {
                    auto queued = feed(std::span<const char>{late->data(), late->size()});
                    late->clear();
                    if (!queued) {
                        one = Result<Response>{queued.error()};
                        halted = true;
                        break;
                    }
                }
                const std::size_t got = read.value();
                if (got == 0) {  // 对端 FIN：UntilClose 据此判全，其余形态提前收摊即协议错位
                    saw_fin = true;
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
                if (assembler.complete()) {
                    // 暂存那批字节已把这条报文交完：本轮新读的字节属下一条报文。池开着时它不再是无关紧要的
                    // 尾巴——容忍模式下存进 leftover，复用判据见它非空即放弃复用（绝不让下一条的请求读到这里）。
                    if (retaining_leftover) {
                        auto parked = assembler.push(std::string_view{chunk.data(), got});
                        if (!parked) {
                            one = Result<Response>{parked.error()};
                            halted = true;
                            break;
                        }
                    }
                    break;
                }
                auto pushed = feed(std::span<const char>{chunk.data(), got});
                if (!pushed) {
                    one = Result<Response>{pushed.error()};
                    halted = true;
                    break;
                }
            }
            if (halted) {
                if (stale_retry_eligible()) {
                    stale_retried = true;
                    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): 协程帧上的跨 continue 读，分析器不可见
                    force_fresh_connect = true;
                    continue;  // 头部都还没到就读折了：幂等方法换新连接重跑本跳（多半是空闲期被对端关了）
                }
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
            // ── 应答侧的 Jar 收取与复用判定（都在本跳收摊前一次问完）。
            const auto response_leftover = assembler.take_leftover();
            harvest_set_cookies(cookie_jar_, target, response_head.headers, target.is_secure());
            if (retaining_leftover && response_leftover.empty() &&
                response_allows_reuse(response_head, assembler.framing(), saw_fin)) {
                lease.arm();  // 五判据全过：本跳出口（析构卫）把这条连接交回池
            }

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
            // 下一跳按新目标的域重新从 Jar 取（cookie 的选择本来就按目标域逐跳算，与代理决策「逐跳重算」
            // 同一条纪律）；换 origin 的跳已被 `follow()` 剥掉旧值，这里补上的只可能是 Jar 给的。
            attach_cookies_from_jar(current, cookie_jar_);
        }

        // 超时改道：失败若因某档看门狗取消了令牌，一律改报 http-timeout，并在 detail 标明是哪一档。
        // 排队等不到空闲连接也走 connect 档（等待计入同一份预算），但它的成因要说成「排队」而不是
        // 「连不上」——两条成因的排查方向完全不同，故在改道之后把 detail 换回排队的说法。
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
        if (queue_failed && tstate->fired.load() == static_cast<int>(TimeoutStage::Connect)) {
            one = Result<Response>{
                make_error(ErrorCode::HttpTimeout,
                           "connect timeout: queued too long for an idle connection (pool per-host limit)")};
        }

        // 进度收尾：两个方向的末值各发一次，不看闸号。成败由 `one` 表达，这一发只宣告「这一趟到此收摊」，
        // 让挂着回调的响应式状态落到确定值而不是停在一个半途数字上。写环里已发过上传末值的不重复发。
        if (gate.armed()) {
            if (upload_running && !upload_settled) {
                gate.observe(Progress{.phase = ProgressPhase::Upload,
                                      .transferred_bytes = uploaded,
                                      .total_bytes = upload_total,
                                      .total_known = upload_total_known,
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

        // ── 认证环（三环次序：策略环 → 认证环 → 重试环之内层；五条裁决见 `net/http_auth.h`）。两个触发面在此
        //    合一：应答里的 401 / 407，与被代理拒掉的 `CONNECT`（那次失败带着那句挑战原文回来，见
        //    `detail::ProxyNegotiation`）。问到凭据就原地重跑本次尝试——不吃 `attempts` 名额，故本块排在重试
        //    裁决之前、而递增 `attempt` 的动作在环尾。物化与流式两条路各写一遍这段（`co_await` 出不了普通函数，
        //    口径同本文件开头的「刻意不做抽象」）。
        if (ring.provider != nullptr && !ring.asked) {
            std::optional<AuthChallenge> challenge;
            if (one && is_auth_challenge(one.value().status())) {
                const bool from_proxy = one.value().status() == 407;
                const auto fields = one.value().headers().values(auth_challenge_field(from_proxy));
                const std::string_view host =
                    from_proxy ? std::string_view{hop.proxy_label}
                               : (current.url().has_value() ? current.url()->host() : std::string_view{});
                challenge = make_auth_challenge(current, fields, one.value().status(), host);
            } else if (!one && hop.tunneled_via_proxy && negotiation.status == 407) {
                challenge = make_auth_challenge(current, negotiation.authenticate, negotiation.status, hop.proxy_label);
            }
            if (challenge.has_value()) {
                ring.asked = true;  // 问过即不再问：带着新凭据重跑后又吃一次挑战，交出的就是那一份
                // 挑战存在**交换作用域**上而不是本块的临时量里：提供方若在问答中途挂起、又被本环放弃（超时或
                // 取消），它那帧协程仍可能引用这份事实（`Authenticator::authenticate` 的存活期契约）。
                asked_challenge = std::move(*challenge);
                auto task = ring.provider->authenticate(*asked_challenge);
                for (std::size_t polls = 0; !task.is_done() && polls < AURORA_AUTH_POLL_CEILING; ++polls) {
                    co_await detail::await_sleep(AURORA_AUTH_POLL_INTERVAL, token);
                }
                if (!task.is_done()) {
                    outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "the authenticator never answered")};
                    break;
                }
                // 双层解包：`CoroTask<Result<T>>::result()` 求得 `Result<Result<T>>`——外层是「这帧协程有没有
                // 跑完」、内层才是「提供方有没有给出凭据」，两枚失败各有各的说法，不得合流。
                const auto outer = task.result();
                if (!outer) {
                    outcome = Result<Response>{make_error(ErrorCode::RuntimeCoroutineException, outer.error().message)};
                    break;
                }
                const auto &answered = outer.value();
                if (!answered) {
                    // 提供方自己的失败就是本次交换的结局：拿一份没到手的凭据去重发，只是把一次明确的失败拖成
                    // 一次含糊的 401。
                    outcome = Result<Response>{answered.error()};
                    break;
                }
                const auto reply = place_auth_decision(*asked_challenge, answered.value(), origin, ring);
                if (reply == AuthReply::Malformed) {
                    outcome = Result<Response>{make_error(ErrorCode::HttpRequestInvalid,
                                                          "the authenticator answered with a field carrying CR or LF")};
                    break;
                }
                if (reply != AuthReply::Declined) {
                    replay_for_credentials = true;
                    continue;  // 回到本环开头重跑：新令牌、新的逐尝试 `total`、原始请求带上补好的凭据
                }
            }
        }

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
        // 递增留在这里，正是为了让上面那条认证重跑 `continue` 绕过它：补一次凭据不算打了一次，
        // `RetryPolicy::attempts` 管的是「服务暂时不行，再打几次」。
        ++attempt;
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
auto Client::open_stream(Request request, DataSink &sink, Cancellation cancellation) const
    -> CoroTask<Result<StreamOutcome>> {
    // 取消先于一切判定，且与 `send` 同一位置：已取消的令牌不问策略、不看本腿能力、不查表，也不调宿主任何一口。
    if (cancellation.is_cancelled()) {
        co_return Result<StreamOutcome>{
            make_error(ErrorCode::NetCancelled, "the exchange was cancelled before it started")};
    }
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

    // 三道资格与 `send` 同判据：策略开着、方法幂等、体可回放到原点（`restart()` 是流式体唯一的重放判据）。
    const bool replayable = request.body_source() == nullptr || request.body_source()->restart();
    const bool loop_open =
        detail::retry_enabled(policy) && detail::is_idempotent_method(request.method()) && replayable;

    // 跨全部尝试（含退避等待）的墙登记在这一枚令牌上；每一次尝试各拿一份自己的令牌与 `total` 看门狗，
    // 故 `Timeouts::total` 天然逐尝试重新计时——这正是它与 `RetryPolicy::budget` 分家的落点。
    // 环不开时 `budget` 一并摘掉：一枚「跨全部尝试」的墙在没有第二次尝试时不该变成单段超时。
    Cancellation sequence;
    auto wall = std::make_shared<TimeoutState>();
    StageTimer budget_timer(loop_open ? policy.budget : std::optional<std::chrono::milliseconds>{},
                            TimeoutStage::Budget, sequence, wall);
    // 宿主的令牌接到跨尝试序列上，转发与竞态口径同 `send`；已交出去的那一段宿主字节不回滚，
    // 取消管的是「此后不再交付」。
    cancellation.on_cancel([sequence]() mutable { sequence.cancel(); });
    Request origin = std::move(request);
    // 链路标识与 `send` 同一处落一次：本入口的每一跳与每一次尝试都带同一串出去。
    stamp_request_id(origin, request_id_);
    log_exchange_identity(origin);
    // Jar 起手附着与 `send` 同一处同判据（排在查表之前，缓存键因此诚实）。
    attach_cookies_from_jar(origin, cookie_jar_);
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
    // 认证环同 `send`：每次交换一份状态、问过不再问、代理答复只留一份在环上（那条路下面另说）。
    AuthRing ring{.provider = authenticator_};
    std::optional<AuthChallenge> asked_challenge;
    bool replay_for_credentials = false;

    // 重试环与单次交换**同帧**：`CoroTask` 不是 awaitable（`state/coroutine.h` 的 promise 永不首挂也不终挂，
    // 结果只经 `is_done()` / `result()` 轮询），故「把一次尝试抽成子协程再 co_await」在本库无对应机制。
    for (std::uint32_t attempt = 1;;) {
        // 收摊口与 `send` 同一条：取消落在两次尝试之间时，一次都没打过才交「被取消」，打过的那次留下自己的事实。
        if (sequence.is_cancelled()) {
            if (attempt == 1 && !replay_for_credentials) {
                outcome = Result<StreamOutcome>{
                    make_error(ErrorCode::NetCancelled, "the exchange was cancelled before it ran")};
            }
            break;
        }
        Cancellation token;
        // 墙到点即掐掉在途的这一次尝试（`Cancellation` 契约：钩子至多触发一次），随后本环据 `is_cancelled()` 收摊。
        sequence.on_cancel([token]() mutable { token.cancel(); });
        Request current = origin;
        if (attempt > 1 || replay_for_credentials) {
            // 上一次把流式体读到了半途，本次须从原点重发；回退失败即按不可重放收摊（与 `send` 同判据，认证环的
            // 重跑同样算「再发一遍」）。
            if (auto *source = current.body_source(); source != nullptr && !source->restart()) {
                outcome = Result<StreamOutcome>{make_error(
                    ErrorCode::HttpBodyNotReplayable,
                    "a streaming request body refused to return to its starting point before a retry attempt")};
                break;
            }
        }
        replay_for_credentials = false;
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
        // 逐跳的路由与 `CONNECT` 补充事实留一份到尝试级，供下面两处认证问答读（口径同 `send`）。
        HopRoute hop;
        detail::ProxyNegotiation negotiation;
        bool auth_hop = false;  // 本次尝试被认证问答掐住：头部尚未交出，凭据到位后整次重跑
        bool stale_retried = false;  // 本尝试是否已对一条陈旧的池连接重试过（一尝试一次，保证收敛）
        bool queue_failed = false;  // 本尝试是否在排队上没等到（connect 档到点或取消）：收尾据此把 detail 说成「排队」

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
        bool upload_total_known = false;  // 请求体总量给得出否（分块形态给不出）
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
            upload_total_known = false;
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
            hop = route;
            negotiation = detail::ProxyNegotiation{};
            if (hop.tunneled_via_proxy) {
                negotiation.proxy_authorization = ring.proxy_answer.value_or(std::string{});
            }
            // ── 写出物化：请求头部 + 体（与 `send` 同判据，含「挂了回调才分块写」那条）。
            //    代理答复只写面向代理那一侧，且从一份副本渲染（同 `send`：`current` 不沾它，后续跳换了直连或
            //    隧道就不会把代理凭据送出去）。
            auto outbound = std::optional<Request>{};
            const Request *on_wire = &current;
            if (route.absolute_form && ring.proxy_answer.has_value()) {
                outbound = current;
                outbound->set_header(auth_credential_field(true), *ring.proxy_answer);
                on_wire = &outbound.value();
            }
            auto head = on_wire->render_head(route.absolute_form);
            if (!head) {
                one = Result<StreamOutcome>{head.error()};
                break;
            }
            // TLS 握手与证书校验的对象恒是目标主机（隧道之上再握手），代理看不见这些。
            const std::string host{target.host()};

            // ── 建连并统一读写口：明文 / TLS 两条流经捕获 shared_ptr 的类型擦除闭包收敛到同一对 awaiter，
            //    第三枚「带窗口的读」只服务 `Expect: 100-continue`（与 `send` 同形态，见那里的说明）。
            //    连接池的取用与排队同 `send`（读那条的注释；流式路径的归还判据在收尾处，见下）。
            std::function<detail::CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
            std::function<detail::CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
            std::function<detail::CallbackAwaiter<detail::WindowedRead>(std::size_t, std::chrono::milliseconds)>
                read_within_one;
            auto late = std::make_shared<std::vector<char>>();
            StageTimer connect_timer(timeouts.connect, TimeoutStage::Connect, token, tstate);
            const bool retaining_leftover = pool_active(pool_, pool_options_);
            detail::HttpPoolLease lease;  // 空卫 = 池关着，析构零作为；池开着时析构即恰好归还一次
            bool leased_from_pool = false;
            bool force_fresh_connect = false;  // 陈旧连接重试的那一轮不再信任空闲条目（保证一定换新）
            if (retaining_leftover) {
                const auto key =
                    make_pool_key(route, target.is_secure(), tls_generation_ != nullptr ? *tls_generation_ : 0);
                lease = detail::HttpPoolLease{*pool_, key, pool_options_};
                for (;;) {
                    auto got = pool_->acquire(key, pool_options_, std::chrono::steady_clock::now());
                    if (got.verdict == detail::HttpPoolAcquire::Ready && force_fresh_connect) {
                        pool_->release(key, std::nullopt, got.born, pool_options_, std::chrono::steady_clock::now());
                        continue;  // 陈旧事件之后不再信任空闲条目：逐条丢弃，直到能开新连接
                    }
                    if (got.verdict == detail::HttpPoolAcquire::Ready) {
                        leased_from_pool = true;
                        lease.adopt(std::move(got.connection), got.born);
                        connect_timer.stop();  // 命中即停表：建连（含握手）这一段没有发生
                        break;
                    }
                    if (got.verdict == detail::HttpPoolAcquire::OpenNew) {
                        lease.adopt(std::move(got.connection), got.born);
                        break;
                    }
                    if (tstate->fired.load() != static_cast<int>(TimeoutStage::None)) {
                        one = Result<StreamOutcome>{make_error(
                            ErrorCode::HttpTimeout,
                            "connect timeout: queued too long for an idle connection (pool per-host limit)")};
                        queue_failed = true;
                        break;
                    }
                    co_await detail::await_sleep(AURORA_POOL_QUEUE_POLL, token);
                    if (token.is_cancelled() && tstate->fired.load() == static_cast<int>(TimeoutStage::None)) {
                        one = Result<StreamOutcome>{make_error(
                            ErrorCode::NetCancelled, "the exchange was cancelled while queued for a connection")};
                        queue_failed = true;
                        break;
                    }
                }
                if (queue_failed) {
                    break;
                }
            }
            std::shared_ptr<StreamTransport> plain_holder;
            std::shared_ptr<TlsTransport> secure_holder;
            if (leased_from_pool) {
                plain_holder = lease.connection().plain;
                secure_holder = lease.connection().secure;
            } else if (target.is_secure()) {
                auto plain =
                    co_await detail::await_connect_via(route.connect_with, route.peer_host, route.peer_port, options,
                                                       token, hop.tunneled_via_proxy ? &negotiation : nullptr);
                if (!plain) {
                    one = Result<StreamOutcome>{plain.error()};
                    break;
                }
                auto secure = co_await detail::await_handshake(std::move(plain.value()), host, tls_, token);
                if (!secure) {
                    one = Result<StreamOutcome>{secure.error()};
                    break;
                }
                secure_holder = std::make_shared<TlsTransport>(std::move(secure.value()));
            } else {
                auto plain =
                    co_await detail::await_connect_via(route.connect_with, route.peer_host, route.peer_port, options,
                                                       token, hop.tunneled_via_proxy ? &negotiation : nullptr);
                if (!plain) {
                    one = Result<StreamOutcome>{plain.error()};
                    break;
                }
                plain_holder = std::make_shared<StreamTransport>(std::move(plain.value()));
            }
            if (!leased_from_pool) {
                lease.adopt(detail::HttpPoolConnection{.plain = plain_holder, .secure = secure_holder}, lease.born());
            }
            if (secure_holder != nullptr) {
                read_one = [holder = secure_holder](std::span<char> buffer) {
                    return detail::await_read(*holder, buffer);
                };
                write_one = [holder = secure_holder](std::span<const char> data) {
                    return detail::await_write(*holder, data);
                };
                read_within_one = [holder = secure_holder, late](std::size_t capacity,
                                                                 std::chrono::milliseconds window) {
                    return detail::await_read_within(*holder, capacity, window, late);
                };
            } else {
                read_one = [holder = plain_holder](std::span<char> buffer) {
                    return detail::await_read(*holder, buffer);
                };
                write_one = [holder = plain_holder](std::span<const char> data) {
                    return detail::await_write(*holder, data);
                };
                read_within_one = [holder = plain_holder, late](std::size_t capacity,
                                                                std::chrono::milliseconds window) {
                    return detail::await_read_within(*holder, capacity, window, late);
                };
            }
            connect_timer.stop();

            // ── 发出请求：三条腿（快路一次写出 / 体泵逐段 / 先问再发体）与 `send` 同判据，理由见那里。
            //    装配器与还原链先于写出就位，因为窗口里那条 1xx 要在发出请求体之前喂进装配器。
            // ── 逐段装配并交付：装配器**不设线上体上限**（物化路径的内存闸在这里没有对应的内存可闸），
            //    每段解出的明文喂进流式还原链，链末直交宿主。
            wire::ResponseAssembler assembler{std::nullopt, current.is_head(), retaining_leftover};
            std::vector<char> scratch;  // 本段刚解出的线上字节（每轮先清空，故峰值只有一段的量）
            std::array<char, AURORA_READ_CHUNK> chunk{};
            std::optional<detail::StreamingDecoder> decoder;  // 头部定形后才建链（建链要读 Content-Encoding）
            bool halted = false;
            bool saw_fin = false;  // 本跳是否观察到对端 FIN（复用资格判据之一）
            // 陈旧连接重试的资格与 `send` 同判据（读那条的注释）。
            auto stale_retry_eligible = [&]() -> bool {
                return leased_from_pool && !stale_retried && !assembler.head_ready() &&
                       tstate->fired.load() == static_cast<int>(TimeoutStage::None) && !token.is_cancelled() &&
                       detail::is_idempotent_method(current.method());
            };
            bool redirect_hop = false;
            bool not_modified_hop = false;  // 本次交换替表里的条目重验证并拿到了 304

            // 产出出口：链末每交一段明文记一次交付量。宿主回 `Stop` 只中止交付，本库不替它决定成败。
            auto emit = [&outlet, &delivered_bytes](std::span<const char> bytes) -> Result<detail::EmitFlow> {
                delivered_bytes += bytes.size();
                const auto flow = outlet.on_data(bytes);
                return Result<detail::EmitFlow>{flow == SinkFlow::Stop ? detail::EmitFlow::Stop
                                                                       : detail::EmitFlow::Continue};
            };

            auto plan = detail::plan_request_body(current);
            if (!plan) {
                // 与 `send` 同理：`render_head()` 已按同一份判定交出过失败，这里只堵「声明与字节分岔」那一条路。
                one = Result<StreamOutcome>{plan.error()};
                break;
            }
            const bool await_continue = current.expect_continue() && plan.value().has_body;
            const bool one_shot = !gate.armed() && !await_continue &&
                                  plan.value().framing != detail::RequestBodyFraming::Chunked &&
                                  current.body_source() == nullptr;
            bool send_body = true;  // 是否获准发体：不等 100 时恒真
            bool carried = false;  // 窗口那批字节已喂进装配器：本环第一轮不再读，只补头部判定与交付
            if (one_shot) {
                std::string payload = head.value();
                payload.insert(payload.end(), current.body().begin(), current.body().end());
                auto written = co_await write_one(std::span<const char>{payload});
                if (!written) {
                    if (stale_retry_eligible()) {
                        stale_retried = true;
                        // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): 协程帧上的跨 continue 读，分析器不可见
                        force_fresh_connect = true;
                        continue;  // 空闲期被对端关了：幂等方法换新连接重跑本跳（一个字节都没出去过）
                    }
                    one = Result<StreamOutcome>{written.error()};
                    break;
                }
            } else {
                bool write_failed = false;
                auto written_head = co_await write_one(std::span<const char>{head.value()});
                if (!written_head) {
                    one = Result<StreamOutcome>{written_head.error()};
                    write_failed = true;
                }
                if (!write_failed && await_continue) {
                    // 那条 1xx 只喂进装配器、不做判定也不交付：头部判定在环内统一走，本处只要「对端回过话没有」
                    // 这一个答案。一条 101 在 `push()` 里当场落 `http-protocol-error`，与环内同形。
                    auto handed = co_await read_within_one(AURORA_READ_CHUNK, AURORA_EXPECT_WINDOW);
                    const auto &probe = handed.value();  // awaiter 恒以成功 `Result` 交出结局，失败在载荷里
                    if (probe.outcome == detail::WindowedRead::Outcome::Failure) {
                        one = Result<StreamOutcome>{*probe.error};
                        write_failed = true;
                    }
                    if (!write_failed && !probe.bytes.empty()) {
                        auto queued = assembler.push(std::string_view{probe.bytes.data(), probe.bytes.size()});
                        if (!queued) {
                            one = Result<StreamOutcome>{queued.error()};
                            write_failed = true;
                        } else {
                            carried = true;
                        }
                    }
                    if (!write_failed) {
                        // 只有对端把最终应答整个交出来、且它不是 1xx 才关死发体（那是 417 一类的直接作答）。
                        send_body = !assembler.head_ready() || assembler.has_informational();
                    }
                }
                if (!write_failed && send_body) {
                    detail::RequestBodyPump pump{current, plan.value()};
                    const bool length_known =
                        plan.value().framing != detail::RequestBodyFraming::Chunked && plan.value().length.has_value();
                    upload_total_known = length_known;
                    // 与 `send` 同一条口径：写出侧的停止条件不看进度回调挂没挂。
                    upload_total = plan.value().length.value_or(0);
                    if (gate.armed() && pump.has_body()) {
                        upload_running = true;
                    }
                    while (!pump.done()) {
                        auto segment = pump.next_segment();
                        if (!segment) {
                            one = Result<StreamOutcome>{segment.error()};
                            write_failed = true;
                            break;
                        }
                        if (segment.value().empty()) {
                            break;  // 已交完（分块终止块也已在上一轮随段交出）
                        }
                        auto written = co_await write_one(std::span<const char>{segment.value()});
                        if (!written) {
                            one = Result<StreamOutcome>{written.error()};
                            write_failed = true;
                            break;
                        }
                        uploaded = pump.payload_bytes();
                        const bool settled = length_known ? uploaded >= upload_total : pump.done();
                        if (gate.armed()) {
                            gate.observe(Progress{.phase = ProgressPhase::Upload,
                                                  .transferred_bytes = uploaded,
                                                  .total_bytes = upload_total,
                                                  .total_known = length_known,
                                                  .finished = settled});
                            gate.flush();
                        }
                        upload_settled = upload_settled || settled;
                        if (settled) {
                            break;
                        }
                    }
                }
                if (write_failed) {
                    if (stale_retry_eligible()) {
                        stale_retried = true;
                        // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): 协程帧上的跨 continue 读，分析器不可见
                        force_fresh_connect = true;
                        continue;  // 头部都还没到就折了：幂等方法换新连接重跑本跳
                    }
                    break;
                }
            }

            while (!assembler.complete() || carried) {
                if (carried) {
                    carried = false;  // 窗口里那批字节已在装配器内，本轮直接从判定与交付走
                } else if (!late->empty()) {
                    // 被放弃那次「带窗口的读」后来到过的字节排在本次读之前（传输层的 FIFO 口径），先兑现它的
                    // 线上次序。队列只可能被填一次，喂完即清。
                    auto queued = assembler.push(std::string_view{late->data(), late->size()});
                    late->clear();
                    if (!queued) {
                        one = Result<StreamOutcome>{queued.error()};
                        halted = true;
                        break;
                    }
                } else {
                    StageTimer read_timer(timeouts.read_idle, TimeoutStage::ReadIdle, token, tstate);
                    auto read = co_await read_one(std::span<char>{chunk});
                    read_timer.stop();
                    if (!read) {
                        one = Result<StreamOutcome>{read.error()};
                        halted = true;
                        break;
                    }
                    // 被放弃那次「带窗口的读」后来到过的字节排在本次读之前（传输层的 FIFO 口径），先兑现它的
                    // 线上次序再判本轮结局——结局即便是「对端已关」也一样：那批字节可能正是完整的一条应答。
                    // 队列只可能被填一次，喂完即清。
                    if (!late->empty()) {
                        auto queued = assembler.push(std::string_view{late->data(), late->size()});
                        late->clear();
                        if (!queued) {
                            one = Result<StreamOutcome>{queued.error()};
                            halted = true;
                            break;
                        }
                    }
                    const std::size_t got = read.value();
                    if (got == 0) {  // 对端 FIN：UntilClose 据此判全，其余形态提前收摊即协议错位
                        saw_fin = true;
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
                        // 不在这里 break：本轮可能就是「头部与整段体都出自暂存字节」的那一轮，宿主的
                        // `on_head` → `on_data` 次序得跟着走完，之后 `while` 的条件自然收场。
                    } else if (!assembler.complete()) {
                        auto pushed = assembler.push(std::string_view{chunk.data(), got});
                        if (!pushed) {
                            one = Result<StreamOutcome>{pushed.error()};
                            halted = true;
                            break;
                        }
                    }
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
                    // ── 认证问答（三环次序与五条裁决见 `net/http_auth.h`）。流式路径的难点在位置：重播资格在
                    //    `on_head` 那一次转发时就耗尽，首段字节交出去收不回来，故本环**只能问在交付之前**、且在
                    //    协程体内就地展开（`co_await` 出不了普通函数，口径同本文件开头的「刻意不做抽象」）。
                    //    三种结局：拿到凭据即掐掉本跳整次重跑；提供方拒答就往下走，那枚 401/407 是对端给的答案、
                    //    照原版交给宿主；其余（超时 / 提供方失败 / 答复含 CR 或 LF）按错误收摊，绝不交付半份头。
                    if (ring.provider != nullptr && !ring.asked && is_auth_challenge(assembler.head().status)) {
                        const bool from_proxy = assembler.head().status == 407;
                        const std::string_view host_str =
                            current.url().has_value() ? current.url()->host() : std::string_view{};
                        auto challenge = make_auth_challenge(
                            current, assembler.head().headers.values(auth_challenge_field(from_proxy)),
                            assembler.head().status, host_str);
                        ring.asked = true;  // 问过即不再问：重跑后又吃一次挑战，交出的就是那一份应答
                        // 挑战存在**交换作用域**上而不是本块的临时量里：提供方若在问答中途挂起、又被本环放弃
                        // （超时或取消），它那帧协程仍可能引用这份事实（`Authenticator::authenticate` 的存活期契约）。
                        asked_challenge = std::move(challenge);
                        auto task = ring.provider->authenticate(*asked_challenge);
                        for (std::size_t polls = 0; !task.is_done() && polls < AURORA_AUTH_POLL_CEILING; ++polls) {
                            co_await detail::await_sleep(AURORA_AUTH_POLL_INTERVAL, token);
                        }
                        const std::optional<AuthReply> reply = [&]() -> std::optional<AuthReply> {
                            if (!task.is_done()) {
                                one = Result<StreamOutcome>{
                                    make_error(ErrorCode::HttpTimeout, "the authenticator never answered")};
                                return std::nullopt;
                            }
                            // 双层解包：`CoroTask<Result<T>>::result()` 求得 `Result<Result<T>>`——外层是「这帧协程
                            // 有没有跑完」、内层才是「提供方有没有给出凭据」，两枚失败各有各的说法，不得合流。
                            const auto outer = task.result();
                            if (!outer) {
                                one = Result<StreamOutcome>{
                                    make_error(ErrorCode::RuntimeCoroutineException, outer.error().message)};
                                return std::nullopt;
                            }
                            const auto &answered = outer.value();
                            if (!answered) {
                                // 提供方自己的失败就是本次交换的结局：拿一份没到手的凭据去重发，只是把一次明确的
                                // 失败拖成一次含糊的 401。
                                one = Result<StreamOutcome>{answered.error()};
                                return std::nullopt;
                            }
                            const auto placed = place_auth_decision(*asked_challenge, answered.value(), origin, ring);
                            if (placed == AuthReply::Malformed) {
                                one = Result<StreamOutcome>{
                                    make_error(ErrorCode::HttpRequestInvalid,
                                               "the authenticator answered with a field carrying CR or LF")};
                                return std::nullopt;
                            }
                            return placed;
                        }();
                        if (!reply.has_value()) {
                            halted = true;
                            break;
                        }
                        if (reply != AuthReply::Declined) {
                            auth_hop = true;
                            replay_for_credentials = true;
                            halted = true;
                            break;  // 头部尚未交出、字节尚未交付：本跳作废，回到尝试环开头带凭据重跑
                        }
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
                if (stale_retry_eligible()) {
                    stale_retried = true;
                    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): 协程帧上的跨 continue 读，分析器不可见
                    force_fresh_connect = true;
                    continue;  // 头部都还没到就读折了：幂等方法换新连接重跑本跳（多半是空闲期被对端关了）
                }
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
                Response resp{assembler.head().status, assembler.head().reason, assembler.head().headers, {}};
                // 重定向跳只读了头部、体未读：这条连接上还压着没归属的字节，绝不回池（卫未武装，析构即关）。
                harvest_set_cookies(cookie_jar_, target, resp.headers(), target.is_secure());
                auto next = follow(current, resp);
                if (!next) {
                    one = Result<StreamOutcome>{next.error()};
                    break;
                }
                current = std::move(next.value());
                // 下一跳按新目标的域重新从 Jar 取（与 `send` 同一条逐跳纪律）。
                attach_cookies_from_jar(current, cookie_jar_);
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
                    // 304 那条消息已完整读过（无体、无 leftover 风险面之外的富余即被容忍模式记下），判干净即回池。
                    if (retaining_leftover && assembler.take_leftover().empty() &&
                        response_allows_reuse(assembler.head(), assembler.framing(), saw_fin)) {
                        lease.arm();
                    }
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
            // ── 应答侧的 Jar 收取与复用判定：最终一跳的消息已完整交付（宿主回 Stop 的收摊不算——那条流
            //    没走完，连接上还压着未读字节，卫未武装即析构关闭）。
            harvest_set_cookies(cookie_jar_, target, assembler.head().headers, target.is_secure());
            if (retaining_leftover && !stopped_by_sink && assembler.take_leftover().empty() &&
                response_allows_reuse(assembler.head(), assembler.framing(), saw_fin)) {
                lease.arm();  // 五判据全过：本跳出口（析构卫）把这条连接交回池
            }
            break;
        }

        // 超时改道：与 `send` 同判据——失败若因某档看门狗取消了令牌，一律改报 http-timeout 并标明是哪一档。
        // 排队等不到空闲连接也走 connect 档，但成因要说成「排队」而不是「连不上」（细节同 `send` 那处）。
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
        if (queue_failed && tstate->fired.load() == static_cast<int>(TimeoutStage::Connect)) {
            one = Result<StreamOutcome>{
                make_error(ErrorCode::HttpTimeout,
                           "connect timeout: queued too long for an idle connection (pool per-host limit)")};
        }

        // 进度收尾：与 `send` 同判据，两方向末值各发一次、不看闸号；成败由 `one` 表达。
        if (gate.armed()) {
            if (upload_running && !upload_settled) {
                gate.observe(Progress{.phase = ProgressPhase::Upload,
                                      .transferred_bytes = uploaded,
                                      .total_bytes = upload_total,
                                      .total_known = upload_total_known,
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

        // ── 隧道那一侧的问答：代理把 `CONNECT` 拒成 407 时，那次失败带着挑战原文回来（见
        //    `detail::ProxyNegotiation`），此时还没有头部可问，故本块的地位等同 `send` 里的同一处。答复只写
        //    `Proxy-Authorization` 一份、且只上面向代理那条腿（`place_auth_decision` 的分工，口径同 `send`）。
        if (!auth_hop && ring.provider != nullptr && !ring.asked && !one && hop.tunneled_via_proxy &&
            negotiation.status == 407) {
            auto challenge =
                make_auth_challenge(current, negotiation.authenticate, negotiation.status, hop.proxy_label);
            ring.asked = true;  // 问过即不再问：重跑后又吃一次挑战，交出的就是那一份应答
            asked_challenge = std::move(challenge);  // 存活期契约同上：挑战住在交换作用域而非本块临时量里
            auto task = ring.provider->authenticate(*asked_challenge);
            for (std::size_t polls = 0; !task.is_done() && polls < AURORA_AUTH_POLL_CEILING; ++polls) {
                co_await detail::await_sleep(AURORA_AUTH_POLL_INTERVAL, token);
            }
            if (!task.is_done()) {
                outcome = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "the authenticator never answered")};
                break;
            }
            const auto outer = task.result();  // 双层解包：外层是帧跑完否，内层才是提供方给没给凭据
            if (!outer) {
                outcome =
                    Result<StreamOutcome>{make_error(ErrorCode::RuntimeCoroutineException, outer.error().message)};
                break;
            }
            const auto &answered = outer.value();
            if (!answered) {
                outcome = Result<StreamOutcome>{answered.error()};
                break;
            }
            const auto reply = place_auth_decision(*asked_challenge, answered.value(), origin, ring);
            if (reply == AuthReply::Malformed) {
                outcome = Result<StreamOutcome>{make_error(
                    ErrorCode::HttpRequestInvalid, "the authenticator answered with a field carrying CR or LF")};
                break;
            }
            if (reply != AuthReply::Declined) {
                replay_for_credentials = true;
                continue;  // 回到本环开头重跑：新令牌、新的逐尝试 `total`、隧道带上补好的代理凭据
            }
        }

        // 头部问答在协程体内就地掐掉了本跳（那里说明过为什么只能问在那一步）：凭据已就位，整次重跑。
        if (auth_hop) {
            continue;
        }

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
        // 递增只发生在「这一次算重试环的一次尝试」那条路上：认证问答的重跑从上面直接 `continue`，绕开这里，
        // 故它不吃 `RetryPolicy::attempts` 名额（那条裁决见 `net/http_auth.h`）。
        ++attempt;
    }

    // 被跨尝试的墙掐掉的尝试以 `net-cancelled` 形态抵达，那既不是对端说的也不是本库造的码——按 `total`
    // 档的先例改报 `http-timeout` 并在 detail 点名是 budget，使「超时」在两枚旋钮之间仍可分辨。
    if (!outcome && wall->fired.load() == static_cast<int>(TimeoutStage::Budget)) {
        outcome = Result<StreamOutcome>{make_error(ErrorCode::HttpTimeout, "retry budget exhausted")};
    }
    co_return outcome;
}

// Sink 由调用方持有、协程帧只借用（`include/aurora/net/http_client.h` 的声明处同一口径）。声明处的那条紧邻豁免
// 不跨翻译单元传导：本定义被 `http_binding.cpp` 之外的一切包含者看到时，这里就是它的告警点。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
auto Client::connect_websocket(Request request, WebSocketSink &sink, WebSocketOptions options,
                               Cancellation cancellation) const -> CoroTask<Result<WebSocket>> {
    // 取消先于一切判定，与 `send` / `open_stream` 同一位置：已取消的令牌不问策略、不看本腿能力，也不调宿主
    // 那四个入口的任何一口——那次会话从未存在过，`on_closed` 也无从谈起。
    if (cancellation.is_cancelled()) {
        co_return Result<WebSocket>{
            make_error(ErrorCode::NetCancelled, "the upgrade handshake was cancelled before it started")};
    }
    // 方法闸在起手，而不是「静默改成 GET」：调用方以为发出去了体、对端收到的却是一条无体 GET，比一次失败
    // 更难排查。协议文法只承认 GET 升级（`RFC 6455 §5.1`）。
    if (request.method() != Method::Get) {
        co_return Result<WebSocket>{
            make_error(ErrorCode::HttpRequestInvalid, "only GET can be upgraded to a WebSocket connection")};
    }
    // 本腿要的是**一条可以被持续读写的裸连接**；wasm 的 `fetch` 连可读的 socket 都不给（裁决见提案 §16 第 3
    // 项）。绝不退化成别的形态：拿一次物化交换冒充一条不会结束的会话，比直接失败更误导调用方。
    if (!Engine::instance().capabilities().streaming) {
        co_return Result<WebSocket>{
            make_error(ErrorCode::NetPlatformUnavailable, "connect_websocket: this leg has no raw byte stream")};
    }

    Request origin = std::move(request);
    // 链路标识与 Cookie 附着同另两腿：写在原始请求上，一次交换一枚。
    stamp_request_id(origin, request_id_);
    log_exchange_identity(origin);
    attach_cookies_from_jar(origin, cookie_jar_);
    // 策略读定与起手闸同另两腿：被拒时连头部都没渲染，更不发起连接。
    const NetPolicy security = effective_policy(policy_);
    if (const auto verdict = gate_origin(proxy_, tls_, security, origin); !verdict) {
        co_return Result<WebSocket>{verdict.error()};
    }
    const auto &slot = origin.url();
    if (!slot.has_value()) {
        // 目标解析失败由 `render_head()` 说（它带着解析时记下的那枚原因），策略不抢这个活。
        auto undeliverable = origin.render_head();
        co_return Result<WebSocket>{undeliverable.error()};
    }
    const Url &target = slot.value();
    const std::string_view scheme = target.scheme();
    if (scheme != "ws" && scheme != "wss") {
        co_return Result<WebSocket>{
            make_error(ErrorCode::WsHandshakeRejected,
                       ErrorParams{{"detail", "the target scheme is '" + std::string{scheme} + "', not ws or wss"}})};
    }
    const auto port = target.effective_port();
    if (!port.has_value()) {
        co_return Result<WebSocket>{
            make_error(ErrorCode::WsHandshakeRejected, ErrorParams{{"detail", "the target has no effective port"}})};
    }
    // 路由与另两腿同一条 `plan_route`（库内「有没有经代理」只有这一个答案）：`ws://` 遇明文转发代理时请求行
    // 写成绝对形态，`wss://` 一律走隧道——隧道之上的 TLS 握手对象恒是目标主机。
    auto routed = plan_route(proxy_, tls_, security, target, *port);
    if (!routed) {
        co_return Result<WebSocket>{routed.error()};
    }
    const HopRoute &route = routed.value();  // 只借用：`routed` 活到本函数收尾，与另两腿同一形态

    // 令牌与看门狗只服务握手这一段：结算之后这条连接归会话与句柄，取消不再掐一条宿主正在使用的会话。
    // `read_idle` 在本腿上无事可管——它管的是「一次交换的两段读之间」，而升级后的读环属于会话。
    Cancellation token;
    cancellation.on_cancel([token]() mutable { token.cancel(); });
    auto tstate = std::make_shared<TimeoutState>();
    const Timeouts timeouts = origin.timeouts();
    StageTimer total_timer(timeouts.total, TimeoutStage::Total, token, tstate);
    StageTimer connect_timer(timeouts.connect, TimeoutStage::Connect, token, tstate);
    const StreamOptions stream_options;
    detail::ProxyNegotiation negotiation;  // 只带回 `CONNECT` 的补充事实；本腿不进认证环
    Result<WebSocket> outcome{make_error(ErrorCode::HttpProtocolError, "connect_websocket produced no outcome")};

    for (;;) {
        // ── 起新连接：本腿不进池（句柄是这条连接唯一的持有者，池既拿不到它也收不回它）。
        std::shared_ptr<StreamTransport> plain_holder;
        std::shared_ptr<TlsTransport> secure_holder;
        auto plain = co_await detail::await_connect_via(route.connect_with, route.peer_host, route.peer_port,
                                                        stream_options, token, &negotiation);
        if (!plain) {
            outcome = Result<WebSocket>{plain.error()};
            break;
        }
        if (target.is_secure()) {
            auto secure =
                co_await detail::await_handshake(std::move(plain.value()), std::string{target.host()}, tls_, token);
            if (!secure) {
                outcome = Result<WebSocket>{secure.error()};
                break;
            }
            secure_holder = std::make_shared<TlsTransport>(std::move(secure.value()));
        } else {
            plain_holder = std::make_shared<StreamTransport>(std::move(plain.value()));
        }
        connect_timer.stop();
        std::function<detail::CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
        std::function<detail::CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
        if (secure_holder != nullptr) {
            read_one = [holder = secure_holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
            write_one = [holder = secure_holder](std::span<const char> data) {
                return detail::await_write(*holder, data);
            };
        } else {
            read_one = [holder = plain_holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
            write_one = [holder = plain_holder](std::span<const char> data) {
                return detail::await_write(*holder, data);
            };
        }

        // ── 写出握手：四枚字段由库写定，调用方写的同名值被覆写（口径见 `detail/ws_handshake.h`）；体与
        //    `Content-Length` / `Transfer-Encoding` 在那一层已被剥掉，本腿也压根不看 `Expect: 100-continue`。
        const detail::WsHandshakeOffer offer{.subprotocols = options.subprotocols,
                                             .require_subprotocol = options.require_subprotocol};
        std::string key;
        auto head = detail::render_ws_handshake(target, origin.headers(), offer, route.absolute_form, key);
        if (!head) {
            outcome = Result<WebSocket>{head.error()};
            break;
        }
        const std::string handshake_bytes = std::move(head.value());
        auto written = co_await write_one(std::span<const char>{handshake_bytes});
        if (!written) {
            outcome = Result<WebSocket>{written.error()};
            break;
        }

        // ── 逐段读头部块，装配器一律不上：`wire::ResponseAssembler` 把 101 当场判成协议违例（那对一条普通
        //    交换是正确的），而这里要的恰恰是那一句应答。`parse_response_head` 只吃头部块自身占用的字节。
        std::string buffered;
        std::optional<wire::ResponseHead> head_block;
        bool interrupted = false;
        std::array<char, AURORA_READ_CHUNK> chunk{};
        while (!head_block.has_value()) {
            auto got = co_await read_one(std::span<char>{chunk});
            if (!got) {
                outcome = Result<WebSocket>{got.error()};
                interrupted = true;
                break;
            }
            if (got.value() == 0U) {
                outcome = Result<WebSocket>{make_error(
                    ErrorCode::WsHandshakeRejected,
                    ErrorParams{{"detail", "the peer closed the connection without answering the upgrade"}})};
                interrupted = true;
                break;
            }
            buffered.append(chunk.data(), got.value());
            auto parsed = wire::parse_response_head(std::string_view{buffered});
            if (!parsed) {
                outcome = Result<WebSocket>{parsed.error()};
                interrupted = true;
                break;
            }
            head_block = parsed.value();  // 空 = 头部块还没读完，继续读下一段
        }
        if (interrupted) {
            break;
        }

        auto accept = detail::validate_ws_handshake(*head_block, key, offer);
        if (!accept) {
            outcome = Result<WebSocket>{accept.error()};
            break;
        }
        // 应答侧的 Jar 收取照做：升级应答里的 `Set-Cookie` 同样是服务端要本会话记住的身份（畸形条目逐条丢弃）。
        harvest_set_cookies(cookie_jar_, target, head_block->headers, target.is_secure());
        // 头部块之后已经到手的字节属于帧流：一次读不会刚好停在那道边界上。交给会话先喂分帧器，既不丢半条
        // 消息，也不把它当成「上一条消息的余货」判成畸形。
        std::string leftover;
        if (head_block->header_bytes < buffered.size()) {
            leftover = buffered.substr(head_block->header_bytes);
        }
        auto session = std::make_shared<detail::WsSession>(plain_holder, secure_holder, sink, std::move(options),
                                                           accept.value().subprotocol, std::move(leftover));
        session->start(session);
        outcome = Result<WebSocket>{WebSocket{session}};
        // 交权即撤钩：本枚令牌到此刻说完了「这次交换别做了」，此后这条连接归句柄与会话（口径见公共头文件头
        // 末段）。不撤的话，宿主日后取消一枚它早已不该过问的令牌，会经建连时登记的连接级钩子把会话当场掐断
        // ——同一份资源两个互不知情的所有者。`Cancellation::on_cancel` 交空体即撤销登记（`net/cancellation.h`）。
        cancellation.on_cancel({});
        break;
    }

    // 超时改道与另两腿同判据：失败若因某档看门狗取消了令牌，一律改报 `http-timeout` 并标明是哪一档；
    // 令牌本身被取消则留着 `net-cancelled`——那是宿主说的手，不是时限到的事实。
    if (!outcome) {
        switch (static_cast<TimeoutStage>(tstate->fired.load())) {
            case TimeoutStage::Connect:
                outcome = Result<WebSocket>{make_error(ErrorCode::HttpTimeout, "connect timeout")};
                break;
            case TimeoutStage::Total:
                outcome = Result<WebSocket>{make_error(ErrorCode::HttpTimeout, "total timeout")};
                break;
            case TimeoutStage::ReadIdle:  // 本腿不设读空闲档：升级后的读环不属于这次交换
            case TimeoutStage::Budget:  // 跨尝试的墙属重试环，本腿既无重试也无外层登记
            case TimeoutStage::None:
                break;
        }
    }
    co_return outcome;
}
}  // namespace aurora::net::http
