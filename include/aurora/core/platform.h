#pragma once

/// @file platform.h
/// @brief 编译期目标平台 / 架构 / 位宽探测宏（纯宏定义：零 `#include`、零运行时成本）。
///
/// 全库（含消费者代码）做平台分支时统一使用本头的 `AURORA_*` 目标宏；
/// **禁止**在业务/库代码中直接书写 `_WIN32` / `__linux__` / `__APPLE__` 等原生宏
/// （例外：`third_party/` 三方源码、CMake 脚本、以及 `_WIN32_WINNT`/`_WIN32_IE` 这类
/// Windows SDK 版本旋钮——它们不是平台探测，而是 SDK 头的开关）。
///
/// 五类目标宏（均为「编译器内建宏探测」，不经 CMake 注入，任何 TU 直接可用）：
/// - `AURORA_PLATFORM_*` 平台家族：受支持的平台上**恰好一个**具体平台宏置 1，其余保持未定义；
/// 另有聚合宏 `AURORA_PLATFORM_UNIX`（unix-like 家族命中即置 1，可与具体平台宏同时为真）。
/// - `AURORA_ARCH_*`     CPU 架构：已知架构上**恰好一个**置 1。
/// - `AURORA_BIT_*`      位宽：`AURORA_BIT_64` / `AURORA_BIT_32` **恰好一个**置 1
/// （由架构宏推导；未知架构回退到编译器指针宽度 `__SIZEOF_POINTER__`，再退 `_WIN64/_WIN32`）。
/// - `AURORA_COMPILER_*` 编译器/工具链：基础宏 `AURORA_COMPILER_GCC` / `AURORA_COMPILER_CLANG` /
/// `AURORA_COMPILER_MSVC` **恰好一个**置 1；派生精化宏 `AURORA_COMPILER_APPLE_CLANG` /
/// `AURORA_COMPILER_CLANG_CL` / `AURORA_COMPILER_MINGW` / `AURORA_COMPILER_EMSCRIPTEN`
/// 可与对应 base 同时为真，用于细分工具链（Clang 家族可细分出 Apple / CL / Emscripten）。
/// - `AURORA_CAP_*`      编译期能力：**恒定义**且取值为 0/1，回答「本 TU 可用什么能力」而非
/// 「目标是什么平台」；现仅 `AURORA_CAP_THREADS`（Emscripten 未开 `-pthread` 时为 0，其余目标为 1）。
///
/// 原生宏 → Aurora 宏 映射表（判定顺序即下述先后，前缀命中优先）：
/// | 原生宏                                        | Aurora 宏                    |
/// |-----------------------------------------------|------------------------------|
/// | `_WIN32`                                      | `AURORA_PLATFORM_WINDOWS`    |
/// | `__APPLE__ && __MACH__`                       | `AURORA_PLATFORM_MACOS`      |
/// | `__EMSCRIPTEN__`                              | `AURORA_PLATFORM_WASM`       |
/// | `__ANDROID__`                                 | `AURORA_PLATFORM_ANDROID`    |
/// | `__linux__`（扣除上述二者后）                  | `AURORA_PLATFORM_LINUX`      |
/// | `__FreeBSD__`/`__NetBSD__`/`__OpenBSD__`/`__DragonFly__` | `AURORA_PLATFORM_BSD` |
/// | `__unix__`/`__unix` 或以上任一 unix-like        | `AURORA_PLATFORM_UNIX`       |
/// | `_M_X64`/`_M_AMD64`/`__x86_64__`/`__amd64__`  | `AURORA_ARCH_X64`            |
/// | `_M_IX86`/`__i386__`                          | `AURORA_ARCH_X86`            |
/// | `_M_ARM64`/`__aarch64__`                      | `AURORA_ARCH_AARCH64`        |
/// | `_M_ARM`/`__arm__`                            | `AURORA_ARCH_ARM32`          |
/// | `__riscv` 且 `__riscv_xlen == 64`             | `AURORA_ARCH_RISCV64`        |
/// | `__EMSCRIPTEN__`/`__wasm__`                   | `AURORA_ARCH_WASM`           |
/// | `__clang__`（任意 clang 变体）                  | `AURORA_COMPILER_CLANG`      |
/// | `__clang__ && __apple_build_version__`         | `AURORA_COMPILER_APPLE_CLANG`|
/// | `__clang__ && _MSC_VER`                        | `AURORA_COMPILER_CLANG_CL`   |
/// | `__EMSCRIPTEN__`（clang 派生）                 | `AURORA_COMPILER_EMSCRIPTEN` |
/// | `_MSC_VER`（非 clang）                          | `AURORA_COMPILER_MSVC`       |
/// | `__GNUC__`（非 clang）                          | `AURORA_COMPILER_GCC`        |
/// | `__GNUC__ && (__MINGW32__||__MINGW64__)`       | `AURORA_COMPILER_MINGW`      |
/// | `__EMSCRIPTEN__` 且无 `__EMSCRIPTEN_PTHREADS__` | `AURORA_CAP_THREADS` = 0     |
///
/// 判定顺序要点：
/// - WASM 必须先于 Linux 判定（Emscripten 工具链基于 musl，会预定义 `__linux__`/`__unix__`）；
/// - Android 必须先于 Linux 判定（`__ANDROID__` 蕴含 `__linux__`）；
/// - macOS 先于其它 unix（`__APPLE__` 蕴含 Mach 内核标记）。
/// - 编译器：Clang 必须先于 MSVC 判定（clang-cl 同时定义 `__clang__` 与 `_MSC_VER`），否则 clang-cl 会被误归为 MSVC。
///
/// @note Thread: n/a（纯编译期）。用法示例：
/// @code
/// #include "aurora/core/platform.h"
/// #if defined(AURORA_PLATFORM_WINDOWS)
/// // Win32 专属路径
/// #elif defined(AURORA_PLATFORM_UNIX)
/// // POSIX 通用路径
/// #endif
/// @endcode

