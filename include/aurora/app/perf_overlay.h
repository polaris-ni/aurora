#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/string_util.h"
#include "aurora/environment/media_query.h"
#include "aurora/perf/counters.h"
#include "aurora/perf/profiler.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 帧统计（ARCHITECTURE.md §10.1）：帧时间滑动窗口 + FPS 推导。
 *
 * `Application::run` 每帧调用 `record(dt)`；`PerfOverlay` / 工具读取。
 * 进程级单例（单线程 UI 模型下无需加锁）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class FrameStats {
  public:
    [[nodiscard]] static auto instance() -> FrameStats & {
        static FrameStats stats;
        return stats;
    }

    /// @brief 记录一帧耗时（秒）。**调用方仅在「本帧确实渲染了」时调用**（见 `WindowHost::render_frame`）。
    ///
    /// 若 dt 对应的毫秒值超过 AURORA_IDLE_THRESHOLD_MS，视为 idle 段：
    /// 按帧预算折算跳过帧数，仅递增 idle/total 计数器，不记入环形缓冲区。
    ///
    /// 无论走哪个分支都清空空闲累计（`idle_ms_`）：本帧渲染过了，`fps()` 即恢复「新鲜」。
    /// 注意 dt 偏大**不等于**应用空闲——它多半是「本帧本身很慢」（长任务、最小化后还原）：
    /// 那是掉帧/hitch 的语义，不是停帧。
    auto record(double dt_seconds) -> void {
        if (dt_seconds <= 0.0) {
            return;
        }
        idle_ms_ = 0.0;
        const double dt_ms = dt_seconds * 1000.0;
        // Idle 检测：帧间隔远超阈值 → 视为 idle 段（仅计计数器，不记入帧时间窗口）
        if (dt_ms > AURORA_IDLE_THRESHOLD_MS) {
            const auto skipped = static_cast<std::size_t>(dt_ms / frame_budget_ms_);
            if (skipped > 0) {
                idle_frames_ += skipped;
                total_frames_ += skipped;
                return;
            }
        }
        // 如果缓冲区已满，减去即将被覆盖的旧值
        if (count_ == AURORA_WINDOW_SIZE) {
            const double old = frames_.at(head_);
            sum_ -= old;
            sum_sq_ -= old * old;
        }
        frames_.at(head_) = dt_seconds;
        sum_ += dt_seconds;
        sum_sq_ += dt_seconds * dt_seconds;
        head_ = (head_ + 1) % AURORA_WINDOW_SIZE;
        if (count_ < AURORA_WINDOW_SIZE) {
            ++count_;
        }
        ++total_frames_;
        // 掉帧/hitch 检测
        if (dt_ms > frame_budget_ms_) {
            ++dropped_;
            if (dt_ms > frame_budget_ms_ * 2.0) {
                ++hitch_;
            }
        }
    }

    /// @brief 记录一次 idle 跳帧（不影响帧时间窗口，只计计数器并累加空闲时长）。
    ///
    /// @param dt_seconds 本帧的墙钟间隔（秒）。帧循环在 idle 帧上同样消耗了一段等待时间，
    ///        这段时长就是「距上一次实际渲染已过去多久」的增量；不传则只计计数器，
    ///        `stale_duration_ms()` 不增长（老调用点的零成本兼容路径）。
    auto record_idle(double dt_seconds = 0.0) -> void {
        ++idle_frames_;
        ++total_frames_;
        if (dt_seconds > 0.0) {
            idle_ms_ += dt_seconds * 1000.0;
        }
    }

    /// @brief 滑动窗口平均 FPS（无数据返回 0）。
    ///
    /// 该值是**窗口内已记录帧**的均值，不做时间衰减：渲染一旦停止，环形缓冲不再有新样本，
    /// 返回值便停留在最后一次活跃 burst 上。调用方须用 `is_stale()` 判断它是否还代表当前帧率
    /// （HUD 显示「421.0 FPS」而实际已停帧数秒，是误导而非信息）。
    [[nodiscard]] auto fps() const -> double {
        if (count_ == 0) {
            return 0.0;
        }
        return sum_ > 0.0 ? static_cast<double>(count_) / sum_ : 0.0;
    }

    /// @brief 窗口内 FPS 是否已陈旧：距最近一次**实际渲染**的帧已累计空闲 ≥ AURORA_FPS_STALE_MS。
    ///
    /// 判据基于空闲**时长**而非帧数——空闲段里帧循环可能以任意间隔被唤醒（事件、定时器、
    /// HUD 刷新），用帧数衡量会把「一帧都没有」误判成「刚过了几帧」。
    [[nodiscard]] auto is_stale() const -> bool { return idle_ms_ >= AURORA_FPS_STALE_MS; }

    /// @brief 空闲累计时长（毫秒）：自最近一次实际渲染的帧起累计的无帧时长。
    ///
    /// 供 HUD 展示「421.0 FPS（stale 3.2s）」这类带上下文的读数——归零会丢掉「上次活跃帧率」
    /// 这一排障信息。
    [[nodiscard]] auto stale_duration_ms() const -> double { return idle_ms_; }

    /// @brief FPS 陈旧判定阈值（毫秒）：累计空闲达到此值后 `is_stale()` 为 true。
    ///
    /// 取值的两个约束：**下界**须明显大于一帧预算，否则动画中偶发跳过的单帧就会被判成停帧、
    /// HUD 数字无谓地闪烁；**上界**须接近 HUD 刷新周期，否则明显停帧后还会继续报告「新鲜」的
    /// 旧值。本值与 `Window::AURORA_HUD_REFRESH_MS`（HUD 叠加层刷新周期）同量级——两者是
    /// **独立的策略**：前者是「多久算停帧」，后者是「多久重绘一次叠加层」，取值接近只是
    /// 因为同一个开发者可感知的时间尺度约在 0.5 秒量级。
    static constexpr double AURORA_FPS_STALE_MS = 500.0;

    /// @brief 滑动窗口平均帧时间（毫秒）。
    [[nodiscard]] auto avg_frame_ms() const -> double {
        if (count_ == 0) {
            return 0.0;
        }
        return sum_ / static_cast<double>(count_) * 1000.0;
    }

    /// @brief 窗口内最差帧时间（毫秒）。
    [[nodiscard]] auto worst_frame_ms() const -> double {
        if (count_ == 0) {
            return 0.0;
        }
        double worst = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            const std::size_t idx = (head_ + AURORA_WINDOW_SIZE - 1 - i) % AURORA_WINDOW_SIZE;
            worst = std::max(worst, frames_.at(idx));
        }
        return worst * 1000.0;
    }

    /// @brief 帧时间标准差（毫秒）。
    [[nodiscard]] auto jitter_ms() const -> double {
        if (count_ < 2) {
            return 0.0;
        }
        const double mean = sum_ / static_cast<double>(count_);
        const double var = (sum_sq_ / static_cast<double>(count_)) - (mean * mean);
        return var > 0.0 ? std::sqrt(var) * 1000.0 : 0.0;
    }

    /// @brief 百分位帧时间（毫秒），p 范围 [0,1]。
    [[nodiscard]] auto percentile_ms(double p) const -> double {
        if (count_ == 0) {
            return 0.0;
        }
        std::array<double, AURORA_WINDOW_SIZE> sorted{};
        const std::size_t n = count_;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx = (head_ + AURORA_WINDOW_SIZE - n + i) % AURORA_WINDOW_SIZE;
            sorted.at(i) = frames_.at(idx);
        }
        std::sort(sorted.begin(), std::next(sorted.begin(), static_cast<std::ptrdiff_t>(n)));
        const double idx = p * static_cast<double>(n - 1);
        const auto lo = static_cast<std::size_t>(idx);
        const std::size_t hi = lo + 1;
        if (hi >= n) {
            return sorted.at(n - 1) * 1000.0;
        }
        const double frac = idx - static_cast<double>(lo);
        return ((sorted.at(lo) * (1.0 - frac)) + (sorted.at(hi) * frac)) * 1000.0;
    }

    /// @brief 获取窗口内第 i 帧的帧时间（秒），i=0 为最新帧。
    [[nodiscard]] auto frame_at(std::size_t i) const -> double {
        if (i >= count_) {
            return 0.0;
        }
        const std::size_t idx = (head_ + AURORA_WINDOW_SIZE - 1 - i) % AURORA_WINDOW_SIZE;
        return frames_.at(idx);
    }

    /// @brief 有效帧数。
    [[nodiscard]] auto window_size() const -> std::size_t { return count_; }

    [[nodiscard]] auto dropped_frame_count() const -> std::size_t { return dropped_; }
    [[nodiscard]] auto dropped_frame_ratio() const -> double {
        return total_frames_ > 0 ? static_cast<double>(dropped_) / static_cast<double>(total_frames_) : 0.0;
    }
    [[nodiscard]] auto hitch_count() const -> std::size_t { return hitch_; }
    [[nodiscard]] auto idle_frame_count() const -> std::size_t { return idle_frames_; }

    auto set_frame_budget_ms(double ms) -> void { frame_budget_ms_ = ms; }
    [[nodiscard]] auto frame_budget_ms() const -> double { return frame_budget_ms_; }

    /// @brief 记录分阶段计时（毫秒）。
    auto record_phases(double layout_ms, double paint_ms, double present_ms) -> void {
        if (phase_count_ == AURORA_PHASE_WINDOW) {
            layout_sum_ -= layout_ms_.at(phase_head_);
            paint_sum_ -= paint_ms_.at(phase_head_);
            present_sum_ -= present_ms_.at(phase_head_);
        }
        layout_ms_.at(phase_head_) = layout_ms;
        paint_ms_.at(phase_head_) = paint_ms;
        present_ms_.at(phase_head_) = present_ms;
        layout_sum_ += layout_ms;
        paint_sum_ += paint_ms;
        present_sum_ += present_ms;
        // 近零相位时长（headless 下 layout/paint 耗时≈0）在环形缓冲累加时，可能因 FP 舍入使
        // `sum - old_head + new` 跨过 0 边界变成 -1e-16（打印为 -0.000）。相位时长物理上非负
        // （来自单调时钟差值），钳回非负守住 avg_*() >= 0 的不变量——影响 perf_overlay / perf_log
        // 显示与 test_perf_frame_loop 断言。此处为数值健壮性兜底，不改变任何正时长语义。
        layout_sum_ = layout_sum_ < 0.0 ? 0.0 : layout_sum_;
        paint_sum_ = paint_sum_ < 0.0 ? 0.0 : paint_sum_;
        present_sum_ = present_sum_ < 0.0 ? 0.0 : present_sum_;
        phase_head_ = (phase_head_ + 1) % AURORA_PHASE_WINDOW;
        if (phase_count_ < AURORA_PHASE_WINDOW) {
            ++phase_count_;
        }
    }

    [[nodiscard]] auto avg_layout_ms() const -> double {
        return phase_count_ > 0 ? layout_sum_ / static_cast<double>(phase_count_) : 0.0;
    }
    [[nodiscard]] auto avg_paint_ms() const -> double {
        return phase_count_ > 0 ? paint_sum_ / static_cast<double>(phase_count_) : 0.0;
    }
    [[nodiscard]] auto avg_present_ms() const -> double {
        return phase_count_ > 0 ? present_sum_ / static_cast<double>(phase_count_) : 0.0;
    }

    /// @brief 累计帧数。
    [[nodiscard]] auto total_frames() const -> std::size_t { return total_frames_; }

    // ---- 帧循环唤醒/睡眠观测 ----

    /// @brief 记录一次帧循环等待（`Window::run` 每次 `wait_events` 返回后调用）。
    auto record_wait(double waited_ms) -> void {
        const auto now = std::chrono::steady_clock::now();
        if (!wait_epoch_set_) {
            wait_epoch_ = now;
            wait_epoch_set_ = true;
        }
        last_wait_end_ = now;
        ++wakeups_;
        wait_total_ms_ += waited_ms;
    }

    /// @brief 累计唤醒次数（wait_events 返回次数；忙轮询/未接等待时为 0）。
    [[nodiscard]] auto wakeup_count() const -> std::size_t { return wakeups_; }

    /// @brief 每秒唤醒次数（自首次等待起的墙钟均值；无数据返回 0）。
    [[nodiscard]] auto wakeups_per_sec() const -> double {
        if (!wait_epoch_set_) {
            return 0.0;
        }
        const double s = std::chrono::duration<double>(last_wait_end_ - wait_epoch_).count();
        return s > 0.0 ? static_cast<double>(wakeups_) / s : 0.0;
    }

    /// @brief 睡眠占比（等待总时长 / 墙钟总时长，[0,1]；靠近 1 = 几乎全程睡眠）。
    [[nodiscard]] auto sleep_ratio() const -> double {
        if (!wait_epoch_set_) {
            return 0.0;
        }
        const double wall_ms = std::chrono::duration<double, std::milli>(last_wait_end_ - wait_epoch_).count();
        return wall_ms > 0.0 ? std::min(1.0, wait_total_ms_ / wall_ms) : 0.0;
    }

    /// @brief 清空（测试用）。
    auto reset() -> void {
        frames_.fill(0.0);
        head_ = 0;
        count_ = 0;
        sum_ = 0.0;
        sum_sq_ = 0.0;
        max_ = 0.0;
        total_frames_ = 0;
        dropped_ = 0;
        hitch_ = 0;
        idle_frames_ = 0;
        idle_ms_ = 0.0;
        frame_budget_ms_ = 16.67;
        layout_ms_.fill(0.0);
        paint_ms_.fill(0.0);
        present_ms_.fill(0.0);
        phase_head_ = 0;
        phase_count_ = 0;
        layout_sum_ = 0;
        paint_sum_ = 0;
        present_sum_ = 0;
        wakeups_ = 0;
        wait_total_ms_ = 0.0;
        wait_epoch_set_ = false;
    }

  private:
    static constexpr std::size_t AURORA_WINDOW_SIZE = 128;  ///< 环形缓冲区帧数（约 2 秒 @60Fps）

    std::array<double, AURORA_WINDOW_SIZE> frames_{};
    std::size_t head_ = 0;  ///< 下一个写入位置
    std::size_t count_ = 0;  ///< 当前有效帧数（<= AURORA_WINDOW_SIZE）
    std::size_t total_frames_ = 0;

    double sum_ = 0.0;  ///< 窗口总和
    double sum_sq_ = 0.0;  ///< 窗口平方和（用于 jitter）
    double max_ = 0.0;  ///< 窗口最大值
    std::size_t dropped_ = 0;  ///< 累计掉帧数
    std::size_t hitch_ = 0;  ///< 累计 hitch 数
    std::size_t idle_frames_ = 0;  ///< 累计 idle 跳过帧数
    double idle_ms_ = 0.0;  ///< 距最近一次实际渲染帧的累计空闲时长（毫秒）；`record` 清零、`record_idle` 累加
    double frame_budget_ms_ = 16.67;  ///< 帧预算目标

    static constexpr double AURORA_IDLE_THRESHOLD_MS = 100.0;  ///< idle 检测阈值（毫秒）

    static constexpr std::size_t AURORA_PHASE_WINDOW = 64;
    std::array<double, AURORA_PHASE_WINDOW> layout_ms_{};
    std::array<double, AURORA_PHASE_WINDOW> paint_ms_{};
    std::array<double, AURORA_PHASE_WINDOW> present_ms_{};
    std::size_t phase_head_ = 0;
    std::size_t phase_count_ = 0;
    double layout_sum_ = 0;
    double paint_sum_ = 0;
    double present_sum_ = 0;

    // 帧循环唤醒/睡眠观测
    std::size_t wakeups_ = 0;  ///< 累计唤醒次数（wait_events 返回计数）。
    double wait_total_ms_ = 0.0;  ///< 累计等待时长（毫秒）。
    bool wait_epoch_set_ = false;  ///< 是否已记录首次等待（墙钟基准有效）。
    std::chrono::steady_clock::time_point wait_epoch_;  ///< 首次等待时刻（墙钟基准）。
    std::chrono::steady_clock::time_point last_wait_end_;  ///< 最近一次等待结束时刻。
};

