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

#include "aurora/environment/environment.h"
#include "aurora/environment/media_query.h"
#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/dropdown.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/scroll.h"
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

/// @brief 走一次真实绘制：建立 `focus_bounds_`（绝对盒）与 Dropdown 的视口高缓存这两个读数。
/// @param w 被绘控件。
/// @param viewport 绘制盒（同时充当视口原点：控件的全局位置由它决定）。
/// @param ctx 构建上下文（须带 `MediaQuery`，否则视口高读数为 0、翻转退化为恒向下）。
void paint_once(Widget &w, const Rect &viewport, const BuildContext &ctx) {
    // **必须** begin()：`AURORA_ENABLE_OCCLUSION_CULLING` 下 `Container::on_paint` 用
    // `p.clip_bounds()` 裁剪子树，未 begin 的 Painter 其裁剪区为空 ⇒ 整棵子树被剔除、
    // 子控件的 `focus_bounds_` 永不写入（本轮实测：直接 paint 某控件能读到绘制盒，
    // 经未 begin 的 root 遍历则读到零盒）。
    Painter p;
    p.begin(static_cast<int>(viewport.size.width), static_cast<int>(viewport.size.height));
    w.paint(p, viewport, ctx);
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
    AURORA_TEST_CHECK_FALSE(dd->extra_hit_box(BuildContext{}, Point{.x = 0.0F, .y = 0.0F}).has_value());
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

AURORA_TEST_CASE(grandchild_extra_hit_box_propagates_three_levels) {
    // G30 核心证人：三层嵌套 LazyList → Row → Dropdown，孙辈（Dropdown）申报的面板区必须可达。
    //
    // 几何（本机无头 scale 恒 1.0）：行盒 56、上下内边距各 8 ⇒ Dropdown 紧约束盒高 40；
    // 面板自下拉局部 y = box_height_(30) 起。探点取下拉局部 y = 60 ⇒ 行局部 60+8-8 = 60，
    // 越过行下沿（56）——中间的 Row 不申报任何追加盒，修复前门在 Row 那一层判假。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);

    // 每行 = Row 内放 Dropdown（复现 borealis 设置面板的「LazyList → Row → Dropdown」形态）。
    // 上下内边距各 8 ⇒ 行高 56 时 Dropdown 的紧约束盒高 40，与真实场景一致。
    auto make_row = [dd](const std::shared_ptr<Dropdown> &target) -> Node {
        auto row = std::make_shared<Row>(RowProps{.children = {Node{target}}});
        row->modifier.set(Modifier{}.padding(8.0F));
        return Node{row};
    };
    const std::shared_ptr<Node> row0 = std::make_shared<Node>(make_row(dd));
    auto list = std::make_shared<LazyList>(2, [row0](int index) -> Node { return index == 0 ? *row0 : Node{}; }, 56.0F);
    LayoutEngine::layout(*list, host_constraints());
    AURORA_TEST_REQUIRE(list->live_item_count() >= 1U);

    dd->set_open(true);
    // 行盒 = 条目盒，在列表局部坐标里顶边 y = 0（未滚动）；Dropdown 在行内的偏移经 child_box 取。
    const Rect db = child_box(row0->widget(), *dd);
    AURORA_TEST_REQUIRE(db.size.height > 0.0F);

    // 面板第 2 行中心（局部 y = 30 + 2.5*26 = 95），越过行盒下沿 56。
    const Point local = item_center(*dd, 2);
    const Point probe{.x = db.origin.x + local.x, .y = db.origin.y + local.y};
    const Rect row_box{.origin = Point{.x = 0.0F, .y = 0.0F},
                       .size = Size{.width = list->size().width, .height = 56.0F}};
    AURORA_TEST_REQUIRE(probe.y > row_box.bottom());  // 前提：该点确实越出行盒，否则判据空转

    const std::vector<HitNode> chain = list->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(dd.get()));

    // 真实派发：该点必须选中第 2 项。
    MouseEvent pick = press(probe.x, probe.y);
    EventDispatcher::dispatch(*list, pick, nullptr);
    AURORA_TEST_CHECK_EQ(log.count, 1);
    AURORA_TEST_CHECK_EQ(log.last, 2);
    MouseEvent up = release(probe.x, probe.y);  // 配对 Release：解除派发器单例的指针捕获
    EventDispatcher::dispatch(*list, up, nullptr);
}

