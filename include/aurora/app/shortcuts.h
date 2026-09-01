#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/event/keycode.h"

namespace aurora {

/**
 * @brief 键组合（快捷键描述）：修饰键位掩码 + 主键。
 *
 * 对标 Qt `QKeySequence`、WPF `KeyGesture`、Flutter `SingleActivator`。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
struct KeyCombo {
    ModifierKey modifiers = ModifierKey::None; ///< 修饰键组合（Ctrl/Shift/Alt/Meta 位掩码）
    KeyCode key = KeyCode::Unknown;            ///< 主键

    KeyCombo() = default;
    KeyCombo(ModifierKey mods, KeyCode k) : modifiers(mods), key(k) {}
    explicit KeyCombo(KeyCode k) : key(k) {}

    /// @brief 检查键盘事件是否匹配本组合（按下事件 + 键码 + 修饰键完全一致）。
    [[nodiscard]] auto matches(const KeyEvent &e) const -> bool {
        if (e.action != KeyAction::Down) {
            return false;
        }
        if (static_cast<KeyCode>(e.key) != key) {
            return false;
        }
        return static_cast<std::uint8_t>(e.modifiers) == static_cast<std::uint8_t>(modifiers);
    }

    /// @brief 可读文本（如 "Ctrl+Shift+O"），用于菜单显示与调试。
    [[nodiscard]] auto to_string() const -> std::string {
        std::string s;
        if ((modifiers & ModifierKey::Control) != 0) { // NOLINT(*-redundant-parentheses)
            s += "Ctrl+";
        }
        if ((modifiers & ModifierKey::Shift) != 0) { // NOLINT(*-redundant-parentheses)
            s += "Shift+";
        }
        if ((modifiers & ModifierKey::Alt) != 0) { // NOLINT(*-redundant-parentheses)
            s += "Alt+";
        }
        if ((modifiers & ModifierKey::Meta) != 0) { // NOLINT(*-redundant-parentheses)
            s += "Meta+";
        }
        s += key_name(key);
        return s;
    }
};

/// @brief 快捷键作用域。
enum class ShortcutScope : std::uint8_t {
    Global, ///< 全局：无论焦点在哪都响应
    Focus,  ///< 焦点：仅当作用域内控件持有焦点时响应
};

/// @brief 单条快捷键绑定：键组合 -> 动作。
struct ShortcutBinding {
    KeyCombo combo;                              ///< 触发键组合
    std::function<void()> action;                ///< 触发动作
    ShortcutScope scope = ShortcutScope::Global; ///< 作用域
    bool enabled = true;                         ///< 是否启用
    std::string description;                     ///< 描述（供调试/帮助面板）
};

/**
 * @brief 快捷键注册表：集中管理应用级快捷键绑定（specification/06-app-platform.md §8.4）。
 *
 * 由 Application 持有并在键盘事件派发前查询；也可独立用于测试。
 * 匹配成功即消费事件（返回 true），不再向焦点控件派发。
 *
 * 对标 Qt `QShortcut`、WPF `InputBinding`、Flutter `Shortcuts`/`Actions`。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class ShortcutRegistry {
  public:
    /// @brief 注册一条快捷键绑定，返回绑定 ID（用于解绑）。
    auto add(KeyCombo combo, std::function<void()> action, ShortcutScope scope = ShortcutScope::Global,
             std::string description = {}) -> int {
        const int id = m_next_id++;
        m_bindings.emplace_back(id, ShortcutBinding{ .combo = combo,
                                                     .action = std::move(action),
                                                     .scope = scope,
                                                     .enabled = true,
                                                     .description = std::move(description) });
        return id;
    }

    /// @brief 解绑指定 ID 的快捷键。
    auto remove(int id) -> void {
        for (auto it = m_bindings.begin(); it != m_bindings.end(); ++it) {
            if (it->first == id) {
                m_bindings.erase(it);
                return;
            }
        }
    }

    /// @brief 启用/禁用指定 ID 的快捷键。
    auto set_enabled(int id, bool enabled) -> void {
        for (auto &kv : m_bindings) {
            if (kv.first == id) {
                kv.second.enabled = enabled;
                return;
            }
        }
    }

    /// @brief 尝试处理键盘事件：匹配到已启用的绑定则执行动作并返回 true（消费）。
    /// @param e key event
    /// @param has_focus_widget 当前是否有焦点控件（Focus 作用域绑定仅在有焦点时触发）
    [[nodiscard]] auto handle(const KeyEvent &e, bool has_focus_widget = false) const -> bool {
        // NOLINTNEXTLINE(readability-use-anyofallof): 循环含副作用（执行动作并提前返回）
        for (const auto &kv : m_bindings | std::views::values) {
            const ShortcutBinding &b = kv;
            if (!b.enabled) {
                continue;
            }
            if (b.scope == ShortcutScope::Focus && !has_focus_widget) {
                continue;
            }
            if (b.combo.matches(e)) {
                if (b.action) {
                    b.action();
                }
                return true;
            }
        }
        return false;
    }

    /// @brief 已注册绑定数。
    [[nodiscard]] auto count() const -> std::size_t { return m_bindings.size(); }

    /// @brief 枚举全部绑定（帮助面板/调试用）。
    [[nodiscard]] auto bindings() const -> std::vector<ShortcutBinding> {
        std::vector<ShortcutBinding> out;
        out.reserve(m_bindings.size());
        for (const auto &kv : m_bindings | std::views::values) {
            out.push_back(kv);
        }
        return out;
    }

    /// @brief 清空全部绑定。
    auto clear() -> void { m_bindings.clear(); }

  private:
    std::vector<std::pair<int, ShortcutBinding>> m_bindings;
    int m_next_id = 1;
};

} // namespace aurora
