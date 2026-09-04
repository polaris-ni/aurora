#pragma once

#include <functional>
#include <utility>

#include "aurora/widget/provider.h"

namespace aurora {

/**
 * @brief 主题解析查询（specification/07-environment-modifier.md §5.1）。
 *
 * 沿构建好的控件树自顶向下 DFS，遇到 `ThemeProvider` 即更新"当前生效主题"，
 * 返回指定 widget（或整树根）在树中所处位置的**最近生效主题**。默认兜底为
 * `Theme::with_defaults()`。单线程 UI 假设。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */

/// @brief 解析 `target` 在 `root` 树中所处位置的生效主题。
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
[[nodiscard]] inline auto resolve_theme(const Node &root) -> Theme { return resolve_theme(root, root.widget()); }

}  // namespace aurora