/**
 * @brief 性能覆盖层（ARCHITECTURE.md §10.2）：包裹内容并在角落叠加 FPS/帧时间统计。
 *
 * `set_visible(false)` 关闭显示（内容不受影响）。数据源 `FrameStats::instance()`。
 * 对标 Flutter Performance Overlay。
 *
 * @note Thread: main-thread only
 * @note Side-effects: paints
 * @note Rebuildable: yes, via from_json
 */
class PerfOverlay : public SingleChild {
  public:
    PerfOverlay() = default;
    explicit PerfOverlay(Node content) : SingleChild(std::move(content)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "PerfOverlay"; }

    /// @brief 禁止 DisplayList 缓存：PerfOverlay 的文本（实时 FPS/P99/jitter/唤醒率等）
    /// 每帧由 `on_paint` 从 `FrameStats::instance()` 重读，属于「内容每帧变动」控件。
    /// 若允许缓存，首帧（尚未有统计样本，显示「采样中」）会被录进 DL，之后 `Widget::paint`
    /// 在 bounds 不变时直接 replay 首帧缓存、再不调用 `on_paint`，导致 HUD 冻结在「采样中」、
    /// 永不刷新——这正是 demo 中 PerfOverlay 面板静止全零的根因。由 Window 的 HUD 层以 2Hz
    /// 重绘本控件的离屏缓冲并合成（见 window.h present_root 的 HUD 合成段）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "PerfOverlay",
            .properties =
                {
                    {.name = "visible",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "是否显示统计",
                     .json_type = "boolean"},
                    {.name = "show_counters",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "是否显示渲染计数器与长任务行",
                     .json_type = "boolean"},
                },
            .events = {},
            .children_policy = "single",
            .examples = {"au::PerfOverlay(root)"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto set_visible(bool v) -> PerfOverlay & {
        visible_ = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto visible() const -> bool { return visible_; }

    /// @brief 是否显示渲染计数器与长任务行。
    auto set_show_counters(bool v) -> PerfOverlay & {
        show_counters_ = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto show_counters() const -> bool { return show_counters_; }

    /// @brief 绑定本面板读取的帧统计实例（多窗口：绑到宿主窗口自己的统计）。
    ///
    /// 未绑定时回退 `FrameStats::instance()`（进程级单例）——历史行为，单窗口用法零变化。
    /// 多窗口下各 `WindowHost` 有独立统计，不绑定会读到混合/空数据。
    auto bind_frame_stats(const FrameStats *s) -> PerfOverlay & {
        bound_stats_ = s;
        mark_needs_paint();
        return *this;
    }
    /// @brief 当前绑定的帧统计实例（未绑定返回 nullptr，表示回退全局单例）。
    [[nodiscard]] auto bound_frame_stats() const -> const FrameStats * { return bound_stats_; }

    /// @brief 第一行统计文本：FPS + P99 + jitter（数据源：进程级单例 `FrameStats::instance()`）。
    [[nodiscard]] static auto stats_line1() -> std::string { return stats_line1(FrameStats::instance()); }
    /// @brief 同上，数据源显式给定（多窗口 / 离线统计场景）。
    [[nodiscard]] static auto stats_line1(const FrameStats &s) -> std::string {
        // 样本不足（<2 帧）时除零会得到 9765.6 这类假 FPS，直接显示 — 而非误导数字。
        if (s.window_size() < 2) {
            return "FPS — (采样中) | P99 — | jitter —";
        }
        std::string line = aurora::internal::string_format("FPS %.1f (avg %.1f) | P99 %.1fms | jitter %.1fms", s.fps(),
                                                           s.avg_frame_ms(), s.percentile_ms(0.99), s.jitter_ms());
        // 已停帧：数值仍取窗口内最后一次活跃 burst 的均值（不归零，保留排障信息），
        // 但追加空闲时长标注，避免把冻结值当成本刻帧率读。
        if (s.is_stale()) {
            line += aurora::internal::string_format(" | stale %.1fs", s.stale_duration_ms() / 1000.0);
        }
        return line;
    }

    /// @brief 第二行统计文本：dropped + hitch + idle（数据源：进程级单例）。
    [[nodiscard]] static auto stats_line2() -> std::string { return stats_line2(FrameStats::instance()); }
    /// @brief 同上，数据源显式给定。
    [[nodiscard]] static auto stats_line2(const FrameStats &s) -> std::string {
        return aurora::internal::string_format("dropped: %zu | hitch: %zu | idle: %zu", s.dropped_frame_count(),
                                               s.hitch_count(), s.idle_frame_count());
    }

    /// @brief 第三行统计文本：唤醒频率 + 睡眠占比（数据源：进程级单例）。
    [[nodiscard]] static auto stats_line3() -> std::string { return stats_line3(FrameStats::instance()); }
    /// @brief 同上，数据源显式给定。
    [[nodiscard]] static auto stats_line3(const FrameStats &s) -> std::string {
        return aurora::internal::string_format("wakeups/s: %.1f | sleep: %.0F%%", s.wakeups_per_sec(),
                                               s.sleep_ratio() * 100.0);
    }

    /**
     * @brief 第四行统计文本：渲染计数器（树遍历规模 + DisplayList 复用率）。
     *
     * 数据源 `RenderCounters::current()`（当帧快照）。`AURORA_ENABLE_PROFILING`
     * 关闭时计数恒为 0，此时直接显示状态提示而非一排误导性的零。
     */
    [[nodiscard]] static auto stats_line4() -> std::string {
        if constexpr (!profiling_enabled()) {
            return "counters: profiling off";
        } else {
            const RenderCounters &c = RenderCounters::current();
            return aurora::internal::string_format("nodes L%u/P%u | DL rec%u/rep%u | glyph %u", c.layout_nodes,
                                                   c.paint_nodes, c.dl_records, c.dl_replays, c.glyphs_rendered);
        }
    }

    /// @brief 第五行统计文本：脏区效率 + 长任务累计（脏区效率的主验收指标）。
    [[nodiscard]] static auto stats_line5() -> std::string {
        if constexpr (!profiling_enabled()) {
            return "dirty/long-task: profiling off";
        } else {
            const RenderCounters &c = RenderCounters::current();
            return aurora::internal::string_format(
                "dirty %u (%.0F%%)%s | long %llu", c.dirty_rect_count, c.dirty_area_ratio * 100.0,
                c.full_redraw ? " FULL" : "",
                static_cast<unsigned long long>(Profiler::instance().total_long_task_count()));
        }
    }

    /// @brief 计数器行的告警颜色：发生整帧重绘时转红（需要避免的场景）。
    [[nodiscard]] static auto counters_color() -> Color {
        if constexpr (!profiling_enabled()) {
            return {140, 140, 140, 255};  // 灰色 — 数据不可用
        } else {
            return RenderCounters::current().full_redraw ? Color(255, 60, 60, 255) : Color(200, 200, 200, 255);
        }
    }

    /// @brief 根据 FPS 值返回告警颜色。
    [[nodiscard]] static auto fps_color(double fps) -> Color {
        if (fps >= 55.0) {
            return {0, 255, 128, 255};  // 绿色 — 流畅
        }
        if (fps >= 30.0) {
            return {255, 200, 0, 255};  // 黄色 — 卡顿警告
        }
        return {255, 60, 60, 255};  // 红色 — 严重卡顿
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 320.0F, .height = 240.0F};
        }
        if (child_) {
            const Constraints inner{.min = Size{.width = 0.0F, .height = 0.0F}, .max = self};
            child_.widget().layout(inner, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self});
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, bounds, ctx);
        }
        if (!visible_) {
            return;
        }

