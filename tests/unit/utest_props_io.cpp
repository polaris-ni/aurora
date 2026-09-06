/// 测试类型: unit
/// 目标单元: include/aurora/aurora.h
/// 测试说明: props_io 单元测试
///

// props_io 属性读写/序列化键 1:1 测试：Length/Color/EdgeInsets/枚举往返。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

#include <string>

#include "aurora/aurora.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_props_io {



static void test_length_roundtrip() {
    // wrap / expand / fixed / ratio
    const Json w = length_to_json(Length::wrap());
    AURORA_TEST_CHECK_MSG(w == "auto" && json_to_length(w).kind == LengthKind::WrapContent, "props_io: wrap roundtrip");
    const Json f = length_to_json(Length::expand());
    AURORA_TEST_CHECK_MSG(f == "fill" && json_to_length(f).kind == LengthKind::Expand, "props_io: expand roundtrip");

    constexpr Length fixed = Length::fixed(12.5F);
    Json fj = length_to_json(fixed);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(fj.is_array() && fj[0] == "px" && near_f(fj[1].get<float>(), 12.5F),
                          "props_io: fixed -> [px,12.5]");
    const Length back = json_to_length(fj);
    AURORA_TEST_CHECK_MSG(back.kind == LengthKind::Fixed && near_f(back.value, 12.5F), "props_io: fixed roundtrip");

    constexpr Length ratio = Length::ratio(0.5F);
    Json rj = length_to_json(ratio);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(rj.is_array() && rj[0] == "percent" && near_f(rj[1].get<float>(), 0.5F),
                          "props_io: ratio -> [percent,0.5]");
    AURORA_TEST_CHECK_MSG(json_to_length(rj).kind == LengthKind::Fraction && near_f(json_to_length(rj).value, 0.5F),
                          "props_io: ratio roundtrip");

    // 未知格式回退 wrap
    AURORA_TEST_CHECK_MSG(json_to_length(Json(42)).kind == LengthKind::WrapContent, "props_io: unknown length -> wrap");
}

static void test_color_roundtrip() {
    constexpr Color c{1, 2, 3, 4};
    Json j = color_to_json(c);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j.is_array() && j[0] == 1 && j[1] == 2 && j[2] == 3 && j[3] == 4,
                          "props_io: color -> [1,2,3,4]");
    const Color back = json_to_color(j);
    AURORA_TEST_CHECK_MSG(back.r == 1 && back.g == 2 && back.b == 3 && back.a == 4,
                          "props_io: color roundtrip");
    // 格式不符回退默认色（RGB 归零；alpha 沿用 Color 默认值）。
    const Color def = json_to_color(Json("nope"));
    AURORA_TEST_CHECK_MSG(def.r == 0 && def.g == 0 && def.b == 0, "props_io: bad color -> rgb zero");
}

static void test_edge_insets_roundtrip() {
    constexpr EdgeInsets e{.left = 1.0F, .top = 2.0F, .right = 3.0F, .bottom = 4.0F};
    Json j = edge_insets_to_json(e);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(j["left"].get<float>(), 1.0F) && near_f(j["top"].get<float>(), 2.0F) &&
                              // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                              // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                              near_f(j["right"].get<float>(), 3.0F) && near_f(j["bottom"].get<float>(), 4.0F),
                          "props_io: edge_insets -> object");
    auto [left, top, right, bottom] = json_to_edge_insets(j);
    AURORA_TEST_CHECK_MSG(near_f(left, 1.0F) && near_f(top, 2.0F) && near_f(right, 3.0F) && near_f(bottom, 4.0F),
                          "props_io: edge_insets roundtrip");
    // 缺字段回退 0
    AURORA_TEST_CHECK_MSG(json_to_edge_insets(Json::object()).left == 0.0F, "props_io: empty edge_insets -> 0");
}

