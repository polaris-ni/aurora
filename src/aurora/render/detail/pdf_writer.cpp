#include "aurora/render/detail/pdf_writer.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"

namespace aurora::render::detail {

namespace {

/// @brief Adler-32 校验（RFC 1950 尾部）。
[[nodiscard]] auto adler32(std::span<const std::uint8_t> data) -> std::uint32_t {
    constexpr std::uint32_t modulus = 65521U;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % modulus;
        b = (b + a) % modulus;
    }
    return (b << 16U) | a;
}

/// @brief 追加小端 16 位（stored block 的 LEN / NLEN）。
auto append_le16(std::vector<std::uint8_t> &out, std::uint16_t value) -> void {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

/// @brief 追加大端 32 位（Adler-32 尾）。
auto append_be32(std::vector<std::uint8_t> &out, std::uint32_t value) -> void {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

/// @brief 数值格式化（固定 4 位小数、经典 locale：不产出逗号小数点）。
[[nodiscard]] auto format_number(double value) -> std::string {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(4) << value;
    std::string text = stream.str();
    // 去掉无意义的尾零（保留至少一位小数），让 golden 可读且稳定。
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.push_back('0');
        }
    }
    return text;
}

auto append(std::string &out, std::string_view text) -> void { out.append(text); }

}  // namespace

auto deflate_zlib_store(std::span<const std::uint8_t> input) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out;
    // zlib 头：CMF=0x78（deflate, 32K window），FLG=0x01（无字典、最低压缩级），(0x7801 % 31 == 0)。
    out.push_back(0x78U);
    out.push_back(0x01U);
    constexpr std::size_t max_block = 65535U;
    std::size_t offset = 0;
    do {
        const std::size_t chunk = std::min(max_block, input.size() - offset);
        const bool final_block = (offset + chunk) >= input.size();
        out.push_back(final_block ? 0x01U : 0x00U);  // BFINAL + BTYPE=00（stored）
        const auto length = static_cast<std::uint16_t>(chunk);
        append_le16(out, length);
        append_le16(out, static_cast<std::uint16_t>(~length & 0xFFFFU));
        if (chunk != 0) {
            out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(offset),
                       input.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        }
        offset += chunk;
    } while (offset < input.size());
    append_be32(out, adler32(input));
    return out;
}

