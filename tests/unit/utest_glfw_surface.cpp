/// 测试类型: unit
/// 目标单元: include/aurora/window/glfw_surface.h
/// 测试说明: GlfwSurface 配置结构默认值与可定制性、类型契约（继承 Surface、不可拷贝）；
/// 该头自身无宏门控（声明始终可编译），窗口/GL 上下文创建路径不测

#include <string>
#include <type_traits>

#include "aurora/window/glfw_surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_glfw_surface {

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
#if defined(AURORA_BACKEND_GLFW)
    // GlfwSurface 构造会 glfwInit + 创建真实窗口与 GL 上下文（无显示环境时抛
    // std::runtime_error）；帧管线/present 依赖真实窗口，属集成层覆盖范围。
    AURORA_TEST_SKIP("GlfwSurface 构造会创建真实窗口与 OpenGL 上下文，单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW 未开启（默认 OFF），后端实现未编译链接");
#endif
}

}  // namespace aurora::test_cases::utest_glfw_surface
