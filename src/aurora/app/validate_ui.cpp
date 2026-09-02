#include "aurora/app/validate_ui.h"

#include <algorithm>
#include <set>
#include <sstream>

#include "aurora/widget/serialization.h"
#include "aurora/widget/widget.h"

namespace aurora {

namespace {

/// @brief 缓存的 schema 查找表：类型名 -> schema JSON
auto schema_cache() -> std::map<std::string, Json> & {
    static std::map<std::string, Json> cache;
    return cache;
}

/// @brief 确保核心 widget 已注册并缓存 schema。
auto ensure_schemas() -> void {
    if (!schema_cache().empty()) {
        return;
    }
    serialization::register_core_widgets();
    for (const auto &s : list_all_schemas()) {
        if (s.contains("type") && s["type"].is_string()) {
            schema_cache()[s["type"].get<std::string>()] = s;
        }
    }
}

/// @brief 获取某类型的 schema（不存在返回 nullptr）。
auto get_schema(const std::string &type) -> const Json * {
    ensure_schemas();
    auto it = schema_cache().find(type);
    return it != schema_cache().end() ? &it->second : nullptr;
}

/// @brief JSON 值类型名称（用于错误信息）。
auto json_type_name(const Json &v) -> std::string_view {
    if (v.is_string()) {
        return "string";
    }
    if (v.is_number()) {
        return "number";
    }
    if (v.is_boolean()) {
        return "bool";
    }
    if (v.is_object()) {
        return "object";
    }
    if (v.is_array()) {
        return "array";
    }
    if (v.is_null()) {
        return "null";
    }
    return "unknown";
}

/// @brief 检查 JSON 值是否与 schema 声明的类型兼容。
auto type_matches(const Json &value, const std::string &declared_type) -> bool {
    // 声明类型可能是 C++ 类型名，做宽松匹配
    if (declared_type == "string" || declared_type == "LocalizedString" || declared_type == "std::string") {
        return value.is_string();
    }
    if (declared_type == "bool" || declared_type == "boolean") {
        return value.is_boolean();
    }
    if (declared_type == "int" || declared_type == "integer") {
        return value.is_number_integer();
    }
    if (declared_type == "float" || declared_type == "double" || declared_type == "number") {
        return value.is_number();
    }
    if (declared_type == "Color") {
        return value.is_string() || value.is_object();
    }
    if (declared_type == "Length") {
        return value.is_string() || value.is_object() || value.is_number();
    }
    if (declared_type == "EdgeInsets") {
        return value.is_object();
    }
    if (declared_type == "Alignment") {
        return value.is_string() || value.is_object();
    }
    // 未知声明类型：宽松通过（不阻断验证）
    return true;
}

/// @brief 递归验证 UI 树节点。
/// @param node 要验证的 UI 节点 JSON
/// @param path 当前节点在 UI 树中的路径
/// @param errors 收集验证错误的输出列表
/// @param depth 当前嵌套深度（根为 0）
/// @param max_depth 最大允许深度（默认 AURORA_DEFAULT_MAX_WIDGET_DEPTH）
// NOLINTNEXTLINE(readability-function-cognitive-complexity): 递归校验分支较多，复杂度 41 超阈值 25，重构收益低
auto validate_node(const Json &node, const std::string &path, std::vector<ValidationError> &errors,
                   std::size_t depth = 0, std::size_t max_depth = AURORA_DEFAULT_MAX_WIDGET_DEPTH) -> void {
    // 0. 深度守卫（有界层深度，见 specification/08-tooling.md §2.2）
    if (depth > max_depth) {
        errors.push_back({ .path = path,
                           .message = "nesting depth exceeded maximum of " + std::to_string(max_depth) +
                                      " (possible infinite recursion)",
                           .suggestion = "reduce the nesting depth or use a Repeater for dynamic lists" });
        return; // 不再递归验证更深层级
    }

    // 1. 必须有 type 字段
    if (!node.is_object()) {
        errors.push_back({ .path = path,
                           .message = "node must be a JSON object",
                           .suggestion = "wrap the value in an object with a \"type\" field" });
        return;
    }
    if (!node.contains("type") || !node["type"].is_string()) {
        errors.push_back({ .path = path,
                           .message = "missing or invalid \"type\" field",
                           .suggestion = R"(add "type": "WidgetName" (e.g. "Text", "Button", "Column"))" });
        return;
    }

    const std::string type = node["type"].get<std::string>();
    const std::string type_path = path + ".type";

    // 2. 类型必须在已注册列表中
    const Json *schema = get_schema(type);
    if (schema == nullptr) {
        // 收集已知类型列表作为建议
        std::string known;
        for (const auto &kv : schema_cache() | std::views::keys) {
            if (!known.empty()) {
                known += ", ";
            }
            known += kv;
        }
        errors.push_back({ .path = type_path,
                           .message = "unknown widget type: \"" + type + "\"",
                           .suggestion = "use one of: " + known });
        return; // 未知类型无法继续验证属性
    }

    // 3. 验证 props
    const std::string props_path = path + ".props";
    if (node.contains("props") && !node["props"].is_object()) {
        errors.push_back({ .path = props_path,
                           .message = "\"props\" must be a JSON object",
                           .suggestion = R"(change to an object, e.g. {"text": "Hello"})" });
    }

    Json empty_props = Json::object();
    const Json &props = node.value("props", empty_props);

    // 3a. 检查必填属性
    if (schema->contains("prop_descriptors") && (*schema)["prop_descriptors"].is_array()) {
        for (const auto &pd : (*schema)["prop_descriptors"]) {
            const std::string p_name = pd.value("name", "");
            const bool required = pd.value("required", false);
            const std::string ptype = pd.value("type", "");

            if (required && !props.contains(p_name)) {
                std::string p_path;
                p_path += props_path;
                p_path += '.';
                p_path += p_name;
                std::string msg = "missing required prop: \"";
                msg += p_name;
                msg += '\"';
                std::string sug = "add \"";
                sug += p_name;
                sug += "\": <";
                sug += ptype;
                sug += "> to props";
                errors.push_back({ .path = p_path, .message = msg, .suggestion = sug });
                continue;
            }

            // 3b. 检查类型匹配
            if (props.contains(p_name) && !ptype.empty()) {
                if (!type_matches(props[p_name], ptype)) {
                    std::string p_path;
                    p_path += props_path;
                    p_path += '.';
                    p_path += p_name;
                    std::string msg = "type mismatch: prop \"";
                    msg += p_name;
                    msg += "\" expects ";
                    msg += ptype;
                    msg += " but got ";
                    msg += json_type_name(props[p_name]);
                    std::string sug = "change the value to a ";
                    sug += ptype;
                    errors.push_back({ .path = p_path, .message = msg, .suggestion = sug });
                }
            }
        }
    }

    // 4. 验证子节点策略
    const std::string children_policy = schema->value("children_policy", "none");
    const std::string children_path = path + ".children";

    if (node.contains("children")) {
        if (!node["children"].is_array()) {
            errors.push_back({ .path = children_path,
                               .message = "\"children\" must be an array",
                               .suggestion = "change to an array of node objects" });
        } else {
            if (children_policy == "none" && !node["children"].empty()) {
                errors.push_back(
                    { .path = children_path,
                      .message = "widget \"" + type + "\" does not accept children (children_policy=none)",
                      .suggestion = "remove the \"children\" field or use a container widget (Column/Row/Stack)" });
            } else if (children_policy == "single" && node["children"].size() > 1) {
                errors.push_back(
                    { .path = children_path,
                      .message = "widget \"" + type + "\" accepts at most 1 child but got " +
                                 std::to_string(node["children"].size()),
                      .suggestion = "reduce children to a single element or use a multi-child container" });
            }

            // 递归验证子节点（深度 +1）
            for (size_t i = 0; i < node["children"].size(); ++i) {
                validate_node(node["children"][i], children_path + "[" + std::to_string(i) + "]", errors, depth + 1,
                              max_depth);
            }
        }
    }
}

} // namespace

auto validate_ui_tree(const Json &tree) -> std::vector<ValidationError> {
    ensure_schemas();
    std::vector<ValidationError> errors;
    validate_node(tree, "$", errors);
    return errors;
}

auto validate_ui_tree_json(const Json &tree) -> Json {
    const auto errors = validate_ui_tree(tree);
    Json out;
    out["valid"] = errors.empty();
    Json err_array = Json::array();
    for (const auto &e : errors) {
        err_array.push_back(e.to_json());
    }
    out["errors"] = err_array;
    return out;
}

} // namespace aurora
