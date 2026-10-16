/// 测试类型: unit
/// 目标单元: src/aurora/net/http_client.cpp（发起侧代理路由与策略闸）+ include/aurora/net/policy.h 的接入面
/// 测试说明: HTTP 发起侧「一条建连路、两种交付」与声明式安全策略的离线回归。
///           对端一律是回环上的进程内假代理与假源站（`ScriptedPeer`），断言落在**写出的字节**与**哪一条监听口
///           被连上**这两处，因此零真实远端、零 DNS（目标主机名从不被解析：明文转发把请求行交给代理，隧道把
///           名字写进 `CONNECT`，两者都不查解析器）。覆盖六条只有接了线才成立的判据：
///           - **明文经 HTTP 代理走转发形态**：请求行写成绝对形态 `GET http://host:port/path HTTP/1.1`，而
///             `Host` 字段仍取目标（不是代理）；默认端口不进文本、显式非默认端口照写。`https://` 与 SOCKS5
///             一律建隧道，代理口上**看不到任何请求行**。
///           - **不配代理时一个字节都不变**：直连的明文跳照写 origin-form，绝对形态只在转发那一支出现。
///           - **代理不可达绝不回退直连**：目标监听口的 `accepts()` 恒为 0，失败以代理侧原因收尾。
///           - **`NO_PROXY` 例外命中的目标就是直连**：请求行回到 origin-form，代理监听口一条都不被连。
///           - **策略闸在建连之前、也在缓存之前**：被拒的一跳 `accepts()` 恒 0（零套接字、零字节），且表里
///             有新鲜副本也一样先判后交付；重定向的下一跳各问一次，换到不合规矩的那一跳同样掐住。
///           - **生效次序三级**：客户端显式设过 → 进程级默认 → `NetPolicy{}` 的保守缺省，逐条各有正反断言。
///           进程级默认是共享状态，用例用 RAII 守卫退出即清，不留给同 runner 的其它套件。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/http.h"
#include "aurora/net/http_auth.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/policy.h"
#include "aurora/net/proxy.h"
#include "aurora/net/tls.h"
#include "aurora/state/coroutine.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_proxy {
namespace {

using aurora::CoroTask;
using aurora::Result;
using aurora::net::NetPolicy;
using aurora::net::ProxyConfig;
using aurora::net::ProxyEnvironment;
using aurora::net::TlsContext;
using aurora::net::http::BasicAuthenticator;
using aurora::net::http::CacheOptions;
using aurora::net::http::Client;
using aurora::net::http::DataSink;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::SinkFlow;
using aurora::net::http::StreamOutcome;
using aurora::test::dead_port;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::per_connection;
using aurora::test::scripts::reply_after_reading;

/// @brief 一份最小可用的 200 应答（假代理与假源站都拿它收尾）。
/// @return 应答字节。
[[nodiscard]] auto service_ok() -> std::string { return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"; }

/// @brief 取应答体的文本形态（体的事实形态是字节段，断言写成串才可读）。
/// @param response 应答。
/// @return 体文本。
[[nodiscard]] auto body_text(const Response &response) -> std::string {
    return std::string{response.body().begin(), response.body().end()};
}

/// @brief 拼一个指向回环端口的明文目标。
/// @param port 端口。
/// @param path 路径。
/// @return URL 文本。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 固定回报的代理环境读取器：只认这里列出的变量，其余视为未设置。
/// @param values 变量名到文本的映射。
/// @return 可交给 `ProxyEnvironment::from_environment` 的读取器（不读真实环境变量，故三平台一致）。
[[nodiscard]] auto reader_of(std::vector<std::pair<std::string, std::string>> values) -> ProxyEnvironment::Reader {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        for (const auto &[candidate, value] : values) {
            if (candidate == name) {
                return value;
            }
        }
        return std::nullopt;
    };
}

/// @brief 装配一份代理环境；写死的都是合法文本，解析失败就是用例本身写错了。
/// @param values 变量名到文本的映射。
/// @return 代理环境。
[[nodiscard]] auto environment(std::vector<std::pair<std::string, std::string>> values) -> ProxyEnvironment {
    auto built = ProxyEnvironment::from_environment(reader_of(std::move(values)));
    AURORA_TEST_REQUIRE_MSG(bool(built), built ? std::string{} : built.error().message);
    return built.value();
}

/// @brief 放行明文的主机清单条目：本套件的目标恒为 `127.0.0.1` 与两个不解析的名字。
/// @return 一份只放开这些主机的策略（其余维度取保守缺省）。
[[nodiscard]] auto permissive_cleartext() -> NetPolicy {
    NetPolicy policy;
    policy.allow_cleartext_hosts = {"127.0.0.1", "api.example.test", "legacy.example.test"};
    return policy;
}

/// @brief 记录被写出的请求字节、随后按给定应答收摊的对端脚本。
///
/// 转发形态的判据（请求行写成绝对形态、`Host` 取目标而非代理）**只能在线上字节上看**；故这里把头部块
/// 留一份。脚本线程写、用例线程读，用互斥量交接（用例在交换收尾后才取，锁只为把同步写成可见的事实）。
/// @param recorded 交接缓冲（脚本线程写入）。
/// @param guard 交接缓冲的锁。
/// @param payload 读满一个头部块后写出的应答字节。
/// @return 对端脚本。
[[nodiscard]] auto record_then_reply(std::string &recorded, std::mutex &guard, std::string payload) -> PeerScript {
    return [&recorded, &guard, payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket,
                                                             PeerStop &stop) -> void {
        (void)socket.set_io_timeout(std::chrono::milliseconds{200});
        std::vector<char> buffer(4096);
        std::string seen;
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                break;
            }
            seen.append(buffer.data(), read.value());
            if (seen.find("\r\n\r\n") != std::string::npos) {
                break;  // 头部块到位即应答（GET 无体，再读只会挂到超时）
            }
        }
        {
            const std::scoped_lock held(guard);
            recorded = seen;
        }
        (void)socket.write_all(std::span<const char>{payload});
    };
}

