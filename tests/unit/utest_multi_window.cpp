/// 测试类型: unit
/// 目标单元: include/aurora/app/application.h, include/aurora/app/window_host.h
/// 测试说明: 多窗口（specification/06-app-platform.md §2.4）——宿主登记与主窗口语义、Scene/FocusManager
/// 逐窗隔离、逐窗独立渲染、帧统计隔离与单窗口全局单例兼容、关闭一个窗口不影响其他窗口的帧循环回收。
/// 全部以 Headless 多实例驱动（无 OS 窗口），不依赖任何真实后端。

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_multi_window {

namespace {

auto make_scene(const std::string &label) -> Scene { return Scene{Node{std::make_shared<Text>(label)}}; }

/// @brief 产出一个 Headless 窗口（多实例安全，无 OS 状态）。
auto make_window(std::string title, int frames = -1) -> std::unique_ptr<Window> {
    HeadlessOptions opts;
    static_cast<WindowOptions &>(opts) = WindowOptions{};
    opts.title = std::move(title);
    opts.size = Size{.width = 200.0F, .height = 150.0F};
    opts.max_frames = frames;
    auto res = create_window(opts);
    return res ? std::move(res.value()) : nullptr;
}

/// @brief 取宿主背后的 HeadlessSurface（测试观测点：present 帧数、程序化关闭）。
auto headless_of(WindowHost *host) -> HeadlessSurface * {
    if (host == nullptr || host->window() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<HeadlessSurface *>(&host->window()->surface());
}

}  // namespace

AURORA_TEST_CASE(open_window_registers_distinct_hosts) {
    Application app{make_scene("main"), 320, 240};

    const WindowId first = app.window_host(app.windows().front()->id())->id();  // 无头主宿主
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
    // 主宿主无 Window：既有 `window()` 语义（无头构造返回 nullptr）保持不变。
    AURORA_TEST_CHECK_NULL(app.window());

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    const WindowId id_a = app.open_window(std::move(wa), make_scene("A"));
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));

    AURORA_TEST_CHECK_EQ(app.window_count(), 3U);
    // id 唯一、非哨兵，且互不相等。
    AURORA_TEST_CHECK_NE(id_a, AURORA_INVALID_WINDOW_ID);
    AURORA_TEST_CHECK_NE(id_a, id_b);
    AURORA_TEST_CHECK_NE(first, id_a);
    // 按 id 可检索到宿主；未知 id 返回 nullptr。
    AURORA_TEST_CHECK_NOT_NULL(app.window_host(id_a));
    AURORA_TEST_CHECK_NOT_NULL(app.window_host(id_b));
    AURORA_TEST_CHECK_NULL(app.window_host(static_cast<WindowId>(99999)));
}

AURORA_TEST_CASE(scene_and_focus_are_per_window) {
    auto wa = make_window("main");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("main"), std::move(wa)};
    auto wb = make_window("aux");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("aux"));

    WindowHost *const main = app.window_host(app.windows().front()->id());
    WindowHost *const aux = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(aux);

    // 每个宿主持有**独立的** Scene 与 FocusManager：地址即隔离证据。
    AURORA_TEST_CHECK_NE(&main->scene(), &aux->scene());
    AURORA_TEST_CHECK_NE(&main->focus(), &aux->focus());
    // `scene()` / `focus()` 访问器作用于主窗口。
    AURORA_TEST_CHECK_EQ(&app.scene(), &main->scene());
    AURORA_TEST_CHECK_EQ(&app.focus(), &main->focus());
    // 两个窗口各自的 UI 树互不相干。
    AURORA_TEST_CHECK_EQ(std::string{main->scene().root().type_name()}, "Text");
    AURORA_TEST_CHECK_EQ(std::string{aux->scene().root().type_name()}, "Text");
    // 焦点初值各自为空，且不共享槽位。
    AURORA_TEST_CHECK_NULL(main->focus().focused());
    AURORA_TEST_CHECK_NULL(aux->focus().focused());
}

