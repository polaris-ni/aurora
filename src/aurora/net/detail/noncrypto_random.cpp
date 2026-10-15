// noncrypto_random.cpp —— 库内置非密码学序列的推进体（口径见同名头文件）。
#include "aurora/net/detail/noncrypto_random.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace aurora::net::detail {
namespace {

/// @brief 序列状态：Knuth MMIX 的 LCG 参数，进程内首次使用时以单调时钟播种。
/// @return 状态的引用（CAS 推进的唯一落点）。
[[nodiscard]] auto sequence_state() -> std::atomic<std::uint64_t> & {
    constexpr std::uint64_t seed_mix = UINT64_C(0x9E3779B97F4A7C15);  // 黄金比例常量：把时钟低位搅开
    static std::atomic<std::uint64_t> state{
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ seed_mix};
    return state;
}

/// @brief 推进一步并交出新的状态值（调用方自行取所需的高位段）。
/// @return 推进后的 64 位状态。
[[nodiscard]] auto advance() -> std::uint64_t {
    constexpr std::uint64_t multiplier = UINT64_C(6364136223846793005);
    constexpr std::uint64_t increment = UINT64_C(1442695040888963407);
    auto &state = sequence_state();
    auto current = state.load(std::memory_order_relaxed);
    for (;;) {
        const auto next = (current * multiplier) + increment;
        if (state.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
            return next;
        }
    }
}

/// @brief 交出状态的高 32 位（低数位是 LCG 的弱位，故弃之）。
/// @return 高 32 位的有效段。
[[nodiscard]] auto high_half(std::uint64_t value) -> std::uint64_t { return value >> 32; }

}  // namespace

auto noncrypto_unit() -> double {
    // 高 31 位折算到 `[0, 1)`：除以 2^31 而不是 2^32——后者把上界压到 0.5，会让公共契约里
    // 「`jitter` 幅度 `[0, 1]`」实际只兑现一半（`RetryPolicy::jitter` 的注释即该契约的出处）。
    return static_cast<double>(advance() >> 33) * (1.0 / 2147483648.0);
}

auto noncrypto_u64() -> std::uint64_t { return (high_half(advance()) << 32) | high_half(advance()); }

}  // namespace aurora::net::detail
