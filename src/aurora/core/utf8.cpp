#include "aurora/core/utf8.h"

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS

#include <windows.h>

namespace aurora::internal {

auto utf8_to_wstr(const std::string &s) -> std::wstring {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

auto wstr_to_utf8(const wchar_t *ws) -> std::string {
    if (ws == nullptr || *ws == L'\0') {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n - 1));
    return s;
}

} // namespace aurora::internal
#endif

// 跨平台纯逻辑：Unicode 码点 ↔ UTF-8（1~4 字节）。不依赖平台 API，非 AURORA_PLATFORM_WINDOWS 也可编译验证。
#include <array>
#include <cstdint>

namespace aurora {

auto utf8_encode(std::uint32_t cp) -> std::string {
    // 权威版（取 glfw_surface 原 utf8_from_codepoint，完整 Unicode 含辅助平面）。
    if (cp <= 0x7F) {
        return { 1, static_cast<char>(cp) };
    }
    if (cp <= 0x7FF) {
        const std::array b{ static_cast<char>(0xC0U | (cp >> 6U)), static_cast<char>(0x80U | (cp & 0x3FU)) };
        return { b.data(), b.size() };
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        return {}; // UTF-16 代理区不是合法 Unicode 标量值，编码将产出非 UTF-8 字节序列，拒绝
    }
    if (cp <= 0xFFFF) {
        const std::array b{
            static_cast<char>(0xE0U | (cp >> 12U)),
            static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)),
            static_cast<char>(0x80U | (cp & 0x3FU)),
        };
        return { b.data(), b.size() };
    }
    if (cp <= 0x10FFFF) {
        const std::array b{
            static_cast<char>(0xF0U | (cp >> 18U)),
            static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)),
            static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)),
            static_cast<char>(0x80U | (cp & 0x3FU)),
        };
        return { b.data(), b.size() };
    }
    return {};
}

auto utf8_cp_len(unsigned char c) -> int {
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0U) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0U) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8U) == 0xF0) {
        return 4;
    }
    return 1;
}

auto utf8_cp_count(const std::string &s) -> std::size_t {
    std::size_t count = 0;
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        i += static_cast<std::size_t>(utf8_cp_len(static_cast<unsigned char>(s[i])));
        ++count;
    }
    return count;
}

auto utf8_cp_slice(const std::string &s, std::size_t start, std::size_t count) -> std::string {
    std::size_t i = 0;
    std::size_t cp = 0;
    const std::size_t n = s.size();
    // 定位 start 码点起始字节
    while (i < n && cp < start) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        i += static_cast<std::size_t>(utf8_cp_len(static_cast<unsigned char>(s[i])));
        ++cp;
    }
    if (i >= n) {
        return {};
    }
    // 取 count 个码点
    std::size_t end = i;
    std::size_t taken = 0;
    while (end < n && taken < count) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        end += static_cast<std::size_t>(utf8_cp_len(static_cast<unsigned char>(s[end])));
        ++taken;
    }
    return s.substr(i, end - i);
}

} // namespace aurora