auto build_pdf(double page_width_pt, double page_height_pt, const std::vector<PdfImagePage> &pages)
    -> Result<std::vector<std::uint8_t>> {
    if (pages.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument, "pdf: at least one page is required");
    }
    if (!(page_width_pt > 0.0) || !(page_height_pt > 0.0)) {
        return make_error(ErrorCode::GeneralInvalidArgument, "pdf: page size must be positive");
    }
    for (const auto &page : pages) {
        if (page.pixel_width <= 0 || page.pixel_height <= 0 || !(page.place_width_pt > 0.0) ||
            !(page.place_height_pt > 0.0)) {
            return make_error(ErrorCode::GeneralInvalidArgument, "pdf: page raster or placement size must be positive");
        }
        const auto expected =
            static_cast<std::size_t>(page.pixel_width) * static_cast<std::size_t>(page.pixel_height) * 3U;
        if (page.rgb.size() != expected) {
            return make_error(ErrorCode::IOImageEncodeFailed, "pdf: page raster length does not match width*height*3");
        }
    }

    std::string out;
    append(out, "%PDF-1.4\n");
    append(out, "%\xE2\xE3\xCF\xD3\n");  // 二进制标记：告诉阅读器这是二进制文档
    std::vector<std::size_t> offsets;
    offsets.push_back(0);  // 对象号从 1 起；0 号是空闲项
    const auto total_objects = 2U + (pages.size() * 3U);
    offsets.resize(total_objects + 1U, 0);

    const auto begin_object = [&out, &offsets](std::size_t number) {
        offsets[number] = out.size();
        out.append(std::to_string(number));
        out.append(" 0 obj\n");
    };
    const auto end_object = [&out] { out.append("endobj\n"); };

    // 1: Catalog
    begin_object(1);
    append(out, "<< /Type /Catalog /Pages 2 0 R >>\n");
    end_object();

    // 2: Pages
    begin_object(2);
    out.append("<< /Type /Pages /Kids [");
    for (std::size_t index = 0; index < pages.size(); ++index) {
        if (index != 0) {
            out.push_back(' ');
        }
        out.append(std::to_string(3U + (index * 3U)));
        out.append(" 0 R");
    }
    out.append("] /Count ");
    out.append(std::to_string(pages.size()));
    out.append(" >>\n");
    end_object();

    for (std::size_t index = 0; index < pages.size(); ++index) {
        const auto &page = pages[index];
        const auto page_number = 3U + (index * 3U);
        const auto content_number = page_number + 1U;
        const auto image_number = page_number + 2U;

        // content stream：只做一次图像绘制（无文本操作符——栅格入 PDF 的机械证据）。
        std::string content;
        content.append("q\n");
        content.append(format_number(page.place_width_pt));
        content.append(" 0 0 ");
        content.append(format_number(-page.place_height_pt));
        content.append(" ");
        content.append(format_number(page.place_x_pt));
        content.append(" ");
        content.append(format_number(page.place_y_pt + page.place_height_pt));
        content.append(" cm\n/Im0 Do\nQ\n");

        begin_object(page_number);
        out.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ");
        out.append(format_number(page_width_pt));
        out.append(" ");
        out.append(format_number(page_height_pt));
        out.append("] /Resources << /XObject << /Im0 ");
        out.append(std::to_string(image_number));
        out.append(" 0 R >> >> /Contents ");
        out.append(std::to_string(content_number));
        out.append(" 0 R >>\n");
        end_object();

        const auto compressed = deflate_zlib_store(std::span<const std::uint8_t>(page.rgb.data(), page.rgb.size()));

        begin_object(content_number);
        out.append("<< /Length ");
        out.append(std::to_string(content.size()));
        out.append(" >>\nstream\n");
        out.append(content);
        out.append("endstream\n");
        end_object();

        begin_object(image_number);
        out.append("<< /Type /XObject /Subtype /Image /Width ");
        out.append(std::to_string(page.pixel_width));
        out.append(" /Height ");
        out.append(std::to_string(page.pixel_height));
        out.append(" /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length ");
        out.append(std::to_string(compressed.size()));
        out.append(" >>\nstream\n");
        // 字节流写进文本容器：同一段内存的另一种视图，不经转换无处可绕。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        out.append(reinterpret_cast<const char *>(compressed.data()), compressed.size());
        out.append("\nendstream\n");
        end_object();
    }

    const std::size_t xref_offset = out.size();
    out.append("xref\n0 ");
    out.append(std::to_string(total_objects + 1U));
    out.append("\n");
    append(out, "0000000000 65535 f\r\n");
    for (std::size_t number = 1; number <= total_objects; ++number) {
        std::ostringstream entry;
        entry.imbue(std::locale::classic());
        entry << std::setw(10) << std::setfill('0') << offsets[number] << " 00000 n\r\n";
        out.append(entry.str());
    }
    append(out, "trailer\n<< /Size ");
    out.append(std::to_string(total_objects + 1U));
    append(out, " /Root 1 0 R >>\nstartxref\n");
    out.append(std::to_string(xref_offset));
    append(out, "\n%%EOF\n");
    return std::vector<std::uint8_t>(out.begin(), out.end());
}

auto write_pdf(const std::string &path, double page_width_pt, double page_height_pt,
               const std::vector<PdfImagePage> &pages) -> Result<void> {
    auto built = build_pdf(page_width_pt, page_height_pt, pages);
    if (!built) {
        return built.error();
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return make_error(ErrorCode::IOFileNotFound, {{"path", path}});
    }
    const auto &bytes = built.value();
    // `ofstream::write` 的形参是文本指针；字节缓冲与其同源，平台无关的替代路径不存在。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file.good()) {
        return make_error(ErrorCode::IOFileNotFound, {{"path", path}});
    }
    return Result<void>{};
}

}  // namespace aurora::render::detail
