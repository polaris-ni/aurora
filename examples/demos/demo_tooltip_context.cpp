// Tooltip / ContextMenu / blur 修饰扩展 demo：悬停提示 + 右键菜单 + 毛玻璃。
#include "demo_common.h"

auto main() -> int {
    // Tooltip：悬停 500ms 显示提示
    au::Text hover_me{ "悬停我 500ms 看提示" };
    hover_me.modifier.set(au::Modifier{}.tooltip("这是 Tooltip 提示气泡"));

    // ContextMenu：右键弹出菜单
    au::Text right_click_me{ "右键我弹出菜单" };
    right_click_me.modifier.set(au::Modifier{}.context_menu({
        au::MenuItem{ "复制", []() -> void { AURORA_LOG_INFO("demo", "复制"); } },
        au::MenuItem{ "粘贴", []() -> void { AURORA_LOG_INFO("demo", "粘贴"); } },
        au::MenuItem::separator_item(),
        au::MenuItem{ "删除", []() -> void { AURORA_LOG_INFO("demo", "删除"); } },
    }));

    // blur：内容模糊
    au::Text blurred{ "这段文字被模糊了" };
    blurred.modifier.set(au::Modifier{}.background(au::Color(255, 235, 190, 255)).blur(2.0f));

    au::Node root = au::Column{
        GradientTitle{ "Tooltip / ContextMenu / Blur" },
        gap(12),
        std::move(hover_me),
        gap(8),
        std::move(right_click_me),
        gap(8),
        std::move(blurred),
    };
    return run_demo(Card{ std::move(root) }, "TooltipMenu · Aurora Demo", 480.0f, 400.0f);
}
