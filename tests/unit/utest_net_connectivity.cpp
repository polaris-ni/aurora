/// 测试类型: unit
/// 目标单元: include/aurora/net/connectivity.h 与其实现
/// 测试说明: 链路态公共面——快照首态、订阅交付、插队合并（50 次失败只采 1 次）、登记失败清待采样位、
///           注册句柄三条契约、停机后快照可读。定时器经 `FakeEngine` 脚本化推进（零实时睡眠），
///           读数经库内探针钩子注入（零出站 I/O）；钩子与进程级运行时在每个用例内成对拆装，
///           避免用例间相互污染。
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/net/connectivity.h"
#include "aurora/net/detail/connectivity_probe.h"
#include "aurora/net/detail/connectivity_state.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"

using aurora::net::ConnectivityChange;
using aurora::net::ConnectivityMonitor;
using aurora::net::ConnectivityOptions;
using aurora::net::ConnectivityRegistration;
using aurora::net::ConnectivitySource;
using aurora::net::NetworkState;
using aurora::net::detail::compiled_source;
using aurora::net::detail::note_connection_outcome;
using aurora::net::detail::ProbeReading;
using aurora::net::detail::reset_connectivity_for_test;
using aurora::net::detail::set_connectivity_probe_hook;

namespace {

/// @brief 受控探针钩子：返回固定读数并计调用次数（无出站 I/O）。
struct ScriptedProbe {
    std::optional<bool> online;  // nullopt = 读不到
    int calls = 0;
    auto fn() -> ProbeReading {
        ++calls;
        return ProbeReading{.online = online};
    }
};

}  // namespace

AURORA_TEST_CASE(connectivity_facade_snapshot_never_sampled_is_unknown) {
    reset_connectivity_for_test();
    set_connectivity_probe_hook({});
    const auto fact = ConnectivityMonitor::instance().snapshot();
    AURORA_TEST_CHECK_EQ(static_cast<int>(fact.state), static_cast<int>(NetworkState::Unknown));
    // `source` 反映本构建的编译期采集腿令牌：有腿平台为 `IpHelper` / `LinkFiles`，无腿（wasm / macOS 腿未落地）
    // 平台为 `None`——「无腿即 `None`」是提案验收 2 的口径，故断言与 `compiled_source()` 对齐而非恒非空。
    AURORA_TEST_CHECK_EQ(static_cast<int>(fact.source), static_cast<int>(compiled_source()));
    AURORA_TEST_CHECK_TRUE(fact.stale);
    AURORA_TEST_CHECK_EQ(fact.generation, 0U);
    set_connectivity_probe_hook({});
}

