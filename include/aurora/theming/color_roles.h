#pragma once

#include "aurora/core/color.h"

namespace aurora {

/// @brief 色角色全集：Material 3 语义色角色的一整套命名取值（强类型、snake_case）。
///
/// 取代旧的扁平字段（`background` / `primary` / `on_primary` / `text`）与 stringly 命名令牌表。
/// 一个实例只持**一列**（亮或暗）；明暗双列由 `ColorRoles::light()` / `ColorRoles::dark()` 给出，
/// 角色名一套、取值两列（对标 Material 3 color roles 与 Fluent 2 alias tokens 的 light/dark variants）。
///
/// 取值口径分两类：
/// - **已消费角色**（`primary` / `on_primary` / `surface` / `on_surface` / `focus_ring` /
///   `hover` / `pressed` / `error` 系列）：基准值取现行主题与已播种令牌，保证既有消费控件机械
///   迁移后观感不变；暗色 `primary`/`on_primary` 为满足 WCAG AA ≥ 4.5:1 采 M3 色调板修正
///   （primary→tone-80、on_primary→tone-20），是本次唯一有意的观感变化，详见变更记录。
/// - **暂未被控件消费的角色**（`secondary` / `tertiary` 系列及其容器、表面容器族 / 描边族 /
///   反转族 / `scrim` / `shadow` / `surface_tint`）：按 M3 标准色调板从主色 seed 生成（primary
///   低彩度=secondary，hue+60°=tertiary，中性=表面/描边/反转族），已是成套自洽的正式取值；
///   待迁移批各控件接线消费时若观感需微调再校准。全组 `on_*` 与其配对角色均满足 WCAG AA ≥ 4.5:1，
///   由 `utest_theme_roles` 逐对断言。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
struct ColorRoles {
    Color primary;  ///< 主色（强调 / 交互填充）。
    Color on_primary;  ///< 叠加于主色之上的前景色。
    Color primary_container;  ///< 主色容器底。
    Color on_primary_container;  ///< 主色容器之上的前景色。
    Color secondary;  ///< 次色。
    Color on_secondary;  ///< 次色之上的前景色。
    Color secondary_container;  ///< 次色容器底。
    Color on_secondary_container;  ///< 次色容器之上的前景色。
    Color tertiary;  ///< 第三色。
    Color on_tertiary;  ///< 第三色之上的前景色。
    Color tertiary_container;  ///< 第三色容器底。
    Color on_tertiary_container;  ///< 第三色容器之上的前景色。
    Color error;  ///< 错误 / 危险色。
    Color on_error;  ///< 错误色之上的前景色。
    Color error_container;  ///< 错误容器底。
    Color on_error_container;  ///< 错误容器之上的前景色。
    Color surface;  ///< 基础表面（承接旧 `background`）。
    Color surface_dim;  ///< 暗化表面。
    Color surface_bright;  ///< 提亮表面。
    Color surface_container_lowest;  ///< 容器高度阶梯最低。
    Color surface_container_low;  ///< 容器高度阶梯低。
    Color surface_container;  ///< 容器高度阶梯基准。
    Color surface_container_high;  ///< 容器高度阶梯高。
    Color surface_container_highest;  ///< 容器高度阶梯最高。
    Color on_surface;  ///< 表面之上的主前景（承接旧 `text`）。
    Color on_surface_variant;  ///< 变体表面之上的前景。
    Color surface_variant;  ///< 变体表面。
    Color outline;  ///< 描边主色。
    Color outline_variant;  ///< 描边变体（更淡）。
    Color inverse_surface;  ///< 反色表面（如亮色档上的深色浮层）。
    Color inverse_on_surface;  ///< 反色表面之上的前景。
    Color inverse_primary;  ///< 反色档的主色。
    Color scrim;  ///< 遮罩层色（模态背景变暗）。
    Color shadow;  ///< 投影色。
    Color surface_tint;  ///< 表面着色剂（状态层 / 高度叠色）。
    Color focus_ring;  ///< 焦点环取极性色，与主色拉开（承接旧 `focus.ring` 令牌）。
    Color hover;  ///< 交互态悬停叠色（含 alpha）。
    Color pressed;  ///< 交互态按压叠色（含 alpha）。