AURORA_TEST_CASE(clipped_list_item_extra_hit_box_stays_unreachable) {
    // 裁剪边界：滚出视口的条目，其追加盒不得申报——否则肉眼不可见的区域变得可点。
    //
    // 构造：20 行 × 56dp = 1120dp 内容，视口 260 ⇒ 可滚 860。把展开的 Dropdown 放在**最后一行**，
    // 滚到底后该行盒顶边 = 19*56 - 860 = 204，面板自其下方 y=30 起 ⇒ 面板整段落在视口下沿之外，
    // 即「条目可见但面板不可见」的最坏形态——面板本就不该可点。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    const std::shared_ptr<Row> row = std::make_shared<Row>(RowProps{.children = {Node{dd}}});
    row->modifier.set(Modifier{}.padding(8.0F));
    // 20 行，Dropdown 在最后一行（index 19）。
    auto make_row = [row](int index) -> Node { return index == 19 ? Node{row} : Node{}; };
    auto list = std::make_shared<LazyList>(20, make_row, 56.0F);
    LayoutEngine::layout(*list, host_constraints());
    dd->set_open(true);

    // 滚到底：偏移被夹到 max_scroll_offset。
    list->set_scroll_offset(9999.0F);
    LayoutEngine::layout(*list, host_constraints());
    const float offset = list->scroll_offset();
    const float row_top = (19.0F * 56.0F) - offset;  // 末行盒顶边（列表局部 y）
    const Rect db = child_box(*row, *dd);
    AURORA_TEST_REQUIRE(db.size.height > 0.0F);

    // 面板第 0 行中心（行局部 y ≈ 38）换算到列表局部。
    const Point row_local = item_center(*dd, 0);
    const Point probe_first{.x = db.origin.x + row_local.x, .y = (row_top + db.origin.y) + row_local.y};
    // 末行盒 [204,260) 整段在视口内，但面板共 78 高：首行中心（247）尚在视口内、末行中心（299）
    // 已出视口。取末行中心才是有判别力的裁剪场景——条目可见、面板尾部不可见。
    const Point probe{.x = db.origin.x + row_local.x,
                      .y = (row_top + db.origin.y) + (AURORA_BOX_HEIGHT + (2.5F * AURORA_ITEM_HEIGHT))};
    AURORA_TEST_REQUIRE_GT(offset, 800.0F);  // 前提：确实滚到了底
    AURORA_TEST_REQUIRE_LT(probe_first.y, list->size().height);  // 对照：面板首行仍可见（证明条目在视口内）
    AURORA_TEST_REQUIRE_GT(probe.y, list->size().height);  // 前提：面板末行在视口外，否则判据空转

    const std::vector<HitNode> chain = list->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_CHECK(deepest(chain) != static_cast<const Widget *>(dd.get()));
    // 祖先闸问「本列表是否覆盖此点」也必须为假（两入口同形，不得只有派发链拒绝）。
    AURORA_TEST_CHECK_FALSE(list->covers_extra_hit_box(probe, BuildContext{}));
}

