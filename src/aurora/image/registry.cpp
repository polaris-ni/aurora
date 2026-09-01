#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "aurora/image/image_codec.h"

#include "codecs/codecs_internal.h"

namespace aurora::image {

// ---------------------------------------------------------------------------
// 格式辅助函数（自由函数）
// ---------------------------------------------------------------------------

auto format_name(ImageFormat f) -> std::string_view {
    switch (f) {
    case ImageFormat::PNG: return "png";
    case ImageFormat::JPEG: return "jpeg";
    case ImageFormat::GIF: return "gif";
    case ImageFormat::BMP: return "bmp";
    case ImageFormat::WebP: return "webp";
    case ImageFormat::SVG: return "svg";
    case ImageFormat::Unknown: return "unknown";
    }
    return "unknown";
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): 嗅探分支较多，复杂度 26 略超阈值 25，重构收益低
auto detect_format(std::span<const std::uint8_t> header) -> ImageFormat {
    if (header.size() >= 8 && header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47 &&
        header[4] == 0x0D && header[5] == 0x0A && header[6] == 0x1A && header[7] == 0x0A) {
        return ImageFormat::PNG;
    }
    if (header.size() >= 3) {
        if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
            return ImageFormat::JPEG;
        }
        if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F') {
            return ImageFormat::GIF;
        }
        if (header[0] == '#') {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 字节流转字符串视图属预期类型双关
            const std::string_view head(reinterpret_cast<const char *>(header.data()),
                                        header.size() < 10 ? header.size() : 10);
            if (head.starts_with("#?RADIANCE") || head.starts_with("#?RGBE")) {
                // HDR 当前枚举未单列；交由 StbCodec 兜底解码（其嗅探覆盖 HDR 魔数）。
                return ImageFormat::Unknown;
            }
        }
    }
    if (header.size() >= 2 && header[0] == 'B' && header[1] == 'M') {
        return ImageFormat::BMP;
    }
    if (header.size() >= 4 && header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F') {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 字节流转字符串视图属预期类型双关
        const std::string_view head(reinterpret_cast<const char *>(header.data()),
                                    header.size() < 12 ? header.size() : 12);
        if (head.find("WEBP") != std::string_view::npos) {
            return ImageFormat::WebP;
        }
    }
    return ImageFormat::Unknown;
}

auto format_from_path(std::filesystem::path const &p) -> ImageFormat {
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
    if (ext == ".png") {
        return ImageFormat::PNG;
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        return ImageFormat::JPEG;
    }
    if (ext == ".gif") {
        return ImageFormat::GIF;
    }
    if (ext == ".bmp") {
        return ImageFormat::BMP;
    }
    if (ext == ".webp") {
        return ImageFormat::WebP;
    }
    if (ext == ".svg") {
        return ImageFormat::SVG;
    }
    return ImageFormat::Unknown;
}

// ---------------------------------------------------------------------------
// 内部辅助（文件级）
// ---------------------------------------------------------------------------

namespace {

auto read_file_bytes(const std::filesystem::path &p) -> Result<std::vector<std::uint8_t>> {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        return make_error(ErrorCode::IOFileNotFound, std::string("image decode: cannot open ") + p.string());
    }
    std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.empty()) {
        return make_error(ErrorCode::IOFileNotFound, std::string("image decode: empty file ") + p.string());
    }
    return buf;
}

auto decode_bytes(const std::vector<std::shared_ptr<ImageCodec>> &codecs, std::span<const std::uint8_t> data,
                  const DecodeOptions &opt) -> Result<Image> {
    // 第一遍：按嗅探匹配的解码器优先
    for (const auto &c : codecs) {
        if (c->can_decode() && c->sniff(data)) {
            if (auto r = c->decode(data, opt)) {
                return r;
            }
        }
    }
    // 兜底：未嗅探命中的解码器逐个尝试（如嗅探不可靠的格式）
    for (const auto &c : codecs) {
        if (c->can_decode() && !c->sniff(data)) {
            if (auto r = c->decode(data, opt)) {
                return r;
            }
        }
    }
    return make_error(ErrorCode::IOImageDecodeFailed, "no registered codec could decode the image");
}

auto decode_animated_bytes(const std::vector<std::shared_ptr<ImageCodec>> &codecs, std::span<const std::uint8_t> data,
                           const DecodeOptions &opt) -> Result<AnimatedImage> {
    for (const auto &c : codecs) {
        if (c->can_decode() && c->sniff(data)) {
            if (auto r = c->decode_animated(data, opt)) {
                return r;
            }
        }
    }
    // 无动图解码器时，回退到单帧静态图
    auto img = decode_bytes(codecs, data, opt);
    if (!img) {
        return img.error();
    }
    AnimatedImage anim;
    anim.width = img.value().width;
    anim.height = img.value().height;
    anim.frames.emplace_back(ImageFrame{ .image = std::make_shared<Image>(std::move(img.value())),
                                         .duration = std::chrono::milliseconds(0),
                                         .blend = 0,
                                         .dispose = 0 });
    return anim;
}

} // namespace

// ---------------------------------------------------------------------------
// 注册表实现（PIMPL）
// ---------------------------------------------------------------------------

struct ImageCodecRegistry::Impl {
    std::vector<std::shared_ptr<ImageCodec>> codecs;
};

