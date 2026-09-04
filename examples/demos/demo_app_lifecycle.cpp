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
};
}  // namespace

using namespace std::chrono_literals;

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
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

    au::Scene scene{std::move(root)};
    au::WindowOptions opts;
    opts.size = au::Size{.width = 520.0F, .height = 440.0F};
    opts.title = "App Lifecycle · Aurora Demo";
    auto win_res = au::create_native_window(opts);
    au::Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};

    // 窗口级生命周期：隐藏/被遮挡时暂停动画，可见时恢复。
    app.set_on_window_state([paused](au::WindowState s) -> void { paused->set(s != au::WindowState::Visible); });
    app.set_on_window_mode(
        [](au::WindowMode m) -> void { AURORA_LOG_INFO("demo", "[AppLifecycle] WindowMode -> ", au::to_string(m)); });

    app.run();
    return 0;
}