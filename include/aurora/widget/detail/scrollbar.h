#pragma once

// widget/detail/scrollbar.h - 四容器滚动条共用的几何与交互内核（库内件，不进伞头）。
//
// 纪律：
//   1. 纯值 + 纯函数，零分配、零字符串、无虚函数、不自读时钟；时间一律由调用方注入 now，
//      与 scroll_viewport.h 的 DragAutoScroll 同口径（宿主可冻结 / 快进，无头夹具可确定性步进）。
//   2. 几何口径只在此处给出一次：轨道内缩、滑块长度下界、拖拽映射、翻页方向四处不得在容器侧
//      各写一份。单位一律逻辑 dp，DPI 折算留给调用方既有路径。
//   3. Overlay 的显隐只在 advance(now) 里定夺一次——绘制期不取时，同一帧两次重绘的像素才
//      必定相同（帧循环非限速，按帧数或墙钟判定都会漂移）。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

#include "aurora/core/types.h"

namespace aurora::detail {

/// @brief 滚动条轴向。
enum class ScrollbarAxis : std::uint8_t {
    Vertical,  ///< 纵向：条贴在容器右侧，沿 y 轴度量。
    Horizontal,  ///< 横向：条贴在容器底侧，沿 x 轴度量。
};

/// @brief 内核几何常量；它们是不随设计语言变化的交互尺寸，故不是主题令牌。
///
/// 24 这一最小滑块长取自 GNOME / Windows / Material 3 三家对「最小可点尺寸」的共识；2 的
/// 内缩与悬停加粗同理。轨道保持窗口 1 秒是 Overlay 档的显隐时长。
inline constexpr float AURORA_SCROLLBAR_INSET = 2.0F;  ///< 轨道两端内缩（dp）。
inline constexpr float AURORA_SCROLLBAR_MIN_THUMB = 24.0F;  ///< 滑块最小长度（dp）。
inline constexpr float AURORA_SCROLLBAR_HOVER_GROWTH = 2.0F;  ///< 悬停 / 拖拽时的粗度增量（dp）。
inline constexpr auto AURORA_SCROLLBAR_OVERLAY_HOLD = std::chrono::seconds{1};  ///< Overlay 滚动后保持可见的时长。
inline constexpr float AURORA_SCROLLBAR_EPSILON = 1e-3F;  ///< 退化几何的浮点容差（dp）。

/// @brief 一次几何计算的输入，全部为容器本帧布局完成后的最终值。
struct ScrollbarMetrics {
    float content_length = 0.0F;  ///< 内容总长（纵向=内容高，横向=内容总宽）。
    float viewport_length = 0.0F;  ///< 视口长（纵向=视口高，横向=视口宽）。
    float offset = 0.0F;  ///< 当前滚动偏移，夹取前的原值。
    float thickness = 0.0F;  ///< 条粗细，来自 ControlMetrics::scrollbar_thickness。
    float inset = AURORA_SCROLLBAR_INSET;  ///< 轨道两端内缩。
    float min_thumb = AURORA_SCROLLBAR_MIN_THUMB;  ///< 滑块最小长度。
};

/// @brief 一次几何计算的结果；矩形均为容器局部坐标（容器自身盒原点归零）。
struct ScrollbarGeometry {
    Rect track{};  ///< 轨道矩形。
    Rect thumb{};  ///< 滑块矩形（不含悬停加粗）。
    float track_length = 0.0F;  ///< 轨道沿轴长度 = 视口长 − 2 × inset。
    float max_offset = 0.0F;  ///< 可滚上限 = 内容长 − 视口长。
    bool overflowing = false;  ///< 内容是否真溢出（半像素容差）。
    bool drawable = false;  ///< 本帧是否应绘制（另需轨道长度与粗细为正）。
};

/// @brief 取点在轴向上的坐标；横向取 x，纵向取 y。
/// @param p 容器局部坐标。
/// @param axis 轴向。
/// @return 沿轴坐标。
[[nodiscard]] constexpr auto scrollbar_axis_value(Point p, ScrollbarAxis axis) noexcept -> float {
    return axis == ScrollbarAxis::Vertical ? p.y : p.x;
}

/// @brief 取滑块沿轴长度。
/// @param g 几何结果。
/// @param axis 轴向。
/// @return 滑块长度（dp）；不可绘制时为 0。
[[nodiscard]] constexpr auto scrollbar_thumb_length(const ScrollbarGeometry &g, ScrollbarAxis axis) noexcept -> float {
    return g.drawable ? (axis == ScrollbarAxis::Vertical ? g.thumb.size.height : g.thumb.size.width) : 0.0F;
}

/// @brief 计算轨道与滑块矩形；四容器共用这一份口径。
///
/// 下界须先与 track_length 取小：极小视口下 track_length < min_thumb 时，直接
/// clamp(v, min_thumb, track_length) 会违反 std::clamp 的 lo <= hi 前提（未定义行为）。
/// track_length <= 0 或粗细非正时整条不绘制，但 overflowing 仍如实反映内容是否溢出。
///
/// @param m 本帧最终度量。
/// @param axis 轴向。
/// @param box 容器自身盒（局部坐标）。
/// @return 几何结果；不可绘制时仅 overflowing / track_length / max_offset 有意义。
[[nodiscard]] constexpr auto scrollbar_geometry(const ScrollbarMetrics &m, ScrollbarAxis axis, const Rect &box) noexcept
    -> ScrollbarGeometry {
    ScrollbarGeometry g{};
    g.overflowing = m.content_length > (m.viewport_length + 0.5F);
    g.max_offset = std::max(0.0F, m.content_length - m.viewport_length);
    g.track_length = m.viewport_length - (2.0F * m.inset);
    g.drawable = g.overflowing && g.track_length > 0.0F && m.thickness > 0.0F && m.content_length > 0.0F;
    if (!g.drawable) {
        return g;
    }
    if (axis == ScrollbarAxis::Vertical) {
        g.track = Rect{.origin = Point{.x = box.right() - m.inset - m.thickness, .y = box.origin.y + m.inset},
                       .size = Size{.width = m.thickness, .height = g.track_length}};
    } else {
        g.track = Rect{.origin = Point{.x = box.origin.x + m.inset, .y = box.bottom() - m.inset - m.thickness},
                       .size = Size{.width = g.track_length, .height = m.thickness}};
    }
    const float lower = std::min(m.min_thumb, g.track_length);
    const float theoretical = g.track_length * m.viewport_length / m.content_length;
    const float thumb_length = std::clamp(theoretical, lower, g.track_length);
    const float offset = std::clamp(m.offset, 0.0F, g.max_offset);
    const float thumb_pos = g.max_offset > 0.0F ? ((g.track_length - thumb_length) * offset / g.max_offset) : 0.0F;
    if (axis == ScrollbarAxis::Vertical) {
        g.thumb = Rect{.origin = Point{.x = g.track.origin.x, .y = g.track.origin.y + thumb_pos},
                       .size = Size{.width = m.thickness, .height = thumb_length}};
    } else {
        g.thumb = Rect{.origin = Point{.x = g.track.origin.x + thumb_pos, .y = g.track.origin.y},
                       .size = Size{.width = thumb_length, .height = m.thickness}};
    }
    return g;
}

/// @brief 悬停 / 拖拽加粗后的滑块矩形，以轨道中轴对齐（沿轴位置与长度不变）。
/// @param g 几何结果。
/// @param axis 轴向。
/// @param growth 粗度增量（dp）。
/// @return 加粗后的矩形；不可绘制时返回空矩形。
[[nodiscard]] constexpr auto scrollbar_hover_thumb(const ScrollbarGeometry &g, ScrollbarAxis axis,
                                                   float growth) noexcept -> Rect {
    if (!g.drawable || growth <= 0.0F) {
        return g.thumb;
    }
    const float half = growth / 2.0F;
    if (axis == ScrollbarAxis::Vertical) {
        return Rect{.origin = Point{.x = g.thumb.origin.x - half, .y = g.thumb.origin.y},
                    .size = Size{.width = g.thumb.size.width + growth, .height = g.thumb.size.height}};
    }
    return Rect{.origin = Point{.x = g.thumb.origin.x, .y = g.thumb.origin.y - half},
                .size = Size{.width = g.thumb.size.width, .height = g.thumb.size.height + growth}};
}

/// @brief 指针是否落在条带内（轨道或加粗滑块）；四容器据此决定命中链只交自身。
/// @param g 几何结果。
/// @param axis 轴向。
/// @param p 容器局部坐标。
/// @return 落在条带内为 true；不可绘制时恒 false。
[[nodiscard]] constexpr auto scrollbar_contains(const ScrollbarGeometry &g, ScrollbarAxis axis, Point p) noexcept
    -> bool {
    return g.drawable &&
           (g.track.contains(p) || scrollbar_hover_thumb(g, axis, AURORA_SCROLLBAR_HOVER_GROWTH).contains(p));
}

/// @brief 指针是否落在滑块上（含加粗后的盒）。
/// @param g 几何结果。
/// @param axis 轴向。
/// @param p 容器局部坐标。
/// @return 落在滑块内为 true；不可绘制时恒 false。
[[nodiscard]] constexpr auto scrollbar_thumb_contains(const ScrollbarGeometry &g, ScrollbarAxis axis, Point p) noexcept
    -> bool {
    return g.drawable && scrollbar_hover_thumb(g, axis, AURORA_SCROLLBAR_HOVER_GROWTH).contains(p);
}

/// @brief 拖拽映射：指针沿轴位移换算成偏移位移。
/// @param g 几何结果。
/// @param axis 轴向。
/// @param delta_pointer 指针相对按下点的沿轴位移（dp）。
/// @return 偏移位移；滑块占满轨道（分母落在容差内）等退化几何返回空，调用方不得响应拖拽。
[[nodiscard]] constexpr auto scrollbar_drag_delta(const ScrollbarGeometry &g, ScrollbarAxis axis,
                                                  float delta_pointer) noexcept -> std::optional<float> {
    const float span = g.track_length - scrollbar_thumb_length(g, axis);
    if (!g.drawable || span <= AURORA_SCROLLBAR_EPSILON || g.max_offset <= AURORA_SCROLLBAR_EPSILON) {
        return std::nullopt;
    }
    return delta_pointer * g.max_offset / span;
}

/// @brief 轨道翻页：点击点落在滑块之前 / 之后各退 / 进一页，落在滑块内不变。
///
/// 不做「点击即居中跳转」：那会让一次误点把内容甩出很远。按住不重复翻页由调用方保证
/// （只有 Press 走本函数，Move / Release 不翻）。
///
/// @param g 几何结果。
/// @param m 本帧度量（提供当前偏移与一页长度）。
/// @param axis 轴向。
/// @param p 容器局部坐标。
/// @return 夹到 [0, max_offset] 的新偏移。
[[nodiscard]] constexpr auto scrollbar_page_offset(const ScrollbarGeometry &g, const ScrollbarMetrics &m,
                                                   ScrollbarAxis axis, Point p) noexcept -> float {
    if (!g.drawable) {
        return m.offset;
    }
    const float along = scrollbar_axis_value(p, axis);
    const float track_start = scrollbar_axis_value(g.track.origin, axis);
    const float thumb_start = scrollbar_axis_value(g.thumb.origin, axis) - track_start;
    const float thumb_end = thumb_start + scrollbar_thumb_length(g, axis);
    float target = m.offset;
    if (along < track_start + thumb_start) {
        target = m.offset - m.viewport_length;
    } else if (along > track_start + thumb_end) {
        target = m.offset + m.viewport_length;
    }
    return std::clamp(target, 0.0F, g.max_offset);
}

/// @brief 指针动作种类（由容器把派发器的按下 / 移动 / 抬起归一后传入）。
enum class ScrollbarPointerAction : std::uint8_t {
    Press,  ///< 按下。
    Move,  ///< 移动。
    Release,  ///< 抬起。
};

/// @brief 一次指针动作的结论；容器据此写偏移、标脏并决定是否把事件交给基类。
struct ScrollbarPointerResult {
    bool handled = false;  ///< 命中条带（或拖拽在途）时置 true，事件不再冒泡。
    bool offset_changed = false;  ///< 是否需要写入新偏移（翻页或拖拽）。
    float offset = 0.0F;  ///< 新偏移，仅在 offset_changed 为真时有效。
};

/// @brief 滚动条的绘制期交互状态（每容器持有一份，值语义）。
struct ScrollbarInteraction {
    bool hovered = false;  ///< 指针是否悬停在滑块上（含加粗盒）。
    bool dragging = false;  ///< 滑块拖拽是否在途；按下时命中链捕获，离开控件仍继续。
    bool visible = false;  ///< 本帧条是否可见；只由 advance(now) 定夺。
    bool pointer_inside_container = false;  ///< Overlay 显隐的「指针悬停在本容器内」输入。
    float drag_origin_offset = 0.0F;  ///< 按下瞬间的偏移。
    float drag_origin_pointer = 0.0F;  ///< 按下瞬间的沿轴指针坐标。
    std::chrono::steady_clock::time_point hold_until;  ///< Overlay 滚动后保持可见的到期时刻（默认构造即 epoch）。

