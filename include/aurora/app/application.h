#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/app/scene.h"
#include "aurora/app/scheduler.h"
#include "aurora/app/shortcuts.h"
#include "aurora/core/log.h"
#include "aurora/core/strict_mode.h"
#include "aurora/core/thread.h"
#include "aurora/core/types.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/state/async.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"
#include "aurora/window/frame_pacing.h"
#include "aurora/window/window.h"
#include "aurora/window/window_state.h"

namespace aurora {

/**
 * @brief 应用组合根：持 Scene 与画布尺寸，提供渲染与同步事件派发。
 *
 * 对应 ARCHITECTURE.md §4.6 / §3.1：单线程 UI；事件同步派发，经 `EventDispatcher` + `FocusManager`
 * 统一处理指针/键盘/滚轮/文本输入，并维护焦点序（Tab 导航）。
 *
 * 后端组合（ARCHITECTURE.md §8.4 / specification/03-layout-render.md §8.5）：
 * - `Application(Scene, unique_ptr<Window>)` 接受由 `create_window(XxxOptions)` 工厂产出的预组装 `Window`；
 *   并注册事件处理器把原生事件经 `EventDispatcher` 集中派发；随后可 `run()` 进入帧循环。
 * - `Application(Scene, unique_ptr<Surface>)` 注入任意自定义 `Surface`（自定义后端稳定入口）。
 * - `Application(Scene, int, int)` 为无头便捷构造（不持有 `Window`），仅用于 `render_to_png` 与
 *   程序化派发（`dispatch_*`），向后兼容既有无头测试。
 *
 * @note Thread: main-thread only
 * @note Side-effects: paints (run loop renders frames)
 * @note Rebuildable: no
 */
class Application {
  public:
    /// @brief 无头便捷构造：不持有 Window，仅用于 render_to_png / 程序化派发。
    Application(Scene scene, int width, int height) : scene_(std::move(scene)), width_(width), height_(height) {
        focus_.set_root(&scene_.root());
    }

    /// @brief 自定义后端（稳定入口，不随 backend 数量增长）：注入已构造的 `unique_ptr<Surface>`。
    /// 经 `create_window(move(surface), opts)` 组装 `Window` 并接线。空 surface 仅 WARN 降级
    /// （错误归属调用方，其持有 `create_window` 的 `Result`）；该路径在「仅自定义 backend」构建下
    /// 始终可用，不依赖任何内置后端是否编译进构建。
    Application(Scene scene, std::unique_ptr<Surface> surface, const WindowOptions &opts = {})
        : scene_(std::move(scene)), width_(static_cast<int>(opts.size.width)),
          height_(static_cast<int>(opts.size.height)), opts_(opts) {
        focus_.set_root(&scene_.root());
        auto res = create_window(std::move(surface), opts);
        if (res) {
            attach_window(std::move(res.value()));
        } else {
            AURORA_LOG_WARN("app", std::string("custom surface create_window failed: ") + res.error().message);
        }
    }

    /// @brief 接受已组装的 `Window`（由 `create_window(XxxOptions)` 工厂产出，类型安全在工厂处保证）。
    /// `opts` 透传运行期参数（尤其 `max_frames`，控制 `run()` 帧数）；其 `size`/`title` 以 `Window` 实值为准。
    /// 空 `Window` 仅 WARN 降级（错误归属调用方，其持有工厂的 `Result`）；`Application`/`App` 只认
    /// `unique_ptr<Window>`，不随 backend 增加而增加构造函数。
    Application(Scene scene, std::unique_ptr<Window> window, const WindowOptions &opts = {})
        : scene_(std::move(scene)) {
        focus_.set_root(&scene_.root());
        if (window) {
            width_ = static_cast<int>(window->size().width);
            height_ = static_cast<int>(window->size().height);
            opts_ = opts;
            opts_.size = window->size();
            opts_.title = window->title();
            attach_window(std::move(window));
        } else {
            AURORA_LOG_WARN("app",
                            "Application(Scene, unique_ptr<Window>): null Window; running headless "
                            "(render_to_png only).");
        }
    }

    [[nodiscard]] auto scene() -> Scene & { return scene_; }

    /// @brief 焦点管理器（键盘导航 / 焦点派发）。
    [[nodiscard]] auto focus() -> FocusManager & { return focus_; }

    /// @brief 已组合的后端窗口（可能为 nullptr，如后端创建失败或无头构造）。
    [[nodiscard]] auto window() const -> Window * { return window_.get(); }

