#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "aurora/app/perf_overlay.h"
#include "aurora/core/assert.h"
#include "aurora/core/result.h"
#include "aurora/core/strict_mode.h" // strict_mode()：StrictMode 影子校验
#include "aurora/core/thread.h"
#include "aurora/core/transform.h" // Matrix2D::from_translate：HUD 层合成
#include "aurora/core/types.h"
#include "aurora/debug/debug_paint.h" // 可视化调试叠层（AURORA_ENABLE_DEBUG 门控；Release 为零开销 no-op）
#include "aurora/environment/environment.h"
#include "aurora/environment/media_query.h"
#include "aurora/perf/profiler.h"
#include "aurora/render/detail/paint_timing.h" // [性能排查] detail::PaintTimer / paint_timing()：整段 widget 绘制计时
#include "aurora/render/dirty_region.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"
#include "aurora/window/surface.h"
#include "aurora/window/window_chrome.h" // WindowChrome：窗口 chrome 服务（present_root 注入根环境）
#include "aurora/window/window_state.h"  // WindowState / WindowMode 及纯函数

namespace aurora {

// =============================================================================
// 窗口构建：选项 / 类型安全工厂 / Surface 种类（原 window/backend.h，已并入本模块）
// -----------------------------------------------------------------------------
class Window; // 前向声明：下方 create_window 工厂返回 unique_ptr<Window>，Window 实体定义见下方
// 每个内置 Surface 后端由一对 CMake 开关 + feature 宏控制，可在构建时整体剔除：
// 关闭后对应 Surface 子类、工厂重载与重型平台头（<windows.h> / GLFW / OpenGL）被
// 预处理器剔除，链接产物不再含该后端。宏由 `aurora` 目标以 PUBLIC 编译定义传播。
//   AURORA_BACKEND_HEADLESS — 无头内存/PNG 后端（默认 ON）
//   AURORA_BACKEND_WIN32    — Win32/GDI 后端（Windows 默认 ON，否则 OFF）
//   AURORA_BACKEND_GLFW     — GLFW/OpenGL 后端（由 AURORA_BACKEND_GLFW 开关控制）
//   AURORA_BACKEND_X11      — X11/Xlib 后端（Linux 桌面，需 libX11；见 AURORA_BACKEND_X11）
//   AURORA_BACKEND_WAYLAND  — 原生 Wayland 后端（Linux 桌面，需 wayland-client/xkbcommon）
//   AURORA_BACKEND_MACOS    — macOS/Cocoa 后端（Apple 平台，需 Cocoa/AppKit 框架）
//   AURORA_BACKEND_WASM      — WebAssembly/Emscripten 后端（需 Emscripten 工具链）
// 自定义 Surface 注入路径（Application(Scene,unique_ptr<Surface>) / App().surface(...) 等）
// 始终可用，故「只用自定义 Surface」可不编译任何内置后端。
// =============================================================================

/// @brief Surface 种类标签：与具体 `Surface` 子类一一对应，仅作类型标签使用。
/// 用于运行期统一检测（`auto_detect_surface()` 返回类型）与平台能力探测
/// （`Platform::surface` 字段）。**不再用于构造选择**——选择 Surface 请走类型安全的
/// `create_window(const XxxOptions&)` 重载或自定义 `Surface` 注入。
enum class SurfaceKind : std::uint8_t {
    Headless, ///< 内存帧缓冲 + 可选 PNG（零依赖、跨平台，用于无头测试/校验）
    Win32,    ///< Win32/GDI 原生窗口（仅 Windows，零三方依赖）
    Glfw,     ///< GLFW + OpenGL（需系统 OpenGL + glfw3，见 AURORA_BACKEND_GLFW）
#ifdef AURORA_BACKEND_D3D11
    D3D11, ///< D3D11 增量上屏（仅 Windows，需 d3d11/dxgi；见 AURORA_BACKEND_D3D11）
#endif
#ifdef AURORA_BACKEND_X11
    X11, ///< X11/Xlib 原生窗口（Linux 桌面，需 libX11；见 AURORA_BACKEND_X11）
#endif
#ifdef AURORA_BACKEND_WAYLAND
    Wayland, ///< 原生 Wayland 窗口（Linux 桌面，需 wayland-client/xkbcommon；见 AURORA_BACKEND_WAYLAND）
#endif
#ifdef AURORA_BACKEND_MACOS
    MacOS, ///< macOS/Cocoa 原生窗口（Apple 平台，需 Cocoa/AppKit；见 AURORA_BACKEND_MACOS）
#endif
#ifdef AURORA_BACKEND_WASM
    Wasm, ///< WebAssembly/Canvas 原生窗口（Emscripten 工具链；见 AURORA_BACKEND_WASM）
#endif
};

/// @brief 渲染后端偏好：统一的硬件加速可选开关。
/// 由 `WindowOptions::renderer` 携带，`create_window(Win32Options)` 工厂据此选择上屏后端：
/// - `Auto`：编译了 `AURORA_BACKEND_D3D11` 且设备创建成功 → D3D11 GPU 上屏；
///   否则回退软件 GDI（回退时 `AURORA_LOG_INFO` 说明原因）。
/// - `Software`：强制软件 GDI 上屏。
/// - `GpuD3D11`：强制 D3D11；未编译/设备创建失败时返回 `Result` 错误（不静默降级，
///   错误归属调用方）。
/// 仅影响「像素如何上屏」：绘制仍由软件 `Painter` 完成，widget 层不感知。
enum class RendererPreference : std::uint8_t {
    Auto,     ///< 自动：优先 GPU 上屏（如可用），否则软件（默认）
    Software, ///< 强制软件上屏（Win32/GDI）
    GpuD3D11, ///< 强制 D3D11 GPU 上屏（不可用时报错，不降级）
};

/// @brief 跨所有后端共享的窗口选项。
struct WindowOptions {
    Size size{ .width = 800.0f, .height = 600.0f }; ///< 逻辑尺寸（设备无关像素）。
    std::string title{ "Aurora" };
    int max_frames = -1;        ///< Application::run 上限；-1 表示跑到 `should_close`。
    WindowStyleOptions style{}; ///< 高级样式（置顶/无边框/尺寸限制，见 surface.h）。
    // 帧调度配置
    int max_fps = 60;         ///< 活跃帧（有脏区/动画）帧率上限；0 = 不限帧率（旧行为）。
    bool power_saving = true; ///< 省电模式：idle 时阻塞等待事件（默认开）；false = 忙轮询旧行为，
                              ///< 供持续重绘场景 opt-out（与 `enable_dirty_tracking(false)` 语义配套）。
    RendererPreference renderer = RendererPreference::Auto; ///< 上屏后端偏好。
};

/// @brief Headless 后端专属选项（其余通用字段见 `WindowOptions`）。
struct HeadlessOptions : WindowOptions {
    std::string png_path; ///< 非空则每帧 `present()` 写出 PNG（覆盖同名）。
};

/// @brief Win32 后端专属选项（当前无专属字段，保留以对齐类型安全工厂）。
/// 仅当 `AURORA_BACKEND_WIN32` 定义（Win32 后端编译进构建）时可用。
#ifdef AURORA_BACKEND_WIN32
struct Win32Options : WindowOptions {};
#endif

/// @brief D3D11 后端专属选项。
/// 仅当 `AURORA_BACKEND_D3D11` 定义（由 CMake `AURORA_BACKEND_D3D11=ON` 开启）时可用。
#ifdef AURORA_BACKEND_D3D11
struct D3D11Options : WindowOptions {
    bool vsync = true; ///< 垂直同步：true = `Present(1,0)` 阻塞到 vblank（后端自带帧节拍）；
                       ///< false = `Present(0,0)` 不等 vblank，交还 CPU 端帧预算节流。
};
#endif

/// @brief Glfw 后端专属选项（其余通用字段见 `WindowOptions`）。
/// 仅当 `AURORA_BACKEND_GLFW` 定义（GLFW 后端编译进构建）时可用。
#ifdef AURORA_BACKEND_GLFW
struct GlfwOptions : WindowOptions {
    int gl_major = 3;      ///< OpenGL 主版本。
    int gl_minor = 3;      ///< OpenGL 次版本。
    bool resizable = true; ///< 窗口是否可缩放。
};
#endif

/// @brief X11 后端专属选项（当前无专属字段，保留以对齐类型安全工厂）。
/// 仅当 `AURORA_BACKEND_X11` 定义（由 CMake `AURORA_BACKEND_X11=ON` 开启，Linux 桌面）时可用。
#ifdef AURORA_BACKEND_X11
struct X11Options : WindowOptions {};
#endif

/// @brief Wayland 后端专属选项（当前无专属字段，保留以对齐类型安全工厂）。
/// 仅当 `AURORA_BACKEND_WAYLAND` 定义（由 CMake `AURORA_BACKEND_WAYLAND=ON` 开启，Linux 桌面）时可用。
#ifdef AURORA_BACKEND_WAYLAND
struct WaylandOptions : WindowOptions {};
#endif

/// @brief macOS 后端专属选项（当前无专属字段，保留以对齐类型安全工厂）。
/// 仅当 `AURORA_BACKEND_MACOS` 定义（Apple 平台，`-framework Cocoa/AppKit`）时可用。
#ifdef AURORA_BACKEND_MACOS
struct MacOSOptions : WindowOptions {};
#endif

/// @brief WASM 后端专属选项（其余通用字段见 `WindowOptions`）。
/// 仅当 `AURORA_BACKEND_WASM` 定义（Emscripten 工具链）时可用。
#ifdef AURORA_BACKEND_WASM
struct WasmOptions : WindowOptions {
    std::string canvas_id{ "aurora-canvas" }; ///< 目标 `<canvas>` 元素 id（默认 "aurora-canvas"）。
};
#endif

/// @brief Headless 专属工厂（接受 `HeadlessOptions`，含 PNG 路径）。
[[nodiscard]] auto create_window(const HeadlessOptions &opts) -> Result<std::unique_ptr<Window>>;

#ifdef AURORA_BACKEND_WIN32
/// @brief Win32 专属工厂（接受 `Win32Options`）。仅 `AURORA_BACKEND_WIN32` 构建可用。
[[nodiscard]] auto create_window(const Win32Options &opts) -> Result<std::unique_ptr<Window>>;
#endif

#ifdef AURORA_BACKEND_D3D11
/// @brief D3D11 专属工厂（接受 `D3D11Options`）。仅 `AURORA_BACKEND_D3D11` 构建可用。
[[nodiscard]] auto create_window(const D3D11Options &opts) -> Result<std::unique_ptr<Window>>;
#endif

#ifdef AURORA_BACKEND_GLFW
/// @brief Glfw 专属工厂（接受 `GlfwOptions`，含 OpenGL 版本/可缩放）。仅 `AURORA_BACKEND_GLFW` 构建可用。
[[nodiscard]] auto create_window(const GlfwOptions &opts) -> Result<std::unique_ptr<Window>>;
#endif

#ifdef AURORA_BACKEND_X11
/// @brief X11 专属工厂（接受 `X11Options`）。仅 `AURORA_BACKEND_X11` 构建（Linux 桌面）可用。
[[nodiscard]] auto create_window(const X11Options &opts) -> Result<std::unique_ptr<Window>>;
#endif

#ifdef AURORA_BACKEND_WAYLAND
/// @brief Wayland 专属工厂（接受 `WaylandOptions`）。仅 `AURORA_BACKEND_WAYLAND` 构建（Linux 桌面）可用。
[[nodiscard]] auto create_window(const WaylandOptions &opts) -> Result<std::unique_ptr<Window>>;
#endif

#ifdef AURORA_BACKEND_MACOS
/// @brief macOS 专属工厂（接受 `MacOSOptions`）。仅 `AURORA_BACKEND_MACOS` 构建（Apple）可用。
[[nodiscard]] auto create_window(const MacOSOptions &opts) -> Result<std::unique_ptr<Window>>;
#endif

#ifdef AURORA_BACKEND_WASM
/// @brief WASM 专属工厂（接受 `WasmOptions`，含 canvas id）。仅 `AURORA_BACKEND_WASM` 构建（Emscripten）可用。
[[nodiscard]] auto create_window(const WasmOptions &opts) -> Result<std::unique_ptr<Window>>;
#endif

/// @brief 自定义 Surface 注入（稳定入口，不随 backend 数量增长）：注入已构造的 `unique_ptr<Surface>`。
/// 空 surface 返回 Error（不崩溃）。这是「扩展点收口于 `Surface` 子类 + `create_window` 工厂」的唯一入口，
/// `Application`/`App` 只认 `unique_ptr<Window>` 或 `unique_ptr<Surface>`，不随 backend 增加构造函数。
[[nodiscard]] auto create_window(std::unique_ptr<Surface> surface, const WindowOptions &opts = {})
    -> Result<std::unique_ptr<Window>>;

/// @brief 跨平台便捷工厂：按 `auto_detect_surface()` 选择当前构建内最佳真实显示后端
/// （Windows → Win32，Linux → X11，macOS → Cocoa，否则 GLFW）；无任何真实显示后端时回退
/// Headless（内存帧缓冲）。供 demo/工具等「开窗即可、不关心平台」的消费者使用；
/// 需要后端专属选项（如 `D3D11Options.vsync`）时仍应走类型安全的 `create_window(XxxOptions)`。
[[nodiscard]] auto create_native_window(const WindowOptions &opts = {}) -> Result<std::unique_ptr<Window>>;

/// @brief 在创建任何窗口前启用进程级高 DPI 感知（规范入口）。
/// Windows 经 `SetProcessDpiAwarenessContext` 一次性启用；非 Windows 为空操作。
/// 必须在 `init_console()` 与 `create_window()` 之前调用。
auto enable_dpi_awareness() -> void;

/// @brief 运行期统一检测当前平台可用 Surface 种类（与 CMake 编译期宏一致）。
/// 策略：按编译期后端可用性优先原生 Wayland/X11（Linux 双后端构建时按会话类型选择），
/// 其次 MacOS/Wasm/Win32/Glfw，最后 Headless 兜底（与 window_factory.cpp 同序）。
[[nodiscard]] auto auto_detect_surface() -> SurfaceKind;

/**
 * @brief 窗口：组合一个 `Surface` 后端（Headless/Glfw/Win32），提供 pumps 事件、
 * present 根 widget、尺寸/标题管理与帧循环（架构 §4.5 后端选择）。
 *
 * 设计要点（AI-first / 概念可枚举）：
 * - 后端不可知：构造时注入 `Surface`（由 `create_window` 工厂决定具体实现），
 *   `Window` 不关心绘制/事件如何落地（headless 内存 PNG / Glfw OpenGL / Win32 GDI）。
 * - 单一职责：`Window` 只负责「事件上抛 + 像素 present」；根 widget 的布局/绘制
 *   由 `present_root` 调用方（如 `Application::run`）驱动，不在此处耦合渲染循环。
 * - 无头友好：`HeadlessSurface` 下 `run()` 退化为单次 present，便于测试/截屏。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
class Window {
  public:
    explicit Window(std::unique_ptr<Surface> surface) : m_surface(std::move(surface)) {}

    [[nodiscard]] auto surface() -> Surface & { return *m_surface; }
    [[nodiscard]] auto surface() const -> const Surface & { return *m_surface; }

    [[nodiscard]] auto size() const -> Size { return m_surface->size(); }
    [[nodiscard]] auto title() const -> const std::string & { return m_title; }
    auto set_title(std::string t) -> void {
        m_title = std::move(t);
        if (m_surface) {
            m_surface->set_title(m_title); // 下发到后端；Headless/GLFW 为空实现，Win32 经 SetWindowText 生效
        }
    }
    /// @brief 运行期更新 CSD 自绘标题栏样式（透传后端；不支持的后端为空实现）。
    auto set_title_bar_style(const TitleBarStyle &style) const -> void {
        if (m_surface) {
            m_surface->set_title_bar_style(style);
        }
    }
    /// @brief 运行期更新 CSD 标题栏图标（shared_ptr 共享像素避免深拷贝；透传后端）。
    auto set_title_bar_icon(const std::shared_ptr<Image> &icon) const -> void {
        if (m_surface) {
            m_surface->set_title_bar_icon(icon);
        }
    }

    [[nodiscard]] auto should_close() const -> bool { return m_surface->should_close(); }

    /// @brief 装饰预留给应用内容的安全区内边距（逻辑 dp），等价于 `surface().content_inset()`。
    /// 应用可据其将根布局下沉，避开 CSD 标题栏/边框（对齐 Flutter `MediaQuery.padding` 安全区）。
    [[nodiscard]] auto content_inset() const -> EdgeInsets { return m_surface->content_inset(); }
    /// @brief 程序化关闭窗口（等效于用户点 ×）。
    auto close() const -> void {
        if (m_surface) {
            m_surface->close();
        }
    }
    /// @brief 程序化最小化。
    auto minimize() const -> void {
        if (m_surface) {
            m_surface->minimize();
        }
    }
    /// @brief 程序化切换最大化。
    auto toggle_maximize() const -> void {
        if (m_surface) {
            m_surface->toggle_maximize();
        }
    }
    /// @brief 程序化设置全屏。
    auto set_fullscreen(bool on) const -> void {
        if (m_surface) {
            m_surface->set_fullscreen(on);
        }
    }
    /// @brief 控件发起窗口拖拽移动（如自绘标题栏空白区按下）。
    auto begin_window_move() const -> void {
        if (m_surface) {
            m_surface->begin_window_move();
        }
    }
    /// @brief 控件发起窗口边缘缩放。
    auto begin_window_resize(WindowResizeEdge edge) const -> void {
        if (m_surface) {
            m_surface->begin_window_resize(edge);
        }
    }

    /// @brief 设置当前窗口可见性状态快照（由 `Application` 在状态变化时调用）。
    /// 该快照每帧经 `present_root` 注入根 `BuildContext`，子树可 `ctx.env->get<WindowState>()` 读取。
    auto set_window_state(WindowState s) -> void { m_window_state = s; }
    /// @brief 设置当前窗口几何态快照（由 `Application` 在状态变化时调用）。
    auto set_window_mode(WindowMode m) -> void { m_window_mode = m; }

    /// @brief pump 平台事件（→ 经 Surface::set_event_handler 上抛给 Application 集中派发，§5.4）。
    auto pump_events() const -> void { m_surface->poll_platform_events(); }

    /// @brief 渲染单帧到后端缓冲（不 swap；present() 才提交）。
    /// ⚠️ 使用 `present_root` 驱动帧循环时**不得**再手调本函数：present_root 内部按需 begin，
    /// 部分脏区帧会刻意跳过 begin 以保留上帧像素；外层多调会把缓冲刷成底色，
    /// 导致脏区外全白。本函数仅供自行拼装 begin→paint→present 的低阶调用方使用。
    [[nodiscard]] auto begin_frame() const -> Result<bool> {
        return m_surface->begin_frame(static_cast<int>(size().width), static_cast<int>(size().height));
    }

    /// @brief 提交当前帧（swap / 保存 PNG）。
    [[nodiscard]] auto present() const -> Result<bool> { return m_surface->present(); }

    /// @brief 渲染并 present 整个 widget 树（供 `Application::run` 每帧调用）。
    /// 等价于 begin_frame → paint(root) → present；headless 下即输出 PNG。
    /// 每帧以 `MediaQuery::from_surface(*surface)`
    /// 注入根 `BuildContext`（存入地址恒定的 `m_root_env`，避免子树 Provider 持悬空父指针），
    /// 整棵树含根 widget 自身无需手动包 `MediaQueryProvider` 即可读取设备上下文；
    /// 手动 `MediaQueryProvider` 仍按「最近祖先优先」覆盖此默认值。
    ///
    /// 脏区域优化（规格 §2.1，默认开启）：脏追踪开启时按「绘制脏 / 布局脏 / 尺寸变化」
    /// 三要素决策本帧——无任一脏且尺寸未变 → 整帧跳过（idle 零开销，上帧画面仍有效）；
    /// 仅绘制脏（如文本选区高亮、主题切换）→ 跳过整树 layout，复用已缓存 Node 几何直接 paint；
    /// 布局脏或尺寸变化 → layout + paint。脏来源：任一控件 `mark_needs_layout` → 布局脏 + 绘制脏；
    /// `mark_needs_paint`（含 `State` 变更）→ 仅绘制脏。二者经 `Widget::request_frame` 沿布局父链
    /// 上溯到根，由本窗口安装在根上的唯一汇聚点接收（见 `install_dirty_sink`）。
    [[nodiscard]] auto present_root(Node &root) -> Result<bool> {
        m_cached_root = root; // 缓存当前根，供 resize/WM_PAINT 同步重渲染回调使用
        wire_present_request_once();
        if (m_presenting) {
            return true; // 防重入兜底（理论上不可达，回调已拦截）
        }
        m_presenting = true;
        struct PresentGuard {
            bool *flag = nullptr;
            explicit PresentGuard(bool &f) : flag(&f) {}
            ~PresentGuard() {
                if (flag != nullptr) {
                    *flag = false;
                }
            }
            PresentGuard(const PresentGuard &) = delete;
            auto operator=(const PresentGuard &) -> PresentGuard & = delete;
            PresentGuard(PresentGuard &&) = delete;
            auto operator=(PresentGuard &&) -> PresentGuard & = delete;
        } guard{ m_presenting };
        // 渲染插桩帧作用域：开帧清零 RenderCounters 并重置 Profiler 当帧 zone；
        // 离开 present_root 时闭帧。数据在闭帧后仍可读，供基准采集器快照。
        // AURORA_ENABLE_PROFILING 关闭时本行完全消失（零开销）。
        AURORA_PROFILE_FRAME();
        m_idle_frame = false; // 进入 present_root 即视为非 idle；idle 跳过分支会重新置 true

        FramePlan plan;
        const bool root_changed = root.operator->() != m_last_root.operator->();
        if (m_dirty_tracking) {
            if (auto skip = evaluate_dirty_plan(root, root_changed, plan)) {
                return *skip;
            }
        }

        auto bf = begin_frame_for_plan(plan);
        if (!bf) {
            return bf;
        }

        Painter &p = m_surface->painter();
        BuildContext ctx = prepare_context(root, root_changed);
        const double layout_ms = run_layout(root, plan, ctx);
        const double paint_ms = run_paint(p, root, ctx, plan);
        const bool hud_refreshed = compose_hud_maybe(p, ctx);
        return finish_present(plan, hud_refreshed, layout_ms, paint_ms);
    }

    // ---- 脏区域追踪（默认开启）----

    /// @brief 启用/关闭脏区域追踪（默认开启：idle 跳帧 + layout/paint 分离；
    /// 关闭时每帧全量重绘，与历史行为一致，供需要持续重绘的场景 opt-out）。
    auto enable_dirty_tracking(bool on) -> void {
        m_dirty_tracking = on;
        m_first_frame = true;  // 重新启用后首帧强制全绘
        m_layout_dirty = true; // 重新启用后首帧强制重排
        m_dirty.clear();
    }
    [[nodiscard]] auto dirty_tracking_enabled() const -> bool { return m_dirty_tracking; }

    /// @brief 本次 present_root 是否为 idle 跳过帧（无脏区、未渲染）。
    [[nodiscard]] auto is_idle_frame() const -> bool { return m_idle_frame; }

    /// @brief 手动标记脏矩形（窗口逻辑坐标）。
    auto mark_dirty(const Rect &r) -> void { m_dirty.mark(r); }

    /// @brief 强制下一帧全量重排重绘（测试 seam / 外部环境变化、动画/视频持续重绘时调用）。
    auto force_full_redraw() -> void {
        m_layout_dirty = true;
        m_dirty.mark_all();
    }

    /// @brief 设置 HUD 叠加层（分层 HUDA）。
    ///
    /// 叠加层（典型为 `PerfOverlay`）独立于 widget 树渲染到离屏缓冲，仅以 ~2Hz 重绘自身，
    /// 不再触发整树重绘；每帧在 tree paint 之后、present 之前合成到主缓冲。app 树仅在其自身
    /// 脏时重绘，叠加层的刷新开销被隔离在离屏缓冲内（~1–2ms）。传入 `nullptr` 关闭叠加层。
    ///
    /// 与「把 PerfOverlay 作为根控件包裹内容」的旧用法互斥：二者取其一。启用叠加层后，
    /// 根 widget 树即 `Scene` 的真实内容，`PerfOverlay` 等不应再出现在树内。
    ///
    /// @note Thread: main-thread only
    auto set_overlay(std::shared_ptr<Widget> overlay) -> void {
        m_overlay = std::move(overlay);
        m_hud_rendered = false; // 强制下一帧重绘 HUD 缓冲（含开关/首次设置）
    }
    [[nodiscard]] auto overlay() const -> const std::shared_ptr<Widget> & { return m_overlay; }

    /// @brief 脏区域追踪器（供性能覆盖层/测试观测）。
    [[nodiscard]] auto dirty_tracker() -> DirtyRegionTracker & { return m_dirty; }

    /// @brief 下一帧是否有待处理的脏（绘制脏/布局脏/首帧未绘），供帧调度决策取值
    /// 脏追踪关闭时视为永远有脏（每帧全绘，仅受帧预算节流）
    [[nodiscard]] auto has_pending_dirty() const -> bool {
        if (!m_dirty_tracking) {
            return true;
        }
        return m_first_frame || m_layout_dirty || !m_dirty.is_empty();
    }

    /// @brief 设置本帧末尾的等待请求（由 `Application::run` 每帧经 `compute_wait_timeout`
    /// 决策后写入；`Window::run` 在 on_frame 之后消费）。语义同 `Surface::wait_events`：
    /// `<0` 无限等待 / `0` 不等（默认，未设置则保持旧忙轮询行为）/ `>0` 等待毫秒数。
    auto set_next_wait(double timeout_ms) -> void { m_next_wait_ms = timeout_ms; }

  private:
    /// @brief 在**根控件**安装子树脏汇聚点（全树仅此一处回调）。
    ///
    /// 取代旧的 `wire_dirty`（每帧递归接线整棵树的 `on_dirty`）。接线式方案有两个结构缺陷：
    ///  ① 接线是树结构的**快照**：`AppShell` 等在 `on_layout` 中动态新建的子控件（骨架→真实
    ///     内容切换后的 banner / 卡片等自驱动动画）不在快照内，其 `mark_needs_paint` 无人接收
    ///     → 动画冻结在首帧、内容永不出现（白屏类 bug）；为此不得不「每帧 layout 后重接」。
    ///  ② 每帧重接又让链式包装（`prev` 嵌套）随帧数**无界增长**（60fps 下每控件每秒 +60 层）。
    /// 改为 `Widget::request_frame` 沿布局父链上溯到根、在此单点汇聚：无快照、无重接，
    /// 动态新建的子树天然被覆盖。
    auto install_dirty_sink(Widget &root_widget) -> void {
        root_widget.on_subtree_dirty = [this](Widget &w, bool layout) -> void {
            if (layout) {
                // boundary 自身 layout 脏：登记到脏集合，由 present_root 局部重排，
                // 不置根脏（避免整树重排）。以 weak_ptr 持有，跨帧销毁安全。
                // weak_from_this 为空表示该控件不由 shared_ptr 持有（无法登记），回落整树重排。
                const std::shared_ptr<Widget> sp = w.is_relayout_boundary() ? w.weak_from_this().lock() : nullptr;
                if (sp) {
                    register_dirty_boundary(sp);
                } else {
                    m_layout_dirty = true; // 非 boundary（或无法登记）：下一帧整树重排
                }
            }
            // 标记控件最近一次 paint 的绝对（窗口逻辑 dp）几何，使脏区裁剪命中精确区域；
            // 空盒（未绘制过/尺寸为 0）被 mark 忽略。布局脏仍走 mark_all → 整帧重绘。
            m_dirty.mark(w.paint_bounds());
        };
    }

  public:
    /**
     * @brief 运行帧循环：pump 事件 → 调 on_frame 回调（渲染根）→ 按需阻塞等待，直到
     * should_close 或达到 max_frames（<0 表示无限，直到关闭）。on_frame 由 `Application` 注入
     * （调用 `present_root`，其内部负责 present），保持 `Window` 与渲染循环解耦；
     * `Window::run` 不再额外 present，避免每帧重复提交。
     *
     * 事件驱动帧循环：on_frame 末尾经 `set_next_wait` 写入本帧
     * 等待决策，循环在帧末 `Surface::wait_events` 阻塞到事件/超时，取代忙轮询；
     * 等待请求一次性消费（未重新设置则不等），直调 `Window::run` 的低阶调用方
     * （测试/自拼帧循环）行为不变。等待时长记入 `FrameStats`（wakeups / sleep ratio）。
     */
    AURORA_MAIN_THREAD auto run(const std::function<void()> &on_frame, int max_frames = -1) -> void {
        int n = 0;
        while (!should_close()) {
            pump_events();
            if (on_frame) {
                on_frame();
            }
            ++n;
            if (max_frames > 0 && n >= max_frames) {
                break;
            }
            const double wait_ms = m_next_wait_ms;
            m_next_wait_ms = 0.0; // 一次性消费：未重新设置则不等待（旧行为兜底）
            if (wait_ms != 0.0 && !should_close()) {
                const auto t0 = std::chrono::steady_clock::now();
                m_surface->wait_events(wait_ms);
                const auto t1 = std::chrono::steady_clock::now();
                FrameStats::instance().record_wait(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
        }
    }

  private:
    std::unique_ptr<Surface> m_surface; ///< 组合的后端（不可知）
    std::string m_title{ "Aurora" };
    Environment m_root_env; ///< 每帧重建的根 MediaQuery 注入环境（地址恒定）；present_root 注入。
    WindowState m_window_state = WindowState::Visible; ///< 当前窗口可见性快照（由 Application 设置）。
    WindowMode m_window_mode = WindowMode::Normal;     ///< 当前窗口几何态快照（由 Application 设置）。
    DirtyRegionTracker m_dirty;                        ///< 绘制脏追踪器（§2.1）。
    bool m_dirty_tracking = true;                      ///< 脏追踪开关（默认开启：idle 跳帧 + layout/paint 分离）。
    bool m_layout_dirty = true; ///< 布局脏：尺寸/结构/约束变化或 mark_needs_layout 触发，下一帧需重排。
    std::vector<std::weak_ptr<Widget>>
        m_dirty_boundaries; ///< 脏 relayout boundary 集合（局部重排用，避免整树重排）。
                            ///< 以 weak_ptr 持有：boundary widget 可能在两次帧之间被销毁
                            ///< （如滚动列表回收 lazy row），裸指针会悬垂导致 use-after-free。

    /// @brief 已安装脏汇聚点的根控件（见 install_dirty_sink）。以 weak_ptr 持有并按被控对象
    /// 指针比对：根控件被销毁后新根即使复用同一地址也会因 lock() 失败而正确重装。
    std::weak_ptr<Widget> m_sink_root;

    /// @brief 去重登记脏 relayout boundary（子树脏汇聚回调中调用）。
    /// 以 weak_ptr 持有，避免 boundary 在帧间被销毁后留下悬垂指针；解引用时 lock() 失败即跳过
    /// （已销毁的 boundary 子树已不存在，无需重排，跳过即为正确行为）。
    auto register_dirty_boundary(std::weak_ptr<Widget> b) -> void {
        const auto sp = b.lock();
        if (!sp) {
            return; // 已销毁，无需登记
        }
        const Widget *const raw = sp.get();
        for (const auto &e : m_dirty_boundaries) {
            if (e.lock().get() == raw) {
                return; // 去重（按被控对象指针）
            }
        }
        m_dirty_boundaries.push_back(std::move(b));
    }
    bool m_first_frame = true;                         ///< 首帧强制全绘。
    Size m_last_size{ .width = 0.0f, .height = 0.0f }; ///< 上帧窗口尺寸（resize 检测）。
    Node m_last_root{};                                ///< 上一次 present 的根（导航切换检测）。
    bool m_root_mounted = false;                       ///< 当前根是否已挂载（接线响应式订阅）。
    Node m_cached_root{};                              ///< 最近一次 present_root 的根，供 resize 同步重渲染。
    bool m_present_wired = false;                      ///< present-request 回调是否已接线到 Surface。
    bool m_presenting = false;                         ///< present_root 重入护栏（同步重渲染回调用）。
    bool m_system_redraw = false; ///< 本次 present_root 由系统重绘请求驱动（WM_PAINT 等）：跳帧时仍须重新上屏。
    bool m_idle_frame = false;    ///< 最近一次 present_root 是否为 idle 跳过（无脏区、未渲染）。
    double m_next_wait_ms = 0.0;  ///< 本帧末尾的等待请求（一次性消费，0=不等）。

    // ---- 分层 HUD 叠加层 ----
    std::shared_ptr<Widget> m_overlay;                        ///< 叠加层 widget（典型 PerfOverlay）；空 = 无叠加层。
    std::unique_ptr<Painter> m_hud_painter;                   ///< 叠加层离屏缓冲（逻辑窗口尺寸 × 窗口 scale）。
    float m_hud_scale = 0.0f;                                 ///< 离屏缓冲的 scale（与窗口不一致时重建）。
    bool m_hud_rendered = false;                              ///< 离屏缓冲是否已渲染过（首帧/开关变更须重建）。
    std::chrono::steady_clock::time_point m_hud_last_refresh; ///< 上次 HUD 离屏重绘时刻（2Hz 节流）。

    /// @brief 重绘 HUD 离屏缓冲（仅 ~2Hz 调用，隔离叠加层刷新开销于树重绘之外）。
    /// 缓冲尺寸 = 逻辑窗口尺寸（scale 同窗口）；每帧 composite 前若距上次 ≥500ms 才调用。
    auto render_hud(const BuildContext &ctx) -> void {
        const Size sz = size();
        const float s = m_surface->scale_factor();
        const int w = static_cast<int>(std::lround(sz.width));
        const int h = static_cast<int>(std::lround(sz.height));
        if (!m_hud_painter) {
            m_hud_painter = std::make_unique<Painter>();
        }
        if (m_hud_painter->width() != w || m_hud_painter->height() != h || m_hud_scale != s) {
            m_hud_painter->set_scale(s);
            m_hud_painter->begin(w, h); // 逻辑 dp → 内部按 scale 放大为物理缓冲
            m_hud_scale = s;
        } else {
            m_hud_painter->clear_rect(
                Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = sz }); // 清回透明零基底，避免与上帧叠加
        }
        // 叠加层控件在 on_paint 内以逻辑坐标绘制；bounding 用整窗逻辑尺寸（与 composite 1:1 对齐）。
        m_overlay->paint(*m_hud_painter, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = sz }, ctx);
        m_hud_last_refresh = std::chrono::steady_clock::now();
        m_hud_rendered = true;
    }

