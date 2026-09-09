/// 测试类型: unit
/// 目标单元: include/aurora/event/focus.h
/// 测试说明: FocusManager 的 set_focus/request_focus/clear 与获失焦通知、on_change 回调新旧对、Tab 序按 tabIndex
/// 稳定排序前进后退循环、跳过隐藏与不可聚焦控件、无根/无候选失败、方向键几何导航与 current_focus_manager 槽位配对

#include <memory>
#include <utility>
#include <vector>

#include "aurora/event/focus.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_focus {

namespace m = aurora::testing::matchers;

namespace {

class FocusProbe final : public LeafWidget {
  public:
    int gained = 0;
    int lost = 0;

    auto type_name() const -> const char* override { return "FocusProbe"; }

    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override {
        size_ = c.constrain(Size{.width = 10.0F, .height = 10.0F});
        return size_;
    }

    auto on_paint(Painter& /*p*/, const Rect& /*bounds*/, const BuildContext& /*ctx*/) -> void override {}

    auto on_focus_change(bool focused) -> void override {
        if (focused) {
            ++gained;
        } else {
            ++lost;
        }
        Widget::on_focus_change(focused);
    }
};

class FocusRow final : public Container {
  public:
    auto type_name() const -> const char* override { return "FocusRow"; }

    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override {
        size_ = c.constrain(Size{.width = 100.0F, .height = 100.0F});
        return size_;
    }

    auto on_paint(Painter& /*p*/, const Rect& /*bounds*/, const BuildContext& /*ctx*/) -> void override {}
};

/// 构造带 n 个探针子控件的根行；根自身不可聚焦，候选集恰为全部探针。
auto make_row(int kids) -> std::pair<std::shared_ptr<FocusRow>, std::vector<std::shared_ptr<FocusProbe>>> {
    auto row = std::make_shared<FocusRow>();
    row->set_focusable(false);
    std::vector<std::shared_ptr<FocusProbe>> probes;
    probes.reserve(static_cast<std::size_t>(kids));
    for (int i = 0; i < kids; ++i) {
        auto probe = std::make_shared<FocusProbe>();
        row->add(Node{probe});
        probes.push_back(std::move(probe));
    }
    return {row, probes};
}

}  // namespace

AURORA_TEST_CASE(set_focus_request_and_clear_notify) {
    auto [row, probes] = make_row(2);
    FocusManager fm;
    fm.set_root(row.get());
    FocusProbe& a = *probes[0];
    FocusProbe& b = *probes[1];

    AURORA_TEST_CHECK(fm.focused() == nullptr);
    AURORA_TEST_CHECK_FALSE(fm.has_focus(&a));

    fm.set_focus(&a);
    AURORA_TEST_CHECK(fm.focused() == &a);
    AURORA_TEST_CHECK_TRUE(fm.has_focus(&a));
    AURORA_TEST_CHECK_TRUE(a.is_focused());
    AURORA_TEST_CHECK_EQ(a.gained, 1);
    AURORA_TEST_CHECK_EQ(a.lost, 0);

    // 重复 set_focus 同一控件：无重复通知
    fm.set_focus(&a);
    AURORA_TEST_CHECK_EQ(a.gained, 1);
    AURORA_TEST_CHECK_EQ(a.lost, 0);

    fm.request_focus(&b);
    AURORA_TEST_CHECK(fm.focused() == &b);
    AURORA_TEST_CHECK_EQ(a.lost, 1);
    AURORA_TEST_CHECK_FALSE(a.is_focused());
    AURORA_TEST_CHECK_EQ(b.gained, 1);

    fm.clear();
    AURORA_TEST_CHECK(fm.focused() == nullptr);
    AURORA_TEST_CHECK_EQ(b.lost, 1);
    AURORA_TEST_CHECK_FALSE(fm.has_focus(&b));
}

AURORA_TEST_CASE(on_change_callback_receives_old_and_new) {
    auto [row, probes] = make_row(2);
    FocusManager fm;
    fm.set_root(row.get());

    std::vector<std::pair<Widget*, Widget*>> transitions;
    fm.set_on_change([&transitions](Widget* old_w, Widget* new_w) -> void { transitions.emplace_back(old_w, new_w); });

    fm.set_focus(probes[0].get());
    fm.set_focus(probes[1].get());
    fm.clear();

    AURORA_TEST_REQUIRE_THAT(transitions, m::size_is(3));
    AURORA_TEST_CHECK(transitions[0].first == nullptr);
    AURORA_TEST_CHECK(transitions[0].second == probes[0].get());
    AURORA_TEST_CHECK(transitions[1].first == probes[0].get());
    AURORA_TEST_CHECK(transitions[1].second == probes[1].get());
    AURORA_TEST_CHECK(transitions[2].first == probes[1].get());
    AURORA_TEST_CHECK(transitions[2].second == nullptr);
}

