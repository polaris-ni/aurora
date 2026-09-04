#pragma once

#include <algorithm>
#include <vector>

#include "aurora/core/types.h"

namespace aurora {

/**
 * @brief 脏区域追踪器（specification/06-app-platform.md §3.2）：收集脏矩形并合并重叠区域。
 *
 * 帧循环协议：状态变更经 `mark(rect)` / `mark_all()` 标记脏区；渲染前
 * 经 `is_empty()` 判断可否跳帧（无脏区 = 上帧画面仍有效，跳过整帧重绘）；
 * 渲染后 `clear()`。矩形合并策略：新增矩形与已有矩形重叠/相邻时取并集，
 * 控制脏区列表规模（上限 `AURORA_MAX_RECTS`，超限退化为整帧脏）。
 *
 * 对标 Qt 脏区域优化、WPF 渲染树脏标记、Flutter `RepaintBoundary` 语义。
 */
class DirtyRegionTracker {
  public:
    /// @brief 脏矩形列表上限（默认 16）：超过则合并为整帧脏（避免碎片化开销超过收益）。
    /// 可经 `set_max_rects()` 调高（如列表滚动场景含大量离散脏矩形），以减少「超限→退化整帧」
    /// 的误触发；提高上限只会增加局部重绘精度（结果像素与整帧重绘逐位一致），不影响正确性。
    static constexpr std::size_t AURORA_MAX_RECTS = 16;

    /// @brief 取当前脏矩形列表上限。
    [[nodiscard]] static auto max_rects() -> std::size_t { return max_rects_; }

    /// @brief 设置脏矩形列表上限（运行期可调；默认 `AURORA_MAX_RECTS`）。
    static auto set_max_rects(std::size_t n) -> void { max_rects_ = n; }

    /// @brief 标记一个脏矩形（与已有矩形重叠时合并为并集）。
    auto mark(const Rect &r) -> void {
        if (is_full_) {
            return;  // 已整帧脏，无需再记录
        }
        if (r.size.width <= 0.0F || r.size.height <= 0.0F) {
            return;
        }

        Rect merged = r;
        // 与既有矩形重叠则吸收合并（可能连锁，简单迭代到不再变化）
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = rects_.begin(); it != rects_.end();) {
                if (intersects(*it, merged)) {
                    merged = union_of(*it, merged);
                    it = rects_.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        rects_.push_back(merged);
        if (rects_.size() > max_rects()) {
            mark_all();
        }
    }

    /// @brief 标记整帧脏（等价于全帧重绘）。
    auto mark_all() -> void {
        is_full_ = true;
        rects_.clear();
    }

    /// @brief 是否无任何脏区（可跳帧）。
    [[nodiscard]] auto is_empty() const -> bool { return !is_full_ && rects_.empty(); }

    /// @brief 是否整帧脏。
    [[nodiscard]] auto is_full() const -> bool { return is_full_; }

    /// @brief 当前脏矩形列表（整帧脏时为空列表——以 `is_full()` 判定）。
    [[nodiscard]] auto rects() const -> const std::vector<Rect> & { return rects_; }

    /// @brief 所有脏矩形的包围盒（空时返回零矩形）。
    [[nodiscard]] auto merged_bounds() const -> Rect {
        if (rects_.empty()) {
            return Rect{};
        }
        Rect acc = rects_[0];  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t i = 1; i < rects_.size(); ++i) {
            acc = union_of(acc, rects_[i]);  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        }
        return acc;
    }

    /// @brief 清空（渲染完成后调用）。
    auto clear() -> void {
        is_full_ = false;
        rects_.clear();
    }

  private:
    [[nodiscard]] static auto intersects(const Rect &a, const Rect &b) -> bool {
        return a.origin.x < b.origin.x + b.size.width && b.origin.x < a.origin.x + a.size.width &&
               a.origin.y < b.origin.y + b.size.height && b.origin.y < a.origin.y + a.size.height;
    }

    [[nodiscard]] static auto union_of(const Rect &a, const Rect &b) -> Rect {
        const float x1 = std::min(a.origin.x, b.origin.x);
        const float y1 = std::min(a.origin.y, b.origin.y);
        const float x2 = std::max(a.origin.x + a.size.width, b.origin.x + b.size.width);
        const float y2 = std::max(a.origin.y + a.size.height, b.origin.y + b.size.height);
        return Rect{.origin = Point{.x = x1, .y = y1}, .size = Size{.width = x2 - x1, .height = y2 - y1}};
    }

    static inline std::size_t max_rects_ = AURORA_MAX_RECTS;
    bool is_full_ = false;
    std::vector<Rect> rects_;
};

}  // namespace aurora
