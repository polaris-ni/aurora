#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/generate_ui.h"
#include "aurora/app/validate_ui.h"
#include "aurora/widget/serialization.h"

namespace aurora {

// ═══════════════════════════════════════════════════════════════════════════
// 1) schema → LLM prompt 的投影
// ═══════════════════════════════════════════════════════════════════════════

/// @brief prompt 投影选项（specification/08-tooling.md §2.6）。
struct UiPromptOptions {
    bool include_children_policy = true;  ///< 是否写 children 策略（none/single/multiple）
    bool include_defaults = true;  ///< 是否写出缺省值 —— 能让 LLM 少写冗余属性
    bool include_examples = false;  ///< 是否附控件自带的示例（体积敏感，默认关）
    std::size_t max_types = 0;  ///< 类型上限，0 = 全部
};

/// @brief 把若干控件的 schema 投影成一段紧凑、可供外部 LLM 直接消费的 prompt。
///
/// **红线**：本库**不发起任何网络请求**。这里只产出「喂给 LLM 的文字」，调用 LLM 的是外部 Agent。
/// 之所以要做投影而不是让 Agent 自己去读 `aurora_api.json` / `get_schema`：全量 schema 体积过大，
/// 直接塞进上下文既贵又容易被截断。
///
/// @param types 需要的类型名；未知类型跳过
/// @return markdown 文本；无可用类型时返回空串
///
/// @note Thread: main-thread only（读注册表）
/// @note Side-effects: none
[[nodiscard]] inline auto build_ui_prompt(const std::vector<std::string> &types, const UiPromptOptions &opt = {})
    -> std::string {
    std::string out;
    out += "# Aurora UI tree\n\n";
    out += "Reply with a single JSON object shaped as:\n";
    out += "{\"node\":{\"type\":\"<Type>\",\"props\":{...},\"children\":[...]}}\n\n";
    out += "Rules: use only the types listed below; every node needs a `type`; ";
    out += "`props` keys must match the names given; omit optional props you do not need.\n\n";

    const std::vector<std::string> registered_types = aurora::list_all_components();
    std::size_t emitted = 0;
    for (const std::string &type : types) {
        if (opt.max_types > 0 && emitted >= opt.max_types) {
            break;
        }
        // 未注册类型直接跳过。判据必须是**注册表成员表**：`component_schema` 会无条件写入
        // `w["type"] = name`，故「返回对象里有 type」不代表该类型存在。
        if (std::find(registered_types.begin(), registered_types.end(), type) == registered_types.end()) {
            continue;
        }
        const Json schema = aurora::describe_component(type);

        out += "## " + type + "\n";
        std::string tags;
        if (schema.value("is_container", false)) {
            tags += " container";
        }
        if (schema.value("is_clickable", false)) {
            tags += " clickable";
        }
        if (!tags.empty()) {
            out += "(" + tags.substr(1) + ")\n";
        }
        if (opt.include_children_policy) {
            out += "- children: " + schema.value("children_policy", std::string("none")) + "\n";
        }

        // 属性：类型取自 prop_descriptors，缺省值取自 default_props（后者是实测序列化结果，最可信）。
        const Json defaults = schema.value("default_props", Json::object());
        if (defaults.is_object() && !defaults.empty()) {
            out += "- props: ";
            bool first = true;
            for (auto it = defaults.begin(); it != defaults.end(); ++it) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                out += it.key();
                if (opt.include_defaults) {
                    Json v = it.value();
                    out += "=" + (v.is_string() ? v.get<std::string>() : v.dump());
                }
            }
            out += "\n";
        }
        if (opt.include_examples && schema.contains("examples")) {
            for (const Json &ex : schema["examples"]) {
                out += "- example: " + (ex.is_string() ? ex.get<std::string>() : ex.dump()) + "\n";
            }
        }
        out += "\n";
        ++emitted;
    }

    return emitted > 0 ? out : std::string{};
}

/// @brief 按自然语言描述**收敛**出相关类型子集，再投影为 prompt。
///
/// 先用 `generate_ui` 的关键词匹配探测描述里提到了哪些控件，再补上几乎总会用到的布局与文本类型。
/// 这样 prompt 只带「这次可能用到的」类型，而不是 70 个全量。
[[nodiscard]] inline auto ui_prompt_for(const std::string &description) -> std::string {
    std::vector<std::string> types;
    auto remember = [&types](const std::string &t) -> void {
        if (std::find(types.begin(), types.end(), t) == types.end()) {
            types.push_back(t);
        }
    };

    // 描述里探测到的类型（generate_ui 已做关键词匹配）。
    if (const auto r = generate_ui(description); r.ok()) {
        const Json &children = r.value().value("node", Json::object()).value("children", Json::array());
        for (const Json &child : children) {
            if (child.contains("type")) {
                remember(child["type"].get<std::string>());
            }
        }
    }
    // 兜底：布局与文本几乎总是要用到。
    for (const std::string_view base : {"Stack", "Column", "Row", "Text"}) {
        remember(std::string(base));
    }

    std::sort(types.begin(), types.end());
    return build_ui_prompt(types);
}

