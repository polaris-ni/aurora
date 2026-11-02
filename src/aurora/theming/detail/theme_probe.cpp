// ============================================================================
// theming/detail/theme_probe.cpp - 风格轴的运行期探测与默认解算
// ----------------------------------------------------------------------------
// 本文件是全库**唯一**为风格轴读取进程环境的地方。三条纪律在这里落地：
//
//   1. **实现体唯一**：桌面外壳证据只有 `detail::probe_desktop_environment()` 一份，公共入口
//      `desktop_environment()`（见 include/aurora/window/platform.h）只做转发。若再开第二处
//      `getenv`，两条链会各自漂移——同一次启动里「主题按 KDE 解算、标题栏探测说没有外壳」这类
//      矛盾就是这么来的。
//   2. **判据与读取分离**：判据表是头文件里的纯函数 `detail::style_from()`，不读环境、不碰缓存，
//      故八格矩阵可逐格单测；本文件只负责「取证据 + 转调」，CI 上的断言因此与 runner 真机外壳
//      无关。
//   3. **每进程读一次**：结果落静态局部量（首次初始化由语言保证线程安全，读取路径无锁）。代价是
//      风格轴不随运行期环境变化——换桌面外壳要重启应用。明暗轴相反，它经三级优先链跟随 OS；
//      两轴在这条上的差异是有意的，口径写进 specification/07-environment-modifier.md §5.2。
//
// 证据源取 `platform().kind`（按编译期 `AURORA_PLATFORM_*` 宏给 Windows / MacOs / Linux），
// **不取** `MediaQuery::platform`——后者只在 Win32 后端填 `Windows`、其余恒 `Unknown`，它表的是
// 渲染后端而非 OS 家族；取错源即「Linux 桌面 + GLFW 后端 → 证据 Unknown → 落回落档」，与判据表
// 直接冲突。
// ============================================================================

#include <array>
#include <cctype>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "aurora/core/platform.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_style.h"
#include "aurora/window/platform.h"

namespace aurora {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_BSD)
namespace {

// 外壳证据的认账表（全小写；比较前把读数折到小写，故这两张表本身必须写成小写字面量）。
// KDE 一支认 `KDE`（XDG_CURRENT_DESKTOP 的官方写法）与 `plasma`（DESKTOP_SESSION 的会话名）。
// GNOME 一支把 GTK 系的外壳一并收进来：它们的视觉语言与 Adwaita 同源，误判成「无证据」反而比
// 判成 Gnome 更偏离真机。
constexpr std::array<std::string_view, 2> AURORA_KDE_TOKENS = {"kde", "plasma"};
constexpr std::array<std::string_view, 6> AURORA_GNOME_TOKENS = {"gnome",    "unity",    "budgie",
                                                                 "cinnamon", "pantheon", "deepin"};

/// @brief 折到小写（只处理 ASCII 字节；外壳变量按 XDG 惯例是 ASCII，非 ASCII 字节原样透传）。
[[nodiscard]] auto folded_to_lower(std::string_view text) -> std::string {
    std::string out{text};
    for (char &ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

/// @brief 读数里是否出现表内任一枚令牌（子串匹配而非整段相等）。
///
/// 选子串匹配是刻意的：真实读数带包装前缀（`X-Cinnamon`、`gnome-classic`、`ubuntu:GNOME`），
/// 整段相等会让这些外壳全部落到 `Other`。误伤面很小——八枚令牌都够长够独特，且只有当
/// `XDG_CURRENT_DESKTOP` / `DESKTOP_SESSION` 里真出现这些词时才会命中。
[[nodiscard]] auto hits(std::string_view value, std::span<const std::string_view> tokens) -> bool {
    const std::string folded = folded_to_lower(value);
    for (const std::string_view token : tokens) {
        if (folded.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace
#endif  // 读环境的目标（其余目标的实现体只返回 Unknown，用不到上面的认账表）

auto detail::probe_desktop_environment() -> DesktopEnvironment {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_BSD)
    // 逐段扫描 XDG_CURRENT_DESKTOP（冒号分隔，如 "ubuntu:GNOME"）：取**第一个认识**的段。
    // 顺序纪律是 KDE 先于 GNOME——同时提到两者的读数属病态环境，此时 Plasma 的观感证据更强。
    const char *current = std::getenv("XDG_CURRENT_DESKTOP");
    const std::string_view current_view{current == nullptr ? "" : current};
    std::size_t segment_start = 0;
    while (segment_start <= current_view.size()) {
        const std::size_t colon = current_view.find(':', segment_start);
        const std::string_view segment = current_view.substr(
            segment_start, colon == std::string_view::npos ? std::string_view::npos : colon - segment_start);
        if (hits(segment, AURORA_KDE_TOKENS)) {
            return DesktopEnvironment::Kde;
        }
        if (hits(segment, AURORA_GNOME_TOKENS)) {
            return DesktopEnvironment::Gnome;
        }
        if (colon == std::string_view::npos) {
            break;
        }
        segment_start = colon + 1;
    }
    // 次级证据：会话名。XDG 变量缺失（裸 X11、自建合成器、某些显示管理器）时会话名常在。
    const char *session = std::getenv("DESKTOP_SESSION");
    const std::string_view session_view{session == nullptr ? "" : session};
    if (hits(session_view, AURORA_KDE_TOKENS)) {
        return DesktopEnvironment::Kde;
    }
    if (hits(session_view, AURORA_GNOME_TOKENS)) {
        return DesktopEnvironment::Gnome;
    }
    // 两枚变量至少有一枚非空却一个也不认识：环境「说了」但说的不是这两套语言 → Other，
    // 与「什么也没说」的 Unknown 分列，诊断时二者不可混。
    if (!current_view.empty() || !session_view.empty()) {
        return DesktopEnvironment::Other;
    }
    return DesktopEnvironment::Unknown;
#else
    // 其余目标（Windows / macOS / WASM / Android / 无头）不读环境变量：它们的外壳证据在
    // PlatformKind 那一轴，由环境变量表态反而制造假证据。
    return DesktopEnvironment::Unknown;
#endif
}

auto Theme::default_style() -> ThemeStyle {
    // 缓存只在「取证据」这一层：判据函数本身是纯的，重复调用无副作用，故不必缓存它的结果。
    // 命名取全大写：函数内 `static const` 走 `.clang-tidy` 的 StaticConstantCase（UPPER_CASE），
    // 不是普通局部常量的 lower_case。
    static const ThemeStyle RESOLVED_STYLE = [] {
        const Platform info = platform();
        const bool has_window_backend = (info.surface != SurfaceKind::Headless);
        return detail::style_from(info.kind, detail::probe_desktop_environment(), has_window_backend);
    }();
    return RESOLVED_STYLE;
}

auto Theme::defaults() -> Theme { return Theme::from_style(Theme::default_style()); }

}  // namespace aurora
