// tools/bench/bench_common.h — 基准工具公共函数（计时 / 浮点格式化）。
//
// 各 tools/bench_*.cpp 共享，避免每个文件重复定义一份。以 inline 形式提供（头文件内
// 定义），链接时无 ODR 问题。仅被基准工具使用，不进入 aurora 库体。
#pragma once

#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>

#include "aurora/aurora.h"

namespace aurora::bench {

// 计时：预热 warmup 次后取 iters 次平均（毫秒/次）。
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

// 按指定小数位格式化浮点（raw 通道不带格式控制，需手动控制精度以对齐原 printf）。
inline auto ffmt(int prec, double v) -> std::string {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}

// 基准产品输出统一免责声明（各 bench_*.cpp 末尾一致：非 CTest 断言，计时受环境抖动影响）。
inline constexpr auto AURORA_BENCH_DISCLAIMER =
    "(benchmark only — 非 CTest 断言；计时受环境抖动影响，仅作相对基线)";

// 打印一行「| 名称 | X.XXX ms |」形式的基准简表行（bench_win32_present 多行同构，bench_render 的
// 5 列含尺寸表为另一格式，故不共用）。统一后改表头/精度只需改这一处。
inline auto bench_row(const char *name, double ms) -> void {
    AURORA_LOG_RAW("bench", "| ", name, " | ", ffmt(3, ms), " ms |\n");
}

} // namespace aurora::bench
