// Splitter 控件 demo：左右分栏可拖拽调整比例（拖动中间分隔条）。
#include "demo_common.h"

auto main() -> int {
    auto splitter = au::HSplitter(au::Node{ au::Column{ au::Text{ "侧边栏" }, au::Text{ "拖动分隔条调整" } } },
                                  au::Node{ au::Column{ au::Text{ "主内容区" } } }, 0.3f);
    splitter.set_min_sizes(80.0f, 120.0f);
    splitter.set_on_ratio_change([](float r) -> void { AURORA_LOG_INFO("demo", "比例: ", r); });

    au::Node root = au::Column{
        GradientTitle{ "Splitter 分割器" },
        gap(12),
        std::move(splitter),
    };
    return run_demo(Card{ std::move(root) }, "Splitter · Aurora Demo", 560.0f, 400.0f);
}
