/// 测试类型: unit
/// 目标单元: include/aurora/widget/splitter.h
/// 测试说明: 覆盖分隔条的命中链组装与真实拖拽闭环——落在分隔条上的 Press 必须经
/// `EventDispatcher` 送达 `Splitter`（此前 `on_hit_test_chain` 在分隔条处返回空链，
/// 派发器按「点击空白」放弃该帧，拖拽主路径永不触发）、Move 按指针位置更新比例、
/// min_first/min_second 在拖到两端时守住下限、Release 结束拖拽、`on_ratio_change`
/// 只在比例真的改变时回调；另覆盖 `Inspector::simulate_pointer` 的按坐标单步派发。

#include <memory>
#include <string>

#include "aurora/event/dispatcher.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/splitter.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_splitter {

namespace {

constexpr float AURORA_WIDTH = 560.0F;
constexpr float AURORA_HEIGHT = 400.0F;
constexpr float AURORA_HANDLE = 6.0F;
constexpr float AURORA_MIN_FIRST = 80.0F;
constexpr float AURORA_MIN_SECOND = 120.0F;
constexpr float AURORA_AVAIL = AURORA_WIDTH - AURORA_HANDLE;  ///< 主轴可分配长度（分隔条除外）= 554

/// @brief 与 `demo_splitter` 同参数的分栏：初值 0.3、两侧下限 80/120。
/// @return 根容器（Column）与其内的 Splitter；根已完成布局，可直接派发事件。
struct Fixture {
    std::shared_ptr<Column> root;
    std::shared_ptr<Splitter> sp;
};

auto make_fixture() -> Fixture {
    auto sp = std::make_shared<Splitter>(SplitterOrientation::Horizontal, Node{Text{"Sidebar"}},
                                         Node{Text{"Main content area"}}, 0.3F);
    sp->set_min_sizes(AURORA_MIN_FIRST, AURORA_MIN_SECOND);
    auto root = std::make_shared<Column>();
    root->add(Node{sp});
    LayoutEngine::layout(*root, Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                                            .max = Size{.width = AURORA_WIDTH, .height = AURORA_HEIGHT}});
    return Fixture{.root = root, .sp = sp};
}

/// @brief 当前比例下的第一区长度（`Splitter::first_extent()` 的同式外算）。
[[nodiscard]] auto first_extent(const Splitter &sp) -> float { return sp.ratio_value() * AURORA_AVAIL; }

/// @brief 分隔条中点（根容器坐标系；本 fixture 中 Splitter 位于原点）。
[[nodiscard]] auto divider_x(const Splitter &sp) -> float { return first_extent(sp) + (AURORA_HANDLE * 0.5F); }

auto mouse(MouseAction action, float x) -> MouseEvent {
    MouseEvent e;
    e.action = action;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = 100.0F};
    return e;
}

}  // namespace

AURORA_TEST_CASE(hit_chain_at_divider_includes_splitter) {
    // 回归点：分隔条处的命中链不得为空——空链等于把 Splitter 排除在派发之外。
    Fixture f = make_fixture();
    const Rect root_rect{.origin = Point{}, .size = Size{.width = AURORA_WIDTH, .height = AURORA_HEIGHT}};
    const std::vector<HitNode> chain =
        f.root->hit_test_chain(Point{.x = divider_x(*f.sp), .y = 100.0F}, root_rect, BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(chain.back().ptr, f.sp.get());
    AURORA_TEST_CHECK_EQ(std::string{chain.back().ptr->type_name()}, std::string{"Splitter"});
}

AURORA_TEST_CASE(divider_drag_via_dispatcher_updates_ratio_and_clamps) {
    Fixture f = make_fixture();
    int cb_hits = 0;
    f.sp->set_on_ratio_change([&cb_hits](float) -> void { ++cb_hits; });

    MouseEvent press = mouse(MouseAction::Press, divider_x(*f.sp));
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, press, nullptr));
    // 回归点：修复前 Press 在分隔条处链空被当作点击空白，dragging_ 永不为真。
    AURORA_TEST_CHECK_TRUE(f.sp->is_dragging());

    MouseEvent mid = mouse(MouseAction::Move, 250.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, mid, nullptr));
    AURORA_TEST_CHECK_TRUE(f.sp->is_dragging());
    AURORA_TEST_CHECK_NEAR(f.sp->ratio_value(), (250.0F - (AURORA_HANDLE * 0.5F)) / AURORA_AVAIL, 1e-4F);

    // 拖到左端以外：第一区恰停在下限 80，不被压没。
    MouseEvent left = mouse(MouseAction::Move, -50.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, left, nullptr));
    AURORA_TEST_CHECK_NEAR(first_extent(*f.sp), AURORA_MIN_FIRST, 1e-3F);

    // 拖到右端以外：第二区仍保住下限 120。
    MouseEvent right = mouse(MouseAction::Move, AURORA_WIDTH + 200.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, right, nullptr));
    AURORA_TEST_CHECK_NEAR(AURORA_AVAIL - first_extent(*f.sp), AURORA_MIN_SECOND, 1e-3F);

    MouseEvent release = mouse(MouseAction::Release, AURORA_WIDTH + 200.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, release, nullptr));
    AURORA_TEST_CHECK_FALSE(f.sp->is_dragging());
    AURORA_TEST_CHECK_GT(cb_hits, 2);  // 中点 + 两端各回调一次

    // 原地再按一次（比例未变）不应重复回调。
    const int before = cb_hits;
    MouseEvent same = mouse(MouseAction::Press, divider_x(*f.sp));
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, same, nullptr));
    MouseEvent same_release = mouse(MouseAction::Release, divider_x(*f.sp));
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, same_release, nullptr));
    AURORA_TEST_CHECK_EQ(cb_hits, before);
}

