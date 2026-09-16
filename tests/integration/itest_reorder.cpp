/// 测试类型: integration
/// 目标单元: include/aurora/widget/reorderable_list.h
/// 测试说明: 端到端验收拖拽重排——TestController 合成真实指针序列（Press→Move→Release）经命中
///           测试、指针捕获与冒泡派发到列表；落位后数据顺序改写、结构变化无障碍事件上报；
///           以及「重排」与「滚动位置保存」两个子系统共存（重排不改滚动位置、位置照常写回）
/// 覆盖说明: ai_compat fixture 的 schema 表达不了「拖拽后断言顺序」（顺序不在 props 里，且
///           `expect` 只能比对属性），故自写 C++ 集成用例

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#ifdef AURORA_BACKEND_HEADLESS
// TestController 的实现体同样以 AURORA_BACKEND_HEADLESS 门控（依赖 HeadlessSurface），
// 故宏关闭时不得引入声明，否则链接期缺符号。
#include "aurora/app/test_controller.h"
#endif

#include "aurora/app/scroll_storage.h"
#include "aurora/aurora.h"
#include "aurora/render/painter.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_reorder {

#ifdef AURORA_BACKEND_HEADLESS

namespace {

/// 列表条目：固定尺寸的纯展示叶控件（不吃指针事件 ⇒ 整项按下即起拖）。
class ReorderItem final : public Widget {
  public:
    ReorderItem(float h) : h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ReorderItem"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 240.0F, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_hit_test(const Point & /*local*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/)
        -> Widget * override {
        return this;  // 可命中：`TestController::drag` 以目标中心为按下点
    }

  private:
    float h_;
};

auto make_builder() -> ReorderableList<int>::ItemBuilder {
    return [](const int & /*value*/, int /*index*/) -> Node { return Node{std::make_shared<ReorderItem>(40.0F)}; };
}

auto make_items(std::size_t n) -> std::shared_ptr<State<std::vector<int>>> {
    std::vector<int> values(n);
    for (std::size_t i = 0; i < n; ++i) {
        values[i] = static_cast<int>(i);
    }
    return std::make_shared<State<std::vector<int>>>(std::move(values));
}

/// 以 16ms 步进推进落位动画（`tc.pump` 未必逐帧驱动手势 tick——测试里显式驱动，确定性可复现）。
auto settle(ReorderableList<int> &list, int max_ticks = 200) -> void {
    auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < max_ticks && list.is_settling(); ++i) {
        now += std::chrono::milliseconds(16);
        list.tick(now);
    }
}

/// 结构变化事件计数器（无障碍通道）。
struct StructureEventCounter {
    int count = 0;

    auto install() -> void {
        set_accessibility_event_handler([this](const AccessibilityEvent &e) -> void {
            if (e.kind == AccessibilityEventKind::StructureChanged) {
                ++count;
            }
        });
    }
    static auto uninstall() -> void { set_accessibility_event_handler({}); }
};

}  // namespace

AURORA_TEST_CASE(drag_reorders_items_through_real_dispatch) {
    auto items = make_items(3);
    auto list = std::make_shared<ReorderableList<int>>(items, make_builder());
    list->set_drag_slop(4.0);
    int reorder_calls = 0;
    list->set_on_reorder([&reorder_calls](int /*from*/, int /*to*/) -> void { ++reorder_calls; });

    StructureEventCounter events;
    events.install();

    // 视口恰为 3 项高（120）：列表占满窗口，`simulate_drag` 的按下点（目标中心 y=60）
    // 确定性落在第 1 项（40..80），且内容不可滚（不掺入 auto-scroll）。
    TestControllerConfig cfg{};
    cfg.width = 320.0F;
    cfg.height = 120.0F;
    TestController tc{Node{list}, cfg};
    AURORA_TEST_REQUIRE(tc.pump().ok());
    AURORA_TEST_REQUIRE(tc.pump().ok());

    const std::vector<Node> found = tc.find_by_type("ReorderItem");
    AURORA_TEST_REQUIRE_EQ(found.size(), 3U);

    // 拖第 1 项下移 60dp：中心 40+20+60=120 越过第 2 项中点（60）→ 落到末位。
    const Result<void> drag = tc.drag(*list, Point{.x = 0.0F, .y = 60.0F});
    AURORA_TEST_CHECK_MSG(drag.ok(), drag.ok() ? "drag ok" : drag.error().message);
    AURORA_TEST_CHECK_FALSE(list->is_dragging());
    settle(*list);
    AURORA_TEST_CHECK_FALSE(list->is_settling());
    AURORA_TEST_REQUIRE(tc.pump().ok());  // 提交只标布局脏：一帧后落到新位置

    AURORA_TEST_CHECK_EQ(reorder_calls, 1);
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{0, 2, 1}));  // 第 1 项落到末位
    AURORA_TEST_CHECK_GT(events.count, 0);  // 结构变化已上报（读屏可感知）
    events.uninstall();
}

