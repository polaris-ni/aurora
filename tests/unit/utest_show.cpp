/// 测试类型: unit
/// 目标单元: include/aurora/widget/show.h
/// 测试说明: 覆盖 Show 条件显示——bool 构造的可见/隐藏布局、State<bool> 响应式驱动、
/// 信号收集、可见性序列化、adopt_children 取首项、自描述

#include <memory>

#include "aurora/layout/layout_engine.h"
#include "aurora/state/state.h"
#include "aurora/widget/show.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_show {
using aurora::testing::require_field;

namespace {

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(show_visible_passes_child_size_through) {
    Show s(true, box(80.0F, 30.0F));
    AURORA_TEST_CHECK_EQ(std::string{s.type_name()}, "Show");
    AURORA_TEST_CHECK_TRUE(s.is_visible());

    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    const Size out = s.size();
    AURORA_TEST_CHECK_NEAR(out.width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out.height, 30.0F, 1e-4F);
}

AURORA_TEST_CASE(show_hidden_collapses_to_zero) {
    Show s(false, box(80.0F, 30.0F));
    AURORA_TEST_CHECK_FALSE(s.is_visible());

    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    const Size out = s.size();
    AURORA_TEST_CHECK_NEAR(out.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out.height, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(show_state_driven_visibility) {
    State<bool> state{false};
    Show s(state, box(80.0F, 30.0F));
    AURORA_TEST_CHECK_FALSE(s.is_visible());
    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 0.0F, 1e-4F);

    // 状态翻转 → 可见（重新布局后尺寸透传）。
    state.set(true);
    AURORA_TEST_CHECK_TRUE(s.is_visible());
    LayoutEngine::layout(s, bounded(200.F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 80.0F, 1e-4F);
}

AURORA_TEST_CASE(show_collects_state_signal) {
    State<bool> state{true};
    Show s(state, box(1.0F, 1.0F));
    StateSink out;
    s.collect_signals(out);
    AURORA_TEST_REQUIRE_EQ(out.sources().size(), 1U);
    const auto *source = dynamic_cast<const detail::SourceOf<bool> *>(out.sources()[0].get());
    AURORA_TEST_REQUIRE_NOT_NULL(source);
    AURORA_TEST_CHECK_TRUE(source->value());

    // bool 构造同样装一份自有常量来源（与 Progress/Checkbox 的字面量构造同口径）：
    // 采集收录当前安装来源，值为构造字面量、不可写。
    Show plain(true, box(1.0F, 1.0F));
    StateSink own;
    plain.collect_signals(own);
    AURORA_TEST_REQUIRE_EQ(own.sources().size(), 1U);
    const auto *own_source = dynamic_cast<const detail::SourceOf<bool> *>(own.sources()[0].get());
    AURORA_TEST_REQUIRE_NOT_NULL(own_source);
    AURORA_TEST_CHECK_TRUE(own_source->value());
}

AURORA_TEST_CASE(show_serializes_visibility) {
    Show on(true, box(1.0F, 1.0F));
    Json props = Json::object();
    on.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "visible"), true);

    Show off(false, box(1.0F, 1.0F));
    off.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "visible"), false);
}

AURORA_TEST_CASE(show_adopt_children_takes_first) {
    Show s(true, box(1.0F, 1.0F));
    s.adopt_children(std::vector<Node>{box(50.0F, 10.0F), box(60.0F, 20.0F)});
    // 仅保留首项（single 策略）。
    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 50.0F, 1e-4F);
}

AURORA_TEST_CASE(show_describe_reports_metadata) {
    const auto d = Show::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Show");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
}

}  // namespace aurora::test_cases::utest_show
