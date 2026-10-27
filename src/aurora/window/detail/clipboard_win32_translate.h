#pragma once

// 剪贴板 Win32 侧**折算层**：`DROPFILES` 与 `CF_HTML` 两种线上形态的编解码纯函数。
//
// 为何单列且零平台依赖：这两件是「同一份语义在两条通道上各长一次」的高危区——文件清单在剪贴板
// 上走 `CF_HDROP`、在拖入侧走 `uri-list`；HTML 在剪贴板上走 `CF_HTML`。若把编解码写进平台桥，
// 它就被 `<windows.h>` 绑死、**在无头 CI 上根本不参与编译**，而这两处的真实缺陷（偏移算错、
// 尾随空字符少一个）恰恰只能靠逐值断言发现。故折算是纯函数 + 可无头单测，桥只做「字节 ↔ HGLOBAL」。
//
// 为何是 UTF-16 中间形态而非直接吃 UTF-8 路径：`CF_HDROP` 的线上形态是 UTF-16LE，而 UTF-8 ↔
// UTF-16 转换在本库只能借平台 API（`MultiByteToWideChar`），那会引入 `<windows.h>`。折算层以
// `std::u16string` 为界：它不解释编码，只负责**布局**；转码归桥。这条界让「布局」可在 Linux CI
// 上逐字节断言，而「转码」由真机探针覆盖。
//
// 本头不引 `<windows.h>`：结构体按 Win32 SDK 的稳定布局**手工定义**（`DROPFILES` 自 Windows 95
// 起未变，20 字节、4 字节对齐无填充），故在任何 CI 作业（含无头 Linux 与 wasm）上都能编译并被断言。
//
// 门控：无。本头在**所有**平台编译。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aurora::clipboard::detail {

/// @brief `DROPFILES` 的字节布局大小（`pFiles` 字段的常规取值，也是清单起点偏移）。
///
/// `DWORD pFiles` + `POINT pt`(8) + `BOOL fNC` + `BOOL fWide` = 20 字节；四个字段均为 4 字节对齐，
/// 无编译器填充。线上出现更小的值一律按畸形拒绝（见 `decode_hdrop`）。
inline constexpr std::size_t AURORA_WIN32_DROPFILES_HEADER_BYTES = 20U;

/// @brief 把 UTF-16 路径清单编码为 `CF_HDROP` 线上字节（`DROPFILES` + 双空结尾的宽字符清单）。
///
/// 布局：`pFiles = 20`、拖点 `(0,0)`、`fNC = 0`、`fWide = 1`，其后每条路径以 `L'\0'` 结尾，
/// 清单整体再补一个 `L'\0'`（**空清单产出空字节**而非「只有头部」——空清单不是合法 HDROP，
/// 与剪贴板侧「空清单为 no-op」的口径一致）。
/// @param paths 路径清单（UTF-16 码元序，不含空字符）。
/// @return 线上字节；空清单产出空字节。
[[nodiscard]] auto encode_hdrop(const std::vector<std::u16string> &paths) -> std::vector<std::uint8_t>;

/// @brief 解码 `CF_HDROP` 线上字节为 UTF-16 路径清单。
///
/// 跨进程信道上的字节一律不可信：`pFiles` 越界、`fWide` 为假（ANSI 清单）、清单尾部缺少收尾空
/// 字符，三种情形都返回 `std::nullopt` 由调用方折成结构化错误——**不猜、不截断补尾**（凭空补一个
/// 空字符会把最后一条路径变成永远收不了尾的串）。
/// @param bytes 剪贴板上的 `CF_HDROP` 字节。
/// @return 路径清单；字节畸形时为 `std::nullopt`。
[[nodiscard]] auto decode_hdrop(const std::vector<std::uint8_t> &bytes) -> std::optional<std::vector<std::u16string>>;

/// @brief 把 HTML 片段封装为 `CF_HTML` 线上文本（`Version:0.9` + 四组十进位字节偏移）。
///
/// 四组偏移是**从整块数据开头算起的字节偏移**，且必须以十进位零填充（`%010u`）书写——这是
/// Windows 侧解析器的硬要求，写短一位即整块被判为非法 HTML。故本函数先按固定宽度算出头部长度
/// （偏移字段宽度恒定，头部长度与片段长度无关），再回填四组数字。
/// @param fragment HTML 片段（UTF-8）。
/// @return `CF_HTML` 线上文本（UTF-8 字节序，可直接写入剪贴板）。
[[nodiscard]] auto build_html_frame(const std::string &fragment) -> std::string;

/// @brief 从 `CF_HTML` 线上文本取出 HTML 片段（按 `StartFragment` / `EndFragment` 切片）。
///
/// 头部缺失、偏移非十进位、越界或次序颠倒一律返回 `std::nullopt`——剪贴板是跨进程信道，别的应用
/// 可能给出任意自称 `HTML Format` 的字节，凭偏移切越界会读出相邻内存并随「粘贴」泄露。
/// @param text `CF_HTML` 线上文本。
/// @return HTML 片段；块非法时为 `std::nullopt`。
[[nodiscard]] auto parse_html_frame(const std::string &text) -> std::optional<std::string>;

}  // namespace aurora::clipboard::detail
