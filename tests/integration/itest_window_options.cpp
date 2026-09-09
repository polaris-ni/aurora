/// 测试类型: integration
/// 目标单元: include/aurora/window/window.h
/// 测试说明: 类型安全的窗口选项工厂重载——HeadlessOptions.png_path 经 present 写出 PNG、
///           Win32/Glfw 专属重载按平台/后端宏可用（创建失败环境容忍，仅验证可调用与
///           标题下发）、Window::set_title 转发到自定义 Surface（跨平台无需真实窗口）

#include <filesystem>
#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/widget/spacer.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace au = aurora;

namespace aurora::test_cases::itest_window_options {

#ifdef AURORA_BACKEND_HEADLESS

AURORA_TEST_CASE(headless_options_png_path_writes_on_present) {
    const std::string png_path =
        (std::filesystem::path{aurora::testing::isolation::temp_dir()} / "window_opts_headless.png").string();

    au::HeadlessOptions opts;
    opts.size = au::Size{.width = 120.0F, .height = 80.0F};
    opts.title = "headless_opts";
    opts.png_path = png_path;
    auto created = au::create_window(opts);
    AURORA_TEST_REQUIRE(static_cast<bool>(created));

    auto win = std::move(created.value());
    au::Node page = au::Text{au::TextProps{.content = au::LocalizedString{"x"}}};
    const auto r = win->present_root(page);
    AURORA_TEST_CHECK(static_cast<bool>(r));
    AURORA_TEST_CHECK_EQ(win->surface().frame_count(), 1);
    AURORA_TEST_CHECK_MSG(std::filesystem::exists(png_path), "png_path must be written on present");
}

#else  // !AURORA_BACKEND_HEADLESS

AURORA_TEST_CASE(headless_options_png_path_writes_on_present) {
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，HeadlessOptions 工厂不可用");
}

#endif  // AURORA_BACKEND_HEADLESS

#if defined(AURORA_PLATFORM_WINDOWS) && defined(AURORA_BACKEND_WIN32)

AURORA_TEST_CASE(win32_options_title_applied) {
    // 真实开窗依赖显示环境：无显示环境可能返回 Error，仅验证重载可调用且标题下发。
    au::Win32Options opts;
    opts.size = au::Size{.width = 320.0F, .height = 200.0F};
    opts.title = "win32_opts";
    const auto res = au::create_window(opts);
    if (res) {
        AURORA_TEST_CHECK_EQ(res.value()->title(), std::string("win32_opts"));
    }
}

#else

AURORA_TEST_CASE(win32_options_title_applied) {
    AURORA_TEST_SKIP("非 Windows 或 AURORA_BACKEND_WIN32 未开启，Win32Options 工厂不可用");
}

#endif  // AURORA_PLATFORM_WINDOWS && AURORA_BACKEND_WIN32

#ifdef AURORA_BACKEND_GLFW

AURORA_TEST_CASE(glfw_options_factory_callable) {
    // GLFW 真实开窗同样依赖显示环境：失败容忍，仅验证专属选项（gl 版本/可缩放）可传。
    au::GlfwOptions opts;
    opts.size = au::Size{.width = 320.0F, .height = 200.0F};
    opts.title = "glfw_opts";
    opts.gl_major = 4;
    opts.resizable = false;
    const auto res = au::create_window(opts);
    if (res) {
        AURORA_TEST_CHECK_EQ(res.value()->title(), std::string("glfw_opts"));
    }
}

#else

AURORA_TEST_CASE(glfw_options_factory_callable) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW 未开启，GlfwOptions 工厂不可用");
}

#endif  // AURORA_BACKEND_GLFW

AURORA_TEST_CASE(set_title_forwards_to_custom_surface) {
    // 标题下发（Window → Surface）：用 RecordingSurface 验证 set_title 转发到后端。
    struct RecordingSurface final : au::Surface {
        bool called = false;
        std::string got;
        auto begin_frame(int /*w*/, int /*h*/) -> au::Result<bool> override { return au::Result<bool>{true}; }
        auto painter() -> au::Painter& override {
            static au::Painter p;
            return p;
        }
        auto present() -> au::Result<bool> override { return au::Result<bool>{true}; }
        [[nodiscard]] auto size() const -> au::Size override { return au::Size{.width = 1.0F, .height = 1.0F}; }
        [[nodiscard]] auto should_close() const -> bool override { return false; }
        auto poll_platform_events() -> void override {}
        auto set_title(const std::string& t) -> void override {
            called = true;
            got = t;
        }
        [[nodiscard]] auto content_inset() const -> au::EdgeInsets override { return au::EdgeInsets{}; }
        auto close() -> void override {}
        auto minimize() -> void override {}
        auto toggle_maximize() -> void override {}
        auto set_fullscreen(bool /*on*/) -> void override {}
        auto begin_window_move() -> void override {}
        auto begin_window_resize(au::WindowResizeEdge /*edge*/) -> void override {}
        auto set_title_bar_style(const au::TitleBarStyle& /*style*/) -> void override {}
        auto set_title_bar_icon(const std::shared_ptr<au::Image>& /*icon*/) -> void override {}
    };

    auto rec = std::make_unique<RecordingSurface>();
    RecordingSurface* raw = rec.get();
    au::Window w{std::move(rec)};
    w.set_title("hello-title");
    AURORA_TEST_CHECK(raw->called);
    AURORA_TEST_CHECK_EQ(raw->got, std::string("hello-title"));
    AURORA_TEST_CHECK_EQ(w.title(), std::string("hello-title"));
}

}  // namespace aurora::test_cases::itest_window_options
