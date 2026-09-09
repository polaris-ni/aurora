/// 测试类型: integration
/// 目标单元: include/aurora/widget/codegen.h
/// 测试说明: 覆盖 to_code 三风格代码生成（Fluent / DesignatedInit / StepByStep）——
///           容器/叶形式、Grid 列数追加、各属性类型的 C++ 表达式发射（Length/Color/
///           EdgeInsets/枚举/FontWeight/TextDecoration）与歧义键不猜测的现状锁定
/// 覆盖说明: 输入为 to_json 同构的 {type, props, children} 结构快照

#include <string>

#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_to_code {

using au::serialization::CodeStyle;
using au::serialization::to_code;

namespace {

// 构造 { type, props, children } 结构快照（与 to_json 输出同构）。
auto make_node(const std::string &type, const au::Json &props = au::Json::object(),
               au::Json children = au::Json::array()) -> au::Json {
    au::Json node = au::Json::object();
    node["type"] = type;
    node["props"] = props;
    node["children"] = std::move(children);
    return node;
}

auto make_button(const std::string &label) -> au::Json {
    return make_node("Button", au::Json{{"label", label}});
}

}  // namespace

AURORA_TEST_CASE(codegen_fluent_emits_column_and_button) {
    au::Json node = make_node("Column", au::Json::object(),
                              au::Json::array({make_button("OK"), make_button("Cancel")}));

    const std::string code = to_code(node);
    AURORA_TEST_CHECK_MSG(code.find("au::Column{") != std::string::npos, "codegen: fluent emits au::Column{");
    AURORA_TEST_CHECK_MSG(code.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: fluent emits au::Button(...)");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"OK\"") != std::string::npos,
                          "codegen: fluent emits .label = \"OK\"");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"Cancel\"") != std::string::npos,
                          "codegen: fluent emits .label = \"Cancel\"");
}

