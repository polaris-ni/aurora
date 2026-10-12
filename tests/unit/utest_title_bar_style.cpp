/// 测试类型: unit
/// 目标单元: include/aurora/window/title_bar_style.h
/// 测试说明: 覆盖 TitleBarStyle 默认值（=AdwaitaDark 预设）、六套视觉预设的数值出处与不变量、
/// 显示开关不变量、TitleBarButtonLayout 枚举互异性与指定初始化聚合语义

#include "aurora/window/title_bar_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_title_bar_style {

AURORA_TEST_CASE(default_style_equals_adwaita_dark_preset) {
    // 头文件契约：TitleBarStyle{} 的默认成员值即 CsdPreset::AdwaitaDark 预设（GNOME 暗色 header bar）。
    const TitleBarStyle def{};
    const TitleBarStyle preset = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_NEAR(def.height, 36.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(def.bg_active, Color{0x30, 0x30, 0x30});
    AURORA_TEST_CHECK_EQ(def.bg_inactive, Color{0x24, 0x24, 0x24});
    AURORA_TEST_CHECK_EQ(def.fg_active, Color{255, 255, 255});
    AURORA_TEST_CHECK_EQ(def.fg_inactive, Color{0x9A, 0x99, 0x96});
    AURORA_TEST_CHECK_EQ(def.hover_tint, Color{255, 255, 255, 32});
    AURORA_TEST_CHECK_EQ(def.close_hover, Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK_EQ(def.button_layout, TitleBarButtonLayout::Adwaita);
    AURORA_TEST_CHECK_EQ(preset.bg_active, def.bg_active);
    AURORA_TEST_CHECK_EQ(preset.button_layout, def.button_layout);
    AURORA_TEST_CHECK_NEAR(preset.height, def.height, 1e-4F);
}

AURORA_TEST_CASE(adwaita_light_preset_values) {
    const TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0xEB, 0xEB, 0xEB});
    AURORA_TEST_CHECK_EQ(s.bg_inactive, Color{0xE0, 0xE0, 0xE0});
    AURORA_TEST_CHECK_EQ(s.fg_active, Color::black());
    AURORA_TEST_CHECK_EQ(s.fg_inactive, Color{0x92, 0x95, 0x95});
    // 亮色悬停罩是暗色白 α32 的镜像：黑 α32。
    AURORA_TEST_CHECK_EQ(s.hover_tint, Color{0, 0, 0, 32});
    AURORA_TEST_CHECK_EQ(s.close_hover, Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK_EQ(s.button_layout, TitleBarButtonLayout::Adwaita);
}

AURORA_TEST_CASE(windows_dark_preset_values) {
    const TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0x20, 0x20, 0x20});
    AURORA_TEST_CHECK_EQ(s.bg_inactive, Color{0x19, 0x19, 0x19});
    AURORA_TEST_CHECK_EQ(s.fg_inactive, Color{0xA0, 0xA0, 0xA0});
    AURORA_TEST_CHECK_EQ(s.hover_tint, Color{255, 255, 255, 26});
    AURORA_TEST_CHECK_EQ(s.close_hover, Color{0xC4, 0x2B, 0x1C});
    // Windows 预设切换按钮视觉语言；前景色与高度保持默认值。
    AURORA_TEST_CHECK_EQ(s.button_layout, TitleBarButtonLayout::Windows);
    AURORA_TEST_CHECK_EQ(s.fg_active, Color{255, 255, 255});
    AURORA_TEST_CHECK_NEAR(s.height, 36.0F, 1e-4F);
}

AURORA_TEST_CASE(presets_keep_all_show_flags_enabled) {
    // 三个预设都不得关闭任何按钮/标题开关：几何层的显隐由 show_* 与 resizable 决定，
    // 预设只管颜色与布局语言。
    const TitleBarStyle dark = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    const TitleBarStyle light = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    const TitleBarStyle windows = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    for (const TitleBarStyle *s : {&dark, &light, &windows}) {
        AURORA_TEST_CHECK_TRUE(s->show_minimize);
        AURORA_TEST_CHECK_TRUE(s->show_maximize);
        AURORA_TEST_CHECK_TRUE(s->show_close);
        AURORA_TEST_CHECK_TRUE(s->show_title);
    }
}

AURORA_TEST_CASE(button_layout_enum_distinguishes_three_languages) {
    // 三种视觉语言两两互异（几何层据此分派布局算法）。
    const TitleBarButtonLayout adwaita = TitleBarButtonLayout::Adwaita;
    const TitleBarButtonLayout windows = TitleBarButtonLayout::Windows;
    const TitleBarButtonLayout mac = TitleBarButtonLayout::Mac;
    AURORA_TEST_CHECK_NE(adwaita, windows);
    AURORA_TEST_CHECK_NE(adwaita, mac);
    AURORA_TEST_CHECK_NE(windows, mac);
}

