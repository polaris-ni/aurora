#pragma once

#include <algorithm>
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
#include "aurora/app/window_bus.h"
#include "aurora/app/window_host.h"
#include "aurora/commands.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"  // NOLINT
#include "aurora/core/strict_mode.h"
#include "aurora/core/thread.h"
#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/state/async.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"
#include "aurora/window/window.h"
#include "aurora/window/window_state.h"

#ifdef AURORA_PLATFORM_WASM
// 浏览器 rAF 帧循环接线（specification/06-app-platform.md §10 Web/WASM「事件循环」行）：
// 主线程不可阻塞，帧体经 emscripten_request_animation_frame_loop 回调按 vsync 驱动。
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

namespace aurora {

namespace preferences {
class Preferences;  // 前置声明：几何持久化存储（仅作指针成员，避免拉入 preferences.h 重型头）
}  // namespace preferences

class AudioContext;  // 前置声明：应用级默认音频上下文（仅作 shared_ptr 成员，避免拉入 audio.h）

/**
 * @brief 应用组合根：持有**一组窗口宿主**（`WindowHost`），以单一帧循环统一驱动它们。
 *
 * 对应 ARCHITECTURE.md §4.6 / §3.1：单线程 UI；事件同步派发，经 `EventDispatcher` + `FocusManager`
 * 统一处理指针/键盘/滚轮/文本输入，并维护焦点序（Tab 导航）。
 *
 * **多窗口模型（specification/06-app-platform.md §2.4）**：
 * - 每个窗口是一个 `WindowHost`：拥有自己的 `Scene`（UI 树）、`FocusManager`、指针/触控捕获表与
 *   帧统计。事件只在目标宿主的作用域内派发——**焦点与捕获不跨窗口**。
 * - `Application` 不拥有上述状态，只负责：统一 pump 事件 → 推进**共享**的 `Animator`/`Scheduler`
 *   → 逐宿主渲染 → 聚合等待时长后一次性阻塞等待 → 帧末回收已关闭窗口。
 * - `Animator` / `Scheduler` / `ShortcutRegistry` / `CommandRegistry` 是应用级共享的（`dt` 只推进
 *   一次、快捷键与命令语义跨窗口一致），不入宿主。
 * - `scene()` / `focus()` / `dispatch_*` / `window()` 等既有访问器作用于**主窗口**（默认首个登记的
 *   窗口）；单窗口用法的行为与历史完全一致。
 *
 * 后端组合（ARCHITECTURE.md §8.4 / specification/03-layout-render.md §8.5）：
 * - `Application(Scene, unique_ptr<Window>)` 接受由 `create_window(XxxOptions)` 工厂产出的预组装 `Window`；
 *   并以宿主为单位把原生事件经 `EventDispatcher` 集中派发；随后可 `run()` 进入帧循环。
 * - `Application(Scene, unique_ptr<Surface>)` 注入任意自定义 `Surface`（自定义后端稳定入口）。
 * - `Application(Scene, int, int)` 为无头便捷构造（登记一个「无 Window 的宿主」），仅用于
 *   `render_to_png` 与程序化派发（`dispatch_*`），向后兼容既有无头测试。
 * - 需要更多窗口时经 `open_window()` 追加宿主，返回 `WindowId`。
 *
 * @note Thread: main-thread only
 * @note Side-effects: paints (run loop renders frames)
 * @note Rebuildable: no
 */
class Application {
  public:
    /// @brief 无头便捷构造：登记一个**无 OS 窗口的宿主**，仅用于 render_to_png / 程序化派发。
    Application(Scene scene, int width, int height) : width_(width), height_(height) {
        (void)register_host(std::move(scene), nullptr, {});
    }

    /// @brief 自定义后端（稳定入口，不随 backend 数量增长）：注入已构造的 `unique_ptr<Surface>`。
    /// 经 `create_window(move(surface), opts)` 组装 `Window` 并登记宿主。组装失败仅 WARN 降级
    /// （错误归属调用方，其持有 `create_window` 的 `Result`）；该路径在「仅自定义 backend」构建下
    /// 始终可用，不依赖任何内置后端是否编译进构建。
    Application(Scene scene, std::unique_ptr<Surface> surface, const WindowOptions &opts = {})
        : width_(static_cast<int>(opts.size.width)), height_(static_cast<int>(opts.size.height)), opts_(opts) {
        std::unique_ptr<Window> window;
        auto res = create_window(std::move(surface), opts);
        if (res) {
            window = std::move(res.value());
        } else {
            AURORA_LOG_WARN("app", std::string("custom surface create_window failed: ") + res.error().message);
        }
        adopt_window_metrics(window);
        (void)register_host(std::move(scene), std::move(window), opts);
    }

