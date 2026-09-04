#pragma once

#include <string>

#include "aurora/state/state.h"

namespace aurora {

/**
 * @brief 只读权限包装（ARCHITECTURE.md §4.1 状态作用域可追踪）。
 *
 * 包装一个 `State<T>&`，仅暴露 const 读访问；写入路径被删除，从而在**类型层面**强制
 * 「某作用域只读取该状态」。可附带一个 scope 标签，汇入 `StateGraph` 用以表达读作用域。
 */
template <typename T>
class Immutable {
  public:
    explicit Immutable(State<T> &src, std::string scope = {}) : src_(&src), scope_(std::move(scope)) {}

    [[nodiscard]] auto get() const -> const T & { return src_->get(); }

    /// @brief 作用域标签（用于 StateGraph 标注读来源）。
    [[nodiscard]] auto scope() const -> const std::string & { return scope_; }

  private:
    State<T> *src_;
    std::string scope_;
};

/**
 * @brief 读写权限包装（ARCHITECTURE.md §4.1）。
 *
 * 包装 `State<T>&`，暴露读与写；相比裸 `State<T>` 多一个显式 scope 标签，
 * 便于把「谁在读 / 谁在写」这个状态作用域显式化并汇入 `StateGraph`。
 */
template <typename T>
class Mutable {
  public:
    explicit Mutable(State<T> &src, std::string scope = {}) : src_(&src), scope_(std::move(scope)) {}

    [[nodiscard]] auto get() const -> const T & { return src_->get(); }
    auto set(T v) -> void { src_->set(std::move(v)); }

    [[nodiscard]] auto scope() const -> const std::string & { return scope_; }

  private:
    State<T> *src_;
    std::string scope_;
};

}  // namespace aurora
