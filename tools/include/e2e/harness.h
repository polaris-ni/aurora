#pragma once

// ============================================================
// E2E 驱动内核（tools/include/e2e/harness.h）
// ------------------------------------------------------------
// 真实后端端到端测试的**框架无关**驱动内核：按指定后端建真实窗口 → 渲染上屏 → 读回像素。
// 本头只依赖 aurora 公共头，**不含任何测试框架宏**，故 `tests/`（etest_ 用例）与
// `tools/verify/`（真机验收探针）双方都可包含同一份实现。
//
// 定位：内核不是新增开发，而是**既有真实后端测试代码的抽取与统一**——建窗 / 推进 / 读回
// 在仓库内只有这一份实现，不得出现第二套容差常量或第二套 skip 约定。
//
// 像素读回基于既有公共基底 `Surface::data()` + `Surface::framebuffer_size()`，
// **未新增任何 `Surface` 公共读回虚方法**；`data()` 为 nullptr（后端未实现读回，或
// `AURORA_ENABLE_DEBUG` 未生效）时返回 unsupported 错误，错误码与消息语义沿用
// `Surface::save_snapshot` 既有的 "framebuffer capture unavailable"。
//
// 帧推进复用 `TestController` 已验证的既有帧序（tick 手势计时 → `Animator::tick` →
// `Scheduler::tick` → `Window::present_root`），但把「无头 Surface」换成真实窗口：
// 内核自持 `Animator` / `Scheduler` 并经其 `set_current` 挂为进程内当前实例，使控件在
// mount 期按既有约定注册的动画与定时任务能被本内核逐帧推进。**不新增任何公共单帧 API**，
// `Application::step_frame()` 保持 `private`。
//
// 可用性探测 `backend_compiled()` 是纯编译期事实，`open()` 的运行期失败以 `ok()` / `reason()`
// 如实上报：内核只回答「这个后端在本构建 / 本环境下能不能用」，**不决定**用例该跳过还是失败
// ——那是测试侧的策略，落在 tests/e2e/e2e_expect.h（`AURORA_E2E_EXPECT` 期望集）。
// ============================================================

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/widget/node.h"
#include "aurora/widget/widget.h"

namespace aurora::e2e {

/// @brief E2E 目标后端标识（用例请求「用哪个后端建窗」的入参）。
///
/// 与 `SurfaceKind` 一一对应但**独立存在**：`SurfaceKind` 是运行期检测标签，本枚举是
/// 建窗请求，故其枚举器无条件出现，不随 `AURORA_BACKEND_*` 宏增减（未编译的后端请求会
/// 得到明确的「不可用 + 原因」，而非编译失败）。
enum class Backend : std::uint8_t {
    Auto,  ///< 交平台自动选择（`create_native_window`，供探针复用）
    Headless,  ///< 无头内存帧缓冲（不建真实窗口；供内核自测与纯逻辑用例）
    Win32,  ///< Win32/GDI 原生窗口
    D3D11,  ///< D3D11 增量上屏（Windows）
    Glfw,  ///< GLFW + OpenGL
    X11,  ///< X11/Xlib（Linux 桌面）
    Wayland,  ///< 原生 Wayland（Linux 桌面）
    Wgpu,  ///< wgpu GPU 栅格上屏（宿主按编译期择一）
};

/// @brief 后端标识的稳定短名（用于错误消息与期望集声明，如 `AURORA_E2E_EXPECT=glfw`）。
[[nodiscard]] constexpr auto backend_name(Backend backend) -> const char * {
    switch (backend) {
        case Backend::Auto:
            return "auto";
        case Backend::Headless:
            return "headless";
        case Backend::Win32:
            return "win32";
        case Backend::D3D11:
            return "d3d11";
        case Backend::Glfw:
            return "glfw";
        case Backend::X11:
            return "x11";
        case Backend::Wayland:
            return "wayland";
        case Backend::Wgpu:
            return "wgpu";
    }
    return "unknown";
}

/// @brief 该后端是否**编译进本次构建**（纯编译期事实，不建窗、不依赖运行环境）。
///
/// 供用例层做「跳过还是失败」的策略判定：未编译的后端在 `open()` 中也会得到明确的
/// 不可用原因，本函数只是让调用方在**建窗之前**就能区分「本构建没有」与「环境不可用」。
[[nodiscard]] constexpr auto backend_compiled(Backend backend) -> bool {
    switch (backend) {
        case Backend::Auto: // NOLINT(*-branch-clone)
            return true;
#ifdef AURORA_BACKEND_HEADLESS
        case Backend::Headless:
            return true;
#endif
#ifdef AURORA_BACKEND_WIN32
        case Backend::Win32:
            return true;
#endif
#ifdef AURORA_BACKEND_D3D11
        case Backend::D3D11:
            return true;
#endif
#ifdef AURORA_BACKEND_GLFW
        case Backend::Glfw:
            return true;
#endif
#ifdef AURORA_BACKEND_X11
        case Backend::X11:
            return true;
#endif
#ifdef AURORA_BACKEND_WAYLAND
        case Backend::Wayland:
            return true;
#endif
#ifdef AURORA_BACKEND_GPU_WGPU
        case Backend::Wgpu:
            return true;
#endif
        default:
            return false;
    }
}

/// @brief 建窗规格。
struct WindowSpec {
    Backend backend = Backend::Glfw;  ///< 目标后端
    int width = 320;  ///< 逻辑宽（dp）
    int height = 200;  ///< 逻辑高（dp）
    std::string title = "aurora_e2e";  ///< 窗口标题（真实窗口下可见于标题栏）

