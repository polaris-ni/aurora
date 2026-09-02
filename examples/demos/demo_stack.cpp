// Stack 控件 demo：多层从 (0,0) 叠加，可用 Alignment 对齐。
#include "demo_common.h"

auto main() -> int {
    au::Node bottom =
        au::Stack{ au::Text{ au::LocalizedString{ "底层文本" } }, au::Text{ au::LocalizedString{ "上层叠加文本" } } };
    au::Node centered =
        au::Stack{ std::vector<au::Node>{ au::Text{ au::LocalizedString{ "居中叠加" } } }, au::Alignment::Center };

    au::Node root = au::Column{
        GradientTitle{ "Stack 控件" },
        gap(12),
        au::Text{ au::LocalizedString{ "默认 top-left 叠加" } },
        std::move(bottom),
        gap(12),
        au::Text{ au::LocalizedString{ "Alignment::Center 叠加" } },
        std::move(centered),
    };
    return run_demo(Card{ std::move(root) }, "Stack · Aurora Demo", 520.0f, 440.0f);
}
