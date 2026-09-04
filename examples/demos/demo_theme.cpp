// 主题 demo：ThemeProvider 注入后，自定义控件通过 ctx.environment<Theme>() 读取主题。
#include "demo_common.h"

namespace {
class ThemedBox : public au::LeafWidget {
  public:
    explicit ThemedBox(std::string label) : label_(std::move(label)) {}

    void collect_signals(std::vector<au::SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "ThemedBox"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "ThemedBox", .children_policy = "none"};
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = 220.0F, .height = 80.0F});
    }

    void on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) override {
        const auto *t = ctx.environment<au::Theme>();
        const au::Color bg = (t != nullptr) ? t->background : au::Color{255, 255, 255};
        const au::Color fg = (t != nullptr) ? t->text : au::Color{17, 24, 39};
        const au::Color primary = (t != nullptr) ? t->primary : au::Color{37, 99, 235};
        constexpr au::Color border = pal::AURORA_BORDER;
        p.fill_rect(b, bg);
        p.fill_rect(au::Rect{.origin = b.origin, .size = au::Size{.width = b.size.width, .height = 10.0F}}, primary);
        const float th = aurora::render::FontEngine::measure_height(au::Font{.size_pt = 16.0F});
        p.draw_text(au::Rect{.origin = au::Point{.x = b.origin.x + 12.0F, .y = b.origin.y + 28.0F},
                             .size = au::Size{.width = b.size.width - 24.0F, .height = th}},
                    label_, au::Font{.size_pt = 16.0F}, fg);
        p.draw_rect(b, border);
    }

  private:
    std::string label_;
};
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto light = au::Theme::light();
    auto dark = au::Theme::dark();

    au::Node root = au::Column{
        GradientTitle{"Theme"},
        gap(12),
        au::Text{au::LocalizedString{"After ThemeProvider injection, widgets read ctx.environment<Theme>()"}},
        gap(8),
        au::ThemeProvider{light, ThemedBox{"light theme"}},
        gap(8),
        au::ThemeProvider{dark, ThemedBox{"dark theme"}},
    };
    return run_demo(Card{std::move(root)}, "Theme · Aurora Demo", 540.0F, 480.0F);
}