    /// @brief 接受已组装的 `Window`（由 `create_window(XxxOptions)` 工厂产出，类型安全在工厂处保证）。
    /// `opts` 透传运行期参数（尤其 `max_frames`，控制 `run()` 帧数）；其 `size`/`title` 以 `Window` 实值为准。
    /// 空 `Window` 仅 WARN 降级（错误归属调用方，其持有工厂的 `Result`）；`Application`/`App` 只认
    /// `unique_ptr<Window>`，不随 backend 增加而增加构造函数。
    Application(Scene scene, std::unique_ptr<Window> window, const WindowOptions &opts = {}) : opts_(opts) {
        adopt_window_metrics(window);
        (void)register_host(std::move(scene), std::move(window), opts);
    }

    /// @brief 析构：WASM 构建下摘除 rAF 循环所有权——蹦床检出 `raf_owner_` 不匹配即自行终止，
    ///        不会解引用已析构实例（浏览器单线程，检查与使用之间无窗口期）。其他平台为保持
    ///        原隐式析构行为；本析构仅为 rAF 守卫存在（`raf_tick` 是类析构后仍会被调度的唯一回调）。
    ~Application() {
#ifdef AURORA_PLATFORM_WASM
        if (raf_owner_ == this) {
            raf_owner_ = nullptr;
        }
#endif
    }

    // 禁复制/移动**早已是既成事实**（成员含 `std::vector<std::unique_ptr<WindowHost>>` 故不可复制，
    // 且用户声明析构会抑制隐式移动），本处只是把隐式结果写成显式契约，不改变任何可编译性。
    // 实例持有原生窗口与 rAF 所有权，语义上也不该被搬走——「一个应用一个实例」。
    Application(const Application &) = delete;
    Application(Application &&) = delete;
    auto operator=(const Application &) -> Application & = delete;
    auto operator=(Application &&) -> Application & = delete;

    // ---- 多窗口（specification/06-app-platform.md §2.4）----

    /// @brief 追加一个窗口宿主并接管预组装 `Window`（由 `create_window(XxxOptions)` 产出）。
    /// @param window 可为 nullptr —— 退化为无 OS 窗口的宿主（渲染静默跳过，等同无头构造）。
    /// @param scene 该窗口的 UI 树（`Scene` 持有根 `Node`）。
    /// @param opts 窗口选项；`role`/`owner`/`modal`/`persist_id` 不随 `Window` 反推，须显式传入。
    /// @return 新登记的窗口 id。宿主立即接线，`run()` 的下一帧起即参与渲染。
    /// @note Thread: main-thread only
    auto open_window(std::unique_ptr<Window> window, Scene scene, const WindowOptions &opts = {}) -> WindowId {
        adopt_window_metrics(window);
        return register_host(std::move(scene), std::move(window), opts);
    }

    /// @brief 追加一个窗口宿主并注入自定义 `Surface`（内部经 `create_window` 组装 `Window`）。
    /// 组装失败返回 Error（不吞错、不登记任何宿主），错误归属调用方。
    /// @param surface 已构造的自定义后端 `Surface`（所有权转移给内部 `Window`）。
    /// @param scene 该窗口的 UI 树（`Scene` 持有根 `Node`）。
    /// @param opts 窗口选项；`role`/`owner`/`modal`/`persist_id` 不随 `Window` 反推，须显式传入。
    [[nodiscard]] auto open_window(std::unique_ptr<Surface> surface, Scene scene, const WindowOptions &opts = {})
        -> Result<WindowId> {
        auto res = create_window(std::move(surface), opts);
        if (!res) {
            return make_error(
                ErrorCode::PlatformUnavailable, std::string("open_window(Surface): ") + res.error().message,
                "Pass a valid Surface, or use the Window-based open_window overload.", "aurora/app/application.h");
        }
        adopt_window_metrics(res.value());
        return Result<WindowId>{register_host(std::move(scene), std::move(res.value()), opts)};
    }

    /// @brief 请求关闭指定窗口：立即 `Window::close()`，`run()` 在下一次帧末回收宿主。
    auto close_window(WindowId id) const -> void;

    // ---- 退出策略与生命周期（specification/06-app-platform.md §2.4）----

    /// @brief 设置退出策略（默认 `ExitPolicy::LastWindowClosed`：关掉最后一个窗口即退出）。
    /// 单窗口用法下三种策略行为一致（关掉唯一的窗口即退出），多窗口下才显现差异。
    auto set_exit_policy(ExitPolicy p) -> void { exit_policy_ = p; }
    /// @brief 当前退出策略。
    [[nodiscard]] auto exit_policy() const -> ExitPolicy { return exit_policy_; }

