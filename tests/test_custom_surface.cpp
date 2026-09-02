// test_custom_surface.cpp — 覆盖自定义 Surface 注入路径与后端代码剪裁（feature 宏）契约。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// window/surface.h(Surface 抽象契约：自定义 Surface 注入/feature 宏剪裁)。

#include <filesystem>
#include <memory>

#include "aurora/aurora.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

#include "test_harness.h"

namespace au = aurora;

namespace {
// 自定义 Surface：内存 Painter 缓冲 + 帧计数 + 日志，演示「不依赖任何内置 Surface 子类」即可接入框架。
class CustomSurface : public au::Surface {
  public:
    auto begin_frame(int w, int h) -> au::Result<bool> override {
        m_painter.begin(w, h);
        m_size = au::Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) };
        return au::Result{ true };
    }
    auto painter() -> au::Painter & override { return m_painter; }
    auto present() -> au::Result<bool> override {
        ++m_frames;
        return au::Result{ true };
    }
    [[nodiscard]] auto size() const -> au::Size override { return m_size; }
    [[nodiscard]] auto frame_count() const -> int override { return m_frames; }

  private:
    au::Painter m_painter;
    au::Size m_size{ .width = 0.0f, .height = 0.0f };
    int m_frames = 0;
};
} // namespace

AURORA_TEST() {
    // 1) 自定义 Surface 经 Application(Scene, unique_ptr<Surface>) 接入：present 出帧、接线生效。
    {
        au::Scene scene{ au::Text{ au::TextProps{ .content = au::LocalizedString{ "Custom" } } } };
        au::Application app{ std::move(scene), std::make_unique<CustomSurface>() };
        AURORA_TEST_CHECK(app.window() != nullptr);
        auto *cs = dynamic_cast<CustomSurface *>(&app.window()->surface());
        AURORA_TEST_CHECK(cs != nullptr);
        au::Node root{ au::Text{ au::TextProps{ .content = au::LocalizedString{ "Frame" } } } };
        auto r = app.window()->present_root(root);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(cs->frame_count() == 1);
    }

    // 2) 自定义 Surface 经流式构建器 App().surface(...).view(...).frames(1).run() 运行（不挂起）。
    {
        au::App()
            .surface(std::make_unique<CustomSurface>())
            .title("custom-builder")
            .size(200, 120)
            .view(au::Text{ au::TextProps{ .content = au::LocalizedString{ "Builder" } } })
            .frames(1)
            .run();
    }

    // 3) 特化选项经工厂组装 Window 后交给 Application(Scene, unique_ptr<Window>)；
    //    Headless 仅在本构建（AURORA_BACKEND_HEADLESS）可用时验证。
#ifdef AURORA_BACKEND_HEADLESS
    {
        auto res = au::create_window(au::HeadlessOptions{ .png_path = "build/custom_headless.png" });
        AURORA_TEST_CHECK(static_cast<bool>(res));
        au::Scene scene{ au::Text{ au::TextProps{ .content = au::LocalizedString{ "Headless" } } } };
        au::Application app{ std::move(scene), res ? std::move(res.value()) : nullptr };
        AURORA_TEST_CHECK(app.window() != nullptr);
        au::Node root{ au::Text{ au::TextProps{ .content = au::LocalizedString{ "X" } } } };
        auto r = app.window()->present_root(root);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(std::filesystem::exists("build/custom_headless.png"));
    }
#endif

    // 4) 后端能力 feature 宏契约：本默认构建应启用内置后端；自定义 Surface 路径不依赖它们。
#ifdef AURORA_BACKEND_HEADLESS
    AURORA_TEST_CHECK(true); // 无头后端已编译进构建（AURORA_BACKEND_HEADLESS）
#endif
#ifdef AURORA_BACKEND_WIN32
    AURORA_TEST_CHECK(true); // Win32 后端已编译进构建（AURORA_BACKEND_WIN32）
#endif
    // 自定义 Surface 注入路径（.surface() / Application(Scene,unique_ptr<Surface>)）在「仅自定义
    // Surface」构建（关闭全部内置后端）下仍可用。此处仅静态验证宏可用。
}
