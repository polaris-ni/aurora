// demo_gpu.cpp — 演示 GPU 栅格路径（GlfwSurface GPU 模式 + GpuGlRhi）。
//
// 与 demo_glfw_surface 的差异：GlfwOptions.gpu = true —— 帧级 DisplayList 经 OpenGL 3.3 core
// 批量渲染进 MSAA 帧缓冲，跳过「CPU 全屏像素 → GL 纹理上传」；present 即 swapBuffers。
// 命令覆盖为全部实路径：几何/裁剪/alpha、渐变（LUT 纹理）、图像（PMA）、文本（字形图集，
// 与软件路径共用光栅化）与效果组（Shadow/Blur/Blend/Mask，经 resolve 纹理 pass）。
// 初始化失败（驱动过老 / 无 GL / 远程桌面）自动回退软件纹理路径，诊断日志说明原因。
// 仅 AURORA_BACKEND_GLFW=ON 构建可用；未开启或初始化失败时回退无头 PNG 渲染。

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
            BrandBadge("GPU raster: DisplayList -> OpenGL 3.3 core", pal::AURORA_PRIMARY),
            gap(4),
            Card(au::Text("This window is rasterized by GpuGlRhi: the frame DisplayList is replayed into an "
                          "MSAA framebuffer (solid geometry path).")),
            Card(au::Text("Falling back to software texture upload automatically when GPU init fails.")),
            gap(8),
        },
        pal::AURORA_BG);
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    au::enable_dpi_awareness();
    au::init_console();

    au::Node root = build_root();
    au::FocusManager fm;
    fm.set_root(&root.widget());

#ifdef AURORA_BACKEND_GLFW
    au::GlfwOptions opts;
    opts.size = au::Size{.width = 560.0F, .height = 380.0F};
    opts.title = "demo_gpu";
    opts.gpu = true;

    std::unique_ptr<au::Window> win;
    try {
        auto win_res = au::create_window(opts);
        if (win_res) {
            win = std::move(win_res.value());
        } else {
            AURORA_LOG_ERROR("demo", "[demo_gpu] create_window failed: ", win_res.error().message);
        }
    } catch (const std::exception &e) {
        // GlfwSurface 构造在 glfwInit/glfwCreateWindow 失败时抛 std::runtime_error。
        AURORA_LOG_ERROR("demo", "[demo_gpu] GlfwSurface init exception: ", e.what());
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

        AURORA_LOG_INFO("demo", "[demo_gpu] GPU window shown (close window to exit)");
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
    AURORA_LOG_WARN("demo", "[demo_gpu] GLFW backend unavailable, falling back to headless PNG render");
#endif

    std::error_code ec;
    std::filesystem::create_directories("build", ec);
    au::Scene scene{root};
    auto r = scene.render_to_png("build/demo_gpu.png", 560, 380);
    if (r) {
        AURORA_LOG_INFO("demo", "[demo_gpu] rendered build/demo_gpu.png");
    } else {
        AURORA_LOG_ERROR("demo", "[demo_gpu] headless render failed: ", r.error().message);
    }
    return 0;
}
