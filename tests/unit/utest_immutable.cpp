/// 测试类型: unit
/// 目标单元: include/aurora/core/immutable.h
/// 测试说明: 覆盖 Immutable 只读穿透与 scope 标签、Mutable 读写穿透，以及「写入路径在类型层面被删除」的编译期契约

#include <string>
#include <type_traits>
#include <utility>

#include "aurora/core/immutable.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_immutable {

namespace {

/// @brief 写入口探测：类型是否有可调用的 set(int)（concept 体内为依赖表达式，缺失成员判 false）。
template <typename W>
concept writable_via_set = requires(W& w, int v) { w.set(v); };

}  // namespace

AURORA_TEST_CASE(immutable_reads_through_to_underlying_state) {
    aurora::State<int> src{5};
    const aurora::Immutable<int> ro{src};
    AURORA_TEST_CHECK_EQ(ro.get(), 5);

    // 读路径实时穿透：底层 State 更新后无需重建包装即可观察到新值。
    src.set(9);
    AURORA_TEST_CHECK_EQ(ro.get(), 9);
}

AURORA_TEST_CASE(immutable_scope_label_default_empty) {
    aurora::State<int> src{1};
    const aurora::Immutable<int> ro{src};
    AURORA_TEST_CHECK(ro.scope().empty());
}

AURORA_TEST_CASE(immutable_scope_label_preserved) {
    aurora::State<std::string> src{"hello"};
    const aurora::Immutable<std::string> ro{src, "reader-scope"};
    AURORA_TEST_CHECK_STREQ(ro.scope(), "reader-scope");
    AURORA_TEST_CHECK_STREQ(ro.get(), "hello");
}

AURORA_TEST_CASE(mutable_writes_through_to_underlying_state) {
    aurora::State<int> src{1};
    aurora::Mutable<int> mu{src, "editor-scope"};  // set 为非 const 成员，包装本体须可变

    // 写路径穿透：Mutable::set 直接更新底层 State。
    mu.set(3);
    AURORA_TEST_CHECK_EQ(src.get(), 3);
    AURORA_TEST_CHECK_EQ(mu.get(), 3);
    AURORA_TEST_CHECK_STREQ(mu.scope(), "editor-scope");
}

AURORA_TEST_CASE(mutable_scope_label_default_empty) {
    aurora::State<int> src{0};
    const aurora::Mutable<int> mu{src};
    AURORA_TEST_CHECK(mu.scope().empty());
}

AURORA_TEST_CASE(write_path_shape_is_compile_time_enforced) {
    // 类型层面契约：Immutable 不暴露任何写入口（set 不存在）；
    // Mutable 暴露 set(T)。用 concept 探测在编译期锁定该形状
    // （concept 体内表达式依赖模板参数 T，对具体类型的不存在成员判 false 而非硬错误）。
    static_assert(!writable_via_set<aurora::Immutable<int>>, "Immutable 不得暴露写路径");
    static_assert(writable_via_set<aurora::Mutable<int>>, "Mutable 必须暴露写路径");
    // 读路径两侧都返回 const 引用（get 对 const 对象可用）。
    static_assert(std::is_same_v<decltype(std::declval<const aurora::Immutable<int>&>().get()), const int&>);
    static_assert(std::is_same_v<decltype(std::declval<const aurora::Mutable<int>&>().get()), const int&>);
    AURORA_TEST_CHECK(true);
}

}  // namespace aurora::test_cases::utest_immutable
