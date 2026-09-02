// test_props_constraint.cpp — 属性系统编译期约束层测试
//
// 覆盖：
//  - validate_prop<T> 各特化（Color / float / int / bool / LocalizedString / Length / EdgeInsets / 枚举）
//  - validate_or_default<T>：非法值回退默认并Diagnostics::degraded 上报；合法值零额外上报
//  - Widget::validate_props() 虚钩子（Column.gap >= 0）
//  - 反序列化接入：Text 的非法 color 经 validate_or_default 降级并上报
//  - 工厂函数正值路径（负值在 debug 下由 AURORA_ASSERT 中止，故仅测正值）
#include "aurora/aurora.h"
#include "aurora/core/diagnostics.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/serialization.h"

#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::Color;
using aurora::Column;
using aurora::Diagnostics;
using aurora::EdgeInsets;
using aurora::Error;
using aurora::Json;
using aurora::Length;
using aurora::LengthKind;
using aurora::LocalizedString;
using aurora::PropDescriptor;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_props_constraint ===\n");
    serialization::register_core_widgets();

    // ---- 1) validate_prop<Color> ----
    {
        PropDescriptor d{ .name = "color", .json_type = "array" };
        AURORA_TEST_CHECK(static_cast<bool>(validate_prop<Color>(Json::array({ 10, 20, 30, 255 }), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<Color>(Json::array({ 300, 0, 0, 255 }), d))); // 分量越界
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<Color>(Json(42), d)));                        // 非数组
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<Color>(Json::array({ 1, 2, 3 }), d)));        // 长度不足
    }

    // ---- 2) validate_prop<float> 含 min/max ----
    {
        PropDescriptor d{ .name = "x", .json_type = "number", .min_value = "0", .max_value = "10" };
        AURORA_TEST_CHECK(static_cast<bool>(validate_prop<float>(Json(5.0), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<float>(Json(-1.0), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<float>(Json(11.0), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<float>(Json("abc"), d)));
    }

    // ---- 3) validate_prop<int> 含 min/max ----
    {
        PropDescriptor d{ .name = "n", .json_type = "integer", .min_value = "1", .max_value = "5" };
        AURORA_TEST_CHECK(static_cast<bool>(validate_prop<int>(Json(3), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<int>(Json(0), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<int>(Json(6), d)));
    }

    // ---- 4) validate_prop<bool> ----
    {
        PropDescriptor d{ .name = "b", .json_type = "boolean" };
        AURORA_TEST_CHECK(static_cast<bool>(validate_prop<bool>(Json(true), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<bool>(Json(1), d))); // 整数非布尔
    }

    // ---- 5) validate_prop<LocalizedString> ----
    {
        PropDescriptor d{ .name = "text", .json_type = "string" };
        AURORA_TEST_CHECK(static_cast<bool>(validate_prop<LocalizedString>(Json("hi"), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<LocalizedString>(Json(7), d)));
    }

    // ---- 6) validate_prop<Length>（接受 "auto"/"fill" 字符串或 [kind,value] 数组）----
    {
        PropDescriptor d{ .name = "len", .json_type = "length" };
        AURORA_TEST_CHECK(static_cast<bool>(validate_prop<Length>(Json("fill"), d)));
        AURORA_TEST_CHECK(static_cast<bool>(validate_prop<Length>(Json("auto"), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<Length>(Json(-1.0), d)));    // 裸数字非法
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<Length>(Json("bogus"), d))); // 非法字符串
    }

    // ---- 7) validate_prop<EdgeInsets>（对象 {left,top,right,bottom}）----
    {
        PropDescriptor d{ .name = "pad", .json_type = "edge_insets" };
        AURORA_TEST_CHECK(static_cast<bool>(validate_prop<EdgeInsets>(
            Json::object({ { "left", 1 }, { "top", 2 }, { "right", 3 }, { "bottom", 4 } }), d)));
        AURORA_TEST_CHECK(!static_cast<bool>(validate_prop<EdgeInsets>(Json::object({ { "left", -1 } }), d)));
    }

    // ---- 8) validate_enum_string 枚举值集合 ----
    {
        PropDescriptor d{ .name = "align", .json_type = "string", .enum_values = { "start", "center", "end" } };
        Error err;
        AURORA_TEST_CHECK(validate_enum_string(Json("center"), d, err));
        AURORA_TEST_CHECK(!validate_enum_string(Json("middle"), d, err));
    }

    // ---- 9) validate_or_default：非法回退默认 + degraded 上报；合法零额外上报 ----
    {
        Diagnostics::take(); // 清空累计
        PropDescriptor d{ .name = "color", .json_type = "array" };
        auto c = validate_or_default<Color>(Json::array({ 999, 0, 0, 255 }), d, Color::black());
        AURORA_TEST_CHECK_EQ(c.m_r, 0u);
        AURORA_TEST_CHECK_EQ(c.m_g, 0u);
        AURORA_TEST_CHECK_EQ(c.m_b, 0u);
        AURORA_TEST_CHECK_EQ(c.m_a, 255u);
        AURORA_TEST_CHECK(Diagnostics::count() >= 1u); // 越界 → 上报

        auto c2 = validate_or_default<Color>(Json::array({ 1, 2, 3, 4 }), d, Color::black());
        AURORA_TEST_CHECK_EQ(c2.m_r, 1u);
        AURORA_TEST_CHECK_EQ(Diagnostics::count(), 1u); // 合法：上报数不变
    }

    // ---- 10) Widget::validate_props() 钩子（Column.gap >= 0）----
    {
        Column col;
        col.gap = -5.0f;
        AURORA_TEST_CHECK(!static_cast<bool>(col.validate_props()));
        col.gap = 4.0f;
        AURORA_TEST_CHECK(static_cast<bool>(col.validate_props()));
    }

    // ---- 11) 反序列化接入：Text 非法 color 经 validate_or_default 降级并上报 ----
    {
        Diagnostics::take();
        Json props = Json::object({
            { "content", "hello" },
            { "font_size", -3 },
            { "color", Json::array({ 300, 0, 0, 255 }) },
        });
        auto w = serialization::WidgetRegistry::instance().make("Text", props);
        AURORA_TEST_CHECK(static_cast<bool>(w));
        AURORA_TEST_CHECK(Diagnostics::count() >= 1u); // color 越界 → degraded
    }

    // ---- 12) 工厂函数正值路径（debug 下负值触发 AURORA_ASSERT 中止，仅测正值）----
    {
        auto fxd = Length::fixed(10.0f);
        AURORA_TEST_CHECK(fxd.kind == LengthKind::Fixed);
        AURORA_TEST_CHECK_NEAR(fxd.value, 10.0f, 1e-3f);

        auto fr = Length::ratio(0.5f);
        AURORA_TEST_CHECK(fr.kind == LengthKind::Fraction);
        AURORA_TEST_CHECK_NEAR(fr.value, 0.5f, 1e-3f);
    }
}
