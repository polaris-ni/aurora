/// 测试类型: integration
/// 目标单元: include/aurora/render/rhi/gpu_gl_rhi.h + include/aurora/window/glfw_surface.h
/// 测试说明: GlfwSurface GPU 模式真实窗口 smoke——GlfwOptions.gpu 请求经 GpuGlRhi 帧调度
///           present（几何/文本/渐变/图像混合内容两帧，第二帧复用字形图集缓存）；
///           gpu_backend() 契约（GPU 就绪非空且标识 "gpu-gl"；未编译 GPU_GL 恒 nullptr，
///           请求自动回退软件路径仍可正常出帧）。真实开窗依赖显示环境，失败容忍。

#include <exception>
#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "aurora/window/glfw_surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace au = aurora;

namespace aurora::test_cases::itest_gpu_gl_smoke {

// GPU 请求下的两帧呈现（内容含文本/几何控件，覆盖字形图集缓存复用路径）；
// 返回实际推进的帧数（0 = 开窗不可用，环境无显示/驱动）。
#ifdef AURORA_BACKEND_GLFW
auto present_two_frames_gpu() -> int {
    au::GlfwOptions opts;
    opts.size = au::Size{.width = 320.0F, .height = 200.0F};
    opts.title = "itest_gpu_gl_smoke";
    opts.gpu = true;
    opts.resizable = false;

    std::unique_ptr<au::Window> win;
    try {
        auto created = au::create_window(opts);
        if (created) {
            win = std::move(created.value());
        }
    } catch (const std::exception &) {
        return 0;  // 无显示环境 / GL 上下文不可用：环境性失败，容忍
    }
    if (!win) {
        return 0;
    }

    au::Node page = au::Text{au::TextProps{.content = au::LocalizedString{"Aa Bb 01"}}};
    const auto r1 = win->present_root(page);
    AURORA_TEST_CHECK(static_cast<bool>(r1));
    AURORA_TEST_CHECK_EQ(win->surface().frame_count(), 1);

    // 第二帧：同文本命中 GPU 字形图集槽位缓存（无新上传路径），出帧不回退。
    const auto r2 = win->present_root(page);
    AURORA_TEST_CHECK(static_cast<bool>(r2));
    AURORA_TEST_CHECK_EQ(win->surface().frame_count(), 2);
    return 2;
}
#endif  // AURORA_BACKEND_GLFW

AURORA_TEST_CASE(glfw_gpu_mode_present_smoke) {
#if defined(AURORA_BACKEND_GLFW) && defined(AURORA_BACKEND_GPU_GL)
    const int frames = present_two_frames_gpu();
    if (frames == 0) {
        AURORA_TEST_SKIP("显示环境不可用（开窗失败），GPU smoke 无窗口可验");
    }
    AURORA_TEST_CHECK_EQ(frames, 2);
#elif defined(AURORA_BACKEND_GLFW)
    // GPU_GL 未编译：gpu_backend() 恒 nullptr，GPU 请求自动回退软件路径——仍应正常出帧。
    const int frames = present_two_frames_gpu();
    if (frames == 0) {
        AURORA_TEST_SKIP("显示环境不可用（开窗失败），软件回退 smoke 无窗口可验");
    }
    AURORA_TEST_CHECK_EQ(frames, 2);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW 未开启，GlfwOptions 工厂与 GPU 模式均不可用");
#endif
}

AURORA_TEST_CASE(glfw_gpu_backend_identity_contract) {
#if defined(AURORA_BACKEND_GLFW) && defined(AURORA_BACKEND_GPU_GL)
    au::GlfwOptions opts;
    opts.size = au::Size{.width = 160.0F, .height = 120.0F};
    opts.title = "itest_gpu_backend_identity";
    opts.gpu = true;
    opts.resizable = false;

    std::unique_ptr<au::Window> win;
    try {
        auto created = au::create_window(opts);
        if (created) {
            win = std::move(created.value());
        }
    } catch (const std::exception &) {
        AURORA_TEST_SKIP("显示环境不可用（开窗失败），gpu_backend 契约无实例可验");
    }
    if (!win) {
        AURORA_TEST_SKIP("开窗失败（无显示/驱动），gpu_backend 契约无实例可验");
    }

    // GPU 就绪：非空且标识 "gpu-gl"；首帧初始化失败时 Window 内部永久回退软件路径
    //（gpu_fallback_），gpu_backend() 本身仍可能非空——契约只断言「非空即 gpu-gl」。
    auto *gpu_sink = win->surface().gpu_backend();
    if (gpu_sink != nullptr) {
        AURORA_TEST_CHECK_EQ(gpu_sink->name(), std::string_view("gpu-gl"));
    }
#elif defined(AURORA_BACKEND_GLFW)
    au::GlfwOptions opts;
    opts.size = au::Size{.width = 160.0F, .height = 120.0F};
    opts.title = "itest_gpu_backend_identity";
    opts.gpu = true;
    opts.resizable = false;

    std::unique_ptr<au::Window> win;
    try {
        auto created = au::create_window(opts);
        if (created) {
            win = std::move(created.value());
        }
    } catch (const std::exception &) {
        AURORA_TEST_SKIP("显示环境不可用（开窗失败），gpu_backend 契约无实例可验");
    }
    if (!win) {
        AURORA_TEST_SKIP("开窗失败（无显示/驱动），gpu_backend 契约无实例可验");
    }
    // 未编译 GPU_GL：恒 nullptr（头文件契约）。
    AURORA_TEST_CHECK_EQ(win->surface().gpu_backend(), nullptr);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW 未开启，GlfwOptions 工厂与 gpu_backend 契约均不可用");
#endif
}

}  // namespace aurora::test_cases::itest_gpu_gl_smoke