    // ---- present_root 拆分出的私有辅助 ----

    /// @brief 帧渲染计划：由脏追踪阶段产出，供后续 begin/layout/paint/present 使用。
    struct FramePlan {
        bool do_layout = true;           ///< 本帧是否需要 layout。
        bool whole_tree_relayout = true; ///< 是否整树重排（否则仅 dirty boundary 局部）。
        std::vector<Rect> dirty_dev;     ///< 设备坐标脏矩形。
        Rect clip_logical{};             ///< 逻辑坐标裁剪区。
    };

    /// @brief 把「立即重绘请求」接线到 Surface（首次 present_root 时执行一次）。
    auto wire_present_request_once() -> void {
        if (m_present_wired) {
            return;
        }
        // 性能：把「立即重绘请求」接为对缓存根再渲染一帧——后端在几何变化
        // （Win32 WM_SIZE 最大化/缩放）或系统要求重绘（WM_PAINT）时调用，使帧缓冲在
        // DWM 合成前已为新尺寸内容，从根源消除最大化白闪/黑屏。
        // 尺寸未变且无脏时本调用被脏追踪跳过（上帧缓冲仍有效，由后端兜底 blit）。
        m_surface->set_present_request([this]() -> void {
            if (m_presenting || !m_cached_root) {
                return; // 防重入：渲染期间抓到的系统消息不得递归重渲染
            }
            // 系统要求的重绘（WM_PAINT/WM_SIZE）：窗口表面可能已失效（最小化还原、
            // 遮挡揭开），即便脏追踪判定整帧跳过，也必须重新上屏兜底（见下方跳帧分支）。
            m_system_redraw = true;
            (void)present_root(m_cached_root);
            m_system_redraw = false;
        });
        m_present_wired = true;
    }

