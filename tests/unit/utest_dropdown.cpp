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
constexpr float AURORA_HOST_WIDTH = 320.0F; ///< 宿主视口宽（dp）
constexpr float AURORA_HOST_HEIGHT = 260.0F; ///< 宿主视口高（dp）
constexpr float AURORA_BOX_HEIGHT = 30.0F; ///< 主框高（dp；与 set_box_height 对齐）
constexpr float AURORA_ITEM_HEIGHT = 26.0F; ///< 选项行高（dp；与 set_item_height 对齐）
constexpr float AURORA_SIBLING_GAP = 60.0F; ///< 下方兄弟按钮与主框的间距（让按钮盒越过面板底边）

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
[[maybe_unused]] void paint_once(Widget &w, const Rect &viewport, const BuildContext &ctx) {
    // **必须** begin()：`AURORA_ENABLE_OCCLUSION_CULLING` 下 `Container::on_paint` 用
    // `p.clip_bounds()` 裁剪子树，未 begin 的 Painter 其裁剪区为空 ⇒ 整棵子树被剔除、
    // 子控件的 `focus_bounds_` 永不写入（本轮实测：直接 paint 某控件能读到绘制盒，
    // 经未 begin 的 root 遍历则读到零盒）。
    Painter p;
    p.begin(static_cast<int>(viewport.size.width), static_cast<int>(viewport.size.height));
    w.paint(p, viewport, ctx);
}
} // namespace

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
    MouseEvent up = release(probe.x, probe.y); // 配对 Release：解除派发器单例的指针捕获
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
    AURORA_TEST_REQUIRE(probe.y > panel_bottom); // 前提：该点确实在面板之外（否则判据空转）
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
    // 核心证人：三层嵌套 LazyList → Row → Dropdown，孙辈（Dropdown）申报的面板区必须可达。
    //
    // 几何（本机无头 scale 恒 1.0）：行盒 56、上下内边距各 8 ⇒ Dropdown 紧约束盒高 40；
    // 面板自下拉局部 y = box_height_(30) 起。探点取下拉局部 y = 60 ⇒ 行局部 60+8-8 = 60，
    // 越过行下沿（56）——中间的 Row 不申报任何追加盒，修复前门在 Row 那一层判假。
    ChangeLog log;
    const std::shared_ptr<Dropdown> dd = make_dropdown(log);

    // 每行 = Row 内放 Dropdown（复现 borealis 设置面板的「LazyList → Row → Dropdown」形态）。
    // 上下内边距各 8 ⇒ 行高 56 时 Dropdown 的紧约束盒高 40，与真实场景一致。
    auto make_row = [](const std::shared_ptr<Dropdown> &target) -> Node {
        const auto row = std::make_shared<Row>(RowProps{.children = {Node{target}}});
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
    AURORA_TEST_REQUIRE(probe.y > row_box.bottom()); // 前提：该点确实越出行盒，否则判据空转

    const std::vector<HitNode> chain = list->hit_test_chain(probe, host_box(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(deepest(chain), static_cast<const Widget *>(dd.get()));

    // 真实派发：该点必须选中第 2 项。
    MouseEvent pick = press(probe.x, probe.y);
    EventDispatcher::dispatch(*list, pick, nullptr);
    AURORA_TEST_CHECK_EQ(log.count, 1);
    AURORA_TEST_CHECK_EQ(log.last, 2);
    MouseEvent up = release(probe.x, probe.y); // 配对 Release：解除派发器单例的指针捕获
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
    const float row_top = (19.0F * 56.0F) - offset; // 末行盒顶边（列表局部 y）
    const Rect db = child_box(*row, *dd);
    AURORA_TEST_REQUIRE(db.size.height > 0.0F);

    // 面板第 0 行中心（行局部 y ≈ 38）换算到列表局部。
    const Point row_local = item_center(*dd, 0);
    const Point probe_first{.x = db.origin.x + row_local.x, .y = (row_top + db.origin.y) + row_local.y};
    // 末行盒 [204,260) 整段在视口内，但面板共 78 高：首行中心（247）尚在视口内、末行中心（299）
    // 已出视口。取末行中心才是有判别力的裁剪场景——条目可见、面板尾部不可见。
    const Point probe{.x = db.origin.x + row_local.x,
                      .y = (row_top + db.origin.y) + (AURORA_BOX_HEIGHT + (2.5F * AURORA_ITEM_HEIGHT))};
    AURORA_TEST_REQUIRE_GT(offset, 800.0F); // 前提：确实滚到了底
    AURORA_TEST_REQUIRE_LT(probe_first.y, list->size().height); // 对照：面板首行仍可见（证明条目在视口内）
    AURORA_TEST_REQUIRE_GT(probe.y, list->size().height); // 前提：面板末行在视口外，否则判据空转

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

    // LazyList 路径：20 行 × 56dp，Dropdown 在末行；滚到底后末行顶边 = 19*56 - offset，
    // 面板自其下方 30dp 起 ⇒ 整段落在视口下沿（260）之外。
    {
        const std::shared_ptr<Dropdown> dd = make_dropdown(log);
        const std::shared_ptr<Row> row = std::make_shared<Row>(RowProps{.children = {Node{dd}}});
        row->modifier.set(Modifier{}.padding(8.0F));
        auto list =
            std::make_shared<LazyList>(20, [row](int i) -> Node { return i == 19 ? Node{row} : Node{}; }, 56.0F);
        LayoutEngine::layout(*list, host_constraints());
        dd->set_open(true);
        list->set_scroll_offset(9999.0F); // 夹到 max_scroll_offset
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
        AURORA_TEST_REQUIRE_GT(probe.y, list->size().height); // 前提：该点确实在视口外，否则判据空转
        AURORA_TEST_CHECK(deepest(list->hit_test_chain(probe, host_box(), BuildContext{})) !=
            static_cast<const Widget *>(dd.get()));
        AURORA_TEST_CHECK_FALSE(list->covers_extra_hit_box(probe, BuildContext{}));
    }

    // Scroll 路径：内容高 400+ 的下拉滚到视口下沿之外，换算与 Scroll::covers_descendant_extra_hit_box 同式。
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

namespace {
/// @brief 记录 `extra_hit_box` 收到的 `ancestor_offset` 的探针（观测追加命中盒通路的坐标系）。
///
/// `Scroll::covers_descendant_extra_hit_box` 把 `ancestor_offset + 内容盒原点` 传给内容子树；
/// 该值的坐标系（视口 vs 窗口）是本用例的直接观测量——它不依赖任何控件状态，只看传进来什么。
class OffsetProbe final : public Widget {
public:
    [[nodiscard]] auto type_name() const -> const char * override { return "OffsetProbe"; }

    [[nodiscard]] auto extra_hit_box(const aurora::BuildContext & /*ctx*/, const Point &ancestor_offset) const
        -> std::optional<Rect> override {
        received_ = ancestor_offset;
        return std::nullopt; // 不申报覆盖区：只观测入参，不干扰命中判定
    }

    [[nodiscard]] auto received() const -> Point { return received_; }

protected:
    auto on_layout(const aurora::Constraints &c, const aurora::BuildContext & /*ctx*/) -> aurora::Size override {
        return c.constrain(aurora::Size{.width = 40.0F, .height = 40.0F});
    }

    auto on_paint(aurora::Painter & /*p*/, const aurora::Rect & /*r*/, const aurora::BuildContext & /*ctx*/)
        -> void override {
    }

private:
    mutable Point received_{}; ///< mutable：`extra_hit_box` 是 const 钩子，本类只做记录
};
}

AURORA_TEST_CASE(scroll_passes_viewport_space_offset_to_descendant_extra_hit_box) {
    // `ancestor_offset` 保持**视口坐标系**（刻意不扣 `offset_y_`），与 `HitNode.origin`（窗口
    // 坐标）是有意不同的两个坐标系。本用例直接观测 Scroll 传给内容后代的那个值，钉住这一口径，
    // 防止未来把两者「统一」而悄悄改掉 Dropdown 在滚动容器内的翻转阈值。
    //
    // 判据构造：探针位于内容顶部。Scroll 即根 ⇒ 视口原点在视口系里恒为 0，故
    //   视口系语义 ⇒ 探针收到的 offset.y 恒为 0，**不随滚动变化**；
    //   窗口系语义（扣 offset_y_）⇒ 收到值随滚动变成 -offset_y_。
    // 断言「滚动前后读数相同」即可区分这两种坐标系。
    auto scroller = std::make_shared<Scroll>();
    auto probe = std::make_shared<OffsetProbe>();
    auto filler = std::make_shared<OffsetProbe>();
    // gap 600dp 撑出远超视口（260dp）的内容高，使滚动真正生效。**单子项时 gap 不产生间距**
    // （它是「相邻子项」间距），故必须两个子项。缺此垫高则 offset 恒 0、判据不可观测。
    auto content = std::make_shared<Column>(ColumnProps{.children = {Node{probe}, Node{filler}}, .gap = 600.0F});
    scroller->add(Node{content});
    LayoutEngine::layout(*scroller, host_constraints());

    constexpr BuildContext ctx;
    (void)scroller->covers_extra_hit_box(Point{.x = 1.0F, .y = 1.0F}, ctx, Point{});
    const Point before = probe->received();
    AURORA_TEST_CHECK_NEAR(before.y, 0.0F, 1e-3F);

    (void)scroller->set_offset(120.0F);
    AURORA_TEST_REQUIRE(scroller->offset_y() > 0.0F);
    LayoutEngine::layout(*scroller, host_constraints());
    (void)scroller->covers_extra_hit_box(Point{.x = 1.0F, .y = 1.0F}, ctx, Point{});
    const Point after = probe->received();

    // 滚动后仍为 0 ⇒ B 路保持视口坐标系。若改成窗口坐标，此处会读到 -offset_y_（≠ 0）而转红。
    AURORA_TEST_CHECK_NEAR(after.y, 0.0F, 1e-3F);
    AURORA_TEST_CHECK(before.y == after.y);
}
} // namespace aurora::test_cases::utest_dropdown