AURORA_TEST_CASE(press_outside_divider_does_not_start_drag) {
    // 落在某一栏空白处（非分隔条）的按下不应进入拖拽态——否则整块面板都变成把手。
    Fixture f = make_fixture();
    MouseEvent press = mouse(MouseAction::Press, first_extent(*f.sp) + AURORA_HANDLE + 40.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, press, nullptr));  // 命中链非空（Splitter 兜底认领）
    AURORA_TEST_CHECK_FALSE(f.sp->is_dragging());

    MouseEvent move = mouse(MouseAction::Move, 300.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, move, nullptr));
    AURORA_TEST_CHECK_FALSE(f.sp->is_dragging());
    MouseEvent release = mouse(MouseAction::Release, 300.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*f.root, release, nullptr));
}

AURORA_TEST_CASE(simulate_pointer_presses_at_given_coordinate) {
    // 按坐标的单步派发：目标式 simulate_click/drag 只能取控件中心，够不到分隔条这类子区域。
    Fixture f = make_fixture();
    AURORA_TEST_REQUIRE_TRUE(
        Inspector::simulate_pointer(*f.sp, Point{.x = divider_x(*f.sp), .y = 100.0F}, MouseAction::Press).ok());
    AURORA_TEST_CHECK_TRUE(f.sp->is_dragging());

    AURORA_TEST_REQUIRE_TRUE(
        Inspector::simulate_pointer(*f.sp, Point{.x = 300.0F, .y = 100.0F}, MouseAction::Move).ok());
    AURORA_TEST_CHECK_NEAR(f.sp->ratio_value(), (300.0F - (AURORA_HANDLE * 0.5F)) / AURORA_AVAIL, 1e-4F);

    AURORA_TEST_REQUIRE_TRUE(
        Inspector::simulate_pointer(*f.sp, Point{.x = 300.0F, .y = 100.0F}, MouseAction::Release).ok());
    AURORA_TEST_CHECK_FALSE(f.sp->is_dragging());

    // 控件外坐标：Press 前置于命中测试即拒绝，不改任何状态。
    const Result<void> miss =
        Inspector::simulate_pointer(*f.sp, Point{.x = AURORA_WIDTH * 10.0F, .y = 100.0F}, MouseAction::Press);
    AURORA_TEST_REQUIRE_FALSE(miss.ok());
    AURORA_TEST_CHECK_EQ(miss.error().code_enum, aurora::ErrorCode::GeneralNotSupported);
    AURORA_TEST_CHECK_FALSE(f.sp->is_dragging());
}

AURORA_TEST_CASE(vertical_splitter_drag_uses_other_axis) {
    auto sp = std::make_shared<Splitter>(SplitterOrientation::Vertical, Node{Text{"Top"}}, Node{Text{"Bottom"}}, 0.4F);
    sp->set_min_sizes(60.0F, 60.0F);
    LayoutEngine::layout(
        *sp, Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 300.0F, .height = 200.0F}});
    const float avail = 200.0F - AURORA_HANDLE;
    MouseEvent press = mouse(MouseAction::Press, 10.0F);
    press.position = Point{.x = 10.0F, .y = (0.4F * avail) + (AURORA_HANDLE * 0.5F)};
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sp, press, nullptr));
    AURORA_TEST_CHECK_TRUE(sp->is_dragging());

    MouseEvent move = press;
    move.action = MouseAction::Move;
    move.position = Point{.x = 10.0F, .y = 0.0F};
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sp, move, nullptr));
    AURORA_TEST_CHECK_NEAR(sp->ratio_value() * avail, 60.0F, 1e-3F);  // 夹在 min_first

    MouseEvent release = move;
    release.action = MouseAction::Release;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sp, release, nullptr));
    AURORA_TEST_CHECK_FALSE(sp->is_dragging());
}

}  // namespace aurora::test_cases::utest_splitter
