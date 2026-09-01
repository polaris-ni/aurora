#pragma once

/**
 * @file native_surfaces.h
 * @brief 真实平台 Surface 统一包含入口（ARCHITECTURE.md §8.4 / specification/03-layout-render.md §8.5 后端与工厂）。
 *
 * 仅包含「平台 Surface 后端」相关头，供需要弹真实窗口的用户使用：
 * @code
 *   #include "aurora/aurora.h"                  // 抽象层（surface/window/painter/font_engine）
 *   #include "aurora/window/native_surfaces.h"  // 真实 Surface：Headless/Win32/Glfw
 *   au::WindowOptions opts{ .size = {1024, 768}, .title = "Demo" };
 *   auto w = au::create_window(au::GlfwOptions{ opts });
 * @endcode
 *
 * 说明：
 * - `HeadlessSurface` 零三方依赖，默认即可用（由 `AURORA_BACKEND_HEADLESS` 控制）。
 * - `Win32Surface` 零三方依赖，仅当 `AURORA_BACKEND_WIN32` 定义（Windows 默认 ON）时包含。
 * - `GlfwSurface` 仅当 `AURORA_BACKEND_GLFW` 定义（由 `AURORA_BACKEND_GLFW` 开关控制）时包含，
 *   避免默认构建引入 GLFW/OpenGL 头文件与链接依赖。GLFW 以 `-DAURORA_BACKEND_GLFW=ON`
 *   开启后经仓库内置 third_party/glfw 源码构建链接（无需额外变量）。
 */

#include "aurora/window/surface.h"

#ifdef AURORA_BACKEND_WIN32
#include "aurora/window/win32_surface.h"
#endif

#ifdef AURORA_BACKEND_D3D11
#include "aurora/window/d3d11_surface.h"
#endif

#ifdef AURORA_BACKEND_GLFW
#include "aurora/window/glfw_surface.h"
#endif

#ifdef AURORA_BACKEND_X11
#include "aurora/window/x11_surface.h"
#endif

#ifdef AURORA_BACKEND_WAYLAND
#include "aurora/window/wayland_surface.h"
#endif

#ifdef AURORA_BACKEND_MACOS
#include "aurora/window/macos_surface.h"
#endif

#ifdef AURORA_BACKEND_WASM
#include "aurora/window/wasm_surface.h"
#endif
