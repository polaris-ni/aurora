/// 测试类型: unit
/// 目标单元: include/aurora/widget/reorderable_list.h
/// 测试说明: 覆盖可拖拽重排列表的数据与几何内核——数据改写正确性（前移/后移/边界/单项/空表）、
///           可变行高 y 累计表、内建滚动的夹取与滚轮语义、restore_key 恢复与写回、
///           可见范围与命中索引换算，以及自绘滚动控件的缓存声明语义
/// 覆盖说明: 拖拽识别 / 让位 / 落位动画 / auto-scroll 见 C2 / C3 用例（同文件续写）

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/reorderable_list.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_reorderable_list {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 无障碍设置的用例级守卫：`reduce_motion` 是进程级全局，用例结束必须还原。
class ScopedAccessibility {
  public:
    explicit ScopedAccessibility(AccessibilitySettings s) : saved_(current_accessibility_settings()) {
        set_accessibility_settings(s);
    }
    ~ScopedAccessibility() { set_accessibility_settings(saved_); }
    ScopedAccessibility(const ScopedAccessibility &) = delete;
    auto operator=(const ScopedAccessibility &) -> ScopedAccessibility & = delete;
    ScopedAccessibility(ScopedAccessibility &&) = delete;
    auto operator=(ScopedAccessibility &&) -> ScopedAccessibility & = delete;

  private:
    AccessibilitySettings saved_;
};

/// 以 16ms 步进驱动 tick 直到落位动画结束（上限 200 帧 ≈ 3.2s 虚拟时间）。
/// @return 结束时刻（供后续 tick 继续使用同一条虚拟时间轴）
auto drive_until_settled(ReorderableList<int> &list, std::chrono::steady_clock::time_point now, int max_ticks = 200)
    -> std::chrono::steady_clock::time_point {
    for (int i = 0; i < max_ticks; ++i) {
        if (!list.is_settling()) {
            break;
        }
        now += std::chrono::milliseconds(16);
        list.tick(now);
    }
    return now;
}

/// 指针事件（走真实派发器：全局坐标 → 命中链 → 本地坐标本地化）。
auto mouse(MouseAction action, float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = action;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = y};
    return e;
}

/// 命中链中是否包含指定控件（链的层次顺序不作为断言目标）。
auto chain_hits(const std::vector<HitNode> &chain, const Widget *w) -> bool {
    return std::ranges::any_of(chain, [w](const HitNode &n) { return n.ptr == w; });
}

/// 便捷派发：`dispatch` 取非 const 引用，故在函数内建栈上事件（临时值无法绑定）。
template <typename T>
auto send(T &root, MouseAction action, float x, float y) -> bool {
    MouseEvent e = mouse(action, x, y);
    return EventDispatcher::dispatch(root, e, nullptr);
}

/// 固定高度哑控件：让行高可精确预期（不依赖文本测量）。
/// 覆写 `on_hit_test` 使其成为可命中的叶控件（默认叶控件返回 nullptr，命中链会跳过它）。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_hit_test(const Point & /*local*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/)
        -> Widget * override {
        return this;
    }

  private:
    float w_;
    float h_;
};