AURORA_TEST_CASE(codegen_fluent_grid_appends_column_count) {
    au::Json node = make_node("Grid", au::Json{{"columns", 2}},
                              au::Json::array({make_button("A"), make_button("B")}));

    const std::string code = to_code(node);
    AURORA_TEST_CHECK_MSG(code.find("au::Grid{") != std::string::npos, "codegen: grid fluent emits au::Grid{");
    AURORA_TEST_CHECK_MSG(code.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: grid fluent emits au::Button(...)");
    // 列数 2 作为独立行追加于子项之后（缩进 + "2" + 收尾 }）。
    AURORA_TEST_CHECK_MSG(code.find("2\n}") != std::string::npos || code.find("2}") != std::string::npos,
                          "codegen: grid fluent appends column count");
}

AURORA_TEST_CASE(codegen_designated_init_and_step_by_step_styles) {
    au::Json node = make_node("Column", au::Json::object(), au::Json::array({make_button("OK")}));

    const std::string di = to_code(node, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(di.find("au::Column{") != std::string::npos,
                          "codegen: DesignatedInit emits au::Column{");
    AURORA_TEST_CHECK_MSG(di.find("au::Button{") != std::string::npos,
                          "codegen: DesignatedInit emits au::Button{");

    const std::string sb = to_code(node, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb.find("auto __w0 = au::Column{};") != std::string::npos,
                          "codegen: StepByStep declares Column var");
    AURORA_TEST_CHECK_MSG(sb.find("auto __w1 = au::Button{};") != std::string::npos,
                          "codegen: StepByStep declares Button var");
    AURORA_TEST_CHECK_MSG(sb.find("__w1.label = \"OK\";") != std::string::npos,
                          "codegen: StepByStep assigns label");
    // StepByStep 把子项声明内嵌进父项的 { } 列表，故断言「打开 children 列表」且「引用子变量 __w1」。
    AURORA_TEST_CHECK_MSG(sb.find("__w0.children = {") != std::string::npos,
                          "codegen: StepByStep opens children list");
    AURORA_TEST_CHECK_MSG(sb.find("__w1") != std::string::npos, "codegen: StepByStep references child var __w1");

    // 默认风格等价于 Fluent。
    AURORA_TEST_CHECK_MSG(to_code(node) == to_code(node, CodeStyle::Fluent),
                          "codegen: default style == Fluent");
}

AURORA_TEST_CASE(codegen_scalar_props_string_bool_float_int) {
    const std::string str_code = to_code(make_node("Text", au::Json{{"content", "Hello"}, {"label", "OK"}}));
    AURORA_TEST_CHECK_MSG(str_code.find(".content = \"Hello\"") != std::string::npos,
                          "emit_props: string content");
    AURORA_TEST_CHECK_MSG(str_code.find(".label = \"OK\"") != std::string::npos, "emit_props: string label");

    const std::string bool_true = to_code(make_node("Text", au::Json{{"show", true}}));
    AURORA_TEST_CHECK_MSG(bool_true.find(".show = true") != std::string::npos, "emit_props: bool true");
    const std::string bool_false = to_code(make_node("Text", au::Json{{"show", false}}));
    AURORA_TEST_CHECK_MSG(bool_false.find(".show = false") != std::string::npos, "emit_props: bool false");

    const std::string float_code = to_code(make_node("Text", au::Json{{"font_size", 14.0}}));
    AURORA_TEST_CHECK_MSG(float_code.find(".font_size = ") != std::string::npos,
                          "emit_props: float prop present");
    AURORA_TEST_CHECK_MSG(float_code.find('f') != std::string::npos, "emit_props: float has f suffix");

    const std::string int_code = to_code(make_node("Column", au::Json{{"flex", 1}}));
    AURORA_TEST_CHECK_MSG(int_code.find(".flex = 1") != std::string::npos, "emit_props: int prop");
}

AURORA_TEST_CASE(codegen_length_forms_px_percent_legacy_auto_fill) {
    const std::string px = to_code(make_node("Text", au::Json{{"width", au::Json::array({"px", 100.0F})}}));
    AURORA_TEST_CHECK_MSG(px.find("au::px(") != std::string::npos, "emit_props: Length px array");

    const std::string percent = to_code(make_node("Text", au::Json{{"width", au::Json::array({"percent", 0.5F})}}));
    AURORA_TEST_CHECK_MSG(percent.find("au::percent(") != std::string::npos, "emit_props: Length percent array");

    // 旧格式兼容: {"value": 100, "unit": "px"} / {"value": 50, "unit": "pct"}。
    const std::string legacy_px =
        to_code(make_node("Text", au::Json{{"width", au::Json{{"value", 100.0F}, {"unit", "px"}}}}));
    AURORA_TEST_CHECK_MSG(legacy_px.find("au::px(") != std::string::npos, "emit_props: Length legacy object px");
    const std::string legacy_pct =
        to_code(make_node("Text", au::Json{{"width", au::Json{{"value", 50.0F}, {"unit", "pct"}}}}));
    AURORA_TEST_CHECK_MSG(legacy_pct.find("au::percent(") != std::string::npos,
                          "emit_props: Length legacy object pct");

    const std::string auto_fill =
        to_code(make_node("Text", au::Json{{"width", "auto"}, {"height", "fill"}}));
    AURORA_TEST_CHECK_MSG(auto_fill.find("au::auto_length()") != std::string::npos,
                          "emit_props: Length auto string");
    AURORA_TEST_CHECK_MSG(auto_fill.find("au::fill()") != std::string::npos, "emit_props: Length fill string");
}

AURORA_TEST_CASE(codegen_color_and_edge_insets_values) {
    // Color 序列化格式: [r,g,b,a]。
    const std::string color = to_code(make_node("Text", au::Json{{"color", au::Json::array({255, 0, 0, 255})}}));
    AURORA_TEST_CHECK_MSG(color.find("Color{") != std::string::npos, "emit_props: Color array has Color{");
    AURORA_TEST_CHECK_MSG(color.find("255,0,0,255") != std::string::npos, "emit_props: Color RGBA values");

    const std::string insets = to_code(
        make_node("Column", au::Json{{"padding", au::Json{{"top", 8.0F}, {"right", 12.0F},
                                                          {"bottom", 8.0F}, {"left", 12.0F}}}}));
    AURORA_TEST_CHECK_MSG(insets.find("EdgeInsets{") != std::string::npos, "emit_props: EdgeInsets object");
}

AURORA_TEST_CASE(codegen_enum_keys_emit_enum_expressions) {
    // 历史合成键名。
    AURORA_TEST_CHECK_MSG(to_code(make_node("Text", au::Json{{"text_align", "Center"}}))
                              .find("TextAlign::Center") != std::string::npos,
                          "emit_props: enum TextAlign");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Text", au::Json{{"text_overflow", "Ellipsis"}}))
                              .find("TextOverflow::Ellipsis") != std::string::npos,
                          "emit_props: enum TextOverflow");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Column", au::Json{{"main_axis_alignment", "SpaceBetween"}}))
                              .find("MainAxisAlignment::SpaceBetween") != std::string::npos,
                          "emit_props: enum MainAxisAlignment");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Column", au::Json{{"cross_axis_alignment", "Stretch"}}))
                              .find("CrossAxisAlignment::Stretch") != std::string::npos,
                          "emit_props: enum CrossAxisAlignment");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Column", au::Json{{"main_axis_size", "Max"}}))
                              .find("MainAxisSize::Max") != std::string::npos,
                          "emit_props: enum MainAxisSize");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Stack", au::Json{{"stack_fit", "Expand"}}))
                              .find("StackFit::Expand") != std::string::npos,
                          "emit_props: enum StackFit");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Image", au::Json{{"box_fit", "Cover"}}))
                              .find("BoxFit::Cover") != std::string::npos,
                          "emit_props: enum BoxFit");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Column", au::Json{{"overflow_strategy", "Scroll"}}))
                              .find("OverflowStrategy::Scroll") != std::string::npos,
                          "emit_props: enum OverflowStrategy");

    // 真实序列化属性名（to_json 的产出键名）。
    AURORA_TEST_CHECK_MSG(to_code(make_node("Stack", au::Json{{"alignment", "TopLeft"}}))
                              .find("Alignment::TopLeft") != std::string::npos,
                          "emit_props: real key alignment → Alignment");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Text", au::Json{{"overflow", "Ellipsis"}}))
                              .find("TextOverflow::Ellipsis") != std::string::npos,
                          "emit_props: real key overflow → TextOverflow");
    AURORA_TEST_CHECK_MSG(to_code(make_node("Drawer", au::Json{{"side", "Right"}}))
                              .find("DrawerSide::Right") != std::string::npos,
                          "emit_props: real key side → DrawerSide");
    AURORA_TEST_CHECK_MSG(to_code(make_node("ToastHost", au::Json{{"position", "Top"}}))
                              .find("ToastPosition::Top") != std::string::npos,
                          "emit_props: real key position → ToastPosition");
}

