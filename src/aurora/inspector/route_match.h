#pragma once

/// @file route_match.h
/// @brief Inspector HTTP 路由表的匹配器：精确项整体优先、前缀项按登记次序兜后。
///
/// 这张表原先是一条贯穿 `route_request` 全文件的 if 链，精确比较与前缀比较交错出现；每加一枚端点
/// 都要在同一枚千行函数里找落点。匹配逻辑抽到这里后，生产表与单测共用同一份次序判定：合成表里
/// 放一条与前缀项重叠的精确项，即可证明「精确优先」不是靠登记顺序碰巧成立。
///
/// 条目只要求暴露两个字段：`pattern`（`std::string_view`）与 `kind`（`RouteKind`）。handler、方法位
/// 掩码与 label 留在 Inspector 自己的表里——匹配器只回答「哪一条」，不回答「能不能调用」。
///
/// @note Thread: 纯函数，无共享状态。
/// @note Side-effects: none。

#include <cstdint>
#include <span>
#include <string_view>

namespace aurora::inspector::detail {

/// @brief 路由条目的匹配形态。
enum class RouteKind : std::uint8_t {
    Exact,  ///< 整条路径相等
    Prefix,  ///< 以 pattern 为前缀（pattern 自带尾斜杠）
};

/// @brief 在条目表中查找命中的条目。
///
/// 两趟查找：先遍历全部精确项，再遍历全部前缀项。故「精确项优先」与登记次序无关；前缀项之间
/// 保留登记次序（先登记先命中），这正是「更具体的前缀族可以排在更宽的前缀族之前」的表达。
///
/// @tparam Entry 条目类型；须有 `pattern`（可比较 `std::string_view`）与 `kind` 两个成员。
/// @param entries 条目表（生产表或测试合成表）。
/// @param route 已剥离查询串的路径。
/// @return 命中条目的地址；无命中为 `nullptr`。返回指针指向 `entries` 内部，生命周期随表。
template <typename Entry>
[[nodiscard]] auto match_route(std::span<const Entry> entries, std::string_view route) -> const Entry * {
    for (const Entry &entry : entries) {
        if (entry.kind == RouteKind::Exact && entry.pattern == route) {
            return &entry;
        }
    }
    for (const Entry &entry : entries) {
        if (entry.kind == RouteKind::Prefix && route.starts_with(entry.pattern)) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace aurora::inspector::detail
