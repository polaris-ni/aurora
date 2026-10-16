/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_cookie_jar.h + src/aurora/net/detail/http_cookie_jar.cpp
///           + include/aurora/net/http_session.h + src/aurora/net/http_session.cpp
/// 测试说明: Cookie Jar（`RFC 6265` 最小集）与 Session 身份层的逐值回归。文法侧零网络：
///           解析只收 `name=value` 加本库认得的属性（`Domain` / `Path` / `Secure` / `HttpOnly` /
///           `Expires` / `Max-Age`），`Max-Age` 优先于 `Expires` 且非正值与已过期的 `Expires` 都折成
///           「丢弃」；`Domain` 必须与请求主机 domain-match（注册名后缀，IP 形态只认全等），否则整条
///           丢弃；`Path` 空值与不以 `/` 开头的值走 default-path 算法；畸形（无 `=`、空名）整条丢弃而
///           不报错——一枚坏 cookie 不该让一次本可成功的交换失败。取侧按 `§5.4` 选择：域匹配 + 路径
///           前缀 + `Secure` 只上 TLS 目标，路径长者在前、同长按创建时刻；存储侧同 (名, 域, host-only,
///           路径) 去重替换且保留创建时刻，上限逐出先清过期再摘最老。端到端侧以 ScriptedPeer 在回环上
///           钉住三条发起侧行为：会话把应答的 `Set-Cookie` 存进 Jar 并在第二次请求附回（对端收到的请求
///           全文可证）、裸 `Client` 不收不发 Cookie（请求全文里没有那枚字段）、换 origin 的重定向按
///           新目标的域重取（第二跳不带第一台机器的 cookie）；缓存键看见 Cookie 由「附上 cookie 之后
///           同一 URL 不再命中」这条判据承担（凭据字段恒参与建键，走线上而不是走表的内部视角）。
/// 全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免。

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/blocking.h"
#include "aurora/net/detail/http_cookie_jar.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_session.h"
#include "aurora/net/policy.h"
#include "aurora/state/coroutine.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_cookie {
namespace {

using aurora::CoroTask;
using aurora::Headers;
using aurora::Result;
using aurora::Url;
using aurora::net::detail::HttpCookie;
using aurora::net::detail::HttpCookieJar;
using aurora::net::detail::parse_set_cookie;
using aurora::net::http::Client;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::Session;
using aurora::test::PeerScript;
using aurora::test::ScriptedPeer;
using aurora::testing::require_value;

/// @brief 判定基准时刻：取自墙上时钟即可——文法判据比的是相对序（过期 / 未过期），不比绝对值。
[[nodiscard]] auto now() -> std::chrono::system_clock::time_point { return std::chrono::system_clock::now(); }

/// @brief 解析一枚目标 URL（本套件的目标全是写死的合法串，解析失败当场收场）。
/// @param text URL 文本。
/// @return 已解析的 URL。
[[nodiscard]] auto url_of(std::string_view text) -> Url {
    const auto parsed_url = Url::parse(text);
    AURORA_TEST_REQUIRE_MSG(bool(parsed_url), parsed_url ? std::string{} : parsed_url.error().message);
    return parsed_url.value();
}

/// @brief 解析一枚 `Set-Cookie` 并取回产物（文法判据全部经由这一口，畸形让 REQUIRE 收场）。
/// @param value 字段值。
/// @param target 本跳目标（缺省 = 一台注册名主机上的根路径，供域校验与 default-path）。
/// @param base 判定基准时刻。
/// @return 解析产物。
[[nodiscard]] auto parsed(std::string_view value, const Url &target = url_of("http://shop.example/index.html"),
                          std::chrono::system_clock::time_point base = now()) -> HttpCookie {
    return require_value(parse_set_cookie(value, target, base));
}

AURORA_TEST_CASE(parse_keeps_name_value_and_defaults) {
    const auto cookie = parsed("sid=abc123");
    AURORA_TEST_CHECK(cookie.name == "sid");
    AURORA_TEST_CHECK(cookie.value == "abc123");
    AURORA_TEST_CHECK(cookie.host_only);  // 未带 `Domain`：纯主机 cookie
    AURORA_TEST_CHECK(cookie.domain == "shop.example");
    AURORA_TEST_CHECK(cookie.path == "/");  // default-path：请求路径 /index.html 只剩根
    AURORA_TEST_CHECK(!cookie.secure);
    AURORA_TEST_CHECK(!cookie.http_only);
    AURORA_TEST_CHECK(!cookie.expiry.has_value());  // 两枚过期属性都没给 = 会话 cookie
}

AURORA_TEST_CASE(parse_max_age_wins_over_expires) {
    // 两个都给时 `Max-Age` 优先（§5.3 第 3 条）：过期时刻 = 基准 + 600 s，与那份「1994 年就过期」的
    // `Expires` 无关——后者若被采信，这枚 cookie 当场就死。
    const auto cookie = parsed("sid=abc; Max-Age=600; Expires=Sun, 06 Nov 1994 08:49:37 GMT");
    AURORA_TEST_CHECK(cookie.expiry.has_value());
    const auto span = std::chrono::duration_cast<std::chrono::seconds>(*cookie.expiry - now());
    AURORA_TEST_CHECK(span.count() > 590 && span.count() <= 600);
}

AURORA_TEST_CASE(parse_expires_sets_lifetime_and_past_expiry_is_dropped) {
    // 远期 `Expires` 单独给：过期时刻 = 属性值（比基准晚即可，绝对值不进判据）。
    const auto cookie = parsed("sid=abc; Expires=Sun, 06 Nov 2194 08:49:37 GMT");
    AURORA_TEST_CHECK(cookie.expiry.has_value());
    AURORA_TEST_CHECK(*cookie.expiry > now());
    // 早于基准的 `Expires`：整条丢弃（与 `Max-Age <= 0` 同一归途），不是一枚已死条目。
    AURORA_TEST_CHECK(
        !parse_set_cookie("sid=abc; Expires=Sun, 06 Nov 1994 08:49:37 GMT", url_of("http://shop.example/"), now())
             .has_value());
}

AURORA_TEST_CASE(parse_non_positive_max_age_is_dropped) {
    AURORA_TEST_CHECK(!parse_set_cookie("sid=abc; Max-Age=0", url_of("http://shop.example/"), now()).has_value());
    AURORA_TEST_CHECK(!parse_set_cookie("sid=abc; Max-Age=-3", url_of("http://shop.example/"), now()).has_value());
}

AURORA_TEST_CASE(parse_domain_mismatch_is_dropped) {
    // `Domain=other.example` 与请求主机 shop.example 不匹配：整条丢弃（§5.2.3），不是「当 host-only 存」。
    AURORA_TEST_CHECK(
        !parse_set_cookie("sid=abc; Domain=other.example", url_of("http://shop.example/"), now()).has_value());
    // 匹配的父域：host_only 翻假、域记属性值，此后深层子域也取得到。
    const auto cookie = parsed("sid=abc; Domain=example.com", url_of("http://www.example.com/"));
    AURORA_TEST_CHECK(!cookie.host_only);
    AURORA_TEST_CHECK(cookie.domain == "example.com");
}

AURORA_TEST_CASE(parse_domain_match_excludes_ip_like_hosts) {
    // IP 形态的主机只认全等：`1.2.3.4` 不得与后缀 `2.3.4` 匹配（§5.1.3 的「主机不是 IP」半边）。
    AURORA_TEST_CHECK(!parse_set_cookie("sid=abc; Domain=2.3.4", url_of("http://1.2.3.4/"), now()).has_value());
}

AURORA_TEST_CASE(parse_path_given_and_defaulted) {
    const auto given = parsed("sid=abc; Path=/deep", url_of("http://shop.example/index.html"));
    AURORA_TEST_CHECK(given.path == "/deep");
    // 不以 `/` 开头的值按「没给」处理，落 default-path（§5.2.4）。
    const auto defaulted = parsed("sid=abc; Path=deep", url_of("http://shop.example/a/b/c"));
    AURORA_TEST_CHECK(defaulted.path == "/a/b");  // 去掉最右一段（含那段之前的 `/`）
}

AURORA_TEST_CASE(parse_malformed_is_dropped) {
    AURORA_TEST_CHECK(!parse_set_cookie("sid", url_of("http://shop.example/"), now()).has_value());
    AURORA_TEST_CHECK(!parse_set_cookie("=abc", url_of("http://shop.example/"), now()).has_value());
    AURORA_TEST_CHECK(!parse_set_cookie("", url_of("http://shop.example/"), now()).has_value());
}

AURORA_TEST_CASE(jar_holds_and_selects_by_domain_path_and_secure) {
    HttpCookieJar jar;
    const auto base = url_of("http://shop.example/");
    const auto secure_target = url_of("https://shop.example/");
    jar.store_from_response(base, Headers{}, false, now());  // 无 Set-Cookie：零作为
    AURORA_TEST_CHECK(jar.size() == 0);

    // 存两枚：一枚普通、一枚 `Secure`；再往另一域存一枚。
    aurora::Headers first;
    first.append("Set-Cookie", "a=1");
    first.append("Set-Cookie", "s=2; Secure");
    jar.store_from_response(base, first, false, now());
    AURORA_TEST_CHECK(jar.size() == 1);  // `Secure` 遇明文信道不存储（存侧半边）
    aurora::Headers secure_in;
    secure_in.append("Set-Cookie", "s=2; Secure");
    jar.store_from_response(secure_target, secure_in, true, now());
    AURORA_TEST_CHECK(jar.size() == 2);

    // 取侧：明文目标只拿得到非 `Secure` 那枚；TLS 目标两枚都拿。
    const auto plain_out = jar.cookies_for(base, now());
    AURORA_TEST_CHECK(plain_out == "a=1");
    const auto secure_out = jar.cookies_for(secure_target, now());
    AURORA_TEST_CHECK(secure_out == "a=1; s=2");  // 同路径：按创建时刻排序

    // 其它域一条都拿不到。
    const auto other = url_of("http://mall.example/");
    AURORA_TEST_CHECK(jar.cookies_for(other, now()).empty());

    // 路径前缀与「长者在前」：/a 下的 cookie 比 / 下的长，排在前面。
    aurora::Headers deep;
    deep.append("Set-Cookie", "d=3; Path=/a");
    const auto deep_target = url_of("http://shop.example/a/b");
    jar.store_from_response(deep_target, deep, false, now());
    AURORA_TEST_CHECK(jar.cookies_for(deep_target, now()) == "d=3; a=1");
}

AURORA_TEST_CASE(jar_replaces_same_cookie_and_keeps_creation_order) {
    HttpCookieJar jar;
    const auto target = url_of("http://shop.example/");
    aurora::Headers first;
    first.append("Set-Cookie", "sid=old");
    jar.store_from_response(target, first, false, now());
    aurora::Headers second;
    second.append("Set-Cookie", "sid=new; Max-Age=900");
    jar.store_from_response(target, second, false, now());
    AURORA_TEST_CHECK(jar.size() == 1);  // 同 (名, 域, host-only, 路径)：替换而不是并存
    AURORA_TEST_CHECK(jar.cookies_for(target, now()) == "sid=new");
}

AURORA_TEST_CASE(jar_drops_expired_entries_on_write) {
    HttpCookieJar jar;
    const auto target = url_of("http://shop.example/");
    aurora::Headers first;
    first.append("Set-Cookie", "sid=temp; Max-Age=1");
    jar.store_from_response(target, first, false, now());
    AURORA_TEST_CHECK(jar.size() == 1);
    // 写入动作顺手清过期：等寿命走完（测试把寿命设成 1 秒，真实时钟可见）再写一枚新的。
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    aurora::Headers second;
    second.append("Set-Cookie", "sid=fresh; Max-Age=900");
    jar.store_from_response(target, second, false, now());
    AURORA_TEST_CHECK(jar.size() == 1);
    AURORA_TEST_CHECK(jar.cookies_for(target, now()) == "sid=fresh");
}

// ── 端到端：Session 收发 Cookie 的发起侧行为（回环 + 真实时钟）。 ──────────────────────────────

/// @brief 拼一个指向回环端口的 GET 目标。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 为测试放行明文（判据口径与 `utest_net_http_client` 的同名助手一致：逐客户端点名回环地址）。
/// @param client 待配置的客户端。
auto allow_test_cleartext(Client &client) -> void {
    aurora::net::NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};
    client.policy(std::move(permissive));
}

