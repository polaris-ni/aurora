#pragma once

#include <string>

#include "aurora/state/state.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief 只读权限包装（ARCHITECTURE.md §4.1 状态作用域可追踪）。
///
/// 包装一个 `State<T>&`，仅暴露 const 读访问；写入路径被删除，从而在**类型层面**强制
/// 「某作用域只读取该状态」。可附带一个 scope 标签，汇入 `StateGraph` 用以表达读作用域。
///
/// @tparam T 被包装 State 的值类型。
template <typename T>
class Immutable {
  public:
    /// @brief 绑定源 State 并可选标注读作用域。
    /// @param src 被包装的可写状态源（非拥有引用，生命周期须长于本包装）。
    /// @param scope 作用域标签（空串表示未标注；仅用于 StateGraph 调试展示）。
    explicit Immutable(State<T> &src, std::string scope = {}) : src_(&src), scope_(std::move(scope)) {}

    /// @brief 读取当前值（透传上游 `State::get()`，在 Effect 作用域内读取会自动登记依赖）。
    /// @return 上游 State 当前值的只读引用（指向状态内部存储，随 State 生命周期有效）。
    [[nodiscard]] auto get() const -> const T & { return src_->get(); }

    /// @brief 作用域标签（用于 StateGraph 标注读来源）。
    /// @return 构造时传入的标签串引用；空串表示未标注作用域。
    [[nodiscard]] auto scope() const -> const std::string & { return scope_; }

  private:
    State<T> *src_;  ///< 上游可写状态（非拥有）
    std::string scope_;  ///< 作用域标签（空串 = 未标注）
};

/// @brief 读写权限包装（ARCHITECTURE.md §4.1）。
///
/// 包装 `State<T>&`，暴露读与写；相比裸 `State<T>` 多一个显式 scope 标签，
/// 便于把「谁在读 / 谁在写」这个状态作用域显式化并汇入 `StateGraph`。
///
/// @tparam T 被包装 State 的值类型。
template <typename T>
class Mutable {
  public:
    /// @brief 绑定源 State 并可选标注写作用域。
    /// @param src 被包装的可写状态源（非拥有引用，生命周期须长于本包装）。
    /// @param scope 作用域标签（空串表示未标注；仅用于 StateGraph 调试展示）。
    explicit Mutable(State<T> &src, std::string scope = {}) : src_(&src), scope_(std::move(scope)) {}

    /// @brief 读取当前值（透传上游 `State::get()`）。
    /// @return 上游 State 当前值的只读引用（指向状态内部存储，随 State 生命周期有效）。
    [[nodiscard]] auto get() const -> const T & { return src_->get(); }
    /// @brief 写入新值并通知上游依赖（等价 `State::set`，触发定点刷新）。
    /// @param v 待写入的新值。
    auto set(T v) -> void { src_->set(std::move(v)); }

    /// @brief 作用域标签（用于 StateGraph 标注写来源）。
    /// @return 构造时传入的标签串引用；空串表示未标注作用域。
    [[nodiscard]] auto scope() const -> const std::string & { return scope_; }

  private:
    State<T> *src_;  ///< 上游可写状态（非拥有）
    std::string scope_;  ///< 作用域标签（空串 = 未标注）
};

}  // namespace aurora
