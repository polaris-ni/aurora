#include "aurora/app/application.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "aurora/app/window_geometry.h"
#include "aurora/core/log.h"
#include "aurora/media/audio.h"

namespace aurora {

namespace {

/// @brief 抓取宿主当前几何（屏幕物理像素 + 几何态 + 所在显示器）。
///
/// 尺寸换算：`Window::size()` 是**逻辑 dp**，持久化须按当前 `scale_factor` 折算为物理像素
/// （与 `app::Display` 坐标系一致），否则换显示器/改缩放后位置会漂移。
auto capture_host_geometry(const WindowHost &host) -> WindowGeometry {
    WindowGeometry g;
    if (!host.has_window()) {
        return g;
    }
    const Window &w = *host.window();
    const float scale = w.surface().scale_factor() > 0.0F ? w.surface().scale_factor() : 1.0F;
    g.origin = w.surface().position();
    g.size = Size{.width = w.size().width * scale, .height = w.size().height * scale};
    g.mode = w.window_mode();
    g.display_id = w.surface().display_id();
    return g;
}

/// @brief 把几何快照应用到宿主（物理像素 → 当前 DPI 的逻辑 dp 后下发）。
auto apply_host_geometry(WindowHost &host, const WindowGeometry &g) -> void {
    if (!host.has_window()) {
        return;
    }
    Surface &sf = host.window()->surface();
    const float scale = sf.scale_factor() > 0.0F ? sf.scale_factor() : 1.0F;
    sf.set_size(Size{.width = g.size.width / scale, .height = g.size.height / scale});
    sf.set_position(g.origin);
}

}  // namespace

// =============================================================================
// 宿主登记与查询（specification/06-app-platform.md §2.4）
// =============================================================================

auto Application::register_host(Scene scene, std::unique_ptr<Window> window, const WindowOptions &opts) -> WindowId {
    const WindowId id = next_id_++;
    // 帧统计归属：登记第二个窗口起，全部宿主切到自有实例（含已登记的），避免多窗数据互相污染；
    // 单窗口用法刻意保留写入全局单例的历史语义（既有 PerfOverlay / bench / 性能集成测试直读之）。
    const bool isolate_stats = !hosts_.empty();
    if (isolate_stats) {
        for (const auto &h : hosts_) {
            h->own_frame_stats();
        }
    }
    auto host = std::make_unique<WindowHost>(id, std::move(scene), std::move(window), opts);
    WindowHost *const raw = host.get();  // 地址在 push_back(move) 后不变，lambda 可安全持有
    if (isolate_stats) {
        raw->own_frame_stats();  // 须先于 attach_surface：接线时把自有实例绑到 Window
    }
    raw->attach_surface();
    // 模态窗口：建立到 owner 的 OS 层从属关系（恒浮于其上、随其最小化）并屏蔽 owner 输入。
    // 焦点无需额外陷阱——FocusManager 逐窗口隔离，焦点本就不跨窗；关闭时由 reap_closed 恢复。
    if (opts.modal && opts.owner != AURORA_INVALID_WINDOW_ID) {
        if (auto *owner_host = window_host(opts.owner)) {
            if (owner_host->window() != nullptr && raw->window() != nullptr) {
                raw->window()->surface().set_owner(&owner_host->window()->surface());
                owner_host->window()->surface().set_enabled(false);
            }
        }
    }
    // 键盘前置拦截：快捷键是应用级共享的，但命中与否取决于**当前宿主**是否已有焦点控件。
    raw->set_key_pre_handler([this, raw](Event &e) -> bool {
        const auto *k = dynamic_cast<KeyEvent *>(&e);
        return k != nullptr && shortcuts_.handle(*k, raw->focus().focused() != nullptr);
    });
    // 窗口级状态上报：沿用「主窗口状态 ≡ 应用窗口状态」的历史语义；非主窗口状态只落在自己的宿主上。
    raw->set_state_sink([this, raw](WindowState s) -> void {
        if (raw == main_host_) {
            on_window_state_changed(s);
        }
    });
    raw->set_mode_sink([this, raw](WindowMode m) -> void {
        if (raw == main_host_) {
            on_window_mode_changed(m);
        }
    });
    if (main_host_ == nullptr) {
        main_host_ = raw;  // 首个登记的宿主即主窗口
    }
    hosts_.push_back(std::move(host));
    // 几何恢复：仅当指定了存储且该窗口声明了持久化键。非法/缺失几何由 load 内部兜底为 nullopt
    // （此时保持窗口的默认位置与尺寸，不回写，等关闭时以真实几何覆盖）。
    restore_geometry_for(*raw, opts.persist_id);
    return id;
}

auto Application::set_window_geometry_store(preferences::Preferences *prefs) -> void {
    geometry_store_ = prefs;
    if (geometry_store_ == nullptr) {
        return;  // 置空 = 停止持久化（已登记窗口保持当前几何）
    }
    // 构造期尚无存储：主窗口（及早期 open_window 出的窗口）只能在此刻补做恢复。
    for (auto &h : hosts_) {
        restore_geometry_for(*h, h->options().persist_id);
    }
}

auto Application::restore_geometry_for(WindowHost &host, const std::string &persist_id) const -> void {
    if (geometry_store_ == nullptr || persist_id.empty() || !host.has_window()) {
        return;
    }
    if (const auto g = load_window_geometry(*geometry_store_, persist_id)) {
        apply_host_geometry(host, *g);
    }
}

auto Application::window_host(WindowId id) const -> WindowHost * {
    for (const auto &h : hosts_) {
        if (h->id() == id) {
            return h.get();
        }
    }
    return nullptr;
}

auto Application::windows() const -> std::vector<WindowHost *> {
    std::vector<WindowHost *> out;
    out.reserve(hosts_.size());
    for (const auto &h : hosts_) {
        out.push_back(h.get());
    }
    return out;
}

auto Application::close_window(WindowId id) const -> void {
    if (auto *h = window_host(id)) {
        h->request_close();  // 下一次帧末由 reap_closed 回收
    }
}

auto Application::set_main_window(WindowId id) -> void {
    if (auto *h = window_host(id)) {
        main_host_ = h;  // 未知 id 为 no-op，保持当前主窗口
    }
}

// =============================================================================
// 应用级默认音频上下文（惰性创建；媒体/audio.h §9.4）
// =============================================================================

auto Application::audio() -> AudioContext & {
    if (!audio_ctx_) {
        // 内置后端未编译 / 设备启动失败时 AudioContext 构造内自动静默降级，无需分支。
        audio_ctx_ = std::make_shared<AudioContext>();
    }
    return *audio_ctx_;
}

auto Application::audio_shared() -> const std::shared_ptr<AudioContext> & {
    static_cast<void>(audio());  // 触发惰性创建
    return audio_ctx_;
}

// =============================================================================
// 帧循环步骤
// =============================================================================

auto Application::has_renderable_window() const -> bool {
    return std::ranges::any_of(hosts_, [](const auto &h) { return h->has_window() && !h->should_close(); });
}

auto Application::should_exit() const -> bool {
    if (quit_requested_) {
        return true;  // quit() 在任何策略下都生效
    }
    if (exit_policy_ == ExitPolicy::ExplicitOnly) {
        return false;  // 只有 quit() 能退出：窗口全关也继续跑（常驻型应用）
    }
    // 主窗口关闭即退出：主窗口必须是「有 OS 窗口」的宿主——无头宿主不产生关闭事件，
    // 若死等它关闭会永远不退出，故退化为「最后一个窗口关闭」判定。
    if (exit_policy_ == ExitPolicy::MainWindowClosed && main_host_ != nullptr && main_host_->has_window()) {
        return main_host_->should_close();
    }
    bool any_window = false;
    for (const auto &h : hosts_) {
        if (h->has_window()) {
            any_window = true;
            if (!h->should_close()) {
                return false;
            }
        }
    }
    return any_window;  // 全为无头宿主（纯无头 app）不驱动循环退出
}

auto Application::pump_all_once() const -> void {
    bool shared_pumped = false;
    // 索引遍历：事件回调内可能 re-entrant 地 open/close 窗口（改动 hosts_ 大小并触发 vector 重分配），
    // 每轮重新取 size() 并按下标取宿主，避免范围 for 的迭代器在 push_back 后失效。
    for (std::size_t i = 0; i < hosts_.size(); ++i) {  // NOLINT(*-loop-convert)
        WindowHost *const h = hosts_[i].get();
        if (!h->has_window()) {
            continue;  // 无头宿主无后端事件可抽
        }
        if (h->window()->surface().pumps_thread_queue()) {
            if (shared_pumped) {
                continue;  // 同一个线程/进程消息泵已被抽干，其余宿主为空转
            }
            shared_pumped = true;
        }
        h->pump_events();
    }
}

auto Application::tick_all() const -> void {
    // 索引遍历：tick 中动画完成等回调可能 re-entrant 地 open/close 窗口，按下标取宿主避免迭代器失效。
    for (std::size_t i = 0; i < hosts_.size(); ++i) {  // NOLINT(*-loop-convert)
        hosts_[i]->tick();
    }
}

auto Application::render_all(double dt) const -> void {
    // 索引遍历：render_frame 可能触发 re-entrant 的 open/close 窗口，按下标取宿主避免迭代器失效。
    for (std::size_t i = 0; i < hosts_.size(); ++i) {  // NOLINT(*-loop-convert)
        (void)hosts_[i]->render_frame(dt);
    }
}

auto Application::reap_closed() -> void {
    // ① 策略连带：`MainWindowClosed` 下主窗关闭即关闭全部窗口（真正回收在下一帧，
    //    本帧仅置位，避免在同一轮里一边遍历一边销毁）。
    if (exit_policy_ == ExitPolicy::MainWindowClosed && main_host_ != nullptr && main_host_->should_close()) {
        for (const auto &h : hosts_) {
            h->request_close();
        }
    }

    // ② 收集本轮待回收的宿主（先只收集 id，容器推迟到 ④ 才修改，避免迭代器/悬垂）。
    //    同时做几何持久化：必须在此刻（Surface 仍有效、宿主未被销毁），且对**所有**已请求关闭的
    //    宿主都做——包括因「保留最后一个」规则而不会真正回收的那个（单窗口关闭即属此列），
    //    否则最典型的「关掉唯一窗口后下次启动恢复几何」永远不生效。
    std::vector<WindowId> closing;
    for (auto &h : hosts_) {
        if (!h->should_close()) {
            continue;
        }
        closing.push_back(h->id());
        if (geometry_store_ != nullptr && !h->options().persist_id.empty() && h->has_window() && !h->geometry_saved()) {
            save_window_geometry(*geometry_store_, h->options().persist_id, capture_host_geometry(*h));
            h->mark_geometry_saved();
        }
    }
    if (closing.empty()) {
        return;
    }
    // 全关时**保留主窗口宿主**：`scene()`/`focus()`/`window()` 等访问器在 run() 结束后仍要可用。
    if (closing.size() == hosts_.size()) {
        const WindowId keep = main_host_ != nullptr ? main_host_->id() : closing.back();
        closing.erase(std::ranges::find(closing, keep));
    }

    // ④ 逐个回收：解绑后端回调 → 销毁宿主 → 通知上层 → 连带关闭其从属窗口。
    for (const WindowId id : closing) {
        const auto it = std::ranges::find_if(hosts_, [id](const auto &h) { return h->id() == id; });
        if (it == hosts_.end()) {
            continue;
        }
        AURORA_LOG_INFO("app", "window host closed and reaped: id=" + std::to_string(id));
        const WindowOptions &o = (*it)->options();
        // 模态窗口关闭 → 恢复被它屏蔽的 owner 输入。必须在宿主销毁**之前**做：
        // 销毁后 owner 将永远停在禁用态。（几何持久化已在 ② 阶段完成。）
        if (o.modal && o.owner != AURORA_INVALID_WINDOW_ID) {
            if (auto *owner_host = window_host(o.owner)) {
                if (owner_host != it->get() && owner_host->window() != nullptr) {
                    owner_host->window()->surface().set_enabled(true);
                }
            }
        }
        if (it->get() == main_host_) {
            main_host_ = nullptr;
        }
        (*it)->teardown();  // 先解绑后端回调（lambda 捕获 this），再释放宿主对象
        hosts_.erase(it);
        if (on_window_closed_ != nullptr) {
            on_window_closed_(id);
        }
        // 从属窗口连带关闭（Transient 依附于 owner）：置位后由**下一次** reap 回收，
        // 保持「每轮只销毁 ② 收集到的宿主」的不变式，杜绝迭代中销毁。
        for (const auto &h : hosts_) {
            if (h->options().owner == id) {
                h->request_close();
            }
        }
    }
    if (main_host_ == nullptr && !hosts_.empty()) {
        main_host_ = hosts_.front().get();
    }
}

auto Application::wait_once(const std::chrono::steady_clock::time_point &frame_start) -> void {
    if (!opts_.power_saving) {
        return;  // 关闭省电：退回旧忙轮询行为
    }
    {
        std::scoped_lock lk(posted_mutex_);
        if (!posted_.empty()) {
            return;  // 已有待排水的回投工作：不睡，立即进入下一帧
        }
    }
    const double budget_ms = opts_.max_fps > 0 ? 1000.0 / opts_.max_fps : 0.0;
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count();
    // 跨宿主聚合：任一宿主要求「不等待」→ 立即返回；否则取最小正值；全部为「无限」才无限等待。
    // 不能直接对所有返回值取 min：`<0`（无限）在数值上最小却语义最宽松，会被误选。
    double wait_ms = -1.0;
    for (const auto &h : hosts_) {
        const double w = h->decide_wait(budget_ms, anim_.has_active(), sched_.next_deadline_ms(), elapsed_ms);
        if (w == 0.0) {
            wait_ms = 0.0;
            break;
        }
        if (w < 0.0) {
            continue;  // 该宿主允许无限等待，不参与最小值竞争
        }
        if (wait_ms < 0.0 || w < wait_ms) {
            wait_ms = w;
        }
    }
    // 超时看守并入唤醒决策：deferred 构建下 `with_timeout` 是登记式到期项（无后台线程可睡），
    // 到点只能由帧尾扫描触发——若不参与最小值，空闲深睡会睡过期限，超时形同虚设。
    const double guard_ms = detail::next_timeout_deadline_ms(std::chrono::steady_clock::now());
    if (guard_ms >= 0.0) {
        wait_ms = wait_ms < 0.0 ? guard_ms : std::min(wait_ms, guard_ms);
    }
    if (wait_ms == 0.0) {
        return;
    }
    // 执行等待的宿主：优先「线程/进程级等待通道」的后端（Win32/GLFW 一次覆盖全部窗口）；
    // 无此类后端时只能等某一个 Surface，多窗口下须压上限防止其余窗口饥饿。
    WindowHost *waiter = first_window_host();
    for (const auto &h : hosts_) {
        if (h->has_window() && h->window()->surface().waits_thread_queue()) {
            waiter = h.get();
            break;
        }
    }
    if (waiter == nullptr) {
        return;  // 全为无头宿主：无等待通道
    }
    if (!waiter->window()->surface().waits_thread_queue() && count_window_hosts() > 1) {
        wait_ms =
            wait_ms < 0.0 ? AURORA_MULTI_SURFACE_WAIT_CAP_MS : std::min(wait_ms, AURORA_MULTI_SURFACE_WAIT_CAP_MS);
    }
    const auto t0 = std::chrono::steady_clock::now();
    waiter->window()->surface().wait_events(wait_ms);
    const auto t1 = std::chrono::steady_clock::now();
    waiter->frame_stats().record_wait(std::chrono::duration<double, std::milli>(t1 - t0).count());
}

auto Application::first_window_host() const -> WindowHost * {
    for (const auto &h : hosts_) {
        if (h->has_window()) {
            return h.get();
        }
    }
    return nullptr;
}

auto Application::count_window_hosts() const -> std::size_t {
    std::size_t n = 0;
    for (const auto &h : hosts_) {
        if (h->has_window()) {
            ++n;
        }
    }
    return n;
}

auto Application::request_wake_all() const -> void {
    // 索引遍历：回调内可能 re-entrant 地修改 hosts_，按下标取宿主避免迭代器失效。
    for (std::size_t i = 0; i < hosts_.size(); ++i) {  // NOLINT(*-loop-convert)
        if (hosts_[i]->has_window()) {
            hosts_[i]->window()->surface().request_wake();
        }
    }
}

// =============================================================================
// 程序化派发（作用于主窗口；多窗口请用 `window_host(id)->dispatch_*`）
// =============================================================================

auto Application::dispatch_click(float x, float y) const -> void {
    if (main_host_ != nullptr) {
        main_host_->dispatch_pointer(x, y, MouseAction::Press);
    }
}

auto Application::dispatch_pointer(float x, float y, MouseAction action) const -> void {
    if (main_host_ != nullptr) {
        main_host_->dispatch_pointer(x, y, action);
    }
}

auto Application::tick() const -> void {
    if (main_host_ != nullptr) {
        main_host_->tick();
    }
}

auto Application::dispatch_key(const KeyEvent &e) const -> bool {
    if (main_host_ == nullptr) {
        return false;
    }
    // 快捷键优先：命中即消费，不再向焦点控件派发（与后端事件路径语义一致）。
    if (shortcuts_.handle(e, main_host_->focus().focused() != nullptr)) {
        return true;
    }
    return main_host_->dispatch_key(e);
}

auto Application::dispatch_text(TextInputEvent e) const -> bool {
    return main_host_ != nullptr && main_host_->dispatch_text(std::move(e));
}

auto Application::dispatch_touch(const TouchEvent &e) const -> void {
    if (main_host_ != nullptr) {
        main_host_->dispatch_touch(e);
    }
}

}  // namespace aurora
