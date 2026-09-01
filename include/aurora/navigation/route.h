#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "aurora/animation/easing.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 转场类型（specification/05-event-navigation.md §7.1）：决定新旧页如何合成。
 */
enum class TransitionKind : std::uint8_t {
    Fade,  ///< 淡入淡出：旧页淡出 + 新页淡入
    Slide, ///< 水平滑动：旧页左出 + 新页右入
};

/**
 * @brief 路由转场配置（specification/05-event-navigation.md §7.1）：驱动 NavigatorHost 的视觉合成。
 */
struct RouteTransition {
    bool animated = false;                      ///< 是否使用转场动画
    TransitionKind kind = TransitionKind::Fade; ///< 转场类型
    Curve curve = Curves::linear();             ///< 转场缓动（动画框架，specification/05-event-navigation.md §6.1）
    double duration_seconds = 0.3;              ///< 转场时长（秒）
};

/**
 * @brief 路由：一个页面（子树根）。对应 specification/05-event-navigation.md §7.1 的 `Route`（"即一个 Scene 或子树根"）。
 *
 * 持有 widget 树根节点、可选名称与转场配置。`Node` 内部为 `shared_ptr`，故 `Route`
 * 可安全拷贝/移动。
 */
class Route {
  public:
    Route() = default;

    explicit Route(Node root, std::string name = "", RouteTransition transition = {})
        : m_root(std::move(root)), m_name(std::move(name)), m_transition(std::move(transition)) {}

    [[nodiscard]] auto root() -> Node & { return m_root; }
    [[nodiscard]] auto root() const -> const Node & { return m_root; }
    [[nodiscard]] auto name() const -> const std::string & { return m_name; }
    [[nodiscard]] auto transition() const -> const RouteTransition & { return m_transition; }
    /// @brief 是否空路由（无根节点）。
    [[nodiscard]] auto empty() const -> bool { return !static_cast<bool>(m_root); }

  private:
    Node m_root;
    std::string m_name;
    RouteTransition m_transition;
};

} // namespace aurora
