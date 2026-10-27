/// 测试类型: unit
/// 目标单元: include/aurora/widget/lazy_list.h
/// 测试说明: 虚拟列表三条运行期入口的口径。条目刷新：只置位，
/// 回收排在下一帧布局（调用栈内不建树、不销毁），且只动内容不动位置；
/// 与 `set_count` 的「动总数、范围内存活条目保值」分工对账。
/// 末尾锚定：布局前调用是意图而非无效调用，由首次可滚动布局按当时的
/// 真实范围夹取（不是内容高），走 `pending_offset_` 故优先于 `restore_key`、
/// 且不落进序列化属性；布局后等价 `scroll_to(max_scroll_offset(), animate)`。
/// 偏移变更通知：来源不限（滚轮、滑动帧、程序化、无障碍、拖入滚边、待定偏移）
/// 各恰一次，同值与被夹取到同值不通知，置空即注销，回调参数与 `scroll_offset()`
/// 同源。三枚入口都不新增序列化属性键。全部走无头布局，不依赖真实桌面会话。

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/accessibility.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/lazy_list.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_lazy_list_runtime_entries {

using aurora::testing::require_field;

namespace {

/// 条目哑控件：把构建当时的数据文本刻进实例，刷新是否吃到新内容读它即知。
class MarkedBox final : public Widget {
  public:
    explicit MarkedBox(std::string label) : label_(std::move(label)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "MarkedBox"; }

    /// @brief 构建期捕获的数据文本。
    /// @return 该实例诞生时对应下标的数据文本。
    [[nodiscard]] auto label() const -> const std::string & { return label_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 300.0F, .height = 48.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    std::string label_;
};

/// 数据台账：数据源 + 每次构建留下的实例与次序（实例被回收后仍可读其内容）。
struct DataLedger {
    std::vector<std::string> labels;  ///< 数据源（刷新用例改的就是它）
    std::map<int, std::shared_ptr<MarkedBox>> latest;  ///< 下标 -> 最近一次构建的实例
    std::vector<int> built_order;

    auto builder() -> LazyList::ItemBuilder {
        return [this](int index) -> Node {
            built_order.push_back(index);
            auto box = std::make_shared<MarkedBox>(labels.at(static_cast<std::size_t>(index)));
            latest[index] = box;
            return Node{std::move(box)};
        };
    }

    /// @brief 改写全部数据文本（不动 builder、不动 count）。
    /// @param text 新文本。
    auto relabel_all(const std::string &text) -> void {
        for (auto &slot : labels) {
            slot = text;
        }
    }
};

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 假想帧钟：单调递增，令收位滑动与拖入滚边逐帧可测（不依赖墙钟抖动）。
auto frame_clock() -> std::chrono::steady_clock::time_point & {
    static std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{};
    return now;
}

/// @brief 推进 n 帧（每帧 16ms），驱动收位滑动 / 拖入滚边的帧计时。
auto pump(LazyList &list, int frames) -> void {
    for (int i = 0; i < frames; ++i) {
        frame_clock() += std::chrono::milliseconds(16);
        list.tick(frame_clock());
    }
}

/// @brief 等滑动走完：滑满时长 150ms + 余量。
auto settle(LazyList &list) -> void { pump(list, 16); }

/// @brief 滚轮事件入口（步进为控件内部常量 40，故 1 单位 = 40dp；负方向 = 向下滚）。
auto wheel(LazyList &list, float delta_y) -> void {
    ScrollEvent e;
    e.delta_y = delta_y;
    list.on_scroll(e);
}

/// 偏移通知台账：逐次记下回调参数，供「每来源恰一次 / 同值不通知」对账。
struct NotifyLog {
    std::vector<float> got;

    /// @brief 已通知次数。
    /// @return 回调被调用的次数。
    [[nodiscard]] auto size() const -> std::size_t { return got.size(); }

    /// @brief 最近一次通知的参数（空台账回不可达哨兵值而非 UB）。
    /// @return 最后一次收到的偏移；无通知时为 -1.0F。
    [[nodiscard]] auto last() const -> float { return got.empty() ? -1.0F : got.back(); }
};

}  // namespace

AURORA_TEST_CASE(invalidate_items_takes_effect_on_next_layout_not_in_call_stack) {
    // 判据：刷新只置位。调用栈内一条树都不建、一个实例都不销毁；下一帧布局才吃到新内容。
    DataLedger rec;
    rec.labels.assign(100, "old");
    LazyList list{100, rec.builder(), 48.0F};
    list.set_cache_extent(0.0F);  // 关预取：窗口即视口覆盖的行
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    const std::size_t window = list.live_item_count();
    AURORA_TEST_REQUIRE_EQ(window, 9U);  // ceil(400 / 48)

    // 先把视口停在非零偏移，刷新后位置须逐位不变。
    list.set_scroll_offset(240.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    const auto range_before = list.visible_range();
    const std::size_t builds_before = rec.built_order.size();
    AURORA_TEST_REQUIRE_EQ(range_before.first, 5);
    auto first_instance = rec.latest.at(range_before.first);
    AURORA_TEST_CHECK_EQ(first_instance->label(), std::string{"old"});

    rec.relabel_all("new");
    list.invalidate_items();

    // 调用栈内：无建树、无回收、内容仍是旧值，位置与范围也没动。
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), builds_before);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), window);
    AURORA_TEST_CHECK_EQ(rec.latest.at(range_before.first)->label(), std::string{"old"});
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 240.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(list.count(), 100);

    // 下一帧布局：同一窗口全部重建为新内容，实例确实换了，位置与范围仍逐位不动。
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(rec.latest.at(range_before.first)->label(), std::string{"new"});
    AURORA_TEST_CHECK_TRUE(rec.latest.at(range_before.first).get() != first_instance.get());
    AURORA_TEST_CHECK_EQ(first_instance->label(), std::string{"old"});  // 旧实例内容从未被改写
    AURORA_TEST_CHECK_EQ(list.live_item_count(), window);  // 仍是窗口量级，不是整表
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 240.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(list.visible_range().first, range_before.first);
    AURORA_TEST_CHECK_EQ(list.visible_range().second, range_before.second);
    AURORA_TEST_CHECK_NEAR(list.max_scroll_offset(), 4400.0F, 1e-3F);  // 内容高没变（4800 - 400）
}