AURORA_TEST_CASE(reorder_and_scroll_restore_coexist) {
    // 重排与滚动位置保存互不干扰：重排不改滚动位置，位置变化照常写回注册表。
    ScrollStorage::instance().clear_all();
    auto items = make_items(20);  // 20 × 40 = 800 内容 ≫ 视口 ⇒ 可滚动余量充足
    auto list = std::make_shared<ReorderableList<int>>(items, make_builder());
    list->set_drag_slop(4.0);
    list->set_restore_key("itest.reorder");
    LayoutEngine::layout(*list, Constraints{.min = Size{},
                                            .max = Size{.width = 240.0F, .height = 240.0F}});
    AURORA_TEST_REQUIRE_GT(list->max_scroll_offset(), 0.0F);
    list->set_scroll_offset(80.0F);
    AURORA_TEST_CHECK_NEAR(ScrollStorage::instance().read("itest.reorder").value_or(-1.0F), 80.0F, 1e-4F);

    TestControllerConfig cfg{};
    cfg.width = 320.0F;
    cfg.height = 300.0F;
    TestController tc{Node{list}, cfg};
    AURORA_TEST_REQUIRE(tc.pump().ok());

    const std::vector<Node> found = tc.find_by_type("ReorderItem");
    AURORA_TEST_REQUIRE_EQ(found.size(), 20U);
    // 窗口 300 高、按下点在窗口中心（y=150）⇒ 内容坐标 150+80=230 ⇒ 落在第 5 项（200..240）。
    // 下拖 40dp：中心 200+20+40=260，越过第 6 项中点（260 处不越 ⇒ 恰落在下标 6）。
    const Result<void> drag = tc.drag(*list, Point{.x = 0.0F, .y = 40.0F});
    AURORA_TEST_CHECK_MSG(drag.ok(), drag.ok() ? "drag ok" : drag.error().message);
    settle(*list);
    AURORA_TEST_REQUIRE(tc.pump().ok());

    // 第 5 项与第 6 项互换（滚动坐标下的命中与换位几何联动正确）。
    AURORA_TEST_CHECK_EQ(items->get(), (std::vector<int>{0, 1, 2, 3, 4, 6, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19}));
    // 滚动位置不受重排影响，且仍在注册表里（App 退出时 sync 落盘）。
    AURORA_TEST_CHECK_NEAR(list->scroll_offset(), 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollStorage::instance().read("itest.reorder").value_or(-1.0F), 80.0F, 1e-4F);

    // 重建实例（同 restore_key）：位置恢复，顺序回到数据源顺序 —— 两个子系统各自独立。
    auto rebuilt = std::make_shared<ReorderableList<int>>(items, make_builder());
    rebuilt->set_restore_key("itest.reorder");
    LayoutEngine::layout(*rebuilt, Constraints{.min = Size{},
                                               .max = Size{.width = 240.0F, .height = 240.0F}});
    AURORA_TEST_CHECK_NEAR(rebuilt->scroll_offset(), 80.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(rebuilt->data(),
                         (std::vector<int>{0, 1, 2, 3, 4, 6, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19}));

    ScrollStorage::instance().clear_all();
}

#else

AURORA_TEST_CASE(drag_reorders_items_through_real_dispatch) {
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
}

AURORA_TEST_CASE(reorder_and_scroll_restore_coexist) {
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
}

#endif  // AURORA_BACKEND_HEADLESS

}  // namespace aurora::test_cases::itest_reorder
