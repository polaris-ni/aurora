/// 测试类型: unit
/// 目标单元: include/aurora/window/surface.h
/// 测试说明: Surface 抽象契约（默认虚实现、非拷贝、快照/截屏默认路径、最小测试桩）与
/// HeadlessSurface 帧生命周期、PNG 落盘、状态/重绘 seam（Headless 部分受 AURORA_BACKEND_HEADLESS 门控）

#include <chrono>
#include <filesystem>
#include <system_error>
#include <type_traits>

#include "aurora/window/surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_surface {

namespace {

/// @brief 最小测试桩：仅实现 4 个纯虚接口，其余全部走 Surface 默认实现。
class MinimalSurface final : public Surface {
  public:
    auto begin_frame(int width, int height) -> Result<bool> override {
        size_ = Size{.width = static_cast<float>(width), .height = static_cast<float>(height)};
        return Result<bool>{true};
    }
    [[nodiscard]] auto painter() -> Painter& override { return painter_; }
    [[nodiscard]] auto present() -> Result<bool> override { return Result<bool>{true}; }
    [[nodiscard]] auto size() const -> Size override { return size_; }

  private:
    Painter painter_;
    Size size_{.width = 0.0F, .height = 0.0F};
};

}  // namespace

AURORA_TEST_CASE(surface_default_virtual_implementations) {
    MinimalSurface surface;
    AURORA_TEST_CHECK_NEAR(surface.scale_factor(), 1.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(surface.should_close());
    AURORA_TEST_CHECK_FALSE(surface.paces_frames());
    AURORA_TEST_CHECK_EQ(surface.frame_count(), 0);
    AURORA_TEST_CHECK_NULL(surface.data());
    AURORA_TEST_CHECK_NULL(surface.native_handle());

    // 默认底色：透明（begin_frame 不铺底色的后端契约）。
    const Color clear = surface.clear_color();
    AURORA_TEST_CHECK_EQ(clear.r, 0);
    AURORA_TEST_CHECK_EQ(clear.g, 0);
    AURORA_TEST_CHECK_EQ(clear.b, 0);
    AURORA_TEST_CHECK_EQ(clear.a, 0);

    // 默认装饰内边距：0（无装饰）。
    const EdgeInsets inset = surface.content_inset();
    AURORA_TEST_CHECK_NEAR(inset.left, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.top, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.right, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.bottom, 0.0F, 1e-4F);

    // 悬停光标默认空实现（I1）：可调用且不崩溃即契约（Headless 无系统光标）。
    surface.set_cursor(CursorShape::IBeam);
    surface.set_cursor(CursorShape::Arrow);

    // 默认空实现的回调注册/窗口控制接口：可调用且不崩溃即契约。
    surface.poll_platform_events();
    surface.set_event_handler({});
    surface.set_title("utest");
    surface.set_title_bar_style(TitleBarStyle{});
    surface.set_title_bar_icon(nullptr);
    surface.set_present_dirty({});
    surface.close();
    surface.minimize();
    surface.toggle_maximize();
    surface.set_fullscreen(true);
    surface.begin_window_move();
    surface.begin_window_resize(WindowResizeEdge::Top);
    surface.wait_events(0.0);  // 契约：timeout_ms == 0 立即返回。
    surface.request_wake();
    AURORA_TEST_CHECK_TRUE(true);
}

AURORA_TEST_CASE(surface_framebuffer_size_defaults_to_size) {
    MinimalSurface surface;
    const auto begun = surface.begin_frame(32, 24);
    AURORA_TEST_REQUIRE_TRUE(begun.ok());
    AURORA_TEST_CHECK_NEAR(surface.size().width, 32.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(surface.size().height, 24.0F, 1e-4F);
    // 契约：默认 framebuffer_size == size（无 DPI 物理缓冲的后端）。
    const Size fb = surface.framebuffer_size();
    AURORA_TEST_CHECK_NEAR(fb.width, surface.size().width, 1e-4F);
    AURORA_TEST_CHECK_NEAR(fb.height, surface.size().height, 1e-4F);
}

AURORA_TEST_CASE(surface_save_snapshot_fails_without_framebuffer) {
    // 契约：data() 返回 nullptr 时 save_snapshot 返回 unsupported 错误而非崩溃。
    MinimalSurface surface;
    const auto snap = surface.save_snapshot("utest_surface_should_not_write.png");
    AURORA_TEST_CHECK_FALSE(snap.ok());
}

AURORA_TEST_CASE(surface_capture_window_unsupported_by_default) {
    // 契约：OS 窗口截图仅 Win32/X11/GLFW 覆写，基类默认返回 unsupported 错误。
    MinimalSurface surface;
    const auto captured = surface.capture_window("utest_surface_should_not_write.png");
    AURORA_TEST_CHECK_FALSE(captured.ok());
}

AURORA_TEST_CASE(surface_is_non_copyable_and_abstract) {
    static_assert(std::is_abstract_v<Surface>);
    static_assert(!std::is_copy_constructible_v<Surface>);
    static_assert(!std::is_copy_assignable_v<Surface>);
    static_assert(!std::is_move_constructible_v<Surface>);
    static_assert(!std::is_move_assignable_v<Surface>);
    AURORA_TEST_CHECK_TRUE(std::is_abstract_v<Surface>);
}

AURORA_TEST_CASE(headless_surface_frame_lifecycle) {
#ifdef AURORA_BACKEND_HEADLESS
    // 构造期传入初始尺寸：size() 在 begin_frame 之前即返回正确值（HEADLESS 特有契约）。
    HeadlessSurface surface("", Size{.width = 100.0F, .height = 80.0F});
    AURORA_TEST_CHECK_NEAR(surface.size().width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(surface.size().height, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(surface.frame_count(), 0);

    const auto begun = surface.begin_frame(64, 48);
    AURORA_TEST_REQUIRE_TRUE(begun.ok());
    AURORA_TEST_CHECK_NEAR(surface.size().width, 64.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(surface.size().height, 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NOT_NULL(surface.data());

    const auto presented = surface.present();
    AURORA_TEST_REQUIRE_TRUE(presented.ok());
    AURORA_TEST_CHECK_EQ(surface.frame_count(), 1);

    // begin_frame 可重置画布尺寸且不计数。
    const auto re_begun = surface.begin_frame(20, 10);
    AURORA_TEST_REQUIRE_TRUE(re_begun.ok());
    AURORA_TEST_CHECK_NEAR(surface.size().width, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(surface.frame_count(), 1);

    // 契约：Headless 覆盖 wait_events 为 no-op，不得阻塞（基类默认实现会睡 >=1000ms）。
    const auto t0 = std::chrono::steady_clock::now();
    surface.wait_events(-1.0);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    AURORA_TEST_CHECK_LT(elapsed_ms, 900.0);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，HeadlessSurface 未编译");
#endif
}

AURORA_TEST_CASE(headless_surface_png_present_writes_file) {
#ifdef AURORA_BACKEND_HEADLESS
    const auto path = std::filesystem::path(testing::isolation::temp_dir()) / "utest_surface_headless.png";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    HeadlessSurface surface;
    surface.set_png_path(path.string());
    AURORA_TEST_REQUIRE_TRUE(surface.begin_frame(32, 16).ok());
    AURORA_TEST_REQUIRE_TRUE(surface.present().ok());
    AURORA_TEST_CHECK_TRUE(std::filesystem::exists(path));
    AURORA_TEST_CHECK_EQ(surface.frame_count(), 1);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，HeadlessSurface 未编译");
#endif
}

AURORA_TEST_CASE(headless_surface_state_seams_dispatch_handlers) {
#ifdef AURORA_BACKEND_HEADLESS
    HeadlessSurface surface;

    WindowState last_state = WindowState::Visible;
    int state_calls = 0;
    surface.set_window_state_handler([&](WindowState s) -> void {
        last_state = s;
        ++state_calls;
    });
    surface.simulate_window_state(WindowState::Hidden);
    AURORA_TEST_CHECK_EQ(state_calls, 1);
    AURORA_TEST_CHECK(last_state == WindowState::Hidden);

    WindowMode last_mode = WindowMode::Normal;
    int mode_calls = 0;
    surface.set_window_mode_handler([&](WindowMode m) -> void {
        last_mode = m;
        ++mode_calls;
    });
    surface.simulate_window_mode(WindowMode::Maximized);
    AURORA_TEST_CHECK_EQ(mode_calls, 1);
    AURORA_TEST_CHECK(last_mode == WindowMode::Maximized);

    int present_calls = 0;
    surface.set_present_request([&]() -> void { ++present_calls; });
    surface.simulate_present_request();
    AURORA_TEST_CHECK_EQ(present_calls, 1);

    // 契约：未接线时 simulate_* 为 no-op（不崩溃）。
    HeadlessSurface bare;
    bare.simulate_window_state(WindowState::Occluded);
    bare.simulate_window_mode(WindowMode::Minimized);
    bare.simulate_present_request();
    AURORA_TEST_CHECK_TRUE(true);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，HeadlessSurface 未编译");
#endif
}

}  // namespace aurora::test_cases::utest_surface