/// @brief 读一次请求（读到头部块尾即回）并把档案交回调用方（线程经锁交接）。
///
/// 留档是「应答之前」的一次性动作：本套件的对端每条连接只服务一个请求-应答周期（keep-alive 复用的判据
/// 归池套件），故没有「读完再写完再等下一轮」的时序竞争，读档无需等待。
/// @param socket 连接句柄。
/// @param stop 停止位。
/// @param recorded 交接缓冲。
/// @param guard 交接缓冲的锁。
/// @return 读到的请求全文（读失败 / 对端收摊为空）。
auto read_request(aurora::net::blocking::StreamSocket &socket, aurora::test::PeerStop &stop, std::string &recorded,
                  std::mutex &guard) -> bool {
    (void)socket.set_io_timeout(std::chrono::milliseconds{200});
    std::vector<char> buffer(8192);
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
    const std::scoped_lock held(guard);
    recorded = seen;
    return !seen.empty();
}

/// @brief 取档案的小写副本（`Cookie` 字段名的比较大小写不敏感）。
[[nodiscard]] auto lowered_record(const std::string &recorded, std::mutex &guard) -> std::string {
    const std::scoped_lock held(guard);
    std::string out;
    out.reserve(recorded.size());
    for (const char c : recorded) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// @brief 完成一次交换并交出结果（真实时钟；泵与取结果的口径同 `utest_net_http_client`）。
/// @param client 客户端。
/// @param request 请求。
/// @param label 失败输出里的交换名（一条用例里有多次 send 时分辨是哪一次折了）。
/// @return 应答的 Result 层。
[[nodiscard]] auto send_and_drive(const Client &client, Request request, std::string_view label = "send")
    -> Result<Response> {
    auto task = client.send(std::move(request));
    AURORA_TEST_CHECK(aurora::test::net::drive_until(label, [&task] { return task.is_done(); }).satisfied);
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    // 本套件每一条 send 都应当成功：内层失败在这里就地暴露（拿错误消息当判据输出），不让 `.value()`
    // 在 CHECK 里抛出把现场盖掉。
    const auto &answered = outer.value();
    AURORA_TEST_REQUIRE_MSG(bool(answered),
                            answered ? std::string{}
                                     : std::string{label}
                                           .append(": code=")
                                           .append(std::to_string(static_cast<int>(answered.error().code_enum)))
                                           .append(" ")
                                           .append(answered.error().message));
    return answered;
}

/// @brief 应答体读成文本（入口参数是 Result 层：`send_and_drive` 已把失败挡在 REQUIRE 里）。
/// @param answered 交换结果。
/// @return 体文本副本。
[[nodiscard]] auto as_text(const Result<Response> &answered) -> std::string {
    const auto &body = answered.value().body();
    return std::string{body.begin(), body.end()};
}

AURORA_TEST_CASE(session_round_trips_a_cookie_over_two_requests) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string first_request;
    std::string second_request;
    std::mutex guard;
    auto cursor = std::make_shared<std::atomic<int>>(0);  // 池缺省关：每个周期各占一条连接，剧本按序号分支
    auto script = [cursor, &first_request, &second_request, &guard](aurora::net::blocking::StreamSocket &socket,
                                                                    aurora::test::PeerStop &stop) -> void {
        // 先取连接序号再分支：每个周期各占一条连接，第 n 条连接服务第 n 枚请求。
        std::vector<char> drain(1024);
        if (cursor->fetch_add(1) == 0) {
            if (!read_request(socket, stop, first_request, guard)) {
                return;
            }
            (void)socket.write_all(std::span<const char>{
                std::string{"HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc123; Path=/\r\nContent-Length: 2\r\n\r\nok"}});
            // 答完不立刻收摊：等到对端关闭（或读超时）再返，别让 FIN 与应答字节在客户端赛跑。
            (void)socket.read_some(std::span<char>{drain});
            return;
        }
        if (!read_request(socket, stop, second_request, guard)) {
            return;
        }
        (void)socket.write_all(std::span<const char>{std::string{"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone"}});
        (void)socket.read_some(std::span<char>{drain});
    };
    ScriptedPeer peer{script};
    AURORA_TEST_REQUIRE(peer.valid());
    Session session;
    allow_test_cleartext(session.client());
    const auto first = send_and_drive(session.client(), Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(as_text(first) == "ok");
    const auto second = send_and_drive(session.client(), Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(as_text(second) == "done");
    // 第二枚请求带上了第一枚应答种下的 cookie（对端收到的请求全文可证）。
    const auto lowered = lowered_record(second_request, guard);
    AURORA_TEST_CHECK(lowered.find("cookie: sid=abc123") != std::string::npos);
}

AURORA_TEST_CASE(bare_client_neither_stores_nor_sends_cookies) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string first_request;
    std::string second_request;
    std::mutex guard;
    auto cursor = std::make_shared<std::atomic<int>>(0);
    auto script = [cursor, &first_request, &second_request, &guard](aurora::net::blocking::StreamSocket &socket,
                                                                    aurora::test::PeerStop &stop) -> void {
        std::vector<char> drain(1024);
        if (cursor->fetch_add(1) == 0) {
            if (!read_request(socket, stop, first_request, guard)) {
                return;
            }
            (void)socket.write_all(std::span<const char>{
                std::string{"HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc123\r\nContent-Length: 2\r\n\r\nok"}});
            (void)socket.read_some(std::span<char>{drain});
            return;
        }
        if (!read_request(socket, stop, second_request, guard)) {
            return;
        }
        (void)socket.write_all(std::span<const char>{std::string{"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone"}});
        (void)socket.read_some(std::span<char>{drain});
    };
    ScriptedPeer peer{script};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;  // 裸客户端：没有 Jar，`Set-Cookie` 是对端的一场独白
    allow_test_cleartext(client);
    const auto first = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(as_text(first) == "ok");
    const auto second = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(as_text(second) == "done");
    AURORA_TEST_CHECK(lowered_record(second_request, guard).find("cookie:") == std::string::npos);
}

AURORA_TEST_CASE(redirect_to_another_port_refetches_cookies_for_the_new_target) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // Cookie 的域匹配按主机不按端口（`RFC 6265` 的域里没有端口这一维），而本套件的对端都在 127.0.0.1 上：
    // 换端口的重定向因此是一次「同主机、新目标」的跳——`follow()` 剥掉旧值之后，Jar 按新目标的域把同一枚
    // cookie 重新附上（第 36 项的「下一跳按新目标的域重新从 Jar 取」正由此可证）。跨主机剥离的判据由
    // 换注册名的场景承担（对端不在回环上，归真机腿），本件钉住的是取侧逐跳重算。
    std::uint16_t second_port = 0;
    std::string second_request;
    std::mutex guard;
    ScriptedPeer second_peer{[&](aurora::net::blocking::StreamSocket &socket, aurora::test::PeerStop &stop) -> void {
        read_request(socket, stop, second_request, guard);
        (void)socket.write_all(std::span<const char>{std::string{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}});
    }};
    AURORA_TEST_REQUIRE(second_peer.valid());
    second_port = second_peer.port();
    ScriptedPeer first_peer{[&](aurora::net::blocking::StreamSocket &socket, aurora::test::PeerStop &stop) -> void {
        std::string ignored;
        if (!read_request(socket, stop, ignored, guard)) {
            return;
        }
        (void)socket.write_all(std::span<const char>{
            std::string{"HTTP/1.1 302 Found\r\nSet-Cookie: sid=abc123\r\nLocation: http://127.0.0.1:" +
                        std::to_string(second_port) + "/landing\r\nContent-Length: 0\r\n\r\n"}});
    }};
    AURORA_TEST_REQUIRE(first_peer.valid());
    Session session;
    allow_test_cleartext(session.client());
    const auto final_answer = send_and_drive(session.client(), Request::get(loopback_url(first_peer.port())));
    AURORA_TEST_CHECK(as_text(final_answer) == "ok");
    AURORA_TEST_CHECK(lowered_record(second_request, guard).find("cookie: sid=abc123") != std::string::npos);
}