/// @brief 隧道版对端脚本：读下 `CONNECT` 头部块、应一句「隧道已建立」，随后原样记录并收摊。
///
/// 脚本返回即关连接：本用例要的不是「隧道里能跑什么」，而是「这一跳交付的是隧道形态、且握手之后的字节
/// 属于隧道」，握手在对面那半没有 TLS 服务时必然失败——失败本身就是「没有回退直连」的证据之一。
/// @param recorded 交接缓冲（脚本线程写入）。
/// @param guard 交接缓冲的锁。
/// @return 对端脚本。
[[nodiscard]] auto record_then_open_tunnel(std::string &recorded, std::mutex &guard) -> PeerScript {
    return [&recorded, &guard](aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        (void)socket.set_io_timeout(std::chrono::milliseconds{200});
        std::vector<char> buffer(4096);
        std::string seen;
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                break;
            }
            seen.append(buffer.data(), read.value());
            if (seen.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }
        {
            const std::scoped_lock held(guard);
            recorded = seen;
        }
        static const char AURORA_TUNNEL_OK[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
        (void)socket.write_all(std::span<const char>{AURORA_TUNNEL_OK});
    };
}

/// @brief 取记录到的字节（脚本线程写入，用例线程在交换收尾后读取）。
/// @param recorded 交接缓冲。
/// @param guard 交接缓冲的锁。
/// @return 副本。
[[nodiscard]] auto recorded_text(const std::string &recorded, std::mutex &guard) -> std::string {
    const std::scoped_lock held(guard);
    return recorded;
}

/// @brief 请求的首行（去掉方法名后的目标文本）；没有换行时返回整段，红时靠它看清写了什么。
/// @param head 记录到的请求字节。
/// @return 首行文本。
[[nodiscard]] auto first_line(std::string_view head) -> std::string {
    const auto newline = head.find("\r\n");
    return std::string{head.substr(0, newline == std::string_view::npos ? head.size() : newline)};
}

