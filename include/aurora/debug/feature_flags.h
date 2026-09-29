#pragma once

/// @brief 编译期 feature 宏开关的运行时查询门面（specification/06-app-platform.md §11）。
/// @file feature_flags.h
///
/// 设计要点：
/// - **归一化镜像**：全部 AURORA_* feature 宏的取值只在 feature_flags.cpp 单点收口
///   （与 BUILD_OPTIONS.md 的开关清单一一对应），消费者禁止散写 `#ifdef` 探测能力
///   （需求 SPEC.PLATFORM.ZERO-IFDEF.14：运行时查询替代宏分支）。
/// - **始终可用**（gated = none）：其意义正在于报告「当前编译开了什么」，若被
///   AURORA_ENABLE_DEBUG 门控则 Release 下将无从获知，自相矛盾。
/// - 结果为**编译期常量快照**：反映链接进来的 aurora 静态库的宏取值，与运行环境无关。
/// - JSON 键 = 完整宏名（自描述），供 Inspector / CLI / MCP 等工具直读。

#include "aurora/widget/props_io.h"

namespace aurora::debug {

/// @brief 当前 aurora 静态库编译时启用的 feature 宏集合（字段名 = 宏名去前缀小写）。
/// 字段按 BUILD_OPTIONS.md 三层命名分组：AURORA_BACKEND_*（§3）、架构级优化（§3.3）、
/// AURORA_ENABLE_* 内部能力（§4，含 AURORA_ENABLE_IMAGE_* 编解码能力，§4.6）。
/// 注：`AURORA_BUILD_INSPECTOR_SERVER` 定义在独立目标 aurora_inspector_server 上，
/// 主库不可见，故不在本结构内。
struct FeatureFlags {
    bool backend_headless = false;  ///< AURORA_BACKEND_HEADLESS：内存/离线渲染后端（HeadlessSurface）。
    bool backend_win32 = false;  ///< AURORA_BACKEND_WIN32：Win32/GDI 表面后端（仅 _WIN32）。
    bool backend_d3d11 = false;  ///< AURORA_BACKEND_D3D11：D3D11 GPU 增量上屏表面后端（仅 Windows）。
    bool backend_glfw = false;  ///< AURORA_BACKEND_GLFW：GLFW + OpenGL 3.x 兼容 profile 表面后端。
    bool backend_x11 = false;  ///< AURORA_BACKEND_X11：X11/Xlib 桌面表面后端（仅 Linux）。
    bool backend_wayland = false;  ///< AURORA_BACKEND_WAYLAND：原生 Wayland 表面后端（wl_shm + xdg-shell）。
    bool backend_macos = false;  ///< AURORA_BACKEND_MACOS：AppKit/CoreGraphics 表面后端（骨架）。
    bool backend_wasm = false;  ///< AURORA_BACKEND_WASM：Emscripten/Canvas 2D 浏览器表面后端。
    bool layout_cache = false;  ///< AURORA_ENABLE_LAYOUT_CACHE：布局约束缓存，约束不变且非脏时跳过子树递归。
    bool occlusion_culling = false;  ///< AURORA_ENABLE_OCCLUSION_CULLING：遮挡剔除，跳过不与裁剪区相交的子控件绘制。
    bool display_list = false;  ///< AURORA_ENABLE_DISPLAY_LIST：Display List 录制/回放，子树未脏时跳过 paint 遍历。
    bool simd = false;  ///< AURORA_ENABLE_SIMD：光栅内核 SIMD 双实现（SSE2 基线 + AVX2 运行时分发）。
    bool profiling = false;  ///< AURORA_ENABLE_PROFILING：渲染性能插桩（作用域计时 + 渲染计数器）。
    bool tracing = false;  ///< AURORA_ENABLE_TRACING：Chrome Trace Event 时间线落盘（隐含 profiling）。
    bool debug = false;  ///< AURORA_ENABLE_DEBUG：真实后端 DEBUG 能力（截图/控件树/性能快照/叠层/拾取）。
    bool audio = false;  ///< AURORA_ENABLE_AUDIO：内置音频设备后端（AudioContext 图 API 恒编译，关=静默模式）。
    bool enable_audio_wasapi = false;  ///< AURORA_ENABLE_AUDIO_WASAPI：WASAPI 设备后端（Windows，依赖 audio）。
    bool enable_audio_alsa = false;  ///< AURORA_ENABLE_AUDIO_ALSA：ALSA 设备后端（Linux，dlopen 运行期绑定）。
    bool enable_audio_webaudio = false;  ///< AURORA_ENABLE_AUDIO_WEBAUDIO：WebAudio 设备后端（浏览器）。
    bool image_jpeg = false;  ///< AURORA_ENABLE_IMAGE_JPEG：JPEG 解码能力。
    bool image_webp = false;  ///< AURORA_ENABLE_IMAGE_WEBP：WebP 解码能力。
    bool image_png = false;  ///< AURORA_ENABLE_IMAGE_PNG：PNG 编解码能力。

    /// @brief JSON 导出：键 = 完整宏名（如 "AURORA_BACKEND_HEADLESS"），值 = bool。
    /// @return JSON 对象，全部字段的键序与本结构字段声明序一致。
    [[nodiscard]] auto to_json() const -> Json;
};

/// @brief 读取当前编译的 feature 宏开关集合（始终可用，编译期常量快照）。
/// @return 逐字段镜像链接库内 AURORA_* 宏取值的 FeatureFlags 快照。
[[nodiscard]] auto feature_flags() -> FeatureFlags;

/// @brief feature_flags().to_json() 便捷封装。
/// @return 同 FeatureFlags::to_json()，键为完整宏名、值为 bool。
[[nodiscard]] auto feature_flags_json() -> Json;

}  // namespace aurora::debug
