#pragma once

#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "aurora/core/types.h"

namespace aurora {

class Widget;  // 前向声明：Node 以 shared_ptr<Widget> 持有；析构在 widget.cpp 中定义（需完整 Widget）

/// @brief 节点包装：接受任意 `Widget` 派生，以 `shared_ptr<Widget>` 共享所有权持有。
///
/// 用户写 `Column{ .children = { Text{}, Button{} } }` 可直接编译，
/// 派生临时对象被包进新的 `shared_ptr<Widget>`（用户不写 new/make_unique）；
/// `Node` 的拷贝/移动即 `shared_ptr` 的拷贝/移动，同一 widget 实例可被多处 `Node` 共享。
///
class Node {
  public:
    /// @brief 默认构造为空节点（widget_ == nullptr）。
    Node() = default;

    /// @brief 从任意 Widget 派生构造，接管所有权（拷贝即共享，整棵树可被复制/移动）。
    /// 用户无需写 new/make_unique；rvalue 经移动接管，lvalue 经拷贝接管（转移 shared_ptr 所有权）。
    /// @param w 待接管的 Widget 派生对象（转发引用）。
    /// @tparam W 派生自 Widget 的具体类型，约束由 `std::derived_from` 把关。
    template <typename W>
        requires std::derived_from<std::remove_cvref_t<W>, Widget>
    Node(W &&w) : widget_(std::make_shared<std::remove_cvref_t<W>>(std::forward<W>(w))) {}

    /// @brief 从已有 shared_ptr 构造（供 Padding 等包装器转移所有权）。
    /// @param w 已有的 Widget 共享指针，移动接管。
    Node(std::shared_ptr<Widget> w) : widget_(std::move(w)) {}

    /// @brief 取出持有的控件引用（非 const 重载）。
    /// @return 所指 Widget 的非 const 引用。
    [[nodiscard]] auto widget() -> Widget & { return *widget_; }
    /// @brief 取出持有的控件引用（const 重载）。
    /// @return 所指 Widget 的 const 引用。
    [[nodiscard]] auto widget() const -> const Widget & { return *widget_; }
    /// @brief 箭头访问持有的控件。
    /// @return 所指 Widget 的裸指针。
    [[nodiscard]] auto operator->() -> Widget * { return widget_.get(); }
    /// @brief 箭头访问持有的控件（const 重载）。
    /// @return 所指 Widget 的 const 裸指针。
    [[nodiscard]] auto operator->() const -> const Widget * { return widget_.get(); }
    /// @brief 节点非空性判定。
    /// @return 持有 widget 时为 true。
    [[nodiscard]] explicit operator bool() const noexcept { return widget_ != nullptr; }

    /// @brief 共享所有权引用计数（诊断用）：本节点之外是否还有别处持有同一控件。
    ///
    /// 与 `widget.cpp` 中 a11y 结构事件的 `use_count() == 1` 判唯一所有权同口径。典型用途：
    /// `Widget::detach_all_children_layout_parent` 借此区分「容器持最后一份 ⇒ 子节点随本容器
    /// 即刻销毁（正常，不告警）」与「子节点活在容器之外（异常，告警）」。
    /// @return 共享该控件的 `shared_ptr` 引用数；空节点为 0。
    [[nodiscard]] auto use_count() const noexcept -> long { return widget_.use_count(); }

    /// @brief 设置节点布局盒（布局阶段由父容器写入）。
    /// @param r 新的布局矩形。
    auto set_bounds(Rect r) -> void { bounds_ = r; }
    /// @brief 读取节点布局盒。
    /// @return 最近一次 set_bounds 的矩形。
    [[nodiscard]] auto bounds() const -> Rect { return bounds_; }

    /// @brief 设置节点标识（dump_tree_rich 渲染的 `#id` 来源；空表示未命名）。
    /// @param id 节点标识字符串视图，拷入 id_。
    auto set_id(std::string_view id) -> void { id_ = std::string(id); }
    /// @brief 读取节点标识（未设置时为空 `std::string_view`）。
    /// @return 当前标识；未设置为空视图。
    [[nodiscard]] auto id() const -> std::string_view { return id_; }

    Node(const Node &) = default;
    Node(Node &&) = default;
    auto operator=(const Node &) -> Node & = default;
    auto operator=(Node &&) -> Node & = default;

    /// @brief 析构定义于 widget.cpp：需完整 Widget 才能调用 set_layout_parent(nullptr)，
    ///        同时避免头文件中以不完整 Node 实例化 std::vector<Node> 析构（clang 严格、gcc 容忍）。
    ~Node();

  private:
    std::shared_ptr<Widget> widget_;
    Rect bounds_;
    std::string id_;  ///< 节点标识（可选；dump_tree_rich 的 `#id`）
};

}  // namespace aurora
