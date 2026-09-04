#pragma once
#include <cstdint>
#include <string>

#include "aurora/core/platform.h"

// Win32 平台专属：UTF-8 ↔ wchar_t 编解码原语。
// 收口自 src/aurora/app/system_tray_win32.cpp 与 file_dialog_win32.cpp 中逐行相同的实现，
// 消除跨文件重复。非 Win32 平台不编译（与 AURORA_BACKEND_WIN32 裁剪一致）。
#ifdef AURORA_PLATFORM_WINDOWS
namespace aurora::internal {

// UTF-8 字节串 → UTF-16（wchar_t）宽串。空串/失败返回空 wstring。
[[nodiscard]] auto utf8_to_wstr(const std::string &s) -> std::wstring;

// UTF-16（wchar_t）宽串 → UTF-8 字节串。空指针/空串/失败返回空 string。
[[nodiscard]] auto wstr_to_utf8(const wchar_t *ws) -> std::string;

}  // namespace aurora::internal
#endif

// 跨平台纯逻辑：Unicode 码点 ↔ UTF-8（1~4 字节，完整 Unicode 含辅助平面）。
// 收口自 win32_window.cpp（to_utf8）、glfw_surface.cpp（utf8_from_codepoint）、
// widget/text.cpp、widget/text_input.h、render/font_engine.cpp 中重复的码点→UTF-8 实现（dup-1）。
// 不进 aurora.h 公共导出，仅被内部/后端/单测 include。
namespace aurora {

// 单个码点 → UTF-8 字节串（cp 超界/无效返回空）。完整 Unicode（BMP + 辅助平面 emoji）。
[[nodiscard]] auto utf8_encode(std::uint32_t cp) -> std::string;

// 首字节决定的 UTF-8 序列长度（1~4；非法首字节按 1 处理，与解码退化一致）。
[[nodiscard]] auto utf8_cp_len(unsigned char c) -> int;

// 字节串中的码点（字符）总数。
[[nodiscard]] auto utf8_cp_count(const std::string &s) -> std::size_t;

// 取 [start, start+count) 区间的码点子串（越界自动截断）。
[[nodiscard]] auto utf8_cp_slice(const std::string &s, std::size_t start, std::size_t count) -> std::string;

}  // namespace aurora
