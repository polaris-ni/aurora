/// 测试类型: unit
/// 目标单元: include/aurora/widget/codegen.h
/// 测试说明: codegen 序列化→C++ 代码生成单元测试（覆盖 emit_prop_value 各值类型分支、
///           to_code 扁平容器/Grid 分支、三种 CodeStyle）。
///

// codegen.h — 把序列化 widget 树 JSON 反向生成为 Aurora C++ 源码。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

#include <string>

#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_codegen {

using au::serialization::to_code;
using au::serialization::CodeStyle;

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
    Json insets = leaf("Padding", Json::object({{"insets", Json::object({{"top", 1.0F},
                                                                          {"right", 2.0F},
                                                                          {"bottom", 3.0F},
                                                                          {"left", 4.0F}})}}));
    AURORA_TEST_CHECK_MSG(to_code(insets).find("EdgeInsets{") != std::string::npos,
                          "codegen: EdgeInsets{...}");

    // object: legacy Length {value,unit}
    Json legacy = leaf("SizedBox", Json::object({{"w", Json::object({{"value", 8.0F}, {"unit", "pct"}})}}));
    AURORA_TEST_CHECK_MSG(to_code(legacy).find("au::percent(8.000000)") != std::string::npos,
                          "codegen: legacy Length pct -> percent(8.000000)");
}

static void test_flat_container_and_grid() {
    Json col = Json::object();
    col["type"] = "Column";
    col["children"] = Json::array(
        {leaf("Button", Json::object({{"label", "A"}})), leaf("Button", Json::object({{"label", "B"}}))});
    const std::string cc = to_code(col);
    AURORA_TEST_CHECK_MSG(cc.find("au::Column{") != std::string::npos, "codegen: Column flat open");
    AURORA_TEST_CHECK_MSG(cc.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: Column child Button(ButtonProps{");

    // Grid 含 columns>1（触发 Grid 特殊分支）
    Json grid = Json::object();
    grid["type"] = "Grid";
    grid["props"] = Json::object({{"columns", 2}});
    grid["children"] = Json::array(
        {leaf("Button", Json::object({{"label", "A"}})), leaf("Button", Json::object({{"label", "B"}}))});
    const std::string cg = to_code(grid);
    AURORA_TEST_CHECK_MSG(cg.find("au::Grid{") != std::string::npos, "codegen: Grid open");
    AURORA_TEST_CHECK_MSG(cg.find(",\n    ") != std::string::npos && cg.find("2") != std::string::npos,
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
    AURORA_TEST_CHECK_MSG(sb.find("auto __w0 = au::Button{};") != std::string::npos,
                          "codegen: SB var declaration");
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

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_codegen ===\n");
    test_emit_prop_value_types();
    test_flat_container_and_grid();
    test_code_styles();
    test_cpp_class_mapping();
}

}  // namespace aurora::test_cases::utest_codegen
