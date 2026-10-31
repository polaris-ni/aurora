// tools/verify/document_output_live_probe.cpp — 文档输出与打印会话真机验收探针（非 CTest）。
//
// 覆盖：`export_pdf()`（include/aurora/app/document_output.h）与最小 PDF 写出器的整条真机通路，
// 以及 `print_targets()` 在真实系统打印服务上的行为面（空集合配成功 / 缺机制报 unsupported）。
//
// 为什么必须有本探针（无头单测证明不了的部分）：
//   * 单测只做结构断言；「导出的文件确实被外部 PDF 阅读器接受」是阅读器的事，只有人能判；
//   * 打印目标枚举触及真实后台处理程序（Windows spooler / macOS 打印系统），空机器与无服务
//     两种环境的区别只有真机才看得到；
//   * 本探针的取证链**独立于**写出器：自带 xref 偏移核对、stored zlib 块解析与 Adler-32 复算，
//     不读写出器的内部结构假定来判「文件是好的」。
//
// 构建（AURORA_BUILD_VERIFY_TOOLS=ON）：
//   cmake -S . -B build-verify -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_document_output
//   build-verify\aurora_verify_document_output.exe [--interactive]
//
// 自动段（无人值守）：
//   1. 能力位 `native_print_session` 与平台接线一致（Windows / macOS 桥为 true；Linux 为 false）；
//   2. `print_targets()`：有位 → 必须 Ok（空列表合法），无位 → 必须 `print-session-unsupported`；
//   3. 导出三页 PDF（A4、10mm 边距、72 dpi）到临时目录，并逐项核对：
//      头标识与版本、`/Count 3`、xref 偏移逐条指向 `N 0 obj`、三幅图像 XObject、
//      每幅 FlateDecode 流是合法 zlib（0x78 0x01 + stored 块 + Adler-32 复算）、解压字节数 =
//      `page_content_raster()` 读回的像素数 × 3、内容流无文本操作符。
// 人工段（`--interactive`）：打印 PDF 路径，由人用本机阅读器打开确认可读后按 y/n。
//
// 退出码：
//   0  自动段全部通过（未跑人工段时人工项如实打印为待确认）
//   1  自动段断言失败
//   2  环境不可用（临时目录建不出来）
//   7  人工段存在未确认项

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "aurora/app/document_output.h"
#include "aurora/core/color.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/render/painter.h"
#include "aurora/window/platform.h"
#include "verify_args.h"
#include "verify_shell.h"

namespace {

using aurora_verify::check;
using aurora_verify::emit;
using aurora_verify::note;

/// @brief 读整个文件为字节串（PDF 结构核对按字节游标读）。
/// @param path 文件路径。
/// @return 文件内容；打不开返回空串。
[[nodiscard]] auto read_file(const std::string &path) -> std::string {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// @brief 解析十进制无符号数（遇到非数字即停）。
/// @param text 文本。
/// @param pos 起始游标（函数返回时推进到数字之后）。
/// @return 数值；游标处不是数字时为 0。
[[nodiscard]] auto parse_unsigned(const std::string &text, std::size_t &pos) -> std::size_t {
    std::size_t value = 0;
    bool any = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = (value * 10U) + static_cast<std::size_t>(text[pos] - '0');
        ++pos;
        any = true;
    }
    return any ? value : 0;
}

/// @brief Adler-32 复算（RFC 1950 尾部）。
/// @param data 原始字节。
/// @return 校验值。
[[nodiscard]] auto adler32_of(std::span<const std::uint8_t> data) -> std::uint32_t {
    constexpr std::uint32_t modulus = 65521U;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const auto byte : data) {
        a = (a + byte) % modulus;
        b = (b + a) % modulus;
    }
    return (b << 16U) | a;
}

