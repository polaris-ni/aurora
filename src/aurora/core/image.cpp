#include "aurora/core/image.h"

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
            img.pixels[d + 0] = b[s + 2];  // R  NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            img.pixels[d + 1] = b[s + 1];  // G  NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            img.pixels[d + 2] = b[s + 0];  // B  NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            img.pixels[d + 3] = 255;  // A  NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
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
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return make_error(ErrorCode::IOFileNotFound, std::string("Image::load_svg: cannot open file: ") + p);
    }
    const std::vector<std::uint8_t> buf(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{});
    if (buf.empty()) {
        return make_error(ErrorCode::IOFileNotFound, std::string("Image::load_svg: empty file: ") + p);
    }
    return detail::load_image_svg(buf, target_w, target_h);
}

}  // namespace aurora
