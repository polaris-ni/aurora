/// 测试类型: unit
/// 目标单元: include/aurora/widget/placeholder.h
/// 测试说明: 覆盖 Placeholder 降级占位控件——默认文案回退 "(placeholder)"、链式 setter、
/// 尺寸由文本测量 + 内边距构成、颜色序列化往返、自描述与信号缺席

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/placeholder.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "framework/json_literals.h"

namespace aurora::test_cases::utest_placeholder {
using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_uses_fallback_text_and_palette) {
    const Placeholder p;
    AURORA_TEST_CHECK_EQ(std::string{p.type_name()}, "Placeholder");

    Json props = Json::object();
    p.serialize_props(props);
    // 空消息序列化原样空串（绘制期才回退 "(placeholder)"）。
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "message"), "");
    // 默认配色：浅灰底 / 警示红边 / 深灰字。
    const auto &bg = *require_child(props, "background_color");
    AURORA_TEST_CHECK_EQ(require_child_at(bg, 0)->as_or<int>(0), 0xF2);
    AURORA_TEST_CHECK_EQ(require_child_at(bg, 3)->as_or<int>(0), 0xFF);
    const auto &border = *require_child(props, "border_color");
    AURORA_TEST_CHECK_EQ(require_child_at(border, 0)->as_or<int>(0), 0xC0);
    const auto &text = *require_child(props, "text_color");
    AURORA_TEST_CHECK_EQ(require_child_at(text, 0)->as_or<int>(0), 0x55);
}

AURORA_TEST_CASE(chain_setters_update_state) {
    Placeholder p;
    p.set_message("boom")
        .set_background_color(Color(1, 2, 3, 4))
        .set_border_color(Color(5, 6, 7, 8))
        .set_text_color(Color(9, 10, 11, 12));

    Json props = Json::object();
    p.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "message"), "boom");
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "background_color"), 2)->as_or<int>(0), 3);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "border_color"), 1)->as_or<int>(0), 6);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "text_color"), 3)->as_or<int>(0), 12);
}

AURORA_TEST_CASE(layout_sizes_from_text_metrics_plus_padding) {
    Placeholder empty;
    LayoutEngine::layout(empty, bounded(500.0F, 200.0F));
    const Size a = empty.size();
    AURORA_TEST_CHECK_TRUE(a.width > 16.0F);  // 至少含 16px 水平内边距
    AURORA_TEST_CHECK_TRUE(a.height > 12.0F);

    // 空消息渲染默认文案 "(placeholder)"；显式短消息更窄，单行高度不变。
    Placeholder msg("x");
    LayoutEngine::layout(msg, bounded(500.0F, 200.0F));
    const Size b = msg.size();
    AURORA_TEST_CHECK_TRUE(b.width < a.width);
    AURORA_TEST_CHECK_NEAR(b.height, a.height, 1e-4F);  // 高度只依赖字号

    // 更长消息更宽或相等（不会更窄）。
    Placeholder longer("this is a fairly long placeholder message");
    LayoutEngine::layout(longer, bounded(5000.0F, 200.0F));
    const Size c = longer.size();
    AURORA_TEST_CHECK_TRUE(c.width >= a.width - 0.5F);
}

AURORA_TEST_CASE(layout_clamps_into_tight_constraints) {
    Placeholder p("hello");
    LayoutEngine::layout(p, bounded(30.0F, 20.0F));
    const Size s = p.size();
    AURORA_TEST_CHECK_NEAR(s.width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.height, 20.0F, 1e-4F);
}

AURORA_TEST_CASE(props_deserialize_roundtrip) {
    Json props = Json::object();
    props.set("message", "restored");
    props.set("background_color", testing::json_arr({10, 20, 30, 40}));
    props.set("border_color", testing::json_arr({50, 60, 70, 80}));
    props.set("text_color", testing::json_arr({90, 100, 110, 120}));

    Placeholder p;
    p.deserialize_props(props);
    Json out = Json::object();
    p.serialize_props(out);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(out, "message"), "restored");
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "background_color"), 0)->as_or<int>(0), 10);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "border_color"), 1)->as_or<int>(0), 60);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "text_color"), 2)->as_or<int>(0), 110);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Placeholder::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Placeholder");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_message = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "message") {
            has_message = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_message);

    // 无自有信号。
    Placeholder p;
    std::vector<aurora::SignalViewBase *> out;
    p.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.size(), 0U);
}

}  // namespace aurora::test_cases::utest_placeholder
