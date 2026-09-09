/// 测试类型: integration
/// 目标单元: include/aurora/widget/descriptor.h
/// 测试说明: 属性约束验证管线——validate_prop<T> 各特化（Color / float / int / bool /
///           LocalizedString / Length / EdgeInsets）按 PropDescriptor 约束接受或拒绝
///           （ErrorCode 归属正确）、validate_enum_string 枚举集合校验、
///           Text::deserialize_props 非法值经 validate_or_default 降级并产生
///           Diagnostics 报告而合法值零上报、Widget::validate_props 默认成功

#include <string>

#include "aurora/aurora.h"
#include "aurora/core/diagnostics.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_prop_validation {

using au::Color;
using au::Diagnostics;
using au::EdgeInsets;
using au::ErrorCode;
using au::Json;
using au::Length;
using au::LengthKind;
using au::LocalizedString;
using au::PropDescriptor;
using au::Text;

AURORA_TEST_CASE(validate_prop_color_specialization) {
    const PropDescriptor desc{.name = "color"};

    // 合法：4 元素数组，值在 0-255。
    {
        const Json j = Json::array({255, 128, 0, 255});
        const auto r = au::validate_prop<Color>(j, desc);
        AURORA_TEST_CHECK_MSG(r.ok(), "validate_prop<Color>: valid [255,128,0,255]");
        AURORA_TEST_CHECK(r.ok() && r.value().r == 255 && r.value().g == 128);
    }
    // 非法：数组太短。
    {
        const Json j = Json::array({255, 128});
        const auto r = au::validate_prop<Color>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<Color>: short array rejected");
        AURORA_TEST_CHECK(!r.ok() && r.error().code_enum == ErrorCode::WidgetInvalidProp);
    }
    // 非法：值超出范围。
    {
        const Json j = Json::array({256, 0, 0, 255});
        const auto r = au::validate_prop<Color>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<Color>: out-of-range value rejected");
    }
    // 非法：非数组。
    {
        const Json j = Json("not-a-color");
        const auto r = au::validate_prop<Color>(j, desc);
        AURORA_TEST_CHECK_MSG(!r.ok(), "validate_prop<Color>: non-array rejected");
    }
}

AURORA_TEST_CASE(validate_prop_float_int_bool_string_specializations) {
    // float：有范围约束 min=0 / max=100。
    {
        const PropDescriptor desc{.name = "opacity", .min_value = "0", .max_value = "100"};
        AURORA_TEST_CHECK(au::validate_prop<float>(Json(50.0F), desc).ok());
        {
            const auto r = au::validate_prop<float>(Json(-1.0F), desc);
            AURORA_TEST_CHECK_MSG(!r.ok(), "float: -1 below min rejected");
            AURORA_TEST_CHECK(!r.ok() && r.error().code_enum == ErrorCode::WidgetPropConstraintViolated);
        }
        AURORA_TEST_CHECK(!au::validate_prop<float>(Json(101.0F), desc).ok());
        // 类型不符：string 拒绝，归 WidgetInvalidProp。
        const auto r = au::validate_prop<float>(Json("not-a-number"), desc);
        AURORA_TEST_CHECK(!r.ok() && r.error().code_enum == ErrorCode::WidgetInvalidProp);
    }
    // float：无范围约束接受任意数。
    {
        const PropDescriptor desc_no_range{.name = "any_float"};
        AURORA_TEST_CHECK(au::validate_prop<float>(Json(-999.0F), desc_no_range).ok());
    }
    // int：min=1。
    {
        const PropDescriptor desc{.name = "max_lines", .min_value = "1"};
        AURORA_TEST_CHECK(au::validate_prop<int>(Json(5), desc).ok());
        AURORA_TEST_CHECK(!au::validate_prop<int>(Json(0), desc).ok());
    }
    // bool：布尔接受，数字拒绝。
    {
        const PropDescriptor desc{.name = "visible"};
        AURORA_TEST_CHECK(au::validate_prop<bool>(Json(true), desc).ok());
        AURORA_TEST_CHECK(!au::validate_prop<bool>(Json(42), desc).ok());
    }
    // LocalizedString：字符串接受，数字拒绝。
    {
        const PropDescriptor desc{.name = "label"};
        const auto r = au::validate_prop<LocalizedString>(Json("Hello"), desc);
        AURORA_TEST_CHECK(r.ok() && r.value().text == "Hello");
        AURORA_TEST_CHECK(!au::validate_prop<LocalizedString>(Json(42), desc).ok());
    }
}

