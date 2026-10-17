/// 测试类型: unit
/// 目标单元: include/aurora/window/title_bar_style.h
/// 测试说明: 覆盖 TitleBarStyle 默认值（=AdwaitaDark 预设）、六套视觉预设的数值出处与不变量、
/// 显示开关不变量、TitleBarButtonLayout 枚举互异性与指定初始化聚合语义

#include "aurora/window/title_bar_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_title_bar_style {

AURORA_TEST_CASE(default_style_equals_adwaita_dark_preset) {
    // 头文件契约：TitleBarStyle{} 的默认成员值即 CsdPreset::AdwaitaDark 预设（GNOME 暗色 header bar）。
    constexpr TitleBarStyle def{};
    constexpr TitleBarStyle preset = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_NEAR(def.height, 36.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(def.bg_active, Color{0x2E, 0x2E, 0x32});
    AURORA_TEST_CHECK_EQ(def.bg_inactive, Color{0x22, 0x22, 0x26});
    AURORA_TEST_CHECK_EQ(def.fg_active, Color{255, 255, 255});
    AURORA_TEST_CHECK_EQ(def.fg_inactive, Color{0x91, 0x91, 0x93});
    AURORA_TEST_CHECK_EQ(def.hover_tint, Color{255, 255, 255, 38});
    AURORA_TEST_CHECK_EQ(def.close_hover, Color{0xE0, 0x1B, 0x24});
    // 原生观感修复：窗口控制圆底常态底色、标题栏底缘分隔线、按压底色均取 libadwaita
    // 现行口径（暗色档）。
    AURORA_TEST_CHECK_EQ(def.button_tint, Color{255, 255, 255, 26});
    AURORA_TEST_CHECK_EQ(def.separator, Color{0x1D, 0x1D, 0x20});
    AURORA_TEST_CHECK_EQ(def.pressed_tint, Color{255, 255, 255, 77});
    AURORA_TEST_CHECK_EQ(def.close_pressed, Color{0xC0, 0x1B, 0x28});
    AURORA_TEST_CHECK_EQ(def.button_layout, TitleBarButtonLayout::Adwaita);
    AURORA_TEST_CHECK_EQ(preset.bg_active, def.bg_active);
    AURORA_TEST_CHECK_EQ(preset.button_layout, def.button_layout);
    AURORA_TEST_CHECK_NEAR(preset.height, def.height, 1e-4F);
}

AURORA_TEST_CASE(adwaita_light_preset_values) {
    constexpr TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0xFF, 0xFF, 0xFF});
    AURORA_TEST_CHECK_EQ(s.bg_inactive, Color{0xFA, 0xFA, 0xFB});
    AURORA_TEST_CHECK_EQ(s.fg_active, Color{0x33, 0x33, 0x34});
    AURORA_TEST_CHECK_EQ(s.fg_inactive, Color{0x7D, 0x7D, 0x80});
    // 亮色悬停罩是暗色白 α38 的镜像：黑 α38（libadwaita `$button_hover_color` = currentColor 15%）。
    AURORA_TEST_CHECK_EQ(s.hover_tint, Color{0, 0, 0, 38});
    AURORA_TEST_CHECK_EQ(s.close_hover, Color{0xE0, 0x1B, 0x24});
    // 原生观感修复：圆底常态底色 = currentColor 10%、按压底色 = 30%、底缘 shade 线。
    AURORA_TEST_CHECK_EQ(s.button_tint, Color{0, 0, 0, 26});
    AURORA_TEST_CHECK_EQ(s.pressed_tint, Color{0, 0, 0, 77});
    AURORA_TEST_CHECK_EQ(s.separator, Color{0xE0, 0xE0, 0xE0});
    AURORA_TEST_CHECK_EQ(s.button_layout, TitleBarButtonLayout::Adwaita);
    // 标题字重取 libadwaita `.title { font-weight: bold }` 的 700（走默认成员值，不钉 metrics）。
    AURORA_TEST_CHECK_EQ(s.metrics.title_font_weight, 700);
    // libadwaita `AdwHeaderBar` 的标题居中 ⇒ 默认 `center_title` 为 true，Adwaita 两档沿用。
    AURORA_TEST_CHECK_TRUE(s.center_title);
}

