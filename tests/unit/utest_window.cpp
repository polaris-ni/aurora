/// 测试类型: unit
/// 目标单元: include/aurora/window/window.h
/// 测试说明: 覆盖 Window 纯逻辑路径——WindowOptions/HeadlessOptions 默认值不变量、
/// 标题/尺寸/装饰内边距与帧生命周期向 Surface 的转发、程序化窗口控制、
/// 脏追踪开关语义、HUD 叠加层槽位、run 帧数上限与空 Surface 工厂拒绝

#include <memory>
#include <optional>
#include <string>

#include "aurora/widget/spacer.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window {

namespace {

/// @brief 记录型 Surface 桩：纯内存实现，记录 Window 转发的调用与参数。
class RecordingSurface final : public Surface {
  public:
    Size size_val{.width = 320.0F, .height = 240.0F};
    EdgeInsets inset_val{};
    std::string last_title;
    bool close_requested = false;
    bool close_called = false;
    bool minimize_called = false;
    bool toggle_maximize_called = false;
    int fullscreen_last = -1;  // -1 未调用 / 1 true / 0 false
    bool begin_move_called = false;
    std::optional<WindowResizeEdge> resize_last;
    std::optional<TitleBarStyle> style_last;
    std::shared_ptr<Image> icon_last;
    int begin_w = 0;
    int begin_h = 0;
    int present_count = 0;
    int pump_count = 0;

    auto begin_frame(int width, int height) -> Result<bool> override {
        begin_w = width;
        begin_h = height;
        return Result<bool>{true};
    }
    auto painter() -> Painter & override { return painter_; }
    auto present() -> Result<bool> override {
        ++present_count;
        return Result<bool>{true};
    }
    auto size() const -> Size override { return size_val; }
    auto should_close() const -> bool override { return close_requested; }
    auto poll_platform_events() -> void override { ++pump_count; }
    auto set_title(const std::string &title) -> void override { last_title = title; }
    auto content_inset() const -> EdgeInsets override { return inset_val; }
    auto close() -> void override { close_called = true; }
    auto minimize() -> void override { minimize_called = true; }
    auto toggle_maximize() -> void override { toggle_maximize_called = true; }
    auto set_fullscreen(bool on) -> void override { fullscreen_last = on ? 1 : 0; }
    auto begin_window_move() -> void override { begin_move_called = true; }
    auto begin_window_resize(WindowResizeEdge edge) -> void override { resize_last = edge; }
    auto set_title_bar_style(const TitleBarStyle &style) -> void override { style_last = style; }
    auto set_title_bar_icon(const std::shared_ptr<Image> &icon) -> void override { icon_last = icon; }

  private:
    Painter painter_;
};

}  // namespace

