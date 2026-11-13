/// @file utest_win32_ua_marshal.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/win32_ua.cpp
/// 测试说明: UIA provider 的主线程回投内核（进程级唯一队列版）。证明「跨线程读/动确实换到
///           主人线程执行」、主人线程就地路径不变、无值班时边界降级为零值且闭包绝不就地执行、
///           超时降级为零值且晚到的队列项被放弃闸拦住、
///           桥析构后仍在队列里的项不再解引用本桥，以及 provider 侧的快照读/导航读都经回投。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "aurora/core/main_queue.h"
#include "aurora/core/platform.h"
#include "aurora/state/async.h"
#include "framework/aurora_test.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))
// 平台可用性开关，供下方 #if 使用，无 constexpr 等价物
// NOLINTNEXTLINE(*-macro-usage)
#define AURORA_WIN32_UA_MARSHAL_AVAILABLE 1
#include "aurora/widget/widget.h"
#include "aurora/window/detail/win32_ua.h"
#else
// 平台可用性开关，供下方 #if 使用，无 constexpr 等价物
// NOLINTNEXTLINE(*-macro-usage)
#define AURORA_WIN32_UA_MARSHAL_AVAILABLE 0
#endif

namespace aurora::test_cases::utest_win32_ua_marshal {

#if AURORA_WIN32_UA_MARSHAL_AVAILABLE

namespace {

/// @brief 最小可投影根：LeafWidget（is_control）让快照有活节点。
class ProbeRoot final : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "ProbeRoot"; }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        return Size{.width = 200.0F, .height = 100.0F};
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

/// @brief 「只入队、由主人线程排水」的值班租约：领一枚空唤醒租约，`eval_on_main`
/// 的回投滞留进程级唯一队列，由用例（主人）线程逐个排空执行。析构还券。
struct DutyLease {
    DutyLease() = default;
    std::optional<MainQueue::Lease> duty{MainQueue::on_duty([]() -> void {})};
    DutyLease(const DutyLease &) = delete;
    auto operator=(const DutyLease &) -> DutyLease & = delete;
    DutyLease(DutyLease &&) = delete;
    auto operator=(DutyLease &&) -> DutyLease & = delete;
    ~DutyLease() {
        if (duty.has_value()) {
            duty->release();
        }
    }

