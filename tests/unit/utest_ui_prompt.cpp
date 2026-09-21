/// 测试类型: unit
/// 目标单元: include/aurora/app/ui_prompt.h
/// 测试说明: 覆盖 NL→UI 的两层闭环——schema→prompt 投影（体积可控、只带相关类型、
///           未知类型跳过），以及自修复环（确定性机修：未知类型修正 / 缺属性回填 / children 策略裁剪；
///           LLM 由外部注入，本库从不发起网络请求；无 LLM 时仍自洽且不空转）

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "aurora/app/ui_prompt.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_ui_prompt {

namespace {

/// 构造一个已知能通过校验的最小树（直接取自 generate_ui 的产出，避免与 Text 的必填属性漂移耦合）。
[[nodiscard]] auto known_good_tree() -> Json {
    const auto r = aurora::generate_ui("text");
    return r.ok() ? r.value()["node"] : Json::object();
}

}  // namespace

// ─────────────────────────── prompt 投影 ───────────────────────────

AURORA_TEST_CASE(prompt_for_unknown_types_only_is_empty) {
    // 未注册类型不应产出空条目 —— 宁可返回空串，也不给 LLM 一段没有类型的内容。
    const std::string prompt = aurora::build_ui_prompt({"DefinitelyNotAWidget"});
    AURORA_TEST_CHECK_TRUE(prompt.empty());
}

AURORA_TEST_CASE(prompt_carries_type_name_and_default_props) {
    const std::string prompt = aurora::build_ui_prompt({"Text"});
    AURORA_TEST_REQUIRE_FALSE(prompt.empty());
    AURORA_TEST_CHECK_NE(prompt.find("## Text"), std::string::npos);
    // 缺省值能让 LLM 少写冗余属性，故默认带出。
    AURORA_TEST_CHECK_NE(prompt.find("content"), std::string::npos);

    // 关掉缺省值后仍应保留类型本身。
    aurora::UiPromptOptions no_defaults;
    no_defaults.include_defaults = false;
    const std::string lean = aurora::build_ui_prompt({"Text"}, no_defaults);
    AURORA_TEST_CHECK_NE(lean.find("## Text"), std::string::npos);
    AURORA_TEST_CHECK_LE(lean.size(), prompt.size());
}

AURORA_TEST_CASE(prompt_states_the_output_shape_and_limits_types) {
    aurora::UiPromptOptions one;
    one.max_types = 1;
    const std::string limited = aurora::build_ui_prompt({"Text", "Button"}, one);
    // 上限生效：只应出现一个类型小节。
    AURORA_TEST_CHECK_EQ(limited.find("## Text") != std::string::npos, true);
    AURORA_TEST_CHECK_EQ(limited.find("## Button"), std::string::npos);

    // 输出形状的约定必须在 prompt 里写清楚，否则 LLM 容易返回不带 node 包装的结构。
    AURORA_TEST_CHECK_NE(limited.find("\"node\""), std::string::npos);
}

AURORA_TEST_CASE(prompt_for_description_narrows_types_and_includes_layout) {
    // 由描述收敛类型，并兜底带上几乎总要用的布局 / 文本类型。
    const std::string prompt = aurora::ui_prompt_for("a button");
    AURORA_TEST_REQUIRE_FALSE(prompt.empty());
    AURORA_TEST_CHECK_NE(prompt.find("## Button"), std::string::npos);
    AURORA_TEST_CHECK_NE(prompt.find("## Column"), std::string::npos);
    AURORA_TEST_CHECK_NE(prompt.find("## Text"), std::string::npos);
}

// ─────────────────────────── 确定性机修 ───────────────────────────

AURORA_TEST_CASE(repair_fixes_case_and_spelling_of_type_name) {
    // 类型名大小写写错是 LLM 最常见的错误，属于可确定性修好的一类。
    Json node = Json::object();
    node["type"] = "text";
    const Json fixed = aurora::repair_ui_tree(node);
    AURORA_TEST_CHECK_EQ(fixed["type"].get<std::string>(), std::string{"Text"});
}

AURORA_TEST_CASE(repair_adds_required_props_only_and_keeps_explicit_values) {
    Json node = Json::object();
    node["type"] = "Text";
    node["props"] = Json::object();
    node["props"]["content"] = "hi";
    const Json fixed = aurora::repair_ui_tree(node);

    // 显式给的值不得被覆盖。
    AURORA_TEST_CHECK_EQ(fixed["props"]["content"].get<std::string>(), std::string{"hi"});

    // 只允许补**必填**项：无脑补全部缺省属性会把 `color`（数组形态）这类与 validator 声明类型
    // 不兼容的值塞进树里，反而把原本合法的树改坏（实测踩到的坑）。
    const Json schema = aurora::describe_component("Text");
    std::set<std::string> required;
    for (const Json &pd : schema["prop_descriptors"]) {
        if (pd.value("required", false)) {
            required.insert(pd.value("name", std::string{}));
        }
    }
    AURORA_TEST_CHECK_GT(required.size(), std::size_t{0});  // 否则下面的循环是空转
    for (auto kv = fixed["props"].begin(); kv != fixed["props"].end(); ++kv) {
        AURORA_TEST_CHECK_TRUE(required.contains(kv.key()));
    }
}

