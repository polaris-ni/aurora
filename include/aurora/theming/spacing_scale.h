#pragma once

// Windows SDK 的 rpcndr.h 把 small 定义为 char（经 ole2/oleauto 等头随 windows.h 带入）；
// 本头的档位字段以 small 命名，宏在生效时会把字段声明改写成 `float char = 6.0F`。此处撤掉该 SDK 遗留宏。
#ifdef small
#undef small
#endif

namespace aurora {

/// @brief 间距档位令牌：一套风格七档，单位逻辑 dp。
///
/// `Theme` 的纯值字段（与 `ColorRoles` / `ShapeScale` 同层）。控件的留白、内/外边距默认值
/// 读本组档位；显式设值仍优先。
///
/// 初值逐位复现令牌层落地前的硬编码：`Button` 内边距水平 12 / 垂直 6 即
/// `large` / `small`，图表留白 8 即 `medium`，切风格前观感零变化。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
struct SpacingScale {
    float none = 0.0F;  ///< 无间距（0dp）。
    float extra_small = 4.0F;  ///< 极小间距（4dp）。
    float small = 6.0F;  ///< 小间距（6dp）——现行控件的垂直内边距档。
    float medium = 8.0F;  ///< 中间距（8dp）。
    float large = 12.0F;  ///< 大间距（12dp）——现行控件的水平内边距档。
    float extra_large = 16.0F;  ///< 特大间距（16dp）。
    float huge = 24.0F;  ///< 超大间距（24dp）。

    /// @brief 逐字段相等比较（七档全等）；供主题解析与测试断言。
    /// @param other 比较对象。
    /// @return 全部档位相等时为 true。
    [[nodiscard]] auto operator==(const SpacingScale &other) const -> bool = default;
};

}  // namespace aurora
