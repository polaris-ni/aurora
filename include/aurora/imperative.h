#pragma once

#include <functional>

/**
 * @brief 命令式逃生舱。
 *
 * 声明式 API 为 Aurora 主体；少数必须命令式表达的操作（焦点控制、滚动定位、动画中断、
 * 强制重绘等）收拢于本命名空间，避免污染声明式主体命名空间，便于静态分析与隔离。
 *
 * @note 与命令系统（`commands.h`）无关：本命名空间提供的是「就地执行一段命令式代码」的入口，
 *       命令系统提供的是可注册/可枚举/可投影到快捷键与菜单的具名动作。
 */
namespace aurora::imperative {

/// @brief 运行一段命令式代码块（逃生舱入口）。声明式优先；仅在无法用声明式表达时使用。
inline auto run_raw(const std::function<void()> &fn) -> void {
    if (fn) {
        fn();
    }
}

}  // namespace aurora::imperative