AURORA_TEST_CASE(each_window_renders_independently) {
    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("A"), std::move(wa)};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));

    WindowHost *const a = app.windows().front();
    WindowHost *const b = app.window_host(id_b);
    HeadlessSurface *const sa = headless_of(a);
    HeadlessSurface *const sb = headless_of(b);
    AURORA_TEST_REQUIRE_NOT_NULL(sa);
    AURORA_TEST_REQUIRE_NOT_NULL(sb);

    const int before_a = sa->frame_count();
    const int before_b = sb->frame_count();
    (void)a->render_frame(0.016);
    (void)b->render_frame(0.016);

    // 两个窗口各自 present 一次，互不干扰。
    AURORA_TEST_CHECK_EQ(sa->frame_count(), before_a + 1);
    AURORA_TEST_CHECK_EQ(sb->frame_count(), before_b + 1);
}

AURORA_TEST_CASE(frame_stats_isolate_only_from_second_window) {
    auto wa = make_window("single");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("single"), std::move(wa)};
    WindowHost *const single = app.windows().front();

    // 单窗口必须与历史一致：统计落入全局单例（既有 PerfOverlay / bench / 性能集成测试直读之）。
    AURORA_TEST_CHECK_EQ(&single->frame_stats(), &FrameStats::instance());

    auto wb = make_window("second");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("second"));
    WindowHost *const second = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(second);

    // 多窗口后各宿主切换到自有实例，且彼此不同。
    AURORA_TEST_CHECK_NE(&single->frame_stats(), &FrameStats::instance());
    AURORA_TEST_CHECK_NE(&second->frame_stats(), &FrameStats::instance());
    AURORA_TEST_CHECK_NE(&single->frame_stats(), &second->frame_stats());
}

AURORA_TEST_CASE(closing_one_window_keeps_others_running) {
    WindowOptions opts;
    opts.max_frames = 3;
    opts.power_saving = false;  // 无 OS 等待通道：退回忙轮询，帧数确定

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    auto *sa = dynamic_cast<HeadlessSurface *>(&wa->surface());
    Application app{make_scene("A"), std::move(wa), opts};

    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    auto *sb = dynamic_cast<HeadlessSurface *>(&wb->surface());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));
    AURORA_TEST_REQUIRE_NOT_NULL(sa);
    AURORA_TEST_REQUIRE_NOT_NULL(sb);

    // 模拟「用户点了 B 的 ×」：只有 B 进入关闭状态，A 不受影响。
    sb->set_should_close(true);
    AURORA_TEST_CHECK_EQ(app.window_count(), 2U);

    const int before_a = sa->frame_count();
    app.run();

    // 帧末回收：B 被摘除，A 保留并继续渲染（多窗口帧循环的核心收益）。
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
    AURORA_TEST_CHECK_NULL(app.window_host(id_b));
    AURORA_TEST_CHECK_GT(sa->frame_count(), before_a);
    AURORA_TEST_CHECK_NOT_NULL(app.window());
}

AURORA_TEST_CASE(close_window_requests_reap_at_frame_end) {
    WindowOptions opts;
    opts.max_frames = 2;
    opts.power_saving = false;

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("A"), std::move(wa), opts};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));

    // 程序化关闭：请求立即生效，回收在帧末（不在调用点销毁宿主）。
    app.close_window(id_b);
    AURORA_TEST_CHECK_NOT_NULL(app.window_host(id_b));

    app.run();
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
    AURORA_TEST_CHECK_NULL(app.window_host(id_b));
    // 未知 id 的关闭请求是安全的 no-op。
    app.close_window(static_cast<WindowId>(4242));
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
}

AURORA_TEST_CASE(headless_host_never_owns_a_surface) {
    Application app{make_scene("headless"), 320, 240};
    WindowHost *const host = app.windows().front();

    AURORA_TEST_CHECK_FALSE(host->has_window());
    AURORA_TEST_CHECK_NULL(host->window());
    // 无 OS 窗口的宿主：渲染与 pump 均为安全 no-op，且不请求关闭。
    (void)host->render_frame(0.016);
    host->pump_events();
    AURORA_TEST_CHECK_FALSE(host->should_close());
    // 程序化关闭是**后端无关**的兜底通道：`Surface::close()` 在不少后端是空实现
    // （Headless 无 OS 关闭语义），故 `request_close()` 必须自身置位，否则关不掉。
    host->request_close();
    AURORA_TEST_CHECK_TRUE(host->should_close());
    // Scene 仍可用（render_to_png 与程序化派发路径）。
    AURORA_TEST_CHECK_EQ(std::string{host->scene().root().type_name()}, "Text");
}