    /// @brief 帧动画管理器：每帧由 run() 按 dt 驱动，供上层注册 AnimationController。
    [[nodiscard]] auto animator() -> Animator & { return anim_; }

    /// @brief 定时任务调度器：每帧由 run() 按 dt 驱动，供 `set_timeout`/`set_interval` 与组件级 `Timer` 使用。
    [[nodiscard]] auto scheduler() -> Scheduler & { return sched_; }

    /// @brief 快捷键注册表：在键盘事件派发到焦点控件前优先匹配（specification/06-app-platform.md §8.4）。
    /// 用法：`app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::O}, []{ open(); })`。
    [[nodiscard]] auto shortcuts() -> ShortcutRegistry & { return shortcuts_; }

    /// @brief 设置每帧回调（在 present_root 之前调用），用于注入自定义每帧逻辑
    ///        （如把共享状态写入 Reactive 标签）。默认为空。
    auto set_on_frame(std::function<void()> cb) -> void { on_frame_ = std::move(cb); }

    /// @brief 设置 HUD 叠加层（分层 HUD）。
    /// 转发给组合的后端 `Window`；叠加层独立于 widget 树渲染，详见 `Window::set_overlay`。
    auto set_overlay(std::shared_ptr<Widget> overlay) const -> void {
        if (window_) {
            window_->set_overlay(std::move(overlay));
        }
    }

    /// @brief 当前窗口可见性状态（响应式：在 `Effect` 内读取可自动订阅刷新）。
    /// 取值见 `WindowState`：Visible（前台激活）/ Occluded（失焦被遮挡）/ Hidden（最小化）。
    [[nodiscard]] auto window_state() -> State<WindowState> & { return window_state_; }
    /// @brief 当前窗口几何态（响应式：在 `Effect` 内读取可自动订阅刷新）。
    /// 取值见 `WindowMode`：Normal / Maximized / Minimized / FullScreen。
    [[nodiscard]] auto window_mode() -> State<WindowMode> & { return window_mode_; }

    /// @brief 注册窗口可见性状态命令式回调（与 `window_state()` 响应式订阅并存）。
    auto set_on_window_state(std::function<void(WindowState)> cb) -> void { on_window_state_ = std::move(cb); }
    /// @brief 注册窗口几何态命令式回调（与 `window_mode()` 响应式订阅并存）。
    auto set_on_window_mode(std::function<void(WindowMode)> cb) -> void { on_window_mode_ = std::move(cb); }

    /// @brief 设置运行时严格模式（specification/01-core.md §4.3 / CI 门禁）。
    /// `run()` 期间套用到线程级开关；默认 Off。
    auto set_strict_mode(StrictMode m) -> void { strict_ = m; }

    /// @brief 当前严格模式（App 上下文）。
    [[nodiscard]] auto strict_mode() const -> StrictMode { return strict_; }

