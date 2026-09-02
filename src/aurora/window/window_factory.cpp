#include <memory>
#include <string>

#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

// Win32 分支仅在 AURORA_BACKEND_WIN32 定义时拉入 <windows.h> 等重型头，否则整体剔除。
#ifdef AURORA_BACKEND_WIN32
#include "aurora/window/win32_surface.h"

// DPI 感知常量（兼容较旧 Windows SDK，避免版本宏依赖）。
// 注：原定义位于 win32_window.h；pimpl 重构后该头不再包含 <windows.h>，
// 故把兼容性垫片移至此文件（本文件使用这些常量且已间接包含 <windows.h>）。
#ifndef PROCESS_PER_MONITOR_DPI_AWARE // NOLINT(*-identifier-naming)
#define PROCESS_PER_MONITOR_DPI_AWARE 2
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)(-3))
#endif
#ifndef SPI_GETCLIENTAREAANIMATION
#define SPI_GETCLIENTAREAANIMATION 0x1042 ///< 系统「客户端区动画」开关（旧 SDK 可能缺定义）。
#endif
#endif

// D3D11 分支仅在 AURORA_BACKEND_D3D11 定义时编译（需 d3d11/dxgi/d3dcompiler）。
#ifdef AURORA_BACKEND_D3D11
#include "aurora/window/d3d11_surface.h"
#endif

// Glfw 分支仅在 AURORA_BACKEND_GLFW 定义时编译，避免默认构建引入 GLFW/OpenGL 依赖
// （保证零三方依赖的默认库）。
#ifdef AURORA_BACKEND_GLFW
#include "aurora/window/glfw_surface.h"
#endif
#ifdef AURORA_BACKEND_X11
#include "aurora/window/x11_surface.h"
#endif
#ifdef AURORA_BACKEND_WAYLAND
#include <cstdlib>

#include "aurora/window/wayland_surface.h"
#endif
#ifdef AURORA_BACKEND_MACOS
#include "aurora/window/macos_surface.h"
#endif
#ifdef AURORA_BACKEND_WASM
#include "aurora/window/wasm_surface.h"
#endif

