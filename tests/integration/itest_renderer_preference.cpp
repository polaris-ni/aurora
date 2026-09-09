/// 测试类型: integration
/// 目标单元: include/aurora/window/window.h
/// 测试说明: 渲染后端偏好 RendererPreference 的工厂选择逻辑——
///           Software 强制 Win32/GDI；Auto（默认值契约）成功开窗且按编译选项
///           落在 D3D11/GDI 二者之一；GpuD3D11 强制 GPU：编译了 D3D11 时
///           成功（真实设备/WARP）或返回 renderer-unavailable，未编译时必须报错
///           不静默降级。无 Win32 后端的构建直接 SKIP

#include <memory>
#include <string>

#include "aurora/window/window.h"
#include "framework/aurora_test.h"

#ifdef AURORA_BACKEND_WIN32
#include "aurora/window/win32_surface.h"
#endif
#ifdef AURORA_BACKEND_D3D11
#include "aurora/window/d3d11_surface.h"
#endif

namespace au = aurora;

namespace aurora::test_cases::itest_renderer_preference {

#ifdef AURORA_BACKEND_WIN32

AURORA_TEST_CASE(renderer_preference_selects_backend) {
    au::enable_dpi_awareness();
    au::WindowOptions base;
    base.size = au::Size{.width = 320.0F, .height = 240.0F};
    base.title = "itest_renderer_preference";

    // ---- 1. Software：强制软件 GDI ----
    {
        au::Win32Options opts{base};
        opts.renderer = au::RendererPreference::Software;
        const auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        if (res) {
            AURORA_TEST_CHECK(dynamic_cast<au::Win32Surface *>(&res.value()->surface()) != nullptr);
        }
    }

    // ---- 2. Auto：默认值即 Auto；必须成功（GPU 可用走 D3D11，否则回退 GDI）----
    {
        au::Win32Options opts{base};
        AURORA_TEST_CHECK(opts.renderer == au::RendererPreference::Auto);  // 默认值契约
        const auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        if (res) {
            au::Surface &s = res.value()->surface();
#ifdef AURORA_BACKEND_D3D11
            // D3D11 编译在内：Auto 要么选 D3D11（含 WARP 兜底），要么设备失败回退 GDI。
            const bool is_gpu = dynamic_cast<au::D3D11Surface *>(&s) != nullptr;
            const bool is_gdi = dynamic_cast<au::Win32Surface *>(&s) != nullptr;
            AURORA_TEST_CHECK(is_gpu || is_gdi);
#else
            AURORA_TEST_CHECK(dynamic_cast<au::Win32Surface *>(&s) != nullptr);  // 未编译：静默走软件
#endif
        }
    }

    // ---- 3. GpuD3D11：强制 GPU ----
    {
        au::Win32Options opts{base};
        opts.renderer = au::RendererPreference::GpuD3D11;
        const auto res = au::create_window(opts);
#ifdef AURORA_BACKEND_D3D11
        // 编译在内：成功（真实设备/WARP）则必为 D3D11Surface；失败则错误码必为 renderer-unavailable。
        if (res) {
            AURORA_TEST_CHECK(dynamic_cast<au::D3D11Surface *>(&res.value()->surface()) != nullptr);
        } else {
            AURORA_TEST_CHECK_EQ(res.error().code, std::string("renderer-unavailable"));
        }
#else
        // 未编译：必须报错，不得静默降级为 GDI。
        AURORA_TEST_CHECK_FALSE(static_cast<bool>(res));
        if (!res) {
            AURORA_TEST_CHECK_EQ(res.error().code, std::string("renderer-unavailable"));
        }
#endif
    }
}

#else  // !AURORA_BACKEND_WIN32

AURORA_TEST_CASE(renderer_preference_selects_backend) {
    AURORA_TEST_SKIP("无 Win32 后端（非 Windows 构建）：renderer preference 不适用");
}

#endif  // AURORA_BACKEND_WIN32

}  // namespace aurora::test_cases::itest_renderer_preference
