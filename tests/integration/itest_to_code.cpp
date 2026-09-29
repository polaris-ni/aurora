/// 测试类型: integration
/// 目标单元: include/aurora/widget/codegen.h
/// 测试说明: 覆盖 to_code 三风格代码生成（Fluent / DesignatedInit / StepByStep）——
///           容器/叶形式、属性落点换算（基类属性/嵌套路径/setter-only/同名遮蔽键）、`au::` 限定与字面量形态、
///           指定初始化器按 `*Props` 声明序与枚举取值按自描述规范化，以及「无公开写入路径即省略」的能力边界
/// 覆盖说明: 输入为 to_json 同构的 {type, props, children} 结构快照
///           产物可编译性由本文件的形态断言 + 全量类型体检（见 specification/08-tooling.md §2.5）共同锁定

#include <string>
#include <vector>

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

auto make_button(const std::string &label) -> au::Json { return make_node("Button", au::Json{{"label", label}}); }

auto make_text(const au::Json &props) -> au::Json { return make_node("Text", props); }

}  // namespace

AURORA_TEST_CASE(codegen_fluent_emits_column_and_button) {
    au::Json node =
        make_node("Column", au::Json::object(), au::Json::array({make_button("OK"), make_button("Cancel")}));

    const std::string code = to_code(node);
    AURORA_TEST_CHECK_MSG(code.find("au::Column{") != std::string::npos, "codegen: fluent emits au::Column{");
    AURORA_TEST_CHECK_MSG(code.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: fluent emits au::Button(...)");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"OK\"") != std::string::npos, "codegen: fluent emits .label = \"OK\"");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"Cancel\"") != std::string::npos,
                          "codegen: fluent emits .label = \"Cancel\"");
}

AURORA_TEST_CASE(codegen_fluent_grid_puts_column_count_in_props) {
    // `au::Grid{a, b, 2}` 不是合法构造（无匹配 ctor），列数只能作 Props 成员发射。
    au::Json node = make_node("Grid", au::Json{{"columns", 2}}, au::Json::array({make_button("A"), make_button("B")}));

    const std::string code = to_code(node);
    AURORA_TEST_CHECK_MSG(code.find("au::Grid(au::GridProps{") != std::string::npos,
                          "codegen: grid with props uses GridProps form");
    AURORA_TEST_CHECK_MSG(code.find(".children = {") != std::string::npos, "codegen: grid children slot first");
    AURORA_TEST_CHECK_MSG(code.find(".columns = 2") != std::string::npos, "codegen: grid emits .columns = 2");
    AURORA_TEST_CHECK_MSG(code.find("au::Grid{") == std::string::npos,
                          "codegen: grid never uses the positional column-count ctor");
}

