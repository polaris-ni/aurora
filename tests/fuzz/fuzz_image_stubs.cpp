// tests/fuzz/fuzz_image_stubs.cpp — fuzz_bmp 的被测面边界声明。
//
// 本目标只把 image.cpp 中定义 detail::load_bmp 的那一个翻译单元编进来，不链接整个图片
// 子系统。Image::load 与 load_svg 会引用解码注册表与 SVG 光栅化，二者都不属于被测面，
// 故在此提供显式桩以闭合链接；桩返回可辨识的错误码，一旦被意外调用即可从结果看出。
// 若日后改为链接整个库，本文件必须删除，否则会与真实定义重复。
#include <cstdint>
#include <filesystem>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/image/image_codec.h"

namespace {

auto stub_error() -> aurora::Error {
    aurora::Error error;
    error.code = "fuzz-stub-not-exercised";
    error.message = "stub: not part of the fuzzed surface";
    return error;
}

}  // namespace

namespace aurora::image {
auto decode_file(const std::filesystem::path & /*p*/, const DecodeOptions & /*opt*/) -> Result<Image> {
    return stub_error();
}
}  // namespace aurora::image

namespace aurora::detail {
auto load_image_svg(const std::vector<std::uint8_t> & /*buf*/, int /*target_w*/, int /*target_h*/) -> Result<Image> {
    return stub_error();
}
}  // namespace aurora::detail
