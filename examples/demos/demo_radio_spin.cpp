// RadioGroup / SpinBox 控件 demo：互斥单选组 + 数字调节框。
#include "demo_common.h"

auto main() -> int {
    au::RadioGroup size_group{ std::vector<std::string>{ "小", "中", "大" }, 1 };
    size_group.set_on_change([](int i) -> void { AURORA_LOG_INFO("demo", "选中: ", i); });

    au::SpinBox quantity{ 1.0, 1.0, 99.0, 1.0 };
    quantity.set_suffix(" 件");
    quantity.set_on_change([](double v) -> void { AURORA_LOG_INFO("demo", "数量: ", v); });

    au::SpinBox price{ 9.9, 0.0, 999.0, 0.1 };
    price.set_prefix("$").set_decimals(1);

    au::Node root = au::Column{
        GradientTitle{ "RadioGroup / SpinBox" },
        gap(12),
        au::Text{ "尺寸（单选组）:" },
        std::move(size_group),
        gap(12),
        au::Text{ "数量 / 价格（数字输入）:" },
        au::Row{ std::move(quantity), std::move(price) },
    };
    return run_demo(Card{ std::move(root) }, "RadioSpin · Aurora Demo", 480.0f, 400.0f);
}
