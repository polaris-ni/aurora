// Spacer 控件 demo：吸收剩余空间，把相邻控件推向两端。
#include "demo_common.h"

auto main() -> int {
    au::Column top{
        au::Text{ au::LocalizedString{ "顶部" } },
        au::Spacer{},
        au::Text{ au::LocalizedString{ "底部（Spacer 吸收中间空间）" } },
    };
    top.modifier.set(
        au::Modifier{}.size(320.0f, 200.0f).background(pal::AURORA_SURFACE).border(1.0f, pal::AURORA_BORDER));

    au::Row row{
        au::Text{ au::LocalizedString{ "左" } },
        au::Spacer{},
        au::Text{ au::LocalizedString{ "右" } },
    };
    row.modifier.set(
        au::Modifier{}.size(320.0f, 48.0f).background(pal::AURORA_SURFACE).border(1.0f, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{ "Spacer 控件" }, gap(12), std::move(top), gap(12), std::move(row),
    };
    return run_demo(Card{ std::move(root) }, "Spacer · Aurora Demo", 520.0f, 480.0f);
}
