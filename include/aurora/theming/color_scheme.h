#pragma once

#include <cstdint>

namespace aurora {

/// @brief 明暗配色档位（三态：未指定 / 亮 / 暗）。
///
/// 三级明暗优先链的取值类型：
/// 显式 `Window::set_theme` > OS 偏好（`os_color_scheme()`）> 明色。`Unspecified` 不等于
/// 「亮」——它表示「该层没有意见」，由下一层继续解析；全链未表态时回落明色（「`Unspecified`
/// ≡ 明色列」是根注入链底 / CSD 桥基座的单一口径，见 `codespec/specification/07-environment-modifier.md` §5）。
enum class ColorScheme : std::uint8_t {
    Unspecified,  ///< 未指定（该层不做决定，交由优先链下一层解析）。
    Light,  ///< 亮色档。
    Dark,  ///< 暗色档。
};

}  // namespace aurora
