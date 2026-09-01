#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "aurora/navigation/route.h"

namespace aurora {

/**
 * @brief 路由表（§5.11）：按名称登记路由工厂，按需构建 `Route`。
 *
 * 工厂模式使每次导航都获得一棵全新 widget 树（参考 Flutter 命名路由），
 * 避免路由在栈间共享同一棵可变树。
 */
class Router {
  public:
    /// @brief 按名称构建 `Route` 的工厂。
    using RouteBuilder = std::function<Route()>;

    /// @brief 登记命名路由（name → 构建该 Route 的工厂）。
    auto register_route(std::string name, RouteBuilder builder) -> void;

    /// @brief 是否已登记该名称。
    [[nodiscard]] auto has(const std::string &name) const -> bool;

    /// @brief 按名称构建路由；未登记返回 nullopt。
    [[nodiscard]] auto build(const std::string &name) const -> std::optional<Route>;

    /// @brief 便捷：构建并取根节点；未登记返回空 `Node`。
    [[nodiscard]] auto build_root(const std::string &name) const -> Node;

  private:
    std::unordered_map<std::string, RouteBuilder> m_routes;
};

} // namespace aurora
