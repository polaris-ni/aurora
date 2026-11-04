#pragma once

// macos_decoration.h —— macOS 装饰档位的映射真源（库实现可见，不属公共 API）。
//
// 为何单列：`DecorationPolicy` 是跨后端公共契约，而 macOS 这一列此前**没有可测的映射**——档位
// 判断直接写在 `MacOSSurface` 建窗期的 ObjC++ 里，`Borderless` 被并入 `Frameless` 一起落
// `NSWindowStyleMaskBorderless`，于是「无标题栏但保留可拖拽缩放边框」这句契约在该后端静默失效，
// 且只有在 macOS 真机上跑探针才看得见。本层把「意图 → 若干布尔」抽成纯函数，ObjC 侧只做
// 「布尔 → AppKit 掩码与属性」的逐字映射，映射本身因此可以在任何 runner 上表驱动钉住。
//
// 与 `csd_decoration.h` 的分工：那边解「有没有服务端装饰 → 要不要自绘 CSD」，消费方是
// Wayland / X11 / Win32；macOS 恒有服务端装饰、不走 CSD 层，故不进那条解析链，只复用它的
// `normalize_policy` 把 `WindowStyleOptions::frameless` 旧字段折叠成 `Frameless`（单一真源）。
//
// 本层边界（刻意）：
// - 不 include 任何平台头、不出现 `NSWindowStyleMask` 字面量，故可在非 Apple 构建里编译与单测；
// - 只回答「装饰怎么落」，不回答「窗钮被点了做什么」——后者仍归原生窗框视图与 `WindowChrome`；
// - 不引入新公共类型：输出是本层的值结构，公共面仍是 `WindowStyleOptions` 的既有字段。

#include "aurora/window/detail/csd_decoration.h"  // csd::normalize_policy
#include "aurora/window/surface.h"  // WindowStyleOptions / DecorationPolicy

namespace aurora::detail {

/// @brief macOS 建窗期的装饰映射结论（每个布尔对应一处 AppKit 侧效果）。
struct MacOSDecoration {
    bool native_borderless = false;  ///< 整窗落 `NSWindowStyleMaskBorderless`（完全无装饰、无缩放边框）
    bool full_size_content = false;  ///< 叠加 `NSWindowStyleMaskFullSizeContentView`：内容区铺满窗口框
    bool sizable = false;  ///< 叠加 `NSWindowStyleMaskResizable`：边缘拖拽缩放能力
    bool hide_title = false;  ///< 标题带透明 + 标题文本隐藏（藏「栏」，不藏钮）
    bool hide_window_buttons = false;  ///< close / miniaturize / zoom 三枚标准窗钮逐枚隐藏
};

/// @brief 按样式选项推导 macOS 的装饰映射。
/// @param style 建窗期样式（本函数只读 `frameless` / `decoration` / `resizable` 三字段）。
/// @return 装饰映射结论；缩放能力一律由 `style.resizable` 决定，任何档位都不得绕过它。
[[nodiscard]] inline auto map_macos_decoration(const WindowStyleOptions &style) -> MacOSDecoration {
    MacOSDecoration out;
    out.sizable = style.resizable;
    switch (csd::normalize_policy(style.frameless, style.decoration)) {
        case DecorationPolicy::Frameless:
            // 契约：完全无装饰、无阴影边距、无缩放边框——与 `Borderless` 的区别正是「不保留缩放」。
            out.native_borderless = true;
            out.sizable = false;
            break;
        case DecorationPolicy::Borderless:
            // 契约：无标题栏但**保留**可拖拽缩放边框。AppKit 没有对应的单一掩码，故取「Titled 壳 +
            // 内容铺满窗框 + 标题带与标题文本藏掉 + 窗钮逐枚藏」这组实测可用形态：视觉上是无边框，
            // 边缘缩放仍由原生窗框视图承担；顶边内缩实测 0，故自绘装饰不会叠加一份原生标题带偏移。
            out.full_size_content = true;
            out.hide_title = true;
            out.hide_window_buttons = true;
            break;
        case DecorationPolicy::Auto:
        case DecorationPolicy::ServerSide:
        case DecorationPolicy::ClientSide:
            // macOS 恒有服务端装饰、无协商环节：三档都落原生标题栏。`ClientSide` 的「强制自绘」在
            // 本后端退化为 SSD——没有「撤掉服务端装饰再自绘」的原生对应口，也不接 CSD / margin 层。
            break;
    }
    return out;
}

}  // namespace aurora::detail
