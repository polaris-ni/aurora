/// 测试类型: unit
/// 目标单元: include/aurora/widget/title_bar.h
/// 测试说明: 覆盖 TitleBar 的 Snap 弹窗「覆盖绘制 ⇄ 命中链」契约——弹窗画在栏的布局盒之外
/// （`on_layout` 只报 `style_.height`），真实派发只取 `hit_test_chain`、而链在祖先侧按**布局盒**
/// 闸控下降，故弹窗必须由 `extra_hit_box()` 声明才会被闸并入。判据：嵌 Column / 嵌 LazyList 条目
/// 两种宿主下弹窗区可点、自定义 Snap 项经真实派发触发、收起态不认弹窗区、关闭后陈旧点不中、
/// 兼容入口（`hit_test`）与派发入口（`hit_test_chain`）逐点一致。
///
/// 回归背景：修复前 TitleBar 只覆写 `on_hit_test`，且弹窗分支还额外要求「点落在自身布局盒内」
/// ——弹窗本就画在盒外，该分支恒不成立；`on_hit_test_chain` 又恒返回空 ⇒ 弹窗点不中。
///
/// 探针取点纪律：弹窗矩形按**控件外契约**独立重建（几何取纯函数 `title_bar_geometry()`、
/// 行高 26 / 弹窗宽 120 / 条目数取公开 `snap_entry_count()`），不读私有成员；取点前先断言
/// 该点在栏的布局盒之外。弹窗按真实交互路径展开（悬停最大化钮 ≥400ms），不走私有入口。

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/title_bar.h"
#include "aurora/window/title_bar_geometry.h"
#include "aurora/window/title_bar_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_title_bar {

namespace {

constexpr float AURORA_HOST_WIDTH = 320.0F;  ///< 宿主视口宽（dp）
constexpr float AURORA_HOST_HEIGHT = 260.0F;  ///< 宿主视口高（dp）
constexpr float AURORA_BAR_HEIGHT = 36.0F;  ///< 栏高（dp；TitleBarStyle{} 默认且与 set_height 对齐）
constexpr float AURORA_ITEM_HEIGHT = 26.0F;  ///< 弹窗条目行高（dp；控件内常量，此处按契约钉住）
constexpr float AURORA_FLYOUT_WIDTH = 120.0F;  ///< 弹窗宽（dp；控件内常量，此处按契约钉住）
constexpr float AURORA_FLYOUT_GAP = 4.0F;  ///< 弹窗与栏底的间隙（dp）
constexpr int AURORA_HOVER_DELAY_MS = 400;  ///< 悬停开弹窗的阈值（ms，与控件实现一致）

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

auto move_to(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Move;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = y};
    return e;
}

/// @brief 构造被测标题栏：打开窗口控制钮、挂一个自定义 Snap 项（回调写入计数器）。
/// @param snap_hits 自定义 Snap 项回调计数器（引用，生命周期覆盖整条用例）。
/// @return 已装配的 TitleBar（弹窗收起态）。
auto make_title_bar(int &snap_hits) -> std::shared_ptr<TitleBar> {
    auto bar = std::make_shared<TitleBar>();
    bar->set_height(AURORA_BAR_HEIGHT);
    bar->set_window_controls(true);
    TitleBarAction custom;
    custom.label = "Custom";
    custom.on_click = [&snap_hits]() -> void { ++snap_hits; };
    bar->add_snap_action(std::move(custom));
    return bar;
}

/// @brief 最大化钮的中心（栏本地坐标）：几何由纯函数算出，与控件内部同源但独立调用。
/// @return 最大化钮盒的中心点。
[[nodiscard]] auto maximize_center() -> Point {
    const TitleBarGeometry geom = title_bar_geometry(AURORA_HOST_WIDTH, TitleBarStyle{}, false, true);
    return Point{.x = geom.maximize.origin.x + (geom.maximize.size.width * 0.5F),
                 .y = geom.maximize.origin.y + (geom.maximize.size.height * 0.5F)};
}

/// @brief 弹窗矩形（**本地坐标**）：按控件外契约独立重建（锚点取最大化钮左缘）。
/// @param entry_count 弹窗条目数（内置 4 + 自定义）。
/// @return 弹窗矩形。
[[nodiscard]] auto flyout_rect(std::size_t entry_count) -> Rect {
    const TitleBarGeometry geom = title_bar_geometry(AURORA_HOST_WIDTH, TitleBarStyle{}, false, true);
    return Rect{.origin = Point{.x = geom.maximize.origin.x, .y = AURORA_BAR_HEIGHT + AURORA_FLYOUT_GAP},
                .size = Size{.width = AURORA_FLYOUT_WIDTH,
                             .height = static_cast<float>(entry_count) * AURORA_ITEM_HEIGHT}};
}

