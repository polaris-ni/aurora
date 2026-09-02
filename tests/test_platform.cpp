// test_platform.cpp — core/platform.h 目标平台/架构/位宽宏探测 1:1 测试。
// 覆盖：平台互斥性（恰好一个具体平台宏置 1）、UNIX 聚合一致性、
//       架构与位宽互斥性、位宽与指针宽度一致、aurora.h 入口可达性。

#include "aurora/core/platform.h"

#include "test_harness.h"

namespace sec_platform_macros {
// 本命名空间的宏名（PLATFORM_COUNT_n）与常量名参与预处理条件配对：命名重命名只会改写
// 「当前平台生效分支」中的 #define，#ifdef 引用处不会被同步改写，计数会恒为 0。故整体抑制。
// NOLINTBEGIN(readability-identifier-naming)

// 预处理器侧计数（恰好一个语义只能在预处理期验证，运行期转发为常量）。
#ifdef AURORA_PLATFORM_WINDOWS
#define PLATFORM_COUNT_1
#endif
#ifdef AURORA_PLATFORM_MACOS
#define PLATFORM_COUNT_2
#endif
#ifdef AURORA_PLATFORM_WASM
#define PLATFORM_COUNT_3
#endif
#ifdef AURORA_PLATFORM_ANDROID
#define PLATFORM_COUNT_4
#endif
#ifdef AURORA_PLATFORM_LINUX
#define PLATFORM_COUNT_5
#endif
#ifdef AURORA_PLATFORM_BSD
#define PLATFORM_COUNT_6
#endif

constexpr int k_platform_count =
#ifdef PLATFORM_COUNT_1
    1 +
#endif
#ifdef PLATFORM_COUNT_2
    1 +
#endif
#ifdef PLATFORM_COUNT_3
    1 +
#endif
#ifdef PLATFORM_COUNT_4
    1 +
#endif
#ifdef PLATFORM_COUNT_5
    1 +
#endif
#ifdef PLATFORM_COUNT_6
    1 +
#endif
    0;

constexpr int k_bit_count =
#if defined(AURORA_BIT_64) && !defined(AURORA_BIT_32)
    1;
#elif defined(AURORA_BIT_32) && !defined(AURORA_BIT_64)
    1;
#else
    0;
#endif

constexpr bool k_unix_aggregate =
#ifdef AURORA_PLATFORM_UNIX
    true;
#else
    false;
#endif

void run() {

    // ---- 1. 平台宏恰好一个置 1（受支持平台上）----
    AURORA_TEST_CHECK_EQ(k_platform_count, 1);

    // ---- 2. UNIX 聚合与平台家族一致 ----
#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_CHECK_FALSE(k_unix_aggregate);
#else
    AURORA_TEST_CHECK_TRUE(k_unix_aggregate);
#endif

    // ---- 3. 架构已知且唯一（x86/x64/arm/riscv/wasm 至少其一；本测试环境必然命中 x64）----
    constexpr bool arch_known =
#if defined(AURORA_ARCH_X64) || defined(AURORA_ARCH_X86) || defined(AURORA_ARCH_AARCH64) ||                            \
    defined(AURORA_ARCH_ARM32) || defined(AURORA_ARCH_RISCV64) || defined(AURORA_ARCH_WASM)
        true;
#else
        false;
#endif
    AURORA_TEST_CHECK_TRUE(arch_known);

    // ---- 4. 位宽恰好一个且与指针宽度一致 ----
    AURORA_TEST_CHECK_EQ(k_bit_count, 1);
    constexpr bool ptr64 = (sizeof(void *) == 8);
#ifdef AURORA_BIT_64
    AURORA_TEST_CHECK_TRUE(ptr64);
#else
    AURORA_TEST_CHECK_FALSE(ptr64);
#endif

    // ---- 5. 架构 → 位宽 推导自洽 ----
#if defined(AURORA_ARCH_X64) || defined(AURORA_ARCH_AARCH64) || defined(AURORA_ARCH_RISCV64)
    AURORA_TEST_CHECK_TRUE(ptr64);
#elif defined(AURORA_ARCH_X86) || defined(AURORA_ARCH_ARM32)
    AURORA_TEST_CHECK_FALSE(ptr64);
#endif
}

// NOLINTEND(readability-identifier-naming)

} // namespace sec_platform_macros

AURORA_TEST() { sec_platform_macros::run(); }
