#include "aurora/animation/animator.h"

#include <algorithm>

namespace aurora {

auto AnimationController::forward(double from) -> void {
    if (from >= 0.0) {
        m_value = from;
    }
    m_status = (m_value >= 1.0) ? AnimationStatus::Completed : AnimationStatus::Forward;
}

auto AnimationController::reverse() -> void {
    m_status = (m_value <= 0.0) ? AnimationStatus::Dismissed : AnimationStatus::Reverse;
}

auto AnimationController::reset(double v) -> void {
    m_value = std::clamp(v, 0.0, 1.0);
    m_status = (m_value >= 1.0) ? AnimationStatus::Completed : AnimationStatus::Dismissed;
}

auto AnimationController::stop() -> void {
    m_status = (m_value >= 1.0) ? AnimationStatus::Completed : AnimationStatus::Dismissed;
}

auto AnimationController::tick(double dt_seconds) -> void {
    m_dirty = false;
    if (!is_animating()) {
        return;
    }
    const double dir = (m_status == AnimationStatus::Forward) ? 1.0 : -1.0;
    const double prev = m_value;
    m_value += dir * dt_seconds / m_duration;
    if (m_value >= 1.0) {
        m_value = 1.0;
        m_status = AnimationStatus::Completed;
        m_dirty = true;
    } else if (m_value <= 0.0) {
        m_value = 0.0;
        m_status = AnimationStatus::Dismissed;
        m_dirty = true;
    } else if (m_value != prev) {
        m_dirty = true;
    }
}

auto Animator::tick(double dt_seconds) const -> void {
    for (AnimationController *c : m_controllers) {
        if (c != nullptr) {
            c->tick(dt_seconds);
        }
    }
    for (const Binding &b : m_on_tick) {
        if (b.fn) {
            b.fn();
        }
    }
    for (AnimationController *c : m_controllers) {
        if (c != nullptr) {
            c->clear_dirty();
        }
    }
}

} // namespace aurora
