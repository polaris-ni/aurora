// Show 控件 demo：基于 State<bool> 的条件显隐。
#include "demo_common.h"

auto main() -> int {
    auto visible = std::make_shared<au::State<bool>>(true);

    au::Button toggle{ au::ButtonProps{ .label = au::LocalizedString{ "toggle banner" } } };
    toggle.on_click = [visible]() -> void { visible->set(!visible->get()); };

    au::Node root = au::Column{
        GradientTitle{ "Show 控件" },
        gap(12),
        std::move(toggle),
        gap(8),
        au::Show{ visible, BrandBadge{ "promo banner", pal::AURORA_ACCENT } },
        au::Text{ au::LocalizedString{ "点击按钮显隐上方徽章" } },
    };
    return run_demo(Card{ std::move(root) }, "Show · Aurora Demo", 520.0f, 380.0f);
}
