#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "aurora/image/image_codec.h"

// wuffs 提供 GIF 动图解码（stb_image 仅返回首帧，wuffs 返回逐帧动画）。
#ifdef AURORA_BUILD_IMAGE_PNG

#include "aurora/core/image.h"

#include "wuffs/release/c/wuffs-v0.3.c"

namespace aurora::image {

namespace {

// 将 wuffs 时长（flicks，1 秒 = 705600000 flicks）转换为毫秒（向上取整，最小 10ms）。
auto wuffs_duration_to_ms(std::int64_t flicks) -> std::chrono::milliseconds {
    if (flicks <= 0) {
        return std::chrono::milliseconds(100);
    }
    // WUFFS_BASE__FLICKS_PER_MILLISECOND = 705600
    const std::int64_t ms = (flicks + 705599) / 705600;
    return std::chrono::milliseconds(ms < 10 ? 10 : ms);
}

class GifWuffsCodec : public ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "gif-wuffs"; }
    [[nodiscard]] auto format() const -> ImageFormat override { return ImageFormat::GIF; }
    [[nodiscard]] auto can_decode() const -> bool override { return true; }

    [[nodiscard]] auto sniff(std::span<const std::uint8_t> h) const -> bool override {
        return h.size() >= 3 && h[0] == 'G' && h[1] == 'I' && h[2] == 'F';
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data, const DecodeOptions &opt) const
        -> Result<Image> override {
        auto anim = decode_animated(data, opt);
        if (!anim) {
            return anim.error();
        }
        Image img;
        img.width = anim.value().width;
        img.height = anim.value().height;
        if (!anim.value().frames.empty() && anim.value().frames.front().image) {
            const Image &f = *anim.value().frames.front().image;
            img.pixels = f.pixels;
        }
        return img;
    }

    [[nodiscard]] auto decode_animated(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<AnimatedImage> override {
        // 解码器结构体在非 WUFFS_IMPLEMENTATION 单元中默认构造被删除，必须用 alloc()。
        auto deleter = [](void *p) -> void { std::free(p); }; // NOLINT(*-no-malloc)
        std::unique_ptr<wuffs_gif__decoder, decltype(deleter)> dec(wuffs_gif__decoder__alloc(), deleter);
        if (!dec) {
            return make_error(ErrorCode::IOImageDecodeFailed, "wuffs gif: alloc failed");
        }

        wuffs_base__io_buffer src{};
        src.data.ptr = const_cast<uint8_t *>(data.data()); // NOLINT(*-pro-type-const-cast)
        src.data.len = data.size();
        src.meta.wi = data.size();
        src.meta.closed = true;

        wuffs_base__image_config ic{};
        wuffs_base__status status = wuffs_gif__decoder__decode_image_config(dec.get(), &ic, &src);
        if (status.repr != nullptr) {
            return make_error(ErrorCode::IOImageDecodeFailed, std::string("wuffs gif: config ") + status.repr);
        }

        // 强制输出为 RGBA 非预乘：保证每行固定 4 字节、与 aurora::Image 像素格式一致。
        // 复用同一 pb 跨帧调用 decode_frame，wuffs 会按 GIF 的 disposal 方法自动处理画布合成。
        wuffs_base__pixel_config pixel_config = ic.pixcfg;
        const int w = static_cast<int>(wuffs_base__pixel_config__width(&pixel_config));
        const int h = static_cast<int>(wuffs_base__pixel_config__height(&pixel_config));
        if (w <= 0 || h <= 0) {
            return make_error(ErrorCode::LayoutInvalidConstraints, "wuffs gif: bad dimensions");
        }
        wuffs_base__pixel_config__set(&pixel_config, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL, 0,
                                      static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));

        const auto pix_buf_len = static_cast<std::size_t>(wuffs_base__pixel_config__pixbuf_len(&pixel_config));
        const std::size_t work_buf_len = static_cast<std::size_t>(wuffs_gif__decoder__workbuf_len(dec.get()).max_incl);
        std::vector<std::uint8_t> pix_mem(pix_buf_len);
        std::vector<std::uint8_t> work_mem(work_buf_len);
        wuffs_base__pixel_buffer pb{};
        status = wuffs_base__pixel_buffer__set_from_slice(&pb, &pixel_config,
                                                          wuffs_base__make_slice_u8(pix_mem.data(), pix_buf_len));
        if (status.repr != nullptr) {
            return make_error(ErrorCode::IOImageDecodeFailed, std::string("wuffs gif: pb ") + status.repr);
        }
        const wuffs_base__slice_u8 work_buf = wuffs_base__make_slice_u8(work_mem.data(), work_buf_len);

        AnimatedImage anim;
        anim.width = w;
        anim.height = h;
        anim.loop_count = 0;

        wuffs_base__frame_config fc{};
        while (true) {
            status = wuffs_gif__decoder__decode_frame_config(dec.get(), &fc, &src);
            if (status.repr != nullptr) {
                break; // 帧序列结束
            }
            const std::int64_t flicks = wuffs_base__frame_config__duration(&fc);
            const std::chrono::milliseconds dur = wuffs_duration_to_ms(flicks);
            status =
                wuffs_gif__decoder__decode_frame(dec.get(), &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, work_buf, nullptr);
            if (status.repr != nullptr) {
                return make_error(ErrorCode::IOImageDecodeFailed, std::string("wuffs gif: decode ") + status.repr);
            }
            wuffs_base__table_u8 plane = wuffs_base__pixel_buffer__plane(&pb, 0);
            Image frame;
            frame.width = w;
            frame.height = h;
            frame.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
            for (int y = 0; y < h; ++y) {
                const std::uint8_t *s = plane.ptr + (static_cast<std::size_t>(y) * plane.stride); // NOLINT
                std::uint8_t *d =
                    frame.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u); // NOLINT
                std::memcpy(d, s, static_cast<std::size_t>(w) * 4u);
            }
            anim.frames.emplace_back(ImageFrame{
                .image = std::make_shared<Image>(std::move(frame)), .duration = dur, .blend = 0, .dispose = 0 });
        }

        if (anim.frames.empty()) {
            return make_error(ErrorCode::IOImageDecodeFailed, "wuffs gif: no frames");
        }
        return anim;
    }
};

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): 工厂函数供 registry.cpp 跨 TU 调用，需外部链接
auto create_gif_wuffs_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<GifWuffsCodec>(); }

} // namespace aurora::image

#endif // AURORA_BUILD_IMAGE_PNG
