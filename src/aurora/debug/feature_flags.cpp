// feature_flags.cpp — 编译期 feature 宏开关的**单点归一化镜像**。
//
// 全部 AURORA_* feature 宏的取值只允许出现在本文件（C++ 侧与 BUILD_OPTIONS.md
// 开关清单一一对应）；新增 feature 宏时：cmake/AuroraFeatures.cmake 的
// aurora_define_feature() 调用点 + 本文件 #ifdef 镜像 + FeatureFlags 字段三处同步。
// 运行时查询入口契约见 include/aurora/debug/feature_flags.h。

#include "aurora/debug/feature_flags.h"

namespace aurora::debug {

auto feature_flags() -> FeatureFlags {
    FeatureFlags f;

    // ---- AURORA_BACKEND_*（后端）----
#ifdef AURORA_BACKEND_HEADLESS
    f.backend_headless = true;
#endif
#ifdef AURORA_BACKEND_WIN32
    f.backend_win32 = true;
#endif
#ifdef AURORA_BACKEND_D3D11
    f.backend_d3d11 = true;
#endif
#ifdef AURORA_BACKEND_GLFW
    f.backend_glfw = true;
#endif
#ifdef AURORA_BACKEND_X11
    f.backend_x11 = true;
#endif
#ifdef AURORA_BACKEND_WAYLAND
    f.backend_wayland = true;
#endif
#ifdef AURORA_BACKEND_MACOS
    f.backend_macos = true;
#endif
#ifdef AURORA_BACKEND_WASM
    f.backend_wasm = true;
#endif

    // ---- 架构级优化 ----
#ifdef AURORA_ENABLE_LAYOUT_CACHE
    f.layout_cache = true;
#endif
#ifdef AURORA_ENABLE_OCCLUSION_CULLING
    f.occlusion_culling = true;
#endif
#ifdef AURORA_ENABLE_DISPLAY_LIST
    f.display_list = true;
#endif

    // ---- AURORA_ENABLE_*（内部能力）----
#ifdef AURORA_ENABLE_SIMD
    f.simd = true;
#endif
#ifdef AURORA_ENABLE_PROFILING
    f.profiling = true;
#endif
#ifdef AURORA_ENABLE_TRACING
    f.tracing = true;
#endif
#ifdef AURORA_ENABLE_DEBUG
    f.debug = true;
#endif

    // ---- AURORA_ENABLE_IMAGE_*（编解码能力）----
#ifdef AURORA_ENABLE_IMAGE_JPEG
    f.image_jpeg = true;
#endif
#ifdef AURORA_ENABLE_IMAGE_WEBP
    f.image_webp = true;
#endif
#ifdef AURORA_ENABLE_IMAGE_PNG
    f.image_png = true;
#endif

    return f;
}

auto FeatureFlags::to_json() const -> Json {
    Json j = Json::object();
    j["AURORA_BACKEND_HEADLESS"] = backend_headless;
    j["AURORA_BACKEND_WIN32"] = backend_win32;
    j["AURORA_BACKEND_D3D11"] = backend_d3d11;
    j["AURORA_BACKEND_GLFW"] = backend_glfw;
    j["AURORA_BACKEND_X11"] = backend_x11;
    j["AURORA_BACKEND_WAYLAND"] = backend_wayland;
    j["AURORA_BACKEND_MACOS"] = backend_macos;
    j["AURORA_BACKEND_WASM"] = backend_wasm;
    j["AURORA_ENABLE_LAYOUT_CACHE"] = layout_cache;
    j["AURORA_ENABLE_OCCLUSION_CULLING"] = occlusion_culling;
    j["AURORA_ENABLE_DISPLAY_LIST"] = display_list;
    j["AURORA_ENABLE_SIMD"] = simd;
    j["AURORA_ENABLE_PROFILING"] = profiling;
    j["AURORA_ENABLE_TRACING"] = tracing;
    j["AURORA_ENABLE_DEBUG"] = debug;
    j["AURORA_ENABLE_IMAGE_JPEG"] = image_jpeg;
    j["AURORA_ENABLE_IMAGE_WEBP"] = image_webp;
    j["AURORA_ENABLE_IMAGE_PNG"] = image_png;
    return j;
}

auto feature_flags_json() -> Json { return feature_flags().to_json(); }

}  // namespace aurora::debug
