#include "aurora/core/string_util.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace aurora::internal {

// 故意保留 printf 风格 C 可变参实现（见 string_util.h 声明处的 NOLINT 说明）。
// NOLINTNEXTLINE(*-avoid-variadic-functions)
auto string_format(const char *fmt, ...) -> std::string {
    if (fmt == nullptr) {
        return {};
    }

    va_list args; // NOLINT(*-init-variables)
    va_start(args, fmt);
    va_list args_copy; // NOLINT(*-init-variables)
    va_copy(args_copy, args);

    // 使用 args_copy 做第一次测长，避免在第一次调用后丢失 args 的状态
    const int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed <= 0) {
        va_end(args);
        return {};
    }

    std::vector<char> buf(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);

    return std::string{buf.data(), static_cast<std::size_t>(needed)};
}

}  // namespace aurora::internal