AURORA_TEST_CASE(set_count_keeps_instances_while_invalidate_items_rebuilds_them) {
    // 分工边界：那一枚动总数（范围内条目保值），本枚动内容（全部重建）。
    DataLedger rec;
    rec.labels.assign(100, "a");
    LazyList list{60, rec.builder(), 48.0F};
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    auto index_zero = rec.latest.at(0);
    const std::size_t builds = rec.built_order.size();

    list.set_count(100);  // 增长：仍在范围内的存活条目不得重建
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), builds);
    AURORA_TEST_CHECK_TRUE(rec.latest.at(0).get() == index_zero.get());

    list.invalidate_items();  // 刷新：窗口内全部重建
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_GT(rec.built_order.size(), builds);
    AURORA_TEST_CHECK_TRUE(rec.latest.at(0).get() != index_zero.get());
    AURORA_TEST_CHECK_EQ(list.count(), 100);  // 刷新不改总数
}

AURORA_TEST_CASE(scroll_to_end_before_layout_lands_on_the_real_end) {
    // 布局前「末尾」是意图：不能按当时的 max_scroll_offset()（= 整个内容高）落位，
    // 也不能被当成无效调用丢弃，由首次可滚动布局夹到那一时刻的真实末尾。
    DataLedger rec;
    rec.labels.assign(100, "a");
    LazyList list{100, rec.builder(), 48.0F};
    NotifyLog log;
    list.set_on_scroll_changed([&log](float offset) -> void { log.got.push_back(offset); });
    list.scroll_to_end();
    AURORA_TEST_CHECK_EQ(log.size(), 0U);  // 记意图不产生落位

    // 意图不得混进序列化状态：那是「借序列化面表达运行期意图」的代偿形态。
    Json props = Json::object();
    list.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("scroll_offset"));
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "scroll_offset"), 0.0F, 1e-4F);

    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-3F);  // 4800 - 400，不是内容高 4800
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), list.max_scroll_offset(), 1e-4F);
    AURORA_TEST_CHECK_EQ(log.size(), 1U);  // 待定偏移落地经落位路径通知恰一次
    AURORA_TEST_CHECK_NEAR(log.last(), 4400.0F, 1e-3F);
}

AURORA_TEST_CASE(scroll_to_end_on_unscrollable_content_is_a_no_op) {
    // 空表 / 内容不足一屏：夹取结果为 0，不产生伪变化，也不通知。
    NotifyLog log;
    LazyList list;
    list.set_on_scroll_changed([&log](float offset) -> void { log.got.push_back(offset); });
    list.scroll_to_end();
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(log.size(), 0U);

    list.scroll_to_end();  // 已布局且不可滚：同样零变化
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(log.size(), 0U);

    LazyList short_content{3, {}, 48.0F};  // 144dp 内容 < 400dp 视口
    short_content.set_on_scroll_changed([&log](float offset) -> void { log.got.push_back(offset); });
    short_content.scroll_to_end();
    LayoutEngine::layout(short_content, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(short_content.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(log.size(), 0U);
}

AURORA_TEST_CASE(scroll_to_end_after_layout_moves_now_and_notifies_once) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    NotifyLog log;
    list.set_on_scroll_changed([&log](float offset) -> void { log.got.push_back(offset); });

    list.scroll_to_end();
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-3F);
    AURORA_TEST_CHECK_FALSE(list.is_gliding());  // 默认立即就位
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
    AURORA_TEST_CHECK_NEAR(log.last(), list.scroll_offset(), 1e-4F);

    list.scroll_to_end();  // 已到底：同值不通知
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
}

