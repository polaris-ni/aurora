#include "aurora/animation/animator.h"

#include <algorithm>

#include "aurora/core/accessibility.h"

namespace aurora {

auto AnimationController::forward(double from) -> void {
    if (from >= 0.0) {
        value_ = std::clamp(from, 0.0, 1.0);  // 起点夹取到合法进度区间（-1 为哨兵=从当前值继续）
    }
    status_ = (value_ >= 1.0) ? AnimationStatus::Completed : AnimationStatus::Forward;
}

auto AnimationController::reverse() -> void {
    status_ = (value_ <= 0.0) ? AnimationStatus::Dismissed : AnimationStatus::Reverse;
}

auto AnimationController::reset(double v) -> void {
    value_ = std::clamp(v, 0.0, 1.0);
    status_ = (value_ >= 1.0) ? AnimationStatus::Completed : AnimationStatus::Dismissed;
}

auto AnimationController::stop() -> void {
    // 「静止于最近端点」：进度冻结在当前位置，状态报最近端点（≥0.5 视为更接近终点）。
    status_ = (value_ >= 0.5) ? AnimationStatus::Completed : AnimationStatus::Dismissed;
}

auto AnimationController::tick(double dt_seconds) -> void {
    dirty_ = false;
    if (!is_animating()) {
        return;
    }
    // 减弱动态效果（`AccessibilitySettings::reduce_motion`）：不再按时间渐变，直接落在本次播放的
    // 目标端点（正向→1 / 反向→0）并置终态。状态机与「动画自然走完」完全一致，只是不产生中间帧，
    // 故绑定端（State 写入 / 排版）无需任何特判。
    if (current_accessibility_settings().reduce_motion) {
        if (status_ == AnimationStatus::Forward) {
            value_ = 1.0;
            status_ = AnimationStatus::Completed;
        } else {
            value_ = 0.0;
            status_ = AnimationStatus::Dismissed;
        }
        dirty_ = true;
        return;
    }
    const double dir = (status_ == AnimationStatus::Forward) ? 1.0 : -1.0;
    const double prev = value_;
    value_ += dir * dt_seconds / duration_;
    if (value_ >= 1.0) {
        value_ = 1.0;
        status_ = AnimationStatus::Completed;
        dirty_ = true;
    } else if (value_ <= 0.0) {
        value_ = 0.0;
        status_ = AnimationStatus::Dismissed;
        dirty_ = true;
    } else if (value_ != prev) {
        dirty_ = true;
    }
}

auto Animator::tick(double dt_seconds) const -> void {
    for (AnimationController *c : controllers_) {
        if (c != nullptr) {
            c->tick(dt_seconds);
        }
    }
    for (const Binding &b : on_tick_) {
        if (b.fn) {
            b.fn();
        }
    }
    for (AnimationController *c : controllers_) {
        if (c != nullptr) {
            c->clear_dirty();
        }
    }
}

}  // namespace aurora