/// @brief 弹窗第 index 条中心的本地坐标。
/// @param index 条目序号。
/// @return 该条中心（本地坐标）。
[[nodiscard]] auto entry_center(int index) -> Point {
    const Rect flyout = flyout_rect(5);
    return Point{.x = flyout.origin.x + (flyout.size.width * 0.5F),
                 .y = flyout.origin.y + ((static_cast<float>(index) + 0.5F) * AURORA_ITEM_HEIGHT)};
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

/// @brief 按真实交互路径展开 Snap 弹窗：悬停最大化钮 → 等过阈值 → 再发一次 Move。
/// @param root 派发根（弹窗宿主所在的树）。
/// @param bar 被测标题栏（须已完成布局）。
/// @param origin 标题栏在根坐标系中的原点。
auto hover_open_snap_flyout(Widget &root, TitleBar &bar, const Point &origin) -> void {
    const Point center = maximize_center();
    MouseEvent first = move_to(origin.x + center.x, origin.y + center.y);
    EventDispatcher::dispatch(root, first, nullptr);
    // 阈值 400ms：等过它再发第二次 Move，控件即在 Move 分支展开弹窗。
    std::this_thread::sleep_for(std::chrono::milliseconds(AURORA_HOVER_DELAY_MS + 60));
    MouseEvent second = move_to(origin.x + center.x, origin.y + center.y);
    EventDispatcher::dispatch(root, second, nullptr);
    (void)bar;
}

}  // namespace

AURORA_TEST_CASE(snap_flyout_point_reaches_title_bar_in_column_host) {
    // 核心证人：弹窗画在栏盒之外，经 Column 宿主派发时该点必须进链并止于 TitleBar。
    int snap_hits = 0;
    const std::shared_ptr<TitleBar> bar = make_title_bar(snap_hits);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect bb = child_box(*root, *bar);
    AURORA_TEST_REQUIRE(bb.size.width > 0.0F);
    AURORA_TEST_REQUIRE(bar->snap_entry_count() == 5U);  // 内置 4 + 自定义 1
    hover_open_snap_flyout(*root, *bar, bb.origin);
    AURORA_TEST_REQUIRE(bar->snap_open());

    const Point local = entry_center(4);  // 自定义项排在内置 4 项之后
    const Point probe{.x = bb.origin.x + local.x, .y = bb.origin.y + local.y};
    // 前提：该点确实在 TitleBar 自身布局盒之外（否则覆盖区前提不成立）。
    AURORA_TEST_REQUIRE_FALSE(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bb.size}.contains(local));

    const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(bar.get()));
}

AURORA_TEST_CASE(snap_flyout_press_fires_custom_entry_via_real_dispatch) {
    // 端到端：弹窗自定义项的 Press 经真实派发触发回调，并收起弹窗。
    int snap_hits = 0;
    const std::shared_ptr<TitleBar> bar = make_title_bar(snap_hits);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect bb = child_box(*root, *bar);
    AURORA_TEST_REQUIRE(bb.size.width > 0.0F);
    hover_open_snap_flyout(*root, *bar, bb.origin);
    AURORA_TEST_REQUIRE(bar->snap_open());

    const Point local = entry_center(4);
    MouseEvent down = press(bb.origin.x + local.x, bb.origin.y + local.y);
    EventDispatcher::dispatch(*root, down, nullptr);
    MouseEvent up = release(bb.origin.x + local.x, bb.origin.y + local.y);
    EventDispatcher::dispatch(*root, up, nullptr);
    AURORA_TEST_CHECK_EQ(snap_hits, 1);
    AURORA_TEST_CHECK_FALSE(bar->snap_open());  // 命中条目后收起
}

