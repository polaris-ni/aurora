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
    // 复位是「退出循环」的既定出口：模式与计数一并清回缺省，避免下次 forward 又自动续播。
    mode_ = PlaybackMode::Once;
    alternate_ = false;
    remaining_ = AURORA_INFINITE_REMAINING;
}

auto AnimationController::stop() -> void {
    // 「静止于最近端点」：进度冻结在当前位置，状态报最近端点（≥0.5 视为更接近终点）。
    status_ = (value_ >= 0.5) ? AnimationStatus::Completed : AnimationStatus::Dismissed;
    mode_ = PlaybackMode::Once;  // 同上：stop 即清循环模式（无限循环的退出方式）
    alternate_ = false;
    remaining_ = AURORA_INFINITE_REMAINING;
}

auto AnimationController::on_endpoint(AnimationStatus terminal) -> void {
    if (mode_ != PlaybackMode::Repeat) {
        status_ = terminal;  // 既有语义：钳制在端点并置终态
        return;
    }
    if (remaining_ != AURORA_INFINITE_REMAINING) {
        --remaining_;  // 有限计数：每次到达端点计一次（与 Flutter repeat 的 reverse 计数同形）
        if (remaining_ <= 0) {
            mode_ = PlaybackMode::Once;  // 次数耗尽：在最后到达的端点置终态，之后不再续播
            alternate_ = false;
            status_ = terminal;
            return;
        }
    }
    if (alternate_) {
        // 往复：停在被到达的端点，只翻转方向，下一帧从该端点反向续跑（无进度跳变）。
        status_ = (terminal == AnimationStatus::Completed) ? AnimationStatus::Reverse : AnimationStatus::Forward;
    } else {
        // 同向重启：进度回到另一端点续跑（本帧有跳变，故调用侧照旧置脏）。
        value_ = (terminal == AnimationStatus::Completed) ? 0.0 : 1.0;
    }
}

auto AnimationController::tick(double dt_seconds) -> void {
    dirty_ = false;  // 只清推进脏：set_progress 的外部脏位须活到帧末 clear_dirty（见 Animator::tick）
    if (!is_animating()) {
        return;
    }
    // 减弱动态效果（`AccessibilitySettings::reduce_motion`）：不再按时间渐变，直接落在本次播放的
    // 目标端点（正向→1 / 反向→0）并置终态。状态机与「动画自然走完」完全一致，只是不产生中间帧，
    // 故绑定端（State 写入 / 排版）无需任何特判。
    //
    // **优先于循环**：无障碍档下 `repeat(-1)` 同样一次到位即止（清循环模式），否则「呼吸态」会
    // 变成「无限瞬移」；缩放也不参与本短路（短路路径不乘倍率，无障碍语义是「一步到位」而非「慢放」）。
    //
    // **排在零步长守卫之前**：本短路不使用 dt，故零步长（冻结 / 无推进帧）不改变它——「一步到位」
    // 本来就是一帧到位，没有什么可被冻住。次序与 `SpringAnimation::Payload::advance` 一致，两个驱动
    // 件对同一无障碍档给出同一形态；既有 `reduce_motion_snaps_controller_to_endpoint` 亦以此为准。
    if (current_accessibility_settings().reduce_motion) {
        if (status_ == AnimationStatus::Forward) {
            value_ = 1.0;
            status_ = AnimationStatus::Completed;
        } else {
            value_ = 0.0;
            status_ = AnimationStatus::Dismissed;
        }
        mode_ = PlaybackMode::Once;
        alternate_ = false;
        remaining_ = AURORA_INFINITE_REMAINING;
        dirty_ = true;
        return;
    }
    // 零步长（时间缩放冻结 / 无推进帧）：不推进也不触发端点计数。若在往复续播时落到端点值上
    // 而不设本守卫，冻结的每一帧都会重复触发端点事件，把有限计数白白耗光。
    if (dt_seconds <= 0.0) {
        return;
    }
    const double dir = (status_ == AnimationStatus::Forward) ? 1.0 : -1.0;
    const double prev = value_;
    value_ += dir * dt_seconds / duration_;
    if (value_ >= 1.0) {
        value_ = 1.0;
        on_endpoint(AnimationStatus::Completed);
        dirty_ = true;
    } else if (value_ <= 0.0) {
        value_ = 0.0;
        on_endpoint(AnimationStatus::Dismissed);
        dirty_ = true;
    } else if (value_ != prev) {
        dirty_ = true;
    }
}

