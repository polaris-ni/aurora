#include "aurora/core/image.h"

#include <bit>
#include <filesystem>
#include <fstream>

#include "aurora/image/image_codec.h"

namespace aurora {

namespace {

// 读取小端 32 位整数（BMP 头字段）。文件级辅助，仅 load_bmp 使用。
auto read_le32(const std::vector<std::uint8_t> &b, std::size_t off) -> std::uint32_t {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v = (v << 8U) | static_cast<std::uint32_t>(b[off + static_cast<std::size_t>(i)]);
    }
    return v;
}

// 读取小端 16 位整数（BMP 头字段）。文件级辅助，仅 load_bmp 使用。
auto read_le16(const std::vector<std::uint8_t> &b, std::size_t off) -> std::uint16_t {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(b[off]) |
                                      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                                      (static_cast<std::uint32_t>(b[off + 1U]) << 8U));
}

/// @brief BMP 宽高上限（BITMAPINFOHEADER 字段为 32 位，但实际不可能这么大）：
/// 用于在任何算术之前截断攻击者可控的维度，避免 `w * 3` 这类 int 运算溢出（UB）。
constexpr std::uint32_t AURORA_BMP_MAX_DIMENSION = 65535U;

}  // namespace

namespace detail {

auto load_bmp(const std::vector<std::uint8_t> &b) -> Result<Image> {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') {
        return make_error(ErrorCode::IOParseFailed, "Image::load: not a BMP file");
    }
    const std::uint32_t data_off = read_le32(b, 10);
    const std::uint32_t raw_w = read_le32(b, 18);
    const std::uint32_t raw_h = read_le32(b, 22);
    // 维度先以无符号解析并设上限，再转 int：攻击者可控的 0x7FFFFFFF 会让下方 `w * 3`
    // 发生有符号溢出（UB），进而使 row_bytes 为负、转 size_t 后成为天文偏移量。
    if (raw_w == 0 || raw_h == 0 || raw_w > AURORA_BMP_MAX_DIMENSION || raw_h > AURORA_BMP_MAX_DIMENSION ||
        static_cast<std::size_t>(data_off) >= b.size()) {
        return make_error(ErrorCode::LayoutInvalidConstraints, "Image::load: invalid BMP dimensions");
    }
    // 本解码器只实现「未压缩 24 位」一种形态（见头文件契约）。8/16/32 位与 RLE 变体的
    // 实际行距与此处假设的 w*3 不同，若不拒绝会按错误行距越界读取整幅像素区。
    const std::uint16_t bit_count = read_le16(b, 28);
    const std::uint32_t compression = read_le32(b, 30);
    if (bit_count != 24 || compression != 0) {
        return make_error(ErrorCode::IOParseFailed, "Image::load: only uncompressed 24-bit BMP is supported");
    }
    const int w = static_cast<int>(raw_w);
    const int h = static_cast<int>(raw_h);
    const std::size_t row_bytes = (((static_cast<std::size_t>(raw_w) * 3U) + 3U) / 4U) * 4U;  // 4 字节对齐
    // 像素区必须完整存在于文件内：此前仅校验 data_off 落在缓冲内，导致一个 55 字节的
    // 文件即可声明 1000×1000 并从堆上越界读出 ~3MB 相邻内存返回给调用方（堆信息泄露）。
    // 用 64 位算术求所需字节数，使 32 位目标上 row_bytes * h 也不会回绕而放行。
    const std::uint64_t need = static_cast<std::uint64_t>(row_bytes) * static_cast<std::uint64_t>(raw_h);
    if (need > static_cast<std::uint64_t>(b.size() - static_cast<std::size_t>(data_off))) {
        return make_error(ErrorCode::IOParseFailed, "Image::load: BMP pixel data truncated");
    }
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    for (int y = 0; y < h; ++y) {
        // BMP 行自底向上：文件第 y 行对应图像第 (h-1-y) 行
        const std::size_t src_row = data_off + (static_cast<std::size_t>(y) * row_bytes);
        const std::size_t dst_row = static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w) * 4U;
        for (int x = 0; x < w; ++x) {
            const std::size_t s = src_row + (static_cast<std::size_t>(x) * 3U);
            const std::size_t d = dst_row + (static_cast<std::size_t>(x) * 4U);
            img.pixels[d + 0] = b[s + 2];  // R  NOLINT
            img.pixels[d + 1] = b[s + 1];  // G  NOLINT
            img.pixels[d + 2] = b[s + 0];  // B  NOLINT
            img.pixels[d + 3] = 255;  // A  NOLINT
        }
    }
    return img;
}