    /// @brief 主动退出帧循环（下一次循环判定生效；**任何**策略下均有效）。
    ///
    /// 与「关闭窗口」解耦：`ExplicitOnly` 策略下这是唯一的退出方式；
    /// 其他策略下用于在窗口仍开着时提前结束 `run()`。
    auto quit() -> void { quit_requested_ = true; }

    /// @brief 指定主窗口（默认：首个登记的宿主）。
    ///
    /// 主窗口决定 `scene()`/`focus()`/`window()` 的作用对象，并参与 `MainWindowClosed`
    /// 退出策略。指定不存在的 id 为 no-op（保持当前主窗口）。
    auto set_main_window(WindowId id) -> void;

    /// @brief 当前主窗口 id（无宿主时为 `AURORA_INVALID_WINDOW_ID`）。
    [[nodiscard]] auto main_window() const -> WindowId {
        return main_host_ != nullptr ? main_host_->id() : AURORA_INVALID_WINDOW_ID;
    }

    /// @brief 注册窗口关闭回调：宿主被帧末回收**之后**触发一次，参数为被关闭窗口 id。
    /// 供上层释放与该窗口绑定的外部资源（不要在此回调内再关闭其他窗口）。
    auto set_on_window_closed(std::function<void(WindowId)> cb) -> void { on_window_closed_ = std::move(cb); }

    // ---- 跨窗通信与几何持久化（specification/06-app-platform.md §2.4）----

    /// @brief 跨窗口事件总线：类型化广播 / 点对点通知，主线程同步扇出。
    ///
    /// 共享状态请用既有 `Store<S>`（多窗口共享同一实例）；本总线只承载「一次性通知 / 命令」。
    [[nodiscard]] auto bus() -> WindowEventBus & { return bus_; }

    /// @brief 指定窗口几何持久化存储（`Preferences`）。
    ///
    /// 指定后，带 `WindowOptions::persist_id` 的窗口在**关闭时自动保存**几何、**打开时自动恢复**；
    /// 存储的几何不可用（显示器被拔除 / 完全落在屏幕外 / 尺寸非正）时回退默认布局并 WARN。
    ///
    /// 由于主窗口在 `Application` 构造期即已登记（彼时无法先设置存储），本方法会对**调用前
    /// 已登记**的窗口补做一次几何恢复——因此 `Application app{...}; app.set_window_geometry_store(&prefs);`
    /// 这种常见写法同样能恢复主窗口几何。
    ///
    /// 落盘时机由调用方决定（`Preferences::flush()`），本类不做隐式 I/O。
    auto set_window_geometry_store(preferences::Preferences *prefs) -> void;
    /// @brief 当前几何持久化存储（未指定时为 nullptr）。
    [[nodiscard]] auto window_geometry_store() const -> preferences::Preferences * { return geometry_store_; }

    /// @brief 按 id 取窗口宿主（未找到返回 nullptr）。
    [[nodiscard]] auto window_host(WindowId id) const -> WindowHost *;
    /// @brief 全部窗口宿主（按登记顺序，含主窗口）。
    [[nodiscard]] auto windows() const -> std::vector<WindowHost *>;
    /// @brief 当前登记的窗口数。
    [[nodiscard]] auto window_count() const -> std::size_t { return hosts_.size(); }

    [[nodiscard]] auto scene() const -> Scene & { return main_host_->scene(); }

    /// @brief 焦点管理器（键盘导航 / 焦点派发）——**主窗口**的焦点序。
    /// 多窗口下焦点不跨窗口：其他窗口请用 `window_host(id)->focus()`。
    [[nodiscard]] auto focus() const -> FocusManager & { return main_host_->focus(); }

    /// @brief 主窗口的后端 `Window`（可能为 nullptr，如后端创建失败或无头构造）。
    [[nodiscard]] auto window() const -> Window * { return main_host_->window(); }

    /// @brief 帧动画管理器：每帧由 run() 按 dt 驱动，供上层注册 AnimationController。
    [[nodiscard]] auto animator() -> Animator & { return anim_; }

    /// @brief 定时任务调度器：每帧由 run() 按 dt 驱动，供 `set_timeout`/`set_interval` 与组件级 `Timer` 使用。
    [[nodiscard]] auto scheduler() -> Scheduler & { return sched_; }

