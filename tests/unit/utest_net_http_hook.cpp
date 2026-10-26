/// 测试类型: unit
/// 目标单元: include/aurora/net/http_hook.h + src/aurora/net/http_hook.cpp +
///           src/aurora/net/detail/http_hook_chain.* + Client 四腿的钩子落点
/// 测试说明: 交换钩子面的离线回归，按「面 — 链 — 落点」三层展开。
///           **面**（类型边界与登记形态）验视图只给读口与头改写通道（体、`BodySource`、`range` /
///           `if_range` / `expect_continue` / `timeout` 与协议旋钮在编译期就够不着，拷贝与移动皆删，
///           构造点私有）、登记的 `Client` 拷贝共用一条链、句柄移动即转移注销权、句柄比发它的 `Client`
///           活得久而不崩、空指针登记是空操作、同一实例登记两次被问两次。
///           **链**（次序与差分）验登记次序即调用次序且后写的同名值取胜、本枚视图取的是它自己入口时刻的
///           快照（看不见自己的改写、看得见前一枚的改写）、快照语义（回调期间注销另一枚不影响本次交换、
///           回调期间登记的下一枚本次不被问）、链跑完就地复检取消（钩子内取消 ⇒ `net-cancelled` 且短路值
///           作废、收摊问一概不问）、被策略拒绝或起手已取消的目标一次都不问、递归闸两问各证一次（钩子
///           在回调里同步发起嵌套交换时该枚被跳过并恰记一行 WARN，链上其余照常）、两问抛异常的后果分家
///           （起手抛即整次 `http-request-invalid` 且其余各枚不再问；收摊抛被吞掉、应答照常交付、异常原文
///           与 URL 一概不留进日志）。
///           **落点**（四腿与三条交付路）验保留头表：`send` 腿十三枚 × 新增 / 改值 / 删除 / 换一种大小写
///           拼写四形态逐枚被拒，detail 只点名钩子序号、腿与规范拼写而不带值；`websocket` 腿再加握手四字段，
///           同一枚字段在 `send` 腿上照常可写；`X-Request-Id` 被改写即拒，未碰时调用方的那一份在起手视图、
///           收摊视图、活动记录与线上四处处处相同。空钩子与不挂钩子的线上字节逐字节相同，加一枚字段是唯一
///           差异。短路只在 `send` 成立且是**终点**：一条连接都不发起、`from_cache` 为假、不查不写缓存、
///           不跟随 `Location`、不再解压、不发进度；交出失败值即本次结局；伪造缓存命中被拒；其余三腿交出
///           短路值一律如实被拒而不是静默忽略。收摊问恰在「交出一枚应答」时问一次（线上 / 新鲜缓存副本 /
///           短路成功，含非 2xx），失败与被取消不问，三条流式腿一概不问；其视图是钩子改写后的逻辑请求
///           （不含库内补的 `Accept-Encoding` / `Host` / 分帧字段，跨 origin 重定向后仍是原始目标）。触点
///           差异集两条去处：跨 origin 的下一跳剥掉钩子经手过的字段而调用方自写的存活、同 origin 全量带走；
///           缓存建键把触点并入 `Vary`，换租户即不命中，无钩子的裸请求也不命中带触点的条目。`Authenticator`
///           的答复压过钩子写下的同名凭据，且认证重跑不重复问钩子。网络活动记账（`AURORA_ENABLE_DEBUG`
///           限定）验短路交付与「真实交换 / 新鲜缓存命中 / 本地拒绝」三类记录可互相分辨。
///           全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免；浏览器腿没有 BSD socket
///           语义，触网那一族按 `AURORA_TEST_SKIP` 登记。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/blocking.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/http.h"
#include "aurora/net/http_auth.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_hook.h"
#include "aurora/net/http_observability.h"
#include "aurora/net/http_sse.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/policy.h"
#include "aurora/net/websocket.h"
#include "aurora/state/coroutine.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"
#include "framework/ws_peer.h"

