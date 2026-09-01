#pragma once

#include <concepts>
#include <utility>

#include "aurora/environment/build_context.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/provider.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 主题作用域：为子树覆盖（或首次注入）主题（规格 §8 显式主题传递）。
 *
 * 语义等价于 `Provider<Theme>`，但提供领域化的名称与构造。子树内任意 widget
 * 经 `inheritTheme(ctx)` 读取最近祖先的 `ThemeScope` 注入值（最近祖先优先，
 * 天然实现主题继承与局部覆盖）。
 *
 * @code
 *   ThemeScope{ Theme::dark(), Column{ .children = { Text{"Hello"} } } };
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: yes, via from_json
 */
class ThemeScope : public Provider<Theme> {
  public:
    using Provider<Theme>::Provider; ///< 复用 Provider<Theme>(Theme, Node/Widget) 构造

    /// @brief 运行时换肤：用共享 `State<Theme>` 注入，主题变化自动重渲染子树。
    explicit ThemeScope(std::shared_ptr<State<Theme>> theme, Node child)
        : Provider<Theme>(std::move(theme), std::move(child)) {}

    template<typename W>
        requires std::derived_from<W, Widget>
    explicit ThemeScope(std::shared_ptr<State<Theme>> theme, W &&child)
        : Provider(std::move(theme), std::forward<W>(child)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ThemeScope"; }
};

/**
 * @brief 读取最近祖先 `ThemeScope` 注入的主题（§8 显式主题传递）。
 *
 * 在 widget 的 `on_paint`/`on_layout` 中调用以获取当前主题令牌（颜色、字体），
 * 从而让绘制随主题变化。无注入主题时返回默认浅色主题，保证永不崩溃。
 *
 * @param ctx 当前构建上下文（由 layout/paint 传入）。
 * @return 最近祖先注入的 Theme；未注入则 `Theme::light()`。
 */
[[nodiscard]] inline auto inherit_theme(const BuildContext &ctx) -> Theme {
    const auto *t = ctx.environment<Theme>();
    return t != nullptr ? *t : Theme::light();
}

} // namespace aurora
