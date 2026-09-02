// LazyList 虚拟滚动 demo：10000 行仅实例化可见窗口，滚轮滚动。
#include "demo_common.h"

auto main() -> int {
    auto list = au::LazyList{
        10000,
        [](int i) -> au::Node {
            au::Text item{ "Row " + std::to_string(i) + " · virtualized list" };
            item.modifier.set(au::Modifier{}.padding(8.0f).clickable(
                [i]() -> void { AURORA_LOG_INFO("demo", "[lazy_list] clicked row " + std::to_string(i) + ""); }));
            return au::Node{ std::move(item) };
        },
        32.0f,
    };

    au::Node root = au::Column{
        GradientTitle{ "LazyList virtual scrolling (10000 rows)" },
        gap(8),
        au::Text{ "Wheel scroll: only instantiate visible area + buffer; click item to log" },
        gap(8),
        std::move(list),
    };
    return run_demo(std::move(root), "LazyList · Aurora Demo", 480.0f, 480.0f);
}
