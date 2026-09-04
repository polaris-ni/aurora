#include <algorithm>
#include <csetjmp>
#include <vector>

#include "aurora/image/image_codec.h"

// libjpeg-turbo 提供 jpeg_mem_src / jpeg_mem_dest（内存源/目标管理器）。
#ifdef AURORA_BUILD_IMAGE_JPEG

// libjpeg-turbo 为 C 库，须 C 链接；jpeglib.h 内部已自带 extern "C" 保护，
// 此处外层包裹为防御性冗余，确保任何包含路径下符号链接正确。
extern "C" {
#include <jpeglib.h>
}

#include "aurora/core/image.h"

namespace aurora::image {

namespace {

struct JpegErrorMgr {
    jpeg_error_mgr pub;
    std::jmp_buf jb;
};

void jpeg_error_exit(j_common_ptr cinfo) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libjpeg C 回调须从基结构指针取回派生错误管理器
    auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
    // NOLINTNEXTLINE(*-avoid-setjmp-longjmp): libjpeg-turbo 标准错误恢复机制，C 回调无法抛异常
    longjmp(err->jb, 1);
}

class JpegTurboCodec : public ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "jpeg-turbo"; }
    [[nodiscard]] auto format() const -> ImageFormat override { return ImageFormat::JPEG; }
    [[nodiscard]] auto can_decode() const -> bool override { return true; }
    [[nodiscard]] auto can_encode() const -> bool override { return true; }

    [[nodiscard]] auto sniff(std::span<const std::uint8_t> h) const -> bool override {
        return h.size() >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF;
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<Image> override {
        Image img;
        JpegErrorMgr err_mgr{};
        jpeg_decompress_struct cinfo{};
        cinfo.err = jpeg_std_error(&err_mgr.pub);
        err_mgr.pub.error_exit = jpeg_error_exit;
        // NOLINTNEXTLINE(*-avoid-setjmp-longjmp): libjpeg-turbo 标准错误恢复机制
        if (setjmp(err_mgr.jb)) {
            jpeg_destroy_decompress(&cinfo);
            return make_error(ErrorCode::IOImageDecodeFailed, "jpeg-turbo: decode failed");
        }
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, data.data(), static_cast<unsigned long>(data.size()));
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_EXT_RGBA;
        jpeg_start_decompress(&cinfo);

        const int w = static_cast<int>(cinfo.output_width);
        const int h = static_cast<int>(cinfo.output_height);
        const unsigned int row_stride = cinfo.output_width * static_cast<unsigned int>(cinfo.output_components);
        img.width = w;
        img.height = h;
        img.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);

        std::vector<std::uint8_t *> rows(static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y) {
            rows[static_cast<std::size_t>(y)] =
                img.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(row_stride));  // NOLINT
        }
        while (cinfo.output_scanline < cinfo.output_height) {
            jpeg_read_scanlines(&cinfo, &rows[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
        }
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return img;
    }

    [[nodiscard]] auto encode(const Image &img, const EncodeOptions &opt) const
        -> Result<std::vector<std::uint8_t>> override {
        volatile const int quality = opt.quality > 0 ? opt.quality : 90;
        jpeg_compress_struct cinfo{};
        JpegErrorMgr err_mgr{};
        cinfo.err = jpeg_std_error(&err_mgr.pub);
        err_mgr.pub.error_exit = jpeg_error_exit;
        // NOLINTNEXTLINE(*-avoid-setjmp-longjmp): libjpeg-turbo 标准错误恢复机制
        if (setjmp(err_mgr.jb)) {
            jpeg_destroy_compress(&cinfo);
            return make_error(ErrorCode::IOImageEncodeFailed, "jpeg-turbo: encode failed");
        }
        jpeg_create_compress(&cinfo);
        unsigned char *out_buf = nullptr;
        unsigned long out_size = 0;
        jpeg_mem_dest(&cinfo, &out_buf, &out_size);

        cinfo.image_width = static_cast<JDIMENSION>(img.width);
        cinfo.image_height = static_cast<JDIMENSION>(img.height);
        cinfo.input_components = 4;
        cinfo.in_color_space = JCS_EXT_RGBA;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, quality, TRUE);

        jpeg_start_compress(&cinfo, TRUE);
        const unsigned int row_stride = static_cast<unsigned int>(img.width) * 4U;
        std::vector<std::uint8_t *> rows(static_cast<std::size_t>(img.height));
        for (int y = 0; y < img.height; ++y) {
            rows[static_cast<std::size_t>(y)] = const_cast<std::uint8_t *>(img.pixels.data()) +  // NOLINT
                                                (static_cast<std::size_t>(y) * static_cast<std::size_t>(row_stride));
        }
        while (cinfo.next_scanline < cinfo.image_height) {
            jpeg_write_scanlines(&cinfo, &rows[cinfo.next_scanline], cinfo.image_height - cinfo.next_scanline);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);

        std::vector<std::uint8_t> out(out_size);
        std::copy_n(out_buf, out_size, out.data());
        free(out_buf);  // NOLINT(*-no-malloc)
        return out;
    }
};

}  // namespace

auto create_jpeg_turbo_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<JpegTurboCodec>(); }

}  // namespace aurora::image

#endif  // AURORA_BUILD_IMAGE_JPEG