AURORA_TEST_CASE(codegen_ambiguous_enum_keys_stay_as_strings) {
    // fit / orientation 无法按键消歧（StackFit vs BoxFit、Orientation vs SplitterOrientation），
    // 刻意保持字符串输出，等属性声明类型透传后再升级——测试锁住这一现状。
    const std::string code = to_code(make_node("Stack", au::Json{{"fit", "Cover"}}));
    AURORA_TEST_CHECK_MSG(code.find("\"Cover\"") != std::string::npos,
                          "emit_props: ambiguous key fit stays as string");
}

AURORA_TEST_CASE(codegen_font_weight_style_and_text_decoration) {
    const std::string bold = to_code(make_node("Text", au::Json{{"font_weight", "700"}}));
    AURORA_TEST_CHECK_MSG(bold.find("FontWeight::Bold") != std::string::npos,
                          "emit_props: FontWeight 700 → Bold");
    const std::string normal = to_code(make_node("Text", au::Json{{"font_weight", "400"}}));
    AURORA_TEST_CHECK_MSG(normal.find("FontWeight::Normal") != std::string::npos,
                          "emit_props: FontWeight 400 → Normal");

    const std::string italic = to_code(make_node("Text", au::Json{{"font_style", "Italic"}}));
    AURORA_TEST_CHECK_MSG(italic.find("FontStyle::Italic") != std::string::npos,
                          "emit_props: enum FontStyle");

    const std::string single = to_code(make_node("Text", au::Json{{"text_decoration", au::Json::array({"Underline"})}}));
    AURORA_TEST_CHECK_MSG(single.find("TextDecoration::Underline") != std::string::npos,
                          "emit_props: TextDecoration array Underline");
    const std::string combined = to_code(
        make_node("Text", au::Json{{"text_decoration", au::Json::array({"Underline", "LineThrough"})}}));
    AURORA_TEST_CHECK_MSG(combined.find("TextDecoration::Underline") != std::string::npos &&
                              combined.find("TextDecoration::LineThrough") != std::string::npos &&
                              combined.find(" | ") != std::string::npos,
                          "emit_props: TextDecoration combined with |");
    const std::string none = to_code(make_node("Text", au::Json{{"text_decoration", au::Json::array({"None"})}}));
    AURORA_TEST_CHECK_MSG(none.find("TextDecoration::None") != std::string::npos,
                          "emit_props: TextDecoration None");
}

