#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/perf_overlay.h"
#include "aurora/app/scene.h"
#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/widget/widget.h"
#include "aurora/window/window.h"

namespace aurora {

/**
 * @brief 窗口宿主：一个「完整可运行单元」 = `Window`（可空） + `Scene` + `FocusManager` + 派发器
 *        + per-window 帧统计（specification/06-app-platform.md §2.4）。
 *
 * 职责边界（与 `Application` 的分工）：
 * - `WindowHost` **拥有**一个窗口的全部运行期状态：UI 树、焦点序、指针/触控捕获表、帧统计。
 *   事件只在本宿主的作用域内派发——**焦点不跨窗口**，一个窗口的输入不会影响另一个窗口。
 * - `Application` **不拥有**这些状态，它只持有一组 `WindowHost`，负责统一 pump 事件、
 *   推进共享的 `Animator`/`Scheduler`、逐宿主渲染、聚合等待时长与回收已关闭窗口。
 *   `Animator`/`Scheduler`/快捷键/命令注册表是应用级共享的，不入本类。
 *
 * **`Window` 可为 nullptr（无头宿主）**：对应既有无头 `Application(Scene, w, h)` 用法——
 * 有 UI 树、可 render_to_png 与程序化派发，但没有 OS 窗口。把它统一建模成「没有 Window 的宿主」
 * 消除了「有/无窗口」两条并行代码路径，帧循环与回收逻辑只有一套。
 *
 * 生命周期：由 `Application` 独占持有（`std::unique_ptr<WindowHost>`）。`teardown()` 先解绑后端
 * 回调并清焦点，**销毁顺序由成员声明序保证**（见 `window_` 与 `scene_` 的声明注释），
 * 使 `Window` 内的 `cached_root_` 先于 UI 树释放，不留下悬垂引用。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none (rendering happens in render_frame)
 * @note Rebuildable: no
 */
class WindowHost {
  public:
    /// @brief 构造一个窗口宿主。
    /// @param id      进程内唯一窗口标识（由 `Application` 分配）。
    /// @param scene   本窗口的 UI 树（宿主独占）。
    /// @param window  后端窗口；**可为 nullptr** —— 表示无头宿主（无 OS 窗口）。
    /// @param opts    窗口选项（角色/帧率上限/省电模式等）。
    WindowHost(WindowId id, Scene scene, std::unique_ptr<Window> window, const WindowOptions &opts = {});

    WindowHost(const WindowHost &) = delete;
    auto operator=(const WindowHost &) -> WindowHost & = delete;
    WindowHost(WindowHost &&) = delete;
    auto operator=(WindowHost &&) -> WindowHost & = delete;

    // ---- 身份 ----

    [[nodiscard]] auto id() const -> WindowId { return id_; }
    [[nodiscard]] auto role() const -> WindowRole { return role_; }
    auto set_role(WindowRole r) -> void { role_ = r; }
    [[nodiscard]] auto options() const -> const WindowOptions & { return opts_; }

    // ---- 组成 ----

    /// @brief 是否持有 OS 窗口（false = 无头宿主）。
    [[nodiscard]] auto has_window() const -> bool { return window_ != nullptr; }
    /// @brief 后端窗口（无头宿主返回 nullptr；调用方须判空）。
    [[nodiscard]] auto window() -> Window * { return window_.get(); }
    [[nodiscard]] auto window() const -> const Window * { return window_.get(); }
    [[nodiscard]] auto scene() -> Scene & { return scene_; }
    [[nodiscard]] auto focus() -> FocusManager & { return focus_; }
    /// @brief 本窗口的帧统计：默认指向进程级单例 `FrameStats::instance()`，
    /// 多窗口下由 `Application` 经 `own_frame_stats()` 切到宿主自有实例，互不污染。
    [[nodiscard]] auto frame_stats() -> FrameStats & { return *active_stats_; }
    [[nodiscard]] auto frame_stats() const -> const FrameStats & { return *active_stats_; }

    /// @brief 切换到宿主自有的帧统计实例（幂等），并重新绑定到 `Window`。
    ///
    /// **为什么要保留「默认共享全局单例」这条路径**：既有 `PerfOverlay` / `bench_idle_cpu` /
    /// 性能集成测试直接读 `FrameStats::instance()`，单窗口用法必须继续写入该实例才不回归。
    /// 因此 `Application` 只在登记**第二个**窗口起才把宿主切到自有实例——单窗口行为与历史逐位一致，
    /// 多窗口则各窗隔离。
    auto own_frame_stats() -> void;

    // ---- 接线（由 Application 在登记宿主后调用一次）----

    /// @brief 把后端 Surface 的事件 / 窗口状态 / 光标通道接到本宿主，并绑定 per-window 帧统计。
    /// 重复调用等价于再次接线（幂等覆盖），无 Window 的宿主为空操作。
    auto attach_surface() -> void;

    /// @brief 注册键盘事件前置拦截器（命中即消费，不再向焦点控件派发）。
    ///
    /// `Application` 用它注入「应用级共享 `ShortcutRegistry` 优先匹配」语义：快捷键是应用级概念
    /// （跨窗口一致），但匹配结果作用在**当前宿主**的焦点上下文里。
    auto set_key_pre_handler(std::function<bool(Event &)> h) -> void { key_pre_ = std::move(h); }

    /// @brief 注册窗口可见性状态上报表（由 `Application` 聚合为响应式 State / 命令式回调）。
    auto set_state_sink(std::function<void(WindowState)> cb) -> void { state_sink_ = std::move(cb); }
    /// @brief 注册窗口几何态上报表（语义同 `set_state_sink`）。
    auto set_mode_sink(std::function<void(WindowMode)> cb) -> void { mode_sink_ = std::move(cb); }

