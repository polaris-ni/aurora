/// 测试类型: unit
/// 目标单元: tests/framework/async_wait.h
/// 测试说明: 条件等待辅助自身的三条判据口径——**无忙等**（`Watch<T>::wait_for` 由条件变量唤醒，
///           `polls` 恒为 0；`wait_until` 是带量化步长的有限轮询，轮数受预算约束）、**超时自报**
///           （结果文本给出主题、实测耗时、预算与轮数，红时一眼分辨「等早了」还是「根本没发生」）、
///           **不吞断言**（等待失败只记这一条，同用例的后续观测照常执行——现场往往比结论更有用）。
///           另测 `Watch<T>` 的写入可见性与并发累加、`CountWatch` 的合并计数，以及谓词已成立时
///           零睡眠即返回。
/// ⚠️ 需要线程的用例先走 `AURORA_TEST_REQUIRE_THREADS()`：无 pthreads 的腿（wasm 单线程）里这些
///    判据没有对应物，框架宏会把用例记为跳过而不是假过。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "framework/async_wait.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_framework_async_wait {
namespace {

using aurora::test::AURORA_WAIT_BUDGET;
using aurora::test::CountWatch;
using aurora::test::wait_until;
using aurora::test::WaitOutcome;
using aurora::test::Watch;

/// @brief 观察窗：明显短于缺省预算，用来证明「命中即返回」而不是等满预算。
constexpr std::chrono::milliseconds AURORA_OBSERVE{120};

/// @brief 跨线程置位的小延时：大到能盖住一次谓词求值，小到不拖慢整套用例。
constexpr std::chrono::milliseconds AURORA_LATE_SET{20};

}  // namespace

AURORA_TEST_CASE(already_true_predicate_returns_without_sleeping) {
    int checked = 0;
    const auto outcome = wait_until(
        "already-true",
        [&checked]() -> bool {
            ++checked;
            return true;
        },
        AURORA_OBSERVE);
    AURORA_TEST_CHECK(outcome.satisfied);
    // 先判后睡：谓词第一次就成立时不该产生任何睡眠，否则「命中即返回」会被预算拖平。
    AURORA_TEST_CHECK_EQ(outcome.polls, 1);
    AURORA_TEST_CHECK_EQ(checked, 1);
    AURORA_TEST_CHECK_LT(outcome.elapsed, AURORA_OBSERVE);
}

AURORA_TEST_CASE(timed_out_wait_reports_budget_and_rounds) {
    const auto outcome = wait_until("never-true", []() -> bool { return false; }, AURORA_OBSERVE);
    AURORA_TEST_CHECK_FALSE(outcome.satisfied);
    AURORA_TEST_CHECK_GE(outcome.elapsed, AURORA_OBSERVE);
    // 轮数受预算约束：量化步长 1 ms，故轮数上界是预算毫秒数加一点调度余量。这同时是「无忙等」的
    // 可断言形态——忙等的轮数由 CPU 决定，会远超这个上界。
    const auto ceiling = static_cast<int>((AURORA_OBSERVE.count() * 4) + 50);
    AURORA_TEST_CHECK_LE(outcome.polls, ceiling);
    AURORA_TEST_CHECK_GT(outcome.polls, 1);

    const auto text = outcome.report();
    AURORA_TEST_CHECK(text.find("never-true") != std::string::npos);
    AURORA_TEST_CHECK(text.find("UNSATISFIED") != std::string::npos);
    AURORA_TEST_CHECK(text.find("budget=120ms") != std::string::npos);
    AURORA_TEST_CHECK(text.find("polls=") != std::string::npos);

    // 通过的形态同样自报，但说 ok 而不说 UNSATISFIED。
    const auto ok = wait_until("fine", []() -> bool { return true; }, AURORA_OBSERVE);
    AURORA_TEST_CHECK(ok.report().find("] ok: elapsed=") != std::string::npos);
}

