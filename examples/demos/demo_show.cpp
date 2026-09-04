// Show 控件 demo：基于 State<bool> 的条件显隐。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto visible = std::make_shared<au::State<bool>>(true);

    au::Button toggle{au::ButtonProps{.label = au::LocalizedString{"toggle banner"}}};
    toggle.on_click = [visible]() -> void { visible->set(!visible->get()); };

    au::Node root = au::Column{
        GradientTitle{"Show widget"},
        gap(12),
        std::move(toggle),
        gap(8),
        au::Show{visible, BrandBadge{"promo banner", pal::AURORA_ACCENT}},
        au::Text{au::LocalizedString{"Click button to show/hide badge above"}},
    };
    return run_demo(Card{std::move(root)}, "Show · Aurora Demo", 520.0F, 380.0F);
}