/// 测试类型: unit
/// 目标单元: include/aurora/net/http_binding.h + src/aurora/net/http_binding.cpp
/// 测试说明: 声明式交付面的离线回归（§12「声明式绑定」与「默认取图」）。以 `ScriptedPeer` 在回环上
///           按脚本应答真实字节，验四件事各自的判据：`deliver` 把物化应答折成作业值且非 2xx 仍算成功落定、
///           传输失败才走错误侧；`bind_resource` 的取消是真的取消——`reload()` 与宿主销毁都会掐掉前一次请求
///           的那条连接（对端读到收摊、且串行监听口因此才腾出手接受第二条），迟到结果被代次守卫丢弃；
///           交换的会话自持到帧收尾（调用方的 `Client` 临时对象在发起那一刻就销毁也不悬垂。本条是**悬垂防护**
///           而非形态判据：把会话改成不自持属未定义行为，实测未必显形，所有权形态的判据住在 `utest_coroutine`
///           的 `on_done_releases_its_own_capture_once_it_has_fired`）；`deliver_stream`
///           逐段提交、偏移连续、成功收尾恰一条 `last` 标记而失败改走 `fail()`；`image_fetcher` 只在 2xx 给
///           字节，其余状态落 `render-image-load-failed` 并点名状态码，其函数形态与 widget 侧的取图口结构一致
///           （`static_assert` 钉住，网络层不因此依赖 widget）。
/// 全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/http.h"
#include "aurora/net/http_binding.h"
#include "aurora/net/http_client.h"
#include "aurora/net/policy.h"
#include "aurora/state/async.h"
#include "aurora/state/resource.h"
#include "aurora/widget/image_widget.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_binding {
namespace {

using aurora::ErrorCode;
using aurora::net::Cancellation;
using aurora::net::NetPolicy;
using aurora::net::http::Client;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::StreamChunk;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::per_connection;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::write_then_hold;
using aurora::test::scripts::write_then_reset;
using Socket = aurora::net::blocking::StreamSocket;
using aurora::testing::require_value;
namespace m = aurora::testing::matchers;

/// @brief 拼一个指向回环端口的 GET 目标。
/// @param port 对端端口。
/// @param path 路径。
/// @return 绝对 URL。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 拼一份 `Content-Length` 分帧的应答。
/// @param status 状态行（含原因短语，不含状态版本前缀之外的分隔）。
/// @param body 消息体。
/// @param fields 额外头部字段（每行自带 `\\r\\n`，可为空）。
/// @return 应答字节。
[[nodiscard]] auto framed(std::string_view status, std::string_view body, std::string_view fields = {}) -> std::string {
    return "HTTP/1.1 " + std::string{status} + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
           std::string{fields} + "\r\n" + std::string{body};
}

/// @brief 拼一份 chunked 应答的前半：状态行 + 逐块分帧，**不含**终止块。
/// @param status 状态行。
/// @param chunks 依次交出的体片段。
/// @return 应答字节；读到这些之后交换仍挂在读上（中途取消的现场）。
[[nodiscard]] auto chunked_prefix(std::string_view status, const std::vector<std::string> &chunks) -> std::string {
    std::string payload = "HTTP/1.1 " + std::string{status} + "\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (const auto &chunk : chunks) {
        payload += std::to_string(chunk.size()) + "\r\n" + chunk + "\r\n";
    }
    return payload;
}

/// @brief 把一段明文按 chunked 逐块分帧（每块一块，末尾跟终止块）。
/// @param status 状态行。
/// @param chunks 依次交出的体片段。
/// @return 应答字节。
[[nodiscard]] auto chunked(std::string_view status, const std::vector<std::string> &chunks) -> std::string {
    return chunked_prefix(status, chunks) + "0\r\n\r\n";
}

/// @brief 为测试放行明文：缺省安全策略拒绝回环 `http://`，触网用例逐客户端点名。
/// @param client 待配置的客户端。
auto allow_test_cleartext(Client &client) -> void {
    NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};
    client.policy(std::move(permissive));
}

