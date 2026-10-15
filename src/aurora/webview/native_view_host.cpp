// webview/native_view_host.cpp — 宿主侧原生子视图书架。
//
// 本 TU 是「控件 dp 矩形 → 宿主物理像素」的**唯一**换算落点与「是否变化才下发」的
// 唯一去重落点。平台实现（engine_*.cpp）不参与这两件事，它们只接收下发结果。

#include "aurora/webview/native_view_host.h"

#include <cmath>

// WebView 完整类型：几何同步要经 owner->window_bounds() 取窗口盒，
// 头文件里它只是前向声明（避免宿主头拉入整个控件层）。
#include "aurora/webview/web_view.h"

namespace aurora::webview {

namespace {

[[nodiscard]] auto host_slot() noexcept -> NativeViewHost *& {
    // 与 event/focus.cpp 的 focus_manager_slot 同形态：函数内 thread_local 惰性构造，
    // 与跨 TU 静态初始化顺序无关。
    thread_local NativeViewHost *slot = nullptr;
    return slot;
}

[[nodiscard]] auto engine_slot() -> std::shared_ptr<NativeViewEngine> & {
    static std::shared_ptr<NativeViewEngine> engine;  // NOLINT 单例读写口
    return engine;
}

// 「调用方是否已对引擎表态」：install_engine()（含置空）一经调用即为真。懒安装只在
// **从未表态**时进行——于是生产代码不装引擎也能自动挑到可用实现；而显式
// install_engine(nullptr) 成为可持久的「硬禁用」，测试亦能确定性地钉住无引擎降级，
// 不再受「本机是否装了 WebView2 运行时」影响。
[[nodiscard]] auto engine_explicit() -> bool & {
    static bool explicit_set = false;  // NOLINT 单例读写口
    return explicit_set;
}

/// 取整：四舍五入而非截断（见头注释的 1.5 倍缩放偏移论证）。
[[nodiscard]] auto to_px(float dips, float scale) -> int {
    return static_cast<int>(std::lround(static_cast<double>(dips) * static_cast<double>(scale)));
}

[[nodiscard]] auto clamp_px(int v) -> int { return v > 0 ? v : 0; }

}  // namespace

auto to_native_rect(const Rect &window_dips, float scale_factor, const EdgeInsets &content_inset) -> NativeViewRect {
    // scale <= 0 视为 1.0：后端未上报比例时按未缩放处理，比把矩形算成零盒安全
    // （零盒会让原生视图永久不可见，而这是「不知道」不是「没有」）。
    const float scale = scale_factor > 0.0F ? scale_factor : 1.0F;
    const float origin_x = window_dips.origin.x + content_inset.left;
    const float origin_y = window_dips.origin.y + content_inset.top;
    return NativeViewRect{
        .x = to_px(origin_x, scale),
        .y = to_px(origin_y, scale),
        .width = clamp_px(to_px(window_dips.size.width, scale)),
        .height = clamp_px(to_px(window_dips.size.height, scale)),
    };
}

// 平台引擎工厂的前向声明：仅当对应宏开启时存在（引擎实现源文件内定义）。
// 本 TU 不引用任何平台符号，故关闭宏的构建（WASM / 未开开关）完全不触碰 WebView2/GTK 头。
#ifdef AURORA_ENABLE_WEBVIEW_WEBVIEW2
// 已在 `namespace aurora::webview` 内，勿重复开 namespace（否则会嵌套成
// `aurora::webview::aurora::webview`，导致声明与定义不在同一命名空间）。
auto create_webview2_engine() -> std::shared_ptr<NativeViewEngine>;
#endif
#ifdef AURORA_ENABLE_WEBVIEW_WEBKITGTK
auto create_webkitgtk_engine() -> std::shared_ptr<NativeViewEngine>;
#endif

// 懒安装：进程内首个能力查询时挑一个编译进来的平台实现装上，否则回落 Null 桩——
// **仅当调用方从未对引擎表态**（`engine_explicit()` 为假）时如此。与 audio 的
// `create_default_device_backend()` 同口径（能力运行期查询、缺则静默降级），且不需要
// 任何外部显式调用：`WebView::on_mount` 先走 `current_capabilities()`，故到达
// `installed_engine()` 时引擎必已就位。一旦调用方显式 `install_engine(...)`（含置空）
// 即视为已表态，懒安装让位——显式置空于是成为可持久的「硬禁用」。装上的引擎自行在
// `probe()` 里报告 `MissingRuntime` / `Available`，不可用（宏关 / 运行期缺运行时）即
// Null 桩的 `Disabled`，两种降级在诊断面上可区分（见 `WebView` 的 `on_mount` 归因）。
auto ensure_engine_installed() -> void {
    if (engine_explicit()) {
        return;  // 调用方已显式表态（装了什么或明确置空），不再懒安装
    }
    if (engine_slot() != nullptr) {
        return;
    }
#ifdef AURORA_ENABLE_WEBVIEW_WEBVIEW2
    if (std::shared_ptr<NativeViewEngine> engine = create_webview2_engine(); engine != nullptr) {
        install_engine(std::move(engine));
        return;
    }
#endif
#ifdef AURORA_ENABLE_WEBVIEW_WEBKITGTK
    if (std::shared_ptr<NativeViewEngine> engine = create_webkitgtk_engine(); engine != nullptr) {
        install_engine(std::move(engine));
        return;
    }
#endif
    install_null_engine();
}

auto install_engine(std::shared_ptr<NativeViewEngine> engine) -> void {
    engine_slot() = std::move(engine);
    engine_explicit() = true;  // 含置空：一旦表态即停用懒安装，尊重调用方的选择
}

auto installed_engine() -> std::shared_ptr<NativeViewEngine> { return engine_slot(); }

auto current_capabilities() -> WebViewCapabilities {
    ensure_engine_installed();
    if (const std::shared_ptr<NativeViewEngine> &engine = engine_slot(); engine != nullptr) {
        return engine->probe();
    }
    return WebViewCapabilities{};
}

// ---- NativeViewHost ----

struct NativeViewHost::Entry {
    WebView *owner = nullptr;
    std::unique_ptr<WebViewSink> view;
    std::optional<NativeViewRect> last_rect;
    bool last_visible = false;
    bool show_natively = true;  ///< 允许宿主置原生可见（快照模式的视图恒 false）
};

// 构造函数体落在本 TU（理由见头文件该函数的注释：`Entry` 完整类型只在此可见）。
// 票据在此诞生：`host = this` 只在本对象构造完成后才有意义，故不能做成类内默认成员初始化。
NativeViewHost::NativeViewHost() {
    ticket_ = std::make_shared<NativeViewTicket>();
    ticket_->host = this;
}

auto NativeViewHost::ticket() const -> std::shared_ptr<NativeViewTicket> { return ticket_; }

auto NativeViewHost::current() noexcept -> NativeViewHost * { return host_slot(); }

auto NativeViewHost::set_current(NativeViewHost *host) noexcept -> void { host_slot() = host; }

NativeViewHost::~NativeViewHost() {
    // 先把票据里的宿主指针抹掉再拆条目：本函数体内 `this` 正在析构，票据对外仍可能可达
    // （控件侧若误持 `shared_ptr` 而非 `weak_ptr`），置空后任何回访都只会拿到空宿主。
    ticket_->host = nullptr;
    for (std::unique_ptr<Entry> &e : entries_) {
        if (e != nullptr && e->view != nullptr) {
            e->view->detach();
        }
    }
}

auto NativeViewHost::bind_window(void *native_window, std::function<float()> scale_factor_provider,
                                 std::function<EdgeInsets()> content_inset_provider) -> void {
    native_window_ = native_window;
    scale_factor_provider_ = std::move(scale_factor_provider);
    content_inset_provider_ = std::move(content_inset_provider);
}

auto NativeViewHost::register_view(WebView *owner, std::unique_ptr<WebViewSink> view, bool show_natively)
    -> std::optional<std::size_t> {
    if (owner == nullptr || native_window_ == nullptr) {
        return std::nullopt;
    }
    // 空槽优先复用（避免登记表随挂载/卸载单调增长）；无空槽则追加。
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i] == nullptr) {
            auto entry = std::make_unique<Entry>();
            entry->owner = owner;
            entry->view = std::move(view);
            entry->show_natively = show_natively;
            entries_[i] = std::move(entry);
            return i;
        }
    }
    auto entry = std::make_unique<Entry>();
    entry->owner = owner;
    entry->view = std::move(view);
    entry->show_natively = show_natively;
    entries_.push_back(std::move(entry));
    return entries_.size() - 1;
}