/// @brief 头部块里 `Host:` 字段的值（大小写不敏感地找，值取到行尾）。
/// @param head 记录到的请求字节。
/// @return 字段值；字段缺席时为空。
[[nodiscard]] auto host_field(std::string_view head) -> std::string {
    std::string lowered;
    lowered.reserve(head.size());
    for (const char ch : head) {
        lowered.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch);
    }
    const auto at = lowered.find("host:");
    if (at == std::string::npos) {
        return {};
    }
    const auto begin = at + 5;
    const auto end = lowered.find("\r\n", begin);
    std::string value{head.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin)};
    const auto content = value.find_first_not_of(" \t");  // 字段值前的分隔空白不属于值
    return content == std::string::npos ? std::string{} : value.substr(content);
}

/// @brief 取物化路径结果的 Result 层（协程本身不抛，外层恒成功）。
/// @param task 协程。
/// @return `Result<Response>`。
[[nodiscard]] auto inner(CoroTask<Result<Response>> &task) -> Result<Response> {
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 取流式路径结果的 Result 层（同上，外层失败即用例红）。
/// @param task 协程。
/// @return `Result<StreamOutcome>`。
[[nodiscard]] auto inner(CoroTask<Result<StreamOutcome>> &task) -> Result<StreamOutcome> {
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 完成一次物化交换（真实时钟 + 宿主驱动）。
/// @param client 客户端。
/// @param request 请求。
/// @return 应答的 Result 层。
[[nodiscard]] auto send_and_drive(const Client &client, Request request) -> Result<Response> {
    const aurora::test::net::HostDriven host;
    auto task = client.send(std::move(request));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);
    return inner(task);
}

/// @brief 一条什么都不收的宿主替身（流式入口的判据在「有没有发起交换」，不在交付内容）。
class NullSink final : public DataSink {
  public:
    auto on_data(std::span<const char> body) -> SinkFlow override {
        (void)body;
        return SinkFlow::Continue;
    }
};

/// @brief 完成一次流式交换。
/// @param client 客户端。
/// @param request 请求。
/// @param sink 宿主替身。
/// @return 出口的 Result 层。
[[nodiscard]] auto open_and_drive(const Client &client, Request request, DataSink &sink) -> Result<StreamOutcome> {
    const aurora::test::net::HostDriven host;
    auto task = client.open_stream(std::move(request), sink);
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "open_stream done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                          .satisfied);
    return inner(task);
}

/// @brief 进程级默认策略的作用域守卫：进入时设定、退出时清回未设（共享状态不得外泄）。
struct DefaultPolicyScope {
    /// @brief 装入一份进程级默认策略。
    /// @param policy 策略。
    explicit DefaultPolicyScope(std::optional<NetPolicy> policy) { aurora::net::set_default_policy(std::move(policy)); }
    ~DefaultPolicyScope() { aurora::net::set_default_policy(std::nullopt); }
    DefaultPolicyScope(const DefaultPolicyScope &) = delete;
    auto operator=(const DefaultPolicyScope &) -> DefaultPolicyScope & = delete;
    DefaultPolicyScope(DefaultPolicyScope &&) = delete;
    auto operator=(DefaultPolicyScope &&) -> DefaultPolicyScope & = delete;
};

/// @brief 错误文本里是否点名了某处配置或某个主机（红时靠它指认是哪一维掐的）。
/// @param haystack 错误消息原文。
/// @param needle 待找子串。
/// @return 找到为真。
[[nodiscard]] auto names_in(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

/// @brief 整段小写副本：字段名的比较口径与线上一致（大小写不敏感）。
/// @param head 请求字节。
/// @return 小写副本。
[[nodiscard]] auto lowered(std::string_view head) -> std::string {
    std::string folded;
    folded.reserve(head.size());
    for (const char ch : head) {
        folded.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch);
    }
    return folded;
}

/// @brief 逐条连接把收到的请求头部块留档、随后按给定应答收摊的对端脚本。
///
/// 代理侧认证环的判据是「第二条带上了什么」，而重跑是同一条监听口上的另一条连接，故档案按连接分条而不是
/// 互相覆盖（`record_then_reply()` 只留最后一条，那一形态证不出次序）。
/// @param filed 每条连接一份档案（脚本线程追加）。
/// @param guard 档案的锁。
/// @param payload 读满一个头部块后写出的应答字节。
/// @return 对端脚本。
[[nodiscard]] auto record_each_then_reply(std::vector<std::string> &filed, std::mutex &guard, std::string payload)
    -> PeerScript {
    return [&filed, &guard, payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket,
                                                          PeerStop &stop) -> void {
        (void)socket.set_io_timeout(std::chrono::milliseconds{200});
        std::vector<char> buffer(4096);
        std::string seen;
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                break;
            }
            seen.append(buffer.data(), read.value());
            if (seen.find("\r\n\r\n") != std::string::npos) {
                break;  // 头部块到位即应答（本套件的认证用例都不带请求体）
            }
        }
        {
            const std::scoped_lock held(guard);
            filed.push_back(seen);
        }
        (void)socket.write_all(std::span<const char>{payload});
    };
}

