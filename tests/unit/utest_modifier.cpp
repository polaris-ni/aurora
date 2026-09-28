/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier.h
/// 测试说明: 覆盖 Modifier 工厂链——链式追加顺序、负 padding 降级、flex_weight 读取、
/// transform() 的 TransformInfo 汇总（平移/内容盒收缩/透明度累乘/矩阵组合）、
/// paint_boxes() 的「Paint 节点作用于内侧那一段盒子」分段规则、
/// 点击/拖拽/长按/Tooltip/上下文菜单的派发流水线与探测谓词

#include <chrono>
#include <functional>
#include <limits>
#include <ranges>
#include <vector>

#include "aurora/modifier/modifier.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_modifier {

namespace {

auto t_ms(int ms) -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{ms}};
}

auto size_of(float w, float h) -> Size { return Size{.width = w, .height = h}; }

/// @brief 按 `Widget::layout` 的同一方式逆序包裹走链（后压入者靠外），
///        让 Align 等盒收缩节点记录 child_size（paint_boxes 依赖该记录）。
auto run_chain(const Modifier &m, const Constraints &c, const Size &leaf) -> Size {
    std::function<Size(const Constraints &)> measure = [leaf](const Constraints &) -> Size { return leaf; };
    for (const auto &it : std::views::reverse(m.nodes())) {
        const ModifierNode *node = it.get();
        auto inner = std::move(measure);
        measure = [node, inner](const Constraints &cc) -> Size { return node->layout(cc, inner); };
    }
    return measure(c);
}

}  // namespace

AURORA_TEST_CASE(chain_appends_nodes_in_factory_order) {
    const Modifier m = Modifier{}.padding(8.0F).background(Color(0, 0, 0, 255)).clickable([]() -> void {});
    AURORA_TEST_REQUIRE_EQ(m.nodes().size(), 3U);
    AURORA_TEST_CHECK_EQ(m.nodes()[0]->kind(), ModifierNode::Kind::Layout);
    AURORA_TEST_CHECK_EQ(m.nodes()[1]->kind(), ModifierNode::Kind::Paint);
    AURORA_TEST_CHECK_EQ(m.nodes()[2]->kind(), ModifierNode::Kind::Input);
}

AURORA_TEST_CASE(factory_returns_new_modifier_chain_reusable) {
    // 工厂方法返回副本：同一 base 可派生多条互不影响的链。
    const Modifier base = Modifier{}.padding(4.0F);
    const Modifier with_bg = base.background(Color(1, 2, 3, 255));
    const Modifier with_border = base.border(1.0F, Color(0, 0, 0, 255));
    AURORA_TEST_CHECK_EQ(base.nodes().size(), 1U);
    AURORA_TEST_CHECK_EQ(with_bg.nodes().size(), 2U);
    AURORA_TEST_CHECK_EQ(with_border.nodes().size(), 2U);
}