    /// @brief 应用级默认音频上下文（**惰性创建**，首次调用时构造并启动内置设备后端）。
    ///
    /// 内置后端未编译（`AURORA_ENABLE_AUDIO=OFF`）或设备启动失败时自动进入静默模式：
    /// 图照常运转、样本消费后丢弃（`AudioContext::device_state()==Silent`），调用方无需分支。
    /// 典型接线：`player.set_audio_context(app.audio_shared());`（见 `VideoPlayer`）。
    /// @note Thread: main-thread only（图变更走命令环，渲染在设备线程或 render_block）
    auto audio() -> AudioContext &;
    /// @brief 默认音频上下文的 shared_ptr 形态（同源惰性创建；传给 `VideoPlayer::set_audio_context`）。
    auto audio_shared() -> const std::shared_ptr<AudioContext> &;

    /// @brief 快捷键注册表：在键盘事件派发到焦点控件前优先匹配（specification/06-app-platform.md §8.4）。
    /// 用法：`app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::O}, []{ open(); })`。
    [[nodiscard]] auto shortcuts() -> ShortcutRegistry & { return shortcuts_; }

    /// @brief 命令注册表：快捷键、菜单与命令面板的统一真源（specification/06-app-platform.md §8.4）。
    /// 用法：注册命令后经 `app.commands().bind_shortcuts(app.shortcuts())` 接入默认快捷键；
    /// 菜单与命令面板分别经 `to_menu_items()` / `CommandPalette` 消费同一份命令。
    [[nodiscard]] auto commands() -> CommandRegistry & { return commands_; }

    /// @brief 设置每帧回调（在 present_root 之前调用），用于注入自定义每帧逻辑
    ///        （如把共享状态写入 Reactive 标签）。默认为空。
    auto set_on_frame(std::function<void()> cb) -> void { on_frame_ = std::move(cb); }