/// 可点击条目：入命中链（`wants_click`）且消费指针事件（列表因此在条目正文上收不到 Press）。
class ClickBox final : public Widget {
  public:
    ClickBox(float h) : h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ClickBox"; }
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 100.0F, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_hit_test(const Point & /*local*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/)
        -> Widget * override {
        return this;
    }

  private:
    float h_;
};

/// 行高表驱动的条目构造器：第 i 项高度 = heights[i]（越界用最后一项）。
/// @param clickable 条目是否自带点击（消费指针事件）——真实列表的常见形态。
auto make_builder(const std::vector<float> &heights, bool clickable = false) -> ReorderableList<int>::ItemBuilder {
    return [heights, clickable](const int &, int index) -> Node {
        const std::size_t i = std::min(static_cast<std::size_t>(index), heights.size() - 1);
        if (clickable) {
            return Node{std::make_shared<ClickBox>(heights[i])};
        }
        return Node{std::make_shared<FixedBox>(100.0F, heights[i])};
    };
}

auto make_items(std::vector<int> values) -> std::shared_ptr<State<std::vector<int>>> {
    return std::make_shared<State<std::vector<int>>>(std::move(values));
}

/// 用例隔离：注册表是进程级单例。
auto reset_storage() -> ScrollStorage & {
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    return storage;
}

}  // namespace

AURORA_TEST_CASE(reorder_moves_item_with_rotate_semantics) {
    auto items = make_items({0, 1, 2, 3});
    ReorderableList<int> list{items, make_builder({20.0F})};
    std::vector<std::pair<int, int>> calls;
    list.set_on_reorder([&calls](int from, int to) -> void { calls.emplace_back(from, to); });

    // 后移：把 0 移到末位（to = 最终下标 3）。
    AURORA_TEST_CHECK_TRUE(list.reorder(0, 3));
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{1, 2, 3, 0}));
    AURORA_TEST_REQUIRE_EQ(calls.size(), 1U);
    AURORA_TEST_CHECK_EQ(calls[0].first, 0);
    AURORA_TEST_CHECK_EQ(calls[0].second, 3);  // 报告落位后的下标

    // 前移：把末位的 0 移回开头。
    AURORA_TEST_CHECK_TRUE(list.reorder(3, 0));
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{0, 1, 2, 3}));
    AURORA_TEST_REQUIRE_EQ(calls.size(), 2U);
    AURORA_TEST_CHECK_EQ(calls[1].second, 0);

    // 相邻交换：把 1 移到下标 2（等价与 2 互换）。
    AURORA_TEST_CHECK_TRUE(list.reorder(1, 2));
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{0, 2, 1, 3}));

    // 无变化语义：to == from 不改写数据、不回调。
    AURORA_TEST_CHECK_FALSE(list.reorder(2, 2));
    AURORA_TEST_CHECK_EQ(calls.size(), 3U);

    // 边界：越界 index / 目标下标越界夹取 / 单项 / 空表。
    AURORA_TEST_CHECK_FALSE(list.reorder(-1, 0));
    AURORA_TEST_CHECK_FALSE(list.reorder(9, 0));
    AURORA_TEST_CHECK_TRUE(list.reorder(0, 99));  // 目标下标夹取到 count-1
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{2, 1, 3, 0}));

    auto one = make_items({7});
    ReorderableList<int> single{one, make_builder({20.0F})};
    AURORA_TEST_CHECK_FALSE(single.reorder(0, 1));  // 单项无可换位
    AURORA_TEST_CHECK_FALSE(single.reorder(0, 0));

    ReorderableList<int> empty{make_items({}), make_builder({20.0F})};
    AURORA_TEST_CHECK_FALSE(empty.reorder(0, 0));

    ReorderableList<int> no_source;  // 未注入数据源：空操作而非崩溃
    AURORA_TEST_CHECK_FALSE(no_source.reorder(0, 1));
}

AURORA_TEST_CASE(layout_builds_y_table_for_variable_heights) {
    auto items = make_items({10, 20, 30});
    ReorderableList<int> list{items, make_builder({40.0F, 60.0F, 50.0F}), 10.0F};
    AURORA_TEST_CHECK_EQ(list.item_count(), 3U);

    LayoutEngine::layout(list, bounded(120.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(list.size().height, 100.0F, 1e-4F);  // 撑满父约束
    AURORA_TEST_CHECK_EQ(list.item_count(), 3U);

    // y 表：40 / +10 / 60 / +10 / 50 → tops = {0, 50, 120}，内容高 170。
    AURORA_TEST_CHECK_NEAR(list.item_top(0), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.item_top(1), 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.item_top(2), 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.item_height(1), 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.content_height(), 170.0F, 1e-4F);

    // 子节点 bounds 记的是内容坐标（滚动偏移在绘制/命中期叠加）。
    AURORA_TEST_CHECK_NEAR(list.child_nodes()[2].bounds().origin.y, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.child_nodes()[1].bounds().size.height, 60.0F, 1e-4F);

    // 内容坐标 → 索引换算（命中 / 可见范围共用）。
    AURORA_TEST_CHECK_EQ(list.index_at_content_y(0.0F), 0);
    AURORA_TEST_CHECK_EQ(list.index_at_content_y(39.9F), 0);
    AURORA_TEST_CHECK_EQ(list.index_at_content_y(50.0F), 1);
    AURORA_TEST_CHECK_EQ(list.index_at_content_y(119.0F), 1);
    AURORA_TEST_CHECK_EQ(list.index_at_content_y(120.0F), 2);
    AURORA_TEST_CHECK_EQ(list.index_at_content_y(1.0e6F), 2);  // 越界取末项

    // 数据长度变化触发重建（Repeater 先例）。
    items->set({10, 20, 30, 40});
    LayoutEngine::layout(list, bounded(120.0F, 100.0F));
    AURORA_TEST_CHECK_EQ(list.item_count(), 4U);
    AURORA_TEST_CHECK_NEAR(list.item_top(3), 180.0F, 1e-4F);  // 40+10+60+10+50+10
    AURORA_TEST_CHECK_NEAR(list.content_height(), 230.0F, 1e-4F);
}

AURORA_TEST_CASE(scroll_clamps_wheel_and_programmatic_offsets) {
    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F, 40.0F, 40.0F}), 0.0F};
    LayoutEngine::layout(list, bounded(100.0F, 100.0F));  // 内容 120 → max offset 20

    AURORA_TEST_CHECK_NEAR(list.max_scroll_offset(), 20.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(list.wants_scroll());
    AURORA_TEST_CHECK_TRUE(list.can_cache_layout() == false);
    AURORA_TEST_CHECK_TRUE(list.can_cache_display_list() == false);

    // 滚轮：delta_y < 0 = 向下滚，offset 增大（共享 ScrollViewport 符号约定）。
    ScrollEvent wheel;
    wheel.delta_y = -1.0F;
    list.on_scroll(wheel);
    AURORA_TEST_CHECK_TRUE(wheel.is_handled);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 20.0F, 1e-4F);  // 40 步长被夹到 20

    // 到底后再滚：偏移不变（幂等），事件仍被消费。
    ScrollEvent again;
    again.delta_y = -1.0F;
    list.on_scroll(again);
    AURORA_TEST_CHECK_TRUE(again.is_handled);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 20.0F, 1e-4F);

    // 向上滚 = offset 减小。
    ScrollEvent up;
    up.delta_y = 1.0F;
    list.on_scroll(up);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);

    // 程序化设置：夹取到 [0, max]，返回是否变化。
    AURORA_TEST_CHECK_TRUE(list.set_scroll_offset(1.0e6F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 20.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.set_scroll_offset(20.0F));  // 无变化
    AURORA_TEST_CHECK_TRUE(list.set_scroll_offset(-5.0F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);

    // 可见范围随偏移移动（行高 40，视口 100）。
    AURORA_TEST_CHECK_EQ(list.visible_range(), (std::pair<int, int>{0, 3}));
    list.set_scroll_offset(20.0F);
    AURORA_TEST_CHECK_EQ(list.visible_range(), (std::pair<int, int>{0, 3}));
}

AURORA_TEST_CASE(restore_key_restores_and_writes_back_offset) {
    auto &storage = reset_storage();
    storage.write("list.key", 20.0F);

    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F, 40.0F, 40.0F})};
    list.set_restore_key("list.key");
    LayoutEngine::layout(list, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 20.0F, 1e-4F);  // 首次可滚动布局恢复

    // 用户滚动即写回（仅内存）。
    list.set_scroll_offset(0.0F);
    AURORA_TEST_CHECK_NEAR(storage.read("list.key").value_or(-1.0F), 0.0F, 1e-4F);
    // 恢复只生效一次：再次布局不会把位置拉回 20。
    LayoutEngine::layout(list, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);

    // 无键实例不参与恢复、不污染注册表。
    auto items2 = make_items({0, 1, 2});
    ReorderableList<int> plain{items2, make_builder({40.0F, 40.0F, 40.0F})};
    LayoutEngine::layout(plain, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(plain.scroll_offset(), 0.0F, 1e-4F);

    reset_storage();
}

AURORA_TEST_CASE(descriptor_and_serialization_surface) {
    const WidgetDescriptor d = ReorderableList<int>::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, std::string{"ReorderableList"});
    AURORA_TEST_CHECK_EQ(d.children_policy, std::string{"multiple"});
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(d.events[0], std::string{"on_reorder"});

    auto has_prop = [&d](const std::string &key) -> bool {
        return std::ranges::any_of(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
    };
    AURORA_TEST_CHECK_TRUE(has_prop("gap"));
    AURORA_TEST_CHECK_TRUE(has_prop("scroll_offset"));
    AURORA_TEST_CHECK_TRUE(has_prop("restore_key"));
    AURORA_TEST_CHECK_TRUE(has_prop("drag_handle"));
    AURORA_TEST_CHECK_TRUE(has_prop("auto_scroll_threshold"));

    ReorderableList<int> list{make_items({0, 1}), make_builder({20.0F})};
    list.set_restore_key("k");
    Json props = Json::object();
    list.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["restore_key"].get<std::string>(), std::string{"k"});
    AURORA_TEST_CHECK_NEAR(props["gap"].get<float>(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(props.contains("note"));  // 运行时数据不序列化，只留 note
    AURORA_TEST_CHECK_EQ(std::string{list.type_name()}, std::string{"ReorderableList"});
}

AURORA_TEST_CASE(default_construct_and_empty_data_are_safe) {
    // 默认构造（无数据源 / 无构造器）+ 无界约束退化尺寸：不崩、空表语义明确。
    ReorderableList<int> empty;
    const Constraints unbounded{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size::infinity()};
    LayoutEngine::layout(empty, unbounded);
    AURORA_TEST_CHECK_GT(empty.size().width, 0.0F);
    AURORA_TEST_CHECK_GT(empty.size().height, 0.0F);
    AURORA_TEST_CHECK_EQ(empty.item_count(), 0U);
    AURORA_TEST_CHECK_NEAR(empty.content_height(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(empty.max_scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(empty.visible_range(), (std::pair<int, int>{0, 0}));
    AURORA_TEST_CHECK_EQ(empty.index_at_content_y(0.0F), -1);
    AURORA_TEST_CHECK_TRUE(empty.data().empty());

    // 空数据源：可布局、不可滚动、不可重排。
    ReorderableList<int> no_items{make_items({}), make_builder({20.0F})};
    LayoutEngine::layout(no_items, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_EQ(no_items.item_count(), 0U);
    AURORA_TEST_CHECK_NEAR(no_items.max_scroll_offset(), 0.0F, 1e-4F);

    // 负 gap 降级为 0（Diagnostics::degraded，不抛）。
    ReorderableList<int> neg_gap{make_items({0}), make_builder({20.0F}), -8.0F};
    AURORA_TEST_CHECK_NEAR(neg_gap.gap(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(slot_geometry_uses_midpoints_with_hysteresis) {
    // 纯逻辑：等高三项（各 40、无间距）→ 压缩序中点 = {20, 60}，插入位 0..2。
    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F})};
    list.set_drag_slop(0.0);
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));

    // 拖第 0 项：中心从中点下沿往上越过才换位。
    AURORA_TEST_CHECK_EQ(list.slot_for_center(0, 0.0F), 0);
    AURORA_TEST_CHECK_EQ(list.slot_for_center(0, 20.0F), 0);   // 恰在中点：不动
    AURORA_TEST_CHECK_EQ(list.slot_for_center(0, 22.0F), 0);   // 滞回带内（+2dp）：仍不动
    AURORA_TEST_CHECK_EQ(list.slot_for_center(0, 23.0F), 1);   // 越过滞回带 → 进一位
    AURORA_TEST_CHECK_EQ(list.slot_for_center(0, 62.0F), 1);
    AURORA_TEST_CHECK_EQ(list.slot_for_center(0, 63.0F), 2);   // 越过第 2 项中点 → 末位

    // 反向：从末位往回拖同样要求越过滞回带（回退对称：60−2=58 为界）。
    AURORA_TEST_CHECK_EQ(list.slot_for_center(2, 59.0F, 2), 2);
    AURORA_TEST_CHECK_EQ(list.slot_for_center(2, 58.0F, 2), 2);
    AURORA_TEST_CHECK_EQ(list.slot_for_center(2, 57.0F, 2), 1);

    // 边缘夹取：极端值不越界。
    AURORA_TEST_CHECK_EQ(list.slot_for_center(1, -1.0e6F), 0);
    AURORA_TEST_CHECK_EQ(list.slot_for_center(1, 1.0e6F), 2);

    // 目标顶端与插入位一致（落位动画 / auto-scroll 共用同一几何）。
    AURORA_TEST_CHECK_NEAR(list.slot_for_center(0, 63.0F), 2, 0.0F);
    AURORA_TEST_CHECK_NEAR(static_cast<float>(list.slot_for_center(0, 0.0F)), 0.0F, 0.0F);
}

AURORA_TEST_CASE(drag_reorders_item_and_shields_child_pointer_events) {
    // 条目自带点击（常见形态）：手柄带由列表自留，故起拖按键在手柄带内（x >= 200-48）。
    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F, 40.0F, 40.0F}, /*clickable=*/true)};
    list.set_drag_slop(4.0);
    list.set_drag_handle(true);
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));
    const Rect bounds{.origin = Point{}, .size = Size{.width = 200.0F, .height = 200.0F}};
    constexpr BuildContext ctx;

    // 探针：记录每次成功的数据改写（(old,new)）。
    std::vector<std::pair<int, int>> reorder_calls;
    list.set_on_reorder([&reorder_calls](int from, int to) -> void { reorder_calls.emplace_back(from, to); });

    // 未超 slop：不起拖，事件照常冒泡给子项。
    MouseEvent press = mouse(MouseAction::Press, 180.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, press, nullptr));
    AURORA_TEST_CHECK_FALSE(list.is_dragging());
    AURORA_TEST_CHECK_FALSE(press.is_handled);

    // 超过 slop：锁定被拖项（第 0 项），跟手 1:1。
    MouseEvent move = mouse(MouseAction::Move, 180.0F, 60.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, move, nullptr));
    AURORA_TEST_CHECK_TRUE(list.is_dragging());
    AURORA_TEST_CHECK_EQ(list.drag_index(), 0);
    AURORA_TEST_CHECK_TRUE(move.is_handled);  // 拖拽中消费事件（父级不响应点击）
    AURORA_TEST_CHECK_NEAR(list.drag_follow(), 50.0F, 1e-4F);
    // 中心 = 0 + 20 + 50 = 70 → 越过第 2 项中点 62 → 插入位 2（末位）。
    AURORA_TEST_CHECK_EQ(list.drop_slot(), 2);

    // 让位位移「绘制期生效、命中同步」：第 1 项被上移到 0..40，故 10 处应命中它
    // （bounds 未改动——几何权威在 Node，逐帧改会击穿子控件 DL 缓存）。
    const std::vector<HitNode> chain = list.hit_test_chain(Point{.x = 100.0F, .y = 10.0F}, bounds, ctx);
    AURORA_TEST_CHECK_TRUE(chain_hits(chain, &list.child_nodes()[1].widget()));
    AURORA_TEST_CHECK_FALSE(chain_hits(chain, &list.child_nodes()[0].widget()));  // 原位置的项已让位
    AURORA_TEST_CHECK_NEAR(list.child_nodes()[1].bounds().origin.y, 40.0F, 1e-4F);  // 布局盒未动

    // 被拖项当前绘制区（50..90）不给子项命中：拖拽中子项指针事件被屏蔽（裁决 18），
    // 命中链只剩本控件自身（拖拽事件才能回到列表继续跟手）。
    const std::vector<HitNode> dragged_chain = list.hit_test_chain(Point{.x = 100.0F, .y = 70.0F}, bounds, ctx);
    AURORA_TEST_CHECK_FALSE(chain_hits(dragged_chain, &list.child_nodes()[0].widget()));

    // 松手前数据未被改动（只改 UI 表现）。
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{0, 1, 2}));

    // 松手：进入 spring 落位动画（此时数据仍未提交），动画静止后一次性改写数据。
    MouseEvent release = mouse(MouseAction::Release, 180.0F, 110.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, release, nullptr));
    AURORA_TEST_CHECK_FALSE(list.is_dragging());
    AURORA_TEST_CHECK_TRUE(list.is_settling());
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{0, 1, 2}));

    (void)drive_until_settled(list, std::chrono::steady_clock::now());
    AURORA_TEST_CHECK_FALSE(list.is_settling());
    AURORA_TEST_CHECK_EQ(list.drag_index(), -1);
    AURORA_TEST_CHECK_EQ(reorder_calls.size(), std::size_t{1});
    AURORA_TEST_CHECK_MSG(reorder_calls.empty() || reorder_calls[0].second == 2,
                          "reported new index = 2 (last slot)");
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{1, 2, 0}));
}

