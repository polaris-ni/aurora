// 事件 demo：EventDispatcher + MouseEvent / KeyEvent / TextInputEvent / KeyCode。
#include "demo_common.h"

auto main() -> int {
    au::Text box{ au::LocalizedString{ "clickable area (click triggers MouseEvent)" } };
    box.modifier.set(
        au::Modifier{}
            .padding(16.0f)
            .size(280.0f, 64.0f)
            .background(pal::AURORA_PRIMARY_SOFT)
            .align(au::Alignment::Center)
            .clickable([]() -> void { AURORA_LOG_INFO("demo", "[event] clicked (MouseEvent dispatched to this widget)"); }));

    au::Node root = au::Column{
        GradientTitle{ "Event" },
        gap(12),
        std::move(box),
        au::Text{ au::LocalizedString{ "KeyCode::Enter = " + std::to_string(static_cast<int>(au::KeyCode::Enter)) } },
        au::Text{ au::LocalizedString{ "Event dispatched to hit widget via EventDispatcher" } },
    };
    return run_demo(Card{ std::move(root) }, "Event · Aurora Demo", 520.0f, 380.0f);
}
