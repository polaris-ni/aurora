/// 测试类型: unit
/// 目标单元: include/aurora/environment/build_context.h
/// 测试说明: BuildContext 只读视图的默认不变量、environment<T>() 环境查找与 env_of 引用读取

#include <string>

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_build_context {

// 说明：env_of<T> 在环境中缺值时按契约硬失败（AURORA_CHECK 常开，所有构建生效），
// 死亡行为由专用死亡测试用例覆盖；本文件其余用例覆盖成功路径与 nullptr 安全路径。

AURORA_TEST_CASE(default_constructed_fields) {
    // 默认构造：env 未注入、缩放因子 1.0、尺寸全零（布局前的初始状态）。
    constexpr aurora::BuildContext ctx;
    AURORA_TEST_CHECK(ctx.env == nullptr);
    AURORA_TEST_CHECK_EQ(ctx.scale_factor, 1.0F);
    AURORA_TEST_CHECK_EQ(ctx.size.width, 0.0F);
    AURORA_TEST_CHECK_EQ(ctx.size.height, 0.0F);
}

AURORA_TEST_CASE(environment_lookup_without_env_returns_nullptr) {
    // env 为 nullptr 时 environment<T>() 必须安全返回 nullptr，不得解引用悬空指针。
    constexpr aurora::BuildContext ctx;
    AURORA_TEST_CHECK(ctx.environment<int>() == nullptr);
    AURORA_TEST_CHECK(ctx.environment<std::string>() == nullptr);
}

AURORA_TEST_CASE(environment_lookup_reads_injected_value) {
    // env 注入后按类型读取；未注入的类型返回 nullptr（按 typeid 寻址）。
    aurora::Environment env;
    env.set_local<int>(42);
    aurora::BuildContext ctx;
    ctx.env = &env;
    const int *value = ctx.environment<int>();
    AURORA_TEST_REQUIRE(value != nullptr);
    AURORA_TEST_CHECK_EQ(*value, 42);
    AURORA_TEST_CHECK(ctx.environment<std::string>() == nullptr);
}

AURORA_TEST_CASE(env_of_returns_reference_to_stored_value) {
    // env_of<T> 返回环境值的 const 引用，读取结果与存储值同源。
    aurora::Environment env;
    env.set_local<std::string>("aurora");
    aurora::BuildContext ctx;
    ctx.env = &env;
    AURORA_TEST_CHECK_EQ(aurora::env_of<std::string>(ctx), std::string("aurora"));
}

AURORA_TEST_CASE(fields_carry_scale_and_layout_size) {
    // scale_factor / size 为快速访问字段，随注入/布局填充后应原样可读。
    aurora::BuildContext ctx;
    ctx.scale_factor = 2.5F;
    ctx.size = aurora::Size{.width = 800.0F, .height = 600.0F};
    AURORA_TEST_CHECK_EQ(ctx.scale_factor, 2.5F);
    AURORA_TEST_CHECK_NEAR(ctx.size.width, 800.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(ctx.size.height, 600.0F, 1e-6F);
}

AURORA_TEST_CASE(env_of_missing_type_aborts_process) {
    // 失败路径（死亡测试）：env_of<T> 缺值按契约硬失败——AURORA_CHECK 常开，
    // 所有构建配置（含 Release）下都 abort，杜绝解引用 nullptr 的 UB。
    // Emscripten 下 AURORA_TEST_CHECK_DEATH 整体退化为 SKIP，ctx 不再被读取——标注避免
    // -Wunused-variable（仅交叉构建触发；原生构建沿用死亡测试路径）。
    [[maybe_unused]] constexpr aurora::BuildContext ctx;  // env 未注入
    // env_of<T> 缺值按契约 abort：此处仅执行语句触发死亡，引用返回值不可能被使用，故显式丢弃。
    AURORA_TEST_CHECK_DEATH((void)aurora::env_of<int>(ctx), "env_of");
}

}  // namespace aurora::test_cases::utest_build_context
