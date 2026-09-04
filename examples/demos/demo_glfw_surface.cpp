// demo_glfw_surface.cpp — 演示 GLFW + OpenGL 真实窗口后端（GlfwSurface / GlfwOptions）。
//
// 与 run_demo 的差异：绕过 create_native_window 的平台优先级（Windows 上 Win32 优先），
// 显式走类型安全工厂 create_window(GlfwOptions)，强制验证 GLFW 后端的帧生命周期
// （begin_frame → Painter 软件光栅 → GL 纹理上屏）与原生事件翻译。
// 仅 AURORA_BACKEND_GLFW=ON 构建可用（源码构建见 third_party/glfw）；未开启或初始化失败时
// 回退无头 PNG 渲染，保证各环境可编译可验证。

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <system_error>

#include "demo_common.h"

namespace au = aurora;

namespace {

auto build_root() -> au::Node {
    return Card(
        {
            gap(8),
            GradientTitle("GLFW + OpenGL"),
            BrandBadge("Software Painter -> GL Texture", pal::AURORA_PRIMARY),
            gap(4),
            Card(au::Text("This window is presented by GlfwSurface: software Painter raster, uploaded as an OpenGL "
                          "texture each frame.")),
            Card(au::Text("Mouse / keyboard / wheel / text input are translated by GLFW into aurora::Event then "
                          "dispatched uniformly.")),
            Card(au::Text("Drag window edge to resize: resize event drives relayout and redraw.")),
            gap(8),
        },
        pal::AURORA_BG);
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::enable_dpi_awareness();
    au::init_console();

    au::Node root = build_root();
    au::FocusManager fm;
    fm.set_root(&root.widget());

#ifdef AURORA_BACKEND_GLFW
    au::GlfwOptions opts;
    opts.size = au::Size{.width = 560.0F, .height = 380.0F};
    opts.title = "demo_glfw_surface";

    std::unique_ptr<au::Window> win;
    try {
        auto win_res = au::create_window(opts);
        if (win_res) {
            win = std::move(win_res.value());
        } else {
            AURORA_LOG_ERROR("demo", "[demo_glfw_surface] create_window failed: ", win_res.error().message);
        }
    } catch (const std::exception &e) {
        // GlfwSurface 构造在 glfwInit/glfwCreateWindow 失败时抛 std::runtime_error。
        AURORA_LOG_ERROR("demo", "[demo_glfw_surface] GlfwSurface init exception: ", e.what());
    }

    if (win) {
        win->surface().set_event_handler([&](au::Event &e) -> void {
            auto &wd = root.widget();
            if (auto *me = dynamic_cast<au::MouseEvent *>(&e)) {
                au::EventDispatcher::dispatch(wd, *me, &fm);
            } else if (auto *ke = dynamic_cast<au::KeyEvent *>(&e)) {
                au::EventDispatcher::dispatch(wd, *ke, fm);
            } else if (auto *se = dynamic_cast<au::ScrollEvent *>(&e)) {
                au::EventDispatcher::dispatch(wd, *se);
            } else if (auto *te = dynamic_cast<au::TextInputEvent *>(&e)) {
                au::EventDispatcher::dispatch(wd, *te, fm);
            }
        });

        AURORA_LOG_INFO("demo", "[demo_glfw_surface] GLFW window shown (close window to exit)");
        win->run([&]() -> void {
            const auto t0 = std::chrono::steady_clock::now();
            (void)win->present_root(root);
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            win->set_next_wait(au::compute_wait_timeout(win->has_pending_dirty(), /*anim_active=*/false,
                                                        /*next_deadline_ms=*/-1.0,
                                                        /*frame_budget_ms=*/1000.0 / 60.0, elapsed_ms,
                                                        win->surface().paces_frames()));
        });
        return 0;
    }
    AURORA_LOG_WARN("demo", "[demo_glfw_surface] GLFW backend unavailable, falling back to headless PNG render");
#endif

    std::error_code ec;
    std::filesystem::create_directories("build", ec);
    au::Scene scene{root};
    auto r = scene.render_to_png("build/demo_glfw_surface.png", 560, 380);
    if (r) {
        AURORA_LOG_INFO("demo", "[demo_glfw_surface] rendered build/demo_glfw_surface.png");
    } else {
        AURORA_LOG_ERROR("demo", "[demo_glfw_surface] headless render failed: ", r.error().message);
    }
    return 0;
}