namespace aurora::test_cases::utest_net_http_hook {
namespace {

using aurora::CoroTask;
using aurora::ErrorCode;
using aurora::Headers;
using aurora::Result;
using aurora::net::Cancellation;
using aurora::net::NetPolicy;
using aurora::net::WebSocket;
using aurora::net::WebSocketOptions;
using aurora::net::WebSocketSink;
using aurora::net::WsMessageKind;
using aurora::net::http::BearerAuthenticator;
using aurora::net::http::ByteRange;
using aurora::net::http::CacheOptions;
using aurora::net::http::Client;
using aurora::net::http::DataSink;
using aurora::net::http::ExchangeHook;
using aurora::net::http::ExchangeHookRegistration;
using aurora::net::http::ExchangeLeg;
using aurora::net::http::ExchangeRequestView;
using aurora::net::http::HttpActivityEntry;
using aurora::net::http::HttpRequestTimeline;
using aurora::net::http::Method;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::SinkFlow;
using aurora::net::http::SseEvent;
using aurora::net::http::SseOptions;
using aurora::net::http::SseOutcome;
using aurora::net::http::SseSink;
using aurora::net::http::StreamOutcome;
using aurora::test::dead_port;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::per_connection;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::silent;
using aurora::test::ws::serve_ws;
using aurora::test::ws::WsLog;
using aurora::test::ws::WsPlan;

/// @brief 拼一个指向回环端口的 GET 目标。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 拼一个指向回环端口的 `ws://` 目标。
[[nodiscard]] auto ws_url(std::uint16_t port, std::string_view path = "/chat") -> std::string {
    return "ws://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 为测试放行明文：缺省安全策略拒绝一切未列入白名单的 `http://` 目标，而本套件的对端恒在回环地址上。
/// @param client 待配置的客户端。
auto allow_test_cleartext(Client &client) -> void {
    NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};
    client.policy(std::move(permissive));
}

/// @brief 物化腿的收尾：泵到协程结束并交出应答那一层。
/// @param client 发起用的客户端。
/// @param request 待发出的请求。
/// @param token 取消令牌。
/// @param label 未收摊时的现场名（表驱动的用例要靠它认出是哪一枚）。
[[nodiscard]] auto send_and_drive(const Client &client, Request request, Cancellation token = {},
                                  std::string_view label = "send") -> Result<Response> {
    auto task = client.send(std::move(request), std::move(token));
    // 未收摊就取 `result()` 是对未初始化的结果存储动手，故这里用致命断言把它挡在前面。
    AURORA_TEST_REQUIRE_MSG(aurora::test::net::drive_until(
                                label, [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                                .satisfied,
                            label);
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 流式腿的收尾：`HostDriven` 由用例自己装（钩子回调内还要嵌套泵，装在本函数里就会提前把驱动方交回）。
[[nodiscard]] auto open_and_drive(const Client &client, Request request, DataSink &sink) -> Result<StreamOutcome> {
    auto task = client.open_stream(std::move(request), sink);
    AURORA_TEST_REQUIRE(aurora::test::net::drive_until(
                            "open_stream done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                            .satisfied);
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 订阅腿的收尾。
[[nodiscard]] auto subscribe_and_drive(const Client &client, Request request, SseSink &sink) -> Result<SseOutcome> {
    auto task = client.open_sse(std::move(request), sink);
    AURORA_TEST_REQUIRE(aurora::test::net::drive_until(
                            "open_sse done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                            .satisfied);
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 升级腿的收尾（句柄是 move-only，外层只读一次）。
[[nodiscard]] auto connect_and_drive(const Client &client, Request request, WebSocketSink &sink) -> Result<WebSocket> {
    auto task = client.connect_websocket(std::move(request), sink, WebSocketOptions{});
    AURORA_TEST_REQUIRE(
        aurora::test::net::drive_until(
            "connect_websocket done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
            .satisfied);
    auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return Result<WebSocket>{std::move(outer.value())};
}

/// @brief 关掉重连的订阅口径：单流用例要的正是「一条流就是全部」。
[[nodiscard]] auto single_stream() -> SseOptions {
    SseOptions options;
    options.reconnect = false;
    return options;
}

/// @brief 立刻重连的订阅口径：额度一枚，窗口压到零，重连判据不必为一次真实睡眠买单。
[[nodiscard]] auto immediate_reconnect(std::uint32_t max_reconnects) -> SseOptions {
    SseOptions options;
    options.max_reconnects = max_reconnects;
    options.retry_default = std::chrono::milliseconds{0};
    return options;
}

/// @brief 宿主驱动守卫 + 收尾收摊泵（与 `utest_net_websocket` 的 `DrivenHost` 同一形态、同一理由）。
///
/// 句柄析构那枚「尽力投递」的 close 要靠宿主 pump 才走得完，故收摊泵跑在对端停机之前；声明在钩子与
/// 客户端之前，销毁顺序才落在它们之后。
struct DrivenHost {
    DrivenHost() = default;
    // NOLINTNEXTLINE(bugprone-exception-escape): 析构期没有可回报的通道，判据在泵之前已落定
    ~DrivenHost() { aurora::test::net::pump_idle(std::chrono::milliseconds{30}); }
    DrivenHost(const DrivenHost &) = delete;
    auto operator=(const DrivenHost &) -> DrivenHost & = delete;
    DrivenHost(DrivenHost &&) = delete;
    auto operator=(DrivenHost &&) -> DrivenHost & = delete;
    aurora::test::net::HostDriven driven;  ///< 宿主驱动开关
};

/// @brief 把文本按 ASCII 小写化（字段名比较不依赖大小写）。
[[nodiscard]] auto lowered(std::string_view text) -> std::string {
    std::string out{text};
    for (char &unit : out) {
        if (unit >= 'A' && unit <= 'Z') {
            unit = static_cast<char>(static_cast<unsigned char>(unit) + 32);
        }
    }
    return out;
}

/// @brief 文本里是否含某段子串。
[[nodiscard]] auto contains(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

/// @brief 逐字母翻转大小写（`Host` → `hOST`）：字段名比较大小写不敏感，故这是「换一种拼写」的形态。
[[nodiscard]] auto recased(std::string_view text) -> std::string {
    std::string out{text};
    for (char &unit : out) {
        if (unit >= 'A' && unit <= 'Z') {
            unit = static_cast<char>(static_cast<unsigned char>(unit) + 32);
        } else if (unit >= 'a' && unit <= 'z') {
            unit = static_cast<char>(static_cast<unsigned char>(unit) - 32);
        }
    }
    return out;
}

/// @brief 删掉文本里第一处指定的整行（含其 CRLF），用于「加一枚字段是线上唯一差异」的比对。
/// @param text 原文。
/// @param line 要删的那一行的完整文本（不含 CRLF）。
/// @return 删除后的副本；未命中即原样交出。
[[nodiscard]] auto erase_line(std::string_view text, std::string_view line) -> std::string {
    const std::string needle = std::string{line} + "\r\n";
    std::string out{text};
    const auto at = out.find(needle);
    if (at != std::string::npos) {
        out.erase(at, needle.size());
    }
    return out;
}

/// @brief 200 应答：`Content-Length` 分帧 + 可读短体。
[[nodiscard]] auto service_ok(std::string_view fields = {}, std::string_view body = "ok") -> std::string {
    return "HTTP/1.1 200 OK\r\n" + std::string{fields} + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
           std::string{body};
}

/// @brief 逐条连接把请求头部块留档、随后按给定应答收摊的对端脚本。
/// @param filed 每条连接一份档案（脚本线程追加）。
/// @param guard 档案的锁。
/// @param payload 读满一个头部块后写出的应答字节。
/// @return 对端脚本。
[[nodiscard]] auto record_each_then_reply(std::vector<std::string> &filed, std::mutex &guard, std::string payload)
    -> PeerScript {
    return [&filed, &guard, payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket,
                                                          PeerStop &stop) -> void {
        (void)socket.set_io_timeout(std::chrono::seconds{10});
        std::vector<char> buffer(4096);
        std::string seen;
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                break;
            }
            seen.append(buffer.data(), read.value());
            if (seen.find("\r\n\r\n") != std::string::npos) {
                break;  // 头部块到位即应答（本套件都不带请求体）
            }
        }
        {
            const std::scoped_lock held(guard);
            filed.push_back(seen);
        }
        (void)socket.write_all(std::span<const char>{payload});
    };
}

/// @brief 取第 `index` 条连接的档案（小写化副本）；还没留到那一条即致命失败。
[[nodiscard]] auto filed_request(const std::vector<std::string> &filed, std::mutex &guard, std::size_t index)
    -> std::string {
    std::vector<std::string> copy;
    {
        const std::scoped_lock held(guard);
        copy = filed;
    }
    AURORA_TEST_REQUIRE_MSG(index < copy.size(), "the peer has not filed that many requests");
    return lowered(copy.at(index));
}

/// @brief 等到对端脚本把第 `want` 条请求留档（档案落在线程脚本侧，取早了是空）。
[[nodiscard]] auto await_filed(const std::vector<std::string> &filed, std::mutex &guard, std::size_t want) -> bool {
    return aurora::test::net ::drive_until("peer filed the request",
                                           [&filed, &guard, want] {
                                               const std::scoped_lock held(guard);
                                               return filed.size() >= want;
                                           })
        .satisfied;
}

/// @brief 捕获 Logger 的 sink 以断言诊断留痕；析构恢复全局缺省，不泄漏给其它用例。
///
/// 构造即把阈值压到 Warn 并确认输出开着：本套件的判据有两枚（递归闸跳过、收摊问抛异常）只能由 WARN 行
/// 证明，缺省阈值虽然也放行 Warn，但同一条 runner 里前一个用例改过阈值不留副本，故这里显式定住。
class CapturedLogger {
  public:
    CapturedLogger() : level_(aurora::Logger::instance().level()), enabled_(aurora::Logger::instance().is_enabled()) {
        auto &logger = aurora::Logger::instance();
        logger.set_level(aurora::LogLevel::Warn);
        logger.set_enabled(true);
        logger.set_sink([this](std::string_view line) -> void { lines.emplace_back(line); });
    }
    ~CapturedLogger() {
        auto &logger = aurora::Logger::instance();
        logger.set_level(level_);
        logger.set_enabled(enabled_);
        logger.set_sink(nullptr);
    }
    CapturedLogger(const CapturedLogger &) = delete;
    auto operator=(const CapturedLogger &) -> CapturedLogger & = delete;
    CapturedLogger(CapturedLogger &&) = delete;
    auto operator=(CapturedLogger &&) -> CapturedLogger & = delete;

    /// @brief 含某段子串的日志行数。
    /// @param needle 子串。
    [[nodiscard]] auto count_containing(std::string_view needle) const -> std::size_t {
        std::size_t hits = 0;
        for (const std::string &line : lines) {
            if (contains(line, needle)) {
                ++hits;
            }
        }
        return hits;
    }

    /// @brief 任何一行是否含某段子串（脱敏判据：越界事实一次都不许出现）。
    [[nodiscard]] auto any_contains(std::string_view needle) const -> bool {
        return std::any_of(lines.begin(), lines.end(),
                           [needle](const std::string &line) { return contains(line, needle); });
    }

    std::vector<std::string> lines;  ///< 捕获到的诊断行

  private:
    aurora::LogLevel level_;
    bool enabled_;
};

/// @brief 一次钩子调用记下的现场。
struct HookCall {
    ExchangeLeg leg{ExchangeLeg::Send};  ///< 本腿
    Method method{Method::Get};  ///< 方法
    std::string url;  ///< 目标的文本形态（比对用，视图给的是 `Url`）
    Headers headers;  ///< 入口时刻的字段表快照
    std::uint16_t status{0};  ///< 仅收摊问：交付应答的状态码
};

/// @brief 一次字段改写的形态：置值 / 追加 / 删除。
struct FieldWrite {
    std::string name;  ///< 字段名（原样拼写）
    std::string value;  ///< 字段值（删除时不读）
    bool append{false};  ///< 追加而非置值
    bool erase{false};  ///< 删除该名的全部条目
};

/// @brief 可编程钩子：按表改写请求头，可选交出短路值或抛异常，并记录两问的入口现场。
///
/// 回调是 `const`，故现场记在互斥量护住的成员里（钩子实现方自己保证线程安全，本替身照此办理）。
/// 形参一律不用引用形参以外的方式规避：视图与写通道都在调用期内有效，这里当场拷贝。
class ScriptHook final : public ExchangeHook {
  public:
    /// @brief 以一枚标签构造（标签进现场，多枚钩子同挂时便于分清是谁）。
    /// @param label 标签。
    explicit ScriptHook(std::string label) : label_{std::move(label)} {}

    /// @brief 起手问：记现场、按表改头、执行回调钩、交出短路值。
    /// @param request 入口时刻的视图。
    /// @param headers 写通道。
    /// @return `hook_delivery`（缺省空 = 照常发起）。
    auto before_request(const ExchangeRequestView &request, Headers &headers) const
        -> std::optional<Result<Response>> override {
        if (throws_in_before) {
            ++before_calls_;
            throw std::runtime_error{before_sentinel_text};
        }
        ++before_calls_;
        {
            const std::scoped_lock held{guard_};
            calls_.push_back(HookCall{
                .leg = request.leg(),
                .method = request.method(),
                .url = url_text(request),
                .headers = request.headers(),
            });
        }
        for (const auto &write : header_writes) {
            if (write.erase) {
                headers.remove(write.name);
            } else if (write.append) {
                headers.append(write.name, write.value);
            } else {
                headers.set(write.name, write.value);
            }
        }
        if (on_before) {
            on_before(headers);
        }
        return hook_delivery;
    }

    /// @brief 收摊问：记现场、执行回调钩、可选抛异常。
    /// @param request 收摊视图。
    /// @param response 交付的应答。
    auto after_response(const ExchangeRequestView &request, const Response &response) const -> void override {
        ++after_calls_;
        {
            const std::scoped_lock held{guard_};
            answers_.push_back(HookCall{
                .leg = request.leg(),
                .method = request.method(),
                .url = url_text(request),
                .headers = request.headers(),
                .status = response.status(),
            });
        }
        if (on_after) {
            on_after();
        }
        if (throws_in_after) {
            throw std::runtime_error{after_sentinel_text};
        }
    }

    /// @brief 视图目标的文本形态。
    [[nodiscard]] static auto url_text(const ExchangeRequestView &request) -> std::string {
        const auto &target = request.url();
        return target.has_value() ? target.value().to_string() : std::string{};
    }

    /// @brief 标签（多枚同挂时便于把现场分清到谁头上）。
    [[nodiscard]] auto label() const -> const std::string & { return label_; }

    /// @brief 起手问被实际执行到的次数（抛异常那一次也计）。
    [[nodiscard]] auto before_calls() const -> int { return before_calls_.load(); }

    /// @brief 收摊问被实际执行到的次数。
    [[nodiscard]] auto after_calls() const -> int { return after_calls_.load(); }

    /// @brief 起手问的入口现场副本。
    [[nodiscard]] auto calls() const -> std::vector<HookCall> {
        const std::scoped_lock held{guard_};
        return calls_;
    }

    /// @brief 收摊问的入口现场副本。
    [[nodiscard]] auto answers() const -> std::vector<HookCall> {
        const std::scoped_lock held{guard_};
        return answers_;
    }

    // 以下八枚是这枚测试替身的公开配置面：登记前配好、回调期间只读，用例直接按字段名写入。
    // 它们不带私有成员那套 `_` 收尾惯例——命名检查按可见性区分后缀，公开字段带尾缀反而违规。
    std::vector<FieldWrite> header_writes;  ///< 字段改写表（在登记前配好，回调期间只读）
    std::optional<Result<Response>> hook_delivery;  ///< 短路值
    bool throws_in_before{false};  ///< 起手问是否抛
    bool throws_in_after{false};  ///< 收摊问是否抛
    std::string before_sentinel_text{"before-sentinel-text"};  ///< 起手抛的异常文本（须处处不外泄）
    std::string after_sentinel_text{"after-sentinel-text"};  ///< 收摊抛的异常文本
    std::function<void(Headers &)> on_before;  ///< 起手问末尾执行的回调钩
    std::function<void()> on_after;  ///< 收摊问末尾执行的回调钩

  private:
    std::string label_;  ///< 标签
    mutable std::mutex guard_;  ///< 护两份现场
    mutable std::vector<HookCall> calls_;  ///< 起手问现场
    mutable std::vector<HookCall> answers_;  ///< 收摊问现场
    mutable std::atomic<int> before_calls_{0};  ///< 起手问次数
    mutable std::atomic<int> after_calls_{0};  ///< 收摊问次数
};

/// @brief 只数入口次数的流式宿主替身。
class CountingSink final : public DataSink {
  public:
    /// @brief 记一次头部到达。
    auto on_head(const Response &head) -> SinkFlow override {
        (void)head;
        ++heads;
        return SinkFlow::Continue;
    }
    /// @brief 记一段体字节。
    auto on_data(std::span<const char> body) -> SinkFlow override {
        (void)body;
        ++segments;
        return SinkFlow::Continue;
    }
    /// @brief 记一次收摊宣告。
    auto on_end() -> void override { ++ends; }

    int heads{0};  ///< `on_head` 次数
    int segments{0};  ///< `on_data` 次数
    int ends{0};  ///< `on_end` 次数
};

/// @brief 只记流条数与事件数的订阅宿主替身。
class CountingSseSink final : public SseSink {
  public:
    /// @brief 记一条流建立。
    auto on_open(const Response &head) -> SinkFlow override {
        ++opens;
        status = head.status();
        return SinkFlow::Continue;
    }
    /// @brief 记一条事件。
    auto on_event(const SseEvent &event) -> SinkFlow override {
        (void)event;
        ++events;
        return SinkFlow::Continue;
    }
    /// @brief 记一次订阅收尾。
    auto on_end() -> void override { ++ends; }

    int opens{0};  ///< `on_open` 次数
    int events{0};  ///< `on_event` 次数
    int ends{0};  ///< `on_end` 次数
    std::uint16_t status{0};  ///< 最后一次建流的状态码
};

/// @brief 只数消息条数的会话宿主替身。
class CountingWsSink final : public WebSocketSink {
  public:
    /// @brief 记一条消息。
    auto on_message(WsMessageKind kind, std::string_view payload) -> SinkFlow override {
        (void)kind;
        (void)payload;
        ++messages;
        return SinkFlow::Continue;
    }

    int messages{0};  ///< `on_message` 次数
};

/// @brief 交出一枚可读的短路应答（体是 `mock`，状态码由用例决定）。
[[nodiscard]] auto mock_response(std::uint16_t status, std::string_view body = "mock", bool from_cache = false,
                                 Headers fields = {}) -> Response {
    return Response{status, "Mocked", std::move(fields), std::vector<char>{body.begin(), body.end()}, from_cache};
}

/// @brief 把短路值包成 `Result<Response>` 的右值。
[[nodiscard]] auto mock_delivery(std::uint16_t status, std::string_view body = "mock") -> Result<Response> {
    return Result<Response>{mock_response(status, body)};
}

/// @brief 取出已交付应答的体文本。
[[nodiscard]] auto body_text(const Response &response) -> std::string {
    const auto &body = response.body();
    return std::string{body.begin(), body.end()};
}

/// @brief 200 应答：`text/event-stream` + 分块传输，事件体由调用方给。
///
/// SSE 腿的落点判据要靠一条真正能解析出事件的流，`service_ok()` 那种 `Content-Length` 形态虽能收摊，
/// 但解析器走的是另一条分支。
[[nodiscard]] auto event_stream(std::string_view body) -> std::string {
    std::string size_text;
    std::size_t remaining = body.size();
    do {
        const int digit = static_cast<int>(remaining % 16);
        const char glyph = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        size_text.insert(size_text.begin(), glyph);
        remaining /= 16;
    } while (remaining != 0);
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n" + size_text +
           "\r\n" + std::string{body} + "\r\n0\r\n\r\n";
}

/// @brief 第一条流：恰好一个事件。
[[nodiscard]] auto event_stream_one() -> std::string { return event_stream("data: one\n\n"); }

/// @brief 第二条流：换一个事件文本，供「每次重连各问一次」的现场分辨。
[[nodiscard]] auto event_stream_two() -> std::string { return event_stream("data: two\n\n"); }

/// @brief 视图上「有没有体取口」——收窄面靠编译期判据兑现，故这里逐枚探。
template <typename T, typename = void>
struct HasBody : std::false_type {};
/// @brief 探到 `body()` 即为真。
template <typename T>
struct HasBody<T, std::void_t<decltype(std::declval<const T &>().body())>> : std::true_type {};
/// @brief 视图上「有没有流式体源取口」。
template <typename T, typename = void>
struct HasBodySource : std::false_type {};
/// @brief 探到 `body_source()` 即为真。
template <typename T>
struct HasBodySource<T, std::void_t<decltype(std::declval<const T &>().body_source())>> : std::true_type {};
/// @brief 视图上「有没有字节范围旋钮」。
template <typename T, typename = void>
struct HasRange : std::false_type {};
/// @brief 探到 `range()` 即为真。
template <typename T>
struct HasRange<T, std::void_t<decltype(std::declval<const T &>().range())>> : std::true_type {};
/// @brief 视图上「有没有条件校验子旋钮」。
template <typename T, typename = void>
struct HasIfRange : std::false_type {};
/// @brief 探到 `if_range()` 即为真。
template <typename T>
struct HasIfRange<T, std::void_t<decltype(std::declval<const T &>().if_range())>> : std::true_type {};
/// @brief 视图上「有没有 `Expect` 旋钮」。
template <typename T, typename = void>
struct HasExpectContinue : std::false_type {};
/// @brief 探到 `expect_continue()` 即为真。
template <typename T>
struct HasExpectContinue<T, std::void_t<decltype(std::declval<const T &>().expect_continue())>> : std::true_type {};
/// @brief 视图上「有没有超时旋钮」。
template <typename T, typename = void>
struct HasTimeout : std::false_type {};
/// @brief 探到 `timeout()` 即为真。
template <typename T>
struct HasTimeout<T, std::void_t<decltype(std::declval<const T &>().timeout())>> : std::true_type {};
/// @brief 视图上「有没有改方法的写口」（无参取口除外）。
template <typename T, typename = void>
struct HasMethodArgument : std::false_type {};
/// @brief 探到 `method(Method)` 即为真。
template <typename T>
struct HasMethodArgument<T,
                         std::void_t<decltype(std::declval<T &>().method(std::declval<aurora::net::http::Method>()))>>
    : std::true_type {};
/// @brief 视图上「有没有改目标的写口」。
template <typename T, typename = void>
struct HasUrlArgument : std::false_type {};
/// @brief 探到 `url(std::string_view)` 即为真。
template <typename T>
struct HasUrlArgument<T, std::void_t<decltype(std::declval<T &>().url(std::declval<std::string_view>()))>>
    : std::true_type {};

#ifdef AURORA_ENABLE_DEBUG

/// @brief 把进程级活动存储恢复到缺省口径并清空：各例互不继承记录。
auto reset_network_activity() -> void {
    aurora::net::http::set_activity_options(aurora::net::http::ActivityOptions{});
    aurora::net::http::clear_network_activity();
}

/// @brief 活动表里指定标识的那一笔。
[[nodiscard]] auto entry_for(std::string_view request_id) -> HttpActivityEntry {
    const auto snapshot = aurora::net::http::activity_snapshot();
    AURORA_TEST_REQUIRE(snapshot.available);
    const auto found =
        std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
                     [request_id](const HttpActivityEntry &entry) { return entry.request_id == request_id; });
    AURORA_TEST_REQUIRE_MSG(found != snapshot.entries.end(), "no activity entry carries that identifier");
    return *found;
}

/// @brief 取指定标识最新一跳的时间线。
[[nodiscard]] auto timeline_for(std::string_view request_id) -> HttpRequestTimeline {
    const auto found = aurora::net::http::timeline_of(request_id);
    AURORA_TEST_REQUIRE(found.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) 契约：上一条 REQUIRE 为空即中止本例，路径分析看不见宏体
    return found.value();
}

/// @brief 时间线上是否留下阶段标记：真实交换必有，短路交付必无。
[[nodiscard]] auto has_marks(std::string_view request_id) -> bool { return !timeline_for(request_id).marks.empty(); }

#endif

}  // namespace

AURORA_TEST_CASE(a_copied_client_shares_the_registered_hook) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(record_each_then_reply(filed, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("shared");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "k"});
    const auto handle = client.use(hook);
    const Client copied = client;  // 拷贝的 `Client` 共用同一条链（与缓存表、连接池同一格局）

    const auto delivered = send_and_drive(copied, Request::get(loopback_url(peer.port())).request_id("rid-shared"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);

    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 1));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 0), "x-api-key: k"));
}

AURORA_TEST_CASE(the_exchange_view_exposes_only_the_header_write_channel) {
    // 收窄由类型兑现而不是注释兑现：视图上没有体、体源、协议旋钮与超时的取口，也没有任何写口。
    // 判据是编译期的——运行期无从观测「够不着」这件事。探针对象住在匿名命名空间：函数体内不允许
    // 定义带部分特化的类模板。
    static_assert(!HasBody<ExchangeRequestView>::value, "the view must not reach the message body");
    static_assert(!HasBodySource<ExchangeRequestView>::value, "the view must not reach the streaming body source");
    static_assert(!HasRange<ExchangeRequestView>::value, "the view must not reach the range knob");
    static_assert(!HasIfRange<ExchangeRequestView>::value, "the view must not reach the if-range knob");
    static_assert(!HasExpectContinue<ExchangeRequestView>::value, "the view must not reach the expect knob");
    static_assert(!HasTimeout<ExchangeRequestView>::value, "the view must not reach the timeout knob");
    static_assert(!HasMethodArgument<ExchangeRequestView>::value, "the view must not offer a method setter");
    static_assert(!HasUrlArgument<ExchangeRequestView>::value, "the view must not offer a target setter");
    static_assert(std::is_same_v<decltype(&ExchangeRequestView::headers),
                                 const Headers &(ExchangeRequestView::*)() const noexcept>,
                  "the view hands out a read-only field table");
    static_assert(!std::is_copy_constructible_v<ExchangeRequestView> &&
                      !std::is_move_constructible_v<ExchangeRequestView> &&
                      !std::is_copy_assignable_v<ExchangeRequestView>,
                  "the view is only valid inside the callback and must not be kept or moved out of it");
    static_assert(
        !std::is_default_constructible_v<ExchangeRequestView> &&
            !std::is_constructible_v<ExchangeRequestView, Method, std::optional<aurora::Url>, Headers, ExchangeLeg>,
        "the call side cannot build a view of its own");

    // 两问的签名把「只改请求头」与「收摊只读应答」钉在同一处。
    using BeforeSignature =
        std::optional<Result<Response>> (ExchangeHook::*)(const ExchangeRequestView &, Headers &) const;
    using AfterSignature = void (ExchangeHook::*)(const ExchangeRequestView &, const Response &) const;
    static_assert(std::is_same_v<decltype(&ExchangeHook::before_request), BeforeSignature>,
                  "before_request takes the read-only view plus a header-only write channel");
    static_assert(std::is_same_v<decltype(&ExchangeHook::after_response), AfterSignature>,
                  "after_response takes the settled view and the delivered response");
    static_assert(std::is_same_v<decltype(std::declval<Request &>().range(ByteRange::between(0, 9))), Request &>,
                  "the range knob stays on Request, out of the view");

    AURORA_TEST_CHECK_TRUE(true);  // 本例的判据全部落在编译期
}

AURORA_TEST_CASE(a_policy_refused_target_is_never_offered_to_the_hooks) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());  // 放行名单空着，任何回环明文都到不了这一侧
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;  // 缺省策略：拒绝一切未列入白名单的明文目标
    auto hook = std::make_shared<ScriptHook>("gate");
    hook->hook_delivery = mock_delivery(200);  // 即便备好了短路值，也不该有一次机会交出
    const auto handle = client.use(hook);

    const auto refused = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 0);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
}

AURORA_TEST_CASE(a_hook_cannot_touch_a_reserved_header_on_the_send_leg) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);

    // 十三枚保留头 × 四种改写形态。改值与删除两形态需要调用方自带同名值；新增与「换一种大小写拼写」
    // 在钩子入口确实没有这一枚（`X-Request-Id` 那一路另用 `request_id("")` 关掉库内自动补齐）。
    struct Shape {
        bool caller_supplied{false};  ///< 调用方是否自带同名值
        bool rewrite{false};  ///< 钩子改值（真）还是新增（假）
        bool erase{false};  ///< 钩子删名
        bool recase{false};  ///< 钩子换成另一套大小写拼写
        const char *label{"add"};  ///< 现场名
    };
    const std::vector<Shape> shapes = {
        Shape{.caller_supplied = false, .rewrite = false, .erase = false, .recase = false, .label = "add"},
        Shape{.caller_supplied = true, .rewrite = true, .erase = false, .recase = false, .label = "rewrite"},
        Shape{.caller_supplied = true, .rewrite = false, .erase = true, .recase = false, .label = "erase"},
        Shape{.caller_supplied = false, .rewrite = false, .erase = false, .recase = true, .label = "recased-add"},
    };
    const std::vector<std::string_view> table = {
        "Host",
        "X-Request-Id",
        "Content-Length",
        "Transfer-Encoding",
        "Content-Encoding",
        "Connection",
        "TE",
        "Trailer",
        "Upgrade",
        "Range",
        "If-Range",
        "Expect",
        "Proxy-Authorization",
    };

    for (std::string_view reserved : table) {
        for (const auto &shape : shapes) {
            const std::string written = shape.recase ? recased(reserved) : std::string{reserved};
            auto hook = std::make_shared<ScriptHook>("reserved");
            hook->header_writes.push_back(FieldWrite{.name = written, .value = "hook-value", .erase = shape.erase});
            const auto handle = client.use(hook);

            Request request = Request::get(loopback_url(peer.port(), "/reserved"));
            if (shape.caller_supplied) {
                request.header(reserved, "caller-value");
            } else if (reserved == "X-Request-Id") {
                request.request_id("");  // 明确不带：让这一枚在钩子入口真的缺席
            } else {
                request.request_id("rid-reserved");  // 其余各枚带上标识，短路拒绝的文本才有同一份现场
            }

            const std::string scene = std::string{reserved} + " / " + shape.label;
            const auto refused = send_and_drive(client, request, {}, scene);
            AURORA_TEST_REQUIRE_MSG(!bool(refused), scene);
            AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::HttpRequestInvalid);
            // detail 点名钩子序号、腿与规范拼写；值与目标文本一概不许出现。
            AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "hook #0"));
            AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "send leg"));
            AURORA_TEST_CHECK_TRUE(
                contains(refused.error().message, "modified the reserved header '" + std::string{reserved} + "'"));
            AURORA_TEST_CHECK_FALSE(contains(refused.error().message, "hook-value"));
            AURORA_TEST_CHECK_FALSE(contains(refused.error().message, "caller-value"));
            AURORA_TEST_CHECK_FALSE(contains(refused.error().message, "127.0.0.1"));
            AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
        }
    }
    // 调用方自带的保留头只要钩子没碰就照常上线：本例的钩子改的是别的名。这一段要真收发，
    // 故换一条会答话的对端（上面那条 `silent()` 的用途是「一次都不该被连接」）。
    ScriptedPeer talker(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(talker.valid());
    auto bystander = std::make_shared<ScriptHook>("bystander");
    bystander->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "k"});
    const auto kept = client.use(bystander);
    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(talker.port())).range(ByteRange::between(0, 9)));
    AURORA_TEST_CHECK_TRUE(bool(delivered));
    AURORA_TEST_CHECK_EQ(bystander->before_calls(), 1);
}

