/// 测试类型: unit
/// 目标单元: include/aurora/widget/lifecycle.h
/// 测试说明: 覆盖 Lifecycle 声明式副作用钩子——mount 恰好触发一次 on_mount（幂等保护）、
/// 析构触发 on_unmount 清理、空回调安全、布局透传子尺寸、自描述事件清单

#include <memory>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/lifecycle.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_lifecycle {

namespace {

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(mount_fires_on_mount_exactly_once) {
    int mounts = 0;
    const BuildContext ctx;
    {
        Lifecycle lc(box(40.0F, 20.0F), [&mounts](const BuildContext&) -> void { ++mounts; });
        lc.mount(ctx);
        lc.mount(ctx);  // 幂等：重复 mount 不重复触发
        AURORA_TEST_CHECK_EQ(mounts, 1);
    }
    // 析构不触发 on_mount。
    AURORA_TEST_CHECK_EQ(mounts, 1);
}

AURORA_TEST_CASE(destructor_fires_on_unmount_once) {
    int unmounts = 0;
    const BuildContext ctx;
    {
        Lifecycle lc(box(40.0F, 20.0F), [](const BuildContext&) -> void {}, [&unmounts]() -> void { ++unmounts; });
        lc.mount(ctx);
        AURORA_TEST_CHECK_EQ(unmounts, 0);
    }
    AURORA_TEST_CHECK_EQ(unmounts, 1);
}

AURORA_TEST_CASE(empty_callbacks_are_safe) {
    const BuildContext ctx;
    {
        Lifecycle lc(box(10.0F, 10.0F), nullptr, nullptr);
        AURORA_TEST_CHECK_NO_THROW(lc.mount(ctx));
    }
    // 无异常即通过。
}

AURORA_TEST_CASE(layout_passes_child_size_through) {
    Lifecycle lc(box(70.0F, 25.0F), [](const BuildContext&) -> void {});
    LayoutEngine::layout(lc, bounded(200.0F, 100.0F));
    const Size s = lc.size();
    AURORA_TEST_CHECK_NEAR(s.width, 70.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.height, 25.0F, 1e-4F);
}

AURORA_TEST_CASE(mount_callback_receives_context) {
    bool ctx_seen = false;
    const BuildContext ctx;
    Lifecycle lc(box(10.0F, 10.0F), [&ctx_seen](const BuildContext&) -> void { ctx_seen = true; });
    lc.mount(ctx);
    AURORA_TEST_CHECK_TRUE(ctx_seen);
}

AURORA_TEST_CASE(describe_reports_events_and_policy) {
    const auto d = Lifecycle::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Lifecycle");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 2U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_mount");
    AURORA_TEST_CHECK_EQ(std::string{d.events[1]}, "on_unmount");
}

}  // namespace aurora::test_cases::utest_lifecycle