// ─────────────────────────── AURORA_PLATFORM_*：平台家族 ───────────────────────────
// 本段（平台家族 / CPU 架构 / 位宽 / 编译器 / 能力）的宏全部必须在**预处理期可求值**：库内与消费者
// 的 `#if defined(AURORA_PLATFORM_UNIX)`、`#ifdef AURORA_ARCH_WASM` 分派，以及「未命中即未定义」的
// 三态语义，都依赖 `#define` 本身；改成 enum/constexpr 后宏从预处理器消失，整条后端/平台剪裁链恒假。
// 故 macro-usage（别用宏定义常量）与 macro-to-enum（宏常量改 enum）对本文件恒为假阳性。
// 采区间式且覆盖整段而非逐点：命中的 `#define` 随编译目标而变（Windows 取 ARCH_X64、Linux 取
// PLATFORM_UNIX 首支、浏览器取 ARCH_WASM），逐点豁免会随口径漂移——本机 0 条的分支换个目标就冒出来。
// NOLINTBEGIN(*-macro-usage, *-macro-to-enum)
#ifdef _WIN32
/// @brief Windows 目标宏：`_WIN32` 命中时置 1（与 unix 家族互斥，具体平台宏恰好一个为真）。
#define AURORA_PLATFORM_WINDOWS 1U
#elif defined(__APPLE__) && defined(__MACH__)
/// @brief macOS 目标宏：`__APPLE__ && __MACH__` 命中时置 1（先于其余 unix 判定）。
#define AURORA_PLATFORM_MACOS 1
#elif defined(__EMSCRIPTEN__)
/// @brief WASM/Emscripten 目标宏：`__EMSCRIPTEN__` 命中时置 1（必须先于 Linux 判定）。
#define AURORA_PLATFORM_WASM 1
#elif defined(__ANDROID__)
/// @brief Android 目标宏：`__ANDROID__` 命中时置 1（必须先于 Linux 判定，其蕴含 `__linux__`）。
#define AURORA_PLATFORM_ANDROID 1
#elif defined(__linux__)
/// @brief Linux 目标宏：`__linux__` 命中且非 WASM/Android 时置 1。
#define AURORA_PLATFORM_LINUX 1
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
/// @brief BSD 目标宏：FreeBSD/NetBSD/OpenBSD/DragonFly 任一命中时置 1。
#define AURORA_PLATFORM_BSD 1
#endif