namespace aurora {

// 通用：用已构造的 Surface 组装 Window，并套用跨后端共享的尺寸/标题。
static auto make_window(std::unique_ptr<Surface> surf, const WindowOptions &o) -> std::unique_ptr<Window> {
    auto w = std::make_unique<Window>(std::move(surf));
    w->set_title(o.title);
    return w;
}

// 自定义 Surface：仅一个稳定入口，注入任意已构造的 Surface（含第三方/自定义实现）。
// 空 surface 返回 Error（不崩溃），错误归属调用方（其持有本 Result）。
auto create_window(std::unique_ptr<Surface> surface, const WindowOptions &opts) -> Result<std::unique_ptr<Window>> {
    if (!surface) {
        return make_error(ErrorCode::PlatformUnavailable, "create_window(Surface): null surface provided.",
                          "Pass a valid Surface (e.g. au::HeadlessSurface) or use App().surface(...).",
                          "aurora/window/window.h");
    }
    return make_window(std::move(surface), opts);
}

// ---- Surface 专属工厂：每个重载只认识自己 Surface 的选项，无被忽略的字段 ----

#ifdef AURORA_BACKEND_HEADLESS
auto create_window(const HeadlessOptions &opts) -> Result<std::unique_ptr<Window>> {
    auto surf = std::make_unique<HeadlessSurface>(opts.png_path, opts.size);
    return make_window(std::move(surf), opts);
}
#endif

#ifdef AURORA_BACKEND_WIN32
auto create_window(const Win32Options &opts) -> Result<std::unique_ptr<Window>> {
    // 渲染后端偏好：统一的硬件加速可选开关。
#ifdef AURORA_BACKEND_D3D11
    if (opts.renderer == RendererPreference::Auto || opts.renderer == RendererPreference::GpuD3D11) {
        auto gpu = std::make_unique<D3D11Surface>(static_cast<int>(opts.size.width), static_cast<int>(opts.size.height),
                                                  opts.title, opts.style);
        if (gpu->is_available()) {
            return make_window(std::move(gpu), opts);
        }
        if (opts.renderer == RendererPreference::GpuD3D11) {
            // 强制 GPU：设备创建失败不静默降级，错误归属调用方。
            return make_error(ErrorCode::RendererUnavailable,
                              "create_window: RendererPreference::GpuD3D11 requested but D3D11 device init failed.",
                              "Use RendererPreference::Auto to fall back to software GDI.", "aurora/window/window.h");
        }
        AURORA_LOG_INFO("window", "D3D11 device unavailable; falling back to software GDI presenter.");
    }
#else
    if (opts.renderer == RendererPreference::GpuD3D11) {
        // 未编译 D3D11 后端：强制 GPU 时报错（Auto 静默走软件）。
        return make_error(
            ErrorCode::RendererUnavailable,
            "create_window: RendererPreference::GpuD3D11 requested but AURORA_BACKEND_D3D11 is not compiled in.",
            "Rebuild with -DAURORA_BACKEND_D3D11=ON, or use RendererPreference::Auto/Software.",
            "aurora/window/window.h");
    }
#endif
    auto surf = std::make_unique<Win32Surface>(static_cast<int>(opts.size.width), static_cast<int>(opts.size.height),
                                               opts.title, opts.style);
    return make_window(std::move(surf), opts);
}
#endif

#ifdef AURORA_BACKEND_D3D11
auto create_window(const D3D11Options &opts) -> Result<std::unique_ptr<Window>> {
    auto surf = std::make_unique<D3D11Surface>(static_cast<int>(opts.size.width), static_cast<int>(opts.size.height),
                                               opts.title, opts.style);
    surf->set_vsync(opts.vsync); // vsync 可选（false 交还 CPU 端帧预算节流）
    return make_window(std::move(surf), opts);
}
#endif

#ifdef AURORA_BACKEND_GLFW
auto create_window(const GlfwOptions &opts) -> Result<std::unique_ptr<Window>> {
    GlfwSurface::Config cfg;
    cfg.size = opts.size;
    cfg.title = opts.title;
    cfg.gl_major = opts.gl_major;
    cfg.gl_minor = opts.gl_minor;
    cfg.resizable = opts.resizable;
    auto surf = std::make_unique<GlfwSurface>(cfg); // 失败抛 std::runtime_error
    return make_window(std::move(surf), opts);
}
#endif

#ifdef AURORA_BACKEND_X11
auto create_window(const X11Options &opts) -> Result<std::unique_ptr<Window>> {
    auto surf = std::make_unique<X11Surface>(static_cast<int>(opts.size.width), static_cast<int>(opts.size.height),
                                             opts.title, opts.style);
    if (!surf->is_available()) {
        // 无 DISPLAY（纯 TTY/CI）或连接失败：不崩溃，错误归属调用方（AI 可枚举）。
        return make_error(ErrorCode::PlatformUnavailable, "create_window(X11): cannot open X display (DISPLAY unset?).",
                          "Run inside an X11/Wayland(XWayland) session, or fall back to "
                          "create_window(HeadlessOptions).",
                          "aurora/window/x11_surface.h");
    }
    return make_window(std::move(surf), opts);
}
#endif

#ifdef AURORA_BACKEND_WAYLAND
auto create_window(const WaylandOptions &opts) -> Result<std::unique_ptr<Window>> {
    auto surf = std::make_unique<WaylandSurface>(static_cast<int>(opts.size.width), static_cast<int>(opts.size.height),
                                                 opts.title, opts.style);
    if (!surf->is_available()) {
        // 无 WAYLAND_DISPLAY（纯 TTY/X11 会话/CI）或连接失败：不崩溃，错误归属调用方（AI 可枚举）。
        return make_error(ErrorCode::PlatformUnavailable,
                          "create_window(Wayland): cannot connect to Wayland compositor (WAYLAND_DISPLAY unset?).",
                          "Run inside a Wayland session, or fall back to create_window(X11Options) / "
                          "create_window(HeadlessOptions).",
                          "aurora/window/wayland_surface.h");
    }
    return make_window(std::move(surf), opts);
}
#endif

#ifdef AURORA_BACKEND_MACOS
auto create_window(const MacOSOptions &opts) -> Result<std::unique_ptr<Window>> {
    auto surf = std::make_unique<MacOSSurface>(static_cast<int>(opts.size.width), static_cast<int>(opts.size.height),
                                               opts.title);
    return make_window(std::move(surf), opts);
}
#endif

#ifdef AURORA_BACKEND_WASM
auto create_window(const WasmOptions &opts) -> Result<std::unique_ptr<Window>> {
    auto surf = std::make_unique<WasmSurface>(static_cast<int>(opts.size.width), static_cast<int>(opts.size.height),
                                              opts.canvas_id.c_str());
    return make_window(std::move(surf), opts);
}
#endif

auto create_native_window(const WindowOptions &opts) -> Result<std::unique_ptr<Window>> {
    // 与 auto_detect_surface() 同序：按当前构建内编译的后端选择真实显示后端，
    // 无任何真实显示后端时回退 Headless（内存帧缓冲，保证 demo/工具跨平台可编译可运行）。
#if defined(AURORA_BACKEND_WAYLAND) || defined(AURORA_BACKEND_X11)
    // Linux 桌面：运行期按会话类型选择——Wayland 会话优先原生 Wayland，
    // 失败（或 X11 会话）再试 X11（Wayland 会话下经 XWayland），最后 Headless 兜底。
#ifdef AURORA_BACKEND_WAYLAND
    if (std::getenv("WAYLAND_DISPLAY") != nullptr) {
        if (auto r = create_window(WaylandOptions{ opts })) {
            return r;
        }
        AURORA_LOG_WARN("window", "create_native_window: Wayland compositor unavailable; trying next backend.");
    }
#endif
#ifdef AURORA_BACKEND_X11
    if (auto r = create_window(X11Options{ opts })) {
        return r;
    }
#endif
#ifdef AURORA_BACKEND_HEADLESS
    // 真实显示不可用（无 DISPLAY/WAYLAND_DISPLAY 的 CI/SSH 环境）：降级内存帧缓冲，demo/工具仍可跑通。
    AURORA_LOG_WARN("window", "create_native_window: no display available; falling back to HeadlessSurface.");
    return create_window(HeadlessOptions{ opts });
#else
    return make_error(
        ErrorCode::PlatformUnavailable, "create_native_window: display unavailable and no Headless fallback.",
        "Run inside an X11/Wayland session or rebuild with -DAURORA_BACKEND_HEADLESS=ON.", "aurora/window/window.h");
#endif
#elif defined(AURORA_BACKEND_MACOS)
    return create_window(MacOSOptions{ opts });
#elif defined(AURORA_BACKEND_WASM)
    return create_window(WasmOptions{ opts });
#elif defined(AURORA_BACKEND_WIN32)
    return create_window(Win32Options{ opts });
#elif defined(AURORA_BACKEND_GLFW)
    return create_window(GlfwOptions{ opts });
#elif defined(AURORA_BACKEND_HEADLESS)
    return create_window(HeadlessOptions{ opts });
#else
    (void)opts;
    return make_error(ErrorCode::PlatformUnavailable, "create_native_window: no built-in Surface backend compiled in.",
                      "Enable a backend (e.g. -DAURORA_BACKEND_HEADLESS=ON) or inject a custom Surface via "
                      "create_window(std::unique_ptr<Surface>, WindowOptions).",
                      "aurora/window/window.h");
#endif
}

auto auto_detect_surface() -> SurfaceKind {
#if defined(AURORA_BACKEND_WAYLAND) && defined(AURORA_BACKEND_X11)
    // 双后端构建：运行期按会话类型选择（Wayland 会话 → 原生 Wayland；否则 X11/XWayland）。
    return std::getenv("WAYLAND_DISPLAY") != nullptr ? SurfaceKind::Wayland : SurfaceKind::X11;
#elif defined(AURORA_BACKEND_WAYLAND)
    return SurfaceKind::Wayland;
#elif defined(AURORA_BACKEND_X11)
    return SurfaceKind::X11;
#elif defined(AURORA_BACKEND_MACOS)
    return SurfaceKind::MacOS;
#elif defined(AURORA_BACKEND_WASM)
    return SurfaceKind::Wasm;
#elif defined(AURORA_BACKEND_WIN32)
    return SurfaceKind::Win32;
#elif defined(AURORA_BACKEND_GLFW)
    return SurfaceKind::Glfw;
#elif defined(AURORA_BACKEND_HEADLESS)
    return SurfaceKind::Headless;
#else
    // 极端：所有内置 Surface 均被剪裁。枚举值仍合法（API 稳定），但 create_window 会返回
    // Error；调用方应改用自定义 Surface 注入路径（Application(Scene,unique_ptr<Surface>) 等）。
    return SurfaceKind::Headless;
#endif
}

auto enable_dpi_awareness() -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    // 进程级一次性启用：优先 Per-Monitor V2，回退 V1，再回退 SetProcessDPIAware
    // （运行时解析函数指针，避免老 SDK 版本宏依赖；兼容常量见 win32_surface.h）。
    // GetProcAddress 返回 FARPROC，先经 void* 中转再转为目标函数指针，规避 GCC 的
    // -Wcast-function-type 误报警告。
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    using SetDpiCtxFn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
    // NOLINTBEGIN(*-pro-type-reinterpret-cast, *-casting-through-void)
    if (const auto f = reinterpret_cast<SetDpiCtxFn>(reinterpret_cast<void *>(
            GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext")))) {
        f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return;
    }
    using SetDpiFn = HRESULT(WINAPI *)(int);
    if (const auto f = reinterpret_cast<SetDpiFn>(
            reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("shcore.dll"), "SetProcessDpiAwareness")))) {
        (void)f(PROCESS_PER_MONITOR_DPI_AWARE);
        return;
    }
    // NOLINTEND(*-pro-type-reinterpret-cast, *-casting-through-void)
    SetProcessDPIAware();
#else
    // macOS / Linux：DPI 感知由系统 compositor / Cocoa 自动处理，无需 opt-in；空实现保证跨平台调用安全。
#endif
}

} // namespace aurora
