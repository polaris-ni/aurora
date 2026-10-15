/// 测试类型: unit
/// 目标单元: include/aurora/net/cancellation.h
/// 测试说明: aurora::net::Cancellation 的取消契约（CHG-018 底座切片）：初始未取消、`cancel()`
///           幂等且钩子至多一次、**取消先于登记时钩子仍立即触发**（不留「登记即失效」的窄竞态）、
///           传空可撤销登记、拷贝共享同一状态而独立构造互不影响、钩子在调用线程上同步执行
///           （引擎据此安排「先释放句柄、后投递结果」的顺序）、钩子内重入 `cancel()` 不死锁。
///           跨线程用例经 `AURORA_TEST_REQUIRE_THREADS()` 守卫；平台分支写在用例体内
///           （registry_integrity 静态扫源码，被 #ifdef 摘掉的声明会在另一种构建里单边失踪）。

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "aurora/core/platform.h"
#include "aurora/net/cancellation.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_cancellation {

// 新建令牌未取消；cancel 置位且可重复调用，状态位只转换一次。
AURORA_TEST_CASE(cancel_is_idempotent) {
    aurora::net::Cancellation token;
    AURORA_TEST_CHECK_FALSE(token.is_cancelled());

    token.cancel();
    AURORA_TEST_CHECK_TRUE(token.is_cancelled());
    token.cancel();
    token.cancel();
    AURORA_TEST_CHECK_TRUE(token.is_cancelled());
}

// 钩子至多一次：三次 cancel 只换来一次回调（引擎据此放行挂起点，重复放行即双份结果）。
AURORA_TEST_CASE(hook_runs_at_most_once) {
    aurora::net::Cancellation token;
    int runs = 0;
    token.on_cancel([&runs]() -> void { ++runs; });

    token.cancel();
    token.cancel();
    token.cancel();
    AURORA_TEST_CHECK_EQ(runs, 1);
}

// 取消先发生、钩子后登记：必须立即在当前线程执行，否则挂起点永不放行。
AURORA_TEST_CASE(hook_registered_after_cancel_fires_immediately) {
    aurora::net::Cancellation token;
    token.cancel();

    int runs = 0;
    token.on_cancel([&runs]() -> void { ++runs; });
    AURORA_TEST_CHECK_EQ(runs, 1);
}

// 空可调用体撤销登记：撤销后 cancel 不再回调（宿主析构时摘钩的入口）。
AURORA_TEST_CASE(empty_hook_clears_registration) {
    aurora::net::Cancellation token;
    int runs = 0;
    token.on_cancel([&runs]() -> void { ++runs; });
    token.on_cancel({});
    token.cancel();
    AURORA_TEST_CHECK_EQ(runs, 0);
    AURORA_TEST_CHECK_TRUE(token.is_cancelled());
}

// 拷贝是「同一状态的另一个句柄」：一侧取消全侧可见，独立构造的两枚互不影响。
AURORA_TEST_CASE(copies_share_one_state) {
    aurora::net::Cancellation origin;
    // 拷贝句柄同样可触发取消（共享状态），故两侧都必须是非 const 视角。
    aurora::net::Cancellation copy = origin;
    aurora::net::Cancellation other;

    AURORA_TEST_CHECK_TRUE(origin.shares_state_with(copy));
    AURORA_TEST_CHECK_FALSE(origin.shares_state_with(other));

    int runs = 0;
    other.on_cancel([&runs]() -> void { ++runs; });
    copy.cancel();

    AURORA_TEST_CHECK_TRUE(origin.is_cancelled());
    AURORA_TEST_CHECK_TRUE(copy.is_cancelled());
    AURORA_TEST_CHECK_FALSE(other.is_cancelled());
    AURORA_TEST_CHECK_EQ(runs, 0);

    other.cancel();
    AURORA_TEST_CHECK_EQ(runs, 1);
}

// 钩子在调用线程上同步执行，且钩子内重入 `cancel()` 不死锁不二次触发：worker 里取消，
// 钩子就跑在 worker——引擎据此把「释放句柄」排在「投递取消结果」之前。
AURORA_TEST_CASE(hook_runs_on_calling_thread) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime is single-threaded; there is no worker thread to cancel from");
#else
    AURORA_TEST_REQUIRE_THREADS();

    aurora::net::Cancellation token;
    const std::thread::id main_id = std::this_thread::get_id();
    std::thread::id hook_id;
    std::mutex gate;
    std::condition_variable done;
    bool finished = false;

    token.on_cancel([&token, main_id, &hook_id, &gate, &done, &finished]() -> void {
        hook_id = std::this_thread::get_id();
        AURORA_TEST_CHECK_TRUE(hook_id != main_id);
        token.cancel();  // 重入：状态已转换，此处既不递归回调也不阻塞
        {
            const std::scoped_lock lock(gate);
            finished = true;
        }
        done.notify_all();
    });

    std::thread worker([&token]() -> void { token.cancel(); });
    {
        std::unique_lock<std::mutex> lock(gate);
        AURORA_TEST_REQUIRE(done.wait_for(lock, std::chrono::seconds{5}, [&finished]() -> bool { return finished; }));
    }
    worker.join();

    AURORA_TEST_CHECK_TRUE(token.is_cancelled());
    AURORA_TEST_CHECK_TRUE(hook_id != main_id);
#endif
}

}  // namespace aurora::test_cases::utest_net_cancellation