static void test_enum_roundtrip() {
    // TextAlign
    AURORA_TEST_CHECK_MSG(json_to_text_align(text_align_to_json(TextAlign::Center)) == TextAlign::Center,
                          "props_io: TextAlign roundtrip");
    AURORA_TEST_CHECK_MSG(json_to_text_align(Json("Nope")) == TextAlign::Left, "props_io: bad TextAlign -> Left");
    // TextOverflow
    AURORA_TEST_CHECK_MSG(
        json_to_text_overflow(text_overflow_to_json(TextOverflow::Ellipsis)) == TextOverflow::Ellipsis,
        "props_io: TextOverflow roundtrip");
    // FontWeight（数值字符串）
    AURORA_TEST_CHECK_MSG(json_to_font_weight(font_weight_to_json(FontWeight::Bold)) == FontWeight::Bold,
                          "props_io: FontWeight roundtrip");
    // MainAxisSize
    AURORA_TEST_CHECK_MSG(json_to_main_axis_size(main_axis_size_to_json(MainAxisSize::Max)) == MainAxisSize::Max,
                          "props_io: MainAxisSize roundtrip");
    AURORA_TEST_CHECK_MSG(json_to_main_axis_size(Json("Nope")) == MainAxisSize::Min,
                          "props_io: bad MainAxisSize -> Min");
    // MainAxisAlignment
    AURORA_TEST_CHECK_MSG(json_to_main_axis_alignment(main_axis_alignment_to_json(MainAxisAlignment::SpaceBetween)) ==
                              MainAxisAlignment::SpaceBetween,
                          "props_io: MainAxisAlignment roundtrip");
    // CrossAxisAlignment
    AURORA_TEST_CHECK_MSG(json_to_cross_axis_alignment(cross_axis_alignment_to_json(CrossAxisAlignment::Stretch)) ==
                              CrossAxisAlignment::Stretch,
                          "props_io: CrossAxisAlignment roundtrip");
    // StackFit
    AURORA_TEST_CHECK_MSG(json_to_stack_fit(stack_fit_to_json(StackFit::Expand)) == StackFit::Expand,
                          "props_io: StackFit roundtrip");
    // BoxFit
    AURORA_TEST_CHECK_MSG(json_to_box_fit(box_fit_to_json(BoxFit::Contain)) == BoxFit::Contain,
                          "props_io: BoxFit roundtrip");
}

static void test_text_decoration_roundtrip() {
    constexpr TextDecoration dec = TextDecoration::Underline | TextDecoration::LineThrough;
    const Json j = text_decoration_to_json(dec);
    AURORA_TEST_CHECK_MSG(j.is_array() && j.size() == 2, "props_io: TextDecoration -> 2-item array");
    const TextDecoration back = json_to_text_decoration(j);
    AURORA_TEST_CHECK_MSG(
        decoration_has(back, TextDecoration::Underline) && decoration_has(back, TextDecoration::LineThrough),
        "props_io: TextDecoration roundtrip");
    AURORA_TEST_CHECK_MSG(json_to_text_decoration(Json("None")) == TextDecoration::None, "props_io: 'None' -> None");
    // 单字符串 / 未知项忽略（回退 None）
    AURORA_TEST_CHECK_MSG(json_to_text_decoration(Json("Underline")) == TextDecoration::Underline,
                          "props_io: single-string Underline");
    AURORA_TEST_CHECK_MSG(json_to_text_decoration(Json("Bogus")) == TextDecoration::None,
                           "props_io: unknown decoration -> None");
}