AURORA_TEST_CASE(validate_prop_length_and_edge_insets_specializations) {
    // Length：auto / fill 字符串与 [kind, value] 数组。
    {
        const PropDescriptor desc{.name = "width"};
        {
            const auto r = au::validate_prop<Length>(Json("auto"), desc);
            AURORA_TEST_CHECK(r.ok() && r.value().kind == LengthKind::WrapContent);
        }
        {
            const auto r = au::validate_prop<Length>(Json("fill"), desc);
            AURORA_TEST_CHECK(r.ok() && r.value().kind == LengthKind::Expand);
        }
        AURORA_TEST_CHECK(au::validate_prop<Length>(Json::array({"px", 10.0F}), desc).ok());
        {
            const auto r = au::validate_prop<Length>(Json::array({"px", -5.0F}), desc);
            AURORA_TEST_CHECK_MSG(!r.ok(), "Length: negative px rejected");
            AURORA_TEST_CHECK(!r.ok() && r.error().code_enum == ErrorCode::WidgetPropConstraintViolated);
        }
        AURORA_TEST_CHECK(!au::validate_prop<Length>(Json("bogus"), desc).ok());
    }
    // EdgeInsets：对象各字段 >= 0。
    {
        const PropDescriptor desc{.name = "padding"};
        const Json good = Json::object({{"left", 10.0F}, {"top", 5.0F}, {"right", 10.0F}, {"bottom", 5.0F}});
        AURORA_TEST_CHECK(au::validate_prop<EdgeInsets>(good, desc).ok());
        AURORA_TEST_CHECK(!au::validate_prop<EdgeInsets>(Json::object({{"left", -1.0F}}), desc).ok());
        AURORA_TEST_CHECK(!au::validate_prop<EdgeInsets>(Json("not-object"), desc).ok());
    }
}

AURORA_TEST_CASE(validate_enum_string_membership) {
    const PropDescriptor desc{.name = "text_align", .enum_values = {"Left", "Right", "Center"}};
    au::Error err;

    AURORA_TEST_CHECK(au::validate_enum_string(Json("Center"), desc, err));
    {
        const bool ok = au::validate_enum_string(Json("Top"), desc, err);
        AURORA_TEST_CHECK(!ok);
        AURORA_TEST_CHECK(err.code_enum == ErrorCode::WidgetInvalidProp);
    }
    AURORA_TEST_CHECK(!au::validate_enum_string(Json(42), desc, err));  // 非字符串拒绝
}

AURORA_TEST_CASE(text_deserialize_reports_degraded_diagnostics) {
    (void)Diagnostics::take();  // 清空诊断

    Text t;
    Json bad_props = Json::object();
    bad_props["font_size"] = "not-a-number";  // 类型错误
    bad_props["color"] = "not-a-color";  // 类型错误
    bad_props["soft_wrap"] = 42;  // 类型错误

    t.deserialize_props(bad_props);

    const auto diags = Diagnostics::take();
    AURORA_TEST_CHECK_MSG(diags.size() >= static_cast<std::size_t>(3), "3 bad props must yield >= 3 diagnostics");
    bool found_font_size = false;
    for (const auto& d : diags) {
        if (d.message.find("font_size") != std::string::npos) {
            found_font_size = true;
        }
    }
    AURORA_TEST_CHECK_MSG(found_font_size, "font_size diagnostic emitted");
}

AURORA_TEST_CASE(text_deserialize_valid_props_no_diagnostics) {
    (void)Diagnostics::take();  // 清空诊断

    Text t;
    Json good_props = Json::object();
    good_props["font_size"] = 14.0F;
    good_props["color"] = Json::array({255, 0, 0, 255});
    good_props["soft_wrap"] = true;

    t.deserialize_props(good_props);

    AURORA_TEST_CHECK_MSG(Diagnostics::take().empty(), "valid props produce no diagnostics");
}

AURORA_TEST_CASE(widget_validate_props_default_success) {
    const Text t;
    AURORA_TEST_CHECK(t.validate_props().ok());
}

}  // namespace aurora::test_cases::itest_prop_validation
