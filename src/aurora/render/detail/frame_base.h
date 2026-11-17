// 内部头（不属于公共 API 面）：帧基底判定与 Painter 的分配通道。
// 仅在库实现与测试内使用；消费者不应包含本头。
#pragma once

#include "aurora/core/color.h"
#include "aurora/render/painter.h"

namespace aurora::detail {

/// @brief Painter 的帧基底内部通道：把「本帧可跳过整缓冲清零」的判定结果转给私有实现。
struct FrameBaseAccess {
    /// @brief 转发到 `Painter::begin_with_base`。
    /// @param painter 目标绘制器。
    /// @param width 画布宽（逻辑 dp）。
    /// @param height 画布高（逻辑 dp）。
    /// @param skip_clear true = 允许在尺寸与 scale 未变时保留既有像素。
    static auto begin(Painter &painter, int width, int height, bool skip_clear) -> void;
};

/// @brief 本帧基底：`begin` 之后立即铺满整块缓冲的底色。
struct FrameBase {
    Color color{0, 0, 0, 0};  ///< 铺满整块缓冲的底色（alpha = 255 才允许跳过清零）
    bool covers_whole_buffer = true;  ///< 该底色是否覆盖整块缓冲（半覆盖时不可跳过）
};

/// @brief 按帧基底分配画布：可证明整块缓冲将被不透明底色覆盖时跳过整缓冲清零。
///
/// 判定为充分条件而非启发式：底色完全不透明、覆盖整块缓冲、且当前不在显示列表录制态
/// （录制态下 `fill_rect` 只入命令流、不落像素）。任何一条不成立都回到全零基底，
/// 与 `Painter::begin` 的行为逐位相同。尺寸变化、首次分配与 `set_scale` 之后的首帧另由
/// 私有实现强制清零，调用方无需自行判断。
/// @param painter 目标绘制器。
/// @param width 画布宽（逻辑 dp）。
/// @param height 画布高（逻辑 dp）。
/// @param base 本帧基底（底色与覆盖范围）。
inline auto begin_frame(Painter &painter, int width, int height, const FrameBase &base) -> void {
    const bool can_skip = base.covers_whole_buffer && base.color.a == 255 && !painter.is_recording();
    FrameBaseAccess::begin(painter, width, height, can_skip);
}

}  // namespace aurora::detail