AURORA_TEST_CASE(factory_padding_degrades_negative_to_zero) {
    const Modifier m = Modifier{}.padding(-3.0F);
    AURORA_TEST_REQUIRE_EQ(m.nodes().size(), 1U);
    const auto *p = dynamic_cast<const Padding *>(m.nodes()[0].get());
    AURORA_TEST_REQUIRE_NOT_NULL(p);
    AURORA_TEST_CHECK_NEAR(p->padding(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(flex_weight_reads_first_positive_weight) {
    AURORA_TEST_CHECK_NEAR(Modifier{}.flex_weight(), 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(Modifier{}.expand(2.0F).flex_weight(), 2.0F, 0.0F);
    // 无权重的其他节点返回 0，不干扰首个正权重读取。
    AURORA_TEST_CHECK_NEAR(Modifier{}.padding(4.0F).expand(1.5F).flex_weight(), 1.5F, 0.0F);
    AURORA_TEST_CHECK_NEAR(Modifier{}.expand(0.0F).flex_weight(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(transform_info_empty_chain_is_identity) {
    const auto info = Modifier{}.transform(size_of(100.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(info.translation.x, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.translation.y, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.content_size.width, 100.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.content_size.height, 80.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.opacity, 1.0F, 0.0F);
}

AURORA_TEST_CASE(transform_info_padding_shifts_and_shrinks_content) {
    const auto info = Modifier{}.padding(10.0F).transform(size_of(100.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(info.translation.x, 10.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.translation.y, 10.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.content_size.width, 80.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.content_size.height, 60.0F, 0.0F);
}

AURORA_TEST_CASE(transform_info_padding_edges_asymmetric_shift) {
    const auto info = Modifier{}
                          .padding(EdgeInsets{.left = 3.0F, .top = 5.0F, .right = 7.0F, .bottom = 1.0F})
                          .transform(size_of(50.0F, 40.0F));
    AURORA_TEST_CHECK_NEAR(info.translation.x, 3.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.translation.y, 5.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.content_size.width, 40.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.content_size.height, 34.0F, 0.0F);
}

AURORA_TEST_CASE(transform_info_offset_accumulates_translation) {
    const auto info = Modifier{}.padding(2.0F).offset(5.0F, 6.0F).transform(size_of(50.0F, 50.0F));
    AURORA_TEST_CHECK_NEAR(info.translation.x, 7.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.translation.y, 8.0F, 0.0F);
}

AURORA_TEST_CASE(transform_info_align_translates_to_child_origin) {
    // Align 的 translation 依赖 child_size（需先经 layout 记录）。
    Modifier m = Modifier{}.align(Alignment::Center);
    const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 100.0F, .height = 80.0F}};
    AURORA_TEST_REQUIRE_EQ(m.nodes().size(), 1U);
    (void)m.nodes()[0]->layout(c, [](const Constraints &) -> Size { return size_of(50.0F, 20.0F); });

    const auto info = m.transform(size_of(100.0F, 80.0F));
    // Center：((100-50)/2, (80-20)/2) = (25, 30)；content_size 收缩为子尺寸。
    AURORA_TEST_CHECK_NEAR(info.translation.x, 25.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(info.translation.y, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(info.content_size.width, 50.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(info.content_size.height, 20.0F, 0.0F);
}

AURORA_TEST_CASE(paint_boxes_falls_back_to_empty_without_box_changing_nodes) {
    // 链上无盒改变节点（Align/Offset）→ 返回空数组，
    // 调用方沿用控件自身视觉盒（历史行为逐位一致）。
    const Rect self{.origin = Point{.x = 10.0F, .y = 20.0F}, .size = size_of(100.0F, 80.0F)};
    const Modifier m = Modifier{}.background(Color(1, 2, 3, 255)).border(1.0F, Color(0, 0, 0, 255));
    AURORA_TEST_CHECK(m.paint_boxes(self).empty());
    AURORA_TEST_CHECK(Modifier{}.opacity(0.5F).paint_boxes(self).empty());
}

AURORA_TEST_CASE(paint_boxes_put_background_inside_the_aligned_box) {
    // 居中块的写法：align 靠外、size 靠内（先压入者靠外）。
    // Column 给非加权子项的交叉轴（宽）是既定槽位、主轴（高）只是剩余空间（loose_height）——
    // Align 只在宽度轴展开成整行，其内侧的背景仍画在居中的 120×40，且不吞掉同列兄弟的空间。
    const Modifier m = Modifier{}.align(Alignment::Center).size(120.0F, 40.0F).background(Color(9, 9, 9, 255));
    const Constraints c{
        .min = size_of(0.0F, 0.0F), .max = Size{.width = 400.0F, .height = 260.0F}, .loose_height = true};
    const Size self = run_chain(m, c, size_of(30.0F, 12.0F));
    AURORA_TEST_CHECK_NEAR(self.width, 400.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(self.height, 40.0F, 0.0F);

    const Rect widget_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self};
    const std::vector<Rect> boxes = m.paint_boxes(widget_box);
    AURORA_TEST_REQUIRE_EQ(boxes.size(), 3U);
    // [0]=Align（占整行）、[1]=Size、[2]=Background（外侧 Align 收缩后的居中子盒）。
    AURORA_TEST_CHECK(boxes[0] == widget_box);
    AURORA_TEST_CHECK_NEAR(boxes[1].origin.x, 140.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(boxes[1].origin.y, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(boxes[1].size.width, 120.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(boxes[1].size.height, 40.0F, 0.0F);
    AURORA_TEST_CHECK(boxes[2] == boxes[1]);

    // 与绘制/命中用的内容盒同源：transform() 的平移与尺寸须落在同一个盒上。
    const auto tf = m.transform(self);
    AURORA_TEST_CHECK_NEAR(tf.translation.x, boxes[2].origin.x, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tf.translation.y, boxes[2].origin.y, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tf.content_size.width, boxes[2].size.width, 0.0F);
    AURORA_TEST_CHECK_NEAR(tf.content_size.height, boxes[2].size.height, 0.0F);
}

AURORA_TEST_CASE(paint_boxes_align_inner_of_size_cannot_center_the_block) {
    // 反序写法 size(120,40).background().align(Center)：Align 在最内层，拿到的是 Size
    // 给的紧约束，只能把「内容」对齐到自身盒内，永远无法把 120×40 块在父容器居中。
    // 背景盒因此保持整块（逐位等于控件自身盒）。
    const Modifier m = Modifier{}.size(120.0F, 40.0F).background(Color(9, 9, 9, 255)).align(Alignment::Center);
    const Constraints c{.min = size_of(0.0F, 0.0F),
                        .max = Size{.width = 400.0F, .height = std::numeric_limits<float>::infinity()}};
    const Size self = run_chain(m, c, size_of(30.0F, 12.0F));
    AURORA_TEST_CHECK_NEAR(self.width, 120.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(self.height, 40.0F, 0.0F);

    const Rect widget_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self};
    const std::vector<Rect> boxes = m.paint_boxes(widget_box);
    AURORA_TEST_REQUIRE_EQ(boxes.size(), 3U);
    AURORA_TEST_CHECK(boxes[0] == widget_box);
    AURORA_TEST_CHECK(boxes[1] == widget_box);  // Background 与控件同盒
    // 最内的 Align 只收缩「其内侧」的盒（此处已无节点），内容盒 = 子尺寸在块内居中。
    const auto tf = m.transform(self);
    AURORA_TEST_CHECK_NEAR(tf.translation.x, 45.0F, 1e-4F);  // (120-30)/2
    AURORA_TEST_CHECK_NEAR(tf.translation.y, 14.0F, 1e-4F);  // (40-12)/2
}

AURORA_TEST_CASE(paint_boxes_offset_translates_inner_paint_nodes) {
    const Rect widget_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = size_of(100.0F, 80.0F)};
    // Offset 靠外：内侧的绘制（含背景）随内容一起平移，尺寸不变（与命中盒一致）。
    const Modifier off = Modifier{}.offset(5.0F, 7.0F).background(Color(1, 1, 1, 255));
    const std::vector<Rect> ob = off.paint_boxes(widget_box);
    AURORA_TEST_REQUIRE_EQ(ob.size(), 2U);
    AURORA_TEST_CHECK(ob[0] == widget_box);
    AURORA_TEST_CHECK_NEAR(ob[1].origin.x, 5.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(ob[1].origin.y, 7.0F, 0.0F);
    AURORA_TEST_CHECK(ob[1] == Rect{.origin = Point{.x = 5.0F, .y = 7.0F}, .size = widget_box.size});
}

AURORA_TEST_CASE(paint_boxes_ignore_padding_by_contract) {
    // 规格 §7.4：Paint 修饰覆盖内边距是既定语义，Padding 不参与分段（两种链序都不改变盒）。
    const Rect widget_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = size_of(100.0F, 80.0F)};
    AURORA_TEST_CHECK(Modifier{}.padding(6.0F).background(Color(1, 1, 1, 255)).paint_boxes(widget_box).empty());
    AURORA_TEST_CHECK(Modifier{}
                          .background(Color(1, 1, 1, 255))
                          .padding(EdgeInsets{.left = 3.0F, .top = 5.0F, .right = 7.0F, .bottom = 1.0F})
                          .paint_boxes(widget_box)
                          .empty());
}

AURORA_TEST_CASE(transform_info_opacity_multiplies) {
    const auto info = Modifier{}.opacity(0.5F).opacity(0.4F).transform(size_of(10.0F, 10.0F));
    AURORA_TEST_CHECK_NEAR(info.opacity, 0.2F, 1e-6F);
}

AURORA_TEST_CASE(transform_info_rotate_matrix_fixes_content_center) {
    const auto info = Modifier{}.rotate(90.0F).transform(size_of(100.0F, 60.0F));
    // 绕内容中心 (50,30)：中心点不动。
    const Point p = info.matrix.apply_to_point(Point{.x = 50.0F, .y = 30.0F});
    AURORA_TEST_CHECK_NEAR(p.x, 50.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(p.y, 30.0F, 1e-3F);
    // 90° 顺时针：(60,30)（中心右 10px）→ (50,40)（中心下 10px）。
    const Point q = info.matrix.apply_to_point(Point{.x = 60.0F, .y = 30.0F});
    AURORA_TEST_CHECK_NEAR(q.x, 50.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(q.y, 40.0F, 1e-3F);
}

AURORA_TEST_CASE(detect_predicates_reflect_chain_content) {
    AURORA_TEST_CHECK_FALSE(Modifier{}.has_clickable());
    AURORA_TEST_CHECK_TRUE(Modifier{}.clickable([]() -> void {}).has_clickable());
    AURORA_TEST_CHECK_FALSE(Modifier{}.has_gesture());
    AURORA_TEST_CHECK_TRUE(Modifier{}.draggable([](Point, Point) -> void {}).has_gesture());
    AURORA_TEST_CHECK_TRUE(Modifier{}.long_press([]() -> void {}).has_gesture());
    AURORA_TEST_CHECK_FALSE(Modifier{}.has_tooltip());
    AURORA_TEST_CHECK_TRUE(Modifier{}.tooltip("t").has_tooltip());
    AURORA_TEST_CHECK_FALSE(Modifier{}.has_context_menu());
    AURORA_TEST_CHECK_TRUE(Modifier{}.context_menu({}).has_context_menu());
}

AURORA_TEST_CASE(invoke_click_fires_all_clickables) {
    int hits = 0;
    const Modifier m =
        Modifier{}.clickable([&hits]() -> void { ++hits; }).padding(2.0F).clickable([&hits]() -> void { hits += 10; });
    m.invoke_click();
    AURORA_TEST_CHECK_EQ(hits, 11);
}

AURORA_TEST_CASE(drag_pipeline_binds_fires_and_releases) {
    int starts = 0;
    int drags = 0;
    int ends = 0;
    const Modifier m = Modifier{}.draggable([&](Point, Point) -> void { ++drags; }, [&]() -> void { ++starts; },
                                            [&]() -> void { ++ends; });

    // 绑定指针 1；指针 2 的移动/抬起不响应。
    m.invoke_drag_start(std::optional<int>(1));
    AURORA_TEST_CHECK_EQ(starts, 1);
    m.invoke_drag(Point{.x = 1.0F, .y = 1.0F}, Point{}, std::optional<int>(2));
    AURORA_TEST_CHECK_EQ(drags, 0);
    m.invoke_drag(Point{.x = 3.0F, .y = 4.0F}, Point{}, std::optional<int>(1));
    AURORA_TEST_CHECK_EQ(drags, 1);
    m.invoke_drag_end(std::optional<int>(2));
    AURORA_TEST_CHECK_EQ(ends, 0);
    m.invoke_drag_end(std::optional<int>(1));
    AURORA_TEST_CHECK_EQ(ends, 1);
    // 抬起解绑后可再次按下（重新绑定任意指针）。
    m.invoke_drag_start(std::optional<int>(7));
    AURORA_TEST_CHECK_EQ(starts, 2);
}

AURORA_TEST_CASE(long_press_pipeline_fires_and_mutex_reports) {
    int fires = 0;
    const Modifier m = Modifier{}.long_press([&fires]() -> void { ++fires; }, 200.0F);
    AURORA_TEST_CHECK_FALSE(m.long_press_fired());
    m.press_long_press(t_ms(1'000), std::optional<int>(1));
    m.tick_long_press(t_ms(1'100));
    AURORA_TEST_CHECK_EQ(fires, 0);
    m.tick_long_press(t_ms(1'200));
    AURORA_TEST_CHECK_EQ(fires, 1);
    AURORA_TEST_CHECK_TRUE(m.long_press_fired());
    // 取消后不再累计触发。
    m.cancel_long_press(std::optional<int>(1));
}

AURORA_TEST_CASE(long_press_cancel_prevents_fire) {
    int fires = 0;
    const Modifier m = Modifier{}.long_press([&fires]() -> void { ++fires; }, 100.0F);
    m.press_long_press(t_ms(1'000), std::nullopt);
    m.cancel_long_press(std::nullopt);
    m.tick_long_press(t_ms(9'999));
    AURORA_TEST_CHECK_EQ(fires, 0);
}

AURORA_TEST_CASE(tooltip_pipeline_delays_then_hides) {
    const Modifier m = Modifier{}.tooltip("tip", 300.0F);
    AURORA_TEST_CHECK_EQ(m.active_tooltip(), "");
    m.tooltip_hover_start(t_ms(1'000));
    m.tick_tooltip(t_ms(1'200));
    AURORA_TEST_CHECK_EQ(m.active_tooltip(), "");
    m.tick_tooltip(t_ms(1'300));
    AURORA_TEST_CHECK_EQ(m.active_tooltip(), "tip");
    m.tooltip_hover_end();
    AURORA_TEST_CHECK_EQ(m.active_tooltip(), "");
}

AURORA_TEST_CASE(context_menu_pipeline_open_items_and_position) {
    MenuItem item;
    item.label = "Paste";
    const Modifier m = Modifier{}.context_menu({item});
    AURORA_TEST_CHECK_EQ(m.active_context_menu_items().size(), 0U);
    m.open_context_menu(Point{.x = 8.0F, .y = 9.0F});
    AURORA_TEST_REQUIRE_EQ(m.active_context_menu_items().size(), 1U);
    AURORA_TEST_CHECK_EQ(m.active_context_menu_items()[0].label, "Paste");
    AURORA_TEST_CHECK_NEAR(m.active_context_menu_position().x, 8.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(m.active_context_menu_position().y, 9.0F, 0.0F);
    m.close_context_menu();
    AURORA_TEST_CHECK_EQ(m.active_context_menu_items().size(), 0U);
}

AURORA_TEST_CASE(on_pointer_event_reaches_touch_listener) {
    int seen = 0;
    const Modifier m = Modifier{}.touch([&seen](const TouchEvent &) -> void { ++seen; });
    TouchEvent e;
    m.on_pointer_event(e);
    m.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(seen, 2);
}

}  // namespace aurora::test_cases::utest_modifier
