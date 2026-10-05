/// 测试类型: unit
/// 目标单元: include/aurora/widget/dropdown.h
/// 测试说明: 覆盖 Dropdown 下拉面板的「覆盖绘制 ⇄ 命中链」契约——面板画在自身布局盒之外，
/// 而真实派发（`EventDispatcher` 只取 `hit_test_chain`）在祖先侧按**布局盒**闸控下降，故面板
/// 必须由 `extra_hit_box()` 声明才会被闸并入。判据分七条：嵌 Column / 嵌 LazyList 条目两种宿主
/// 下面板区可点、面板区外不抢（仍在面板下方的点归下方兄弟按钮）、收起态逐位不变、展开结束后
/// 陈旧点不中、兼容入口（`hit_test`）与派发入口（`hit_test_chain`）逐点一致、
/// `extra_hit_box()` 与面板矩形同域。
///
/// 回归背景：修复前 Dropdown 只覆写 `on_hit_test`（兼容入口），派发入口 `on_hit_test_chain`
/// 恒返回空 ⇒ 面板看得见、点不中（选中回调永不触发）。
///
/// 探针取点纪律：面板矩形由**公开状态实测**重建（主框高 / 行高 / 选项数 / 布局宽），不读私有
/// 成员；取点前先断言「该点确实在布局盒之外」，否则覆盖区前提不成立、判据会退化成空转。

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/dropdown.h"
#include "aurora/widget/lazy_list.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_dropdown {

namespace {

constexpr float AURORA_HOST_WIDTH = 320.0F;  ///< 宿主视口宽（dp）
constexpr float AURORA_HOST_HEIGHT = 260.0F;  ///< 宿主视口高（dp）
constexpr float AURORA_BOX_HEIGHT = 30.0F;  ///< 主框高（dp；与 set_box_height 对齐）
constexpr float AURORA_ITEM_HEIGHT = 26.0F;  ///< 选项行高（dp；与 set_item_height 对齐）
constexpr float AURORA_SIBLING_GAP = 60.0F;  ///< 下方兄弟按钮与主框的间距（让按钮盒越过面板底边）

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

/// @brief 选中回调观测器：记录回调次数与最后一次序号。
struct ChangeLog {
    int count = 0;
    int last = -1;
};

/// @brief 构造被测下拉：三个选项，主框高/行高显式钉住（面板几何的独立基准由此可算）。
/// @param log 选中回调观测器（引用，生命周期覆盖整条用例）。
/// @return 已挂回调的 Dropdown（收起态）。
auto make_dropdown(ChangeLog &log) -> std::shared_ptr<Dropdown> {
    auto dd = std::make_shared<Dropdown>(std::vector<std::string>{"Alpha", "Beta", "Gamma"});
    dd->set_box_height(AURORA_BOX_HEIGHT);
    dd->set_item_height(AURORA_ITEM_HEIGHT);
    dd->set_on_change([&log](int index) -> void {
        ++log.count;
        log.last = index;
    });
    return dd;
}

/// @brief 面板矩形（**本地坐标**，原点即主框左上角）：由公开状态独立重建，不读私有成员。
/// @param dd 已布局的下拉（取布局宽度）。
/// @return 面板矩形；选项数为 0 时高度为零。
[[nodiscard]] auto panel_rect(const Dropdown &dd) -> Rect {
    return Rect{
        .origin = Point{.x = 0.0F, .y = AURORA_BOX_HEIGHT},
        .size = Size{.width = dd.size().width, .height = static_cast<float>(dd.option_count()) * AURORA_ITEM_HEIGHT}};
}

/// @brief 面板第 index 行中心的本地坐标。
/// @param dd 已布局的下拉。
/// @param index 选项序号。
/// @return 该行中心（本地坐标）。
[[nodiscard]] auto item_center(const Dropdown &dd, int index) -> Point {
    return Point{.x = dd.size().width * 0.5F,
                 .y = AURORA_BOX_HEIGHT + ((static_cast<float>(index) + 0.5F) * AURORA_ITEM_HEIGHT)};
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

AURORA_TEST_CASE(panel_point_reaches_dropdown_in_column_host) {
    // 核心证人：面板画在布局盒之外，经 Column 宿主派发时该点必须进链并止于 Dropdown。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{dd}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect db = child_box(*root, *dd);
    AURORA_TEST_REQUIRE(db.size.width > 0.0F);
    dd->set_open(true);

    const Point local = item_center(*dd, 1);
    const Point probe{.x = db.origin.x + local.x, .y = db.origin.y + local.y};
    // 前提：该点确实在 Dropdown 自身布局盒之外（否则覆盖区前提不成立）。
    AURORA_TEST_REQUIRE_FALSE(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = db.size}.contains(local));

    const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(dd.get()));
}

