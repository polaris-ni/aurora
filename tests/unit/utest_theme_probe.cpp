/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme_style.h 的 detail::style_from / detail::probe_desktop_environment
/// + include/aurora/theming/theme.h 的 Theme::default_style / Theme::defaults
/// + include/aurora/window/platform.h 的 desktop_environment 转发
/// 测试说明: 覆盖风格探测判据表逐格（无后端 / 三大家族 / 外壳证据四态 / WASM / BSD 型未映射目标）、
/// `default_style()` 与纯函数在同一格上的取值一致、`defaults()` 只定风格不定明暗。断言只读真机
/// 证据、不改环境变量（风格缓存进程内不可重入，改环境会得到顺序依赖的结果）；探测的缓存次数与
/// 真机读数由手工用例核对。

#include "aurora/theming/theme.h"
#include "aurora/theming/theme_style.h"
#include "aurora/window/platform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_probe {

namespace {

/// @brief 主题逐字段比较（除扩展注册表外的九个值面）；`Theme` 未声明 `operator==`，故在此聚合。
auto same_theme(const Theme &a, const Theme &b) -> bool {
    return a.style == b.style && a.scheme == b.scheme && a.roles == b.roles && a.type_scale == b.type_scale &&
           a.csd == b.csd && a.shapes == b.shapes && a.spacing == b.spacing && a.controls == b.controls &&
           a.conventions == b.conventions;
}

}  // namespace

AURORA_TEST_CASE(style_from_matrix_follows_the_evidence_table) {
    // 判据表逐格：证据三元组 → 风格档。name 只作失败定位（输出须 ASCII）。
    struct Cell {
        PlatformKind kind = PlatformKind::Unknown;
        DesktopEnvironment env = DesktopEnvironment::Unknown;
        bool has_window_backend = true;
        ThemeStyle want = ThemeStyle::Material;
        const char *name = "";
    };
    const Cell cells[] = {
        // 无窗口后端一票否决：哪怕平台与外壳证据都齐，也不给原生风格（golden 与无头构建的确定性）。
        {.kind = PlatformKind::Windows,
         .env = DesktopEnvironment::Unknown,
         .has_window_backend = false,
         .want = ThemeStyle::Material,
         .name = "windows-headless"},
        {.kind = PlatformKind::Linux,
         .env = DesktopEnvironment::Kde,
         .has_window_backend = false,
         .want = ThemeStyle::Material,
         .name = "linux-kde-headless"},
        {.kind = PlatformKind::MacOs,
         .env = DesktopEnvironment::Unknown,
         .has_window_backend = false,
         .want = ThemeStyle::Material,
         .name = "macos-headless"},
        // 三大家族：kind 单独定档，外壳证据不参与（Windows/macOS 没有 XDG 会话可谈）。
        {.kind = PlatformKind::Windows,
         .env = DesktopEnvironment::Unknown,
         .has_window_backend = true,
         .want = ThemeStyle::Windows,
         .name = "windows"},
        {.kind = PlatformKind::MacOs,
         .env = DesktopEnvironment::Unknown,
         .has_window_backend = true,
         .want = ThemeStyle::MacOs,
         .name = "macos"},
        // Linux 第二轴：KDE 单列，GNOME / 无证据 / 不认识的外壳一律 Adwaita 基线。
        {.kind = PlatformKind::Linux,
         .env = DesktopEnvironment::Kde,
         .has_window_backend = true,
         .want = ThemeStyle::Kde,
         .name = "linux-kde"},
        {.kind = PlatformKind::Linux,
         .env = DesktopEnvironment::Gnome,
         .has_window_backend = true,
         .want = ThemeStyle::Gnome,
         .name = "linux-gnome"},
        {.kind = PlatformKind::Linux,
         .env = DesktopEnvironment::Unknown,
         .has_window_backend = true,
         .want = ThemeStyle::Gnome,
         .name = "linux-no-evidence"},
        {.kind = PlatformKind::Linux,
         .env = DesktopEnvironment::Other,
         .has_window_backend = true,
         .want = ThemeStyle::Gnome,
         .name = "linux-other-shell"},
        // 未映射进 PlatformKind 的目标（BSD / Android）：外壳证据仍可用，两者皆无才回落。
        {.kind = PlatformKind::Unknown,
         .env = DesktopEnvironment::Kde,
         .has_window_backend = true,
         .want = ThemeStyle::Kde,
         .name = "unknown-kind-kde-shell"},
        {.kind = PlatformKind::Unknown,
         .env = DesktopEnvironment::Gnome,
         .has_window_backend = true,
         .want = ThemeStyle::Gnome,
         .name = "unknown-kind-gnome-shell"},
        {.kind = PlatformKind::Unknown,
         .env = DesktopEnvironment::Unknown,
         .has_window_backend = true,
         .want = ThemeStyle::Material,
         .name = "unknown-kind-no-evidence"},
        {.kind = PlatformKind::Unknown,
         .env = DesktopEnvironment::Other,
         .has_window_backend = true,
         .want = ThemeStyle::Material,
         .name = "unknown-kind-other-shell"},
        // 浏览器：有窗口后端（Wasm surface）但没有桌面外壳概念。
        {.kind = PlatformKind::Web,
         .env = DesktopEnvironment::Unknown,
         .has_window_backend = true,
         .want = ThemeStyle::Material,
         .name = "wasm"},
    };
    for (const Cell &c : cells) {
        AURORA_TEST_CHECK_EQ(detail::style_from(c.kind, c.env, c.has_window_backend), c.want);
    }
}