/// @brief 聚合宏：任一 unix-like 判据（macOS/Linux/Android/BSD 目标宏或原生 `__unix__`/`__unix`）命中时定义
/// AURORA_PLATFORM_UNIX 1；Windows 下不定义。
/// @param AURORA_PLATFORM_BSD 聚合条件之一（#if 续行末端的宏名，并非函数形参；门禁按预处理文本提取）。
/// @return 命中分支上的预处理产物：#define AURORA_PLATFORM_UNIX 1。
#if defined(AURORA_PLATFORM_MACOS) || defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID) || \
    defined(AURORA_PLATFORM_BSD)
/// @brief unix-like 聚合宏（主支）：macOS/Linux/Android/BSD 目标宏任一已定义时置 1。
#define AURORA_PLATFORM_UNIX 1
#elif defined(__unix__) || defined(__unix)
/// @brief unix-like 聚合宏（回退支）：未命中上述目标宏但编译器预定义 `__unix__`/`__unix` 时置 1。
#define AURORA_PLATFORM_UNIX 1
#endif

// ─────────────────────────── AURORA_ARCH_*：CPU 架构 ───────────────────────────
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__amd64__)
/// @brief X64 架构宏：`_M_X64`/`_M_AMD64`/`__x86_64__`/`__amd64__` 命中时置 1。
#define AURORA_ARCH_X64 1
#elif defined(_M_IX86) || defined(__i386__)
/// @brief X86（32 位）架构宏：`_M_IX86` 或 `__i386__` 命中时置 1。
#define AURORA_ARCH_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__)
/// @brief AArch64 架构宏：`_M_ARM64` 或 `__aarch64__` 命中时置 1。
#define AURORA_ARCH_AARCH64 1
#elif defined(_M_ARM) || defined(__arm__)
/// @brief ARM32 架构宏：`_M_ARM` 或 `__arm__` 命中时置 1。
#define AURORA_ARCH_ARM32 1
#elif defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 64)
/// @brief RISC-V 64 架构宏：`__riscv` 且 `__riscv_xlen == 64` 时置 1。
#define AURORA_ARCH_RISCV64 1
#elif defined(__EMSCRIPTEN__) || defined(__wasm__)
/// @brief WASM 架构宏：`__EMSCRIPTEN__` 或 `__wasm__` 命中时置 1。
#define AURORA_ARCH_WASM 1
#endif

