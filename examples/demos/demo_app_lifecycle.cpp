// App Lifecycle demo：窗口级生命周期 WindowState / WindowMode。
// 订阅 Application::window_state()：在 Hidden/Occluded（最小化/被遮挡）时暂停动画，
// Visible（前台激活）时恢复；并通过 Environment 读取当前窗口状态/几何态（ctx.environment<T>()）。
#include <memory>
#include <string>

#include "aurora/app/application.h"
#include "demo_common.h"
namespace {
/// @brief 从根 Environment 读取当前窗口状态/几何态的叶控件。
class LifecycleReadout : public au::LeafWidget {
  public:
    void collect_signals(std::vector<au::SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "LifecycleReadout"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "LifecycleReadout", .children_policy = "none"};
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = 380.0F, .height = 72.0F});
    }
    void on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) override {
        const auto *ws = ctx.environment<au::WindowState>();
        const auto *wm = ctx.environment<au::WindowMode>();
        const std::string s = (ws != nullptr) ? au::to_string(*ws) : "Visible";
        const std::string m = (wm != nullptr) ? au::to_string(*wm) : "Normal";
        // 两行读数快照走「写入位置不变」协议：状态名均 ≤15 字符（落在 SSO 内联缓冲、永不
        // 重分配），赋值前先清尾，故 HTTP 线程的并发读最多读到半更新的名字，不会触及堆指针。
        last_state_.assign(s);
        last_mode_.assign(m);
        p.fill_rect(b, pal::AURORA_SURFACE);
        p.draw_rect(b, pal::AURORA_BORDER);
        const float th = aurora::render::FontEngine::measure_height(au::Font{.size_pt = 16.0F});
        p.draw_text(au::Rect{.origin = au::Point{.x = b.origin.x + 12.0F, .y = b.origin.y + 14.0F},
                             .size = au::Size{.width = b.size.width - 24.0F, .height = th}},
                    "WindowState = " + s, au::Font{.size_pt = 16.0F}, pal::AURORA_TEXT);
        p.draw_text(au::Rect{.origin = au::Point{.x = b.origin.x + 12.0F, .y = b.origin.y + 40.0F},
                             .size = au::Size{.width = b.size.width - 24.0F, .height = th}},
                    "WindowMode = " + m, au::Font{.size_pt = 16.0F}, pal::AURORA_TEXT);
    }

    /// @brief 把最近一次绘出的两行读数一并序列化，使其可经 `/api/tree` 读取（无人值守取证用）。
    auto serialize_props(au::Json &props) const -> void override {
        au::LeafWidget::serialize_props(props);
        props["window_state"] = last_state_;
        props["window_mode"] = last_mode_;
    }

  private:
    std::string last_state_ = "Visible";
    std::string last_mode_ = "Normal";
};
}  // namespace

using namespace std::chrono_literals;

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    auto paused = std::make_shared<au::State<bool>>(false);
    auto ticks = std::make_shared<au::State<int>>(0);
    auto ticks_label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"ticks = 0"});

    // 动画子树：每 200ms 递增计数（仅在可见时）。被遮挡/最小化时由 window_state 回调暂停。
    au::Node anim = au::Timer(
        200ms,
        [ticks_label](const au::SignalView<int> &) -> au::Text {
            return au::Text{au::TextProps{.content = au::Reactive{ticks_label}}};
        },
        [paused, ticks, ticks_label](int) -> void {
            if (!paused->get()) {
                const int n = ticks->get() + 1;
                ticks->set(n);
                ticks_label->set(au::LocalizedString{"ticks = " + std::to_string(n)});
            }
        });

    au::Node root = au::Column{
        GradientTitle{"App Lifecycle"},
        gap(12),
        LifecycleReadout{},
        gap(8),
        std::move(anim),
        gap(8),
        au::Text{au::LocalizedString{
            "Minimize or switch to another window -> animation pauses; return to foreground -> resumes"}},
    };

    au::Scene scene{au::Node{root}};
    au::WindowOptions opts;
    opts.size = au::Size{.width = 520.0F, .height = 440.0F};
    opts.title = "App Lifecycle · Aurora Demo";
    auto win_res = au::create_native_window(opts);
#ifdef AURORA_PLATFORM_WASM
    // ⚠️ rAF 契约（`Application::run`）：浏览器下注册帧环后即返回、main 随即结束——栈实例当场
    // 析构会让帧环蹦床与进程级主线程回投器捕获的 `this` 悬空。与 `au::App().run` 的 launch 同法
    // 按页面生命周期堆持；非浏览器路径保持栈对象语义。
    static std::unique_ptr<au::Application> keep_alive;
    keep_alive =
        std::make_unique<au::Application>(std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts);
    au::Application &app = *keep_alive;
#else
    au::Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};
#endif
#ifdef AURORA_BUILD_INSPECTOR_SERVER
    // 无人值守取证通道：设 AURORA_INSPECTOR_PORT 即起 InspectorServer，读 `WindowState` /
    // `WindowMode` / `ticks` 三行读数走 `/api/tree`，与本文件自建的 root 句柄同源。
    auto inspector = start_demo_inspector(
        [&root]() -> au::Node { return au::Node{root}; },
        [&app]() -> au::Surface * { return app.window() != nullptr ? &app.window()->surface() : nullptr; });
#endif

    // 窗口级生命周期：隐藏/被遮挡时暂停动画，可见时恢复。
    app.set_on_window_state([paused](au::WindowState s) -> void { paused->set(s != au::WindowState::Visible); });
    app.set_on_window_mode(
        [](au::WindowMode m) -> void { AURORA_LOG_INFO("demo", "[AppLifecycle] WindowMode -> ", au::to_string(m)); });

    app.run();
    return 0;
}