AURORA_TEST_CASE(wheel_is_swallowed_while_dragging) {
    // 6 项 × 40 = 内容 240 > 视口 200：本可滚动 40。
    auto items = make_items({0, 1, 2, 3, 4, 5});
    ReorderableList<int> list{items, make_builder({40.0F})};
    list.set_drag_slop(4.0);
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(list.max_scroll_offset(), 40.0F, 1e-4F);

    // 非拖拽：滚轮生效。
    ScrollEvent idle_wheel;
    idle_wheel.delta_y = -1.0F;
    list.on_scroll(idle_wheel);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 40.0F, 1e-4F);
    list.set_scroll_offset(0.0F);

    MouseEvent press = mouse(MouseAction::Press, 100.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, press, nullptr));
    MouseEvent move = mouse(MouseAction::Move, 100.0F, 40.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, move, nullptr));
    AURORA_TEST_REQUIRE_TRUE(list.is_dragging());

    // 拖拽中：滚轮被吞且偏移不变（内建滚动只由 auto-scroll 驱动，避免叠加）。
    ScrollEvent wheel;
    wheel.delta_y = -1.0F;
    list.on_scroll(wheel);
    AURORA_TEST_CHECK_TRUE(wheel.is_handled);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);

    list.set_scroll_offset(0.0F);
}