    /// @brief 设置 HUD 叠加层（分层 HUD），作用于**主窗口**。
    /// 转发给组合的后端 `Window`；叠加层独立于 widget 树渲染，详见 `Window::set_overlay`。
    /// `PerfOverlay` 会被自动绑定到主窗口的帧统计实例（多窗口下不读混合数据）。
    ///
    /// 其他窗口请显式绑定：`app.window_host(id)->window()->set_overlay(std::make_shared<au::PerfOverlay>()->bind_frame_stats(&app.window_host(id)->frame_stats()))`。
    auto set_overlay(std::shared_ptr<Widget> overlay) const -> void {
        if (main_host_ == nullptr || main_host_->window() == nullptr) {
            return;  // 无头构造：无窗口可挂叠加层
        }
        // 叠加层的数据源跟随其所在窗口：分层 HUD 由该窗口离屏缓冲 2Hz 重绘并合成。
        if (auto *po = dynamic_cast<PerfOverlay *>(overlay.get())) {
            po->bind_frame_stats(&main_host_->frame_stats());
        }
        main_host_->window()->set_overlay(std::move(overlay));
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

    /// @brief 运行**统一帧循环**：每帧 pump 事件（→ 各宿主派发）→ 渲染全部窗口 → tick，
    ///        直到所有窗口请求关闭或达到 `max_frames`（<0 表示无限）。
    ///
    /// 多窗口下的每帧顺序（specification/06-app-platform.md §2.4）：
    ///   1. `drain_posted()` —— 先执行后台回投的主线程工作（可能标脏，当帧即可刷新）；
    ///   2. `pump_all_once()` —— 抽干平台事件（共享队列后端只 pump 一次，见 `Surface::pumps_thread_queue`）；
    ///   3. `on_frame_()` 回调 + 各宿主手势 `tick()`；
    ///   4. `anim_.tick(dt)` / `sched_.tick(dt)` —— **应用级共享**，每帧只推进一次；
    ///   5. 逐宿主 `render_frame(dt)` —— 各窗口独立脏区决策与上屏；
    ///   6. `reap_closed()` —— 帧末统一回收已关闭窗口（不在迭代中销毁宿主）；
    ///   7. `pump_deferred_work()` —— 帧尾兜底推进「无后台线程」的工作：排空 deferred 线程池队列、
    ///      扫描 `with_timeout` 到期看守。**必须挂在这里而非上屏路径**：空闲帧被脏区决策整段跳过
    ///      就没有 `present()`，挂上去等于饿死（同 `wasm_aria` 自驱拍的教训）；
    ///   8. `wait_once()` —— 取各宿主等待时长与最近看守期限的**最小值**后只等待一次。
    ///
    /// 事件驱动帧节流：命令行末经 `compute_wait_timeout` 决策下次唤醒——有脏区/动画时按帧预算
    /// （`WindowOptions::max_fps`）节流；完全空闲时阻塞等待事件或最近定时任务到期（静态界面 CPU
    /// 趋近 0）；`power_saving=false` 退回旧忙轮询。同时安装主线程投递器：`au::async` 的 then
    /// 回调经队列回投主线程，并 `request_wake` 唤醒睡眠中的帧循环（无运行循环时行为不变）。
    ///
    /// **WASM（浏览器）构建**：主线程不可阻塞，`run()` 仅注册 rAF 回调即返回——步骤 1–8 由
    /// `raf_tick` 每个 vsync 帧执行一次（帧节拍由浏览器承担，不调 `wait_once`），退出条件
    /// （`should_exit` / `max_frames` 预算）满足时执行收尾原语并停止循环；rAF 不可用时
    /// （非浏览器宿主）回退同步循环并 ERROR 日志申报。`Application` 实体须活过所有 rAF 回调
    /// （`au::App().run()` 路径已按页面生命周期堆持；手工栈对象须自行保证，见 `raf_owner_` 守卫）。
    AURORA_MAIN_THREAD auto run() -> void {
        if (!has_renderable_window()) {
            AURORA_LOG_WARN(
                "app",
                "run() has no available Window backend; use Application(Scene, unique_ptr<Window>) (Window provided by "
                "produced by au::create_window(XxxOptions)) or Application(Scene, unique_ptr<Surface>).");
            return;
        }
        loop_begin();
#ifdef AURORA_PLATFORM_WASM
        // 宿主有 requestAnimationFrame 才移交帧环（裸 Node 等无 DOM 宿主没有——loop 变体在本
        // Emscripten 版本返回 void 且无失败码，可用性只能前置探测，不能事后判）。
        // EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
        // clang-format off
        if (MAIN_THREAD_EM_ASM_INT(({ return typeof requestAnimationFrame === "function" ? 1 : 0; })) != 0) {
            emscripten_request_animation_frame_loop(&Application::raf_tick, this);
            return;  // 帧循环移交浏览器事件环；收尾在末帧 raf_tick 内完成
        }
        // clang-format on
        AURORA_LOG_ERROR("app", "run(): requestAnimationFrame unavailable; falling back to blocking loop");
#endif
        while (!should_exit()) {
            const auto frame_start = step_frame();
            if (!frame_budget_left()) {
                break;
            }
            wait_once(frame_start);
        }
        loop_end();
    }

    /// @brief 渲染当前场景到 PNG。运行期同样套用严格模式（specification/01-core.md §4.3 / CI 门禁），
    ///        使无窗口后端（HeadlessSurface）的离屏渲染也能触发严格失败。
    [[nodiscard]] auto render_to_png(const char *path) const -> Result<bool> {
        const StrictMode prev_strict = aurora::strict_mode();
        aurora::set_strict_mode(strict_);
        Result<bool> r = scene().render_to_png(path, width_, height_);
        aurora::set_strict_mode(prev_strict);
        return r;
    }

    /// @brief 在坐标 (x,y) 处命中测试并派发指针按下事件（同步回调，ARCHITECTURE.md §3.1）。
    /// 经 `EventDispatcher` 路径触发 `on_pointer_event`（Button::on_click 与 Clickable 回调）。
    auto dispatch_click(float x, float y) const -> void;

    /// @brief 派发任意指针动作（Press/Release/Move），用于拖拽与长按手势。
    /// 例：拖拽 = 依次 `dispatchPointer(x,y,Press)` → 多次 `Move` → `Release`。
    auto dispatch_pointer(float x, float y, MouseAction action) const -> void;

    /// @brief 驱动手势计时（长按阈值检测）；应在每帧或每次派发后调用一次。
    auto tick() const -> void;

    /// @brief 同步派发键盘事件（Tab/Shift+Tab 触发焦点移动，否则派发到焦点 widget）。
    auto dispatch_key(const KeyEvent &e) const -> bool;

    /// @brief 同步派发文本输入事件到焦点 widget。
    auto dispatch_text(TextInputEvent e) const -> bool;

    /// @brief 同步派发多点触控事件（按 pointer id 做指针捕获与并发路由，ARCHITECTURE.md §3.1）。
    /// 派发器对每个触点：① 把完整 `TouchEvent` 交给命中链（`touch()` 原始流 / `PinchRecognizer`）；
    /// ② 合成对应 `MouseEvent`（携带 `pointer_id`）驱动 `Draggable`/`LongPress`/`Clickable`。
    auto dispatch_touch(const TouchEvent &e) const -> void;

    /// @brief 同步派发操作系统文件拖放事件到命中控件（窗口逻辑坐标）。
    /// 经 `EventDispatcher` 路径触发 `on_file_drop`（specification/06-app-platform.md §8 平台 Shell）。
    auto dispatch_file_drop(const std::vector<std::string> &paths, float x, float y) const -> void {
        main_host_->dispatch_file_drop(paths, x, y);
    }

  private:
    // ---- 宿主登记 ----

    /// @brief 以 `Window` 实值为准回填画布尺寸/标题（`Window` 为空则保持传入值）。
    ///
    /// 与既有语义一致：调用方给的 `opts` 只是期望值，真实后端会按 DPI/窗口装饰调整实际尺寸，
    /// 故以 `Window::size()` 为准（无头宿主无 Window，保留调用方给定值）。
    auto adopt_window_metrics(const std::unique_ptr<Window> &window) -> void {
        if (!window) {
            return;
        }
        width_ = static_cast<int>(window->size().width);
        height_ = static_cast<int>(window->size().height);
        opts_.size = window->size();
        opts_.title = window->title();
    }

    /// @brief 登记窗口宿主：创建 `WindowHost`、接线后端通道、安装应用级快捷键前置、指定主窗口。
    /// 所有构造与 `open_window` 共用，避免重复接线。
    [[nodiscard]] auto register_host(Scene scene, std::unique_ptr<Window> window, const WindowOptions &opts)
        -> WindowId;

    // ---- 帧循环步骤 ----

    /// @brief 是否至少存在一个「有 Window 且未请求关闭」的宿主（`run()` 的启动判据）。
    [[nodiscard]] auto has_renderable_window() const -> bool;

    /// @brief 退出判据：所有有 Window 的宿主均已请求关闭（单窗口 ≡ 历史 `should_close` 语义）。
    [[nodiscard]] auto should_exit() const -> bool;

    /// @brief pump 全部窗口的原生事件；**共享队列后端只 pump 一次**（见 `Surface::pumps_thread_queue`）。
    auto pump_all_once() const -> void;

    /// @brief 逐宿主推进手势计时（长按等阈值检测）。
    auto tick_all() const -> void;

    /// @brief 逐宿主渲染一帧（`Window::present_root` + 本窗口帧统计）。
    auto render_all(double dt) const -> void;

    /// @brief 帧末回收已关闭窗口：**保留最后一个宿主**，使 `scene()`/`window()` 访问器始终有效。
    ///
    /// 回收必须在帧末而非事件派发栈内：宿主析构会连带销毁 UI 树，而此时回调栈上可能仍持有控件引用。
    auto reap_closed() -> void;

    /// @brief 帧末统一阻塞等待一次（取各宿主等待时长的最小值）。
    auto wait_once(const std::chrono::steady_clock::time_point &frame_start) -> void;

    /// @brief 首个持有 Window 的宿主（无则 nullptr）。
    [[nodiscard]] auto first_window_host() const -> WindowHost *;

    /// @brief 持有 Window 的宿主数。
    [[nodiscard]] auto count_window_hosts() const -> std::size_t;

    /// @brief 跨线程唤醒：对每个持有 Window 的宿主请求唤醒（无论它是否是当前等待方）。
    auto request_wake_all() const -> void;

    /// @brief 对单个宿主尝试几何恢复（无存储/无持久化键/无窗口/几何不可用时为 no-op）。
    auto restore_geometry_for(WindowHost &host, const std::string &persist_id) const -> void;

    // ---- 窗口级状态聚合 ----

    /// @brief 窗口可见性状态变化时聚合为响应式 State 并触发回调。
    /// 仅在实际状态发生转移（与已知态不同）时更新，避免重复事件刷屏；宿主已同步其 `Window` 快照。
    auto on_window_state_changed(WindowState s) -> void {
        if (s == window_state_.get()) {
            return;
        }
        window_state_.set(s);
        if (on_window_state_) {
            on_window_state_(s);
        }
    }

    /// @brief 窗口几何态变化时聚合为响应式 State 并触发回调（语义同 `on_window_state_changed`）。
    auto on_window_mode_changed(WindowMode m) -> void {
        if (m == window_mode_.get()) {
            return;
        }
        window_mode_.set(m);
        if (on_window_mode_) {
            on_window_mode_(m);
        }
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

    // ---- 帧循环驱动原语（同步 while 与 WASM rAF 回调共用；`run()` 文档注释为帧序契约）----

    /// @brief 循环起手：严格模式、Scheduler 当前指针、主线程回投器、时间基准与帧计数复位。
    auto loop_begin() -> void {
        prev_strict_ = aurora::strict_mode();
        aurora::set_strict_mode(strict_);
        loop_last_ = std::chrono::steady_clock::now();
        loop_frames_ = 0;
        Scheduler::set_current(&sched_);
        // 跨线程回投：后台线程的 then 回调入队 + 唤醒睡眠中的主循环，
        // 下一帧开头在主线程排水执行（兼具线程安全与不丢唤醒）。多窗口下须唤醒**全部**窗口的
        // 等待通道——任一窗口睡在自己的 Surface 上都可能延迟回投的执行。
        Task<bool>::set_main_poster([this](std::function<void()> fn) -> void {
            {
                std::scoped_lock lk(posted_mutex_);
                posted_.push_back(std::move(fn));
            }
            request_wake_all();
        });
#ifdef AURORA_PLATFORM_WASM
        raf_owner_ = this;  // rAF 蹦床守卫：只有持有循环的应用实例可被推进
#endif
    }

    /// @brief 推进恰好一帧（步骤 1–8），返回「帧起始时刻」供 `wait_once` 做帧预算核算。
    auto step_frame() -> std::chrono::steady_clock::time_point {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - loop_last_).count();
        loop_last_ = now;
        drain_posted();
        pump_all_once();
        if (on_frame_) {
            on_frame_();
        }
        tick_all();
        anim_.tick(dt);
        sched_.tick(dt);  // 定时任务随帧推进（在 present 前触发，当帧 UI 即可刷新）
        render_all(dt);
        reap_closed();  // 帧末收割：不在事件派发栈内销毁宿主，避免回调打到半死对象
        pump_deferred_work();
        ++loop_frames_;
        return now;
    }

    /// @brief 步骤 7：帧尾推进「无后台线程」的 deferred 工作——线程池排空 + 超时看守扫描。
    ///
    /// 两步同处一个安全点，因为二者是同一约束的两半：deferred 池（无 pthreads 构建，或宿主显式
    /// `force_deferred`）下任务只在被泵时才跑，`with_timeout` 也因此不能是睡在任务里的看守。
    /// 排空在前、扫描在后：本帧来得及跑完的任务先出结果，看守只对确实没跑完的改道。
    /// 非 deferred 构建下 `pump()` 立即返回 0、看守表恒空，本步等价 no-op（不为浏览器单开分支）。
    auto pump_deferred_work() -> void {
        static_cast<void>(ThreadPool::default_pool().pump());  // 预算 = 进入时已入队任务，续命任务留下帧
        static_cast<void>(detail::sweep_due_timeouts(std::chrono::steady_clock::now()));
    }

    /// @brief `max_frames` 帧预算是否仍有剩余（`<= 0` = 不限帧）。
    [[nodiscard]] auto frame_budget_left() const -> bool {
        return opts_.max_frames <= 0 || loop_frames_ < opts_.max_frames;
    }

    /// @brief 循环收尾：卸载回投器并排尽残留（仍在主线程）、清当前指针、还原严格模式。
    ///        rAF 末帧同样走此处，退出后回到「无 poster 直接调用」的默认行为。
    auto loop_end() -> void {
#ifdef AURORA_PLATFORM_WASM
        if (raf_owner_ == this) {
            raf_owner_ = nullptr;
        }
#endif
        Task<bool>::set_main_poster(nullptr);
        drain_posted();
        Scheduler::set_current(nullptr);
        Animator::set_current(nullptr);
        aurora::set_strict_mode(prev_strict_);
    }

#ifdef AURORA_PLATFORM_WASM
    /// @brief rAF 蹦床（`emscripten_request_animation_frame_loop` 回调）：每个 vsync 推进
    ///        一帧；返回 `true` = 继续（loop 变体内部自动续排下一拍），退出条件满足时走
    ///        `loop_end` 收尾并返回 `false` 终止循环。
    /// 先比对 `raf_owner_` 再解引用 `user_data`：对象析构（见 `~Application`）或循环已收尾时
    /// owner 即空，蹦床不再触碰可能已释放的实例——浏览器单线程，check-then-use 天然无竞态。
    static auto raf_tick(double /*time*/, void *user_data) -> bool {
        auto *self = static_cast<Application *>(user_data);
        if (raf_owner_ != self) {
            return false;  // 实例已析构/循环已收尾：不触碰 user_data，本拍即终止
        }
        self->step_frame();
        if (!self->should_exit() && self->frame_budget_left()) {
            return true;  // 续订下一拍（随 vsync 回调）
        }
        self->loop_end();
        return false;
    }

    /// @brief 当前持有 rAF 循环的实例（无 = nullptr）。仅 WASM 构建存在，析构守卫与蹦床共用。
    inline static Application *raf_owner_ = nullptr;
#endif

    StrictMode prev_strict_ = StrictMode::Off;  ///< 循环期保存的线程级严格模式（收尾还原）。
    std::chrono::steady_clock::time_point loop_last_;  ///< 上一帧起始时刻（帧 dt 基准；默认即 epoch 零点）。
    int loop_frames_ = 0;  ///< 本轮循环已推进帧数（`max_frames` 预算计数）。

    StrictMode strict_ = StrictMode::Off;  ///< 严格模式（run() 期间套用到线程级开关）
    int width_ = 0;  ///< 主窗口逻辑宽（无 Window 时取构造参数，供 render_to_png）。
    int height_ = 0;  ///< 主窗口逻辑高（同上）。
    WindowOptions opts_;  ///< 保留用于 run() 的 max_frames / max_fps / power_saving 等。
    std::vector<std::unique_ptr<WindowHost>> hosts_;  ///< 全部窗口宿主（按登记顺序）。
    WindowHost *main_host_ = nullptr;  ///< 主窗口宿主：`scene()`/`focus()`/`window()` 的作用对象。
    WindowId next_id_ = AURORA_INVALID_WINDOW_ID + 1U;  ///< 下一个待分配窗口 id（1 起；0 保留为无效 id）。
    WindowEventBus bus_;  ///< 跨窗口事件总线（类型化广播 / 点对点）。
    preferences::Preferences *geometry_store_ = nullptr;  ///< 几何持久化存储（可空 = 不持久化）。
    ExitPolicy exit_policy_ = ExitPolicy::LastWindowClosed;  ///< 退出策略（见 `set_exit_policy`）。
    bool quit_requested_ = false;  ///< `quit()` 请求：任何策略下都使 `run()` 退出。
    std::function<void(WindowId)> on_window_closed_;  ///< 窗口关闭回调（宿主回收后触发）。
    /// @brief 多窗口下「无线程级等待通道」后端（X11/Wayland/Wasm）的等待上限（毫秒）。
    ///
    /// 这些后端的 `wait_events` 只覆盖**自身**连接 fd 或事件目标，帧循环每帧只能等其中一个
    /// Surface；若按单窗口语义无限等待，其余窗口的输入会被饿到才有事件。封顶为轮询式短等，
    /// 牺牲至多该毫秒级的唤醒延迟换取多窗口响应性（`waits_thread_queue()==true` 的后端不受此限）。
    static constexpr double AURORA_MULTI_SURFACE_WAIT_CAP_MS = 8.0;
    std::function<void()> on_frame_;  ///< 每帧回调（在 present_root 前调用）。
    Animator anim_;  ///< 帧动画管理器（run() 每帧按 dt 推进）。
    Scheduler sched_;  ///< 定时任务调度器（run() 每帧按 dt 推进）。

    std::shared_ptr<AudioContext> audio_ctx_{nullptr};  ///< 应用级默认音频上下文（惰性创建，见 audio()）。
    CommandRegistry commands_;  ///< 命令注册表（快捷键/菜单/面板的统一真源）。
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
            launch(std::move(scene), std::move(custom_surface_), opts);
        } else if (custom_window_) {
            launch(std::move(scene), std::move(custom_window_), opts);
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
            launch(std::move(scene), std::move(window), opts);
        }
    }