ImageCodecRegistry::ImageCodecRegistry() : m_impl(std::make_shared<Impl>()) {
    // 基础编解码器（始终可用）
    register_codec(create_bmp_codec());
    register_codec(create_svg_codec());
    register_codec(create_stb_codec());
    register_codec(create_png_write_codec());

    // 外部编解码器（按构建开关条件接入）
#ifdef AURORA_BUILD_IMAGE_JPEG
    register_codec(create_jpeg_turbo_codec());
#endif
#ifdef AURORA_BUILD_IMAGE_WEBP
    register_codec(create_webp_codec());
#endif
#ifdef AURORA_BUILD_IMAGE_PNG
    register_codec(create_png_wuffs_codec());
    register_codec(create_gif_wuffs_codec());
#endif
}

auto ImageCodecRegistry::instance() -> ImageCodecRegistry & {
    static ImageCodecRegistry reg;
    return reg;
}

auto ImageCodecRegistry::register_codec(std::shared_ptr<ImageCodec> codec) const -> void {
    if (codec) {
        m_impl->codecs.push_back(std::move(codec));
    }
}

auto ImageCodecRegistry::decode_file(const std::filesystem::path &p, const DecodeOptions &opt) const -> Result<Image> {
    auto buf = read_file_bytes(p);
    if (!buf) {
        return buf.error();
    }
    return decode_bytes(m_impl->codecs, buf.value(), opt);
}

auto ImageCodecRegistry::decode_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt) const
    -> Result<Image> {
    return decode_bytes(m_impl->codecs, data, opt);
}

auto ImageCodecRegistry::decode(const ImageSource &src, const DecodeOptions &opt) const -> Result<Image> {
    if (src.kind == ImageSource::Kind::File) {
        return decode_file(src.path, opt);
    }
    return decode_memory(src.memory, opt);
}

auto ImageCodecRegistry::decode_animated_file(const std::filesystem::path &p, const DecodeOptions &opt) const
    -> Result<AnimatedImage> {
    auto buf = read_file_bytes(p);
    if (!buf) {
        return buf.error();
    }
    return decode_animated_bytes(m_impl->codecs, buf.value(), opt);
}

auto ImageCodecRegistry::decode_animated_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt) const
    -> Result<AnimatedImage> {
    return decode_animated_bytes(m_impl->codecs, data, opt);
}

auto ImageCodecRegistry::decode_animated(const ImageSource &src, const DecodeOptions &opt) const
    -> Result<AnimatedImage> {
    if (src.kind == ImageSource::Kind::File) {
        return decode_animated_file(src.path, opt);
    }
    return decode_animated_memory(src.memory, opt);
}

auto ImageCodecRegistry::encode(const Image &img, const EncodeOptions &opt) const -> Result<std::vector<std::uint8_t>> {
    const ImageFormat fmt = opt.format;
    for (const auto &c : m_impl->codecs) {
        if (c->can_encode() && c->format() == fmt) {
            return c->encode(img, opt);
        }
    }
    // 未指定格式时，回退到任意可用编码器（默认 PNG）
    if (fmt == ImageFormat::Unknown) {
        for (const auto &c : m_impl->codecs) {
            if (c->can_encode()) {
                return c->encode(img, opt);
            }
        }
    }
    return make_error(ErrorCode::GeneralNotSupported, "no encoder available for the requested image format");
}

auto ImageCodecRegistry::save(const Image &img, const std::filesystem::path &p, const EncodeOptions &opt) const
    -> Result<bool> {
    EncodeOptions o = opt;
    if (o.format == ImageFormat::Unknown) {
        o.format = format_from_path(p);
        if (o.format == ImageFormat::Unknown) {
            o.format = ImageFormat::PNG;
        }
    }
    auto bytes = encode(img, o);
    if (!bytes) {
        return bytes.error();
    }
    std::ofstream f(p, std::ios::binary);
    if (!f.is_open()) {
        return make_error(ErrorCode::IOFileNotFound, std::string("image save: cannot open ") + p.string());
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 字节序列写文件属预期类型双关
    f.write(reinterpret_cast<const char *>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
    if (!f.good()) {
        return make_error(ErrorCode::IOFileNotFound, "image save: write incomplete");
    }
    return Result<bool>{ true };
}

auto ImageCodecRegistry::decode_async(const ImageSource &src, const DecodeOptions &opt) const
    -> std::future<Result<Image>> {
    auto data = std::make_shared<std::vector<std::uint8_t>>();
    if (src.kind == ImageSource::Kind::File) {
        auto b = read_file_bytes(src.path);
        if (!b) {
            return std::async(std::launch::deferred, [err = b.error()]() -> Result<Image> { return err; });
        }
        *data = std::move(b.value());
    } else {
        *data = src.memory;
    }
    auto codecs = m_impl->codecs;
    return std::async(std::launch::async,
                      [codecs, data, opt]() -> Result<Image> { return decode_bytes(codecs, *data, opt); });
}

auto ImageCodecRegistry::registered() const -> std::vector<std::string> {
    std::vector<std::string> names;
    names.reserve(m_impl->codecs.size());
    for (const auto &c : m_impl->codecs) {
        names.emplace_back(c->name());
    }
    return names;
}

} // namespace aurora::image
