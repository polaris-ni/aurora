// 目标源单元：core/thread.h（MainThreadOnly 主线程守卫 + AURORA_MAIN_THREAD 标注宏）。
//
// API 覆盖映射：
//   MainThreadOnly<T>(value)          -> test_construct_and_get
//   get()/get() const（同线程）       -> test_construct_and_get / test_const_get
//   set(T)                            -> test_set_roundtrip
//   Check=false 零开销特化            -> test_nocheck_specialization
//   AURORA_MAIN_THREAD 宏                 -> test_main_thread_macro（编译期存在性；GCC 为 no-op）
//
// 覆盖率豁免说明：跨线程访问触发 assert/abort 的路径无法在本进程内断言
// （Debug 下直接终止进程），属不可测路径；同线程安全路径已全覆盖。

#include <string>
#include <thread>

#include "aurora/core/thread.h"

#include "test_harness.h"

using aurora::MainThreadOnly;

namespace {

struct ThreadProbe {
    std::thread::id constructed_on = std::this_thread::get_id();
};

void test_construct_and_get() {
    MainThreadOnly guard{ 42 };
    AURORA_TEST_CHECK_EQ(guard.get(), 42);
    AURORA_TEST_CHECK(guard.get() == 42);

    MainThreadOnly named{ std::string{ "ui" } };
    AURORA_TEST_CHECK_EQ(named.get(), std::string{ "ui" });
}

void test_const_get() {
    const MainThreadOnly guard{ 7 };
    AURORA_TEST_CHECK_EQ(guard.get(), 7);
}

void test_set_roundtrip() {
    MainThreadOnly guard{ std::string{ "a" } };
    guard.set(std::string{ "b" });
    AURORA_TEST_CHECK_EQ(guard.get(), std::string{ "b" });
    guard.set(std::string{});
    AURORA_TEST_CHECK_TRUE(guard.get().empty());
}

void test_nocheck_specialization() {
    // Check=false：不存储 owner、无断言，接口与主特化一致。
    MainThreadOnly<int, false> light{ 1 };
    AURORA_TEST_CHECK_EQ(light.get(), 1);
    light.set(2);
    AURORA_TEST_CHECK_EQ(light.get(), 2);

    const MainThreadOnly<int, false> clight{ 3 };
    AURORA_TEST_CHECK_EQ(clight.get(), 3);
}

void test_owner_is_this_thread() {
    // 构造线程即属主线程序义：本测试全程单线程，owner 应等于当前线程 id。
    MainThreadOnly guard{ ThreadProbe{} };
    AURORA_TEST_CHECK(guard.get().constructed_on == std::this_thread::get_id());
}

} // namespace

AURORA_TEST() {
    test_construct_and_get();
    test_const_get();
    test_set_roundtrip();
    test_nocheck_specialization();
    test_owner_is_this_thread();
}