/// @brief 一条「读到对端收摊才收摊」的静默脚本：它同时是前一次请求被掐断的现场记录仪。
///
/// 为什么以对端视角取证：取消的效果在调用方只看得见「结果被丢弃」，而那条被丢弃的结果**本来就该**被代次守卫
/// 丢掉，两者无从分辨。对端读到的那次收摊（EOF）才是「连接真被掐了」的唯一硬事实；又因为本对端按连接串行跑
/// 脚本，第二条连接能被接受本身就是前一条已收摊的证明。
/// @param closed 观测位：读到收摊时置真。
/// @return 脚本。
[[nodiscard]] auto hold_until_closed(std::atomic<bool> &closed) -> PeerScript {
    return [&closed](Socket &socket, PeerStop &stop) -> void {
        // 静默等对端收摊：超时只是防挂死的兜底。50ms 一档在慢腿上会在客户端关闭到达前就默默放弃，
        // 观测位拿不到那次 EOF，把「取消没生效」误报成现场缺失。放到 10s：EOF 到达即返回，不受影响。
        (void)socket.set_io_timeout(std::chrono::seconds{10});
        std::vector<char> buffer(4096);
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read) {
                return;  // 出错或对端收摊都收摊：观测位只认 0 字节这一次
            }
            if (read.value() == 0) {
                closed.store(true, std::memory_order_release);
                return;
            }
        }
    };
}

/// @brief 泵宿主直到谓词成立（预算 2 s，比用例里任何一枚时限都短）。
/// @tparam Pred 谓词。
/// @param subject 等待主题。
/// @param pred 目标条件。
/// @return 条件成立为真。
template <typename Pred>
[[nodiscard]] auto pump(std::string_view subject, const Pred &pred) -> bool {
    return aurora::test::net::drive_until(subject, pred, std::chrono::milliseconds{2000}).satisfied;
}

/// @brief 取回作业落定的结果：等待一次 `then` 抵达，再交出那份 `Result`。
/// @tparam T 作业值类型。
/// @param subject 等待主题（进超时文本）。
/// @param job 待等的作业。
/// @return 落定结果；等待超时时由 `REQUIRE` 收场。
template <typename T>
[[nodiscard]] auto await_job(std::string_view subject, aurora::Task<T> job) -> Result<T> {
    std::atomic<bool> settled{false};
    Result<T> outcome{make_error(ErrorCode::RuntimeAsyncException, "job never settled")};
    job.then([&settled, &outcome](const Result<T> &r) -> void {
        outcome = r;
        settled.store(true, std::memory_order_release);
    });
    AURORA_TEST_REQUIRE_MSG(pump(subject, [&settled] { return settled.load(std::memory_order_acquire); }),
                            "job did not settle within the wait budget");
    return outcome;
}

}  // namespace

AURORA_TEST_CASE(deliver_projects_a_materialized_response) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, framed("200 OK", "hello")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto job = aurora::net::http::deliver<std::string>(
        client, Request::get(loopback_url(peer.port())), [](const Response &response) -> Result<std::string> {
            return Result<std::string>{std::string(response.body().begin(), response.body().end())};
        });

    const auto outcome = await_job<std::string>("deliver settles", std::move(job));
    AURORA_TEST_REQUIRE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.value(), std::string{"hello"});
}

AURORA_TEST_CASE(a_non_success_status_is_a_ready_value_not_a_failure) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, framed("404 Not Found", "missing")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    // 与 `send` 同口径：状态码归调用方判，本面不新造「HTTP 状态错误」。资源因此进 Ready 而不是 Failed。
    auto resource = aurora::net::http::bind_resource(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(pump("resource ready", [&resource] { return resource.phase() == aurora::ResourcePhase::Ready; }));
    AURORA_TEST_REQUIRE(resource.value() != nullptr);
    AURORA_TEST_CHECK_EQ(resource.value()->status(), std::uint16_t{404});
    AURORA_TEST_CHECK_EQ(std::string(resource.value()->body().begin(), resource.value()->body().end()),
                         std::string{"missing"});
}