// ---- 关闭闸门（销毁前可否决的公共入口）----
//
// 底座与既有用例同款：双窗无头 + `power_saving=false` + `max_frames` 有限帧，
// 用 `HeadlessSurface::set_should_close(true)` 模拟「用户点了 ×」。

AURORA_TEST_CASE(close_gate_veto_keeps_window_alive) {
    WindowOptions opts;
    opts.max_frames = 4;
    opts.power_saving = false;

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("A"), std::move(wa), opts};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    auto *sb = dynamic_cast<HeadlessSurface *>(&wb->surface());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));
    AURORA_TEST_REQUIRE_NOT_NULL(sb);

    int asked = 0;
    int closed = 0;
    app.set_on_window_closed([&closed](WindowId) -> void { ++closed; });
    app.set_on_close_requested([&asked](WindowId, CloseRequestSource src) -> bool {
        ++asked;
        // 纯后端置位（未经 request_close 记账）⇒ 判为 OperatingSystem。
        AURORA_TEST_CHECK_EQ(src, CloseRequestSource::OperatingSystem);
        return false;  // 恒否决
    });

    sb->set_should_close(true);  // 模拟用户点 B 的 ×
    app.run();

    // 无闸门时 B 在首帧即被回收；装了恒否决的闸门后 B 存活且只问一次（边沿闩）。
    AURORA_TEST_CHECK_EQ(asked, 1);
    AURORA_TEST_CHECK_EQ(closed, 0);
    AURORA_TEST_CHECK_EQ(app.window_count(), 2U);
    AURORA_TEST_CHECK_NOT_NULL(app.window_host(id_b));
    AURORA_TEST_CHECK_FALSE(app.window_host(id_b)->should_close());  // 记账位与后端旗标双清
}

AURORA_TEST_CASE(close_gate_reasks_on_a_new_edge) {
    WindowOptions opts;
    opts.max_frames = 4;
    opts.power_saving = false;

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("A"), std::move(wa), opts};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    auto *sb = dynamic_cast<HeadlessSurface *>(&wb->surface());
    app.open_window(std::move(wb), make_scene("B"));
    AURORA_TEST_REQUIRE_NOT_NULL(sb);

    int asked = 0;
    int frames = 0;
    app.set_on_close_requested([&asked](WindowId, CloseRequestSource) -> bool {
        ++asked;
        return false;  // 恒否决
    });
    app.set_on_frame([&frames, sb]() -> void {
        ++frames;
        if (frames == 2) {
            sb->set_should_close(true);  // 否决后再置位 ⇒ 新的一条抬沿
        }
    });

    sb->set_should_close(true);
    app.run();

    AURORA_TEST_CHECK_EQ(asked, 2);  // 两次独立请求 ⇒ 问两次（不是每帧都问）
    AURORA_TEST_CHECK_EQ(app.window_count(), 2U);  // 两次都被撤销
}

AURORA_TEST_CASE(close_gate_approve_reaps_once) {
    WindowOptions opts;
    opts.max_frames = 3;
    opts.power_saving = false;

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("A"), std::move(wa), opts};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    auto *sb = dynamic_cast<HeadlessSurface *>(&wb->surface());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));
    AURORA_TEST_REQUIRE_NOT_NULL(sb);

    int asked = 0;
    int closed = 0;
    app.set_on_window_closed([&closed](WindowId) -> void { ++closed; });
    app.set_on_close_requested([&asked](WindowId, CloseRequestSource) -> bool {
        ++asked;
        return true;  // 恒放行
    });

    sb->set_should_close(true);
    app.run();

    AURORA_TEST_CHECK_EQ(asked, 1);
    AURORA_TEST_CHECK_EQ(closed, 1);  // 放行 ⇒ 当帧回收，关闭回调恰一次
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
    AURORA_TEST_CHECK_NULL(app.window_host(id_b));
}

