/// 测试类型: unit
/// 目标单元: tests/framework/fake_engine.h + tests/framework/scripted_peer.h + tests/framework/net_wait.h
///           + src/aurora/net/detail/engine_override.h
/// 测试说明: 测试替身切片自身的契约，以及「七类故障可脚本化」的可证形态。
///   替身面：`Engine::instance()` 在替身生效期间指向脚本化引擎、退出守卫即恢复本腿真引擎；任务与
///   定时器**只登记不自己走**，时钟由用例推进；同到期时刻按登记序稳定执行；停机丢弃未到期登记（与
///   真实引擎同口径）；拒绝投递回 `net-platform-unavailable`（无引擎腿的同一条码）；能力位可改写，
///   于是上层「能力缺失即降级」的分支今天就能被脚本命中。
///   故障目录面：超时、取消、半关闭、乱序、超大帧、代理不可达、慢连接七类各一条，加上 TLS 对端致命
///   告警一条，全部跑在**真连接**（回环）+ **脚本对端**（+ 脚本时钟）上，零出站。TLS 的第七类故障
///   （`a_peer_fatal_alert_settles_the_handshake_exactly_once`）是提案 §16 第 17 项留给 TLS 切片的那枚，
///   随该切片补齐：告警字节是脚本，握手状态机与结局码是真的；没有 TLS 后端的腿报成缺项而不是伪造一条。
///   等待辅助面：宿主驱动 pump 与预算口径收在 `framework/net_wait.h` 一处，本文件即是它的使用方。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码）；
///    需要 socket 与线程的故障用例在 wasm 腿退化为 AURORA_TEST_SKIP。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/capabilities.h"
#include "aurora/net/detail/engine_override.h"
#include "aurora/net/detail/tls_backend.h"
#include "aurora/net/engine.h"
#include "aurora/net/framer.h"
#include "aurora/net/proxy.h"
#include "aurora/net/tls.h"
#include "aurora/net/transport.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_test_double {
namespace {

using aurora::net::Cancellation;
using aurora::net::Endpoint;
using aurora::net::FrameOptions;
using aurora::net::Framer;
using aurora::net::ProxyConfig;
using aurora::net::StreamOptions;
using aurora::net::StreamTransport;
using aurora::test::AURORA_WAIT_BUDGET;
using aurora::test::FakeEngine;
using aurora::test::FakeEngineScope;
using aurora::test::ScriptedPeer;
using aurora::test::net::drive_until;
using aurora::test::net::HostDriven;
using aurora::test::net::pump_idle;
using aurora::test::scripts::flood;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::scatter;
using aurora::test::scripts::silent;
using aurora::test::scripts::write_then_half_close;
using aurora::test::scripts::write_then_hold;

/// @brief 观察窗：足够盖住一次回环连接与一轮泵，又不至于把「不该发生的第二条结局」伪装成慢。
[[maybe_unused]] constexpr std::chrono::milliseconds AURORA_GRACE_WINDOW{200};

/// @brief 脚本时限：连接类用例的时限由替身推进，真时钟一毫秒都不必等。
[[maybe_unused]] constexpr std::chrono::milliseconds AURORA_SCRIPTED_TIMEOUT{500};

/// @brief 一次连接结局的观测记录。
///
/// 宿主驱动 + 替身时钟的形态下回调恒在用例线程，但记录本身仍加锁：本文件同时是「替身装拆不遗留
/// 跨线程状态」的证据，锁着写比假设单线程更诚实。
struct ConnectLog {
    std::mutex mutex;
    int calls = 0;
    bool failed = false;
    std::string code;
    std::string detail;  ///< 失败原因文本（红时靠它指认阶段）
    StreamTransport stream;  ///< 成功时接管
};

/// @brief 一次读写的观测记录（原子字段，回调与用例线程共用）。
struct LegLog {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> end_of_file{false};
};

/// @brief 取已记录的回调用次数。
/// @param log 目标日志。
/// @return 次数。
[[maybe_unused]] auto calls_of(ConnectLog &log) -> int {
    std::scoped_lock lock(log.mutex);
    return log.calls;
}

/// @brief 取失败原因 slug（成功或未收尾时为空）。
/// @param log 目标日志。
/// @return slug 文本副本。
[[maybe_unused]] auto code_of(ConnectLog &log) -> std::string {
    std::scoped_lock lock(log.mutex);
    return log.code;
}

/// @brief 把一次连接结局记进日志。
/// @param log 目标日志（须存活到回调抵达）。
/// @return 可直接交给 `connect` / `connect_via` 的回调。
[[maybe_unused]] auto connect_into(ConnectLog &log) -> aurora::net::ConnectDone {
    return [&log](auto result) {
        std::scoped_lock lock(log.mutex);
        if (!result) {
            log.failed = true;
            log.code = std::string{result.error().code};
            log.detail = result.error().message;
        } else {
            log.stream = std::move(result.value());
        }
        ++log.calls;
    };
}

/// @brief 把一次读结局记进日志。
/// @param log 目标日志。
/// @return 读回调。
auto read_into(LegLog &log) -> aurora::net::ReadDone {
    return [&log](auto result) {
        if (!result) {
            log.failed.store(true);
        } else if (result.value() == 0) {
            log.end_of_file.store(true);
        } else {
            log.bytes.store(result.value());
        }
        ++log.calls;
    };
}

/// @brief 把一次写结局记进日志。
/// @param log 目标日志。
/// @return 写回调。
auto write_into(LegLog &log) -> aurora::net::WriteDone {
    return [&log](auto result) {
        if (!result) {
            log.failed.store(true);
        } else {
            log.bytes.store(result.value());
        }
        ++log.calls;
    };
}

/// @brief 替身形态下的连接口径：时限交给脚本时钟，其余按缺省。
/// @param timeout 时限；非零值才会登记定时器。
/// @return 连接选项。
[[maybe_unused]] auto scripted_options(std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) -> StreamOptions {
    StreamOptions options;
    options.connect_timeout = timeout;
    return options;
}

/// @brief 读一轮并回报字节（EOF 时返回空串）。
/// @param stream 目标流。
/// @param scratch 读缓冲。
/// @return 本轮读到的字节；对端已停写或失败时为空串（失败另有 `ok` 输出）。
[[maybe_unused]] auto read_round(StreamTransport &stream, std::vector<char> &scratch, bool *ok = nullptr) -> std::string {
    LegLog log;
    stream.read(std::span<char>(scratch), read_into(log));
    const auto wait = drive_until("read-lands", [&log]() -> bool { return log.calls.load() >= 1; });
    AURORA_TEST_WAIT_UNTIL(wait);
    if (ok != nullptr) {
        *ok = !log.failed.load();
    }
    AURORA_TEST_CHECK(!log.failed.load());
    if (log.end_of_file.load() || log.bytes.load() == 0) {
        return {};
    }
    return std::string{scratch.data(), log.bytes.load()};
}

/// @brief 写一段字节并等到写完。
/// @param stream 目标流。
/// @param payload 待写文本。
[[maybe_unused]] auto write_round(StreamTransport &stream, std::string_view payload) -> void {
    LegLog log;
    stream.write(std::span<const char>(payload.data(), payload.size()), write_into(log));
    const auto wait = drive_until("write-lands", [&log]() -> bool { return log.calls.load() >= 1; });
    AURORA_TEST_WAIT_UNTIL(wait);
    AURORA_TEST_CHECK(!log.failed.load());
}

}  // namespace