    /// @brief 运行帧循环：每帧 pump 事件（→ 集中派发）→ 渲染根 → tick；到 should_close 退出。
    ///
    /// 事件驱动帧循环：每帧末尾经 `compute_wait_timeout` 决策下次唤醒——
    /// 有脏区/动画时按帧预算（`WindowOptions::max_fps`）节流；完全空闲时阻塞等待事件或
    /// 最近定时任务到期（静态界面 CPU 趋近 0）；`power_saving=false` 退回旧忙轮询。
    /// 同时安装主线程投递器：`au::async` 的 then 回调经队列回投主线程，并 `request_wake`
    /// 唤醒睡眠中的帧循环（无运行循环时行为不变：直接在完成线程调用）。
    AURORA_MAIN_THREAD auto run() -> void {
        if (!window_) {
            AURORA_LOG_WARN(
                "app",
                "run() has no available Window backend; use Application(Scene, unique_ptr<Window>) (Window provided by "
                "produced by au::create_window(XxxOptions)) or Application(Scene, unique_ptr<Surface>).");
            return;
        }
        const StrictMode prev_strict = aurora::strict_mode();
        aurora::set_strict_mode(strict_);
        auto last = std::chrono::steady_clock::now();
        Scheduler::set_current(&sched_);
        // 帧预算同步到 FrameStats（掉帧/hitch 判定与 max_fps 一致；0 = 不限帧率保持默认 16.67ms）。
        if (opts_.max_fps > 0) {
            FrameStats::instance().set_frame_budget_ms(1000.0 / opts_.max_fps);
        }
        // 跨线程回投：后台线程的 then 回调入队 + 唤醒睡眠中的主循环，
        // 下一帧开头在主线程排水执行（兼具线程安全与不丢唤醒）。
        Task<bool>::set_main_poster([this](std::function<void()> fn) -> void {
            {
                std::scoped_lock lk(posted_mutex_);
                posted_.push_back(std::move(fn));
            }
            if (window_) {
                window_->surface().request_wake();
            }
        });
        window_->run(
            [this, last]() mutable -> void {
                auto now = std::chrono::steady_clock::now();
                const double dt = std::chrono::duration<double>(now - last).count();
                last = now;
                drain_posted();  // 先执行后台回投的主线程工作（可能标脏，当帧即可刷新）
                if (on_frame_) {
                    on_frame_();
                }
                tick();
                anim_.tick(dt);
                sched_.tick(dt);  // 定时任务随帧推进（在 present 前触发，当帧 UI 即可刷新）
                // 先尝试渲染，再根据结果决定帧统计方式
                (void)window_->present_root(scene_.root_node());
                // present_root 返回 Result<bool>：
                //   - idle 跳过时返回 true（但内部未做任何渲染）
                //   - 正常渲染完成也返回 true
                //   - 渲染失败返回 false
                // 使用 Window 提供的脏区状态判断是否 idle
                if (window_->is_idle_frame()) {
                    FrameStats::instance().record_idle();
                } else {
                    FrameStats::instance().record(dt);  // 帧统计（PerfOverlay/工具消费，ARCHITECTURE.md §10.1）
                }
                // 帧调度决策：本帧末尾计算下次唤醒等待，交由 Window::run 执行。
                // 决策在 present_root 之后取脏——渲染期间产生的新脏（如动画 State 写回）
                // 已在 m_dirty 中，查完脏再睡，同线程不丢帧；跨线程经 request_wake 唤醒。
                if (opts_.power_saving) {
                    const double elapsed_ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
                    const double budget_ms = opts_.max_fps > 0 ? 1000.0 / opts_.max_fps : 0.0;
                    const bool pending_posted = [this]() -> bool {
                        std::scoped_lock lk(posted_mutex_);
                        return !posted_.empty();
                    }();
                    // 活跃信号：运行中动画，或本帧实际渲染了（非 idle）——后者覆盖
                    // 「每帧在 on_frame 里标脏」类模式（脏已被同帧 present 消费，
                    // 仅看脏会误判空闲而深睡）；真正空闲帧（idle 跳过）不受影响。
                    const bool active = anim_.has_active() || !window_->is_idle_frame();
                    const double wait_ms =
                        pending_posted
                            ? 0.0  // 已有待排水的回投工作：不睡，立即进入下一帧
                            : compute_wait_timeout(window_->has_pending_dirty(), active, sched_.next_deadline_ms(),
                                                   budget_ms, elapsed_ms, window_->surface().paces_frames());
                    window_->set_next_wait(wait_ms);
                }
            },
            opts_.max_frames);
        // 卸载投递器并排尽残留（仍在主线程）：退出后回到「无 poster 直接调用」的默认行为。
        Task<bool>::set_main_poster(nullptr);
        drain_posted();
        Scheduler::set_current(nullptr);
        aurora::set_strict_mode(prev_strict);
    }

    /// @brief 渲染当前场景到 PNG。运行期同样套用严格模式（specification/01-core.md §4.3 / CI 门禁），
    ///        使无窗口后端（HeadlessSurface）的离屏渲染也能触发严格失败。
    [[nodiscard]] auto render_to_png(const char *path) -> Result<bool> {
        const StrictMode prev_strict = aurora::strict_mode();
        aurora::set_strict_mode(strict_);
        Result<bool> r = scene_.render_to_png(path, width_, height_);
        aurora::set_strict_mode(prev_strict);
        return r;
    }

    /// @brief 在坐标 (x,y) 处命中测试并派发指针按下事件（同步回调，ARCHITECTURE.md §3.1）。
    /// 经 `EventDispatcher` 路径触发 `on_pointer_event`（Button::on_click 与 Clickable 回调）。
    auto dispatch_click(float x, float y) -> void;

    /// @brief 派发任意指针动作（Press/Release/Move），用于拖拽与长按手势。
    /// 例：拖拽 = 依次 `dispatchPointer(x,y,Press)` → 多次 `Move` → `Release`。
    auto dispatch_pointer(float x, float y, MouseAction action) -> void;

