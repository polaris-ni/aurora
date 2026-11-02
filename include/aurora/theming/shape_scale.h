#pragma once

namespace aurora {

/// @brief 形状档位（圆角令牌）：一套风格六档，单位逻辑 dp。
///
/// `Theme` 的纯值字段（与 `ColorRoles` / `TypeScale` 同层）。控件的形状默认值读本组令牌，
/// 显式设值仍优先（两态语义不变）。档位名按尺寸语义命名、不绑死用途，故同一档位可同时
/// 服务圆角与其它几何倾向。
///
/// 初值逐位复现令牌层落地前的硬编码：`Button` 圆角 6 即 `small`，切风格前观感零变化。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
struct ShapeScale {
    float none = 0.0F;  ///< 直角（0dp）。
    float extra_small = 4.0F;  ///< 极小圆角（4dp）。
    float small = 6.0F;  ///< 小圆角（6dp）——现行控件默认档。
    float medium = 8.0F;  ///< 中圆角（8dp）。
    float large = 12.0F;  ///< 大圆角（12dp）。
    float extra_large = 16.0F;  ///< 特大圆角（16dp）。

    /// @brief 逐字段相等比较（六档全等）；供主题解析与测试断言。
    /// @param other 比较对象。
    /// @return 全部档位相等时为 true。
    [[nodiscard]] auto operator==(const ShapeScale &other) const -> bool = default;
};

}  // namespace aurora
