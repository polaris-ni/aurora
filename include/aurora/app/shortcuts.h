#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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
    ModifierKey modifiers = ModifierKey::None; ///< 修饰键组合（Ctrl/Shift/Alt/Meta 位掩码；锁定位被 `matches` 忽略）
    KeyCode key = KeyCode::Unknown; ///< 主键

    /// @brief 默认构造：无修饰键 + Unknown 主键，不匹配任何事件。
    KeyCombo() = default;
    /// @brief 由修饰键组合与主键构造。
    /// @param mods 修饰键位掩码（Ctrl/Shift/Alt/Meta 组合）。
    /// @param k 主键。
    KeyCombo(ModifierKey mods, KeyCode k) : modifiers(mods), key(k) {
    }

    /// @brief 仅主键构造（无修饰键）。
    /// @param k 主键。
    explicit KeyCombo(KeyCode k) : key(k) {
    }

    /// @brief 检查键盘事件是否匹配本组合（按下事件 + 键码 + **可按住的**修饰位完全一致）。
    ///
    /// **只比可按住的位**：比较前两侧都先与 `AURORA_MODIFIER_PRESSABLE_MASK` 取子集，键盘锁定态位
    /// （`NumLock`）两侧一律屏蔽。`e.modifiers` 混装两类语义（按住态 / 锁定态，四个后端都会在事件
    /// 上盖章），而注册侧从不登记锁定位，若按整字节相等比较，`NumLock` 开着时**任何**未登记该位的
    /// 应用内快捷键恒不匹配——整层静默失效，且与焦点、作用域无关。
    ///
    /// 口径细则（四条均为刻意决定）：
    ///  - **仍是「相等」而非「包含」**：`Ctrl+O` 不匹配 `Ctrl+Shift+O`。这是 Qt `QKeySequence` /
    ///    WPF `KeyGesture` / Flutter `SingleActivator` 的共同语义；改成包含会让所有「更具体组合
    ///    优先」的注册表失去区分度。
    ///  - **注册侧的锁定位不可表达**：`KeyCombo{Control | NumLock, K}` 里的锁定位被忽略（等价于
    ///    `Control+K`）。此处**刻意不加断言、不加日志、不返回 `Result`**——`matches` 在每条按键
    ///    消息上被调用，热路径里的噪声比它想防的错更贵，该口径由本注释承担。
    ///  - **不引入 per-combo 的「要求 NumLock 开 / 关」字段**：当前无任何消费者需要，属扩大 API
    ///    预算。若将来需要，应另立字段并走一次独立的公共 API 变更。
    ///  - **不新增匹配档位 / 枚举参数**：保持单一算式，避免同一字段出现两个真值源。
    ///
    /// `to_string()` 不列锁定位，与上述「注册侧不可表达」的口径恰好一致，故不在此重复说明。
    ///
    /// @param e 待匹配的键盘事件。
    /// @return 仅 `KeyAction::Down`、键码一致、且**可按住的**修饰位一致时为 `true`。
    [[nodiscard]] auto matches(const KeyEvent &e) const -> bool {
        if (e.action != KeyAction::Down) {
            return false;
        }
        if (static_cast<KeyCode>(e.key) != key) {
            return false;
        }
        return (static_cast<std::uint8_t>(e.modifiers) & AURORA_MODIFIER_PRESSABLE_MASK) ==
               (static_cast<std::uint8_t>(modifiers) & AURORA_MODIFIER_PRESSABLE_MASK);
    }

    /// @brief 可读文本（如 "Ctrl+Shift+O"），用于菜单显示与调试。
    /// @return 按 Ctrl/Shift/Alt/Meta 顺序拼接修饰键、再接主键名（key_name）的组合文本；无修饰键时仅主键名。
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

    /// @brief 由 `to_string()` 形态的文本反解组合键（与其**逐字节往返**）。
    ///
    /// 可解析的形态**就是 `to_string()` 目前输出的那些**——若干修饰位字面量后接主键名，以 `+`
    /// 连接（`"Ctrl+Shift+P"` / `"Alt+Tab"` / `"F5"`）。**不**额外支持别的写法：组合键的显示串
    /// 规范只有 `to_string()` 一份，多认一种写法就多一份真值源（也多一处可漂移的对照表）。
    /// 反查经 `key_code_from_name` 走 `key_name` 的同一份键名字面量，不另列键名表。
    ///
    /// 三条刻意决定：
    ///  - **修饰次序不敏感**：`"Shift+Ctrl+P"` 与 `"Ctrl+Shift+P"` 同解，产出走同一份修饰位与
    ///    主键，故对 `to_string` 生成的那批串恒有 `to_string(from(s)) == s`。
    ///  - **同一修饰位重复出现即非法**（`"Ctrl+Ctrl+P"`）：它没有无歧义的解释，与其猜「算一次」
    ///    不如回 `nullopt`，交由调用方自己留痕。
    ///  - **畸形一律 `nullopt`，绝不回落**：空串、只有分隔符、`"Ctrl+"` 尾巴、未知键名、非 ASCII
    ///    主键名均**不**产出「无修饰 + 空主键」式的假有效值。回落会把「这一条没配上」伪装成
    ///    「配上了某个键」——宿主按覆盖表重放时，该条目会静默消失且无任何痕迹。
    ///
    /// 失败原因只有「形态不合规范」一种，故以 `std::optional` 而非 `Result` 表达（无错误码与
    /// 修复建议可携带；宿主侧的处置是统一的「记一条 WARN 并跳过该条」）。
    ///
    /// @param text 待解析的文本；须与 `to_string` 的输出逐字节一致（区分大小写）。
    /// @return 解析成功为对应组合键；任何畸形输入为 `std::nullopt`。
    /// @note Thread: main-thread only
    /// @note Side-effects: none
    /// @note Rebuildable: no
    [[nodiscard]] static auto from(std::string_view text) -> std::optional<KeyCombo> {
        if (text.empty()) {
            return std::nullopt; // 空串：连主键都没有，不构成组合键
        }
        auto mods = ModifierKey::None;
        std::size_t pos = 0;
        for (;;) {
            const std::size_t plus = text.find('+', pos);
            const bool is_main = plus == std::string_view::npos;
            const std::string_view token = is_main ? text.substr(pos) : text.substr(pos, plus - pos);
            if (token.empty()) {
                return std::nullopt; // 空片段："Ctrl+" 尾巴 / "Ctrl++P" / 纯分隔符
            }
            if (!is_main) {
                std::optional<ModifierKey> bit;
                if (token == "Ctrl") {
                    // 修饰位字面量与 to_string 的输出同源，不认 "Control" 等键名形态
                    bit = ModifierKey::Control;
                } else if (token == "Shift") {
                    bit = ModifierKey::Shift;
                } else if (token == "Alt") {
                    bit = ModifierKey::Alt;
                } else if (token == "Meta") {
                    bit = ModifierKey::Meta;
                }
                if (!bit.has_value()) {
                    return std::nullopt; // 非修饰位字面量：未知修饰名
                }
                if ((mods & *bit) != 0) { // NOLINT(*-redundant-parentheses)
                    return std::nullopt; // 同一位重复出现
                }
                mods = mods | *bit;
                pos = plus + 1;
                continue;
            }
            const std::optional<KeyCode> key = key_code_from_name(token);
            if (!key.has_value()) {
                return std::nullopt; // 未知主键名（含占位名 "Unknown" 与非 ASCII 串）
            }
            return KeyCombo{mods, *key};
        }
    }
};

