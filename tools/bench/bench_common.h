// tools/bench/bench_common.h — shared benchmark helpers (timing / float formatting).
//
// Shared by the tools/bench_*.cpp files to avoid redefining them in each file. Provided as inline
// (defined in the header), so there is no ODR problem at link time. Used only by benchmark tools;
// it is not part of the aurora library body.
#pragma once

#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>

#include "aurora/aurora.h"

namespace aurora::bench {

// Timing: warm up warmup times, then average over iters runs (milliseconds per run).
inline auto time_ms(const std::function<void()> &fn, int warmup, int iters) -> double {
    for (int i = 0; i < warmup; ++i) {
        fn();
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        fn();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(iters);
}

// Format a float with the given number of decimals (the raw channel has no format control, so the
// precision is set manually to match the original printf output).
inline auto ffmt(int prec, double v) -> std::string {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}

// Unified disclaimer for benchmark product output (identical at the end of each bench_*.cpp:
// not a CTest assertion, timings are affected by environment jitter).
inline constexpr auto AURORA_BENCH_DISCLAIMER =
    "(benchmark only — not a CTest assertion; timings are affected by environment jitter, for relative baselines only)";

// Print one summary row of the form "| name | X.XXX ms |" (bench_win32_present has multiple rows of
// this shape; bench_render uses a different 5-column format including size, so it does not share it).
// After unification, changing the header/precision only needs to happen here.
inline auto bench_row(const char *name, double ms) -> void {
    AURORA_LOG_RAW("bench", "| ", name, " | ", ffmt(3, ms), " ms |\n");
}

}  // namespace aurora::bench
