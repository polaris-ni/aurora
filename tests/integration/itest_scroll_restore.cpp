/// 测试类型: integration
/// 目标单元: include/aurora/app/scroll_storage.h
/// 测试说明: 端到端验收滚动位置保存/恢复跨「控件生命周期」（会话内销毁重建同键控件恢复）与
/// 跨「进程生命周期」（attach + sync 落盘后，清空内存、仅凭 Preferences 懒回读恢复）、
/// 四个滚动控件（Scroll / LazyList / LazyRow / GridView）的一致行为、用户主动滚动优先于恢复、
/// 以及多窗口作用域隔离（同名 key 不串味）
/// 覆盖说明: ai_compat fixture 的 schema 无 destroy/rebuild 动作，故本用例自写 C++；
/// 用 LayoutEngine + Painter 驱动真实布局/绘制路径（不依赖 Application 帧循环，确定性可复现）

#include <memory>
#include <string>
#include <utility>

#include "aurora/app/scroll_storage.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/preferences/preferences.h"
#include "aurora/render/painter.h"
#include "aurora/widget/grid_view.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/lazy_row.h"
#include "aurora/widget/scroll.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_scroll_restore {

using preferences::Preferences;

namespace {

constexpr float AURORA_VIEW_H = 200.0F;

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 固定尺寸哑控件（虚拟列表条目与 Scroll 内容都用它，避免依赖文本/字体）。
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

/// 虚拟列表条目构造器（同一实例可被多个控件复用，故每次新建）。
auto make_item_builder() -> LazyList::ItemBuilder {
    return [](int /*index*/) -> Node { return Node{std::make_shared<FixedBox>(80.0F, 40.0F)}; };
}

/// 用例隔离：注册表是进程级单例，起手与结尾都要清空（含持久化绑定与作用域）。
auto reset_storage() -> ScrollStorage & {
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    return storage;
}

}  // namespace