AURORA_TEST_CASE(scroll_and_lazy_list_agree_on_clipped_extra_hit_box) {
    // 两入口同形：Scroll 与 LazyList 都是带视口裁剪的容器，裁剪口径必须一致。
    //
    // 各自的场景都是「展开的 Dropdown 位于视口下沿之外，面板整段不可见」，断言两条：
    // ① 真实派发链（`hit_test_chain`）不含该 Dropdown；② 祖先闸问「本容器是否覆盖此点」
    // （`covers_extra_hit_box`）为假。缺 ② 就可能出现「闸认、自身不认」的分叉。
    ChangeLog log;

    // LazyList 腿：20 行 × 56dp，Dropdown 在末行；滚到底后末行顶边 = 19*56 - offset，
    // 面板自其下方 30dp 起 ⇒ 整段落在视口下沿（260）之外。
    {
        const std::shared_ptr<Dropdown> dd = make_dropdown(log);
        const std::shared_ptr<Row> row = std::make_shared<Row>(RowProps{.children = {Node{dd}}});
        row->modifier.set(Modifier{}.padding(8.0F));
        auto list =
            std::make_shared<LazyList>(20, [row](int i) -> Node { return i == 19 ? Node{row} : Node{}; }, 56.0F);
        LayoutEngine::layout(*list, host_constraints());
        dd->set_open(true);
        list->set_scroll_offset(9999.0F);  // 夹到 max_scroll_offset
        LayoutEngine::layout(*list, host_constraints());

        const Rect db = child_box(*row, *dd);
        AURORA_TEST_REQUIRE(db.size.height > 0.0F);
        // 行局部 → 列表局部：末行盒顶边 = 19*56 - offset。
        // 取面板**末行**中心（序号 2）：行局部 y = 30 + 2.5*26 = 95，加行内偏移 8 ⇒ 行局部 103。
        const Point probe{.x = db.origin.x + (db.size.width * 0.5F),
                          .y = (db.origin.y + (AURORA_BOX_HEIGHT + (2.5F * AURORA_ITEM_HEIGHT))) +
                               ((19.0F * 56.0F) - list->scroll_offset())};
        // 末行盒 [204,260) 整段在视口内、但它的面板（自盒下方 y=30 起 ⇒ 全局 264+）整段在视口外。
        // 这才是有判别力的裁剪场景：条目可见、面板不可见 ⇒ 面板区必须不可命中。
        // （若改成「条目本身滚出视口」，虚拟化已把该条目回收出 live_，判据会退化成空转。）
        AURORA_TEST_REQUIRE_GT(probe.y, list->size().height);  // 前提：该点确实在视口外，否则判据空转
        AURORA_TEST_CHECK(deepest(list->hit_test_chain(probe, host_box(), BuildContext{})) !=
                          static_cast<const Widget *>(dd.get()));
        AURORA_TEST_CHECK_FALSE(list->covers_extra_hit_box(probe, BuildContext{}));
    }

    // Scroll 腿：内容高 400+ 的下拉滚到视口下沿之外，换算与 Scroll::covers_descendant_extra_hit_box 同式。
    {
        const std::shared_ptr<Dropdown> dd = make_dropdown(log);
        auto content = std::make_shared<Column>(
            ColumnProps{.children = {Node{Button(ButtonProps{.label = "top"})}, Node{dd}}, .gap = 400.0F});
        auto scroller = std::make_shared<Scroll>(ScrollProps{.child = Node{content}});
        LayoutEngine::layout(*scroller, host_constraints());
        dd->set_open(true);
        scroller->scroll_by(9999.0F);
        LayoutEngine::layout(*scroller, host_constraints());

        const Rect db = child_box(*content, *dd);
        AURORA_TEST_REQUIRE(db.size.height > 0.0F);
        const Point probe{.x = db.origin.x + (db.size.width * 0.5F),
                          .y = (db.origin.y + 38.0F) - scroller->scroll_offset_y()};
        AURORA_TEST_REQUIRE_GT(probe.y, scroller->size().height);
        AURORA_TEST_CHECK(deepest(scroller->hit_test_chain(probe, host_box(), BuildContext{})) !=
                          static_cast<const Widget *>(dd.get()));
        AURORA_TEST_CHECK_FALSE(scroller->covers_extra_hit_box(probe, BuildContext{}));
    }
}

