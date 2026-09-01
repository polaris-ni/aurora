#pragma once

#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "aurora/core/types.h" // Rect

namespace aurora {

class Widget; // 前向声明：Node 以 shared_ptr<Widget> 持有；析构在 widget.cpp 中定义（需完整 Widget）

/**
 * @brief 节点包装：接受任意 `Widget` 派生，以 `shared_ptr<Widget>` 共享所有权持有。
 *
 * 用户写 `Column{ .children = { Text{}, Button{} } }` 可直接编译，
 * 派生临时对象被包进新的 `shared_ptr<Widget>`（用户不写 new/make_unique）；
 * `Node` 的拷贝/移动即 `shared_ptr` 的拷贝/移动，同一 widget 实例可被多处 `Node` 共享。
 */
class Node {
  public:
    Node() = default; ///< 默认构造为空节点（m_widget == nullptr）

    /// @brief 从任意 Widget 派生构造，接管所有权（拷贝即共享，整棵树可被复制/移动）。
    /// 用户无需写 new/make_unique；rvalue 经移动接管，lvalue 经拷贝接管（转移 shared_ptr 所有权）。
    template<typename W>
        requires std::derived_from<std::remove_cvref_t<W>, Widget>
    Node(W &&w) : m_widget(std::make_shared<std::remove_cvref_t<W>>(std::forward<W>(w))) {}

    /// @brief 从已有 shared_ptr 构造（供 Padding 等包装器转移所有权）。
    Node(std::shared_ptr<Widget> w) : m_widget(std::move(w)) {}

    [[nodiscard]] auto widget() -> Widget & { return *m_widget; }
    [[nodiscard]] auto widget() const -> const Widget & { return *m_widget; }
    [[nodiscard]] auto operator->() -> Widget * { return m_widget.get(); }
    [[nodiscard]] auto operator->() const -> const Widget * { return m_widget.get(); }
    [[nodiscard]] explicit operator bool() const noexcept { return m_widget != nullptr; }

    auto set_bounds(Rect r) -> void { m_bounds = r; }
    [[nodiscard]] auto bounds() const -> Rect { return m_bounds; }

    /// @brief 设置节点标识（dump_tree_rich 渲染的 `#id` 来源；空表示未命名）。
    auto set_id(std::string_view id) -> void { m_id = std::string(id); }
    /// @brief 读取节点标识（未设置时为空 `std::string_view`）。
    [[nodiscard]] auto id() const -> std::string_view { return m_id; }

    Node(const Node &) = default;
    Node(Node &&) = default;
    auto operator=(const Node &) -> Node & = default;
    auto operator=(Node &&) -> Node & = default;

    // 析构放 widget.cpp：需完整 Widget 才能调用 set_layout_parent(nullptr)；
    // 同时避免头文件中以不完整 Node 实例化 std::vector<Node> 析构（clang 严格、gcc 容忍）。
    ~Node();

  private:
    std::shared_ptr<Widget> m_widget;
    Rect m_bounds;
    std::string m_id; ///< 节点标识（可选；dump_tree_rich 的 `#id`）
};

} // namespace aurora
