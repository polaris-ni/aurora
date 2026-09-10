#pragma once

#include <aurora/core/log.h>

#include <cstdlib>

/**
 * @brief 硬检查（常开，对标 Chromium `CHECK` / Rust `assert!`）：条件不满足时写 FATAL 日志
 *        并 `std::abort()`，**所有构建配置（含 Release/NDEBUG）均生效**。
 *
 * 用于「继续执行必然未定义行为 / 状态不可恢复」的硬不变量（如错误态访问 `expected::value()`、
 * 空指针解引用），fail-fast 优于带病续跑。可恢复的运行时错误不在此列
 * （见 CODING_STANDARDS.md §1 错误处理，应返回 aurora::Result<T>）。
 */
#define AURORA_CHECK(cond, msg)                                         \
    do {                                                                \
        if (!(cond)) {                                                  \
            AURORA_LOG_FATAL("assert", "AURORA_CHECK failed: ", (msg)); \
            std::abort();                                               \
        }                                                               \
    } while (0)

/**
 * @brief 前置条件断言（debug-only，对标 Chromium `DCHECK` / C 标准 `assert`）：
 *        debug 构建下等价于 `AURORA_CHECK`；`NDEBUG`（Release）下**整体裁切、零开销**
 *        （条件不求值）。
 *
 * 用于参数契约、诊断性影子校验与逐像素级热路径检查（如 sRGB LUT 索引复核）。
 * 不用于可恢复的运行时错误（后者见 CODING_STANDARDS.md §1 错误处理，应返回
 * aurora::Result<T>）。
 */
#ifndef NDEBUG
#define AURORA_ASSERT(cond, msg) AURORA_CHECK(cond, msg)
#else
#define AURORA_ASSERT(cond, msg) ((void)(cond))
#endif
