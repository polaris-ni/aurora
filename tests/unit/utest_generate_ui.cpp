/// 测试类型: unit
/// 目标单元: include/aurora/app/generate_ui.h
/// 测试说明: 覆盖 NL→UI 关键词生成——空描述错误码、关键词→类型映射（覆盖面由注册表派生，不再硬编码 8 项）、
/// **文案属性必须写控件真实读的键**（旧版写 props.text，而没有任何控件读它 —— 生成的树看着有文案实际为空）、
/// 无匹配回退 Text（含 20 字符截断）、Stack 包裹结构、validate_generate_ui 的 from_json 往返

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "aurora/app/generate_ui.h"
#include "aurora/widget/serialization.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_generate_ui {

namespace {

[[nodiscard]] auto lower(std::string s) -> std::string {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

[[nodiscard]] auto is_alnum_only(const std::string &s) -> bool {
    return std::ranges::all_of(s, [](char c) -> bool {
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    });
}

/// 生成的树恒为 Stack 包裹，取子节点数组便于断言。
[[nodiscard]] auto children_of(const Json &tree) -> const Json & {
    return tree["node"]["children"];
}

}  // namespace

AURORA_TEST_CASE(empty_description_yields_structured_error) {
    const auto r = generate_ui("");
    AURORA_TEST_REQUIRE_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, aurora::ErrorCode::GenerateUiEmpty);
    AURORA_TEST_CHECK_FALSE(validate_generate_ui(""));
}

AURORA_TEST_CASE(text_uses_the_prop_key_widgets_actually_read) {
    // 关键回归：`Text` 读的是 `content`，不是 `text`。旧版硬编码写 props.text，
    // 生成的树反序列化后文案是空的——看起来成功、实际丢内容。
    const auto r = generate_ui("text");
    AURORA_TEST_REQUIRE_TRUE(r.ok());

    const Json &node = children_of(r.value())[0];
    AURORA_TEST_CHECK_EQ(node["type"].get<std::string>(), std::string{"Text"});
    AURORA_TEST_CHECK_TRUE(node["props"].contains("content"));
    AURORA_TEST_CHECK_FALSE(node["props"].contains("text"));
    AURORA_TEST_CHECK_EQ(node["props"]["content"].get<std::string>(), std::string{"Text"});
}

AURORA_TEST_CASE(button_text_prop_is_label_not_text) {
    // 同上：Button 的文案键是 `label`。
    const auto r = generate_ui("button");
    AURORA_TEST_REQUIRE_TRUE(r.ok());

    const Json &node = children_of(r.value())[0];
    AURORA_TEST_CHECK_EQ(node["type"].get<std::string>(), std::string{"Button"});
    AURORA_TEST_CHECK_FALSE(node["props"].contains("text"));
    AURORA_TEST_CHECK_TRUE(node["props"].contains("label"));
    AURORA_TEST_CHECK_EQ(node["props"]["label"].get<std::string>(), std::string{"Button"});
}

AURORA_TEST_CASE(tree_is_wrapped_in_stack_with_object_props) {
    const auto r = generate_ui("button");
    AURORA_TEST_REQUIRE_TRUE(r.ok());

    const Json &node = r.value()["node"];
    AURORA_TEST_CHECK_EQ(node["type"].get<std::string>(), std::string{"Stack"});
    AURORA_TEST_CHECK_TRUE(node["props"].is_object());
    AURORA_TEST_CHECK_TRUE(node["children"].is_array());
}

AURORA_TEST_CASE(coverage_is_derived_from_the_component_registry) {
    // 覆盖面不再硬编码 8 个关键词，而是由 `list_all_components()` 派生：
    // 任一「名字只含字母数字」的已注册类型，都能被它自己的名字（小写）命中。
    const std::vector<std::string> types = aurora::list_all_components();
    std::size_t checked = 0;

    for (const std::string &type : types) {
        if (!is_alnum_only(type)) {
            continue;  // 模板实例名（含 <> 之类）不作整词匹配的对象
        }
        const auto r = generate_ui(lower(type));
        AURORA_TEST_REQUIRE_TRUE(r.ok());
        const Json &kids = children_of(r.value());
        AURORA_TEST_REQUIRE_GE(kids.size(), std::size_t{1});
        AURORA_TEST_CHECK_EQ(kids[0]["type"].get<std::string>(), type);
        ++checked;
    }
    // 注册表若为空则说明核心控件未注册，这条覆盖断言本身失焦。
    AURORA_TEST_CHECK_GT(checked, std::size_t{10});
}

AURORA_TEST_CASE(multiple_keywords_produce_one_node_each_without_duplicates) {
    // 同一类型被多个词命中时（"text" 与别名 "label"）**不重复产出** —— 早期版本会产出两个相同的 Text。
    const auto r = generate_ui("text with label");
    AURORA_TEST_REQUIRE_TRUE(r.ok());

    const Json &kids = children_of(r.value());
    AURORA_TEST_CHECK_EQ(kids.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(kids[0]["type"].get<std::string>(), std::string{"Text"});
}

AURORA_TEST_CASE(unmatched_description_falls_back_to_text) {
    // 无关键词 → 回退 Text，文案为 "?" + 描述前 20 字符。
    const auto r = generate_ui("zzz");
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Json &fallback = children_of(r.value())[0];
    AURORA_TEST_CHECK_EQ(fallback["type"].get<std::string>(), std::string{"Text"});
    AURORA_TEST_CHECK_EQ(fallback["props"]["content"].get<std::string>(), std::string{"?zzz"});

    // 超长描述截断到 20 字符。
    const auto long_r = generate_ui(std::string(25, 'x'));
    AURORA_TEST_REQUIRE_TRUE(long_r.ok());
    const Json &long_fallback = children_of(long_r.value())[0];
    AURORA_TEST_CHECK_EQ(long_fallback["props"]["content"].get<std::string>(), "?" + std::string(20, 'x'));
}

AURORA_TEST_CASE(validate_generate_ui_roundtrip) {
    // 生成结果应能通过 from_json 往返（schema 匹配）。
    AURORA_TEST_CHECK_TRUE(validate_generate_ui("a button"));
    AURORA_TEST_CHECK_TRUE(validate_generate_ui("column row checkbox switch"));
    AURORA_TEST_CHECK_TRUE(validate_generate_ui("zzz unknown input"));
    // 空描述在生成期即失败。
    AURORA_TEST_CHECK_FALSE(validate_generate_ui(""));
}

}  // namespace aurora::test_cases::utest_generate_ui
