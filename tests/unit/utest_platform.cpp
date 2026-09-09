/// 测试类型: unit
/// 目标单元: include/aurora/core/platform.h
/// 测试说明: 覆盖平台/架构/位宽/编译器四类探测宏的「恰好一个」互斥性、位宽与指针宽度一致性、UNIX 聚合宏与 Windows
/// 的互斥契约、编译器 base 宏与精化宏的蕴含关系

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_platform {

AURORA_TEST_CASE(exactly_one_platform_family_macro_defined) {
    // 契约：受支持平台上 6 个具体平台宏恰好命中一个（聚合宏 AURORA_PLATFORM_UNIX 不计入）。
    int defined_count = 0;
#ifdef AURORA_PLATFORM_WINDOWS
    ++defined_count;
#endif
#ifdef AURORA_PLATFORM_MACOS
    ++defined_count;
#endif
#ifdef AURORA_PLATFORM_WASM
    ++defined_count;
#endif
#ifdef AURORA_PLATFORM_ANDROID
    ++defined_count;
#endif
#ifdef AURORA_PLATFORM_LINUX
    ++defined_count;
#endif
#ifdef AURORA_PLATFORM_BSD
    ++defined_count;
#endif
    AURORA_TEST_CHECK_EQ(defined_count, 1);
}

AURORA_TEST_CASE(exactly_one_arch_macro_defined) {
    // 契约：已知架构上 AURORA_ARCH_* 恰好命中一个。
    int defined_count = 0;
#ifdef AURORA_ARCH_X64
    ++defined_count;
#endif
#ifdef AURORA_ARCH_X86
    ++defined_count;
#endif
#ifdef AURORA_ARCH_AARCH64
    ++defined_count;
#endif
#ifdef AURORA_ARCH_ARM32
    ++defined_count;
#endif
#ifdef AURORA_ARCH_RISCV64
    ++defined_count;
#endif
#ifdef AURORA_ARCH_WASM
    ++defined_count;
#endif
    AURORA_TEST_CHECK_EQ(defined_count, 1);
}

AURORA_TEST_CASE(bit_width_matches_pointer_size_and_arch) {
    // 契约：AURORA_BIT_64 / AURORA_BIT_32 恰好一个置 1，且与指针宽度、64/32 位架构族一致。
#ifdef AURORA_BIT_64
    AURORA_TEST_CHECK_EQ(sizeof(void*), 8U);
#elif defined(AURORA_BIT_32)
    AURORA_TEST_CHECK_EQ(sizeof(void*), 4U);
#else
    AURORA_TEST_FAIL("AURORA_BIT_64 / AURORA_BIT_32 必须恰好定义一个");
#endif

#if defined(AURORA_ARCH_X64) || defined(AURORA_ARCH_AARCH64) || defined(AURORA_ARCH_RISCV64)
    constexpr bool arch_is_64 = true;
#elif defined(AURORA_ARCH_X86) || defined(AURORA_ARCH_ARM32)
    constexpr bool arch_is_64 = false;
#else
    constexpr bool arch_is_64 = (sizeof(void*) == 8U);  // WASM 等未知架构：按指针宽度对齐
#endif

#ifdef AURORA_BIT_64
    constexpr bool bit_is_64 = true;
#else
    constexpr bool bit_is_64 = false;
#endif
    AURORA_TEST_CHECK_EQ(arch_is_64, bit_is_64);
}

AURORA_TEST_CASE(unix_aggregate_follows_platform_family) {
    // 契约：unix-like 家族（macOS/Linux/Android/BSD）任一命中 ⇒ AURORA_PLATFORM_UNIX 必须为真。
#if defined(AURORA_PLATFORM_MACOS) || defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID) || \
    defined(AURORA_PLATFORM_BSD)
    constexpr bool family_hit = true;
#else
    constexpr bool family_hit = false;
#endif
#ifdef AURORA_PLATFORM_UNIX
    constexpr bool unix_defined = true;
#else
    constexpr bool unix_defined = false;
#endif
    AURORA_TEST_CHECK(!family_hit || unix_defined);
}

AURORA_TEST_CASE(windows_platform_excludes_unix_aggregate) {
#ifdef AURORA_PLATFORM_WINDOWS
    // Windows 目标：平台宏取值 1U，且 UNIX 聚合宏必须保持未定义。
    static_assert(AURORA_PLATFORM_WINDOWS == 1U);
#ifdef AURORA_PLATFORM_UNIX
    AURORA_TEST_FAIL("Windows 平台不应定义 AURORA_PLATFORM_UNIX");
#else
    AURORA_TEST_CHECK(true);
#endif
#else
    AURORA_TEST_SKIP("Windows 专属互斥契约，仅 Windows 目标可验");
#endif
}

AURORA_TEST_CASE(exactly_one_compiler_base_macro_defined) {
    // 契约：受支持编译器上 GCC / CLANG / MSVC 三个 base 宏恰好命中一个。
    int defined_count = 0;
#ifdef AURORA_COMPILER_GCC
    ++defined_count;
#endif
#ifdef AURORA_COMPILER_CLANG
    ++defined_count;
#endif
#ifdef AURORA_COMPILER_MSVC
    ++defined_count;
#endif
    AURORA_TEST_CHECK_EQ(defined_count, 1);
}

AURORA_TEST_CASE(compiler_refinement_macros_implied_by_base) {
    // 契约：派生精化宏必须与其 base 同时为真，不能凭空出现。
#ifdef AURORA_COMPILER_APPLE_CLANG
    static_assert(AURORA_COMPILER_CLANG == 1, "APPLE_CLANG implies CLANG");
#endif
#ifdef AURORA_COMPILER_CLANG_CL
    static_assert(AURORA_COMPILER_CLANG == 1, "CLANG_CL implies CLANG");
#endif
#ifdef AURORA_COMPILER_EMSCRIPTEN
    static_assert(AURORA_COMPILER_CLANG == 1, "EMSCRIPTEN implies CLANG");
#endif
#ifdef AURORA_COMPILER_MINGW
    static_assert(AURORA_COMPILER_GCC == 1, "MINGW implies GCC");
#endif
    AURORA_TEST_CHECK(true);
}

AURORA_TEST_CASE(compiler_macros_match_native_builtins) {
    // 契约：base 宏必须与原生内建宏方向一致，避免误判（如 clang-cl 归为 CLANG 而非 MSVC）。
#ifdef AURORA_COMPILER_CLANG
    static_assert(__clang__, "AURORA_COMPILER_CLANG 必须对应 __clang__");
#endif
#ifdef AURORA_COMPILER_MSVC
    static_assert(_MSC_VER, "AURORA_COMPILER_MSVC 必须对应 _MSC_VER");
#endif
#ifdef AURORA_COMPILER_GCC
    static_assert(__GNUC__, "AURORA_COMPILER_GCC 必须对应 __GNUC__");
#endif
    AURORA_TEST_CHECK(true);
}

}  // namespace aurora::test_cases::utest_platform
