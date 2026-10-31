#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

#include "aurora/core/platform.h"
#include "aurora/environment/media_query.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/window/window.h"

namespace aurora {

/// @brief 平台能力标志（显式查询，跳过 Surface 探测；specification/06-app-platform.md §5）。
struct PlatformCapabilities {
    bool multitouch = false;  ///< 支持多点触控（真实显示 Surface Win32/Glfw 为 true，Headless 为 false）。
    bool high_frequency_pointer = false;  ///< 高频率指针采样（Win32/Glfw 为 true）。
    bool desktop = false;  ///< 桌面形态。
    bool mobile = false;  ///< 移动形态（Mobile/Tablet）。
    /// @brief 支持接收系统拖入（跨进程拖放进应用）。
    ///
    /// 能力位只表「机制是否存在」：运行期不可用（会话建立失败、权限不足等）仍走 `Result` 错误，
    /// 二者不互相替代。两个方向**按各自后端独立翻位**——拖入已由 Win32 OLE 桥 / X11 Xdnd 桥 /
    /// Wayland `wl_data_device` 桥 / macOS `NSDraggingDestination` 桥接线，拖出同批接线（Win32
    /// `IDropSource` + `DoDragDrop` + 库自管拖拽线程；X11 指针 grab + `XdndSelection` 所有权 +
    /// 消息序列；Wayland `wl_data_source` + `start_drag`；macOS `NSDraggingSource` +
    /// `beginDraggingSessionWithItems:event:source:`，见 `system_drag_backend.h` 通道注记）；
    /// 未接线的方向不预先声称支持。
    bool system_drag_in = false;
    /// @brief 支持发起系统拖出（从应用拖到其它进程）。语义同上（机制存在性，非运行期保证）。
    bool system_drag_out = false;
    /// @brief OS 侧存在「最近文档」机制，且本库已把 `RecentDocuments::project_to_os()` 接到该机制。
    ///
    /// 能力位只表「机制是否存在」：运行期不可用（投影通路建立失败等）仍走 `Result` 错误，
    /// 二者不互相替代（与 `system_drag_in` 同款口径）。**只在桥已落地的方向翻位**——
    /// Win32 / D3D11 由 `SHAddToRecentDocs` 桥接线（两者共用同一宿主，只是上屏形态不同，故同翻）；
    /// `MacOS` 由 `NSDocumentController` 的 `noteNewRecentDocumentURL:` 桥接线（应用级集合，清空
    /// 走 `clearRecentDocuments:`）。`X11` / `Wayland` / `Glfw` / `Wasm` / `Headless` 恒 false：
    /// Linux 侧唯一沾边的 XDG `recently-used.xbel` 是 GTK 生态共读的用户态文件、不是 OS 提供的
    /// 机制，写它要自备跨进程合并与文件锁而收益只有一处选择器显示，代价不成立，故如实缺位——
    /// 应用内列表在全平台照常工作，不因此降级。
    bool recent_documents_os_projection = false;
    /// @brief 存在 OS 原生菜单栏机制，且本库已把 `NativeMenuBar` 接到该机制（Win32 `HMENU` / macOS `NSMenu` 桥）。
    ///
    /// 能力位只表「机制是否存在」：运行期不可用（菜单安装失败、无窗口宿主等）仍走 `Result` 错误，
    /// 二者不互相替代（与 `system_drag_in` 同款口径）。**只在桥已落地的方向翻位**——
    /// Win32 / D3D11 由 `HMENU` 桥接线（两者共用同一宿主，故同翻）；`MacOS` 由 `NSMenu` 桥接线
    /// （应用级 `NSApp.mainMenu`，不随窗口销毁而消失）；`X11` / `Wayland` / `Glfw` / `Wasm` /
    /// `Headless` 恒 false：Linux 侧无 OS 级菜单栏机制（GNOME 取消全局菜单、GTK4 窗口内自绘即
    /// GTK 同款形态），回落自绘 `MenuBar`，故如实缺位。
    bool native_menu_bar = false;
    /// @brief 存在 OS 运行期显隐机制，且本库已把 `Surface::set_visible()` / `is_visible()` 接到该机制。
    ///
    /// 能力位只表「机制是否存在且已接线」：与运行期失败不互相替代（同 `system_drag_in` 口径）。
    /// **只在桥已落地的方向翻位**——Win32 / D3D11 共用同一 `Win32Host`
    /// （`ShowWindow(SW_SHOWNA / SW_HIDE)`），故同翻；`MacOS` 走 `MacOSSurface` 的 `orderOut:` /
    /// `orderFrontRegardless`（显示不激活），读回原生 `isVisible`；wgpu 是**渲染后端偏好**而非独立
    /// `SurfaceKind`（其宿主仍是 Win32），不在此单列。其余恒 false：`X11` 的 `XMapWindow` /
    /// `XUnmapWindow` 与 `Glfw` 的 `glfwHideWindow` / `glfwShowWindow` 机制在但本库未接线，
    /// `Wayland` 没有「隐藏顶层窗口」的核心协议（只有 minimize），`Wasm` 的
    /// canvas 没有窗口可见性概念，`Headless` 是记录值 seam 而非 OS 机制。缺位平台上
    /// `set_visible()` 是可观测的 no-op（`is_visible()` 保持基类默认 `true`），不静默。
    bool runtime_visibility = false;
    /// @brief 存在 OS 运行期置顶机制，且本库已把 `Surface::set_always_on_top()` 接到该机制。
    ///
    /// 翻位口径同 `runtime_visibility`：Win32 / D3D11（同一宿主的 `SetWindowPos(HWND_TOPMOST /
    /// HWND_NOTOPMOST)`，wgpu 是渲染偏好而非独立 `SurfaceKind`，不另列）与 `MacOS`
    /// （`MacOSSurface` 的 `setLevel:`，Floating / Normal 两档，读回原生 `level`）；
    /// `Wayland` 无核心协议对应（其置顶属 `xdg_window` 的扩展或 compositor 策略，本库不引），
    /// `X11` 需 `_NET_WM_STATE_ABOVE` 客户机消息、`Glfw` 需 `glfwSetWindowAttrib(GLFW_FLOATING)`，
    /// 两者机制在但未接线，故如实缺位。
    bool runtime_always_on_top = false;
    /// @brief 存在 OS 打印会话机制，且本库已接线 `PrintSession`（Windows GDI 打印会话 / macOS AppKit
    /// 打印操作）。Linux 恒 false：本库不引入 libcups（第三方形态全是 vendored 源码，系统动态库是
    /// 新开的口子），替代路径是导出 PDF 后交给默认处理程序——结论见
    /// `specification/06-app-platform.md`。位为 false 时 `print_targets()` 报
    /// `print-session-unsupported`，不静默返回空列表。
    bool native_print_session = false;
    /// @brief 存在 OS 级 URL 方案登记机制，且本库已接线 `register_url_scheme()` 一系。
    ///
    /// Windows 写 `HKCU\Software\Classes`；Linux 写用户级桌面条目与 `mimeapps.list`；macOS 靠包内
    /// `CFBundleURLTypes` + Launch Services（不在 `.app` 包内时调用走 `Result` 失败并点名原因，
    /// 能力位仍为 true——机制在、桥已接）。
    bool url_scheme_registration = false;
    /// @brief 存在 OS 级文件关联数据库，且本库已接线「查询 + 声明候选」一系。
    ///
    /// 三平台同 `url_scheme_registration` 的翻位口径。声明**不抢默认处理程序**（默认归用户偏好）；
    /// 查询只答「当前实际生效的归属」。
    bool file_association = false;
    /// @brief 存在 OS 登录项机制，且本库已接线三态查询 / 开关（Windows HKCU Run / Linux 自动启动
    /// 桌面条目 / macOS `SMAppService`）。
    ///
    /// 位为 false 时 `set_run_at_login` 报 `run-at-login-unsupported`、`run_at_login()` 报
    /// `Unsupported` 并点名原因——三态不压成布尔。
    bool run_at_login = false;
};

/// @brief 平台与运行环境快照（显式查询，跳过 Surface 探测，specification/06-app-platform.md §5）。
/// 平台家族 `PlatformKind` 与设备形态 `DeviceKind` 复用 `MediaQuery` 既有定义，避免重复。
struct Platform {
    /// @brief 平台家族（Windows/MacOs/Linux/Unknown）；由 `platform()` 按编译期 OS 宏填充。
    PlatformKind kind = PlatformKind::Unknown;
    /// @brief 设备形态；当前各已知桌面 OS 均推断为 Desktop，未知平台保持 Unknown。
    DeviceKind device = DeviceKind::Unknown;
    /// @brief 实际可用的 Surface 后端类别；由 `platform()` 经 `auto_detect_surface()` 探测填充。
    SurfaceKind surface = SurfaceKind::Headless;

