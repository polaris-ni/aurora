// test_prop_validation.cpp — 属性约束验证测试：validate_prop<T> 各特化 + Diagnostics 报告 + Widget::validate_props。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/diagnostics.h"
#include "aurora/widget/descriptor.h"

#include "test_harness.h"

using aurora::Color;
using aurora::Diagnostics;
using aurora::EdgeInsets;
using aurora::Error;
using aurora::ErrorCode;
using aurora::Json;
using aurora::Length;
using aurora::LengthKind;
using aurora::LocalizedString;
using aurora::PropDescriptor;
using aurora::Text;

// ---- validate_prop<Color> ----
static void test_validate_color() {
    PropDescriptor desc{ .name = "color" };

    // 合法：4 元素数组，值在 0-255
    {
        Json j = Json::array({ 255, 128, 0, 255 });
        auto r = validate_prop<Color>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<Color>: valid [255,128,0,255]");
        AURORA_TEST_CHECK_MSG(r.ok() && r.value().m_r == 255 && r.value().m_g == 128,
                              "validate_prop<Color>: correct value");
    }
    // 非法：数组太短
    {
        Json j = Json::array({ 255, 128 });
        auto r = validate_prop<Color>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<Color>: short array rejected");
        AURORA_TEST_CHECK_MSG(!r.ok() && r.error().code_enum == ErrorCode::WidgetInvalidProp,
                              "validate_prop<Color>: correct ErrorCode");
    }
    // 非法：值超出范围
    {
        Json j = Json::array({ 256, 0, 0, 255 });
        auto r = validate_prop<Color>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<Color>: out-of-range value rejected");
    }
    // 非法：非数组
    {
        Json j = "not-a-color";
        auto r = validate_prop<Color>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<Color>: non-array rejected");
    }
}

// ---- validate_prop<float> ----
static void test_validate_float() {
    // 有范围约束：min_value="0", max_value="100"
    PropDescriptor desc{ .name = "opacity", .min_value = "0", .max_value = "100" };
    {
        Json j = 50.0f;
        auto r = validate_prop<float>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<float>: 50 in [0,100]");
    }
    {
        Json j = -1.0f;
        auto r = validate_prop<float>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<float>: -1 below min");
        AURORA_TEST_CHECK_MSG(!r.ok() && r.error().code_enum == ErrorCode::WidgetPropConstraintViolated,
                              "validate_prop<float>: correct ErrorCode for range violation");
    }
    {
        Json j = 101.0f;
        auto r = validate_prop<float>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<float>: 101 above max");
    }
    // 无范围约束
    PropDescriptor desc_no_range{ .name = "any_float" };
    {
        Json j = -999.0f;
        auto r = validate_prop<float>(j, desc_no_range);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<float>: no constraint accepts any number");
    }
    // 非法类型
    {
        Json j = "not-a-number";
        auto r = validate_prop<float>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<float>: string rejected");
        AURORA_TEST_CHECK_MSG(!r.ok() && r.error().code_enum == ErrorCode::WidgetInvalidProp,
                              "validate_prop<float>: ErrorCode for type mismatch");
    }
}

// ---- validate_prop<int> ----
static void test_validate_int() {
    PropDescriptor desc{ .name = "max_lines", .min_value = "1" };
    {
        Json j = 5;
        auto r = validate_prop<int>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<int>: 5 >= 1");
    }
    {
        Json j = 0;
        auto r = validate_prop<int>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<int>: 0 < 1 rejected");
    }
}

// ---- validate_prop<bool> ----
static void test_validate_bool() {
    PropDescriptor desc{ .name = "visible" };
    {
        Json j = true;
        auto r = validate_prop<bool>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<bool>: true accepted");
    }
    {
        Json j = 42;
        auto r = validate_prop<bool>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<bool>: number rejected");
    }
}

// ---- validate_prop<LocalizedString> ----
static void test_validate_localized_string() {
    PropDescriptor desc{ .name = "label" };
    {
        Json j = "Hello";
        auto r = validate_prop<LocalizedString>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<LocalizedString>: string accepted");
        AURORA_TEST_CHECK_MSG(r.ok() && r.value().text == "Hello", "validate_prop<LocalizedString>: correct text");
    }
    {
        Json j = 42;
        auto r = validate_prop<LocalizedString>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<LocalizedString>: number rejected");
    }
}

