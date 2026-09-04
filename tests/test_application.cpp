// 目标源单元：app/application.h + src/aurora/app/application.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_app.cpp
//   - test_idle_loop.cpp
//   - test_present_skip.cpp
//   - test_platform.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// window/platform.h(platform()/App() 能力探测，sec_test_platform 段)。

#include <iostream>
#include <memory>
#include <string>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora/window/frame_pacing.h"
#include "aurora/window/platform.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "test_harness.h"

namespace aurora::tests::sec_app {

static auto make_deep(int n) -> Node {
    if (n <= 0) {
        return Node{Text{"leaf"}};
    }
    return Node{Column{make_deep(n - 1)}};
}

static void test_application() {
    bool clicked = false;
    Button btn{"OK"};
    btn.on_click = [&]() -> void { clicked = true; };
    Node root = std::move(btn);
    Scene scene{root};
    Application app{std::move(scene), 200, 100};

    app.tick();  // 不崩溃
    AURORA_TEST_CHECK_MSG(true, "Application: tick no crash");

    // 先渲染一次完成 mount→layout→paint，使控件 bounds 落定，方可命中测试。
    auto pr = app.render_to_png("app_test_out.png");
    AURORA_TEST_CHECK_MSG(pr.ok(), "Application: render_to_png ok (pre-click layout)");

    // 取按钮中心：click = Press→Release，仅 Move 不应触发（命中链派发）。
    [[maybe_unused]] auto &btn_w = dynamic_cast<Button &>(app.scene().root());
    const Rect bb = app.scene().root_node().bounds();
    const Point center{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};

    app.dispatch_pointer(center.x, center.y, MouseAction::Press);
    app.dispatch_pointer(center.x, center.y, MouseAction::Move);
    app.dispatch_pointer(center.x, center.y, MouseAction::Release);
    AURORA_TEST_CHECK_MSG(clicked, "Application: click (Press+Release) triggers Button on_click");

    // 键盘 / 文本事件
    KeyEvent k;
    k.key_ = static_cast<int>(KeyCode::Tab);
    bool kr = app.dispatch_key(k);
    (void)kr;
    AURORA_TEST_CHECK_MSG(true, "Application: dispatch_key no crash");
    TextInputEvent t;
    t.text_ = "a";
    bool tr = app.dispatch_text(t);
    (void)tr;
    AURORA_TEST_CHECK_MSG(true, "Application: dispatch_text no crash");

    // 渲染到 PNG 并回读校验
    auto r = app.render_to_png("app_test_out.png");
    AURORA_TEST_CHECK_MSG(r.ok(), "Application: render_to_png ok");
    auto lr = Image::load("app_test_out.png");
    AURORA_TEST_CHECK_MSG(lr.ok() && lr.value().width == 200 && lr.value().height == 100,
                          "Application: rendered PNG decodes with right size");
}

static void test_validate() {
    Node good = Column{Node{Text{"A"}}, Node{Button{"B"}}};
    auto vr = validate(good);
    AURORA_TEST_CHECK_MSG(vr.ok(), "validate: valid tree passes");

    auto deep = make_deep(70);
    auto dr = validate(deep);
    AURORA_TEST_CHECK_MSG(!dr.ok(), "validate: deep tree fails depth check");
}

static void run() {
    AURORA_TEST_PRINTF("=== app_test ===\n");
    test_application();
    test_validate();
}
}  // namespace aurora::tests::sec_app

