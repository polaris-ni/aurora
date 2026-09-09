/// 测试类型: unit
/// 目标单元: include/aurora/core/utf8.h
/// 测试说明: 覆盖码点↔UTF-8 编码（ASCII/2/3/4 字节与代理区/越界拒绝）、序列长度判定、码点计数与切片，以及 Win32
/// 宽串往返

#include <cstdint>
#include <string>

#include "aurora/core/utf8.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_utf8 {

AURORA_TEST_CASE(encode_ascii_and_multibyte_ranges) {
    // 1 字节：ASCII。
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode('A'), "A");
    // 2 字节：U+00E9 é → C3 A9。
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0xE9), std::string{"\xC3\xA9"});
    // 3 字节：U+4E2D 中 → E4 B8 AD。
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0x4E2D), std::string{"\xE4\xB8\xAD"});
    // 4 字节：U+1F600 emoji → F0 9F 98 80（辅助平面）。
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0x1F600), std::string{"\xF0\x9F\x98\x80"});
}

AURORA_TEST_CASE(encode_boundary_values_of_each_range) {
    // 各区间端点：0x7F（1 字节末）、0x80（2 字节首）、0x7FF（2 字节末）、0x800（3 字节首）、
    // 0xFFFF（3 字节末）、0x10000（4 字节首）、0x10FFFF（Unicode 上界）。
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0x7F).size(), 1U);
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0x80).size(), 2U);
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0x7FF).size(), 2U);
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0x800).size(), 3U);
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0xFFFF).size(), 3U);
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0x10000).size(), 4U);
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0x10FFFF).size(), 4U);
}

AURORA_TEST_CASE(encode_rejects_surrogates_and_out_of_range) {
    // UTF-16 代理区（D800..DFFF）不是合法 Unicode 标量值：契约要求返回空串。
    AURORA_TEST_CHECK(aurora::utf8_encode(0xD800).empty());
    AURORA_TEST_CHECK(aurora::utf8_encode(0xDFFF).empty());
    // 超出 Unicode 上界：返回空串。
    AURORA_TEST_CHECK(aurora::utf8_encode(0x110000).empty());
    AURORA_TEST_CHECK(aurora::utf8_encode(0xFFFFFFFFU).empty());
    // 码点 0 是合法 1 字节序列（NUL 字节），仅以长度验证避免嵌 NUL 的字符串比较歧义。
    AURORA_TEST_CHECK_EQ(aurora::utf8_encode(0).size(), 1U);
}

AURORA_TEST_CASE(cp_len_classifies_lead_bytes) {
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_len(0x41), 1);  // ASCII
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_len(0xC3), 2);  // 2 字节首字节
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_len(0xE4), 3);  // 3 字节首字节
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_len(0xF0), 4);  // 4 字节首字节
    // 非法首字节（续字节 0x80 / 0xF8 以上）：契约规定按 1 处理（与解码退化一致）。
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_len(0x80), 1);
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_len(0xF8), 1);
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_len(0xFF), 1);
}

AURORA_TEST_CASE(cp_count_walks_codepoints_not_bytes) {
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_count(""), 0U);
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_count("abc"), 3U);
    // "中z"：3 字节中文 + 1 字节 ASCII → 2 个码点。
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_count(std::string{"\xE4\xB8\xADz"}), 2U);
    // 4 字节 emoji 单独成 1 个码点。
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_count(std::string{"\xF0\x9F\x98\x80"}), 1U);
    // 非法首字节按 1 退化推进：3 个字节计 3 个码点，不越界、不死循环。
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_count(std::string{"\xF8\x80\x80"}), 3U);
}

AURORA_TEST_CASE(cp_slice_extracts_codepoint_ranges) {
    const std::string text{"a\xE4\xB8\xADz"};  // "a中z"，5 字节 3 码点
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_slice(text, 0, 3), text);  // 全量
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_slice(text, 1, 1), std::string{"\xE4\xB8\xAD"});
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_slice(text, 0, 2), std::string{"a\xE4\xB8\xAD"});
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_slice(text, 2, 1), "z");
    AURORA_TEST_CHECK_EQ(aurora::utf8_cp_slice(text, 1, 10), std::string{"\xE4\xB8\xADz"});  // count 越界截断
    AURORA_TEST_CHECK(aurora::utf8_cp_slice(text, 0, 0).empty());  // 0 个码点
    AURORA_TEST_CHECK(aurora::utf8_cp_slice(text, 3, 1).empty());  // start 越界
    AURORA_TEST_CHECK(aurora::utf8_cp_slice("", 0, 1).empty());  // 空串
}

AURORA_TEST_CASE(win32_wide_conversion_roundtrip) {
#ifndef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_SKIP("utf8_to_wstr/wstr_to_utf8 仅在 AURORA_PLATFORM_WINDOWS 编译");
#else
    // 空输入契约：空串 → 空 wstring；空指针 → 空 string。
    AURORA_TEST_CHECK(aurora::internal::utf8_to_wstr("").empty());
    AURORA_TEST_CHECK(aurora::internal::wstr_to_utf8(nullptr).empty());

    // ASCII 与多字节 UTF-8（中文 + emoji）往返一致。
    const std::string ascii{"aurora"};
    AURORA_TEST_CHECK_EQ(aurora::internal::utf8_to_wstr(ascii), std::wstring{L"aurora"});
    AURORA_TEST_CHECK_EQ(aurora::internal::wstr_to_utf8(L"aurora"), ascii);

    const std::string mixed{"aurora\xe4\xb8\xad\xf0\x9f\x98\x80"};  // "aurora中🚀"
    const auto wide = aurora::internal::utf8_to_wstr(mixed);
    AURORA_TEST_REQUIRE_FALSE(wide.empty());
    AURORA_TEST_CHECK_EQ(aurora::internal::wstr_to_utf8(wide.c_str()), mixed);
#endif
}

}  // namespace aurora::test_cases::utest_utf8
