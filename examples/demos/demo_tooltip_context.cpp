// Tooltip / ContextMenu / blur 修饰扩展 demo：悬停提示 + 右键菜单 + 毛玻璃。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    // Tooltip：悬停 500ms 显示提示
    au::Text hover_me{"Hover me 500ms for tooltip"};
    hover_me.modifier.set(au::Modifier{}.tooltip("This is a Tooltip bubble"));

    // ContextMenu：右键弹出菜单
    au::Text right_click_me{"Right-click me for context menu"};
    right_click_me.modifier.set(au::Modifier{}.context_menu({
        au::MenuItem{"Copy", []() -> void { AURORA_LOG_INFO("demo", "Copy"); }},
        au::MenuItem{"Paste", []() -> void { AURORA_LOG_INFO("demo", "Paste"); }},
        au::MenuItem::separator_item(),
        au::MenuItem{"Delete", []() -> void { AURORA_LOG_INFO("demo", "Delete"); }},
    }));

    // blur：内容模糊
    au::Text blurred{"This text is blurred"};
    blurred.modifier.set(au::Modifier{}.background(au::Color(255, 235, 190, 255)).blur(2.0F));

    au::Node root = au::Column{
        GradientTitle{"Tooltip / ContextMenu / Blur"},
        gap(12),
        std::move(hover_me),
        gap(8),
        std::move(right_click_me),
        gap(8),
        std::move(blurred),
    };
    return run_demo(Card{std::move(root)}, "TooltipMenu · Aurora Demo", 480.0F, 400.0F);
}