AURORA_TEST_CASE(snap_flyout_point_reaches_title_bar_in_lazy_list_item) {
    // 祖先换成 LazyList：条目盒只有行高（60dp），弹窗条目 4 位于 y∈[144,170] 越出条目盒。
    int snap_hits = 0;
    const std::shared_ptr<TitleBar> bar = make_title_bar(snap_hits);
    auto other = std::make_shared<TitleBar>();
    other->set_height(AURORA_BAR_HEIGHT);
    auto list = std::make_shared<LazyList>(2,
                                           [bar, other](int index) -> Node {
                                               return index == 0 ? Node{bar} : Node{other};
                                           },
                                           60.0F);
    LayoutEngine::layout(*list, host_constraints());
    hover_open_snap_flyout(*list, *bar, Point{.x = 0.0F, .y = 0.0F});  // 条目 0 位于视口顶端
    AURORA_TEST_REQUIRE(bar->snap_open());

    const Point probe = entry_center(4);
    AURORA_TEST_REQUIRE(probe.y > 60.0F);  // 前提：已越出条目盒

    const std::vector<HitNode> chain = list->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(bar.get()));

    MouseEvent down = press(probe.x, probe.y);
    EventDispatcher::dispatch(*list, down, nullptr);
    // 必须配对 Release：派发器是进程内单例且 Press 会建立指针捕获，不解除会串到后续用例
    // （其 Move 被投递给上一条已失效的命中链，本套件后面的悬停开窗将静默失败）。
    MouseEvent up = release(probe.x, probe.y);
    EventDispatcher::dispatch(*list, up, nullptr);
    AURORA_TEST_CHECK_EQ(snap_hits, 1);
}

AURORA_TEST_CASE(closed_title_bar_does_not_claim_flyout_area) {
    // 收起态逐位不变 + 关闭后陈旧点不中。
    int snap_hits = 0;
    const std::shared_ptr<TitleBar> bar = make_title_bar(snap_hits);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect bb = child_box(*root, *bar);
    AURORA_TEST_REQUIRE(bb.size.width > 0.0F);
    const Point local = entry_center(4);
    const Point probe{.x = bb.origin.x + local.x, .y = bb.origin.y + local.y};

    AURORA_TEST_CHECK(root->hit_test_chain(probe, host_box(), BuildContext{}).empty());
    AURORA_TEST_CHECK_FALSE(bar->covers_extra_hit_box(local, BuildContext{}));

    hover_open_snap_flyout(*root, *bar, bb.origin);
    AURORA_TEST_REQUIRE(bar->snap_open());
    AURORA_TEST_REQUIRE_FALSE(root->hit_test_chain(probe, host_box(), BuildContext{}).empty());

    // 点弹窗外（栏条空白）收起：用一次 Press 落在栏条左端、弹窗之外。
    MouseEvent dismiss = press(bb.origin.x + 2.0F, bb.origin.y + 2.0F);
    EventDispatcher::dispatch(*root, dismiss, nullptr);
    AURORA_TEST_CHECK_FALSE(bar->snap_open());
    // 配对 Release：不解除指针捕获会让后续用例的 Move 被投递给本用例已销毁的命中链。
    MouseEvent dismiss_up = release(bb.origin.x + 2.0F, bb.origin.y + 2.0F);
    EventDispatcher::dispatch(*root, dismiss_up, nullptr);

    const std::vector<HitNode> stale = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_CHECK(deepest(stale) != static_cast<const Widget *>(bar.get()));
    AURORA_TEST_CHECK_EQ(snap_hits, 0);
}

AURORA_TEST_CASE(compat_entry_and_dispatch_entry_agree_pointwise) {
    // 两入口逐点一致（单源化的前提）：同一棵树、同一批点，`hit_test`（兼容入口）与
    // `hit_test_chain`（派发入口）必须判到同一个控件——弹窗分支此前要求「点在自身盒内」而
    // 与派发入口分叉，本用例钉住二者已同源。
    int snap_hits = 0;
    const std::shared_ptr<TitleBar> bar = make_title_bar(snap_hits);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect bb = child_box(*root, *bar);
    AURORA_TEST_REQUIRE(bb.size.width > 0.0F);
    hover_open_snap_flyout(*root, *bar, bb.origin);
    AURORA_TEST_REQUIRE(bar->snap_open());

    // 循环变量取整型（bugprone-float-loop-counter）：55 × 11 个点与「y: 0→190 / x: 4→320」的
    // 浮点取点集合一致。
    for (int iy = 0; iy < 55; ++iy) {
        const float y = static_cast<float>(iy) * 3.5F;
        for (int ix = 0; ix < 11; ++ix) {
            const float x = 4.0F + (static_cast<float>(ix) * 29.0F);
            const Point probe{.x = x, .y = y};
            const Widget *compat = root->hit_test(probe, host_box(), BuildContext{});
            const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
            AURORA_TEST_CHECK_EQ(compat, deepest(chain));
        }
    }
    AURORA_TEST_CHECK_EQ(snap_hits, 0);  // 命中查询不得有副作用
}

}  // namespace aurora::test_cases::utest_title_bar