    /// @brief 脏追踪决策：计算 FramePlan；若本帧可跳过则返回跳过结果。
    [[nodiscard]] auto evaluate_dirty_plan(const Node &root, bool root_changed, FramePlan &plan)
        -> std::optional<Result<bool>> {
        const bool size_changed = size().width != m_last_size.width || size().height != m_last_size.height;
        if (root_changed) {
            // 根节点变化（如导航切换页面）：必须整体重绘，避免停留旧页面。
            m_dirty.mark_all();
            m_layout_dirty = true;
        }
        // 无绘制脏、无布局脏、尺寸未变、根未变 → 整帧跳过（上帧画面仍有效）
        if (!m_first_frame && m_dirty.is_empty() && !m_layout_dirty && !size_changed) {
            // 系统要求重绘时不能只跳过：帧缓冲内容仍有效，但窗口表面已被 OS 置为
            // 无效（最小化还原后为类背景刷底色）——须全量 blit 重新上屏，否则白屏。
            if (m_system_redraw) {
                m_surface->set_present_dirty({}); // 空向量 = 全量 blit（不残留旧增量脏区）
                return present();
            }
            m_idle_frame = true; // 标记 idle 帧：无脏区、未做任何渲染
            return Result<bool>{ true };
        }
        // 布局决策：首帧、布局脏或尺寸变化时必须重排；否则复用上帧 Node 几何仅重绘。
        plan.do_layout = m_first_frame || m_layout_dirty || size_changed || !m_dirty_boundaries.empty();
        // 捕获整树重排决策快照：m_layout_dirty / m_first_frame 在下方即刻清 0，布局块须用本快照
        // 判定「整树重排」还是「仅脏 boundary 子树局部重排」；否则首帧/根脏帧会误走局部分支
        // （m_layout_dirty 已被清 0）→ 整树不重排（Root.on_layout 调用次数归零，边界尺寸未初始化）。
        plan.whole_tree_relayout = m_layout_dirty || m_first_frame || size_changed;
        // 捕获脏矩形（逻辑→设备坐标）供增量上屏；布局/尺寸变化整帧重绘则全量上传。
        if (!plan.do_layout) {
            const float s = m_surface->scale_factor();
            for (const Rect &r : m_dirty.rects()) {
                plan.dirty_dev.push_back(
                    Rect{ .origin = Point{ .x = r.origin.x * s, .y = r.origin.y * s },
                          .size = Size{ .width = r.size.width * s, .height = r.size.height * s } });
            }
            // 脏区裁剪绘制：把脏矩形并界作为裁剪矩形（逻辑 dp，与 Painter push_clip 同坐标系）。
            // 逐像素裁剪保证脏区重绘与整帧绘制逐位一致——即便脏区边界外控件有文本/AA 溢出，
            // 其溢出像素也会被裁剪到与整帧等价的结果，不引入绘制残留。
            if (!m_dirty.is_full() && !m_dirty.is_empty()) {
                plan.clip_logical = m_dirty.merged_bounds();
            }
        }
        // 脏区规模计数：必须在下方 m_dirty.clear() 之前快照。
        // dirty_area_ratio 取「合并包围盒 / 视口」——它才是实际被裁剪重绘的面积，
        // 逐矩形面积和会因重叠而失真（且可能 > 1）。
        AURORA_PROFILE_SET(dirty_rect_count, static_cast<std::uint32_t>(m_dirty.rects().size()));
        AURORA_PROFILE_SET(dirty_area_ratio, [this]() -> double {
            const Size vp = size();
            if (vp.width <= 0.0f || vp.height <= 0.0f || m_dirty.is_empty()) {
                return 0.0;
            }
            if (m_dirty.is_full()) {
                return 1.0;
            }
            const Rect mb = m_dirty.merged_bounds();
            return static_cast<double>(mb.size.width) * static_cast<double>(mb.size.height) /
                   (static_cast<double>(vp.width) * static_cast<double>(vp.height));
        }());
        // 渲染前清脏：渲染期间产生的新脏标记（如动画驱动的 State 变更）保留到下一帧。
        m_dirty.clear();
        m_layout_dirty = false;
        m_first_frame = false;
        m_last_size = size();
        m_last_root = root;
        return std::nullopt;
    }

