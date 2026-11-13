/// 测试类型: unit
/// 目标单元: include/aurora/theming/shape_scale.h + spacing_scale.h + control_metrics.h + motion_scale.h
///           （并入 color_roles.h 的对照表）
/// 测试说明: 覆盖度量令牌各组的三条不变量——(1) 档位取值逐位复现令牌层落地前的硬编码且沿尺寸轴单调
/// 递增（接线批把控件接到同档后观感逐位不变）；(2) 度量组不分明暗，故只持一列（声明处默认值即唯一列）；
/// (3) 各组「成员指针 → 短名」对照表的登记完整性：每个档名恰登记一次、短名统一以 `theme.<组名>.` 开头、
/// 反查与表项一致、未登记成员指针（含空指针）回落空串而不猜名。对照表还须编译期可查——属性行的
/// 活令牌回落点在编译期烧名，`token_name` 退化成运行期查表即意味着回落点无法进 constexpr 表。

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/theming/color_roles.h"
#include "aurora/theming/control_metrics.h"
#include "aurora/theming/motion_scale.h"
#include "aurora/theming/shape_scale.h"
#include "aurora/theming/spacing_scale.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_scale_tokens {

namespace {

/// @brief 拼出期望短名：`theme.shapes.` + `small` → `theme.shapes.small`。
/// @param prefix 组前缀（含尾点）。
/// @param suffix 档名。
/// @return 拼接后的完整短名。
[[nodiscard]] auto full_name(const std::string_view prefix, const std::string_view suffix) -> std::string {
    return std::string{prefix} + std::string{suffix};
}

/// @brief 校验一张「成员指针 → 短名」对照表的登记完整性与反查一致性。
///
/// 漏登记（档名清单里查不到）、多登记（枚数不符）、重名（同名命中两次）都会红；表与实际字段的
/// 同源关系统由令牌登记表门禁在源码侧校验，本函数校验的是运行可见的那一半。
/// @tparam Scale 令牌结构体（须有 `AURORA_TOKENS` 表与 `token_name` 反查）。
/// @param prefix 期望短名前缀（含尾点），如 `theme.shapes.`。
/// @param suffixes 期望的档名全集。
template <class Scale>
auto check_token_table(const std::string_view prefix, std::initializer_list<std::string_view> suffixes) -> void {
    const auto &tokens = Scale::AURORA_TOKENS;
    AURORA_TEST_CHECK_EQ(tokens.size(), suffixes.size());  // 枚数 = 档名清单长度
    for (const std::string_view suffix : suffixes) {
        const std::string expected = full_name(prefix, suffix);
        std::size_t hits = 0;
        for (const auto &[member, name] : tokens) {
            static_cast<void>(member);
            if (name == expected) {
                ++hits;
            }
        }
        AURORA_TEST_CHECK_EQ(hits, 1U);  // 恰登记一次：0 = 漏登记，2 = 重名
    }
    for (const auto &[member, name] : tokens) {
        AURORA_TEST_CHECK(name.starts_with(prefix));  // 组前缀统一
        AURORA_TEST_CHECK_FALSE(name.empty());
        AURORA_TEST_CHECK_EQ(Scale::token_name(member), name);  // 反查命中自身
    }
    AURORA_TEST_CHECK(Scale::token_name(nullptr).empty());  // 未登记回落空串，不产出猜测名
}

}  // namespace

// 编译期可查：回落点须在 constexpr 表里烧名，运行期查表形态在此即编译失败。
static_assert(ShapeScale::token_name(&ShapeScale::small) == "theme.shapes.small");
static_assert(SpacingScale::token_name(&SpacingScale::large) == "theme.spacing.large");
static_assert(ControlMetrics::token_name(&ControlMetrics::indicator_size) == "theme.controls.indicator_size");
static_assert(MotionScale::token_name(&MotionScale::medium) == "theme.motion.medium");
static_assert(ColorRoles::token_name(&ColorRoles::primary) == "theme.primary");

