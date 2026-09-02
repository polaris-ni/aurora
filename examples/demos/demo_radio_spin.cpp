// RadioGroup / SpinBox 控件 demo：互斥单选组 + 数字调节框。
#include "demo_common.h"

auto main() -> int {
    au::RadioGroup size_group{ std::vector<std::string>{ "Small", "Medium", "Large" }, 1 };
    size_group.set_on_change([](int i) -> void { AURORA_LOG_INFO("demo", "Selected: ", i); });

    au::SpinBox quantity{ 1.0, 1.0, 99.0, 1.0 };
    quantity.set_suffix(" items");
    quantity.set_on_change([](double v) -> void { AURORA_LOG_INFO("demo", "Quantity: ", v); });

    au::SpinBox price{ 9.9, 0.0, 999.0, 0.1 };
    price.set_prefix("$").set_decimals(1);

    au::Node root = au::Column{
        GradientTitle{ "RadioGroup / SpinBox" },
        gap(12),
        au::Text{ "Size (radio group):" },
        std::move(size_group),
        gap(12),
        au::Text{ "Quantity / price (number input):" },
        au::Row{ std::move(quantity), std::move(price) },
    };
    return run_demo(Card{ std::move(root) }, "RadioSpin · Aurora Demo", 480.0f, 400.0f);
}