auto Animator::tick(double dt_seconds) const -> void {
    // 单一漏斗：本 Animator 的倍率在此折算一次，登记控制器与带 dt 的绑定都只看到折算后的步长
    // （绑定回调拿不到裸 dt，正是为了堵掉绕过漏斗的路径）。缩放为 0 时全部控制器本帧不推进，
    // 既不置脏也不写回，帧调度因此仍走空闲分支——冻结不等于满帧空转。
    const double dt = scale_frame_dt(dt_seconds);
    for (AnimationController *c : controllers_) {
        if (c != nullptr) {
            c->tick(dt);
        }
    }
    for (const Binding &b : on_tick_) {
        if (b.fn) {
            b.fn();
        }
        if (b.dt_fn) {
            b.dt_fn(dt);
        }
    }
    for (AnimationController *c : controllers_) {
        if (c != nullptr) {
            c->clear_dirty();
        }
    }
}

// ---- TimelinePlayer ----

TimelinePlayer::TimelinePlayer(TimelineResolved spec) : m_(std::make_shared<Payload>(std::move(spec))) {}

auto TimelinePlayer::bind_track(std::size_t slot, Track t) const -> void {
    if (slot >= m_->spec.slot_count()) {
        return;  // 越界槽位：无操作（防御式，同 interval() 的全区间哨兵）
    }
    // 槽位区间+目标做轨道身份：覆盖同槽位同目标的旧绑定，其余按绑定序追加（确定写序）。
    for (auto &existing : m_->tracks) {
        if (existing.interval == t.interval && existing.target == t.target) {
            existing = std::move(t);
            return;
        }
    }
    m_->tracks.push_back(std::move(t));
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

auto TimelinePlayer::forward(double from) const -> void {
    m_->master.forward(from);
    if (from == 0.0) {
        // 起播瞬间：全部轨道统一初始化到 begin 值（消除「未播轨道保持旧值」）。
        apply_all(m_->tracks, m_->master.value());
    }
}

auto TimelinePlayer::reverse() const -> void { m_->master.reverse(); }

auto TimelinePlayer::stop() const -> void { m_->master.stop(); }

auto TimelinePlayer::progress() const -> double { return m_->master.value(); }

auto TimelinePlayer::status() const -> AnimationStatus { return m_->master.status(); }

auto TimelinePlayer::is_completed() const -> bool { return m_->master.is_completed(); }

auto TimelinePlayer::is_animating() const -> bool { return m_->master.is_animating(); }

auto TimelinePlayer::on_completed(std::function<void()> cb) const -> void { m_->on_completed = std::move(cb); }

auto TimelinePlayer::attach(Animator &a) const -> void {
    a.drive(m_->master);
    const auto p = m_;  // binding 持载荷副本：句柄析构后帧循环仍安全驱动（同 AnimatedValue）
    a.add_binding([p]() -> void {
        if (p->master.dirty()) {
            apply_all(p->tracks, p->master.value());
            if (p->master.is_completed() && !p->fired_completed) {
                p->fired_completed = true;
                if (p->on_completed) {
                    p->on_completed();
                }
            }
        }
    });
}

auto TimelinePlayer::tick(double dt_seconds) const -> void {
    // 手动自驱路径同样只经漏斗取步长（`Animator::current()` 为空时恒等，golden 不受影响）。
    m_->master.tick(scaled_frame_dt(dt_seconds));
    if (m_->master.dirty()) {
        apply_all(m_->tracks, m_->master.value());
        if (m_->master.is_completed() && !m_->fired_completed) {
            m_->fired_completed = true;
            if (m_->on_completed) {
                m_->on_completed();
            }
        }
    }
    m_->master.clear_dirty();
}

}  // namespace aurora