/// @brief 取已留档的连接条数。
/// @param filed 档案。
/// @param guard 档案的锁。
/// @return 条数。
[[nodiscard]] auto filed_count(const std::vector<std::string> &filed, std::mutex &guard) -> std::size_t {
    const std::scoped_lock held(guard);
    return filed.size();
}

/// @brief 取第 `index` 条连接的档案（小写副本）。
/// @param filed 档案。
/// @param guard 档案的锁。
/// @param index 连接序号（从 0 起）。
/// @return 该条请求的头部块文本；还没留到那一条即致命失败。
[[nodiscard]] auto filed_request(const std::vector<std::string> &filed, std::mutex &guard, std::size_t index)
    -> std::string {
    std::vector<std::string> copy;
    {
        const std::scoped_lock held(guard);
        copy = filed;
    }
    AURORA_TEST_REQUIRE_MSG(index < copy.size(), "the proxy has not filed that many requests");
    return lowered(copy.at(index));
}

/// @brief 头部块里是否存在以某字段名起头的那一行（小写副本上的行首匹配）。
///
/// 不能用 `find("authorization:")`：`proxy-authorization` 以它结尾，子串匹配会把「补对了字段」读成「多写了
/// 字段」，而这两句正是本用例要分开的那两件事。
/// @param head 已小写的请求头部块。
/// @param field 字段名 + 冒号，全小写。
/// @return 命中为真。
[[nodiscard]] auto has_field(std::string_view head, std::string_view field) -> bool {
    if (head.starts_with(field)) {
        return true;
    }
    return head.find(std::string{"\r\n"} + std::string{field}) != std::string::npos;
}

/// @brief 拼一份代理「要凭据」的 407：挑战原文 + 一枚可读的体。
/// @param challenge `Proxy-Authenticate` 的字段值原文。
/// @param body 消息体文本。
/// @return 应答字节。
[[nodiscard]] auto challenge_407(std::string_view challenge, std::string_view body = "no") -> std::string {
    return "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: " + std::string{challenge} +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
}

/// @brief 拼一份放行用的 200（转发形态下的应答体）。
/// @param body 消息体文本。
/// @return 应答字节。
[[nodiscard]] auto service_body(std::string_view body) -> std::string {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
}

}  // namespace

AURORA_TEST_CASE(cleartext_through_an_http_proxy_writes_the_absolute_form) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::mutex guard;
    std::string recorded;
    ScriptedPeer proxy(record_then_reply(recorded, guard, service_ok()));
    ScriptedPeer origin(reply_after_reading(1, service_ok()));  // 本不该被连上
    AURORA_TEST_REQUIRE(proxy.valid());
    AURORA_TEST_REQUIRE(origin.valid());

    Client client;
    client.policy(permissive_cleartext());
    client.proxy(environment({{"HTTP_PROXY", "http://127.0.0.1:" + std::to_string(proxy.port())}}));

    const auto response = send_and_drive(client, Request::get("http://api.example.test/v1/items"));
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});

    const auto head = recorded_text(recorded, guard);
    // 绝对形态：请求行带完整 scheme://authority（RFC 9112 §3.2.2 对转发代理的要求）。
    AURORA_TEST_CHECK_EQ(first_line(head), "GET http://api.example.test/v1/items HTTP/1.1");
    // `Host` 仍取目标：代理不替目标声明身份，字段与请求行的 authority 必须一致。
    AURORA_TEST_CHECK_EQ(host_field(head), "api.example.test");
    // 交付的是转发形态，代理就是这一跳的对端；源站一条连接都不该被连上。
    AURORA_TEST_CHECK_EQ(proxy.accepts(), 1);
    AURORA_TEST_CHECK_EQ(origin.accepts(), 0);
}

