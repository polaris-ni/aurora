/// 测试类型: unit
/// 目标单元: include/aurora/theming/shape_scale.h + spacing_scale.h + control_metrics.h + layout_conventions.h
/// 测试说明: 钉死四组令牌的 Material 基线「逐位复现现行硬编码」这条零回归判据——形状 / 间距 / 度量
/// 三组的可复现项与控件在无注入主题下的默认解算值（`Button` / `Divider` 的 `resolve_*` 通道）逐字段
/// 相等，惯例组的默认项与落地前的排布行为一致。
/// 标「候选」的三项（控件紧凑档之外的命中下限、滚动条粗细）只断言声明值，不对账控件。

#include "aurora/theming/theme.h"
#include "aurora/widget/button.h"
#include "aurora/widget/divider.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_tokens {

namespace {

/// 探针：把 protected 的 `resolve_*` 钩子与属性字段提到可读位置（对账走的与生产渲染同一条函数）。
class ButtonProbe final : public Button {
  public:
    using Button::resolve_corner_radius;
    using Button::resolve_padding;
};

}  // namespace

AURORA_TEST_CASE(shape_scale_reproduces_current_button_corner) {
    const ShapeScale shapes;
    AURORA_TEST_CHECK(shapes.none == 0.0F);
    AURORA_TEST_CHECK(shapes.extra_small == 4.0F);
    AURORA_TEST_CHECK(shapes.small == 6.0F);
    AURORA_TEST_CHECK(shapes.medium == 8.0F);
    AURORA_TEST_CHECK(shapes.large == 12.0F);
    AURORA_TEST_CHECK(shapes.extra_large == 16.0F);
    // 零回归对账：未显式设圆角时 Button 解算到的正是 `shapes.small`——接线批把默认值改成读令牌后，
    // 这条判据从「props 常量相等」迁到「同一条 resolve 通道在无注入主题下相等」。
    const BuildContext bare;
    ButtonProbe b;
    AURORA_TEST_CHECK_FALSE(b.corner_radius.has_value());  // 未设值才走主题档
    AURORA_TEST_CHECK(b.resolve_corner_radius(bare) == shapes.small);
}

AURORA_TEST_CASE(spacing_scale_reproduces_current_button_padding) {
    const SpacingScale spacing;
    AURORA_TEST_CHECK(spacing.none == 0.0F);
    AURORA_TEST_CHECK(spacing.extra_small == 4.0F);
    AURORA_TEST_CHECK(spacing.small == 6.0F);
    AURORA_TEST_CHECK(spacing.medium == 8.0F);
    AURORA_TEST_CHECK(spacing.large == 12.0F);
    AURORA_TEST_CHECK(spacing.extra_large == 16.0F);
    AURORA_TEST_CHECK(spacing.huge == 24.0F);
    // 零回归对账：未显式设内边距时 Button 解算到水平 = `large`、垂直 = `small`。
    const BuildContext bare;
    ButtonProbe b;
    AURORA_TEST_CHECK_FALSE(b.padding.has_value());  // 未设值才走主题档
    const EdgeInsets pad = b.resolve_padding(bare);
    AURORA_TEST_CHECK(pad.left == spacing.large);
    AURORA_TEST_CHECK(pad.right == spacing.large);
    AURORA_TEST_CHECK(pad.top == spacing.small);
    AURORA_TEST_CHECK(pad.bottom == spacing.small);
}

