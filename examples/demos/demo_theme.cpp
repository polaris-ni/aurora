// 主题 demo：ThemeProvider 注入后，自定义控件通过 ctx.environment<Theme>() 读取主题。
#include "demo_common.h"

namespace {
class ThemedBox : public au::LeafWidget {
  public:
    explicit ThemedBox(std::string label) : m_label(std::move(label)) {}

    void collect_signals(std::vector<au::SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "ThemedBox"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{ .name = "ThemedBox", .children_policy = "none" };
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{ .width = 220.0f, .height = 80.0f });
    }

    void on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) override {
        const auto *t = ctx.environment<au::Theme>();
        const au::Color bg = (t != nullptr) ? t->background : au::Color{ 255, 255, 255 };
        const au::Color fg = (t != nullptr) ? t->text : au::Color{ 17, 24, 39 };
        const au::Color primary = (t != nullptr) ? t->primary : au::Color{ 37, 99, 235 };
        constexpr au::Color border = pal::AURORA_BORDER;
        p.fill_rect(b, bg);
        p.fill_rect(au::Rect{ .origin = b.origin, .size = au::Size{ .width = b.size.width, .height = 10.0f } },
                    primary);
        const float th = aurora::render::FontEngine::measure_height(au::Font{ .size_pt = 16.0f });
        p.draw_text(au::Rect{ .origin = au::Point{ .x = b.origin.x + 12.0f, .y = b.origin.y + 28.0f },
                              .size = au::Size{ .width = b.size.width - 24.0f, .height = th } },
                    m_label, au::Font{ .size_pt = 16.0f }, fg);
        p.draw_rect(b, border);
    }

  private:
    std::string m_label;
};
} // namespace

auto main() -> int {
    auto light = au::Theme::light();
    auto dark = au::Theme::dark();

    au::Node root = au::Column{
        GradientTitle{ "Theme" },
        gap(12),
        au::Text{ au::LocalizedString{ "After ThemeProvider injection, widgets read ctx.environment<Theme>()" } },
        gap(8),
        au::ThemeProvider{ light, ThemedBox{ "light theme" } },
        gap(8),
        au::ThemeProvider{ dark, ThemedBox{ "dark theme" } },
    };
    return run_demo(Card{ std::move(root) }, "Theme · Aurora Demo", 540.0f, 480.0f);
}
