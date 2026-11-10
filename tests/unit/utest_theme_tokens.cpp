/// 测试类型: unit
/// 目标单元: include/aurora/theming/shape_scale.h + spacing_scale.h + control_metrics.h + layout_conventions.h
/// 测试说明: 钉死四组令牌的 Material 基线「逐位复现现行硬编码」这条零回归判据——形状 / 间距 / 度量
/// 三组的可复现项与控件在无注入主题下的默认生效值（`Button` 的属性行读件、`Divider` 的 `resolve_*`
/// 通道）逐字段相等，惯例组的默认项与落地前的排布行为一致。
/// 标「候选」的三项（控件紧凑档之外的命中下限、滚动条粗细）只断言声明值，不对账控件。

#include "aurora/theming/theme.h"
#include "aurora/widget/button.h"
#include "aurora/widget/divider.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_tokens {

namespace {

/// 探针：把库内消费的表态位提到可读位置——生效值本身已经是公共读件，不需要探针。
class ButtonProbe final : public Button {
  public:
    using Button::present;
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
    // 零回归对账：未表态时 Button 的圆角读件解算到的正是 `shapes.small`——本行回落点是活令牌，
    // 「同一条读件在无注入主题下与令牌相等」即接线批的判据形态（原 `resolve_corner_radius` 钩子已退役）。
    const BuildContext bare;
    ButtonProbe b;
    AURORA_TEST_CHECK_FALSE(b.present(ButtonProps::corner_radius_row()));  // 未表态才走主题档
    AURORA_TEST_CHECK(b.corner_radius(bare) == shapes.small);
    // 两态语义的另一半：表态后不再跟随档，撤表态又回到档。
    b.set_corner_radius(11.5F);
    AURORA_TEST_CHECK(b.present(ButtonProps::corner_radius_row()));
    AURORA_TEST_CHECK(b.corner_radius(bare) == 11.5F);
    b.clear(ButtonProps::corner_radius_row());
    AURORA_TEST_CHECK_FALSE(b.present(ButtonProps::corner_radius_row()));
    AURORA_TEST_CHECK(b.corner_radius(bare) == shapes.small);
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
    // 零回归对账：未表态时 Button 的内边距读件解算到水平 = `large`、垂直 = `small`。
    // 本行是派生档（四边结构与单一标量档不同型，故由回落点按档拼装），生效值仍随主题间距档而变。
    const BuildContext bare;
    ButtonProbe b;
    AURORA_TEST_CHECK_FALSE(b.present(ButtonProps::padding_row()));  // 未表态才走主题档
    const EdgeInsets pad = b.padding(bare);
    AURORA_TEST_CHECK(pad.left == spacing.large);
    AURORA_TEST_CHECK(pad.right == spacing.large);
    AURORA_TEST_CHECK(pad.top == spacing.small);
    AURORA_TEST_CHECK(pad.bottom == spacing.small);
    // 两态语义的另一半：表态后不再跟随档，撤表态又回到档。
    b.set_padding(EdgeInsets{.left = 3.0F, .top = 5.0F, .right = 3.0F, .bottom = 5.0F});
    AURORA_TEST_CHECK(b.padding(bare).left == 3.0F);
    b.clear(ButtonProps::padding_row());
    AURORA_TEST_CHECK(b.padding(bare).left == spacing.large);
}

AURORA_TEST_CASE(control_metrics_reproduces_current_widget_metrics) {
    const ControlMetrics controls;
    // 可对账项：与控件公共头 / Widget 常量的现值逐位相等。
    // Divider 的线厚在控件侧改读 `controls.separator_thickness`（未表态时），对账同走带 ctx 的读件。
    const Divider divider;
    Json divider_props = Json::object();
    divider.serialize_props(divider_props);
    AURORA_TEST_CHECK_FALSE(divider_props.contains("thickness"));  // 未表态即不写键
    AURORA_TEST_CHECK(divider.thickness(BuildContext{}) == controls.separator_thickness);
    // 焦点环两枚令牌已由 `Widget::paint_content` 与 `dirty_bounds()` 同批接线：绘制期读值后缓存，
    // 标脏取两帧外扩的并集，换主题不留残影；「改令牌即随动、绘制与标脏同步」的成对像素断言在
    // 焦点环单测里。此处仍只钉声明值 2.0 / 2.0，保证默认档与改动前逐位相同。
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