AURORA_TEST_CASE(scroll_to_end_animated_glides_frame_by_frame) {
    // animate=true 沿用 scroll_to 的既有滑动时序：逐帧推进、每帧一次通知、末次即最终落位。
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    list.set_scroll_offset(400.0F);
    NotifyLog log;
    list.set_on_scroll_changed([&log](float offset) -> void { log.got.push_back(offset); });

    list.scroll_to_end(/*animate=*/true);
    AURORA_TEST_CHECK_TRUE(list.is_gliding());
    AURORA_TEST_CHECK_EQ(log.size(), 0U);  // 起滑不通知，只有实际落位才通知

    pump(list, 1);
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
    AURORA_TEST_CHECK_GT(list.scroll_offset(), 400.0F);
    AURORA_TEST_CHECK_LT(list.scroll_offset(), 4400.0F);

    settle(list);
    AURORA_TEST_CHECK_FALSE(list.is_gliding());
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-3F);
    AURORA_TEST_CHECK_GT(log.size(), 2U);
    AURORA_TEST_CHECK_NEAR(log.last(), 4400.0F, 1e-3F);
}

AURORA_TEST_CASE(scroll_to_end_wins_over_restore_key) {
    // 与 restore_key 同时存在时取显式意图——沿用既有优先级（显式 > 按键），不另造时序。
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    storage.write("runtime.tail", 900.0F);

    DataLedger rec;
    rec.labels.assign(100, "a");
    LazyList list{100, rec.builder(), 48.0F};
    list.set_restore_key("runtime.tail");
    list.scroll_to_end();
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-3F);  // 而非 stored 的 900

    // 只有关保存键（无末尾意图）时按键恢复照常生效——反证上一步取的是显式意图。
    LazyList restored{100, rec.builder(), 48.0F};
    restored.set_restore_key("runtime.tail");
    LayoutEngine::layout(restored, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(restored.scroll_offset(), 900.0F, 1e-3F);
    storage.clear_all();
}

AURORA_TEST_CASE(scroll_changed_fires_once_per_offset_source) {
    // 来源不限：滚轮、程序化、无障碍定位、读屏动作、拖入滚边各驱动恰一次。
    LazyList list{100, {}, 48.0F};
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    NotifyLog log;
    list.set_on_scroll_changed([&log](float offset) -> void { log.got.push_back(offset); });

    wheel(list, -4.0F);  // 向下 4 单位 × 40dp
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 160.0F, 1e-4F);
    log.got.clear();

    list.set_scroll_offset(300.0F);
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
    log.got.clear();

    list.accessibility_scroll_to(500.0);  // 读屏协议按 double 给值
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
    AURORA_TEST_CHECK_NEAR(log.last(), 500.0F, 1e-3F);
    log.got.clear();

    const AccessibilityActionRequest down{.action = AccessibilityAction::ScrollDown};
    AURORA_TEST_CHECK_TRUE(list.perform_accessibility_action(down));  // 一屏增量
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 900.0F, 1e-3F);  // 500 + 视口高 400
    log.got.clear();

    // 拖入自动滚边：底带内落点（视口高 400、触发带 48）由帧计时推进。
    AURORA_TEST_CHECK_TRUE(list.on_system_drag_auto_scroll(Point{.x = 150.0F, .y = 390.0F}));
    pump(list, 1);
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
    AURORA_TEST_CHECK_GT(list.scroll_offset(), 900.0F);
    log.got.clear();

    AURORA_TEST_CHECK_TRUE(list.on_system_drag_auto_scroll(std::nullopt));  // 收车
    pump(list, 2);
    AURORA_TEST_CHECK_EQ(log.size(), 0U);
}

