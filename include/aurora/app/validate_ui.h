#pragma once

#include <string>
#include <vector>

#include "aurora/widget/props_io.h" // Json

namespace aurora {

/**
 * @brief UI 树 JSON Schema 验证错误（规格 §7.1）。
 *
 * 每条错误携带 JSON 路径、错误信息与修复建议，供 AI Agent 自动修复。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
struct ValidationError {
    std::string path;       ///< JSON 路径，如 "$.children[0].props.text"
    std::string message;    ///< 错误描述，如 "missing required prop"
    std::string suggestion; ///< 修复建议，如 "add text: \"Hello\""

    [[nodiscard]] auto to_json() const -> Json {
        Json j;
        j["path"] = path;
        j["message"] = message;
        if (!suggestion.empty()) {
            j["suggestion"] = suggestion;
        }
        return j;
    }
};

/**
 * @brief 验证 UI 树 JSON 是否符合 aurora_api.json schema（规格 §7.1）。
 *
 * 用途：AI 生成 UI JSON 后先验证再消费，提前捕获：
 * - 未知 widget 类型（type 字段不在已注册列表中）
 * - 缺失必填属性（required=true 的 prop 未提供）
 * - 类型不匹配（属性值类型与 schema 声明不符）
 * - 非法枚举值（属性值不在已知枚举范围内）
 * - 子节点策略违规（如 children_policy="none" 但提供了 children）
 *
 * @param tree UI 树 JSON（to_json 格式：{type, props, children}）
 * @return 错误列表（空 = 验证通过）
 */
[[nodiscard]] auto validate_ui_tree(const Json &tree) -> std::vector<ValidationError>;

/**
 * @brief 验证 UI 树 JSON 并返回 JSON 格式的错误报告。
 *
 * 便捷方法：失败时返回 {"valid": false, "errors": [...]}，
 * 成功时返回 {"valid": true, "errors": []}。
 */
[[nodiscard]] auto validate_ui_tree_json(const Json &tree) -> Json;

} // namespace aurora
