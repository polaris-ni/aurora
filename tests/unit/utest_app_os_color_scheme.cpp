/// 测试类型: unit
/// 目标单元: include/aurora/app/application.h include/aurora/app/window_host.h
/// 测试说明: OS 明暗偏好的应用级聚合读点：未 seed 时读 `Unspecified`（构造期不隐式读注册表）、
///           跨宿主同值切换只通知一次、同窗重复同值不通知、不同值按序通知、
///           `refresh_os_color_scheme()` 幂等且只在变化时通知、回调恒在主线程、
///           响应式订阅（`Effect` 内读 State）随切换重跑、`teardown()` 后应用级上报表同步解绑、
///           框架既有那一半（`Window` 快照 + 整帧置脏）行为不动。
///           全部走 `HeadlessSurface::emit_os_color_scheme_change` seam，不依赖真机档位。

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/state/effect.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/widget/text.h"
#include "aurora/window/platform.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_app_os_color_scheme {

namespace {

auto make_scene() -> Scene { return Scene{Node{std::make_shared<Text>("hi")}}; }

/// @brief 产出一枚无头窗口（每枚自带独立 `HeadlessSurface`，可各自 emit）。
/// @param title 窗口标题（仅用于日志归因）。
auto make_headless_window(std::string title) -> std::unique_ptr<Window> {
    HeadlessOptions opts;
    static_cast<WindowOptions &>(opts) = WindowOptions{};
    opts.title = std::move(title);
    opts.size = Size{.width = 200.0F, .height = 150.0F};
    auto res = create_window(opts);
    return res ? std::move(res.value()) : nullptr;
}

/// @brief 取宿主的无头表面（seam 入口）；非无头后端或无窗口宿主返回 nullptr。
auto surface_of(WindowHost *host) -> HeadlessSurface * {
    if (host == nullptr || host->window() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<HeadlessSurface *>(&host->window()->surface());
}

/// @brief 追加一枚无头第二窗并回其宿主。
/// @param app 目标应用。
/// @param title 第二窗标题。
/// @return 第二窗宿主指针。
auto add_second_window(Application &app, const char *title) -> WindowHost * {
    auto window = make_headless_window(title);
    if (window == nullptr) {
        return nullptr;
    }
    const WindowId id = app.open_window(std::move(window), make_scene());
    return app.window_host(id);
}

}  // namespace

AURORA_TEST_CASE(state_reads_unspecified_without_seed) {
    // 初值口径：登记窗口（含无头窗口）与注册连接都不构成 seed，构造期不隐式读注册表；
    // 未显式 refresh 且系统未上报过变化时读 `Unspecified`。
    Application app{make_scene(), make_headless_window("os_unseeded")};
    AURORA_TEST_REQUIRE_NOT_NULL(app.window());
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), ColorScheme::Unspecified);

    // `Unspecified` 是合法上报值（框架原样透传，不替应用回落明/暗）：未 seed 时初值即它，
    // 故后端上报 `Unspecified` 被同值去重吸收，不产生噪声通知。
    // 主窗登记时框架自己会读一次系统偏好推给 `Window`（既有行为，本条不改），追加第二扇窗同样
    // 不 seed 应用级 State——否则初值就成了隐式探测。
    auto *second = add_second_window(app, "os_unseeded_b");
    AURORA_TEST_REQUIRE_NOT_NULL(second);
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), ColorScheme::Unspecified);

    std::vector<ColorScheme> seen;
    app.set_on_os_color_scheme([&seen](ColorScheme s) -> void { seen.push_back(s); });
    HeadlessSurface *const surface = surface_of(app.windows().front());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);
    surface->emit_os_color_scheme_change(ColorScheme::Unspecified);
    AURORA_TEST_CHECK(seen.empty());
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), ColorScheme::Unspecified);
}