AURORA_TEST_CASE(the_absolute_form_omits_a_default_port_and_keeps_an_explicit_one) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::mutex guard;
    std::string recorded;
    ScriptedPeer proxy(record_then_reply(recorded, guard, service_ok()));
    AURORA_TEST_REQUIRE(proxy.valid());

    Client client;
    client.policy(permissive_cleartext());
    client.proxy(environment({{"HTTP_PROXY", "http://127.0.0.1:" + std::to_string(proxy.port())}}));

    const auto implied = send_and_drive(client, Request::get("http://api.example.test/"));
    AURORA_TEST_REQUIRE_MSG(bool(implied), implied ? std::string{} : implied.error().message);
    // 默认端口不进文本：请求行与 `Host` 都只写主机名（与直连时同一套省略口径）。
    AURORA_TEST_CHECK_EQ(first_line(recorded_text(recorded, guard)), "GET http://api.example.test/ HTTP/1.1");
    AURORA_TEST_CHECK_EQ(host_field(recorded_text(recorded, guard)), "api.example.test");

    const auto explicit_port = send_and_drive(client, Request::get("http://api.example.test:8080/"));
    AURORA_TEST_REQUIRE_MSG(bool(explicit_port), explicit_port ? std::string{} : explicit_port.error().message);
    const auto second = recorded_text(recorded, guard);
    AURORA_TEST_CHECK_EQ(first_line(second), "GET http://api.example.test:8080/ HTTP/1.1");
    AURORA_TEST_CHECK_EQ(host_field(second), "api.example.test:8080");
    AURORA_TEST_CHECK_EQ(proxy.accepts(), 2);
}