AURORA_TEST_CASE(panel_press_selects_option_via_real_dispatch) {
    // 端到端：Press 开合走真实派发，面板行 Press 触发 on_change 并改写选中序号。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    auto btn = Button(ButtonProps{.label = "Below"});
    int btn_hits = 0;
    btn.set_on_click([&btn_hits]() -> void { ++btn_hits; });
    auto root =
        std::make_shared<Column>(ColumnProps{.children = {Node{dd}, Node{std::move(btn)}}, .gap = AURORA_SIBLING_GAP});
    LayoutEngine::layout(*root, host_constraints());

    const Rect db = child_box(*root, *dd);
    AURORA_TEST_REQUIRE(db.size.width > 0.0F);

    // 主框内 Press+Release：展开（真实派发路径，不直接改状态）。
    MouseEvent open_press = press(db.origin.x + (db.size.width * 0.5F), db.origin.y + (AURORA_BOX_HEIGHT * 0.5F));
    EventDispatcher::dispatch(*root, open_press, nullptr);
    MouseEvent open_release = release(open_press.position.x, open_press.position.y);
    EventDispatcher::dispatch(*root, open_release, nullptr);
    AURORA_TEST_REQUIRE(dd->is_open());

    // 面板第 1 行：选中并收起。
    const Point local = item_center(*dd, 1);
    MouseEvent pick = press(db.origin.x + local.x, db.origin.y + local.y);
    EventDispatcher::dispatch(*root, pick, nullptr);
    AURORA_TEST_CHECK_EQ(log.count, 1);
    AURORA_TEST_CHECK_EQ(log.last, 1);
    AURORA_TEST_CHECK_EQ(dd->selected_index(), 1);
    AURORA_TEST_CHECK_EQ(std::string{dd->selected_text()}, std::string{"Beta"});
    // 面板没有把下方兄弟按钮的点击抢走（该次点击落在面板内，按钮不应响）。
    AURORA_TEST_CHECK_EQ(btn_hits, 0);
}

AURORA_TEST_CASE(panel_point_reaches_dropdown_in_lazy_list_item) {
    // 祖先换成 LazyList：条目盒只有行高（60dp），面板 y∈[30,108] 越出条目盒 ⇒ 闸必须并入追加盒。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    int other_hits = 0;
    auto other = std::make_shared<Button>(ButtonProps{.label = "Other"});
    other->set_on_click([&other_hits]() -> void { ++other_hits; });

    auto list = std::make_shared<LazyList>(
        2, [dd, other](int index) -> Node { return index == 0 ? Node{dd} : Node{other}; }, 60.0F);
    LayoutEngine::layout(*list, host_constraints());
    dd->set_open(true);

    // 条目 0 的盒 = (0, 0, 视口宽, 60)：面板第 2 行中心 y = 95 已越出条目盒。
    const Point probe = item_center(*dd, 2);
    AURORA_TEST_REQUIRE(probe.y > 60.0F);

    const std::vector<HitNode> chain = list->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(dd.get()));

    MouseEvent pick = press(probe.x, probe.y);
    EventDispatcher::dispatch(*list, pick, nullptr);
    AURORA_TEST_CHECK_EQ(log.count, 1);
    AURORA_TEST_CHECK_EQ(log.last, 2);
    AURORA_TEST_CHECK_EQ(other_hits, 0);
    MouseEvent up = release(probe.x, probe.y);  // 配对 Release：解除派发器单例的指针捕获
    EventDispatcher::dispatch(*list, up, nullptr);
}

AURORA_TEST_CASE(closed_dropdown_never_covers_the_sibling_below) {
    // 收起态逐位不变：追加盒为空，主框下方的点照旧归下方兄弟按钮。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    auto btn = Button(ButtonProps{.label = "Below"});
    int btn_hits = 0;
    btn.set_on_click([&btn_hits]() -> void { ++btn_hits; });
    auto root =
        std::make_shared<Column>(ColumnProps{.children = {Node{dd}, Node{std::move(btn)}}, .gap = AURORA_SIBLING_GAP});
    LayoutEngine::layout(*root, host_constraints());

    const Widget &sibling = root->child_nodes().back().widget();
    const Rect bb = child_box(*root, sibling);
    AURORA_TEST_REQUIRE(bb.size.height > 0.0F);
    const Point probe{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.origin.y + (bb.size.height * 0.5F)};

    const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(&sibling));

    MouseEvent down = press(probe.x, probe.y);
    EventDispatcher::dispatch(*root, down, nullptr);
    MouseEvent up = release(probe.x, probe.y);
    EventDispatcher::dispatch(*root, up, nullptr);
    AURORA_TEST_CHECK_EQ(btn_hits, 1);
    AURORA_TEST_CHECK_EQ(log.count, 0);
}