AURORA_TEST_CASE(the_upgrade_leg_reserves_the_four_handshake_fields) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    WsLog log;
    ScriptedPeer upgrade_peer(serve_ws(WsPlan{}, log));
    AURORA_TEST_REQUIRE(upgrade_peer.valid());
    const DrivenHost host;  // 收尾泵 + 驱动方（声明在句柄之前，故析构在其后）

    Client client;
    allow_test_cleartext(client);
    CountingWsSink sink;

    for (const std::string_view name :
         {"Sec-WebSocket-Key", "Sec-WebSocket-Version", "Sec-WebSocket-Protocol", "Sec-WebSocket-Extensions"}) {
        auto hook = std::make_shared<ScriptHook>("handshake");
        hook->header_writes.push_back(FieldWrite{.name = std::string{name}, .value = "hook-value"});
        const auto handle = client.use(hook);
        const auto refused = connect_and_drive(client, Request::get(ws_url(upgrade_peer.port())), sink);
        AURORA_TEST_REQUIRE_MSG(!bool(refused), std::string{name});
        AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::HttpRequestInvalid);
        AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "websocket leg"));
        AURORA_TEST_CHECK_TRUE(
            contains(refused.error().message, "modified the reserved header '" + std::string{name} + "'"));
        AURORA_TEST_CHECK_FALSE(contains(refused.error().message, "127.0.0.1"));
    }
    AURORA_TEST_CHECK_EQ(upgrade_peer.accepts(), 0);

    // 同一枚握手字段在 `send` 腿上不在保留表里：那条腿不写握手，字段对它就是普通字段。
    ScriptedPeer plain(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(plain.valid());
    auto on_send = std::make_shared<ScriptHook>("not-reserved-here");
    on_send->header_writes.push_back(FieldWrite{.name = "Sec-WebSocket-Protocol", .value = "chat"});
    const auto handle = client.use(on_send);
    const auto delivered = send_and_drive(client, Request::get(loopback_url(plain.port())));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(on_send->before_calls(), 1);
}

