// ============================================================================
// known_enums — 工具链「已知枚举」登记表的单一来源（SSOT）
// ----------------------------------------------------------------------------
// 消费方：tools/gen/gen_api.cpp（写 aurora_api.json 的 enums 段）、tools/servers/aurora_mcp.cpp、
//         tools/servers/aurora_cli.cpp、tools/servers/aurora_lsp.cpp（补全与校验）。
//
// 本头存在的原因：此前四份文件各抄一份字面量表，已实际漂移且含错值 —— LSP 的
// `Alignment` 写成 {Leading, Center, Trailing}（真值是 9 个方位）、其余三份缺
// `DrawerSide` / `Orientation` / `SplitterOrientation` / `ToastPosition`（均为真实属性类型），
// 且 `LengthKind` / `Curve` / `ColorPalette` 里登记了代码中不存在的取值，
// 会让 LSP / MCP 向 AI 推荐出编译不过的名字。
//
// 维护规则：
// 1. 键 = 属性描述符里出现的类型名（`prop_descriptors[].type`），不是随手起的名字；
//    `Curve` 是 `class Curve`（easing.h:39），其取值来自判别枚举 `CurveKind`。
//    `ColorPalette` 是 `au::colors::` 下的具名常量集（color.h），非 enum。
// 2. 取值必须与 `include/aurora/**` 中真实成员逐字一致；新增/改名枚举时同步此处。
//    `tests/test_known_enums.cpp` 会同时守护「取值存在性」与「属性类型覆盖率」。
// ============================================================================
#pragma once

#include <map>
#include <string>
#include <vector>

namespace aurora::tools {

/// @brief 已知枚举登记表（键为类型名，值为该类型可用的成员名，均取自代码真值）。
[[nodiscard]] inline auto known_enums() -> std::map<std::string, std::vector<std::string>> {
    std::map<std::string, std::vector<std::string>> enums;

    // ---- 布局与对齐 ----
    enums["Alignment"] = { "TopLeft",     "TopCenter",  "TopRight",     "CenterLeft", "Center",
                           "CenterRight", "BottomLeft", "BottomCenter", "BottomRight" };
    enums["MainAxisAlignment"] = { "Start", "Center", "End", "SpaceBetween", "SpaceAround", "SpaceEvenly" };
    enums["CrossAxisAlignment"] = { "Start", "Center", "End", "Stretch" };
    enums["MainAxisSize"] = { "Min", "Max" };
    enums["StackFit"] = { "Loose", "Expand", "Passthrough" };
    enums["LengthKind"] = { "WrapContent", "Expand", "Fixed", "Fraction" };

    // ---- 文本 ----
    enums["TextAlign"] = { "Left", "Right", "Center", "Start", "End", "Justify" };
    enums["TextOverflow"] = { "Clip", "Ellipsis", "Fade" };
    enums["TextDecoration"] = { "None", "Underline", "Overline", "LineThrough" };
    enums["FontWeight"] = {
        "Thin", "ExtraLight", "Light", "Normal", "Medium", "SemiBold", "Bold", "ExtraBold", "Black"
    };
    enums["FontStyle"] = { "Normal", "Italic" };

    // ---- 图像 / 媒体 ----
    enums["BoxFit"] = { "Fill", "Contain", "Cover", "FitWidth", "FitHeight", "None", "ScaleDown" };

    // ---- 动画（class Curve 的判别枚举 CurveKind）----
    enums["Curve"] = { "Linear",      "EaseIn",        "EaseOut",        "EaseInOut",   "EaseInSine",
                       "EaseOutSine", "EaseInOutSine", "EaseInQuad",     "EaseOutQuad", "EaseInOutQuad",
                       "EaseInCubic", "EaseOutCubic",  "EaseInOutCubic", "BounceOut",   "Custom" };

    // ---- 输入 ----
    enums["KeyCode"] = { "Unknown",
                         "A",
                         "B",
                         "C",
                         "D",
                         "E",
                         "F",
                         "G",
                         "H",
                         "I",
                         "J",
                         "K",
                         "L",
                         "M",
                         "N",
                         "O",
                         "P",
                         "Q",
                         "R",
                         "S",
                         "T",
                         "U",
                         "V",
                         "W",
                         "X",
                         "Y",
                         "Z",
                         "D0",
                         "D1",
                         "D2",
                         "D3",
                         "D4",
                         "D5",
                         "D6",
                         "D7",
                         "D8",
                         "D9",
                         "Space",
                         "Enter",
                         "Escape",
                         "Tab",
                         "Backspace",
                         "Delete",
                         "ArrowLeft",
                         "ArrowRight",
                         "ArrowUp",
                         "ArrowDown",
                         "Shift",
                         "Control",
                         "Alt",
                         "Meta",
                         "Home",
                         "End",
                         "PageUp",
                         "PageDown",
                         "Minus",
                         "Equal",
                         "LeftBracket",
                         "RightBracket",
                         "Backslash",
                         "Semicolon",
                         "Quote",
                         "Comma",
                         "Period",
                         "Slash",
                         "Backquote",
                         "F1",
                         "F2",
                         "F3",
                         "F4",
                         "F5",
                         "F6",
                         "F7",
                         "F8",
                         "F9",
                         "F10",
                         "F11",
                         "F12" };

    // ---- 控件专属枚举（此前四份登记表均缺失，导致属性值无法校验 / 补全）----
    enums["DrawerSide"] = { "Left", "Right" };
    enums["Orientation"] = { "Horizontal", "Vertical" };
    enums["SplitterOrientation"] = { "Horizontal", "Vertical" };
    enums["ToastPosition"] = { "Bottom", "Top" };

    // ---- 具名颜色（au::colors::AURORA_*，非 enum，供 Color 属性取值提示）----
    enums["ColorPalette"] = { "AURORA_WHITE", "AURORA_BLACK", "AURORA_BLUE",   "AURORA_RED",
                              "AURORA_GREEN", "AURORA_GRAY",  "AURORA_YELLOW", "AURORA_TRANSPARENT" };

    return enums;
}

} // namespace aurora::tools