AURORA_TEST_CASE(timed_out_wait_does_not_swallow_later_observations) {
    // 等待失败之后，同一次用例里「现场为什么没变成那样」的观测才是定位线索：本头刻意把断言做成
    // 非致命（`CHECK` 级），这里用「超时 + 后续观测仍可得」证明它不吞。
    Watch<int> state{0};
    const auto outcome =
        state.wait_for("stays-zero", [](const int &value) -> bool { return value == 7; }, AURORA_OBSERVE);
    AURORA_TEST_CHECK_FALSE(outcome.satisfied);
    // 通知驱动形态一条都不轮询：这是与 `wait_until` 的关键差别，也是「无忙等」的直接证据。
    AURORA_TEST_CHECK_EQ(outcome.polls, 0);
    // 后续观测照常可得（若等待是致命断言，这两行根本不会被执行）。
    AURORA_TEST_CHECK_EQ(state.get(), 0);
    AURORA_TEST_CHECK_EQ(state.writes(), 0);
}

AURORA_TEST_CASE(watch_wakes_the_waiter_without_polling) {
    AURORA_TEST_REQUIRE_THREADS();
    Watch<std::string> status{"idle"};
    std::thread producer([&status]() -> void {
        std::this_thread::sleep_for(AURORA_LATE_SET);
        status.set("done");
    });
    const auto outcome = status.wait_for_value("reach-done", std::string{"done"}, AURORA_WAIT_BUDGET);
    producer.join();

    AURORA_TEST_CHECK(outcome.satisfied);
    AURORA_TEST_CHECK_EQ(outcome.polls, 0);
    // 命中即醒：等到的时间应当远小于预算，否则说明这条路径也在按预算轮询。
    AURORA_TEST_CHECK_LT(outcome.elapsed, AURORA_WAIT_BUDGET / 2);
    AURORA_TEST_CHECK_EQ(status.get(), "done");
    AURORA_TEST_CHECK_EQ(status.writes(), 1);
}

AURORA_TEST_CASE(watch_predicate_sees_full_snapshots_only) {
    AURORA_TEST_REQUIRE_THREADS();
    Watch<std::size_t> counter{0};
    // 谓词在锁内求值，故它看到的永远是完整快照：这里等「至少 3」，而写入方每次 +1。
    std::vector<std::thread> producers;
    producers.reserve(4);
    for (std::size_t i = 0; i < 4; ++i) {
        producers.emplace_back([&counter]() -> void { counter.update([](std::size_t &value) -> void { ++value; }); });
    }
    for (auto &producer : producers) {
        producer.join();
    }
    const auto outcome =
        counter.wait_for("at-least-three", [](const std::size_t &value) -> bool { return value >= 3; }, AURORA_OBSERVE);
    AURORA_TEST_CHECK(outcome.satisfied);
    AURORA_TEST_CHECK_EQ(counter.get(), std::size_t{4});
    AURORA_TEST_CHECK_EQ(counter.writes(), 4);
}

AURORA_TEST_CASE(count_watch_merges_concurrent_bumps_exactly) {
    AURORA_TEST_REQUIRE_THREADS();
    CountWatch events{0};
    constexpr int threads = 6;
    constexpr int per_thread = 200;
    const auto total = static_cast<std::size_t>(threads) * static_cast<std::size_t>(per_thread);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&events]() -> void {
            for (int n = 0; n < per_thread; ++n) {
                events.update([](std::size_t &value) -> void { ++value; });
            }
        });
    }
    const auto wait =
        events.wait_for("all-bumps-landed", [total](const std::size_t &value) -> bool { return value >= total; });
    for (auto &worker : workers) {
        worker.join();
    }
    // 一次都不丢也不重：等待与写入共用同一把锁，累加没有竞态窗口。
    AURORA_TEST_CHECK(wait.satisfied);
    AURORA_TEST_CHECK_EQ(events.get(), total);
}

AURORA_TEST_CASE(wait_outcome_defaults_are_the_unwaited_state) {
    const WaitOutcome fresh{};
    AURORA_TEST_CHECK_FALSE(fresh.satisfied);
    AURORA_TEST_CHECK_EQ(fresh.polls, 0);
    AURORA_TEST_CHECK_EQ(fresh.elapsed.count(), 0);
    AURORA_TEST_CHECK(fresh.subject.empty());
}

AURORA_TEST_CASE(wait_macro_passes_a_satisfied_outcome_without_reporting) {
    // 宏层自身的判据：成立时不产生任何失败记录（红绿只由 `satisfied` 决定），现场文本随结果给出。
    const auto outcome = wait_until("macro-path", []() -> bool { return true; }, AURORA_OBSERVE);
    AURORA_TEST_WAIT_UNTIL(outcome);
    AURORA_TEST_CHECK_EQ(outcome.polls, 1);
}

}  // namespace aurora::test_cases::utest_framework_async_wait
