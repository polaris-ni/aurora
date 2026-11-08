// tests/fuzz/fuzz_ws_frame.cpp — WebSocket 帧解析面的覆盖引导模糊测试目标。
//
// 被测面：WsFrameReader 的 push / finish 状态机与背压读数；输入为任意字节串。
// 只在 AURORA_BUILD_FUZZ=ON 的独立 clang 构建里编译，不进产品库、不进 CTest。
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aurora/core/result.h"
#include "aurora/net/detail/random_draw.h"
#include "aurora/net/detail/ws_frame.h"

// 本目标只编译解帧路径所需的少数翻译单元。write_ws_frame 的掩码熵源仅为满足链接提供，
// 模糊测试不调用该路径，因此不引入平台随机源与日志依赖。链接整个库时此处定义须删除。
namespace aurora::net::detail {
auto random_fill(std::span<std::byte> out) -> Result<void> {
    for (auto &byte : out) {
        byte = std::byte{0};
    }
    return {};
}
}  // namespace aurora::net::detail

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    const std::string_view payload{reinterpret_cast<const char *>(data), size};

    std::size_t facts = 0;
    aurora::net::detail::WsFrameSinks sinks{
        [&facts](const aurora::net::detail::WsParsedMessage &) -> bool {
            ++facts;
            return true;
        },
        [&facts](std::string_view) -> void { ++facts; },
        [&facts](std::string_view) -> void { ++facts; },
        [&facts](const aurora::net::detail::WsParsedClose &) -> void { ++facts; },
    };

    aurora::net::detail::WsFrameReader reader{std::move(sinks)};
    const auto pushed = reader.push(payload);
    const auto finished = reader.finish();
    volatile std::size_t sink =
        facts + reader.buffered_bytes() + (pushed ? 0U : 1U) + (finished ? 0U : 1U) + (reader.host_paused() ? 1U : 0U);
    (void)sink;
    return 0;
}
