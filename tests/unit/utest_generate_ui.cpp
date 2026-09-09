/// 测试类型: unit
/// 目标单元: include/aurora/app/generate_ui.h
/// 测试说明: 覆盖 NL→UI 关键词生成——空描述错误码、关键词→类型映射与顺序、
/// Button 默认文案、无匹配回退 Text（含 20 字符截断）、Stack 包裹结构、
/// validate_generate_ui 的 from_json 往返放行/拦截

#include <string>

#include "aurora/app/generate_ui.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_generate_ui {

AURORA_TEST_CASE(empty_description_yields_structured_error) {
    const auto r = generate_ui("");
    AURORA_TEST_REQUIRE_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, aurora::ErrorCode::GenerateUiEmpty);
    AURORA_TEST_CHECK_FALSE(validate_generate_ui(""));
}

AURORA_TEST_CASE(keyword_mapping_produces_widgets_in_order) {
    // "button" 与 "slider" 均命中 → 按映射表顺序产出 [Button, Slider]。
    const auto r = generate_ui("a button and a slider");
    AURORA_TEST_REQUIRE_TRUE(r.ok());

    const auto &node = r.value()["node"];
    AURORA_TEST_CHECK_EQ(node["type"].get<std::string>(), std::string{"Stack"});
    AURORA_TEST_CHECK_TRUE(node["props"].is_object());
    AURORA_TEST_CHECK_TRUE(node["children"].is_array());
    AURORA_TEST_CHECK_EQ(node["children"].size(), std::size_t{2});

    const auto &button = node["children"][0];
    AURORA_TEST_CHECK_EQ(button["type"].get<std::string>(), std::string{"Button"});
    AURORA_TEST_CHECK_EQ(button["props"]["text"].get<std::string>(), std::string{"Button"});

    const auto &slider = node["children"][1];
    AURORA_TEST_CHECK_EQ(slider["type"].get<std::string>(), std::string{"Slider"});
    // 无默认文案的控件不写 props.text。
    AURORA_TEST_CHECK_FALSE(slider.contains("props"));
}

AURORA_TEST_CASE(text_and_label_both_map_to_text) {
    const auto r = generate_ui("text with label");
    AURORA_TEST_REQUIRE_TRUE(r.ok());

    const auto &children = r.value()["node"]["children"];
    AURORA_TEST_CHECK_EQ(children.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(children[0]["type"].get<std::string>(), std::string{"Text"});
    AURORA_TEST_CHECK_EQ(children[0]["props"]["text"].get<std::string>(), std::string{"Text"});
    AURORA_TEST_CHECK_EQ(children[1]["type"].get<std::string>(), std::string{"Text"});
    AURORA_TEST_CHECK_EQ(children[1]["props"]["text"].get<std::string>(), std::string{"Text"});
}

AURORA_TEST_CASE(container_keywords_map_to_containers) {
    const auto r = generate_ui("column with button");
    AURORA_TEST_REQUIRE_TRUE(r.ok());

    const auto &children = r.value()["node"]["children"];
    AURORA_TEST_CHECK_EQ(children.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(children[0]["type"].get<std::string>(), std::string{"Button"});
    AURORA_TEST_CHECK_EQ(children[1]["type"].get<std::string>(), std::string{"Column"});
}

AURORA_TEST_CASE(unmatched_description_falls_back_to_text) {
    // 无关键词 → 回退 Text，文案为 "?" + 描述前 20 字符。
    const auto r = generate_ui("zzz");
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const auto &fallback = r.value()["node"]["children"][0];
    AURORA_TEST_CHECK_EQ(fallback["type"].get<std::string>(), std::string{"Text"});
    AURORA_TEST_CHECK_EQ(fallback["props"]["text"].get<std::string>(), std::string{"?zzz"});

    // 超长描述截断到 20 字符。
    const auto long_r = generate_ui(std::string(25, 'x'));
    AURORA_TEST_REQUIRE_TRUE(long_r.ok());
    const auto &long_fallback = long_r.value()["node"]["children"][0];
    AURORA_TEST_CHECK_EQ(long_fallback["props"]["text"].get<std::string>(), "?" + std::string(20, 'x'));
}

AURORA_TEST_CASE(validate_generate_ui_roundtrip) {
    // 关键词命中与回退路径均应能通过 from_json 往返（schema 匹配）。
    AURORA_TEST_CHECK_TRUE(validate_generate_ui("a button"));
    AURORA_TEST_CHECK_TRUE(validate_generate_ui("column row checkbox switch"));
    AURORA_TEST_CHECK_TRUE(validate_generate_ui("zzz unknown input"));
    // 空描述在生成期即失败。
    AURORA_TEST_CHECK_FALSE(validate_generate_ui(""));
}

}  // namespace aurora::test_cases::utest_generate_ui