/// @brief 独立解析 zlib 包装的 stored DEFLATE 块，并复算 Adler-32 尾。
/// @param stream 待解析流。
/// @param out 解压结果。
/// @return 结构合法且校验一致时为 true。
[[nodiscard]] auto inflate_stored_checked(std::span<const std::uint8_t> stream, std::vector<std::uint8_t> &out)
    -> bool {
    if (stream.size() < 2U + 5U + 4U || stream[0] != 0x78U || stream[1] != 0x01U) {
        return false;
    }
    out.clear();
    std::size_t offset = 2U;
    bool final_block = false;
    while (!final_block && offset + 5U <= stream.size()) {
        const std::uint8_t header = stream[offset++];
        final_block = (header & 0x01U) != 0U;
        if (((header >> 1U) & 0x03U) != 0x00U) {
            return false;
        }
        const std::size_t length =
            static_cast<std::size_t>(stream[offset]) | (static_cast<std::size_t>(stream[offset + 1U]) << 8U);
        const std::size_t complement =
            static_cast<std::size_t>(stream[offset + 2U]) | (static_cast<std::size_t>(stream[offset + 3U]) << 8U);
        offset += 4U;
        if ((length ^ complement) != 0xFFFFU || offset + length > stream.size()) {
            return false;
        }
        const auto block = stream.subspan(offset, length);
        out.insert(out.end(), block.begin(), block.end());
        offset += length;
    }
    if (!final_block || offset + 4U > stream.size()) {
        return false;
    }
    const std::uint32_t expected =
        (static_cast<std::uint32_t>(stream[offset]) << 24U) | (static_cast<std::uint32_t>(stream[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(stream[offset + 2U]) << 8U) | static_cast<std::uint32_t>(stream[offset + 3U]);
    return expected == adler32_of(out);
}

/// @brief 核对 xref 表：每个在用项的偏移都指向对应的 `N 0 obj`。
/// @param pdf PDF 文本。
/// @return 结构正确且逐项命中的条数；结构不合法返回 0 并置 `ok` 为 false。
[[nodiscard]] auto verify_xref(const std::string &pdf, bool &ok) -> std::size_t {
    ok = false;
    const auto start = pdf.rfind("startxref\n");
    if (start == std::string::npos) {
        return 0;
    }
    std::size_t cursor = start + std::string("startxref\n").size();
    const std::size_t offset = parse_unsigned(pdf, cursor);
    if (offset == 0 || offset >= pdf.size() || pdf.compare(offset, 4U, "xref") != 0) {
        return 0;
    }
    cursor = offset + 4U;
    if (cursor >= pdf.size() || pdf[cursor] != '\n') {
        return 0;
    }
    ++cursor;
    if (cursor >= pdf.size() || pdf[cursor] != '0') {
        return 0;
    }
    while (cursor < pdf.size() && pdf[cursor] >= '0' && pdf[cursor] <= '9') {
        ++cursor;
    }
    if (cursor >= pdf.size() || pdf[cursor] != ' ') {
        return 0;
    }
    ++cursor;
    const std::size_t count = parse_unsigned(pdf, cursor);
    if (count < 2U) {
        return 0;
    }
    while (cursor < pdf.size() && (pdf[cursor] == '\n' || pdf[cursor] == '\r')) {
        ++cursor;
    }
    std::size_t verified = 0;
    for (std::size_t number = 0; number < count; ++number) {
        if (cursor + 20U > pdf.size()) {
            return 0;
        }
        std::size_t entry = cursor;
        const std::size_t entry_offset = parse_unsigned(pdf, entry);
        if (number != 0) {
            const std::string token = std::to_string(number) + " 0 obj";
            if (entry_offset == 0 || entry_offset + token.size() > pdf.size() ||
                pdf.compare(entry_offset, token.size(), token) != 0) {
                return 0;
            }
            ++verified;
        }
        cursor += 20U;
    }
    ok = true;
    return verified;
}

/// @brief 取 PDF 里每幅图像 XObject 的解压字节（独立解析 `/Length` 后按 zlib stored 块解）。
/// @param pdf PDF 文本。
/// @param out 解压结果（原地清空后填充）。
/// @return 结构合法的图像流幅数；任何一幅不合法即返回 0。
[[nodiscard]] auto image_streams(const std::string &pdf, std::vector<std::vector<std::uint8_t>> &out) -> std::size_t {
    out.clear();
    // PDF 文本与字节流共享同一段内存；`inflate_stored_checked` 吃只读字节视图。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::span<const std::uint8_t> pdf_bytes(reinterpret_cast<const std::uint8_t *>(pdf.data()), pdf.size());
    const std::string marker = "/Filter /FlateDecode /Length ";
    std::size_t pos = 0;
    while ((pos = pdf.find(marker, pos)) != std::string::npos) {
        const auto object_start = pdf.rfind(" 0 obj\n", pos);
        if (object_start == std::string::npos || pdf.find("/Subtype /Image", object_start) > pos) {
            return 0;
        }
        std::size_t cursor = pos + marker.size();
        const auto length = parse_unsigned(pdf, cursor);
        const auto stream_marker = pdf.find("\nstream\n", cursor);
        if (stream_marker == std::string::npos || length == 0) {
            return 0;
        }
        const auto data_start = stream_marker + std::string("\nstream\n").size();
        if (data_start + length > pdf.size()) {
            return 0;
        }
        std::vector<std::uint8_t> plain;
        if (!inflate_stored_checked(pdf_bytes.subspan(data_start, length), plain)) {
            return 0;
        }
        out.push_back(std::move(plain));
        pos = data_start + length;
    }
    return out.size();
}

/// @brief 统计内容流里是否出现独立成词的文本操作符。
/// @param pdf PDF 文本。
/// @return 命中数量（0 = 纯图像内容流）。
[[nodiscard]] auto text_operator_hits(const std::string &pdf) -> std::size_t {
    std::size_t hits = 0;
    for (const char *name : {"BT", "ET", "Tj", "TJ", "Tf"}) {
        std::size_t pos = 0;
        while ((pos = pdf.find(name, pos)) != std::string::npos) {
            const bool left = pos == 0U || pdf[pos - 1U] == ' ' || pdf[pos - 1U] == '\n';
            const auto after = pos + std::string(name).size();
            const bool right = after >= pdf.size() || pdf[after] == ' ' || pdf[after] == '\n';
            if (left && right) {
                ++hits;
            }
            pos += 1U;
        }
    }
    return hits;
}

/// @brief 自动段：能力位 / 打印目标 / PDF 结构与流核对。
/// @param exe 可执行路径（写进日志）。
/// @param pdf_path 成功导出时写入 PDF 路径。
/// @return 进程退出码（0 / 1 / 2）。
auto run_auto_stage(const std::string &exe, std::string &pdf_path) -> int {
    emit("== Aurora document output live probe ==");
    emit("executable: " + exe);

    const auto caps = aurora::platform().capabilities();
#if defined(AURORA_PLATFORM_WINDOWS) || (defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS))
    constexpr bool expected_print = true;
#else
    constexpr bool expected_print = false;
#endif
    check(caps.native_print_session == expected_print,
          std::string("[capability] native_print_session == ") + (expected_print ? "true" : "false"));

    auto targets = aurora::print_targets();
    if (expected_print) {
        check(targets.ok(), "[print] print_targets() returns Ok on a platform with a native print session");
        if (targets.ok()) {
            note("[print] printer count = " + std::to_string(targets.value().size()));
            for (const auto &target : targets.value()) {
                note("[print] target: " + target.name + (target.is_default ? " (default)" : ""));
            }
        } else {
            note("[print] error: " + targets.error().code + " / " + targets.error().message);
        }
    } else {
        const bool unsupported =
            !targets.ok() && targets.error().code_enum == aurora::ErrorCode::PrintSessionUnsupported;
        check(unsupported, "[print] print_targets() reports print-session-unsupported without a native bridge");
        if (!targets.ok()) {
            note("[print] error: " + targets.error().code + " / " + targets.error().message);
        }
    }

    const std::string directory = aurora_verify::make_probe_directory("aurora-document-output");
    if (directory.empty()) {
        aurora_verify::env_fail("[env] cannot create a probe directory under the system temp directory",
                                "check TMP / TEMP permissions and free space");
        return 2;
    }
    pdf_path = (std::filesystem::path(directory) / "aurora-document-output-probe.pdf").string();

    aurora::PageSpec spec;
    spec.width_mm = 210.0;
    spec.height_mm = 297.0;
    spec.margins = aurora::PageMargins{.top_mm = 10.0, .right_mm = 10.0, .bottom_mm = 10.0, .left_mm = 10.0};
    spec.dpi = 72;  // stored zlib 不压缩：72 dpi 让三页文件仍在数 MB 量级
    auto raster = aurora::page_content_raster(spec);
    check(raster.ok(), "[raster] page_content_raster() succeeds for the probe page description");
    if (!raster.ok()) {
        return 1;
    }
    const std::size_t expected_bytes =
        static_cast<std::size_t>(raster.value().width_px) * static_cast<std::size_t>(raster.value().height_px) * 3U;
    note("[raster] content raster = " + std::to_string(raster.value().width_px) + " x " +
         std::to_string(raster.value().height_px) + " px (scale " + std::to_string(raster.value().scale) + ")");

    const auto width_dp = static_cast<float>(raster.value().width_dp);
    const auto height_dp = static_cast<float>(raster.value().height_dp);
    auto exported =
        aurora::export_pdf(pdf_path, spec, 3U, [width_dp, height_dp](std::size_t page, aurora::Painter &painter) {
            const float inset = 24.0F + (static_cast<float>(page) * 24.0F);
            painter.fill_rect(
                aurora::Rect{
                    .origin = aurora::Point{.x = inset, .y = inset},
                    .size = aurora::Size{.width = width_dp - (inset * 2.0F), .height = height_dp - (inset * 2.0F)}},
                page == 0U ? aurora::Color::red() : (page == 1U ? aurora::Color::green() : aurora::Color::blue()));
        });
    check(exported.ok(), "[pdf] export_pdf() writes a three-page document");
    if (!exported.ok()) {
        note("[pdf] error: " + exported.error().code + " / " + exported.error().message);
        return 1;
    }

    const std::string pdf = read_file(pdf_path);
    check(pdf.size() > 1024U, "[pdf] exported file is non-trivial (" + std::to_string(pdf.size()) + " bytes)");
    check(pdf.starts_with("%PDF-1.4\n"), "[pdf] starts with the %PDF-1.4 header");
    check(pdf.find("%%EOF") != std::string::npos, "[pdf] carries the %%EOF trailer marker");
    check(pdf.find("/Count 3") != std::string::npos, "[pdf] page tree declares /Count 3");
    check(pdf.find("/MediaBox [0 0 595.2756 841.8898]") != std::string::npos,
          "[pdf] A4 MediaBox is expressed in PostScript points");
    check(pdf.find("/ColorSpace /DeviceRGB") != std::string::npos, "[pdf] images declare DeviceRGB");
    check(pdf.find("/Im0 Do") != std::string::npos, "[pdf] page content paints the image XObject");

    bool xref_ok = false;
    const auto xref_entries = verify_xref(pdf, xref_ok);
    check(xref_ok, "[pdf] xref table offsets resolve to object headers (" + std::to_string(xref_entries) + " entries)");

    std::vector<std::vector<std::uint8_t>> streams;
    const auto stream_count = image_streams(pdf, streams);
    check(stream_count == 3U, "[pdf] exactly three image XObject streams (" + std::to_string(stream_count) + ")");
    bool streams_ok = stream_count == 3U;
    for (const auto &stream : streams) {
        streams_ok = streams_ok && stream.size() == expected_bytes;
    }
    check(streams_ok, "[pdf] every FlateDecode stream inflates to width*height*3 bytes with a valid Adler-32");
    check(text_operator_hits(pdf) == 0U, "[pdf] no text operators anywhere in the document (raster-only evidence)");

    note("[pdf] output: " + pdf_path);
    return 0;
}

/// @brief 人工段：由人用本机 PDF 阅读器打开并确认可读。
/// @param pdf_path 导出的 PDF 路径。
/// @return 0 = 已确认；7 = 未确认 / 无输入。
auto run_interactive_stage(const std::string &pdf_path) -> int {
    emit("---- interactive segment ----");
    emit("Open the following file with the PDF reader installed on this machine:");
    emit("  " + pdf_path);
    emit("Confirm that all three pages render (red / green / blue insets) and no page is blank.");
    emit("Type y to confirm, anything else to leave the item pending: ");
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return 7;
    }
    for (char &ch : answer) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    if (!answer.empty() && answer.front() == 'y') {
        emit("[PASS] an external reader opened the exported PDF");
        return 0;
    }
    emit("[PEND] external-reader confirmation not given; the item stays unverified");
    return 7;
}

}  // namespace

// 探针入口不做异常兜底：任何抛出都该让进程带非零码终止，而不是被吞成「通过了」。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Document output (raster PDF) live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    aurora::init_console();

    const std::string exe = aurora_verify::executable_path(argc > 0 ? *argv : nullptr);
    std::string pdf_path;
    const int auto_code = run_auto_stage(exe, pdf_path);
    if (auto_code != 0) {
        return auto_code;
    }
    if (!interactive) {
        emit("Skipping the interactive stage: whether an external reader accepts the file is a human judgement");
        emit("(re-run with --interactive).");
        emit(aurora_verify::env_unavailable() ? "ENV-UNAVAILABLE" : "PASS (auto stage)");
        return aurora_verify::env_unavailable() ? 2 : (aurora_verify::failures() == 0 ? 0 : 1);
    }
    const int human_code = run_interactive_stage(pdf_path);
    if (aurora_verify::failures() != 0) {
        return 1;
    }
    if (aurora_verify::env_unavailable()) {
        return 2;
    }
    return human_code;
}
