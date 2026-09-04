#include "aurora/animation/animator.h"

#include <algorithm>

namespace aurora {

auto AnimationController::forward(double from) -> void {
    if (from >= 0.0) {
        value_ = from;
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
    status_ = (value_ >= 1.0) ? AnimationStatus::Completed : AnimationStatus::Dismissed;
}

auto AnimationController::tick(double dt_seconds) -> void {
    dirty_ = false;
    if (!is_animating()) {
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
