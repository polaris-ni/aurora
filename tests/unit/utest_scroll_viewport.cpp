/// 测试类型: unit
/// 目标单元: include/aurora/widget/scroll_viewport.h
/// 测试说明: 滚动视口内核——clamp_offset 符号与夹取、apply_scroll 状态机、remaining_offset 余量回传
/// （端点被吃掉的部分、step<=0 全退）、ScrollSnap::page/enabled 生效条件、
/// snap_target 三方位（Start/Center/End）与分页、末端不足一周期时夹到 max、吸附关闭时退化普通夹取、
/// ScrollGlide 启动/重定向/tick 收敛与到点精确终止

#include "aurora/widget/scroll_viewport.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scroll_viewport {

AURORA_TEST_CASE(clamp_offset_follows_sign_convention) {
    // delta_y 正 = 向上滚 → offset 减小；负 = 向下滚 → offset 增大
    AURORA_TEST_CHECK_NEAR(ScrollViewport::clamp_offset(100.0F, -5.0F, 10.0F, 800.0F, 200.0F), 150.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::clamp_offset(100.0F, 5.0F, 10.0F, 800.0F, 200.0F), 50.0F, 1e-4F);
    // 双向越界夹到 [0, 内容-视口]
    AURORA_TEST_CHECK_NEAR(ScrollViewport::clamp_offset(10.0F, 100.0F, 10.0F, 800.0F, 200.0F), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::clamp_offset(10.0F, -1000.0F, 10.0F, 800.0F, 200.0F), 600.0F, 1e-4F);
    // 内容不足视口：不可滚，恒 0
    AURORA_TEST_CHECK_NEAR(ScrollViewport::clamp_offset(0.0F, -50.0F, 16.0F, 100.0F, 200.0F), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(apply_scroll_mutates_only_on_real_change) {
    ScrollViewport vp{.offset_y = 0.0F, .content_h = 800.0F, .viewport_h = 200.0F, .step = 10.0F};
    AURORA_TEST_CHECK_NEAR(vp.max_offset(), 600.0F, 1e-4F);

    AURORA_TEST_CHECK_TRUE(vp.apply_scroll(-5.0F));  // 吃掉 50dp
    AURORA_TEST_CHECK_NEAR(vp.offset_y, 50.0F, 1e-4F);

    // 已到底且继续向下：无变化 → false（余量方由调用方回传）
    vp.apply_scroll(-1000.0F);
    AURORA_TEST_CHECK_NEAR(vp.offset_y, 600.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(vp.apply_scroll(-1.0F));
    AURORA_TEST_CHECK_NEAR(vp.offset_y, 600.0F, 1e-4F);
}

AURORA_TEST_CASE(remaining_offset_reports_clipped_overshoot) {
    // 全量吃掉 → 余量 0
    AURORA_TEST_CHECK_NEAR(ScrollViewport::remaining_offset(100.0F, 150.0F, -5.0F, 10.0F), 0.0F, 1e-4F);
    // 底部被夹掉 3 单位 → 余量带原增量符号（负 = 仍想向下滚）
    AURORA_TEST_CHECK_NEAR(ScrollViewport::remaining_offset(100.0F, 800.0F, -100.0F, 10.0F), -30.0F, 1e-4F);
    // 顶部被夹掉：只吃了 10dp（1 单位），余下 9 单位上冒
    AURORA_TEST_CHECK_NEAR(ScrollViewport::remaining_offset(10.0F, 0.0F, 10.0F, 10.0F), 9.0F, 1e-4F);
    // step<=0 视为不可滚：全量退为余量
    AURORA_TEST_CHECK_NEAR(ScrollViewport::remaining_offset(0.0F, 0.0F, 7.0F, 0.0F), 7.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_enablement_and_page_factory) {
    const ScrollSnap off;  // extent 0 且非分页 = 关闭
    AURORA_TEST_CHECK_FALSE(off.enabled(200.0F));

    const ScrollSnap by_extent{.extent = 120.0F};
    AURORA_TEST_CHECK_TRUE(by_extent.enabled(200.0F));
    AURORA_TEST_CHECK_TRUE(by_extent.enabled(0.0F));  // 周期制不依赖视口

    const ScrollSnap paging = ScrollSnap::page(ScrollSnapAlignment::Center);
    AURORA_TEST_CHECK_TRUE(paging.paging);
    AURORA_TEST_CHECK_EQ(static_cast<int>(paging.alignment), static_cast<int>(ScrollSnapAlignment::Center));
    AURORA_TEST_CHECK_TRUE(paging.enabled(200.0F));
    AURORA_TEST_CHECK_FALSE(paging.enabled(0.0F));  // 视口未定（未布局）→ 分页无从生效
}

AURORA_TEST_CASE(snap_target_aligns_by_alignment) {
    constexpr float content = 1000.0F;
    constexpr float view = 200.0F;  // max_off = 800

    const ScrollSnap start{.extent = 200.0F, .alignment = ScrollSnapAlignment::Start};
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(150.0F, content, view, start), 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(90.0F, content, view, start), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(400.0F, content, view, start), 400.0F, 1e-4F);  // 已对齐：不动
    // 恰在两条目中点（2.5 周期）：lround 远离零取整 → 上取到 600（确定性行为，钉住以免漂移）
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(500.0F, content, view, start), 600.0F, 1e-4F);

    // Center：条目 k 覆盖 [k·300,(k+1)·300)，中心贴视口中心 → offset = k·300 + 50
    const ScrollSnap center{.extent = 300.0F, .alignment = ScrollSnapAlignment::Center};
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(100.0F, content, view, center), 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(400.0F, content, view, center), 350.0F, 1e-4F);

    // End：条目后沿贴视口后沿 → offset = (k+1)·300 − 200
    const ScrollSnap end{.extent = 300.0F, .alignment = ScrollSnapAlignment::End};
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(100.0F, content, view, end), 100.0F, 1e-4F);  // 已是 End 点
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(200.0F, content, view, end), 100.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_target_paging_uses_viewport_period) {
    const ScrollSnap paging = ScrollSnap::page();
    // 周期 = 视口高 200
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(350.0F, 1000.0F, 200.0F, paging), 400.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(250.0F, 1000.0F, 200.0F, paging), 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(0.0F, 1000.0F, 200.0F, paging), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_target_clamps_to_reachable_tail) {
    // 末端不足一个周期：夹到 max_off 而非回落整条目，保证内容末尾可达
    const ScrollSnap start{.extent = 200.0F};
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(780.0F, 1000.0F, 200.0F, start), 800.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(-50.0F, 1000.0F, 200.0F, start), 0.0F, 1e-4F);

    // 内容短于视口（不可滚）：任何目标都是 0
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(0.0F, 100.0F, 200.0F, start), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_target_degrades_to_plain_clamp_when_disabled) {
    const ScrollSnap off;  // 未启用的吸附
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(123.0F, 1000.0F, 200.0F, off), 123.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(-9.0F, 1000.0F, 200.0F, off), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(9999.0F, 1000.0F, 200.0F, off), 800.0F, 1e-4F);

    // 分页但视口未定 → 同样关闭
    const ScrollSnap paging = ScrollSnap::page();
    AURORA_TEST_CHECK_NEAR(ScrollViewport::snap_target(123.0F, 1000.0F, 0.0F, paging), 123.0F, 1e-4F);
}