AURORA_TEST_CASE(repair_result_of_a_bare_node_passes_validation) {
    // 机修的唯一目的就是让校验通过 —— 直接断言这个终态，比断言中间补了哪些键更稳。
    Json node = Json::object();
    node["type"] = "Text";
    const Json fixed = aurora::repair_ui_tree(node);
    const std::vector<aurora::ValidationError> errors = aurora::validate_ui_tree(fixed);
    AURORA_TEST_CHECK_MSG(errors.empty(), errors.empty() ? "" : errors[0].message);
}

AURORA_TEST_CASE(repair_drops_children_when_policy_is_none) {
    // 叶子控件带了子节点 → 树非法；留着只会继续报错，故直接丢弃。
    Json child = Json::object();
    child["type"] = "Text";
    Json node = Json::object();
    node["type"] = "Text";
    node["props"] = Json::object();
    node["props"]["content"] = "hi";
    node["children"] = Json::array({child});

    const Json fixed = aurora::repair_ui_tree(node);
    AURORA_TEST_CHECK_FALSE(fixed.contains("children"));
}

AURORA_TEST_CASE(repair_leaves_unresolvable_types_alone) {
    // 无法辨认的类型**不猜**：宁可留给 LLM 重来，也不静默换成一个不相干的控件。
    Json node = Json::object();
    node["type"] = "ZzzNotAWidget";
    const Json fixed = aurora::repair_ui_tree(node);
    AURORA_TEST_CHECK_EQ(fixed["type"].get<std::string>(), std::string{"ZzzNotAWidget"});
}

// ─────────────────────────── 自修复环 ───────────────────────────

AURORA_TEST_CASE(repair_loop_without_llm_stops_after_one_attempt) {
    // 未注入 LLM 时，机修已经尽力 —— 再循环也只是重复同一结果，不该空转。
    const aurora::UiRepairResult r = aurora::generate_ui_repair("text");
    AURORA_TEST_CHECK_EQ(r.attempts_used, std::size_t{1});
    AURORA_TEST_CHECK_EQ(r.history.size(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(r.ok);  // 关键词生成 + 机修即可通过校验
}

AURORA_TEST_CASE(repair_loop_retries_with_errors_when_llm_is_injected) {
    std::size_t calls = 0;
    std::size_t errors_seen_on_second_call = 0;

    const aurora::GenerateUiFn llm = [&](const std::string &prompt,
                                         const std::vector<aurora::ValidationError> &errors) -> Json {
        ++calls;
        AURORA_TEST_CHECK_FALSE(prompt.empty());
        if (calls == 1) {
            return Json{{"type", "ZzzNotAWidget"}};  // 第一轮故意给一棵修不好的树
        }
        errors_seen_on_second_call = errors.size();
        return known_good_tree();
    };

    const aurora::UiRepairResult r = aurora::generate_ui_repair("anything", llm, 3);
    AURORA_TEST_CHECK_TRUE(r.ok);
    AURORA_TEST_CHECK_EQ(calls, std::size_t{2});
    AURORA_TEST_CHECK_EQ(r.attempts_used, std::size_t{2});
    // 第二轮必须带上第一轮的错误 —— 否则 LLM 无从知道上次错在哪。
    AURORA_TEST_CHECK_GT(errors_seen_on_second_call, std::size_t{0});
    AURORA_TEST_CHECK_TRUE(!r.history[0].errors.empty());
}

AURORA_TEST_CASE(repair_loop_machine_fix_avoids_a_second_llm_round_trip) {
    // 机修优先：能确定性修好的，不浪费一次 LLM 往返。
    std::size_t calls = 0;
    const aurora::GenerateUiFn llm = [&](const std::string &, const std::vector<aurora::ValidationError> &) -> Json {
        ++calls;
        Json node = Json::object();
        node["type"] = "text";  // 大小写写错 —— 机修可确定性修好
        node["props"] = Json::object();
        node["props"]["content"] = "hello";
        return node;
    };

    const aurora::UiRepairResult r = aurora::generate_ui_repair("anything", llm, 3);
    AURORA_TEST_CHECK_EQ(calls, std::size_t{1});
    AURORA_TEST_CHECK_TRUE(r.history[0].machine_fixed);
    AURORA_TEST_CHECK_TRUE(r.ok);
}

AURORA_TEST_CASE(repair_result_serializes_for_inspection) {
    const aurora::UiRepairResult r = aurora::generate_ui_repair("text");
    const Json j = r.to_json();
    AURORA_TEST_CHECK_TRUE(j.contains("ok"));
    AURORA_TEST_CHECK_TRUE(j.contains("tree"));
    AURORA_TEST_CHECK_TRUE(j["history"].is_array());
    AURORA_TEST_REQUIRE_FALSE(j["history"].empty());
    // 逐轮记录是给人/AI 自省「为什么没修好」用的，故必须带错误明细。
    AURORA_TEST_CHECK_TRUE(j["history"][0].contains("errors"));
    AURORA_TEST_CHECK_TRUE(j["history"][0].contains("machine_fixed"));
}

}  // namespace aurora::test_cases::utest_ui_prompt