auto NativeViewHost::unregister_view(std::size_t token) -> void {
    Entry *e = entry_by_token(token);
    if (e == nullptr) {
        return;  // 重复注销幂等
    }
    if (e->view != nullptr) {
        e->view->detach();
    }
    entries_[token].reset();  // 置空而非 erase：token 作为稳定索引，不因注销而移位
}

auto NativeViewHost::view_of(std::size_t token) -> WebViewSink * {
    const Entry *e = entry_by_token(token);
    if (e == nullptr) {
        return nullptr;
    }
    return e->view.get();  // 降级登记的条目无视图，如实返回空
}

auto NativeViewHost::view_count() const -> std::size_t {
    std::size_t n = 0;
    for (const std::unique_ptr<Entry> &e : entries_) {
        if (e != nullptr) {
            ++n;
        }
    }
    return n;
}

auto NativeViewHost::sync_geometry(bool geometry_may_have_changed) -> std::size_t {
    if (!geometry_may_have_changed) {
        return 0;  // 纯绘制帧：window_bounds() 契约禁止进入每帧路径
    }
    const float scale = scale_factor_provider_ != nullptr ? scale_factor_provider_() : 1.0F;
    const EdgeInsets inset = content_inset_provider_ != nullptr ? content_inset_provider_() : EdgeInsets{};

    std::size_t dispatched = 0;
    for (std::unique_ptr<Entry> &slot : entries_) {
        if (slot == nullptr) {
            continue;
        }
        Entry &e = *slot;
        const std::optional<Rect> dips = e.owner->window_bounds();
        const bool visible = dips.has_value() && dips->size.width > 0.0F && dips->size.height > 0.0F;

        if (!visible) {
            if (e.last_visible) {
                e.last_visible = false;
                if (e.view != nullptr) {
                    e.view->set_native_visible(false);
                }
            }
            continue;  // 不可见时不重算矩形：下一次可见必然伴随一次几何同步
        }

        const NativeViewRect rect = to_native_rect(*dips, scale, inset);
        // 矩形去重与可见性恢复是**两件独立的事**：控件「隐去后再现」时矩形往往一字未改，
        // 若把可见性恢复挂在矩形下发的分支里，这条可见性就被去重吞掉——原生视图此后
        // 永久不可见，而宿主侧看不出任何异常（返回值、矩形序列都对）。故先算矩形是否变化，
        // 再各自下发。
        const bool rect_changed = !e.last_rect.has_value() || *e.last_rect != rect;
        if (rect_changed) {
            e.last_rect = rect;
            if (e.view != nullptr) {
                e.view->set_native_rect(rect);
            }
            ++dispatched;
        }
        if (!e.show_natively) {
            continue;  // 快照模式：视图只作像素来源，宿主永不授予原生可见
        }
        if (!e.last_visible) {
            e.last_visible = true;
            if (e.view != nullptr) {
                e.view->set_native_visible(true);
            }
        }
    }
    return dispatched;
}

auto NativeViewHost::poll_watches() const -> std::vector<WatchFd> {
    const std::shared_ptr<NativeViewEngine> &engine = engine_slot();
    if (engine == nullptr) {
        return {};
    }
    return engine->poll_watches();
}

auto NativeViewHost::pump_engine_events() -> void {
    const std::shared_ptr<NativeViewEngine> &engine = engine_slot();
    if (engine != nullptr) {
        engine->pump_events();
    }
}

auto NativeViewHost::last_dispatched_rect(std::size_t token) const -> std::optional<NativeViewRect> {
    const Entry *e = entry_by_token(token);
    if (e == nullptr) {
        return std::nullopt;
    }
    return e->last_rect;
}

auto NativeViewHost::entry_by_token(std::size_t token) -> Entry * {
    if (token >= entries_.size()) {
        return nullptr;
    }
    return entries_[token].get();
}

auto NativeViewHost::entry_by_token(std::size_t token) const -> const Entry * {
    if (token >= entries_.size()) {
        return nullptr;
    }
    return entries_[token].get();
}

}  // namespace aurora::webview
