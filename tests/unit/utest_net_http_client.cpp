/// 测试类型: unit
/// 目标单元: include/aurora/net/http_client.h + src/aurora/net/http_client.cpp
/// 测试说明: HTTP 发起侧 send 核心闭环的离线回归（CHG-010 §6 / §12）。以 ScriptedPeer 在回环上按脚本
///           应答真实字节，覆盖 Content-Length 与 Chunked 两种分帧的物化、302 重定向跟随（跨到第二个对端）
///           与超限落 `http-redirect-limit`；整请求超时用 FakeEngine 脚本化时钟（连上但静默的对端 + 推进到
///           时限）验 `http-timeout`。全部判据零出站（只绑 127.0.0.1），宿主驱动 pump 在用例线程推进协程。
/// 全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免。

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/state/coroutine.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_client {
namespace {

using aurora::CoroTask;
using aurora::Result;
using aurora::net::http::Client;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::Timeouts;
using aurora::test::FakeEngine;
using aurora::test::FakeEngineScope;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::silent;
using aurora::test::scripts::write_then_hold;

/// @brief 拼一个指向回环端口的 GET 目标。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 取协程结果的内存活 Response 的 Result 层（协程本身不抛，外层恒成功）。
[[nodiscard]] auto inner(CoroTask<Result<Response>> &task) -> Result<Response> {
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

}  // namespace

AURORA_TEST_CASE(send_materializes_content_length) {
    Client client;
    ScriptedPeer peer(
        reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "hello");
}

AURORA_TEST_CASE(send_materializes_chunked_body) {
    Client client;
    ScriptedPeer peer(reply_after_reading(
        1, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "hello world");
}

AURORA_TEST_CASE(send_follows_redirect_across_peers) {
    Client client;
    ScriptedPeer target(reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nZ"));
    AURORA_TEST_REQUIRE(target.valid());
    const std::string redirect =
        "HTTP/1.1 302 Found\r\nLocation: " + loopback_url(target.port()) + "\r\nContent-Length: 0\r\n\r\n";
    ScriptedPeer origin(reply_after_reading(1, redirect));
    AURORA_TEST_REQUIRE(origin.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(origin.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "Z");
    AURORA_TEST_CHECK_EQ(origin.accepts(), 1);
    AURORA_TEST_CHECK_EQ(target.accepts(), 1);
}

AURORA_TEST_CASE(send_stops_at_redirect_limit) {
    Client client;
    // 每跳都回 302 指向自身路径：跟随必然撞到上限而不是收敛。
    ScriptedPeer peer(reply_after_reading(1, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port(), "/loop")).max_redirects(2));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpRedirectLimit);
    // 初始一跳 + 两跳跟随 = 三次连接，其后因超限而止。
    AURORA_TEST_CHECK_EQ(peer.accepts(), 3);
}

AURORA_TEST_CASE(send_times_out_whole_request) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;

    Client client;
    ScriptedPeer peer(silent());  // 连得上，但永不回字节：读侧一直挂着。
    AURORA_TEST_REQUIRE(peer.valid());

    auto task = client.send(Request::get(loopback_url(peer.port())).timeout(std::chrono::milliseconds{50}));
    // 先泵到连接与写出落地（对端已 accept、读请求挂起）。
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    // 推进脚本化时钟到整请求时限：到期取消令牌，挂起的读以 net-cancelled 收尾，收尾据 timed_out 改道。
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

AURORA_TEST_CASE(send_times_out_read_idle) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    Client client;
    // 对端只写头部（声明 100 字节体却不再给）后静默：读体阶段挂起，read_idle 到点。
    ScriptedPeer peer(write_then_hold("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());

    auto task = client.send(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.read_idle = std::chrono::milliseconds{50}}));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

AURORA_TEST_CASE(send_times_out_connect) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    Client client;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    // 不 pump：让 connect 始终不落地，直接推进时钟触发 connect 档。
    auto task = client.send(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.connect = std::chrono::milliseconds{50}}));
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

}  // namespace aurora::test_cases::utest_net_http_client
