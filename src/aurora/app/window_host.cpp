#include "aurora/app/window_host.h"

#include <algorithm>

#include "aurora/app/display.h"
#include "aurora/app/scroll_storage.h"
#include "aurora/window/frame_pacing.h"

namespace aurora {

// =============================================================================
// 构造与生命周期
// =============================================================================

WindowHost::WindowHost(WindowId id, Scene scene, std::unique_ptr<Window> window, const WindowOptions &opts)
    : id_(id), scroll_scope_(std::to_string(id)), role_(opts.role), opts_(opts), scene_(std::move(scene)),
      window_(std::move(window)) {
    focus_.set_root(&scene_.root());  // Tab 焦点序遍历的起点（可为 nullptr → 禁用 Tab 导航）
}

auto WindowHost::teardown() -> void {
    if (torn_down_) {
        return;
    }
    torn_down_ = true;
    if (window_ != nullptr) {
        // 解绑捕获 `this` 的后端回调：同一消息泵不停，宿主析构后回调会打到已释放对象。
        auto &sf = window_->surface();
        sf.set_event_handler({});
        sf.set_window_state_handler({});
        sf.set_window_mode_handler({});
        sf.set_composition_caret_provider({});  // 同样捕获 this，须解绑
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
}

auto WindowHost::tick() -> void { scene_.root().tick(std::chrono::steady_clock::now()); }

auto WindowHost::render_frame(double dt) -> Result<bool> {
    if (window_ == nullptr) {
        return Result<bool>{true};  // 无头宿主不参与上屏（render_to_png 另有路径）
    }
    // 滚动位置按窗口隔离：`BuildContext` 不携带窗口标识，故由宿主在渲染入口给出作用域，
    // 同进程多窗口下同名 `restore_key` 不互相串味（同值重复构造走零分配快路径）。
    const ScrollStorage::Scope scroll_scope(scroll_scope_);
    Result<bool> r = window_->present_root(scene_.root_node());
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

auto WindowHost::request_close() -> void {
    close_requested_ = true;
    if (window_ != nullptr) {
        window_->close();  // 通知真实后端（有 OS 关闭语义时生效）
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
    auto &root = scene_.root();
    if (auto *m = dynamic_cast<MouseEvent *>(&e)) {
        mouse_.dispatch_mouse(root, *m, &focus_);
    } else if (auto *s = dynamic_cast<ScrollEvent *>(&e)) {
        EventDispatcher::dispatch(root, *s);
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
        touch_.dispatch(root, *te, &focus_);
    } else if (auto *fde = dynamic_cast<FileDropEvent *>(&e)) {
        EventDispatcher::dispatch(root, *fde);
    }
    set_current_focus_manager(prev_fm);
}

auto WindowHost::dispatch_pointer(float x, float y, MouseAction action) -> void {
    MouseEvent e;
    e.position = Point{.x = x, .y = y};
    e.action = action;
    mouse_.dispatch_mouse(scene_.root(), e, &focus_);
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
    touch_.dispatch(scene_.root(), copy, &focus_);
}

auto WindowHost::dispatch_file_drop(const std::vector<std::string> &paths, float x, float y) -> void {
    FileDropEvent e;
    e.position = Point{.x = x, .y = y};
    e.paths = paths;
    EventDispatcher::dispatch(scene_.root(), e);
}

}  // namespace aurora
