// LazyList 虚拟滚动 demo：10000 行仅实例化可见窗口，滚轮滚动。
#include "demo_common.h"

auto main() -> int {
    auto list = au::LazyList{
        10000,
        [](int i) -> au::Node {
            au::Text item{ "第 " + std::to_string(i) + " 行 · 虚拟化列表" };
            item.modifier.set(au::Modifier{}.padding(8.0f).clickable(
                [i]() -> void { AURORA_LOG_INFO("demo", "[lazy_list] 点击了第 " + std::to_string(i) + " 行"); }));
            return au::Node{ std::move(item) };
        },
        32.0f,
    };

    au::Node root = au::Column{
        GradientTitle{ "LazyList 虚拟滚动（10000 行）" },
        gap(8),
        au::Text{ "滚轮滚动：仅实例化可见区 + 缓冲区；点击子项输出日志" },
        gap(8),
        std::move(list),
    };
    return run_demo(std::move(root), "LazyList · Aurora Demo", 480.0f, 480.0f);
}
