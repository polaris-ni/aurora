#pragma once

#include <string>

namespace aurora::internal {

/// @brief printf 风格安全格式化，返回 std::string（自动按需求扩容，无定长缓冲区溢出风险）。
/// @note 内部工具，**不进 `aurora.h` 公共导出**；仅用于收敛各模块 `std::snprintf` 进栈缓冲的重复样板。
/// @param fmt printf 风格格式串（与 <cstdio> 语义一致）。
/// @param ... args
/// @return 格式化结果；若格式化失败返回空串。
// 故意保留 printf 风格 C 可变参包装：收敛各模块 snprintf 样板，并保持 68+ 处调用点语义不变。
// 项目 .clang-tidy 已禁用 cppcoreguidelines-pro-type-vararg；类型安全替代见 std::format。
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
[[nodiscard]] auto string_format(const char *fmt, ...) -> std::string;

} // namespace aurora::internal