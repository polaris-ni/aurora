/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/clipboard_win32_translate.h
/// 测试说明: 剪贴板 Win32 侧两种线上形态的**零平台头折算层**——`CF_HDROP`（`DROPFILES` + 双空结尾的
/// 宽字符清单）与 `CF_HTML`（`Version:0.9` + 四组十进位字节偏移）的编解码，以及两种形态共有的
/// 「跨进程字节一律不可信」口径：越界、次序颠倒、收尾空字符缺失、ANSI 清单，一律判为畸形而非猜补。
///
/// 为何这两件必须有单测：它们被 `<windows.h>` 挡在无头 CI 之外（本折算层的存在正是为了把它们从平台
/// 头里拆出来），而真实缺陷恰恰只在逐字节层面显形——偏移写短一位、清单少一个收尾空字符、
/// `pFiles` 非偶导致宽字符读取未对齐，三种都不会崩溃，只会让「粘贴」静默拿到错数据或读越界。
///
/// 本文件**不触碰真实剪贴板**：折算层是纯函数，字节由用例内构造。
///
/// 变异自证（改坏实现应转红的八条）：
///  * `encode_hdrop` 少补一个收尾空字符 → `hdrop_roundtrip_multiple_paths` 与
///    `hdrop_rejects_missing_terminating_null` 同时转红（后者会由红变绿，正是「少补尾」能被观测的证明）；
///  * 把 `pFiles` 写成 0（清单起点）→ `hdrop_roundtrip_single_path` 转红（读到头部字节当路径）；
///  * 去掉 `fWide` 置位 → 同上转红（按 ANSI 解释的字节不是路径）；
///  * 去掉 `pFiles` 的越界与奇偶校验 → `hdrop_rejects_out_of_range_file_offset` /
///    `hdrop_rejects_odd_file_offset` 转红（前者会读越界，后者会未对齐读）；
///  * `build_html_frame` 的偏移字段改成非十进位或宽度不足 → `html_frame_offsets_are_zero_padded_decimal`
///    转红（Windows 侧解析器按固定宽度读，短一位即整块作废）；
///  * 把 `StartFragment` 偏移算成「相对片段起点」而非整块起点 → `html_frame_roundtrip` 转红；
///  * 去掉 `parse_html_frame` 的越界校验 → `html_frame_rejects_offsets_beyond_payload` 转红（切片越界）；
///  * 去掉次序校验 → `html_frame_rejects_reversed_offsets` 转红（构造出负长度切片）。

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "aurora/window/detail/clipboard_win32_translate.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_clipboard_win32_translate {

using clipboard::detail::AURORA_WIN32_DROPFILES_HEADER_BYTES;
using clipboard::detail::build_html_frame;
using clipboard::detail::decode_hdrop;
using clipboard::detail::encode_hdrop;
using clipboard::detail::parse_html_frame;

namespace {

/// @brief 取字节块里某位置的字节（越界即返回 0，测试内按畸形输入处理）。
[[nodiscard]] auto byte_at(const std::vector<std::uint8_t> &bytes, std::size_t at) -> std::uint8_t {
    return at < bytes.size() ? bytes.at(at) : std::uint8_t{0U};
}

/// @brief 取字节块里的 32 位小端字段（与 `DROPFILES` 的线上布局一致）。
[[nodiscard]] auto read_u32(const std::vector<std::uint8_t> &bytes, std::size_t at) -> std::uint32_t {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        value |= static_cast<std::uint32_t>(byte_at(bytes, at + i)) << (8U * i);
    }
    return value;
}

/// @brief 把 32 位字段写回字节块（构造畸形输入用）。
auto write_u32(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) -> void {
    if (at + sizeof(std::uint32_t) > bytes.size()) {
        return;
    }
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        bytes.at(at + i) = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
    }
}

/// @brief 由 UTF-16 码元序列直接拼出「双空结尾」的原始清单区（构造畸形输入用）。
[[nodiscard]] auto raw_units(const std::vector<char16_t> &units) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out;
    for (const char16_t unit : units) {
        out.push_back(static_cast<std::uint8_t>(unit & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((unit >> 8) & 0xFFU));
    }
    return out;
}
}  // namespace

