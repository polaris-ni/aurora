/// 测试类型: unit
/// 目标单元: include/aurora/widget/lazy_row.h
/// 测试说明: 覆盖 LazyRow——默认不变量与自描述、属性权威表五行读写、内容/内边距布局尺寸、按需构建仅可见窗口
/// （cache_extent 预取与重复绘制复用）、滚轮位移与钳制、程序化 set_scroll_offset 的夹取与可见窗口位移、
/// 条目点击回调、拖拽抑制点击、序列化往返。
/// 属性单一声明迁移后另覆盖：权威表五行即描述符的 `properties`（`offset` 不在表里）、未表态的行不写键而
/// 表外运行键 `offset` 恒在场、逐行 `clear` 撤表态后该键退盘且生效值退回该行回落基线、非法 JSON 逐行降级并
/// 上报（`restore_key` 是未登记 `validate` 的字符串行 ⇒ 按宽容通路照常表态）、挂载后按行声明档位标脏
/// （`cache_extent` 仅绘制档、`restore_key` 无档、三枚双档行两档各一次、同值写入短路）、常量档读件与库内
/// `resolve(row, ctx)` 同值、`offset` 只有控件面通道而没有属性面同名写件

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/diagnostics.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/lazy_row.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_lazy_row {

using aurora::testing::require_field;

namespace {

/// 权威表五行的键名（表序 = 声明序 = schema 序；`padding` 本轮进表、追加表尾）。
constexpr std::array<std::string_view, 5> AURORA_TABLE_KEYS = {"item_count", "item_extent", "cache_extent",
                                                               "restore_key", "padding"};
/// 各行描述符的 `type` 与 `note` 期望（逐位对号；用 `std::array` + `.at(i)` 而非裸数组下标，
/// 是本仓测试侧对 `cppcoreguidelines-pro-bounds-constant-array-index` 的既有写法）。
constexpr std::array<std::string_view, 5> AURORA_TYPES = {"int", "float", "float", "string", "EdgeInsets"};
constexpr std::array<std::string_view, 5> AURORA_NOTES = {
    "Child count", "Fixed child width (px)", "Prebuild buffer beyond the viewport (px)",
    "Scroll position save key (empty = excluded from restore)", "Padding around items (dp)"};

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

/// 探针：把只在库内消费的读件提到可读位置（`present` 表态位、`resolve` 带上下文解析）。
/// 两者都不升公共面，故判据只能经派生类点名读取；读到的仍是生产侧同一条函数，不是用例另算一遍。
class LazyRowProbe final : public LazyRow {
  public:
    using LazyRow::present;
    using LazyRow::resolve;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct LazyRowDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(LazyRow &target) -> void {
        paints = 0;
        layouts = 0;
        target.on_dirty = [this](bool layout) {
            if (layout) {
                ++layouts;
            } else {
                ++paints;
            }
        };
    }
};

/// 「属性面有没有 `set_offset`」的可检测性：`offset` 是表外运行键，词根规则推不出也不该推出这枚写件。
template <typename Props>
concept has_offset_writer = requires(Props props) { props.set_offset(1.0F); };

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
    LazyRow const row;
    AURORA_TEST_CHECK_EQ(std::string{row.type_name()}, "LazyRow");
    // 五行皆未表态 ⇒ 生效值即各行回落基线（与迁移前的字段声明处默认值逐值相同）。
    AURORA_TEST_CHECK_EQ(row.item_count(), 0);
    AURORA_TEST_CHECK_NEAR(row.item_extent(), 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.cache_extent(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(row.restore_key().empty());
    AURORA_TEST_CHECK_NEAR(row.padding().left, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.padding().bottom, 0.0F, 1e-4F);

    const LazyRowProps d = LazyRow::defaults();
    AURORA_TEST_CHECK_EQ(d.item_count(), 0);
    AURORA_TEST_CHECK_NEAR(d.item_extent(), 96.0F, 1e-4F);

    const auto desc = LazyRow::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{desc.name}, "LazyRow");
    AURORA_TEST_CHECK_EQ(std::string{desc.children_policy}, "virtual");
    AURORA_TEST_REQUIRE_FALSE(desc.events.empty());
    AURORA_TEST_CHECK_EQ(desc.events[0], "on_item_click");
}

AURORA_TEST_CASE(descriptor_properties_are_table_derived) {
    // `properties` 全部由权威表发射：五行、表序即声明序、类型与说明逐行对号；
    // 运行时 `offset` 不在表里 ⇒ 描述符没有那一枚（它由 `serialize_props` 追加，不是属性行）。
    const auto desc = LazyRow::describe_static();
    AURORA_TEST_REQUIRE_EQ(desc.properties.size(), 5U);
    for (std::size_t i = 0; i < 5U; ++i) {
        AURORA_TEST_CHECK_EQ(desc.properties[i].name, std::string{AURORA_TABLE_KEYS.at(i)});
        AURORA_TEST_CHECK_EQ(desc.properties[i].type, std::string{AURORA_TYPES.at(i)});
        AURORA_TEST_CHECK_EQ(desc.properties[i].note, std::string{AURORA_NOTES.at(i)});
        AURORA_TEST_CHECK_FALSE(desc.properties[i].required);
    }
    for (const auto &entry : desc.properties) {
        AURORA_TEST_CHECK_NE(entry.name, "offset");
    }
}

AURORA_TEST_CASE(ctors_write_the_same_rows_the_readers_read) {
    // 位置参数构造转调属性面写件：私有副本与聚合字段那处双存储消失后，
    // 「构造给的格宽」与「读件与序列化落盘的格宽」是同一枚行的同一个值（迁移前此处有两处不同步）。
    LazyRow positional{5, {}, 80.0F};
    AURORA_TEST_CHECK_EQ(positional.item_count(), 5);
    AURORA_TEST_CHECK_NEAR(positional.item_extent(), 80.0F, 1e-4F);
    LayoutEngine::layout(positional, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(positional.size().height, 80.0F, 1e-4F);  // 行高即该行生效值

    LazyRow from_props{LazyRowProps().set_item_count(10).set_item_extent(64.0F).set_cache_extent(32.0F)};
    AURORA_TEST_CHECK_EQ(from_props.item_count(), 10);
    AURORA_TEST_CHECK_NEAR(from_props.item_extent(), 64.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(from_props.cache_extent(), 32.0F, 1e-4F);
    LayoutEngine::layout(from_props, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(from_props.size().height, 64.0F, 1e-4F);

    // 位置构造的第三枚实参省略 ⇒ 该行未表态，生效值即回落基线（迁移前的 96 默认实参同值）。
    LazyRow const defaulted{3, {}};
    AURORA_TEST_CHECK_NEAR(defaulted.item_extent(), 96.0F, 1e-4F);
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
    AURORA_TEST_CHECK_NEAR(padded.padding().right, 20.0F, 1e-4F);  // 布局读的就是该行生效值
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

AURORA_TEST_CASE(programmatic_scroll_offset_clamps_and_shifts_window) {
    BuildRecorder rec;
    LazyRow row{10, rec.builder(), 96.0F};
    LayoutEngine::layout(row, bounded(300.0F, 96.0F));

    AURORA_TEST_CHECK_NEAR(row.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.max_scroll_offset(), (10.0F * 96.0F) - 300.0F, 1e-3F);

    Painter p;
    p.begin(300, 96);
    row.paint(p, viewport(300.0F, 96.0F), BuildContext{});
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->paint_bounds().origin.x, 0.0F, 1e-4F);

    row.set_scroll_offset(96.0F);  // 右移一项
    AURORA_TEST_CHECK_NEAR(row.scroll_offset(), 96.0F, 1e-4F);
    p.begin(300, 96);
    row.paint(p, viewport(300.0F, 96.0F), BuildContext{});
    // 可见窗口 1..4：第 1 项贴左缘、第 2 项紧随其后；第 0 项滚出窗口被回收（不再重绘，缓存条目保留）。
    AURORA_TEST_CHECK_NEAR(rec.items.at(1)->paint_bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(2)->paint_bounds().origin.x, 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(4)->paint_bounds().origin.x, 288.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 5U);  // 首帧 0..3，本帧新构建仅第 4 项

    row.set_scroll_offset(-10.0F);  // 负值夹到 0
    AURORA_TEST_CHECK_NEAR(row.scroll_offset(), 0.0F, 1e-4F);
    row.set_scroll_offset(1.0e6F);  // 超界夹到最大
    AURORA_TEST_CHECK_NEAR(row.scroll_offset(), row.max_scroll_offset(), 1e-3F);
}

AURORA_TEST_CASE(restore_key_restores_offset_and_writes_back) {
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    storage.write("row.k", 192.0F);  // 模拟上一次会话留下的位置

    BuildRecorder rec;
    LazyRow row{10, rec.builder(), 96.0F};
    row.set_restore_key("row.k");
    LayoutEngine::layout(row, bounded(300.0F, 96.0F));
    AURORA_TEST_CHECK_NEAR(row.scroll_offset(), 192.0F, 1e-4F);  // 首次可滚动布局即恢复

    // 用户滚动 → 写回注册表（供下次重建 / 跨进程恢复）。
    ScrollEvent e;
    e.delta_y = 1.0F;  // offset += 1 * 96 * 0.5 = 48
    row.on_scroll(e);
    AURORA_TEST_CHECK_NEAR(row.scroll_offset(), 240.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(storage.read("row.k").value_or(-1.0F), 240.0F, 1e-4F);

    // 序列化面：offset（表外运行键）与 restore_key（属性行）双向可见。
    Json props = Json::object();
    row.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "offset"), 240.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "restore_key"), std::string{"row.k"});
    storage.clear_all();
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
    // 属性块构造与位置构造走同一批行：`item_extent` 只有一枚存储，故往返不必再绕「私有副本 vs 聚合字段」。
    LazyRow const src{LazyRowProps().set_item_count(7).set_item_extent(48.0F).set_cache_extent(32.0F)};
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "item_count"), 7);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "item_extent"), 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "cache_extent"), 32.0F, 1e-4F);

    LazyRow dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.item_count(), 7);
    AURORA_TEST_CHECK_NEAR(dst.item_extent(), 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.cache_extent(), 32.0F, 1e-4F);
    // 反序列化结果参与布局：内容 7*48=336 > 视口 300 → 宽度钳到视口；行高 = 48。
    LayoutEngine::layout(dst, bounded(300.0F, 96.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.size().height, 48.0F, 1e-4F);
}

