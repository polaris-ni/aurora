#pragma once

#include <any>
#include <typeindex>
#include <unordered_map>

namespace aurora {

/**
 * @brief 环境：沿树向下传播的类型化键值表（参考 Flutter InheritedWidget /
 * SwiftUI Environment / Compose CompositionLocal）。
 *
 * 采用「父指针 + 覆盖映射」的不可变链式结构：`with<T>()` 生成一个子环境，
 * 只覆盖键 T，读取时沿父链向上查找最近的定义。从而 Provider 注入的值对
 * 其子树可见，且天然实现「最近祖先优先」。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class Environment {
  public:
    Environment() = default;

    /// @brief 生成子环境，覆盖类型 T 的值为 `value`。原环境不被修改。
    template <typename T>
    [[nodiscard]] auto with(T value) const -> Environment {
        Environment child;
        child.parent_ = this;
        child.map_.emplace(typeid(T), std::any(std::move(value)));
        return child;
    }

    /// @brief 读取类型 T 的环境值；不存在则返回 nullptr。
    template <typename T>
    [[nodiscard]] auto get() const -> const T * {
        const auto it = map_.find(typeid(T));
        if (it != map_.end()) {
            return std::any_cast<T>(&it->second);
        }
        if (parent_ != nullptr) {
            return parent_->get<T>();
        }
        return nullptr;
    }

    /// @brief 在当前环境本地设置键 T（无父指针，供根 Provider 使用，避免悬空父）。
    template <typename T>
    auto set_local(T value) -> void {
        map_.emplace(typeid(T), std::any(std::move(value)));
    }

    /// @brief 在当前环境本地设置/覆盖键 T（覆盖既有值，供根级每帧更新复用）。
    /// 仅改写 `map_`，保留 `parent_` 指针，地址恒定，子树持有的父指针不失效。
    template <typename T>
    auto set(T value) -> void {
        map_[typeid(T)] = std::any(std::move(value));
    }

  private:
    std::unordered_map<std::type_index, std::any> map_;
    const Environment *parent_ = nullptr;
};

// detail_env_get 的定义：需 Environment 完整类型（见上方 class Environment），故置于命名空间内。
// BuildContext::environment() 在 build_context.h 中内联、转发至此函数，从而 build_context.h
// 无需 Environment 完整即可解析（规避「在 environment.h 之前包含本头」的 incomplete-type 问题）。
template <typename T>
[[nodiscard]] auto detail_env_get(const Environment *env) -> const T * {
    return env != nullptr ? env->get<T>() : nullptr;
}

}  // namespace aurora

// BuildContext 使用 Environment 指针；build_context.h 仅前向声明 Environment 并通过 detail_env_get
// 转发，故此处包含不构成循环依赖。
#include "aurora/environment/build_context.h"