AURORA_TEST_CASE(a_hook_cannot_replace_the_chain_identifier) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("id");
    hook->header_writes.push_back(FieldWrite{.name = "X-Request-Id", .value = "hook-id"});
    const auto handle = client.use(hook);

    const auto refused = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("caller-id"));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "modified the reserved header 'X-Request-Id'"));
    AURORA_TEST_CHECK_FALSE(contains(refused.error().message, "hook-id"));
    AURORA_TEST_CHECK_FALSE(contains(refused.error().message, "caller-id"));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
}

AURORA_TEST_CASE(the_identifier_is_one_value_across_both_views_the_record_and_the_wire) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
#ifdef AURORA_ENABLE_DEBUG
    const aurora::test::net::HostDriven host;
    reset_network_activity();
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(record_each_then_reply(filed, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("id-unchanged");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "k"});
    const auto handle = client.use(hook);

    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/identity")).request_id("rid-one-value"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);

    const auto calls = hook->calls();
    const auto answers = hook->answers();
    AURORA_TEST_REQUIRE_EQ(calls.size(), std::size_t{1});
    AURORA_TEST_REQUIRE_EQ(answers.size(), std::size_t{1});
    const std::optional<std::string> in_before = calls.at(0).headers.get("X-Request-Id");
    const std::optional<std::string> in_after = answers.at(0).headers.get("X-Request-Id");
    AURORA_TEST_REQUIRE(in_before.has_value());
    AURORA_TEST_REQUIRE(in_after.has_value());
    // 两枚视图里的同一枚 id：整枚 optional 参与比较，取值这一步不交给解引用（判据要的是「同名同值」）。
    AURORA_TEST_CHECK(in_before == std::optional<std::string>{"rid-one-value"});
    AURORA_TEST_CHECK(in_after == in_before);
    AURORA_TEST_CHECK_EQ(entry_for("rid-one-value").request_id, "rid-one-value");
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 1));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 0), "x-request-id: rid-one-value"));
#else
    AURORA_TEST_SKIP("the activity record is compiled out when AURORA_ENABLE_DEBUG is off");
#endif
}