AURORA_TEST_CASE(move_focus_cycles_forward_and_backward_by_tab_index) {
    auto [row, probes] = make_row(3);
    probes[0]->set_tab_index(1);
    probes[1]->set_tab_index(0);
    probes[2]->set_tab_index(1);

    FocusManager fm;
    fm.set_root(row.get());

    // 候选按 (tabIndex, 遍历序) 稳定排序：[probes[1](0), probes[0](1), probes[2](1)]
    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK(fm.focused() == probes[1].get());
    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK(fm.focused() == probes[0].get());
    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK(fm.focused() == probes[2].get());
    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK(fm.focused() == probes[1].get());  // 循环回绕

    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Backward));
    AURORA_TEST_CHECK(fm.focused() == probes[2].get());
}

AURORA_TEST_CASE(move_focus_skips_hidden_and_unfocusable_widgets) {
    auto [row, probes] = make_row(3);
    probes[1]->show.set(false);  // 隐藏控件不参与焦点序
    probes[2]->set_focusable(false);  // 不可聚焦控件不参与

    FocusManager fm;
    fm.set_root(row.get());
    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK(fm.focused() == probes[0].get());

    // 全部不可聚焦（含根）→ 无候选，移动失败且焦点保持
    probes[0]->set_focusable(false);
    AURORA_TEST_CHECK_FALSE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK(fm.focused() == probes[0].get());
}

AURORA_TEST_CASE(move_focus_fails_without_root_or_candidates) {
    FocusManager fm;
    AURORA_TEST_CHECK_FALSE(fm.move_focus(FocusDirection::Forward));  // 未设置根

    auto [row, probes] = make_row(0);
    AURORA_TEST_CHECK_TRUE(probes.empty());
    fm.set_root(row.get());
    AURORA_TEST_CHECK_FALSE(fm.move_focus(FocusDirection::Backward));  // 树内无可聚焦控件
    AURORA_TEST_CHECK(fm.focused() == nullptr);
}

AURORA_TEST_CASE(directional_move_focus_uses_focus_bounds) {
    auto [row, probes] = make_row(3);
    const std::vector<Rect> boxes = {
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 10.0F, .height = 10.0F}},
        Rect{.origin = Point{.x = 20.0F, .y = 0.0F}, .size = Size{.width = 10.0F, .height = 10.0F}},
        Rect{.origin = Point{.x = 40.0F, .y = 0.0F}, .size = Size{.width = 10.0F, .height = 10.0F}},
    };
    for (std::size_t i = 0; i < probes.size(); ++i) {
        probes[i]->set_focus_bounds(boxes[i]);
    }

    FocusManager fm;
    fm.set_root(row.get());
    fm.set_focus(probes[1].get());  // 中间

    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Right));
    AURORA_TEST_CHECK(fm.focused() == probes[2].get());

    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Left));
    AURORA_TEST_CHECK(fm.focused() == probes[1].get());

    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Left));
    AURORA_TEST_CHECK(fm.focused() == probes[0].get());

    // 同一水平线上没有上方候选：移动失败且焦点保持
    AURORA_TEST_CHECK_FALSE(fm.move_focus(FocusDirection::Up));
    AURORA_TEST_CHECK(fm.focused() == probes[0].get());
}

AURORA_TEST_CASE(widget_request_focus_uses_current_manager_slot) {
    auto [row, probes] = make_row(1);
    FocusProbe& probe = *probes[0];

    set_current_focus_manager(nullptr);
    AURORA_TEST_CHECK(current_focus_manager() == nullptr);
    probe.request_focus();  // 无管理器：静默 no-op

    FocusManager fm;
    set_current_focus_manager(&fm);
    probe.request_focus();
    AURORA_TEST_CHECK(fm.focused() == &probe);
    AURORA_TEST_CHECK_EQ(probe.gained, 1);

    set_current_focus_manager(nullptr);  // 复原槽位，避免泄漏到后续用例
    AURORA_TEST_CHECK(current_focus_manager() == nullptr);
}

}  // namespace aurora::test_cases::utest_focus