        // 数据源：优先本面板绑定的实例（多窗口每窗口独立），未绑定回退进程级单例。
        const FrameStats &s = bound_stats_ != nullptr ? *bound_stats_ : FrameStats::instance();
        Font f;
        f.size_pt = 10.0F;

        // 面板尺寸（需容纳 5 行文本 + 条形图，每行不换行）
        constexpr float w = 440.0F;
        constexpr float line_h = 18.0F;  // 单行文本高度（10pt 字体需 ≥18px 避免截断/错位）
        constexpr float graph_h = 48.0F;  // 条形图高度
        constexpr float pad = 6.0F;
        // 基础三行 + 可选的计数器/脏区两行
        const int line_count = show_counters_ ? 5 : 3;
        const float text_total_h = line_h * static_cast<float>(line_count);
        const float total_h = text_total_h + graph_h + (pad * 2);

        // 面板位置（右上角）：Y 额外下沉安全区顶部（Wayland CSD 标题栏高度，
        // 经 content_inset → MediaQuery.padding 注入；无装饰后端为 0），避免被标题栏遮挡。
        const EdgeInsets safe = MediaQuery::of(ctx).padding;
        const float x = bounds.origin.x + bounds.size.width - w - 8.0F;
        const float y = bounds.origin.y + 8.0F + safe.top;
        const Rect box{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = total_h}};

        // 背景（不透明）：作为独立 HUD 层合成时需叠在「保留自上一帧」的主缓冲之上，
        // 透明面板会在 partial-clip 帧与旧 HUD 像素二次混合产生重影，故此处必须为不透明。
        p.fill_rect(box, Color(0, 0, 0, 255));

        // 逐行绘制：行号自增，避免新增行时手工累加各行高度出错
        int line_index = 0;
        const auto draw_line = [&](const std::string &text, const Color &color) -> void {
            const float ly = y + pad + (line_h * static_cast<float>(line_index++));
            p.draw_text(
                Rect{.origin = Point{.x = x + pad, .y = ly}, .size = Size{.width = w - (pad * 2), .height = line_h}},
                text, f, color);
        };

        // 第一行：FPS + P99 + jitter（颜色告警；已停帧时灰化——冻结值不再代表当前帧率，
        // 继续按「绿 = 流畅」上色会给出与事实相反的观感）。统计行一律显式传 `s`：
        // 无参重载读的是进程级单例，多窗口下会与本面板 `bound_stats_` 不一致。
        draw_line(stats_line1(s), s.is_stale() ? Color(140, 140, 140, 255) : fps_color(s.fps()));
        // 第二行：dropped + hitch + idle（浅灰文本）
        draw_line(stats_line2(s), Color(200, 200, 200, 255));
        // 第三行：唤醒频率 + 睡眠占比（事件驱动帧循环观测）
        draw_line(stats_line3(s), Color(200, 200, 200, 255));
        if (show_counters_) {
            // 第四 / 五行：渲染计数器与脏区效率
            draw_line(stats_line4(), counters_color());
            draw_line(stats_line5(), counters_color());
        }

        // 帧时间条形图
        const float graph_x = x + pad;
        const float graph_y = y + pad + text_total_h;
        constexpr float graph_w = w - (pad * 2);
        const double budget_ms = s.frame_budget_ms();
        constexpr std::size_t max_bars = 64;
        const std::size_t bar_count = std::min(s.window_size(), max_bars);
        constexpr float bar_w = graph_w / static_cast<float>(max_bars);

        for (std::size_t i = 0; i < bar_count; ++i) {
            const double frame_ms = s.frame_at(i) * 1000.0;  // frame_at 返回秒
            float bar_h = static_cast<float>(frame_ms / budget_ms) * graph_h;
            bar_h = std::min(bar_h, graph_h);
            const float bx = graph_x + (static_cast<float>(i) * bar_w);
            const float by = graph_y + graph_h - bar_h;
            const Color bar_color = (frame_ms > budget_ms) ? Color(255, 60, 60, 200) : Color(0, 255, 128, 200);
            p.fill_rect(Rect{.origin = Point{.x = bx, .y = by},
                             .size = Size{.width = std::max(bar_w - 1.0F, 1.0F), .height = bar_h}},
                        bar_color);
        }
        // 注：2Hz 低频刷新不再由本控件 self-mark 驱动（否则作为根控件会标脏整屏触发整树重绘，
        // 约 ~24ms，是 P99 尖峰来源）。改为由 Window 的 HUD 层在 present_root 中以 2Hz 重绘
        // 本控件的离屏缓冲并合成，app 树仅在其自身脏时重绘——见 window.h present_root 的 HUD 合成段。
    }

  private:
    bool visible_ = true;
    bool show_counters_ = true;
    const FrameStats *bound_stats_ = nullptr;  ///< 面板数据源绑定（nullptr = 回退 `FrameStats::instance()`）。
};

}  // namespace aurora
