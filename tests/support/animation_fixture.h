#pragma once

/// 动画类单测的作用域桩（tests/support/）。
///
/// 存在理由：`Animator::current()` 与 `current_accessibility_settings()` 都是进程级槽位，
/// 缩放 / 冻结 / reduce_motion 判据必须在用例内临时改写它们再复原——否则同二进制的其他
/// 用例会继承改写（单测以 `--run=<suite>` 与全量两种口径跑，泄漏会表现为「只有全量红」）。
/// 两处改写都成对出现，故收在本头，由各 utest 以栈对象使用。

#include "aurora/animation/animator.h"
#include "aurora/core/accessibility.h"

namespace aurora::testing {

/// @brief 作用域内把给定 `Animator` 登记为运行实例，出作用域还原此前的槽位。
///
/// `scaled_frame_dt` 的倍率取自 `Animator::current()`；无运行实例时按恒等返回，
/// 所以缩放 / 冻结类判据必须先把实例装进来才测得到。
class ScopedCurrentAnimator {
  public:
    /// @brief 安装实例并保存原槽位。
    /// @param anim 本作用域内作为 `Animator::current()` 的实例（非拥有；须比本对象存活更久）。
    explicit ScopedCurrentAnimator(Animator &anim) : saved_(Animator::current()) { Animator::set_current(&anim); }
    ScopedCurrentAnimator(const ScopedCurrentAnimator &) = delete;
    ScopedCurrentAnimator &operator=(const ScopedCurrentAnimator &) = delete;
    ScopedCurrentAnimator(ScopedCurrentAnimator &&) = delete;
    ScopedCurrentAnimator &operator=(ScopedCurrentAnimator &&) = delete;
    ~ScopedCurrentAnimator() { Animator::set_current(saved_); }

  private:
    Animator *saved_;  ///< 进入作用域前的运行实例槽位（可为 nullptr）
};

/// @brief 作用域内改写进程级无障碍设置的 `reduce_motion`，出作用域整份还原。
///
/// 还原按**值快照**而非只回写 false：宿主或其他用例可能已置其他字段，整份拷贝才不串改。
class ScopedReduceMotion {
  public:
    /// @brief 快照当前设置并置 `reduce_motion`。
    /// @param enabled 减弱动态效果开关值。
    explicit ScopedReduceMotion(bool enabled) : saved_(current_accessibility_settings()) {
        AccessibilitySettings next = saved_;
        next.reduce_motion = enabled;
        set_accessibility_settings(next);
    }
    ScopedReduceMotion(const ScopedReduceMotion &) = delete;
    ScopedReduceMotion &operator=(const ScopedReduceMotion &) = delete;
    ScopedReduceMotion(ScopedReduceMotion &&) = delete;
    ScopedReduceMotion &operator=(ScopedReduceMotion &&) = delete;
    ~ScopedReduceMotion() { set_accessibility_settings(saved_); }

  private:
    AccessibilitySettings saved_;  ///< 进入作用域前的完整设置快照
};

}  // namespace aurora::testing
