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

/// @brief 形状档位（圆角令牌）：一套风格六档，单位逻辑 dp。
///
/// `Theme` 的纯值字段（与 `ColorRoles` / `TypeScale` 同层）。控件的形状默认值读本组令牌，
/// 显式设值仍优先（两态语义不变）。档位名按尺寸语义命名、不绑死用途，故同一档位可同时
/// 服务圆角与其它几何倾向。
///
/// 初值逐位复现令牌层落地前的硬编码：`Button` 圆角 6 即 `small`，切风格前观感零变化。
///
/// 本组令牌同时是属性行的**活令牌回落点**：回落点以成员指针形态写进表行
/// （`.fallback<&ShapeScale::small>()`），编译期烧成一次成员指针解引用，令牌改名即编译失败，
/// 渲染路径不出现字符串查名（`CODING_STANDARDS.md` §6「stringly 只允许存在于序列化边界」）。
///
/// 本档只服务**标量**几何属性：四边结构的 `EdgeInsets` 与单一标量档不同型，接档需先裁「一个档
/// 展开成四边同值」还是「一组四元档」，那是新的语义，不在本档面内。
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

    /// @brief 逐字段相等比较（六档全等）；供主题解析、测试断言与快照的同值守卫。
    /// @param other 比较对象。
    /// @return 全部档位相等时为 true。
    [[nodiscard]] auto operator==(const ShapeScale &other) const -> bool = default;

    /// @brief 「成员指针 → 令牌短名」对照表（`AURORA_TOKENS.size()` 由列表推出，不手写档数）。
    ///
    /// 短名即 `aurora_api.json` 的 `default_expr` 用名，前缀映射规则见
    /// `codespec/specification/07-environment-modifier.md` §5.1；表与实际字段的一致性由令牌登记表门禁校验。
    static constexpr auto AURORA_TOKENS = std::to_array<std::pair<float ShapeScale::*, std::string_view>>({
        {&ShapeScale::none, "theme.shapes.none"},
        {&ShapeScale::extra_small, "theme.shapes.extra_small"},
        {&ShapeScale::small, "theme.shapes.small"},
        {&ShapeScale::medium, "theme.shapes.medium"},
        {&ShapeScale::large, "theme.shapes.large"},
        {&ShapeScale::extra_large, "theme.shapes.extra_large"},
    });

    /// @brief 反查一枚形状档的令牌短名（编译期与运行期同形，零分配）。
    /// @param member 本结构体的成员指针；传空指针不命中。
    /// @return 命中时为形如 `theme.shapes.small` 的短名；未登记时为空串（调用方须判空）。
    [[nodiscard]] static constexpr auto token_name(float ShapeScale::*member) noexcept -> std::string_view {
        for (const auto &[ptr, name] : AURORA_TOKENS) {
            if (ptr == member) {
                return name;
            }
        }
        return {};
    }
};

}  // namespace aurora
