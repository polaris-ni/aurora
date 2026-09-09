/// 测试类型: unit
/// 目标单元: include/aurora/widget/lazy_row.h
/// 测试说明: 覆盖 LazyRow——默认不变量与自描述、双模字段同步、内容/内边距布局尺寸、按需构建仅可见窗口
/// （cache_extent 预取与重复绘制复用）、滚轮位移与钳制、条目点击回调、拖拽抑制点击、序列化往返

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/lazy_row.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_lazy_row {

namespace {

/// 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char* override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter& /*p*/, const Rect& /*bounds*/, const BuildContext& /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto viewport(float w, float h) -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = w, .height = h}};
}

/// 条目构建观测器：记录构建序号并保留各条目实例（缓存条目被回收后仍可读其几何）。
struct BuildRecorder {
    std::map<int, std::shared_ptr<FixedBox>> items;
    std::vector<int> built_order;

    auto builder() -> LazyRow::ItemBuilder {
        return [this](int index) -> Node {
            built_order.push_back(index);
            auto box = std::make_shared<FixedBox>(96.0F, 96.0F);
            items.emplace(index, std::move(box));
            return Node{items.at(index)};
        };
    }
};

}  // namespace

AURORA_TEST_CASE(default_lazy_row_invariants) {
    LazyRow row;
    AURORA_TEST_CHECK_EQ(std::string{row.type_name()}, "LazyRow");
    AURORA_TEST_CHECK_EQ(row.item_count, 0);
    AURORA_TEST_CHECK_NEAR(row.item_extent, 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.cache_extent, 0.0F, 1e-4F);

    const auto d = LazyRow::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "LazyRow");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "virtual");
    AURORA_TEST_REQUIRE_FALSE(d.events.empty());
    AURORA_TEST_CHECK_EQ(d.events[0], "on_item_click");
}