AURORA_TEST_CASE(session_rebuild_restores_scroll_position) {
    // 会话内「销毁重建」：旧实例销毁后，同 key 的新实例在首次可滚动布局即恢复位置。
    // 这正是标签页切换 / 详情页返回 / 热重建的语义。
    auto &storage = reset_storage();
    {
        LazyList feed{100, make_item_builder(), 40.0F};
        feed.set_restore_key("feed");
        LayoutEngine::layout(feed, bounded(200.0F, AURORA_VIEW_H));
        AURORA_TEST_CHECK_NEAR(feed.scroll_offset(), 0.0F, 1e-4F);
        feed.set_scroll_offset(240.0F);
        // 变化即写回注册表（仅内存；落盘由 App 决定）。
        AURORA_TEST_CHECK_NEAR(storage.read("feed").value_or(-1.0F), 240.0F, 1e-4F);
    }  // 旧实例在此销毁

    LazyList rebuilt{100, make_item_builder(), 40.0F};
    rebuilt.set_restore_key("feed");
    LayoutEngine::layout(rebuilt, bounded(200.0F, AURORA_VIEW_H));
    AURORA_TEST_CHECK_NEAR(rebuilt.scroll_offset(), 240.0F, 1e-4F);

    // 可见窗口随恢复后的偏移生成：第 240/40 = 6 项必须落在窗口内。
    const auto [first, last] = rebuilt.visible_range();
    AURORA_TEST_CHECK_LT(first, 6);
    AURORA_TEST_CHECK_GT(last, 6);

    // 无 restore_key 的实例不参与恢复，也不污染注册表。
    LazyList plain{100, make_item_builder(), 40.0F};
    LayoutEngine::layout(plain, bounded(200.0F, AURORA_VIEW_H));
    AURORA_TEST_CHECK_NEAR(plain.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(storage.read("feed").value_or(-1.0F), 240.0F, 1e-4F);
    reset_storage();
}

AURORA_TEST_CASE(restart_restores_from_persisted_preferences) {
    // 跨进程重启：会话末 sync 落盘（Preferences 内存 + flush），新「进程」清空内存后
    // 仅凭持久化后端懒回读恢复。
    auto &storage = reset_storage();
    Preferences prefs;  // 仅内存模式：充当同一份配置文件
    storage.attach(prefs);
    {
        LazyList feed{100, make_item_builder(), 40.0F};
        feed.set_restore_key("feed");
        LayoutEngine::layout(feed, bounded(200.0F, AURORA_VIEW_H));
        feed.set_scroll_offset(320.0F);
    }
    storage.sync();  // 批量写穿
    AURORA_TEST_CHECK_NEAR(prefs.group(ScrollStorage::AURORA_GROUP_NAME).get<float>("feed", -1.0F), 320.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(storage.pending_writes(), std::size_t{0});

    // 重启：进程内状态全清（内存值 / 待落盘 / 作用域 / 认领记录），只剩持久化后端。
    storage.clear_all();
    AURORA_TEST_CHECK_FALSE(storage.read("feed").has_value());
    storage.attach(prefs);

    LazyList reopened{100, make_item_builder(), 40.0F};
    reopened.set_restore_key("feed");
    LayoutEngine::layout(reopened, bounded(200.0F, AURORA_VIEW_H));
    AURORA_TEST_CHECK_NEAR(reopened.scroll_offset(), 320.0F, 1e-4F);  // 懒回读恢复
    reset_storage();
}

AURORA_TEST_CASE(all_scroll_controls_restore_their_key) {
    // 四个滚动控件走同一契约（各自 key 独立、互不干扰）。
    auto &storage = reset_storage();
    storage.write("k.scroll", 100.0F);
    storage.write("k.list", 80.0F);
    storage.write("k.row", 96.0F);
    storage.write("k.grid", 96.0F);

    Scroll scroll;
    scroll.restore_key = "k.scroll";
    scroll.add(Node{std::make_shared<FixedBox>(100.0F, 400.0F)});
    LayoutEngine::layout(scroll, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(scroll.offset_y(), 100.0F, 1e-4F);

    LazyList list{50, make_item_builder(), 40.0F};
    list.set_restore_key("k.list");
    LayoutEngine::layout(list, bounded(200.0F, AURORA_VIEW_H));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 80.0F, 1e-4F);

    LazyRow row{50, make_item_builder(), 96.0F};
    row.restore_key = "k.row";
    LayoutEngine::layout(row, bounded(200.0F, 96.0F));
    AURORA_TEST_CHECK_NEAR(row.scroll_offset(), 96.0F, 1e-4F);

    GridView grid{200, 2, make_item_builder(), 96.0F};
    grid.set_restore_key("k.grid");
    LayoutEngine::layout(grid, bounded(200.0F, AURORA_VIEW_H));
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 96.0F, 1e-4F);

    // 越界值被各自的可滚动范围夹取（不产生越界偏移）。
    storage.write("k.row", 1.0e6F);
    LazyRow clamped{50, make_item_builder(), 96.0F};
    clamped.restore_key = "k.row";
    LayoutEngine::layout(clamped, bounded(200.0F, 96.0F));
    AURORA_TEST_CHECK_NEAR(clamped.scroll_offset(), clamped.max_scroll_offset(), 1e-3F);
    reset_storage();
}

AURORA_TEST_CASE(user_scroll_wins_over_restore) {
    // 恢复只在「首次可滚动布局」生效一次；此后用户滚动不得被恢复值回拉。
    auto &storage = reset_storage();
    storage.write("k", 400.0F);

    LazyList list{100, make_item_builder(), 40.0F};
    list.set_restore_key("k");
    LayoutEngine::layout(list, bounded(200.0F, AURORA_VIEW_H));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 400.0F, 1e-4F);

    list.set_scroll_offset(120.0F);  // 用户滚动（等价于滚轮/拖拽后的位置）
    AURORA_TEST_CHECK_NEAR(storage.read("k").value_or(-1.0F), 120.0F, 1e-4F);

    LayoutEngine::layout(list, bounded(200.0F, AURORA_VIEW_H));  // 再次布局（模拟后续帧）
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 120.0F, 1e-4F);
    reset_storage();
}

AURORA_TEST_CASE(scope_isolates_same_key_across_windows) {
    // 多窗口隔离：同一 restore_key 在不同作用域下互不可见（宿主每帧设置本窗口作用域）。
    auto &storage = reset_storage();
    {
        const ScrollStorage::Scope win_a{"win-a"};
        LazyList feed{100, make_item_builder(), 40.0F};
        feed.set_restore_key("feed");
        LayoutEngine::layout(feed, bounded(200.0F, AURORA_VIEW_H));
        feed.set_scroll_offset(200.0F);
    }
    {
        const ScrollStorage::Scope win_b{"win-b"};
        LazyList feed{100, make_item_builder(), 40.0F};
        feed.set_restore_key("feed");
        LayoutEngine::layout(feed, bounded(200.0F, AURORA_VIEW_H));
        AURORA_TEST_CHECK_NEAR(feed.scroll_offset(), 0.0F, 1e-4F);  // 看不到窗口 A 的位置
    }
    {
        const ScrollStorage::Scope win_a{"win-a"};
        LazyList feed{100, make_item_builder(), 40.0F};
        feed.set_restore_key("feed");
        LayoutEngine::layout(feed, bounded(200.0F, AURORA_VIEW_H));
        AURORA_TEST_CHECK_NEAR(feed.scroll_offset(), 200.0F, 1e-4F);  // 回到 A 的作用域即恢复
    }
    reset_storage();
}

}  // namespace aurora::test_cases::itest_scroll_restore
