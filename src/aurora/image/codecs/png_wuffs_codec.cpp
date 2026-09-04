#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "aurora/image/image_codec.h"

// wuffs 提供 PNG 解码（stb_image 之外的备选健壮实现）。
#ifdef AURORA_BUILD_IMAGE_PNG

// 以头文件库方式包含 wuffs 单文件 C 库（仅声明；WUFFS_IMPLEMENTATION 由
// cmake 中 aurora_wuffs OBJECT 库定义，链接进 aurora 提供实现）。
#include "aurora/core/image.h"
#include "wuffs/release/c/wuffs-v0.3.c"

namespace aurora::image {

namespace {

class PngWuffsCodec : public ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "png-wuffs"; }
    [[nodiscard]] auto format() const -> ImageFormat override { return ImageFormat::PNG; }
    [[nodiscard]] auto can_decode() const -> bool override { return true; }

    [[nodiscard]] auto sniff(std::span<const std::uint8_t> h) const -> bool override {
        return h.size() >= 8 && h[0] == 0x89 && h[1] == 0x50 && h[2] == 0x4E && h[3] == 0x47 && h[4] == 0x0D &&
               h[5] == 0x0A && h[6] == 0x1A && h[7] == 0x0A;
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<Image> override {
        // 解码器结构体在非 WUFFS_IMPLEMENTATION 单元中默认构造被删除，必须用 alloc()
        // （内部 calloc + initialize）。unique_ptr + std::free 负责释放。
        auto deleter = [](void *p) -> void { std::free(p); };  // NOLINT(*-no-malloc)
        const std::unique_ptr<wuffs_png__decoder, decltype(deleter)> dec(wuffs_png__decoder__alloc(), deleter);
        if (!dec) {
            return make_error(ErrorCode::IOImageDecodeFailed, "wuffs png: alloc failed");
        }

        wuffs_base__io_buffer src{};
        src.data.ptr = const_cast<std::uint8_t *>(data.data());  // NOLINT(*-pro-type-const-cast)
        src.data.len = data.size();
        src.meta.wi = data.size();
        src.meta.closed = true;

        wuffs_base__image_config ic{};
        wuffs_base__status status = wuffs_png__decoder__decode_image_config(dec.get(), &ic, &src);
        if (status.repr != nullptr) {
            return make_error(ErrorCode::IOImageDecodeFailed, std::string("wuffs png: config ") + status.repr);
        }

        // 强制输出为 RGBA 非预乘：保证每行固定 4 字节，与 aurora::Image 的像素格式一致，
        // 同时让 wuffs 把索引/灰度等原生格式正确 swizzle 成 RGBA。
        wuffs_base__pixel_config pixel_config = ic.pixcfg;
        const int w = static_cast<int>(wuffs_base__pixel_config__width(&pixel_config));
        const int h = static_cast<int>(wuffs_base__pixel_config__height(&pixel_config));
        if (w <= 0 || h <= 0) {
            return make_error(ErrorCode::LayoutInvalidConstraints, "wuffs png: bad dimensions");
        }
        wuffs_base__pixel_config__set(&pixel_config, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL, 0,
                                      static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));

        const auto pix_buf_len = static_cast<std::size_t>(wuffs_base__pixel_config__pixbuf_len(&pixel_config));
        const std::size_t work_buf_len = static_cast<std::size_t>(wuffs_png__decoder__workbuf_len(dec.get()).max_incl);
        std::vector<std::uint8_t> pix_mem(pix_buf_len);
        std::vector<std::uint8_t> work_mem(work_buf_len);
        wuffs_base__pixel_buffer pb{};
        status = wuffs_base__pixel_buffer__set_from_slice(&pb, &pixel_config,
                                                          wuffs_base__make_slice_u8(pix_mem.data(), pix_buf_len));
        if (status.repr != nullptr) {
            return make_error(ErrorCode::IOImageDecodeFailed, std::string("wuffs png: pb ") + status.repr);
        }
        status = wuffs_png__decoder__decode_frame(dec.get(), &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
                                                  wuffs_base__make_slice_u8(work_mem.data(), work_buf_len), nullptr);
        if (status.repr != nullptr) {
            return make_error(ErrorCode::IOImageDecodeFailed, std::string("wuffs png: decode ") + status.repr);
        }

        const wuffs_base__table_u8 plane = wuffs_base__pixel_buffer__plane(&pb, 0);
        Image img;
        img.width = w;
        img.height = h;
        img.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
        for (int y = 0; y < h; ++y) {
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const std::uint8_t *s = plane.ptr + (static_cast<std::size_t>(y) * plane.stride);
            std::uint8_t *d = img.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4U);
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            std::memcpy(d, s, static_cast<std::size_t>(w) * 4U);
        }
        return img;
    }
};

}  // namespace

auto create_png_wuffs_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<PngWuffsCodec>(); }

}  // namespace aurora::image

#endif  // AURORA_BUILD_IMAGE_PNG