    // ---- 每帧步骤（由 Application 统一驱动）----

    /// @brief pump 本窗口的原生事件（无头宿主为空操作）。
    auto pump_events() -> void;

    /// @brief 手势计时推进（直觉 ratings → `Node::tick`，供长按等手势阈值检测）。
    auto tick() -> void;

    /// @brief 渲染本窗口一帧：`Window::present_root` + 本窗口帧统计。
    /// @param dt 本帧时间间隔（秒），供 `FrameStats::record` 推导 FPS。
    /// @return 渲染结果（idle 跳过帧亦为 true）。无头宿主恒 true。
    [[nodiscard]] auto render_frame(double dt) -> Result<bool>;

    /// @brief 计算本帧末尾的等待时长（毫秒），供帧循环取跨窗口最小值后一次性 `wait_events`。
    ///
    /// 语义同 `compute_wait_timeout`：`<0` 无限等待 / `0` 不等 / `>0` 等待该毫秒数。
    /// 无头宿主返回 `0`（不参与等待决策）。
    [[nodiscard]] auto decide_wait(double frame_budget_ms, bool anim_active, double next_deadline_ms,
                                   double elapsed_ms) const -> double;

    // ---- 关闭 ----

    /// @brief 是否已请求关闭（窗口 × / `request_close()`）；无头宿主恒 false。
    [[nodiscard]] auto should_close() const -> bool;
    /// @brief 程序化请求关闭（经 `Window::close()`；无头宿主为空操作）。
    auto request_close() -> void;

    /// @brief 解绑后端回调并清理焦点/拦截器（**不**释放成员，释放在析构）。
    ///
    /// 必须在销毁宿主**之前**调用：Surface 的回调 lambda 捕获了 `this`，若宿主析构后
    /// 后端仍在 pumping 消息（同一消息泵未停），回调会打到已释放对象。
    auto teardown() -> void;

    // ---- 显示、z 序与 DPI（多窗口）----

    /// @brief 本窗口当前所在显示器的 id（与 `app::Display::id` 同源）；无窗口/未知返回 -1。
    [[nodiscard]] auto display_id() const -> int;
    /// @brief 把本窗口迁移到指定显示器（居中到其工作区）。
    auto move_to_display(int display_id) -> void;
    /// @brief 几何是否已持久化（帧末回收阶段的幂等守卫，避免重复写存储）。
    [[nodiscard]] auto geometry_saved() const -> bool { return geometry_saved_; }
    /// @brief 标记几何已持久化（由 `Application` 在帧末调用）。
    auto mark_geometry_saved() -> void { geometry_saved_ = true; }

    /// @brief 提升本窗口到同组 z 序顶部（不改变激活状态）。
    auto raise() -> void;
    /// @brief 激活本窗口（置顶 + 取得键盘焦点）。
    auto focus_window() -> void;

    // ---- 事件派发（作用域恒为本宿主的 Scene + FocusManager）----

    /// @brief 集中派发来自后端的上抛事件。
    auto dispatch(Event &e) -> void;
    auto dispatch_pointer(float x, float y, MouseAction action) -> void;
    auto dispatch_key(KeyEvent e) -> bool;
    auto dispatch_text(TextInputEvent e) -> bool;
    auto dispatch_touch(const TouchEvent &e) -> void;
    auto dispatch_file_drop(const std::vector<std::string> &paths, float x, float y) -> void;

  private:
    /// @brief 窗口可见性状态上报：更新 Window 快照后转交 sink（`Application` 聚合）。
    auto on_window_state(WindowState s) -> void;
    /// @brief 窗口几何态上报（语义同 `on_window_state`）。
    auto on_window_mode(WindowMode m) -> void;

    /// @brief DPI 缩放变化：强制全量重排重绘。
    ///
    /// 逻辑↔物理换算变更后，沿用旧的布局缓存与帧缓冲会内容错位/发虚；HUD 离屏缓冲由
    /// `Window::render_hud` 按 scale 变化自动重建，此处只需标脏整帧。
    auto on_scale_changed() -> void;

    WindowId id_;
    std::string scroll_scope_;  ///< `ScrollStorage` 作用域键（本窗口 id 的字符串形式，构造时定稿）
    WindowRole role_;
    WindowOptions opts_;
    // ⚠️ 声明顺序即销毁顺序的逆序：`window_` 声明在 `scene_` 之后 → `window_` 先析构，
    // 其 `cached_root_`（持 root 的 Node 副本）先释放，UI 树随后销毁，不留下悬垂引用。
    Scene scene_;
    std::unique_ptr<Window> window_;
    FocusManager focus_;
    EventDispatcher mouse_;  ///< 鼠标指针捕获表（实例级，防止跨窗口捕获串味）。
    TouchDispatcher touch_;  ///< 多点触控指针捕获表（实例级）。
    FrameStats stats_;  ///< 宿主自有帧统计实例（切换后启用）。
    FrameStats *active_stats_ = &FrameStats::instance();  ///< 当前生效的统计实例（见 `own_frame_stats`）。
    std::function<bool(Event &)> key_pre_;  ///< 键盘前置拦截（应用级快捷键）。
    std::function<void(WindowState)> state_sink_;  ///< 可见性状态上报表。
    std::function<void(WindowMode)> mode_sink_;  ///< 几何态上报表。
    bool close_requested_ = false;  ///< 程序化关闭请求（见 `request_close`；后端无关的兜底通道）。
    bool geometry_saved_ = false;  ///< 几何是否已持久化（见 `geometry_saved()`）。
    bool torn_down_ = false;  ///< teardown 幂等守卫。
};

}  // namespace aurora