AURORA_TEST_CASE(plain_items_support_whole_item_drag) {
    // 条目不吃指针事件（纯展示型 Row/Text）：整项按下即起拖，无需手柄。
    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F, 40.0F, 40.0F})};
    list.set_drag_slop(4.0);
    AURORA_TEST_CHECK_FALSE(list.drag_handle());
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));

    MouseEvent press = mouse(MouseAction::Press, 100.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, press, nullptr));
    MouseEvent move = mouse(MouseAction::Move, 100.0F, 60.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, move, nullptr));
    AURORA_TEST_CHECK_TRUE(list.is_dragging());
    AURORA_TEST_CHECK_EQ(list.drop_slot(), 2);

    MouseEvent release = mouse(MouseAction::Release, 100.0F, 110.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, release, nullptr));
    AURORA_TEST_CHECK_TRUE(list.is_settling());
    (void)drive_until_settled(list, std::chrono::steady_clock::now());
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{1, 2, 0}));
    AURORA_TEST_CHECK_FALSE(list.is_settling());
}

AURORA_TEST_CASE(clickable_item_body_does_not_start_drag_but_handle_band_does) {
    // 条目自带点击时，其 Press 被条目消费（`is_handled=true`）⇒ 事件冒不到列表 ⇒
    // 条目正文无法起拖；手柄带由列表自留（命中链不下降给条目）⇒ `set_drag_handle(true)`
    // 在「条目可点击」的常见形态下依然可用。这是文档化的交互边界。
    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F}, /*clickable=*/true)};
    list.set_drag_slop(4.0);
    list.set_drag_handle(true);
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));

    // 正文（非手柄带）：Press 被条目消费 → 不起拖，数据不变。
    MouseEvent press_body = mouse(MouseAction::Press, 60.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, press_body, nullptr));
    AURORA_TEST_CHECK_TRUE(press_body.is_handled);
    MouseEvent move_body = mouse(MouseAction::Move, 60.0F, 60.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, move_body, nullptr));
    AURORA_TEST_CHECK_FALSE(list.is_dragging());
    MouseEvent release_body = mouse(MouseAction::Release, 60.0F, 60.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, release_body, nullptr));
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{0, 1, 2}));

    // 手柄带：列表自己收到 Press/Move → 起拖并落位（下拖 40dp 越过相邻中点 → 下标 1）。
    MouseEvent press_handle = mouse(MouseAction::Press, 180.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, press_handle, nullptr));
    AURORA_TEST_CHECK_FALSE(press_handle.is_handled);
    MouseEvent move_handle = mouse(MouseAction::Move, 180.0F, 50.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, move_handle, nullptr));
    AURORA_TEST_CHECK_TRUE(list.is_dragging());
    AURORA_TEST_CHECK_EQ(list.drag_index(), 0);
    AURORA_TEST_CHECK_EQ(list.drop_slot(), 1);
    MouseEvent release_handle = mouse(MouseAction::Release, 180.0F, 50.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, release_handle, nullptr));
    (void)drive_until_settled(list, std::chrono::steady_clock::now());
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{1, 0, 2}));
    AURORA_TEST_CHECK_FALSE(list.is_dragging());
}

