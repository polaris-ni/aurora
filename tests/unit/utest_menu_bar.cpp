/// 测试类型: unit
/// 目标单元: include/aurora/widget/menu_bar.h
/// 测试说明: 覆盖 MenuBar 下拉浮层的「覆盖绘制 ⇄ 命中链」契约——浮层画在栏的布局盒之外
/// （`on_layout` 只报栏高），真实派发只取 `hit_test_chain`、而链在祖先侧按**布局盒**闸控下降，
/// 故浮层必须由 `extra_hit_box()` 声明才会被闸并入。判据：嵌 Column / 嵌 LazyList 条目两种宿主
/// 下浮层区可点、菜单项回调经真实派发触发、收起态不认浮层区、展开关闭后陈旧点不中、
/// 兼容入口（`hit_test`）与派发入口（`hit_test_chain`）逐点一致。
///
/// 回归背景：修复前 MenuBar 只覆写 `on_hit_test`，`on_hit_test_chain` 恒返回空 ⇒ 菜单项点不中。
///
/// 探针取点纪律：浮层矩形按**控件外契约**独立重建（栏高 / 行高 26 / 浮层宽 180 / 菜单项数），
/// 并与公开的 `dropdown_bounds()` 对账（两者分叉即报警）；取点前先断言该点在栏的布局盒之外。

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/menu.h"
#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/menu_bar.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_menu_bar {

namespace {

constexpr float AURORA_HOST_WIDTH = 320.0F;  ///< 宿主视口宽（dp）
constexpr float AURORA_HOST_HEIGHT = 260.0F;  ///< 宿主视口高（dp）
constexpr float AURORA_BAR_HEIGHT = 28.0F;  ///< 栏高（dp；与 set_bar_height 对齐）
constexpr float AURORA_ITEM_HEIGHT = 26.0F;  ///< 菜单项行高（dp；控件内常量，此处按契约钉住）
constexpr float AURORA_FLYOUT_WIDTH = 180.0F;  ///< 浮层宽（dp；控件内常量，此处按契约钉住）

auto host_box() -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                .size = Size{.width = AURORA_HOST_WIDTH, .height = AURORA_HOST_HEIGHT}};
}

auto host_constraints() -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                       .max = Size{.width = AURORA_HOST_WIDTH, .height = AURORA_HOST_HEIGHT}};
}

auto press(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = y};
    return e;
}

auto release(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Release;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = y};
    return e;
}

/// @brief 构造被测菜单栏：两个顶级菜单，第一个含三个可点击项（回调写入 log）。
/// @param hits 菜单项回调计数器（引用，生命周期覆盖整条用例）。
/// @param last 最后一次触发的菜单项序号（-1 = 未触发）。
/// @return 已挂好菜单的 MenuBar（收起态）。
auto make_menu_bar(int &hits, int &last) -> std::shared_ptr<MenuBar> {
    std::vector<Menu> menus;
    Menu file;
    file.title = "File";
    for (int i = 0; i < 3; ++i) {
        MenuItem item;
        item.label = "Item" + std::to_string(i);
        item.on_click = [&hits, &last, i]() -> void {
            ++hits;
            last = i;
        };
        file.items.push_back(std::move(item));
    }
    Menu help;
    help.title = "Help";
    MenuItem about;
    about.label = "About";
    about.on_click = [&hits, &last]() -> void {
        ++hits;
        last = 99;
    };
    help.items.push_back(std::move(about));
    menus.push_back(std::move(file));
    menus.push_back(std::move(help));

    auto bar = std::make_shared<MenuBar>(std::move(menus));
    bar->set_bar_height(AURORA_BAR_HEIGHT);
    return bar;
}

/// @brief 浮层矩形（**本地坐标**，原点即栏左上角）：按控件外契约独立重建。
/// @param item_count 展开菜单的菜单项数。
/// @return 浮层矩形（x 取 0：展开的是第 0 个顶级菜单）。
[[nodiscard]] auto flyout_rect(std::size_t item_count) -> Rect {
    return Rect{
        .origin = Point{.x = 0.0F, .y = AURORA_BAR_HEIGHT},
        .size = Size{.width = AURORA_FLYOUT_WIDTH, .height = static_cast<float>(item_count) * AURORA_ITEM_HEIGHT}};
}

/// @brief 浮层第 index 行中心的本地坐标。
/// @param index 菜单项序号。
/// @return 该行中心（本地坐标）。
[[nodiscard]] auto item_center(int index) -> Point {
    return Point{.x = AURORA_FLYOUT_WIDTH * 0.5F,
                 .y = AURORA_BAR_HEIGHT + ((static_cast<float>(index) + 0.5F) * AURORA_ITEM_HEIGHT)};
}

/// @brief 在宿主的子节点表里取目标控件的盒（相对宿主内容区原点）。
/// @param host 已完成布局的容器宿主。
/// @param target 目标控件（按地址比对）。
/// @return 目标盒；表中无该控件时返回零盒。
[[nodiscard]] auto child_box(const Widget &host, const Widget &target) -> Rect {
    for (const Node &n : host.child_nodes()) {
        if (&n.widget() == &target) {
            return n.bounds();
        }
    }
    return Rect{};
}

/// @brief 命中链的最深节点（派发的实际目标）；空链为 nullptr。
/// @param chain `hit_test_chain` 的产物。
/// @return 最深控件指针；链空为 nullptr。
[[nodiscard]] auto deepest(const std::vector<HitNode> &chain) -> const Widget * {
    return chain.empty() ? nullptr : chain.back().get();
}

}  // namespace

