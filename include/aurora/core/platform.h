#pragma once

/**
 * @file platform.h
 * @brief 编译期目标平台 / 架构 / 位宽探测宏（纯宏定义：零 #include、零运行时成本）。
 *
 * 全库（含消费者代码）做平台分支时统一使用本头的 `AURORA_*` 目标宏；
 * **禁止**在业务/库代码中直接书写 `_WIN32` / `__linux__` / `__APPLE__` 等原生宏
 * （例外：`third_party/` 三方源码、CMake 脚本、以及 `_WIN32_WINNT`/`_WIN32_IE` 这类
 * Windows SDK 版本旋钮——它们不是平台探测，而是 SDK 头的开关）。
 *
 * 五类目标宏（均为「编译器内建宏探测」，不经 CMake 注入，任何 TU 直接可用）：
 *  - `AURORA_PLATFORM_*` 平台家族：受支持的平台上**恰好一个**具体平台宏置 1，其余保持未定义；
 *    另有聚合宏 `AURORA_PLATFORM_UNIX`（unix-like 家族命中即置 1，可与具体平台宏同时为真）。
 *  - `AURORA_ARCH_*`     CPU 架构：已知架构上**恰好一个**置 1。
 *  - `AURORA_BIT_*`      位宽：`AURORA_BIT_64` / `AURORA_BIT_32` **恰好一个**置 1
 *    （由架构宏推导；未知架构回退到编译器指针宽度 `__SIZEOF_POINTER__`，再退 `_WIN64/_WIN32`）。
 *  - `AURORA_COMPILER_*` 编译器/工具链：基础宏 `AURORA_COMPILER_GCC` / `AURORA_COMPILER_CLANG` /
 *    `AURORA_COMPILER_MSVC` **恰好一个**置 1；派生精化宏 `AURORA_COMPILER_APPLE_CLANG` /
 *    `AURORA_COMPILER_CLANG_CL` / `AURORA_COMPILER_MINGW` / `AURORA_COMPILER_EMSCRIPTEN`
 *    可与对应 base 同时为真，用于细分工具链（Clang 家族可细分出 Apple / CL / Emscripten）。
 *  - `AURORA_CAP_*`      编译期能力：**恒定义**且取值为 0/1，回答「本 TU 可用什么能力」而非
 *    「目标是什么平台」；现仅 `AURORA_CAP_THREADS`（Emscripten 未开 `-pthread` 时为 0，其余目标为 1）。
 *
 * 原生宏 → Aurora 宏 映射表（判定顺序即下述先后，前缀命中优先）：
 *  | 原生宏                                        | Aurora 宏                    |
 *  |-----------------------------------------------|------------------------------|
 *  | `_WIN32`                                      | `AURORA_PLATFORM_WINDOWS`    |
 *  | `__APPLE__ && __MACH__`                       | `AURORA_PLATFORM_MACOS`      |
 *  | `__EMSCRIPTEN__`                              | `AURORA_PLATFORM_WASM`       |
 *  | `__ANDROID__`                                 | `AURORA_PLATFORM_ANDROID`    |
 *  | `__linux__`（扣除上述二者后）                  | `AURORA_PLATFORM_LINUX`      |
 *  | `__FreeBSD__`/`__NetBSD__`/`__OpenBSD__`/`__DragonFly__` | `AURORA_PLATFORM_BSD` |
 *  | `__unix__`/`__unix` 或以上任一 unix-like        | `AURORA_PLATFORM_UNIX`       |
 *  | `_M_X64`/`_M_AMD64`/`__x86_64__`/`__amd64__`  | `AURORA_ARCH_X64`            |
 *  | `_M_IX86`/`__i386__`                          | `AURORA_ARCH_X86`            |
 *  | `_M_ARM64`/`__aarch64__`                      | `AURORA_ARCH_AARCH64`        |
 *  | `_M_ARM`/`__arm__`                            | `AURORA_ARCH_ARM32`          |
 *  | `__riscv` 且 `__riscv_xlen == 64`             | `AURORA_ARCH_RISCV64`        |
 *  | `__EMSCRIPTEN__`/`__wasm__`                   | `AURORA_ARCH_WASM`           |
 *  | `__clang__`（任意 clang 变体）                  | `AURORA_COMPILER_CLANG`      |
 *  | `__clang__ && __apple_build_version__`         | `AURORA_COMPILER_APPLE_CLANG`|
 *  | `__clang__ && _MSC_VER`                        | `AURORA_COMPILER_CLANG_CL`   |
 *  | `__EMSCRIPTEN__`（clang 派生）                 | `AURORA_COMPILER_EMSCRIPTEN` |
 *  | `_MSC_VER`（非 clang）                          | `AURORA_COMPILER_MSVC`       |
 *  | `__GNUC__`（非 clang）                          | `AURORA_COMPILER_GCC`        |
 *  | `__GNUC__ && (__MINGW32__||__MINGW64__)`       | `AURORA_COMPILER_MINGW`      |
 *  | `__EMSCRIPTEN__` 且无 `__EMSCRIPTEN_PTHREADS__` | `AURORA_CAP_THREADS` = 0     |
 *
 * 判定顺序要点：
 *  - WASM 必须先于 Linux 判定（Emscripten 工具链基于 musl，会预定义 `__linux__`/`__unix__`）；
 *  - Android 必须先于 Linux 判定（`__ANDROID__` 蕴含 `__linux__`）；
 *  - macOS 先于其它 unix（`__APPLE__` 蕴含 Mach 内核标记）。
 *  - 编译器：Clang 必须先于 MSVC 判定（clang-cl 同时定义 `__clang__` 与 `_MSC_VER`），否则 clang-cl 会被误归为 MSVC。
 *
 * @note Thread: n/a（纯编译期）。用法示例：
 * @code
 *   #include "aurora/core/platform.h"
 *   #if defined(AURORA_PLATFORM_WINDOWS)
 *       // Win32 专属路径
 *   #elif defined(AURORA_PLATFORM_UNIX)
 *       // POSIX 通用路径
 *   #endif
 * @endcode
 */

