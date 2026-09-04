#pragma once

// ============================================================================
// version.h — 库版本常量（单一事实来源：CMakeLists.txt project(VERSION) +
// AURORA_VERSION_SUFFIX 缓存变量，经 AuroraVersion.cmake 编译定义注入）。
// ----------------------------------------------------------------------------
// 完整版本串遵循 semver 2.0.0：MAJOR.MINOR.PATCH[-SUFFIX]，如 `1.0.0-alpha.1`。
// 稳定版后缀为空，AURORA_VERSION_STRING 即 `1.0.0`。
//
// CMake 注入宏：
//   AURORA_VERSION_MAJOR / _MINOR / _PATCH — 纯数字分量
//   AURORA_VERSION_SUFFIX_STR              — 预发布后缀串（如 "alpha.1"）
//   AURORA_HAS_VERSION_SUFFIX              — 后缀非空时为 1，稳定版为 0
// 直接包含本头而未走 CMake 构建时回退到内置默认值（与仓库当前版本一致）。
// ============================================================================

#ifndef AURORA_VERSION_MAJOR
#define AURORA_VERSION_MAJOR 1
#endif

#ifndef AURORA_VERSION_MINOR
#define AURORA_VERSION_MINOR 0
#endif

#ifndef AURORA_VERSION_PATCH
#define AURORA_VERSION_PATCH 0
#endif

// 预发布后缀字符串（不含前导 '-'；稳定版置 AURORA_HAS_VERSION_SUFFIX 为 0 即可）。
#ifndef AURORA_VERSION_SUFFIX_STR
#define AURORA_VERSION_SUFFIX_STR "alpha.1"
#endif

#ifndef AURORA_HAS_VERSION_SUFFIX
#define AURORA_HAS_VERSION_SUFFIX 1
#endif

// 字符串化辅助（两级宏以展开数值参数）。
#define AURORA_VERSION_STR2(x) #x
#define AURORA_VERSION_STR(x) AURORA_VERSION_STR2(x)

#define AURORA_VERSION_NUMERIC               \
    AURORA_VERSION_STR(AURORA_VERSION_MAJOR) \
    "." AURORA_VERSION_STR(AURORA_VERSION_MINOR) "." AURORA_VERSION_STR(AURORA_VERSION_PATCH)

/// @brief 完整 semver 版本串，如 "1.0.0-alpha.1"（稳定版无后缀）。
#if AURORA_HAS_VERSION_SUFFIX
#define AURORA_VERSION_STRING AURORA_VERSION_NUMERIC "-" AURORA_VERSION_SUFFIX_STR
#else
#define AURORA_VERSION_STRING AURORA_VERSION_NUMERIC
#endif
