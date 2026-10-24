#pragma once

/// @internal 消息引擎的私有接口：模板解析（MF1 子集）与格式化，只被 message_catalog.cpp 消费。
///
/// 模板来自词表数据面：解析失败回退原文，不进错误码体系（生成器已在构建期拦截结构错误）。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aurora/i18n/format.h"
#include "aurora/i18n/localized_string.h"

namespace aurora::detail {

/// @brief 模板节点类别。
enum class NodeKind : std::uint8_t {
    Text,  ///< 字面文本段（转义已展开）
    Arg,  ///< 占位：`{name}` / `{name, number[, integer]}` / `{name, currency[, ISO 码]}` / `{name, date[, style]}`
    Selector,  ///< 选择器：`{name, plural|selectordinal|select, 分支…}`
    Hash,  ///< 选择器分支内的 `#`（格式化时替换为生效后的选择器值）
};

/// @brief 模板 AST 节点（解析产物，随词条缓存复用）。
///
/// 根节点与选择器分支体都是「子序列」节点：`children` 依原文顺序混排 Text / Arg / Selector / Hash。
struct MessageNode {
    NodeKind kind = NodeKind::Text;  ///< 节点类别
    std::string text;  ///< Text：字面文本
    std::string arg_name;  ///< Arg / Selector：占位名
    std::string func;  ///< Arg：""（纯文本）/ "number" / "currency" / "date" / "#"
    std::string style;  ///< Arg 风格：number "integer"、currency ISO 码、date short/medium/long
    std::string selector_kind;  ///< Selector："plural" / "selectordinal" / "select"
    std::int64_t offset = 0;  ///< Selector：plural / selectordinal 的 offset
    std::vector<std::pair<std::string, std::unique_ptr<MessageNode>>>
        branches;  ///< Selector 分支（键：=N / 类别名 / select 值）
    std::vector<std::unique_ptr<MessageNode>> children;  ///< 子序列（根节点与分支体）
};

/// @brief 解析模板为 AST（递归深度上限 8，超深 / 配平失败返回 nullptr）。
/// @param tmpl 模板串（UTF-8 原文）。
/// @return AST 根节点（序列节点）；失败 nullptr（调用方回退原文）。
[[nodiscard]] auto parse_message_template(std::string_view tmpl) -> std::unique_ptr<MessageNode>;

/// @brief 按参数与 locale 格式化 AST。
/// @param node AST 根节点。
/// @param args 具名参数（按名线性查找；缺失占位输出空串，选择器缺失走 other 分支）。
/// @param loc 区域设置（数字 / 日期格式化与选择器判定共用）。
/// @return 格式化后的显示串。
[[nodiscard]] auto format_message_node(const MessageNode &node, const std::vector<MessageArg> &args, const Locale &loc)
    -> std::string;

}  // namespace aurora::detail