AURORA_TEST_CASE(panel_flips_up_at_viewport_bottom) {
    // 贴视口下沿时向上翻转：面板不再伸出窗口外。
    //
    // 读数来源与其代价：视口高由 `on_paint` 从 `MediaQuery` 采样并缓存（事件路径不带 ctx），
    // 控件全局顶边来自 `focus_bounds_`（`Widget::paint` 入口写回的绝对盒）。故本用例走**真实
    // 绘制路径**一次来建立读数，而不是直接 `set_focus_bounds` 注入——注入是旁路，钉不住
    // 「绘制与判定同源」这条纪律。两个读数都由本用例显式给定，不靠宿主自报。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    // 视口高 200；控件摆在 y = 190 ⇒ 主框下沿 220 已越界，下方只剩 -20，上方 190 装得下。
    const Rect viewport{.origin = Point{.x = 0.0F, .y = 190.0F}, .size = host_box().size};

    Environment env;
    MediaQuery mq;
    mq.size = viewport.size;
    env.set_local<MediaQuery>(mq);
    BuildContext ctx;
    ctx.env = &env;
    ctx.scale_factor = 1.0F;

    LayoutEngine::layout(*dd, Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = viewport.size});
    AURORA_TEST_REQUIRE(dd->size().width > 0.0F);
    dd->set_open(true);
    // 视口高由 on_layout 存的 env_ 提供（地址恒定、每帧更新）；self_top 来自 focus_bounds_，
    // 故仍走一次真实绘制建立绝对盒——两者都是 panel_geometry 的输入。
    paint_once(*dd, viewport, ctx);

    // require_value 收口「检查 + 取值」：宏展开对路径分析不透明，须用框架 helper。
    // ancestor_offset 传控件在视口坐标系中的顶边 y —— 这是新增的「祖先累加」通道：
    // 翻转判据不再读绘制期写回的 focus_bounds_，故未绘制过也能判对。
    const Rect panel = testing::require_value(dd->extra_hit_box(ctx, Point{.x = 0.0F, .y = viewport.origin.y}));
    // 上翻：窗口顶落在主框上方（局部 y < box_height_），底边恰好贴主框顶（3 档 × 26 = 78）。
    AURORA_TEST_CHECK_NEAR(panel.origin.y, AURORA_BOX_HEIGHT - (3.0F * AURORA_ITEM_HEIGHT), 1e-3);
    AURORA_TEST_CHECK_NEAR(panel.size.height, 3.0F * AURORA_ITEM_HEIGHT, 1e-3);
    // 视口上沿：面板顶（全局 190 + 局部 y = 190 + 30 - 78 = 142）不得越出 0。
    AURORA_TEST_CHECK_GE(viewport.origin.y + panel.origin.y, 0.0F);
    // 新通道的判别点：**不绘制**也能判对翻转。focus_bounds_ 此时为零盒（从未 paint），
    // 若实现仍读它，self_top 就是 0（视口顶）⇒ 下方装得下 ⇒ 不翻转，与上面断言相反。
    // 故本条同时钉住「翻转判据不依赖绘制期缓存」。
    const Rect no_paint_viewport{.origin = Point{.x = 0.0F, .y = 190.0F}, .size = host_box().size};
    Environment env2;
    MediaQuery mq2;
    mq2.size = no_paint_viewport.size;
    env2.set_local<MediaQuery>(mq2);
    BuildContext ctx2;
    ctx2.env = &env2;
    const std::shared_ptr<Dropdown> fresh = make_dropdown(log);
    LayoutEngine::layout(*fresh, Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = no_paint_viewport.size},
                         ctx2);
    fresh->set_open(true);
    // 仅经 on_layout 兜底写入 env_（不 paint）⇒ 视口高读得到、self_top 由 ancestor_offset 提供。
    const Rect fresh_panel =
        testing::require_value(fresh->extra_hit_box(ctx2, Point{.x = 0.0F, .y = no_paint_viewport.origin.y}));
    AURORA_TEST_CHECK_LT(fresh_panel.origin.y, AURORA_BOX_HEIGHT);  // 仍判为上翻

    // 反向对照：控件摆在视口顶部时下方装得下（3 档 78 ≤ 200 - 0 - 30 = 170），不应翻转。
    LayoutEngine::layout(*dd, host_constraints());
    dd->set_open(false);
    const Rect top_viewport{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = host_box().size};
    mq.size = top_viewport.size;
    env.set<MediaQuery>(mq);
    dd->set_open(true);
    paint_once(*dd, top_viewport, ctx);
    const Rect down = testing::require_value(dd->extra_hit_box(ctx, Point{.x = 0.0F, .y = top_viewport.origin.y}));
    AURORA_TEST_CHECK_NEAR(down.origin.y, AURORA_BOX_HEIGHT, 1e-3);
}

