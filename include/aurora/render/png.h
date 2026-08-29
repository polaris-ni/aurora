#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {

/// @brief 内置最小化 PNG 编码器（RGBA8，stored-block deflate，零三方依赖）。
namespace detail {

/// @brief 将原始像素数据打包为 zlib stored-block IDAT 负载。
[[nodiscard]] inline auto build_idat(const std::uint8_t *raw, std::size_t raw_len) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> idat;
    idat.push_back(0x78); // CMF (deflate, 32K window)
    idat.push_back(0x01); // FLG (valid FCHECK, no dict)

    std::size_t off = 0;
    while (off < raw_len) {
        constexpr std::size_t max_stored = 65535;
        const bool last = (off + max_stored >= raw_len);
        const std::size_t chunk = last ? (raw_len - off) : max_stored;
        idat.push_back(static_cast<std::uint8_t>(last ? 0x01U : 0x00U)); // BFINAL | BTYPE=00
        const auto len32 = static_cast<std::uint32_t>(chunk);
        const std::uint32_t nlen32 = ~len32;
        idat.push_back(static_cast<std::uint8_t>(len32 & 0xFFU));
        idat.push_back(static_cast<std::uint8_t>((len32 >> 8U) & 0xFFU));
        idat.push_back(static_cast<std::uint8_t>(nlen32 & 0xFFU));
        idat.push_back(static_cast<std::uint8_t>((nlen32 >> 8U) & 0xFFU));
        for (std::size_t i = 0; i < chunk; ++i) {
            idat.push_back(raw[off + i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic,
                                          // cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        off += chunk;
    }

    return idat;
}

/// @brief 将 RGBA8 像素编码为 PNG 字节流（不写文件）。
/// @return 成功返回字节向量；失败返回带信息的 Error。
[[nodiscard]] inline auto write_png_to_memory(const std::uint8_t *rgba, int width, int height)
    -> Result<std::vector<std::uint8_t>> {
    if (width <= 0 || height <= 0 || rgba == nullptr) {
        return make_error(ErrorCode::LayoutInvalidConstraints, "writePNG: invalid dimensions");
    }

    // ---- CRC32 ----
    static constexpr auto crc_table = []() -> std::array<std::uint32_t, 256> {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xedb88320U ^ (c >> 1U)) : (c >> 1U);
            }
            // NOLINTNEXTLINE(*-pro-bounds-constant-array-index, *-pro-bounds-avoid-unchecked-container-access)
            t[n] = c;
        }
        return t;
    }();
    auto crc32 = [&](const std::uint8_t *buf, std::size_t len) -> unsigned int {
        std::uint32_t c = 0xFFFFFFFFU;
        for (std::size_t i = 0; i < len; ++i) {
            // NOLINTNEXTLINE
            c = crc_table[(c ^ static_cast<std::uint32_t>(buf[i])) & 0xFFU] ^ (c >> 8U);
        }
        return c ^ 0xFFFFFFFFU;
    };
    auto adler32 = [](const std::uint8_t *buf, std::size_t len) -> std::uint32_t {
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (std::size_t i = 0; i < len; ++i) {
            a = (a + buf[i]) % 65521U; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            b = (b + a) % 65521U;
        }
        return (b << 16U) | a;
    };

    std::vector<std::uint8_t> out;
    auto put_u32 = [&](std::uint32_t v) -> void {
        out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    };
    auto put_chunk = [&](const char *type, const std::uint8_t *data, std::size_t len) -> void {
        put_u32(static_cast<std::uint32_t>(len));
        const std::size_t start = out.size();
        out.push_back(static_cast<std::uint8_t>(type[0])); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        out.push_back(static_cast<std::uint8_t>(type[1])); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        out.push_back(static_cast<std::uint8_t>(type[2])); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        out.push_back(static_cast<std::uint8_t>(type[3])); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        for (std::size_t i = 0; i < len; ++i) {
            out.push_back(data[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
        const std::uint32_t crc = crc32(&out[start], len + 4); // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        put_u32(crc);
    };

    // 签名
    const std::array<std::uint8_t, 8> sig = { 137, 80, 78, 71, 13, 10, 26, 10 };
    out.insert(out.end(), sig.begin(), sig.end());

    // IHDR
    std::array<std::uint8_t, 13> ihdr{};
    const auto w = static_cast<std::uint32_t>(width);
    const auto h = static_cast<std::uint32_t>(height);
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    ihdr[0] = static_cast<std::uint8_t>((w >> 24U) & 0xFFU);
    ihdr[1] = static_cast<std::uint8_t>((w >> 16U) & 0xFFU);
    ihdr[2] = static_cast<std::uint8_t>((w >> 8U) & 0xFFU);
    ihdr[3] = static_cast<std::uint8_t>(w & 0xFFU);
    ihdr[4] = static_cast<std::uint8_t>((h >> 24U) & 0xFFU);
    ihdr[5] = static_cast<std::uint8_t>((h >> 16U) & 0xFFU);
    ihdr[6] = static_cast<std::uint8_t>((h >> 8U) & 0xFFU);
    ihdr[7] = static_cast<std::uint8_t>(h & 0xFFU);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    put_chunk("IHDR", ihdr.data(), ihdr.size());

    // IDAT：zlib 头 + 单个 stored block（无压缩）+ adler32
    const std::size_t stride = (static_cast<std::size_t>(width) * 4U) + 1U; // 1 filter byte + RGBA
    const std::size_t raw_len = stride * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> raw;
    raw.reserve(raw_len);
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const std::uint8_t *row = rgba + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U);
        for (int x = 0; x < width * 4; ++x) {
            raw.push_back(row[x]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
    }

    std::vector<std::uint8_t> idat = build_idat(raw.data(), raw_len);

    const std::uint32_t ad = adler32(raw.data(), raw_len);
    idat.push_back(static_cast<std::uint8_t>((ad >> 24U) & 0xFFU));
    idat.push_back(static_cast<std::uint8_t>((ad >> 16U) & 0xFFU));
    idat.push_back(static_cast<std::uint8_t>((ad >> 8U) & 0xFFU));
    idat.push_back(static_cast<std::uint8_t>(ad & 0xFFU));
    put_chunk("IDAT", idat.data(), idat.size());

    // IEND
    put_chunk("IEND", nullptr, 0);

    return out;
}

} // namespace detail

/// @brief 将 RGBA8 像素编码并写入 PNG 文件（内置最小化编码器，零三方依赖）。
/// @return 成功返回空值；失败返回带信息的 Error。
[[nodiscard]] inline auto write_png(const char *path, int width, int height, const std::uint8_t *rgba) -> Result<bool> {
    auto bytes = detail::write_png_to_memory(rgba, width, height);
    if (!bytes) {
        return bytes.error();
    }
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return make_error(ErrorCode::IOFileNotFound, std::string("writePNG: cannot open ") + path);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    f.write(reinterpret_cast<const char *>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
    if (!f.good()) {
        return make_error(ErrorCode::IOFileNotFound, "writePNG: write incomplete");
    }
    return Result{ true };
}

} // namespace aurora