AURORA_TEST_CASE(emit_on_either_window_notifies_once) {
    // 判据「跨宿主聚合」：同一次切换打在任一窗上，应用级恰一次、值相等；
    // 另一窗再打同值 ⇒ 仍恰一次（进程级事实只有一个真值源）。
    Application app{make_scene(), make_headless_window("os_cross_host")};
    auto *first = app.windows().front();
    auto *second = add_second_window(app, "os_cross_host_b");
    AURORA_TEST_REQUIRE_NOT_NULL(second);
    AURORA_TEST_REQUIRE_EQ(app.window_count(), 2U);

    std::vector<ColorScheme> seen;
    app.set_on_os_color_scheme([&seen](ColorScheme s) -> void { seen.push_back(s); });

    HeadlessSurface *const a = surface_of(first);
    HeadlessSurface *const b = surface_of(second);
    AURORA_TEST_REQUIRE_NOT_NULL(a);
    AURORA_TEST_REQUIRE_NOT_NULL(b);

    a->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), ColorScheme::Dark);

    b->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);

    // 反向：打在另一扇窗上的不同值照常通知（聚合不是「只认主窗」）。
    b->emit_os_color_scheme_change(ColorScheme::Light);
    AURORA_TEST_CHECK_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), ColorScheme::Light);
}

AURORA_TEST_CASE(same_value_repeats_do_not_notify_and_order_is_kept) {
    // 判据「同值去重 + 顺序」：同窗重复同值不产生第二次通知；
    // Light → Dark 两次且顺序正确（`State::set` 对相等值也发信号，去重只能在转发处做）。
    Application app{make_scene(), make_headless_window("os_dedup")};
    HeadlessSurface *const surface = surface_of(app.windows().front());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    std::vector<ColorScheme> seen;
    app.set_on_os_color_scheme([&seen](ColorScheme s) -> void { seen.push_back(s); });

    surface->emit_os_color_scheme_change(ColorScheme::Light);
    surface->emit_os_color_scheme_change(ColorScheme::Light);
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);

    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_EQ(seen.at(0), ColorScheme::Light);
    AURORA_TEST_CHECK_EQ(seen.at(1), ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), ColorScheme::Dark);

    // 回到 `Unspecified` 是一次**真实**的档位变化（已 seed 过明/暗），照常通知。
    surface->emit_os_color_scheme_change(ColorScheme::Unspecified);
    AURORA_TEST_CHECK_EQ(seen.size(), 3U);
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), ColorScheme::Unspecified);
}

AURORA_TEST_CASE(refresh_seeds_idempotently_and_notifies_only_on_change) {
    // 判据「seed」：`refresh_os_color_scheme()` 现读一次并写入 State，幂等——
    // 第二次调用值未变 ⇒ 不再通知。真机档位随环境而变，故判据按「与自由函数实值相等」
    // 写死，而通知次数按「实值是否区别于初值 `Unspecified`」推得，任何环境都确定。
    Application app{make_scene(), make_headless_window("os_refresh")};
    const ColorScheme probe = aurora::os_color_scheme();  // 同一真值源（无缓存，每次现读）

    std::vector<ColorScheme> seen;
    app.set_on_os_color_scheme([&seen](ColorScheme s) -> void { seen.push_back(s); });

    app.refresh_os_color_scheme();
    app.refresh_os_color_scheme();
    app.refresh_os_color_scheme();

    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), probe);
    AURORA_TEST_CHECK_EQ(seen.size(), probe == ColorScheme::Unspecified ? 0U : 1U);
    if (probe != ColorScheme::Unspecified) {
        AURORA_TEST_CHECK_EQ(seen.at(0), probe);
    }

    // seed 之后同值的上报仍被去重；不同值的上报照常通知。
    const ColorScheme other = probe == ColorScheme::Dark ? ColorScheme::Light : ColorScheme::Dark;
    HeadlessSurface *const surface = surface_of(app.windows().front());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);
    surface->emit_os_color_scheme_change(probe);
    AURORA_TEST_CHECK_EQ(seen.size(), probe == ColorScheme::Unspecified ? 0U : 1U);
    surface->emit_os_color_scheme_change(other);
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), other);
}

