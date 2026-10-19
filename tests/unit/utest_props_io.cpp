/// 测试类型: unit
/// 目标单元: include/aurora/widget/props_io.h
/// 测试说明: 覆盖属性值 <-> JSON 双向序列化自由函数：Length / Color / EdgeInsets / 共享枚举
/// （TextAlign·TextDirection·TextOverflow·FontWeight·FontStyle·TextDecoration·MainAxisSize·
/// ScrollSnapAlignment·MainAxisAlignment·CrossAxisAlignment·StackFit·BoxFit·OverflowStrategy）。
/// 每对函数均验：正向编码、反向解析往返、未知/畸形输入回退默认。
/// 判据：to_json(X) 的输出经 json_to_X 解析还原为 X；未知名字符串/数组/非对象输入各自回退到规范默认。

#include "aurora/widget/props_io.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_props_io {

AURORA_TEST_CASE(length_roundtrip_and_fallback) {
    // 内容自适应 / 撑满 -> 字符串；固定 / 比例 -> ["px"|"percent", 数值]。
    AURORA_TEST_CHECK_EQ(length_to_json(Length::wrap()).as_or<std::string>(""), "auto");
    AURORA_TEST_CHECK_EQ(length_to_json(Length::expand()).as_or<std::string>(""), "fill");
    const Json fixed = length_to_json(Length::fixed(10.0F));
    AURORA_TEST_CHECK(fixed.is_array() && fixed.size() == 2);
    AURORA_TEST_CHECK_EQ(fixed.at(0)->as_or<std::string>(""), "px");
    AURORA_TEST_CHECK(std::abs(fixed.at(1)->as_or<float>(0.0F) - 10.0F) < 1e-3F);
    const Json ratio = length_to_json(Length::ratio(0.5F));
    AURORA_TEST_CHECK_EQ(ratio.at(0)->as_or<std::string>(""), "percent");
    AURORA_TEST_CHECK(std::abs(ratio.at(1)->as_or<float>(0.0F) - 0.5F) < 1e-3F);

    // 反向解析：已知串/数组还原；未知字符串与畸形数组回退 wrap。
    AURORA_TEST_CHECK_EQ(json_to_length(Json{"auto"}).kind, LengthKind::WrapContent);
    AURORA_TEST_CHECK_EQ(json_to_length(Json{"fill"}).kind, LengthKind::Expand);
    AURORA_TEST_CHECK_EQ(json_to_length(fixed).kind, LengthKind::Fixed);
    AURORA_TEST_CHECK(std::abs(json_to_length(fixed).value - 10.0F) < 1e-3F);
    AURORA_TEST_CHECK_EQ(json_to_length(ratio).kind, LengthKind::Fraction);
    AURORA_TEST_CHECK_EQ(json_to_length(Json{"bogus"}).kind, LengthKind::WrapContent);
    AURORA_TEST_CHECK_EQ(json_to_length(Json::object()).kind, LengthKind::WrapContent);
}

AURORA_TEST_CASE(color_roundtrip_and_fallback) {
    const Color c{10, 20, 30, 40};
    const Json j = color_to_json(c);
    AURORA_TEST_CHECK(j.is_array() && j.size() >= 4);
    AURORA_TEST_CHECK_EQ(j.at(0)->as_or<std::int32_t>(0), 10);
    AURORA_TEST_CHECK_EQ(j.at(1)->as_or<std::int32_t>(0), 20);
    AURORA_TEST_CHECK_EQ(j.at(2)->as_or<std::int32_t>(0), 30);
    AURORA_TEST_CHECK_EQ(j.at(3)->as_or<std::int32_t>(0), 40);
    const Color back = json_to_color(j);
    AURORA_TEST_CHECK_EQ(back.r, static_cast<std::uint8_t>(10));
    AURORA_TEST_CHECK_EQ(back.g, static_cast<std::uint8_t>(20));
    AURORA_TEST_CHECK_EQ(back.b, static_cast<std::uint8_t>(30));
    AURORA_TEST_CHECK_EQ(back.a, static_cast<std::uint8_t>(40));
    // 非数组输入回退黑色。
    AURORA_TEST_CHECK_EQ(json_to_color(Json{"red"}).r, static_cast<std::uint8_t>(0));
    AURORA_TEST_CHECK_EQ(json_to_color(Json{"red"}).a, static_cast<std::uint8_t>(255));
}

