// tests/fuzz/fuzz_http_wire.cpp — HTTP/1.1 线上字节解析面的覆盖引导模糊测试目标。
//
// 被测面：响应头解析、分块传输解码与整条应答装配；输入为任意字节串。
// 只在 AURORA_BUILD_FUZZ=ON 的独立 clang 构建里编译，不进产品库、不进 CTest。
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "aurora/net/detail/http_wire.h"

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    const std::string_view payload{reinterpret_cast<const char *>(data), size};

    const auto head = aurora::net::wire::parse_response_head(payload);
    if (head) {
        const auto &maybe_head = head.value();
        if (maybe_head.has_value()) {
            const auto framing = aurora::net::wire::framing_of(*maybe_head, false);
            volatile std::size_t sink = framing ? std::size_t{1} : framing.error().message.size();
            (void)sink;
        }
    }

    aurora::net::wire::ChunkedDecoder chunked;
    const auto pushed = chunked.push(payload);
    std::vector<char> plain;
    chunked.drain(plain);

    aurora::net::wire::ResponseAssembler assembler{std::nullopt, false};
    const auto absorbed = assembler.push(payload);
    const auto closed = assembler.peer_closed();

    volatile std::size_t sink2 = plain.size() + (pushed ? 0U : 1U) + (absorbed ? 0U : 1U) + (closed ? 0U : 1U);
    (void)sink2;
    return 0;
}
