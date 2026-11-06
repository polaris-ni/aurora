#pragma once

#include <array>
#include <string_view>
#include <utility>

#include "aurora/core/duration.h"

// Windows SDK 的 rpcndr.h 把 small 定义为 char（经 ole2/oleauto 等头随 windows.h 带入）；
// 本头的时长字段以 small 命名，宏在生效时会把字段声明改写成 `Duration char{...}`、把成员指针
// 登记改写为 `&MotionScale::char`。与 shape_scale / spacing_scale 同法撤掉该 SDK 遗留宏。
#ifdef small
#undef small
#endif

namespace aurora {

/// @brief 动效令牌：时长档 6 格，每格一枚 `Duration`（强类型、snake_case、不分明暗）。
///
/// 与 `ColorRoles`（配色）/ `TypeScale`（排印）/ `ShapeScale`（形状）/ `SpacingScale`（间距）同构，是
/// `Theme` 的一等字段（`theme.motion`），供动效与时长类属性以**成员指针**作活令牌回落点
/// （`.fallback<&MotionScale::medium>()`）。档名对标 Material 3 motion tokens 的尺寸轴命名。
///
/// 取值口径：`medium` = 150 ms 锚定本库现行滚动视口滑行动效的默认时长（`default_duration_s()`），
/// 其余档按 M3 短/中时长梯度取值；`none` = 0 是**无障碍短路档**——系统「减弱动态效果」开关置位时
/// 快照的本档整族取零，动画侧一步落端点（与 `AnimationController` 现行的 `reduce_motion` 短路同语义）。
///
/// @note 本轮时长档尚无属性行消费端（库内唯一时长型属性 `Skeleton::duration` 是 shimmer **周期**而非
///       动效时长，默认 1.5 s，不属本档梯度）；接线点在动效侧改读解析入口的那一批，未接线前本档按
///       「未消费档」登记，不得当作已生效能力描述。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
struct MotionScale {
    Duration none{Duration::from_ms(0.0)};  ///< 零时长档（减动效开关的短路取值）。
    Duration extra_small{Duration::from_ms(50.0)};  ///< 极短档（状态层淡入）。
    Duration small{Duration::from_ms(100.0)};  ///< 短档（图标 / 勾选切换）。
    Duration medium{Duration::from_ms(150.0)};  ///< 基准档（滚动滑行 / 悬停态过渡）。
    Duration large{Duration::from_ms(250.0)};  ///< 中档（展开 / 折叠）。
    Duration extra_large{Duration::from_ms(400.0)};  ///< 长档（页面级转场）。

    /// @brief 逐字段相等比较（六档全等）；供主题快照的「同值覆写不触发重排重绘」判据。
    /// @param other 比较对象。
    /// @return 全部字段相等时为 true。
    [[nodiscard]] auto operator==(const MotionScale &other) const -> bool = default;

    /// @brief 「成员指针 → 令牌短名」对照表（`AURORA_TOKENS.size()` 由列表推出，不手写档数）。
    ///
    /// 短名即 `aurora_api.json` 的 `default_expr` 用名，前缀映射规则见
    /// `codespec/specification/07-environment-modifier.md` §5.1；表与实际字段的一致性由令牌登记表门禁校验。
    static constexpr auto AURORA_TOKENS = std::to_array<std::pair<Duration MotionScale::*, std::string_view>>({
        {&MotionScale::none, "theme.motion.none"},
        {&MotionScale::extra_small, "theme.motion.extra_small"},
        {&MotionScale::small, "theme.motion.small"},
        {&MotionScale::medium, "theme.motion.medium"},
        {&MotionScale::large, "theme.motion.large"},
        {&MotionScale::extra_large, "theme.motion.extra_large"},
    });

    /// @brief 反查一枚时长档的令牌短名（编译期与运行期同形，零分配）。
    /// @param member 本结构体的成员指针；传空指针不命中。
    /// @return 命中时为形如 `theme.motion.medium` 的短名；未登记时为空串（调用方须判空）。
    [[nodiscard]] static constexpr auto token_name(Duration MotionScale::*member) noexcept -> std::string_view {
        for (const auto &[ptr, name] : AURORA_TOKENS) {
            if (ptr == member) {
                return name;
            }
        }
        return {};
    }
};

}  // namespace aurora
