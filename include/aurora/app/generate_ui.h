#pragma once

#include <array>
#include <string>

#include "aurora/core/result.h"
#include "aurora/widget/serialization.h"

namespace aurora {

/// @brief NL→UI 生成（specification/08-tooling.md §2.5）：由自然语言描述生成 Widget JSON 树。
///
/// 当前为关键词匹配简版（不依赖 LLM）：扫描描述中的控件关键词（button/text/column/row/checkbox），
/// 从 aurora_api.json schema 提取默认属性，组装为可 `from_json` 的 JSON 片段。
/// 完整 NL→UI 需外部 LLM 工具链。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] inline auto generate_ui(const std::string &description) -> Result<Json> {
    if (description.empty()) {
        return make_error(ErrorCode::GenerateUiEmpty, "empty description");
    }

    Json tree = Json::object();
    Json children = Json::array();

    // 关键词 → 类型映射
    struct Entry {
        std::string keyword;
        std::string type;
        std::string text;
    };
    static const std::array<Entry, 8> MAP = {{
        {.keyword = "button", .type = "Button", .text = "Button"},
        {.keyword = "text", .type = "Text", .text = "Text"},
        {.keyword = "label", .type = "Text", .text = "Text"},
        {.keyword = "column", .type = "Column", .text = ""},
        {.keyword = "row", .type = "Row", .text = ""},
        {.keyword = "checkbox", .type = "Checkbox", .text = ""},
        {.keyword = "switch", .type = "Switch", .text = ""},
        {.keyword = "slider", .type = "Slider", .text = ""},
    }};

    for (const auto &m : MAP) {
        if (description.find(m.keyword) != std::string::npos) {
            Json node;
            node["type"] = m.type;
            if (!m.text.empty()) {
                node["props"]["text"] = m.text;
            }
            children.push_back(node);
        }
    }

    // 无匹配时返回空
    if (children.empty()) {
        Json node;
        node["type"] = "Text";
        node["props"]["text"] = "?" + description.substr(0, std::min<std::size_t>(description.size(), 20));
        children.push_back(node);
    }

    tree["node"] = Json::object();
    tree["node"]["type"] = "Stack";
    tree["node"]["props"] = Json::object();
    tree["node"]["children"] = children;

    return tree;
}

/// @brief 快速验证：描述 → JSON → from_json 往返成功即表示 schema 匹配。
[[nodiscard]] inline auto validate_generate_ui(const std::string &desc) -> bool {
    auto r = generate_ui(desc);
    if (!r.ok()) {
        return false;
    }
    // generate_ui 返回带 "node" 包装的树，from_json 需要顶层 "type" 的节点对象。
    const Json &node = r.value().value("node", Json::object());
    auto w = serialization::from_json(node);
    return w.ok();
}

}  // namespace aurora