AURORA_TEST_CASE(point_below_open_panel_still_goes_to_sibling) {
    // 覆盖区外不抢：面板底边以下的点（落在下方兄弟按钮盒内）仍归兄弟，不被追加盒吞掉。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    auto btn = Button(ButtonProps{.label = "Below"});
    int btn_hits = 0;
    btn.set_on_click([&btn_hits]() -> void { ++btn_hits; });
    auto root =
        std::make_shared<Column>(ColumnProps{.children = {Node{dd}, Node{std::move(btn)}}, .gap = AURORA_SIBLING_GAP});
    LayoutEngine::layout(*root, host_constraints());

    const Rect db = child_box(*root, *dd);
    const Widget &sibling = root->child_nodes().back().widget();
    const Rect bb = child_box(*root, sibling);
    AURORA_TEST_REQUIRE(bb.size.height > 0.0F);
    dd->set_open(true);

    const float panel_bottom = db.origin.y + panel_rect(*dd).bottom();
    // 取按钮盒内、但已在面板底边以下的点。
    const Point probe{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.bottom() - 2.0F};
    AURORA_TEST_REQUIRE(probe.y > panel_bottom);  // 前提：该点确实在面板之外（否则判据空转）
    AURORA_TEST_REQUIRE(bb.contains(probe));

    const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(&sibling));

    MouseEvent down = press(probe.x, probe.y);
    EventDispatcher::dispatch(*root, down, nullptr);
    MouseEvent up = release(probe.x, probe.y);
    EventDispatcher::dispatch(*root, up, nullptr);
    AURORA_TEST_CHECK_EQ(btn_hits, 1);
    AURORA_TEST_CHECK_EQ(log.count, 0);
}

AURORA_TEST_CASE(stale_panel_point_misses_after_close) {
    // 展开结束后陈旧点不中：收起即追加盒为空，上一帧还在面板内的点不得再命中 Dropdown。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{dd}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect db = child_box(*root, *dd);
    AURORA_TEST_REQUIRE(db.size.width > 0.0F);
    const Point local = item_center(*dd, 0);
    const Point probe{.x = db.origin.x + local.x, .y = db.origin.y + local.y};

    dd->set_open(true);
    AURORA_TEST_REQUIRE_FALSE(root->hit_test_chain(probe, host_box(), BuildContext{}).empty());

    dd->set_open(false);
    const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_CHECK(deepest(chain) != static_cast<const Widget *>(dd.get()));
    AURORA_TEST_CHECK_FALSE(dd->covers_extra_hit_box(local, BuildContext{}));
    AURORA_TEST_CHECK_EQ(log.count, 0);
}

AURORA_TEST_CASE(compat_entry_and_dispatch_entry_agree_pointwise) {
    // 两入口逐点一致（单源化的前提）：同一棵树、同一批点，`hit_test`（兼容入口）判定的目标
    // 必须与 `hit_test_chain`（派发入口）的最深节点相同；两者都判「无」时亦须相同。
    // 树上只放可点击控件（Button/Dropdown）——纯叶控件（如 Text）不入链是另一条既有规则。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    auto above = Button(ButtonProps{.label = "Above"});
    auto below = Button(ButtonProps{.label = "Below"});
    auto root = std::make_shared<Column>(
        ColumnProps{.children = {Node{std::move(above)}, Node{dd}, Node{std::move(below)}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());
    dd->set_open(true);

    // 取点步距沿用浮点版本，但循环变量必须是整型（bugprone-float-loop-counter）：
    // 40 × 9 个点覆盖宿主视口，与「y: 0→140 / x: 4→320」的浮点循环取点集合一致。
    for (int iy = 0; iy < 40; ++iy) {
        const float y = static_cast<float>(iy) * 3.5F;
        for (int ix = 0; ix < 9; ++ix) {
            const float x = 4.0F + (static_cast<float>(ix) * 37.0F);
            const Point probe{.x = x, .y = y};
            const Widget *compat = root->hit_test(probe, host_box(), BuildContext{});
            const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
            AURORA_TEST_CHECK_EQ(compat, deepest(chain));
        }
    }
}

AURORA_TEST_CASE(extra_hit_box_matches_panel_rect_and_compat_entry) {
    // 追加盒的域 == 面板矩形，且与兼容入口同一份判据：收起态恒空，展开态逐点吻合。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    LayoutEngine::layout(*dd, host_constraints());
    AURORA_TEST_REQUIRE(dd->size().width > 0.0F);
    const Rect self{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = dd->size()};

    // 收起态：追加盒为空（可命中区 == 自身布局盒）。
    AURORA_TEST_CHECK_FALSE(dd->extra_hit_box(BuildContext{}).has_value());
    AURORA_TEST_CHECK_FALSE(dd->covers_extra_hit_box(item_center(*dd, 0), BuildContext{}));

    dd->set_open(true);
    const Rect panel = panel_rect(*dd);
    const int nx = static_cast<int>((dd->size().width - 2.0F) / 11.0F) + 1;
    for (int iy = 0; iy < 40; ++iy) {
        const float y = static_cast<float>(iy) * 3.5F;
        for (int ix = 0; ix < nx; ++ix) {
            const float x = 2.0F + (static_cast<float>(ix) * 11.0F);
            const Point p{.x = x, .y = y};
            const bool in_panel = panel.contains(p);
            AURORA_TEST_CHECK_EQ(dd->covers_extra_hit_box(p, BuildContext{}), in_panel);
            // 面板内的点：兼容入口同样认（判定同源）；面板外且在盒外的点：两入口都不认。
            AURORA_TEST_CHECK_EQ(dd->hit_test(p, self, BuildContext{}) == dd.get(), self.contains(p) || in_panel);
        }
    }
}

}  // namespace aurora::test_cases::utest_dropdown
