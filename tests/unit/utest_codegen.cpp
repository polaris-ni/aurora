/// 测试类型: unit
/// 目标单元: include/aurora/widget/codegen.h
/// 测试说明: codegen 序列化→C++ 代码生成单元测试（覆盖 emit_prop_value 各值类型分支、
///           to_code 扁平容器/Grid 分支、三种 CodeStyle、枚举真实/合成键名、FontWeight 与
///           TextDecoration 边界、escape_cpp_string、unknown 跳过、StepByStep 子项、
///           to_code(Widget) 便捷重载）。
///

// codegen.h — 把序列化 widget 树 JSON 反向生成为 Aurora C++ 源码。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

#include <string>

#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_codegen {

using au::serialization::CodeStyle;
using au::serialization::from_json;
using au::serialization::register_core_widgets;
using au::serialization::to_code;

static auto leaf(const std::string &type, const Json &props) -> Json {
    Json n = Json::object();
    n["type"] = type;
    n["props"] = props;
    return n;
}

static void test_emit_prop_value_types() {
    // 枚举字符串（经 enum_type_for_key 分派）
    Json b = leaf("Button", Json::object({{"label", "OK"}, {"text_align", "Center"}}));
    const std::string cb = to_code(b);
    AURORA_TEST_CHECK_MSG(cb.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: leaf -> au::Button(au::ButtonProps{");
    AURORA_TEST_CHECK_MSG(cb.find(".label = \"OK\"") != std::string::npos, "codegen: string prop");
    AURORA_TEST_CHECK_MSG(cb.find(".text_align = TextAlign::Center") != std::string::npos,
                          "codegen: enum string -> TextAlign::Center");

    // font_weight：数值字符串 + 数值通道
    Json fw1 = leaf("Text", Json::object({{"font_weight", "700"}}));
    AURORA_TEST_CHECK_MSG(to_code(fw1).find("FontWeight::Bold") != std::string::npos,
                          "codegen: font_weight string 700 -> Bold");
    Json fw2 = leaf("Text", Json::object({{"font_weight", 300}}));
    AURORA_TEST_CHECK_MSG(to_code(fw2).find("font_weight = 300") != std::string::npos,
                          "codegen: font_weight number -> raw int (only string path maps enum)");

    // Length 特殊字符串
    Json len = leaf("SizedBox", Json::object({{"width", "auto"}, {"height", "fill"}}));
    const std::string cl = to_code(len);
    AURORA_TEST_CHECK_MSG(cl.find("au::auto_length()") != std::string::npos, "codegen: 'auto' -> auto_length()");
    AURORA_TEST_CHECK_MSG(cl.find("au::fill()") != std::string::npos, "codegen: 'fill' -> fill()");

    // bool
    Json flag = leaf("Checkbox", Json::object({{"checked", true}}));
    AURORA_TEST_CHECK_MSG(to_code(flag).find("= true") != std::string::npos, "codegen: bool true");

    // number int / float
    Json num = leaf("Slider", Json::object({{"min", 0}, {"max", 1.5}}));
    const std::string cn = to_code(num);
    AURORA_TEST_CHECK_MSG(cn.find("= 0") != std::string::npos, "codegen: int 0");
    AURORA_TEST_CHECK_MSG(cn.find("1.5f") != std::string::npos, "codegen: float 1.5f");

    // array: Length ["px"/"percent", N]
    Json px = leaf("Padding", Json::object({{"pad", Json::array({"px", 12})}}));
    AURORA_TEST_CHECK_MSG(to_code(px).find("au::px(12") != std::string::npos, "codegen: [px,12] -> px(12)");
    Json pct = leaf("Padding", Json::object({{"pad", Json::array({"percent", 0.5F})}}));
    AURORA_TEST_CHECK_MSG(to_code(pct).find("au::percent(0.5") != std::string::npos,
                          "codegen: [percent,0.5] -> percent(0.5)");

    // array: Color [r,g,b,a]
    Json col = leaf("Container", Json::object({{"color", Json::array({1, 2, 3, 4})}}));
    AURORA_TEST_CHECK_MSG(to_code(col).find("Color{1,2,3,4}") != std::string::npos, "codegen: Color{1,2,3,4}");

    // array: text_decoration
    Json dec = leaf("Text", Json::object({{"text_decoration", Json::array({"Underline", "LineThrough"})}}));
    const std::string cd = to_code(dec);
    AURORA_TEST_CHECK_MSG(cd.find("TextDecoration::Underline") != std::string::npos, "codegen: decoration Underline");
    AURORA_TEST_CHECK_MSG(cd.find("TextDecoration::LineThrough") != std::string::npos,
                          "codegen: decoration LineThrough");

    // object: EdgeInsets
    Json insets = leaf(
        "Padding",
        Json::object({{"insets", Json::object({{"top", 1.0F}, {"right", 2.0F}, {"bottom", 3.0F}, {"left", 4.0F}})}}));
    AURORA_TEST_CHECK_MSG(to_code(insets).find("EdgeInsets{") != std::string::npos, "codegen: EdgeInsets{...}");

    // object: legacy Length {value,unit}
    Json legacy = leaf("SizedBox", Json::object({{"w", Json::object({{"value", 8.0F}, {"unit", "pct"}})}}));
    AURORA_TEST_CHECK_MSG(to_code(legacy).find("au::percent(8.000000)") != std::string::npos,
                          "codegen: legacy Length pct -> percent(8.000000)");
}

static void test_flat_container_and_grid() {
    Json col = Json::object();
    col["type"] = "Column";
    col["children"] =
        Json::array({leaf("Button", Json::object({{"label", "A"}})), leaf("Button", Json::object({{"label", "B"}}))});
    const std::string cc = to_code(col);
    AURORA_TEST_CHECK_MSG(cc.find("au::Column{") != std::string::npos, "codegen: Column flat open");
    AURORA_TEST_CHECK_MSG(cc.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: Column child Button(ButtonProps{");

    // Grid 含 columns>1（触发 Grid 特殊分支）
    Json grid = Json::object();
    grid["type"] = "Grid";
    grid["props"] = Json::object({{"columns", 2}});
    grid["children"] =
        Json::array({leaf("Button", Json::object({{"label", "A"}})), leaf("Button", Json::object({{"label", "B"}}))});
    const std::string cg = to_code(grid);
    AURORA_TEST_CHECK_MSG(cg.find("au::Grid{") != std::string::npos, "codegen: Grid open");
    AURORA_TEST_CHECK_MSG(cg.find(",\n    ") != std::string::npos && cg.find('2') != std::string::npos,
                          "codegen: Grid columns>1 appended");
}

static void test_code_styles() {
    Json b = leaf("Button", Json::object({{"label", "OK"}, {"text_align", "Center"}}));

    // DesignatedInit
    const std::string di = to_code(b, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(di.find("au::Button{") != std::string::npos, "codegen: DI au::Button{");
    AURORA_TEST_CHECK_MSG(di.find(".label = \"OK\"") != std::string::npos, "codegen: DI label");

    // StepByStep（生成 __w0 变量 + 分步赋值）
    const std::string sb = to_code(b, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb.find("auto __w0 = au::Button{};") != std::string::npos, "codegen: SB var declaration");
    AURORA_TEST_CHECK_MSG(sb.find("__w0.label = \"OK\"") != std::string::npos, "codegen: SB assignment");

    // Fluent 默认与显式 Fluent 一致
    AURORA_TEST_CHECK_MSG(to_code(b, CodeStyle::Fluent) == to_code(b), "codegen: Fluent explicit == default");
}

static void test_cpp_class_mapping() {
    // Image -> ImageView / ImageViewProps（类名与 type 名不同）
    Json img = leaf("Image", Json::object({{"src", "x.png"}}));
    const std::string ci = to_code(img);
    AURORA_TEST_CHECK_MSG(ci.find("au::ImageView(au::ImageViewProps{") != std::string::npos,
                          "codegen: Image -> ImageView/ImageViewProps");
}

// ---------- 枚举键名全覆盖（真实键名 = to_json 产出；合成键名 = 手工 JSON 历史） ----------

static void test_enum_keys_full_coverage() {
    // to_json 真实产出的属性键名
    AURORA_TEST_CHECK_MSG(to_code(leaf("Stack", Json::object({{"alignment", "TopLeft"}})))
                                  .find("Alignment::TopLeft") != std::string::npos,
                          "codegen: real key alignment -> Alignment::TopLeft");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"overflow", "Ellipsis"}})))
                                  .find("TextOverflow::Ellipsis") != std::string::npos,
                          "codegen: real key overflow -> TextOverflow::Ellipsis");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Drawer", Json::object({{"side", "Right"}})))
                                  .find("DrawerSide::Right") != std::string::npos,
                          "codegen: real key side -> DrawerSide::Right");
    AURORA_TEST_CHECK_MSG(to_code(leaf("ToastHost", Json::object({{"position", "Top"}})))
                                  .find("ToastPosition::Top") != std::string::npos,
                          "codegen: real key position -> ToastPosition::Top");
    // 合成（历史）键名
    Json row = leaf("Row", Json::object({{"main_axis_alignment", "SpaceBetween"},
                                         {"cross_axis_alignment", "Stretch"},
                                         {"main_axis_size", "Max"}}));
    const std::string cr = to_code(row);
    AURORA_TEST_CHECK_MSG(cr.find("MainAxisAlignment::SpaceBetween") != std::string::npos,
                          "codegen: main_axis_alignment -> MainAxisAlignment");
    AURORA_TEST_CHECK_MSG(cr.find("CrossAxisAlignment::Stretch") != std::string::npos,
                          "codegen: cross_axis_alignment -> CrossAxisAlignment");
    AURORA_TEST_CHECK_MSG(cr.find("MainAxisSize::Max") != std::string::npos, "codegen: main_axis_size -> MainAxisSize");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Stack", Json::object({{"stack_fit", "Expand"}})))
                                  .find("StackFit::Expand") != std::string::npos,
                          "codegen: stack_fit -> StackFit::Expand");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Image", Json::object({{"box_fit", "Cover"}})))
                                  .find("BoxFit::Cover") != std::string::npos,
                          "codegen: box_fit -> BoxFit::Cover");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Column", Json::object({{"overflow_strategy", "Scroll"}})))
                                  .find("OverflowStrategy::Scroll") != std::string::npos,
                          "codegen: overflow_strategy -> OverflowStrategy::Scroll");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"font_style", "Italic"}})))
                                  .find("FontStyle::Italic") != std::string::npos,
                          "codegen: font_style -> FontStyle::Italic");
    // 字符串形态的 decoration 键（数组形态另走 emit_text_decoration）
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"decoration", "Underline"}})))
                                  .find("TextDecoration::Underline") != std::string::npos,
                          "codegen: string decoration -> TextDecoration::Underline");
}

