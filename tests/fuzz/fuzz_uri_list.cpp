// tests/fuzz/fuzz_uri_list.cpp — URI list 解码与编码往返的覆盖引导模糊测试目标。
//
// 被测面：uri_list::decode 的结构性过滤与 encode 的逐字节往返，输入为任意字节串。
// 只在 AURORA_BUILD_FUZZ=ON 的独立 clang 构建里编译，不进产品库、不进 CTest。
// 零发现只说明未触发内存错误与未定义行为；路径边界与授权属调用方契约，不在本目标覆盖内。
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aurora/app/uri_list.h"

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    const std::string_view payload{reinterpret_cast<const char *>(data), size};
    const auto paths = aurora::uri_list::decode(payload);
    const auto round_trip = aurora::uri_list::encode(paths);
    volatile std::size_t sink = round_trip.size() + paths.size();
    (void)sink;
    return 0;
}
