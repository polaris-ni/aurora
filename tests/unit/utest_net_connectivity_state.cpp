/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/connectivity_state.h 与 src/aurora/net/detail/connectivity_state.cpp
/// 测试说明: 链路态判据层（`aurora::net::detail::ConnectivityMachine`）：注入时钟 + 内部读数 +
///           正证 / 失败 / 陈旧翻转，全部逐值断言。不依赖引擎、不触定时器、不读 OS 状态，
///           故三平台行为一致、wasm 腿同样可跑，无需 `AURORA_TEST_SKIP`；平台采集腿的真实读数
///           由 `connectivity_probe.cpp` 提供，本文件用 `optional<bool>` 直接驱动状态机。
#include <chrono>
#include <cstdint>
#include <optional>

#include "aurora/core/error_codes.h"
#include "aurora/net/connectivity.h"
#include "aurora/net/detail/connectivity_state.h"
#include "framework/aurora_test.h"

using aurora::net::ConnectivityChangeCause;
using aurora::net::ConnectivityOptions;
using aurora::net::ConnectivitySource;
using aurora::net::NetworkState;
using aurora::net::detail::ConnectivityMachine;

namespace {

/// @brief 受控时钟：时间由用例显式推进，状态机据此算 `since` / `last_evidence_at` / 陈旧位。
struct ManualClock {
    std::chrono::steady_clock::time_point t;
    auto advance(std::chrono::milliseconds d) -> void { t += d; }
    [[nodiscard]] auto now() const -> std::chrono::steady_clock::time_point { return t; }
};

/// @brief 用受控时钟 + 给定旋钮造一枚状态机（source 只作令牌，不影响判据逻辑）。
[[nodiscard]] auto make_machine(ManualClock &clock, ConnectivityOptions options = ConnectivityOptions{
                                                        .sample_interval = std::chrono::milliseconds{5000},
                                                        .offline_evidence = 1U,
                                                        .online_evidence = 2U}) -> ConnectivityMachine {
    return ConnectivityMachine{ConnectivityMachine::Config{
        .source = ConnectivitySource::LinkFiles, .options = options, .now = [&clock]() { return clock.now(); }}};
}

}  // namespace

AURORA_TEST_CASE(connectivity_state_never_sampled_is_unknown_with_stale) {
    ManualClock clock;
    auto machine = make_machine(clock);
    const auto fact = machine.fact();
    AURORA_TEST_CHECK_EQ(static_cast<int>(fact.state), static_cast<int>(NetworkState::Unknown));
    AURORA_TEST_CHECK_EQ(static_cast<int>(fact.source), static_cast<int>(ConnectivitySource::LinkFiles));
    AURORA_TEST_CHECK_TRUE(fact.stale);
    AURORA_TEST_CHECK_EQ(fact.generation, 0U);
    // 从未采样：`since` 取 monitor 建立时刻（此处即构造时），不是 epoch 零点之外的随机值。
    AURORA_TEST_CHECK(fact.since == std::chrono::steady_clock::time_point{});
}

AURORA_TEST_CASE(connectivity_state_single_offline_probe_transitions_to_offline) {
    ManualClock clock;
    auto machine = make_machine(clock);  // offline_evidence 缺省 1
    const auto step = machine.apply_probe(std::optional<bool>{false}, ConnectivityChangeCause::PeriodicSample);
    AURORA_TEST_CHECK(step.change.has_value());
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Offline));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);
    // 明确离线不应清零展示字段之外的计数以外内容；`since` 落在转换时刻。
    AURORA_TEST_CHECK(machine.fact().since == clock.t);
}

AURORA_TEST_CASE(connectivity_state_unknown_to_online_with_one_positive_probe) {
    ManualClock clock;
    auto machine = make_machine(clock);
    const auto step = machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);
    AURORA_TEST_CHECK(step.change.has_value());
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);
    AURORA_TEST_CHECK_FALSE(machine.fact().stale);
    AURORA_TEST_CHECK(machine.fact().since == clock.t);
}