    /// @brief 驱动手势计时（长按阈值检测）；应在每帧或每次派发后调用一次。
    auto tick() -> void;

    /// @brief 同步派发键盘事件（Tab/Shift+Tab 触发焦点移动，否则派发到焦点 widget）。
    auto dispatch_key(KeyEvent e) -> bool;

    /// @brief 同步派发文本输入事件到焦点 widget。
    auto dispatch_text(TextInputEvent e) -> bool;

    /// @brief 同步派发多点触控事件（按 pointer id 做指针捕获与并发路由，ARCHITECTURE.md §3.1）。
    /// 派发器对每个触点：① 把完整 `TouchEvent` 交给命中链（`touch()` 原始流 / `PinchRecognizer`）；
    /// ② 合成对应 `MouseEvent`（携带 `pointer_id`）驱动 `Draggable`/`LongPress`/`Clickable`。
    auto dispatch_touch(const TouchEvent &e) -> void;

    /// @brief 同步派发操作系统文件拖放事件到命中控件（窗口逻辑坐标）。
    /// 经 `EventDispatcher` 路径触发 `on_file_drop`（specification/06-app-platform.md §8 平台 Shell）。
    auto dispatch_file_drop(const std::vector<std::string> &paths, float x, float y) -> void {
        FileDropEvent e;
        e.position = Point{.x = x, .y = y};
        e.paths = paths;
        dispatch(e);
    }

  private:
    /// @brief 集中事件派发：把后端上抛的 Event 按类型经 EventDispatcher 派发到 widget 树。
    auto dispatch(Event &e) -> void {
        auto &root = scene_.root();
        if (auto *m = dynamic_cast<MouseEvent *>(&e)) {
            mouse_.dispatch_mouse(root, *m, &focus_);
        } else if (auto *s = dynamic_cast<ScrollEvent *>(&e)) {
            EventDispatcher::dispatch(root, *s);
        } else if (auto *k = dynamic_cast<KeyEvent *>(&e)) {
            // 快捷键优先：匹配到已启用绑定则消费，不再向焦点控件派发。
            if (shortcuts_.handle(*k, focus_.focused() != nullptr)) {
                k->is_handled = true;
            } else {
                EventDispatcher::dispatch(root, *k, focus_);
            }
        } else if (auto *t = dynamic_cast<TextInputEvent *>(&e)) {
            EventDispatcher::dispatch(root, *t, focus_);
        } else if (auto *te = dynamic_cast<TouchEvent *>(&e)) {
            touch_.dispatch(root, *te, &focus_);
        } else if (auto *fde = dynamic_cast<FileDropEvent *>(&e)) {
            EventDispatcher::dispatch(root, *fde);
        }
    }

    /// @brief 窗口可见性状态变化时聚合为响应式 State 并 forward 到 Window（供根 Environment 注入）。
    /// 仅在实际状态发生转移（与已知态不同）时更新 State 并触发回调，避免重复事件刷屏。
    auto on_window_state_changed(WindowState s) -> void {
        if (s == window_state_.get()) {
            return;
        }
        window_state_.set(s);
        if (window_) {
            window_->set_window_state(s);
        }
        if (on_window_state_) {
            on_window_state_(s);
        }
    }

    /// @brief 窗口几何态变化时聚合为响应式 State 并 forward 到 Window（供根 Environment 注入）。
    /// 仅在实际几何态发生转移（与已知态不同）时更新 State 并触发回调，避免重复事件刷屏。
    auto on_window_mode_changed(WindowMode m) -> void {
        if (m == window_mode_.get()) {
            return;
        }
        window_mode_.set(m);
        if (window_) {
            window_->set_window_mode(m);
        }
        if (on_window_mode_) {
            on_window_mode_(m);
        }
    }

    /// @brief 接线共享三段：注入 Window 后集中事件派发 + 窗口级可见性/几何态上报聚合。
    /// 所有持有 `Window` 的构造（自定义 Surface / 预组装 Window）共用，避免重复接线。
    auto attach_window(std::unique_ptr<Window> w) -> void {
        window_ = std::move(w);
        window_->surface().set_event_handler([this](Event &e) -> void { dispatch(e); });
        window_->surface().set_window_state_handler([this](WindowState s) -> void { on_window_state_changed(s); });
        window_->surface().set_window_mode_handler([this](WindowMode m) -> void { on_window_mode_changed(m); });
    }

