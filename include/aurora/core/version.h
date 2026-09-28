#pragma once

// ============================================================================
// version.h — 库版本常量（单一事实来源：CMakeLists.txt project(VERSION) +
// AURORA_VERSION_SUFFIX 缓存变量，经 AuroraVersion.cmake 编译定义注入）。
// ----------------------------------------------------------------------------
// 完整版本串遵循 semver 2.0.0：MAJOR.MINOR.PATCH[-SUFFIX]，如 `1.0.0-alpha.3`。
// 稳定版后缀为空，AURORA_VERSION_STRING 即 `1.0.0`。
//
// CMake 注入宏：
//   AURORA_VERSION_MAJOR / _MINOR / _PATCH — 纯数字分量
//   AURORA_VERSION_SUFFIX_STR              — 预发布后缀串（如 "alpha.3"）
//   AURORA_HAS_VERSION_SUFFIX              — 后缀非空时为 1，稳定版为 0
// 直接包含本头而未走 CMake 构建时回退到内置默认值（与仓库当前版本一致）。
// ============================================================================

// 版本宏族必须在预处理期可见，constexpr 常量无法满足：① 消费者以
// `#if AURORA_VERSION_MAJOR >= N` 做编译期版本分支；② 本头自身用
// `#if AURORA_HAS_VERSION_SUFFIX` 拼接版本串、以 `AURORA_VERSION_STR`（#x 字符串化）
// 把数值分量并入字面量，均是只有宏能承担的预处理期操作；③ 各分量以
// `#ifndef` 守卫接受 CMake 编译定义注入、缺省时回退内置默认值，constexpr 定义
// 无法被构建系统条件覆盖（路径见文件头注释与 BUILD_OPTIONS.md）。
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
/// @brief 主版本号（semver MAJOR 分量）；由 CMake 编译定义注入，未注入时回退内置默认。
#ifndef AURORA_VERSION_MAJOR
#define AURORA_VERSION_MAJOR 1
#endif

/// @brief 次版本号（semver MINOR 分量）；由 CMake 编译定义注入，未注入时回退内置默认。
#ifndef AURORA_VERSION_MINOR
#define AURORA_VERSION_MINOR 0
#endif

/// @brief 补丁版本号（semver PATCH 分量）；由 CMake 编译定义注入，未注入时回退内置默认。
#ifndef AURORA_VERSION_PATCH
#define AURORA_VERSION_PATCH 0
#endif

/// @brief 预发布后缀字符串（不含前导 '-'；稳定版置 AURORA_HAS_VERSION_SUFFIX 为 0 即可）。
#ifndef AURORA_VERSION_SUFFIX_STR
#define AURORA_VERSION_SUFFIX_STR "alpha.9"
#endif

/// @brief 是否存在预发布后缀：后缀非空时为 1，稳定版为 0。
#ifndef AURORA_HAS_VERSION_SUFFIX
#define AURORA_HAS_VERSION_SUFFIX 1
#endif

/// @brief 字符串化第一级：直接用 `#` 把参数转成字面量（不先行展开）。
/// @param x 待字符串化的预处理表达式。
#define AURORA_VERSION_STR2(x) #x
/// @brief 字符串化第二级：先展开 x 再交给 AURORA_VERSION_STR2，以展开数值参数。
/// @param x 需先行展开的宏名或表达式。
#define AURORA_VERSION_STR(x) AURORA_VERSION_STR2(x)

/// @brief 由 MAJOR.MINOR.PATCH 拼出的纯数字版本串（不含预发布后缀）。
#define AURORA_VERSION_NUMERIC               \
    AURORA_VERSION_STR(AURORA_VERSION_MAJOR) \
    "." AURORA_VERSION_STR(AURORA_VERSION_MINOR) "." AURORA_VERSION_STR(AURORA_VERSION_PATCH)

/// @brief 完整 semver 版本串，如 "1.0.0-alpha.3"（稳定版无后缀）。
#if AURORA_HAS_VERSION_SUFFIX
#define AURORA_VERSION_STRING AURORA_VERSION_NUMERIC "-" AURORA_VERSION_SUFFIX_STR
#else
/// @brief 完整 semver 版本串的稳定版分支（无后缀，仅 MAJOR.MINOR.PATCH）。
#define AURORA_VERSION_STRING AURORA_VERSION_NUMERIC
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)
