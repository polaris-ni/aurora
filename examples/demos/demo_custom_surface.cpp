// demo_custom_surface.cpp — 演示自定义 Surface 注入 + 特化选项经工厂进 Application。
//
// 两种稳定扩展点（不随 Surface 数量增长）：
//   A) App().surface(unique_ptr<Surface>) / Application(Scene, unique_ptr<Surface>) —— 注入任意自定义 Surface。
//   B) App().window(create_window(XxxOptions)) / Application(Scene, unique_ptr<Window>) —— 特化选项先经工厂组装
//   Window。
#include <memory>

#include "demo_common.h"

namespace au = aurora;

namespace {
// 最简自定义 Surface：内存 Painter 缓冲 + 帧计数 + 日志（不依赖任何内置 Surface 子类）。
class DemoSurface : public au::Surface {
  public:
    auto begin_frame(int w, int h) -> au::Result<bool> override {
        painter_.begin(w, h);
        size_ = au::Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
        return au::Result{true};
    }
    auto painter() -> au::Painter& override { return painter_; }
    auto present() -> au::Result<bool> override {
        ++frames_;
        AURORA_LOG_INFO("demo", "[DemoSurface] present #", frames_);
        return au::Result{true};
    }
    [[nodiscard]] auto size() const -> au::Size override { return size_; }
    [[nodiscard]] auto frame_count() const -> int override { return frames_; }

  private:
    au::Painter painter_;
    au::Size size_{.width = 0.0F, .height = 0.0F};
    int frames_ = 0;
};
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::init_console();

    // 方案 A：自定义 Surface 经 Application 接入，离屏 render_to_png 产出可见产物（无事件循环挂起）。
    {
        au::Scene scene{au::Text{au::TextProps{.content = au::LocalizedString{"Custom Surface"}}}};
        au::Application app{std::move(scene), std::make_unique<DemoSurface>()};
        auto r = app.render_to_png("build/demo_custom_surface_custom.png");
        AURORA_LOG_INFO("demo", "[demo_custom_surface] custom Surface PNG: ", r ? "OK" : r.error().message);
    }

    // 方案 B：特化选项经工厂组装 Window 后交给流式 App（Headless 写 PNG，frames(1) 后退出）。
    {
        auto win_res = au::create_window(au::HeadlessOptions{.png_path = "build/demo_custom_surface_headless.png"});
        au::App()
            .window(win_res ? std::move(win_res.value()) : nullptr)
            .title("surface-demo")
            .size(360, 200)
            .view(au::Text{au::TextProps{.content = au::LocalizedString{"Headless Window"}}})
            .frames(1)
            .run();
    }

    return 0;
}