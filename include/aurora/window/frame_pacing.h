#pragma once

// 帧调度决策：纯逻辑、无状态、独立可测。
// 由 Application::run 每帧末尾调用，产出下一次 wait_events 的超时；
// Window::run 消费该值在帧末阻塞等待，取代忙轮询（静态界面 CPU 100% 单核的根因）。

namespace aurora {

/**
 * @brief 计算帧循环下一次 `Surface::wait_events` 的超时（毫秒）。
 *
 * 返回值语义（与 `Surface::wait_events` 一致）：
 * - `< 0`：无限等待（无脏区、无动画、无定时任务——纯事件驱动，idle 零 CPU）；
 * - `== 0`：不等待（下一帧需立即渲染：有活但预算已耗尽 / 不限帧率 / 后端自带节拍）；
 * - `> 0`：等待该毫秒数（帧节流剩余预算 / 距最近定时任务到期的时间）。
 *
 * @param has_dirty        有绘制脏/布局脏（下一帧需要渲染）。
 * @param anim_active      有运行中动画（下一帧需要 tick + 渲染）。
 * @param next_deadline_ms Scheduler 最近到期任务的剩余毫秒；< 0 表示无定时任务。
 * @param frame_budget_ms  帧预算（1000/max_fps）；<= 0 表示不限帧率（活跃帧不节流）。
 * @param elapsed_ms       本帧已消耗毫秒（帧起点到决策点）。
 * @param backend_paced    后端自带帧节拍（如 D3D11 vsync）：活跃帧跳过 CPU 节流（阶段 B2）。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
[[nodiscard]] auto compute_wait_timeout(bool has_dirty, bool anim_active, double next_deadline_ms,
                                        double frame_budget_ms, double elapsed_ms, bool backend_paced = false)
    -> double;

} // namespace aurora