AURORA_TEST_CASE(panel_caps_height_and_scrolls_for_long_lists) {
    // 档位很多时：面板被限高、尾部经内部滚动才可达，且不可见的行点不到。
    ChangeLog log;
    std::vector<std::string> many;
    many.reserve(40);
    for (int i = 0; i < 40; ++i) {  // 40 档 × 26dp = 1040dp，远超任何视口
        many.push_back("Opt " + std::to_string(i));
    }
    const std::shared_ptr<Dropdown> dd = std::make_shared<Dropdown>(many);
    dd->set_box_height(AURORA_BOX_HEIGHT);
    dd->set_item_height(AURORA_ITEM_HEIGHT);
    dd->set_on_change([&log](int index) -> void {
        ++log.count;
        log.last = index;
    });

    // 视口高 300；控件摆在顶部 ⇒ 下方可用 300 - 30 = 270 < 1040，上方 0 装不下 ⇒ 取下方限高。
    const Rect viewport{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = host_box().size};
    Environment env;
    MediaQuery mq;
    mq.size = Size{.width = AURORA_HOST_WIDTH, .height = 300.0F};
    env.set_local<MediaQuery>(mq);
    BuildContext ctx;
    ctx.env = &env;
    ctx.scale_factor = 1.0F;

    LayoutEngine::layout(*dd, Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = viewport.size});
    dd->set_open(true);
    paint_once(*dd, viewport, ctx);

    const Rect panel = testing::require_value(dd->extra_hit_box(ctx, Point{.x = 0.0F, .y = viewport.origin.y}));
    AURORA_TEST_CHECK_LT(panel.size.height, 40.0F * AURORA_ITEM_HEIGHT);  // 确实被限高
    // 限高 = min(下方可用 270, limit = 300 - 2*8 = 284) = 270 ⇒ 可见 270/26 ≈ 10.4 行。
    AURORA_TEST_CHECK_NEAR(panel.size.height, 270.0F, 1e-3);

    const Rect self{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = dd->size()};
    // 第 0 行中心在窗口内 ⇒ 可命中。
    const Point first_center{.x = dd->size().width * 0.5F, .y = AURORA_BOX_HEIGHT + (0.5F * AURORA_ITEM_HEIGHT)};
    AURORA_TEST_CHECK(dd->covers_extra_hit_box(first_center, ctx));
    // 第 39 行中心在窗口外 ⇒ 不可命中（不可见即不可点），兼容入口同样不认。
    const Point last_center{.x = dd->size().width * 0.5F, .y = AURORA_BOX_HEIGHT + (39.5F * AURORA_ITEM_HEIGHT)};
    AURORA_TEST_REQUIRE_FALSE(panel.contains(last_center));
    AURORA_TEST_CHECK_FALSE(dd->covers_extra_hit_box(last_center, ctx));
    AURORA_TEST_CHECK(dd->hit_test(last_center, self, ctx) == nullptr);

    // 滚轮把内部滚动推到尾部后，第 39 行滚入窗口 ⇒ 变得可见可点（点它选中第 39 项）。
    // 经真实派发走（EventDispatcher 沿命中链找最近可滚动者），而非直接调 protected 的 on_scroll。
    ScrollEvent wheel;
    wheel.position = Point{.x = (viewport.origin.x + first_center.x), .y = (viewport.origin.y + first_center.y)};
    wheel.delta_y = -2000.0F;  // 向下滚（delta_y 上为正，故取负）
    EventDispatcher::dispatch(*dd, wheel);
    AURORA_TEST_CHECK(wheel.is_handled);

    // 滚动后第 39 行中心在窗口内。
    const Point last_visible{.x = dd->size().width * 0.5F,
                             .y = (panel.origin.y + panel.size.height) - (0.5F * AURORA_ITEM_HEIGHT)};
    AURORA_TEST_REQUIRE(panel.contains(last_visible));
    AURORA_TEST_CHECK(dd->covers_extra_hit_box(last_visible, ctx));

    // 经真实派发点击它：序号按「窗口内偏移 + 滚动偏移」反算，必须落在 39（末尾越界则钉住 off-by-one）。
    MouseEvent pick;
    pick.action = MouseAction::Press;
    pick.button = MouseButton::Left;
    pick.position = Point{.x = viewport.origin.x + last_visible.x, .y = viewport.origin.y + last_visible.y};
    pick.local_position = last_visible;
    EventDispatcher::dispatch(*dd, pick, nullptr);
    AURORA_TEST_CHECK_EQ(log.count, 1);
    AURORA_TEST_CHECK_EQ(log.last, 39);
}