AURORA_TEST_CASE(an_added_header_is_the_only_difference_on_the_wire) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(record_each_then_reply(filed, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    const std::string url = loopback_url(peer.port(), "/diff");

    const auto bare = send_and_drive(client, Request::get(url).request_id("rid-diff"));
    AURORA_TEST_REQUIRE_MSG(bool(bare), bare ? std::string{} : bare.error().message);
    auto hook = std::make_shared<ScriptHook>("one-line");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "k"});
    const auto handle = client.use(hook);
    const auto armed = send_and_drive(client, Request::get(url).request_id("rid-diff"));
    AURORA_TEST_REQUIRE_MSG(bool(armed), armed ? std::string{} : armed.error().message);

    AURORA_TEST_REQUIRE(await_filed(filed, guard, 2));
    const std::string first = filed_request(filed, guard, 0);
    const std::string second = filed_request(filed, guard, 1);
    AURORA_TEST_CHECK_TRUE(contains(second, "x-api-key: k"));
    // 把多出来的那一行抹掉，两次线上字节逐字节相同：钩子不改分帧、不改次序、不补任何东西。
    AURORA_TEST_CHECK_EQ(erase_line(second, "x-api-key: k"), first);
}

AURORA_TEST_CASE(a_no_op_hook_leaves_the_wire_bytes_untouched) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(record_each_then_reply(filed, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    const std::string url = loopback_url(peer.port(), "/no-op");

    const auto bare = send_and_drive(client, Request::get(url).request_id("rid-noop"));
    AURORA_TEST_REQUIRE_MSG(bool(bare), bare ? std::string{} : bare.error().message);
    auto hook = std::make_shared<ScriptHook>("nothing");
    const auto handle = client.use(hook);  // 挂了钩子但一个字都不写
    const auto armed = send_and_drive(client, Request::get(url).request_id("rid-noop"));
    AURORA_TEST_REQUIRE_MSG(bool(armed), armed ? std::string{} : armed.error().message);

    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 1);
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 2));
    AURORA_TEST_CHECK_EQ(filed_request(filed, guard, 1), filed_request(filed, guard, 0));
}

AURORA_TEST_CASE(hooks_run_in_registration_order_and_later_writes_win) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(record_each_then_reply(filed, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto first = std::make_shared<ScriptHook>("first");
    first->header_writes.push_back(FieldWrite{.name = "X-Tenant", .value = "one"});
    auto second = std::make_shared<ScriptHook>("second");
    second->header_writes.push_back(FieldWrite{.name = "X-Tenant", .value = "two"});
    const auto first_handle = client.use(first);
    const auto second_handle = client.use(second);

    const auto delivered = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-order"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);

    AURORA_TEST_CHECK_EQ(first->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(second->before_calls(), 1);
    const auto seen_by_second = second->calls();
    AURORA_TEST_REQUIRE_EQ(seen_by_second.size(), std::size_t{1});
    // 视图是本枚入口时刻的快照：第二枚看得见前一枚的改写，看不见自己的。
    const std::optional<std::string> tenant_in_second_view = seen_by_second.at(0).headers.get("X-Tenant");
    AURORA_TEST_REQUIRE(tenant_in_second_view.has_value());
    AURORA_TEST_CHECK(tenant_in_second_view == std::optional<std::string>{"one"});
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 1));
    const std::string wire = filed_request(filed, guard, 0);
    AURORA_TEST_CHECK_TRUE(contains(wire, "x-tenant: two"));
    AURORA_TEST_CHECK_FALSE(contains(wire, "x-tenant: one"));  // `set` 先删同名再追加：线上只留一条
}

AURORA_TEST_CASE(a_hook_touched_header_does_not_cross_the_origin_boundary) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> target_filed;
    std::mutex target_guard;
    ScriptedPeer target(record_each_then_reply(target_filed, target_guard, service_ok("", "Z")));
    AURORA_TEST_REQUIRE(target.valid());
    const std::string redirect =
        "HTTP/1.1 302 Found\r\nLocation: " + loopback_url(target.port()) + "\r\nContent-Length: 0\r\n\r\n";
    std::vector<std::string> origin_filed;
    std::mutex origin_guard;
    ScriptedPeer origin(record_each_then_reply(origin_filed, origin_guard, redirect));
    AURORA_TEST_REQUIRE(origin.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("tenant");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "secret-tenant"});
    const auto handle = client.use(hook);

    const auto delivered = send_and_drive(client, Request::get(loopback_url(origin.port()))
                                                      .request_id("rid-cross")
                                                      .header("X-Caller", "caller-value")
                                                      .header("Authorization", "Bearer caller"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);

    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);  // 一次逻辑交换只问一次，重定向不重问
    AURORA_TEST_REQUIRE(await_filed(origin_filed, origin_guard, 1));
    AURORA_TEST_REQUIRE(await_filed(target_filed, target_guard, 1));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(origin_filed, origin_guard, 0), "x-api-key: secret-tenant"));
    const std::string second = filed_request(target_filed, target_guard, 0);
    AURORA_TEST_CHECK_FALSE(contains(second, "x-api-key"));  // 钩子经手过的字段属本侧凭据，跨侧即剥
    AURORA_TEST_CHECK_FALSE(contains(second, "bearer caller"));  // 凭据恒不跨 origin
    AURORA_TEST_CHECK_TRUE(contains(second, "x-caller: caller-value"));  // 调用方自写的非凭据字段照常带走
}

AURORA_TEST_CASE(a_same_origin_redirect_keeps_the_hook_touched_header) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    const std::string redirect = "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n";
    ScriptedPeer peer(per_connection(
        {record_each_then_reply(filed, guard, redirect), record_each_then_reply(filed, guard, service_ok("", "Z"))}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("same-origin");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "secret-tenant"});
    const auto handle = client.use(hook);

    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/here")).request_id("rid-same-origin"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);

    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 2));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 0), "x-api-key: secret-tenant"));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 1), "x-api-key: secret-tenant"));
}

AURORA_TEST_CASE(a_hook_touched_header_is_part_of_the_cache_key) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_ok("Cache-Control: max-age=600\r\n", "payload")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    client.cache(CacheOptions{.enabled = true});
    const std::string url = loopback_url(peer.port(), "/vary");

    auto tenant_a = std::make_shared<ScriptHook>("tenant-a");
    tenant_a->header_writes.push_back(FieldWrite{.name = "X-Tenant", .value = "a"});
    auto handle = client.use(tenant_a);
    const auto primed = send_and_drive(client, Request::get(url).request_id("rid-tenant-a"));
    AURORA_TEST_REQUIRE_MSG(bool(primed), primed ? std::string{} : primed.error().message);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

    const auto reused = send_and_drive(client, Request::get(url).request_id("rid-tenant-a"));
    AURORA_TEST_REQUIRE_MSG(bool(reused), reused ? std::string{} : reused.error().message);
    AURORA_TEST_CHECK(reused.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);  // 同租户第二次一条都不发

    handle.clear();
    auto tenant_b = std::make_shared<ScriptHook>("tenant-b");
    tenant_b->header_writes.push_back(FieldWrite{.name = "X-Tenant", .value = "b"});
    handle = client.use(tenant_b);
    const auto other = send_and_drive(client, Request::get(url).request_id("rid-tenant-b"));
    AURORA_TEST_REQUIRE_MSG(bool(other), other ? std::string{} : other.error().message);
    AURORA_TEST_CHECK_FALSE(other.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);  // 换租户绝不读回别人的副本

    handle.clear();
    const auto bare = send_and_drive(client, Request::get(url).request_id("rid-bare"));
    AURORA_TEST_REQUIRE_MSG(bool(bare), bare ? std::string{} : bare.error().message);
    AURORA_TEST_CHECK_FALSE(bare.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 3);  // 条目带着变体名，无触点的裸请求不够覆盖资格

    // 没有钩子时凭据字段也恒参与建键：那是钩子之前就在的口径，本例把它当对照组钉住。
    const std::string cred_url = loopback_url(peer.port(), "/cred");
    const auto first_credential =
        send_and_drive(client, Request::get(cred_url).request_id("rid-cred").header("Authorization", "Bearer one"));
    AURORA_TEST_REQUIRE_MSG(bool(first_credential),
                            first_credential ? std::string{} : first_credential.error().message);
    const auto other_credential =
        send_and_drive(client, Request::get(cred_url).request_id("rid-cred").header("Authorization", "Bearer two"));
    AURORA_TEST_REQUIRE_MSG(bool(other_credential),
                            other_credential ? std::string{} : other_credential.error().message);
    AURORA_TEST_CHECK_FALSE(other_credential.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 5);
}

AURORA_TEST_CASE(a_short_circuit_response_is_delivered_without_touching_the_wire) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_ok("", "real")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("mock");
    hook->hook_delivery = mock_delivery(201, "mock");
    auto handle = client.use(hook);

    const auto delivered = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-mock"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(delivered.value().status(), std::uint16_t{201});
    AURORA_TEST_CHECK_EQ(body_text(delivered.value()), "mock");
    AURORA_TEST_CHECK_FALSE(delivered.value().from_cache());  // 短路不是缓存命中
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);  // 一条连接都不发起
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 1);  // 短路成功照样是一次交付
    const auto answers = hook->answers();
    AURORA_TEST_REQUIRE_EQ(answers.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(answers.at(0).status, std::uint16_t{201});

    handle.clear();
    const auto after_clear = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-mock-2"));
    AURORA_TEST_REQUIRE_MSG(bool(after_clear), after_clear ? std::string{} : after_clear.error().message);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);  // 注销之后同一条 URL 照常上线
    AURORA_TEST_CHECK_EQ(body_text(after_clear.value()), "real");
}