    /// @brief 窗口可见性策略。默认 `Hidden`：E2E 用例默认不把窗口推入用户视野（不抢焦点、不闪烁），
    ///        且隐藏窗口仍须可渲染、可读回像素。
    WindowVisibility visibility = WindowVisibility::Hidden;

    /// @brief 请求 GPU 栅格路径（仅 GLFW 后端有此二选一；其余后端本身就是 GPU 路径，忽略）。
    bool gpu = false;

    /// @brief 帧推进的固定步长（秒）。取固定值而非真实时钟，保证动画/定时任务在用例内可复现。
    double frame_seconds = 1.0 / 60.0;
};

/// @brief 一帧像素：RGBA 8bit，行序自顶向下，宽高为**帧缓冲物理像素**。
struct Frame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  ///< 长度 == width * height * 4
};

/// @brief 取帧内某点的 RGBA；越界返回全 0（调用方应先自行断言尺寸）。
[[nodiscard]] inline auto pixel_at(const Frame &frame, int x, int y) -> Color {
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
        return Color{0, 0, 0, 0};
    }
    const auto off =
        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)) + static_cast<std::size_t>(x)) * 4U;
    return Color{frame.pixels[off], frame.pixels[off + 1U], frame.pixels[off + 2U], frame.pixels[off + 3U]};
}

// ============================================================
// 控件树查询面
// ------------------------------------------------------------
// 全部建立在**公共自描述通道**上（`Node::id` / `Widget::type_name` / `Widget::serialize_props`
// / `Widget::child_nodes`），故不依赖 `TestController`——后者整头受 `AURORA_BACKEND_HEADLESS`
// 门控，若查询面依赖它，则在「关掉无头后端但开真实后端」的构建里 E2E 恰好失去查询能力，
// 与本内核的存在理由相悖。
// ============================================================

/// @brief 前序收集整棵控件树（`Node` 无父指针，自顶向下遍历 `Widget::child_nodes()`）。
inline auto collect_preorder(const Node &node, std::vector<Node> &out) -> void {
    if (!node) {
        return;
    }
    out.push_back(node);
    for (const Node &child : node.widget().child_nodes()) {
        collect_preorder(child, out);
    }
}

/// @brief 前序收集整棵控件树（返回值形态）。
[[nodiscard]] inline auto collect_preorder(const Node &root) -> std::vector<Node> {
    std::vector<Node> all;
    collect_preorder(root, all);
    return all;
}