// ─────────────────────────── AURORA_BIT_*：位宽 ───────────────────────────
#if !defined(AURORA_BIT_64) && !defined(AURORA_BIT_32)
#if defined(AURORA_ARCH_X64) || defined(AURORA_ARCH_AARCH64) || defined(AURORA_ARCH_RISCV64)
/// @brief 64 位宽宏：X64/AArch64/RISC-V64 架构命中时置 1。
#define AURORA_BIT_64 1
#elif defined(AURORA_ARCH_X86) || defined(AURORA_ARCH_ARM32)
/// @brief 32 位宽宏：X86/ARM32 架构命中时置 1。
#define AURORA_BIT_32 1
#elif defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ == 8)
/// @brief 64 位宽宏：架构未知时按编译器指针宽度 `__SIZEOF_POINTER__ == 8` 回退判定。
#define AURORA_BIT_64 1
#elif defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ == 4)
/// @brief 32 位宽宏：架构未知时按编译器指针宽度 `__SIZEOF_POINTER__ == 4` 回退判定。
#define AURORA_BIT_32 1
#elif defined(_WIN64)
/// @brief 64 位宽宏：指针宽度不可知且 `_WIN64` 命中时置 1（MSVC 系）。
#define AURORA_BIT_64 1
#elif defined(_WIN32)
/// @brief 32 位宽宏：以上判据均未命中且 `_WIN32` 命中时置 1（MSVC 32 位兜底）。
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
/// @brief Clang 基础宏：`__clang__` 命中时置 1（三大基础宏互斥恰一；先于 MSVC 判定，clang-cl 归此家族）。
#define AURORA_COMPILER_CLANG 1
#ifdef __apple_build_version__
/// @brief Apple Clang 精化宏：Clang 家族下 `__apple_build_version__` 命中时置 1（可与 CLANG 同真）。
#define AURORA_COMPILER_APPLE_CLANG 1
#elif defined(_MSC_VER)
/// @brief clang-cl 精化宏：Clang 家族下 `_MSC_VER` 命中时置 1（可与 CLANG 同真）。
#define AURORA_COMPILER_CLANG_CL 1
#endif
#ifdef __EMSCRIPTEN__
/// @brief Emscripten 精化宏：Clang 家族下 `__EMSCRIPTEN__` 命中时置 1（可与 CLANG 同真）。
#define AURORA_COMPILER_EMSCRIPTEN 1
#endif
#elif defined(_MSC_VER)
/// @brief MSVC 基础宏：非 Clang 且 `_MSC_VER` 命中时置 1。
#define AURORA_COMPILER_MSVC 1
#elif defined(__GNUC__)
/// @brief GCC 基础宏：非 Clang、非 MSVC 且 `__GNUC__` 命中时置 1。
#define AURORA_COMPILER_GCC 1
#if defined(__MINGW32__) || defined(__MINGW64__)
/// @brief MinGW 精化宏：GCC 家族下 `__MINGW32__`/`__MINGW64__` 命中时置 1（可与 GCC 同真）。
#define AURORA_COMPILER_MINGW 1
#endif
#endif

// ─────────────────────────── AURORA_CAP_*：编译期能力 ───────────────────────────
// 与平台家族的区别：能力宏**恒定义**且取值 0/1，回答「本 TU 能用什么」，而非「目标是什么平台」。
//   AURORA_CAP_THREADS —— Emscripten 未开 `-pthread` 时 `std::thread` 构造直接抛 "Not supported"
//   （`__EMSCRIPTEN_PTHREADS__` 仅由 `-pthread` 定义，实测），该组合取 0；其余目标一律取 1。
//   判据是**编译期能力**而非运行期试探：开 `-pthread` 会让 wasm 产物要求 SharedArrayBuffer + COOP/COEP，
//   属产品级取舍，由构建方决定后在此如实反映。
// AURORA_CAP_THREADS 参与多处预处理 `#if` 条件（如 src/aurora/image/registry.cpp 与
// tests/framework/assertions.h 的 `#if AURORA_CAP_THREADS`），必须保持宏形态在预处理期可见；
// 改 enum/constexpr 会让这些 `#if` 见到未定义宏而恒假。恒定义且取值 0/1 是文档化契约
// （utest_platform 以 static_assert 把关），不适用 macro-to-enum 收敛。
// （此处原有逐段区间式豁免，现由文件上方覆盖整段的区间式豁免统一承担，不再嵌套。）
#if defined(AURORA_PLATFORM_WASM) && !defined(__EMSCRIPTEN_PTHREADS__)
/// @brief 线程能力宏（取 0 支）：Emscripten 未开 `-pthread`（无 `__EMSCRIPTEN_PTHREADS__`）时 std::thread 不可用。
#define AURORA_CAP_THREADS 0
#else
/// @brief 线程能力宏（取 1 支）：其余目标（含已开 `-pthread` 的 Emscripten）线程可用。
#define AURORA_CAP_THREADS 1
#endif

// NOLINTEND(*-macro-usage, *-macro-to-enum)
