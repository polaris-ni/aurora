// 事件 demo：EventDispatcher + MouseEvent / KeyEvent / TextInputEvent / KeyCode。
#include "demo_common.h"

auto main() -> int {
    au::Text box{ au::LocalizedString{ "clickable area（点击触发 MouseEvent）" } };
    box.modifier.set(
        au::Modifier{}
            .padding(16.0f)
            .size(280.0f, 64.0f)
            .background(pal::AURORA_PRIMARY_SOFT)
            .align(au::Alignment::Center)
            .clickable([]() -> void { AURORA_LOG_INFO("demo", "[event] clicked（MouseEvent 已派发到该控件）"); }));

    au::Node root = au::Column{
        GradientTitle{ "事件 / Event" },
        gap(12),
        std::move(box),
        au::Text{ au::LocalizedString{ "KeyCode::Enter = " + std::to_string(static_cast<int>(au::KeyCode::Enter)) } },
        au::Text{ au::LocalizedString{ "事件经 EventDispatcher 派发到命中控件" } },
    };
    return run_demo(Card{ std::move(root) }, "Event · Aurora Demo", 520.0f, 380.0f);
}