AURORA_TEST_CASE(settle_animation_approaches_slot_without_jump) {
    // 落位动画：位移单调逼近目标槽位，终点等于提交后的自然位置（无跳变）。
    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F})};
    list.set_drag_slop(4.0);
    list.set_drag_handle(true);
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));

    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Press, 180.0F, 10.0F));
    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Move, 180.0F, 60.0F));
    AURORA_TEST_REQUIRE_TRUE(list.is_dragging());
    AURORA_TEST_CHECK_EQ(list.drop_slot(), 2);
    const float follow_at_release = list.drag_follow();
    AURORA_TEST_CHECK_NEAR(follow_at_release, 50.0F, 1e-4F);

    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Release, 180.0F, 60.0F));
    AURORA_TEST_REQUIRE_TRUE(list.is_settling());

    // 逐帧推进：位移单调不减、且不超过目标（80 = 末位顶端）。
    auto now = std::chrono::steady_clock::now();
    float prev = follow_at_release;
    int frames = 0;
    while (list.is_settling() && frames < 200) {
        now += std::chrono::milliseconds(16);
        list.tick(now);
        ++frames;
        if (list.is_settling()) {
            const float cur = list.drag_follow();
            AURORA_TEST_CHECK_GE(cur, prev - 1e-3F);
            AURORA_TEST_CHECK_LE(cur, 80.0F + 1e-3F);
            prev = cur;
        }
    }
    AURORA_TEST_CHECK_FALSE(list.is_settling());
    AURORA_TEST_CHECK_GT(frames, 0);
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{1, 2, 0}));
    AURORA_TEST_CHECK_NEAR(list.drag_follow(), 0.0F, 1e-4F);
    // 提交只标布局脏（与真实帧循环一致：tick 在 layout 之前，同帧即落定）。
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));
    // 被拖项（数据值 0）现在落在末位，其顶端 = 动画终点 ⇒ 无缝衔接（无跳变）。
    AURORA_TEST_CHECK_EQ(items->get()[2], 0);
    AURORA_TEST_CHECK_NEAR(list.item_top(2), 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(prev, list.item_top(2), 0.5F);
}