AURORA_TEST_CASE(probe_never_fails_and_stays_inside_the_four_states) {
    // 探测无失败态：任何读数都必须落进四态域（无证据即 Unknown，由解算侧回落 Material，不报错）。
    const DesktopEnvironment env = desktop_environment();
    const bool inside_domain = env == DesktopEnvironment::Unknown || env == DesktopEnvironment::Gnome ||
                               env == DesktopEnvironment::Kde || env == DesktopEnvironment::Other;
    AURORA_TEST_CHECK(inside_domain);
#if !defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_BSD)
    // 非 Linux/BSD 目标不读环境变量：它们的平台证据在 PlatformKind 那一轴，环境变量表态只会造假。
    AURORA_TEST_CHECK_EQ(env, DesktopEnvironment::Unknown);
#endif
    // 转发口径：公共入口与 detail 实现体同值（全库只有一份 getenv 读取，不允许第二条链漂移）。
    AURORA_TEST_CHECK_EQ(env, detail::probe_desktop_environment());
}

AURORA_TEST_CASE(default_style_agrees_with_the_pure_function_on_the_same_evidence) {
    // 真机证据经公共入口取；断言的是「解算链 = 判据函数」，不是具体档位——具体读数属手工用例。
    const Platform info = platform();
    const bool has_window_backend = (info.surface != SurfaceKind::Headless);
    const ThemeStyle want = detail::style_from(info.kind, desktop_environment(), has_window_backend);
    AURORA_TEST_CHECK_EQ(Theme::default_style(), want);
    // 缓存语义：同一进程内重复调用取值不变（与「每进程至多一次证据读取」的可观察面）。
    AURORA_TEST_CHECK_EQ(Theme::default_style(), want);
}

AURORA_TEST_CASE(defaults_fixes_the_style_axis_only_and_leaves_the_scheme_open) {
    const Theme t = Theme::defaults();
    // 等价于 from_style(default_style())：整套令牌随探测到的风格档，明暗位不表态。
    AURORA_TEST_CHECK_TRUE(same_theme(t, Theme::from_style(Theme::default_style())));
    AURORA_TEST_CHECK_EQ(t.style, Theme::default_style());
    AURORA_TEST_CHECK_EQ(t.scheme, ColorScheme::Unspecified);
    // 不表态 ≠ 亮：scheme 字段保持 Unspecified，只是取值落明色列（与 resolve_color_scheme 同一口径）。
    const Theme light = Theme::from_style(t.style, ColorScheme::Light);
    AURORA_TEST_CHECK(t.roles == light.roles);
    AURORA_TEST_CHECK(t.csd == light.csd);
    AURORA_TEST_CHECK_EQ(light.scheme, ColorScheme::Light);
}

AURORA_TEST_CASE(material_is_the_fallback_and_light_dark_stay_orthogonal) {
    // 回落档必须逐位等于现行基线：无证据环境（无头 CI、WASM）拿到的观感与本提案之前完全相同。
    AURORA_TEST_CHECK_TRUE(
        same_theme(Theme::from_style(ThemeStyle::Material, ColorScheme::Unspecified), Theme::with_defaults()));
    AURORA_TEST_CHECK_TRUE(same_theme(Theme::from_style(ThemeStyle::Material, ColorScheme::Light), Theme::light()));
    AURORA_TEST_CHECK_TRUE(same_theme(Theme::from_style(ThemeStyle::Material, ColorScheme::Dark), Theme::dark()));
    // 两轴正交：同一风格的明暗两档只在配色面分叉，风格身份位与令牌结构逐字段同构。
    const Theme wind_light = Theme::from_style(ThemeStyle::Windows, ColorScheme::Light);
    const Theme wind_dark = Theme::from_style(ThemeStyle::Windows, ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(wind_light.style, wind_dark.style);
    AURORA_TEST_CHECK(!(wind_light.roles == wind_dark.roles));
    AURORA_TEST_CHECK(wind_light.type_scale == wind_dark.type_scale);
}

}  // namespace aurora::test_cases::utest_theme_probe