static void test_enum_ambiguous_keys_stay_string() {
    // fit / orientation 无法按键消歧（StackFit vs BoxFit、Orientation vs SplitterOrientation），
    // 刻意保持字符串输出，等属性声明类型透传后再升级——测试锁住这一现状。
    AURORA_TEST_CHECK_MSG(to_code(leaf("Stack", Json::object({{"fit", "Cover"}})))
                                  .find("\"Cover\"") != std::string::npos,
                          "codegen: ambiguous key fit stays a string literal");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Splitter", Json::object({{"orientation", "Horizontal"}})))
                                  .find("\"Horizontal\"") != std::string::npos,
                          "codegen: ambiguous key orientation stays a string literal");
}

static void test_font_weight_boundaries() {
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"font_weight", "400"}})))
                                  .find("FontWeight::Normal") != std::string::npos,
                          "codegen: font_weight 400 -> Normal");
    // 越界值走 default 分支
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"font_weight", "950"}})))
                                  .find("FontWeight::Normal") != std::string::npos,
                          "codegen: font_weight 950 -> Normal fallback");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"font_weight", "800"}})))
                                  .find("FontWeight::ExtraBold") != std::string::npos,
                          "codegen: font_weight 800 -> ExtraBold");
}

static void test_text_decoration_forms() {
    // 字符串形态
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"text_decoration", "Overline"}})))
                                  .find("TextDecoration::Overline") != std::string::npos,
                          "codegen: decoration string Overline");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"text_decoration", "None"}})))
                                  .find("TextDecoration::None") != std::string::npos,
                          "codegen: decoration string None");
    // 数组形态：None 短路、组合 " | "、空数组、含非字符串项
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"text_decoration", Json::array({"None", "Underline"})}})))
                                  .find("TextDecoration::None") != std::string::npos,
                          "codegen: decoration array None short-circuits");
    Json both = leaf("Text", Json::object({{"text_decoration", Json::array({"Overline", "LineThrough"})}}));
    const std::string cb = to_code(both);
    AURORA_TEST_CHECK_MSG(cb.find("TextDecoration::Overline") != std::string::npos &&
                              cb.find("TextDecoration::LineThrough") != std::string::npos && cb.find(" | ") != std::string::npos,
                          "codegen: decoration array joins with |");
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"text_decoration", Json::array()}})))
                                  .find("TextDecoration::None") != std::string::npos,
                          "codegen: empty decoration array -> None");
    AURORA_TEST_CHECK_MSG(
        to_code(leaf("Text", Json::object({{"text_decoration", Json::array({1, "Underline"})}})))
            .find("TextDecoration::Underline") != std::string::npos,
        "codegen: non-string decoration items skipped");
    // 数组里只有未知项 -> None
    AURORA_TEST_CHECK_MSG(to_code(leaf("Text", Json::object({{"text_decoration", Json::array({"Bogus"})}})))
                                  .find("TextDecoration::None") != std::string::npos,
                          "codegen: unknown-only decoration array -> None");
}

