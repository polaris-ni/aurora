#pragma once

#include <array>
#include <string_view>
#include <utility>

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
/// 本组令牌同时是属性行的**活令牌回落点**（成员指针形态，口径同 `ShapeScale`）。直接消费端是
/// `Column` / `Row` / `Grid` / `Form` / `ReorderableList` 的 `gap` 一类 **float** 属性；四边结构的
/// `EdgeInsets` 与单一标量档不同型，本轮不接活令牌（同 `ShapeScale` 的口径）。
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

    /// @brief 逐字段相等比较（七档全等）；供主题解析、测试断言与快照的同值守卫。
    /// @param other 比较对象。
    /// @return 全部档位相等时为 true。
    [[nodiscard]] auto operator==(const SpacingScale &other) const -> bool = default;

    /// @brief 「成员指针 → 令牌短名」对照表（`AURORA_TOKENS.size()` 由列表推出，不手写档数）。
    ///
    /// 短名即 `aurora_api.json` 的 `default_expr` 用名，前缀映射规则见
    /// `codespec/specification/07-environment-modifier.md` §5.1；表与实际字段的一致性由令牌登记表门禁校验。
    static constexpr auto AURORA_TOKENS = std::to_array<std::pair<float SpacingScale::*, std::string_view>>({
        {&SpacingScale::none, "theme.spacing.none"},
        {&SpacingScale::extra_small, "theme.spacing.extra_small"},
        {&SpacingScale::small, "theme.spacing.small"},
        {&SpacingScale::medium, "theme.spacing.medium"},
        {&SpacingScale::large, "theme.spacing.large"},
        {&SpacingScale::extra_large, "theme.spacing.extra_large"},
        {&SpacingScale::huge, "theme.spacing.huge"},
    });

    /// @brief 反查一枚间距档的令牌短名（编译期与运行期同形，零分配）。
    /// @param member 本结构体的成员指针；传空指针不命中。
    /// @return 命中时为形如 `theme.spacing.large` 的短名；未登记时为空串（调用方须判空）。
    [[nodiscard]] static constexpr auto token_name(float SpacingScale::*member) noexcept -> std::string_view {
        for (const auto &[ptr, name] : AURORA_TOKENS) {
            if (ptr == member) {
                return name;
            }
        }
        return {};
    }
};

}  // namespace aurora