/// @brief 按节点标识查找（`Node::set_id` 设置的 key）。
[[nodiscard]] inline auto find_by_key(const Node &root, std::string_view id) -> std::vector<Node> {
    std::vector<Node> hits;
    for (const Node &node : collect_preorder(root)) {
        if (node.id() == id) {
            hits.push_back(node);
        }
    }
    return hits;
}

/// @brief 按控件类型名查找（`Widget::type_name()`）。
[[nodiscard]] inline auto find_by_type(const Node &root, std::string_view type) -> std::vector<Node> {
    std::vector<Node> hits;
    for (const Node &node : collect_preorder(root)) {
        if (node.widget().type_name() == type) {
            hits.push_back(node);
        }
    }
    return hits;
}

/// @brief 文本类属性键（各控件的文本属性名不统一，逐个比对；与 `TestController::find_by_text` 同源）。
inline constexpr std::string_view AURORA_TEXT_PROP_KEYS[] = {"content", "text", "label",
                                                             "value",   "hint", "placeholder"};

/// @brief 读单个属性的 JSON 值（缺失返回 null）。
[[nodiscard]] inline auto read_prop(const Widget &widget, std::string_view key) -> Json {
    Json props = Json::object();
    widget.serialize_props(props);
    const std::string name{key};
    if (props.contains(name)) {
        return props[name];  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
    }
    return Json{};
}

/// @brief 按文本内容查找：逐个比对文本类属性（启发式，语义与 `TestController::find_by_text` 一致）。
[[nodiscard]] inline auto find_by_text(const Node &root, std::string_view text) -> std::vector<Node> {
    std::vector<Node> hits;
    for (const Node &node : collect_preorder(root)) {
        for (const std::string_view key : AURORA_TEXT_PROP_KEYS) {
            const Json value = read_prop(node.widget(), key);
            if (value.is_string() && value.get<std::string>() == text) {
                hits.push_back(node);
                break;
            }
        }
    }
    return hits;
}

/// @brief 读回一帧像素：基底是 `Surface::data()` + `Surface::framebuffer_size()` 组合。
///
/// 宽高取**帧缓冲物理像素**（DPI 缩放下与逻辑尺寸不同）。`data()` 为 nullptr（后端未实现
/// 读回，或 `AURORA_ENABLE_DEBUG` 未生效）时返回 unsupported 错误，不返回空帧、不伪造内容。
/// 单列自由函数而非只做 `Session` 成员：探针与内核自测需要从**任意** `Surface` 读回。
[[nodiscard]] inline auto capture_frame(const Surface &surface) -> Result<Frame> {
    const std::uint8_t *px = surface.data();
    if (px == nullptr) {
        return Result<Frame>{
            make_error(ErrorCode::GeneralNotSupported,
                       "read_pixels: Surface::data() returned nullptr (framebuffer capture unavailable)")};
    }
    const auto sz = surface.framebuffer_size();
    Frame frame;
    frame.width = static_cast<int>(sz.width);
    frame.height = static_cast<int>(sz.height);
    const auto bytes = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4U;
    frame.pixels.assign(px, px + bytes);  // NOLINT(*-pro-bounds-pointer-arithmetic)
    return Result<Frame>{std::move(frame)};
}

/// @brief 一帧推进所需的两个驱动器（`Animator` + `Scheduler`）。
///
/// 单列为一个结构并以 `unique_ptr` 持有，是为了让 `Session` 保持可移动（默认移动语义）：
/// `set_current` 登记的是这两个成员的地址，若它们直接作为 `Session` 的成员，移动 `Session`
/// 会让登记地址失效。堆上持有使地址在整个会话期内稳定。
struct FrameDrivers {
    Animator animator;
    Scheduler scheduler;
};

