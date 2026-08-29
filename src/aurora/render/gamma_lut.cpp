// Aurora — gamma LUT 定义与初始化，供 SIMD 双实现共享
#include "aurora/render/detail/gamma_lut.h"

namespace aurora::detail {

auto init_gamma_tables() -> void {
    if (g_gamma_tables.ready.load(std::memory_order_acquire)) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        const auto c = static_cast<float>(i) / 255.0f;
        g_gamma_tables.srgb_to_linear.at(static_cast<std::size_t>(i)) =
            (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    for (int i = 0; i < AURORA_LINEAR_TO_SRGB_SIZE; ++i) {
        const auto c = static_cast<float>(i) / static_cast<float>(AURORA_LINEAR_TO_SRGB_SIZE - 1);
        const float lin = (c <= 0.0031308f) ? (c * 12.92f) : ((1.055f * std::pow(c, 1.0f / 2.4f)) - 0.055f);
        g_gamma_tables.linear_to_srgb.at(static_cast<std::size_t>(i)) =
            static_cast<std::uint8_t>(std::lround(lin * 255.0f));
    }
    g_gamma_tables.ready.store(true, std::memory_order_release);
}

} // namespace aurora::detail