AURORA_TEST_CASE(connectivity_state_offline_to_online_needs_online_evidence_positives) {
    // online_evidence = 2：连续两次一致正证才转 Online；计票未到时只排一次采样。
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{false}, ConnectivityChangeCause::PeriodicSample);  // → Offline
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Offline));

    const auto first = machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);
    AURORA_TEST_CHECK_FALSE(first.change.has_value());  // 仍在 Offline，未到阈值
    AURORA_TEST_CHECK_TRUE(first.want_sample);  // 计票后仍未到 ⇒ 再采一次求第二枚一致读数
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Offline));

    const auto second = machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);
    AURORA_TEST_CHECK(second.change.has_value());
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 2U);
}

AURORA_TEST_CASE(connectivity_state_offline_to_online_one_probe_one_connect_success) {
    // 三种组合之一：一次采样在线 + 一次成功建连，合计 online_evidence 枚一致正证。
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{false}, ConnectivityChangeCause::PeriodicSample);  // → Offline
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // count 1, still Offline

    const auto step = machine.note_connect_success(false);  // 非回环成功建连：第二枚正证 ⇒ Online
    AURORA_TEST_CHECK(step.change.has_value());
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 2U);
}

AURORA_TEST_CASE(connectivity_state_unknown_to_online_with_one_connect_success) {
    // `Unknown→Online` 一枚正证即可（验收 3）：从 Unknown 一次非回环成功建连即转 Online。
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    const auto step = machine.note_connect_success(false);  // 非回环成功建连 ⇒ 第一枚正证即转 Online
    AURORA_TEST_CHECK(step.change.has_value());
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);
    // 计票后已在 Online：再成功建连不新增转换、不采样。
    const auto again = machine.note_connect_success(false);
    AURORA_TEST_CHECK_FALSE(again.change.has_value());
    AURORA_TEST_CHECK_FALSE(again.want_sample);
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);
}

AURORA_TEST_CASE(connectivity_state_two_connect_success_from_offline) {
    // 「两次成功建连」这一组合归 `Offline→Online`（online_evidence 缺省 2）：从 Offline 起算，
    // 避免插队采样的在线票干扰（一次离线探测只置 Offline、不贡献在线计数）。
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{false}, ConnectivityChangeCause::PeriodicSample);  // → Offline
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Offline));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);

    machine.note_connect_success(false);  // count 1, Offline→未到
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Offline));
    machine.note_connect_success(false);  // count 2 ⇒ Online
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 2U);
}

AURORA_TEST_CASE(connectivity_state_loopback_success_does_not_count) {
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    const auto step = machine.note_connect_success(true);  // 回环对端
    AURORA_TEST_CHECK_FALSE(step.change.has_value());
    AURORA_TEST_CHECK_FALSE(step.want_sample);
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Unknown));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 0U);
}

AURORA_TEST_CASE(connectivity_state_already_online_positive_does_not_resample) {
    ManualClock clock;
    auto machine = make_machine(clock);
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // → Online
    const auto step = machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);
    AURORA_TEST_CHECK_FALSE(step.change.has_value());
    AURORA_TEST_CHECK_FALSE(step.want_sample);  // 已在 Online：不发布、不插队
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);
}

AURORA_TEST_CASE(connectivity_state_read_failure_does_not_count_or_transition) {
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    const auto step = machine.apply_probe(std::nullopt, ConnectivityChangeCause::PeriodicSample);
    AURORA_TEST_CHECK_FALSE(step.change.has_value());
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Unknown));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 0U);
    // 读不到之后一枚正证仍按「Unknown→Online 一枚即可」立即转（证明前面没把计数污染成在线）。
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);
}

AURORA_TEST_CASE(connectivity_state_reverse_evidence_clears_other_count) {
    // 经状态转换清零两侧：Online 之后需要 online_evidence 枚一致正证才回 Online，证明 offline 路径
    // 没有把曾经的 online_count 留下来。
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 2U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{false}, ConnectivityChangeCause::PeriodicSample);  // count1
    machine.apply_probe(std::optional<bool>{false}, ConnectivityChangeCause::PeriodicSample);  // count2 ⇒ Offline
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Offline));

    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // online count1
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Offline));
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // online count2 ⇒ Online
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 2U);
}