    /// @brief OS 明暗偏好快照；默认 `Unspecified`，须由调用方经 `os_color_scheme()` 显式填充。
    ///
    /// 与本结构其余探测字段不同：读 OS 偏好有文件/注册表访问副作用，`platform()` 不代做，
    /// 保持「显式查询，跳过探测」的口径（见 `os_color_scheme()`）。
    ColorScheme color_scheme = ColorScheme::Unspecified;

    /// @brief 是否为移动形态设备。
    /// @return device 为 Mobile 或 Tablet 时 true，否则 false。
    [[nodiscard]] auto is_mobile() const -> bool {
        return device == DeviceKind::Mobile || device == DeviceKind::Tablet;
    }
    /// @brief 是否为桌面形态设备。
    /// @return device 为 Desktop 时 true，否则 false。
    [[nodiscard]] auto is_desktop() const -> bool { return device == DeviceKind::Desktop; }

    /// @brief 由设备形态与 Surface 后端类别推导平台能力标志。
    /// @return 聚合结果：desktop/mobile 按 DeviceKind 判定；multitouch 与 high_frequency_pointer
    /// 在 Win32/Glfw/X11/Wayland/MacOS 五类真实显示 Surface 下为 true；`system_drag_in` / `system_drag_out`
    /// 仅在已接线的后端下为 true（当前为 Win32 / D3D11 / X11 / Wayland / MacOS，两方向同口径；
    /// 未接线的方向不预先声称支持）；`recent_documents_os_projection` 仅在已落 OS 投影桥的后端下为
    /// true；`native_menu_bar` 与之同口径（当前均为 Win32 / D3D11 / MacOS，macOS 两条桥同批落地）；
    /// `runtime_visibility` 与 `runtime_always_on_top` 同样仅在已接运行期窗口开关的后端下为 true
    /// （当前仅 Win32 / D3D11，其余后端为「可查询的不支持」，公共 setter 是可观测的 no-op 而非静默失效）。
    /// `native_print_session` / `url_scheme_registration` / `file_association` / `run_at_login`
    /// 与 `SurfaceKind` 无关（入口是进程级自由函数），按平台桥的编译期接线整体置位；Linux 的
    /// `native_print_session` 恒 false（不引入 libcups）。
    [[nodiscard]] auto capabilities() const -> PlatformCapabilities {
        // 待填充的能力标志聚合：默认值起步，先按设备形态、再按 SurfaceKind 逐项置位。
        PlatformCapabilities c;
        c.desktop = is_desktop();
        c.mobile = is_mobile();
        // `SurfaceKind` 所有枚举器无条件存在（见 `window.h` 稳定性契约），此处比较不再受
        // `AURORA_BACKEND_*` 宏影响；未编译的后端其标签只是运行期不会被产出，比较恒为 false。
        c.multitouch = (surface == SurfaceKind::Win32 || surface == SurfaceKind::Glfw || surface == SurfaceKind::X11 ||
                        surface == SurfaceKind::Wayland || surface == SurfaceKind::MacOS);
        c.high_frequency_pointer =
            (surface == SurfaceKind::Win32 || surface == SurfaceKind::Glfw || surface == SurfaceKind::X11 ||
             surface == SurfaceKind::Wayland || surface == SurfaceKind::MacOS);
        // 系统拖入：Win32 OLE 桥 / X11 Xdnd 桥 / Wayland `wl_data_device` 桥 / macOS
        // `NSDraggingDestination` 桥已接线。D3D11 与 Win32
        // 共用同一个 `Win32Host`（与同一桥实例），只是上屏形态不同，故同翻；wgpu 是**渲染后端偏好**
        // 而非独立的 `SurfaceKind`（其宿主仍是 Win32 / X11 / Wayland / MacOS 之一），不在此单列。Glfw 恒 false：
        // 零 `#ifdef` 公共头无法按宿主 OS 分叉，且 GLFW 自持 drop 不承载四态协商。
        //
        // ⚠️ 能力位是**编译期后端**的静态声明，不看运行期桥是否 install 成功：X11 会话缺失
        // （无 DISPLAY）时 `X11Surface` 构造即失败、不会有该 `SurfaceKind` 被产出，故此处不必
        // 也无法回读运行期状态。MacOS 同口径（与 `multitouch` 等位一致，只表机制归属）：窗口壳
        // 的真实 AppKit 视图负责登记拖放类型，运行期 install 失败仍由宿主记日志降级。
        c.system_drag_in =
            (surface == SurfaceKind::Win32 || surface == SurfaceKind::D3D11 || surface == SurfaceKind::X11 ||
             surface == SurfaceKind::Wayland || surface == SurfaceKind::MacOS);
        // 系统拖出：同批接线（Win32 走 `IDropSource` + `DoDragDrop` + 库自管拖拽线程；X11 走指针
        // grab + `XdndSelection` 所有权 + 消息序列；Wayland 走 `wl_data_source` + `start_drag`；
        // macOS 走 `NSDraggingSource` + `beginDraggingSessionWithItems:event:source:`，后三者的会话
        // 都跑在 UI 线程上）。翻位口径与拖入完全相同：D3D11 与 Win32 共用同一
        // `Win32Host`（拖出机制与窗口句柄无关，注册面进程级常驻）；wgpu 是渲染后端偏好而非独立
        // `SurfaceKind`；Glfw 恒 false（自持 drop 不承载四态协商，同拖入）。
        c.system_drag_out =
            (surface == SurfaceKind::Win32 || surface == SurfaceKind::D3D11 || surface == SurfaceKind::X11 ||
             surface == SurfaceKind::Wayland || surface == SurfaceKind::MacOS);
        // OS 级最近文档投影：Win32 / D3D11 走 `SHAddToRecentDocs`（共用同一宿主故同翻）；
        // MacOS 走 `NSDocumentController`（应用级集合，随窗口宿主接线、无窗口也可自举）。
        // Linux 三形态与 Glfw / Wasm / Headless 恒 false：OS 侧无该机制
        // （XDG `recently-used.xbel` 是 GTK 生态共读文件而非 OS 机制），应用内列表照常工作。
        c.recent_documents_os_projection =
            (surface == SurfaceKind::Win32 || surface == SurfaceKind::D3D11 || surface == SurfaceKind::MacOS);
        // 原生菜单栏：Win32 / D3D11 由 `HMENU` 桥接线（同一宿主故同翻），MacOS 由应用级
        // `NSMenu`（`NSApp.mainMenu`）桥接线，不随窗口销毁而消失；其余方向无 OS 级机制，回落自绘 MenuBar。
        c.native_menu_bar =
            (surface == SurfaceKind::Win32 || surface == SurfaceKind::D3D11 || surface == SurfaceKind::MacOS);
        c.runtime_visibility =
            (surface == SurfaceKind::Win32 || surface == SurfaceKind::D3D11 || surface == SurfaceKind::MacOS);
        c.runtime_always_on_top =
            (surface == SurfaceKind::Win32 || surface == SurfaceKind::D3D11 || surface == SurfaceKind::MacOS);
        // 桌面外壳集成四枚位：入口是进程级自由函数（`include/aurora/app/` 的五个新头），与
        // `SurfaceKind` 无关——平台桥按编译期 OS 宏接线（见 `src/aurora/app/detail/` 的三套
        // `*_shell_integration.cpp`），故这里按平台整体置位，不随 Surface 变化；Headless 在
        // Windows / Linux 上同样持有这些 OS 机制。Linux 的 `native_print_session` 恒 false：
        // 本库不引入 libcups，替代路径是导出 PDF 后交给默认处理程序（结论见
        // `specification/06-app-platform.md`）。文档输出（PDF）**不入**能力位——纯库内能力，
        // 不依赖任何 OS 机制。
#if defined(AURORA_PLATFORM_WINDOWS) || (defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS))
        c.native_print_session = true;
        c.url_scheme_registration = true;
        c.file_association = true;
        c.run_at_login = true;
#elif defined(AURORA_PLATFORM_LINUX)
        c.url_scheme_registration = true;
        c.file_association = true;
        c.run_at_login = true;
#endif
        return c;
    }
};

/// @brief 显式查询当前平台与运行环境（编译期 OS + 自动探测 Surface，specification/06-app-platform.md §5）。
/// 不构造任何 `Window` / `Surface`，保持 widget 不反向依赖 Surface 分层。
/// @return Platform 快照：surface 为自动探测结果，kind/device 按编译期 OS 宏推断
/// （Windows/MacOs/Linux→Desktop，其余→Unknown）。
[[nodiscard]] inline auto platform() -> Platform {
    Platform p;
    p.surface = auto_detect_surface();
#ifdef AURORA_PLATFORM_WINDOWS
    p.kind = PlatformKind::Windows;
    p.device = DeviceKind::Desktop;
#elif defined(AURORA_PLATFORM_MACOS)
    p.kind = PlatformKind::MacOs;
    p.device = DeviceKind::Desktop;
#elif defined(AURORA_PLATFORM_LINUX)
    p.kind = PlatformKind::Linux;
    p.device = DeviceKind::Desktop;
#else
    p.kind = PlatformKind::Unknown;
    p.device = DeviceKind::Unknown;
#endif
    return p;
}

#ifdef AURORA_PLATFORM_WINDOWS
namespace detail {
/// @brief Win32 注册表读取 OS 明暗偏好（`os_color_scheme` 的 Windows 分支实现）。
/// 定义见 `src/aurora/window/win32_platform.cpp`——公共头不引入 `<windows.h>`。
/// @return Dark/Light；打不开注册表键时 `Unspecified`。
[[nodiscard]] auto win32_os_color_scheme() -> ColorScheme;
}  // namespace detail
#elif defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
namespace detail {
/// @brief macOS `NSAppearance` 读取 OS 明暗偏好（`os_color_scheme` 的 macOS 分支实现）。
/// 定义见 `src/aurora/window/macos_platform.cpp`——公共头不引入 `<AppKit/AppKit.h>`。
/// @return Dark/Light；AppKit 尚未初始化（`NSApp == nil`）时 `Unspecified`。
[[nodiscard]] auto macos_os_color_scheme() -> ColorScheme;
}  // namespace detail
#endif

/// @brief 显式查询 OS 明暗偏好（明暗三级优先链的最低层）。
///
/// 各平台实现与局限：
/// - **Linux**：GTK 配置启发式——读 `$XDG_CONFIG_HOME/gtk-4.0/settings.ini`（缺省
///   `~/.config/...`），`gtk-application-prefer-dark-theme=true` 或 `gtk-theme` 含
///   `-dark` 后缀判 Dark，其余判 Light。**局限**：不查询 xdg-desktop-portal 的
///   `org.freedesktop.appearance`（异步 DBus，库侧不引入）；非 GTK 桌面可能误报。
/// - **Windows**：读注册表 `HKCU\...\Themes\Personalize\AppsUseLightTheme`（REG_DWORD，
///   0=Dark、1=Light）。键不存在（Win10 1803 之前）按 Light；打不开键返回 `Unspecified`。
///   无缓存轮询（每次直读，OS 偏好可能被用户随时更改）。
/// - **macOS**：`NSApp.effectiveAppearance` 的 bestMatch（`NSAppearanceNameDarkAqua`
///   → Dark，Aqua → Light）。AppKit 未初始化时返回 `Unspecified`。运行期明暗切换经
///   `MacOSSurface` 内容视图的 `viewDidChangeEffectiveAppearance` 上报
///   `notify_os_color_scheme_change`（已在本仓库真机 AppKit 构建内验证）。
/// - 其余平台：`Unspecified`。
/// @return OS 档位；无证据时不猜，返回 `Unspecified` 交由调用方回落。
[[nodiscard]] inline auto os_color_scheme() -> ColorScheme {
#ifdef AURORA_PLATFORM_WINDOWS
    // 实现见 src/aurora/window/win32_platform.cpp：注册表查询留在此处会把 <windows.h>
    // 拖进公共头（min/max 宏污染跨平台消费者），故声明在下、实现在独立 TU。
    return detail::win32_os_color_scheme();
#elif defined(AURORA_PLATFORM_LINUX)
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::string base =
        (xdg != nullptr && *xdg != '\0') ? std::string(xdg) : std::string(home == nullptr ? "" : home) + "/.config";
    for (const char *ver : {"gtk-4.0", "gtk-3.0"}) {
        std::ifstream in(base + "/" + ver + "/settings.ini");
        if (!in.is_open()) {
            continue;
        }
        std::string line;
        bool prefer_dark = false;
        bool dark_theme = false;
        bool any_line = false;
        while (std::getline(in, line)) {
            any_line = true;
            if (line.find("gtk-application-prefer-dark-theme") != std::string::npos &&
                line.find("true") != std::string::npos) {
                prefer_dark = true;
            }
            if (line.find("gtk-theme") != std::string::npos && line.find("-dark") != std::string::npos) {
                dark_theme = true;
            }
        }
        if (any_line) {
            return (prefer_dark || dark_theme) ? ColorScheme::Dark : ColorScheme::Light;
        }
    }
    return ColorScheme::Unspecified;
#elif defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
    return detail::macos_os_color_scheme();
#elif defined(AURORA_PLATFORM_MACOS)
    // 无 macOS 窗口后端：不初始化 AppKit（init 有进程级副作用），如实无证据。
    return ColorScheme::Unspecified;
#else
    return ColorScheme::Unspecified;
#endif
}

}  // namespace aurora
