#pragma once

#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief 撤销命令（specification/02-state.md §6）：命令模式，成对提供 redo/undo。
///
/// 对标 Qt `QUndoCommand`。用 lambda 组合（AI 友好：无需继承）：
/// @code
/// stack.push(UndoCommand{
/// .redo = [&]{ value = 2; },
/// .undo = [&]{ value = 1; },
/// .description = "set value to 2" });
/// @endcode
///
struct UndoCommand {
    std::function<void()> redo;  ///< 执行/重做动作
    std::function<void()> undo;  ///< 撤销动作
    std::string description;  ///< 描述（供历史面板/调试）
};

/// @brief 撤销/重做栈（specification/02-state.md §6）。
///
/// `push` 执行命令的 redo 并入栈（截断重做历史）；`undo`/`redo` 沿栈移动。
/// 深度上限 `set_limit`（默认 100），超限丢弃最旧命令。
///
/// 对标 Qt `QUndoStack`、WPF `UndoEngine`。
/// @note Thread: main-thread only
/// @note Side-effects: none
class UndoStack {
  public:
    /// @brief 构造空栈（深度上限取默认 100）。
    UndoStack() = default;

    /// @brief 入栈并立即执行 redo；清空当前位置之后的重做历史。
    /// @param cmd 待入栈的命令；其 redo 在入栈前执行，undo 留待撤销时调用。
    auto push(UndoCommand cmd) -> void {
        // 截断重做分支
        commands_.resize(index_);
        if (cmd.redo) {
            cmd.redo();
        }
        commands_.push_back(std::move(cmd));
        ++index_;
        // 深度上限：丢弃最旧
        while (commands_.size() > limit_) {
            commands_.erase(commands_.begin());
            --index_;
        }
    }

    /// @brief 撤销一步（不可撤销时无操作，返回 false）。
    /// @return 成功执行撤销为 true；栈已无可撤销命令时为 false。
    auto undo() -> bool {
        if (!can_undo()) {
            return false;
        }
        --index_;
        if (commands_[index_].undo) {
            commands_[index_].undo();
        }
        return true;
    }

    /// @brief 重做一步（不可重做时无操作，返回 false）。
    /// @return 成功执行重做为 true；当前位置之后无命令可重做时为 false。
    auto redo() -> bool {
        if (!can_redo()) {
            return false;
        }
        if (commands_[index_].redo) {
            commands_[index_].redo();
        }
        ++index_;
        return true;
    }

    /// @brief 是否存在可撤销的命令（当前位置之前有历史）。
    /// @return index > 0 时为 true。
    [[nodiscard]] auto can_undo() const -> bool { return index_ > 0; }
    /// @brief 是否存在可重做的命令（当前位置之后有被撤销的历史）。
    /// @return index < 命令总数 时为 true。
    [[nodiscard]] auto can_redo() const -> bool { return index_ < commands_.size(); }

    /// @brief 下一次 undo 撤销的命令描述（不可撤销时空串）。
    /// @return 当前位置前一条命令的 description；无历史时返回空字符串。
    [[nodiscard]] auto undo_description() const -> std::string {
        return can_undo() ? commands_[index_ - 1].description : std::string{};
    }
    /// @brief 下一次 redo 重做的命令描述（不可重做时空串）。
    /// @return 当前位置那条命令的 description；无可重做时返回空字符串。
    [[nodiscard]] auto redo_description() const -> std::string {
        return can_redo() ? commands_[index_].description : std::string{};
    }

    /// @brief 历史命令总数（含可重做部分）。
    /// @return 命令表长度；与「可撤销数」的差即当前可重做条数。
    [[nodiscard]] auto count() const -> std::size_t { return commands_.size(); }
    /// @brief 当前位置（= 已执行命令数）。
    /// @return 区间 [0, count()] 的索引：[0, index) 已执行，[index, count) 可重做。
    [[nodiscard]] auto index() const -> std::size_t { return index_; }

    /// @brief 设置深度上限（立即按新上限丢弃最旧）。
    /// @param limit 新上限；传 0 时钳为 1（栈至少保留一条命令）。
    auto set_limit(std::size_t limit) -> void {
        limit_ = limit == 0 ? 1 : limit;
        while (commands_.size() > limit_) {
            commands_.erase(commands_.begin());
            if (index_ > 0) {
                --index_;
            }
        }
    }
    /// @brief 当前深度上限。
    /// @return 生效的上限值（恒 ≥ 1；`set_limit(0)` 会被钳为 1）。
    [[nodiscard]] auto limit() const -> std::size_t { return limit_; }

    /// @brief 清空全部历史。
    auto clear() -> void {
        commands_.clear();  // 丢弃全部命令（含可重做段）；位置指针由下一行归零
        index_ = 0;
    }

    /// @brief 把多个命令合并为单个宏命令（一次 undo/redo 整组执行）。
    /// @param cmds 子命令列表：redo 按序整体重放，undo 逆序整体回退（共享所有权持有）。
    /// @param description 宏命令的展示描述（供历史面板/调试）。
    /// @return 合成后的 UndoCommand，可正常 push 进栈。
    [[nodiscard]] static auto macro(std::vector<UndoCommand> cmds, std::string description) -> UndoCommand {
        auto shared = std::make_shared<std::vector<UndoCommand>>(std::move(cmds));
        UndoCommand out;
        out.description = std::move(description);
        out.redo = [shared]() -> void {
            for (auto &c : *shared) {
                if (c.redo) {
                    c.redo();
                }
            }
        };
        out.undo = [shared]() -> void {
            // 逆序撤销
            for (auto &it : std::views::reverse(*shared)) {
                if (it.undo) {
                    it.undo();
                }
            }
        };
        return out;
    }

  private:
    std::vector<UndoCommand> commands_;
    std::size_t index_ = 0;  ///< 当前位置：[0, index) 已执行，[index, size) 可重做
    std::size_t limit_ = 100;
};

}  // namespace aurora