namespace aurora::tests::sec_idle_loop {

static void run() {
    // ---- 1. 静态场景跑 N 帧：首帧渲染，其余全部 idle 跳过 ----
    {
        FrameStats::instance().reset();
        Scene scene{Text("static ui")};
        auto surface = std::make_unique<HeadlessSurface>();
        (void)surface->begin_frame(320, 240);
        WindowOptions opts;
        opts.size = Size{.width = 320.0F, .height = 240.0F};
        opts.max_frames = 30;
        Application app{std::move(scene), std::move(surface), opts};
        AURORA_TEST_CHECK(app.window() != nullptr);
        app.run();  // Headless wait_events 为 no-op：有限循环快速跑完，不引入等待
        const auto &s = FrameStats::instance();
        // 30 帧中仅首帧真实渲染，其余 29 帧应为 idle 跳过
        AURORA_TEST_CHECK_GE(s.idle_frame_count(), 29U);
        AURORA_TEST_CHECK_EQ(app.window()->surface().frame_count(), 1);
    }

    // ---- 2. has_pending_dirty：首帧前有、渲染后无、标脏后有 ----
    {
        auto surface = std::make_unique<HeadlessSurface>();
        (void)surface->begin_frame(320, 240);
        Window win{std::move(surface)};
        AURORA_TEST_CHECK_TRUE(win.has_pending_dirty());  // 首帧未绘：视为有脏
        Node root = Text("hello");
        AURORA_TEST_CHECK(win.present_root(root).ok());
        AURORA_TEST_CHECK_FALSE(win.has_pending_dirty());  // 渲染完成：稳态无脏
        win.mark_dirty(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 10.0F, .height = 10.0F}});
        AURORA_TEST_CHECK_TRUE(win.has_pending_dirty());  // 手动标脏：下一帧需渲染
        // 脏追踪关闭：视为永远有脏（每帧全绘，仅受帧预算节流）
        win.enable_dirty_tracking(false);
        AURORA_TEST_CHECK_TRUE(win.has_pending_dirty());
    }

    // ---- 3. 静态稳态的调度决策：无脏/无动画/无定时任务 → 无限等待 ----
    {
        auto surface = std::make_unique<HeadlessSurface>();
        (void)surface->begin_frame(320, 240);
        Window win{std::move(surface)};
        Node root = Text("idle");
        AURORA_TEST_CHECK(win.present_root(root).ok());
        Animator anim;
        Scheduler sched;
        const double wait =
            compute_wait_timeout(win.has_pending_dirty(), anim.has_active(), sched.next_deadline_ms(), 16.67, 1.0);
        AURORA_TEST_CHECK(wait < 0.0);  // 无限等待：真实后端将阻塞睡眠，CPU 趋近 0
        // Animator/Scheduler 空闲信息
        AURORA_TEST_CHECK_FALSE(anim.has_active());
        AURORA_TEST_CHECK(sched.next_deadline_ms() < 0.0);
        auto h = sched.set_timeout(std::chrono::milliseconds(500), []() -> void {});
        AURORA_TEST_CHECK_NEAR(static_cast<float>(sched.next_deadline_ms()), 500.0F, 1.0F);
        h.cancel();
        AURORA_TEST_CHECK(sched.next_deadline_ms() < 0.0);  // 取消后不再唤醒
    }

    // ---- 4. FrameStats 唤醒/睡眠观测 ----
    {
        FrameStats::instance().reset();
        AURORA_TEST_CHECK_EQ(FrameStats::instance().wakeup_count(), 0U);
        AURORA_TEST_CHECK_NEAR(static_cast<float>(FrameStats::instance().sleep_ratio()), 0.0F, 1e-6F);
        FrameStats::instance().record_wait(10.0);
        FrameStats::instance().record_wait(10.0);
        AURORA_TEST_CHECK_EQ(FrameStats::instance().wakeup_count(), 2U);
        AURORA_TEST_CHECK(FrameStats::instance().sleep_ratio() >= 0.0);
        AURORA_TEST_CHECK(FrameStats::instance().sleep_ratio() <= 1.0);
        FrameStats::instance().reset();  // 不污染后续测试
    }
}
}  // namespace aurora::tests::sec_idle_loop

namespace aurora::tests::sec_present_skip {

namespace {

/// @brief 统计 layout/paint 调用次数的间谍控件，用于断言「仅 paint 脏时跳过整树重排」。
class SpyWidget : public LeafWidget {
  public:
    int layout_calls_ = 0;
    int paint_calls_ = 0;

