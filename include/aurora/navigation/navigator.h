#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "aurora/navigation/route.h"

namespace aurora {

/**
 * @brief 导航器：持有 `Route` 栈，管理 push/pop/replace（specification/05-event-navigation.md §7.2，
 * 参考 UIKit UINavigationController / Android NavController / Flutter Navigator）。
 *
 * MVP 支持完整栈（push/pop/replace/popToRoot）；转场动画经 `Route::transition`
 * 配置，由上层在切换当前页后请求重绘/转场（specification/05-event-navigation.md §7.1）。栈变化触发
 * `onRouteChanged` 回调，供帧循环请求下一帧（VSync 合并，ARCHITECTURE.md §5.2），避免中间重绘。
 */
/// @brief 导航栈默认最大深度（specification/05-event-navigation.md §7.2 栈深上限守卫）。超过此深度的 push/restore
/// 经 `Diagnostics` 降级拒绝，避免无限深栈导致的栈溢出 / 渲染雪崩。
inline constexpr std::size_t AURORA_DEFAULT_MAX_NAV_DEPTH = 32;

/// @brief 轻量路由表（deep linking 辅助）：名称 → 路由构造器。
using RouteRegistry = std::map<std::string, std::function<Route(const std::string &)>>;

class Navigator {
  public:
    Navigator() = default;
    explicit Navigator(Route initial);

    /// @brief 压入新页面（成为当前页）。
    auto push(Route route) -> void;

    /// @brief 替换栈顶（原地换页，深度不变）。
    auto push_replacement(Route route) -> void;

    /// @brief 弹栈；仅剩根路由时拒绝（返回 false）。
    [[nodiscard]] auto pop() -> bool;

    /// @brief 回到根路由（清空到仅剩首个）。
    auto pop_to_root() -> void;

    [[nodiscard]] auto current() -> Route &;
    [[nodiscard]] auto current() const -> const Route &;
    /// @brief 当前路由的根 widget（供渲染；无路由返回空 Node）。
    [[nodiscard]] auto current_root() -> Node;
    [[nodiscard]] auto depth() const -> std::size_t;
    [[nodiscard]] auto can_pop() const -> bool;
    [[nodiscard]] auto stack() const -> const std::vector<Route> &;

    /// @brief 导出当前路由栈名序列（deep linking）。
    [[nodiscard]] auto path() const -> std::vector<std::string>;

    /// @brief 按名称序列重建路由栈（deep linking）；build 把名称映射回 Route。
    auto restore(const std::vector<std::string> &names, const std::function<Route(std::string)> &build) -> void;

    /// @brief 按 URI 字符串重建路由栈（deep linking）：以 '/' 切分名称序列后委托 restore。
    auto open_uri(const std::string &uri, const std::function<Route(const std::string &)> &build) -> void;

    /// @brief 按 URI 字符串 + 路由表重建路由栈；表中缺失的名称段被跳过。
    auto open_uri(const std::string &uri, const RouteRegistry &registry) -> void;

    /// @brief 栈变化回调（请求下一帧重绘，ARCHITECTURE.md §5.2）。
    auto set_on_route_changed(std::function<void()> cb) -> void;

    /// @brief 当前允许的最大路由栈深度（默认 `AURORA_DEFAULT_MAX_NAV_DEPTH`）。
    [[nodiscard]] auto max_depth() const -> std::size_t;
    /// @brief 设置最大路由栈深度（push/restore 超限将被 `Diagnostics` 拒绝）。
    auto set_max_depth(std::size_t d) -> void;

  private:
    auto notify() const -> void;

    /// @brief 将 URI 按 '/' 切分为名称序列，丢弃空段（如 "home//detail/" -> {"home","detail"}）。
    [[nodiscard]] static auto split_uri(const std::string &uri) -> std::vector<std::string>;

    std::vector<Route> m_stack;
    std::function<void()> m_on_changed;
    std::size_t m_max_depth = AURORA_DEFAULT_MAX_NAV_DEPTH;
};

} // namespace aurora
