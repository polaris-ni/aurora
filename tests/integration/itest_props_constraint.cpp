/// 测试类型: integration
/// 目标单元: include/aurora/widget/descriptor.h
/// 测试说明: 属性系统约束层端到端——validate_prop<T> 各特化（含 min/max 与枚举集合）、
///           validate_or_default 非法回退默认 + Diagnostics::degraded 上报（合法零额外上报）、
///           Widget::validate_props 虚钩子（Column.gap >= 0）、注册表反序列化接入
///           （Text 非法 color 降级并上报）、Length 工厂正值路径
///           （负值在 debug 下由 AURORA_ASSERT 中止，故仅测正值）

#include "aurora/aurora.h"
#include "aurora/core/diagnostics.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/serialization.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_props_constraint {

namespace serialization = au::serialization;

using au::Color;
using au::Column;
using au::Diagnostics;
using au::EdgeInsets;
using au::Error;
using au::Json;
using au::Length;
using au::LengthKind;
using au::LocalizedString;
using au::PropDescriptor;

AURORA_TEST_CASE(validate_prop_scalar_specializations) {
    // Color：数组长度 >= 4 且分量 0-255。
    {
        const PropDescriptor d{.name = "color", .json_type = "array"};
        AURORA_TEST_CHECK(au::validate_prop<Color>(Json::array({10, 20, 30, 255}), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<Color>(Json::array({300, 0, 0, 255}), d).ok());  // 分量越界
        AURORA_TEST_CHECK(!au::validate_prop<Color>(Json(42), d).ok());  // 非数组
        AURORA_TEST_CHECK(!au::validate_prop<Color>(Json::array({1, 2, 3}), d).ok());  // 长度不足
    }
    // float：min/max 约束。
    {
        const PropDescriptor d{.name = "x", .json_type = "number", .min_value = "0", .max_value = "10"};
        AURORA_TEST_CHECK(au::validate_prop<float>(Json(5.0), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<float>(Json(-1.0), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<float>(Json(11.0), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<float>(Json("abc"), d).ok());
    }
    // int：min/max 约束。
    {
        const PropDescriptor d{.name = "n", .json_type = "integer", .min_value = "1", .max_value = "5"};
        AURORA_TEST_CHECK(au::validate_prop<int>(Json(3), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<int>(Json(0), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<int>(Json(6), d).ok());
    }
    // bool：整数非布尔。
    {
        const PropDescriptor d{.name = "b", .json_type = "boolean"};
        AURORA_TEST_CHECK(au::validate_prop<bool>(Json(true), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<bool>(Json(1), d).ok());
    }
    // LocalizedString。
    {
        const PropDescriptor d{.name = "text", .json_type = "string"};
        AURORA_TEST_CHECK(au::validate_prop<LocalizedString>(Json("hi"), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<LocalizedString>(Json(7), d).ok());
    }
}

AURORA_TEST_CASE(validate_prop_length_edge_insets_and_enum) {
    // Length：接受 "auto"/"fill" 字符串或 [kind, value] 数组；裸数字/非法串拒绝。
    {
        const PropDescriptor d{.name = "len", .json_type = "length"};
        AURORA_TEST_CHECK(au::validate_prop<Length>(Json("fill"), d).ok());
        AURORA_TEST_CHECK(au::validate_prop<Length>(Json("auto"), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<Length>(Json(-1.0), d).ok());
        AURORA_TEST_CHECK(!au::validate_prop<Length>(Json("bogus"), d).ok());
    }
    // EdgeInsets：对象 {left,top,right,bottom}，字段 >= 0。
    {
        const PropDescriptor d{.name = "pad", .json_type = "edge_insets"};
        AURORA_TEST_CHECK(
            au::validate_prop<EdgeInsets>(Json::object({{"left", 1}, {"top", 2}, {"right", 3}, {"bottom", 4}}), d)
                .ok());
        AURORA_TEST_CHECK(!au::validate_prop<EdgeInsets>(Json::object({{"left", -1}}), d).ok());
    }
    // 枚举值集合。
    {
        const PropDescriptor d{.name = "align", .json_type = "string", .enum_values = {"start", "center", "end"}};
        Error err;
        AURORA_TEST_CHECK(au::validate_enum_string(Json("center"), d, err));
        AURORA_TEST_CHECK(!au::validate_enum_string(Json("middle"), d, err));
    }
}

AURORA_TEST_CASE(validate_or_default_falls_back_and_reports) {
    Diagnostics::take();  // 清空累计

    const PropDescriptor d{.name = "color", .json_type = "array"};
    // 非法：越界分量 → 回退默认值（黑）并 degraded 上报。
    const auto c = au::validate_or_default<Color>(Json::array({999, 0, 0, 255}), d, Color::black());
    AURORA_TEST_CHECK_EQ(c.r, 0U);
    AURORA_TEST_CHECK_EQ(c.g, 0U);
    AURORA_TEST_CHECK_EQ(c.b, 0U);
    AURORA_TEST_CHECK_EQ(c.a, 255U);
    AURORA_TEST_CHECK(Diagnostics::count() >= 1U);

    // 合法：解析成功且上报数不变。
    const auto c2 = au::validate_or_default<Color>(Json::array({1, 2, 3, 4}), d, Color::black());
    AURORA_TEST_CHECK_EQ(c2.r, 1U);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), 1U);
}

AURORA_TEST_CASE(column_gap_constraint_via_validate_props_hook) {
    Column col;
    col.gap = -5.0F;
    AURORA_TEST_CHECK(!col.validate_props().ok());
    col.gap = 4.0F;
    AURORA_TEST_CHECK(col.validate_props().ok());
}

AURORA_TEST_CASE(registry_make_degrades_invalid_color_and_reports) {
    serialization::register_core_widgets();
    Diagnostics::take();

    const Json props = Json::object({
        {"content", "hello"},
        {"font_size", -3},
        {"color", Json::array({300, 0, 0, 255})},
    });
    const auto w = serialization::WidgetRegistry::instance().make("Text", props);
    AURORA_TEST_CHECK(w.ok());
    AURORA_TEST_CHECK_MSG(Diagnostics::count() >= 1U, "color out of range must be degraded and reported");
}

AURORA_TEST_CASE(length_factories_positive_values) {
    const auto fxd = Length::fixed(10.0F);
    AURORA_TEST_CHECK(fxd.kind == LengthKind::Fixed);
    AURORA_TEST_CHECK_NEAR(fxd.value, 10.0F, 1e-3F);

    const auto fr = Length::ratio(0.5F);
    AURORA_TEST_CHECK(fr.kind == LengthKind::Fraction);
    AURORA_TEST_CHECK_NEAR(fr.value, 0.5F, 1e-3F);
}

}  // namespace aurora::test_cases::itest_props_constraint