/// @brief 快捷键作用域。
enum class ShortcutScope : std::uint8_t {
    Global, ///< 全局：无论焦点在哪都响应
    Focus, ///< 焦点：仅当作用域内控件持有焦点时响应
};

/// @brief 单条快捷键绑定：键组合 -> 动作。
struct ShortcutBinding {
    KeyCombo combo; ///< 触发键组合
    std::function<void()> action; ///< 触发动作
    ShortcutScope scope = ShortcutScope::Global; ///< 作用域
    bool enabled = true; ///< 是否启用
    std::string description; ///< 描述（供调试/帮助面板）
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
        bindings_.emplace_back(id, ShortcutBinding{.combo = combo, // 指定初始化列表跨行续写：构造绑定并登记
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
        std::vector<ShortcutBinding> out; // 逐条拷贝注册表后返回
        out.reserve(bindings_.size()); // 预分配与注册表等容量的空间
        for (const auto &kv : bindings_ | std::views::values) {
            out.push_back(kv);
        }
        return out;
    }

    /// @brief 清空全部绑定。
    auto clear() -> void { bindings_.clear(); }

private:
    std::vector<std::pair<int, ShortcutBinding>> bindings_;
    int next_id_ = 1; ///< 下一个可分配绑定 ID（自 1 起，add() 每次 +1，回收不复用）
};
} // namespace aurora