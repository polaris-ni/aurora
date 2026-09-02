// ============================================================================
// au_lint_core.h — au-lint 结构化检查核心（可单元测试）
// ----------------------------------------------------------------------------
// 把 au-lint 的 lint 逻辑从 au-lint.cpp 的匿名命名空间抽出，作为 inline 自由函数
// 暴露，供 au-lint 主程序与 tests/test_au_lint.cpp 共用，避免「工具实现 / 测试」
// 两份重复且漂移。行为与原匿名命名空间实现逐字等价。
//
// 消费方须先确保组件已注册（tests 中调用 aurora::serialization::register_core_widgets()，
// au-lint 主程序依赖全局静态注册），否则 list_all_components() 为空会把所有类型判为未知。
// ============================================================================
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <aurora/aurora.h>

namespace aurora::tools {

/// @brief 单条 lint 发现（结构化、机器可读，与 au::ErrorSeverity 对齐）。
struct LintFinding {
    au::ErrorSeverity severity = ErrorSeverity::Warning; // 与 Diagnostic/Error 对齐
    std::string code;
    std::string message;
    std::string path;
};

constexpr int AURORA_MAX_LINT_DEPTH = 64;

/// @brief 收集「已知组件类型」集合与「类型 → 已知属性键」映射。
/// 修饰节点（header-only Modifier<T>，不在 WidgetRegistry 中）显式允许，避免误报。
inline auto load_known_types() -> std::pair<std::set<std::string>, std::map<std::string, std::set<std::string>>> {
    auto types = au::list_all_components();
    std::set known(types.begin(), types.end());

    for (const char *m : { "Padding", "FlexWeight", "Background", "Border", "Clip", "SizeModifier", "Clickable",
                           "IgnorePointer", "Opacity", "Transform", "Align", "Center", "Expanded", "Flexible" }) {
        known.insert(m);
    }

    std::map<std::string, std::set<std::string>> props;
    for (const auto &t : types) {
        const au::Json schema = au::describe_component(t);
        // component_schema() 将属性键以「数组」形式返回（见 serialization::component_schema），
        // 故此处按数组解析（曾误判为 object，导致 unknown-prop 守卫形同虚设）。
        if (schema.contains("props") && schema["props"].is_array()) {
            std::set<std::string> keys;
            for (const auto &k : schema["props"]) {
                if (k.is_string()) {
                    keys.insert(k.get<std::string>());
                }
            }
            props[t] = std::move(keys);
        }
    }
    return { known, props };
}

inline void lint_node_impl(const au::Json &node, const std::string &path, int depth, const std::set<std::string> &known,
                           const std::map<std::string, std::set<std::string>> &props, std::vector<LintFinding> &out) {
    if (!node.is_object()) {
        out.push_back({ .severity = ErrorSeverity::Error,
                        .code = "node-not-object",
                        .message = "UI 树节点必须是对象",
                        .path = path });
        return;
    }
    if (!node.contains("type") || !node["type"].is_string()) {
        out.push_back({ .severity = ErrorSeverity::Error,
                        .code = "node-no-type",
                        .message = "节点缺少字符串 \"type\" 字段",
                        .path = path });
        return;
    }

    const std::string type = node["type"].get<std::string>();
    if (!known.contains(type)) {
        out.push_back({ .severity = ErrorSeverity::Warning,
                        .code = "unknown-type",
                        .message = "未知组件/修饰类型: " + type,
                        .path = path });
    }

    if (node.contains("props") && node["props"].is_object()) {
        if (auto pit = props.find(type); pit != props.end()) {
            for (auto it = node["props"].begin(); it != node["props"].end(); ++it) {
                if (!pit->second.contains(it.key())) {
                    out.push_back({ .severity = ErrorSeverity::Warning,
                                    .code = "unknown-prop",
                                    .message = "类型 " + type + " 的未知属性: " + it.key(),
                                    .path = path });
                }
            }
        }
    }

    if (node.contains("children")) {
        if (!node["children"].is_array()) {
            out.push_back({ .severity = ErrorSeverity::Error,
                            .code = "children-not-array",
                            .message = "\"children\" 必须是数组",
                            .path = path });
            return;
        }
        const auto &ch = node["children"];
        if (ch.empty()) {
            out.push_back({ .severity = ErrorSeverity::Info,
                            .code = "empty-container",
                            .message = type + " 容器没有子节点",
                            .path = path });
        }
        for (std::size_t i = 0; i < ch.size(); ++i) {
            lint_node_impl(ch.at(i), path + "/children/" + std::to_string(i), depth + 1, known, props, out);
        }
    }

    if (depth > AURORA_MAX_LINT_DEPTH) {
        out.push_back({ .severity = ErrorSeverity::Warning,
                        .code = "depth-exceeded",
                        .message = "UI 树深度超过 " + std::to_string(AURORA_MAX_LINT_DEPTH) + "（可能为无限递归）",
                        .path = path });
    }
}

/// @brief 对整棵 UI 树做结构化检查，返回全部发现（error 级决定 au-lint 退出码为 1）。
inline auto lint_ui_tree(const Json &root) -> std::vector<LintFinding> {
    const auto [known, props] = load_known_types();
    std::vector<LintFinding> out;
    lint_node_impl(root, "$", 0, known, props, out);
    return out;
}

} // namespace aurora::tools