AURORA_TEST_CASE(flyout_point_reaches_menu_bar_in_column_host) {
    // 核心证人：浮层画在栏盒之外，经 Column 宿主派发时该点必须进链并止于 MenuBar。
    int hits = 0;
    int last = -1;
    const std::shared_ptr<MenuBar> bar = make_menu_bar(hits, last);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect bb = child_box(*root, *bar);
    AURORA_TEST_REQUIRE(bb.size.width > 0.0F);
    bar->open(0);

    // 独立基准与控件自报对账：分叉即报警（行高/浮层宽等常量漂移会在此暴露）。
    const Rect expected = flyout_rect(3);
    AURORA_TEST_CHECK_NEAR(expected.origin.y, bar->dropdown_bounds().origin.y, 1e-3F);
    AURORA_TEST_CHECK_NEAR(expected.size.height, bar->dropdown_bounds().size.height, 1e-3F);

    const Point local = item_center(1);
    const Point probe{.x = bb.origin.x + local.x, .y = bb.origin.y + local.y};
    // 前提：该点确实在 MenuBar 自身布局盒之外（否则覆盖区前提不成立）。
    AURORA_TEST_REQUIRE_FALSE(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bb.size}.contains(local));

    const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(bar.get()));
}

AURORA_TEST_CASE(flyout_press_triggers_menu_item_via_real_dispatch) {
    // 端到端：浮层行的 Press 经真实派发触发菜单项回调，并收起浮层。
    int hits = 0;
    int last = -1;
    const std::shared_ptr<MenuBar> bar = make_menu_bar(hits, last);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect bb = child_box(*root, *bar);
    AURORA_TEST_REQUIRE(bb.size.width > 0.0F);
    bar->open(0);

    const Point local = item_center(2);
    MouseEvent down = press(bb.origin.x + local.x, bb.origin.y + local.y);
    EventDispatcher::dispatch(*root, down, nullptr);
    AURORA_TEST_CHECK_EQ(hits, 1);
    AURORA_TEST_CHECK_EQ(last, 2);
    AURORA_TEST_CHECK_FALSE(bar->is_open());  // 命中菜单项后收起
}

AURORA_TEST_CASE(flyout_point_reaches_menu_bar_in_lazy_list_item) {
    // 祖先换成 LazyList：条目盒只有行高（60dp），浮层 y∈[28,106] 越出条目盒 ⇒ 闸须并入追加盒。
    int hits = 0;
    int last = -1;
    const std::shared_ptr<MenuBar> bar = make_menu_bar(hits, last);
    auto other = std::make_shared<MenuBar>(std::vector<Menu>{});
    auto list = std::make_shared<LazyList>(
        2, [bar, other](int index) -> Node { return index == 0 ? Node{bar} : Node{other}; }, 60.0F);
    LayoutEngine::layout(*list, host_constraints());
    bar->open(0);

    const Point probe = item_center(2);
    AURORA_TEST_REQUIRE(probe.y > 60.0F);  // 前提：已越出条目盒

    const std::vector<HitNode> chain = list->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(bar.get()));

    MouseEvent down = press(probe.x, probe.y);
    EventDispatcher::dispatch(*list, down, nullptr);
    AURORA_TEST_CHECK_EQ(hits, 1);
    AURORA_TEST_CHECK_EQ(last, 2);
    MouseEvent up = release(probe.x, probe.y);  // 配对 Release：解除派发器单例的指针捕获
    EventDispatcher::dispatch(*list, up, nullptr);
}

AURORA_TEST_CASE(closed_menu_bar_does_not_claim_flyout_area) {
    // 收起态逐位不变：追加盒为空，浮层区不归菜单栏。
    int hits = 0;
    int last = -1;
    const std::shared_ptr<MenuBar> bar = make_menu_bar(hits, last);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect bb = child_box(*root, *bar);
    AURORA_TEST_REQUIRE(bb.size.width > 0.0F);
    const Point local = item_center(0);
    const Point probe{.x = bb.origin.x + local.x, .y = bb.origin.y + local.y};
    AURORA_TEST_CHECK(root->hit_test_chain(probe, host_box(), BuildContext{}).empty());
    AURORA_TEST_CHECK_FALSE(bar->covers_extra_hit_box(local, BuildContext{}));

    // 展开→收起：上一帧还在浮层内的点不得再命中。
    bar->open(0);
    AURORA_TEST_REQUIRE_FALSE(root->hit_test_chain(probe, host_box(), BuildContext{}).empty());
    bar->close();
    const std::vector<HitNode> stale = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_CHECK(deepest(stale) != static_cast<const Widget *>(bar.get()));
    AURORA_TEST_CHECK_EQ(hits, 0);
}

AURORA_TEST_CASE(compat_entry_and_dispatch_entry_agree_pointwise) {
    // 两入口逐点一致（单源化的前提）：同一棵树、同一批点，`hit_test`（兼容入口）与
    // `hit_test_chain`（派发入口）必须判到同一个控件。
    int hits = 0;
    int last = -1;
    const std::shared_ptr<MenuBar> bar = make_menu_bar(hits, last);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());
    bar->open(0);

    // 循环变量取整型（bugprone-float-loop-counter）：35 × 9 个点与「y: 0→120 / x: 4→320」的
    // 浮点取点集合一致。
    for (int iy = 0; iy < 35; ++iy) {
        const float y = static_cast<float>(iy) * 3.5F;
        for (int ix = 0; ix < 9; ++ix) {
            const float x = 4.0F + (static_cast<float>(ix) * 37.0F);
            const Point probe{.x = x, .y = y};
            const Widget *compat = root->hit_test(probe, host_box(), BuildContext{});
            const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
            AURORA_TEST_CHECK_EQ(compat, deepest(chain));
        }
    }
    AURORA_TEST_CHECK_EQ(hits, 0);  // 命中查询不得有副作用
}

}  // namespace aurora::test_cases::utest_menu_bar
