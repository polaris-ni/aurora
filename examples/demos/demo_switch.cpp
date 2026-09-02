// Switch 控件 demo：布尔开关，点击切换，配合 State 实时反映。
#include "demo_common.h"

auto main() -> int {
    auto on = std::make_shared<au::State<bool>>(false);
    auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "关" });

    au::Switch sw{ au::Reactive{ on }, [on, label](bool v) -> void {
                      on->set(v);
                      label->set(au::LocalizedString{ v ? "开" : "关" });
                  } };

    au::Node root = au::Column{
        GradientTitle{ "Switch 控件" },
        gap(12),
        au::Row{ std::move(sw), au::Text{ au::TextProps{ .content = au::Reactive{ label } } } },
    };
    return run_demo(Card{ std::move(root) }, "Switch · Aurora Demo", 480.0f, 320.0f);
}
