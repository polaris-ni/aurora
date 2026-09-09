/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme_scope.h
/// 测试说明: 覆盖 ThemeScope 的类型名与 Provider<Theme> 身份、静态值与共享 State 两种注入方式的取值、
/// 经 State 换肤后取值的跟随性，以及 inherit_theme 在无注入环境下的兜底契约

#include <memory>
#include <string>

#include "aurora/core/color.h"
#include "aurora/environment/build_context.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/placeholder.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_scope {

AURORA_TEST_CASE(theme_scope_type_name_is_domain_named) {
    // 领域化命名：序列化/检视面板上要能识别为 ThemeScope 而非通用 Provider。
    const ThemeScope scope{Theme::dark(), Placeholder{}};
    AURORA_TEST_CHECK_EQ(std::string{scope.type_name()}, std::string{"ThemeScope"});
}

AURORA_TEST_CASE(theme_scope_exposes_injected_value) {
    const Theme expected = Theme::dark();
    const ThemeScope scope{expected, Placeholder{}};
    AURORA_TEST_CHECK_EQ(scope.value().background, expected.background);
    AURORA_TEST_CHECK_EQ(scope.value().primary, expected.primary);
    AURORA_TEST_CHECK_EQ(scope.value().text, expected.text);
}

AURORA_TEST_CASE(theme_scope_is_a_theme_provider) {
    // 语义等价于 Provider<Theme>：可经 Provider<Theme>* 多态取用。
    const ThemeScope scope{Theme::dark(), Placeholder{}};
    const Provider<Theme> *as_provider = &scope;
    AURORA_TEST_CHECK_NOT_NULL(as_provider);
    AURORA_TEST_CHECK_EQ(as_provider->value().background, Theme::dark().background);
}

AURORA_TEST_CASE(theme_scope_from_shared_state_tracks_updates) {
    // 运行时换肤：共享 State 变化后，value() 须立即反映新主题。
    auto state = std::make_shared<State<Theme>>(Theme::light());
    const ThemeScope scope{state, Placeholder{}};

    AURORA_TEST_CHECK_EQ(scope.value().background, Theme::light().background);
    state->set(Theme::dark());
    AURORA_TEST_CHECK_EQ(scope.value().background, Theme::dark().background);
}

AURORA_TEST_CASE(inherit_theme_falls_back_to_light_without_injection) {
    // 未注入主题时返回浅色主题，保证绘制路径永不因缺主题而崩溃。
    constexpr BuildContext ctx;
    const Theme inherited = inherit_theme(ctx);
    AURORA_TEST_CHECK_EQ(inherited.background, Theme::light().background);
    AURORA_TEST_CHECK_EQ(inherited.text, Theme::light().text);
}

}  // namespace aurora::test_cases::utest_theme_scope
