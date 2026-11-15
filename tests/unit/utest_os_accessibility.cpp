/// 测试类型: unit
/// 目标单元: include/aurora/window/platform.h（os_accessibility_settings）
///           include/aurora/app/application.h（refresh_os_accessibility_settings）
/// 测试说明: OS 无障碍偏好的**读点契约**与显式刷新：三档读数的归一化（字号倍率永远可乘、
///           重复直读一致、`screen_reader_active` 不归该函数管）、显式 `refresh` 把系统真值
///           写进进程级单例（覆盖先前注入值）、幂等，以及「无头建窗不播种」这条接缝不被越界——
///           平台真值随机器不同，故只钉契约不钉具体档位

#include <utility>

#include "aurora/app/application.h"
#include "aurora/core/accessibility.h"
#include "aurora/widget/text.h"
#include "aurora/window/platform.h"
#include "aurora/window/surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_os_accessibility {

namespace {

/// @brief 复原进程级单例（与 `utest_accessibility` 的同类守卫同口径：本文件会改写它）。
class ScopedSettings {
  public:
    ScopedSettings() : saved_(current_accessibility_settings()) {}
    ~ScopedSettings() { set_accessibility_settings(saved_); }
    ScopedSettings(const ScopedSettings &) = delete;
    auto operator=(const ScopedSettings &) -> ScopedSettings & = delete;
    ScopedSettings(ScopedSettings &&) = delete;
    auto operator=(ScopedSettings &&) -> ScopedSettings & = delete;

  private:
    AccessibilitySettings saved_;
};

auto make_scene() -> Scene { return Scene{Node{std::make_shared<Text>("hi")}}; }

/// @brief 产出一枚无头窗口（无头 Surface 是「不播种 OS 无障碍偏好」的固定接缝）。
/// @param title 窗口标题（仅用于日志归因）。
auto make_headless_window(std::string title) -> std::unique_ptr<Window> {
    HeadlessOptions opts;
    static_cast<WindowOptions &>(opts) = WindowOptions{};
    opts.title = std::move(title);
    opts.size = Size{.width = 200.0F, .height = 150.0F};
    auto res = create_window(opts);
    return res ? std::move(res.value()) : nullptr;
}

}  // namespace

// 平台真值随机器不同（有无高对比 / 是否关动画 / 字号倍率多少），故本组只钉**契约**：
// 归一化后的倍率永远可乘、重复直读结果一致（直读不缓存）、桥的字段不被本函数染指。
AURORA_TEST_CASE(os_settings_are_normalized_and_stable_across_reads) {
    const AccessibilitySettings first = os_accessibility_settings();
    const AccessibilitySettings again = os_accessibility_settings();

    // 倍率必须落在「可以乘进字阶」的区间：0 / 负值 / NaN 都会把度量打崩，`resolved_font_scale`
    // 兜的是非法值，而读点自身也不该产出 > 1e6 的荒唐值（Windows 上限 225%）。
    AURORA_TEST_CHECK_TRUE(first.resolved_font_scale() > 0.0F);
    AURORA_TEST_CHECK_TRUE(first.resolved_font_scale() < 1.0e6F);

    // 直读不缓存：同一环境连续两次读到的三档一致。
    AURORA_TEST_CHECK_EQ(first.reduce_motion, again.reduce_motion);
    AURORA_TEST_CHECK_EQ(first.high_contrast, again.high_contrast);
    AURORA_TEST_CHECK_NEAR(first.resolved_font_scale(), again.resolved_font_scale(), 1e-6F);

    // `screen_reader_active` 由无障碍桥在激活时回填，读点不碰它（保持结构默认）。
    AURORA_TEST_CHECK_FALSE(first.screen_reader_active);
}

AURORA_TEST_CASE(refresh_writes_os_truth_into_process_singleton) {
    const ScopedSettings guard;
    // 先注入一份「与系统不同」的档位，确认刷新是**覆盖**而不是保留注入值。
    set_accessibility_settings(
        AccessibilitySettings{.reduce_motion = false, .high_contrast = true, .font_scale = 3.0F});

    auto window = make_headless_window("os_accessibility_refresh");
    AURORA_TEST_REQUIRE_TRUE(window != nullptr);
    Application app{make_scene(), std::move(window)};

    app.refresh_os_accessibility_settings();

    const AccessibilitySettings now = current_accessibility_settings();
    const AccessibilitySettings os = os_accessibility_settings();
    AURORA_TEST_CHECK_EQ(now.reduce_motion, os.reduce_motion);
    AURORA_TEST_CHECK_EQ(now.high_contrast, os.high_contrast);
    AURORA_TEST_CHECK_NEAR(now.resolved_font_scale(), os.resolved_font_scale(), 1e-6F);

    // 幂等：再刷一次结果不变（且不因「读回同一份」而抖出别的值）。
    app.refresh_os_accessibility_settings();
    AURORA_TEST_CHECK_EQ(current_accessibility_settings().reduce_motion, os.reduce_motion);
    AURORA_TEST_CHECK_EQ(current_accessibility_settings().high_contrast, os.high_contrast);
    AURORA_TEST_CHECK_NEAR(current_accessibility_settings().resolved_font_scale(), os.resolved_font_scale(), 1e-6F);
}

AURORA_TEST_CASE(headless_window_creation_does_not_overwrite_injected_settings) {
    const ScopedSettings guard;
    // 无头 Surface 是测试注入的接缝：建窗**不得**把进程级单例改写成 OS 真值，否则
    // 「开关置位后如何表现」这一类用例就再也测不了（播种只对真实后端做，见 window_factory）。
    const AccessibilitySettings injected{.reduce_motion = true, .high_contrast = false, .font_scale = 1.75F};
    set_accessibility_settings(injected);

    auto window = make_headless_window("os_accessibility_headless_injection");
    AURORA_TEST_REQUIRE_TRUE(window != nullptr);

    const AccessibilitySettings after = current_accessibility_settings();
    AURORA_TEST_CHECK_EQ(after.reduce_motion, injected.reduce_motion);
    AURORA_TEST_CHECK_EQ(after.high_contrast, injected.high_contrast);
    AURORA_TEST_CHECK_NEAR(after.resolved_font_scale(), 1.75F, 1e-6F);
}

}  // namespace aurora::test_cases::utest_os_accessibility
