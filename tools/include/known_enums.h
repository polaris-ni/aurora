// ============================================================================
// known_enums — single source of truth (SSOT) for the toolchain "known enums" registry
// ----------------------------------------------------------------------------
// Consumers: tools/gen/gen_api.cpp (writes the enums section of aurora_api.json),
//            tools/servers/aurora_mcp.cpp, tools/servers/aurora_cli.cpp,
//            tools/servers/aurora_lsp.cpp (completion and validation).
//
// Why this header exists: previously four files each kept a hand-copied literal table, which had
// actually drifted and contained wrong values — the LSP's `Alignment` was written as
// {Leading, Center, Trailing} (the real value has 9 positions), the other three were missing
// `DrawerSide` / `Orientation` / `SplitterOrientation` / `ToastPosition` (all real property types),
// and `LengthKind` / `Curve` / `ColorPalette` registered values that do not exist in the code,
// which would make the LSP / MCP suggest names that do not compile.
//
// Maintenance rules:
// 1. Key = the type name appearing in property descriptors (`prop_descriptors[].type`), not an
//    arbitrary name; `Curve` is `class Curve` (easing.h:39), whose values come from the
//    discriminating enum `CurveKind`. `ColorPalette` is a set of named constants under
//    `au::colors::` (color.h), not an enum.
// 2. Values must match the real members in `include/aurora/**` verbatim; keep this in sync when
//    adding or renaming enums. `tests/test_known_enums.cpp` guards both "value existence" and
//    "property type coverage".
// ============================================================================
#pragma once

#include <map>
#include <string>
#include <vector>

namespace aurora::tools {

/// @brief Known-enums registry (key = type name, value = the member names available for that type,
/// all taken from the real code values).
[[nodiscard]] inline auto known_enums() -> std::map<std::string, std::vector<std::string>> {
    std::map<std::string, std::vector<std::string>> enums;

    // ---- layout and alignment ----
    enums["Alignment"] = {"TopLeft",     "TopCenter",  "TopRight",     "CenterLeft", "Center",
                          "CenterRight", "BottomLeft", "BottomCenter", "BottomRight"};
    enums["MainAxisAlignment"] = {"Start", "Center", "End", "SpaceBetween", "SpaceAround", "SpaceEvenly"};
    enums["CrossAxisAlignment"] = {"Start", "Center", "End", "Stretch"};
    enums["MainAxisSize"] = {"Min", "Max"};
    enums["StackFit"] = {"Loose", "Expand", "Passthrough"};
    enums["LengthKind"] = {"WrapContent", "Expand", "Fixed", "Fraction"};

    // ---- text ----
    enums["TextAlign"] = {"Left", "Right", "Center", "Start", "End", "Justify"};
    enums["TextOverflow"] = {"Clip", "Ellipsis", "Fade"};
    enums["TextDecoration"] = {"None", "Underline", "Overline", "LineThrough"};
    enums["FontWeight"] = {"Thin", "ExtraLight", "Light", "Normal", "Medium", "SemiBold", "Bold", "ExtraBold", "Black"};
    enums["FontStyle"] = {"Normal", "Italic"};

    // ---- image / media ----
    enums["BoxFit"] = {"Fill", "Contain", "Cover", "FitWidth", "FitHeight", "None", "ScaleDown"};

    // ---- animation (CurveKind, the discriminating enum of class Curve) ----
    enums["Curve"] = {"Linear",      "EaseIn",        "EaseOut",        "EaseInOut",   "EaseInSine",
                      "EaseOutSine", "EaseInOutSine", "EaseInQuad",     "EaseOutQuad", "EaseInOutQuad",
                      "EaseInCubic", "EaseOutCubic",  "EaseInOutCubic", "BounceOut",   "Custom"};

    // ---- input ----
    enums["KeyCode"] = {"Unknown",
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
                        "F12"};

    // ---- widget-specific enums (previously missing from all four registries, which made property
    // values impossible to validate / complete) ----
    enums["DrawerSide"] = {"Left", "Right"};
    enums["Orientation"] = {"Horizontal", "Vertical"};
    enums["SplitterOrientation"] = {"Horizontal", "Vertical"};
    enums["ToastPosition"] = {"Bottom", "Top"};

    // ---- named colors (au::colors::AURORA_*, not an enum; provides value hints for the Color property) ----
    enums["ColorPalette"] = {"AURORA_WHITE", "AURORA_BLACK", "AURORA_BLUE",   "AURORA_RED",
                             "AURORA_GREEN", "AURORA_GRAY",  "AURORA_YELLOW", "AURORA_TRANSPARENT"};

    return enums;
}

}  // namespace aurora::tools