    /// @brief 等到有任务入队（**不**执行）；`timeout_ms` 内没有返回 false。
    auto wait_for_task(int timeout_ms) -> bool {
        for (int i = 0; i < timeout_ms; ++i) {
            if (MainQueue::pending_count() > 0) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return MainQueue::pending_count() > 0;
    }

    /// @brief 在当前线程执行一个排队任务（本函数所在线程即用例的主人线程）；多余项回投队列。
    auto drain_one(int timeout_ms = 5000) -> bool {
        if (!wait_for_task(timeout_ms)) {
            return false;
        }
        auto batch = MainQueue::take_batch();
        const std::function<void()> task = std::move(batch.front());
        for (auto it = batch.begin() + 1; it != batch.end(); ++it) {
            static_cast<void>(MainQueue::try_post(std::move(*it)));
        }
        task();
        return true;
    }

    /// @brief 滞留项数（已入队未执行）。
    [[nodiscard]] auto pending() -> std::size_t { return MainQueue::pending_count(); }
};

}  // namespace

// 目标：非主人线程的回投必须在主人线程执行，结果按值带回调用线程。
AURORA_TEST_CASE(cross_thread_eval_runs_on_the_owner_thread) {
    detail::Win32UiaBridge bridge(nullptr);
    DutyLease lease;
    const std::thread::id owner = std::this_thread::get_id();

    std::thread::id ran_on{};
    int returned = -1;
    std::thread caller([&bridge, &ran_on, &returned]() -> void {
        returned = bridge.eval_on_main<int>(
            [&ran_on]() -> int {
                ran_on = std::this_thread::get_id();
                return 7;
            },
            2000U);
    });
    AURORA_TEST_REQUIRE_TRUE(lease.drain_one(3000));
    caller.join();

    AURORA_TEST_CHECK_TRUE(ran_on == owner);  // 闭包落在主人线程，而非调用线程
    AURORA_TEST_CHECK_EQ(returned, 7);
}

// 目标：主人线程自己调回投口时就地执行、零入队（宿主接线与 `UiaDisconnectProvider` 重入不受代价）。
AURORA_TEST_CASE(owner_thread_eval_runs_inline_without_queuing) {
    detail::Win32UiaBridge bridge(nullptr);
    DutyLease lease;

    int calls = 0;
    const int returned = bridge.eval_on_main<int>(
        [&calls]() -> int {
            ++calls;
            return 3;
        },
        2000U);

    AURORA_TEST_CHECK_EQ(returned, 3);
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_TRUE(lease.pending() == 0U);  // 一次都没入队，故不需要帧循环
}

// 目标：无值班循环（主人线程不在环）→ 契约边界诚实降级：零值 `R{}`（provider 侧映射
// UIA_E_ELEMENTNOTAVAILABLE），闭包绝不就地执行——旧「未装投递器就地跑」正是 RPC 线程
// 越权碰 widget 树的通道，判据 3 钉死其退场。
AURORA_TEST_CASE(missing_duty_degrades_to_zero_value_without_inline) {
    detail::Win32UiaBridge bridge(nullptr);
    const std::thread::id owner = std::this_thread::get_id();
    std::thread::id worker{};  // `join()` 会把 thread 对象的 id 复位，故由调用线程自己记下
    std::thread::id ran_on{};
    int returned = -1;
    std::thread caller([&bridge, &worker, &ran_on, &returned]() -> void {
        worker = std::this_thread::get_id();
        returned = bridge.eval_on_main<int>(
            [&ran_on]() -> int {
                ran_on = std::this_thread::get_id();
                return 11;
            },
            2000U);
    });
    caller.join();

    AURORA_TEST_CHECK_EQ(returned, 0);  // 降级为零值，进程存活、不 terminate
    AURORA_TEST_CHECK_TRUE(worker != owner);
    AURORA_TEST_CHECK_TRUE(ran_on == std::thread::id{});  // 闭包从未被执行
    AURORA_TEST_CHECK_EQ(MainQueue::pending_count(), std::size_t{0});  // 也从未入队
}

// 目标：超时预算内没排空即降级为零值；**且**晚到的队列项不得补做（用户以为没发生的事不能事后发生）。
AURORA_TEST_CASE(timeout_returns_zero_value_and_drops_the_late_task) {
    detail::Win32UiaBridge bridge(nullptr);
    DutyLease lease;
    std::atomic<bool> ran{false};
    int returned = -1;
    {
        std::thread caller([&bridge, &ran, &returned]() -> void {
            returned = bridge.eval_on_main<int>(
                [&ran]() -> int {
                    ran.store(true);
                    return 7;
                },
                30U);  // 主人线程故意不排水 ⇒ 必然超时
        });
        caller.join();
    }
    AURORA_TEST_CHECK_EQ(returned, 0);  // R{} ⇒ provider 侧映射为 UIA_E_ELEMENTNOTAVAILABLE
    AURORA_TEST_CHECK_FALSE(ran.load());
    AURORA_TEST_REQUIRE_TRUE(lease.drain_one(1000));  // 此刻才排空
    AURORA_TEST_CHECK_FALSE(ran.load());  // 放弃闸必须拦住补做
}

// 目标：桥先于在途队列项析构时，存活闸拦住闭包（这是回投内核唯一的 use-after-free 通道）。
AURORA_TEST_CASE(bridge_destruction_drops_a_still_queued_task) {
    DutyLease lease;
    auto bridge = std::make_unique<detail::Win32UiaBridge>(nullptr);
    std::atomic<bool> ran{false};
    int returned = -1;
    std::thread caller([bridge_ptr = bridge.get(), &ran, &returned]() -> void {
        returned = bridge_ptr->eval_on_main<int>(
            [&ran]() -> int {
                ran.store(true);
                return 7;
            },
            2000U);
    });
    AURORA_TEST_REQUIRE_TRUE(lease.wait_for_task(3000));  // 已入队、尚未执行
    bridge.reset();  // 析构：存活闸落下
    AURORA_TEST_REQUIRE_TRUE(lease.drain_one(3000));  // 队列项被拒绝 → 立即唤醒等待方
    caller.join();

    AURORA_TEST_CHECK_FALSE(ran.load());
    AURORA_TEST_CHECK_EQ(returned, 0);
}

// 目标：provider 侧的快照副本读与导航读都经回投，跨线程拿到的只是副本。
AURORA_TEST_CASE(provider_reads_project_through_the_marshal) {
    detail::Win32UiaBridge bridge(nullptr);
    bridge.activate();
    if (!bridge.is_active()) {
        AURORA_TEST_SKIP("COM apartment or UIAutomationCore.dll unavailable: bridge inactive, no snapshot projection");
        return;
    }
    DutyLease lease;
    ProbeRoot root;
    bridge.set_root(&root);
    bridge.sync_if_dirty();  // 主人线程：先把首帧快照建出来
    const std::uint64_t id = bridge.id_of(&root);
    AURORA_TEST_REQUIRE_TRUE(id != 0);

    // 工作线程只带回「已解码」的标量：可选值在取回线程就地拆完，断言侧不碰 optional。
    struct Remote {
        bool had_node = false;
        bool dangling_stripped = false;
        std::uint64_t node_id = 0;
        bool parent_known = false;
        std::uint64_t parent_id = 1;  // 缺省 1（非 0）⇒「必须有值且为 0」两半都能失败
        bool unknown_known = false;
    };
    Remote remote;
    std::thread caller([&bridge, &remote, id]() -> void {
        const auto n = bridge.snapshot_copy(id);
        if (n.has_value()) {
            remote.had_node = true;
            remote.node_id = n->id;
            remote.dangling_stripped = n->widget == nullptr;
        }
        const auto parent = bridge.navigate_target(id, detail::NavigationKind::Parent);
        remote.parent_known = parent.has_value();
        remote.parent_id = parent.value_or(1ULL);
        remote.unknown_known = bridge.navigate_target(id + 987'654'321ULL, detail::NavigationKind::Parent).has_value();
    });
    // 调用方在一条线程上连发三次回投，主人线程逐个排空。
    AURORA_TEST_REQUIRE_TRUE(lease.drain_one(3000));
    AURORA_TEST_REQUIRE_TRUE(lease.drain_one(3000));
    AURORA_TEST_REQUIRE_TRUE(lease.drain_one(3000));
    caller.join();

    AURORA_TEST_CHECK_TRUE(remote.had_node);
    AURORA_TEST_CHECK_EQ(remote.node_id, id);
    AURORA_TEST_CHECK_TRUE(remote.dangling_stripped);  // 副本不得带出活 widget 指针
    AURORA_TEST_CHECK_TRUE(remote.parent_known);
    AURORA_TEST_CHECK_EQ(remote.parent_id, 0ULL);  // 根无父：值 0 = 该方向无目标
    AURORA_TEST_CHECK_FALSE(remote.unknown_known);
}

#else

// 目标：非主人线程的回投必须在主人线程执行，结果按值带回调用线程。
AURORA_TEST_CASE(cross_thread_eval_runs_on_the_owner_thread) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32/D3D11 is not enabled (non-Windows); the UIA bridge TU is compiled out");
}