AURORA_TEST_CASE(control_metrics_reproduces_current_widget_metrics) {
    const ControlMetrics controls;
    // 可对账项：与控件公共头 / Widget 常量的现值逐位相等。
    // Divider 的线厚在控件侧改读 `controls.separator_thickness`（未显式设值时），对账同走 resolve 通道。
    const Divider divider;
    AURORA_TEST_CHECK_FALSE(divider.thickness.has_value());
    AURORA_TEST_CHECK(divider.resolve_thickness(BuildContext{}) == controls.separator_thickness);
    // 焦点环现值住在 `Widget` 的受保护常量里（绘制与脏区共用），测试侧不可直读，故只钉声明值 2.0 / 2.0。
    // 本模块未改读这两枚令牌：环宽除绘制侧还决定脏区外扩，而 `dirty_bounds()` 没有 `BuildContext`
    // 可拿主题，单接绘制侧会让「画的环」与「标脏的盒」取到不同宽度（残影像素）。留待脏区具备
    // 主题入口时两处一并改读，届时以「替换前后环宽不变」的像素断言兜底。
    AURORA_TEST_CHECK(controls.focus_ring_inset == 2.0F);
    AURORA_TEST_CHECK(controls.focus_ring_width == 2.0F);
    // 声明值对账（出处见头注释：Checkbox 20 / MenuBar 26 / Dropdown 与 SpinBox 框高 30 /
    // Switch 轨高 24 / Icon 24 / IconButton 内边距 8 / TextInput 与 Dropdown 描边 1.0）。
    AURORA_TEST_CHECK(controls.indicator_size == 20.0F);
    AURORA_TEST_CHECK(controls.menu_item_height == 26.0F);
    AURORA_TEST_CHECK(controls.control_height == 30.0F);
    AURORA_TEST_CHECK(controls.control_height_small == 24.0F);
    AURORA_TEST_CHECK(controls.border_thickness == 1.0F);
    AURORA_TEST_CHECK(controls.icon_default_size == 24.0F);
    AURORA_TEST_CHECK(controls.content_padding == 8.0F);
    // 候选项（无既有实现）只钉声明值，落码补出处前不得据其改任何控件。
    AURORA_TEST_CHECK(controls.min_touch_target == 40.0F);
    AURORA_TEST_CHECK(controls.scrollbar_thickness == 4.0F);
}

AURORA_TEST_CASE(layout_conventions_default_to_current_behavior) {
    const LayoutConventions conventions;
    // 现状：`confirm()` 把接受钮排在拒绝钮之前、标题随正文起排。
    AURORA_TEST_CHECK(conventions.dialog_button_order == DialogButtonOrder::AcceptFirst);
    AURORA_TEST_CHECK(conventions.dialog_title_alignment == DialogTitleAlignment::Leading);
    // 现状：菜单栏在窗口内自绘（原生菜单栏是能力位而非默认惯例）。
    AURORA_TEST_CHECK(conventions.menu_bar_placement == MenuBarPlacement::InWindow);
    // 现状：无覆盖式滚动条实现，滚动条模式取经典占位。
    AURORA_TEST_CHECK(conventions.scrollbar_mode == ScrollbarMode::Classic);
    // 现状：焦点环为沿缘描边（Widget 统一画环），动效倍率不缩放。
    AURORA_TEST_CHECK(conventions.focus_ring_style == FocusRingStyle::Outline);
    AURORA_TEST_CHECK(conventions.motion_scale == 1.0F);
}

AURORA_TEST_CASE(theme_aggregate_carries_tokens_in_light_and_dark) {
    // 四组令牌不分明暗：同一工厂的明暗两档令牌逐字段相同。
    const Theme light = Theme::light();
    const Theme dark = Theme::dark();
    AURORA_TEST_CHECK(light.shapes == dark.shapes);
    AURORA_TEST_CHECK(light.spacing == dark.spacing);
    AURORA_TEST_CHECK(light.controls == dark.controls);
    AURORA_TEST_CHECK(light.conventions == dark.conventions);
    // 裸主题即 Material 基线：令牌取声明默认值。
    const Theme bare = Theme::with_defaults();
    AURORA_TEST_CHECK(bare.shapes == ShapeScale{});
    AURORA_TEST_CHECK(bare.spacing == SpacingScale{});
    AURORA_TEST_CHECK(bare.controls == ControlMetrics{});
    AURORA_TEST_CHECK(bare.conventions == LayoutConventions{});
    // 令牌是值语义：随主题拷贝独立，改一份不影响另一份。
    Theme copy = light;
    copy.shapes.small = 99.0F;
    AURORA_TEST_CHECK(light.shapes.small != copy.shapes.small);
}

}  // namespace aurora::test_cases::utest_theme_tokens
