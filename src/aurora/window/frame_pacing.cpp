#include "aurora/window/frame_pacing.h"

#include <algorithm>

namespace aurora {

auto compute_wait_timeout(bool has_dirty, bool anim_active, double next_deadline_ms, double frame_budget_ms,
                          double elapsed_ms, bool backend_paced) -> double {
    // 活跃帧（有脏区/动画）：需要持续渲染，按帧预算节流。
    if (has_dirty || anim_active) {
        if (backend_paced) {
            return 0.0; // 后端自带节拍（vsync Present 阻塞）：CPU 端不再叠加 sleep，避免双重限速
        }
        if (frame_budget_ms <= 0.0) {
            return 0.0; // 不限帧率（max_fps=0）：立即进入下一帧，等价旧行为
        }
        return std::max(0.0, frame_budget_ms - elapsed_ms); // 剩余预算内睡到下一帧起点
    }
    // 空闲帧：仅剩定时任务可能唤醒——睡到最近到期时刻；无任务则无限等待（纯事件驱动）。
    if (next_deadline_ms < 0.0) {
        return -1.0;
    }
    return std::max(0.0, next_deadline_ms);
}

} // namespace aurora
