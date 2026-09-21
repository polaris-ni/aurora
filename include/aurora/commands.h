#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aurora/app/menu.h"
#include "aurora/app/shortcuts.h"
#include "aurora/widget/props_io.h"

namespace aurora {

/**
 * @brief 一条命令：可执行动作的一等公民描述。
 *
 * 命令是**快捷键、菜单项、命令面板三者的共同数据源**——三者均为它的投影，避免同一动作
 * 在多处重复定义（对标 VSCode Command、Qt `QAction`、WPF `ICommand`/`RoutedCommand`、
 * Flutter `Intent`+`Actions`）。
 *
 * 启用条件采用「谓词实判 + 标签描述」两段式：`enabled` 承担运行期正确性（空 = 恒启用），
 * `when_label` 仅作展示与序列化标签（**不参与求值**，也不解析任何条件 DSL）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none（`action` 由注册表在 `invoke` 时调用）
 * @note Rebuildable: no
 */
struct Command {
    std::string id;  ///< 唯一标识（如 "file.open"）
    std::string title;  ///< 显示文本（面板/菜单/帮助）
    std::string icon;  ///< 图标标识（与 `MenuItem::icon` 对齐）
    std::string category;  ///< 分组（菜单分组 / 面板次级信息）
    std::function<void()> action;  ///< 执行体；空 = 占位（不可调用）
    std::optional<KeyCombo> default_binding;  ///< 默认快捷键（供 `bind_shortcuts` 投影）
    ShortcutScope scope = ShortcutScope::Global;  ///< 快捷键作用域（随 `default_binding`）
    std::function<bool()> enabled;  ///< 启用谓词；空 = 恒启用
    std::string when_label;  ///< 启用条件的展示/序列化标签（不求值）
};

/// @brief 命令名的模糊匹配得分（不区分大小写的子序列匹配）。
/// @return -1 = 不匹配；空 `query` = 0（匹配全部）；否则得分越大越优（有效匹配恒 ≥ 0）。
[[nodiscard]] auto command_fuzzy_score(const std::string &query, const std::string &text) -> int;

/**
 * @brief 命令注册表：命令的唯一真源（注册 / 启停 / 调用 / 检索 / 投影）。
 *
 * 快捷键与菜单是它的两个投影（`bind_shortcuts` / `to_menu_items`），命令面板是第三个消费方；
 * 三者共用同一 `invoke(id)` 出口，故启用条件与空动作判定单点生效。
 *
 * @note Thread: main-thread only
 * @note Side-effects: `invoke` / `bind_shortcuts` 有副作用
 * @note Rebuildable: no
 */
class CommandRegistry {
  public:
    /// @brief 注册命令，返回注册号；同 `id` 重复注册**覆盖**旧定义并保持注册序。
    /// 空 `id` 视为非法，返回 -1 且不入表。
    auto add(Command cmd) -> int;

    /// @brief 按 `id` 解绑，并连带撤销其快捷键绑定（若已 `bind_shortcuts`）；返回是否命中。
    auto remove(const std::string &id) -> bool;

    auto clear() -> void;

    [[nodiscard]] auto count() const -> std::size_t { return cmds_.size(); }
    [[nodiscard]] auto contains(const std::string &id) const -> bool { return find(id) != nullptr; }

    /// @brief 按 `id` 查找；未命中返回 nullptr。
    [[nodiscard]] auto find(const std::string &id) const -> const Command *;

    /// @brief 全部命令（注册序）。
    [[nodiscard]] auto all() const -> std::vector<Command> { return cmds_; }

    /// @brief 当前是否启用 = 覆盖开关（若有）&& 谓词求值（无谓词 = 恒真）。未命中 `id` = false。
    [[nodiscard]] auto is_enabled(const std::string &id) const -> bool;

    /// @brief 设置覆盖开关（叠加在谓词之上）；返回是否命中。用于临时禁用某命令而不改谓词。
    auto set_enabled(const std::string &id, bool on) -> bool;

    /// @brief 求值启用条件后执行 `action`。返回是否真的执行（未命中 / 未启用 / 无 action = false）。
    [[nodiscard]] auto invoke(const std::string &id) const -> bool;

    /// @brief 模糊检索：按（得分降序, 标题升序）稳定排序；`only_enabled` 时过滤未启用者。
    [[nodiscard]] auto search(const std::string &query, bool only_enabled = true) const -> std::vector<const Command *>;

    /// @brief 序列化信封：`{"commands":[{id,title,icon?,category?,when?,enabled,invocable,default_binding?}]}`。
    /// 可选字段仅在非空时输出；自描述信封可直接作为工具面入参。
    [[nodiscard]] auto to_json() const -> Json;

    /// @brief 投影 1：把各命令的 `default_binding` 注册进快捷键表，动作即 `invoke(id)`。
    /// 幂等——重复调用先移除上次产生的绑定再重建；并记住 `sr` 供命令面板复用。
    auto bind_shortcuts(ShortcutRegistry &sr) -> void;

    /// @brief 投影 2：生成菜单项（`label←title`、`icon`、`enabled`、`shortcut_text`、`on_click←invoke`）。
    [[nodiscard]] auto to_menu_items() const -> std::vector<MenuItem>;

    /// @brief `bind_shortcuts` 的目标（未绑定 = nullptr）。
    [[nodiscard]] auto shortcuts() const -> ShortcutRegistry * { return shortcuts_; }

  private:
    /// @brief 命令本身的启用判定（不含 `id` 查找）：覆盖开关 && 谓词。
    [[nodiscard]] auto enabled_of(const Command &cmd) const -> bool;

    std::vector<Command> cmds_;  ///< 注册序容器
    std::unordered_map<std::string, std::size_t> index_;  ///< id → cmds_ 下标
    std::unordered_map<std::string, bool> enabled_overrides_;  ///< id → 覆盖开关
    std::unordered_map<std::string, int> shortcut_of_;  ///< id → 快捷键绑定 id
    ShortcutRegistry *shortcuts_ = nullptr;  ///< bind_shortcuts 的目标（非拥有）
    int next_reg_id_ = 1;  ///< add 的注册号自增源
};

}  // namespace aurora
