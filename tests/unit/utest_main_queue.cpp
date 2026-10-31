/// 测试类型: unit
/// 目标单元: include/aurora/core/main_queue.h
/// 测试说明: MainQueue 的队列与值班不变量——独占上岗（并发领取拒绝）、租约转移与幂等卸任、
/// 世代单调不复用（重入 run 拒旧世投递）、接受判据（同世放行/代际无关放行/打烊双拒）、
/// 收尾时序（先还后捞兜住 release 前入队项）、打烊窗口有界（closing_scope 开关与 is_closing）、
/// try_post 纯判据不触发失败处理器、drop 计数可观测、多线程并发投递恰一执行。
/// 发起侧三分（硬失败/出生即取消）依赖 state/async.h 接线，见 utest_async。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/main_queue.h"
#include "aurora/core/strict_mode.h"  // 判据「拒绝不惊动失败处理器」的注入点
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_main_queue {

using aurora::testing::require_ref;

namespace {

/// @brief 用例内 RAII 清场：无论断言走到哪里，退出时队列排空、不留在岗租约。
struct QueueGuard {
    QueueGuard() = default;
    QueueGuard(const QueueGuard &) = delete;
    auto operator=(const QueueGuard &) -> QueueGuard & = delete;
    QueueGuard(QueueGuard &&) = delete;
    auto operator=(QueueGuard &&) -> QueueGuard & = delete;
    // 排空会执行已入队闭包（可能抛），与 `Application::loop_end` 同口径：收尾路径不外抛。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~QueueGuard() {
        auto batch = MainQueue::take_batch();  // 排空残留（含被捞后未跑的任务）
        (void)batch;
    }
};

}  // namespace

AURORA_TEST_CASE(duty_is_exclusive_and_reusable_after_release) {
    QueueGuard guard;
    auto first = MainQueue::on_duty([]() -> void {});
    auto &first_lease = require_ref(first);
    // CAS：此刻已有人 → 第二家领取被拒。
    auto second = MainQueue::on_duty([]() -> void {});
    AURORA_TEST_CHECK_FALSE(second.has_value());

    const std::uint64_t epoch1 = MainQueue::current_epoch();
    AURORA_TEST_CHECK_NE(epoch1, 0U);
    first_lease.release();
    AURORA_TEST_CHECK_EQ(MainQueue::current_epoch(), 0U);  // 归还即不在岗

    // 重入循环复用：release 后槽位可再次领取，且世代号严格更大（永不复用）。
    auto third = MainQueue::on_duty([]() -> void {});
    auto &third_lease = require_ref(third);
    const std::uint64_t epoch3 = MainQueue::current_epoch();
    AURORA_TEST_CHECK_GT(epoch3, epoch1);
    third_lease.release();
}

AURORA_TEST_CASE(lease_moves_and_release_is_idempotent) {
    QueueGuard guard;
    auto lease = MainQueue::on_duty([]() -> void {});
    auto &held = require_ref(lease);
    const std::uint64_t epoch = MainQueue::current_epoch();

    // 转移凭证：源即刻失去卸任资格（凭证世代号归零，结构性防双份冒卸），接手方成为唯一持有人。
    auto moved = std::move(held);
    AURORA_TEST_CHECK_EQ(MainQueue::current_epoch(), epoch);

    moved.release();  // 真持有人卸任
    AURORA_TEST_CHECK_EQ(MainQueue::current_epoch(), 0U);
    moved.release();  // 幂等：二次卸任 no-op
    AURORA_TEST_CHECK_EQ(MainQueue::current_epoch(), 0U);
}

AURORA_TEST_CASE(post_acceptance_matrix) {
    QueueGuard guard;
    // 落地相的一切拒绝（无值班 / 旧世）只记诊断与计数，绝不惊动进程级失败处理器：
    // 处理器抛出即把「静默失败」的缺席变成可断言的失败。
    auto handler_hits = std::make_shared<std::atomic<int>>(0);
    aurora::set_strict_failure_handler([handler_hits](std::string_view) -> void {
        handler_hits->fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error{"landing-phase rejection reached the failure handler"};
    });
    auto rejected = std::make_shared<std::atomic<bool>>(false);
    const std::uint64_t before = MainQueue::dropped_count();

    // 不在岗：一切投递拒绝（含代际无关）；计数只属 `post` 路径（`try_post` 边界降级不记诊断）。
    AURORA_TEST_CHECK_FALSE(MainQueue::try_post([rejected]() -> void { rejected->store(true); }));
    MainQueue::post([rejected]() -> void { rejected->store(true); });  // 打烊取消形态，不抛出
    AURORA_TEST_CHECK_FALSE(rejected->load());
    AURORA_TEST_CHECK_EQ(MainQueue::dropped_count(), before + 1);

    // 在岗：同世放行、代际无关放行、旧世拒绝。
    auto lease = MainQueue::on_duty([]() -> void {});
    auto &held = require_ref(lease);
    const std::uint64_t now = MainQueue::current_epoch();
    auto ran_wild = std::make_shared<std::atomic<bool>>(false);
    auto ran_same = std::make_shared<std::atomic<bool>>(false);
    MainQueue::post([ran_wild]() -> void { ran_wild->store(true); }, 0);
    MainQueue::post([ran_same]() -> void { ran_same->store(true); }, now);
    AURORA_TEST_CHECK_TRUE(MainQueue::try_post([]() -> void {}, now));
    // 伪造旧世：先领一代再卸，换新一代后旧世代号必失配。
    held.release();
    auto next = MainQueue::on_duty([]() -> void {});
    auto &next_held = require_ref(next);
    auto stale = std::make_shared<std::atomic<bool>>(false);
    MainQueue::post([stale]() -> void { stale->store(true); }, now);  // 上一世在途任务
    AURORA_TEST_CHECK_FALSE(MainQueue::try_post([]() -> void {}, now));

    // 放行 ≠ 执行：执行属于 take_batch 的调用线程。
    auto batch = MainQueue::take_batch();
    AURORA_TEST_CHECK_EQ(batch.size(), std::size_t{3});  // wild + same + try_post(now) 的空体
    for (auto &task : batch) {
        task();
    }
    AURORA_TEST_CHECK_TRUE(ran_wild->load());
    AURORA_TEST_CHECK_TRUE(ran_same->load());
    AURORA_TEST_CHECK_FALSE(stale->load());
    AURORA_TEST_CHECK_EQ(MainQueue::dropped_count(), before + 2);  // 旧世 post 被拒并计数；旧世 try_post 拒而不计
    next_held.release();

    // 判定先行复位：进程级处理器勿留给后续用例；走到这里即证明上面的拒绝从未触发它。
    aurora::set_strict_failure_handler(nullptr);
    AURORA_TEST_CHECK_EQ(handler_hits->load(std::memory_order_relaxed), 0);
}