    /// @brief 排水跨线程回投队列（主线程，每帧开头/退出前调用）。
    auto drain_posted() -> void {
        std::vector<std::function<void()>> q;
        {
            std::scoped_lock lk(posted_mutex_);
            q.swap(posted_);
        }
        for (auto &fn : q) {
            if (fn) {
                fn();
            }
        }
    }

    Scene scene_;
    FocusManager focus_;
    TouchDispatcher touch_;  ///< 多点触控指针捕获表（实例级，避免跨子树悬空引用）
    EventDispatcher mouse_;  ///< 鼠标指针捕获表（实例级）：Press 后即使光标移出窗口/重叠控件仍持续派发
    StrictMode strict_ = StrictMode::Off;  ///< 严格模式（run() 期间套用到线程级开关）
    int width_ = 0;
    int height_ = 0;
    WindowOptions opts_;  ///< 保留用于 run() 的 max_frames 等。
    std::unique_ptr<Window> window_;  ///< 组合的后端窗口（无头构造时为空）。
    std::function<void()> on_frame_;  ///< 每帧回调（在 present_root 前调用）。
    Animator anim_;  ///< 帧动画管理器（run() 每帧按 dt 推进）。
    Scheduler sched_;  ///< 定时任务调度器（run() 每帧按 dt 推进）。
    ShortcutRegistry shortcuts_;  ///< 快捷键注册表（键盘事件派发前优先匹配）。
    State<WindowState> window_state_{WindowState::Visible};  ///< 窗口可见性状态（响应式）。
    State<WindowMode> window_mode_{WindowMode::Normal};  ///< 窗口几何态（响应式）。
    std::function<void(WindowState)> on_window_state_;  ///< 可见性状态命令式回调。
    std::function<void(WindowMode)> on_window_mode_;  ///< 几何态命令式回调。
    std::mutex posted_mutex_;  ///< 跨线程回投队列锁。
    std::vector<std::function<void()>> posted_;  ///< 待主线程执行的回投工作。
};

/// @brief 流式应用构建器（specification/06-app-platform.md
/// §4）：`au::App().title("X").size(800,600).view(root).run()`。
///
/// 与既有 `Application` 构造语义一致：`run()` 内部构造 `Application` 并进入帧循环，不破坏旧用法。
/// 后端可由 `.window()`（预组装 Window）/ `.surface()`（自定义 Surface）指定，
/// 优先级依次递增；不指定时 `run()` 经 `auto_detect_surface()` 自动选择可用后端。需要自定义每帧逻辑（如驱动外部
/// Animator）可用 `on_frame`。
///
/// @note Thread: main-thread only
/// @note Side-effects: paints
/// @note Rebuildable: no
class App {
  public:
    App() = default;
    explicit App(Node view) : view_(std::move(view)) {}

    /// @brief 默认构造（供自由函数 `au::App()` 使用）。
    static auto make() -> App { return App{}; }

    /// @brief 设置窗口标题。
    auto title(std::string t) -> App & {
        title_ = std::move(t);
        return *this;
    }
    /// @brief 自定义后端（稳定入口）：注入已构造 `Surface`，`run()` 时经 `create_window` 组装 `Window`。
    /// 与 `window()` 互斥；`run()` 优先级：自定义 Surface > 预组装 Window。
    auto surface(std::unique_ptr<Surface> surface) -> App & {
        custom_surface_ = std::move(surface);
        return *this;
    }
    /// @brief 接受已组装 `Window`（由 `create_window(XxxOptions)` 工厂产出）。
    auto window(std::unique_ptr<Window> win) -> App & {
        custom_window_ = std::move(win);
        return *this;
    }
    /// @brief 设置逻辑尺寸（设备无关像素）。
    auto size(int w, int h) -> App & {
        size_ = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
        return *this;
    }
    /// @brief 设置根 widget 树。
    auto view(Node v) -> App & {
        view_ = std::move(v);
        return *this;
    }
    /// @brief 设置每帧回调（在 present_root 之前调用）。
    auto on_frame(std::function<void()> cb) -> App & {
        on_frame_ = std::move(cb);
        return *this;
    }
    /// @brief 设置 HUD 叠加层（分层 HUD）。
    /// 转发给组合的后端 `Window`；叠加层独立于 widget 树渲染（如 `PerfOverlay`）。
    auto overlay(std::shared_ptr<Widget> w) -> App & {
        overlay_ = std::move(w);
        return *this;
    }
    /// @brief 限制帧数（默认 -1 跑到 should_close）；测试/一次性运行可用。
    auto frames(int n) -> App & {
        max_frames_ = n;
        return *this;
    }
    /// @brief 设置运行时严格模式（specification/01-core.md §4.3 / CI 门禁）。
    /// `run()` 期间套用到 Application 上下文（严格模式下降级即致命失败）。
    auto strict_mode(StrictMode m) -> App & {
        strict_ = m;
        return *this;
    }