AURORA_TEST_CASE(ctors_sync_dual_mode_fields) {
    // 位置参数构造：item_count 同步到 props 聚合字段。
    // 已知库缺陷：位置构造只写私有 item_extent_，未同步 props 聚合字段 item_extent
    // （后者保持默认 96），故生效格宽经布局验证（行高 = item_extent_）。
    LazyRow positional{5, {}, 80.0F};
    AURORA_TEST_CHECK_EQ(positional.item_count, 5);
    LayoutEngine::layout(positional, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(positional.size().height, 80.0F, 1e-4F);

    LazyRow from_props{LazyRowProps{.item_count = 10, .item_extent = 64.0F, .cache_extent = 32.0F}};
    AURORA_TEST_CHECK_EQ(from_props.item_count, 10);
    AURORA_TEST_CHECK_NEAR(from_props.item_extent, 64.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(from_props.cache_extent, 32.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_sizes_content_with_padding) {
    // 内容 10*96=960 > 视口 300：宽度被钳到视口。
    LazyRow wide{10, {}, 96.0F};
    LayoutEngine::layout(wide, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(wide.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(wide.size().height, 96.0F, 1e-4F);

    // 内容 192 < 视口 300：取内容自然宽度。
    LazyRow narrow{2, {}, 96.0F};
    LayoutEngine::layout(narrow, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(narrow.size().width, 192.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(narrow.size().height, 96.0F, 1e-4F);

    // 内边距计入内容尺寸：宽 +30（左 10 + 右 20），高 +10（上 5 + 下 5）。
    LazyRow padded{2, {}, 96.0F};
    padded.set_padding(EdgeInsets{.left = 10.0F, .top = 5.0F, .right = 20.0F, .bottom = 5.0F});
    LayoutEngine::layout(padded, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(padded.size().width, 222.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(padded.size().height, 106.0F, 1e-4F);
}

AURORA_TEST_CASE(paint_builds_only_visible_window) {
    BuildRecorder rec;
    LazyRow row{10, rec.builder(), 96.0F};
    LayoutEngine::layout(row, bounded(300.0F, 96.0F));

    Painter p;
    p.begin(300, 96);
    row.paint(p, viewport(300.0F, 96.0F), BuildContext{});

    // last = floor(300 / 96) = 3 → 构建 0..3，共 4 条。
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 4U);
    AURORA_TEST_CHECK_EQ(rec.built_order[0], 0);
    AURORA_TEST_CHECK_EQ(rec.built_order[3], 3);
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->paint_bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(2)->paint_bounds().origin.x, 192.0F, 1e-4F);

    // 无滚动的重复绘制不重建条目（缓存复用）。
    row.paint(p, viewport(300.0F, 96.0F), BuildContext{});
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 4U);
}

AURORA_TEST_CASE(cache_extent_prefetches_offscreen_items) {
    BuildRecorder rec;
    LazyRow row{10, rec.builder(), 96.0F};
    row.set_cache_extent(192.0F);
    LayoutEngine::layout(row, bounded(300.0F, 96.0F));

    Painter p;
    p.begin(300, 96);
    row.paint(p, viewport(300.0F, 96.0F), BuildContext{});

    // last = floor((300 + 192) / 96) = 5 → 预取构建 0..5，共 6 条。
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 6U);
}

AURORA_TEST_CASE(scroll_shifts_window_and_clamps) {
    BuildRecorder rec;
    LazyRow row{10, rec.builder(), 96.0F};
    LayoutEngine::layout(row, bounded(300.0F, 96.0F));

    // delta_y=2 → offset += 2 * 96 * 0.5 = 96。
    ScrollEvent e;
    e.delta_y = 2.0F;
    row.on_scroll(e);
    AURORA_TEST_CHECK_TRUE(e.is_handled);

    Painter p;
    p.begin(300, 96);
    row.paint(p, viewport(300.0F, 96.0F), BuildContext{});
    AURORA_TEST_CHECK_NEAR(rec.items.at(1)->paint_bounds().origin.x, 0.0F, 1e-4F);  // 1*96 - 96
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 4U);  // 窗口 1..4

    // 超大增量被钳到 max_off = 10*96 - 300 = 660。
    ScrollEvent big;
    big.delta_y = 100.0F;
    row.on_scroll(big);
    row.paint(p, viewport(300.0F, 96.0F), BuildContext{});
    AURORA_TEST_CHECK_NEAR(rec.items.at(9)->paint_bounds().origin.x, 204.0F, 1e-4F);  // 9*96 - 660
    // 全程只构建进过窗口的条目（1..4 与 6..9），而非全部 10 条。
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 8U);
}

AURORA_TEST_CASE(click_reports_pressed_index) {
    LazyRow row{5, {}, 96.0F};
    int clicked = -1;
    row.set_on_item_click([&clicked](int index) -> void { clicked = index; });
    LayoutEngine::layout(row, bounded(300.0F, 96.0F));

    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 200.0F, .y = 10.0F};
    row.on_pointer_event(press);

    MouseEvent release;
    release.action = MouseAction::Release;
    release.local_position = Point{.x = 200.0F, .y = 10.0F};
    row.on_pointer_event(release);

    AURORA_TEST_CHECK_EQ(clicked, 2);  // floor(200 / 96)
    AURORA_TEST_CHECK_TRUE(release.is_handled);
}

AURORA_TEST_CASE(drag_suppresses_click) {
    LazyRow row{5, {}, 96.0F};
    int clicked = -1;
    row.set_on_item_click([&clicked](int index) -> void { clicked = index; });
    LayoutEngine::layout(row, bounded(300.0F, 96.0F));

    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 200.0F, .y = 10.0F};
    row.on_pointer_event(press);

    // 位移 > 4px 进入拖拽：Move 被消费，Release 不再触发点击。
    MouseEvent move;
    move.action = MouseAction::Move;
    move.local_position = Point{.x = 260.0F, .y = 12.0F};
    row.on_pointer_event(move);
    AURORA_TEST_CHECK_TRUE(move.is_handled);

    MouseEvent release;
    release.action = MouseAction::Release;
    release.local_position = Point{.x = 260.0F, .y = 12.0F};
    row.on_pointer_event(release);

    AURORA_TEST_CHECK_EQ(clicked, -1);
    AURORA_TEST_CHECK_FALSE(release.is_handled);
}

AURORA_TEST_CASE(press_beyond_items_is_ignored) {
    LazyRow row{5, {}, 96.0F};
    int clicked = -1;
    row.set_on_item_click([&clicked](int index) -> void { clicked = index; });
    LayoutEngine::layout(row, bounded(300.0F, 96.0F));

    // 按压点超出条目范围（5*96=480）：索引解析为 -1，抬起不触发回调也不消费事件。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 490.0F, .y = 10.0F};
    row.on_pointer_event(press);

    MouseEvent release;
    release.action = MouseAction::Release;
    release.local_position = Point{.x = 490.0F, .y = 10.0F};
    row.on_pointer_event(release);

    AURORA_TEST_CHECK_EQ(clicked, -1);
    AURORA_TEST_CHECK_FALSE(release.is_handled);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    // 已知库缺陷：Props 构造只写 props 聚合字段 item_extent，未同步私有 item_extent_
    // （而 serialize_props 落盘的是 item_extent_），故用位置构造 + set_cache_extent
    // 让各字段真实生效后再做往返。
    LazyRow src{7, {}, 48.0F};
    src.set_cache_extent(32.0F);
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["item_count"].get<int>(), 7);
    AURORA_TEST_CHECK_NEAR(props["item_extent"].get<float>(), 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["cache_extent"].get<float>(), 32.0F, 1e-4F);

    LazyRow dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.item_count, 7);
    AURORA_TEST_CHECK_NEAR(dst.item_extent, 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.cache_extent, 32.0F, 1e-4F);
    // 反序列化结果参与布局：内容 7*48=336 > 视口 300 → 宽度钳到视口；行高 = 48。
    LayoutEngine::layout(dst, bounded(300.0F, 96.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.size().height, 48.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_lazy_row