AURORA_TEST_CASE(engine_override_routes_the_instance_and_restores_it_on_scope_exit) {
    // 装拆成对是这条接缝唯一的危险点：留下替身等于把整个进程的时间源交给一个已销毁的对象。
    const auto before = aurora::net::Engine::instance().name();
    FakeEngine fake;
    {
        const FakeEngineScope scope{fake};
        AURORA_TEST_CHECK_EQ(aurora::net::Engine::instance().name(), "fake");
        AURORA_TEST_CHECK_EQ(aurora::net::detail::engine_override(), &fake);
    }
    AURORA_TEST_CHECK_EQ(aurora::net::detail::engine_override(), nullptr);
    AURORA_TEST_CHECK_NE(aurora::net::Engine::instance().name(), "fake");
    // 恢复的是同一个真引擎入口（本腿为 poll / none，二者都不该是替身名）。
    AURORA_TEST_CHECK_EQ(aurora::net::Engine::instance().name(), before);
}

AURORA_TEST_CASE(work_is_registered_but_only_runs_when_the_clock_is_advanced) {
    FakeEngine fake;
    const FakeEngineScope scope{fake};

    int ran = 0;
    AURORA_TEST_CHECK(fake.post([&ran]() -> void { ++ran; }).ok());
    AURORA_TEST_CHECK(fake.post_after(std::chrono::milliseconds{10}, [&ran]() -> void { ++ran; }).ok());
    // 登记不等于执行：这是替身存在的全部理由——时限由用例说了算。
    AURORA_TEST_CHECK_EQ(ran, 0);
    AURORA_TEST_CHECK_EQ(fake.pending(), std::size_t{2});
    AURORA_TEST_CHECK(fake.running());

    AURORA_TEST_CHECK_EQ(fake.run_pending(), std::size_t{1});  // 只有立即到期那条
    AURORA_TEST_CHECK_EQ(ran, 1);
    AURORA_TEST_CHECK_EQ(fake.now().count(), 0);

    AURORA_TEST_CHECK_EQ(fake.advance(std::chrono::milliseconds{9}), std::size_t{0});  // 还差 1 ms 到期
    AURORA_TEST_CHECK_EQ(ran, 1);
    AURORA_TEST_CHECK_EQ(fake.now().count(), 9);

    AURORA_TEST_CHECK_EQ(fake.advance(std::chrono::milliseconds{1}), std::size_t{1});
    AURORA_TEST_CHECK_EQ(ran, 2);
    AURORA_TEST_CHECK_EQ(fake.pending(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(fake.tasks_run(), 2);
}

AURORA_TEST_CASE(same_instant_timers_run_in_registration_order) {
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    std::vector<int> order;
    // 同一到期时刻内先登记先执行：竞速交错与「谁先落结局」的判据需要这条稳定性，否则替身自身就成了抖动源。
    for (int i = 0; i < 5; ++i) {
        AURORA_TEST_CHECK(
            fake.post_after(std::chrono::milliseconds{7}, [&order, i]() -> void { order.push_back(i); }).ok());
    }
    (void)fake.advance(std::chrono::milliseconds{7});
    AURORA_TEST_REQUIRE_EQ(order.size(), std::size_t{5});
    for (int i = 0; i < 5; ++i) {
        AURORA_TEST_CHECK_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

AURORA_TEST_CASE(shutdown_drops_unexpired_work_just_like_the_real_engine) {
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    int fired = 0;
    AURORA_TEST_CHECK(fake.post_after(std::chrono::milliseconds{50}, [&fired]() -> void { ++fired; }).ok());
    fake.shutdown();
    AURORA_TEST_CHECK_EQ(fake.pending(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(fake.dropped_on_shutdown(), 1);
    AURORA_TEST_CHECK_FALSE(fake.running());

    // 时钟推过到期时刻也不会执行已被丢弃的定时器：真实引擎的定时器只承载「放弃等待」，
    // 停机后静默跑一个过期续体会造出假的完成时序。
    (void)fake.advance(std::chrono::milliseconds{100});
    AURORA_TEST_CHECK_EQ(fired, 0);

    // 停机后再次投递即重新工作（惰性启动口径）。
    AURORA_TEST_CHECK(fake.post([&fired]() -> void { ++fired; }).ok());
    AURORA_TEST_CHECK(fake.running());
    (void)fake.run_pending();
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(refusing_posts_answers_with_the_no_engine_leg_code) {
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    fake.refuse_posts(true);
    const auto posted = fake.post([]() -> void {});
    const auto timed = fake.post_after(std::chrono::milliseconds{1}, []() -> void {});
    AURORA_TEST_CHECK_FALSE(posted.ok());
    AURORA_TEST_CHECK_FALSE(timed.ok());
    // 与真实「无引擎腿」同一条码：上层「排队失败要把原因带进回调而不静默悬挂」的判据据此可测。
    AURORA_TEST_CHECK_STREQ(std::string{posted.error().code}, "net-platform-unavailable");
    AURORA_TEST_CHECK_STREQ(std::string{timed.error().code}, "net-platform-unavailable");
    AURORA_TEST_CHECK_EQ(fake.posts_refused(), 2);
    AURORA_TEST_CHECK_EQ(fake.pending(), std::size_t{0});

    fake.refuse_posts(false);
    AURORA_TEST_CHECK(fake.post([]() -> void {}).ok());
}

AURORA_TEST_CASE(capabilities_are_scriptable_for_degradation_branches) {
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    // 缺省形态：事件源与流式可用作真，其余为假——替身不得比真实引擎更乐观。
    AURORA_TEST_CHECK(aurora::net::Engine::instance().capabilities().event_source);
    AURORA_TEST_CHECK_FALSE(aurora::net::Engine::instance().capabilities().tls);

    auto caps = aurora::net::Engine::instance().capabilities();
    caps.tls = true;
    caps.local_ipc = true;
    fake.set_capabilities(caps);
    AURORA_TEST_CHECK(aurora::net::Engine::instance().capabilities().tls);
    AURORA_TEST_CHECK(aurora::net::Engine::instance().capabilities().local_ipc);
    AURORA_TEST_CHECK_FALSE(aurora::net::Engine::instance().capabilities().udp);
}

AURORA_TEST_CASE(a_silent_proxy_times_out_exactly_once_without_wall_clock) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to tunnel over");
#else
    AURORA_TEST_REQUIRE_THREADS();
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    const HostDriven driven;

    ScriptedPeer proxy{silent()};
    AURORA_TEST_REQUIRE(proxy.valid());

    const auto config = ProxyConfig::http_connect("127.0.0.1", proxy.port(), {});
    AURORA_TEST_REQUIRE(config.ok());

    ConnectLog log;
    StreamTransport::connect_via(config.value(), "tunnel.example", 443, scripted_options(AURORA_SCRIPTED_TIMEOUT),
                                 Cancellation{}, connect_into(log));

    // 连接是真的：对端确实 accept 到了这条 TCP，只是它永远不说握手应答。
    AURORA_TEST_WAIT_UNTIL(drive_until("proxy-accepted", [&proxy]() -> bool { return proxy.accepts() >= 1; }));
    AURORA_TEST_CHECK(fake.pending() >= 1);  // 时限定时器登记在替身名下，而不是挂在真实时钟上

    // 推进时钟即到期：整个过程真时钟只走了泵的时间，判定与机器快慢无关。
    (void)fake.advance(AURORA_SCRIPTED_TIMEOUT);
    AURORA_TEST_WAIT_UNTIL(drive_until("timeout-lands", [&log]() -> bool { return calls_of(log) >= 1; }));
    AURORA_TEST_CHECK_EQ(code_of(log), "net-connect-timeout");

    // 再推一段也不会出第二条结局：一次连接只交付一次结局。
    (void)fake.advance(std::chrono::seconds{2});
    pump_idle(AURORA_GRACE_WINDOW);
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
#endif
}

AURORA_TEST_CASE(cancellation_wins_and_a_stale_timer_cannot_deliver_twice) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to cancel");
#else
    AURORA_TEST_REQUIRE_THREADS();
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    const HostDriven driven;

    ScriptedPeer proxy{silent()};
    AURORA_TEST_REQUIRE(proxy.valid());
    const auto config = ProxyConfig::http_connect("127.0.0.1", proxy.port(), {});
    AURORA_TEST_REQUIRE(config.ok());

    Cancellation cancel;
    ConnectLog log;
    StreamTransport::connect_via(config.value(), "tunnel.example", 443, scripted_options(AURORA_SCRIPTED_TIMEOUT),
                                 cancel, connect_into(log));
    AURORA_TEST_WAIT_UNTIL(drive_until("proxy-accepted", [&proxy]() -> bool { return proxy.accepts() >= 1; }));

    cancel.cancel();
    AURORA_TEST_WAIT_UNTIL(drive_until("cancel-lands", [&log]() -> bool { return calls_of(log) >= 1; }));
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    AURORA_TEST_CHECK_EQ(code_of(log), "net-cancelled");

    // 时限定时器仍在替身队列里（引擎接口没有撤销口），它到点时那条连接已经没了：
    // 「取消先落、定时器后到」必须只有一次结局，这条判据在没有脚本时钟时只能靠真睡 500 ms 碰运气。
    const auto armed = fake.pending();
    AURORA_TEST_CHECK_GE(armed, std::size_t{1});
    (void)fake.advance(std::chrono::seconds{5});
    pump_idle(AURORA_GRACE_WINDOW);
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
#endif
}

AURORA_TEST_CASE(a_peer_that_stops_writing_reads_as_zero_byte_eof) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no loopback peer");
#else
    AURORA_TEST_REQUIRE_THREADS();
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    const HostDriven driven;

    ScriptedPeer peer{write_then_half_close("bye")};
    AURORA_TEST_REQUIRE(peer.valid());

    const auto endpoint = Endpoint::loopback(peer.port());
    ConnectLog log;
    StreamTransport::connect(endpoint, scripted_options(), Cancellation{}, connect_into(log));
    AURORA_TEST_WAIT_UNTIL(drive_until("connected", [&log]() -> bool { return calls_of(log) >= 1; }));
    AURORA_TEST_REQUIRE_EQ(calls_of(log), 1);
    AURORA_TEST_REQUIRE_FALSE(log.failed);

    std::vector<char> scratch(4096);
    {
        std::scoped_lock lock(log.mutex);
        AURORA_TEST_CHECK_EQ(read_round(log.stream, scratch), "bye");
        // 半关闭是「对端停写」而不是错误：读得 0 字节且是 OK，本端仍可继续写（提案 §18-2 的判据）。
        bool ok = false;
        AURORA_TEST_CHECK_EQ(read_round(log.stream, scratch, &ok), "");
        AURORA_TEST_CHECK(ok);
        AURORA_TEST_CHECK(log.stream.is_open());
        write_round(log.stream, "still-writing");
        AURORA_TEST_CHECK(log.stream.is_open());
        // 替身期间用例线程不睡真实时限，这条连接的每一步都由泵与脚本时钟决定。
        AURORA_TEST_CHECK_EQ(fake.tasks_run(), 0);
    }
#endif
}

AURORA_TEST_CASE(scattered_fragments_reassemble_without_loss_or_dupes) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no loopback peer");
#else
    AURORA_TEST_REQUIRE_THREADS();
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    const HostDriven driven;

    // 「乱序」在流上的可证形态是**分片边界与消息边界错位**：同一条应答被打成六段、段间留间隔，
    // 到达次序仍由 TCP 保证（流上没有真正的乱序，声称能测乱序就是假能力）。
    const std::vector<std::string> wire{"HTTP/1.1 20", "0 OK\r\nConte", "nt-Length: 5", "\r\n\r\nhel", "lo", "\r\n"};
    const std::string whole{[&wire]() {
        std::string joined;
        for (const auto &fragment : wire) {
            joined += fragment;
        }
        return joined;
    }()};

    ScriptedPeer peer{scatter(wire)};
    AURORA_TEST_REQUIRE(peer.valid());

    ConnectLog log;
    StreamTransport::connect(Endpoint::loopback(peer.port()), scripted_options(), Cancellation{}, connect_into(log));
    AURORA_TEST_WAIT_UNTIL(drive_until("connected", [&log]() -> bool { return calls_of(log) >= 1; }));
    AURORA_TEST_REQUIRE_FALSE(log.failed);

    auto made = Framer::lines(FrameOptions{});
    AURORA_TEST_REQUIRE(made.ok());
    auto &framer = made.value();
    std::vector<std::string> frames;
    std::string seen;
    std::vector<char> scratch(4096);
    const auto sink = [&frames](const aurora::net::Frame &frame) -> bool {
        frames.emplace_back(frame.bytes);
        return true;
    };
    {
        std::scoped_lock lock(log.mutex);
        for (std::size_t round = 0; round < 12 && seen.size() < whole.size(); ++round) {
            const auto chunk = read_round(log.stream, scratch);
            if (chunk.empty()) {
                break;
            }
            seen += chunk;
            const auto fed = framer.feed(std::span<const char>{chunk}, sink);
            AURORA_TEST_REQUIRE(fed.ok());
        }
    }
    // 字节一个不丢：分片怎么切都不改变线上内容。
    AURORA_TEST_CHECK_EQ(seen, whole);
    // 帧边界只由终止符决定，与分片边界无关：六段碎片重组出四条逻辑行（含一条空行 = 头部块结束）。
    AURORA_TEST_REQUIRE_EQ(frames.size(), std::size_t{4});
    AURORA_TEST_CHECK_STREQ(frames[0], "HTTP/1.1 200 OK");
    AURORA_TEST_CHECK_STREQ(frames[1], "Content-Length: 5");
    AURORA_TEST_CHECK_STREQ(frames[2], "");
    AURORA_TEST_CHECK_STREQ(frames[3], "hello");
#endif
}

AURORA_TEST_CASE(a_flooded_peer_overruns_the_frame_limit_deterministically) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no loopback peer");
#else
    AURORA_TEST_REQUIRE_THREADS();
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    const HostDriven driven;

    ScriptedPeer peer{flood(200000, 'z')};
    AURORA_TEST_REQUIRE(peer.valid());

    ConnectLog log;
    StreamTransport::connect(Endpoint::loopback(peer.port()), scripted_options(), Cancellation{}, connect_into(log));
    AURORA_TEST_WAIT_UNTIL(drive_until("connected", [&log]() -> bool { return calls_of(log) >= 1; }));
    AURORA_TEST_REQUIRE_FALSE(log.failed);

    auto options = FrameOptions{};
    options.max_frame_bytes = 1024;  // 单帧上限：一条没有 LF 的 200 KB 流必然越限
    options.max_buffered_bytes = std::size_t{1024} * 1024;  // 缓冲放宽到不会先撞另一枚码
    auto made = Framer::lines(options);
    AURORA_TEST_REQUIRE(made.ok());
    auto &framer = made.value();

    const auto sink = [](const aurora::net::Frame &) -> bool { return true; };
    std::vector<char> scratch(4096);
    std::string code;
    {
        std::scoped_lock lock(log.mutex);
        for (std::size_t round = 0; round < 200 && code.empty(); ++round) {
            const auto chunk = read_round(log.stream, scratch);
            if (chunk.empty()) {
                break;
            }
            const auto fed = framer.feed(std::span<const char>{chunk}, sink);
            if (!fed.ok()) {
                code = std::string{fed.error().code};
            }
        }
    }
    // 超限处不重同步，且报错是终态：这条码来自分帧器而不是对端，故「超大帧」不需要任何真实远端。
    AURORA_TEST_CHECK_STREQ(code, "net-framer-overflow");
    const auto again = framer.feed(std::span<const char>{}, sink);
    AURORA_TEST_CHECK_FALSE(again.ok());
    AURORA_TEST_CHECK_STREQ(std::string{again.error().code}, "net-framer-overflow");
    AURORA_TEST_CHECK_EQ(fake.tasks_run(), 0);  // 全程没用到脚本时钟：字节类故障与时间类故障各归其位
#endif
}

AURORA_TEST_CASE(an_unreachable_proxy_fails_once_and_never_falls_back_to_direct) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no proxy to dial");
#else
    AURORA_TEST_REQUIRE_THREADS();
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    const HostDriven driven;

    ScriptedPeer target{silent()};
    AURORA_TEST_REQUIRE(target.valid());
    const std::uint16_t absent = aurora::test::dead_port();
    AURORA_TEST_REQUIRE_NE(absent, std::uint16_t{0});

    const auto config = ProxyConfig::http_connect("127.0.0.1", absent, {});
    AURORA_TEST_REQUIRE(config.ok());

    ConnectLog log;
    StreamTransport::connect_via(config.value(), "direct-should-not-happen.example", target.port(),
                                 scripted_options(AURORA_SCRIPTED_TIMEOUT), Cancellation{}, connect_into(log));
    // 端口上根本没有服务：拒连是内核给的事实，脚本时钟只用来兜住「这台机器把 SYN 丢掉」的分支。
    (void)drive_until("first-outcome", [&log]() -> bool { return calls_of(log) >= 1; }, std::chrono::milliseconds{300});
    (void)fake.advance(AURORA_SCRIPTED_TIMEOUT);
    AURORA_TEST_WAIT_UNTIL(drive_until("outcome-lands", [&log]() -> bool { return calls_of(log) >= 1; }));

    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    const auto code = code_of(log);
    AURORA_TEST_CHECK_TRUE(code == "net-connect-failed" || code == "net-connect-timeout");
    // 「不回退直连」用目标侧的 accept 计数证明：一条都没来，比「没收到数据」强。
    pump_idle(AURORA_GRACE_WINDOW);
    AURORA_TEST_CHECK_EQ(target.accepts(), 0);
#endif
}

AURORA_TEST_CASE(a_slow_peer_is_scriptable_and_every_scenario_stays_on_loopback) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no loopback peer");
#else
    AURORA_TEST_REQUIRE_THREADS();
    FakeEngine fake;
    const FakeEngineScope scope{fake};
    const HostDriven driven;

    // 慢连接：真连接、真字节，只是对端按脚本先读后答——HTTP 切片的重试与分段超时全靠这一形态做判据。
    ScriptedPeer peer{reply_after_reading(5, "pong")};
    AURORA_TEST_REQUIRE(peer.valid());

    ConnectLog log;
    StreamTransport::connect(Endpoint::loopback(peer.port()), scripted_options(), Cancellation{}, connect_into(log));
    AURORA_TEST_WAIT_UNTIL(drive_until("connected", [&log]() -> bool { return calls_of(log) >= 1; }));
    AURORA_TEST_REQUIRE_FALSE(log.failed);

    std::vector<char> scratch(4096);
    {
        std::scoped_lock lock(log.mutex);
        write_round(log.stream, "ping!");
        // 对端读满 5 字节才答，所以这一读天然带等待；等待由泵完成，不需要真睡。
        AURORA_TEST_CHECK_EQ(read_round(log.stream, scratch), "pong");
        // 禁网证据：这条连接的 peer 就是 127.0.0.1，本文件的场景没有一条能出局外。
        AURORA_TEST_CHECK(log.stream.is_loopback_peer());
    }
#endif
}

