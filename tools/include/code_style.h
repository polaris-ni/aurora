// ============================================================================
// code_style.h — CodeStyle string-parsing primitive
// ----------------------------------------------------------------------------
// Reused by aurora_cli / aurora_mcp to avoid two duplicate if-else parsers for
// "step" -> StepByStep / "di" -> DesignatedInit. The enum is defined in
// include/aurora/widget/codegen.h.
// ============================================================================
#pragma once

#include <string>

#include "aurora/widget/codegen.h"

namespace aurora::tools {

using serialization::CodeStyle;

// Parse the style argument of to-code / to-yaml into a CodeStyle enum; unknown values fall back to Fluent.
inline auto parse_code_style(const std::string &style_str) -> CodeStyle {
    auto style = CodeStyle::Fluent;
    if (style_str == "step") {
        style = CodeStyle::StepByStep;
    } else if (style_str == "di") {
        style = CodeStyle::DesignatedInit;
    }
    return style;
}

}  // namespace aurora::tools