    /// @brief 亮色档色角色全集。
    /// @return 与现行 `Theme::light()` 观感一致（已消费角色）、其余取 M3 标准中性 / 占位的角色组。
    [[nodiscard]] static constexpr auto light() noexcept -> ColorRoles {
        return ColorRoles{
            // 主色四组：primary / on_primary 取现行值（蓝底白字，观感不变）；容器族取 M3 色调。
            .primary = Color{0x00, 0x00, 0xFF},
            .on_primary = Color{0xFF, 0xFF, 0xFF},
            .primary_container = Color{0xEA, 0xDD, 0xFF},
            .on_primary_container = Color{0x00, 0x00, 0x6F},
            .secondary = Color{0x5C, 0x5C, 0x73},
            .on_secondary = Color{0xFF, 0xFF, 0xFF},
            .secondary_container = Color{0xE1, 0xE0, 0xFB},
            .on_secondary_container = Color{0x18, 0x1A, 0x2D},
            .tertiary = Color{0x89, 0x4B, 0x66},
            .on_tertiary = Color{0xFF, 0xFF, 0xFF},
            .tertiary_container = Color{0xFF, 0xD8, 0xE7},
            .on_tertiary_container = Color{0x3C, 0x03, 0x22},
            .error = Color{0xE0, 0x1B, 0x24},
            .on_error = Color{0xFF, 0xFF, 0xFF},
            .error_container = Color{0xFF, 0xDA, 0xD4},
            .on_error_container = Color{0x3B, 0x09, 0x00},
            // 表面族：surface / on_surface 取现行值（白面黑字）；容器阶梯取 M3 中性色调。
            .surface = Color{0xFF, 0xFF, 0xFF},
            .surface_dim = Color{0xDA, 0xDA, 0xDA},
            .surface_bright = Color{0xF9, 0xF9, 0xF9},
            .surface_container_lowest = Color{0xFF, 0xFF, 0xFF},
            .surface_container_low = Color{0xF3, 0xF3, 0xF3},
            .surface_container = Color{0xEE, 0xEE, 0xEE},
            .surface_container_high = Color{0xE8, 0xE8, 0xE8},
            .surface_container_highest = Color{0xE2, 0xE2, 0xE2},
            .on_surface = Color{0x00, 0x00, 0x00},
            .on_surface_variant = Color{0x47, 0x47, 0x47},
            .surface_variant = Color{0xE2, 0xE2, 0xE2},
            // 描边 / 反转 / 其他
            .outline = Color{0x77, 0x77, 0x77},
            .outline_variant = Color{0xC6, 0xC6, 0xC6},
            .inverse_surface = Color{0x30, 0x30, 0x30},
            .inverse_on_surface = Color{0xF1, 0xF1, 0xF1},
            .inverse_primary = Color{0xC6, 0xC6, 0xC6},
            .scrim = Color{0x00, 0x00, 0x00},
            .shadow = Color{0x00, 0x00, 0x00},
            .surface_tint = Color{0x5E, 0x5E, 0x5E},
            // Aurora 扩展角色（取现行令牌基准，观感不变）
            .focus_ring = Color{0x00, 0x00, 0x00},
            .hover = Color{0, 0, 0, 32},
            .pressed = Color{0, 0, 0, 56},
        };
    }

    /// @brief 暗色档色角色全集。
    /// @return 深底（`#202124`）+ WCAG 修正后的主色族 + M3 中性暗色表面族。
    [[nodiscard]] static constexpr auto dark() noexcept -> ColorRoles {
        return ColorRoles{
            // 主色四组：primary / on_primary 为 WCAG 修正对（tone-80 主色 + tone-20 前景，7.73:1）。
            .primary = Color{0xC0, 0xC1, 0xFF},
            .on_primary = Color{0x00, 0x2B, 0x73},
            .primary_container = Color{0x00, 0x40, 0xA1},
            .on_primary_container = Color{0xE0, 0xE0, 0xFF},
            .secondary = Color{0xC5, 0xC4, 0xDE},
            .on_secondary = Color{0x2E, 0x2F, 0x43},
            .secondary_container = Color{0x44, 0x45, 0x5B},
            .on_secondary_container = Color{0xE1, 0xE0, 0xFB},
            .tertiary = Color{0xF9, 0xB3, 0xCF},
            .on_tertiary = Color{0x55, 0x1B, 0x37},
            .tertiary_container = Color{0x6E, 0x33, 0x4E},
            .on_tertiary_container = Color{0xFF, 0xD8, 0xE7},
            .error = Color{0xE0, 0x1B, 0x24},
            .on_error = Color{0xFF, 0xFF, 0xFF},
            .error_container = Color{0x93, 0x00, 0x10},
            .on_error_container = Color{0xFF, 0xDA, 0xD4},
            // 表面族：surface 取旧 background 暗值（command_palette 消费，观感不变）；黑底白字。
            .surface = Color{0x20, 0x21, 0x24},
            .surface_dim = Color{0x13, 0x13, 0x13},
            .surface_bright = Color{0x39, 0x39, 0x39},
            .surface_container_lowest = Color{0x0E, 0x0E, 0x0E},
            .surface_container_low = Color{0x1B, 0x1B, 0x1B},
            .surface_container = Color{0x1F, 0x1F, 0x1F},
            .surface_container_high = Color{0x2A, 0x2A, 0x2A},
            .surface_container_highest = Color{0x35, 0x35, 0x35},
            .on_surface = Color{0xFF, 0xFF, 0xFF},
            .on_surface_variant = Color{0xC6, 0xC6, 0xC6},
            .surface_variant = Color{0x47, 0x47, 0x47},
            // 描边 / 反转 / 其他
            .outline = Color{0x91, 0x91, 0x91},
            .outline_variant = Color{0x47, 0x47, 0x47},
            .inverse_surface = Color{0xE2, 0xE2, 0xE2},
            .inverse_on_surface = Color{0x30, 0x30, 0x30},
            .inverse_primary = Color{0x5E, 0x5E, 0x5E},
            .scrim = Color{0x00, 0x00, 0x00},
            .shadow = Color{0x00, 0x00, 0x00},
            .surface_tint = Color{0xC6, 0xC6, 0xC6},
            // Aurora 扩展角色
            .focus_ring = Color{0xFF, 0xFF, 0xFF},
            .hover = Color{255, 255, 255, 32},
            .pressed = Color{255, 255, 255, 56},
        };
    }
};

}  // namespace aurora
