/// 测试类型: unit
/// 目标单元: include/aurora/widget/provider.h
/// 测试说明: provider 单元测试
///

// Provider<Theme> 环境注入 1:1 测试：后代可读注入值。
#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_provider {

namespace {
// 探针：在布局阶段读取注入的环境值（验证 Provider 向下传播）。
class EnvProbe : public LeafWidget {
  public:
    const Theme *seen = nullptr;
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "EnvProbe"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "EnvProbe", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        seen = ctx.environment<Theme>();
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
    AURORA_TEST_CHECK_MSG(probe->seen != nullptr, "Provider<Theme>: value visible to descendants");
    AURORA_TEST_CHECK_MSG(probe->seen && probe->seen->background == Theme::dark().background,
                          "Provider<Theme>: correct value injected");

    theme_state->set(Theme::light());
    const auto probe2 = std::make_shared<EnvProbe>();
    Provider prov2{theme_state, Node{probe2}};
    const BuildContext ctx2;
    prov2.mount(ctx2);
    prov2.layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 100, .height = 100}}, ctx2);
    AURORA_TEST_CHECK_MSG(probe2->seen && probe2->seen->background == Theme::light().background,
                          "Provider<Theme>: state change propagates");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_provider ===\n");
    test_provider();
}

}  // namespace aurora::test_cases::utest_provider