AURORA_TEST_CASE(a_direct_cleartext_hop_still_writes_the_origin_form) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 不配代理时一切照旧：绝对形态只属于「明文交转发代理」那一支，别的形态一个字节都不变。
    std::mutex guard;
    std::string recorded;
    ScriptedPeer peer(record_then_reply(recorded, guard, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.policy(permissive_cleartext());

    const auto response = send_and_drive(client, Request::get(loopback_url(peer.port(), "/v1/items")));
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto head = recorded_text(recorded, guard);
    AURORA_TEST_CHECK_EQ(first_line(head), "GET /v1/items HTTP/1.1");
    AURORA_TEST_CHECK_EQ(host_field(head), "127.0.0.1:" + std::to_string(peer.port()));
}

AURORA_TEST_CASE(a_secure_hop_asks_for_a_tunnel_and_sends_no_request_line) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::mutex guard;
    std::string recorded;
    ScriptedPeer proxy(record_then_open_tunnel(recorded, guard));
    ScriptedPeer origin(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(proxy.valid());
    AURORA_TEST_REQUIRE(origin.valid());

    Client client;
    // 目标仍写数字字面量：隧道把名字原样交给代理，本库不解析；握手入参随后按 RFC 6125 拒掉 IP 字面量，
    // 于是这一跳在「隧道已建立、握手未开始」处收尾——判据要的就是这个形态，不是握手能不能谈成。
    client.policy(NetPolicy{.allow_cleartext_hosts = {"127.0.0.1"}});
    client.proxy(environment({{"HTTPS_PROXY", "http://127.0.0.1:" + std::to_string(proxy.port())}}));

    const auto response =
        send_and_drive(client, Request::get("https://127.0.0.1:" + std::to_string(origin.port()) + "/"));
    AURORA_TEST_REQUIRE_FALSE(bool(response));  // 对面没有 TLS 服务：失败是预期结局

    const auto head = recorded_text(recorded, guard);
    AURORA_TEST_CHECK_EQ(first_line(head), "CONNECT 127.0.0.1:" + std::to_string(origin.port()) + " HTTP/1.1");
    // 隧道那一支绝不把 HTTP 请求行交给代理：载荷要在加密之后才越过它。
    AURORA_TEST_CHECK_FALSE(names_in(head, "GET "));
    // 「一条建连路」的字面兑现：这一跳连的是代理，目标监听口一条都不被连；失败也不是回退直连。
    AURORA_TEST_CHECK_EQ(proxy.accepts(), 1);
    AURORA_TEST_CHECK_EQ(origin.accepts(), 0);
}

AURORA_TEST_CASE(an_unreachable_proxy_never_falls_back_to_direct) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    ScriptedPeer origin(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(origin.valid());

    Client client;
    client.policy(permissive_cleartext());
    client.proxy(environment({{"HTTP_PROXY", "http://127.0.0.1:" + std::to_string(dead_port())}}));

    const auto response = send_and_drive(client, Request::get(loopback_url(origin.port())));
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    // 源站一条都不被连：代理不可达就是失败，库里没有「那就直连试试」这条路。
    AURORA_TEST_CHECK_EQ(origin.accepts(), 0);
}

AURORA_TEST_CASE(a_no_proxy_exception_goes_straight_to_the_target) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::mutex proxy_guard;
    std::mutex origin_guard;
    std::string proxy_recorded;
    std::string origin_recorded;
    ScriptedPeer proxy(record_then_reply(proxy_recorded, proxy_guard, service_ok()));
    ScriptedPeer origin(record_then_reply(origin_recorded, origin_guard, service_ok()));
    AURORA_TEST_REQUIRE(proxy.valid());
    AURORA_TEST_REQUIRE(origin.valid());

    Client client;
    client.policy(permissive_cleartext());
    client.proxy(
        environment({{"HTTP_PROXY", "http://127.0.0.1:" + std::to_string(proxy.port())}, {"NO_PROXY", "127.0.0.1"}}));

    const auto response = send_and_drive(client, Request::get(loopback_url(origin.port(), "/v1/items")));
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);

    // 例外命中即直连：请求行回到 origin-form，代理监听口一条都不被连（与没配代理时逐字同形）。
    const auto head = recorded_text(origin_recorded, origin_guard);
    AURORA_TEST_CHECK_EQ(first_line(head), "GET /v1/items HTTP/1.1");
    AURORA_TEST_CHECK_EQ(origin.accepts(), 1);
    AURORA_TEST_CHECK_EQ(proxy.accepts(), 0);
}

AURORA_TEST_CASE(the_policy_gate_refuses_before_any_connection) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    ScriptedPeer peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client unlisted;  // 缺省策略：明文全拒，且不给回环留隐含例外
    const auto refused = send_and_drive(unlisted, Request::get(loopback_url(peer.port())));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK(names_in(refused.error().message, "allow_cleartext_hosts"));
    // 「一条连接都不发起」的可证形态：对端的 accept 计数恒为 0。
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);

    // 流式入口走同一道闸：判据不因为在逐段交付而缺席。
    NullSink host;
    const auto streamed = open_and_drive(unlisted, Request::get(loopback_url(peer.port())), host);
    AURORA_TEST_REQUIRE_FALSE(bool(streamed));
    AURORA_TEST_CHECK_EQ(streamed.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
}