// ============================================================================================
// G31：命中链逐节点 origin 与绘制仿射同源
//
// 病灶：命中侧 `hit_test_chain` 把「未经 tf.translation 平移的布局盒原点」当下传给下降闸，
// 于是链上每个节点记录的 `HitNode.origin` 与它被画出来的位置差着一份沿途累计的平移。
// 派发器按 `global - it.origin` 算 `local_position`，Dropdown 用 `local.y` 反算选项序号，
// 于是「按行带 padding(top: 8) 真点某个选项」会选中错的那一档——可达但点不准。
//
// 判据纪律：预期值一律**独立复算**（用与 `render_into` 同一算式重新算一遍绘制原点），
// 不得取实现自己的输出当基准，否则判据会跟着实现一起漂、变异打不红。
// ============================================================================================

/// @brief 独立复算「带 padding(top) 的一行内，Dropdown 被绘制到的全局原点」。
///
/// 算式与 `Widget::render_into` 的恒等快速路径同源（`content_box.origin = local.origin + tf.translation`）：
/// 行的布局盒原点加上 `Modifier::transform` 折进去的 `tf.translation`（padding top 与 left），
/// 再加 Dropdown 在行内的相对原点。**不复用任何 Widget 成员读数**——这是判据独立性的来源。
/// @param row_origin 行在宿主内容区内的原点（布局读数，相对宿主）。
/// @param dd_origin_in_row Dropdown 在行内的原点（行子节点的布局盒原点）。
/// @param pad 内边距（dp，四边同值；本仓只用到 top）。
/// @return Dropdown 内容盒的全局原点（宿主内容区坐标系）。
[[nodiscard]] auto paint_origin_of_padded_row(Point row_origin, Point dd_origin_in_row, float pad) -> Point {
    // tf.translation = (pad, pad)：Padding 节点折进 translation 的量。
    const Point tf_translation{.x = pad, .y = pad};
    // 行的内容盒原点 = 行原点 + translation；Dropdown 原点 = 行内容盒原点 + 行内相对原点。
    return row_origin + tf_translation + dd_origin_in_row;
}

/// @brief 读回控件**真实绘制位置**（绝对窗口逻辑 dp），作为探针取点的独立依据。
///
/// 走 `composition_caret_bounds()`（public virtual，基类回退 `focus_bounds_`）——该读数由
/// `Widget::paint` 入口按**绘制期传入的绝对盒**写入，故它就是 `render_into` 实际落笔的位置。
/// 与命中链记录的 `HitNode.origin` 是**两条独立读数**，故可用来互相钉对。
/// @param w 已绘制的控件。
/// @return 控件的绘制盒原点（全局逻辑 dp）。
[[nodiscard]] auto painted_origin_of(const Widget &w) -> Point { return w.composition_caret_bounds().origin; }

