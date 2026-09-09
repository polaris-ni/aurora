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

namespace aurora::test_cases::utest_show {

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
    auto state = std::make_shared<State<bool>>(false);
    Show s(state, box(80.0F, 30.0F));
    AURORA_TEST_CHECK_FALSE(s.is_visible());
    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 0.0F, 1e-4F);

    // 状态翻转 → 可见（重新布局后尺寸透传）。
    state->set(true);
    AURORA_TEST_CHECK_TRUE(s.is_visible());
    LayoutEngine::layout(s, bounded(200.F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 80.0F, 1e-4F);
}

AURORA_TEST_CASE(show_collects_state_signal) {
    auto state = std::make_shared<State<bool>>(true);
    Show s(state, box(1.0F, 1.0F));
    std::vector<aurora::SignalViewBase*> out;
    s.collect_signals(out);
    AURORA_TEST_REQUIRE_EQ(out.size(), 1U);
    AURORA_TEST_CHECK_EQ(out[0], state.get());

    // bool 构造无信号。
    Show plain(true, box(1.0F, 1.0F));
    std::vector<aurora::SignalViewBase*> empty;
    plain.collect_signals(empty);
    AURORA_TEST_CHECK_EQ(empty.size(), 0U);
}

AURORA_TEST_CASE(show_serializes_visibility) {
    Show on(true, box(1.0F, 1.0F));
    Json props;
    on.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["visible"].get<bool>(), true);

    Show off(false, box(1.0F, 1.0F));
    off.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["visible"].get<bool>(), false);
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
