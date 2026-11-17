#pragma once

// 指针移动事件的帧内合并槽（**内部状态**，不进公共头）。
//
// 语义：一帧内到达的多次移动只保留「最新位置」一项（不插值），帧尾或遇到任何非移动事件时
// 冲刷一次。这样悬停命中、光标解析与移动回调的每帧工作量从「事件数」降为「1 次」，而最终
// 指针位置、按下/抬起目标与悬停进出顺序与逐事件处理一致（按下/抬起等事件前先冲刷，相对
// 顺序不变）。
//
// 只做「存 / 取」：调用方（窗口宿主）决定冲刷时机，测试可直接驱动同一套函数。

#include <optional>

#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief 把事件中的指针移动样本写入合并槽（覆盖旧样本，只保留最新位置）。
/// @param e    待派发事件；非 `MouseEvent` 或非 `MouseAction::Move` 时不写入。
/// @param slot 合并槽；被覆盖的旧样本不再派发（合并语义）。
/// @return 已缓存（调用方应直接返回、等待冲刷）为 true；否则 false（调用方照常派发）。
inline auto stage_pointer_move(Event &e, std::optional<MouseEvent> &slot) -> bool {
    const auto *move = dynamic_cast<const MouseEvent *>(&e);
    if (move == nullptr || move->action != MouseAction::Move) {
        return false;
    }
    slot = *move;  // 多帧样本只保留最新位置：不插值，终态即最后一个样本
    return true;
}

/// @brief 取出合并槽中的最新样本并清空槽位。
/// @param slot 合并槽。
/// @return 最新移动样本；槽位为空时为 `std::nullopt`。
inline auto take_pointer_move(std::optional<MouseEvent> &slot) -> std::optional<MouseEvent> {
    std::optional<MouseEvent> out;
    out.swap(slot);
    return out;
}

}  // namespace aurora::detail