AURORA_TEST_CASE(a_short_circuit_skips_cache_redirect_and_decompression) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_ok("Cache-Control: max-age=600\r\n", "real")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    client.cache(CacheOptions{.enabled = true});

    auto hook = std::make_shared<ScriptHook>("terminal");
    Headers fields;
    fields.set("Location", loopback_url(peer.port(), "/elsewhere"));
    fields.set("Content-Encoding", "gzip");
    hook->hook_delivery = Result<Response>{mock_response(302, "not-gzip-bytes", false, std::move(fields))};
    int progress_calls = 0;
    auto handle = client.use(hook);

    Request request = Request::get(loopback_url(peer.port(), "/terminal")).request_id("rid-terminal");
    request.on_progress([&progress_calls](const aurora::net::http::Progress &) -> void { ++progress_calls; });
    const auto delivered = send_and_drive(client, request);
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    // 原样交出：状态码不判重定向、宣告的编码不去还原、不查也不写表、一次进度都不发。
    AURORA_TEST_CHECK_EQ(delivered.value().status(), std::uint16_t{302});
    AURORA_TEST_CHECK_EQ(body_text(delivered.value()), "not-gzip-bytes");
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
    AURORA_TEST_CHECK_EQ(progress_calls, 0);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);

    handle.clear();
    const auto later =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/terminal")).request_id("rid-later"));
    AURORA_TEST_REQUIRE_MSG(bool(later), later ? std::string{} : later.error().message);
    AURORA_TEST_CHECK_FALSE(later.value().from_cache());
    AURORA_TEST_CHECK_EQ(later.value().status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);  // 短路那次没进表，同一条 URL 照样触网
}

AURORA_TEST_CASE(a_short_circuit_can_carry_a_failure) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("failing-mock");
    hook->hook_delivery =
        Result<Response>{aurora::make_error(ErrorCode::NetConnectFailed, "the mock could not connect")};
    const auto handle = client.use(hook);

    const auto delivered = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-failing"));
    AURORA_TEST_REQUIRE_FALSE(bool(delivered));
    AURORA_TEST_CHECK_EQ(delivered.error().code_enum, ErrorCode::NetConnectFailed);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);  // 失败交付不收摊问
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
}

AURORA_TEST_CASE(a_hook_may_not_forge_a_cache_hit) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("forgery");
    hook->hook_delivery = Result<Response>{mock_response(200, "mock", true)};
    const auto handle = client.use(hook);

    const auto refused = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-forge"));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "may not forge a cache hit"));
    AURORA_TEST_CHECK_FALSE(contains(refused.error().message, "127.0.0.1"));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);
}

AURORA_TEST_CASE(cancelling_from_a_request_hook_beats_the_short_circuit) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    Cancellation token;
    auto hook = std::make_shared<ScriptHook>("cancel-mock");
    hook->hook_delivery = mock_delivery(200);
    hook->on_before = [&token](Headers &) -> void { token.cancel(); };
    const auto handle = client.use(hook);

    const auto refused =
        send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-cancel"), Cancellation{token});
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::NetCancelled);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);  // 短路值作废，收摊问一概不问
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
}

AURORA_TEST_CASE(an_already_cancelled_token_never_reaches_the_hooks) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("late");
    hook->hook_delivery = mock_delivery(200);
    const auto handle = client.use(hook);

    Cancellation token;
    token.cancel();
    const auto refused = send_and_drive(client, Request::get(loopback_url(peer.port())), std::move(token));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::NetCancelled);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 0);  // 取消先于一切判定，钩子也不问
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
}

AURORA_TEST_CASE(a_short_circuit_on_the_stream_leg_is_refused_not_ignored) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("stream-mock");
    hook->hook_delivery = mock_delivery(200);
    const auto handle = client.use(hook);

    CountingSink sink;
    const auto refused = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::HttpRequestInvalid);
    // 如实被拒而不是悄悄忽略：静默放过会让宿主的替身「以为 mock 了，实际打到了真网」。
    AURORA_TEST_CHECK_TRUE(
        contains(refused.error().message, "stream leg returned a short-circuit from before_request"));
    AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "only honored on the send leg"));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
    AURORA_TEST_CHECK_EQ(sink.heads, 0);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);
}

AURORA_TEST_CASE(a_short_circuit_on_the_subscription_leg_ends_the_subscription) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("sse-mock");
    hook->hook_delivery = mock_delivery(200);
    const auto handle = client.use(hook);

    CountingSseSink sink;
    const auto refused =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(immediate_reconnect(3)), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "server-sent-events leg returned a short-circuit"));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);  // 本地拒绝不重连：一次都不触网，也没有第二条流
    AURORA_TEST_CHECK_EQ(sink.opens, 0);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
}

AURORA_TEST_CASE(a_short_circuit_on_the_upgrade_leg_ends_the_handshake) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    WsLog log;
    ScriptedPeer peer(serve_ws(WsPlan{}, log));
    AURORA_TEST_REQUIRE(peer.valid());
    const DrivenHost host;

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("ws-mock");
    hook->hook_delivery = mock_delivery(200);
    const auto handle = client.use(hook);

    CountingWsSink sink;
    const auto refused = connect_and_drive(client, Request::get(ws_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "websocket leg returned a short-circuit"));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
    AURORA_TEST_CHECK_EQ(sink.messages, 0);
}

AURORA_TEST_CASE(a_subscription_asks_the_request_hook_once_per_connection) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(per_connection({record_each_then_reply(filed, guard, event_stream_one()),
                                      record_each_then_reply(filed, guard, event_stream_two())}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("per-connection");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "k"});
    const auto handle = client.use(hook);

    CountingSseSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(immediate_reconnect(1)), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);

    AURORA_TEST_CHECK_EQ(sink.opens, 2);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 2);  // 每次（重）连各问一次起手问
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);  // 本腿的收摊值不是 `Response`
    const auto calls = hook->calls();
    AURORA_TEST_REQUIRE_EQ(calls.size(), std::size_t{2});
    for (const auto &call : calls) {
        AURORA_TEST_CHECK_TRUE(call.leg == ExchangeLeg::ServerSentEvents);
        // 重连那份请求是干净的原始请求：视图里看不到上一连接那次改写。
        AURORA_TEST_CHECK_FALSE(call.headers.contains("X-Api-Key"));
    }
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 2));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 0), "x-api-key: k"));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 1), "x-api-key: k"));
}

AURORA_TEST_CASE(the_streaming_legs_never_ask_for_a_response) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    auto hook = std::make_shared<ScriptHook>("no-after");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "k"});
    const aurora::test::net::HostDriven http_host;

    {
        Client client;
        allow_test_cleartext(client);
        const auto handle = client.use(hook);
        ScriptedPeer stream_peer(reply_after_reading(1, service_ok()));
        AURORA_TEST_REQUIRE(stream_peer.valid());
        CountingSink sink;
        const auto streamed = open_and_drive(client, Request::get(loopback_url(stream_peer.port())), sink);
        AURORA_TEST_REQUIRE_MSG(bool(streamed), streamed ? std::string{} : streamed.error().message);
        AURORA_TEST_CHECK_EQ(sink.heads, 1);
        AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
        AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);
    }

    {
        Client client;
        allow_test_cleartext(client);
        const auto handle = client.use(hook);
        ScriptedPeer sse_peer(reply_after_reading(1, event_stream_one()));
        AURORA_TEST_REQUIRE(sse_peer.valid());
        CountingSseSink sink;
        const auto subscribed =
            subscribe_and_drive(client, Request::get(loopback_url(sse_peer.port())).sse(single_stream()), sink);
        AURORA_TEST_REQUIRE_MSG(bool(subscribed), subscribed ? std::string{} : subscribed.error().message);
        AURORA_TEST_CHECK_EQ(sink.opens, 1);
        AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);
        const auto calls = hook->calls();
        AURORA_TEST_REQUIRE_EQ(calls.size(), std::size_t{2});
        AURORA_TEST_CHECK_TRUE(calls.at(1).leg == ExchangeLeg::ServerSentEvents);
    }

    {
        WsLog log;
        ScriptedPeer ws_peer(serve_ws(WsPlan{}, log));
        AURORA_TEST_REQUIRE(ws_peer.valid());
        const DrivenHost host;
        Client client;
        allow_test_cleartext(client);
        const auto handle = client.use(hook);
        CountingWsSink sink;
        const auto connected = connect_and_drive(client, Request::get(ws_url(ws_peer.port())), sink);
        AURORA_TEST_REQUIRE_MSG(bool(connected), connected ? std::string{} : connected.error().message);
        AURORA_TEST_CHECK_EQ(hook->after_calls(), 0);
        const auto calls = hook->calls();
        AURORA_TEST_REQUIRE_EQ(calls.size(), std::size_t{3});
        AURORA_TEST_CHECK_TRUE(calls.at(2).leg == ExchangeLeg::WebSocket);
    }
}

AURORA_TEST_CASE(the_response_hook_is_asked_once_per_delivered_response) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer wire(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(wire.valid());
    ScriptedPeer stale(reply_after_reading(1, service_ok("Cache-Control: max-age=600\r\n", "cached")));
    AURORA_TEST_REQUIRE(stale.valid());
    ScriptedPeer missing(reply_after_reading(1, "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnope"));
    AURORA_TEST_REQUIRE(missing.valid());
    const std::uint16_t unreachable = dead_port();
    AURORA_TEST_REQUIRE_MSG(unreachable > 0, "the loopback listener could not be opened");

    Client client;
    allow_test_cleartext(client);
    client.cache(CacheOptions{.enabled = true});
    auto hook = std::make_shared<ScriptHook>("pairing");
    const auto handle = client.use(hook);

    const auto delivered = send_and_drive(client, Request::get(loopback_url(wire.port())).request_id("rid-wire"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 1);  // 线上交付：一次

    const auto cached = send_and_drive(client, Request::get(loopback_url(stale.port(), "/c")).request_id("rid-cache"));
    AURORA_TEST_REQUIRE_MSG(bool(cached), cached ? std::string{} : cached.error().message);
    const auto again = send_and_drive(client, Request::get(loopback_url(stale.port(), "/c")).request_id("rid-cache"));
    AURORA_TEST_REQUIRE_MSG(bool(again), again ? std::string{} : again.error().message);
    AURORA_TEST_CHECK(again.value().from_cache());
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 3);  // 取回 + 新鲜副本：各一次

    const auto not_found = send_and_drive(client, Request::get(loopback_url(missing.port())).request_id("rid-404"));
    AURORA_TEST_REQUIRE_MSG(bool(not_found), not_found ? std::string{} : not_found.error().message);
    AURORA_TEST_CHECK_EQ(not_found.value().status(), std::uint16_t{404});
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 4);  // 非 2xx 是对端说的事实，不是本库的失败

    const auto failed = send_and_drive(client, Request::get(loopback_url(unreachable)).request_id("rid-fail"));
    AURORA_TEST_REQUIRE_FALSE(bool(failed));
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 4);  // 一次都没交出应答

    Client strict;  // 缺省策略：拒绝回环明文
    const auto refused = send_and_drive(strict, Request::get(loopback_url(wire.port())));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 4);  // 起手就被闸拦下，两问都不问
    const auto calls = hook->calls();
    AURORA_TEST_CHECK_EQ(calls.size(), std::size_t{5});  // 前五次交付各问一次，策略拒绝那一次没进这份前缀
}

