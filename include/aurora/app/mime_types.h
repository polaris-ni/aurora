#pragma once

/// @brief MIME 词汇表：剪贴板（`app/clipboard.h`）与系统拖放共用的格式名单一真源。
/// @file
///
/// 取值一律为规范小写形——MIME 是跨进程互操作的货币，对端按字面量比较，故大小写敏感；
/// 业务代码一律引用本表常量，不得就地硬编码这些字符串。表内只登记「本库自身会产出 / 消费」
/// 的词汇：自定义 MIME 不在表内，不等于调用方不能用它——`Clipboard::set_payload` /
/// `get_payload` 接受任意 MIME 键。
#include <string_view>

namespace aurora {

constexpr std::string_view AURORA_MIME_TEXT_PLAIN = "text/plain";  ///< 纯文本基线格式。
constexpr std::string_view AURORA_MIME_URI_LIST = "text/uri-list";  ///< URI 清单（文件复制 / 拖放的标准载荷）。
constexpr std::string_view AURORA_MIME_IMAGE_PNG = "image/png";  ///< PNG 位图（跨进程图像载荷的规范形）。
constexpr std::string_view AURORA_MIME_TEXT_HTML = "text/html";  ///< HTML 片段（富文本互操作载荷）。
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
           mime == AURORA_MIME_TEXT_HTML || mime == AURORA_MIME_WIDGET;
}

}  // namespace aurora