AURORA_TEST_CASE(observer_is_called_on_calling_thread) {
    // 判据「主线程」：通知发生在 emit 的调用线程（生产侧即 Win32 消息泵线程），
    // 不引入后端线程或探测线程。用例主线程 emit、主线程断言。
    Application app{make_scene(), make_headless_window("os_thread")};
    HeadlessSurface *const surface = surface_of(app.windows().front());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    const std::thread::id main_thread = std::this_thread::get_id();
    std::thread::id seen_thread{};
    app.set_on_os_color_scheme([&seen_thread](ColorScheme) -> void { seen_thread = std::this_thread::get_id(); });

    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_CHECK(seen_thread == main_thread);
    AURORA_TEST_CHECK(std::this_thread::get_id() == main_thread);
}

AURORA_TEST_CASE(state_subscription_reruns_on_switch) {
    // 响应式通路：`Effect` 内读 State 即自动订阅，切换后重跑一次；同值重复上报不重跑。
    Application app{make_scene(), make_headless_window("os_effect")};
    HeadlessSurface *const surface = surface_of(app.windows().front());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    int runs = 0;
    ColorScheme last = ColorScheme::Unspecified;
    Effect e{[&]() -> void {
        ++runs;
        last = app.os_color_scheme_state().get();
    }};
    e.run();  // 首跑建立依赖
    AURORA_TEST_CHECK_EQ(runs, 1);

    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(last, ColorScheme::Dark);

    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(runs, 2);  // 同值不通知 ⇒ 不重跑
}

AURORA_TEST_CASE(framework_leg_unchanged_and_teardown_unbinds_sink) {
    // 判据「框架行为不动」：emit 仍把档位推入 `Window` 并整帧置脏（既有 CSD/根主题链的依赖面）。
    // 基线自证：接线态先置脏再断言，否则「解绑后无通知」区分不出「已解绑」与「从未接线」。
    Application app{make_scene(), make_headless_window("os_framework_leg")};
    WindowHost *const host = app.windows().front();
    HeadlessSurface *const surface = surface_of(host);
    AURORA_TEST_REQUIRE_NOT_NULL(surface);
    AURORA_TEST_REQUIRE_NOT_NULL(host->window());

    std::vector<ColorScheme> seen;
    app.set_on_os_color_scheme([&seen](ColorScheme s) -> void { seen.push_back(s); });

    host->window()->set_os_color_scheme(ColorScheme::Light);
    (void)host->render_frame(0.016);  // 消费残脏，回到无脏稳态
    AURORA_TEST_CHECK_FALSE(host->window()->has_pending_dirty());

    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_CHECK_TRUE(host->window()->has_pending_dirty());  // 框架那一半照常置脏
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);  // 应用级同时收到一次
    (void)host->render_frame(0.016);

    host->teardown();
    surface->emit_os_color_scheme_change(ColorScheme::Light);
    AURORA_TEST_CHECK_FALSE(host->window()->has_pending_dirty());  // 框架腿已解绑
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);  // 应用级上报表同步解绑
}

AURORA_TEST_CASE(headless_host_without_window_still_aggregates_nothing) {
    // 无头宿主（无 `Window`）没有 Surface 通道，也不该凭空产生通知；
    // 同应用里带窗宿主的切换照常聚合（混合宿主场景）。
    Application app{make_scene(), 320, 240};  // 无 Window 的宿主
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
    AURORA_TEST_CHECK_NULL(app.window());

    auto *with_window = add_second_window(app, "os_mixed_host");
    AURORA_TEST_REQUIRE_NOT_NULL(with_window);
    int calls = 0;
    app.set_on_os_color_scheme([&calls](ColorScheme) -> void { ++calls; });

    HeadlessSurface *const surface = surface_of(with_window);
    AURORA_TEST_REQUIRE_NOT_NULL(surface);
    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(app.os_color_scheme_state().get(), ColorScheme::Dark);
}

}  // namespace aurora::test_cases::utest_app_os_color_scheme
