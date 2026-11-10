#pragma once

#include <array>
#include <string_view>
#include <utility>

#include "aurora/core/color.h"

namespace aurora {

/// @brief 色角色全集：Material 3 语义色角色的一整套命名取值（强类型、snake_case）。
///
/// 取代旧的扁平字段（`background` / `primary` / `on_primary` / `text`）与 stringly 命名令牌表。
/// 一个实例只持**一列**（亮或暗）；明暗双列由 `ColorRoles::light()` / `ColorRoles::dark()` 给出，
/// 角色名一套、取值两列（对标 Material 3 color roles 与 Fluent 2 alias tokens 的 light/dark variants）。
/// 高对比**不是第三列**：`with_high_contrast()` 从既有那一列派生，取值出处只有判据登记表
/// `detail/contrast_audit.h` 一处，不存在另打一份手时常量的第二张配色表。
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
    Color scrollbar_thumb;  ///< 滚动条滑块（形状承载当前滚动位置，非文本档）。
    Color scrollbar_track;  ///< 滚动条轨道凹槽（半透明叠层，登记为不适用）。

    /// @brief 逐字段相等比较（四十项全等）；供主题解析、「风格档 ≡ 基线档」断言与快照的同值守卫。
    /// @param other 比较对象。
    /// @return 全部字段相等时为 true。
    [[nodiscard]] auto operator==(const ColorRoles &other) const -> bool = default;

    /// @brief 「成员指针 → 令牌短名」对照表（`AURORA_TOKENS.size()` 由列表推出，不手写枚数）。
    ///
    /// 短名即 `aurora_api.json` 的 `default_expr` 用名（沿用既有的 `theme.<角色>` 拼法，与 C++ 真路径
    /// `roles.<角色>` 的前缀映射由本表登记，规则见 `codespec/specification/07-environment-modifier.md`
    /// §5.1）；表与实际角色的一致性由令牌登记表门禁校验。
    static constexpr auto AURORA_TOKENS = std::to_array<std::pair<Color ColorRoles::*, std::string_view>>({
        {&ColorRoles::primary, "theme.primary"},
        {&ColorRoles::on_primary, "theme.on_primary"},
        {&ColorRoles::primary_container, "theme.primary_container"},
        {&ColorRoles::on_primary_container, "theme.on_primary_container"},
        {&ColorRoles::secondary, "theme.secondary"},
        {&ColorRoles::on_secondary, "theme.on_secondary"},
        {&ColorRoles::secondary_container, "theme.secondary_container"},
        {&ColorRoles::on_secondary_container, "theme.on_secondary_container"},
        {&ColorRoles::tertiary, "theme.tertiary"},
        {&ColorRoles::on_tertiary, "theme.on_tertiary"},
        {&ColorRoles::tertiary_container, "theme.tertiary_container"},
        {&ColorRoles::on_tertiary_container, "theme.on_tertiary_container"},
        {&ColorRoles::error, "theme.error"},
        {&ColorRoles::on_error, "theme.on_error"},
        {&ColorRoles::error_container, "theme.error_container"},
        {&ColorRoles::on_error_container, "theme.on_error_container"},
        {&ColorRoles::surface, "theme.surface"},
        {&ColorRoles::surface_dim, "theme.surface_dim"},
        {&ColorRoles::surface_bright, "theme.surface_bright"},
        {&ColorRoles::surface_container_lowest, "theme.surface_container_lowest"},
        {&ColorRoles::surface_container_low, "theme.surface_container_low"},
        {&ColorRoles::surface_container, "theme.surface_container"},
        {&ColorRoles::surface_container_high, "theme.surface_container_high"},
        {&ColorRoles::surface_container_highest, "theme.surface_container_highest"},
        {&ColorRoles::on_surface, "theme.on_surface"},
        {&ColorRoles::on_surface_variant, "theme.on_surface_variant"},
        {&ColorRoles::surface_variant, "theme.surface_variant"},
        {&ColorRoles::outline, "theme.outline"},
        {&ColorRoles::outline_variant, "theme.outline_variant"},
        {&ColorRoles::inverse_surface, "theme.inverse_surface"},
        {&ColorRoles::inverse_on_surface, "theme.inverse_on_surface"},
        {&ColorRoles::inverse_primary, "theme.inverse_primary"},
        {&ColorRoles::scrim, "theme.scrim"},
        {&ColorRoles::shadow, "theme.shadow"},
        {&ColorRoles::surface_tint, "theme.surface_tint"},
        {&ColorRoles::focus_ring, "theme.focus_ring"},
        {&ColorRoles::hover, "theme.hover"},
        {&ColorRoles::pressed, "theme.pressed"},
        {&ColorRoles::scrollbar_thumb, "theme.scrollbar_thumb"},
        {&ColorRoles::scrollbar_track, "theme.scrollbar_track"},
    });

    /// @brief 反查一枚色角色的令牌短名（编译期与运行期同形，零分配）。
    /// @param member 本结构体的成员指针；传空指针不命中。
    /// @return 命中时为形如 `theme.primary` 的短名；未登记时为空串（调用方须判空）。
    [[nodiscard]] static constexpr auto token_name(Color ColorRoles::*member) noexcept -> std::string_view {
        for (const auto &[ptr, name] : AURORA_TOKENS) {
            if (ptr == member) {
                return name;
            }
        }
        return {};
    }

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
            // 滚动条：滑块取中性灰（白底约 5.1:1），轨道是调用点按固定 alpha 合成的凹槽。
            .scrollbar_thumb = Color{0x6E, 0x6E, 0x6E},
            .scrollbar_track = Color{0, 0, 0, 20},
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
            // 滚动条：滑块取中性灰（#202124 底约 5.7:1），轨道是调用点按固定 alpha 合成的凹槽。
            .scrollbar_thumb = Color{0x9A, 0x9A, 0x9A},
            .scrollbar_track = Color{255, 255, 255, 20},
        };
    }

    /// @brief 高对比派生列：按判据登记表 `detail::AURORA_CONTRAST_AUDITS` 整族调 tone，让每一行都
    ///        达到它自己的档位下限——本库对 `AccessibilitySettings::high_contrast` 的唯一响应通道。
    ///
    /// 派生只改**相对亮度**、不重新配色：把线性 RGB 等比缩放到目标亮度（仅当目标越出色域壳时才
    /// 向白点收敛），故观感是「同一套配色的强对比版」。可动端优先取底色、其次取前景——前景标签
    /// 一动即换观感；当某底色为满足它自己的档位而必须移动时，压在它上面的前景标签在同一步内被
    /// 修复（只在登记表里作前景的角色不会连带破坏其他行，修复因此不级联）。纯底色（只作背景出现
    /// 者，`surface` 族等）恒不动：高对比语义下页面地不得移位。
    ///
    /// 判据表是唯一规则来源：本函数、`check_theme_contrast` 门禁与 `utest_theme_roles` 读同一张表、
    /// 同一份 `aurora::contrast_ratio`，不存在第二套对比度口径。档位下限就地再乘一点余量，避免
    /// 8 位量化把 4.4998 这类值压到线下。
    ///
    /// @return 逐行达标的副本；本已全达标的列返回逐字段相等的副本（不扰动，默认档 golden 因此不变）。
    /// @note Thread: thread-safe (pure value type)
    /// @note Side-effects: none
    [[nodiscard]] auto with_high_contrast() const -> ColorRoles;
};

}  // namespace aurora
