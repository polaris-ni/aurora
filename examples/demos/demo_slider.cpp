// Slider 控件 demo：连续值 [0,1]，拖拽设置，配合 State 实时反映。
#include "demo_common.h"

auto main() -> int {
    auto val = std::make_shared<au::State<double>>(0.5);
    auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "值 0.50" });

    au::Slider sl{ au::Reactive{ val }, [val, label](double v) -> void {
                      val->set(v);
                      label->set(au::LocalizedString{ "值 " + std::to_string(v) });
                  } };
    sl.set_active_color(pal::AURORA_PRIMARY);

    au::Node root = au::Column{
        GradientTitle{ "Slider 控件" },
        gap(12),
        std::move(sl),
        au::Text{ au::TextProps{ .content = au::Reactive{ label } } },
    };
    return run_demo(Card{ std::move(root) }, "Slider · Aurora Demo", 480.0f, 360.0f);
}
