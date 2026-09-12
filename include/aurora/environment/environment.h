#pragma once

#include <any>
#include <memory>
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
        // 持有 `*this` 的堆拷贝，避免形如 `Environment{}.with<T>(v)` 的临时父环境
        // 析构后留下悬垂父指针；下游经环境链回查其它类型（如 Text::resolved_text
        // 读取 Locale）时解引用已释放的 unordered_map，在 MSVC 上表现为 SEGFAULT
        // （libstdc++ 通常保留已释放内存可读而侥幸通过）。链语义不变：子环境仅含自身
        // 覆盖项，其余沿 parent_ 向上查找最近祖先（堆拷贝保留了其祖先链）。
        child.parent_ = std::make_shared<const Environment>(*this);
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
    std::shared_ptr<const Environment> parent_ = nullptr;
};

}  // namespace aurora