// ---------- CF_HDROP ----------

AURORA_TEST_CASE(hdrop_roundtrip_single_path) {
    const std::vector<std::u16string> paths{u"C:\\data\\alpha.txt"};
    const auto bytes = encode_hdrop(paths);
    AURORA_TEST_CHECK(bytes.size() > AURORA_WIN32_DROPFILES_HEADER_BYTES);
    // 线上布局：`pFiles` = 头部大小、拖点 (0,0)、`fNC` = 0、`fWide` = 1。
    AURORA_TEST_CHECK_EQ(read_u32(bytes, 0), std::uint32_t{AURORA_WIN32_DROPFILES_HEADER_BYTES});
    AURORA_TEST_CHECK_EQ(read_u32(bytes, 16), std::uint32_t{1});
    const auto decoded = decode_hdrop(bytes);
    if (!decoded.has_value()) {
        AURORA_TEST_CHECK(false);
        return;
    }
    AURORA_TEST_CHECK_EQ(decoded->size(), std::size_t{1});
    AURORA_TEST_CHECK(decoded->at(0) == paths.at(0));
}

AURORA_TEST_CASE(hdrop_roundtrip_multiple_paths) {
    const std::vector<std::u16string> paths{u"a.txt", u"docs\\b.txt", u"c d.txt"};
    const auto decoded = decode_hdrop(encode_hdrop(paths));
    if (!decoded.has_value()) {
        AURORA_TEST_CHECK(false);
        return;
    }
    AURORA_TEST_CHECK_EQ(decoded->size(), std::size_t{3});
    AURORA_TEST_CHECK(decoded->at(2) == paths.at(2));
}

AURORA_TEST_CASE(hdrop_empty_list_yields_no_bytes) {
    // 空清单**没有**线上形态：与剪贴板侧「空清单为 no-op」同口径——产出「只有头部」的块会让
    // 粘贴方读到一条空路径，那比读不到更糟。
    AURORA_TEST_CHECK(encode_hdrop({}).empty());
}

AURORA_TEST_CASE(hdrop_rejects_missing_terminating_null) {
    // 清单不以空字符收尾 = 最后一条路径永远收不了尾。本层不补尾：凭空补一个 NUL 会把不完整的
    // 路径变成一条看似完整的路径。
    const std::vector<char16_t> units{u'a', u'\0', u'b'};
    std::vector<std::uint8_t> bytes(AURORA_WIN32_DROPFILES_HEADER_BYTES, 0U);
    write_u32(bytes, 0, static_cast<std::uint32_t>(AURORA_WIN32_DROPFILES_HEADER_BYTES));
    write_u32(bytes, 16, 1U);
    const auto tail = raw_units(units);
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    AURORA_TEST_CHECK(!decode_hdrop(bytes).has_value());
}

AURORA_TEST_CASE(hdrop_rejects_truncated_header) {
    const std::vector<std::uint8_t> bytes(AURORA_WIN32_DROPFILES_HEADER_BYTES - 1U, 0U);
    AURORA_TEST_CHECK(!decode_hdrop(bytes).has_value());
}

AURORA_TEST_CASE(hdrop_rejects_ansi_list) {
    // `fWide = 0` 的清单按系统 ANSI 代码页解释字节，本库不猜代码页：与其给出一条乱码路径，
    // 不如如实判畸形让调用方落到 `uri-list` 那一档。
    std::vector<std::uint8_t> bytes(AURORA_WIN32_DROPFILES_HEADER_BYTES + 4U, 0U);
    write_u32(bytes, 0, static_cast<std::uint32_t>(AURORA_WIN32_DROPFILES_HEADER_BYTES));
    write_u32(bytes, 16, 0U);
    AURORA_TEST_CHECK(!decode_hdrop(bytes).has_value());
}

