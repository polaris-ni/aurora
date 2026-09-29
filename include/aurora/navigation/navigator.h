#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "aurora/navigation/route.h"

namespace aurora {

/// @brief 导航栈默认最大深度（specification/05-event-navigation.md §7.2 栈深上限守卫）。超过此深度的 push/restore
/// 经 `Diagnostics` 降级拒绝，避免无限深栈导致的栈溢出 / 渲染雪崩。
inline constexpr std::size_t AURORA_DEFAULT_MAX_NAV_DEPTH = 32;

/// @brief 轻量路由表（deep linking 辅助）：名称 → 路由构造器。
using RouteRegistry = std::map<std::string, std::function<Route(const std::string &)>>;

/// @brief 导航器：持有 `Route` 栈，管理 push/pop/replace（specification/05-event-navigation.md §7.2，
/// 参考 UIKit UINavigationController / Android NavController / Flutter Navigator）。
///
/// MVP 支持完整栈（push/pop/replace/popToRoot）；转场动画经 `Route::transition`
/// 配置，由上层在切换当前页后请求重绘/转场（specification/05-event-navigation.md §7.1）。栈变化触发
/// `onRouteChanged` 回调，供帧循环请求下一帧（VSync 合并，ARCHITECTURE.md §5.2），避免中间重绘。
class Navigator {
  public:
    /// @brief 空栈构造：无任何路由，current_root() 返回空 Node，首个路由须经 push/push_replacement 进入。
    Navigator() = default;
    /// @brief 以初始路由构造：入栈该路由作为根页面。
    /// @param initial 初始（根）路由。
    explicit Navigator(Route initial);

    /// @brief 压入新页面（成为当前页）。
    /// @param route 待压入的路由；栈深超过 max_depth() 时经 `Diagnostics` 降级拒绝，不入栈。
    auto push(Route route) -> void;

    /// @brief 替换栈顶（原地换页，深度不变）。
    /// @param route 替换后的路由；栈为空时等价于 push（同样受栈深守卫）。
    auto push_replacement(Route route) -> void;

    /// @brief 弹栈；仅剩根路由时拒绝（返回 false）。
    /// @return 成功弹出返回 true；栈深 ≤1（仅根路由）返回 false。
    [[nodiscard]] auto pop() -> bool;

    /// @brief 回到根路由（清空到仅剩首个）。
    auto pop_to_root() -> void;

    /// @brief 栈顶路由的可写引用（要求栈非空）。
    /// @return 当前（栈顶）Route 的引用。
    [[nodiscard]] auto current() -> Route &;
    /// @brief 栈顶路由的只读引用（要求栈非空）。
    /// @return 当前（栈顶）Route 的常量引用。
    [[nodiscard]] auto current() const -> const Route &;
    /// @brief 当前路由的根 widget（供渲染；无路由返回空 Node）。
    /// @return 栈顶路由的 root()；栈为空时返回默认构造的 Node。
    [[nodiscard]] auto current_root() -> Node;
    /// @brief 当前路由栈深度（含根路由）。
    /// @return 栈内路由数量。
    [[nodiscard]] auto depth() const -> std::size_t;
    /// @brief 是否还能弹栈（栈深大于 1）。
    /// @return 可弹返回 true；仅根路由或空栈返回 false。
    [[nodiscard]] auto can_pop() const -> bool;
    /// @brief 只读路由栈快照（自底向上）。
    /// @return 内部栈的常量引用。
    [[nodiscard]] auto stack() const -> const std::vector<Route> &;

    /// @brief 导出当前路由栈名序列（deep linking）。
    /// @return 自底向上各路由 name() 组成的序列。
    [[nodiscard]] auto path() const -> std::vector<std::string>;

    /// @brief 按名称序列重建路由栈（deep linking）；build 把名称映射回 Route。
    /// @param names 自底向上的路由名序列；空序列直接返回不改栈。
    /// @param build 名称到 Route 的构造回调；序列长度超过 max_depth() 时经 `Diagnostics` 拒绝。
    auto restore(const std::vector<std::string> &names, const std::function<Route(std::string)> &build) -> void;

    /// @brief 按 URI 字符串重建路由栈（deep linking）：以 '/' 切分名称序列后委托 restore。
    /// @param uri 以 '/' 分隔的路由名序列（空段丢弃）。
    /// @param build 名称到 Route 的构造回调。
    auto open_uri(const std::string &uri, const std::function<Route(const std::string &)> &build) -> void;

    /// @brief 按 URI 字符串 + 路由表重建路由栈；表中缺失的名称段被跳过。
    /// @param uri 以 '/' 分隔的路由名序列（空段丢弃）。
    /// @param registry 名称 → 路由构造器的查表（RouteRegistry）。
    auto open_uri(const std::string &uri, const RouteRegistry &registry) -> void;

    /// @brief 栈变化回调（请求下一帧重绘，ARCHITECTURE.md §5.2）。
    /// @param cb 回调闭包；每次栈内容变化后同步调用，传空即解除挂接。
    auto set_on_route_changed(std::function<void()> cb) -> void;

    /// @brief 当前允许的最大路由栈深度（默认 `AURORA_DEFAULT_MAX_NAV_DEPTH`）。
    /// @return 现行的栈深上限。
    [[nodiscard]] auto max_depth() const -> std::size_t;
    /// @brief 设置最大路由栈深度（push/restore 超限将被 `Diagnostics` 拒绝）。
    /// @param d 新的栈深上限。
    auto set_max_depth(std::size_t d) -> void;

  private:
    auto notify() const -> void;

    /// @brief 将 URI 按 '/' 切分为名称序列，丢弃空段（如 "home//detail/" -> {"home","detail"}）。
    [[nodiscard]] static auto split_uri(const std::string &uri) -> std::vector<std::string>;

    std::vector<Route> stack_;
    std::function<void()> on_changed_;
    std::size_t max_depth_ = AURORA_DEFAULT_MAX_NAV_DEPTH;
};

}  // namespace aurora