    [[nodiscard]] auto type_name() const -> const char * override { return "SpyWidget"; }
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor { return WidgetDescriptor{.name = "SpyWidget"}; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        ++layout_calls_;
        return c.constrain(Size{.width = 100.0F, .height = 30.0F});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        ++paint_calls_;
        p.fill_rect(bounds, Color{200, 200, 200, 255});
    }
};

/// @brief 创建已定尺寸的 Headless 窗口，并返回窗口与底层 surface 裸指针（供 resize 测试）。
auto make_sized_window(int w, const int h, HeadlessSurface *&out_raw) -> Window {
    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    out_raw = surface.get();  // NOLINT
    return Window{std::move(surface)};
}

}  // namespace

static void run() {
    // ---- 1. idle 跳帧：无脏/无尺寸变化/同根 → 整帧跳过（frame_count 不变）----
    {
        HeadlessSurface *raw = nullptr;
        auto win = make_sized_window(512, 512, raw);
        auto spy = std::make_shared<SpyWidget>();
        Node root = std::static_pointer_cast<Widget>(spy);  // 间谍控件直接作为根
        auto r1 = win.present_root(root);
        AURORA_TEST_CHECK(r1.ok());
        AURORA_TEST_CHECK(raw->frame_count() == 1);
        // 第二帧 idle：无任何变更 → 应被整帧跳过（不调用 present）
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
        AURORA_TEST_CHECK(raw->frame_count() == 1);
    }

    // ---- 2. 脏变更触发重绘：mark_needs_paint 应使下一帧 present ----
    {
        HeadlessSurface *raw = nullptr;
        auto win = make_sized_window(512, 512, raw);
        auto spy = std::make_shared<SpyWidget>();
        Node root = std::static_pointer_cast<Widget>(spy);
        auto r1 = win.present_root(root);
        AURORA_TEST_CHECK(r1.ok());
        AURORA_TEST_CHECK(raw->frame_count() == 1);
        spy->mark_needs_paint();  // 模拟外观变更（如选中态）
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
        AURORA_TEST_CHECK(raw->frame_count() == 2);
    }

    // ---- 3. layout/paint 分离：仅 paint 脏时跳过整树重排（layout 计数不变）----
    {
        HeadlessSurface *raw = nullptr;  // NOLINT
        auto win = make_sized_window(512, 512, raw);
        auto spy = std::make_shared<SpyWidget>();
        Node root = std::static_pointer_cast<Widget>(spy);
        auto r1 = win.present_root(root);
        AURORA_TEST_CHECK(r1.ok());
        AURORA_TEST_CHECK(spy->layout_calls_ == 1);
        AURORA_TEST_CHECK(spy->paint_calls_ == 1);
        spy->mark_needs_paint();  // 仅外观变更：不应触发重排
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
        AURORA_TEST_CHECK(spy->layout_calls_ == 1);  // 重排被跳过
        AURORA_TEST_CHECK(spy->paint_calls_ == 2);  // 仅重绘
    }

    // ---- 4. mark_needs_layout 应触发整树重排（对照，确保分离是单向的）----
    {
        HeadlessSurface *raw = nullptr;  // NOLINT
        auto win = make_sized_window(512, 512, raw);
        auto spy = std::make_shared<SpyWidget>();
        Node root = std::static_pointer_cast<Widget>(spy);
        auto r1 = win.present_root(root);
        AURORA_TEST_CHECK(r1.ok());
        AURORA_TEST_CHECK(spy->layout_calls_ == 1);
        AURORA_TEST_CHECK(spy->paint_calls_ == 1);
        spy->mark_needs_layout();  // 布局变更：应重排
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
        AURORA_TEST_CHECK(spy->layout_calls_ == 2);  // 重排发生
        AURORA_TEST_CHECK(spy->paint_calls_ == 2);
    }

    // ---- 5. resize 强制全绘：尺寸变化 → 下一帧必须重绘 ----
    {
        HeadlessSurface *raw = nullptr;
        auto win = make_sized_window(512, 512, raw);
        auto spy = std::make_shared<SpyWidget>();
        Node root = std::static_pointer_cast<Widget>(spy);
        auto r1 = win.present_root(root);
        AURORA_TEST_CHECK(r1.ok());
        AURORA_TEST_CHECK(raw->frame_count() == 1);
        (void)raw->begin_frame(400, 300);  // 模拟窗口缩放/最大化
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
        AURORA_TEST_CHECK(raw->frame_count() == 2);
    }

    // ---- 6. 系统重绘请求（最小化还原后的 WM_PAINT）：idle 跳帧时仍须重新上屏 ----
    // 回归：窗口表面被 OS 置无效后，脏追踪判定「无脏/尺寸未变」直接 return，
    // present 不被调用 → 还原后停留在类背景刷底色（白屏）。修复后：重绘请求驱动的
    // 跳帧分支全量 blit 重新上屏（frame_count 增加），但不重绘（paint_calls 不变）。
    {
        HeadlessSurface *raw = nullptr;
        auto win = make_sized_window(512, 512, raw);
        auto spy = std::make_shared<SpyWidget>();
        Node root = std::static_pointer_cast<Widget>(spy);
        auto r1 = win.present_root(root);
        AURORA_TEST_CHECK(r1.ok());
        AURORA_TEST_CHECK(raw->frame_count() == 1);
        AURORA_TEST_CHECK(spy->paint_calls_ == 1);
        // 无任何脏变更，模拟 OS 要求重绘（如最小化还原）：必须重新 present 兜底
        raw->simulate_present_request();
        AURORA_TEST_CHECK(raw->frame_count() == 2);  // 重新上屏（修前：仍为 1，白屏）
        AURORA_TEST_CHECK(spy->paint_calls_ == 1);  // 帧缓冲仍有效，不重绘
        // 普通 idle 帧（非系统请求）仍正常跳帧，不因本修复退化为每帧上屏
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
        AURORA_TEST_CHECK(raw->frame_count() == 2);
    }
}
}  // namespace aurora::tests::sec_present_skip