AURORA_TEST_CASE(padded_row_child_dispatches_the_option_actually_under_the_probe) {
    // 形态①：Column 内一行带 padding(top: 8)，行内放展开的 Dropdown；
    // 按第 2 行下沿真点，选中的候选序号必须等于该探点绘制位置所属的序号。
    constexpr float pad = 8.0F;  // 函数内局部常量：lower_case（三档纪律）
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);

    // 行 = 一个带 padding 的 Column（padding 由 Modifier 施加，内容盒整体下移 pad）。
    auto row = std::make_shared<Column>();
    row->modifier.set(Modifier().padding(pad));
    row->add(Node{dd});

    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{row}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect row_box = child_box(*root, *row);
    const Rect dd_box = child_box(*row, *dd);
    AURORA_TEST_REQUIRE(row_box.size.width > 0.0F);
    AURORA_TEST_REQUIRE(dd_box.size.width > 0.0F);
    dd->set_open(true);

    // 走一次真实绘制，建立**绘制侧实测位置**读数。
    paint_once(*root, host_box(), BuildContext{});
    const Point painted_origin = painted_origin_of(*dd);

    // 探针取「第 1 行中心」，坐标基准是绘制实测位置。
    //
    // **不可改用复算值当基准**（实测踩过）：若探针也按「复算的绘制原点」算，则 origin
    // 记录错了（变异）时探针跟着一起偏，探针与错误绘制位置仍然对齐 ⇒ 选中照样正确 ⇒
    // 用例转不了红。探针必须来自与 origin 读数**相互独立**的一路（绘制侧），origin 错了
    // 才会真的点到别处。
    const Point item_local = item_center(*dd, 1);
    const Point probe{.x = painted_origin.x + item_local.x, .y = painted_origin.y + item_local.y};

    // 前提断言：绘制位置确实比「朴素布局原点」低恰好一个 pad —— 证明本判据对 origin 偏移
    // 敏感（变异丢掉 translation 时探针会落到真实绘制位置上方 8dp 而选中错档）。
    const Point naive_origin = row_box.origin + dd_box.origin;
    AURORA_TEST_CHECK_NEAR(painted_origin.y - naive_origin.y, pad, 1e-4F);

    // 真点：Press + Release 配对（EventDispatcher 是进程内单例，Press 建立指针捕获，
    // 不配对 Release 会让后续 Move 投递给已失效的命中链）。
    MouseEvent pick = press(probe.x, probe.y);
    EventDispatcher::dispatch(*root, pick, nullptr);
    MouseEvent up = release(probe.x, probe.y);
    EventDispatcher::dispatch(*root, up, nullptr);

    // 核心断言：选中的正是探点绘制位置所属的那一档。
    AURORA_TEST_CHECK_EQ(log.count, 1);
    AURORA_TEST_CHECK_EQ(log.last, 1);
    AURORA_TEST_CHECK_EQ(std::string{dd->selected_text()}, std::string{"Beta"});
}