// 遍历所有枚举取值，覆盖各 to_json/from_json switch 分支（原测试仅各取一值）。
static void test_enum_exhaustive() {
    for (auto v : {TextAlign::Left, TextAlign::Right, TextAlign::Center, TextAlign::Start, TextAlign::End,
                   TextAlign::Justify}) {
        AURORA_TEST_CHECK_MSG(json_to_text_align(text_align_to_json(v)) == v, "props_io: TextAlign all");
    }
    for (auto v : {TextOverflow::Clip, TextOverflow::Ellipsis, TextOverflow::Fade}) {
        AURORA_TEST_CHECK_MSG(json_to_text_overflow(text_overflow_to_json(v)) == v, "props_io: TextOverflow all");
    }
    for (auto v : {FontWeight::Thin, FontWeight::ExtraLight, FontWeight::Light, FontWeight::Normal,
                   FontWeight::Medium, FontWeight::SemiBold, FontWeight::Bold, FontWeight::ExtraBold,
                   FontWeight::Black}) {
        AURORA_TEST_CHECK_MSG(json_to_font_weight(font_weight_to_json(v)) == v, "props_io: FontWeight all");
    }
    // 非法数值串（如 "bold"）此前裸 stoi 会抛异常致崩溃；现回退 Normal
    AURORA_TEST_CHECK_MSG(json_to_font_weight(Json("bold")) == FontWeight::Normal,
                          "props_io: FontWeight bad string -> Normal");
    // 数值通道（Inspector PUT / 文件加载）
    AURORA_TEST_CHECK_MSG(json_to_font_weight(Json(700)) == FontWeight::Bold, "props_io: FontWeight number -> Bold");
    AURORA_TEST_CHECK_MSG(json_to_font_weight(Json(350)) == FontWeight::Normal,
                          "props_io: FontWeight unknown number -> Normal");
    for (auto v : {FontStyle::Normal, FontStyle::Italic}) {
        AURORA_TEST_CHECK_MSG(json_to_font_style(font_style_to_json(v)) == v, "props_io: FontStyle all");
    }
    for (auto v : {MainAxisSize::Min, MainAxisSize::Max}) {
        AURORA_TEST_CHECK_MSG(json_to_main_axis_size(main_axis_size_to_json(v)) == v, "props_io: MainAxisSize all");
    }
    for (auto v : {MainAxisAlignment::Start, MainAxisAlignment::Center, MainAxisAlignment::End,
                   MainAxisAlignment::SpaceBetween, MainAxisAlignment::SpaceAround,
                   MainAxisAlignment::SpaceEvenly}) {
        AURORA_TEST_CHECK_MSG(json_to_main_axis_alignment(main_axis_alignment_to_json(v)) == v,
                              "props_io: MainAxisAlignment all");
    }
    for (auto v : {CrossAxisAlignment::Start, CrossAxisAlignment::Center, CrossAxisAlignment::End,
                   CrossAxisAlignment::Stretch}) {
        AURORA_TEST_CHECK_MSG(json_to_cross_axis_alignment(cross_axis_alignment_to_json(v)) == v,
                              "props_io: CrossAxisAlignment all");
    }
    for (auto v : {StackFit::Loose, StackFit::Expand, StackFit::Passthrough}) {
        AURORA_TEST_CHECK_MSG(json_to_stack_fit(stack_fit_to_json(v)) == v, "props_io: StackFit all");
    }
    for (auto v : {BoxFit::Fill, BoxFit::Contain, BoxFit::Cover, BoxFit::FitWidth, BoxFit::FitHeight,
                   BoxFit::None, BoxFit::ScaleDown}) {
        AURORA_TEST_CHECK_MSG(json_to_box_fit(box_fit_to_json(v)) == v, "props_io: BoxFit all");
    }
    for (auto v : {OverflowStrategy::Visible, OverflowStrategy::Hidden, OverflowStrategy::Clip,
                   OverflowStrategy::Scroll}) {
        AURORA_TEST_CHECK_MSG(json_to_overflow_strategy(overflow_strategy_to_json(v)) == v,
                              "props_io: OverflowStrategy all");
    }
    // to_json 字符串形态抽查
    AURORA_TEST_CHECK_MSG(text_align_to_json(TextAlign::Justify) == "Justify", "props_io: TextAlign to_json str");
    AURORA_TEST_CHECK_MSG(box_fit_to_json(BoxFit::ScaleDown) == "ScaleDown", "props_io: BoxFit to_json str");
    AURORA_TEST_CHECK_MSG(font_style_to_json(FontStyle::Italic) == "Italic", "props_io: FontStyle to_json str");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_props_io ===\n");
    test_length_roundtrip();
    test_color_roundtrip();
    test_edge_insets_roundtrip();
    test_enum_roundtrip();
    test_text_decoration_roundtrip();
    test_enum_exhaustive();
}

}  // namespace aurora::test_cases::utest_props_io