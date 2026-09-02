#pragma once

#include <functional>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 渲染前校验整棵 UI 树（规格 #9）。
 *
 * 检查三类问题：
 *  - 空子节点（nullptr）：结构不完整，渲染会崩溃；
 *  - 深度超限：嵌套过深（默认上限 64），可能是递归 bug；
 *  - 未知控件类型：类型名拼写错误或尚未注册。
 *
 * 返回 `Result<bool>`：成功为 `true`；首个问题转为结构化 `Error`（含 code / message / suggestion）。
 *
 * @param root 待校验的根节点（必须非空；空根请先构造合法 widget）
 * @param max_depth 允许的最大嵌套深度（默认 64）
 *
 * @code
 *   auto ok = au::validate(root);
 *   if (!ok) { log_error(ok.error()); return; }
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
[[nodiscard]] inline auto validate(const Node &root, int max_depth = 64) -> Result<bool> {
    serialization::register_core_widgets(); // 确保核心控件类型已在注册表

    std::vector<Error> errs;
    std::function<void(const Node &, int)> walk = [&](const Node &n, int depth) -> void {
        if (depth > max_depth) {
            errs.push_back(
                make_error(ErrorCode::ValidationTreeTooDeep,
                           "UI tree depth " + std::to_string(depth) + " exceeds limit " + std::to_string(max_depth),
                           "Reduce nesting level, or increase the max_depth parameter of validate"));
            return;
        }

        const std::string t = n.widget().type_name();
        const auto made = serialization::WidgetRegistry::instance().make(t, Json::object());
        // 比对枚举而非字符串字面量：slug 拼写漂移（曾误写为 "unknown-widget-type"）会让
        // 未注册控件被静默判为合法。code_enum 由 g_error_table 表驱动注入，无拼写风险。
        if (!made && made.error().code_enum == ErrorCode::WidgetUnknownType) {
            errs.push_back(make_error(ErrorCode::ValidationUnknownWidget, "Unknown widget type '" + t + "'",
                                      "Check type name spelling, or register the widget with "
                                      "WidgetRegistry::instance().register_factory first"));
        }

        for (const Node &c : n.widget().child_nodes()) {
            if (!c) {
                errs.push_back(make_error(
                    ErrorCode::ValidationNullChild, "Widget '" + t + "' contains a null child node (nullptr)",
                    "Use conditional construction (e.g. Show) or ensure children contains no nullptr"));
                continue;
            }
            walk(c, depth + 1);
        }
    };

    walk(root, 0);
    if (!errs.empty()) {
        return errs.front();
    }
    return Result{ true };
}

} // namespace aurora