    /// @brief 按帧计划启动新帧：仅非裁剪帧才调用 begin_frame（裁剪帧保留上帧像素）。
    [[nodiscard]] auto begin_frame_for_plan(const FramePlan &plan) const -> Result<bool> {
        const bool partial_clip = plan.clip_logical.size.width > 0.0f && plan.clip_logical.size.height > 0.0f;
        // 全量重绘标记：无裁剪帧 = 整个视口从零基底重新合成。
        AURORA_PROFILE_SET(full_redraw, !partial_clip);
        if (!partial_clip) {
            return begin_frame();
        }
        return Result<bool>{ true };
    }

    /// @brief 构建根 BuildContext 并挂载/安装脏汇聚点（如需要）。
    [[nodiscard]] auto prepare_context(Node &root, bool root_changed) -> BuildContext {
        m_root_env.set<MediaQuery>(MediaQuery::from_surface(*m_surface));
        // 注入窗口级生命周期快照：子树可 ctx.env->get<WindowState>() / ctx.env->get<WindowMode>() 读取。
        m_root_env.set<WindowState>(m_window_state);
        m_root_env.set<WindowMode>(m_window_mode);
        // 注入窗口 chrome 服务（与状态快照同源，同一 Surface；此处 m_surface 已解引用必非空）：
        // 子树控件经 ctx.env->get<WindowChrome>() 驱动移动/缩放/最小化/最大化/全屏/关闭。
        m_root_env.set<WindowChrome>(WindowChrome{ &*m_surface });
        BuildContext ctx;
        ctx.env = &m_root_env;
        ctx.scale_factor = m_surface->scale_factor();
        ctx.size = size();
        // 首次（或根变化后的新树）自动挂载：接线响应式订阅，使 State/修饰变更能标脏重绘。
        if (!m_root_mounted || root_changed) {
            root.widget().mount(ctx);
            m_root_mounted = true;
        }
        // 安装子树脏汇聚点（见 install_dirty_sink）：仅在根控件实例变化时安装一次——
        // 后代脏经布局父链上溯至根，动态新建的子控件天然被覆盖，无需每帧重接。
        // 必须在 layout / paint 之前完成：这两个阶段内产生的脏（延期布局、自驱动动画在
        // on_paint 中 mark_needs_paint）需被本帧捕获，才能驱动下一帧。
        if (m_sink_root.lock().get() != root.operator->()) {
            install_dirty_sink(root.widget());
            m_sink_root = root.widget().weak_from_this();
        }
        return ctx;
    }

