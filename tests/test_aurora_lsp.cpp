// test_aurora_lsp.cpp — 覆盖 #1 aurora-lsp 的轻量分析器逻辑（completion / hover /
// diagnostics / codeAction）。直接驱动 tools/include/lsp_features.h（汇聚 schema/document/features 三层），不依赖真实
// LSP socket。 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <string>

#include "../tools/include/lsp_features.h"
#include "test_harness.h"

using aurora::tools::analyze;
using aurora::tools::ComponentSchema;
using aurora::tools::Diagnostic;
using aurora::tools::Document;
using aurora::tools::EnumSchema;
using aurora::tools::PropSchema;
using aurora::tools::Schema;

namespace {

auto make_schema() -> Schema {
    Schema s;
    ComponentSchema button;
    button.type = "Button";
    button.category = "controls";
    button.children_policy = "one";
    button.props = {
        PropSchema{
            .name = "label", .type = "std::string", .default_value = "\"\"", .required = false, .note = "按钮文字" },
        PropSchema{
            .name = "on_click", .type = "Callback", .default_value = "", .required = false, .note = "点击回调" },
        PropSchema{ .name = "align",
                    .type = "Alignment",
                    .default_value = "\"Center\"",
                    .required = false,
                    .note = "对齐方式" },
        PropSchema{ .name = "enabled", .type = "bool", .default_value = "true", .required = true, .note = "是否可用" },
    };
    s.components.push_back(std::move(button));

    EnumSchema align;
    align.name = "Alignment";
    align.values = { "Leading", "Center", "Trailing" };
    s.enums.push_back(std::move(align));
    return s;
}

// 在 text 中定位 sub 的 (line,col)，单/多行通用。
auto find_pos(const std::string &text, const std::string &sub) -> std::pair<size_t, size_t> {
    const auto p = text.find(sub);
    if (p == std::string::npos) {
        return { 0, 0 };
    }
    size_t line = 0;
    size_t col = 0;
    for (size_t i = 0; i < p; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return { line, col };
}

void test_type_completion() {
    const Schema schema = make_schema();
    const std::string doc = "au::"; // 正在输入类型名
    const Document d = analyze(doc);
    const auto items = completions(doc, d, schema, 0, 4); // 光标在 au:: 之后
    bool has_button = false;
    for (const auto &it : items) {
        if (it.label == "Button" && it.kind == "Class") {
            has_button = true;
        }
    }
    AURORA_TEST_CHECK(has_button);
}

void test_prop_completion() {
    const Schema schema = make_schema();
    const std::string doc = "au::ButtonProps{ .lab }"; // 未闭合，光标在 lab 后
    auto [l, c] = find_pos(doc, "lab");
    c += 3; // 越过 "lab"
    const Document d = analyze(doc);
    const auto items = completions(doc, d, schema, l, c);
    bool has_label = false;
    for (const auto &it : items) {
        if (it.label == "label" && it.kind == "Property") {
            has_label = true;
        }
    }
    AURORA_TEST_CHECK(has_label);
    // 已用的属性不应再出现。
    bool has_used_again = false;
    const std::string doc2 = "au::ButtonProps{ .label = \"x\" .lab }";
    auto [l2, c2] = find_pos(doc2, "lab }");
    c2 += 3;
    const Document d2 = analyze(doc2);
    const auto items2 = completions(doc2, d2, schema, l2, c2);
    for (const auto &it : items2) {
        if (it.label == "label") {
            has_used_again = true;
        }
    }
    AURORA_TEST_CHECK(!has_used_again);
}

void test_hover() {
    const Schema schema = make_schema();
    // 悬停在 au::Button 类型上。
    const std::string doc1 = "au::Button x;";
    auto [l1, c1] = find_pos(doc1, "Button");
    c1 += 2; // 落在 Button 内部
    const Document d1 = analyze(doc1);
    const auto h1 = hover(d1, schema, l1, c1);
    AURORA_TEST_REQUIRE(h1.has_value());
    AURORA_TEST_CHECK(h1->content.find("Button") != std::string::npos);

    // 悬停在 .label 属性上。
    const std::string doc2 = "au::ButtonProps{ .label = \"x\" }";
    auto [l2, c2] = find_pos(doc2, ".label");
    c2 += 2; // 落在 label 内部
    const Document d2 = analyze(doc2);
    const auto h2 = hover(d2, schema, l2, c2);
    AURORA_TEST_REQUIRE(h2.has_value());
    AURORA_TEST_CHECK(h2->content.find("label") != std::string::npos);
}

void test_diagnostics() {
    const Schema schema = make_schema();

    // 未知类型（引用形式：au::Xxx 后不接 '{'，避免误报 Font/WidgetDescriptor 等非控件结构体）。
    const std::string doc1 = "au::Frobnicate foo;";
    Document d1 = analyze(doc1);
    auto diags1 = diagnostics(d1, schema);
    bool found_type = false;
    for (const auto &dg : diags1) {
        if (dg.message.find("Frobnicate") != std::string::npos) {
            found_type = true;
        }
    }
    AURORA_TEST_CHECK(found_type);

    // 未知属性。
    const std::string doc2 = "au::ButtonProps{ .bogus = 1 }";
    Document d2 = analyze(doc2);
    auto diags2 = diagnostics(d2, schema);
    bool found_prop = false;
    for (const auto &dg : diags2) {
        if (dg.message.find("bogus") != std::string::npos) {
            found_prop = true;
        }
    }
    AURORA_TEST_CHECK(found_prop);

    // 缺失必填属性（warning）。
    const std::string doc3 = "au::ButtonProps{ .label = \"x\" }";
    Document d3 = analyze(doc3);
    auto diags3 = diagnostics(d3, schema);
    bool found_req = false;
    for (const auto &dg : diags3) {
        if (dg.severity == Diagnostic::Severity::Warning && dg.message.find("enabled") != std::string::npos) {
            found_req = true;
        }
    }
    AURORA_TEST_CHECK(found_req);

    // 非法枚举值。
    const std::string doc4 = "au::Alignment::Sideways";
    auto diags4 = validate_enum_values(doc4, schema);
    bool found_enum = false;
    for (const auto &dg : diags4) {
        if (dg.message.find("Sideways") != std::string::npos) {
            found_enum = true;
        }
    }
    AURORA_TEST_CHECK(found_enum);
}

void test_code_action() {
    const Schema schema = make_schema();
    const std::string doc = "au::ButtonProps{ .label = \"x\" }";
    const Document d = analyze(doc);
    const auto actions = code_actions(d, schema);
    AURORA_TEST_CHECK(actions.size() == 1);
    AURORA_TEST_CHECK(actions[0].title.find("Button") != std::string::npos);
    AURORA_TEST_CHECK(actions[0].edits.size() == 1);
    AURORA_TEST_CHECK(actions[0].edits[0].new_text.find("enabled") != std::string::npos);
    // 插入点应在 '}' 处。
    auto [l, c] = find_pos(doc, "}");
    AURORA_TEST_CHECK(actions[0].edits[0].line == l);
    AURORA_TEST_CHECK(actions[0].edits[0].col == c);
}

} // namespace

AURORA_TEST() {
    test_type_completion();
    test_prop_completion();
    test_hover();
    test_diagnostics();
    test_code_action();
}