AURORA_TEST_CASE(edge_insets_roundtrip_and_partial) {
    const EdgeInsets e{.left = 1.0F, .top = 2.0F, .right = 3.0F, .bottom = 4.0F};
    const Json j = edge_insets_to_json(e);
    AURORA_TEST_CHECK_EQ(j.at("left")->as_or<float>(0.0F), 1.0F);
    AURORA_TEST_CHECK_EQ(j.at("top")->as_or<float>(0.0F), 2.0F);
    AURORA_TEST_CHECK_EQ(j.at("right")->as_or<float>(0.0F), 3.0F);
    AURORA_TEST_CHECK_EQ(j.at("bottom")->as_or<float>(0.0F), 4.0F);
    const EdgeInsets back = json_to_edge_insets(j);
    AURORA_TEST_CHECK_EQ(back.left, 1.0F);
    AURORA_TEST_CHECK_EQ(back.bottom, 4.0F);
    // 非对象输入整体视为缺省（全 0）。
    const EdgeInsets def = json_to_edge_insets(Json::array());
    AURORA_TEST_CHECK_EQ(def.left, 0.0F);
    AURORA_TEST_CHECK_EQ(def.right, 0.0F);
}

AURORA_TEST_CASE(text_align_direction_overflow_roundtrip) {
    AURORA_TEST_CHECK_EQ(text_align_to_json(TextAlign::Center).as_or<std::string>(""), "Center");
    AURORA_TEST_CHECK_EQ(json_to_text_align(Json{"Center"}), TextAlign::Center);
    AURORA_TEST_CHECK_EQ(json_to_text_align(Json{"Nope"}), TextAlign::Left);

    AURORA_TEST_CHECK_EQ(text_direction_to_json(TextDirection::RTL).as_or<std::string>(""), "RTL");
    AURORA_TEST_CHECK_EQ(json_to_text_direction(Json{"RTL"}), TextDirection::RTL);
    AURORA_TEST_CHECK_EQ(json_to_text_direction(Json{"LTR"}), TextDirection::LTR);

    AURORA_TEST_CHECK_EQ(text_overflow_to_json(TextOverflow::Ellipsis).as_or<std::string>(""), "Ellipsis");
    AURORA_TEST_CHECK_EQ(json_to_text_overflow(Json{"Fade"}), TextOverflow::Fade);
    AURORA_TEST_CHECK_EQ(json_to_text_overflow(Json{"??"}), TextOverflow::Clip);
}

AURORA_TEST_CASE(font_weight_handles_string_number_and_invalid) {
    AURORA_TEST_CHECK_EQ(font_weight_to_json(FontWeight::Bold).as_or<std::string>(""), "700");
    AURORA_TEST_CHECK_EQ(json_to_font_weight(Json{"700"}), FontWeight::Bold);
    AURORA_TEST_CHECK_EQ(json_to_font_weight(Json{700}), FontWeight::Bold);
    AURORA_TEST_CHECK_EQ(json_to_font_weight(Json{"400"}), FontWeight::Normal);
    // 不可信通道的非法字重串（如 "bold"）此前会抛 invalid_argument 致崩，现回退 Normal。
    AURORA_TEST_CHECK_EQ(json_to_font_weight(Json{"bold"}), FontWeight::Normal);
    AURORA_TEST_CHECK_EQ(json_to_font_weight(Json{"abc"}), FontWeight::Normal);
}

AURORA_TEST_CASE(font_style_and_decoration_roundtrip) {
    AURORA_TEST_CHECK_EQ(font_style_to_json(FontStyle::Italic).as_or<std::string>(""), "Italic");
    AURORA_TEST_CHECK_EQ(json_to_font_style(Json{"Italic"}), FontStyle::Italic);
    AURORA_TEST_CHECK_EQ(json_to_font_style(Json{"Normal"}), FontStyle::Normal);

    const TextDecoration deco = TextDecoration::Underline | TextDecoration::Overline;
    const Json dj = text_decoration_to_json(deco);
    AURORA_TEST_CHECK(dj.is_array());
    AURORA_TEST_CHECK_EQ(json_to_text_decoration(dj), deco);
    AURORA_TEST_CHECK_EQ(json_to_text_decoration(Json{"None"}), TextDecoration::None);
    AURORA_TEST_CHECK_EQ(json_to_text_decoration(Json{"Underline"}), TextDecoration::Underline);
    AURORA_TEST_CHECK_EQ(json_to_text_decoration(Json{"garbage"}), TextDecoration::None);
}

