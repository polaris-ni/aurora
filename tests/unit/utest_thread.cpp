/// 测试类型: unit
/// 目标单元: include/aurora/core/thread.h
/// 测试说明: MainThreadOnly 的 get/set 往返、const 只读访问、move-only 类型承载、零开销特化（Check=false 不存
/// owner）、owner 即构造线程（工作线程内自构造自访问）、AURORA_MAIN_THREAD 宏可声明可调用

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "aurora/core/thread.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_thread {

/// @brief AURORA_MAIN_THREAD 标注的样例函数（GCC 下为 no-op，clang 下为 annotate 属性）。
AURORA_MAIN_THREAD static auto sample_main_thread_function() -> int { return 42; }

AURORA_TEST_CASE(main_thread_only_get_set_roundtrip) {
    aurora::MainThreadOnly<int> guard{7};
    AURORA_TEST_CHECK_EQ(guard.get(), 7);
    guard.set(9);  // 仅 owner 线程可写
    AURORA_TEST_CHECK_EQ(guard.get(), 9);

    // const 访问返回只读引用。
    const auto& view = guard;
    AURORA_TEST_CHECK_EQ(view.get(), 9);

    // 非平凡类型同样适用。
    AURORA_TEST_CHECK_EQ(aurora::MainThreadOnly<std::string>{std::string{"au"}}.get(), std::string{"au"});
}

AURORA_TEST_CASE(main_thread_only_supports_move_only_types) {
    // 构造与 set 均按值接收：move-only 类型经移动进入包装。
    aurora::MainThreadOnly<std::unique_ptr<int>> guard{std::make_unique<int>(5)};
    AURORA_TEST_CHECK_NOT_NULL(guard.get().get());
    AURORA_TEST_CHECK_EQ(*guard.get(), 5);
    guard.set(std::make_unique<int>(6));
    AURORA_TEST_CHECK_EQ(*guard.get(), 6);
}

AURORA_TEST_CASE(zero_overhead_specialization_behaves_identically) {
    aurora::MainThreadOnly<int, false> guard{3};
    AURORA_TEST_CHECK_EQ(guard.get(), 3);
    guard.set(8);
    AURORA_TEST_CHECK_EQ(guard.get(), 8);
    const auto& view = guard;
    AURORA_TEST_CHECK_EQ(view.get(), 8);

    // Check=false 不存储 owner 线程：不得大于开启检查的形态。
    AURORA_TEST_CHECK_LE(sizeof(aurora::MainThreadOnly<int, false>), sizeof(aurora::MainThreadOnly<int, true>));
}

AURORA_TEST_CASE(owner_is_constructing_thread) {
    AURORA_TEST_REQUIRE_THREADS();
    // owner 取构造线程：在工作线程内构造并访问该实例（get/set 全程同线程，不触发断言中止）。
    std::promise<int> done;
    auto fut = done.get_future();
    std::thread worker([&done]() -> void {
        aurora::MainThreadOnly<int> local{5};
        local.set(6);
        done.set_value(local.get());
    });
    // 确定性：用 future + 充裕超时等待，不做任何时序假设。
    AURORA_TEST_REQUIRE(fut.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    AURORA_TEST_CHECK_EQ(fut.get(), 6);
    worker.join();
}

AURORA_TEST_CASE(main_thread_annotation_macro_declarable) {
    // AURORA_MAIN_THREAD 标注的函数可正常声明与调用。
    AURORA_TEST_CHECK_EQ(sample_main_thread_function(), 42);
}

}  // namespace aurora::test_cases::utest_thread