AURORA_TEST_CASE(unstated_rows_are_not_serialized) {
    // 「落盘的是意图不是现值」：未表态的行没有键；表外运行键 `offset` 与行无关，恒在场。
    LazyRow const bare;
    Json bare_props = Json::object();
    bare.serialize_props(bare_props);
    for (const std::string_view key : AURORA_TABLE_KEYS) {
        AURORA_TEST_CHECK_FALSE(bare_props.contains(key));
    }
    AURORA_TEST_CHECK_TRUE(bare_props.contains("offset"));

    // 只表态两行 ⇒ 盘上恰多这两枚键，其余三行仍缺席（生效值照旧由回落基线给出）。
    LazyRow partial{2, {}, 96.0F};
    partial.set_item_extent(48.0F);
    partial.set_padding(EdgeInsets{.left = 4.0F, .top = 4.0F, .right = 4.0F, .bottom = 4.0F});
    Json partial_props = Json::object();
    partial.serialize_props(partial_props);
    AURORA_TEST_CHECK_TRUE(partial_props.contains("item_count"));
    AURORA_TEST_CHECK_TRUE(partial_props.contains("item_extent"));
    AURORA_TEST_CHECK_TRUE(partial_props.contains("padding"));
    AURORA_TEST_CHECK_FALSE(partial_props.contains("cache_extent"));
    AURORA_TEST_CHECK_FALSE(partial_props.contains("restore_key"));
    AURORA_TEST_CHECK_NEAR(require_field<float>(partial_props, "item_extent"), 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(partial.cache_extent(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(clear_row_drops_key_and_restores_baseline) {
    // 逐行撤表态：该键退出序列化、生效值退回该行回落基线（不是 0，也不是「上一个值」）。
    LazyRowProbe d;
    d.set_item_count(3).set_item_extent(64.0F).set_cache_extent(30.0F).set_restore_key("row.k").set_padding(
        EdgeInsets{.left = 10.0F, .top = 5.0F, .right = 20.0F, .bottom = 5.0F});
    Json stated = Json::object();
    d.serialize_props(stated);
    for (const std::string_view key : AURORA_TABLE_KEYS) {
        AURORA_TEST_CHECK_TRUE(stated.contains(key));  // 五行全表态 ⇒ 五键逐键在盘
    }

    d.clear(LazyRowProps::item_count_row());
    AURORA_TEST_CHECK_FALSE(d.present(LazyRowProps::item_count_row()));
    AURORA_TEST_CHECK_EQ(d.item_count(), 0);

    d.clear(LazyRowProps::item_extent_row());
    AURORA_TEST_CHECK_FALSE(d.present(LazyRowProps::item_extent_row()));
    AURORA_TEST_CHECK_NEAR(d.item_extent(), 96.0F, 1e-4F);

    d.clear(LazyRowProps::cache_extent_row());
    AURORA_TEST_CHECK_NEAR(d.cache_extent(), 0.0F, 1e-4F);

    d.clear(LazyRowProps::restore_key_row());
    AURORA_TEST_CHECK_TRUE(d.restore_key().empty());

    d.clear(LazyRowProps::padding_row());
    AURORA_TEST_CHECK_NEAR(d.padding().left, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.padding().bottom, 0.0F, 1e-4F);

    Json cleared = Json::object();
    d.serialize_props(cleared);
    for (const std::string_view key : AURORA_TABLE_KEYS) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));  // 意图集合里再无一键
    }

    // 撤表态真的回到几何：行高 = 该行回落基线 96。
    LayoutEngine::layout(d, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(d.size().height, 96.0F, 1e-4F);
}

AURORA_TEST_CASE(invalid_json_degrades_per_row) {
    // 登记了 `validate` 的四行（int / float / float / EdgeInsets）：形态不符一律不写入、撤表态、
    // 生效值退回该行回落基线，且降级经 `Diagnostics` 上报。
    // `restore_key` 是 `std::string` 行，其登记没有 `validate` ⇒ 按 `from_json` 宽容通路（非字符串取空串）
    // 照常表态且不上报——与数组行同一口径，不是本轮新决定。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    LazyRowProbe d;
    d.set_item_count(4).set_item_extent(64.0F).set_cache_extent(20.0F).set_restore_key("row.keep");

    Json bad = Json::object();
    bad.set("item_count", Json{"four"});
    bad.set("item_extent", Json{"wide"});
    bad.set("cache_extent", Json{true});
    bad.set("padding", Json{3.0});
    d.deserialize_props(bad);

    AURORA_TEST_CHECK_FALSE(d.present(LazyRowProps::item_count_row()));
    AURORA_TEST_CHECK_EQ(d.item_count(), 0);
    AURORA_TEST_CHECK_FALSE(d.present(LazyRowProps::item_extent_row()));
    AURORA_TEST_CHECK_NEAR(d.item_extent(), 96.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(d.present(LazyRowProps::cache_extent_row()));
    AURORA_TEST_CHECK_NEAR(d.cache_extent(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(d.present(LazyRowProps::padding_row()));
    AURORA_TEST_CHECK_NEAR(d.padding().top, 0.0F, 1e-4F);

    // 字符串行：形态不符走宽容通路 ⇒ 值为空串但**仍表态**（盘上的空串是用户意图），且不新增上报。
    const std::size_t reported = Diagnostics::count();
    AURORA_TEST_CHECK_TRUE(reported >= 4U);  // 四行降级逐一上报（不是静默吞掉）

    Json lenient = Json::object();
    lenient.set("restore_key", Json{7});
    d.deserialize_props(lenient);
    AURORA_TEST_CHECK_TRUE(d.present(LazyRowProps::restore_key_row()));
    AURORA_TEST_CHECK_TRUE(d.restore_key().empty());
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);  // 宽容解析不报降级

    // 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("item_count", Json{9});
    good.set("item_extent", Json{24.0});
    good.set("cache_extent", Json{6.0});
    d.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(d.present(LazyRowProps::item_count_row()));
    AURORA_TEST_CHECK_EQ(d.item_count(), 9);
    AURORA_TEST_CHECK_NEAR(d.item_extent(), 24.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.cache_extent(), 6.0F, 1e-4F);

    Diagnostics::take();
}

AURORA_TEST_CASE(mounted_writers_mark_declared_tiers) {
    // 挂载后的写入按行声明的档位标脏：三枚双档行两档各落一次，`cache_extent` 只落绘制档，
    // `restore_key` 两档都不落（它只决定偏移往哪个键写回）。
    LazyRow row;
    const BuildContext ctx;
    row.mount(ctx);
    LazyRowDirty dirty;
    dirty.attach_to(row);

    row.set_item_extent(64.0F);  // 布局档 + 绘制档
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, 1);

    const int layouts_after_extent = dirty.layouts;
    const int paints_after_extent = dirty.paints;
    row.set_cache_extent(120.0F);  // 只改 on_paint 的窗口边界 ⇒ 纯绘制档
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_extent);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_extent + 1);

    const int layouts_after_cache = dirty.layouts;
    const int paints_after_cache = dirty.paints;
    row.set_restore_key("row.tier");  // 无档行：不改几何也不改像素
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_cache);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_cache);

    row.set_padding(EdgeInsets{.left = 2.0F, .top = 2.0F, .right = 2.0F, .bottom = 2.0F});  // 双档行
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_cache + 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_cache + 1);  // 双档 = 布局一笔 + 绘制一笔

    const int layouts_after_padding = dirty.layouts;
    const int paints_after_padding = dirty.paints;
    row.set_item_count(8);  // 控件面同名覆写转调属性面那件：档位仍只由该行给出，不多落一笔
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_padding + 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_padding + 1);

    const int layouts_before = dirty.layouts;
    const int paints_before = dirty.paints;
    row.set_item_count(8);  // 同值再写：判等短路挡住，不落账
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_before);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before);
    row.set_item_extent(64.0F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_before);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before);
}