    /// @brief 构建 `Application` 并进入帧循环。后端选择优先级：
    ///        自定义 Surface（`.surface()`）> 预组装 Window（`.window()`）> 自动检测（默认）。
    auto run() -> void {
        Scene scene{std::move(view_)};
        WindowOptions opts;
        opts.title = title_;
        opts.size = size_;
        opts.max_frames = max_frames_;
        if (custom_surface_) {
            Application app{std::move(scene), std::move(custom_surface_), opts};
            if (on_frame_) {
                app.set_on_frame(on_frame_);
            }
            app.set_strict_mode(strict_);
            if (overlay_) {
                app.set_overlay(std::move(overlay_));
            }
            app.run();
        } else if (custom_window_) {
            Application app{std::move(scene), std::move(custom_window_), opts};
            if (on_frame_) {
                app.set_on_frame(on_frame_);
            }
            app.set_strict_mode(strict_);
            if (overlay_) {
                app.set_overlay(std::move(overlay_));
            }
            app.run();
        } else {
            auto kind = auto_detect_surface();
            std::unique_ptr<Window> window;
            switch (kind) {
#ifdef AURORA_BACKEND_HEADLESS
                case SurfaceKind::Headless:
                    if (auto res = create_window(HeadlessOptions{opts})) {
                        window = std::move(res.value());
                    }
                    break;
#endif
#ifdef AURORA_BACKEND_WIN32
                case SurfaceKind::Win32:
                    if (auto res = create_window(Win32Options{opts})) {
                        window = std::move(res.value());
                    }
                    break;
#endif
#ifdef AURORA_BACKEND_GLFW
                case SurfaceKind::Glfw:
                    if (auto res = create_window(GlfwOptions{opts})) {
                        window = std::move(res.value());
                    }
                    break;
#endif
#ifdef AURORA_BACKEND_X11
                case SurfaceKind::X11:
                    if (auto res = create_window(X11Options{opts})) {
                        window = std::move(res.value());
                    }
                    break;
#endif
#ifdef AURORA_BACKEND_WAYLAND
                case SurfaceKind::Wayland:
                    if (auto res = create_window(WaylandOptions{opts})) {
                        window = std::move(res.value());
                    }
                    break;
#endif
#ifdef AURORA_BACKEND_MACOS
                case SurfaceKind::MacOS:
                    if (auto res = create_window(MacOSOptions{opts})) {
                        window = std::move(res.value());
                    }
                    break;
#endif
#ifdef AURORA_BACKEND_WASM
                case SurfaceKind::Wasm:
                    if (auto res = create_window(WasmOptions{opts})) {
                        window = std::move(res.value());
                    }
                    break;
#endif
                default:
                    break;
            }
            Application app{std::move(scene), std::move(window), opts};
            if (on_frame_) {
                app.set_on_frame(on_frame_);
            }
            app.set_strict_mode(strict_);
            if (overlay_) {
                app.set_overlay(std::move(overlay_));
            }
            app.run();
        }
    }

  private:
    Node view_;
    std::string title_{"Aurora"};
    Size size_{.width = 800.0F, .height = 600.0F};
    int max_frames_ = -1;
    StrictMode strict_ = StrictMode::Off;
    std::function<void()> on_frame_;
    std::unique_ptr<Surface> custom_surface_;  ///< 自定义后端（`.surface()`）：注入后 `run()` 组装 Window。
    std::unique_ptr<Window> custom_window_;  ///< 预组装 Window（`.window()`）：由 `create_window(XxxOptions)` 产出。
    std::shared_ptr<Widget> overlay_;  ///< HUD 叠加层（`.overlay()`）：独立于 widget 树渲染。
};

/// @brief 便捷构造：返回流式构建器（specification/06-app-platform.md §4）。
// NOLINTNEXTLINE(readability-identifier-naming): 工厂名 `App` 与类型同名，保持 CamelCase 以匹配流式 DSL
[[nodiscard]] inline auto App() -> App { return App::make(); }

}  // namespace aurora