AURORA_TEST_CASE(shape_tiers_anchor_current_corner_radius_constants) {
    // 六档取值逐位复现令牌层落地前的硬编码：small = 6 dp 即 Button 现行圆角，余下各档为风格预留位。
    constexpr ShapeScale s;
    AURORA_TEST_CHECK_EQ(s.none, 0.0F);
    AURORA_TEST_CHECK_EQ(s.extra_small, 4.0F);
    AURORA_TEST_CHECK_EQ(s.small, 6.0F);
    AURORA_TEST_CHECK_EQ(s.medium, 8.0F);
    AURORA_TEST_CHECK_EQ(s.large, 12.0F);
    AURORA_TEST_CHECK_EQ(s.extra_large, 16.0F);
    AURORA_TEST_CHECK_LT(s.none, s.extra_small);  // 档位沿尺寸轴单调递增
    AURORA_TEST_CHECK_LT(s.extra_small, s.small);
    AURORA_TEST_CHECK_LT(s.small, s.medium);
    AURORA_TEST_CHECK_LT(s.medium, s.large);
    AURORA_TEST_CHECK_LT(s.large, s.extra_large);
    check_token_table<ShapeScale>("theme.shapes.", {"none", "extra_small", "small", "medium", "large", "extra_large"});
}

AURORA_TEST_CASE(spacing_tiers_anchor_current_gap_constants) {
    // 七档取值逐位复现令牌层落地前的硬编码：large / small = 12 / 6 即 Button 现行水平 / 垂直内边距，
    // medium = 8 即图表留白。
    constexpr SpacingScale s;
    AURORA_TEST_CHECK_EQ(s.none, 0.0F);
    AURORA_TEST_CHECK_EQ(s.extra_small, 4.0F);
    AURORA_TEST_CHECK_EQ(s.small, 6.0F);
    AURORA_TEST_CHECK_EQ(s.medium, 8.0F);
    AURORA_TEST_CHECK_EQ(s.large, 12.0F);
    AURORA_TEST_CHECK_EQ(s.extra_large, 16.0F);
    AURORA_TEST_CHECK_EQ(s.huge, 24.0F);
    AURORA_TEST_CHECK_LT(s.none, s.extra_small);
    AURORA_TEST_CHECK_LT(s.extra_small, s.small);
    AURORA_TEST_CHECK_LT(s.small, s.medium);
    AURORA_TEST_CHECK_LT(s.medium, s.large);
    AURORA_TEST_CHECK_LT(s.large, s.extra_large);
    AURORA_TEST_CHECK_LT(s.extra_large, s.huge);
    check_token_table<SpacingScale>("theme.spacing.",
                                    {"none", "extra_small", "small", "medium", "large", "extra_large", "huge"});
}

AURORA_TEST_CASE(control_metrics_anchor_current_control_constants) {
    // 十二项逐位复现令牌层落地前的硬编码（出处见头文件字段注记）；本用例钉住「接线前后取值不变」，
    // 加进对照表后短名与 aurora_api.json 里既有的 theme.controls.<项> 拼法同源。
    constexpr ControlMetrics c;
    AURORA_TEST_CHECK_EQ(c.control_height, 30.0F);
    AURORA_TEST_CHECK_EQ(c.control_height_small, 24.0F);
    AURORA_TEST_CHECK_EQ(c.min_touch_target, 40.0F);
    AURORA_TEST_CHECK_EQ(c.menu_item_height, 26.0F);
    AURORA_TEST_CHECK_EQ(c.focus_ring_width, 2.0F);
    AURORA_TEST_CHECK_EQ(c.focus_ring_inset, 2.0F);
    AURORA_TEST_CHECK_EQ(c.indicator_size, 20.0F);
    AURORA_TEST_CHECK_EQ(c.scrollbar_thickness, 4.0F);
    AURORA_TEST_CHECK_EQ(c.separator_thickness, 1.0F);
    AURORA_TEST_CHECK_EQ(c.border_thickness, 1.0F);
    AURORA_TEST_CHECK_EQ(c.icon_default_size, 24.0F);
    AURORA_TEST_CHECK_EQ(c.content_padding, 8.0F);
    AURORA_TEST_CHECK(c == ControlMetrics{});
    check_token_table<ControlMetrics>(
        "theme.controls.", {"control_height", "control_height_small", "min_touch_target", "menu_item_height",
                            "focus_ring_width", "focus_ring_inset", "indicator_size", "scrollbar_thickness",
                            "separator_thickness", "border_thickness", "icon_default_size", "content_padding"});
}