// ═══════════════════════════════════════════════════════════════════════════
// 2) 生成 → 校验 → 修复 的自修复环
// ═══════════════════════════════════════════════════════════════════════════

/// @brief 外部 LLM 生成函数：`(prompt, 上一轮错误) -> UI 树 JSON`。
///
/// 约定：返回的对象形如 `{"node": {...}}` 或直接的节点对象；返回 null 表示放弃。
/// **本库从不自己调用它** —— 是否联网、用哪个模型，完全由注入方决定。
using GenerateUiFn = std::function<Json(const std::string &prompt, const std::vector<ValidationError> &errors)>;

/// @brief 自修复环中的一轮。
struct UiRepairStep {
    std::size_t attempt = 0;  ///< 轮次（从 0 起）
    Json generated;  ///< 本轮产出的树
    std::vector<ValidationError> errors;  ///< 校验结果（含 path / message / suggestion）
    bool machine_fixed = false;  ///< 是否由库侧**确定性**修好（未消耗 LLM 往返）

    [[nodiscard]] auto to_json() const -> Json {
        Json j = Json::object();
        j["attempt"] = attempt;
        j["generated"] = generated;
        j["machine_fixed"] = machine_fixed;
        Json errs = Json::array();
        for (const ValidationError &e : errors) {
            errs.push_back(e.to_json());
        }
        j["errors"] = errs;
        return j;
    }
};

/// @brief 自修复环的结果。
struct UiRepairResult {
    bool ok = false;  ///< 是否最终拿到一棵通过校验的树
    Json tree;  ///< 最终树（ok 为 false 时是最后一轮的产物）
    std::vector<UiRepairStep> history;  ///< 逐轮记录，供 AI 自省「为什么没修好」
    std::size_t attempts_used = 0;  ///< 实际用掉的轮次

    [[nodiscard]] auto to_json() const -> Json {
        Json j = Json::object();
        j["ok"] = ok;
        j["tree"] = tree;
        j["attempts_used"] = attempts_used;
        Json steps = Json::array();
        for (const UiRepairStep &s : history) {
            steps.push_back(s.to_json());
        }
        j["history"] = steps;
        return j;
    }
};