AURORA_TEST_CASE(connectivity_state_failure_timeout_only_triggers_sample_and_last_failure) {
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // → Online
    const auto before = machine.fact();

    const auto step = machine.note_connect_failure(aurora::ErrorCode::HttpTimeout);
    AURORA_TEST_CHECK_FALSE(step.change.has_value());  // 超时只触发采样，不判决
    AURORA_TEST_CHECK_TRUE(step.want_sample);
    const auto after = machine.fact();
    AURORA_TEST_CHECK_EQ(static_cast<int>(after.state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(after.generation, before.generation);  // 不推进代际
    AURORA_TEST_CHECK(after.last_failure.has_value());
    AURORA_TEST_CHECK_EQ(*after.last_failure, std::string{"http-timeout"});
}

AURORA_TEST_CASE(connectivity_state_failure_cancelled_is_noop) {
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // → Online
    const auto before = machine.fact();

    const auto step = machine.note_connect_failure(aurora::ErrorCode::NetCancelled);
    AURORA_TEST_CHECK_FALSE(step.change.has_value());
    AURORA_TEST_CHECK_FALSE(step.want_sample);  // 取消既不采样也不写 last_failure
    const auto after = machine.fact();
    AURORA_TEST_CHECK_EQ(after.generation, before.generation);
    AURORA_TEST_CHECK_FALSE(after.last_failure.has_value());
}

AURORA_TEST_CASE(connectivity_state_connect_failure_never_counts_negative) {
    // 变异自证支撑：把连接失败计为链路负证 ⇒ 「ECONNREFUSED 不得产生离线」转红。
    // 这里正向断言：NetConnectFailed（= ECONNREFUSED / ENETUNREACH）只采样、不转 Offline。
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // → Online
    machine.note_connect_failure(aurora::ErrorCode::NetConnectFailed);
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);
}

AURORA_TEST_CASE(connectivity_state_stale_flips_once_after_threshold) {
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{100}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // → Online, has_evidence
    AURORA_TEST_CHECK_FALSE(machine.fact().stale);

    clock.advance(std::chrono::milliseconds{301});  // > 3×100ms
    const auto first = machine.mark_stale_if_due();
    AURORA_TEST_CHECK(first.change.has_value());
    if (!first.change.has_value()) {
        return;  // 上一断言已固定为真；显式收窄供静态分析追踪
    }
    AURORA_TEST_CHECK_EQ(static_cast<int>(first.change->cause),
                         static_cast<int>(ConnectivityChangeCause::StalenessMarked));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 2U);
    AURORA_TEST_CHECK_TRUE(machine.fact().stale);

    // 已发布：再次调用不再翻转（false→true 恰好一次）。
    const auto second = machine.mark_stale_if_due();
    AURORA_TEST_CHECK_FALSE(second.change.has_value());
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 2U);
}

AURORA_TEST_CASE(connectivity_state_options_clamp_out_of_range) {
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.set_options(ConnectivityOptions{
        .sample_interval = std::chrono::milliseconds{10}, .offline_evidence = 0U, .online_evidence = 99U});  // 越界
    const auto options = machine.options();
    AURORA_TEST_CHECK_EQ(options.sample_interval, std::chrono::milliseconds{100});  // 下界
    AURORA_TEST_CHECK_EQ(options.offline_evidence, 1U);  // 下界
    AURORA_TEST_CHECK_EQ(options.online_evidence, 16U);  // 上界
}

AURORA_TEST_CASE(connectivity_state_set_options_does_not_reset_counts_or_transition) {
    ManualClock clock;
    auto machine = make_machine(
        clock, ConnectivityOptions{
                   .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U});
    machine.apply_probe(std::optional<bool>{true}, ConnectivityChangeCause::PeriodicSample);  // → Online
    machine.set_options(ConnectivityOptions{
        .sample_interval = std::chrono::milliseconds{100}, .offline_evidence = 1U, .online_evidence = 2U});
    AURORA_TEST_CHECK_EQ(static_cast<int>(machine.fact().state), static_cast<int>(NetworkState::Online));
    AURORA_TEST_CHECK_EQ(machine.fact().generation, 1U);  // 不立即转换、不重置在途计数
}
