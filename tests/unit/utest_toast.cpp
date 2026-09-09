/// 测试类型: unit
/// 目标单元: include/aurora/widget/toast.h
/// 测试说明: 覆盖 ToastHost 的 show 入队与同屏 3 条上限、tick 驱动过期出队与候补重新计时、clear/position 链式设置、position 序列化、负时长钳制及布局与离屏绘制冒烟

#include <chrono>
#include <string>
#include <type_traits>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/text.h"
#include "aurora/widget/toast.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_toast {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(toast_host_type_contract) {
    static_assert(std::is_base_of_v<aurora::SingleChild, ToastHost>);  // 单子容器宿主
    static_assert(!std::is_copy_constructible_v<ToastHost>);  // Widget 拷贝被删除

    const ToastHost host{Text{"base"}};
    AURORA_TEST_CHECK_EQ(std::string{host.type_name()}, "ToastHost");
    AURORA_TEST_CHECK_EQ(host.pending_count(), 0U);  // 初始队列为空
    AURORA_TEST_CHECK_THAT(host.visible_toasts(), aurora::testing::matchers::is_empty());
    AURORA_TEST_CHECK_EQ(host.child_nodes().size(), 1U);  // children_policy = single

    const auto d = ToastHost::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "ToastHost");
    AURORA_TEST_CHECK_EQ(d.children_policy, "single");
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 1U);
    AURORA_TEST_CHECK_EQ(d.properties[0].name, "position");
    AURORA_TEST_CHECK_EQ(d.properties[0].type, "ToastPosition");
    AURORA_TEST_CHECK_EQ(d.properties[0].default_value, "Bottom");
}

AURORA_TEST_CASE(toast_show_queues_fifo_and_caps_visible) {
    ToastHost host{Text{"base"}};
    host.show("first", 1000.0F);
    host.show("second", 1000.0F);
    AURORA_TEST_CHECK_EQ(host.pending_count(), 2U);

    const auto vis = host.visible_toasts();
    AURORA_TEST_REQUIRE_EQ(vis.size(), 2U);
    AURORA_TEST_CHECK_EQ(vis[0], std::string{"first"});  // FIFO 入队序
    AURORA_TEST_CHECK_EQ(vis[1], std::string{"second"});

    for (int i = 0; i < 5; ++i) {
        host.show("t" + std::to_string(i), 1000.0F);
    }
    AURORA_TEST_CHECK_EQ(host.pending_count(), 7U);  // 全部排队
    AURORA_TEST_CHECK_THAT(host.visible_toasts(),
                           aurora::testing::matchers::size_is(3U));  // 同屏最多 3 条
    AURORA_TEST_CHECK_EQ(host.visible_toasts()[0], std::string{"first"});  // 队首优先可见
}

AURORA_TEST_CASE(toast_tick_expires_after_duration) {
    ToastHost host{Text{"base"}};
    host.show("ephemeral", 100.0F);

    const auto t0 = std::chrono::steady_clock::now();
    host.tick(t0);  // 首次可见：开始计时
    AURORA_TEST_CHECK_EQ(host.pending_count(), 1U);

    host.tick(t0 + std::chrono::milliseconds(50));  // 未到时长
    AURORA_TEST_CHECK_EQ(host.pending_count(), 1U);

    host.tick(t0 + std::chrono::milliseconds(150));  // 超时出队
    AURORA_TEST_CHECK_EQ(host.pending_count(), 0U);
    AURORA_TEST_CHECK_TRUE(host.visible_toasts().empty());
}

AURORA_TEST_CASE(toast_queue_promotes_waiter_with_fresh_timer) {
    ToastHost host{Text{"base"}};
    for (int i = 0; i < 4; ++i) {
        host.show("q" + std::to_string(i), 100.0F);
    }

    const auto t0 = std::chrono::steady_clock::now();
    host.tick(t0);                                   // 前 3 条开始计时
    host.tick(t0 + std::chrono::milliseconds(150));  // 前 3 条过期
    AURORA_TEST_CHECK_EQ(host.pending_count(), 1U);
    AURORA_TEST_CHECK_EQ(host.visible_toasts()[0], std::string{"q3"});  // 候补顶上

    host.tick(t0 + std::chrono::milliseconds(200));  // 候补此刻才开始计时：50ms 未过期
    AURORA_TEST_CHECK_EQ(host.pending_count(), 1U);

    host.tick(t0 + std::chrono::milliseconds(400));  // 候补已计时 200ms：过期
    AURORA_TEST_CHECK_EQ(host.pending_count(), 0U);
}

AURORA_TEST_CASE(toast_negative_duration_expires_immediately) {
    ToastHost host{Text{"base"}};
    host.show("neg", -5.0F);  // 负时长钳为 0

    const auto t0 = std::chrono::steady_clock::now();
    host.tick(t0);  // duration 0：首帧即过期
    AURORA_TEST_CHECK_EQ(host.pending_count(), 0U);
}

AURORA_TEST_CASE(toast_clear_and_chained_position) {
    ToastHost host{Text{"base"}};
    AURORA_TEST_CHECK_TRUE(&host.set_position(ToastPosition::Top) == &host);  // 链式返回自身

    host.show("a", 5000.0F);
    host.show("b", 5000.0F);
    AURORA_TEST_CHECK_EQ(host.pending_count(), 2U);

    host.clear();  // 立即清空全部（可见 + 排队）
    AURORA_TEST_CHECK_EQ(host.pending_count(), 0U);
    AURORA_TEST_CHECK_TRUE(host.visible_toasts().empty());
}

AURORA_TEST_CASE(toast_serialize_position_and_base_props) {
    ToastHost host{Text{"base"}};
    Json props;
    host.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["position"].get<std::string>(), std::string{"bottom"});  // 默认 Bottom
    AURORA_TEST_CHECK_EQ(props["show"].get<bool>(), true);  // 基类通用属性保留

    host.set_position(ToastPosition::Top);
    Json top_props;
    host.serialize_props(top_props);
    AURORA_TEST_CHECK_EQ(top_props["position"].get<std::string>(), std::string{"top"});
}

AURORA_TEST_CASE(toast_layout_and_offscreen_paint_smoke) {
    ToastHost host{Text{"base"}};
    host.set_position(ToastPosition::Top);
    host.show("visible", 5000.0F);

    const BuildContext ctx;
    host.mount(ctx);
    LayoutEngine::layout(host, bounded(320.0F, 240.0F));
    AURORA_TEST_CHECK_NEAR(host.size().width, 320.0F, 1e-3F);  // 有界约束 → 取约束上限
    AURORA_TEST_CHECK_NEAR(host.size().height, 240.0F, 1e-3F);

    Painter p;
    p.begin(320, 240);
    const Rect full{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 320.0F, .height = 240.0F}};
    host.paint(p, full, ctx);  // 叠加通知绘制（顶部堆叠）
    AURORA_TEST_CHECK_EQ(p.width(), 320);
    AURORA_TEST_CHECK_EQ(p.height(), 240);

    host.clear();
    AURORA_TEST_CHECK_EQ(host.pending_count(), 0U);
    host.paint(p, full, ctx);  // 清空后叠加绘制为无操作，不崩
}

}  // namespace aurora::test_cases::utest_toast