AURORA_TEST_CASE(reduce_motion_commits_without_frame_animation) {
    // reduce_motion：跳过逐帧动画，松手即落位（同 DragToDismiss / AnimationController 的短路语义）。
    const ScopedAccessibility a11y{AccessibilitySettings{.reduce_motion = true}};
    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F})};
    list.set_drag_slop(4.0);
    list.set_drag_handle(true);
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));

    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Press, 180.0F, 10.0F));
    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Move, 180.0F, 60.0F));
    AURORA_TEST_REQUIRE_TRUE(list.is_dragging());
    AURORA_TEST_CHECK_EQ(list.drop_slot(), 2);

    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Release, 180.0F, 60.0F));
    AURORA_TEST_CHECK_FALSE(list.is_settling());               // 无动画阶段
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{1, 2, 0}));  // 已提交
    AURORA_TEST_CHECK_NEAR(list.drag_follow(), 0.0F, 1e-4F);

    // 落位动画中途开启 reduce_motion：下一帧直接落定（不等 spring 收敛）。
    auto items2 = make_items({0, 1, 2});
    ReorderableList<int> list2{items2, make_builder({40.0F})};
    list2.set_drag_slop(4.0);
    list2.set_drag_handle(true);
    LayoutEngine::layout(list2, bounded(200.0F, 200.0F));
    {
        const ScopedAccessibility off{AccessibilitySettings{}};
        AURORA_TEST_CHECK_TRUE(send(list2, MouseAction::Press, 180.0F, 10.0F));
        AURORA_TEST_CHECK_TRUE(send(list2, MouseAction::Move, 180.0F, 60.0F));
        AURORA_TEST_CHECK_TRUE(send(list2, MouseAction::Release, 180.0F, 60.0F));
        AURORA_TEST_REQUIRE_TRUE(list2.is_settling());
    }
    list2.tick(std::chrono::steady_clock::now() + std::chrono::milliseconds(16));
    AURORA_TEST_CHECK_FALSE(list2.is_settling());
    AURORA_TEST_CHECK_EQ(items2->get(), (std::vector<int>{1, 2, 0}));
}

