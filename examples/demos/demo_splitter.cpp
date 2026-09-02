// Splitter 控件 demo：左右分栏可拖拽调整比例（拖动中间分隔条）。
#include "demo_common.h"

auto main() -> int {
    auto splitter = au::HSplitter(au::Node{ au::Column{ au::Text{ "Sidebar" }, au::Text{ "Drag divider to adjust" } } },
                                  au::Node{ au::Column{ au::Text{ "Main content area" } } }, 0.3f);
    splitter.set_min_sizes(80.0f, 120.0f);
    splitter.set_on_ratio_change([](float r) -> void { AURORA_LOG_INFO("demo", "Ratio: ", r); });

    au::Node root = au::Column{
        GradientTitle{ "Splitter" },
        gap(12),
        std::move(splitter),
    };
    return run_demo(Card{ std::move(root) }, "Splitter · Aurora Demo", 560.0f, 400.0f);
}
