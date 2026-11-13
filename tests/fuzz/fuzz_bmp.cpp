// tests/fuzz/fuzz_bmp.cpp — 内置未压缩 24 位 BMP 解码器的覆盖引导模糊测试目标。
//
// 被测面：detail::load_bmp 的头部校验与 BGR 转 RGBA 主循环；输入为任意字节串。
// 只在 AURORA_BUILD_FUZZ=ON 的独立 clang 构建里编译，不进产品库、不进 CTest。
// 头部校验会在像素循环之前拒绝绝大多数输入，本目标的边覆盖显著低于其余四个目标，
// 其零发现的置信度须按此打折；PNG/GIF/WebP 与 JPEG 走第三方解码器，不属本仓库模糊测试面。
#include <cstddef>
#include <cstdint>
#include <vector>

#include "aurora/core/image.h"

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    const std::vector<std::uint8_t> bytes{data, data + size};
    const auto image = aurora::detail::load_bmp(bytes);
    if (image) {
        volatile std::size_t sink = image.value().pixels.size();
        (void)sink;
    } else {
        volatile std::size_t sink = image.error().message.size();
        (void)sink;
    }
    return 0;
}
