// 单一翻译单元：包含 stb_image 实现（STB_IMAGE_IMPLEMENTATION 必须仅定义一次）。
// 提供 PNG/JPG/GIF/BMP(经 stb)/TGA/HDR 等格式的零依赖解码。

#define STB_IMAGE_IMPLEMENTATION // NOLINT(*-identifier-naming)
#define STB_IMAGE_STATIC         // NOLINT(*-identifier-naming)
// stb_image 是单头库，定义大量 static 函数，项目仅用到其中少数；GCC 会对其余
// 未调用函数报 -Wunused-function（第三方代码，非本仓库问题），此处局部抑制。
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "stb_image.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <cstring>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"

namespace aurora::detail {

auto load_image_stb(const std::vector<std::uint8_t> &buf, std::string_view path) -> Result<Image> {
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc *data =
        stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()), &w, &h, &channels, 4); // 强制 4 通道 RGBA8
    if (data == nullptr) {
        return make_error(ErrorCode::IOImageDecodeFailed,
                          std::string("Image::load: stb decode failed (unsupported format or corrupted file): ") +
                              std::string(path));
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(data);
        return make_error(ErrorCode::IOImageInvalidDimensions,
                          std::string("Image::load: decoded image has invalid dimensions: ") + std::string(path));
    }

    Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    std::memcpy(img.pixels.data(), data, img.pixels.size());
    stbi_image_free(data);
    return img;
}

} // namespace aurora::detail