AURORA_TEST_CASE(axis_and_fit_enums_roundtrip) {
    AURORA_TEST_CHECK_EQ(main_axis_size_to_json(MainAxisSize::Max).as_or<std::string>(""), "Max");
    AURORA_TEST_CHECK_EQ(json_to_main_axis_size(Json{"Max"}), MainAxisSize::Max);
    AURORA_TEST_CHECK_EQ(json_to_main_axis_size(Json{"Min"}), MainAxisSize::Min);

    AURORA_TEST_CHECK_EQ(snap_alignment_to_json(ScrollSnapAlignment::Center).as_or<std::string>(""), "Center");
    AURORA_TEST_CHECK_EQ(json_to_snap_alignment(Json{"End"}), ScrollSnapAlignment::End);
    AURORA_TEST_CHECK_EQ(json_to_snap_alignment(Json{"?"}), ScrollSnapAlignment::Start);

    AURORA_TEST_CHECK_EQ(main_axis_alignment_to_json(MainAxisAlignment::SpaceEvenly).as_or<std::string>(""),
                         "SpaceEvenly");
    AURORA_TEST_CHECK_EQ(json_to_main_axis_alignment(Json{"SpaceAround"}), MainAxisAlignment::SpaceAround);
    AURORA_TEST_CHECK_EQ(json_to_main_axis_alignment(Json{"?"}), MainAxisAlignment::Start);

    AURORA_TEST_CHECK_EQ(cross_axis_alignment_to_json(CrossAxisAlignment::Baseline).as_or<std::string>(""), "Baseline");
    AURORA_TEST_CHECK_EQ(json_to_cross_axis_alignment(Json{"Stretch"}), CrossAxisAlignment::Stretch);
    AURORA_TEST_CHECK_EQ(json_to_cross_axis_alignment(Json{"?"}), CrossAxisAlignment::Start);

    AURORA_TEST_CHECK_EQ(stack_fit_to_json(StackFit::Passthrough).as_or<std::string>(""), "Passthrough");
    AURORA_TEST_CHECK_EQ(json_to_stack_fit(Json{"Expand"}), StackFit::Expand);
    AURORA_TEST_CHECK_EQ(json_to_stack_fit(Json{"?"}), StackFit::Loose);

    AURORA_TEST_CHECK_EQ(box_fit_to_json(BoxFit::Cover).as_or<std::string>(""), "Cover");
    AURORA_TEST_CHECK_EQ(json_to_box_fit(Json{"ScaleDown"}), BoxFit::ScaleDown);
    AURORA_TEST_CHECK_EQ(json_to_box_fit(Json{"?"}), BoxFit::Fill);

    AURORA_TEST_CHECK_EQ(overflow_strategy_to_json(OverflowStrategy::Scroll).as_or<std::string>(""), "Scroll");
    AURORA_TEST_CHECK_EQ(json_to_overflow_strategy(Json{"Hidden"}), OverflowStrategy::Hidden);
    AURORA_TEST_CHECK_EQ(json_to_overflow_strategy(Json{"?"}), OverflowStrategy::Visible);
}

// 各 json_to_* 的防御分支：非字符串（此处统一用数组）输入一律回退规范默认，不抛不崩。
// 这条同时守住「Inspector PUT / JSON 文件加载等不可信通道」不会因类型错位触发未定义行为。
AURORA_TEST_CASE(non_string_inputs_fall_back_to_default) {
    const Json not_a_string = Json::array();
    AURORA_TEST_CHECK_EQ(json_to_text_align(not_a_string), TextAlign::Left);
    AURORA_TEST_CHECK_EQ(json_to_text_direction(not_a_string), TextDirection::LTR);
    AURORA_TEST_CHECK_EQ(json_to_text_overflow(not_a_string), TextOverflow::Clip);
    AURORA_TEST_CHECK_EQ(json_to_font_style(not_a_string), FontStyle::Normal);
    AURORA_TEST_CHECK_EQ(json_to_text_decoration(not_a_string), TextDecoration::None);
    AURORA_TEST_CHECK_EQ(json_to_main_axis_size(not_a_string), MainAxisSize::Min);
    AURORA_TEST_CHECK_EQ(json_to_snap_alignment(not_a_string), ScrollSnapAlignment::Start);
    AURORA_TEST_CHECK_EQ(json_to_main_axis_alignment(not_a_string), MainAxisAlignment::Start);
    AURORA_TEST_CHECK_EQ(json_to_cross_axis_alignment(not_a_string), CrossAxisAlignment::Start);
    AURORA_TEST_CHECK_EQ(json_to_stack_fit(not_a_string), StackFit::Loose);
    AURORA_TEST_CHECK_EQ(json_to_box_fit(not_a_string), BoxFit::Fill);
    AURORA_TEST_CHECK_EQ(json_to_overflow_strategy(not_a_string), OverflowStrategy::Visible);
    // font_weight 对数组（非数字非串）同样回退 Normal，不进入 stoi 解析路径。
    AURORA_TEST_CHECK_EQ(json_to_font_weight(not_a_string), FontWeight::Normal);
}

}  // namespace aurora::test_cases::utest_props_io
