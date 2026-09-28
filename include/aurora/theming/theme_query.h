#pragma once

#include <functional>
#include <utility>

#include "aurora/widget/provider.h"

namespace aurora {

/// @brief 解析 `target` 在 `root` 树中所处位置的生效主题（specification/07-environment-modifier.md §5.1）。
///
/// 沿构建好的控件树自顶向下 DFS，遇到 `ThemeProvider` 即更新"当前生效主题"，返回指定 widget
/// 在树中所处位置的**最近生效主题**。默认兜底为 `Theme::with_defaults()`。
///
/// @param root 遍历起点节点，DFS 从其控件（`root.widget()`）开始向下逐层展开。
/// @param target 待查询的控件，按地址与树中控件比对，首次命中即取其当前生效主题并停止遍历。
/// @return `target` 位置最近的生效主题；树中无 `ThemeProvider` 或 `target` 不在树内时为
///         `Theme::with_defaults()`。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] inline auto resolve_theme(const Node &root, const Widget &target) -> Theme {
    Theme result = Theme::with_defaults();
    bool found = false;
    std::function<void(const Widget &, Theme)> walk = [&](const Widget &w, Theme cur) -> void {
        Theme here = std::move(cur);
        // 命中 ThemeProvider 即更新当前生效主题（dynamic_cast 依赖 Widget 的多态性）。
        if (const auto *tp = dynamic_cast<const ThemeProvider *>(&w)) {
            here = tp->value();
        }
        if (&w == &target) {
            result = std::move(here);
            found = true;
            return;
        }
        w.for_each_child([&](const Widget &child) -> void {
            if (!found) {
                walk(child, here);
            }
        });
    };
    walk(root.widget(), Theme::with_defaults());
    return result;
}

/// @brief 解析整树根所处位置的生效主题（便捷重载）。
/// @param root 树根节点；以其自身控件为查询目标。
/// @return 根节点位置的生效主题，即树中最外层 `ThemeProvider` 的值；无 Provider 时为
///         `Theme::with_defaults()`。
[[nodiscard]] inline auto resolve_theme(const Node &root) -> Theme { return resolve_theme(root, root.widget()); }

}  // namespace aurora