    /// @brief 处理一次指针动作；不取时钟，不在此改动 visible。
    /// @param action 动作种类。
    /// @param p 容器局部坐标。
    /// @param g 本帧几何。
    /// @param m 本帧度量。
    /// @param axis 轴向。
    /// @param overlay true 为 Overlay 档；Classic 档恒可见。
    /// @param now 帧首注入的时间（当前仅用于记录，显隐由 advance 定夺）。
    /// @param inside 指针是否落在本容器内（Overlay 显隐输入）。
    /// @return 事件消费与偏移写入结论。
    [[nodiscard]] auto on_pointer(ScrollbarPointerAction action, Point p, const ScrollbarGeometry &g,
                                  const ScrollbarMetrics &m, ScrollbarAxis axis, [[maybe_unused]] bool overlay,
                                  [[maybe_unused]] std::chrono::steady_clock::time_point now, bool inside) noexcept
        -> ScrollbarPointerResult {
        pointer_inside_container = inside;
        hovered = scrollbar_thumb_contains(g, axis, p);
        ScrollbarPointerResult result{};
        const bool hit = scrollbar_contains(g, axis, p);
        if (action == ScrollbarPointerAction::Press) {
            if (!hit) {
                return result;
            }
            result.handled = true;
            if (hovered) {
                dragging = true;
                drag_origin_offset = m.offset;
                drag_origin_pointer = scrollbar_axis_value(p, axis);
            } else {
                result.offset_changed = true;
                result.offset = scrollbar_page_offset(g, m, axis, p);
            }
            return result;
        }
        if (action == ScrollbarPointerAction::Move) {
            if (!dragging) {
                return result;
            }
            result.handled = true;
            const auto delta = scrollbar_drag_delta(g, axis, scrollbar_axis_value(p, axis) - drag_origin_pointer);
            if (delta.has_value()) {
                result.offset_changed = true;
                result.offset = std::clamp(drag_origin_offset + *delta, 0.0F, g.max_offset);
            }
            return result;
        }
        if (dragging) {
            dragging = false;
            result.handled = true;
        }
        return result;
    }

    /// @brief 帧首按注入时间定夺一次显隐，返回可见性是否翻转（供宿主决定是否标脏）。
    /// @param now 帧首时间。
    /// @param overlay true 为 Overlay 档；Classic 档恒可见。
    /// @return 可见性与上一帧不同为 true。
    [[nodiscard]] auto advance(std::chrono::steady_clock::time_point now, bool overlay) noexcept -> bool {
        const bool next = !overlay || pointer_inside_container || now < hold_until;
        const bool changed = next != visible;
        visible = next;
        return changed;
    }

    /// @brief 滚轮或程序化滚动后刷新保持窗口；显隐仍留给下一次 advance 定夺。
    /// @param now 帧首时间。
    /// @param overlay true 为 Overlay 档；Classic 档无需保持窗口。
    auto on_scroll(std::chrono::steady_clock::time_point now, bool overlay) noexcept -> void {
        if (overlay) {
            hold_until = now + AURORA_SCROLLBAR_OVERLAY_HOLD;
        }
    }
};

}  // namespace aurora::detail
