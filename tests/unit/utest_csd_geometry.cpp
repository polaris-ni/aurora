/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/csd_geometry.h
/// 测试说明: 覆盖 Wayland CSD 阴影边距架构的纯值逻辑——边距塌缩矩阵（Normal/最大化/全屏/
/// 平铺 × 是否客户端装饰）、整幅表面尺寸换算、content_inset 四分量口径、表面→内容坐标映射、
/// 八向缩放热区分类（边/角/带外/零边距）与标题栏垂直带判定。无 Wayland 连接依赖。

#include "aurora/core/types.h"
#include "aurora/window/detail/csd_geometry.h"
#include "aurora/window/window_state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_csd_geometry {

namespace {
namespace csd = aurora::csd;
}  // namespace

AURORA_TEST_CASE(shadow_margin_collapse_matrix) {
    using WindowMode::FullScreen;
    using WindowMode::Maximized;
    using WindowMode::Minimized;
    using WindowMode::Normal;

    // Normal + 客户端装饰（标题栏或 Borderless）：唯一带边距的组合。
    AURORA_TEST_CHECK_TRUE(csd::has_shadow_margin(true, Normal, false));
    AURORA_TEST_CHECK_EQ(csd::shadow_margin_dp(true, Normal, false), csd::AURORA_SHADOW_MARGIN_DP);
    AURORA_TEST_CHECK_GE(csd::AURORA_SHADOW_MARGIN_DP, 8);  // Wayland 边缘缩放热区可用性下限

    // 塌缩条件枚举：非客户端装饰（SSD / Frameless）、最大化、全屏、挂起、合成器平铺。
    AURORA_TEST_CHECK_FALSE(csd::has_shadow_margin(false, Normal, false));  // SSD/Frameless
    AURORA_TEST_CHECK_FALSE(csd::has_shadow_margin(true, Maximized, false));
    AURORA_TEST_CHECK_FALSE(csd::has_shadow_margin(true, FullScreen, false));
    AURORA_TEST_CHECK_FALSE(csd::has_shadow_margin(true, Minimized, false));
    AURORA_TEST_CHECK_FALSE(csd::has_shadow_margin(true, Normal, true));  // 任一 TILED_*
    // 平铺与其它塌缩态叠加不复活边距。
    AURORA_TEST_CHECK_FALSE(csd::has_shadow_margin(true, Maximized, true));
    AURORA_TEST_CHECK_EQ(csd::shadow_margin_dp(false, Normal, false), 0);
    AURORA_TEST_CHECK_EQ(csd::shadow_margin_dp(true, FullScreen, true), 0);
}

AURORA_TEST_CASE(surface_size_expands_by_two_margins) {
    const Size full = csd::surface_size(Size{.width = 800.0F, .height = 600.0F}, csd::AURORA_SHADOW_MARGIN_DP);
    AURORA_TEST_CHECK_EQ(full.width, 820.0F);
    AURORA_TEST_CHECK_EQ(full.height, 620.0F);

    // 塌缩态：整幅表面 = 内容尺寸（逐值相等，不放大）。
    const Size collapsed = csd::surface_size(Size{.width = 800.0F, .height = 600.0F}, 0);
    AURORA_TEST_CHECK_EQ(collapsed.width, 800.0F);
    AURORA_TEST_CHECK_EQ(collapsed.height, 600.0F);
}

AURORA_TEST_CASE(content_inset_matches_decoration_matrix) {
    constexpr float tb = 32.0F;
    const int m = csd::AURORA_SHADOW_MARGIN_DP;
    const auto mf = static_cast<float>(m);

    // Normal + 标题栏：{m, m+tb, m, m}——顶分量是边距与标题栏之和。
    const EdgeInsets titled = csd::content_inset_for(true, tb, m, WindowMode::Normal);
    AURORA_TEST_CHECK_EQ(titled.left, mf);
    AURORA_TEST_CHECK_EQ(titled.top, mf + tb);
    AURORA_TEST_CHECK_EQ(titled.right, mf);
    AURORA_TEST_CHECK_EQ(titled.bottom, mf);

    // Borderless（无标题栏）：四边各一份边距，无标题栏叠加。
    const EdgeInsets borderless = csd::content_inset_for(false, tb, m, WindowMode::Normal);
    AURORA_TEST_CHECK_EQ(borderless.left, mf);
    AURORA_TEST_CHECK_EQ(borderless.top, mf);
    AURORA_TEST_CHECK_EQ(borderless.right, mf);
    AURORA_TEST_CHECK_EQ(borderless.bottom, mf);

    // 最大化：边距塌缩、标题栏保留 {0,tb,0,0}。
    const EdgeInsets maximized = csd::content_inset_for(true, tb, 0, WindowMode::Maximized);
    AURORA_TEST_CHECK_EQ(maximized.left, 0.0F);
    AURORA_TEST_CHECK_EQ(maximized.top, tb);
    AURORA_TEST_CHECK_EQ(maximized.right, 0.0F);
    AURORA_TEST_CHECK_EQ(maximized.bottom, 0.0F);

    // 平铺（mode 仍为 Normal，仅 margin 传 0）：口径同最大化，标题栏不塌。
    const EdgeInsets tiled = csd::content_inset_for(true, tb, 0, WindowMode::Normal);
    AURORA_TEST_CHECK_EQ(tiled.top, tb);
    AURORA_TEST_CHECK_EQ(tiled.left, 0.0F);

    // 全屏：标题栏退化为悬停揭示覆盖层，不回流布局 → 全零。
    const EdgeInsets fullscreen = csd::content_inset_for(true, tb, 0, WindowMode::FullScreen);
    AURORA_TEST_CHECK_EQ(fullscreen.left, 0.0F);
    AURORA_TEST_CHECK_EQ(fullscreen.top, 0.0F);
    AURORA_TEST_CHECK_EQ(fullscreen.right, 0.0F);
    AURORA_TEST_CHECK_EQ(fullscreen.bottom, 0.0F);

    // SSD/Frameless：无标题栏且边距 0 → 全零。
    const EdgeInsets ssd = csd::content_inset_for(false, tb, 0, WindowMode::Normal);
    AURORA_TEST_CHECK_EQ(ssd.top, 0.0F);
    AURORA_TEST_CHECK_EQ(ssd.left, 0.0F);
}

