#include <algorithm>
#include <vector>

#include "aurora/image/image_codec.h"

// WebP 动图解码依赖 WebPDemux（libwebp 自带）。
#ifdef AURORA_BUILD_IMAGE_WEBP

// libwebp 为 C 库，须 C 链接；webp/*.h 内部已自带 extern "C" 保护，
// 此处外层包裹为防御性冗余，确保任何包含路径下符号链接正确。
extern "C" {
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
}

#include "aurora/core/image.h"

namespace aurora::image {

namespace {

class WebpCodec : public ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "webp"; }
    [[nodiscard]] auto format() const -> ImageFormat override { return ImageFormat::WebP; }
    [[nodiscard]] auto can_decode() const -> bool override { return true; }
    [[nodiscard]] auto can_encode() const -> bool override { return true; }

    [[nodiscard]] auto sniff(std::span<const std::uint8_t> h) const -> bool override {
        return h.size() >= 12 && h[0] == 'R' && h[1] == 'I' && h[2] == 'F' && h[3] == 'F' && h[8] == 'W' &&
               h[9] == 'E' && h[10] == 'B' && h[11] == 'P';
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<Image> override {
        int w = 0;
        int h = 0;
        std::uint8_t *rgba = WebPDecodeRGBA(data.data(), data.size(), &w, &h);
        if (rgba == nullptr) {
            return make_error(ErrorCode::IOImageDecodeFailed, "webp: decode failed");
        }
        Image img;
        img.width = w;
        img.height = h;
        const std::size_t pixel_count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
        img.pixels.resize(pixel_count);
        std::copy_n(rgba, pixel_count, img.pixels.data());
        WebPFree(rgba);
        return img;
    }

    [[nodiscard]] auto decode_animated(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<AnimatedImage> override {
        WebPData wp{};
        wp.bytes = data.data();
        wp.size = data.size();
        WebPDemuxer *demux = WebPDemux(&wp);
        if (demux == nullptr) {
            return make_error(ErrorCode::IOImageDecodeFailed, "webp: demux failed");
        }
        AnimatedImage anim;
        anim.loop_count = static_cast<int>(WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT));
        std::uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
        (void)flags;

        WebPIterator iter{};
        if (WebPDemuxGetFrame(demux, 1, &iter) != 0) {
            do {
                int w = 0;
                int h = 0;
                std::uint8_t *rgba = WebPDecodeRGBA(iter.fragment.bytes, iter.fragment.size, &w, &h);
                if (rgba != nullptr) {
                    Image frame;
                    frame.width = w;
                    frame.height = h;
                    const std::size_t pixel_count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U;
                    frame.pixels.resize(pixel_count);
                    std::copy_n(rgba, pixel_count, frame.pixels.data());
                    WebPFree(rgba);
                    anim.width = w;
                    anim.height = h;
                    anim.frames.emplace_back(ImageFrame{ .image = std::make_shared<Image>(std::move(frame)),
                                                         .duration = std::chrono::milliseconds(iter.duration),
                                                         .blend = 0,
                                                         .dispose = 0 });
                }
            } while (WebPDemuxNextFrame(&iter) != 0);
            WebPDemuxReleaseIterator(&iter);
        }
        WebPDemuxDelete(demux);
        if (anim.frames.empty()) {
            return make_error(ErrorCode::IOImageDecodeFailed, "webp: no frames");
        }
        return anim;
    }

    [[nodiscard]] auto encode(const Image &img, const EncodeOptions &opt) const
        -> Result<std::vector<std::uint8_t>> override {
        const int stride = img.width * 4;
        std::uint8_t *out = nullptr;
        std::size_t out_size = 0;
        if (opt.lossless) {
            out_size = WebPEncodeLosslessRGBA(img.pixels.data(), img.width, img.height, stride, &out);
        } else {
            const auto q = static_cast<float>(opt.quality > 0 ? opt.quality : 90);
            out_size = WebPEncodeRGBA(img.pixels.data(), img.width, img.height, stride, q, &out);
        }
        if (out == nullptr || out_size == 0) {
            return make_error(ErrorCode::IOImageEncodeFailed, "webp: encode failed");
        }
        std::vector<std::uint8_t> buf(out_size);
        std::copy_n(out, out_size, buf.data());
        WebPFree(out);
        return buf;
    }
};

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): 工厂函数供 registry.cpp 跨 TU 调用，需外部链接
auto create_webp_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<WebpCodec>(); }

} // namespace aurora::image

#endif // AURORA_BUILD_IMAGE_WEBP
