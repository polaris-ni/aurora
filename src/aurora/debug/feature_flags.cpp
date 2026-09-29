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
#ifdef AURORA_ENABLE_AUDIO
    f.audio = true;
#endif
#ifdef AURORA_ENABLE_AUDIO_WASAPI
    f.enable_audio_wasapi = true;
#endif
#ifdef AURORA_ENABLE_AUDIO_ALSA
    f.enable_audio_alsa = true;
#endif
#ifdef AURORA_ENABLE_AUDIO_WEBAUDIO
    f.enable_audio_webaudio = true;
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
    j.set("AURORA_BACKEND_HEADLESS", Json{backend_headless});
    j.set("AURORA_BACKEND_WIN32", Json{backend_win32});
    j.set("AURORA_BACKEND_D3D11", Json{backend_d3d11});
    j.set("AURORA_BACKEND_GLFW", Json{backend_glfw});
    j.set("AURORA_BACKEND_X11", Json{backend_x11});
    j.set("AURORA_BACKEND_WAYLAND", Json{backend_wayland});
    j.set("AURORA_BACKEND_MACOS", Json{backend_macos});
    j.set("AURORA_BACKEND_WASM", Json{backend_wasm});
    j.set("AURORA_ENABLE_LAYOUT_CACHE", Json{layout_cache});
    j.set("AURORA_ENABLE_OCCLUSION_CULLING", Json{occlusion_culling});
    j.set("AURORA_ENABLE_DISPLAY_LIST", Json{display_list});
    j.set("AURORA_ENABLE_SIMD", Json{simd});
    j.set("AURORA_ENABLE_PROFILING", Json{profiling});
    j.set("AURORA_ENABLE_TRACING", Json{tracing});
    j.set("AURORA_ENABLE_DEBUG", Json{debug});
    j.set("AURORA_ENABLE_AUDIO", Json{audio});
    j.set("AURORA_ENABLE_AUDIO_WASAPI", Json{enable_audio_wasapi});
    j.set("AURORA_ENABLE_AUDIO_ALSA", Json{enable_audio_alsa});
    j.set("AURORA_ENABLE_AUDIO_WEBAUDIO", Json{enable_audio_webaudio});
    j.set("AURORA_ENABLE_IMAGE_JPEG", Json{image_jpeg});
    j.set("AURORA_ENABLE_IMAGE_WEBP", Json{image_webp});
    j.set("AURORA_ENABLE_IMAGE_PNG", Json{image_png});
    return j;
}

auto feature_flags_json() -> Json { return feature_flags().to_json(); }

}  // namespace aurora::debug