AURORA_TEST_CASE(designated_initialization_overrides_selected_fields_only) {
    // 纯数据聚合：指定初始化只覆写列出字段，其余保持默认（可直接 TitleBarStyle{...} 定制）；
    // 指定初始化器须按成员声明顺序排列（height → button_layout → center_title）。
    const TitleBarStyle ordered{.height = 48.0F, .button_layout = TitleBarButtonLayout::Mac, .center_title = true};
    AURORA_TEST_CHECK_NEAR(ordered.height, 48.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(ordered.center_title);
    AURORA_TEST_CHECK_EQ(ordered.button_layout, TitleBarButtonLayout::Mac);
    // 未列出的字段回落默认（AdwaitaDark 基准）。
    AURORA_TEST_CHECK_EQ(ordered.bg_active, Color{0x30, 0x30, 0x30});
    AURORA_TEST_CHECK_EQ(ordered.close_hover, Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK_TRUE(ordered.show_minimize);
    AURORA_TEST_CHECK_TRUE(ordered.show_close);
}

AURORA_TEST_CASE(active_inactive_pairs_dim_for_focus_loss) {
    // 激活/失焦成对颜色语义：失焦背景更深、失焦前景更暗（绘制层据此自动变暗）。
    const TitleBarStyle s = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
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
    const TitleBarStyle def{};
    const TitleBarStyle preset = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_EQ(preset.bg_active, def.bg_active);
    AURORA_TEST_CHECK_EQ(preset.bg_inactive, def.bg_inactive);
    AURORA_TEST_CHECK_EQ(preset.fg_active, def.fg_active);
    AURORA_TEST_CHECK_EQ(preset.fg_inactive, def.fg_inactive);
    AURORA_TEST_CHECK_EQ(preset.hover_tint, def.hover_tint);
    AURORA_TEST_CHECK_EQ(preset.close_hover, def.close_hover);
    AURORA_TEST_CHECK_EQ(preset.pressed_tint, def.pressed_tint);
    AURORA_TEST_CHECK_EQ(preset.close_pressed, def.close_pressed);
    AURORA_TEST_CHECK_EQ(preset.button_layout, def.button_layout);
}

AURORA_TEST_CASE(six_presets_table) {
    // 六套预设逐格锁值：明暗成对（背景明暗反转）、语言分派正确、按压强于悬停、显隐全开。
    struct Row {
        CsdPreset preset;
        TitleBarButtonLayout layout;
        bool dark;  // true = 暗色档（白前景 / 亮背景偏暗）
        const char *name;  // 失败时定位用（测试输出须 ASCII）
    };
    const Row rows[] = {
        {.preset = CsdPreset::AdwaitaLight,
         .layout = TitleBarButtonLayout::Adwaita,
         .dark = false,
         .name = "adwaita-light"},
        {.preset = CsdPreset::AdwaitaDark,
         .layout = TitleBarButtonLayout::Adwaita,
         .dark = true,
         .name = "adwaita-dark"},
        {.preset = CsdPreset::WindowsLight,
         .layout = TitleBarButtonLayout::Windows,
         .dark = false,
         .name = "windows-light"},
        {.preset = CsdPreset::WindowsDark,
         .layout = TitleBarButtonLayout::Windows,
         .dark = true,
         .name = "windows-dark"},
        {.preset = CsdPreset::MacLight, .layout = TitleBarButtonLayout::Mac, .dark = false, .name = "mac-light"},
        {.preset = CsdPreset::MacDark, .layout = TitleBarButtonLayout::Mac, .dark = true, .name = "mac-dark"},
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
        AURORA_TEST_CHECK_TRUE(s.bg_active.r <= (r.dark ? 0x40U : 0xF5U));
        AURORA_TEST_CHECK_EQ(s.fg_active.r, r.dark ? 255U : 0U);
        AURORA_TEST_CHECK_EQ(s.fg_active.g, r.dark ? 255U : 0U);
        // 按压强于悬停（同色相下 alpha 单调）：非关闭钮与关闭钮各自成对。
        AURORA_TEST_CHECK_TRUE(s.pressed_tint.a >= s.hover_tint.a);
        AURORA_TEST_CHECK_TRUE(s.close_pressed.a >= s.close_hover.a);
        // 高度基准统一 36dp：预设不改布局尺度。
        AURORA_TEST_CHECK_NEAR(s.height, 36.0F, 1e-4F);
        (void)r.name;  // 预留失败定位；当前断言消息不含名字
    }
}

AURORA_TEST_CASE(light_dark_pairs_share_close_red_across_languages) {
    // 特征红跨明暗恒定（Adwaita red3 / Windows 关闭红），这是各视觉语言的身份色。
    const TitleBarStyle al = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    const TitleBarStyle ad = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_EQ(al.close_hover, ad.close_hover);
    AURORA_TEST_CHECK_EQ(al.close_hover, Color{0xE0, 0x1B, 0x24});
    const TitleBarStyle wl = TitleBarStyle::from_preset(CsdPreset::WindowsLight);
    const TitleBarStyle wd = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    AURORA_TEST_CHECK_EQ(wl.close_hover, wd.close_hover);
    AURORA_TEST_CHECK_EQ(wl.close_hover, Color{0xC4, 0x2B, 0x1C});
}

}  // namespace aurora::test_cases::utest_title_bar_style