namespace aurora::tests::sec_platform {

static void run() {
    const Platform p = platform();

    // 种类应与 auto_detect 一致。
    AURORA_TEST_CHECK(p.surface == auto_detect_surface());

    // 桌面平台：is_desktop 为真，device 为 Desktop。
    AURORA_TEST_CHECK(p.is_desktop());
    AURORA_TEST_CHECK(!p.is_mobile());
    AURORA_TEST_CHECK(p.device == DeviceKind::Desktop);

    // 能力标志与种类一致：真实显示 Surface 支持多点触控与高频率指针。
    const PlatformCapabilities c = p.capabilities();
    AURORA_TEST_CHECK(c.desktop);
    AURORA_TEST_CHECK(!c.mobile);
    if (p.surface == SurfaceKind::Win32 || p.surface == SurfaceKind::Glfw
#ifdef AURORA_BACKEND_X11
        || p.surface == SurfaceKind::X11
#endif
#ifdef AURORA_BACKEND_WAYLAND
        || p.surface == SurfaceKind::Wayland
#endif
#ifdef AURORA_BACKEND_MACOS
        || p.surface == SurfaceKind::MacOS
#endif
    ) {
        AURORA_TEST_CHECK(c.multitouch);
        AURORA_TEST_CHECK(c.high_frequency_pointer);
    } else {
        // Headless / 其他种类：无触摸能力。
        AURORA_TEST_CHECK(!c.multitouch);
        AURORA_TEST_CHECK(!c.high_frequency_pointer);
    }

    // App() 流式构建器可链式构造且不崩溃（不实际开窗口）。
    auto &&builder = App().title("Platform Test").size(320, 240);
    (void)builder;

    AURORA_LOG_INFO("test", "platform_test: ALL PASS (surface=", static_cast<int>(p.surface), ")");
}
}  // namespace aurora::tests::sec_platform

AURORA_TEST() {
    aurora::tests::sec_app::run();
    aurora::tests::sec_idle_loop::run();
    aurora::tests::sec_present_skip::run();
    aurora::tests::sec_platform::run();
}
