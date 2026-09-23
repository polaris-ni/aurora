#pragma once

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "aurora/navigation/route.h"

namespace aurora {

/**
 * @brief 路由表（specification/05-event-navigation.md §7.3）：按名称登记路由工厂，按需构建 `Route`。
 *
 * 工厂模式使每次导航都获得一棵全新 widget 树（参考 Flutter 命名路由），
 * 避免路由在栈间共享同一棵可变树。
 *
 * 两种建表写法等价，取顺手者：
 * ```cpp
 * // ① 链式（`with` 返回新表，可挂成 const 表一次建好）
 * const au::Router router = au::Router{}.with("home", home_route).with("detail", detail_route);
 * // ② 表形态（一次并入多条）
 * const au::Router router = au::Router{}.with({{"home", home_route}, {"detail", detail_route}});
 * // ③ 命令式（既有写法）
 * au::Router router; router.register_route("home", home_route);
 * ```
 */
// 豁免 bugprone-exception-escape：本表值即 std::function<RouteBuilder>，拷贝 routes_ 触发 .clang-tidy
// 已记录的系统性假告警面——「任何转入 std::function 的可调用对象一律判『不应抛出』」（std::function::
// operator() 无 noexcept 规格，分析器无法证明其不抛），据此误判隐式特殊成员。抛出仅可能为 bad_alloc，
// 由顶层统一兜底，非本类需就地吞掉的抛出面。
// NOLINTNEXTLINE(bugprone-exception-escape)
class Router {
  public:
    /// @brief 按名称构建 `Route` 的工厂。
    using RouteBuilder = std::function<Route()>;

    /// @brief 路由表项（`with` 的 initializer-list 形态用）。
    struct Entry {
        std::string name;  ///< 路由名称
        RouteBuilder builder;  ///< 构建该路由的工厂
    };

    /// @brief 登记命名路由（name → 构建该 Route 的工厂）。同名覆盖。
    auto register_route(std::string name, RouteBuilder builder) -> void;

    /// @brief 便捷工厂：返回**新表** = 本表 + 这一条命名路由（本表不变，对标 `Environment::with`）。
    ///
    /// 链式建表即由此而来。每次调用复制整张表（`unordered_map` 浅拷贝，成本与路由条数成正比），
    /// 故适合「建一次、到处读」的常量路由表；高频增量登记请直接用 `register_route`。
    [[nodiscard]] auto with(std::string name, RouteBuilder builder) const -> Router;

    /// @brief 便捷工厂：返回**新表** = 本表 + 这批命名路由（同名按给定顺序后者覆盖）。
    [[nodiscard]] auto with(std::initializer_list<Entry> entries) const -> Router;

    /// @brief 是否已登记该名称。
    [[nodiscard]] auto has(const std::string &name) const -> bool;

    /// @brief 按名称构建路由；未登记返回 nullopt。
    [[nodiscard]] auto build(const std::string &name) const -> std::optional<Route>;

    /// @brief 便捷：构建并取根节点；未登记返回空 `Node`。
    [[nodiscard]] auto build_root(const std::string &name) const -> Node;

  private:
    std::unordered_map<std::string, RouteBuilder> routes_;
};

}  // namespace aurora
