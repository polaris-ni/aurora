// theming_test.cpp — 覆盖主题系统（Theme / ThemeScope / Provider<Theme> / inherit_theme / resolve_theme）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// theming/theme_query.h(theme_query 解析链)、theming/theme_scope.h(ThemeScope 注入)、
// environment/build_context.h(BuildContext 依赖查找)。

#include <cmath>
#include <string>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Column;
using aurora::Constraints;
using aurora::Environment;
using aurora::LeafWidget;
using aurora::Node;
using aurora::Painter;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::State;
using aurora::Text;
using aurora::Theme;
using aurora::ThemeProvider;
using aurora::ThemeScope;
using aurora::WidgetDescriptor;

static void test_theme() {
    Theme light = Theme::light();
    Theme dark = Theme::dark();
    Theme wd = Theme::with_defaults();
    AURORA_TEST_CHECK_MSG(light.background != dark.background, "Theme: light/dark backgrounds differ");
    AURORA_TEST_CHECK_MSG(wd.background.m_a == 255, "Theme: with_defaults has opaque background");
    // 成员齐全
    AURORA_TEST_CHECK_MSG(light.primary.m_a == 255, "Theme: primary member opaque");
    AURORA_TEST_CHECK_MSG(light.on_primary.m_a == 255, "Theme: on_primary member opaque");
    AURORA_TEST_CHECK_MSG(light.text.m_a == 255, "Theme: text member opaque");
    AURORA_TEST_CHECK_MSG(light.font.family == std::string("sans-serif"), "Theme: font member present");
    // with_defaults 覆盖
    Theme custom = Theme::with_defaults();
    AURORA_TEST_CHECK_MSG(custom.primary == wd.primary, "Theme: with_defaults stable");
}

static void test_theme_scope() {
    Node root = ThemeScope{ Theme::dark(), Column{ Node{ Text{ "x" } } } };
    AURORA_TEST_CHECK_MSG(std::string(root.widget().type_name()) == "ThemeScope", "ThemeScope: type_name");

    // Provider<Theme> 基类注入值可读
    ThemeScope scope{ Theme::dark(), Text{ "x" } };
    const auto *tp = dynamic_cast<const ThemeProvider *>(&scope);
    AURORA_TEST_CHECK_MSG(tp != nullptr, "ThemeScope: is a ThemeProvider");
    AURORA_TEST_CHECK_MSG(tp->value().background == Theme::dark().background, "ThemeScope: injected value");

    // resolve_theme 沿树解析最近生效主题
    Theme resolved = resolve_theme(root);
    AURORA_TEST_CHECK_MSG(resolved.background == Theme::dark().background, "resolve_theme: root resolves to dark");

    // 嵌套覆盖：内层 ThemeScope 生效
    Node txt = Text{ "x" };
    Node inner = ThemeScope{ Theme::light(), ThemeScope{ Theme::dark(), txt } };
    Theme inner_resolved = resolve_theme(inner, txt.widget());
    AURORA_TEST_CHECK_MSG(inner_resolved.background == Theme::dark().background,
                          "resolve_theme: nearest ancestor wins");

    // inherit_theme：带 Environment 时返回注入主题，否则回退 light
    Environment env;
    env.set_local<Theme>(Theme::dark());
    BuildContext ctx;
    ctx.env = &env;
    Theme inherited = inherit_theme(ctx);
    AURORA_TEST_CHECK_MSG(inherited.background == Theme::dark().background, "inherit_theme: reads from env");
    BuildContext ctx2;
    Theme fallback = inherit_theme(ctx2);
    AURORA_TEST_CHECK_MSG(fallback.background == Theme::light().background, "inherit_theme: falls back to light");
}

namespace {
// 探测定制控件：记录布局阶段可见的主题（验证运行时换肤）。从 components_test 归并。
// 测试入口 / 构造，异常由 runner 统一捕获。
// NOLINTNEXTLINE(bugprone-exception-escape)
class ThemeProbe : public LeafWidget {
  public:
    Theme seen = Theme::light();
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "ThemeProbe"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "ThemeProbe", .children_policy = "none" };
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const auto *t = ctx.environment<Theme>();
        seen = (t != nullptr) ? *t : Theme::light();
        return c.constrain(Size{ .width = 10.0f, .height = 10.0f });
    }
    void on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) override {}
};
} // namespace

// 主题运行时换肤：ThemeScope 包裹 State<Theme>，状态变更触发重建（从 components_test 归并）。
static void test_theme_scope_state() {
    const auto theme_state = std::make_shared<State<Theme>>(Theme::light());
    auto scope = ThemeScope{ theme_state, Node{ ThemeProbe{} } };
    const BuildContext ctx;
    scope.mount(ctx);
    scope.layout(Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 100, .height = 100 } },
                 ctx);
    AURORA_TEST_CHECK_MSG(true, "ThemeScope with State<Theme> builds");

    theme_state->set(Theme::dark());
    const BuildContext ctx2;
    scope.mount(ctx2);
    scope.layout(Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 100, .height = 100 } },
                 ctx2);
    AURORA_TEST_CHECK_MSG(true, "ThemeScope rebuilds after State change");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== theming_test ===\n");
    test_theme();
    test_theme_scope();
    test_theme_scope_state();
}