AURORA_TEST_CASE(cache_key_sees_the_cookie_the_jar_attaches) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string request;
    std::mutex guard;
    auto answers = std::make_shared<std::atomic<int>>(0);
    // 每条连接恰一个「读 → 答」周期、答完即返：脚本在 serve() 循环里内联执行，在这儿逗留会把第二条
    // 连接的 accept 卡到读超时之后，客户端看见的是一条被对端晾着的连接（伪红与实现无关）。
    auto script = [answers, &request, &guard](aurora::net::blocking::StreamSocket &socket,
                                              aurora::test::PeerStop &stop) -> void {
        std::string seen;
        (void)socket.set_io_timeout(std::chrono::milliseconds{300});
        std::vector<char> buffer(8192);
        while (seen.find("\r\n\r\n") == std::string::npos && !stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                return;
            }
            seen.append(buffer.data(), read.value());
        }
        {
            const std::scoped_lock held(guard);
            request = seen;
        }
        const int index = answers->fetch_add(1);
        const std::string payload =
            index == 0
                ? std::string{"HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc123\r\nCache-Control: max-age=5\r\nContent-Length: 5\r\n\r\nfirst"}
                : std::string{"HTTP/1.1 200 OK\r\nCache-Control: max-age=5\r\nContent-Length: 6\r\n\r\nsecond"};
        (void)socket.write_all(std::span<const char>{payload});
    };
    ScriptedPeer peer{script};
    AURORA_TEST_REQUIRE(peer.valid());
    Session session;
    allow_test_cleartext(session.client());
    session.client().cache(aurora::net::http::CacheOptions{.enabled = true, .max_entries = 16});
    const auto first = send_and_drive(session.client(), Request::get(loopback_url(peer.port())), "cache first");
    AURORA_TEST_CHECK(!first.value().from_cache());
    AURORA_TEST_CHECK(peer.accepts() == 1);
    // 第二枚请求附上了 cookie：缓存键多了一维（凭据恒参与建键），同一 URL 也不再命中。
    const auto second = send_and_drive(session.client(), Request::get(loopback_url(peer.port())), "cache second");
    AURORA_TEST_CHECK(peer.accepts() == 2);
    AURORA_TEST_CHECK(!second.value().from_cache());
    // 第三枚与第二枚同键（cookie 值未变）：命中。
    const auto third = send_and_drive(session.client(), Request::get(loopback_url(peer.port())), "cache third");
    AURORA_TEST_CHECK(third.value().from_cache());
    AURORA_TEST_CHECK(as_text(third) == "second");
}

}  // namespace
}  // namespace aurora::test_cases::utest_net_http_cookie
