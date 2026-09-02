// ProgressIndicator 控件 demo：线性进度条，值范围 [0,1]，配合 State 实时反映。
#include "demo_common.h"

auto main() -> int {
    const auto prog = std::make_shared<au::State<double>>(0.4);
    const auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "Progress 40%" });

    au::ProgressIndicator pi{ au::Reactive{ prog } };
    pi.set_color(pal::AURORA_PRIMARY);
    pi.set_track_color(pal::AURORA_BORDER);

    au::ProgressIndicator pi2{ au::Reactive{ std::make_shared<au::State<double>>(0.72) } };
    pi2.set_color(pal::AURORA_OK);

    au::Node root = au::Column{
        GradientTitle{ "Progress widget" },
        gap(12),
        std::move(pi),
        au::Text{ au::TextProps{ .content = au::Reactive{ label } } },
        gap(12),
        std::move(pi2),
    };
    return run_demo(Card{ std::move(root) }, "Progress · Aurora Demo", 480.0f, 360.0f);
}