  private:
    /**
     * @brief 统一的「构造 → 接线 → 跑帧循环」出口（三个后端分支共用）。
     *
     * 浏览器（WASM）下 `Application::run()` 注册 rAF 回调后即返回：若 `Application` 是栈对象，
     * `launch` 返回即析构，rAF 回调捕获的 `this` 随之悬空。故该路径把实例交给函数级
     * `static std::unique_ptr` 持有**至页面生命周期结束**（Emscripten 程序的常规语义：
     * main 返回后堆对象继续服务帧回调，进程即浏览器标签页，关闭即整体回收），不做释放。
     * 非浏览器路径保持栈对象语义，零变化。
     */
    template <typename... Args>
    auto launch(Args &&...args) -> void {
#ifdef AURORA_PLATFORM_WASM
        static std::unique_ptr<Application> keep_alive;
        // 重复 run() 时旧实例析构：~Application 清空 raf_owner_ 守卫 → 旧 rAF 回调下一拍自停，无悬空。
        keep_alive = std::make_unique<Application>(std::forward<Args>(args)...);
        Application &app = *keep_alive;
#else
        Application app{std::forward<Args>(args)...};
#endif
        if (on_frame_) {
            app.set_on_frame(on_frame_);
        }
        app.set_strict_mode(strict_);
        if (overlay_) {
            app.set_overlay(std::move(overlay_));
        }
        app.run();
    }

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