AURORA_TEST_CASE(the_settled_view_is_the_logical_request_the_hooks_wrote) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> target_filed;
    std::mutex target_guard;
    ScriptedPeer target(record_each_then_reply(target_filed, target_guard, service_ok("", "Z")));
    AURORA_TEST_REQUIRE(target.valid());
    const std::string redirect =
        "HTTP/1.1 302 Found\r\nLocation: " + loopback_url(target.port()) + "\r\nContent-Length: 0\r\n\r\n";
    ScriptedPeer origin(reply_after_reading(1, redirect));
    AURORA_TEST_REQUIRE(origin.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("settling");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "written-by-hook"});
    const auto handle = client.use(hook);

    const auto delivered = send_and_drive(client, Request::get(loopback_url(origin.port(), "/origin"))
                                                      .request_id("rid-settling")
                                                      .header("X-Caller", "caller-value"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);

    const auto answers = hook->answers();
    AURORA_TEST_REQUIRE_EQ(answers.size(), std::size_t{1});
    const HookCall &settled = answers.at(0);
    AURORA_TEST_CHECK_EQ(settled.url, loopback_url(origin.port(), "/origin"));  // 跨 origin 之后仍是原始目标
    AURORA_TEST_CHECK_EQ(settled.status, std::uint16_t{200});
    AURORA_TEST_CHECK_TRUE(settled.headers.contains("X-Caller"));  // 调用方自写的在
    AURORA_TEST_CHECK_TRUE(settled.headers.contains("X-Api-Key"));  // 钩子写下的也在（链末那一刻的字段表）
    // 库内稍后才补的字段一概不在收摊视图里：协商宣告、`Host` 与分帧都由 `render_head()` 现地写定。
    AURORA_TEST_CHECK_FALSE(settled.headers.contains("Accept-Encoding"));
    AURORA_TEST_CHECK_FALSE(settled.headers.contains("Host"));
    AURORA_TEST_CHECK_FALSE(settled.headers.contains("Content-Length"));
    AURORA_TEST_CHECK_FALSE(settled.headers.contains("Transfer-Encoding"));
    AURORA_TEST_CHECK_FALSE(settled.headers.contains("Connection"));
    AURORA_TEST_CHECK_FALSE(settled.headers.contains("If-None-Match"));
    AURORA_TEST_CHECK_TRUE(settled.leg == ExchangeLeg::Send);
}

AURORA_TEST_CASE(a_hook_that_throws_after_the_response_does_not_undo_the_delivery) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    CapturedLogger capture;
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto throwing = std::make_shared<ScriptHook>("boom-after");
    throwing->throws_in_after = true;
    auto follower = std::make_shared<ScriptHook>("follower");
    follower->header_writes.push_back(FieldWrite{.name = "X-Follow", .value = "still-asked"});
    const auto throwing_handle = client.use(throwing);
    const auto follower_handle = client.use(follower);

    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-after-throw"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(delivered.value().status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(throwing->after_calls(), 1);
    AURORA_TEST_CHECK_EQ(follower->after_calls(), 1);  // 链上其余各枚照常问
    AURORA_TEST_CHECK_EQ(capture.count_containing("exchange hook threw from after_response"), std::size_t{1});
    // 脱敏纪律：异常原文与 URL 都不进日志。
    AURORA_TEST_CHECK_FALSE(capture.any_contains(throwing->after_sentinel_text));
    AURORA_TEST_CHECK_FALSE(capture.any_contains("127.0.0.1"));
}

AURORA_TEST_CASE(a_hook_that_throws_before_the_request_refuses_the_exchange) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    CapturedLogger capture;
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto throwing = std::make_shared<ScriptHook>("boom-before");
    throwing->throws_in_before = true;
    auto follower = std::make_shared<ScriptHook>("follower");
    const auto throwing_handle = client.use(throwing);
    const auto follower_handle = client.use(follower);

    const auto refused = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-before-throw"));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK_TRUE(contains(refused.error().message, "hook #0 on the send leg threw from before_request"));
    AURORA_TEST_CHECK_FALSE(contains(refused.error().message, throwing->before_sentinel_text));
    AURORA_TEST_CHECK_FALSE(contains(refused.error().message, "127.0.0.1"));
    AURORA_TEST_CHECK_EQ(follower->before_calls(), 0);  // 半截改写不发给对端：其余各枚不再问
    AURORA_TEST_CHECK_EQ(throwing->after_calls(), 0);
    AURORA_TEST_CHECK_EQ(follower->after_calls(), 0);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
}

AURORA_TEST_CASE(a_nested_exchange_from_a_request_hook_does_not_reask_that_hook) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    CapturedLogger capture;
    const aurora::test::net::HostDriven host;
    ScriptedPeer outer(reply_after_reading(1, service_ok("", "outer")));
    AURORA_TEST_REQUIRE(outer.valid());
    ScriptedPeer inner_peer(reply_after_reading(1, service_ok("", "inner")));
    AURORA_TEST_REQUIRE(inner_peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("reentrant");
    hook->header_writes.push_back(FieldWrite{.name = "X-Api-Key", .value = "k"});
    const std::string nested_url = loopback_url(inner_peer.port(), "/nested");
    hook->on_before = [&client, &nested_url](Headers &) -> void {
        // 协程体在调用那一刻就跑起手，钩子链也在首个 `co_await` 之前——嵌套交换的链正是在这里同步展开的。
        const auto nested = send_and_drive(client, Request::get(nested_url).request_id("rid-nested"));
        AURORA_TEST_CHECK_TRUE(bool(nested));
    };
    const auto handle = client.use(hook);

    const auto delivered = send_and_drive(client, Request::get(loopback_url(outer.port())).request_id("rid-outer"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);

    AURORA_TEST_CHECK_EQ(body_text(delivered.value()), "outer");
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);  // 嵌套那一次被递归闸跳过，本枚只被问一次
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 1);  // 嵌套那次没问收摊问，本枚这一次是自己的
    AURORA_TEST_CHECK_EQ(outer.accepts(), 1);
    AURORA_TEST_CHECK_EQ(inner_peer.accepts(), 1);  // 嵌套交换照常上线，只是不再问同一枚钩子
    AURORA_TEST_CHECK_EQ(capture.count_containing("exchange hook skipped on re-entry"), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(capture.any_contains("hook #0"));
    AURORA_TEST_CHECK_TRUE(capture.any_contains("leg send"));
}

AURORA_TEST_CASE(a_nested_exchange_from_a_response_hook_does_not_reask_that_hook) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    CapturedLogger capture;
    const aurora::test::net::HostDriven host;
    ScriptedPeer outer(reply_after_reading(1, service_ok("", "outer")));
    AURORA_TEST_REQUIRE(outer.valid());
    ScriptedPeer inner_peer(reply_after_reading(1, service_ok("", "inner")));
    AURORA_TEST_REQUIRE(inner_peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("reentrant-after");
    const std::string nested_url = loopback_url(inner_peer.port(), "/nested");
    hook->on_after = [&client, &nested_url]() -> void {
        const auto nested = send_and_drive(client, Request::get(nested_url).request_id("rid-nested-after"));
        AURORA_TEST_CHECK_TRUE(bool(nested));
    };
    const auto handle = client.use(hook);

    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(outer.port())).request_id("rid-outer-after"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);

    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 1);
    AURORA_TEST_CHECK_EQ(inner_peer.accepts(), 1);
    // 嵌套交换的起手问被闸跳过；它自己的收摊前缀是空的，故只有这一行 WARN。
    AURORA_TEST_CHECK_EQ(capture.count_containing("exchange hook skipped on re-entry"), std::size_t{1});
}

AURORA_TEST_CASE(deregistering_another_hook_mid_exchange_does_not_cut_it_out) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(record_each_then_reply(filed, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto second = std::make_shared<ScriptHook>("second");
    second->header_writes.push_back(FieldWrite{.name = "X-Second", .value = "asked-anyway"});
    auto first = std::make_shared<ScriptHook>("first");
    first->header_writes.push_back(FieldWrite{.name = "X-First", .value = "1"});
    auto holder = std::make_shared<std::optional<ExchangeHookRegistration>>();
    // 起手问与收摊问都跑在同一条快照上：链上另一枚在两次调用之间被注销，本次交换照旧问它。
    // 注销方登记在前，判据才有力量——它被问时「另一枚」还没被问到，快照必须仍然带着它。
    first->on_before = [holder](Headers &) -> void {
        if (holder->has_value()) {
            holder->value().clear();
        }
    };
    const auto first_handle = client.use(first);
    holder->emplace(client.use(second));

    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/snapshot")).request_id("rid-snapshot"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(second->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(second->after_calls(), 1);
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 1));
    const std::string wire = filed_request(filed, guard, 0);
    AURORA_TEST_CHECK_TRUE(contains(wire, "x-first: 1"));
    AURORA_TEST_CHECK_TRUE(contains(wire, "x-second: asked-anyway"));

    const auto later =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/snapshot")).request_id("rid-after-clear"));
    AURORA_TEST_REQUIRE_MSG(bool(later), later ? std::string{} : later.error().message);
    AURORA_TEST_CHECK_EQ(second->before_calls(), 1);  // 第二条交换起它不在链上了
    AURORA_TEST_CHECK_EQ(first->before_calls(), 2);
}

