#pragma once

// 真实后端 DEBUG 能力门面（specification/06-app-platform.md §11）。
//
// 设计要点：
// - 本头**始终声明** `aurora::debug` 门面函数（capture / surface_state / 输出目录 API），
//   使消费端调用可编译、Surface 契约稳定（ODR 安全）。
// - 函数**体**按 `AURORA_ENABLE_DEBUG` 裁切：Release（未开开关）构建下，`capture` 返回
//   disabled 错误、`surface_state` 返回 unavailable JSON；零截图 / 零调试代码被编译进 Release。
// - 输出目录 API（set_output_directory / output_directory / resolve_output_path）为纯文件系统
//   辅助，无调试内部依赖，始终可用（不门控）。
//
// 收编原则：本文件是门面，不搬迁任何生产子系统引擎（Inspector / Diagnostics / perf 留原地）。

#include <cstdint>
#include <string>

#include "aurora/core/result.h"
#include "aurora/widget/props_io.h"
#include "aurora/window/surface.h"

namespace aurora::debug {

/// @brief 截图源：软件帧缓冲 / 真实屏幕窗口。
enum class CaptureSource : std::uint8_t {
    Framebuffer,  ///< Surface 软件帧缓冲（RGBA，全后端通用、确定性）。
    OnScreenWindow,  ///< 真实屏幕窗口（含 OS 装饰，按后端能力尽力；Wayland/Headless 不支持）。
};

/// @brief 抓取截图。
/// @param s 目标 Surface。
/// @param path 输出 PNG 路径；规则见 `resolve_output_path()`：纯文件名（无目录）落入
///             `output_directory()`；含目录（绝对或相对带分隔）按原样使用。目标父目录自动创建。
/// @param src 截图源，默认软件帧缓冲。
/// @return 成功返回 true；失败（如 Release 未开 DEBUG、后端不支持、写盘失败）返回结构化错误。
[[nodiscard]] auto capture(Surface &s, const std::string &path, CaptureSource src = CaptureSource::Framebuffer)
    -> Result<bool>;

/// @brief 将「相对且无目录」的输出路径解析进缺省输出目录；含目录的路径原样返回。
///        绝对路径、或已含目录分隔的相对路径均视为显式路径，不改动。
/// @note 空串返回 `output_directory()`（即仅目录，无文件名）。
[[nodiscard]] auto resolve_output_path(const std::string &path) -> std::string;

/// @brief 设置缺省输出目录（影响 `capture` / `save_snapshot` 的目录解析）。
///        传入空串恢复为「当前程序运行目录下的 ./aurora_debug/」缺省值。
auto set_output_directory(const std::string &dir) -> void;

/// @brief 当前缺省输出目录（未显式设置时为 current_path()/aurora_debug，懒计算）。
[[nodiscard]] auto output_directory() -> std::string;

/// @brief Surface 运行时状态快照（JSON）：size / scale_factor / frame_count / clear_color /
///        should_close / has_native_window（全部来自 Surface 既有公共接口，无后端私有访问）。
///        Release（未开 DEBUG）返回 `{"available":false, "reason": ...}`。
[[nodiscard]] auto surface_state(const Surface &s) -> Json;

}  // namespace aurora::debug
