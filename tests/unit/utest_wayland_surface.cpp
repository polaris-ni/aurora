/// 测试类型: unit
/// 目标单元: include/aurora/window/wayland_surface.h
/// 测试说明: Wayland 后端类型契约 skip 桩 + 真机选择加入的光标提交断言与 text-input-v3 桥
/// 观测面断言（AURORA_LIVE_WAYLAND=1）——头整体被
/// AURORA_PLATFORM_LINUX && !AURORA_PLATFORM_ANDROID && AURORA_BACKEND_WAYLAND 门控，非 Linux 平台无法编译

#include <chrono>
#include <cstdlib>
#include <thread>
#include <type_traits>

#include "aurora/core/platform.h"  // 守卫求值前必须先有平台宏（TU 自包含，不依赖 PCH 伞头带入）
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)
#include "aurora/window/cursor_map.h"
#include "aurora/window/wayland_surface.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_wayland_surface {

AURORA_TEST_CASE(wayland_surface_type_contract) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)
    static_assert(std::is_base_of_v<aurora::Surface, aurora::WaylandSurface>);
    static_assert(std::is_final_v<aurora::WaylandSurface>);
    static_assert(!std::is_copy_constructible_v<aurora::WaylandSurface>);
    static_assert(!std::is_move_constructible_v<aurora::WaylandSurface>);
    static_assert(!std::is_default_constructible_v<aurora::WaylandSurface>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aurora::Surface, aurora::WaylandSurface>);
#else
    AURORA_TEST_SKIP("Wayland 后端仅在 Linux + AURORA_BACKEND_WAYLAND 下编译，当前平台未开启");
#endif
}

AURORA_TEST_CASE(wayland_surface_os_dependent_paths_skipped) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)
    // 构造需要 WAYLAND_DISPLAY 连接（wl_display/registry/xdg-shell 握手）；
    // is_available()==false 分支由工厂返回 Result 错误，需真实合成器或 headless 组合器
    // （如 wlheadless）驱动，属集成层覆盖范围。
    AURORA_TEST_SKIP("WaylandSurface 构造依赖真实合成器连接（wl_display/xdg-shell），单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("Wayland 后端仅在 Linux + AURORA_BACKEND_WAYLAND 下编译，当前平台未开启");
#endif
}

AURORA_TEST_CASE(wayland_surface_live_cursor_commit_sweep) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)
    const char *opt_in = std::getenv("AURORA_LIVE_WAYLAND");
    if (opt_in == nullptr || *opt_in == '\0') {
        AURORA_TEST_SKIP("需显式置 AURORA_LIVE_WAYLAND=1：本用例会连接真实合成器并创建真实窗口");
    }

    aurora::WaylandSurface surface(320, 240, "aurora-live-wayland-cursor");
    AURORA_TEST_REQUIRE_TRUE(surface.is_available());
    // 出帧才让 xdg_toplevel 进入 mapped（合成器随即才可能给 pointer enter）。
    AURORA_TEST_CHECK(static_cast<bool>(surface.begin_frame(320, 240)));
    AURORA_TEST_CHECK(static_cast<bool>(surface.present()));
    // 铺满输出以取得 enter serial：Wayland 客户端无 warp API，只能让静止指针必然落在表面内。
    // 两级策略——WSLg 实测最大化后表面不含顶部面板带（3840x2088），指针停在那儿即拿不到 enter，
    // 须再退到全屏（见 tools/verify/wayland_cursor_live_probe.cpp 同口径）。
    auto wait_enter = [&surface](int rounds) {
        for (int i = 0; i < rounds; ++i) {
            surface.poll_platform_events();
            if (surface.cursor_state().pointer_entered) {
                return true;
            }
            (void)surface.begin_frame(static_cast<int>(surface.size().width),
                                      static_cast<int>(surface.size().height));
            (void)surface.present();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    surface.toggle_maximize();
    bool entered = wait_enter(80);
    if (!entered) {
        surface.set_fullscreen(true);
        entered = wait_enter(80);
    }
    if (!entered) {
        AURORA_TEST_SKIP("未取得 wl_pointer.enter（无指针设备或指针未落入窗口）：无合法 serial 可下发");
    }
    // 「桌面未装图标主题」是环境缺失、「主题位图没提交上去」才是接线缺陷，两者要分开申报：
    // 用一个不同于 enter 默认（Arrow）的形状逼一次真实提交，theme_size 仍为 0 即主题根本没加载成功。
    surface.set_cursor(aurora::CursorShape::Wait);
    surface.poll_platform_events();
    if (const auto probe = surface.cursor_state(); !probe.applied && probe.theme_size == 0) {
        AURORA_TEST_SKIP("wl_cursor_theme_load 未成功（本机无可用图标主题）：形状提交无从验证");
    }

    // 两轮全形状（形状逐一变化，故每轮每形状各提交一次）。命中名取**严格等值**断言：走 default/left_ptr
    // 回退即「未按请求形状解析」，属缺陷而非主题差异（freedesktop 规范名是主题的必备项）。
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < static_cast<int>(aurora::AURORA_CURSOR_SHAPE_COUNT); ++i) {
            const auto shape = static_cast<aurora::CursorShape>(i);
            surface.set_cursor(shape);
            surface.poll_platform_events();
            const auto st = surface.cursor_state();
            AURORA_TEST_CHECK_TRUE(st.applied);
            AURORA_TEST_CHECK_EQ(st.resolved_name, std::string{aurora::cursor_rfc_name(shape)});
            AURORA_TEST_CHECK_TRUE(st.image_width > 0 && st.image_height > 0);
        }
    }
    // 同形状连续两次：第 2 次命中「同形状同缩放」去重，提交次数不得增长（幂等契约）。
    surface.set_cursor(aurora::CursorShape::IBeam);
    const int commits_after_first = surface.cursor_state().commits;
    surface.set_cursor(aurora::CursorShape::IBeam);
    AURORA_TEST_CHECK_EQ(surface.cursor_state().commits, commits_after_first);

    // 光标下发不得影响窗口自身可用性（协议错误会直接断链）。
    AURORA_TEST_CHECK_TRUE(surface.size().width > 0.0F);
    surface.set_title("aurora-live-wayland-cursor");
    AURORA_TEST_CHECK_FALSE(surface.should_close());
