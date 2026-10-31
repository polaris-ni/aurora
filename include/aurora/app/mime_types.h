#pragma once

/// @brief MIME 词汇表：剪贴板（`app/clipboard.h`）与系统拖放共用的格式名单一真源。
/// @file
///
/// 取值一律为规范小写形——MIME 是跨进程互操作的货币，对端按字面量比较，故大小写敏感；
/// 业务代码一律引用本表常量，不得就地硬编码这些字符串。表内只登记「本库自身会产出 / 消费」
/// 的词汇：自定义 MIME 不在表内，不等于调用方不能用它——`Clipboard::set_payload` /
/// `get_payload` 接受任意 MIME 键。
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace aurora {

constexpr std::string_view AURORA_MIME_TEXT_PLAIN = "text/plain";  ///< 纯文本基线格式。
constexpr std::string_view AURORA_MIME_URI_LIST = "text/uri-list";  ///< URI 清单（文件复制 / 拖放的标准载荷）。
constexpr std::string_view AURORA_MIME_IMAGE_PNG = "image/png";  ///< PNG 位图（跨进程图像载荷的规范形）。
constexpr std::string_view AURORA_MIME_IMAGE_JPEG = "image/jpeg";  ///< JPEG 位图（文件关联查询 / 声明的常见族）。
constexpr std::string_view AURORA_MIME_IMAGE_SVG = "image/svg+xml";  ///< SVG 矢量图（文件关联声明用）。
constexpr std::string_view AURORA_MIME_TEXT_HTML = "text/html";  ///< HTML 片段（富文本互操作载荷）。
constexpr std::string_view AURORA_MIME_APPLICATION_PDF = "application/pdf";  ///< PDF 文档（打印替代路径 / 文件关联）。
constexpr std::string_view AURORA_MIME_WIDGET = "aurora/widget";  ///< 应用内拖拽的控件树（进程内既有词汇）。

/// @brief 判定给定 MIME 是否属于本表登记的格式。
///
/// 空串与未登记的自定 MIME 均为 false——false 只表示「本表不认」，不代表该 MIME 不可用。
/// @param mime 待判定的 MIME 字符串（大小写敏感）。
/// @return 命中本表任一常量取值为 true；否则为 false。
/// @note Thread: thread-safe
/// @note Side-effects: none
[[nodiscard]] constexpr auto is_known_mime(std::string_view mime) -> bool {
    return mime == AURORA_MIME_TEXT_PLAIN || mime == AURORA_MIME_URI_LIST || mime == AURORA_MIME_IMAGE_PNG ||
           mime == AURORA_MIME_IMAGE_JPEG || mime == AURORA_MIME_IMAGE_SVG || mime == AURORA_MIME_TEXT_HTML ||
           mime == AURORA_MIME_APPLICATION_PDF || mime == AURORA_MIME_WIDGET;
}

/// @brief 扩展名 → MIME 的方向映射（大小写与前导点不敏感）。
///
/// 与剪贴板 / 拖放共用同一张词汇表：本函数只在该表已登记的词汇上补「扩展名」这一侧，
/// 不另起一张 MIME 表（两张表各自演化时，同一扩展名迟早查出两个 MIME）。
/// 未登记的扩展名返回 `std::nullopt`——文件关联的声明侧仍允许调用方自带任意 MIME 字符串。
/// @param extension 扩展名（可带前导点，任意大小写，如 `.PNG` / `png` / `PNG`）。
/// @return 命中的 MIME 常量；未登记或空串为 `std::nullopt`。
/// @note Thread: thread-safe
/// @note Side-effects: none
[[nodiscard]] constexpr auto mime_type_for_extension(std::string_view extension) -> std::optional<std::string_view> {
    std::string_view key = extension;
    if (!key.empty() && key.front() == '.') {
        key.remove_prefix(1);
    }
    if (key.empty()) {
        return std::nullopt;
    }
    // ASCII 小写比较：扩展名是大小写不敏感的键（`.PNG` / `png` / `PNG` 同一键）。
    std::array<char, 16> folded{};
    if (key.size() > folded.size()) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < key.size(); ++index) {
        const auto ch = static_cast<unsigned char>(key[index]);
        folded.at(index) = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    }
    const std::string_view lower(folded.data(), key.size());
    if (lower == "png") {
        return AURORA_MIME_IMAGE_PNG;
    }
    if (lower == "jpg" || lower == "jpeg") {
        return AURORA_MIME_IMAGE_JPEG;
    }
    if (lower == "svg") {
        return AURORA_MIME_IMAGE_SVG;
    }
    if (lower == "html" || lower == "htm") {
        return AURORA_MIME_TEXT_HTML;
    }
    if (lower == "pdf") {
        return AURORA_MIME_APPLICATION_PDF;
    }
    if (lower == "txt" || lower == "log" || lower == "md") {
        return AURORA_MIME_TEXT_PLAIN;
    }
    return std::nullopt;
}

}  // namespace aurora