/// @brief E2E 会话：持有一个真实窗口，RAII 兜底其生命周期。
///
/// 用例中途断言失败、抛出异常或提前 return 时，析构即关闭窗口并回收宿主，不残留幽灵窗口；
/// 该保证不依赖用例显式调用清理函数，也不依赖测试框架的断言宏（异常与提前 return 路径同等成立）。
///
/// @note Thread: main-thread only
/// @note Side-effects: 构造后持有 OS 窗口；析构时销毁
class Session {
  public:
    Session() = default;
    ~Session() {
        // 撤销本会话对进程内「当前驱动器」的登记，避免析构后残留悬垂指针（同 `TestController::Impl`）。
        // 仅在登记确实指向自己时撤销：多会话并存时不误清他者。
        if (drivers_ != nullptr) {
            if (Animator::current() == &drivers_->animator) {
                Animator::set_current(nullptr);
            }
            if (Scheduler::current() == &drivers_->scheduler) {
                Scheduler::set_current(nullptr);
            }
        }
    }
    Session(const Session &) = delete;
    auto operator=(const Session &) -> Session & = delete;
    Session(Session &&) noexcept = default;
    auto operator=(Session &&) noexcept -> Session & = default;

    /// @brief 窗口是否已就绪（false = 建窗失败，原因见 `reason()`）。
    [[nodiscard]] auto ok() const -> bool { return window_ != nullptr; }
    /// @brief 建窗失败原因（成功时为空串）。
    [[nodiscard]] auto reason() const -> const std::string & { return reason_; }
    /// @brief 请求的后端（诊断用）。
    [[nodiscard]] auto backend() const -> Backend { return backend_; }
    /// @brief 持有的窗口（须先确认 `ok()`）。
    [[nodiscard]] auto window() -> Window & { return *window_; }
    /// @brief 持有的窗口（const 视图）。
    [[nodiscard]] auto window() const -> Window & { return *window_; }
    /// @brief 持有的窗口表面（须先确认 `ok()`）。
    [[nodiscard]] auto surface() -> Surface & { return window_->surface(); }
    /// @brief 持有的窗口表面（const 视图；`data()` / `framebuffer_size()` 均为 const 成员）。
    [[nodiscard]] auto surface() const -> const Surface & { return window_->surface(); }

    /// @brief 是否发生了 Headless 回退（仅 `Backend::Auto` 有意义）。
    ///
    /// `Auto` 走 `create_native_window`，在无任何真实显示后端时回退内存帧缓冲；判定方式是
    /// 对已建成的 Surface 做 `dynamic_cast<const HeadlessSurface *>`（`HeadlessSurface` 是公共类型），
    /// **不新增公共查询 API**。
    [[nodiscard]] auto fell_back_to_headless() const -> bool {
#ifdef AURORA_BACKEND_HEADLESS
        return dynamic_cast<const HeadlessSurface *>(&surface()) != nullptr;
#else
        // 无头后端未编译：`create_native_window` 没有回退目标，恒不成立。
        return false;
#endif
    }

    // ---- 根控件 ----

    /// @brief 挂载根控件（`Node` 按值拷贝，与调用方共享同一 widget）。
    auto mount(Node root) -> void { root_ = std::move(root); }
    /// @brief 已挂载的根控件（未挂载时为空 `Node`）。
    [[nodiscard]] auto root() -> Node & { return root_; }
    /// @brief 已挂载的根控件（const 视图）。
    [[nodiscard]] auto root() const -> const Node & { return root_; }

    // ---- 帧推进 ----

    /// @brief 渲染并上屏一帧（复用 `Window::present_root`，不新增公共单帧 API）。
    /// @note 首次调用前会挂上本会话的 `Animator` / `Scheduler`，使 mount 期注册的动画与定时任务
    ///       能被后续 `pump*` 推进（`Animator::current()` 是 mount 期的注册点）。
    [[nodiscard]] auto present() -> Result<bool> {
        auto ready = require_mounted();
        if (!ready) {
            return Result<bool>{ready.error()};
        }
        return window_->present_root(root_);
    }

    /// @brief 挂载根控件并渲染上屏一帧（便捷形式）。
    [[nodiscard]] auto present(Node &root) -> Result<bool> {
        mount(root);
        return present();
    }

