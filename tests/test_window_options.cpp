// test_window_options.cpp — 覆盖类型安全的窗口选项工厂重载
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <filesystem>
#include <memory>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/window/window.h"

#include "test_harness.h"

namespace au = aurora;

AURORA_TEST() {
    // Headless 专属重载：png_path 应被识别并在 present 时写出 PNG。
    {
        au::HeadlessOptions opts;
        opts.size = au::Size{ .width = 120.0f, .height = 80.0f };
        opts.title = "headless_opts";
        opts.png_path = "build/window_opts_headless.png";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        if (res) {
            auto win = std::move(res.value());
            au::Node page = au::Text{ au::TextProps{ .content = au::LocalizedString{ "x" } } };
            auto r = win->present_root(page);
            AURORA_TEST_CHECK(static_cast<bool>(r));
            AURORA_TEST_CHECK(win->surface().frame_count() == 1);
            AURORA_TEST_CHECK(std::filesystem::exists(opts.png_path));
        }
    }

    // Win32 专属重载（仅 Windows 编译/可用；无显示环境可能返回 Error，仅验证重载可调用）。
#ifdef AURORA_PLATFORM_WINDOWS
    {
        au::Win32Options opts;
        opts.size = au::Size{ .width = 320.0f, .height = 200.0f };
        opts.title = "win32_opts";
        auto res = au::create_window(opts);
        if (res) {
            AURORA_TEST_CHECK(res.value()->title() == "win32_opts");
        }
    }
#endif

    // Glfw 专属重载（仅开启 AURORA_BACKEND_GLFW 时可用）。
#ifdef AURORA_BACKEND_GLFW
    {
        au::GlfwOptions opts;
        opts.size = au::Size{ .width = 320.0f, .height = 200.0f };
        opts.title = "glfw_opts";
        opts.gl_major = 4;
        opts.resizable = false;
        auto res = au::create_window(opts);
        if (res) {
            AURORA_TEST_CHECK(res.value()->title() == "glfw_opts");
        }
    }
#endif

    // 标题下发（Window → Surface）：用 RecordingSurface 验证 set_title 转发到后端（跨平台、无需真实窗口）。
    {
        struct RecordingSurface : au::Surface {
            bool called = false;
            std::string got;
            [[nodiscard]] auto begin_frame(int /*w*/, int /*h*/) -> au::Result<bool> override {
                return au::Result{ true };
            }
            [[nodiscard]] auto painter() -> au::Painter & override {
                static au::Painter p;
                return p;
            }
            [[nodiscard]] auto present() -> au::Result<bool> override { return au::Result{ true }; }
            [[nodiscard]] auto size() const -> au::Size override { return au::Size{ .width = 1.0f, .height = 1.0f }; }
            auto set_title(const std::string &t) -> void override {
                called = true;
                got = t;
            }
        };
        auto rec = std::make_unique<RecordingSurface>();
        auto *raw = rec.get();
        auto w = au::Window(std::move(rec));
        w.set_title("hello-title");
        AURORA_TEST_CHECK(raw->called);
        AURORA_TEST_CHECK(raw->got == "hello-title");
        AURORA_TEST_CHECK(w.title() == "hello-title");
    }
}