AURORA_TEST_CASE(codegen_designated_init_and_step_by_step_styles) {
    au::Json node = make_node("Column", au::Json::object(), au::Json::array({make_button("OK")}));

    // 指定初始化器只对 `*Props` 聚合合法：控件类本身是非聚合类型，`au::Button{.label=...}` 编译不过。
    const std::string di = to_code(node, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(di.find("au::Column(au::ColumnProps{") != std::string::npos,
                          "codegen: DesignatedInit wraps container in ColumnProps");
    AURORA_TEST_CHECK_MSG(di.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: DesignatedInit wraps leaf in ButtonProps");
    AURORA_TEST_CHECK_MSG(di.find(".children = {") != std::string::npos, "codegen: DesignatedInit uses .children slot");

    const std::string sb = to_code(node, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb.find("auto __w1 = au::Button{};") != std::string::npos,
                          "codegen: StepByStep declares Button var");
    AURORA_TEST_CHECK_MSG(sb.find("__w1.label = \"OK\";") != std::string::npos, "codegen: StepByStep assigns label");
    AURORA_TEST_CHECK_MSG(sb.find("auto __w0 = au::Column(au::ColumnProps{") != std::string::npos,
                          "codegen: StepByStep passes children at construction");
    AURORA_TEST_CHECK_MSG(sb.find("__w0.children =") == std::string::npos,
                          "codegen: StepByStep never assigns children post-construction");
    AURORA_TEST_CHECK_MSG(sb.find("au::Node{std::move(__w1)}") != std::string::npos,
                          "codegen: StepByStep hands child over by move");

    // 默认风格等价于 Fluent。
    AURORA_TEST_CHECK_MSG(to_code(node) == to_code(node, CodeStyle::Fluent), "codegen: default style == Fluent");
}

AURORA_TEST_CASE(codegen_scalar_props_string_bool_float_int) {
    const std::string str_code = to_code(make_text(au::Json{{"content", "Hello"}}));
    AURORA_TEST_CHECK_MSG(str_code.find(".content = \"Hello\"") != std::string::npos, "emit_props: string content");

    const std::string bool_true = to_code(make_text(au::Json{{"soft_wrap", true}}));
    AURORA_TEST_CHECK_MSG(bool_true.find(".soft_wrap = true") != std::string::npos, "emit_props: bool true");
    const std::string bool_false = to_code(make_text(au::Json{{"soft_wrap", false}}));
    AURORA_TEST_CHECK_MSG(bool_false.find(".soft_wrap = false") != std::string::npos, "emit_props: bool false");

    // 整数值浮点属性（`14.0`）必须带小数点：`14f` 会被解析成用户字面量 `operator""f`。
    const std::string float_code = to_code(make_node("Button", au::Json{{"corner_radius", 14.0}}));
    AURORA_TEST_CHECK_MSG(float_code.find(".corner_radius = 14.0f") != std::string::npos,
                          "emit_props: integral float keeps decimal point");
    AURORA_TEST_CHECK_MSG(float_code.find("14f") == std::string::npos, "emit_props: no bare integral-f literal");

    const std::string frac_code = to_code(make_node("Button", au::Json{{"corner_radius", 6.5}}));
    AURORA_TEST_CHECK_MSG(frac_code.find(".corner_radius = 6.5f") != std::string::npos,
                          "emit_props: fractional float keeps f suffix");

    const std::string int_code = to_code(make_node("Grid", au::Json{{"columns", 3}}));
    AURORA_TEST_CHECK_MSG(int_code.find(".columns = 3") != std::string::npos, "emit_props: int prop");
}

AURORA_TEST_CASE(codegen_length_forms_px_percent_legacy_auto_fill) {
    const std::string px =
        to_code(make_node("Button", au::Json{{"corner_radius", 8.0}, {"width", au::Json::array({"px", 100.0F})}}));
    // 宽度是基类属性，表达式风格不承载；此处只验长度字面量形态（同一发射函数亦服务于 step 风格）。
    AURORA_TEST_CHECK_MSG(px.find(".corner_radius = 8.0f") != std::string::npos,
                          "emit_props: Length sibling prop kept");
    const std::string step_px =
        to_code(make_node("Button", au::Json{{"width", au::Json::array({"px", 100.0F})}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(step_px.find("__w0.width(au::px(100.0f));") != std::string::npos,
                          "emit_props: Length px array via width() setter");

    const std::string percent =
        to_code(make_node("Button", au::Json{{"width", au::Json::array({"percent", 0.5F})}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(percent.find("au::percent(0.5f)") != std::string::npos, "emit_props: Length percent array");

    // 旧格式兼容: {"value": 100, "unit": "px"} / {"value": 50, "unit": "pct"}。
    const std::string legacy_px = to_code(
        make_node("Button", au::Json{{"width", au::Json{{"value", 100.0F}, {"unit", "px"}}}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(legacy_px.find("au::px(100.0f)") != std::string::npos, "emit_props: Length legacy object px");
    const std::string legacy_pct = to_code(
        make_node("Button", au::Json{{"width", au::Json{{"value", 50.0F}, {"unit", "pct"}}}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(legacy_pct.find("au::percent(50.0f)") != std::string::npos,
                          "emit_props: Length legacy object pct");

    const std::string auto_fill =
        to_code(make_node("Button", au::Json{{"width", "auto"}, {"height", "fill"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(auto_fill.find("au::auto_length()") != std::string::npos, "emit_props: Length auto string");
    AURORA_TEST_CHECK_MSG(auto_fill.find("au::fill()") != std::string::npos, "emit_props: Length fill string");
}

AURORA_TEST_CASE(codegen_color_and_edge_insets_values) {
    // Color 序列化格式: [r,g,b,a]；产物按 demos 口径做 `au::` 限定，使代码在任意命名空间可编译。
    const std::string color = to_code(make_text(au::Json{{"color", au::Json::array({255, 0, 0, 255})}}));
    AURORA_TEST_CHECK_MSG(color.find("au::Color{255,0,0,255}") != std::string::npos,
                          "emit_props: Color array as au::Color RGBA");
    // Text 的 `color` 键落在聚合成员 `text_color`（异名换算）。
    AURORA_TEST_CHECK_MSG(color.find(".text_color = au::Color{") != std::string::npos,
                          "emit_props: Text color aliases to text_color");

    const std::string insets = to_code(make_node(
        "Button", au::Json{{"padding", au::Json{{"top", 8.0F}, {"right", 12.0F}, {"bottom", 8.0F}, {"left", 12.0F}}}}));
    AURORA_TEST_CHECK_MSG(insets.find("au::EdgeInsets{8.0f,12.0f,8.0f,12.0f}") != std::string::npos,
                          "emit_props: EdgeInsets object with f suffix");
}

AURORA_TEST_CASE(codegen_enum_keys_emit_qualified_expressions) {
    AURORA_TEST_CHECK_MSG(
        to_code(make_text(au::Json{{"text_align", "Center"}})).find("au::TextAlign::Center") != std::string::npos,
        "emit_props: enum TextAlign");
    AURORA_TEST_CHECK_MSG(
        to_code(make_text(au::Json{{"overflow", "Ellipsis"}})).find("au::TextOverflow::Ellipsis") != std::string::npos,
        "emit_props: Text overflow key → TextOverflow");
    AURORA_TEST_CHECK_MSG(
        to_code(make_text(au::Json{{"font_style", "Italic"}})).find("au::FontStyle::Italic") != std::string::npos,
        "emit_props: enum FontStyle");
    AURORA_TEST_CHECK_MSG(
        to_code(make_text(au::Json{{"direction", "RTL"}})).find("au::TextDirection::RTL") != std::string::npos,
        "emit_props: enum TextDirection");
    // 基类 `overflow`（非 Text 类型）是 OverflowStrategy，经 overflow_strategy() setter 写入。
    const std::string scroll_overflow =
        to_code(make_node("Scroll", au::Json{{"overflow", "Scroll"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(
        scroll_overflow.find("__w0.overflow_strategy(au::OverflowStrategy::Scroll);") != std::string::npos,
        "emit_props: base overflow → OverflowStrategy via setter");
    // 历史合成键名仍可用（手工 JSON 的入口）。
    AURORA_TEST_CHECK_MSG(
        to_code(make_text(au::Json{{"text_overflow", "Ellipsis"}})).find("au::TextOverflow::Ellipsis") !=
            std::string::npos,
        "emit_props: legacy key text_overflow");
}

AURORA_TEST_CASE(codegen_ambiguous_enum_keys_resolved_by_type) {
    // 同名键在不同控件上属于不同枚举；生成时已知类型，故按 (type, key) 消歧。
    const std::string divider =
        to_code(make_node("Divider", au::Json{{"orientation", "Vertical"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(divider.find("au::Orientation::Vertical") != std::string::npos,
                          "emit_props: Divider orientation → Orientation");
    const std::string stack_fit = to_code(make_node("Stack", au::Json{{"fit", "Expand"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(stack_fit.find("au::StackFit::Expand") != std::string::npos,
                          "emit_props: Stack fit → StackFit");
}

AURORA_TEST_CASE(codegen_key_without_public_write_path_is_dropped_not_invented) {
    // `Splitter.orientation` 确属 `SplitterOrientation`，但该类型既无 `*Props` 聚合也没有
    // `set_orientation()`（方向只能进构造形参）：与其臆造一条编译不过的赋值，不如整条省略并告警。
    const std::string splitter =
        to_code(make_node("Splitter", au::Json{{"orientation", "Vertical"}, {"ratio", 0.5F}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(splitter.find("orientation") == std::string::npos,
                          "no write path: Splitter orientation dropped rather than invented");
    AURORA_TEST_CHECK_MSG(splitter.find("SplitterOrientation") == std::string::npos,
                          "no write path: does not invent an enum landing point");
    // 未注册类型同理：无从判定写入路径，就不产出 `au::Whatever` 之类的臆造形态。
    const std::string unknown = to_code(make_node("Whatever", au::Json{{"fit", "Cover"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(unknown.find("Cover") == std::string::npos,
                          "no write path: props of unregistered types are omitted");
    AURORA_TEST_CHECK_MSG(unknown.find("WhateverProps") == std::string::npos,
                          "no write path: does not invent a Props aggregate");
}

AURORA_TEST_CASE(codegen_font_weight_and_text_decoration) {
    // 落点 `Font::weight` 是 int（CSS 100..900），故发射数值字面量而非枚举名。
    const std::string bold = to_code(make_text(au::Json{{"font_weight", "700"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(bold.find("__w0.font.weight = 700;") != std::string::npos,
                          "emit_props: FontWeight 700 → font.weight int");

    const std::string single = to_code(make_text(au::Json{{"decoration", au::Json::array({"Underline"})}}));
    AURORA_TEST_CHECK_MSG(single.find("au::TextDecoration::Underline") != std::string::npos,
                          "emit_props: TextDecoration array Underline");
    const std::string combined =
        to_code(make_text(au::Json{{"decoration", au::Json::array({"Underline", "LineThrough"})}}));
    AURORA_TEST_CHECK_MSG(combined.find("au::TextDecoration::Underline") != std::string::npos &&
                              combined.find("au::TextDecoration::LineThrough") != std::string::npos &&
                              combined.find(" | ") != std::string::npos,
                          "emit_props: TextDecoration combined with |");
    const std::string none = to_code(make_text(au::Json{{"decoration", au::Json::array({"None"})}}));
    AURORA_TEST_CHECK_MSG(none.find("au::TextDecoration::None") != std::string::npos,
                          "emit_props: TextDecoration None");
}

AURORA_TEST_CASE(codegen_base_props_only_reach_step_style) {
    // `width`/`height`/`show`/a11y 三键由 `Widget::serialize_props` 无条件写出，任何 `*Props` 聚合都没有
    // 同名成员：表达式风格一律省略，StepByStep 走真实公开 API（链式 setter / 公有成员）。
    const au::Json props = au::Json{{"label", "OK"},
                                    {"width", au::Json::array({"px", 120.0F})},
                                    {"height", "fill"},
                                    {"show", false},
                                    // CJK-LITERAL: cjk-fixture - Han generator payload, asserted below
                                    {"accessibility_label", "提交"},
                                    {"stable_key", "submit-btn"},
                                    {"labelled_by", "form-title"}};
    const std::string expr = to_code(make_node("Button", props));
    for (const char *const forbidden :
         {".show", ".width", ".height", ".accessibility_label", ".stable_key", ".labelled_by"}) {
        AURORA_TEST_CHECK_MSG(expr.find(forbidden) == std::string::npos,
                              std::string("base prop absent from expression style: ") + forbidden);
    }
    AURORA_TEST_CHECK_MSG(expr.find(".label = \"OK\"") != std::string::npos, "base prop filter keeps real props");

    const std::string step = to_code(make_node("Button", props), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(step.find("__w0.show = false;") != std::string::npos, "step assigns public show member");
    AURORA_TEST_CHECK_MSG(step.find("__w0.width(au::px(120.0f));") != std::string::npos, "step calls width() setter");
    AURORA_TEST_CHECK_MSG(step.find("__w0.height(au::fill());") != std::string::npos, "step calls height() setter");
    // CJK-LITERAL: cjk-fixture - same Han label expected back from the generator verbatim
    AURORA_TEST_CHECK_MSG(step.find("__w0.set_accessibility_label(\"提交\");") != std::string::npos,
                          "step calls set_accessibility_label");
    AURORA_TEST_CHECK_MSG(step.find("__w0.set_stable_key(\"submit-btn\");") != std::string::npos,
                          "step calls set_stable_key");
    AURORA_TEST_CHECK_MSG(step.find("__w0.set_labelled_by(\"form-title\");") != std::string::npos,
                          "step calls set_labelled_by");
}

AURORA_TEST_CASE(codegen_never_invents_props_aggregate) {
    // 只有 17 个类型有 `*Props` 聚合；给其余类型臆造 `au::StackProps` 会直接编译失败。
    const std::string stack = to_code(make_node("Stack", au::Json{{"alignment", "TopLeft"}}));
    AURORA_TEST_CHECK_MSG(stack.find("StackProps") == std::string::npos, "codegen: no invented StackProps");
    AURORA_TEST_CHECK_MSG(stack.find("au::Stack{}") != std::string::npos,
                          "codegen: bare Stack uses plain construction");

    const std::string spacer = to_code(make_node("Spacer", au::Json{{"show", true}}), CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(spacer.find("SpacerProps") == std::string::npos, "codegen: no invented SpacerProps");
    AURORA_TEST_CHECK_MSG(spacer.find("au::Spacer{}") != std::string::npos, "codegen: DI falls back to plain Spacer");

    const std::string container =
        to_code(make_node("Stack", au::Json::object(), au::Json::array({make_node("Spacer"), make_button("OK")})));
    AURORA_TEST_CHECK_MSG(container.find("au::Stack{") != std::string::npos, "codegen: Stack children as init list");
}

AURORA_TEST_CASE(codegen_nested_and_setter_props_only_reach_step_style) {
    // 嵌套路径（font.size_pt / flex.main_axis）与 setter-only（TextInput 样式组）无法进指定初始化器：
    // GCC 的 C++ 指定初始化器不支持嵌套设计符，protected 字段不能在类外赋值。
    const au::Json text_props = au::Json{{"content", "Hi"}, {"font_size", 16.0F}, {"main_axis_alignment", "Center"}};
    const std::string expr = to_code(make_text(text_props));
    AURORA_TEST_CHECK_MSG(expr.find(".font_size") == std::string::npos, "expression style drops nested font_size");
    AURORA_TEST_CHECK_MSG(expr.find(".content = \"Hi\"") != std::string::npos,
                          "expression style keeps top-level member");

    const std::string step =
        to_code(make_text(au::Json{{"content", "Hi"}, {"font_size", 16.0F}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(step.find("__w0.font.size_pt = 16.0f;") != std::string::npos,
                          "step writes nested font.size_pt");

    const au::Json col_props = au::Json{{"main_axis_alignment", "SpaceBetween"},
                                        {"cross_axis_alignment", "Stretch"},
                                        {"main_axis_size", "Max"},
                                        {"gap", 12.0F}};
    const std::string col_expr = to_code(make_node("Column", col_props));
    AURORA_TEST_CHECK_MSG(col_expr.find(".gap = 12.0f") != std::string::npos, "container keeps plain gap member");
    AURORA_TEST_CHECK_MSG(col_expr.find("main_axis_alignment") == std::string::npos,
                          "expression style drops nested flex alignment");
    const std::string col_step = to_code(make_node("Column", col_props), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(
        col_step.find("__w0.flex.main_axis = au::MainAxisAlignment::SpaceBetween;") != std::string::npos,
        "step writes flex.main_axis");
    AURORA_TEST_CHECK_MSG(col_step.find("__w0.flex.cross_axis = au::CrossAxisAlignment::Stretch;") != std::string::npos,
                          "step writes flex.cross_axis");
    AURORA_TEST_CHECK_MSG(col_step.find("__w0.flex.main_axis_size = au::MainAxisSize::Max;") != std::string::npos,
                          "step writes flex.main_axis_size");

    // CJK-LITERAL: cjk-fixture - Han placeholder fed to the generator as payload data
    const std::string input_step = to_code(
        make_node("TextInput", au::Json{{"placeholder", "姓名"}, {"corner_radius", 4.0F}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(input_step.find("__w0.set_corner_radius(4.0f);") != std::string::npos,
                          "step routes setter-only TextInput style through set_corner_radius");
}

AURORA_TEST_CASE(codegen_skips_props_without_write_path) {
    // `Scroll.offset` 是运行态（构造期无内容高度可夹取，控件也没有同名成员）——省略胜过发射不可编译的语句。
    const std::string step =
        to_code(make_node("Scroll", au::Json{{"offset", 240.0F}, {"step", 24.0F}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(step.find("offset") == std::string::npos,
                          "codegen: Scroll offset has no write path, skipped");
    AURORA_TEST_CHECK_MSG(step.find("__w0.step = 24.0f;") != std::string::npos, "codegen: sibling prop still emitted");
}

AURORA_TEST_CASE(codegen_step_show_member_shadowed_by_method_uses_qualified_name) {
    // Dialog/Drawer/ToastHost/ProgressDialog 自带成员函数 `show()`，遮蔽了基类成员 `Widget::show`；
    // 裸写 `w.show = true;` 取到的是成员函数，编译失败，故走限定名。
    for (const std::string type : {"Dialog", "Drawer", "ToastHost", "ProgressDialog"}) {
        const std::string step = to_code(make_node(type, au::Json{{"show", false}}), CodeStyle::StepByStep);
        AURORA_TEST_CHECK_MSG(step.find("__w0.Widget::show = false;") != std::string::npos,
                              "codegen: shadowed show uses Widget:: qualification for " + type);
    }
    const std::string plain = to_code(make_node("Button", au::Json{{"show", false}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(plain.find("__w0.show = false;") != std::string::npos,
                          "codegen: unshadowed show stays plain member assignment");
}

AURORA_TEST_CASE(codegen_step_statements_are_complete_and_children_declared_first) {
    // 结构守卫（TC-WIDGET-006 的原始缺陷就是把变量声明写进了初始化列表）：
    // ① 每条非空行都以 `;`、`,`、`{` 或 `}` 收尾；② 子节点变量先声明后被 `std::move` 引用。
    au::Json node = make_node(
        "Column", au::Json{{"gap", 8.0F}},
        au::Json::array({make_node("Row", au::Json::object(),
                                   au::Json::array({make_text(au::Json{{"content", "a"}}), make_button("b")})),
                         make_node("Spacer")}));
    const std::string step = to_code(node, CodeStyle::StepByStep);

    std::vector<std::string> lines{};
    std::string current{};
    for (const char c : step) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    for (const std::string &line : lines) {
        std::string trimmed{};
        for (const char c : line) {
            if (c != ' ' && c != '\t') {
                trimmed += c;
            }
        }
        if (trimmed.empty()) {
            continue;
        }
        const char last = trimmed.back();
        AURORA_TEST_CHECK_MSG(last == ';' || last == ',' || last == '{' || last == '}',
                              "step line is a complete statement: [" + trimmed + "]");
        AURORA_TEST_CHECK_MSG(trimmed.find("= { auto ") == std::string::npos,
                              "step never declares inside an initializer list: [" + trimmed + "]");
    }

    const std::size_t decl_w2 = step.find("auto __w2 =");
    const std::size_t use_w2 = step.find("std::move(__w2)");
    AURORA_TEST_CHECK_MSG(decl_w2 != std::string::npos && use_w2 != std::string::npos && decl_w2 < use_w2,
                          "codegen: child declared before being handed to parent");
    AURORA_TEST_CHECK_MSG(step.find("auto __w0 =") != std::string::npos, "codegen: root keeps __w0");
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
        {"max_lines", 2},
    };
    const std::string expr = to_code(make_text(props));
    AURORA_TEST_CHECK_MSG(expr.find(".content = \"Hello\"") != std::string::npos, "mixed: content");
    AURORA_TEST_CHECK_MSG(expr.find("au::TextAlign::Center") != std::string::npos, "mixed: text_align");
    AURORA_TEST_CHECK_MSG(expr.find("au::Color{255,0,0,255}") != std::string::npos, "mixed: color");
    AURORA_TEST_CHECK_MSG(expr.find(".max_lines = 2") != std::string::npos, "mixed: int member");
    AURORA_TEST_CHECK_MSG(expr.find(".show") == std::string::npos, "mixed: base show omitted from expression");
    AURORA_TEST_CHECK_MSG(expr.find(".font_size") == std::string::npos, "mixed: nested font_size omitted");

    const std::string step = to_code(make_text(props), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(step.find("__w0.content = \"Hello\";") != std::string::npos, "mixed step: content");
    AURORA_TEST_CHECK_MSG(step.find("__w0.show = true;") != std::string::npos, "mixed step: show");
    AURORA_TEST_CHECK_MSG(step.find("__w0.font.weight = 700;") != std::string::npos, "mixed step: font weight");
    AURORA_TEST_CHECK_MSG(step.find("__w0.width(au::px(200.0f));") != std::string::npos, "mixed step: width setter");
}

AURORA_TEST_CASE(codegen_from_widget_snapshot_compiles_shape) {
    // 便捷入口：直接对控件取快照再生成（Inspector / export_code 的同一条路径）。
    auto text = au::Text("Hi");
    const std::string code = au::serialization::to_code(text);
    AURORA_TEST_CHECK_MSG(code.find("au::Text(au::TextProps{") != std::string::npos, "widget overload emits TextProps");
    AURORA_TEST_CHECK_MSG(code.find(".content = \"Hi\"") != std::string::npos, "widget overload keeps content");
}

AURORA_TEST_CASE(codegen_designated_initializers_follow_declaration_order) {
    // GCC 的 C++ 指定初始化器必须按成员**声明序**书写，而快照 JSON 的键是字典序（nlohmann 用 std::map）。
    // 属性顺序取自 `describe` 的 `prop_descriptors`（即 `*Props` 的声明序），乱序产物直接编译失败。
    const au::Json props =
        au::Json{{"enabled", false},
                 {"padding", au::Json{{"top", 6.0F}, {"right", 12.0F}, {"bottom", 6.0F}, {"left", 12.0F}}},
                 {"corner_radius", 6.0F},
                 {"on_color", au::Json::array({255, 255, 255, 255})},
                 {"label", "OK"}};
    const std::string di = to_code(make_node("Button", props), CodeStyle::DesignatedInit);
    const std::vector<std::string> order = {".label", ".on_color", ".corner_radius", ".padding", ".enabled"};
    std::size_t prev = 0;
    for (const std::string &needle : order) {
        const std::size_t at = di.find(needle);
        AURORA_TEST_CHECK_MSG(at != std::string::npos, std::string("declaration order: ") + needle + " present");
        AURORA_TEST_CHECK_MSG(at > prev, std::string("declaration order: ") + needle + " follows its predecessor");
        prev = at;
    }
    // 子节点槽排在所有标量成员之前（`ColumnProps.children` 是聚合首成员）。
    const std::string container = to_code(
        make_node("Column", au::Json{{"gap", 8.0F}}, au::Json::array({make_button("OK")})), CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(container.find(".children") < container.find(".gap"),
                          "declaration order: children slot precedes scalar members");
}

AURORA_TEST_CASE(codegen_enum_values_canonicalized_from_self_description) {
    // 快照里的枚举取值可能来自手改 JSON 或外部 LLM（`"center"`），而声明是 `TextAlign::Center`：
    // 按 `prop_descriptors[].enum` 忽略大小写规范化，产物才写得出真实的枚举项名。
    const std::string lower = to_code(make_text(au::Json{{"content", "Hi"}, {"text_align", "center"}}));
    AURORA_TEST_CHECK_MSG(lower.find("au::TextAlign::Center") != std::string::npos,
                          "canonicalize: lowercase enum value → declared name");
    const std::string divider =
        to_code(make_node("Divider", au::Json{{"orientation", "vertical"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(divider.find("au::Orientation::Vertical") != std::string::npos,
                          "canonicalize: Divider orientation value case fixed");
    // 列表里没有的取值不猜：原样输出，交由产物编译体检暴露。
    const std::string odd = to_code(make_text(au::Json{{"content", "Hi"}, {"text_align", "Justified"}}));
    AURORA_TEST_CHECK_MSG(odd.find("au::TextAlign::Justified") != std::string::npos,
                          "canonicalize: undeclared value kept as-is, not rewritten to another enum member");
}

AURORA_TEST_CASE(codegen_font_weight_and_enum_numeric_forms) {
    // `font_weight` 的落点 `Font::weight` 是 int（CSS 100..900），自描述给的枚举名列表是 `Thin`..`Black`：
    // 数字取值必须留在 int 形态，不能拼成 `au::FontWeight::400`（那不是合法标识符）。
    const std::string weight =
        to_code(make_text(au::Json{{"content", "Hi"}, {"font_weight", "400"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(weight.find("__w0.font.weight = 400;") != std::string::npos,
                          "font_weight: CSS numeric string -> int literal");
    AURORA_TEST_CHECK_MSG(weight.find("au::FontWeight::") == std::string::npos,
                          "font_weight: does not assemble an enum member name");

    // 枚举属性（`Stack.fit` 是 `StackFit`）遇数字取值：自描述没给「序号 → 取值名」，
    // 按序号猜枚举项等于臆造 API，故省略整条属性。
    const std::string fit = to_code(make_node("Stack", au::Json{{"fit", 2}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(fit.find("fit") == std::string::npos,
                          "numeric value on an enum prop is omitted rather than guessed");
    AURORA_TEST_CHECK_MSG(fit.find("StackFit") == std::string::npos, "numeric value on an enum prop invents no enum");
}

AURORA_TEST_CASE(codegen_bare_number_for_length_prop_becomes_px) {
    // `describe` 的 `default_props` 用裸数字承载 Length（`Skeleton.height: 16.0`），手写 JSON 也常这么写；
    // `Length` 不可由 float 隐式转换，必须包成 `au::px(...)`。
    const std::string step =
        to_code(make_node("Stack", au::Json{{"width", 240.0F}, {"height", 60.0F}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(step.find("__w0.width(au::px(240.0f));") != std::string::npos,
                          "Length prop wraps a bare number as px");
    AURORA_TEST_CHECK_MSG(step.find("__w0.height(au::px(60.0f));") != std::string::npos,
                          "Length prop wraps a bare number as px (height)");
    AURORA_TEST_CHECK_MSG(step.find("width(240") == std::string::npos, "no unconvertible bare float is emitted");
}

AURORA_TEST_CASE(codegen_shadowed_size_keys_stay_with_their_own_widget) {
    // `Skeleton`/`TitleBar` 的 `serialize_props` 用同名键覆写基类写入：那是它们自己的 float 尺寸，
    // 不是基类的 `Length` 约束——发成 `w.height(au::px(...))` 会改错对象，故各走自己的公开 API。
    const std::string title =
        to_code(make_node("TitleBar", au::Json{{"height", 36.0F}, {"title", "Aurora"}}), CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(title.find("__w0.set_height(36.0f);") != std::string::npos,
                          "TitleBar height uses its own set_height");
    AURORA_TEST_CHECK_MSG(title.find("__w0.set_title(\"Aurora\");") != std::string::npos,
                          "TitleBar title uses set_title");
    AURORA_TEST_CHECK_MSG(title.find("au::px(36.0f)") == std::string::npos,
                          "TitleBar height is not treated as base-class Length");

    // `Skeleton` 的占位尺寸由成对的 `set_size(Size)` 一次给出，两键各自无写入口 → 省略，不冒充基类宽度。
    const std::string skeleton =
        to_code(make_node("Skeleton", au::Json{{"width", 120.0F}, {"height", 16.0F}, {"duration", 1.5}}),
                CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(
        skeleton.find("__w0.width(") == std::string::npos && skeleton.find("__w0.height(") == std::string::npos,
        "Skeleton width/height are not written as base-class Length setters");
    AURORA_TEST_CHECK_MSG(skeleton.find("__w0.set_duration(1.5f);") != std::string::npos,
                          "Skeleton duration uses its own set_duration");
}

AURORA_TEST_CASE(codegen_bespoke_ctor_types_drop_children_rather_than_misconstruct) {
    // `Badge(count, child)`、`LazyRow`（虚拟化：子项经 `item_builder` 生成）等类型没有通用构造入口，
    // 把子节点硬塞进初始化列表必然编译失败，故保留类型本身、省略子节点。
    const std::string badge =
        to_code(make_node("Badge", au::Json{{"count", 3}}, au::Json::array({make_text(au::Json{{"content", "new"}})})));
    AURORA_TEST_CHECK_MSG(badge.find("au::Badge") != std::string::npos, "bespoke: Badge type still appears in output");
    AURORA_TEST_CHECK_MSG(badge.find("au::Node{") == std::string::npos,
                          "bespoke: Badge children are not fed into ctor args");

    const std::string lazy_row = to_code(
        make_node("LazyRow", au::Json{{"item_count", 8}}, au::Json::array({make_text(au::Json{{"content", "row"}})})),
        CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(lazy_row.find("au::LazyRow{}") != std::string::npos,
                          "bespoke: LazyRow emits an empty construction");
    AURORA_TEST_CHECK_MSG(lazy_row.find("au::Node{") == std::string::npos,
                          "bespoke: LazyRow items build lazily and are not materialized");
}

AURORA_TEST_CASE(codegen_unconstructible_types_keep_their_real_type_name) {
    // `Provider`/`Repeater` 等类模板与 `Hero` 这类只有位置参数构造的类型，连 `au::T{}` 都编译不过。
    // 产物保留真实类型名（比替换成别的控件诚实），由告警点名「须手工补构造实参」。
    const std::string hero = to_code(make_node("Hero", au::Json{{"show", true}}));
    AURORA_TEST_CHECK_MSG(hero.find("au::Hero") != std::string::npos, "unconstructible: keeps the Hero type name");
    AURORA_TEST_CHECK_MSG(hero.find("HeroProps") == std::string::npos,
                          "unconstructible: no invented HeroProps aggregate");
    AURORA_TEST_CHECK_MSG(hero.find("au::Spacer") == std::string::npos,
                          "unconstructible: not swapped for another widget");
}

AURORA_TEST_CASE(codegen_text_input_props_route_through_setters) {
    // `TextInput` 以 `TextInputProps` 作构造参数、把值拷进 protected 字段，对外只有 `set_*()`/链式 setter：
    // 同名赋值（`w.value = ...`）取到的是同名成员函数，必须走登记的 setter 落点。
    // CJK-LITERAL: cjk-fixture - Han placeholder fed to the generator, asserted verbatim below
    const std::string step = to_code(make_node("TextInput", au::Json{{"value", "Ada"},
                                                                     {"placeholder", "姓名"},
                                                                     {"font_size", 16.0F},
                                                                     {"max_length", 40},
                                                                     {"read_only", true},
                                                                     {"obscure_text", false}}),
                                     CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(step.find("__w0.set_value(\"Ada\");") != std::string::npos, "TextInput value → set_value");
    // CJK-LITERAL: cjk-fixture - same Han placeholder expected back verbatim
    AURORA_TEST_CHECK_MSG(step.find("__w0.set_placeholder(\"姓名\");") != std::string::npos,
                          "TextInput placeholder -> set_placeholder");
    AURORA_TEST_CHECK_MSG(step.find("__w0.font_size(16.0f);") != std::string::npos,
                          "TextInput font_size -> chained setter");
    AURORA_TEST_CHECK_MSG(step.find("__w0.set_max_length(40);") != std::string::npos, "TextInput max_length → setter");
    AURORA_TEST_CHECK_MSG(step.find("__w0.set_read_only(true);") != std::string::npos, "TextInput read_only → setter");
    AURORA_TEST_CHECK_MSG(step.find("__w0.set_obscure_text(false);") != std::string::npos,
                          "TextInput obscure_text → setter");
    AURORA_TEST_CHECK_MSG(step.find("__w0.value =") == std::string::npos,
                          "TextInput never assigns the same-named member");
}

}  // namespace aurora::test_cases::itest_to_code