AURORA_TEST_CASE(the_gate_runs_before_the_cache) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    ScriptedPeer peer(
        reply_after_reading(1, "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 2\r\n\r\nok"));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.policy(permissive_cleartext());
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());

    const auto first = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE(bool(first));
    AURORA_TEST_CHECK_FALSE(first.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

    // 表里已有一条新鲜的副本，策略却在这一次改回全拒：闸在先，命中也不交付（少打一次网络不等于少一道闸）。
    client.policy(NetPolicy{});
    const auto gated = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE_FALSE(bool(gated));
    AURORA_TEST_CHECK_EQ(gated.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(the_gate_is_asked_again_for_every_redirect_hop) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const std::string redirect = "HTTP/1.1 302 Found\r\nLocation: https://127.0.0.1:1/ok\r\nContent-Length: 0\r\n\r\n";
    ScriptedPeer peer(reply_after_reading(1, redirect));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    // 第一跳（明文、主机在清单内）放行；第二跳换成 https 后撞上版本下限那一维：逐跳各问一次才掐得住跳转。
    NetPolicy floor_13 = permissive_cleartext();
    floor_13.min_tls_version = aurora::net::TlsVersion::Tls13;
    client.policy(floor_13);
    TlsContext context;
    context.min_version = aurora::net::TlsVersion::Tls12;  // 上下文能谈到下限以下 → 冲突即拒
    client.tls_context(context);

    const auto hopped = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_REQUIRE_FALSE(bool(hopped));
    AURORA_TEST_CHECK_EQ(hopped.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK(names_in(hopped.error().message, "TLS 1.2"));
    // 掐在第二跳之前：第一跳确实触网，第二跳一条连接都没发起。
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(the_effective_policy_follows_the_documented_order) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    ScriptedPeer peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());
    const auto url = loopback_url(peer.port());

    {
        // 客户端没设过策略时，进程级默认就是它的策略。
        NetPolicy open;
        open.allow_cleartext_hosts = {"127.0.0.1"};
        const DefaultPolicyScope scope{std::optional<NetPolicy>{open}};

        Client follower;
        AURORA_TEST_CHECK_FALSE(follower.policy().has_value());
        const auto served = send_and_drive(follower, Request::get(url));
        AURORA_TEST_REQUIRE_MSG(bool(served), served ? std::string{} : served.error().message);
        AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

        // 客户端显式设过的，覆盖进程级默认——覆盖的方向是收紧也照办。
        Client strict;
        strict.policy(NetPolicy{});
        const auto refused = send_and_drive(strict, Request::get(url));
        AURORA_TEST_REQUIRE_FALSE(bool(refused));
        AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
        AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
    }

    // 进程级回到「未设」后，没设过策略的客户端落回 `NetPolicy{}` 的保守缺省：明文全拒。
    Client after;
    const auto defaulted = send_and_drive(after, Request::get(url));
    AURORA_TEST_REQUIRE_FALSE(bool(defaulted));
    AURORA_TEST_CHECK_EQ(defaulted.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(the_effective_policy_is_snapshot_at_the_start_of_the_exchange) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    ScriptedPeer peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    NetPolicy open;
    open.allow_cleartext_hosts = {"127.0.0.1"};
    const DefaultPolicyScope scope{std::optional<NetPolicy>{open}};

    Client client;  // 不设策略：跟着进程级默认走
    const aurora::test::net::HostDriven host;
    auto task = client.send(Request::get(loopback_url(peer.port())));
    // 发起之后立刻改掉进程级默认：判定用的是发起那一刻读定的那一份，半途改写不影响在途的交换。
    aurora::net::set_default_policy(NetPolicy{});

    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(a_forwarded_407_is_answered_on_proxy_authorization) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::mutex guard;
    std::vector<std::string> filed;
    ScriptedPeer proxy(per_connection({record_each_then_reply(filed, guard, challenge_407(R"(Basic realm="corp")")),
                                       record_each_then_reply(filed, guard, service_body("done"))}));
    AURORA_TEST_REQUIRE(proxy.valid());

    Client client;
    client.policy(permissive_cleartext());
    client.proxy(environment({{"HTTP_PROXY", "http://127.0.0.1:" + std::to_string(proxy.port())}}));
    client.authenticator(std::make_shared<BasicAuthenticator>("lyn", "pw"));

    const auto response = send_and_drive(client, Request::get("http://api.example.test/v1/items"));
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    // 代理那一环生在交付之前：宿主拿到的是补了凭据之后的那份 200，而不是先收到 407 再自己处理。
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(body_text(response.value()), "done");
    AURORA_TEST_CHECK_EQ(proxy.accepts(), 2);
    AURORA_TEST_REQUIRE(aurora::test::net::drive_until("both requests filed", [&filed, &guard] {
                            return filed_count(filed, guard) == 2;
                        }).satisfied);
    AURORA_TEST_CHECK_FALSE(has_field(filed_request(filed, guard, 0), "proxy-authorization:"));
    AURORA_TEST_CHECK_TRUE(has_field(filed_request(filed, guard, 1), "proxy-authorization: basic "));
    // 补的是代理那一侧的字段，源站字段一枚都不多写：写了就等于把代理凭据交给源站（同源、同一份字节）。
    AURORA_TEST_CHECK_FALSE(has_field(filed_request(filed, guard, 1), "authorization:"));
}

AURORA_TEST_CASE(a_source_challenge_through_a_proxy_is_answered_on_authorization) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::mutex guard;
    std::vector<std::string> filed;
    // 401 是源站在说话、只是经代理转回：本环按 `AuthChallenge::proxy == false` 归到源站那一支。
    ScriptedPeer proxy(
        per_connection({record_each_then_reply(filed, guard,
                                               "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"api\"\r\n"
                                               "Content-Length: 2\r\n\r\nno"),
                        record_each_then_reply(filed, guard, service_body("yes"))}));
    AURORA_TEST_REQUIRE(proxy.valid());

    Client client;
    client.policy(permissive_cleartext());
    client.proxy(environment({{"HTTP_PROXY", "http://127.0.0.1:" + std::to_string(proxy.port())}}));
    client.authenticator(std::make_shared<BasicAuthenticator>("lyn", "pw"));

    const auto response = send_and_drive(client, Request::get("http://api.example.test/v1/items"));
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    AURORA_TEST_REQUIRE(aurora::test::net::drive_until("both requests filed", [&filed, &guard] {
                            return filed_count(filed, guard) == 2;
                        }).satisfied);
    AURORA_TEST_CHECK_TRUE(has_field(filed_request(filed, guard, 1), "authorization: basic "));
    AURORA_TEST_CHECK_FALSE(has_field(filed_request(filed, guard, 1), "proxy-authorization:"));
}

AURORA_TEST_CASE(a_tunnel_refused_with_407_reissues_connect_with_the_credential) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::mutex guard;
    std::vector<std::string> filed;
    ScriptedPeer proxy(
        per_connection({record_each_then_reply(filed, guard, challenge_407(R"(Basic realm="corp")", "")),
                        record_each_then_reply(filed, guard, "HTTP/1.1 200 Connection Established\r\n\r\n")}));
    ScriptedPeer origin(reply_after_reading(1, service_ok()));  // 隧道之后对面没有 TLS 服务：它不该被连上
    AURORA_TEST_REQUIRE(proxy.valid());
    AURORA_TEST_REQUIRE(origin.valid());

    Client client;
    client.policy(NetPolicy{.allow_cleartext_hosts = {"127.0.0.1"}});
    client.proxy(environment({{"HTTPS_PROXY", "http://127.0.0.1:" + std::to_string(proxy.port())}}));
    client.authenticator(std::make_shared<BasicAuthenticator>("lyn", "pw"));

    const auto response =
        send_and_drive(client, Request::get("https://127.0.0.1:" + std::to_string(origin.port()) + "/"));
    // 结局仍是失败：隧道谈成之后没有 TLS 服务可谈。判据不在结局，在「隧道这一腿也被问过、也补过凭据」。
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_REQUIRE(aurora::test::net::drive_until("both connects filed", [&filed, &guard] {
                            return filed_count(filed, guard) == 2;
                        }).satisfied);
    AURORA_TEST_CHECK_EQ(first_line(filed_request(filed, guard, 1)),
                         "connect 127.0.0.1:" + std::to_string(origin.port()) + " http/1.1");
    AURORA_TEST_CHECK_FALSE(has_field(filed_request(filed, guard, 0), "proxy-authorization:"));
    AURORA_TEST_CHECK_TRUE(has_field(filed_request(filed, guard, 1), "proxy-authorization: basic "));
    // 重发的是同一条腿上的第二条 `CONNECT`，不是「放弃隧道改直连」：目标监听口一条都不被连。
    AURORA_TEST_CHECK_EQ(proxy.accepts(), 2);
    AURORA_TEST_CHECK_EQ(origin.accepts(), 0);
}

}  // namespace aurora::test_cases::utest_net_http_proxy
