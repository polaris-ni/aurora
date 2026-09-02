// enums_test.cpp — 覆盖 core/enums.h 共享枚举的 JSON 互转、位运算与 FontWeight 数值。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/enums.h"
#include "aurora/widget/props_io.h"

#include "test_harness.h"

using aurora::BoxFit;
using aurora::FontStyle;
using aurora::FontWeight;
using aurora::Json;
using aurora::json_to_box_fit;
using aurora::json_to_font_style;
using aurora::json_to_font_weight;
using aurora::json_to_main_axis_size;
using aurora::json_to_stack_fit;
using aurora::json_to_text_align;
using aurora::json_to_text_decoration;
using aurora::json_to_text_overflow;
using aurora::MainAxisSize;
using aurora::StackFit;
using aurora::TextAlign;
using aurora::TextDecoration;
using aurora::TextOverflow;

static void test_text_align() {
    AURORA_TEST_CHECK_MSG(text_align_to_json(TextAlign::Center).get<std::string>() == "Center",
                          "TextAlign->json Center");
    AURORA_TEST_CHECK_MSG(json_to_text_align(Json("End")) == TextAlign::End, "json->TextAlign End");
    AURORA_TEST_CHECK_MSG(json_to_text_align(Json("Bogus")) == TextAlign::Left,
                          "json->TextAlign unknown falls back Left");
    AURORA_TEST_CHECK_MSG(json_to_text_align(text_align_to_json(TextAlign::Justify)) == TextAlign::Justify,
                          "TextAlign round-trip Justify");
}

static void test_text_overflow_and_font() {
    AURORA_TEST_CHECK_MSG(text_overflow_to_json(TextOverflow::Ellipsis).get<std::string>() == "Ellipsis",
                          "TextOverflow->json Ellipsis");
    AURORA_TEST_CHECK_MSG(json_to_text_overflow(Json("Fade")) == TextOverflow::Fade, "json->TextOverflow Fade");
    AURORA_TEST_CHECK_MSG(json_to_text_overflow(Json("Nope")) == TextOverflow::Clip,
                          "json->TextOverflow unknown falls back Clip");

    AURORA_TEST_CHECK_MSG(font_style_to_json(FontStyle::Italic).get<std::string>() == "Italic",
                          "FontStyle->json Italic");
    AURORA_TEST_CHECK_MSG(json_to_font_style(Json("Italic")) == FontStyle::Italic, "json->FontStyle Italic");
    AURORA_TEST_CHECK_MSG(json_to_font_style(Json("Nope")) == FontStyle::Normal,
                          "json->FontStyle unknown falls back Normal");
}

static void test_font_weight() {
    AURORA_TEST_CHECK_MSG(font_weight_to_json(FontWeight::Bold).get<std::string>() == "700",
                          "FontWeight->json Bold=700");
    AURORA_TEST_CHECK_MSG(json_to_font_weight(Json("700")) == FontWeight::Bold, "json->FontWeight 700=Bold");
    AURORA_TEST_CHECK_MSG(json_to_font_weight(Json(400)) == FontWeight::Normal, "json->FontWeight number 400=Normal");
    AURORA_TEST_CHECK_MSG(json_to_font_weight(Json("999")) == FontWeight::Normal,
                          "json->FontWeight unknown number falls back Normal");
    AURORA_TEST_CHECK_MSG(json_to_font_weight(font_weight_to_json(FontWeight::Black)) == FontWeight::Black,
                          "FontWeight round-trip Black");
}

static void test_text_decoration() {
    constexpr TextDecoration combo = TextDecoration::Underline | TextDecoration::LineThrough;
    AURORA_TEST_CHECK_MSG(decoration_has(combo, TextDecoration::Underline), "TextDecoration has Underline");
    AURORA_TEST_CHECK_MSG(decoration_has(combo, TextDecoration::LineThrough), "TextDecoration has LineThrough");
    AURORA_TEST_CHECK_MSG(!decoration_has(combo, TextDecoration::Overline), "TextDecoration not has Overline");

    const Json j = text_decoration_to_json(combo);
    AURORA_TEST_CHECK_MSG(j.is_array() && j.size() == 2, "TextDecoration->json array of 2");
    const TextDecoration back = json_to_text_decoration(j);
    AURORA_TEST_CHECK_MSG(back == combo, "TextDecoration round-trip combo");

    AURORA_TEST_CHECK_MSG(text_decoration_to_json(TextDecoration::None)[0].get<std::string>() == "None",
                          "TextDecoration None -> [None]");
    AURORA_TEST_CHECK_MSG(json_to_text_decoration(Json("Underline")) == TextDecoration::Underline,
                          "TextDecoration single string parse");
    AURORA_TEST_CHECK_MSG(json_to_text_decoration(Json("Bogus")) == TextDecoration::None,
                          "TextDecoration unknown string -> None");
}

static void test_layout_enums() {
    AURORA_TEST_CHECK_MSG(main_axis_size_to_json(MainAxisSize::Max).get<std::string>() == "Max",
                          "MainAxisSize->json Max");
    AURORA_TEST_CHECK_MSG(json_to_main_axis_size(Json("Max")) == MainAxisSize::Max, "json->MainAxisSize Max");
    AURORA_TEST_CHECK_MSG(json_to_main_axis_size(Json("Nope")) == MainAxisSize::Min, "json->MainAxisSize unknown Min");

    AURORA_TEST_CHECK_MSG(stack_fit_to_json(StackFit::Expand).get<std::string>() == "Expand", "StackFit->json Expand");
    AURORA_TEST_CHECK_MSG(json_to_stack_fit(Json("Passthrough")) == StackFit::Passthrough,
                          "json->StackFit Passthrough");

    AURORA_TEST_CHECK_MSG(box_fit_to_json(BoxFit::Cover).get<std::string>() == "Cover", "BoxFit->json Cover");
    AURORA_TEST_CHECK_MSG(json_to_box_fit(Json("Contain")) == BoxFit::Contain, "json->BoxFit Contain");
    AURORA_TEST_CHECK_MSG(json_to_box_fit(Json("Nope")) == BoxFit::Fill, "json->BoxFit unknown falls back Fill");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== enums_test ===\n");
    test_text_align();
    test_text_overflow_and_font();
    test_font_weight();
    test_text_decoration();
    test_layout_enums();
}