AURORA_TEST_CASE(a_transport_failure_lands_on_the_error_side) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(write_then_reset(""));  // 连上即 RST：读侧拿到一次真实的套接口错误
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto resource = aurora::net::http::bind_resource(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        pump("resource failed", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; }));
    AURORA_TEST_CHECK_NE(require_value(resource.error()).code_enum, ErrorCode::NetCancelled);
    AURORA_TEST_CHECK(resource.value() == nullptr);
}

AURORA_TEST_CASE(a_reload_cuts_the_socket_of_the_previous_request) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::atomic<bool> first_closed{false};
    // 第一条连接静默到被掐断，第二条照常应答：串行监听口能接受第二条，本身就是前一条已收摊的证明。
    ScriptedPeer peer(
        per_connection({hold_until_closed(first_closed), reply_after_reading(1, framed("200 OK", "second"))}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto resource = aurora::net::http::bind_resource(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(pump("first attempt connects", [&peer] { return peer.accepts() >= 1; }));
    AURORA_TEST_CHECK_EQ(resource.phase(), aurora::ResourcePhase::Loading);

    resource.reload();  // ← 取消前序 + 重发：这一步必须同时掐掉那条挂着读的连接
    AURORA_TEST_CHECK(
        pump("second attempt lands", [&resource] { return resource.phase() == aurora::ResourcePhase::Ready; }));
    AURORA_TEST_CHECK(first_closed.load(std::memory_order_acquire));  // 对端读到收摊：不是只丢了结果
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);  // 串行对端腾出了第二条连接
    AURORA_TEST_REQUIRE(resource.value() != nullptr);
    AURORA_TEST_CHECK_EQ(std::string(resource.value()->body().begin(), resource.value()->body().end()),
                         std::string{"second"});  // 前一次的迟到结果没有盖住这一次
}

AURORA_TEST_CASE(destroying_the_owner_releases_the_in_flight_socket) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::atomic<bool> closed{false};
    ScriptedPeer peer(hold_until_closed(closed));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    {
        auto resource = aurora::net::http::bind_resource(client, Request::get(loopback_url(peer.port())));
        AURORA_TEST_CHECK(pump("attempt connects", [&peer] { return peer.accepts() >= 1; }));
    }  // 最后一个句柄放手 → 内核析构 → 取消作业 → 取消钩子掐连接
    AURORA_TEST_CHECK(pump("socket released", [&closed] { return closed.load(std::memory_order_acquire); }));
    AURORA_TEST_CHECK_TRUE(closed.load(std::memory_order_acquire));
}

AURORA_TEST_CASE(the_exchange_survives_the_callers_client) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, framed("200 OK", "late")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    std::optional<aurora::Task<Response>> job;
    {
        Client temporary;
        allow_test_cleartext(temporary);
        // 临时客户端在这条语句末尾就销毁，而交换还在协程里跑：帧持有的是那份堆上的会话拷贝，不是这里的 this。
        job.emplace(aurora::net::http::deliver(temporary, Request::get(loopback_url(peer.port()))));
    }
    const auto outcome = await_job<Response>("deliver settles", std::move(*job));
    AURORA_TEST_REQUIRE(bool(outcome));
    AURORA_TEST_CHECK_EQ(std::string(outcome.value().body().begin(), outcome.value().body().end()),
                         std::string{"late"});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
    (void)client;  // 另一份常驻客户端只为证明两种形态都能起
}