AURORA_TEST_CASE(surface_to_content_subtracts_margin) {
    AURORA_TEST_CHECK_EQ(csd::surface_to_content(10.0, 10), 0.0);    // 表面内容原点
    AURORA_TEST_CHECK_EQ(csd::surface_to_content(0.0, 10), -10.0);   // 表面左上角（margin 外缘）
    AURORA_TEST_CHECK_EQ(csd::surface_to_content(42.5, 10), 32.5);
    // 塌缩态映射恒等（指针坐标不偏移）。
    AURORA_TEST_CHECK_EQ(csd::surface_to_content(123.0, 0), 123.0);
}

AURORA_TEST_CASE(resize_zone_classifies_eight_directions) {
    constexpr double w = 100.0;
    constexpr double h = 80.0;
    constexpr int margin = 10;
    using Z = csd::CsdResizeZone;

    // 四条边（内容坐标域外延环的中段，避开角部 10×10 方）。
    AURORA_TEST_CHECK(csd::classify_resize_zone(-5.0, 40.0, w, h, margin) == Z::Left);
    AURORA_TEST_CHECK(csd::classify_resize_zone(105.0, 40.0, w, h, margin) == Z::Right);
    AURORA_TEST_CHECK(csd::classify_resize_zone(50.0, -5.0, w, h, margin) == Z::Top);
    AURORA_TEST_CHECK(csd::classify_resize_zone(50.0, 85.0, w, h, margin) == Z::Bottom);
    // 四个角。
    AURORA_TEST_CHECK(csd::classify_resize_zone(-5.0, -5.0, w, h, margin) == Z::TopLeft);
    AURORA_TEST_CHECK(csd::classify_resize_zone(105.0, -5.0, w, h, margin) == Z::TopRight);
    AURORA_TEST_CHECK(csd::classify_resize_zone(-5.0, 85.0, w, h, margin) == Z::BottomLeft);
    AURORA_TEST_CHECK(csd::classify_resize_zone(105.0, 85.0, w, h, margin) == Z::BottomRight);

    // 带边界：环为半开区间 [-m,0)/(W,W+m)，恰在内容边上的点属于内容而非热区；
    // 环外（超出 margin 厚度）不带方向。
    AURORA_TEST_CHECK(csd::classify_resize_zone(0.0, 40.0, w, h, margin) == Z::None);
    AURORA_TEST_CHECK(csd::classify_resize_zone(-10.0, 40.0, w, h, margin) == Z::Left);  // 含外缘
    AURORA_TEST_CHECK(csd::classify_resize_zone(-11.0, 40.0, w, h, margin) == Z::None);
    AURORA_TEST_CHECK(csd::classify_resize_zone(110.0, 40.0, w, h, margin) == Z::None);  // x<W+m 为严格小于
    AURORA_TEST_CHECK(csd::classify_resize_zone(50.0, 40.0, w, h, margin) == Z::None);   // 内容内部

    // 零边距（塌缩态）：任何坐标都不分类，含负坐标（此时指针不可能在表面外）。
    AURORA_TEST_CHECK(csd::classify_resize_zone(-1.0, 40.0, w, h, 0) == Z::None);
    AURORA_TEST_CHECK(csd::classify_resize_zone(50.0, -1.0, w, h, 0) == Z::None);
}

AURORA_TEST_CASE(title_bar_band_uses_content_domain) {
    constexpr float tb = 28.0F;
    AURORA_TEST_CHECK_FALSE(csd::point_in_title_bar(-1.0, tb));
    AURORA_TEST_CHECK_TRUE(csd::point_in_title_bar(0.0, tb));    // 含内容上边界
    AURORA_TEST_CHECK_TRUE(csd::point_in_title_bar(27.9, tb));
    AURORA_TEST_CHECK_FALSE(csd::point_in_title_bar(28.0, tb));  // 严格小于标题栏高度
    AURORA_TEST_CHECK_FALSE(csd::point_in_title_bar(60.0, tb));
    // 零高度（无标题栏）恒 false。
    AURORA_TEST_CHECK_FALSE(csd::point_in_title_bar(0.0, 0.0F));
}

}  // namespace aurora::test_cases::utest_csd_geometry
