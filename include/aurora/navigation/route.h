#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "aurora/animation/easing.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 转场类型（specification/05-event-navigation.md §7.1）：决定新旧页如何合成。
enum class TransitionKind : std::uint8_t {
    Fade,  ///< 淡入淡出：旧页淡出 + 新页淡入
    Slide,  ///< 水平滑动：旧页左出 + 新页右入
};

/// @brief 路由转场配置（specification/05-event-navigation.md §7.1）：驱动 NavigatorHost 的视觉合成。
struct RouteTransition {
    bool animated = false;  ///< 是否使用转场动画
    TransitionKind kind = TransitionKind::Fade;  ///< 转场类型
    Curve curve = Curves::linear();  ///< 转场缓动（动画框架，specification/05-event-navigation.md §6.1）
    double duration_seconds = 0.3;  ///< 转场时长（秒）
};

/// @brief 路由：一个页面（子树根）。
///
/// 对应 specification/05-event-navigation.md §7.1 的 `Route`（即一个 Scene 或子树根）。
/// 持有 widget 树根节点、可选名称与转场配置。`Node` 内部为 `shared_ptr`，故 `Route`
/// 可安全拷贝/移动。
class Route {
  public:
    Route() = default;

    /// @brief 构造路由：装载页面子树根与可选名称/转场配置。
    /// @param root 页面子树根节点。
    /// @param name 路由名称（默认空串，仅用于标识/调试）。
    /// @param transition 转场配置（默认构造：不动画、Fade）。
    explicit Route(Node root, std::string name = "", RouteTransition transition = {})
        : root_(std::move(root)), name_(std::move(name)), transition_(std::move(transition)) {}

    /// @brief 页面子树根（可变访问）。
    /// @return root_ 的原样引用（未装载时为空 `Node`）。
    [[nodiscard]] auto root() -> Node & { return root_; }
    /// @brief 页面子树根（只读访问）。
    /// @return root_ 的 const 引用。
    [[nodiscard]] auto root() const -> const Node & { return root_; }
    /// @brief 读取路由名称。
    /// @return 名称引用（未提供时为空串）。
    [[nodiscard]] auto name() const -> const std::string & { return name_; }
    /// @brief 读取转场配置。
    /// @return 转场配置引用。
    [[nodiscard]] auto transition() const -> const RouteTransition & { return transition_; }
    /// @brief 是否空路由（无根节点）。
    /// @return root_ 为空时 true（默认构造即空路由）。
    [[nodiscard]] auto empty() const -> bool { return !static_cast<bool>(root_); }

  private:
    Node root_;
    std::string name_;
    RouteTransition transition_;
};

}  // namespace aurora