AURORA_TEST_CASE(close_race_release_then_drain_catches_all) {
    QueueGuard guard;
    auto woke = std::make_shared<std::atomic<int>>(0);
    auto lease = MainQueue::on_duty([woke]() -> void { woke->fetch_add(1, std::memory_order_relaxed); });
    auto &held = require_ref(lease);

    auto ran = std::make_shared<std::atomic<bool>>(false);
    MainQueue::post([ran]() -> void { ran->store(true, std::memory_order_release); });
    AURORA_TEST_CHECK_GT(woke->load(), 0);  // 入队唤醒过值班者

    // 时序纪律：先 release、再收尾 take_batch——release 前入队项必被兜住。
    held.release();
    auto batch = MainQueue::take_batch();
    AURORA_TEST_REQUIRE_EQ(batch.size(), std::size_t{1});
    for (auto &task : batch) {
        task();
    }
    AURORA_TEST_CHECK_TRUE(ran->load(std::memory_order_acquire));

    // release 之后的投递只可能走拒绝路径（无孤儿）。
    const std::uint64_t before = MainQueue::dropped_count();
    MainQueue::post([ran]() -> void { ran->store(false); });
    AURORA_TEST_CHECK_EQ(MainQueue::dropped_count(), before + 1);
    AURORA_TEST_CHECK_TRUE(ran->load(std::memory_order_acquire));  // 拒绝 = 任务体绝不执行
}

AURORA_TEST_CASE(closing_scope_is_bounded_flag) {
    QueueGuard guard;
    AURORA_TEST_CHECK_FALSE(MainQueue::is_closing());
    {
        auto window = MainQueue::closing_scope();
        AURORA_TEST_CHECK_TRUE(MainQueue::is_closing());
        // 打烊窗口内投递照拒（epoch==0 已成立）；is_closing 只影响发起侧形态。
        AURORA_TEST_CHECK_EQ(MainQueue::current_epoch(), 0U);
        AURORA_TEST_CHECK_FALSE(MainQueue::try_post([]() -> void {}));
    }
    AURORA_TEST_CHECK_FALSE(MainQueue::is_closing());  // 守卫析构即关窗（有界）

    // 在岗时 closing 不成立：两标志独立。
    auto lease = MainQueue::on_duty([]() -> void {});
    auto &held = require_ref(lease);
    AURORA_TEST_CHECK_FALSE(MainQueue::is_closing());
    held.release();
}

AURORA_TEST_CASE(has_on_duty_iff_epoch_nonzero) {
    QueueGuard guard;
    AURORA_TEST_CHECK_FALSE(MainQueue::has_on_duty());
    AURORA_TEST_CHECK_EQ(MainQueue::current_epoch(), 0U);
    auto lease = MainQueue::on_duty([]() -> void {});
    auto &held = require_ref(lease);
    AURORA_TEST_CHECK_TRUE(MainQueue::has_on_duty());
    AURORA_TEST_CHECK_NE(MainQueue::current_epoch(), 0U);
    held.release();
    AURORA_TEST_CHECK_FALSE(MainQueue::has_on_duty());
}

AURORA_TEST_CASE(concurrent_producers_exactly_once) {
    AURORA_TEST_REQUIRE_THREADS();
    QueueGuard guard;
    auto lease = MainQueue::on_duty([]() -> void {});
    auto &held = require_ref(lease);

    constexpr int producers = 4;
    constexpr int per_producer = 200;
    auto counter = std::make_shared<std::atomic<int>>(0);
    std::vector<std::thread> threads;
    threads.reserve(producers);
    for (int t = 0; t < producers; ++t) {
        threads.emplace_back([counter]() -> void {
            for (int i = 0; i < per_producer; ++i) {
                MainQueue::post([counter]() -> void { counter->fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }

    int seen = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (seen < producers * per_producer && std::chrono::steady_clock::now() < deadline) {
        for (auto &task : MainQueue::take_batch()) {
            task();
            ++seen;
        }
    }
    AURORA_TEST_CHECK_EQ(seen, producers * per_producer);
    AURORA_TEST_CHECK_EQ(counter->load(), producers * per_producer);  // 恰一执行，无重无漏
    held.release();
}

}  // namespace aurora::test_cases::utest_main_queue
