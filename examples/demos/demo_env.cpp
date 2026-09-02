// 环境 demo：MediaQuery / MediaQueryProvider / BuildContext::environment<T>()。
#include "demo_common.h"

namespace {
class EnvReadout : public au::LeafWidget {
  public:
    void collect_signals(std::vector<au::SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "EnvReadout"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{ .name = "EnvReadout", .children_policy = "none" };
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{ .width = 300.0f, .height = 64.0f });
    }

    void on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) override {
        const auto *mq = ctx.environment<au::MediaQuery>();
        const double sf = (mq != nullptr) ? mq->scale_factor : 1.0;
        p.fill_rect(b, pal::AURORA_SURFACE);
        p.draw_rect(b, pal::AURORA_BORDER);
        const float th = aurora::render::FontEngine::measure_height(au::Font{ .size_pt = 16.0f });
        p.draw_text(au::Rect{ .origin = au::Point{ .x = b.origin.x + 12.0f, .y = b.origin.y + 22.0f },
                              .size = au::Size{ .width = b.size.width - 24.0f, .height = th } },
                    "scale_factor = " + std::to_string(sf), au::Font{ .size_pt = 16.0f }, pal::AURORA_TEXT);
    }
};
} // namespace

auto main() -> int {
    const auto mq = au::MediaQuery::of(2.0);

    au::Node root = au::Column{
        GradientTitle{ "Environment" },
        gap(12),
        au::Text{ au::LocalizedString{ "MediaQueryProvider injects scale factor" } },
        gap(8),
        au::MediaQueryProvider{ mq, EnvReadout{} },
    };
    return run_demo(Card{ std::move(root) }, "Environment · Aurora Demo", 520.0f, 380.0f);
}
