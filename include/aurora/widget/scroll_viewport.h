#pragma once

#include <algorithm>

namespace aurora {

/// @brief 滚动视口内核（D0b）：全库滚动共享的 offset/clamp/符号约定。
///
/// 抽取自 Scroll 组件的滚动数学，供两类滚动方复用：
///  - 真实滚动控件（Scroll / LazyList / LazyRow / GridView）：自带内容测量与离屏缓冲，
///    通过 `clamp_offset` 复用同一夹取与符号约定；
///  - 通用容器 `OverflowStrategy::Scroll`（Widget 基类内嵌实例）：仅做「裁剪 + 内容平移」
///    的轻量滚动，复用同一 `apply_scroll` 语义，保证滚轮手感全库一致。
///
/// 符号约定（与全库滚动一致，见 Scroll::on_scroll 注释）：`delta_y` 正方向为「向上滚动」，
/// 此时 offset 应减小；offset 增大表示内容上移露出下方内容。
/// @note Thread: main-thread only
struct ScrollViewport {
    float offset_y = 0.0F;  ///< 当前滚动偏移（内容坐标，0 = 顶部）
    float content_h = 0.0F;  ///< 内容自然高度
    float viewport_h = 0.0F;  ///< 视口高度
    float step = 16.0F;  ///< 每单位滚轮增量的滚动像素（与 ScrollProps::step 默认一致）

    /// @brief 最大可滚动偏移：内容超出视口的部分，不足时为 0（不可滚）。
    [[nodiscard]] auto max_offset() const -> float { return std::max(0.0F, content_h - viewport_h); }

    /// @brief 滚轮增量驱动滚动：夹取到 [0, max_offset]，offset 变化时返回 true。
    auto apply_scroll(float delta_y) -> bool {
        const float target = clamp_offset(offset_y, delta_y, step, content_h, viewport_h);
        if (target == offset_y) {
            return false;
        }
        offset_y = target;
        return true;
    }

    /// @brief 共享夹取数学：由 (offset, delta_y, step, content_h, viewport_h) 算出新 offset。
    ///        无状态静态形式，供真实滚动控件在自有状态上复用同一约定。
    static auto clamp_offset(float offset, float delta_y, float step, float content_h, float viewport_h) -> float {
        const float max_off = std::max(0.0F, content_h - viewport_h);
        return std::clamp(offset - (delta_y * step), 0.0F, max_off);
    }
};

}  // namespace aurora