namespace detail {

/// @brief 已注册类型集合（小写 → 原名），供模糊匹配使用。
[[nodiscard]] inline auto ui_registered_types() -> const std::vector<std::string> & {
    static const std::vector<std::string> types = aurora::list_all_components();
    return types;
}

/// @brief 类型是否已注册。
///
/// ⚠️ 不能用 `describe_component(name).contains("type")` 判断 —— `component_schema` 会**无条件**写入
/// `w["type"] = name`，未注册类型同样返回一个带 `type` 的对象（只是没有 `default_props`）。
/// 只有拿注册表成员表来判才准。
[[nodiscard]] inline auto ui_is_registered(const std::string &type) -> bool {
    const std::vector<std::string> &types = ui_registered_types();
    return std::find(types.begin(), types.end(), type) != types.end();
}

/// @brief 大小写不敏感的**最佳**类型名修正：先精确（忽略大小写），再取唯一的子串命中。
/// 多个子串命中时返回空串 —— 宁可交给 LLM 重来，也不瞎猜。
[[nodiscard]] inline auto ui_resolve_type(const std::string &name) -> std::string {
    auto lower = [](std::string s) -> std::string {
        for (char &c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    const std::string needle = lower(name);
    for (const std::string &t : ui_registered_types()) {
        if (lower(t) == needle) {
            return t;
        }
    }
    std::string found;
    std::size_t count = 0;
    for (const std::string &t : ui_registered_types()) {
        if (lower(t).find(needle) != std::string::npos) {
            found = t;
            ++count;
        }
    }
    return count == 1 ? found : std::string{};
}

/// @brief 递归机修：修正未知类型、补齐缺省属性、按 children 策略裁剪子节点。
[[nodiscard]] inline auto ui_repair_node(const Json &node) -> Json {
    if (!node.is_object()) {
        return node;
    }
    Json out = node;

    // 1) 未知类型 → 尝试确定性修正
    if (out.contains("type") && out["type"].is_string()) {
        const std::string type = out["type"].get<std::string>();
        bool known = false;
        for (const std::string &t : ui_registered_types()) {
            if (t == type) {
                known = true;
                break;
            }
        }
        if (!known) {
            const std::string fixed = ui_resolve_type(type);
            if (!fixed.empty()) {
                out["type"] = fixed;
            }
        }
    }

    if (out.contains("type") && out["type"].is_string()) {
        const Json schema = aurora::describe_component(out["type"].get<std::string>());

        // 2) 补齐**必填**属性。值取自 default_props（实测序列化结果），类型保真度最高。
        //
        // ⚠️ 刻意**不**补齐全部缺省属性：validator 按 `PropDescriptor` 的声明类型判值，而缺省属性里
        // 有 `color` 这类以数组承载 Color 的项，与 validator 对 "Color" 的宽松判定（string/object）
        // 不兼容 —— 无脑回填会把一棵本来合法的树改成一堆类型错误（实测踩到）。
        // 校验只关心必填项，故只补必填项。
        const Json defaults = schema.value("default_props", Json::object());
        const Json descriptors = schema.value("prop_descriptors", Json::array());
        for (const Json &pd : descriptors) {
            if (!pd.value("required", false)) {
                continue;
            }
            const std::string name = pd.value("name", std::string{});
            if (name.empty()) {
                continue;
            }
            if (!out.contains("props") || !out["props"].is_object()) {
                out["props"] = Json::object();
            }
            if (!out["props"].contains(name) && defaults.contains(name)) {
                out["props"][name] = defaults[name];
            }
        }

        // 3) children 策略：声明 none 却带了子节点 → 丢弃（树本身非法，留着只会继续报错）。
        if (schema.value("children_policy", std::string("none")) == "none") {
            if (out.contains("children") && out["children"].is_array() && !out["children"].empty()) {
                out.erase("children");
            }
        }
    }

    // 4) 递归子节点
    if (out.contains("children") && out["children"].is_array()) {
        Json kids = Json::array();
        for (const Json &child : out["children"]) {
            kids.push_back(ui_repair_node(child));
        }
        out["children"] = kids;
    }
    return out;
}

}  // namespace detail

/// @brief 确定性机修（specification/08-tooling.md §2.6）：**不调用 LLM**，把明显可修的问题修掉。
///
/// 覆盖三类：未知类型（模糊匹配到唯一已注册类型）、缺失属性（按 `default_props` 回填）、
/// children 策略违规（声明 none 却带子节点则丢弃）。修不了的（如结构错误、类型彻底无法辨认）
/// 原样保留，交给 `generate_ui_repair` 的重试环让 LLM 重来。
///
/// @return 修复后的树（总是返回合法 JSON；不代表一定通过 `validate_ui_tree`）
///
/// @note Thread: main-thread only（读注册表）
/// @note Side-effects: none
[[nodiscard]] inline auto repair_ui_tree(const Json &tree) -> Json { return detail::ui_repair_node(tree); }

/// @brief NL→UI 的自修复环（specification/08-tooling.md §2.6）。
///
/// 流程（每轮）：机修 → 校验 → 通过即止；否则若注入了 LLM，把**错误列表**拼进 prompt 再生成一轮。
/// 机修优先的意义：能确定性修好的不必浪费一次 LLM 往返（既省钱也降低抖动）。
///
/// @param description   自然语言描述
/// @param llm           外部注入的生成函数；**为空时只用关键词生成 + 机修**，仍然自洽可测
/// @param max_attempts  轮次上限（>=1）
///
/// @note Thread: main-thread only
/// @note Side-effects: 调用注入的 `llm`（可能联网，由注入方承担）
[[nodiscard]] inline auto generate_ui_repair(const std::string &description, GenerateUiFn llm = {},
                                             std::size_t max_attempts = 3) -> UiRepairResult {
    UiRepairResult result;
    if (max_attempts == 0) {
        max_attempts = 1;
    }
    const std::string prompt = ui_prompt_for(description);

    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        UiRepairStep step;
        step.attempt = attempt;

        // ── 产出：有 LLM 且非首轮时带上上一轮的错误 ──
        if (llm != nullptr) {
            const std::vector<ValidationError> previous =
                attempt == 0 ? std::vector<ValidationError>{} : result.history.back().errors;
            step.generated = llm(prompt, previous);
        } else {
            const auto r = generate_ui(description);
            step.generated = r.ok() ? r.value().value("node", Json::object()) : Json::object();
        }
        if (step.generated.is_null() || step.generated.empty()) {
            step.errors = {};
            result.history.push_back(step);
            result.attempts_used = attempt + 1;
            return result;
        }

        // ── 机修（确定性，不耗 LLM）──
        const Json repaired = repair_ui_tree(step.generated);
        step.machine_fixed = (repaired != step.generated);
        step.generated = repaired;

        // ── 校验 ──
        step.errors = validate_ui_tree(repaired);
        result.history.push_back(step);
        result.attempts_used = attempt + 1;

        if (step.errors.empty()) {
            result.ok = true;
            result.tree = repaired;
            return result;
        }
        if (llm == nullptr) {
            // 无 LLM：机修已经尽力，再循环也只是重复同一结果。
            break;
        }
    }

    result.tree = result.history.empty() ? Json::object() : result.history.back().generated;
    return result;
}

}  // namespace aurora
