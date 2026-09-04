// win32_capture.h —— 内部共享：经 PrintWindow 抓取真实窗口（含非客户区/标题栏/边框）为 PNG。
// 仅 Windows 平台可用；调用方（Win32Surface / GlfwSurface-on-Windows）自行用 AURORA_ENABLE_DEBUG 门控。
// 文件位于 src/（仅库实现可见），不属公共 API；消费者不应包含。
#pragma once
#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/render/png.h"
#include "aurora/window/swizzle.h"

// PW_RENDERFULLCONTENT 自 Windows 8.1（_WIN32_WINNT >= 0x0603）起才在 winuser.h 中定义。
// 本项目未全局抬升 _WIN32_WINNT（file_dialog/system_tray 等 TU 仍按 0x0601 编译，且 windows.h 可能被
// 先行以低版本引入），此处就地兜底定义，值取自 SDK 官方常量（0x00000002）。PrintWindow API 自
// Win2000 即在，PW_RENDERFULLCONTENT 仅作额外选项；旧系统上 PrintWindow(...,2) 失败会自动回退 BitBlt。
#ifndef PW_RENDERFULLCONTENT  // NOLINT(*-identifier-naming)
#define PW_RENDERFULLCONTENT 0x00000002  // NOLINT(*-identifier-naming)
#endif

namespace aurora::detail {

/// @brief 抓取指定 HWND 的真实屏幕画面（含非客户区）并写入 PNG（RGBA8）。
/// 主路径：`PrintWindow(hwnd, memDC, PW_RENDERFULLCONTENT)` 入 32bpp BGRA DIB；
/// 失败回退 `BitBlt` 从窗口 DC（仅未遮挡时可靠）。最终 BGRA→RGBA swizzle 后 `write_png`。
/// @return 成功返回 true；任何环节失败返回 `GeneralNotSupported` 错误（不崩溃）。
[[nodiscard]] inline auto capture_window_by_hwnd(HWND hwnd, const std::string &path) -> Result<bool> {
    if (hwnd == nullptr || (IsWindow(hwnd) == 0)) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: invalid or null HWND")};
    }
    // 确保窗口可见：PrintWindow 对从未显示的窗口可能抓不到内容。
    if (IsWindowVisible(hwnd) == 0) {
        ShowWindow(hwnd, SW_SHOWNA);
    }
    // 外接矩形（含标题栏与边框）—— 满足「真实窗口截图包含非客户区」。
    RECT wr{};
    if (GetWindowRect(hwnd, &wr) == 0) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: GetWindowRect failed")};
    }
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: zero-size window rect")};
    }

    const HDC hdc_screen = GetWindowDC(hwnd);
    if (hdc_screen == nullptr) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: GetWindowDC failed")};
    }
    const HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    ReleaseDC(hwnd, hdc_screen);
    if (hdc_mem == nullptr) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: CreateCompatibleDC failed")};
    }

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h;  // 顶向下，避免图像上下翻转
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    RGBQUAD *bits = nullptr;
    // NOLINTBEGIN(*-pro-type-reinterpret-cast)
    const HBITMAP hbmp = CreateDIBSection(hdc_mem, reinterpret_cast<BITMAPINFO *>(&bi), DIB_RGB_COLORS,
                                          reinterpret_cast<void **>(&bits), nullptr, 0);
    // NOLINTEND(*-pro-type-reinterpret-cast)
    if (hbmp == nullptr) {
        DeleteDC(hdc_mem);
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: CreateDIBSection failed")};
    }
    auto *const hbmp_old = static_cast<HBITMAP>(SelectObject(hdc_mem, hbmp));

    BOOL ok = PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT);
    if (ok == 0) {
        // 回退：直接从窗口 DC BitBlt（仅未遮挡时可靠）。
        const HDC hdc_win = GetWindowDC(hwnd);
        if (hdc_win != nullptr) {
            ok = BitBlt(hdc_mem, 0, 0, w, h, hdc_win, 0, 0, SRCCOPY);
            ReleaseDC(hwnd, hdc_win);
        }
    }

    auto result =
        Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: PrintWindow and BitBlt both failed")};
    if (ok != 0) {
        // 32bpp BI_RGB DIB 字节序为 BGRA；转为 RGBA 后写 PNG。
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        std::vector<std::uint8_t> rgba(n * 4);
        // NOLINTBEGIN(*-pro-type-reinterpret-cast)
        swizzle_bgra_to_rgba(reinterpret_cast<const std::uint32_t *>(bits),
                             reinterpret_cast<std::uint32_t *>(rgba.data()), n);
        // NOLINTEND(*-pro-type-reinterpret-cast)
        if (write_png(path.c_str(), w, h, rgba.data())) {
            result = Result<bool>{true};
        } else {
            result = Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: write_png failed")};
        }
    }

    SelectObject(hdc_mem, hbmp_old);
    DeleteObject(hbmp);
    DeleteDC(hdc_mem);
    return result;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS
