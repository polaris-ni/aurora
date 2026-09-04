// Popup / OverlayHost 控件 demo：点击按钮在锚点弹出浮层，点击外部自动关闭。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto popup = std::make_shared<au::Popup>(au::Node{au::Column{
        au::Text{"Popup content"},
        au::Text{"Click outside to close"},
    }});

    au::Button open_btn{au::ButtonProps{.label = "Open Popup"}};
    open_btn.on_click = [popup]() -> void { popup->open_at(au::Point{.x = 140.0F, .y = 120.0F}); };

    au::Node root = au::Column{
        GradientTitle{"Popup / Overlay widget"},
        gap(12),
        std::move(open_btn),
        au::Node{std::shared_ptr<au::Widget>(popup)},
    };
    return run_demo(Card{std::move(root)}, "Popup · Aurora Demo", 480.0F, 360.0F);
}