auto load_dibv5(const std::vector<std::uint8_t> &b) -> Result<Image> {
    // BITMAPINFOHEADER 起步的 DIB 块至少 40 字节；更小的块连维度字段都读不出来。
    if (b.size() < 40) {
        return make_error(ErrorCode::IOParseFailed, "Image::load: DIB header truncated");
    }
    const auto byte_at = [&b](std::size_t off) -> std::uint8_t {
        // 像素区完整性在下方用 64 位算术核过；头字段读取按有界访问兜底，越界即视为截断。
        return off < b.size() ? b[off] : static_cast<std::uint8_t>(0U);
    };
    const auto le32 = [&byte_at](std::size_t off) -> std::uint32_t {
        return static_cast<std::uint32_t>(byte_at(off)) | (static_cast<std::uint32_t>(byte_at(off + 1U)) << 8U) |
               (static_cast<std::uint32_t>(byte_at(off + 2U)) << 16U) |
               (static_cast<std::uint32_t>(byte_at(off + 3U)) << 24U);
    };
    const auto le16 = [&byte_at](std::size_t off) -> std::uint16_t {
        return static_cast<std::uint16_t>(static_cast<std::uint32_t>(byte_at(off)) |
                                          (static_cast<std::uint32_t>(byte_at(off + 1U)) << 8U));
    };
    const std::uint32_t header_size = le32(0);
    if (header_size < 40 || header_size > b.size()) {
        return make_error(ErrorCode::IOParseFailed, "Image::load: invalid DIB header size");
    }
    const std::uint32_t raw_w = le32(4);
    // bV5Height 是有符号 LONG：负值表示自顶向下行序，绝对值才是行数。
    const std::uint32_t raw_h_signed = le32(8);
    const bool top_down = (raw_h_signed & 0x80000000U) != 0U;
    const std::uint32_t raw_h = top_down ? (0xFFFFFFFFU - raw_h_signed + 1U) : raw_h_signed;
    const std::uint16_t bit_count = le16(14);
    const std::uint32_t compression = le32(16);
    // 维度先设上限再参与算术：攻击者可控的 0x7FFFFFFF 会让行距/行数乘法在 32 位上回绕放行。
    if (raw_w == 0 || raw_h == 0 || raw_w > AURORA_BMP_MAX_DIMENSION || raw_h > AURORA_BMP_MAX_DIMENSION) {
        return make_error(ErrorCode::IOParseFailed, "Image::load: invalid DIB dimensions");
    }
    // 与 load_bmp 同一条「如实不为」纪律：行距与这里假设不符的变体（8/16 位调色板、RLE）一律
    // 拒绝，不按错误行距越界读整幅像素区。
    const bool is_24_rgb = bit_count == 24 && compression == 0;
    const bool is_32 =
        bit_count == 32 && (compression == 0 || compression == 3 || compression == 6);  // BI_RGB / BITFIELDS / ALPHA
    if (!is_24_rgb && !is_32) {
        return make_error(ErrorCode::IOParseFailed, "Image::load: unsupported DIB variant (bit depth / compression)");
    }
    // 通道掩码：BI_RGB 用隐含值；BITFIELDS 族从头部（bV5*Mask，偏移 40/44/48/52）或
    // 40 字节头后随的掩码 DWORD 取（Windows 对 40 字节头 + BITFIELDS 的历史布局）。
    std::uint32_t mask_r = 0x00FF0000U;
    std::uint32_t mask_g = 0x0000FF00U;
    std::uint32_t mask_b = 0x000000FFU;
    std::uint32_t mask_a = 0U;
    std::size_t pixel_off = header_size;
    if (compression != 0) {
        const bool masks_in_header = header_size >= 52;
        const std::size_t mask_count = compression == 6 ? 4U : 3U;  // ALPHABITFIELDS 带 4 份
        if (masks_in_header) {
            mask_r = le32(40);
            mask_g = le32(44);
            mask_b = le32(48);
            if (header_size >= 56) {
                mask_a = le32(52);
            }
        } else {
            const std::size_t masks_bytes = mask_count * 4U;
            if (b.size() < header_size + masks_bytes) {
                return make_error(ErrorCode::IOParseFailed, "Image::load: DIB mask table truncated");
            }
            mask_r = le32(header_size);
            mask_g = le32(header_size + 4U);
            mask_b = le32(header_size + 8U);
            if (mask_count == 4U) {
                mask_a = le32(header_size + 12U);
            }
            pixel_off = header_size + masks_bytes;
        }
        // 掩码重叠或为空都会让通道折算出错值：直接拒绝，不猜。
        if ((mask_r & mask_g) != 0 || (mask_r & mask_b) != 0 || (mask_g & mask_b) != 0 || (mask_r & mask_a) != 0 ||
            (mask_g & mask_a) != 0 || (mask_b & mask_a) != 0) {
            return make_error(ErrorCode::IOParseFailed, "Image::load: overlapping DIB channel masks");
        }
    }
    const std::size_t bytes_per_pixel = is_32 ? 4U : 3U;
    const std::size_t row_bytes =
        is_32 ? static_cast<std::size_t>(raw_w) * 4U : (((static_cast<std::size_t>(raw_w) * 3U) + 3U) / 4U) * 4U;
    const std::uint64_t need = static_cast<std::uint64_t>(row_bytes) * static_cast<std::uint64_t>(raw_h);
    if (pixel_off > b.size() || need > static_cast<std::uint64_t>(b.size() - pixel_off)) {
        return make_error(ErrorCode::IOParseFailed, "Image::load: DIB pixel data truncated");
    }
    // 掩码 → (移位, 位宽)：通道值按位宽折算到 8 位（非 8 位掩码如 10/12 位用就近缩放）。
    const auto channel_of = [](std::uint32_t pixel, std::uint32_t mask, std::uint8_t fallback) -> std::uint8_t {
        if (mask == 0) {
            return fallback;
        }
        const auto bits = static_cast<unsigned>(mask);
        const auto shift = static_cast<unsigned>(std::countr_zero(bits));
        const auto width = static_cast<unsigned>(std::popcount(bits));
        const std::uint32_t raw = (pixel & mask) >> shift;
        if (width >= 8U) {
            return static_cast<std::uint8_t>(raw >> (width - 8U));
        }
        const std::uint32_t max = (1U << width) - 1U;
        return static_cast<std::uint8_t>(((raw * 255U) + (max / 2U)) / max);
    };
    const int w = static_cast<int>(raw_w);
    const int h = static_cast<int>(raw_h);
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    for (std::uint32_t y = 0; y < raw_h; ++y) {
        // 自底向上（height 为正）：DIB 第 y 行对应图像第 (h-1-y) 行；自顶向下逐行对应。
        const std::uint32_t image_y = top_down ? y : (raw_h - 1U - y);
        const std::size_t src_row = pixel_off + (static_cast<std::size_t>(y) * row_bytes);
        const std::size_t dst_row = static_cast<std::size_t>(image_y) * static_cast<std::size_t>(w) * 4U;
        for (int x = 0; x < w; ++x) {
            const std::size_t s = src_row + (static_cast<std::size_t>(x) * bytes_per_pixel);
            const std::uint32_t pixel = is_32 ? le32(s)
                                              : static_cast<std::uint32_t>(byte_at(s)) |
                                                    (static_cast<std::uint32_t>(byte_at(s + 1U)) << 8U) |
                                                    (static_cast<std::uint32_t>(byte_at(s + 2U)) << 16U);
            const std::size_t d = dst_row + (static_cast<std::size_t>(x) * 4U);
            img.pixels[d + 0] = channel_of(pixel, mask_r, 0U);  // R
            img.pixels[d + 1] = channel_of(pixel, mask_g, 0U);  // G
            img.pixels[d + 2] = channel_of(pixel, mask_b, 0U);  // B
            img.pixels[d + 3] = channel_of(pixel, mask_a, 255U);  // A：无 alpha 掩码按不透明
        }
    }
    return img;
}

}  // namespace detail

