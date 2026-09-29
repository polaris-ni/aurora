#pragma once

#include <cstdint>
#include <functional>
#include <ranges>
#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/event/keycode.h"

namespace aurora {

/// @brief 键组合（快捷键描述）：修饰键位掩码 + 主键。
///
/// 对标 Qt `QKeySequence`、WPF `KeyGesture`、Flutter `SingleActivator`。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct KeyCombo {
    ModifierKey modifiers = ModifierKey::None;  ///< 修饰键组合（Ctrl/Shift/Alt/Meta 位掩码）
    KeyCode key = KeyCode::Unknown;  ///< 主键

    /// @brief 默认构造：无修饰键 + Unknown 主键，不匹配任何事件。
    KeyCombo() = default;
    /// @brief 由修饰键组合与主键构造。
    /// @param mods 修饰键位掩码（Ctrl/Shift/Alt/Meta 组合）。
    /// @param k 主键。
    KeyCombo(ModifierKey mods, KeyCode k) : modifiers(mods), key(k) {}
    /// @brief 仅主键构造（无修饰键）。
    /// @param k 主键。
    explicit KeyCombo(KeyCode k) : key(k) {}

    /// @brief 检查键盘事件是否匹配本组合（按下事件 + 键码 + 修饰键完全一致）。
    /// @param e 待匹配的键盘事件。
    /// @return 仅 `KeyAction::Down` 且键码与修饰键位掩码都一致时为 `true`。
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
    /// @return 按 Ctrl/Shift/Alt/Meta 顺序拼接修饰键、再接主键名（key_name）的组合文本；无修饰键时仅主键名。
    [[nodiscard]] auto to_string() const -> std::string {
        std::string s;
        if ((modifiers & ModifierKey::Control) != 0) {  // NOLINT(*-redundant-parentheses)
            s += "Ctrl+";
        }
        if ((modifiers & ModifierKey::Shift) != 0) {  // NOLINT(*-redundant-parentheses)
            s += "Shift+";
        }
        if ((modifiers & ModifierKey::Alt) != 0) {  // NOLINT(*-redundant-parentheses)
            s += "Alt+";
        }
        if ((modifiers & ModifierKey::Meta) != 0) {  // NOLINT(*-redundant-parentheses)
            s += "Meta+";
        }
        s += key_name(key);
        return s;
    }
};

/// @brief 快捷键作用域。
enum class ShortcutScope : std::uint8_t {
    Global,  ///< 全局：无论焦点在哪都响应
    Focus,  ///< 焦点：仅当作用域内控件持有焦点时响应
};

/// @brief 单条快捷键绑定：键组合 -> 动作。
struct ShortcutBinding {
    KeyCombo combo;  ///< 触发键组合
    std::function<void()> action;  ///< 触发动作
    ShortcutScope scope = ShortcutScope::Global;  ///< 作用域
    bool enabled = true;  ///< 是否启用
    std::string description;  ///< 描述（供调试/帮助面板）
};

/// @brief 快捷键注册表：集中管理应用级快捷键绑定（specification/06-app-platform.md §8.4）。
///
/// 由 Application 持有并在键盘事件派发前查询；也可独立用于测试。
/// 匹配成功即消费事件（返回 true），不再向焦点控件派发。
///
/// 对标 Qt `QShortcut`、WPF `InputBinding`、Flutter `Shortcuts`/`Actions`。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class ShortcutRegistry {
  public:
    /// @brief 注册一条快捷键绑定，返回绑定 ID（用于解绑）。
    /// @param combo 触发键组合。
    /// @param action 命中时执行的动作（可空；空动作仍会消费事件）。
    /// @param scope 作用域（默认 Global：无焦点也响应；Focus 仅在有焦点控件时触发）。
    /// @param description 描述文本（供帮助面板/调试展示，默认空）。
    /// @return 分配的绑定 ID（自 1 起单调递增，供 remove/set_enabled 使用）。
    auto add(KeyCombo combo, std::function<void()> action, ShortcutScope scope = ShortcutScope::Global,
             std::string description = {}) -> int {
        const int id = next_id_++;
        bindings_.emplace_back(id, ShortcutBinding{.combo = combo,  // 指定初始化列表跨行续写：构造绑定并登记
                                                   .action = std::move(action),
                                                   .scope = scope,
                                                   .enabled = true,
                                                   .description = std::move(description)});
        return id;
    }

    /// @brief 解绑指定 ID 的快捷键。
    /// @param id 绑定 ID（不存在的 ID 静默忽略）。
    auto remove(int id) -> void {
        for (auto it = bindings_.begin(); it != bindings_.end(); ++it) {
            if (it->first == id) {
                bindings_.erase(it);
                return;
            }
        }
    }

    /// @brief 启用/禁用指定 ID 的快捷键。
    /// @param id 绑定 ID（不存在的 ID 静默忽略）。
    /// @param enabled true 恢复响应，false 暂停响应（保留注册，handle 时跳过）。
    auto set_enabled(int id, bool enabled) -> void {
        for (auto &kv : bindings_) {
            if (kv.first == id) {
                kv.second.enabled = enabled;
                return;
            }
        }
    }

    /// @brief 尝试处理键盘事件：匹配到已启用的绑定则执行动作并返回 true（消费）。
    /// @param e key event
    /// @param has_focus_widget 当前是否有焦点控件（Focus 作用域绑定仅在有焦点时触发）
    /// @return 匹配到已启用绑定并已执行动作（事件被消费）时为 `true`，否则 `false`。
    [[nodiscard]] auto handle(const KeyEvent &e, bool has_focus_widget = false) const -> bool {
        // NOLINTNEXTLINE(readability-use-anyofallof): 循环含副作用（执行动作并提前返回）
        for (const auto &kv : bindings_ | std::views::values) {
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
    /// @return 当前注册表中的绑定条数（含被禁用的）。
    [[nodiscard]] auto count() const -> std::size_t { return bindings_.size(); }

    /// @brief 枚举全部绑定（帮助面板/调试用）。
    /// @return 绑定副本列表（含被禁用的；顺序为注册序）。
    [[nodiscard]] auto bindings() const -> std::vector<ShortcutBinding> {
        std::vector<ShortcutBinding> out;  // 逐条拷贝注册表后返回
        out.reserve(bindings_.size());  // 预分配与注册表等容量的空间
        for (const auto &kv : bindings_ | std::views::values) {
            out.push_back(kv);
        }
        return out;
    }

    /// @brief 清空全部绑定。
    auto clear() -> void { bindings_.clear(); }

  private:
    std::vector<std::pair<int, ShortcutBinding>> bindings_;
    int next_id_ = 1;  ///< 下一个可分配绑定 ID（自 1 起，add() 每次 +1，回收不复用）
};

}  // namespace aurora