    /// @brief 推进若干帧：每帧「泵平台事件 → tick 手势计时 → Animator → Scheduler → present_root」。
    ///
    /// 帧序对齐 `Application::run`（与 `TestController::pump` 同源，仅多一步真实平台事件泵）。
    /// @param frames 帧数；<= 0 视为 1。
    /// @return 实际推进的帧数；任一帧 present 失败即返回该错误。
    [[nodiscard]] auto pump(int frames = 1) -> Result<int> {
        auto ready = require_mounted();
        if (!ready) {
            return Result<int>{ready.error()};
        }
        const int limit = frames > 0 ? frames : 1;
        for (int i = 0; i < limit; ++i) {
            auto one = pump_one();
            if (!one) {
                return Result<int>{one.error()};
            }
        }
        return Result<int>{limit};
    }

    /// @brief 推进到静止态：某帧为 idle 跳帧且无运行中动画即收敛（判据与 `Application` 的帧调度同源）。
    ///
    /// @param max_frames 帧数上界（防死循环）。
    /// @return 收敛时返回实际推进的帧数；预算内未收敛返回错误，消息含已推进帧数与最后脏区状态
    ///         （`has_pending_dirty` / `is_idle_frame` / 活跃动画），供用例直接作为失败原因。
    [[nodiscard]] auto pump_until_settled(int max_frames = 60) -> Result<int> {
        auto ready = require_mounted();
        if (!ready) {
            return Result<int>{ready.error()};
        }
        const int limit = max_frames > 0 ? max_frames : 1;
        for (int i = 0; i < limit; ++i) {
            auto one = pump_one();
            if (!one) {
                return Result<int>{one.error()};
            }
            if (window_->is_idle_frame() && !drivers_->animator.has_active()) {
                return Result<int>{i + 1};
            }
        }
        return Result<int>{
            make_error(ErrorCode::RuntimeAsyncTimeout,
                       std::string{"pump_until_settled: not settled within "} + std::to_string(limit) +
                           " frames (pending_dirty=" + (window_->has_pending_dirty() ? "true" : "false") +
                           ", idle_frame=" + (window_->is_idle_frame() ? "true" : "false") +
                           ", active_animations=" + (drivers_->animator.has_active() ? "true" : "false") + ")",
                       "Raise the frame budget, or check for an animation that never reaches a steady state.",
                       "aurora/e2e/harness.h")};
    }

    /// @brief 最近一帧是否为 idle 跳帧（未渲染）。
    [[nodiscard]] auto is_idle_frame() const -> bool { return window_->is_idle_frame(); }
    /// @brief 下一帧是否有待处理的脏（供用例构造自定义收敛判据）。
    [[nodiscard]] auto has_pending_dirty() const -> bool { return window_->has_pending_dirty(); }
    /// @brief 已呈现帧数（idle 跳帧不计数）。
    [[nodiscard]] auto frame_count() const -> int { return surface().frame_count(); }
    /// @brief 泵一次平台事件（非阻塞）。
    auto pump_events() const -> void { window_->pump_events(); }

    /// @brief 本会话的动画驱动器（首帧前挂为进程内当前实例）。
    [[nodiscard]] auto animator() -> Animator & {
        ensure_drivers();
        return drivers_->animator;
    }
    /// @brief 本会话的定时任务驱动器（首帧前挂为进程内当前实例）。
    [[nodiscard]] auto scheduler() -> Scheduler & {
        ensure_drivers();
        return drivers_->scheduler;
    }

    // ---- 输入注入（目标式语义，经 `Inspector::simulate_*` 走真实命中测试 + 冒泡派发）----
    //
    // 与 `TestController` 同口径：注入成功后登记「下一帧全量重绘」——真实平台每个输入事件都会
    // 唤醒帧循环，不登记则紧随其后的 `pump` 会被脏区决策判为 idle 跳过，像素停留在交互前。

