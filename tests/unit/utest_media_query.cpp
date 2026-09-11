/// 测试类型: unit
/// 目标单元: include/aurora/environment/media_query.h
/// 测试说明: MediaQuery 纯逻辑部分——默认值、of(scale) 便捷构造、经 BuildContext 环境链的读取与无 Provider 降级

#include <cstdint>

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/environment/media_query.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/breakpoint_builder.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_media_query {

// 说明：from_surface(const Surface&) 依赖真实 Surface（尺寸/缩放/内容边距，需窗口或渲染上下文），
// 按约定以 skip 桩登记，不在此覆盖；其余均为纯逻辑（值语义 + 环境链查找），全量覆盖。

AURORA_TEST_CASE(default_instance_uses_conservative_defaults) {
    // 默认实例：缩放 1.0、竖屏、平台/设备 Unknown、无安全区、不减弱动效。
    const aurora::MediaQuery mq;
    AURORA_TEST_CHECK_NEAR(mq.scale_factor, 1.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(mq.text_scale_factor, 1.0F, 1e-6F);
    AURORA_TEST_CHECK(mq.orientation == aurora::ScreenOrientation::Portrait);
    AURORA_TEST_CHECK(mq.platform == aurora::PlatformKind::Unknown);
    AURORA_TEST_CHECK(mq.device == aurora::DeviceKind::Unknown);
    AURORA_TEST_CHECK_FALSE(mq.prefer_reduced_motion);
    AURORA_TEST_CHECK_EQ(mq.size.width, 0.0F);
    AURORA_TEST_CHECK_EQ(mq.size.height, 0.0F);
    AURORA_TEST_CHECK_EQ(mq.screen_size.width, 0.0F);
    AURORA_TEST_CHECK_EQ(mq.padding.left, 0.0F);
    AURORA_TEST_CHECK_EQ(mq.padding.top, 0.0F);
}

AURORA_TEST_CASE(of_scale_only_overrides_scale_factor) {
    // of(scale)：轻量注入——仅缩放因子变化，其余字段保持默认。
    const aurora::MediaQuery mq = aurora::MediaQuery::of(2.625F);
    AURORA_TEST_CHECK_NEAR(mq.scale_factor, 2.625F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(mq.text_scale_factor, 1.0F, 1e-6F);
    AURORA_TEST_CHECK(mq.orientation == aurora::ScreenOrientation::Portrait);
    AURORA_TEST_CHECK(mq.platform == aurora::PlatformKind::Unknown);
    AURORA_TEST_CHECK(mq.device == aurora::DeviceKind::Unknown);
}

AURORA_TEST_CASE(of_ctx_reads_nearest_provider_value) {
    // 经 Environment 注入链读取「最近祖先 Provider」生效的 MediaQuery。
    aurora::Environment root;
    root.set_local<aurora::MediaQuery>(aurora::MediaQuery::of(1.0F));
    const aurora::Environment child = root.with<aurora::MediaQuery>(aurora::MediaQuery::of(3.0F));
    aurora::BuildContext ctx;
    ctx.env = &child;
    const aurora::MediaQuery *nearest = aurora::media_query_of(ctx);
    AURORA_TEST_REQUIRE(nearest != nullptr);
    AURORA_TEST_CHECK_NEAR(nearest->scale_factor, 3.0F, 1e-6F);  // 最近祖先（child）胜出
    AURORA_TEST_CHECK_NEAR(aurora::MediaQuery::of(ctx).scale_factor, 3.0F, 1e-6F);
}

AURORA_TEST_CASE(of_ctx_penetrates_to_ancestor_for_uncovered_type) {
    // 子环境只覆盖其他类型时，MediaQuery 沿父链取祖先 Provider 的值。
    aurora::Environment root;
    root.set_local<aurora::MediaQuery>(aurora::MediaQuery::of(2.0F));
    const aurora::Environment child = root.with<int>(1);  // 只覆盖 int
    aurora::BuildContext ctx;
    ctx.env = &child;
    AURORA_TEST_CHECK_NEAR(aurora::MediaQuery::of(ctx).scale_factor, 2.0F, 1e-6F);
}

AURORA_TEST_CASE(of_ctx_falls_back_to_default_without_provider) {
    // 无 Provider 注入：media_query_of 返回 nullptr；of(ctx) 返回进程级默认实例（保守降级）。
    aurora::BuildContext ctx;  // env == nullptr
    AURORA_TEST_CHECK(aurora::media_query_of(ctx) == nullptr);
    const aurora::MediaQuery &fallback = aurora::MediaQuery::of(ctx);
    AURORA_TEST_CHECK_NEAR(fallback.scale_factor, 1.0F, 1e-6F);
    AURORA_TEST_CHECK(fallback.platform == aurora::PlatformKind::Unknown);
    AURORA_TEST_CHECK(fallback.orientation == aurora::ScreenOrientation::Portrait);
}

AURORA_TEST_CASE(from_surface_requires_surface_backend) {
    // from_surface(const Surface&) 需要 Surface 实例（窗口/渲染上下文边界），纯逻辑单测不覆盖。
    AURORA_TEST_SKIP("from_surface 依赖 Surface/窗口上下文，属集成层覆盖范围");
}

AURORA_TEST_CASE(resolve_breakpoint_boundaries_and_custom_thresholds) {
    // 默认阈值 600/840：边界值归属高档（左闭右开）。
    AURORA_TEST_CHECK(aurora::resolve_breakpoint(0.0F, 600.0F, 840.0F) == aurora::Breakpoint::Compact);
    AURORA_TEST_CHECK(aurora::resolve_breakpoint(599.9F, 600.0F, 840.0F) == aurora::Breakpoint::Compact);
    AURORA_TEST_CHECK(aurora::resolve_breakpoint(600.0F, 600.0F, 840.0F) == aurora::Breakpoint::Medium);
    AURORA_TEST_CHECK(aurora::resolve_breakpoint(839.9F, 600.0F, 840.0F) == aurora::Breakpoint::Medium);
    AURORA_TEST_CHECK(aurora::resolve_breakpoint(840.0F, 600.0F, 840.0F) == aurora::Breakpoint::Expanded);

    // 自定义阈值生效
    AURORA_TEST_CHECK(aurora::resolve_breakpoint(500.0F, 400.0F, 700.0F) == aurora::Breakpoint::Medium);
    AURORA_TEST_CHECK(aurora::resolve_breakpoint(750.0F, 400.0F, 700.0F) == aurora::Breakpoint::Expanded);
}

AURORA_TEST_CASE(breakpoint_builder_follows_media_query_width) {
    int builds = 0;
    Breakpoint last = Breakpoint::Compact;
    BreakpointBuilder bb([&](Breakpoint bp) -> Node {
        ++builds;
        last = bp;
        return Node{std::make_shared<Text>("bp")};
    });

    auto layout_with_width = [&](float w) {
        Environment env;
        MediaQuery mq;
        mq.size = Size{.width = w, .height = 800.0F};
        env.set_local<MediaQuery>(mq);
        BuildContext ctx;
        ctx.env = &env;
        LayoutEngine::layout(bb, Constraints{}, ctx);
    };

    // 500 → Compact（首次构建）
    layout_with_width(500.0F);
    AURORA_TEST_CHECK(bb.current_breakpoint() == Breakpoint::Compact);
    AURORA_TEST_CHECK(last == Breakpoint::Compact);
    AURORA_TEST_CHECK_EQ(builds, 1);

    // 700 → Medium（档位变化触发重建）
    layout_with_width(700.0F);
    AURORA_TEST_CHECK(bb.current_breakpoint() == Breakpoint::Medium);
    AURORA_TEST_CHECK_EQ(builds, 2);

    // 900 → Expanded
    layout_with_width(900.0F);
    AURORA_TEST_CHECK(bb.current_breakpoint() == Breakpoint::Expanded);
    AURORA_TEST_CHECK_EQ(builds, 3);

    // 同档位内变化（950）：只重新布局，不重建子树
    layout_with_width(950.0F);
    AURORA_TEST_CHECK(bb.current_breakpoint() == Breakpoint::Expanded);
    AURORA_TEST_CHECK_EQ(builds, 3);
}

AURORA_TEST_CASE(breakpoint_builder_falls_back_to_constraint_width) {
    // 无 MediaQuery 注入：退化为父约束最大宽度（无限约束视 0 → Compact）。
    int builds = 0;
    Breakpoint last = Breakpoint::Compact;
    BreakpointBuilder bb([&](Breakpoint bp) -> Node {
        ++builds;
        last = bp;
        return Node{std::make_shared<Text>("bp")};
    });

    BuildContext ctx;  // env == nullptr
    LayoutEngine::layout(bb, Constraints{}, ctx);  // max 无限 → 0 → Compact
    AURORA_TEST_CHECK(bb.current_breakpoint() == Breakpoint::Compact);
    AURORA_TEST_CHECK_EQ(builds, 1);

    const Constraints bounded{.min = Size{.width = 0.0F, .height = 0.0F},
                              .max = Size{.width = 700.0F, .height = 800.0F}};
    LayoutEngine::layout(bb, bounded, ctx);
    AURORA_TEST_CHECK(bb.current_breakpoint() == Breakpoint::Medium);
    AURORA_TEST_CHECK_EQ(builds, 2);
}

}  // namespace aurora::test_cases::utest_media_query