AURORA_TEST_CASE(motion_tiers_anchor_library_default_duration_and_none_is_reduce_motion) {
    // medium = 150 ms 锚定现行滚动视口默认时长；none = 0 是「减弱动态效果」的短路档（一步落端点）。
    constexpr MotionScale m;
    AURORA_TEST_CHECK(m.none == Duration::from_ms(0.0));
    AURORA_TEST_CHECK(m.extra_small == Duration::from_ms(50.0));
    AURORA_TEST_CHECK(m.small == Duration::from_ms(100.0));
    AURORA_TEST_CHECK(m.medium == Duration::from_ms(150.0));
    AURORA_TEST_CHECK(m.medium == Duration::from_seconds(0.15));
    AURORA_TEST_CHECK(m.large == Duration::from_ms(250.0));
    AURORA_TEST_CHECK(m.extra_large == Duration::from_ms(400.0));
    AURORA_TEST_CHECK_LT(m.none.seconds, m.extra_small.seconds);  // 时长沿梯度单调递增
    AURORA_TEST_CHECK_LT(m.extra_small.seconds, m.small.seconds);
    AURORA_TEST_CHECK_LT(m.small.seconds, m.medium.seconds);
    AURORA_TEST_CHECK_LT(m.medium.seconds, m.large.seconds);
    AURORA_TEST_CHECK_LT(m.large.seconds, m.extra_large.seconds);
    check_token_table<MotionScale>("theme.motion.", {"none", "extra_small", "small", "medium", "large", "extra_large"});
}

AURORA_TEST_CASE(measure_scales_are_scheme_independent_single_column) {
    // 度量不分明暗：各组各自只持一列，比较靠默认构造与基准列同值（无 second column）。
    constexpr ShapeScale shape;
    constexpr SpacingScale spacing;
    constexpr ControlMetrics controls;
    constexpr MotionScale motion;
    AURORA_TEST_CHECK(shape == ShapeScale{});  // 声明处默认值即唯一列，无第二列可偏离
    AURORA_TEST_CHECK(spacing == SpacingScale{});
    AURORA_TEST_CHECK(controls == ControlMetrics{});
    AURORA_TEST_CHECK(motion == MotionScale{});
    AURORA_TEST_CHECK(ShapeScale{.medium = 1.0F} != shape);  // operator== 逐字段判等，不是恒等
    AURORA_TEST_CHECK(MotionScale{.medium = Duration::from_ms(1.0)} != motion);
}

AURORA_TEST_CASE(color_roles_token_table_covers_all_roles) {
    // 色角色的短名沿用旧 stringly 令牌命名（`theme.<role>`，无组段），迁移批的 default_expr 逐位不变。
    check_token_table<ColorRoles>("theme.", {"primary",
                                             "on_primary",
                                             "primary_container",
                                             "on_primary_container",
                                             "secondary",
                                             "on_secondary",
                                             "secondary_container",
                                             "on_secondary_container",
                                             "tertiary",
                                             "on_tertiary",
                                             "tertiary_container",
                                             "on_tertiary_container",
                                             "error",
                                             "on_error",
                                             "error_container",
                                             "on_error_container",
                                             "surface",
                                             "surface_dim",
                                             "surface_bright",
                                             "surface_container_lowest",
                                             "surface_container_low",
                                             "surface_container",
                                             "surface_container_high",
                                             "surface_container_highest",
                                             "on_surface",
                                             "on_surface_variant",
                                             "surface_variant",
                                             "outline",
                                             "outline_variant",
                                             "inverse_surface",
                                             "inverse_on_surface",
                                             "inverse_primary",
                                             "scrim",
                                             "shadow",
                                             "surface_tint",
                                             "focus_ring",
                                             "hover",
                                             "pressed",
                                             "scrollbar_thumb",
                                             "scrollbar_track"});
}

}  // namespace aurora::test_cases::utest_theme_scale_tokens