    /// @brief 点击目标：Press + Release（目标中心）。
    [[nodiscard]] auto tap(Widget &widget) const -> Result<void> {
        return after_input(Inspector::simulate_click(widget));
    }
    /// @brief 点击目标节点（内部拷一份 `Node`，共享同一 widget，故可接受 const 实参）。
    [[nodiscard]] auto tap(const Node &node) const -> Result<void> {
        Node target = node;
        return tap(target.widget());
    }
    /// @brief 拖拽目标：Press（中心）→ Move（中心 + delta）→ Release。
    [[nodiscard]] auto drag(Widget &widget, const Point &delta) const -> Result<void> {
        return after_input(Inspector::simulate_drag(widget, delta.x, delta.y));
    }
    /// @brief 拖拽目标节点。
    [[nodiscard]] auto drag(const Node &node, const Point &delta) const -> Result<void> {
        Node target = node;
        return drag(target.widget(), delta);
    }
    /// @brief 滚动目标：以目标中心为指针位置派发滚轮事件。
    [[nodiscard]] auto scroll(Widget &widget, float dx, float dy) const -> Result<void> {
        return after_input(Inspector::simulate_scroll(widget, dx, dy));
    }
    /// @brief 滚动目标节点。
    [[nodiscard]] auto scroll(const Node &node, float dx, float dy) const -> Result<void> {
        Node target = node;
        return scroll(target.widget(), dx, dy);
    }
    /// @brief 文本输入：置焦后向目标派发文本输入事件。
    [[nodiscard]] auto enter_text(Widget &widget, std::string_view text) const -> Result<void> {
        return after_input(Inspector::simulate_text_input(widget, text));
    }
    /// @brief 文本输入（节点重载）。
    [[nodiscard]] auto enter_text(const Node &node, std::string_view text) const -> Result<void> {
        Node target = node;
        return enter_text(target.widget(), text);
    }

    // ---- 像素读回 ----

    /// @brief 读回当前帧像素（`capture_frame` 的会话绑定形式）。
    ///
    /// 未建窗（`ok() == false`）时返回 `GeneralInvalidArgument`，不空指针解引用——用例漏检
    /// `ok()` 时应得到可诊断的错误而非崩溃。
    [[nodiscard]] auto read_pixels() const -> Result<Frame> {
        if (window_ == nullptr) {
            return Result<Frame>{
                make_error(ErrorCode::GeneralInvalidArgument,
                           std::string{"read_pixels: session has no window (backend unavailable: "} + reason_ + ")")};
        }
        return capture_frame(window_->surface());
    }

  private:
    friend auto open(const WindowSpec &spec) -> Session;

    /// @brief 采纳工厂结果：成功则持有窗口，失败则记录错误消息（不静默降级）。
    auto adopt(Result<std::unique_ptr<Window>> created) -> void {
        if (!created) {
            reason_ = created.error().message;
            return;
        }
        window_ = std::move(created.value());
    }

    /// @brief 首帧前把本会话的驱动器挂为进程内当前实例（`Animator::current()` 是 mount 期注册点）。
    auto ensure_drivers() -> void {
        if (drivers_ != nullptr) {
            return;
        }
        drivers_ = std::make_unique<FrameDrivers>();
        Animator::set_current(&drivers_->animator);
        Scheduler::set_current(&drivers_->scheduler);
    }

    /// @brief 前置校验：窗口就绪且根已挂载（帧推进与 present 的共同前提）。
    [[nodiscard]] auto require_mounted() -> Result<void> {
        if (window_ == nullptr) {
            return Result<void>{
                make_error(ErrorCode::GeneralInvalidArgument,
                           std::string{"session has no window (backend unavailable: "} + reason_ + ")")};
        }
        if (!root_) {
            return Result<void>{make_error(ErrorCode::GeneralInvalidArgument,
                                           "no root mounted; call mount(root) or present(root) first")};
        }
        ensure_drivers();
        return Result<void>{};
    }

    /// @brief 单帧：泵平台事件 → tick 手势计时 → Animator → Scheduler → present_root。
    [[nodiscard]] auto pump_one() -> Result<void> {
        window_->pump_events();
        root_.widget().tick(std::chrono::steady_clock::now());
        drivers_->animator.tick(frame_seconds_);
        drivers_->scheduler.tick(frame_seconds_);
        const Result<bool> presented = window_->present_root(root_);
        if (!presented) {
            return Result<void>{presented.error()};
        }
        return Result<void>{};
    }

