#pragma once

#include <aurora/core/log.h>

#include <cstdlib>

/**
 * @brief 前置条件断言：debug 下触发并中断，release（NDEBUG）下编译掉。
 *
 * 用于检查编程错误 / 内部不变量，不用于可恢复的运行时错误
 * （后者见 CODING_STANDARDS.md §1 错误处理，应返回 aurora::Result<T>）。
 */
#define AURORA_ASSERT(cond, msg)                                         \
    do {                                                                 \
        if (!(cond)) {                                                   \
            AURORA_LOG_FATAL("assert", "AURORA_ASSERT failed: ", (msg)); \
            std::abort();                                                \
        }                                                                \
    } while (0)
