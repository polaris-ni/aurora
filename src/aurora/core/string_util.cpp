#include "aurora/core/string_util.h"

#include <cstdarg>
#include <cstdio>

namespace aurora::internal {

// 故意保留 printf 风格 C 可变参实现（见 string_util.h 声明处的 NOLINT 说明）。
// NOLINTNEXTLINE(*-avoid-variadic-functions)
auto string_format(const char *fmt, ...) -> std::string {
    if (fmt == nullptr) {
        return {};
    }
    va_list args = nullptr;
    va_start(args, fmt);
    va_list args_copy = nullptr;
    va_copy(args_copy, args);

    const int needed = std::vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    if (needed <= 0) {
        va_end(args_copy);
        return {};
    }

    std::string out;
    out.resize(static_cast<std::size_t>(needed));
    std::vsnprintf(out.data(), static_cast<std::size_t>(needed) + 1u, fmt, args_copy);
    va_end(args_copy);
    return out;
}

} // namespace aurora::internal