    /// @brief 对单个脏 relayout boundary 执行局部重排（含 StrictMode 校验）。
    static auto relayout_boundary_widget(Widget &w, const BuildContext &ctx) -> void {
        const Size before = w.size();
        w.layout(w.cached_constraints(), ctx);
        if (strict_mode() == StrictMode::On) {
            // StrictMode 影子校验：合法 boundary 的尺寸只由约束决定，滚动（约束不变）
            // 不应改变其尺寸；若变化说明它实际依赖子节点，误判为 boundary。
            const Size after = w.size();
            const bool size_unchanged =
                std::fabsf(before.width - after.width) < 1e-3f && std::fabsf(before.height - after.height) < 1e-3f;
            AURORA_ASSERT(
                size_unchanged,
                "relayout boundary 误判：boundary 尺寸随重排变化，说明其依赖子节点，不能作为 relayout boundary");
        }
    }

    /// @brief 布局整棵子树或仅脏 boundary 子树；返回 layout 耗时（ms）。
    [[nodiscard]] auto run_layout(Node &root, const FramePlan &plan, const BuildContext &ctx) -> double {
        // 布局整棵子树：LayoutBuilder 等依赖 layout 阶段构建子节点（约束不变则复用缓存），
        // 也是 T8 根 MediaQuery 注入对子树可见的前提。
        Constraints c;
        c.max = size();
        const auto t_layout_start = std::chrono::steady_clock::now();
        {
            AURORA_PROFILE_SCOPE("Window::layout");
            if (plan.do_layout) {
                if (plan.whole_tree_relayout) {
                    // 根级脏（首帧/尺寸变化/非 boundary 子节点脏）：整树重排，沿途刷新所有布局缓存。
                    root.widget().layout(c, ctx);
                    m_dirty_boundaries.clear();
                } else {
                    // 仅脏 relayout boundary 子树局部重排；子树内未脏节点靠布局缓存短路跳过，
                    // 复杂度从「全树节点数」降至「可见/脏 boundary 子树节点数」。
                    for (const auto &wp : m_dirty_boundaries) {
                        auto sp = wp.lock();
                        if (!sp) {
                            // boundary 已在帧间被销毁（如滚动中被回收的 lazy row）
                            // 跳过：其子树已不存在，无需重排，且避免悬垂指针 use-after-free。
                            continue;
                        }
                        relayout_boundary_widget(*sp, ctx);
                    }
                    m_dirty_boundaries.clear();
                }
            }
        }
        auto t_layout_end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t_layout_end - t_layout_start).count();
    }

    /// @brief 绘制 widget 树（全量或脏区裁剪）；返回 paint 耗时（ms）。
    [[nodiscard]] auto run_paint(Painter &p, Node &root, const BuildContext &ctx, const FramePlan &plan) const
        -> double {
        const auto t_paint_start = std::chrono::steady_clock::now();
        {
            AURORA_PROFILE_SCOPE("Window::paint");
            detail::PaintTimer scene_guard{ &detail::paint_timing().scene }; // [性能排查] 整段 widget 绘制耗时
#ifdef AURORA_ENABLE_DEBUG
            // 调试帧计数器前移：使本帧 render_into 打的重绘标记与当前帧一致（repaint_highlight 时序）。
            if (debug::any_flag_enabled()) {
                debug::bump_debug_frame();
            }
#endif
            const Rect full_bounds{ .origin = Point{ .x = 0, .y = 0 }, .size = size() };
            // 部分裁剪帧：跳过 begin_frame 保留上帧缓冲（Headless/D3D11 的 begin 会清零整帧，
            // Win32 尺寸不变时保留），再把裁剪区重置为零基底后裁剪重绘——裁剪内从零基底
            // 重新合成（与整帧重绘同序同基底），裁剪外沿用上帧像素，两侧均与整帧重绘
            // 逐位一致；若直接 begin_frame 则裁剪外被清零，产生黑屏/残留。
            if (plan.clip_logical.size.width > 0.0f && plan.clip_logical.size.height > 0.0f) {
                p.set_skip_dl_record(true);      // 抑制 DL 录制/回放：partial clip 下录制会丢失 clip 外子节点命令
                p.clear_rect(plan.clip_logical); // 裁剪区先回到新帧零基底，避免与上帧像素双重混合
                p.push_clip(plan.clip_logical);
                // 以后端 begin_frame 的底色重铺脏区：跳过 begin_frame 保留了上帧缓冲，但脏区已被
                // clear_rect 归零；若不重铺底色，脏区内无不透明背景的控件（裸 Text / 无背景 LazyList
                // 子项）会露出零基底（黑）。铺不透明底色（a>0）覆盖零基底后再绘制内容，与整帧
                // begin_frame「铺底色 + 绘树」逐位一致（透明底色的后端如 Headless/D3D11 跳过重铺，
                // 零基底本就与其 begin_frame 一致）。
                if (const Color cc = m_surface->clear_color(); cc.m_a > 0) {
                    p.fill_rect(plan.clip_logical, cc);
                }
                root.widget().paint(p, full_bounds, ctx);
                p.pop_clip();
                p.set_skip_dl_record(false);
            } else {
                root.widget().paint(p, full_bounds, ctx);
            }
#ifdef AURORA_ENABLE_DEBUG
            // 全树绘制后统一叠加可视化调试层（layout_guides / relayout_boundaries /
            // layer_borders / repaint_highlight / overdraw），画在 app 树之上。
            if (debug::any_flag_enabled()) {
                debug::paint_debug_overlays(p, root.widget(),
                                            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = size() }, ctx);
            }
#endif
        }
        // [性能排查] 提交本绘制帧的光栅计时快照（idle 帧不进此域，不会用零值覆盖上一绘制帧）。
        detail::commit_paint_frame(true);
        const auto t_paint_end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t_paint_end - t_paint_start).count();
    }

    /// @brief 若启用 HUD 叠加层，按需重绘并合成到主缓冲；返回是否发生了 HUD 刷新。
    [[nodiscard]] auto compose_hud_maybe(Painter &p, const BuildContext &ctx) -> bool {
        // ---- 分层 HUD 叠加层（CPU 性能专项选项 A）----
        // 叠加层独立于 widget 树，仅以 ~2Hz 重绘离屏缓冲；每帧（full / partial 均）合成到主缓冲。
        // 面板为不透明，故叠在「保留自上一帧」的主缓冲之上不会产生重影；app 树仅在自身脏时重绘，
        // 叠加层刷新开销被隔离在离屏缓冲内（~1–2ms），不再触发整树重绘。
        bool hud_refreshed = false;
        if (m_overlay) {
            const auto now = std::chrono::steady_clock::now();
            const double since_ms = std::chrono::duration<double, std::milli>(now - m_hud_last_refresh).count();
            if (!m_hud_rendered || since_ms >= 500.0) {
                render_hud(ctx);
                hud_refreshed = true;
            }
            p.composite(*m_hud_painter, Matrix2D::from_translate(0.0f, 0.0f));
        }
        return hud_refreshed;
    }

    /// @brief 上屏并记录阶段耗时；返回 present 结果。
    [[nodiscard]] auto finish_present(const FramePlan &plan, bool hud_refreshed, double layout_ms,
                                      double paint_ms) const -> Result<bool> {
        const auto t_present_start = std::chrono::steady_clock::now();
        Result<bool> result{ true };
        {
            AURORA_PROFILE_SCOPE("Window::present");
            // 把本帧脏矩形交给后端（空向量 = 全量上传）；增量上屏后端据此仅更新变化区。
            // 叠加层内容本帧发生变化（2Hz 重绘）时强制全量上传：HUD 像素可能落在 app 脏区之外，
            // 仅增量上传会让新 HUD 滞留在主缓冲、未上屏（最多 1 帧滞后）。未变化则复用增量脏区。
            m_surface->set_present_dirty((m_overlay && hud_refreshed) ? std::vector<Rect>{} : plan.dirty_dev);
            result = present();
        }
        const auto t_present_end = std::chrono::steady_clock::now();
        const double present_ms = std::chrono::duration<double, std::milli>(t_present_end - t_present_start).count();
        // 记录阶段计时
        FrameStats::instance().record_phases(layout_ms, paint_ms, present_ms);
        return result;
    }
};

} // namespace aurora
