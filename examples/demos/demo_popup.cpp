// Popup / OverlayHost 控件 demo：点击按钮在锚点弹出浮层，点击外部自动关闭。
#include "demo_common.h"

auto main() -> int {
    auto popup = std::make_shared<au::Popup>(au::Node{ au::Column{
        au::Text{ "Popup content" },
        au::Text{ "Click outside to close" },
    } });

    au::Button open_btn{ au::ButtonProps{ .label = "Open Popup" } };
    open_btn.on_click = [popup]() -> void { popup->open_at(au::Point{ .x = 140.0f, .y = 120.0f }); };

    au::Node root = au::Column{
        GradientTitle{ "Popup / Overlay widget" },
        gap(12),
        std::move(open_btn),
        au::Node{ std::shared_ptr<au::Widget>(popup) },
    };
    return run_demo(Card{ std::move(root) }, "Popup · Aurora Demo", 480.0f, 360.0f);
}