auto Image::load(std::string_view path) -> Result<Image> {
    // 统一委托给 ImageCodecRegistry（按扩展名/嗅探自动选择编解码器）。
    return image::decode_file(std::string(path));
}

auto Image::load_svg(std::string_view path, int target_w, int target_h) -> Result<Image> {
    const std::string p(path);
    // 与 image::read_file_bytes 同理：目录必须先拦，否则 Linux 下读取时报 EISDIR，
    // libstdc++ 的 basic_filebuf::underflow 会无条件抛出异常。
    std::error_code dir_ec;
    if (std::filesystem::is_directory(p, dir_ec)) {
        return make_error(ErrorCode::IOFileNotFound, std::string("Image::load_svg: not a regular file: ") + p);
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return make_error(ErrorCode::IOFileNotFound, std::string("Image::load_svg: cannot open file: ") + p);
    }
    std::vector<std::uint8_t> buf;
    try {
        buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{});
    } catch (const std::ios_base::failure &) {
        return make_error(ErrorCode::IOFileNotFound, std::string("Image::load_svg: cannot read file: ") + p);
    }
    if (buf.empty()) {
        return make_error(ErrorCode::IOFileNotFound, std::string("Image::load_svg: empty file: ") + p);
    }
    return detail::load_image_svg(buf, target_w, target_h);
}

auto Image::content_hash() const -> std::uint64_t {
    if (!content_hash_value_valid) {
        // FNV-1a 64 位：逐字节折叠像素缓冲。单线程 UI 模型下无并发求值；
        // const 访问经 mutable 缓存落回本对象（拷贝携带缓存，见头文件契约）。
        std::uint64_t h = 14695981039346656037ULL;
        for (const std::uint8_t byte : pixels) {
            h ^= byte;
            h *= 1099511628211ULL;
        }
        content_hash_value = h;
        content_hash_value_valid = true;
    }
    return content_hash_value;
}

}  // namespace aurora