// ---- validate_prop<Length> ----
static void test_validate_length() {
    PropDescriptor desc{ .name = "width" };
    // auto / fill
    {
        Json j = "auto";
        auto r = validate_prop<Length>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok() && r.value().kind == LengthKind::WrapContent, "validate_prop<Length>: 'auto'");
    }
    {
        Json j = "fill";
        auto r = validate_prop<Length>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok() && r.value().kind == LengthKind::Expand, "validate_prop<Length>: 'fill'");
    }
    // fixed(px) >= 0
    {
        Json j = Json::array({ "px", 10.0f });
        auto r = validate_prop<Length>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<Length>: [px, 10] valid");
    }
    {
        Json j = Json::array({ "px", -5.0f });
        auto r = validate_prop<Length>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<Length>: [px, -5] rejected");
        AURORA_TEST_CHECK_MSG(!r.ok() && r.error().code_enum == ErrorCode::WidgetPropConstraintViolated,
                              "validate_prop<Length>: correct ErrorCode for negative value");
    }
    // 非法字符串
    {
        Json j = "bogus";
        auto r = validate_prop<Length>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<Length>: unknown string rejected");
    }
}

// ---- validate_prop<EdgeInsets> ----
static void test_validate_edge_insets() {
    PropDescriptor desc{ .name = "padding" };
    {
        Json j = Json::object({ { "left", 10.0f }, { "top", 5.0f }, { "right", 10.0f }, { "bottom", 5.0f } });
        auto r = validate_prop<EdgeInsets>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<EdgeInsets>: valid object");
    }
    {
        Json j = Json::object({ { "left", -1.0f } });
        auto r = validate_prop<EdgeInsets>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<EdgeInsets>: negative field rejected");
    }
    {
        Json j = "not-object";
        auto r = validate_prop<EdgeInsets>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<EdgeInsets>: non-object rejected");
    }
}

// ---- validate_enum_string ----
static void test_validate_enum_string_fn() {
    PropDescriptor desc{ .name = "text_align", .enum_values = { "Left", "Right", "Center" } };
    Error err;
    {
        Json j = "Center";
        bool ok = validate_enum_string(j, desc, err);
        AURORA_TEST_CHECK_MSG(ok, "validate_enum_string: 'Center' in {Left,Right,Center}");
    }
    {
        Json j = "Top";
        bool ok = validate_enum_string(j, desc, err);
        AURORA_TEST_CHECK_MSG(!ok, "validate_enum_string: 'Top' not in set");
        AURORA_TEST_CHECK_MSG(err.code_enum == ErrorCode::WidgetInvalidProp, "validate_enum_string: correct ErrorCode");
    }
    {
        Json j = 42;
        bool ok = validate_enum_string(j, desc, err);
        AURORA_TEST_CHECK_MSG(!ok, "validate_enum_string: non-string rejected");
    }
}

// ---- Diagnostics 报告：Text::deserialize_props 非法值触发 degraded ----
static void test_text_deserialize_diagnostics() {
    // 清空诊断
    (void)Diagnostics::take();

    Text t;
    Json bad_props = Json::object();
    bad_props["font_size"] = "not-a-number"; // 类型错误
    bad_props["color"] = "not-a-color";      // 类型错误
    bad_props["soft_wrap"] = 42;             // 类型错误

    t.deserialize_props(bad_props);

    const auto diags = Diagnostics::take();
    AURORA_TEST_CHECK_MSG(diags.size() >= 3, "Text::deserialize_props: >= 3 diagnostics for 3 bad props");
    // 验证至少有一条包含 "font_size" 的诊断
    bool found_font_size = false;
    for (const auto &d : diags) {
        if (d.message.find("font_size") != std::string::npos) {
            found_font_size = true;
            break;
        }
    }
    AURORA_TEST_CHECK_MSG(found_font_size, "Text::deserialize_props: font_size diagnostic emitted");
}

// ---- Diagnostics 报告：合法值不触发诊断 ----
static void test_text_deserialize_valid_no_diagnostics() {
    (void)Diagnostics::take();

    Text t;
    Json good_props = Json::object();
    good_props["font_size"] = 14.0f;
    good_props["color"] = Json::array({ 255, 0, 0, 255 });
    good_props["soft_wrap"] = true;

    t.deserialize_props(good_props);

    const auto diags = Diagnostics::take();
    AURORA_TEST_CHECK_MSG(diags.empty(), "Text::deserialize_props: valid props produce no diagnostics");
}

// ---- Widget::validate_props 默认返回成功 ----
static void test_validate_props_default() {
    const Text t;
    const auto r = t.validate_props();
    AURORA_TEST_CHECK_MSG(r.ok(), "Widget::validate_props() default returns success");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_prop_validation ===\n");

    test_validate_color();
    test_validate_float();
    test_validate_int();
    test_validate_bool();
    test_validate_localized_string();
    test_validate_length();
    test_validate_edge_insets();
    test_validate_enum_string_fn();
    test_text_deserialize_diagnostics();
    test_text_deserialize_valid_no_diagnostics();
    test_validate_props_default();
}