static void test_escape_cpp_string() {
    Json esc = leaf("Button", Json::object({{"label", "a\"b\\c\nd\te\rf"}}));
    const std::string code = to_code(esc);
    AURORA_TEST_CHECK_MSG(code.find("\\\"") != std::string::npos, "codegen: escape double quote");
    AURORA_TEST_CHECK_MSG(code.find("\\\\") != std::string::npos, "codegen: escape backslash");
    AURORA_TEST_CHECK_MSG(code.find("\\n") != std::string::npos, "codegen: escape newline");
    AURORA_TEST_CHECK_MSG(code.find("\\t") != std::string::npos, "codegen: escape tab");
    AURORA_TEST_CHECK_MSG(code.find("\\r") != std::string::npos, "codegen: escape carriage return");
}

static void test_unknown_value_skipped() {
    // 数组长度 3 / 未知对象两种形态均落到 "/* unknown */"，emit_props 应跳过
    Json unk = leaf("Box", Json::object({{"mystery", Json::array({1, 2, 3})}, {"cfg", Json::object({{"a", 1}})}}));
    const std::string code = to_code(unk);
    AURORA_TEST_CHECK_MSG(code.find("unknown") == std::string::npos, "codegen: unknown values skipped by emit_props");
}

static void test_step_by_step_children_flat_and_nested() {
    // 扁平容器（Column）：children 属性直接赋值 __wN.children = { ... }
    Json col = Json::object();
    col["type"] = "Column";
    col["props"] = Json::object();
    col["children"] = Json::array({leaf("Button", Json::object({{"label", "A"}}))});
    const std::string cf = to_code(col, CodeStyle::StepByStep);
    // 注意：子项的声明语句先写入 os，再返回变量名，故 "{ " 后紧跟子项声明而非直接是 __w1。
    AURORA_TEST_CHECK_MSG(cf.find("__w0.children = {") != std::string::npos &&
                              cf.find("auto __w1 = au::Button{};") != std::string::npos && cf.find("__w1") != std::string::npos,
                          "codegen: SB flat container assigns .children");

    // 非扁平容器（Padding）：无 .children 前缀
    Json pad = Json::object();
    pad["type"] = "Padding";
    pad["props"] = Json::object();
    pad["children"] = Json::array({leaf("Button", Json::object({{"label", "A"}}))});
    const std::string cn = to_code(pad, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(cn.find("__w0 = {") != std::string::npos && cn.find("__w0.children") == std::string::npos,
                          "codegen: SB non-flat container assigns bare brace list");
}

static void test_designated_init_children_and_indent() {
    Json col = Json::object();
    col["type"] = "Column";
    col["props"] = Json::object();
    col["children"] = Json::array({leaf("Button", Json::object({{"label", "A"}})),
                                   leaf("Button", Json::object({{"label", "B"}}))});
    const std::string di = to_code(col, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(di.find(".children = {\n") != std::string::npos, "codegen: DI opens .children list");
    AURORA_TEST_CHECK_MSG(di.find("\n    au::Button{") != std::string::npos,
                          "codegen: DI nests children with 4-space indent");

    // Grid 在 DesignatedInit 下不追加列数（当前行为锁定）
    Json grid = Json::object();
    grid["type"] = "Grid";
    grid["props"] = Json::object({{"columns", 2}});
    grid["children"] = Json::array({leaf("Button", Json::object({{"label", "A"}}))});
    const std::string dg = to_code(grid, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(dg.find("au::Grid{") != std::string::npos, "codegen: DI Grid opens");
    // Fluent 会追加独立列数行 ",\n    2"，DI 只以 .columns = 2 形式出现
    AURORA_TEST_CHECK_MSG(dg.find(",\n    2") == std::string::npos,
                          "codegen: DI Grid does not append standalone column count");
}

static void test_fluent_indent_parameter() {
    Json col = Json::object();
    col["type"] = "Column";
    col["props"] = Json::object();
    col["children"] = Json::array({leaf("Button", Json::object({{"label", "A"}}))});
    // indent=1 -> 子项缩进 (1+2)*4 = 12 空格
    const std::string ci = to_code(col, 1);
    AURORA_TEST_CHECK_MSG(ci.find("\n            au::Button(") != std::string::npos,
                          "codegen: fluent indent parameter controls nesting");
}

static void test_to_code_widget_overload() {
    register_core_widgets();
    auto made = from_json(leaf("Button", Json::object({{"label", "Hi"}})));
    AURORA_TEST_REQUIRE(made.ok());
    const std::string code = to_code(*made.value());
    AURORA_TEST_CHECK_MSG(code.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: to_code(Widget) emits Button constructor");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"Hi\"") != std::string::npos, "codegen: to_code(Widget) keeps label");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_codegen ===\n");
    test_emit_prop_value_types();
    test_flat_container_and_grid();
    test_code_styles();
    test_cpp_class_mapping();
    test_enum_keys_full_coverage();
    test_enum_ambiguous_keys_stay_string();
    test_font_weight_boundaries();
    test_text_decoration_forms();
    test_escape_cpp_string();
    test_unknown_value_skipped();
    test_step_by_step_children_flat_and_nested();
    test_designated_init_children_and_indent();
    test_fluent_indent_parameter();
    test_to_code_widget_overload();
}

}  // namespace aurora::test_cases::utest_codegen
