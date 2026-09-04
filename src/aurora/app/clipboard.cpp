#include "aurora/app/clipboard.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "aurora/core/image.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN // NOLINT(readability-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
#include <array>
#include <cstdio>
#endif

#ifdef AURORA_PLATFORM_MACOS
#include <array>
#include <cstdio>
#endif

namespace aurora {

#if (defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)) || defined(AURORA_PLATFORM_MACOS)
namespace {
/// @brief 执行命令并向其 stdin 写入数据（用于 xclip/pbcopy 写入剪贴板）。
/// @return 命令是否成功执行。
auto pipe_to_command(const std::string &cmd, const std::string &data) -> bool {
    FILE *pipe = popen(cmd.c_str(), "w");
    if (!pipe) return false;
    if (!data.empty()) {
        std::fwrite(data.data(), 1, data.size(), pipe);
    }
    const int ret = pclose(pipe);
    return ret == 0;
}

/// @brief 执行命令并读取其 stdout（用于 xsel/pbpaste 读取剪贴板）。
/// @return 命令输出内容（UTF-8）。
auto read_from_command(const std::string &cmd) -> std::string {
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string result;
    std::array<char, 4096> buf{};
    while (auto *s = std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        result += s;
    }
    const int ret = pclose(pipe);
    return (ret == 0) ? result : std::string{};
}
} // namespace
#endif

auto Clipboard::set_text(const std::string &text) -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    if (text.empty()) {
        return;
    }
    if (OpenClipboard(nullptr) == 0) {
        AURORA_LOG_WARN("clipboard", "OpenClipboard failed");
        return;
    }
    EmptyClipboard();
    const int wchar_len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wchar_len > 0) {
        if (const HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wchar_len) * sizeof(wchar_t))) {
            if (auto *p = static_cast<wchar_t *>(GlobalLock(h))) {
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, p, wchar_len);
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            } else {
                GlobalFree(h);
            }
        }
    }
    CloseClipboard();
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    if (text.empty()) return;
    // xclip 需 -selection clipboard（默认 primary 为中键粘贴）；回退 xsel --clipboard
    if (!pipe_to_command("xclip -selection clipboard 2>/dev/null", text)) {
        if (!pipe_to_command("xsel --clipboard --input", text)) {
            AURORA_LOG_WARN("clipboard", "set_text: xclip/xsel not available");
        }
    }
#elif defined(AURORA_PLATFORM_MACOS)
    if (text.empty()) return;
    if (!pipe_to_command("pbcopy", text)) {
        AURORA_LOG_WARN("clipboard", "set_text: pbcopy failed");
    }
#else
    (void)text;
    AURORA_LOG_DEBUG("clipboard", "set_text no-op on this platform");
#endif
}

auto Clipboard::get_text() -> std::string {
#ifdef AURORA_PLATFORM_WINDOWS
    if (OpenClipboard(nullptr) == 0) {
        AURORA_LOG_WARN("clipboard", "OpenClipboard failed (get_text)");
        return {};
    }
    std::string result;
    if (const HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto *w = static_cast<const wchar_t *>(GlobalLock(h))) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                result.resize(static_cast<size_t>(len) - 1);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, result.data(), len, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return result;
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    // xclip 输出可能含尾随换行；xsel --clipboard 更干净
    auto r = read_from_command("xsel --clipboard --output 2>/dev/null");
    if (r.empty()) {
        r = read_from_command("xclip -selection clipboard -o 2>/dev/null");
    }
    return r;
#elif defined(AURORA_PLATFORM_MACOS)
    return read_from_command("pbpaste");
#else
    AURORA_LOG_DEBUG("clipboard", "get_text no-op on this platform");
    return {};
#endif
}

auto Clipboard::set_image(const Image &img) -> void {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast): Win32
    // HGLOBAL 字节搬运不可避免
#ifdef AURORA_PLATFORM_WINDOWS
    if (img.width <= 0 || img.height <= 0 || img.pixels.empty()) {
        return; // 空图像早退（不清除已有内容）
    }
    // 剪贴板载荷上限 + 64 位尺寸算术：stride*h 用 32 位在 w/h ≥ 2^15 量级时回绕，
    // 分配出过小的堆块，后续逐像素拷贝即越界写。同时校验像素缓冲与维度一致，
    // 防止构造不一致的 Image 导致越界读。
    constexpr std::int64_t k_max_clipboard_dim = 16384;
    if (img.width > k_max_clipboard_dim || img.height > k_max_clipboard_dim ||
        img.pixels.size() != static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4U) {
        AURORA_LOG_WARN("clipboard", "set_image: image too large or pixel buffer size mismatch");
        return; // 早退，不清除已有内容
    }
    if (OpenClipboard(nullptr) == 0) {
        AURORA_LOG_WARN("clipboard", "OpenClipboard failed (set_image)");
        return;
    }
    EmptyClipboard();
    const int w = img.width;
    const int h = img.height;
    const std::uint64_t stride64 = static_cast<std::uint64_t>(w) * 4U;  // 32bpp
    const std::uint64_t dib_size64 = sizeof(BITMAPINFOHEADER) + (stride64 * static_cast<std::uint64_t>(h));
    const HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(dib_size64));
    if (hg == nullptr) {
        CloseClipboard();
        return;
    }
    auto *p = static_cast<std::uint8_t *>(GlobalLock(hg));
    if (p == nullptr) {
        GlobalFree(hg);
        CloseClipboard();
        return;
    }
    auto *bi = reinterpret_cast<BITMAPINFOHEADER *>(p);
    std::memset(bi, 0, sizeof(BITMAPINFOHEADER));
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = w;
    bi->biHeight = -h; // 负值 = 自顶向下（origin 左上），避免翻转
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = static_cast<DWORD>(stride64 * static_cast<std::uint64_t>(h)); // 上限内必 < 2^31，不回绕
    std::uint8_t *dst = p + sizeof(BITMAPINFOHEADER);
    const std::uint8_t *src = img.pixels.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* px = src + (((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + x) * 4U);
            // RGBA8 → BGRA（CF_DIB 32bpp BI_RGB 期望 BGRX/BGRA）。
            *dst++ = px[2];
            *dst++ = px[1];
            *dst++ = px[0];
            *dst++ = px[3];
        }
    }
    GlobalUnlock(hg);
    SetClipboardData(CF_DIB, hg);
    CloseClipboard();
