#pragma once

#include <cstdint>

namespace aurora::render {

/**
 * @brief 文本抗锯齿策略（FreeType 驱动，跨平台一致、确定性）。
 *
 * - `Supersample`：灰度 AA——`FT_RENDER_MODE_NORMAL` 输出 A8 覆盖度，盒式合成。
 *   颜色安全、背景无关，对半透明文本与任意背景均正确。**优点**：在**多色/非均匀/渐变背景**
 *   上不会出现红/蓝子像素羽化。
 * - `ClearType`：屏幕最佳——`FT_RENDER_MODE_LCD` 输出 3× 水平 RGB 子像素覆盖度，由
 *   `Painter::blend_subpixel` 逐通道合成，得到真·子像素锐利文本（非灰度降级）。
 *   仅当文本不透明（`c.a == 255`）时使用，否则自动回退 `Supersample`。
 *   **限制**：子像素着色在**多色/非均匀/渐变背景**上会出现轻微红/蓝羽化（与系统 ClearType 同构）；
 *   跨机一致（字体由引擎内置打包），不依赖系统 ClearType 调谐。
 */
enum class TextAAMode : std::uint8_t {
    Supersample,
    ClearType,
};

}  // namespace aurora::render