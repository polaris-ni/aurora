// test_provider.cpp — Provider<Theme> 环境注入 1:1 测试：后代可读注入值。
#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::LeafWidget;
using aurora::Node;
using aurora::Painter;
using aurora::Provider;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::State;
using aurora::Theme;
using aurora::WidgetDescriptor;

namespace {
// 探针：在布局阶段读取注入的环境值（验证 Provider 向下传播）。
class EnvProbe : public LeafWidget {
  public:
    const Theme *seen_ = nullptr;
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "EnvProbe"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "EnvProbe", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        seen_ = ctx.environment<Theme>();
        return c.constrain(Size{.width = 10.0F, .height = 10.0F});
    }
    void on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) override {}
};
}  // namespace

static void test_provider() {
    const auto theme_state = std::make_shared<State<Theme>>(Theme::dark());
    const auto probe = std::make_shared<EnvProbe>();
    Provider prov{theme_state, Node{probe}};
    const BuildContext ctx;
    prov.mount(ctx);
    prov.layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 100, .height = 100}}, ctx);
    AURORA_TEST_CHECK_MSG(probe->seen_ != nullptr, "Provider<Theme>: value visible to descendants");
    AURORA_TEST_CHECK_MSG(probe->seen_ && probe->seen_->background == Theme::dark().background,
                          "Provider<Theme>: correct value injected");

    theme_state->set(Theme::light());
    const auto probe2 = std::make_shared<EnvProbe>();
    Provider prov2{theme_state, Node{probe2}};
    const BuildContext ctx2;
    prov2.mount(ctx2);
    prov2.layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 100, .height = 100}}, ctx2);
    AURORA_TEST_CHECK_MSG(probe2->seen_ && probe2->seen_->background == Theme::light().background,
                          "Provider<Theme>: state change propagates");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_provider ===\n");
    test_provider();
}