AURORA_TEST_CASE(glide_starts_only_when_target_differs_and_eases_out) {
    ScrollGlide g;
    AURORA_TEST_CHECK_NEAR(ScrollGlide::default_duration_s(), 0.15, 1e-9);
    AURORA_TEST_CHECK_FALSE(g.active);

    g.start(100.0F, 100.0F);  // 已在目标：不启动、不重置
    AURORA_TEST_CHECK_FALSE(g.active);

    g.start(0.0F, 100.0F);
    AURORA_TEST_CHECK_TRUE(g.active);
    AURORA_TEST_CHECK_NEAR(g.from, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(g.to, 100.0F, 1e-4F);

    const float mid = g.tick(0.075);  // 半程：easeOutCubic 已过中点（前段快）
    AURORA_TEST_CHECK_TRUE(mid > 50.0F);
    AURORA_TEST_CHECK_TRUE(mid < 100.0F);
    AURORA_TEST_CHECK_TRUE(g.active);

    AURORA_TEST_CHECK_NEAR(g.tick(0.075), 100.0F, 1e-4F);  // 到点：精确 to，且终止
    AURORA_TEST_CHECK_FALSE(g.active);
    AURORA_TEST_CHECK_NEAR(g.tick(1.0), 100.0F, 1e-4F);  // 结束后 tick 保持终点
}

AURORA_TEST_CASE(glide_redirect_keeps_progress_and_finishes) {
    ScrollGlide g;
    g.duration_s = 1.0;
    g.start(0.0F, 1000.0F);
    g.tick(0.5);  // 已推进一半

    // 中途重定向：起点取当前值、计时归零，不保留旧轨迹
    g.start(500.0F, 200.0F);
    AURORA_TEST_CHECK_NEAR(g.from, 500.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(g.to, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(g.elapsed_s, 0.0, 1e-9);

    AURORA_TEST_CHECK_NEAR(g.tick(2.0), 200.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(g.active);
}

}  // namespace aurora::test_cases::utest_scroll_viewport