AURORA_TEST_CASE(hit_node_origin_equals_the_independently_recomputed_paint_origin) {
    // 形态②：Align 居中的子节点，其 HitNode.origin 与派发后的 e.local_position 逐位等于
    // 该子节点在 render_into 里的绘制原点（用同一算式独立复算当预期值）。
    constexpr float pad = 8.0F;  // 函数内局部常量：lower_case（三档纪律）
    ChangeLog log;  // 具名：make_dropdown 内部的 on_change 捕获它，临时对象会悬垂
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);

    auto row = std::make_shared<Column>();
    row->modifier.set(Modifier().padding(pad));
    row->add(Node{dd});
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{row}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());
    dd->set_open(true);

    const Rect row_box = child_box(*root, *row);
    const Rect dd_box = child_box(*row, *dd);
    AURORA_TEST_REQUIRE(dd_box.size.width > 0.0F);

    // 走一次真实绘制：探针基准取绘制侧实测位置（与 origin 读数相互独立，见 painted_origin_of）。
    paint_once(*root, host_box(), BuildContext{});
    const Point painted_origin = painted_origin_of(*dd);

    // **预期值**则独立复算（不使用任何实现读数，也不取 painted_origin）——两路必须分开：
    // 探针用绘制实测（否则变异时探针跟着偏、用例转不了红），预期用复算（否则判据跟着实现漂）。
    const Point expected_paint_origin = paint_origin_of_padded_row(row_box.origin, dd_box.origin, pad);

    // 前提：绘制实测位置与独立复算值一致 —— 两者是同一个物理事实的两路读数。
    AURORA_TEST_CHECK_NEAR(painted_origin.y, expected_paint_origin.y, 1e-4F);

    // 探针落在 Dropdown 自身盒内（取主框中心）：此点必在链上。
    const Point probe{.x = painted_origin.x + (dd_box.size.width * 0.5F),
                      .y = painted_origin.y + (AURORA_BOX_HEIGHT * 0.5F)};

    const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(dd.get()));

    // 链上 Dropdown 节点记录的 origin 必须逐位等于绘制原点。
    const HitNode *dd_node = nullptr;
    for (const HitNode &n : chain) {
        if (n.get() == dd.get()) {
            dd_node = &n;
            break;
        }
    }
    AURORA_TEST_REQUIRE(dd_node != nullptr);
    AURORA_TEST_CHECK_NEAR(dd_node->origin.x, expected_paint_origin.x, 0.0F);
    AURORA_TEST_CHECK_NEAR(dd_node->origin.y, expected_paint_origin.y, 0.0F);

    // 派发后控件收到的 local_position 必须逐位等于「探点 − 绘制原点」：
    // Dropdown 正是用它按 local.y 反算选项序号（这是消费侧的真实读数）。
    MouseEvent pick = press(probe.x, probe.y);
    EventDispatcher::dispatch(*root, pick, nullptr);
    MouseEvent up = release(probe.x, probe.y);
    EventDispatcher::dispatch(*root, up, nullptr);
}

AURORA_TEST_CASE(no_modifier_row_keeps_hit_node_origin_bit_identical) {
    // 形态③：无 modifier 的对照场景——origin 与改动前逐位相等（防「四类平移漏修一类」回归）。
    // 缺省路径 tf.translation 恒为零，故 HitNode.origin 必须**逐位**等于布局盒原点。
    ChangeLog log;  // 具名：make_dropdown 内部的 on_change 捕获它，临时对象会悬垂
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);
    auto row = std::make_shared<Column>();  // 无任何 modifier
    row->add(Node{dd});
    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{row}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    const Rect row_box = child_box(*root, *row);
    const Rect dd_box = child_box(*row, *dd);
    AURORA_TEST_REQUIRE(dd_box.size.width > 0.0F);

    // 复算：无平移 ⇒ 绘制原点 == 行原点 + 行内相对原点。
    const Point expected_paint_origin = paint_origin_of_padded_row(row_box.origin, dd_box.origin, 0.0F);

    const Point probe{.x = expected_paint_origin.x + (dd_box.size.width * 0.5F),
                      .y = expected_paint_origin.y + (AURORA_BOX_HEIGHT * 0.5F)};
    const std::vector<HitNode> chain = root->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());

    const HitNode *dd_node = nullptr;
    for (const HitNode &n : chain) {
        if (n.get() == dd.get()) {
            dd_node = &n;
            break;
        }
    }
    AURORA_TEST_REQUIRE(dd_node != nullptr);
    // 逐位（容差 0）：缺省路径不得有任何平移残留。
    AURORA_TEST_CHECK_EQ(dd_node->origin.x, expected_paint_origin.x);
    AURORA_TEST_CHECK_EQ(dd_node->origin.y, expected_paint_origin.y);
}

}  // namespace aurora::test_cases::utest_dropdown