// ─────────────────────────── AURORA_PLATFORM_*：平台家族 ───────────────────────────
// NOLINTBEGIN(*-macro-usage)
#ifdef _WIN32
#define AURORA_PLATFORM_WINDOWS 1U
#elif defined(__APPLE__) && defined(__MACH__)
#define AURORA_PLATFORM_MACOS 1
#elif defined(__EMSCRIPTEN__)
#define AURORA_PLATFORM_WASM 1
#elif defined(__ANDROID__)
#define AURORA_PLATFORM_ANDROID 1
#elif defined(__linux__)
#define AURORA_PLATFORM_LINUX 1
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define AURORA_PLATFORM_BSD 1
#endif

/// 聚合宏：unix-like 家族（macOS/Linux/Android/BSD）或任意 `__unix__` 命中时置 1；Windows 为假。
#if defined(AURORA_PLATFORM_MACOS) || defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID) || \
    defined(AURORA_PLATFORM_BSD)
#define AURORA_PLATFORM_UNIX 1
#elif defined(__unix__) || defined(__unix)
#define AURORA_PLATFORM_UNIX 1
#endif

// ─────────────────────────── AURORA_ARCH_*：CPU 架构 ───────────────────────────
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__amd64__)
#define AURORA_ARCH_X64 1
#elif defined(_M_IX86) || defined(__i386__)
#define AURORA_ARCH_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define AURORA_ARCH_AARCH64 1
#elif defined(_M_ARM) || defined(__arm__)
#define AURORA_ARCH_ARM32 1
#elif defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 64)
#define AURORA_ARCH_RISCV64 1
#elif defined(__EMSCRIPTEN__) || defined(__wasm__)
#define AURORA_ARCH_WASM 1
#endif

// ─────────────────────────── AURORA_BIT_*：位宽 ───────────────────────────
#if !defined(AURORA_BIT_64) && !defined(AURORA_BIT_32)
#if defined(AURORA_ARCH_X64) || defined(AURORA_ARCH_AARCH64) || defined(AURORA_ARCH_RISCV64)
#define AURORA_BIT_64 1
#elif defined(AURORA_ARCH_X86) || defined(AURORA_ARCH_ARM32)
#define AURORA_BIT_32 1
#elif defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ == 8)
#define AURORA_BIT_64 1
#elif defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ == 4)
#define AURORA_BIT_32 1
#elif defined(_WIN64)
#define AURORA_BIT_64 1
#elif defined(_WIN32)
#define AURORA_BIT_32 1
#endif
#endif

// ─────────────────────────── AURORA_COMPILER_*：编译器/工具链 ───────────────────────────
// 三类基础工具链互斥：受支持的编译器上**恰好一个** base 宏置 1
//   （AURORA_COMPILER_GCC / AURORA_COMPILER_CLANG / AURORA_COMPILER_MSVC）；
// 派生精化宏（APPLE_CLANG / CLANG_CL / MINGW / EMSCRIPTEN）可与对应 base 同时为真，用于细分。
// 探测顺序：Clang 优先于 MSVC（clang-cl 同时定义 __clang__ 与 _MSC_VER，须先归到 Clang 家族）；
//          其次 MSVC；最后 GCC（MinGW 属 GCC 家族，另行用 MINGW 标记）。
#ifdef __clang__
#define AURORA_COMPILER_CLANG 1
#if defined(__apple_build_version__)
#define AURORA_COMPILER_APPLE_CLANG 1
#elif defined(_MSC_VER)
#define AURORA_COMPILER_CLANG_CL 1
#endif
#if defined(__EMSCRIPTEN__)
#define AURORA_COMPILER_EMSCRIPTEN 1
#endif
#elif defined(_MSC_VER)
#define AURORA_COMPILER_MSVC 1
#elif defined(__GNUC__)
#define AURORA_COMPILER_GCC 1
#if defined(__MINGW32__) || defined(__MINGW64__)
#define AURORA_COMPILER_MINGW 1
#endif
#endif

// ─────────────────────────── AURORA_CAP_*：编译期能力 ───────────────────────────
// 与平台家族的区别：能力宏**恒定义**且取值 0/1，回答「本 TU 能用什么」，而非「目标是什么平台」。
//   AURORA_CAP_THREADS —— Emscripten 未开 `-pthread` 时 `std::thread` 构造直接抛 "Not supported"
//   （`__EMSCRIPTEN_PTHREADS__` 仅由 `-pthread` 定义，实测），该组合取 0；其余目标一律取 1。
//   判据是**编译期能力**而非运行期试探：开 `-pthread` 会让 wasm 产物要求 SharedArrayBuffer + COOP/COEP，
//   属产品级取舍，由构建方决定后在此如实反映。
#if defined(AURORA_PLATFORM_WASM) && !defined(__EMSCRIPTEN_PTHREADS__)
#define AURORA_CAP_THREADS 0
#else
#define AURORA_CAP_THREADS 1
#endif

// NOLINTEND(*-macro-usage)
