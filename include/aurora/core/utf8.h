#pragma once

/// @brief UTF-8 码点编解码原语：跨平台「码点↔UTF-8」，以及 Win32 专属「UTF-8↔wchar_t」。
/// @file
#include <cstdint>
#include <string>

#include "aurora/core/platform.h"

// Win32 平台专属：UTF-8 ↔ wchar_t 编解码原语。
// 收口自 src/aurora/app/system_tray_win32.cpp 与 file_dialog_win32.cpp 中逐行相同的实现，
// 消除跨文件重复。非 Win32 平台不编译（与 AURORA_BACKEND_WIN32 裁剪一致）。
#ifdef AURORA_PLATFORM_WINDOWS
namespace aurora::internal {

/// @brief UTF-8 字节串 → UTF-16（wchar_t）宽串。
/// @param s 待转换的 UTF-8 字节串。
/// @return 转换后的宽串；空串/失败返回空 `wstring`。
[[nodiscard]] auto utf8_to_wstr(const std::string &s) -> std::wstring;

/// @brief UTF-16（wchar_t）宽串 → UTF-8 字节串。
/// @param ws 以空字符结尾的宽字符串指针。
/// @return 转换后的 UTF-8 串；空指针/空串/失败返回空 `string`。
[[nodiscard]] auto wstr_to_utf8(const wchar_t *ws) -> std::string;

}  // namespace aurora::internal
#endif

// 跨平台纯逻辑：Unicode 码点 ↔ UTF-8（1~4 字节，完整 Unicode 含辅助平面）。
// 收口自 win32_host.cpp（to_utf8）、glfw_surface.cpp（utf8_from_codepoint）、
// widget/text.cpp、widget/text_input.h、render/font_engine.cpp 中重复的码点→UTF-8 实现（dup-1）。
// 不进 aurora.h 公共导出，仅被内部/后端/单测 include。
namespace aurora {

/// @brief 单个 Unicode 码点 → UTF-8 字节串（完整 Unicode，含 BMP 与辅助平面 emoji）。
/// @param cp Unicode 码点。
/// @return 1~4 字节的 UTF-8 串；码点超界/无效返回空串。
[[nodiscard]] auto utf8_encode(std::uint32_t cp) -> std::string;

/// @brief 由首字节推断该 UTF-8 序列的字节长度。
/// @param c UTF-8 首字节。
/// @return 序列长度 1~4；非法首字节按 1 处理（与解码退化一致）。
[[nodiscard]] auto utf8_cp_len(unsigned char c) -> int;

/// @brief 统计字节串中的码点（字符）总数。
/// @param s UTF-8 字节串。
/// @return 码点个数。
[[nodiscard]] auto utf8_cp_count(const std::string &s) -> std::size_t;

/// @brief 取码点区间 `[start, start+count)` 对应的子串。
/// @param s UTF-8 字节串。
/// @param start 起始码点下标。
/// @param count 取用的码点个数。
/// @return 子串；越界自动截断。
[[nodiscard]] auto utf8_cp_slice(const std::string &s, std::size_t start, std::size_t count) -> std::string;

}  // namespace aurora
