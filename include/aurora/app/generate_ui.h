#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/widget/serialization.h"

namespace aurora {

/// @brief 「文案类属性」的候选键（按优先级）。
///
/// 各控件的文本属性名并不统一（`Text` 用 `content`、`Button` 用 `label`），生成时按下表挑第一个
/// **在该类型 schema 里真实存在**的键。⚠️ 早期版本硬编码写 `props.text`，而没有任何控件读 `text`
/// —— 生成的树看着有文案，实际反序列化后是空的。这是本函数改为查 schema 的直接原因。
inline constexpr std::array<std::string_view, 3> AURORA_UI_TEXT_PROP_CANDIDATES = {"content", "label", "text"};

/// @brief NL→UI 生成（specification/08-tooling.md §2.6）：由自然语言描述生成 Widget JSON 树。
///
/// 当前为关键词匹配简版（**不依赖 LLM**）：把描述切成词，逐个词与已注册控件类型名（小写）比对，
/// 命中即生成一个该类型的节点，全部平铺在一个 `Stack` 下。完整 NL→UI 需外部 LLM，
/// 见 `ui_prompt.h`（prompt 投影 + 自修复环）。
///
/// 覆盖面由 `serialization::list_all_components()` 派生 —— 新增控件无需改本函数即可被识别。
/// 额外的口语别名见 `AURORA_UI_KEYWORD_ALIASES`（如 `label`→`Text`、`btn`→`Button`）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] inline auto generate_ui(const std::string &description) -> Result<Json> {
    if (description.empty()) {
        return make_error(ErrorCode::GenerateUiEmpty, "empty description");
    }

    // ── 切词：非字母数字即分隔符，统一小写 ──
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (const char c : description) {
            const auto uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) != 0) {
                cur.push_back(static_cast<char>(std::tolower(uc)));
            } else if (!cur.empty()) {
                tokens.push_back(std::move(cur));
                cur.clear();
            }
        }
        if (!cur.empty()) {
            tokens.push_back(std::move(cur));
        }
    }

    auto lower = [](std::string s) -> std::string {
        for (char &c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };

    // ── 别名表：口语词 → 类型。仅在精确匹配阶段使用，故不会误伤子串 ──
    static const std::array<std::pair<std::string_view, std::string_view>, 4> AURORA_UI_KEYWORD_ALIASES = {{
        {"label", "Text"},
        {"btn", "Button"},
        {"pic", "ImageView"},
        {"dropdown", "Dropdown"},
    }};

    const std::vector<std::string> types = aurora::list_all_components();

    std::vector<std::string> hits;
    auto remember = [&hits](const std::string &type) -> void {
        if (std::find(hits.begin(), hits.end(), type) == hits.end()) {
            hits.push_back(type);
        }
    };

    // ── 第一轮：整词精确匹配类型名（小写）与别名 ──
    for (const std::string &tok : tokens) {
        for (const std::string &type : types) {
            if (lower(type) == tok) {
                remember(type);
            }
        }
        for (const auto &[alias, target] : AURORA_UI_KEYWORD_ALIASES) {
            if (tok == alias) {
                remember(std::string(target));
            }
        }
    }

    // ── 第二轮：仍无命中时，放宽为「长词做子串匹配」（短词太容易误命中，故要求 >= 4 字符）──
    if (hits.empty()) {
        for (const std::string &tok : tokens) {
            if (tok.size() < 4U) {
                continue;
            }
            for (const std::string &type : types) {
                if (lower(type).find(tok) != std::string::npos) {
                    remember(type);
                }
            }
        }
    }

    // ── 组装：命中的按注册顺序平铺进一个 Stack ──
    Json children = Json::array();
    for (const std::string &type : hits) {
        Json node = Json::object();
        node["type"] = type;

        // 该类型若有文案类属性，填入类型名作为占位文案（与旧行为一致：Button→"Button"）。
        const Json schema = aurora::describe_component(type);
        if (schema.contains("default_props")) {
            const Json &defaults = schema["default_props"];
            for (const std::string_view key : AURORA_UI_TEXT_PROP_CANDIDATES) {
                if (defaults.contains(std::string(key))) {
                    node["props"][std::string(key)] = type;
                    break;
                }
            }
        }
        children.push_back(node);
    }

    // ── 回退：一个都没命中时产出带原描述片段的 Text（截断 20 字符）──
    if (children.empty()) {
        Json node = Json::object();
        node["type"] = "Text";
        node["props"]["content"] = "?" + description.substr(0, std::min<std::size_t>(description.size(), 20));
        children.push_back(node);
    }

    Json tree = Json::object();
    tree["node"] = Json::object();
    tree["node"]["type"] = "Stack";
    tree["node"]["props"] = Json::object();
    tree["node"]["children"] = children;
    return tree;
}

/// @brief 快速验证：描述 → JSON → from_json 往返成功即表示 schema 匹配。
[[nodiscard]] inline auto validate_generate_ui(const std::string &desc) -> bool {
    const auto r = generate_ui(desc);
    if (!r.ok()) {
        return false;
    }
    // generate_ui 返回带 "node" 包装的树，from_json 需要顶层 "type" 的节点对象。
    const Json &node = r.value().value("node", Json::object());
    return aurora::serialization::from_json(node).ok();
}

}  // namespace aurora
