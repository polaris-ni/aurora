// 事件 demo：EventDispatcher + MouseEvent / KeyEvent / TextInputEvent / KeyCode。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Text box{au::LocalizedString{"clickable area (click triggers MouseEvent)"}};
    box.modifier.set(au::Modifier{}
                         .padding(16.0F)
                         .size(280.0F, 64.0F)
                         .background(pal::AURORA_PRIMARY_SOFT)
                         .align(au::Alignment::Center)
                         .clickable([]() -> void {
                             AURORA_LOG_INFO("demo", "[event] clicked (MouseEvent dispatched to this widget)");
                         }));

    au::Node root = au::Column{
        GradientTitle{"Event"},
        gap(12),
        std::move(box),
        au::Text{au::LocalizedString{"KeyCode::Enter = " + std::to_string(static_cast<int>(au::KeyCode::Enter))}},
        au::Text{au::LocalizedString{"Event dispatched to hit widget via EventDispatcher"}},
    };
    return run_demo(Card{std::move(root)}, "Event · Aurora Demo", 520.0F, 380.0F);
}