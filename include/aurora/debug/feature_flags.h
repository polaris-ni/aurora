#pragma once

// 编译期 feature 宏开关的运行时查询门面（specification/06-app-platform.md §11.2）。
//
// 设计要点：
// - **归一化镜像**：全部 AURORA_* feature 宏的取值只在 feature_flags.cpp 单点收口
//   （与 BUILD_OPTIONS.md 的开关清单一一对应），消费者禁止散写 #ifdef 探测能力
//   （需求 #14：运行时查询替代宏分支）。
// - **始终可用**（gated = none）：其意义正在于报告「当前编译开了什么」，若被
//   AURORA_ENABLE_DEBUG 门控则 Release 下将无从获知，自相矛盾。
// - 结果为**编译期常量快照**：反映链接进来的 aurora 静态库的宏取值，与运行环境无关。
// - JSON 键 = 完整宏名（自描述），供 Inspector / CLI / MCP 等工具直读。

#include "aurora/widget/props_io.h"  // Json

namespace aurora::debug {

/// @brief 当前 aurora 静态库编译时启用的 feature 宏集合（字段名 = 宏名去前缀小写）。
///
/// 字段按 BUILD_OPTIONS.md 三层命名分组：AURORA_BACKEND_*（§3）、架构级优化（§3.3）、
/// AURORA_ENABLE_* 内部能力（§4，含 AURORA_ENABLE_IMAGE_* 编解码能力，§4.6）。
/// 注：`AURORA_BUILD_INSPECTOR_SERVER` 定义在独立目标 aurora_inspector_server 上，
/// 主库不可见，故不在本结构内。
struct FeatureFlags {
    // ---- AURORA_BACKEND_*（后端）----
    bool backend_headless = false;
    bool backend_win32 = false;
    bool backend_d3d11 = false;
    bool backend_glfw = false;
    bool backend_x11 = false;
    bool backend_wayland = false;
    bool backend_macos = false;
    bool backend_wasm = false;
    // ---- 架构级优化 ----
    bool layout_cache = false;
    bool occlusion_culling = false;
    bool display_list = false;
    // ---- AURORA_ENABLE_*（内部能力）----
    bool simd = false;
    bool profiling = false;
    bool tracing = false;
    bool debug = false;
    // ---- AURORA_ENABLE_IMAGE_*（编解码能力）----
    bool image_jpeg = false;
    bool image_webp = false;
    bool image_png = false;

    /// @brief JSON 导出：键 = 完整宏名（如 "AURORA_BACKEND_HEADLESS"），值 = bool。
    [[nodiscard]] auto to_json() const -> Json;
};

/// @brief 读取当前编译的 feature 宏开关集合（始终可用，编译期常量快照）。
[[nodiscard]] auto feature_flags() -> FeatureFlags;

/// @brief feature_flags().to_json() 便捷封装。
[[nodiscard]] auto feature_flags_json() -> Json;

}  // namespace aurora::debug