AURORA_TEST_CASE(windows_dark_preset_values) {
    constexpr TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0x20, 0x20, 0x20});
    AURORA_TEST_CHECK_EQ(s.bg_inactive, Color{0x19, 0x19, 0x19});
    AURORA_TEST_CHECK_EQ(s.fg_inactive, Color{0xA0, 0xA0, 0xA0});
    AURORA_TEST_CHECK_EQ(s.hover_tint, Color{255, 255, 255, 15});
    AURORA_TEST_CHECK_EQ(s.close_hover, Color{0xC4, 0x2B, 0x1C});
    // Windows 预设切换按钮视觉语言，并把度量钉到真机 DWM 标题栏口径（栏高不再沿用 Adwaita 基准 36dp）。
    AURORA_TEST_CHECK_EQ(s.button_layout, TitleBarButtonLayout::Windows);
    AURORA_TEST_CHECK_EQ(s.fg_active, Color{255, 255, 255});
    AURORA_TEST_CHECK_NEAR(s.height, 32.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.button_size, 46.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.icon_size, 16.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.title_font_size_pt, 9.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(s.metrics.title_font_weight, 400);
    // WinUI 标题左对齐 ⇒ 默认 true（Adwaita/Mac 口径）在 Windows 两档显式置回 false。
    AURORA_TEST_CHECK_FALSE(s.center_title);
}

AURORA_TEST_CASE(windows_light_preset_values) {
    constexpr TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::WindowsLight);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0xF3, 0xF3, 0xF3});
    AURORA_TEST_CHECK_EQ(s.bg_inactive, Color{0xEE, 0xEE, 0xEE});
    AURORA_TEST_CHECK_EQ(s.fg_active, Color::black());
    AURORA_TEST_CHECK_EQ(s.hover_tint, Color{0, 0, 0, 15});  // 暗色白 α15 的镜像
    AURORA_TEST_CHECK_EQ(s.close_hover, Color{0xC4, 0x2B, 0x1C});
    AURORA_TEST_CHECK_EQ(s.button_layout, TitleBarButtonLayout::Windows);
    // 度量与暗色档同源：两档只差配色，不差几何。
    constexpr TitleBarStyle dark = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    AURORA_TEST_CHECK_NEAR(s.height, dark.height, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.button_size, dark.metrics.button_size, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.icon_size, dark.metrics.icon_size, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.title_font_size_pt, dark.metrics.title_font_size_pt, 1e-4F);
    AURORA_TEST_CHECK_EQ(s.metrics.title_font_weight, dark.metrics.title_font_weight);
    // 关闭钮底色不用前景色（亮色档前景是黑，特征红才是身份色）。
    AURORA_TEST_CHECK_NE(s.close_hover, s.fg_active);
    AURORA_TEST_CHECK_EQ(s.center_title, dark.center_title);  // 两档同为左对齐
}

AURORA_TEST_CASE(mac_dark_preset_values) {
    constexpr TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::MacDark);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0x32, 0x32, 0x32});
    AURORA_TEST_CHECK_EQ(s.bg_inactive, Color{0x28, 0x28, 0x28});
    AURORA_TEST_CHECK_EQ(s.button_layout, TitleBarButtonLayout::Mac);
    // macOS 红绿灯悬停不换底色（只加淡罩），故 close_hover 等于普通悬停罩。
    AURORA_TEST_CHECK_EQ(s.close_hover, s.hover_tint);
    // 度量按 macOS 标准窗口口径钉值（不走栏高推导式）。
    AURORA_TEST_CHECK_NEAR(s.height, 28.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.button_size, 12.0F, 1e-4F);  // 红绿灯直径
    AURORA_TEST_CHECK_NEAR(s.metrics.button_gap, 8.0F, 1e-4F);  // 圆心距 20 − 直径 12
    AURORA_TEST_CHECK_NEAR(s.metrics.button_margin, 14.0F, 1e-4F);  // 圆心距左缘 20 − 半径 6
    AURORA_TEST_CHECK_NEAR(s.metrics.icon_size, 16.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.title_font_size_pt, 13.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(s.metrics.title_font_weight, 400);
    // macOS 窗口标题居中（与 libadwaita 同；WinUI 左对齐，故只有 Windows 两档置 false）。
    AURORA_TEST_CHECK_TRUE(s.center_title);
}

