/// 测试类型: unit
/// 目标单元: include/aurora/environment/environment.h
/// 测试说明: Environment 链式父子作用域（with / set_local / set）的类型化读写与最近祖先优先语义

#include <string>

#include "aurora/environment/environment.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_environment {

AURORA_TEST_CASE(default_environment_is_empty) {
    // 默认构造：无任何类型化键值。
    const aurora::Environment env;
    AURORA_TEST_CHECK(env.get<int>() == nullptr);
    AURORA_TEST_CHECK(env.get<std::string>() == nullptr);
}

AURORA_TEST_CASE(with_creates_child_without_mutating_parent) {
    // with<T> 生成覆盖 T 的子环境；原环境保持不变（不可变链式结构契约）。
    const aurora::Environment parent;
    const aurora::Environment child = parent.with<int>(7);
    AURORA_TEST_CHECK(parent.get<int>() == nullptr);  // 原环境未被写入
    AURORA_TEST_REQUIRE(child.get<int>() != nullptr);
    AURORA_TEST_CHECK_EQ(*child.get<int>(), 7);
}

AURORA_TEST_CASE(nearest_ancestor_wins_in_chain) {
    // 沿父链向上查找「最近的定义」：内层覆盖外层，未覆盖类型穿透到祖先。
    aurora::Environment root;  // 根 Provider：set_local 本地注入
    root.set_local<std::string>("theme");
    const aurora::Environment mid = root.with<int>(1);
    const aurora::Environment leaf = mid.with<float>(2.5F);
    AURORA_TEST_CHECK_EQ(*leaf.get<float>(), 2.5F);  // 本层命中
    AURORA_TEST_CHECK_EQ(*leaf.get<int>(), 1);  // 穿透到 mid
    AURORA_TEST_CHECK_EQ(*leaf.get<std::string>(), std::string("theme"));  // 穿透到 root
    AURORA_TEST_CHECK_EQ(*mid.get<int>(), 1);
    AURORA_TEST_CHECK(mid.get<float>() == nullptr);  // 祖先看不到后代的覆盖
}

AURORA_TEST_CASE(sibling_children_are_independent) {
    // 同一父环境派生的两个子环境互不可见。
    const aurora::Environment parent;
    const aurora::Environment first = parent.with<int>(1);
    const aurora::Environment second = parent.with<int>(2);
    AURORA_TEST_CHECK_EQ(*first.get<int>(), 1);
    AURORA_TEST_CHECK_EQ(*second.get<int>(), 2);
    AURORA_TEST_CHECK(parent.get<int>() == nullptr);
}

AURORA_TEST_CASE(set_local_inserts_without_parent_link) {
    // set_local 仅写入本层映射（供根 Provider 使用，无父指针）；子环境本地插入不影响父环境。
    aurora::Environment root;
    root.set_local<int>(7);
    AURORA_TEST_CHECK_EQ(*root.get<int>(), 7);
    aurora::Environment child = root.with<std::string>("x");
    child.set_local<int>(9);
    AURORA_TEST_CHECK_EQ(*child.get<int>(), 9);
    AURORA_TEST_CHECK_EQ(*root.get<int>(), 7);  // 父环境不受子环境 set_local 影响
}

AURORA_TEST_CASE(set_overwrites_in_place_and_keeps_parent_chain) {
    // set<T> 覆盖本层既有值且保留父指针：覆盖后仍能沿链读到祖先类型。
    aurora::Environment root;
    root.set_local<std::string>("anc");
    aurora::Environment child = root.with<int>(1);
    child.set<int>(2);
    AURORA_TEST_CHECK_EQ(*child.get<int>(), 2);
    AURORA_TEST_CHECK_EQ(*child.get<std::string>(), std::string("anc"));  // 父链未因 set 失效
}

AURORA_TEST_CASE(type_identity_keys_are_distinct) {
    // 键即类型：相近标量类型互不可见；自定义类型同样按类型寻址。
    aurora::Environment env;
    env.set_local<int>(5);
    AURORA_TEST_CHECK(env.get<float>() == nullptr);  // int 与 float 是不同的键
    struct SessionConfig {  // 本地类型作键
        int padding = 0;
    };
    env.set_local<SessionConfig>(SessionConfig{.padding = 8});
    const auto *cfg = env.get<SessionConfig>();
    AURORA_TEST_REQUIRE(cfg != nullptr);
    AURORA_TEST_CHECK_EQ(cfg->padding, 8);
    AURORA_TEST_CHECK_EQ(*env.get<int>(), 5);
}

}  // namespace aurora::test_cases::utest_environment