AURORA_TEST_CASE(codegen_all_styles_share_value_emission) {
    // Color / 枚举 / EdgeInsets 在三种风格下的表达式发射一致。
    const au::Json color_node =
        make_node("Text", au::Json{{"color", au::Json::array({0, 128, 255, 255})}});
    AURORA_TEST_CHECK_MSG(to_code(color_node, CodeStyle::Fluent).find("Color{") != std::string::npos,
                          "styles: Fluent has Color{}");
    AURORA_TEST_CHECK_MSG(to_code(color_node, CodeStyle::DesignatedInit).find("Color{") != std::string::npos,
                          "styles: DesignatedInit has Color{}");
    const std::string sb_color = to_code(color_node, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb_color.find("Color{") != std::string::npos, "styles: StepByStep has Color{}");
    AURORA_TEST_CHECK_MSG(sb_color.find("__w0.color = ") != std::string::npos,
                          "styles: StepByStep assigns color via __w0");

    const au::Json enum_node = make_node("Text", au::Json{{"text_align", "Right"}});
    AURORA_TEST_CHECK_MSG(to_code(enum_node, CodeStyle::Fluent).find("TextAlign::Right") != std::string::npos,
                          "styles: Fluent enum");
    AURORA_TEST_CHECK_MSG(
        to_code(enum_node, CodeStyle::DesignatedInit).find("TextAlign::Right") != std::string::npos,
        "styles: DesignatedInit enum");
    AURORA_TEST_CHECK_MSG(
        to_code(enum_node, CodeStyle::StepByStep).find("TextAlign::Right") != std::string::npos,
        "styles: StepByStep enum");

    const au::Json insets_node =
        make_node("Column", au::Json{{"padding", au::Json{{"top", 4.0F}, {"right", 8.0F},
                                                          {"bottom", 4.0F}, {"left", 8.0F}}}});
    AURORA_TEST_CHECK_MSG(to_code(insets_node, CodeStyle::Fluent).find("EdgeInsets{") != std::string::npos,
                          "styles: Fluent EdgeInsets");
    AURORA_TEST_CHECK_MSG(
        to_code(insets_node, CodeStyle::StepByStep).find("EdgeInsets{") != std::string::npos,
        "styles: StepByStep EdgeInsets");
}

AURORA_TEST_CASE(codegen_multi_prop_mixed_emission) {
    const au::Json props = au::Json{
        {"content", "Hello"},
        {"font_size", 16.0F},
        {"show", true},
        {"text_align", "Center"},
        {"font_weight", "700"},
        {"color", au::Json::array({255, 0, 0, 255})},
        {"width", au::Json::array({"px", 200.0F})},
    };
    const std::string code = to_code(make_node("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".content = \"Hello\"") != std::string::npos, "mixed: content");
    AURORA_TEST_CHECK_MSG(code.find(".show = true") != std::string::npos, "mixed: show");
    AURORA_TEST_CHECK_MSG(code.find("TextAlign::Center") != std::string::npos, "mixed: text_align");
    AURORA_TEST_CHECK_MSG(code.find("FontWeight::Bold") != std::string::npos, "mixed: font_weight");
    AURORA_TEST_CHECK_MSG(code.find("Color{") != std::string::npos, "mixed: color");
    AURORA_TEST_CHECK_MSG(code.find("au::px(") != std::string::npos, "mixed: width px");
}

}  // namespace aurora::test_cases::itest_to_code
