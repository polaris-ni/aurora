/// 测试类型: unit
/// 目标单元: include/aurora/core/directionality.h
/// 测试说明: A2 方向性双来源解析——显式来源三级优先（环境注入 > 进程级 host_set > 无）、
///           `explicit_text_direction` 无来源返回 nullopt（shaping 保持内容 guess）、
///           `resolved_text_direction` 确定值回落 LTR、进程级单例测试后复原

#include <optional>
#include <type_traits>

#include "aurora/core/directionality.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_directionality {

namespace {

/// @brief 最小假上下文：只提供 `environment<T>()` 模板（与 BuildContext 的消费面一致）。
struct FakeCtx {
    std::optional<Directionality> injected;

    template <typename T>
    [[nodiscard]] auto environment() const -> const T * {
        if constexpr (std::is_same_v<T, Directionality>) {
            return injected.has_value() ? &*injected : nullptr;
        } else {
            return nullptr;
        }
    }
};

/// @brief 复原进程级单例（测试隔离：不污染后续用例）。
struct ProcessGuard {
    ~ProcessGuard() { current_directionality() = Directionality{}; }
};

}  // namespace

AURORA_TEST_CASE(no_explicit_source_means_nullopt_and_ltr_fallback) {
    ProcessGuard guard;
    // 干净环境 + 进程级默认（host_set=false）：无显式来源。
    const FakeCtx ctx{};
    AURORA_TEST_CHECK_TRUE(explicit_text_direction(ctx).has_value() == false);
    AURORA_TEST_CHECK_TRUE(resolved_text_direction(ctx) == TextDirection::LTR);
}

AURORA_TEST_CASE(environment_injection_wins_over_process) {
    ProcessGuard guard;
    set_directionality(Directionality{.direction = TextDirection::RTL});  // 进程级 host_set

    FakeCtx ctx;
    AURORA_TEST_CHECK_TRUE(explicit_text_direction(ctx).has_value());
    AURORA_TEST_CHECK_TRUE(resolved_text_direction(ctx) == TextDirection::RTL);  // 进程级兜底

    ctx.injected = Directionality{.direction = TextDirection::LTR};  // 环境注入覆盖进程级
    AURORA_TEST_CHECK_TRUE(explicit_text_direction(ctx) == std::optional<TextDirection>{TextDirection::LTR});
    AURORA_TEST_CHECK_TRUE(resolved_text_direction(ctx) == TextDirection::LTR);
}

AURORA_TEST_CASE(process_default_without_host_set_is_not_explicit) {
    ProcessGuard guard;
    // 进程级默认值（host_set=false）不构成显式来源——shaping 保持按内容 guess，
    // 否则默认 LTR 会破坏既有纯 RTL 内容（如阿拉伯语）的自动 guess 行为。
    const FakeCtx ctx{};
    current_directionality().direction = TextDirection::RTL;
    current_directionality().host_set = false;
    AURORA_TEST_CHECK_TRUE(explicit_text_direction(ctx).has_value() == false);
}

}  // namespace aurora::test_cases::utest_directionality
