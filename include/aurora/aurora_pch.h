// aurora_pch.h — 预编译头（规格 §3.5 增量编译）。
//
// 收录原则：只纳入「稳定且不常变更」的重型头——标准库 + vendored 三方
// （nlohmann/json.hpp 单头 2.5 万行，是全库最大的单次解析成本）；
// **不纳入任何 aurora 自有头**：widget/render 等头高频变更，纳入会使每次
// 库内头编辑都击穿 PCH（PCH 重建 + 全部 TU 重编），命中率归零。
//
// 消费方式：aurora 静态库 PRIVATE 编译本 PCH（仅一份 .gch）；消费者（demo/测试/
// 工具）不用本文件，而是复用含 aurora.h 伞头的共享 PCH（aurora_consumer_pch 目标，
// 经 target_precompile_headers(REUSE_FROM aurora_consumer_pch)），同样全局仅一份
// （见 CMakeLists.txt 与 cmake/ 各模块）。
#pragma once

// 本头仅含 C++ 内容（标准库 + vendored 三方单头）。aurora 目标在接入原生 Wayland
// 后端时会并入 wayland-scanner 生成的 C 胶水源（xdg-shell/xdg-decoration），使目标
// 同时含 C 与 C++ 源；CMake 会据此再生成一份 C 语言 PCH，此时须整体跳过 C++ 内容，
// 否则 gcc -x c-header 会因 <algorithm> 等非法 C 而编译失败。C++ 翻译单元下 __cplusplus
// 恒定义，行为不变；与生成文件 cmake_pch.hxx 的 #ifdef __cplusplus 守卫保持一致。
#ifdef __cplusplus

// ---- 标准库（按库内头文件实际引用频次收录） ----
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// ---- vendored 三方（稳定不变更；json.hpp 为最大单头解析成本） ----
#include <nlohmann/json.hpp>

#endif // __cplusplus