AURORA_TEST_CASE(constant_rows_readers_equal_library_resolve) {
    // 五行皆常量档 ⇒ 没有 `x(ctx)` 第二件；无参读件即生效值，与库内 `resolve(row, ctx)` 取值全同
    // （缺这组断言则「窄面不丢语义」只是断言）。
    const BuildContext ctx;
    LazyRowProbe d;
    d.set_item_count(6).set_item_extent(37.5F).set_cache_extent(12.0F).set_restore_key("row.resolve");
    d.set_padding(EdgeInsets{.left = 1.0F, .top = 2.0F, .right = 3.0F, .bottom = 4.0F});

    AURORA_TEST_CHECK_EQ(d.item_count(), d.resolve(LazyRowProps::item_count_row(), ctx));
    AURORA_TEST_CHECK_NEAR(d.item_extent(), d.resolve(LazyRowProps::item_extent_row(), ctx), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.cache_extent(), d.resolve(LazyRowProps::cache_extent_row(), ctx), 1e-6F);
    AURORA_TEST_CHECK_EQ(d.restore_key(), d.resolve(LazyRowProps::restore_key_row(), ctx));
    AURORA_TEST_CHECK_NEAR(d.padding().left, d.resolve(LazyRowProps::padding_row(), ctx).left, 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.padding().bottom, d.resolve(LazyRowProps::padding_row(), ctx).bottom, 1e-6F);

    // 未表态的行同样解析到该行回落基线（读件与解析同源，不是读件另算一遍）。
    LazyRowProbe const bare;
    AURORA_TEST_CHECK_NEAR(bare.item_extent(), bare.resolve(LazyRowProps::item_extent_row(), ctx), 1e-6F);
    AURORA_TEST_CHECK_NEAR(bare.item_extent(), 96.0F, 1e-6F);
}