AURORA_TEST_CASE(mac_light_preset_values) {
    constexpr TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::MacLight);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0xEC, 0xEC, 0xEC});
    AURORA_TEST_CHECK_EQ(s.fg_active, Color::black());
    AURORA_TEST_CHECK_EQ(s.button_layout, TitleBarButtonLayout::Mac);
    // 度量与暗色档同源：两档只差配色，不差几何。
    constexpr TitleBarStyle dark = TitleBarStyle::from_preset(CsdPreset::MacDark);
    AURORA_TEST_CHECK_NEAR(s.height, dark.height, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.button_size, dark.metrics.button_size, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.button_gap, dark.metrics.button_gap, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.button_margin, dark.metrics.button_margin, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.metrics.icon_size, dark.metrics.icon_size, 1e-4F);
    AURORA_TEST_CHECK_EQ(s.metrics.title_font_weight, dark.metrics.title_font_weight);
    AURORA_TEST_CHECK_EQ(s.center_title, dark.center_title);
}

AURORA_TEST_CASE(presets_keep_all_show_flags_enabled) {
    // 三个预设都不得关闭任何按钮/标题开关：几何层的显隐由 show_* 与 resizable 决定，
    // 预设只管颜色与布局语言。
    constexpr TitleBarStyle dark = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    constexpr TitleBarStyle light = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    constexpr TitleBarStyle windows = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    for (const TitleBarStyle *s : {&dark, &light, &windows}) {
        AURORA_TEST_CHECK_TRUE(s->show_minimize);
        AURORA_TEST_CHECK_TRUE(s->show_maximize);
        AURORA_TEST_CHECK_TRUE(s->show_close);
        AURORA_TEST_CHECK_TRUE(s->show_title);
    }
}

AURORA_TEST_CASE(button_layout_enum_distinguishes_three_languages) {
    // 三种视觉语言两两互异（几何层据此分派布局算法）。
    constexpr TitleBarButtonLayout adwaita = TitleBarButtonLayout::Adwaita;
    constexpr TitleBarButtonLayout windows = TitleBarButtonLayout::Windows;
    constexpr TitleBarButtonLayout mac = TitleBarButtonLayout::Mac;
    AURORA_TEST_CHECK_NE(adwaita, windows);
    AURORA_TEST_CHECK_NE(adwaita, mac);
    AURORA_TEST_CHECK_NE(windows, mac);
}