#else
    (void)img;
    AURORA_LOG_DEBUG("clipboard", "set_image no-op on this platform");
#endif
} // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast)

auto Clipboard::get_image() -> Image {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast): Win32
    // HGLOBAL 字节搬运不可避免
#ifdef AURORA_PLATFORM_WINDOWS
    Image out;
    if (OpenClipboard(nullptr) == 0) {
        AURORA_LOG_WARN("clipboard", "OpenClipboard failed (get_image)");
        return out;
    }
    if (IsClipboardFormatAvailable(CF_DIB) == 0) {
        CloseClipboard();
        return out;
    }
    const HANDLE hg = GetClipboardData(CF_DIB);
    if (hg == nullptr) {
        CloseClipboard();
        return out;
    }
    const auto *p = static_cast<const std::uint8_t *>(GlobalLock(hg));
    if (p == nullptr) {
        CloseClipboard();
        return out;
    }
    // 剪贴板是跨进程信道：同一桌面上的任意进程都能放入任意 CF_DIB 字节块，故头部字段
    // （biWidth/biHeight/biBitCount/biSize/biClrUsed）全部是不可信输入，必须逐项校验后
    // 才能用于指针运算，否则可越界读出相邻堆内存并随「粘贴的图片」泄露出去。
    const SIZE_T avail = GlobalSize(hg);
    if (avail < sizeof(BITMAPINFOHEADER)) {
        GlobalUnlock(hg);
        CloseClipboard();
        return out;
    }
    const auto *bi = reinterpret_cast<const BITMAPINFOHEADER *>(p);
    const int w = bi->biWidth;
    int hgt = bi->biHeight;
    const bool top_down = (hgt < 0);
    hgt = (hgt == INT_MIN) ? 0 : std::abs(hgt); // std::abs(INT_MIN) 为 UB
    const int bpp = bi->biBitCount;
    if (w <= 0 || hgt <= 0 || (bpp != 24 && bpp != 32)) {
        GlobalUnlock(hg);
        CloseClipboard();
        return out;
    }
    // 全部用 64 位算术：biSize/biClrUsed 可被构造成 0xFFFFFFF0 之类，32 位下会回绕，
    // 使 off/src_stride 变成看似合法的小值而绕过下面的容量检查。
    const std::uint64_t off = static_cast<std::uint64_t>(bi->biSize) + (static_cast<std::uint64_t>(bi->biClrUsed) * 4U);
    const std::uint64_t src_stride =
        (((static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(bpp)) + 31U) / 32U) * 4U;
    // 像素区必须完整落在剪贴板块内；off 还须至少跳过它自称的头部。
    if (off < sizeof(BITMAPINFOHEADER) || off > avail ||
        src_stride * static_cast<std::uint64_t>(hgt) > static_cast<std::uint64_t>(avail) - off) {
        GlobalUnlock(hg);
        CloseClipboard();
        return out;
    }
    out.width = w;
    out.height = hgt;
    out.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(hgt) * 4U, 0U);
    const std::uint8_t *src = p + off;
    for (int y = 0; y < hgt; ++y) {
        // 自底向上（biHeight>0）时 DIB 首行是图像底部；top-down 时首行是顶部。
        const int sy = top_down ? y : (hgt - 1 - y);
        const std::uint8_t *row = src + (static_cast<std::size_t>(sy) * src_stride);
        for (int x = 0; x < w; ++x) {
            std::uint8_t* px =
                out.pixels.data() + (((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + x) * 4U);
            if (bpp == 32) {
                const std::uint8_t* s = row + (static_cast<std::size_t>(x) * 4U);
                px[0] = s[2];
                px[1] = s[1];
                px[2] = s[0];
                px[3] = s[3]; // BGRA → RGBA
            } else if (bpp == 24) {
                const std::uint8_t* s = row + (static_cast<std::size_t>(x) * 3U);
                px[0] = s[2];
                px[1] = s[1];
                px[2] = s[0];
                px[3] = 255;
            } else {
                px[0] = px[1] = px[2] = 0;
                px[3] = 255;
            }
        }
    }
    GlobalUnlock(hg);
    CloseClipboard();
    return out;
#else
    AURORA_LOG_DEBUG("clipboard", "get_image no-op on this platform");
    return Image{};
#endif
} // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace aurora
