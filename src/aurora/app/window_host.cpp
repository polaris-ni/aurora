#include "aurora/app/window_host.h"

#include <algorithm>

#include "aurora/app/display.h"
#include "aurora/app/scroll_storage.h"
#include "aurora/window/frame_pacing.h"
#include "aurora/window/platform.h"

namespace aurora {

// =============================================================================
// 构造与生命周期
// =============================================================================

WindowHost::WindowHost(WindowId id, Scene scene, std::unique_ptr<Window> window, const WindowOptions &opts)
    : id_(id), scroll_scope_(std::to_string(id)), role_(opts.role), opts_(opts), scene_(std::move(scene)),
      window_(std::move(window)) {
    // Tab 焦点序遍历的起点（可为 nullptr → 禁用 Tab 导航）。
    // 空场景（根节点不持 widget，如 e2e harness 建窗期的 `Scene{Node{}}`）必须落 nullptr 分支：
    // `Scene::root()` 内部对空 `shared_ptr` 解引用取 `Widget&`，加固构建（libstdc++
    // `_GLIBCXX_ASSERTIONS`）下 `__shared_ptr_deref` 断言直接 abort。与 teardown 清
    // `on_subtree_dirty` 前的 `root_node()` 判定同一口径（无断言构建下旧行为本就是静默
    // 解出空指针再交给 `set_root(nullptr)`，本改动对非加固构建逐位无行为差异）。
    focus_.set_root(scene_.root_node() ? &scene_.root() : nullptr);
}

auto WindowHost::teardown() -> void {
    if (torn_down_) {
        return;
    }
    torn_down_ = true;
    if (window_ != nullptr) {
        // 先断开无障碍桥：UIA RPC 线程可在非主线程经 provider 回调进桥（provider 持桥裸指针），
        // 而析构序列中 widget 树先亡、桥后亡（~Impl → DestroyWindow → handle_destroy 才断 a11y），
        // 此窗口期内 RPC 线程访问已析构桥即 use-after-free（0xC0000005，实测崩于 wait_on_main
        // 拷贝 alive_ 成员）。在 widget 树析构前 deactivate 即同步断连全部 provider 并注销注册表。
        if (auto *a11y = window_->surface().accessibility_provider()) {
            a11y->deactivate();
        }
        // 清空根 widget 的脏传播回调：lambda 捕获 Window* this，Window 析构后悬垂。
        // 仅当场景已挂载（根控件存在）时才有此回调可清；未挂载（如仅 open 未 mount）根节点
        // 为空，`Scene::root()` 会解引用空 widget 指针。
        if (scene_.root_node()) {
            scene_.root().on_subtree_dirty = {};
        }
        // 解绑捕获 `this` 的后端回调：同一消息泵不停，宿主析构后回调会打到已释放对象。
        auto &sf = window_->surface();
        sf.set_accessibility_root(nullptr);  // 抹掉桥侧根指针，防 RPC 线程重入拿悬垂根重建
        sf.set_event_handler({});
        sf.set_window_state_handler({});
        sf.set_window_mode_handler({});
        sf.set_scale_change_handler({});  // 同上：WM_DPICHANGED（多显示器/系统缩放变更）随时可到
        sf.set_os_color_scheme_handler({});  // 同上：WM_SETTINGCHANGE（系统深浅色切换）随时可到
        sf.set_composition_caret_provider({});  // 同样捕获 this，须解绑
        // `Window::wire_present_request_once` 注册（非 attach_surface）：捕获 `Window*`。
        // 宿主回收（teardown → 析构级联）中 Window 的其他成员先于 surface_ 析构，而 Win32 的
        // DestroyWindow 同步派发可经 WM_DPICHANGED → SetWindowPos → WM_SIZE 触发同步重渲染，
        // 回调打到半亡窗口即 use-after-free。Win32 `Impl::~Impl` 已有兜底置空（仅该后端），
        // 此处按同一清单在正常路径收敛，三后端经虚派发一并生效。
        sf.set_present_request({});
        // 原生子视图的引擎事件源并入（attach_surface 安装）：lambda 捕获 this，须解绑。
        sf.set_extra_event_source({}, {});
    }
    key_pre_ = nullptr;
    state_sink_ = nullptr;
    mode_sink_ = nullptr;
    mouse_.set_cursor_handler({});
    // 焦点控件持有的 shared_ptr 不释放前，本宿主已关但树仍未销毁，此时 Tab/键入仍可命中旧树。
    focus_.clear();
    focus_.set_root(nullptr);
}

// =============================================================================
// 接线
// =============================================================================

auto WindowHost::attach_surface() -> void {
    if (window_ == nullptr) {
        return;  // 无头宿主：无后端通道可接
    }
    auto &sf = window_->surface();
    sf.set_event_handler([this](Event &e) -> void { dispatch(e); });
    sf.set_window_state_handler([this](WindowState s) -> void { on_window_state(s); });
    sf.set_window_mode_handler([this](WindowMode m) -> void { on_window_mode(m); });
    // 悬停光标下发到**本窗口**：解析出的形状由当前宿主的 Surface 生效，不串到其他窗口。
    mouse_.set_cursor_handler([this](CursorShape shape) -> void {
        if (window_ != nullptr) {
            window_->surface().set_cursor(shape);
        }
    });
    // DPI 缩放变化：窗口被拖到不同缩放比的显示器 / 系统缩放变更 → 强制全量重排重绘。
    sf.set_scale_change_handler([this](float /*scale*/) -> void { on_scale_changed(); });
    // OS 明暗偏好变化：系统深浅色切换（Win32 WM_SETTINGCHANGE）→ 推入 Window 快照并重排重绘
    // （默认根主题每帧经 prepare_context 解析注入，应用未显式 set_theme 时下一帧即跟随）。
    sf.set_os_color_scheme_handler([this](ColorScheme s) -> void { on_os_color_scheme(s); });
    // IME 候选窗定位：后端桥据此将候选列表摆到当前焦点控件的插入点旁（逻辑 dp，桥内换算像素）。
    // 只对**可编辑文本**角色给出（Wayland text-input-v3 的 enable/disable 判据即「当前是否有
    // 文本录入焦点」，基类 composition_caret_bounds 的 focus_bounds_ 兜底会让按钮也返回非零盒）；
    // 非文本焦点返回零盒 = 「无有效定位」，Win32/X11 侧行为退化为系统默认位置，不变。
    sf.set_composition_caret_provider([this]() -> Rect {
        Widget *focused = focus_.focused();
        if (focused == nullptr || focused->accessibility_role() != AccessibilityRole::TextInput) {
            return Rect{};
        }
        return focused->composition_caret_bounds();
    });
    // per-window 帧统计：默认仍写全局单例（单窗口用法零回归），多窗口下已由
    // `own_frame_stats()` 切到自有实例后再接线。
    window_->set_frame_stats(*active_stats_);
    // 原生子视图书架：绑定宿主窗口句柄（Linux = X11 Window XID，由引擎自行解释）与
    // 两个换算参数的现取 provider（DPI / CSD 安全区帧内可能变化，不缓存）。
    native_view_host_.bind_window(
        window_->surface().native_handle(), [this]() -> float { return window_->surface().scale_factor(); },
        [this]() -> EdgeInsets { return window_->surface().content_inset(); });
    // 引擎事件源并入帧循环等待：WebView 引擎（Linux = GLib 主上下文）的待监听 fd 进
    // Surface::wait_events 的 poll 集，就绪后经 pump 非阻塞推进一轮。未装 Linux 引擎时
    // 两个通道均为空转（poll_watches 返回空、pump 无操作），不影响其余后端。
    sf.set_extra_event_source(
        [this]() -> std::vector<Surface::ExtraWatch> {
            std::vector<Surface::ExtraWatch> out;
            for (const webview::WatchFd &w : native_view_host_.poll_watches()) {
                out.push_back(Surface::ExtraWatch{.fd = w.fd, .readable = w.readable, .writable = w.writable});
            }
            return out;
        },
        [this]() -> void { native_view_host_.pump_engine_events(); });
    // 默认根主题注入的 OS 档：窗口创建时读一次系统明暗偏好并推给 Window（`Window` 自身不直读
    // 注册表/配置文件，避免公共头反向依赖平台层）。系统外观实时变更的再推送属后续生产钩子
    // （见 提案「OS 偏好读取口径」）；应用亦可 `Window::set_theme` 显式接管最高优先。
    window_->set_os_color_scheme(os_color_scheme());
    if (opts_.max_fps > 0) {
        stats_.set_frame_budget_ms(1000.0 / static_cast<double>(opts_.max_fps));
        active_stats_->set_frame_budget_ms(1000.0 / static_cast<double>(opts_.max_fps));
    }
}

auto WindowHost::on_scale_changed() -> void {
    if (window_ != nullptr) {
        // 逻辑↔物理换算已变：整帧标脏重排重绘（HUD 离屏缓冲按 scale 变化自动重建）。
        window_->force_full_redraw();
    }
}

auto WindowHost::on_os_color_scheme(ColorScheme s) -> void {
    if (window_ != nullptr) {
        // OS 档位快照推入 Window（未显式 set_theme 时参与默认根主题解析）；主题面取色全变，
        // 整帧标脏重排重绘。应用显式 set_theme 后本推送不参与解析，重绘一次无副作用。
        window_->set_os_color_scheme(s);
        window_->force_full_redraw();
    }
}

auto WindowHost::display_id() const -> int { return window_ != nullptr ? window_->surface().display_id() : -1; }

auto WindowHost::move_to_display(int display_id) -> void {
    if (window_ != nullptr) {
        app::move_window_to_display(*window_, display_id);
    }
}

auto WindowHost::raise() -> void {
    if (window_ != nullptr) {
        window_->surface().raise();
    }
}

auto WindowHost::focus_window() -> void {
    if (window_ != nullptr) {
        window_->surface().focus_window();
    }
}

auto WindowHost::own_frame_stats() -> void {
    if (active_stats_ == &stats_) {
        return;  // 已切到自有实例（幂等）
    }
    active_stats_ = &stats_;
    if (window_ != nullptr) {
        window_->set_frame_stats(stats_);
        if (opts_.max_fps > 0) {
            stats_.set_frame_budget_ms(1000.0 / static_cast<double>(opts_.max_fps));
        }
    }
}

auto WindowHost::on_window_state(WindowState s) -> void {
    if (window_ != nullptr) {
        window_->set_window_state(s);  // 供本窗口根 Environment 注入（子树 ctx.env->get<WindowState>()）
    }
    if (state_sink_ != nullptr) {
        state_sink_(s);
    }
}

auto WindowHost::on_window_mode(WindowMode m) -> void {
    if (window_ != nullptr) {
        window_->set_window_mode(m);
    }
    if (mode_sink_ != nullptr) {
        mode_sink_(m);
    }
}

// =============================================================================
// 每帧步骤
// =============================================================================

auto WindowHost::pump_events() -> void {
    if (window_ == nullptr) {
        return;
    }
    window_->pump_events();
    // 引擎事件循环的非阻塞推进：WebView 引擎（Linux = GLib 主上下文）在此消化一轮。
    // 与 AT-SPI 桥的帧首 pump 同构——阻塞等待已由 Surface::wait_events 并入引擎 fd，
    // 此处只负责「就绪后 / 自然醒后」的非阻塞分发（未装 Linux 引擎时为空操作）。
    native_view_host_.pump_engine_events();
}

auto WindowHost::tick() -> void { scene_.root().tick(std::chrono::steady_clock::now()); }

auto WindowHost::render_frame(double dt) -> Result<bool> {
    if (window_ == nullptr) {
        return Result<bool>{true};  // 无头宿主不参与上屏（render_to_png 另有路径）
    }
    // 滚动位置按窗口隔离：`BuildContext` 不携带窗口标识，故由宿主在渲染入口给出作用域，
    // 同进程多窗口下同名 `restore_key` 不互相串味（同值重复构造走零分配快路径）。
    const ScrollStorage::Scope scroll_scope(scroll_scope_);
    // 帧首安装原生子视图书架：控件挂载发生在 present_root 的树重建里，安装口必须早于
    // 挂载（同 FocusManager 的帧级契约）；帧尾恢复。
    webview::NativeViewHost::set_current(&native_view_host_);
    Result<bool> r = window_->present_root(scene_.root_node());
    // present_root 返回时本帧布局已落定（window_bounds 查询时现算，此刻取到的是本帧
    // 布局结果）。门控取「非 idle」：idle 帧无脏 ⇒ 几何不可能变化，免付 window_bounds
    // 的树上溯（该查询契约禁止进入每帧路径）。下发比帧上屏晚一拍——原生视图位置在
    // 下一帧 present 时生效，与 Win32 腿（put_Bounds 同在 render_frame 之后）同构。
    if (!window_->is_idle_frame()) {
        native_view_host_.sync_geometry(true);
    }
    webview::NativeViewHost::set_current(nullptr);
    // present_root 返回 Result<bool>：idle 跳过亦为 true（内部未做任何渲染）。
    if (window_->is_idle_frame()) {
        // 传入本帧墙钟间隔：idle 帧同样消耗了一段等待时间，「距上次实际渲染已过去多久」
        // 要靠它累加（`FrameStats::is_stale()` 的判据）。
        active_stats_->record_idle(dt);
    } else {
        active_stats_->record(dt);
    }
    return r;
}

auto WindowHost::decide_wait(double frame_budget_ms, bool anim_active, double next_deadline_ms, double elapsed_ms) const
    -> double {
    if (window_ == nullptr) {
        return 0.0;  // 无头宿主不等待（测试以有限帧驱动，引入等待会破坏确定性）
    }
    // 活跃信号：运行中动画，或本帧实际渲染了（非 idle）——后者覆盖「每帧在 on_frame 里标脏」
    // 类模式（脏已被同帧 present 消费，仅看脏会误判空闲而深睡）；真正空闲帧不受影响。
    const bool active = anim_active || !window_->is_idle_frame();
    // HUD 刷新并入「非渲染唤醒」截止时间，取二者最早。不并入的后果：整树无脏时下面的空闲
    // 分支会睡到下一个定时器（无定时器即 `<0` 无限深睡），叠加层再也醒不过来、HUD 永远停在
    // 最后一帧的读数上。 `<0` 表示「无此唤醒源」，不参与最小比较。
    double deadline_ms = next_deadline_ms;
    const double hud_due_ms = window_->hud_refresh_due_ms();
    if (hud_due_ms >= 0.0) {
        deadline_ms = deadline_ms < 0.0 ? hud_due_ms : std::min(deadline_ms, hud_due_ms);
    }
    return compute_wait_timeout(window_->has_pending_dirty(), active, deadline_ms, frame_budget_ms, elapsed_ms,
                                window_->surface().paces_frames());
}

// =============================================================================
// 关闭
// =============================================================================

auto WindowHost::should_close() const -> bool {
    // 程序化请求优先：后端未必实现 `close()`（Headless 无 OS 关闭语义，为空实现），
    // 若只信后端，程序化关闭在无头/未接线后端上会静默失效。
    return close_requested_ || (window_ != nullptr && window_->should_close());
}

auto WindowHost::request_close(CloseRequestSource source) -> void {
    close_requested_ = true;
    close_source_ = source;  // 记账：OS 路不经过本函数，见 Application 询问步的来源判定式
    if (window_ != nullptr) {
        window_->close();  // 通知真实后端（有 OS 关闭语义时生效）
    }
}

auto WindowHost::cancel_close() -> void {
    close_requested_ = false;
    close_source_ = CloseRequestSource::Programmatic;
    close_asked_ = false;
    if (window_ != nullptr) {
        window_->surface().cancel_close();  // 转发：后端旗标的清位入口（默认空实现 ⇒ fail-open）
    }
}

// =============================================================================
// 事件派发
// =============================================================================

auto WindowHost::dispatch(Event &e) -> void {
    // 派发上下文：快捷键动作与控件回调内同样可取到**本窗口**的焦点管理器（弹层开合、模态
    // 焦点陷阱依赖它）；子派发路径会自行保存/复原该槽位，嵌套安全。
    FocusManager *const prev_fm = current_focus_manager();
    set_current_focus_manager(&focus_);
    // 位置类事件以整窗下沉壳为派发根（`hit_root`）：后端上抛的表面坐标与绘制
    // 几何经同一条 PaddingEdges 修饰平移命中，inset 非零档（CSD 自绘）不再有「像素与点击
    // 差一栏」的漂移；键盘/文本/组合不经命中链，仍走场景根。
    auto &root = scene_.root();
    auto &hit = hit_root();
    if (auto *m = dynamic_cast<MouseEvent *>(&e)) {
        mouse_.dispatch_mouse(hit, *m, &focus_);
    } else if (auto *s = dynamic_cast<ScrollEvent *>(&e)) {
        EventDispatcher::dispatch(hit, *s);
    } else if (auto *k = dynamic_cast<KeyEvent *>(&e)) {
        // 键盘前置拦截：应用级快捷键共享于此，命中即消费，不再向本窗口焦点控件派发。
        if (key_pre_ != nullptr && key_pre_(e)) {
            k->is_handled = true;
            set_current_focus_manager(prev_fm);
            return;
        }
        EventDispatcher::dispatch(root, *k, focus_);
    } else if (auto *t = dynamic_cast<TextInputEvent *>(&e)) {
        EventDispatcher::dispatch(root, *t, focus_);
    } else if (auto *ce = dynamic_cast<TextCompositionEvent *>(&e)) {
        EventDispatcher::dispatch(root, *ce, focus_);
    } else if (auto *te = dynamic_cast<TouchEvent *>(&e)) {
        touch_.dispatch(hit, *te, &focus_);
    } else if (auto *sde = dynamic_cast<SystemDropEvent *>(&e)) {
        // 系统拖放同样是位置类事件：走整窗下沉壳，与绘制共用同一条平移。
        EventDispatcher::dispatch(hit, *sde);
    }
    set_current_focus_manager(prev_fm);
}

auto WindowHost::dispatch_pointer(float x, float y, MouseAction action) -> void {
    MouseEvent e;
    e.position = Point{.x = x, .y = y};
    e.action = action;
    mouse_.dispatch_mouse(hit_root(), e, &focus_);
}

auto WindowHost::dispatch_key(KeyEvent e) -> bool {
    // 调用方（Application）已在同一契约下优先过快捷键；此处只做焦点树派发。
    FocusManager *const prev_fm = current_focus_manager();
    set_current_focus_manager(&focus_);
    const bool result = EventDispatcher::dispatch(scene_.root(), e, focus_);
    set_current_focus_manager(prev_fm);
    return result;
}

auto WindowHost::dispatch_text(TextInputEvent e) -> bool { return EventDispatcher::dispatch(scene_.root(), e, focus_); }

auto WindowHost::dispatch_touch(const TouchEvent &e) -> void {
    // 派发器需要非 const 事件（命中链按 pointer id 缓存、is_handled_ 回写），此处构造可变副本。
    TouchEvent copy = e;
    touch_.dispatch(hit_root(), copy, &focus_);
}

auto WindowHost::dispatch_system_drop(SystemDropEvent &e) -> void { EventDispatcher::dispatch(hit_root(), e); }

}  // namespace aurora