AURORA_TEST_CASE(offset_is_table_external) {
    // 运行时偏移：序列化面有键、属性面没有写件（词根规则推不出也不该推出 `set_offset`），
    // 唯一写入通道是控件面的 `set_scroll_offset()`；反序列化把它记进 pending，首次可滚动布局才落位。
    AURORA_TEST_CHECK_FALSE(has_offset_writer<LazyRowProps>);

    LazyRow src{10, {}, 96.0F};
    LayoutEngine::layout(src, bounded(300.0F, 96.0F));
    src.set_scroll_offset(120.0F);
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "offset"), 120.0F, 1e-4F);

    LazyRow dst{10, {}, 96.0F};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.scroll_offset(), 0.0F, 1e-4F);  // 内容/视口未定 ⇒ 不提前夹取
    LayoutEngine::layout(dst, bounded(300.0F, 96.0F));
    AURORA_TEST_CHECK_NEAR(dst.scroll_offset(), 120.0F, 1e-4F);  // 首次可滚动布局落位

    // 往返后的偏移与行值互不相干：撤掉 `item_extent` 的表态带不走 `offset`，反之亦然。
    // （位置构造 `LazyRow{10, {}, 96.0F}` 把 `item_count` / `item_extent` 两行装了进去，故先撤该行
    // 才能看到「键退盘而 `offset` 仍在」；写入即表态，位置件与链式写件同口径。）
    dst.clear(LazyRowProps::item_extent_row());
    Json bare_key = Json::object();
    dst.serialize_props(bare_key);
    AURORA_TEST_CHECK_TRUE(bare_key.contains("offset"));
    AURORA_TEST_CHECK_FALSE(bare_key.contains("item_extent"));
    AURORA_TEST_CHECK_NEAR(dst.scroll_offset(), 120.0F, 1e-4F);  // 撤行不动运行时偏移
}

}  // namespace aurora::test_cases::utest_lazy_row
