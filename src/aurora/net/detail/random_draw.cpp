// random_draw.cpp —— 系统熵取数封装的推进体（口径见同名头文件）。
#include "aurora/net/detail/random_draw.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/net/random.h"

namespace aurora::net::detail {

auto random_fill(std::span<std::byte> out) -> Result<void> {
    // 恒走进程级缺省源，不接任何可注入的源：协议面上的不可预测值若跟着可替换的源走，等于把协议要求
    // 变成一个可以关掉的开关（`aurora/net/random.h` 第二条口径）。测试要断言的是映射与形状，不是熵本身。
    return aurora::net::default_random_source().fill(out);
}

auto random_u64() -> Result<std::uint64_t> {
    std::array<std::byte, 8> bytes{};
    if (const auto filled = random_fill(bytes); !filled) {
        return filled.error();
    }
    // 首字节落在最高位：字节序在这里写死而不是 memcpy 视图，两条腿（little / big endian）才交出同一个值。
    std::uint64_t value = 0;
    for (const std::byte unit : bytes) {
        value = (value << 8U) | static_cast<std::uint64_t>(unit);
    }
    return value;
}

auto unit_from_u64(std::uint64_t value) -> double {
    // 高 53 位 / 2^53：53 是 `double` 尾数精度，取更多位只会被舍入回上界，取更少位白白丢掉分辨率。
    constexpr double inverse_precision = 1.0 / 9007199254740992.0;  // 2^-53
    return static_cast<double>(value >> 11U) * inverse_precision;
}

auto random_unit() -> double {
    const auto value = random_u64();
    if (!value) {
        note_entropy_unavailable("retry jitter");
        return 0.0;  // 退回纯指数退避：重放照常，只是不再打散同一时刻的整批重发
    }
    return unit_from_u64(value.value());
}

auto note_entropy_unavailable(std::string_view where) -> void {
    static std::atomic<bool> announced{false};
    if (announced.exchange(true, std::memory_order_relaxed)) {
        return;  // 进程内只记第一次：刷屏会把首条真正的原因冲掉
    }
    AURORA_LOG_WARN("net", "system entropy unavailable, degraded at: ", where);
}

}  // namespace aurora::net::detail