    /// @brief 注入成功即登记「下一帧全量重绘」，模拟真实平台输入事件唤醒帧循环的行为。
    [[nodiscard]] auto after_input(Result<void> injected) const -> Result<void> {
        if (!injected) {
            return injected;
        }
        window_->force_full_redraw();
        return Result<void>{};
    }

    std::unique_ptr<Window> window_;
    std::string reason_;
    Backend backend_ = Backend::Auto;
    Node root_;
    double frame_seconds_ = 1.0 / 60.0;
    std::unique_ptr<FrameDrivers> drivers_;
};

/// @brief 按规格建窗。失败时返回 `ok() == false` 的会话，原因写入 `reason()`。
///
/// **不静默降级**：请求的后端未编译或初始化失败即失败，绝不改建成别的后端——是否容忍由
/// 用例层按 `AURORA_E2E_EXPECT` 决定（期望集内判失败，期望集外跳过）。
[[nodiscard]] inline auto open(const WindowSpec &spec) -> Session {
    Session session;
    session.backend_ = spec.backend;
    session.frame_seconds_ = spec.frame_seconds;
    const Size size{.width = static_cast<float>(spec.width), .height = static_cast<float>(spec.height)};
    try {
        if (spec.backend == Backend::Headless) {
            HeadlessOptions opts;
            opts.size = size;
            opts.title = spec.title;
            opts.visibility = spec.visibility;
            session.adopt(create_window(opts));
        } else if (spec.backend == Backend::Auto) {
            WindowOptions opts;
            opts.size = size;
            opts.title = spec.title;
            opts.visibility = spec.visibility;
            session.adopt(create_native_window(opts));
        }
#ifdef AURORA_BACKEND_WIN32
        else if (spec.backend == Backend::Win32) {
            Win32Options opts;
            opts.size = size;
            opts.title = spec.title;
            opts.visibility = spec.visibility;
            session.adopt(create_window(opts));
        }
#endif
#ifdef AURORA_BACKEND_D3D11
        else if (spec.backend == Backend::D3D11) {
            D3D11Options opts;
            opts.size = size;
            opts.title = spec.title;
            opts.visibility = spec.visibility;
            session.adopt(create_window(opts));
        }
#endif
#ifdef AURORA_BACKEND_GLFW
        else if (spec.backend == Backend::Glfw) {
            GlfwOptions opts;
            opts.size = size;
            opts.title = spec.title;
            opts.visibility = spec.visibility;
            opts.resizable = false;
            opts.gpu = spec.gpu;
            session.adopt(create_window(opts));
        }
#endif
#ifdef AURORA_BACKEND_X11
        else if (spec.backend == Backend::X11) {
            X11Options opts;
            opts.size = size;
            opts.title = spec.title;
            opts.visibility = spec.visibility;
            session.adopt(create_window(opts));
        }
#endif
#ifdef AURORA_BACKEND_WAYLAND
        else if (spec.backend == Backend::Wayland) {
            WaylandOptions opts;
            opts.size = size;
            opts.title = spec.title;
            opts.visibility = spec.visibility;
            session.adopt(create_window(opts));
        }
#endif
#ifdef AURORA_BACKEND_GPU_WGPU
        else if (spec.backend == Backend::Wgpu) {
            WgpuOptions opts;
            opts.size = size;
            opts.title = spec.title;
            opts.visibility = spec.visibility;
            session.adopt(create_window(opts));
        }
#endif
        else {
            session.reason_ = std::string{"backend not compiled in this build: "} + backend_name(spec.backend);
        }
    } catch (const std::exception &ex) {
        // 建窗中途抛出（无显示环境 / 驱动初始化失败）等同「后端不可用」，翻译为原因交给用例层。
        session.window_.reset();
        session.reason_ = std::string{"window creation failed: "} + ex.what();
    }
    return session;
}

}  // namespace aurora::e2e