AURORA_TEST_CASE(a_hook_registered_during_an_exchange_is_not_applied_to_it) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(record_each_then_reply(filed, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto late = std::make_shared<ScriptHook>("late");
    late->header_writes.push_back(FieldWrite{.name = "X-Late", .value = "next-time"});
    auto holder = std::make_shared<std::optional<ExchangeHookRegistration>>();
    auto first = std::make_shared<ScriptHook>("first");
    first->on_before = [&client, holder, late](Headers &) -> void {
        if (!holder->has_value()) {
            holder->emplace(client.use(late));
        }
    };
    const auto first_handle = client.use(first);

    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/register")).request_id("rid-register"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(late->before_calls(), 0);  // 本次交换看的是起手那一刻的快照

    const auto later =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/register")).request_id("rid-register-2"));
    AURORA_TEST_REQUIRE_MSG(bool(later), later ? std::string{} : later.error().message);
    AURORA_TEST_CHECK_EQ(late->before_calls(), 1);
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 2));
    AURORA_TEST_CHECK_FALSE(contains(filed_request(filed, guard, 0), "x-late"));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 1), "x-late: next-time"));
}

AURORA_TEST_CASE(the_same_hook_instance_registered_twice_is_asked_twice) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    ScriptedPeer peer(record_each_then_reply(filed, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("twice");
    hook->header_writes.push_back(FieldWrite{.name = "X-Twice", .value = "1"});
    const auto first_handle = client.use(hook);
    const auto second_handle = client.use(hook);  // 登记是「两次登记」而不是「同一枚去重」

    const auto delivered = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-twice"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 2);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 2);
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 1));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 0), "x-twice: 1"));
    AURORA_TEST_CHECK_FALSE(contains(filed_request(filed, guard, 0), "x-twice: 1\r\nx-twice: 1"));  // 同名以值为准
}

AURORA_TEST_CASE(a_null_hook_is_an_empty_registration) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("never-asked");
    const ExchangeHookRegistration null_handle = client.use(nullptr);  // 空指针即一次空登记
    const auto real_handle = client.use(hook);

    const auto delivered = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-null"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);  // 链上只有那一枚真钩子
}

AURORA_TEST_CASE(a_registration_outlives_the_client_that_issued_it) {
    // 句柄不延长链的生命期：所有 `Client` 都没了，链即销毁，句柄析构与 `clear()` 都是空操作。
    auto hook = std::make_shared<ScriptHook>("orphan");
    ExchangeHookRegistration handle;
    {
        Client client;
        handle = client.use(hook);  // 移动赋值：先摘掉自己原来那份（这里没有），再接手对方的
    }
    handle.clear();  // 链已随最后一个 `Client` 销毁：不得崩、不得越界触碰
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 0);
}

AURORA_TEST_CASE(moving_a_handle_transfers_the_deregistration) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("moved");
    auto source = client.use(hook);
    ExchangeHookRegistration target;
    target = std::move(source);  // 被移走的句柄析构即空操作：注销权只有一份

    const auto delivered = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-move"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);

    target.clear();
    target.clear();  // 重复注销幂等
    const auto later = send_and_drive(client, Request::get(loopback_url(peer.port())).request_id("rid-move-2"));
    AURORA_TEST_REQUIRE_MSG(bool(later), later ? std::string{} : later.error().message);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(clearing_a_handle_inside_its_own_callback_only_affects_the_next_exchange) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto hook = std::make_shared<ScriptHook>("self-remove");
    auto holder = std::make_shared<std::optional<ExchangeHookRegistration>>();
    hook->on_before = [holder](Headers &) -> void {
        if (holder->has_value()) {
            holder->value().clear();  // 回调里注销自己：锁外调用，合法形态
        }
    };
    holder->emplace(client.use(hook));

    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/self")).request_id("rid-self"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 1);  // 本次交换的收摊前缀已经取到

    const auto later =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/self")).request_id("rid-self-2"));
    AURORA_TEST_REQUIRE_MSG(bool(later), later ? std::string{} : later.error().message);
    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);  // 下一条交换不再问它
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(a_short_circuit_delivery_is_distinguishable_in_the_activity_record) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
#ifdef AURORA_ENABLE_DEBUG
    const aurora::test::net::HostDriven host;
    reset_network_activity();
    ScriptedPeer wire_peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(wire_peer.valid());
    ScriptedPeer cache_peer(reply_after_reading(1, service_ok("Cache-Control: max-age=600\r\n", "cached")));
    AURORA_TEST_REQUIRE(cache_peer.valid());

    Client client;
    allow_test_cleartext(client);
    client.cache(CacheOptions{.enabled = true});

    const auto wired = send_and_drive(client, Request::get(loopback_url(wire_peer.port(), "/w")).request_id("rid-w"));
    AURORA_TEST_REQUIRE_MSG(bool(wired), wired ? std::string{} : wired.error().message);

    const auto cached = send_and_drive(client, Request::get(loopback_url(cache_peer.port(), "/c")).request_id("rid-c"));
    AURORA_TEST_REQUIRE_MSG(bool(cached), cached ? std::string{} : cached.error().message);
    const auto hit = send_and_drive(client, Request::get(loopback_url(cache_peer.port(), "/c")).request_id("rid-c"));
    AURORA_TEST_REQUIRE_MSG(bool(hit), hit ? std::string{} : hit.error().message);

    Client strict;  // 缺省策略挡下回环明文：一次本地拒绝
    const auto refused = send_and_drive(strict, Request::get(loopback_url(wire_peer.port(), "/r")).request_id("rid-r"));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));

    auto hook = std::make_shared<ScriptHook>("recorded-mock");
    hook->hook_delivery = mock_delivery(200, "mock");
    const auto handle = client.use(hook);
    const auto mocked = send_and_drive(client, Request::get(loopback_url(wire_peer.port(), "/m")).request_id("rid-m"));
    AURORA_TEST_REQUIRE_MSG(bool(mocked), mocked ? std::string{} : mocked.error().message);

    const HttpActivityEntry cache_hit = entry_for("rid-c");  // 同一标识的最新一跳即命中那一笔
    const HttpActivityEntry short_circuit = entry_for("rid-m");
    const HttpActivityEntry local_refusal = entry_for("rid-r");

    // 三类既有形态各自的指纹：真实交换有阶段、缓存命中带 `from_cache`、本地拒绝只有错误码。
    AURORA_TEST_CHECK_TRUE(has_marks("rid-w"));
    AURORA_TEST_CHECK_EQ(entry_for("rid-w").status, std::uint16_t{200});
    AURORA_TEST_CHECK_FALSE(entry_for("rid-w").from_cache);
    AURORA_TEST_CHECK_TRUE(entry_for("rid-w").sent > 0);  // 请求确实上线了
    AURORA_TEST_CHECK_FALSE(has_marks("rid-c"));  // 命中一条连接都不发起
    AURORA_TEST_CHECK(cache_hit.from_cache);
    AURORA_TEST_CHECK_EQ(cache_hit.status, std::uint16_t{200});
    AURORA_TEST_CHECK_TRUE(cache_hit.error_code.empty());
    AURORA_TEST_CHECK_FALSE(has_marks("rid-r"));
    AURORA_TEST_CHECK_EQ(local_refusal.status, std::uint16_t{0});  // 没收到过任何应答头
    AURORA_TEST_CHECK_FALSE(local_refusal.error_code.empty());
    AURORA_TEST_CHECK_FALSE(local_refusal.from_cache);

    // 短路交付：零阶段 + 有状态码 + 无错误码 + 非缓存命中 —— 与上面三类都不同
    AURORA_TEST_CHECK_FALSE(short_circuit.from_cache);
    AURORA_TEST_CHECK_EQ(short_circuit.status, std::uint16_t{200});
    AURORA_TEST_CHECK(short_circuit.error_code.empty());
    AURORA_TEST_CHECK_EQ(short_circuit.sent, std::uint64_t{0});  // 没有请求上线
    AURORA_TEST_CHECK_EQ(short_circuit.received, std::uint64_t{4});  // 交付体字节
    AURORA_TEST_CHECK_FALSE(has_marks("rid-m"));
    AURORA_TEST_CHECK_FALSE(short_circuit.in_flight);
#else
    AURORA_TEST_SKIP("network activity is compiled out when AURORA_ENABLE_DEBUG is off");
#endif
}

AURORA_TEST_CASE(the_authenticator_answers_override_a_hook_written_credential) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::vector<std::string> filed;
    std::mutex guard;
    const std::string challenge =
        "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer realm=\"x\"\r\nContent-Length: 3\r\n\r\nnope";
    ScriptedPeer peer(per_connection({record_each_then_reply(filed, guard, challenge),
                                      record_each_then_reply(filed, guard, service_ok("", "yes"))}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    client.authenticator(std::make_shared<BearerAuthenticator>("right"));
    auto hook = std::make_shared<ScriptHook>("wrong-credential");
    hook->header_writes.push_back(FieldWrite{.name = "Authorization", .value = "Bearer wrong"});
    const auto handle = client.use(hook);

    const auto delivered =
        send_and_drive(client, Request::get(loopback_url(peer.port(), "/auth")).request_id("rid-auth-hook"));
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    AURORA_TEST_CHECK_EQ(delivered.value().status(), std::uint16_t{200});

    AURORA_TEST_CHECK_EQ(hook->before_calls(), 1);  // 凭据重跑不重问钩子：一次逻辑交换只问一次
    AURORA_TEST_CHECK_EQ(hook->after_calls(), 1);
    AURORA_TEST_REQUIRE(await_filed(filed, guard, 2));
    AURORA_TEST_CHECK_TRUE(contains(filed_request(filed, guard, 0), "authorization: bearer wrong"));
    const std::string replay = filed_request(filed, guard, 1);
    AURORA_TEST_CHECK_TRUE(contains(replay, "authorization: bearer right"));
    AURORA_TEST_CHECK_FALSE(contains(replay, "bearer wrong"));  // 答复压过钩子写下的那一枚
}

}  // namespace aurora::test_cases::utest_net_http_hook
