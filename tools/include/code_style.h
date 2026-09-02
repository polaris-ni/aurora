// ============================================================================
// code_style.h — CodeStyle 字符串解析原语
// ----------------------------------------------------------------------------
// 由 aurora_cli / aurora_mcp 复用，避免「"step" -> StepByStep / "di" -> DesignatedInit」
// 两份重复的 if-else 解析。枚举定义在 include/aurora/widget/codegen.h。
// ============================================================================
#pragma once

#include <string>

#include "aurora/widget/codegen.h"

namespace aurora::tools {

using serialization::CodeStyle;

// 将 to-code / to-yaml 的 style 参数解析为 CodeStyle 枚举；未知值回退 Fluent。
inline auto parse_code_style(const std::string &style_str) -> CodeStyle {
    auto style = CodeStyle::Fluent;
    if (style_str == "step") {
        style = CodeStyle::StepByStep;
    } else if (style_str == "di") {
        style = CodeStyle::DesignatedInit;
    }
    return style;
}

} // namespace aurora::tools
