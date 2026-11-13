/// 测试类型: integration
/// 目标单元: tools/include/lsp_features.h
/// 测试说明: 驱动 aurora_lsp 的语义服务层（补全/hover/诊断/代码动作）与 schema 桥
///           （tools/include/lsp_live_schema.h）：先用合成 Schema 逐项验证链式写件形态的行为，
///           再用库 live API 验证桥与 codegen 能力位同源，不依赖真实 LSP 线程
/// 覆盖说明: 属性面单一声明形态下 `au::T(au::TProps().set_<key>(...))` 是唯一可编译的属性装载写法；
///           花括号属性块是非法形态（一条错误 + 整块改写），花括号子节点列表合法（不当属性块校验）

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "framework/aurora_test.h"
#include "lsp_features.h"
#include "lsp_live_schema.h"

namespace aurora::test_cases::itest_aurora_lsp {

using aurora::tools::analyze;
using aurora::tools::code_actions;
using aurora::tools::CompletionItem;
using aurora::tools::completions;
using aurora::tools::ComponentSchema;
using aurora::tools::Diagnostic;
using aurora::tools::diagnostics;
using aurora::tools::Document;
using aurora::tools::EnumSchema;
using aurora::tools::find_prop;
using aurora::tools::hover;
using aurora::tools::live_schema;
using aurora::tools::PropSchema;
using aurora::tools::Schema;
using aurora::tools::validate_enum_values;

namespace {

// 合成 Schema：只取足以逼出各条判据的最小面，字段形态与真表一致（写件名、C++ 拼写、能力位）。
// Button 链式 + label 必填；Image 的 C++ 类名与序列化 type 名不同；Stack 根本没有 Props 聚合；
// Column 用于 kids-first 形态（子节点列表在前、Props 在第二形参位）。
auto make_schema() -> Schema {
    Schema s;

    ComponentSchema button;
    button.type = "Button";
    button.category = "controls";
    button.children_policy = "one";
    button.cpp_type = "Button";
    button.props_type = "ButtonProps";
    button.chain_writers = true;
    button.props = {
        PropSchema{.name = "label",
                   .type = "LocalizedString",
                   .default_value = "",
                   .required = true,
                   .note = "the caption",
                   .writer = "set_label"},
        PropSchema{.name = "align",
                   .type = "Alignment",
                   .default_value = "\"Center\"",
                   .required = false,
                   .note = "Alignment",
                   .writer = "set_align"},
        PropSchema{.name = "enabled",
                   .type = "bool",
                   .default_value = "true",
                   .required = false,
                   .note = "Enabled",
                   .writer = "set_enabled"},
        // 基类布局键：值在 Widget 侧，Props 聚合没有同名成员，故无写件、不进链式补全。
        PropSchema{.name = "width",
                   .type = "Length",
                   .default_value = "",
                   .required = false,
                   .note = "layout constraint",
                   .writer = ""},
    };
    button.events = {"on_click"};
    s.components.push_back(std::move(button));

    ComponentSchema image;
    image.type = "Image";
    image.category = "media";
    image.children_policy = "none";
    image.cpp_type = "ImageView";
    image.props_type = "ImageViewProps";
    image.chain_writers = true;
    image.props = {
        PropSchema{.name = "source",
                   .type = "std::string",
                   .default_value = "\"\"",
                   .required = false,
                   .note = "image source",
                   .writer = "set_source"},
        // 派生值：由解码结果现算，没有写件（codegen 的 expressible 也拒绝发射它）。
        PropSchema{.name = "image_width",
                   .type = "int",
                   .default_value = "0",
                   .required = false,
                   .note = "decoded width",
                   .writer = ""},
    };
    s.components.push_back(std::move(image));

    ComponentSchema stack;
    stack.type = "Stack";
    stack.category = "layout";
    stack.children_policy = "many";
    stack.cpp_type = "Stack";
    stack.props_type = "";  // 没有 Props 聚合：au::StackProps 本身编译不过
    stack.chain_writers = false;
    s.components.push_back(std::move(stack));

    ComponentSchema column;
    column.type = "Column";
    column.category = "layout";
    column.children_policy = "many";
    column.cpp_type = "Column";
    column.props_type = "ColumnProps";
    column.chain_writers = true;
    // gap 在真表里非必填；夹具标必填只为把「缺必填」判据逼到 kids-first 的第二形参位，
    // 那里正是「链是不是构造实参」最容易判错的位置（向前回溯会先撞到逗号）。
    column.props = {
        PropSchema{.name = "gap",
                   .type = "float",
                   .default_value = "8.0f",
                   .required = true,
                   .note = "gap between children",
                   .writer = "set_gap"},
    };
    s.components.push_back(std::move(column));

    // 取值与 tools/include/known_enums.h 逐字一致：夹具若登记不存在的成员，补全就会插出编译不过的名字。
    EnumSchema align;
    align.name = "Alignment";
    align.values = {"TopLeft",     "TopCenter",  "TopRight",     "CenterLeft", "Center",
                    "CenterRight", "BottomLeft", "BottomCenter", "BottomRight"};
    s.enums.push_back(std::move(align));
    return s;
}

// 绝对下标 → (line,col)，0 基，与 LSP 协议一致。
auto pos_of(const std::string &text, size_t index) -> std::pair<size_t, size_t> {
    size_t line = 0;
    size_t col = 0;
    for (size_t i = 0; i < index && i < text.size(); ++i) {
        if (text.at(i) == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col};
}

// 光标落在 sub 之后（补全的触发位）。
auto pos_after(const std::string &text, const std::string &sub) -> std::pair<size_t, size_t> {
    const auto p = text.find(sub);
    return pos_of(text, p == std::string::npos ? text.size() : p + sub.size());
}

// 光标落在 sub 内部（hover 的命中位，要求 sub 至少两个字符）。
auto pos_in(const std::string &text, const std::string &sub) -> std::pair<size_t, size_t> {
    const auto p = text.find(sub);
    return pos_of(text, p == std::string::npos ? 0 : p + 2);
}

auto has_item(const std::vector<CompletionItem> &items, const std::string &label) -> bool {
    return std::ranges::any_of(items, [&label](const CompletionItem &it) { return it.label == label; });
}

auto has_item_kind(const std::vector<CompletionItem> &items, const std::string &label, const std::string &kind)
    -> bool {
    return std::ranges::any_of(
        items, [&label, &kind](const CompletionItem &it) { return it.label == label && it.kind == kind; });
}

auto has_message(const std::vector<Diagnostic> &diags, const std::string &sub) -> bool {
    return std::ranges::any_of(diags, [&sub](const Diagnostic &d) { return d.message.find(sub) != std::string::npos; });
}

auto count_severity(const std::vector<Diagnostic> &diags, Diagnostic::Severity sev) -> size_t {
    size_t n = 0;
    for (const auto &d : diags) {
        if (d.severity == sev) {
            ++n;
        }
    }
    return n;
}

auto count_missing_required(const std::vector<Diagnostic> &diags) -> size_t {
    size_t n = 0;
    for (const auto &d : diags) {
        if (d.severity == Diagnostic::Severity::Warning &&
            d.message.find("missing required property") != std::string::npos) {
            ++n;
        }
    }
    return n;
}

}  // namespace

AURORA_TEST_CASE(lsp_completes_widget_types_after_au_scope) {
    const Schema schema = make_schema();
    const std::string doc = "au::";  // 正在输入类型名
    const Document d = analyze(doc);
    auto [l, c] = pos_after(doc, "au::");
    const auto items = completions(doc, d, schema, l, c);
    AURORA_TEST_CHECK(has_item_kind(items, "Button", "Class"));
    // label 就是插入文本：Image 的类名是 au::ImageView，补出 "Image" 会编译不过。
    AURORA_TEST_CHECK(has_item_kind(items, "ImageView", "Class"));
    AURORA_TEST_CHECK(!has_item(items, "Image"));
    AURORA_TEST_CHECK(has_item_kind(items, "Stack", "Class"));
    AURORA_TEST_CHECK(has_item_kind(items, "Alignment", "Enum"));
}

AURORA_TEST_CASE(lsp_completes_chain_writers_and_matches_both_spellings) {
    const Schema schema = make_schema();

    // 打词根（lab）也要命中 set_label：schema 与诊断说的是键名，插入的是写件名。
    const std::string doc1 = "au::Button(au::ButtonProps().set_lab)";
    const Document d1 = analyze(doc1);
    auto [l1, c1] = pos_after(doc1, "set_lab");
    const auto items1 = completions(doc1, d1, schema, l1, c1);
    AURORA_TEST_CHECK(has_item_kind(items1, "set_label", "Method"));

    // 打写件名片段（set_ali）同样命中。
    const std::string doc2 = "au::Button(au::ButtonProps().set_ali)";
    const Document d2 = analyze(doc2);
    auto [l2, c2] = pos_after(doc2, "set_ali");
    const auto items2 = completions(doc2, d2, schema, l2, c2);
    AURORA_TEST_CHECK(has_item_kind(items2, "set_align", "Method"));
    AURORA_TEST_CHECK(!has_item(items2, "set_label"));

    // 空词：列出全部未用写件，已用的不再出现。
    const std::string doc3 = "au::Button(au::ButtonProps().set_label(\"x\").)";
    const Document d3 = analyze(doc3);
    auto [l3, c3] = pos_after(doc3, "\"x\").");
    const auto items3 = completions(doc3, d3, schema, l3, c3);
    AURORA_TEST_CHECK(has_item_kind(items3, "set_align", "Method"));
    AURORA_TEST_CHECK(has_item_kind(items3, "set_enabled", "Method"));
    AURORA_TEST_CHECK(!has_item(items3, "set_label"));
    // 无写件的键不进链式补全。
    AURORA_TEST_CHECK(!has_item(items3, "set_width"));
    AURORA_TEST_CHECK(!has_item(items3, "width"));
}

AURORA_TEST_CASE(lsp_completes_enum_members_in_writer_and_qualified_form) {
    const Schema schema = make_schema();

    // 枚举行写件的实参位：按该行的类型取枚举取值。
    const std::string doc1 = "au::Button(au::ButtonProps().set_align())";
    const Document d1 = analyze(doc1);
    auto [l1, c1] = pos_after(doc1, "set_align(");
    const auto items1 = completions(doc1, d1, schema, l1, c1);
    AURORA_TEST_CHECK(items1.size() == 9);
    AURORA_TEST_CHECK(has_item_kind(items1, "Center", "EnumMember"));
    AURORA_TEST_CHECK(has_item_kind(items1, "BottomRight", "EnumMember"));

    // 实参位的前缀过滤。
    const std::string doc2 = "au::Button(au::ButtonProps().set_align(Cen)";
    const Document d2 = analyze(doc2);
    auto [l2, c2] = pos_after(doc2, "Cen");
    const auto items2 = completions(doc2, d2, schema, l2, c2);
    AURORA_TEST_CHECK(has_item_kind(items2, "Center", "EnumMember"));
    AURORA_TEST_CHECK(!has_item(items2, "TopLeft"));

    // 限定写法 au::Enum:: 同样补成员。
    const std::string doc3 = "au::Alignment::";
    const Document d3 = analyze(doc3);
    auto [l3, c3] = pos_after(doc3, "au::Alignment::");
    const auto items3 = completions(doc3, d3, schema, l3, c3);
    AURORA_TEST_CHECK(items3.size() == 9);
    AURORA_TEST_CHECK(has_item_kind(items3, "TopLeft", "EnumMember"));
}

AURORA_TEST_CASE(lsp_hover_describes_type_writer_and_enum) {
    const Schema schema = make_schema();

    const std::string doc1 = "au::Button x;";
    const Document d1 = analyze(doc1);
    auto [l1, c1] = pos_in(doc1, "Button");
    const auto h1 = hover(d1, schema, l1, c1);
    AURORA_TEST_REQUIRE(h1.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK(h1.value().content.find("Button") != std::string::npos);

    // 悬停在写件上：报写件名、类型与必填标记。
    const std::string doc2 = "au::Button(au::ButtonProps().set_label(\"x\"))";
    const Document d2 = analyze(doc2);
    auto [l2, c2] = pos_in(doc2, "set_label");
    const auto h2 = hover(d2, schema, l2, c2);
    AURORA_TEST_REQUIRE(h2.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK(h2.value().content.find("set_label") != std::string::npos);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK(h2.value().content.find("required") != std::string::npos);

    // 限定枚举引用：整串被读成一个引用名，枚举部分取首段。
    const std::string doc3 = "au::Alignment::Center";
    const Document d3 = analyze(doc3);
    auto [l3, c3] = pos_in(doc3, "Alignment");
    const auto h3 = hover(d3, schema, l3, c3);
    AURORA_TEST_REQUIRE(h3.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK(h3.value().content.find("enum Alignment") != std::string::npos);
}

AURORA_TEST_CASE(lsp_diagnostics_reports_unknown_type_and_writer) {
    const Schema schema = make_schema();

    // 未知类型（引用形式：au::Xxx 后不接 '{'，避免误报 Font/WidgetDescriptor 等非控件结构体）。
    const std::string doc1 = "au::Frobnicate foo;";
    const auto diags1 = diagnostics(analyze(doc1), schema);
    AURORA_TEST_CHECK(has_message(diags1, "Frobnicate"));

    // 未知写件：链式形态下词根规则会为任何键名推出 set_<key>，故要逐键问表。
    const std::string doc2 = "au::Button(au::ButtonProps().set_label(\"x\").set_bogus(1))";
    const auto diags2 = diagnostics(analyze(doc2), schema);
    AURORA_TEST_CHECK(has_message(diags2, "has no property writer: set_bogus"));

    // 根本没有 Props 聚合的类型：au::StackProps() 不存在。
    const std::string doc3 = "au::Stack(au::StackProps())";
    const auto diags3 = diagnostics(analyze(doc3), schema);
    AURORA_TEST_CHECK(has_message(diags3, "has no Props aggregate"));

    // 限定枚举引用不是未知类型（首段回落）。
    const std::string doc4 = "au::Alignment::Center";
    const auto diags4 = diagnostics(analyze(doc4), schema);
    AURORA_TEST_CHECK(!has_message(diags4, "Unknown Aurora type"));

    // C++ 类名与序列化 type 名不同的控件，两种拼写都认。
    const std::string doc5 = "au::ImageView(au::ImageViewProps().set_source(\"\"))";
    const auto diags5 = diagnostics(analyze(doc5), schema);
    AURORA_TEST_CHECK(diags5.empty());
}

AURORA_TEST_CASE(lsp_diagnostics_keeps_widget_side_keys_out_of_props) {
    const Schema schema = make_schema();
    // width 是基类布局键：值在 Widget 侧，写进 Props 链编译不过，要指引到构造后写入。
    const std::string doc = "au::Button(au::ButtonProps().set_label(\"x\").set_width(0))";
    const auto diags = diagnostics(analyze(doc), schema);
    AURORA_TEST_CHECK(has_message(diags, "lives on the widget"));
    AURORA_TEST_CHECK(has_message(diags, "width"));
    AURORA_TEST_CHECK(count_missing_required(diags) == 0);
}

AURORA_TEST_CASE(lsp_diagnostics_missing_required_only_for_closed_construction) {
    const Schema schema = make_schema();

    // 闭合且作为构造实参：缺必填报 warning。
    const std::string doc1 = "au::Button(au::ButtonProps())";
    const auto diags1 = diagnostics(analyze(doc1), schema);
    AURORA_TEST_CHECK(count_missing_required(diags1) == 1);
    AURORA_TEST_CHECK(has_message(diags1, "missing required property: label"));

    // 独立变量：构造后再配置，不算缺必填。
    const std::string doc2 = "auto p = au::ButtonProps();";
    const auto diags2 = diagnostics(analyze(doc2), schema);
    AURORA_TEST_CHECK(diags2.empty());

    // 外层构造未闭合、链本身已闭合：仍在输入，缺必填判据等整条构造闭合后再算。
    const std::string doc3 = "au::Button(au::ButtonProps().set_enabled(true)";
    const auto diags3 = diagnostics(analyze(doc3), schema);
    AURORA_TEST_CHECK(count_missing_required(diags3) == 0);

    // 链本身未闭合（写件实参还没写完）：同样不报。
    const std::string doc4 = "au::Button(au::ButtonProps().set_enabled(";
    AURORA_TEST_CHECK(count_missing_required(diagnostics(analyze(doc4), schema)) == 0);
}

AURORA_TEST_CASE(lsp_diagnostics_accepts_kids_first_props_argument) {
    const Schema schema = make_schema();
    // 子节点列表在前、Props 在第二形参位：链仍然是构造实参（向前回溯会先撞到逗号而判否）。
    const std::string doc = "au::Column({ au::Button(au::ButtonProps().set_label(\"A\")) }, au::ColumnProps())";
    const Document d = analyze(doc);
    const auto diags = diagnostics(d, schema);
    AURORA_TEST_CHECK(count_missing_required(diags) == 1);
    AURORA_TEST_CHECK(has_message(diags, "Widget Column missing required property: gap"));

    const auto actions = code_actions(d, schema);
    AURORA_TEST_REQUIRE(actions.size() == 1);
    if (actions.empty() || actions[0].edits.empty()) {
        return;
    }
    AURORA_TEST_CHECK(actions[0].edits.size() == 1);
    AURORA_TEST_CHECK(actions[0].edits[0].new_text == ".set_gap(8.0f)");
}

AURORA_TEST_CASE(lsp_brace_child_list_is_not_a_property_block) {
    const Schema schema = make_schema();
    // 花括号子节点列表是合法形态：不装属性，也就不该报「不接受指定初始化器」或「缺必填」。
    const std::string doc = "au::Column({ au::Button(au::ButtonProps().set_label(\"A\")) })";
    const Document d = analyze(doc);
    AURORA_TEST_CHECK(diagnostics(d, schema).empty());
    AURORA_TEST_CHECK(code_actions(d, schema).empty());
}

AURORA_TEST_CASE(lsp_brace_property_block_is_error_with_whole_block_rewrite) {
    const Schema schema = make_schema();

    // Props 拼写：整块改写成链；一条错误即可，逐键报错只是噪声。
    const std::string doc1 = "au::ButtonProps{ .label = \"x\", .enabled = true }";
    const Document d1 = analyze(doc1);
    const auto diags1 = diagnostics(d1, schema);
    AURORA_TEST_CHECK(count_severity(diags1, Diagnostic::Severity::Error) == 1);
    AURORA_TEST_CHECK(has_message(diags1, "does not accept designated initializers"));
    // 非法形态不再补 `.key = ` 碎片：补出来的东西编译不过，交给诊断与整块改写。
    auto [l1, c1] = pos_after(doc1, ".label");
    AURORA_TEST_CHECK(completions(doc1, d1, schema, l1, c1).empty());

    const auto actions1 = code_actions(d1, schema);
    AURORA_TEST_REQUIRE(actions1.size() == 1);
    if (actions1.empty() || actions1[0].edits.empty()) {
        return;
    }
    AURORA_TEST_CHECK(actions1[0].title.find("Rewrite") != std::string::npos);
    const auto &e1 = actions1[0].edits[0];
    AURORA_TEST_CHECK(e1.line == 0);
    AURORA_TEST_CHECK(e1.col == 0);
    AURORA_TEST_CHECK(e1.end_line == 0);
    AURORA_TEST_CHECK(e1.end_col == doc1.size());  // 替换整块，含收尾 '}'
    AURORA_TEST_CHECK(e1.new_text == "au::ButtonProps().set_label(\"x\").set_enabled(true)");

    // 控件拼写：改写后仍构造同一个控件，故外层补上控件构造。
    const std::string doc2 = "au::Button{ .label = \"x\" }";
    const auto actions2 = code_actions(analyze(doc2), schema);
    AURORA_TEST_REQUIRE(actions2.size() == 1);
    if (actions2.empty() || actions2[0].edits.empty()) {
        return;
    }
    AURORA_TEST_CHECK(actions2[0].edits[0].new_text == "au::Button(au::ButtonProps().set_label(\"x\"))");

    // 没有 Props 聚合的类型：报「构造后装载」，且无从改写。
    const std::string doc3 = "au::Stack{ .fit = 1 }";
    const auto diags3 = diagnostics(analyze(doc3), schema);
    AURORA_TEST_CHECK(has_message(diags3, "load its properties after construction"));
    AURORA_TEST_CHECK(code_actions(analyze(doc3), schema).empty());
}

AURORA_TEST_CASE(lsp_code_action_appends_missing_required_writers) {
    const Schema schema = make_schema();
    const std::string doc = "au::Button(au::ButtonProps())";
    const Document d = analyze(doc);
    const auto actions = code_actions(d, schema);
    AURORA_TEST_REQUIRE(actions.size() == 1);
    if (actions.empty() || actions[0].edits.empty()) {
        return;
    }
    AURORA_TEST_CHECK(actions[0].title.find("Button") != std::string::npos);
    AURORA_TEST_CHECK(actions[0].edits.size() == 1);
    const auto &e = actions[0].edits[0];
    AURORA_TEST_CHECK(e.new_text == ".set_label(\"\")");
    // 纯插入：起止同点，落在 Props 链尾（外层构造的 ')' 之前）。
    auto [l, c] = pos_after(doc, "au::ButtonProps()");
    AURORA_TEST_CHECK(e.line == l);
    AURORA_TEST_CHECK(e.col == c);
    AURORA_TEST_CHECK(e.end_line == l);
    AURORA_TEST_CHECK(e.end_col == c);
}

AURORA_TEST_CASE(lsp_diagnostics_reports_bad_enum_value) {
    const Schema schema = make_schema();
    AURORA_TEST_CHECK(has_message(validate_enum_values("au::Alignment::Sideways", schema), "Sideways"));
    // 合法取值不报。
    AURORA_TEST_CHECK(validate_enum_values("au::Alignment::BottomCenter", schema).empty());
}

AURORA_TEST_CASE(lsp_live_schema_writers_match_codegen) {
    const Schema live = live_schema();

    const auto *button = live.find_component("Button");
    AURORA_TEST_REQUIRE(button != nullptr);
    if (button == nullptr) {
        return;
    }
    AURORA_TEST_CHECK(button->chain_writers);
    AURORA_TEST_CHECK(button->props_type == "ButtonProps");
    const auto *label = find_prop(*button, "label");
    AURORA_TEST_REQUIRE(label != nullptr);
    if (label == nullptr) {
        return;
    }
    AURORA_TEST_CHECK(label->writer == "set_label");
    AURORA_TEST_CHECK(label->required);
    // 回调是事件，不是属性行。
    AURORA_TEST_CHECK(find_prop(*button, "on_click") == nullptr);

    // 基类布局键在真表里也在 props union 中，但一律无写件。
    const std::string doc = "au::Button(au::ButtonProps().)";
    const auto items = completions(doc, analyze(doc), live, 0, doc.size() - 1);
    AURORA_TEST_CHECK(has_item_kind(items, "set_label", "Method"));
    AURORA_TEST_CHECK(!has_item(items, "set_width"));
    AURORA_TEST_CHECK(!has_item(items, "set_height"));
    AURORA_TEST_CHECK(!has_item(items, "set_show"));
    AURORA_TEST_CHECK(!has_item(items, "set_overflow"));

    // 运行态键：滚动位置由交互产生，构造期无从写入，codegen 与 LSP 同时拒绝。
    const auto *lazy = live.find_component("LazyRow");
    AURORA_TEST_REQUIRE(lazy != nullptr);
    if (lazy == nullptr) {
        return;
    }
    const auto *offset = find_prop(*lazy, "offset");
    AURORA_TEST_REQUIRE(offset != nullptr);
    if (offset == nullptr) {
        return;
    }
    AURORA_TEST_CHECK(offset->writer.empty());

    // C++ 拼写与序列化 type 名不同的控件：补全插的是类名，查表两种拼写都认。
    const auto *image = live.find_component("ImageView");
    AURORA_TEST_REQUIRE(image != nullptr);
    if (image == nullptr) {
        return;
    }
    AURORA_TEST_CHECK(image->type == "Image");
    AURORA_TEST_CHECK(image->props_type == "ImageViewProps");
    const auto *derived = find_prop(*image, "image_width");
    AURORA_TEST_REQUIRE(derived != nullptr);
    if (derived == nullptr) {
        return;
    }
    AURORA_TEST_CHECK(derived->writer.empty());
}

AURORA_TEST_CASE(lsp_live_schema_chain_diagnostics_are_clean) {
    const Schema live = live_schema();
    size_t checked = 0;
    for (const auto &c : live.components) {
        if (!c.chain_writers) {
            continue;
        }
        // 把桥给出的每个写件都装进构造表达式：诊断若报「没有这个写件」，说明桥与属性表漂了。
        std::string doc = "au::" + c.cpp_type + "(au::" + c.props_type + "()";
        for (const auto &p : c.props) {
            if (!p.writer.empty()) {
                doc += "." + p.writer + "(0)";
            }
        }
        doc += "))";
        const auto diags = diagnostics(analyze(doc), live);
        if (!diags.empty()) {
            AURORA_TEST_PRINTF("chain diagnostics not clean for %s: %s\n", c.type.c_str(),
                               diags.front().message.c_str());
        }
        AURORA_TEST_CHECK(diags.empty());
        ++checked;
    }
    // 名单非空才算真检过（当前 18 型；名单增删时这条下界不必跟着改）。
    AURORA_TEST_CHECK(checked >= 18);
}

}  // namespace aurora::test_cases::itest_aurora_lsp