// 目标：主人线程自己调回投口时就地执行、零入队。
AURORA_TEST_CASE(owner_thread_eval_runs_inline_without_queuing) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32/D3D11 is not enabled (non-Windows); the UIA bridge TU is compiled out");
}

// 目标：无值班循环 → 边界降级为零值，闭包不就地执行。
AURORA_TEST_CASE(missing_duty_degrades_to_zero_value_without_inline) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32/D3D11 is not enabled (non-Windows); the UIA bridge TU is compiled out");
}

// 目标：超时预算内没排空即降级为零值，且晚到的队列项不得补做。
AURORA_TEST_CASE(timeout_returns_zero_value_and_drops_the_late_task) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32/D3D11 is not enabled (non-Windows); the UIA bridge TU is compiled out");
}

// 目标：桥先于在途队列项析构时，存活闸拦住闭包。
AURORA_TEST_CASE(bridge_destruction_drops_a_still_queued_task) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32/D3D11 is not enabled (non-Windows); the UIA bridge TU is compiled out");
}

// 目标：provider 侧的快照副本读与导航读都经回投。
AURORA_TEST_CASE(provider_reads_project_through_the_marshal) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32/D3D11 is not enabled (non-Windows); the UIA bridge TU is compiled out");
}

#endif

}  // namespace aurora::test_cases::utest_win32_ua_marshal
