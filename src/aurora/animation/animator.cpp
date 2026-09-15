#include "aurora/animation/animator.h"

#include <algorithm>

#include "aurora/core/accessibility.h"

namespace aurora {

// 当前运行中的 Animator 槽位（无 Application 在跑时为 nullptr，见图表 grow-in 的终态降级）。
Animator *Animator::current_ = nullptr;

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

// ---- TimelinePlayer ----

TimelinePlayer::TimelinePlayer(TimelineResolved spec)
    : m_(std::make_shared<Payload>(std::move(spec))) {}

auto TimelinePlayer::bind_track(std::size_t slot, Track t) -> void {
    if (slot >= m_->spec.slot_count()) {
        return;  // 越界槽位：无操作（防御式，同 interval() 的全区间哨兵）
    }
    // 槽位区间+目标做轨道身份：覆盖同槽位同目标的旧绑定，其余按绑定序追加（确定写序）。
    for (auto &existing : m_->tracks_) {
        if (existing.interval == t.interval && existing.target == t.target) {
            existing = std::move(t);
            return;
        }
    }
    m_->tracks_.push_back(std::move(t));
}

auto TimelinePlayer::apply_all(const std::vector<Track> &tracks, double master_t) -> void {
    // 轨道按绑定序写入（确定序）；轨道间无顺序依赖假设——同帧写多个 State 的刷新
    // 由 State 信号机制定点触发，互不叠加。
    for (const Track &t : tracks) {
        if (t.apply != nullptr) {
            t.apply(t, master_t);
        }
    }
}

auto TimelinePlayer::forward(double from) -> void {
    m_->master_.forward(from);
    if (from == 0.0) {
        // 起播瞬间：全部轨道统一初始化到 begin 值（消除「未播轨道保持旧值」）。
        apply_all(m_->tracks_, m_->master_.value());
    }
}

auto TimelinePlayer::reverse() -> void { m_->master_.reverse(); }

auto TimelinePlayer::stop() -> void { m_->master_.stop(); }

auto TimelinePlayer::progress() const -> double { return m_->master_.value(); }

auto TimelinePlayer::status() const -> AnimationStatus { return m_->master_.status(); }

auto TimelinePlayer::is_completed() const -> bool { return m_->master_.is_completed(); }

auto TimelinePlayer::is_animating() const -> bool { return m_->master_.is_animating(); }

auto TimelinePlayer::on_completed(std::function<void()> cb) -> void { m_->on_completed = std::move(cb); }

auto TimelinePlayer::attach(Animator &a) -> void {
    a.drive(m_->master_);
    const auto p = m_;  // binding 持载荷副本：句柄析构后帧循环仍安全驱动（同 AnimatedValue）
    a.add_binding([p]() -> void {
        if (p->master_.dirty()) {
            apply_all(p->tracks_, p->master_.value());
            if (p->master_.is_completed() && !p->fired_completed) {
                p->fired_completed = true;
                if (p->on_completed) {
                    p->on_completed();
                }
            }
        }
    });
}

auto TimelinePlayer::tick(double dt_seconds) -> void {
    m_->master_.tick(dt_seconds);
    if (m_->master_.dirty()) {
        apply_all(m_->tracks_, m_->master_.value());
        if (m_->master_.is_completed() && !m_->fired_completed) {
            m_->fired_completed = true;
            if (m_->on_completed) {
                m_->on_completed();
            }
        }
    }
    m_->master_.clear_dirty();
}

}  // namespace aurora