AURORA_TEST_CASE(deliver_stream_commits_segments_then_one_final_marker) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, chunked("200 OK", {"abc", "de", "fghij"})));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    // 写出条目类型：流资源装的就是 `StreamChunk`，段与收尾标记同一种形态。
    aurora::StreamResource<StreamChunk> resource =
        aurora::net::http::deliver_stream(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(pump("stream closes", [&resource] {
        const auto &items = resource.items();
        return !items.empty() && items.back().last;
    }));

    const auto &items = resource.items();
    AURORA_TEST_REQUIRE(items.size() >= 2);  // 至少一段体 + 一枚收尾标记
    std::uint64_t next_offset = 0;
    std::string text;
    for (const auto &chunk : items) {
        AURORA_TEST_CHECK_EQ(chunk.offset, next_offset);  // 偏移连续：乱序或多出一段当场可见
        AURORA_TEST_CHECK_EQ(chunk.status, std::uint16_t{200});
        next_offset += chunk.bytes.size();
        text.append(chunk.bytes.begin(), chunk.bytes.end());
    }
    AURORA_TEST_CHECK(items.back().bytes.empty());  // 收尾标记只说「收摊了」，不带字节
    AURORA_TEST_CHECK_EQ(text, std::string{"abcdefghij"});
    AURORA_TEST_CHECK_EQ(resource.phase(), aurora::ResourcePhase::Ready);
}

AURORA_TEST_CASE(a_cancelled_stream_fails_without_a_final_marker) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    // 两块发完就不再说（缺终止块）：交换挂在读上，正是「中途取消」的现场。
    ScriptedPeer peer(write_then_hold(chunked_prefix("200 OK", {"abc", "de"})));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    Cancellation token;
    auto resource = aurora::net::http::deliver_stream(client, Request::get(loopback_url(peer.port())), token);
    // 先让至少一段落地：取消要停在「已经交付过」之后，才能验「已提交数据保留、收尾标记不再出」。
    AURORA_TEST_CHECK(pump("first segment lands", [&resource] { return !resource.items().empty(); }));
    AURORA_TEST_REQUIRE_MSG(!resource.items().empty(), "no segment reached the stream before cancel");
    const std::size_t committed = resource.items().size();

    token.cancel();
    AURORA_TEST_CHECK(pump("stream fails", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; }));
    AURORA_TEST_CHECK_EQ(require_value(resource.error()).code_enum, ErrorCode::NetCancelled);
    AURORA_TEST_REQUIRE(!resource.items().empty());
    AURORA_TEST_CHECK_FALSE(resource.items().back().last);  // 失败的流不产假收尾标记
    AURORA_TEST_CHECK_EQ(resource.items().size(), committed);  // 取消之后不再提交，已提交的都留着
}

AURORA_TEST_CASE(image_fetcher_returns_the_body_on_a_success_status) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    const std::string png{"\x89PNG\r\n\x1a\nFAKE"};
    ScriptedPeer peer(reply_after_reading(1, framed("200 OK", png)));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto fetch = aurora::net::http::image_fetcher(client);
    const auto outcome =
        await_job<std::vector<std::uint8_t>>("image lands", fetch(std::string_view{loopback_url(peer.port())}));
    AURORA_TEST_REQUIRE(bool(outcome));
    const std::vector<std::uint8_t> expected(png.begin(), png.end());
    AURORA_TEST_CHECK(outcome.value() == expected);  // 逐字节等价：符号性转换不能改一个字节
}

AURORA_TEST_CASE(image_fetcher_maps_a_not_found_to_an_image_load_error) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, framed("404 Not Found", "<html>nope</html>")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto fetch = aurora::net::http::image_fetcher(client);
    const auto outcome =
        await_job<std::vector<std::uint8_t>>("image fails", fetch(std::string_view{loopback_url(peer.port())}));
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, ErrorCode::RenderImageLoadFailed);
    // 原因必须点名状态码：404 的 HTML 喂给解码器只会换来「不是图片」，现场就此丢了。
    AURORA_TEST_CHECK_THAT(outcome.error().message, m::has_substr("404"));
}

AURORA_TEST_CASE(the_fetcher_shape_matches_the_widget_slot_without_depending_on_it) {
    // 结构一致是本面能装进 `ImageView` 的全部前提；网络层不 include widget，故这里由测试把两侧对齐。
    // 取图口别名住在 `aurora`（声明于 widget 侧头文件），本用例 include 它只为拿到这个别名。
    static_assert(std::is_same_v<decltype(aurora::net::http::image_fetcher()), aurora::ImageFetcher>);
    AURORA_TEST_CHECK_TRUE(true);
}

}  // namespace aurora::test_cases::utest_net_http_binding