AURORA_TEST_CASE(designated_initialization_overrides_selected_fields_only) {
    // 纯数据聚合：指定初始化只覆写列出字段，其余保持默认（可直接 TitleBarStyle{...} 定制）；
    // 指定初始化器须按成员声明顺序排列（height → button_layout → center_title）。
    constexpr TitleBarStyle ordered{.height = 48.0F, .button_layout = TitleBarButtonLayout::Mac, .center_title = true};
    AURORA_TEST_CHECK_NEAR(ordered.height, 48.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(ordered.center_title);
    AURORA_TEST_CHECK_EQ(ordered.button_layout, TitleBarButtonLayout::Mac);
    // 未列出的字段回落默认（AdwaitaDark 基准）。
    AURORA_TEST_CHECK_EQ(ordered.bg_active, Color{0x2E, 0x2E, 0x32});
    AURORA_TEST_CHECK_EQ(ordered.close_hover, Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK_TRUE(ordered.show_minimize);
    AURORA_TEST_CHECK_TRUE(ordered.show_close);
}

AURORA_TEST_CASE(active_inactive_pairs_dim_for_focus_loss) {
    // 激活/失焦成对颜色语义：失焦背景更深、失焦前景更暗（绘制层据此自动变暗）。
    constexpr TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_NE(s.bg_active, s.bg_inactive);
    AURORA_TEST_CHECK_NE(s.fg_active, s.fg_inactive);
    AURORA_TEST_CHECK_EQ(s.fg_active, Color::white());
    // 失焦背景亮度低于激活背景（逐通道单调）。
    AURORA_TEST_CHECK_TRUE(s.bg_inactive.r < s.bg_active.r);
    AURORA_TEST_CHECK_TRUE(s.bg_inactive.g < s.bg_active.g);
    AURORA_TEST_CHECK_TRUE(s.bg_inactive.b < s.bg_active.b);
}

AURORA_TEST_CASE(from_preset_adwaita_dark_equals_default_construction) {
    // 基准契约：AdwaitaDark 与默认构造逐值相同——预设迁移不会改变 TitleBarStyle{} 既有调用方。
    constexpr TitleBarStyle def{};
    constexpr TitleBarStyle preset = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_EQ(preset.bg_active, def.bg_active);
    AURORA_TEST_CHECK_EQ(preset.bg_inactive, def.bg_inactive);
    AURORA_TEST_CHECK_EQ(preset.fg_active, def.fg_active);
    AURORA_TEST_CHECK_EQ(preset.fg_inactive, def.fg_inactive);
    AURORA_TEST_CHECK_EQ(preset.hover_tint, def.hover_tint);
    AURORA_TEST_CHECK_EQ(preset.close_hover, def.close_hover);
    AURORA_TEST_CHECK_EQ(preset.pressed_tint, def.pressed_tint);
    AURORA_TEST_CHECK_EQ(preset.close_pressed, def.close_pressed);
    AURORA_TEST_CHECK_EQ(preset.button_layout, def.button_layout);
    // 度量与居中开关同样逐值相同：Adwaita 档**刻意不钉** metrics——钉值会让 `TitleBarStyle{}`
    // 与预设分叉（Windows / Mac 档钉值不受此约束，它们本就是另一套视觉语言）。其原生口径
    // （按钮盒 26dp、间距与外边距 6dp）由几何层的推导式直接给出，故两处读数恒等。
    AURORA_TEST_CHECK_NEAR(preset.metrics.button_size, def.metrics.button_size, 1e-4F);
    AURORA_TEST_CHECK_NEAR(preset.metrics.button_gap, def.metrics.button_gap, 1e-4F);
    AURORA_TEST_CHECK_NEAR(preset.metrics.button_margin, def.metrics.button_margin, 1e-4F);
    AURORA_TEST_CHECK_NEAR(preset.metrics.icon_size, def.metrics.icon_size, 1e-4F);
    AURORA_TEST_CHECK_NEAR(preset.metrics.title_font_size_pt, def.metrics.title_font_size_pt, 1e-4F);
    AURORA_TEST_CHECK_EQ(preset.metrics.title_font_weight, def.metrics.title_font_weight);
    AURORA_TEST_CHECK_EQ(preset.center_title, def.center_title);
}

AURORA_TEST_CASE(six_presets_table) {
    // 六套预设逐格锁值：明暗成对（背景明暗反转）、语言分派正确、按压强于悬停、显隐全开。
    struct Row {
        CsdPreset preset;
        TitleBarButtonLayout layout;
        bool dark;  // true = 暗色档（白前景 / 亮背景偏暗）
        float height;  // 该档栏高基准（Windows 档按真机 DWM 实测 32dp，其余沿用 36dp）
        const char *name;  // 失败时定位用（测试输出须 ASCII）
    };
    const Row rows[] = {
        {.preset = CsdPreset::AdwaitaLight,
         .layout = TitleBarButtonLayout::Adwaita,
         .dark = false,
         .height = 36.0F,
         .name = "adwaita-light"},
        {.preset = CsdPreset::AdwaitaDark,
         .layout = TitleBarButtonLayout::Adwaita,
         .dark = true,
         .height = 36.0F,
         .name = "adwaita-dark"},
        {.preset = CsdPreset::WindowsLight,
         .layout = TitleBarButtonLayout::Windows,
         .dark = false,
         .height = 32.0F,
         .name = "windows-light"},
        {.preset = CsdPreset::WindowsDark,
         .layout = TitleBarButtonLayout::Windows,
         .dark = true,
         .height = 32.0F,
         .name = "windows-dark"},
        {.preset = CsdPreset::MacLight,
         .layout = TitleBarButtonLayout::Mac,
         .dark = false,
         .height = 28.0F,
         .name = "mac-light"},
        {.preset = CsdPreset::MacDark,
         .layout = TitleBarButtonLayout::Mac,
         .dark = true,
         .height = 28.0F,
         .name = "mac-dark"},
    };
    for (const Row &r : rows) {
        const TitleBarStyle s = TitleBarStyle::from_preset(r.preset);
        AURORA_TEST_CHECK_EQ(s.button_layout, r.layout);
        // 显隐全开：预设只管颜色与布局，显隐属调用方定制面。
        AURORA_TEST_CHECK_TRUE(s.show_minimize);
        AURORA_TEST_CHECK_TRUE(s.show_maximize);
        AURORA_TEST_CHECK_TRUE(s.show_close);
        AURORA_TEST_CHECK_TRUE(s.show_title);
        // 明暗档位：暗色档背景暗于亮色档背景（逐通道单调），前景黑白相应反转。
        AURORA_TEST_CHECK_TRUE(s.bg_active.r >= (r.dark ? 0x20U : 0xE0U));
        AURORA_TEST_CHECK_TRUE(s.bg_active.r <= (r.dark ? 0x40U : 0xFFU));
        // 明暗档位：暗色档前景纯白；亮色档前景近黑（AdwaitaLight 取真机灰 0x333334 而非纯黑）。
        if (r.dark) {
            AURORA_TEST_CHECK_EQ(s.fg_active, Color{255, 255, 255});
        } else {
            AURORA_TEST_CHECK_TRUE(s.fg_active.r <= 0x40U);
            AURORA_TEST_CHECK_TRUE(s.fg_active.g <= 0x40U);
            AURORA_TEST_CHECK_TRUE(s.fg_active.b <= 0x40U);
        }
        // 按压强于悬停（同色相下 alpha 单调）：非关闭钮与关闭钮各自成对。
        AURORA_TEST_CHECK_TRUE(s.pressed_tint.a >= s.hover_tint.a);
        AURORA_TEST_CHECK_TRUE(s.close_pressed.a >= s.close_hover.a);
        // 栏高按档位基准：Windows 档对齐真机 32dp、Mac 档对齐 macOS 标准窗口 28dp，
        // Adwaita 维持既有 36dp（= `TitleBarStyle{}` 默认成员值，契约要求两者同值）。
        AURORA_TEST_CHECK_NEAR(s.height, r.height, 1e-4F);
        (void)r.name;  // 预留失败定位；当前断言消息不含名字
    }
}

AURORA_TEST_CASE(light_dark_pairs_share_close_red_across_languages) {
    // 特征红跨明暗恒定（Adwaita red3 / Windows 关闭红），这是各视觉语言的身份色。
    constexpr TitleBarStyle al = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    constexpr TitleBarStyle ad = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_EQ(al.close_hover, ad.close_hover);
    AURORA_TEST_CHECK_EQ(al.close_hover, Color{0xE0, 0x1B, 0x24});
    constexpr TitleBarStyle wl = TitleBarStyle::from_preset(CsdPreset::WindowsLight);
    constexpr TitleBarStyle wd = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    AURORA_TEST_CHECK_EQ(wl.close_hover, wd.close_hover);
    AURORA_TEST_CHECK_EQ(wl.close_hover, Color{0xC4, 0x2B, 0x1C});
}

}  // namespace aurora::test_cases::utest_title_bar_style
