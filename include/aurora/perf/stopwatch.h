#pragma once

/**
 * @file stopwatch.h
 * @brief 基础计时原语。
 *
 * 全库统一的单调时钟封装，替代散落各处的裸 `std::chrono::steady_clock` 用法，
 * 保证「口径一致 + 单位显式」。header-only、零依赖、可在热路径内联展开。
 */

#include <chrono>
#include <cstdint>

namespace aurora {

namespace detail {

/**
 * @brief 进程级性能时间原点（首次动态初始化时锁定）。
 *
 * 用 inline 变量而非函数内 static，避免热路径每次取时间戳都要检查线程安全初始化
 * 的 guard variable。仅供 `Stopwatch::now_ms()` 使用，不作为公共 API。
 */
inline const std::chrono::steady_clock::time_point AURORA_PERF_EPOCH = std::chrono::steady_clock::now();

}  // namespace detail

/**
 * @brief 单调秒表：构造即开始计时。
 *
 * 用于替换全库裸 `steady_clock` 计时片段，统一为 ms/us 两种显式单位读数。
 * 基于 `std::chrono::steady_clock`，不受系统时间调整影响。
 *
 * @code
 *   Stopwatch sw;
 *   do_work();
 *   const double cost = sw.elapsed_ms();
 *
 *   // 连续分段计时
 *   Stopwatch phase;
 *   layout();  const double layout_ms  = phase.lap_ms();
 *   paint();   const double paint_ms   = phase.lap_ms();
 * @endcode
 *
 * @note Thread: any（各实例独立，无共享状态）
 * @note Side-effects: none
 */
class Stopwatch {
  public:
    /// @brief 构造即开始计时。
    Stopwatch() noexcept : start_(Clock::now()) {}

    /// @brief 重新开始计时（丢弃已累计时长）。
    auto reset() noexcept -> void { start_ = Clock::now(); }

    /// @brief 自构造/上次 `reset()` 起经过的毫秒数。
    [[nodiscard]] auto elapsed_ms() const noexcept -> double {
        return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
    }

    /// @brief 自构造/上次 `reset()` 起经过的微秒数。
    [[nodiscard]] auto elapsed_us() const noexcept -> double {
        return std::chrono::duration<double, std::micro>(Clock::now() - start_).count();
    }

    /// @brief 自构造/上次 `reset()` 起经过的纳秒数（整数，用于需要精确累加的场合）。
    [[nodiscard]] auto elapsed_ns() const noexcept -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count();
    }

    /**
     * @brief 取当前读数并立即重置，用于连续分段计时。
     * @return 本段耗时（毫秒）。
     */
    auto lap_ms() noexcept -> double {
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - start_).count();
        start_ = now;
        return ms;
    }

    /**
     * @brief 进程内单调时间戳（毫秒，相对首次使用性能模块的时刻）。
     *
     * 供 `Profiler` / `TraceWriter` 标注事件发生时刻；同一进程内可比较、可相减，
     * 跨进程无意义。
     */
    [[nodiscard]] static auto now_ms() noexcept -> double {
        return std::chrono::duration<double, std::milli>(Clock::now() - detail::AURORA_PERF_EPOCH).count();
    }

  private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point start_;
};

}  // namespace aurora
