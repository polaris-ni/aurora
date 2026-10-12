#pragma once

#include <cstdint>

namespace aurora {

/// @brief 明暗配色档位（三态：未指定 / 亮 / 暗）。
///
/// 三级明暗优先链（`codespec/changes/window-csd-style-system/proposal.md`）的取值类型：
/// 显式配置 > `Theme` 令牌（`Theme::scheme`）> OS 偏好（`os_color_scheme()`）。
/// `Unspecified` 不等于「亮」——它表示「该层没有意见」，由下一层继续解析；
/// 全链未指定时由调用方回落预设默认档（`CsdPreset::AdwaitaDark`）。
enum class ColorScheme : std::uint8_t {
    Unspecified,  ///< 未指定（该层不做决定，交由优先链下一层解析）。
    Light,  ///< 亮色档。
    Dark,  ///< 暗色档。
};

}  // namespace aurora
