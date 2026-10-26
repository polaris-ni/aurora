#pragma once

#include <cstdint>

namespace aurora {

/// @brief 无效词条 Id 的原始值（与二进制词表的缺译哨兵同值，不参与词表分配）。
inline constexpr std::uint32_t AURORA_INVALID_MESSAGE_ID_VALUE = 0xFFFFFFFFU;

/// @brief 本地化词条 Id（i18n 运行时，specification/07-environment-modifier.md §6 国际化）。
///
/// 全库唯一的强类型：各词表（库内置词表 / 下游应用词表）都产 `constexpr MessageId`
/// 常量并共用本类型，因此跨词表的词条可进入同一 `MessageCatalog` 按 Id 直排查表；
/// 裸整数不发生隐式转换，词表外的 Id 无法混入。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct MessageId {
    std::uint32_t value = AURORA_INVALID_MESSAGE_ID_VALUE;  ///< 词表内连续编号（直排下标）

    /// @brief 相等性比较：按原始值逐位比较（默认实现，形参无名）。
    /// @return 两 Id 原始值相等时为 true。
    friend constexpr auto operator==(MessageId, MessageId) -> bool = default;
};

/// @brief 无效词条 Id：未携带词条的 `LocalizedString` 持有它，resolve 走字面回退。
inline constexpr MessageId AURORA_INVALID_MESSAGE_ID{AURORA_INVALID_MESSAGE_ID_VALUE};

}  // namespace aurora