AURORA_TEST_CASE(a_peer_fatal_alert_settles_the_handshake_exactly_once) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no loopback peer");
#else
    if (!aurora::net::detail::tls_backend_available()) {
        AURORA_TEST_SKIP("this build carries no TLS backend, so there is no alert branch to reach");
    }
    AURORA_TEST_REQUIRE_THREADS();
    const HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    const std::size_t baseline = registry.sources().size();

    // 目录里的第七类：对端在 ClientHello 之后回一条**致命告警**（提案 §16 第 17 项留给本切片的那枚）。
    // 字节是脚本、连接是真的（回环），故禁网 CI 零出站也能命中该分支。这条记录是未加密的 alert 记录：
    // 型 21 = alert、版本 3.3、长度 2、级别 2 = fatal、描述 40 = handshake_failure。
    const std::string alert{"\x15\x03\x03\x00\x02\x02\x28", 7};
    ScriptedPeer peer{reply_after_reading(5, alert)};
    AURORA_TEST_REQUIRE(peer.valid());

    ConnectLog plain;
    StreamTransport::connect(Endpoint::loopback(peer.port()), scripted_options(), Cancellation{}, connect_into(plain));
    AURORA_TEST_WAIT_UNTIL(drive_until("connected", [&plain]() -> bool { return calls_of(plain) >= 1; }));
    AURORA_TEST_REQUIRE_EQ(calls_of(plain), 1);
    AURORA_TEST_REQUIRE_FALSE(plain.failed);

    aurora::net::TlsContext context;
    struct Verdict {
        std::atomic<int> calls{0};
        bool opened = false;
        std::string code;
        std::string detail;  ///< 失败原因文本（红时靠它指认阶段）
    } verdict;
    // 服务器名只作 SNI 输入：本对端不会解析它，也不会有证书可校验。
    aurora::net::TlsTransport::handshake(std::move(plain.stream), "alert.invalid", context, Cancellation{},
                                         [&verdict](auto result) {
                                             verdict.opened = result.ok();
                                             if (!result) {
                                                 verdict.code = std::string{result.error().code};
                                                 verdict.detail = result.error().message;
                                             }
                                             ++verdict.calls;
                                         });
    AURORA_TEST_WAIT_UNTIL(
        drive_until("handshake-verdict", [&verdict]() -> bool { return verdict.calls.load() >= 1; }));

    AURORA_TEST_CHECK_EQ(verdict.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(verdict.opened);
    // 三条腿都把对端致命告警归到这一枚码（本目录没有 `net-tls-alert-received` 这种码，见 ERROR_CATALOG 的
    // net-tls 段），成因写在 message 里。
    AURORA_TEST_CHECK_EQ(verdict.code, std::string{"net-tls-handshake-failed"});
    // 恰一次 + 句柄归还：告警之后不再有第二种结局，该连接的事件源也必须交回注册口。
    pump_idle(AURORA_GRACE_WINDOW);
    AURORA_TEST_CHECK_EQ(verdict.calls.load(), 1);
    AURORA_TEST_CHECK_EQ(registry.sources().size(), baseline);
#endif
}

}  // namespace aurora::test_cases::utest_net_test_double
