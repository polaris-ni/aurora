/// 测试类型: unit
/// 目标单元: include/aurora/window/glfw_surface.h
/// 测试说明: GlfwSurface 配置结构默认值与可定制性、类型契约（继承 Surface、不可拷贝）；
/// 该头自身无宏门控（声明始终可编译），窗口/GL 上下文创建路径默认不触碰 OS 资源，
/// 真机用例（`AURORA_LIVE_GLFW=1` 选择加入）创建真实窗口并验证 capture_window 帧缓冲读回落盘

#include <cstdlib>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include "aurora/debug/feature_flags.h"  // 运行时探测 AURORA_ENABLE_DEBUG 的归一化镜像
#include "aurora/window/glfw_surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_glfw_surface {

using aurora::debug::feature_flags;  // 运行时探测 AURORA_ENABLE_DEBUG（Release 下截图体被宏裁切）

AURORA_TEST_CASE(glfw_surface_config_defaults) {
    const GlfwSurface::Config cfg;
    AURORA_TEST_CHECK_NEAR(cfg.size.width, 800.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(cfg.size.height, 600.0F, 1e-4F);
    AURORA_TEST_CHECK(cfg.title == std::string{"Aurora"});
    AURORA_TEST_CHECK_EQ(cfg.gl_major, 3);
    AURORA_TEST_CHECK_EQ(cfg.gl_minor, 3);
    AURORA_TEST_CHECK_TRUE(cfg.resizable);
}

AURORA_TEST_CASE(glfw_surface_config_is_customizable) {
    // AI 友好性契约：配置结构体逐字段可覆盖。
    GlfwSurface::Config cfg;
    cfg.size = Size{.width = 1024.0F, .height = 768.0F};
    cfg.title = "Custom";
    cfg.gl_major = 3;
    cfg.gl_minor = 2;
    cfg.resizable = false;
    AURORA_TEST_CHECK_NEAR(cfg.size.width, 1024.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(cfg.size.height, 768.0F, 1e-4F);
    AURORA_TEST_CHECK(cfg.title == std::string{"Custom"});
    AURORA_TEST_CHECK_EQ(cfg.gl_minor, 2);
    AURORA_TEST_CHECK_FALSE(cfg.resizable);
}

AURORA_TEST_CASE(glfw_surface_type_contract) {
    // 注意：glfw_surface.h 自身无 #ifdef 门控（与 d3d11/win32 头不同），类型声明始终可编译；
    // 门控发生在 native_surfaces.h 的包含点与 .cpp 的编译开关上。
    static_assert(std::is_base_of_v<aurora::Surface, aurora::GlfwSurface>);
    static_assert(!std::is_copy_constructible_v<aurora::GlfwSurface>);
    static_assert(!std::is_move_constructible_v<aurora::GlfwSurface>);
    static_assert(!std::is_default_constructible_v<aurora::GlfwSurface>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aurora::Surface, aurora::GlfwSurface>);
}

AURORA_TEST_CASE(glfw_surface_window_creation_skipped) {
#ifdef AURORA_BACKEND_GLFW
    // GlfwSurface 构造会 glfwInit + 创建真实窗口与 GL 上下文（无显示环境时抛
    // std::runtime_error）；帧管线/present 依赖真实窗口，属集成层覆盖范围。
    AURORA_TEST_SKIP("GlfwSurface 构造会创建真实窗口与 OpenGL 上下文，单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW 未开启（默认 OFF），后端实现未编译链接");
#endif
}

AURORA_TEST_CASE(glfw_surface_live_capture_window) {
#ifdef AURORA_BACKEND_GLFW
    const char *opt_in = std::getenv("AURORA_LIVE_GLFW");
    if (opt_in == nullptr || *opt_in == '\0') {
        AURORA_TEST_SKIP("需显式置 AURORA_LIVE_GLFW=1：本用例会创建真实窗口与 GL 上下文并截帧");
    }
    if (!feature_flags().debug) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用（Release/AUTO 裁切）：capture_window 恒返回 disabled");
    }

    GlfwSurface::Config cfg;
    cfg.size = Size{.width = 160.0F, .height = 120.0F};
    cfg.title = "aurora-live-glfw-capture";
    cfg.resizable = false;
    GlfwSurface surface{cfg};

    // 画一帧可辨识内容（底色 + 纯色块）并上屏两次：capture 走帧缓冲读回，须先有 present。
    AURORA_TEST_REQUIRE_TRUE(surface.begin_frame(160, 120).ok());
    surface.painter().fill_rect(Rect{.origin = Point{.x = 20.0F, .y = 30.0F}, .size = Size{.width = 60.0F,
                                                                                           .height = 40.0F}},
                                Color{10, 20, 30, 255});
    AURORA_TEST_REQUIRE_TRUE(surface.present().ok());
    surface.poll_platform_events();
    AURORA_TEST_REQUIRE_TRUE(surface.begin_frame(160, 120).ok());
    surface.painter().fill_rect(Rect{.origin = Point{.x = 20.0F, .y = 30.0F}, .size = Size{.width = 60.0F,
                                                                                           .height = 40.0F}},
                                Color{10, 20, 30, 255});
    AURORA_TEST_REQUIRE_TRUE(surface.present().ok());

    const std::string path = "aurora_live_glfw_capture.png";
    std::remove(path.c_str());
    const Result<bool> r = surface.capture_window(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_TRUE(r.value());

    // 落盘文件存在、非空、以 PNG 魔数开头。
    std::ifstream f(path, std::ios::binary);
    AURORA_TEST_REQUIRE_TRUE(f.is_open());
    std::vector<char> bytes{(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>{}};
    AURORA_TEST_CHECK_GT(bytes.size(), 8U);
    AURORA_TEST_CHECK_EQ(bytes[0], '\x89');
    AURORA_TEST_CHECK_EQ(bytes[1], 'P');
    AURORA_TEST_CHECK_EQ(bytes[2], 'N');
    AURORA_TEST_CHECK_EQ(bytes[3], 'G');
    f.close();
    std::remove(path.c_str());
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW 未开启（默认 OFF），后端实现未编译链接");
#endif
}

}  // namespace aurora::test_cases::utest_glfw_surface