AURORA_TEST_CASE(close_gate_reports_programmatic_source) {
    WindowOptions opts;
    opts.max_frames = 3;
    opts.power_saving = false;

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("A"), std::move(wa), opts};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));

    int asked = 0;
    CloseRequestSource got = CloseRequestSource::OperatingSystem;
    app.set_on_close_requested([&asked, &got](WindowId, CloseRequestSource src) -> bool {
        ++asked;
        got = src;
        return false;
    });

    app.close_window(id_b);  // 程序化关闭 ⇒ 经 request_close 记账
    app.run();

    AURORA_TEST_CHECK_EQ(asked, 1);
    AURORA_TEST_CHECK_EQ(got, CloseRequestSource::Programmatic);  // 与「后端置位 ⇒ OS」区分开
    AURORA_TEST_CHECK_EQ(app.window_count(), 2U);
}

AURORA_TEST_CASE(close_gate_policy_cascade_asks_each_window) {
    WindowOptions opts;
    opts.max_frames = 4;
    opts.power_saving = false;

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    auto *sa = dynamic_cast<HeadlessSurface *>(&wa->surface());
    Application app{make_scene("A"), std::move(wa), opts};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));
    AURORA_TEST_REQUIRE_NOT_NULL(sa);

    app.set_exit_policy(ExitPolicy::MainWindowClosed);
    int frames = 0;
    std::vector<std::pair<WindowId, CloseRequestSource>> asked;
    app.set_on_close_requested([&asked](WindowId id, CloseRequestSource src) -> bool {
        asked.emplace_back(id, src);
        return false;  // 全部否决
    });
    // 关闭必须在**帧内**触发：主窗旗标若在 run() 之前就置位，MainWindowClosed 下 should_exit()
    // 立即为真、循环一帧都不推进，闸门也就没有机会被问（该边界已写入提案的不做清单）。
    app.set_on_frame([&frames, sa]() -> void {
        ++frames;
        if (frames == 1) {
            sa->set_should_close(true);  // 主窗被请求关闭
        }
    });

    app.run();

    // 主窗（后端置位 ⇒ OS）与连带扇（PolicyCascade）各被问一次；全否 ⇒ 无一扇被收。
    AURORA_TEST_REQUIRE_EQ(asked.size(), 2U);  // 空容器上 front() 是 UB：先致命断言再取元素
    AURORA_TEST_CHECK_EQ(asked.front().second, CloseRequestSource::OperatingSystem);
    AURORA_TEST_CHECK_EQ(asked.front().first, app.main_window());
    AURORA_TEST_CHECK_EQ(asked.back().second, CloseRequestSource::PolicyCascade);
    AURORA_TEST_CHECK_EQ(asked.back().first, id_b);
    AURORA_TEST_CHECK_EQ(app.window_count(), 2U);
    AURORA_TEST_CHECK_NOT_NULL(app.window_host(id_b));
    AURORA_TEST_CHECK_EQ(frames, 4);  // 未提前退出（无闸门时首帧即退出）
}

AURORA_TEST_CASE(close_gate_throwing_callback_is_fail_open) {
    WindowOptions opts;
    opts.max_frames = 3;
    opts.power_saving = false;

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("A"), std::move(wa), opts};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    auto *sb = dynamic_cast<HeadlessSurface *>(&wb->surface());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));
    AURORA_TEST_REQUIRE_NOT_NULL(sb);

    app.set_on_close_requested([](WindowId, CloseRequestSource) -> bool {
        throw std::runtime_error("gate boom");  // 回调抛异常
    });
    sb->set_should_close(true);

    bool escaped = false;  // 异常绝不穿出 step_frame / run()
    try {
        app.run();
    } catch (...) {
        escaped = true;
    }
    AURORA_TEST_CHECK_FALSE(escaped);
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);  // fail-open：按放行处理
    AURORA_TEST_CHECK_NULL(app.window_host(id_b));
}

}  // namespace aurora::test_cases::utest_multi_window