AURORA_TEST_CASE(auto_scroll_follows_dragged_item_near_viewport_edge) {
    // 6 × 40 = 240 内容 / 200 视口 → 可滚 40；近下边缘拖动时按比例持续滚动，
    // 且「被拖项屏幕位置守恒」（滚动量吃进跟手位移，不叠加）。
    auto items = make_items({0, 1, 2, 3, 4, 5});
    ReorderableList<int> list{items, make_builder({40.0F})};
    list.set_drag_slop(4.0);
    list.set_auto_scroll_threshold(48.0F);
    AURORA_TEST_CHECK_NEAR(list.auto_scroll_threshold(), 48.0F, 1e-4F);
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));
    AURORA_TEST_REQUIRE_NEAR(list.max_scroll_offset(), 40.0F, 1e-4F);

    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Press, 100.0F, 10.0F));
    // 拖到视口底部之外（跟手位移被夹在内容范围内）：进入下边缘带。
    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Move, 100.0F, 190.0F));
    AURORA_TEST_REQUIRE_TRUE(list.is_dragging());
    const float follow_before = list.drag_follow();
    const float local_before = list.item_top(0) - list.scroll_offset() + follow_before;

    auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        now += std::chrono::milliseconds(16);
        list.tick(now);
        AURORA_TEST_CHECK_TRUE(list.is_dragging());  // 自动滚动期间拖拽状态保持
    }
    AURORA_TEST_CHECK_GT(list.scroll_offset(), 0.0F);  // 已向下自动滚动
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 40.0F, 1e-4F);
    // 屏幕位置守恒：内容坐标下的跟手位移补偿了滚动量。
    AURORA_TEST_CHECK_NEAR(list.item_top(0) - list.scroll_offset() + list.drag_follow(), local_before, 1e-3F);
    AURORA_TEST_CHECK_GT(list.drag_follow(), follow_before);

    // 手指移回顶部内侧：反向滚动回 0 后停下。
    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Move, 100.0F, 20.0F));
    for (int i = 0; i < 20; ++i) {
        now += std::chrono::milliseconds(16);
        list.tick(now);
    }
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);

    // 停止拖拽（松手落位）后不再自动滚动。
    AURORA_TEST_CHECK_TRUE(send(list, MouseAction::Release, 100.0F, 20.0F));
    (void)drive_until_settled(list, now);
    const float offset_after = list.scroll_offset();
    list.tick(now + std::chrono::milliseconds(16));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), offset_after, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.is_dragging());
}

AURORA_TEST_CASE(horizontal_drag_does_not_start_reorder) {
    // 本控件只做垂直重排：横向拖过 slop 不接管（轴锁定后不再改轴）。
    auto items = make_items({0, 1, 2});
    ReorderableList<int> list{items, make_builder({40.0F})};
    list.set_drag_slop(4.0);
    LayoutEngine::layout(list, bounded(200.0F, 200.0F));

    MouseEvent press = mouse(MouseAction::Press, 40.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, press, nullptr));
    MouseEvent move = mouse(MouseAction::Move, 180.0F, 12.0F);  // |dx| > |dy|
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, move, nullptr));
    AURORA_TEST_CHECK_FALSE(list.is_dragging());
    AURORA_TEST_CHECK_EQ(list.drag_index(), -1);

    MouseEvent release = mouse(MouseAction::Release, 180.0F, 12.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(list, release, nullptr));
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{0, 1, 2}));
}

}  // namespace aurora::test_cases::utest_reorderable_list
