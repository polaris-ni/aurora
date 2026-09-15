#pragma once
#include <cstddef>

#include "aurora/core/enums.h"

namespace aurora {

/// @file cursor_map.h
/// @brief 光标形状的跨后端映射 SSOT（光标形状 API 第二半：平台接线）。
///
/// `CursorShape`（core/enums.h）只声明「语义形状」，不绑定任何平台常量；各窗口后端把它
/// 翻译为平台原生光标句柄。本头提供唯一一份**平台中立**的映射：freedesktop 光标主题名
/// （同时就是 W3C CSS `cursor` 关键字，见 CSS Basic User Interface §Cursor）。
///
/// 分工（为何只有一份中性表）：
/// - **Wayland**：`wl_cursor_theme_get_cursor(theme, name)` 直接吃主题名字符串 → 本表即其映射。
/// - **Wasm / 浏览器**：`canvas.style.cursor` 直接吃 CSS 关键字 → 本表即其映射。
/// - **GLFW / Win32 / X11 / macOS**：需要各自原生句柄枚举（`GLFW_*_CURSOR` / `IDC_*` /
///   `XC_*` / `NSCursor` 选择器），无法共用字符串，故在各自后端 `.cpp` 内 `switch`
///   按 `CursorShape` 取值序索引（见 `AURORA_CURSOR_SHAPE_COUNT` 长度契约）。
///
/// @note 本表是「语义 → 规范名」的单向权威。新增 `CursorShape` 取值时须同步扩本表、
/// 各后端映射表与 `utest_cursor_map`，三处长度契约由 `AURORA_CURSOR_SHAPE_COUNT` 对齐。

/// @brief `CursorShape` 的全部取值个数。
/// 各后端映射表（GLFW/Win32/X11/macOS）以 `static_assert(std::size(kMap) == AURORA_CURSOR_SHAPE_COUNT)`
/// 断言长度，新增形状漏填即编译期红灯；`utest_cursor_map` 另断言本表本身覆盖全部取值。
inline constexpr std::size_t AURORA_CURSOR_SHAPE_COUNT = 11;

/// @brief 光标形状 → 规范名（freedesktop 光标主题名 / CSS `cursor` 关键字）。
///
/// 取值与 `CursorShape` 一一对应且两两互异（由 `utest_cursor_map` 锁定）；未知取值回退
/// `"default"`（等价箭头，符合「无法识别的光标不得隐藏系统光标」的可用性契约）。
///
/// | `CursorShape` | 规范名 | 语义 |
/// |:---|:---|:---|
/// | `Arrow` | `default` | 默认箭头 |
/// | `IBeam` | `text` | 文本插入 I 形 |
/// | `PointingHand` | `pointer` | 可点击手型 |
/// | `ResizeNS` | `ns-resize` | 上下调整大小 |
/// | `ResizeEW` | `ew-resize` | 左右调整大小 |
/// | `ResizeNWSE` | `nwse-resize` | 主对角线（↘↖）调整大小 |
/// | `ResizeNESW` | `nesw-resize` | 副对角线（↗↙）调整大小 |
/// | `Move` | `move` | 移动 |
/// | `Crosshair` | `crosshair` | 十字准星 |
/// | `NotAllowed` | `not-allowed` | 禁止 |
/// | `Wait` | `wait` | 等待/忙碌 |
[[nodiscard]] constexpr auto cursor_rfc_name(CursorShape shape) -> const char * {
    switch (shape) {
        case CursorShape::Arrow:
            return "default";
        case CursorShape::IBeam:
            return "text";
        case CursorShape::PointingHand:
            return "pointer";
        case CursorShape::ResizeNS:
            return "ns-resize";
        case CursorShape::ResizeEW:
            return "ew-resize";
        case CursorShape::ResizeNWSE:
            return "nwse-resize";
        case CursorShape::ResizeNESW:
            return "nesw-resize";
        case CursorShape::Move:
            return "move";
        case CursorShape::Crosshair:
            return "crosshair";
        case CursorShape::NotAllowed:
            return "not-allowed";
        case CursorShape::Wait:
            return "wait";
    }
    return "default";
}

}  // namespace aurora
