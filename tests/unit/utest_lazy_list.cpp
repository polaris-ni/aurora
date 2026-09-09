/// 测试类型: unit
/// 目标单元: include/aurora/widget/lazy_list.h
/// 测试说明: 覆盖 LazyList——默认不变量、count/行高参数钳制与降级、按需构建仅可见窗口条目（实例复用）、
/// cache_extent 窗口、滚动偏移钳制与 scroll_to_item、滚轮步进、滚出窗口回收重建、序列化与自描述

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/lazy_list.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_lazy_list {

namespace {

/// 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 条目构建观测器：记录构建序号并保留各条目实例（虚拟化实例被回收后仍可读其几何）。
struct BuildRecorder {
    std::map<int, std::shared_ptr<FixedBox>> items;
    std::vector<int> built_order;

    auto builder() -> LazyList::ItemBuilder {
        return [this](int index) -> Node {
            built_order.push_back(index);
            auto box = std::make_shared<FixedBox>(300.0F, 48.0F);
            items.emplace(index, std::move(box));
            return Node{items.at(index)};
        };
    }
};

}  // namespace

AURORA_TEST_CASE(default_lazy_list_invariants) {
    LazyList list;
    AURORA_TEST_CHECK_EQ(std::string{list.type_name()}, "LazyList");
    AURORA_TEST_CHECK_EQ(list.count(), 0);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.content_height(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.max_scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(list.visible_range().first, 0);
    AURORA_TEST_CHECK_EQ(list.visible_range().second, 0);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 0U);
}

AURORA_TEST_CASE(ctor_clamps_count_and_degrades_extent) {
    LazyList negative{-5, {}, 48.0F};
    AURORA_TEST_CHECK_EQ(negative.count(), 0);
    AURORA_TEST_CHECK_NEAR(negative.content_height(), 0.0F, 1e-4F);

    // 非正行高降级为默认 48（经 Diagnostics::degraded 上报）。
    LazyList degraded{5, {}, -1.0F};
    AURORA_TEST_CHECK_NEAR(degraded.content_height(), 240.0F, 1e-4F);

    LazyList normal{100, {}, 48.0F};
    AURORA_TEST_CHECK_NEAR(normal.content_height(), 4800.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_builds_only_visible_window) {
    BuildRecorder rec;
    LazyList list{100, rec.builder(), 48.0F};  // 默认 cache_extent = 200
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(list.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.size().height, 400.0F, 1e-4F);
    const auto [first, last] = list.visible_range();
    AURORA_TEST_CHECK_EQ(first, 0);
    AURORA_TEST_CHECK_EQ(last, 13);  // ceil((400 + 200) / 48)
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 13U);
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 13U);  // 只构建窗口内条目，而非 count=100

    // 约束不变的重复布局不重建任何条目（实例复用）。
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 13U);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 13U);
}

AURORA_TEST_CASE(cache_extent_zero_builds_exact_window) {
    BuildRecorder rec;
    LazyList list{100, rec.builder(), 48.0F};
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(list.visible_range().first, 0);
    AURORA_TEST_CHECK_EQ(list.visible_range().second, 9);  // ceil(400 / 48)
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 9U);
    // 存活条目被条目约束（min == max = 视口宽 × 行高）强制整形。
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->size().height, 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(8)->size().height, 48.0F, 1e-4F);
}

AURORA_TEST_CASE(scroll_offset_setter_clamps_to_content) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(list.max_scroll_offset(), 4400.0F, 1e-4F);  // 4800 - 400
    list.set_scroll_offset(-10.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    list.set_scroll_offset(99999.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-4F);
}

AURORA_TEST_CASE(scroll_to_item_aligns_index_top) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    list.scroll_to_item(50);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 2400.0F, 1e-4F);  // 50 * 48
    // 越界索引钳到 [0, count-1]，偏移再钳到内容范围：99*48=4752 → 4400。
    list.scroll_to_item(1000);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-4F);
    list.scroll_to_item(-3);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(wheel_scroll_steps_offset) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    list.set_scroll_offset(200.0F);
    // delta_y 正方向为向上滚动：offset 减小一个滚轮步进 40。
    ScrollEvent e;
    e.delta_y = 1.0F;
    list.on_scroll(e);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 160.0F, 1e-4F);
    ScrollEvent down;
    down.delta_y = -1.0F;
    list.on_scroll(down);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 200.0F, 1e-4F);
}

AURORA_TEST_CASE(scrolling_recycles_and_rebuilds_window) {
    BuildRecorder rec;
    LazyList list{100, rec.builder(), 48.0F};
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 9U);  // 窗口 0..8

    list.set_scroll_offset(960.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 9U);   // 新窗口 20..28
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 18U);  // 旧窗口全部回收、新窗口全部新建

    // 绘制后条目落位于内容坐标 - 滚动偏移处。
    Painter p;
    p.begin(300, 400);
    list.paint(p,
               Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 300.0F, .height = 400.0F}},
               BuildContext{});
    AURORA_TEST_CHECK_NEAR(rec.items.at(20)->paint_bounds().origin.y, 0.0F, 1e-4F);    // 20*48 - 960
    AURORA_TEST_CHECK_NEAR(rec.items.at(24)->paint_bounds().origin.y, 192.0F, 1e-4F);  // 24*48 - 960
    AURORA_TEST_CHECK_NEAR(rec.items.at(24)->paint_bounds().size.height, 48.0F, 1e-4F);
}

AURORA_TEST_CASE(serialize_props_and_describe_metadata) {
    LazyList src{7, {}, 24.0F};
    src.set_cache_extent(0.0F);
    src.set_scroll_offset(96.0F);
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["count"].get<int>(), 7);
    AURORA_TEST_CHECK_NEAR(props["item_extent"].get<float>(), 24.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["scroll_offset"].get<float>(), 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["cache_extent"].get<float>(), 0.0F, 1e-4F);

    const auto d = LazyList::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "LazyList");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool count_required = false;
    for (const auto &p : d.properties) {
        if (p.name == "count") {
            count_required = p.required;
        }
    }
    AURORA_TEST_CHECK_TRUE(count_required);
    AURORA_TEST_CHECK_FALSE(d.invariants.empty());
}

}  // namespace aurora::test_cases::utest_lazy_list