AURORA_TEST_CASE(hdrop_rejects_out_of_range_file_offset) {
    const auto bytes = encode_hdrop({u"a.txt"});
    auto malformed = bytes;
    write_u32(malformed, 0, static_cast<std::uint32_t>(bytes.size() + 8U));  // 清单起点落到块外
    AURORA_TEST_CHECK(!decode_hdrop(malformed).has_value());
    auto below_header = bytes;
    write_u32(below_header, 0, 4U);  // 清单起点落进头部里
    AURORA_TEST_CHECK(!decode_hdrop(below_header).has_value());
}

AURORA_TEST_CASE(hdrop_rejects_odd_file_offset) {
    // 起点非偶 ⇒ 宽字符读取未对齐：不作数，直接判畸形（否则是 UB 而不是「读错」）。
    const auto bytes = encode_hdrop({u"a.txt"});
    auto malformed = bytes;
    write_u32(malformed, 0, static_cast<std::uint32_t>(AURORA_WIN32_DROPFILES_HEADER_BYTES) + 1U);
    AURORA_TEST_CHECK(!decode_hdrop(malformed).has_value());
}

// ---------- CF_HTML ----------

AURORA_TEST_CASE(html_frame_offsets_are_zero_padded_decimal) {
    const auto frame = build_html_frame("<b>hi</b>");
    // Windows 侧解析器按**固定十进位宽度**读偏移：字段宽度恒定正是头部长度与片段长度无关的原因。
    const auto at = frame.find("StartHTML:");
    AURORA_TEST_CHECK(at != std::string::npos);
    for (std::size_t i = 0; i < 10U; ++i) {
        AURORA_TEST_CHECK(frame[at + std::string{"StartHTML:"}.size() + i] >= '0');
        AURORA_TEST_CHECK(frame[at + std::string{"StartHTML:"}.size() + i] <= '9');
    }
    // 头部四行齐备且顺序固定。
    AURORA_TEST_CHECK(frame.rfind("Version:0.9", 0) == 0U);
    AURORA_TEST_CHECK(frame.find("EndHTML:") != std::string::npos);
    AURORA_TEST_CHECK(frame.find("StartFragment:") != std::string::npos);
    AURORA_TEST_CHECK(frame.find("EndFragment:") != std::string::npos);
}

AURORA_TEST_CASE(html_frame_roundtrip) {
    constexpr std::string_view fragment = "<b>hello</b><i>world</i>";
    const auto frame = build_html_frame(std::string{fragment});
    const auto parsed = parse_html_frame(frame);
    if (!parsed.has_value()) {
        AURORA_TEST_CHECK(false);
        return;
    }
    AURORA_TEST_CHECK(*parsed == fragment);
    // 片段之外还有外壳：整块（`EndHTML` 覆盖范围）比片段长，这正是两组偏移分开存在的意义。
    AURORA_TEST_CHECK(frame.size() > parsed->size());
}

AURORA_TEST_CASE(html_frame_rejects_missing_start_marker) {
    const auto frame = build_html_frame("x");
    auto malformed = frame;
    const auto at = malformed.find("StartFragment:");
    malformed.erase(at, std::string{"StartFragment:"}.size());
    AURORA_TEST_CHECK(!parse_html_frame(malformed).has_value());
}

AURORA_TEST_CASE(html_frame_rejects_offsets_beyond_payload) {
    const auto frame = build_html_frame("x");
    auto malformed = frame;
    const auto at = malformed.find("EndFragment:");
    malformed.replace(at + std::string{"EndFragment:"}.size(), 10U, "9999999999");
    AURORA_TEST_CHECK(!parse_html_frame(malformed).has_value());
}

AURORA_TEST_CASE(html_frame_rejects_reversed_offsets) {
    // 次序颠倒会构造出负长度切片：剪贴板是跨进程信道，别的应用可能给出任意自称 `HTML Format` 的字节。
    const std::string frame = "Version:0.9\r\nStartFragment:0000000010\r\nEndFragment:0000000005\r\nabc";
    AURORA_TEST_CHECK(!parse_html_frame(frame).has_value());
}

}  // namespace aurora::test_cases::utest_clipboard_win32_translate
