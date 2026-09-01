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
    template<typename T> [[nodiscard]] auto with(T value) const -> Environment {
        Environment child;
        child.m_parent = this;
        child.m_map.emplace(typeid(T), std::any(std::move(value)));
        return child;
    }

    /// @brief 读取类型 T 的环境值；不存在则返回 nullptr。
    template<typename T> [[nodiscard]] auto get() const -> const T * {
        const auto it = m_map.find(typeid(T));
        if (it != m_map.end()) {
            return std::any_cast<T>(&it->second);
        }
        if (m_parent != nullptr) {
            return m_parent->get<T>();
        }
        return nullptr;
    }

    /// @brief 在当前环境本地设置键 T（无父指针，供根 Provider 使用，避免悬空父）。
    template<typename T> auto set_local(T value) -> void { m_map.emplace(typeid(T), std::any(std::move(value))); }

    /// @brief 在当前环境本地设置/覆盖键 T（覆盖既有值，供根级每帧更新复用）。
    /// 仅改写 `m_map`，保留 `m_parent` 指针，地址恒定，子树持有的父指针不失效。
    template<typename T> auto set(T value) -> void { m_map[typeid(T)] = std::any(std::move(value)); }

  private:
    std::unordered_map<std::type_index, std::any> m_map;
    const Environment *m_parent = nullptr;
};

} // namespace aurora

// BuildContext 使用 Environment 指针，需在 Environment 完整定义后再包含，
// 避免 environment.h ↔ build_context.h 的循环包含。
#include "aurora/environment/build_context.h"