AURORA_TEST_CASE(window_options_defaults) {
    // 跨后端共享选项的默认值不变量（AI 消费者据此可零配置开窗）。
    const WindowOptions opts;
    AURORA_TEST_CHECK_NEAR(opts.size.width, 800.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.size.height, 600.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(opts.title, std::string{"Aurora"});
    AURORA_TEST_CHECK_EQ(opts.max_frames, -1);
    AURORA_TEST_CHECK_EQ(opts.max_fps, 60);
    AURORA_TEST_CHECK_TRUE(opts.power_saving);
    AURORA_TEST_CHECK_EQ(opts.renderer, RendererPreference::Auto);
    // 高级样式默认：普通可缩放窗口 + 自动装饰策略。
    AURORA_TEST_CHECK_FALSE(opts.style.always_on_top);
    AURORA_TEST_CHECK_FALSE(opts.style.frameless);
    AURORA_TEST_CHECK_FALSE(opts.style.transparent);
    AURORA_TEST_CHECK_TRUE(opts.style.resizable);
    AURORA_TEST_CHECK_EQ(opts.style.decoration, DecorationPolicy::Auto);
    AURORA_TEST_CHECK_NEAR(opts.style.min_size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.style.max_size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.style.title_bar.height, 36.0F, 1e-4F);
    // Headless 专属选项：继承通用默认，png_path 默认留内存。
    const HeadlessOptions headless;
    AURORA_TEST_CHECK_TRUE(headless.png_path.empty());
    AURORA_TEST_CHECK_EQ(headless.renderer, RendererPreference::Auto);
    AURORA_TEST_CHECK_NEAR(headless.size.width, 800.0F, 1e-4F);
}

AURORA_TEST_CASE(window_title_defaults_and_forwards_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window w{std::move(stub)};
    AURORA_TEST_CHECK_EQ(w.title(), std::string{"Aurora"});
    w.set_title("Hello");
    AURORA_TEST_CHECK_EQ(w.title(), std::string{"Hello"});
    // set_title 同步下发到后端（Win32 经 SetWindowText 生效）。
    AURORA_TEST_CHECK_EQ(surf.last_title, std::string{"Hello"});
}

AURORA_TEST_CASE(window_size_and_content_inset_delegate_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window w{std::move(stub)};
    const Size sz = w.size();
    AURORA_TEST_CHECK_NEAR(sz.width, 320.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sz.height, 240.0F, 1e-4F);
    // 默认无装饰：安全区内边距全零。
    const EdgeInsets default_inset = w.content_inset();
    AURORA_TEST_CHECK_NEAR(default_inset.horizontal(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(default_inset.vertical(), 0.0F, 1e-4F);
    // 后端报告 CSD 装饰时逐字段镜像（应用据此下沉根布局）。
    surf.inset_val = EdgeInsets{.left = 2.0F, .top = 36.0F, .right = 2.0F, .bottom = 0.0F};
    const EdgeInsets inset = w.content_inset();
    AURORA_TEST_CHECK_NEAR(inset.top, 36.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.left, 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.bottom, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(window_frame_lifecycle_delegates_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window w{std::move(stub)};
    // begin_frame 以窗口逻辑尺寸（取整）启动后端帧。
    const auto bf = w.begin_frame();
    AURORA_TEST_CHECK_TRUE(bf.ok());
    AURORA_TEST_CHECK_TRUE(bf.value());
    AURORA_TEST_CHECK_EQ(surf.begin_w, 320);
    AURORA_TEST_CHECK_EQ(surf.begin_h, 240);
    // present 提交当前帧（无头后端即计数 +1）。
    const auto pr = w.present();
    AURORA_TEST_CHECK_TRUE(pr.ok());
    AURORA_TEST_CHECK_TRUE(pr.value());
    AURORA_TEST_CHECK_EQ(surf.present_count, 1);
}

AURORA_TEST_CASE(window_control_actions_forward_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window w{std::move(stub)};
    w.minimize();
    w.toggle_maximize();
    w.set_fullscreen(true);
    w.begin_window_move();
    w.begin_window_resize(WindowResizeEdge::BottomRight);
    AURORA_TEST_CHECK_TRUE(surf.minimize_called);
    AURORA_TEST_CHECK_TRUE(surf.toggle_maximize_called);
    AURORA_TEST_CHECK_EQ(surf.fullscreen_last, 1);
    AURORA_TEST_CHECK_TRUE(surf.begin_move_called);
    AURORA_TEST_REQUIRE_TRUE(surf.resize_last.has_value());
    AURORA_TEST_CHECK_EQ(*surf.resize_last, WindowResizeEdge::BottomRight);
    w.set_fullscreen(false);
    AURORA_TEST_CHECK_EQ(surf.fullscreen_last, 0);
    // 关闭流：close() 转发，后端置关闭请求后 should_close() 为真。
    AURORA_TEST_CHECK_FALSE(w.should_close());
    w.close();
    AURORA_TEST_CHECK_TRUE(surf.close_called);
    surf.close_requested = true;
    AURORA_TEST_CHECK_TRUE(w.should_close());
}

AURORA_TEST_CASE(window_title_bar_style_and_icon_forward_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window w{std::move(stub)};
    TitleBarStyle style;
    style.height = 42.0F;
    style.button_layout = TitleBarButtonLayout::Windows;
    w.set_title_bar_style(style);
    AURORA_TEST_REQUIRE_TRUE(surf.style_last.has_value());
    AURORA_TEST_CHECK_NEAR(surf.style_last->height, 42.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(surf.style_last->button_layout, TitleBarButtonLayout::Windows);
    // 图标槽：空指针也走透传路径（清除图标语义）。
    w.set_title_bar_icon(nullptr);
    AURORA_TEST_CHECK_TRUE(surf.icon_last == nullptr);
}

AURORA_TEST_CASE(window_dirty_tracking_toggle_and_pending_dirty) {
    auto stub = std::make_unique<RecordingSurface>();
    Window w{std::move(stub)};
    // 默认开启脏追踪；首帧/布局脏使下一帧必绘。
    AURORA_TEST_CHECK_TRUE(w.dirty_tracking_enabled());
    AURORA_TEST_CHECK_TRUE(w.has_pending_dirty());
    AURORA_TEST_CHECK_TRUE(w.dirty_tracker().is_empty());
    const Rect dirty_rect{.origin = Point{.x = 10.0F, .y = 10.0F}, .size = Size{.width = 40.0F, .height = 30.0F}};
    w.mark_dirty(dirty_rect);
    AURORA_TEST_CHECK_FALSE(w.dirty_tracker().is_empty());
    w.force_full_redraw();
    AURORA_TEST_CHECK_TRUE(w.dirty_tracker().is_full());
    // 关闭后视为永远有脏（每帧全绘，仅受帧预算节流）。
    w.enable_dirty_tracking(false);
    AURORA_TEST_CHECK_FALSE(w.dirty_tracking_enabled());
    AURORA_TEST_CHECK_TRUE(w.has_pending_dirty());
    // 重新开启：清空脏集合并重置首帧标志（下一帧强制全绘）。
    w.enable_dirty_tracking(true);
    AURORA_TEST_CHECK_TRUE(w.dirty_tracking_enabled());
    AURORA_TEST_CHECK_TRUE(w.dirty_tracker().is_empty());
    AURORA_TEST_CHECK_TRUE(w.has_pending_dirty());
}

AURORA_TEST_CASE(window_overlay_slot_roundtrip) {
    auto stub = std::make_unique<RecordingSurface>();
    Window w{std::move(stub)};
    AURORA_TEST_CHECK_TRUE(w.overlay() == nullptr);
    const auto overlay = std::make_shared<Spacer>();
    w.set_overlay(overlay);
    AURORA_TEST_CHECK_TRUE(w.overlay() == overlay);
    // nullptr 关闭叠加层。
    w.set_overlay(nullptr);
    AURORA_TEST_CHECK_TRUE(w.overlay() == nullptr);
}

AURORA_TEST_CASE(window_run_executes_on_frame_exactly_max_frames_times) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window w{std::move(stub)};
    int frames = 0;
    // max_frames>0 有限循环：每帧 pump 事件 + 调 on_frame，到量即止（无头确定性驱动）。
    w.run([&frames]() -> void { ++frames; }, 3);
    AURORA_TEST_CHECK_EQ(frames, 3);
    AURORA_TEST_CHECK_EQ(surf.pump_count, 3);
}

AURORA_TEST_CASE(create_window_rejects_null_surface) {
    // 文档化契约：空 Surface 注入返回 Error（不崩溃），错误归属调用方。
    const auto result = create_window(std::unique_ptr<Surface>{});
    AURORA_TEST_CHECK_FALSE(result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::PlatformUnavailable);
}

}  // namespace aurora::test_cases::utest_window