AURORA_TEST_CASE(drag_edge_scroll_advances_without_glide_history) {
    // 回归：滚边在途时不得借用滑动块。未激活的 `ScrollGlide::tick` 返回的是上一目标
    // （从未滑过即 0），曾把刚推进的偏移拽回去——拖到列表边缘反而跳回开头。
    LazyList list{100, {}, 48.0F};
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    list.set_scroll_offset(900.0F);
    NotifyLog log;
    list.set_on_scroll_changed([&log](float offset) -> void { log.got.push_back(offset); });
    log.got.clear();

    AURORA_TEST_CHECK_TRUE(list.on_system_drag_auto_scroll(Point{.x = 150.0F, .y = 390.0F}));
    float previous = list.scroll_offset();
    for (int frame = 0; frame < 4; ++frame) {
        pump(list, 1);
        AURORA_TEST_CHECK_GT(list.scroll_offset(), previous);  // 只向前，不被拽回旧值
        previous = list.scroll_offset();
    }
    AURORA_TEST_CHECK_EQ(log.size(), 4U);  // 每帧恰一次通知
    AURORA_TEST_CHECK_FALSE(list.is_gliding());  // 滚边不占用滑动态

    // 收位滑动走完后再滚边：滑动留下的终点不得成为滚边的拽回目标（两态互不抢占）。
    AURORA_TEST_CHECK_TRUE(list.on_system_drag_auto_scroll(std::nullopt));
    list.scroll_to(1200.0F, /*animate=*/true);
    settle(list);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 1200.0F, 1e-3F);
    log.got.clear();
    AURORA_TEST_CHECK_TRUE(list.on_system_drag_auto_scroll(Point{.x = 150.0F, .y = 390.0F}));
    pump(list, 2);
    AURORA_TEST_CHECK_GT(list.scroll_offset(), 1200.0F);
    AURORA_TEST_CHECK_GT(log.size(), 0U);
}

AURORA_TEST_CASE(scroll_changed_skips_values_that_do_not_move) {
    // 去重来自两条落位路径的「夹取后同值即返回」：同值 set、被夹到同值都不通知。
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    NotifyLog log;
    list.set_on_scroll_changed([&log](float offset) -> void { log.got.push_back(offset); });

    list.set_scroll_offset(200.0F);
    AURORA_TEST_CHECK_EQ(log.size(), 1U);
    list.set_scroll_offset(200.0F);
    AURORA_TEST_CHECK_EQ(log.size(), 1U);  // 同值
    list.set_scroll_offset(-10.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(log.size(), 2U);  // 真落位（夹到 0）才通知
    list.set_scroll_offset(-1.0F);
    AURORA_TEST_CHECK_EQ(log.size(), 2U);  // 夹取后仍是 0：不通知
    list.set_scroll_offset(99999.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-3F);
    AURORA_TEST_CHECK_EQ(log.size(), 3U);
    list.scroll_to_end();
    AURORA_TEST_CHECK_EQ(log.size(), 3U);  // 已到底再请求末尾：不通知

    list.set_on_scroll_changed({});  // 置空即注销
    list.set_scroll_offset(0.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(log.size(), 3U);
}

AURORA_TEST_CASE(scroll_changed_parameter_matches_the_readback) {
    // 回调在偏移与脏标记都落定之后调用：回调内读 scroll_offset() 等于刚送出的那个值。
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    LazyList *self = &list;
    bool consistent = true;
    int calls = 0;
    list.set_on_scroll_changed([self, &consistent, &calls](float offset) -> void {
        ++calls;
        if (self->scroll_offset() != offset) {
            consistent = false;
        }
    });

    list.set_scroll_offset(600.0F);
    AURORA_TEST_CHECK_EQ(calls, 1);
    wheel(list, -2.0F);
    AURORA_TEST_CHECK_GT(calls, 1);
    list.scroll_to_end(/*animate=*/true);
    settle(list);
    AURORA_TEST_CHECK_GT(calls, 2);  // 滑动帧也各通知一次
    AURORA_TEST_CHECK_TRUE(consistent);
}

AURORA_TEST_CASE(runtime_entries_add_no_serialized_property_keys) {
    // 本批只加成员方法与私有位：三枚入口都不许把运行期意图写成序列化属性。
    LazyList list{100, {}, 48.0F};
    list.set_restore_key("runtime.keys");
    Json before = Json::object();
    list.serialize_props(before);

    list.invalidate_items();
    list.scroll_to_end();
    list.set_on_scroll_changed([](float /*offset*/) -> void {});

    Json after = Json::object();
    list.serialize_props(after);
    for (const char *key : {"count", "item_extent", "scroll_offset", "cache_extent", "restore_key", "snap_extent",
                            "snap_paging", "snap_alignment"}) {
        AURORA_TEST_CHECK_TRUE(before.contains(key));
        AURORA_TEST_CHECK_TRUE(after.contains(key));
        AURORA_TEST_CHECK_TRUE(*before.at(key) == *after.at(key));
    }
    // 刷新意图与末尾意图都不落成键（既无 invalidate 键，也无「末尾偏移」键）。
    AURORA_TEST_CHECK_FALSE(after.contains("invalidate_items"));
    AURORA_TEST_CHECK_FALSE(after.contains("scroll_to_end"));
    AURORA_TEST_CHECK_NEAR(require_field<float>(after, "scroll_offset"), 0.0F, 1e-4F);
    ScrollStorage::instance().clear_all();
}

}  // namespace aurora::test_cases::utest_lazy_list_runtime_entries