AURORA_TEST_CASE(connectivity_facade_subscribe_delivers_online_after_sample) {
    reset_connectivity_for_test();
    set_connectivity_probe_hook({});
    aurora::test::FakeEngine engine;
    const aurora::test::FakeEngineScope scope{engine};

    ScriptedProbe probe;
    probe.online = true;
    set_connectivity_probe_hook([&probe]() -> ProbeReading { return probe.fn(); });

    std::vector<ConnectivityChange> seen;
    auto registration = ConnectivityMonitor::instance().subscribe(
        [&seen](const ConnectivityChange &change) { seen.push_back(change); });
    AURORA_TEST_CHECK(static_cast<bool>(registration));
    AURORA_TEST_CHECK(registration.value().is_registered());
    AURORA_TEST_CHECK_NE(registration.value().token(), 0U);

    // 订阅登记了一枚采样；推进一个采样周期触发它，钩子回 online ⇒ 转 Online，回调恰好一次。
    engine.advance(std::chrono::milliseconds{5000});
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(static_cast<int>(seen.back().current.state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(probe.calls, 1);  // 仅一次真实探测
    set_connectivity_probe_hook({});
}

AURORA_TEST_CASE(connectivity_facade_fifty_failures_merge_to_one_probe) {
    // 验收 7：连投 50 次失败旁证，有效探测调用恰为 1 次、订阅者不因 50 次失败刷 50 条通知。
    reset_connectivity_for_test();
    set_connectivity_probe_hook({});
    aurora::test::FakeEngine engine;
    const aurora::test::FakeEngineScope scope{engine};

    ScriptedProbe probe;
    probe.online = std::nullopt;  // 读不到：不计数、不转换
    set_connectivity_probe_hook([&probe]() -> ProbeReading { return probe.fn(); });

    std::vector<ConnectivityChange> seen;
    auto registration = ConnectivityMonitor::instance().subscribe(
        [&seen](const ConnectivityChange &change) { seen.push_back(change); });
    AURORA_TEST_CHECK(static_cast<bool>(registration));

    for (int i = 0; i < 50; ++i) {
        note_connection_outcome(false, false, aurora::ErrorCode::HttpTimeout);
    }
    engine.advance(std::chrono::milliseconds{5000});

    AURORA_TEST_CHECK_EQ(probe.calls, 1);  // 订阅采样 + 50 失败合并为同一枚待采样轮次 ⇒ 只采 1 次
    AURORA_TEST_CHECK_EQ(seen.size(), 0U);  // 读不到不转换 ⇒ 无通知
    set_connectivity_probe_hook({});
}

AURORA_TEST_CASE(connectivity_facade_failed_registration_clears_pending) {
    // 验收 10：登记一拍失败时待采样位必须清零；否则此后每一拍都合并进一枚不存在的定时器，
    // 链路永久停在首态且再没有第二次尝试的机会。
    reset_connectivity_for_test();
    set_connectivity_probe_hook({});
    aurora::test::FakeEngine engine;
    const aurora::test::FakeEngineScope scope{engine};

    ScriptedProbe probe;
    probe.online = true;
    set_connectivity_probe_hook([&probe]() -> ProbeReading { return probe.fn(); });

    // 拒绝期内的订阅：样本登记失败（`subscribe()` 本身不因此失败），pending 不得残留。
    engine.refuse_posts(true);
    std::vector<ConnectivityChange> seen;
    auto registration = ConnectivityMonitor::instance().subscribe(
        [&seen](const ConnectivityChange &change) { seen.push_back(change); });
    AURORA_TEST_CHECK(static_cast<bool>(registration));

    // 证据拍如实回报错误码，且不因这次失败把 pending 卡死。
    const auto refused = ConnectivityMonitor::instance().refresh();
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::NetPlatformUnavailable);

    // 恢复投递后下一次证据拍必须能重新登记，并在一个周期后真正探测一次。
    engine.refuse_posts(false);
    const auto accepted = ConnectivityMonitor::instance().refresh();
    AURORA_TEST_CHECK(static_cast<bool>(accepted));
    engine.advance(std::chrono::milliseconds{5000});
    AURORA_TEST_CHECK_EQ(probe.calls, 1);
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(static_cast<int>(seen.back().current.state), static_cast<int>(NetworkState::Online));
    set_connectivity_probe_hook({});
}

AURORA_TEST_CASE(connectivity_facade_registration_handle_contracts) {
    reset_connectivity_for_test();
    set_connectivity_probe_hook({});
    aurora::test::FakeEngine engine;
    const aurora::test::FakeEngineScope scope{engine};

    auto reg = ConnectivityMonitor::instance().subscribe([](const ConnectivityChange &) {});
    AURORA_TEST_CHECK(static_cast<bool>(reg));
    auto handle = std::move(reg.value());
    AURORA_TEST_CHECK(handle.is_registered());
    const auto token = handle.token();
    AURORA_TEST_CHECK_NE(token, 0U);

    // 移动后源句柄转为空态、不再二次注销；目标句柄仍生效且令牌一致。
    ConnectivityRegistration moved = std::move(handle);
    AURORA_TEST_CHECK(moved.is_registered());
    AURORA_TEST_CHECK_EQ(moved.token(), token);
    // 故意读 moved-from 源句柄：契约要求它可读且为空态，不属于使用已移动对象。
    // NOLINTNEXTLINE(bugprone-use-after-move, clang-analyzer-cplusplus.Move)
    AURORA_TEST_CHECK_FALSE(handle.is_registered());  // 源 move 后为空态
    AURORA_TEST_CHECK_EQ(handle.token(), 0U);  // 空句柄的令牌为 0（不是把原令牌原样留下）

    // 移动赋值（目标析构旧订阅即注销）后查即假：一条订阅只被注销一次。
    moved = ConnectivityRegistration{};
    AURORA_TEST_CHECK_FALSE(moved.is_registered());
    set_connectivity_probe_hook({});
}

AURORA_TEST_CASE(connectivity_facade_shutdown_keeps_snapshot_readable) {
    reset_connectivity_for_test();
    set_connectivity_probe_hook({});
    aurora::test::FakeEngine engine;
    const aurora::test::FakeEngineScope scope{engine};

    // 停机后快照仍可读、不再转换；越过阈值 `stale` 置真。
    engine.shutdown();
    const auto fact = ConnectivityMonitor::instance().snapshot();
    AURORA_TEST_CHECK_EQ(static_cast<int>(fact.state), static_cast<int>(NetworkState::Unknown));
    AURORA_TEST_CHECK_TRUE(fact.stale);
    set_connectivity_probe_hook({});
}

AURORA_TEST_CASE(connectivity_facade_options_round_trip) {
    reset_connectivity_for_test();
    set_connectivity_probe_hook({});
    const auto defaults = ConnectivityMonitor::defaults();
    AURORA_TEST_CHECK_EQ(defaults.sample_interval, std::chrono::milliseconds{5000});
    AURORA_TEST_CHECK_EQ(defaults.offline_evidence, 1U);
    AURORA_TEST_CHECK_EQ(defaults.online_evidence, 2U);

    ConnectivityMonitor::instance().set_options(ConnectivityOptions{
        .sample_interval = std::chrono::milliseconds{1000}, .offline_evidence = 3U, .online_evidence = 4U});
    const auto applied = ConnectivityMonitor::instance().options();
    AURORA_TEST_CHECK_EQ(applied.sample_interval, std::chrono::milliseconds{1000});
    AURORA_TEST_CHECK_EQ(applied.offline_evidence, 3U);
    AURORA_TEST_CHECK_EQ(applied.online_evidence, 4U);
    set_connectivity_probe_hook({});
}