#else
    AURORA_TEST_SKIP("Wayland 后端仅在 Linux + AURORA_BACKEND_WAYLAND 下编译，当前平台未开启");
#endif
}

AURORA_TEST_CASE(wayland_surface_live_text_input_bridge_invariants) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)
    const char *opt_in = std::getenv("AURORA_LIVE_WAYLAND");
    if (opt_in == nullptr || *opt_in == '\0') {
        AURORA_TEST_SKIP("需显式置 AURORA_LIVE_WAYLAND=1：本用例会连接真实合成器并创建真实窗口");
    }

    aurora::WaylandSurface surface(320, 240, "aurora-live-wayland-ime");
    AURORA_TEST_REQUIRE_TRUE(surface.is_available());
    AURORA_TEST_CHECK(static_cast<bool>(surface.begin_frame(320, 240)));
    AURORA_TEST_CHECK(static_cast<bool>(surface.present()));
    // 泵一段全局/能力事件（registry global 与 seat 能力在连接建立后陆续到达）。
    for (int i = 0; i < 15; ++i) {
        surface.poll_platform_events();
        surface.wait_events(20.0);
    }

    auto st = surface.text_input_state();
    if (st.protocol_disabled) {
        AURORA_TEST_SKIP("本次构建缺 text-input-unstable-v3 XML（AURORA_HAVE_WL_TEXT_INPUT=0）⇒ 桥未编译");
    }
    // 一致性不变量：input 只能来自 manager 绑定，enable 只能发生在本端已建 input 之后。
    AURORA_TEST_CHECK(!(st.input_created && !st.manager_bound));
    AURORA_TEST_CHECK(!(st.enabled && !st.input_created));

    surface.set_composition_caret_provider([] {
        return aurora::Rect{aurora::Point{24.0F, 40.0F}, aurora::Size{6.0F, 16.0F}};
    });
    for (int i = 0; i < 10; ++i) {  // present 内含 IME 状态刷新
        (void)surface.begin_frame(320, 240);
        (void)surface.present();
        surface.poll_platform_events();
    }
    st = surface.text_input_state();
    if (!st.manager_bound) {
        // WSLg Weston 常态：合成器不发布 v3 ⇒ 桥必须零请求且不影响连接健康。
        AURORA_TEST_CHECK(!st.input_created && !st.enabled && st.commits == 0);
        AURORA_TEST_CHECK_FALSE(surface.should_close());
        AURORA_TEST_SKIP("合成器未发布 text-input-v3 ⇒ enable 判据段无从驱动（完整验收见 "
                         "aurora_verify_wayland_ime 探针）");
    }
    AURORA_TEST_CHECK_TRUE(st.input_created);  // seat 键盘能力到达即建 input 对象
    if (!st.entered) {
        AURORA_TEST_SKIP("未取得 text_input.enter（键盘焦点未落入本表面）⇒ enable 判据交探针/人工");
    }
    // enable 判据：entered + 非零盒 ⇒ enabled；归零盒 ⇒ disable；再非零 ⇒ 可逆重启用。
    AURORA_TEST_CHECK_TRUE(surface.text_input_state().enabled);
    const int commits_enabled = surface.text_input_state().commits;
    (void)surface.begin_frame(320, 240);
    (void)surface.present();
    AURORA_TEST_CHECK_EQ(surface.text_input_state().commits, commits_enabled);  // 同状态去重
    surface.set_composition_caret_provider([] { return aurora::Rect{}; });
    surface.poll_platform_events();
    AURORA_TEST_CHECK_FALSE(surface.text_input_state().enabled);
    AURORA_TEST_CHECK_FALSE(surface.should_close());
#else
    AURORA_TEST_SKIP("Wayland 后端仅在 Linux + AURORA_BACKEND_WAYLAND 下编译，当前平台未开启");
#endif
}

}  // namespace aurora::test_cases::utest_wayland_surface
