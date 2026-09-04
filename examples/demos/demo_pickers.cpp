// DatePicker / TimePicker / ColorPicker 选择器 demo。
#include <format>

#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::DatePicker date{au::Date{.year = 2026, .month = 7, .day = 25}};
    date.set_on_change([](au::Date d) -> void { AURORA_LOG_INFO("demo", "Date: ", d.to_string()); });

    au::TimePicker time{au::TimeOfDay{.hour = 14, .minute = 30}};
    time.set_on_change([](au::TimeOfDay t) -> void { AURORA_LOG_INFO("demo", "Time: ", t.to_string()); });

    au::ColorPicker color{au::Color(0, 122, 255, 255)};
    color.set_on_change([](au::Color c) -> void {
        AURORA_LOG_INFO("demo", std::format("Color: #{:02X}{:02X}{:02X}", c.r, c.g, c.b));
    });

    au::Node root = au::Column{
        GradientTitle{"Pickers trio"},
        gap(12),
        au::Row{std::move(date), au::Column{std::move(time), gap(8), std::move(color)}},
    };
    return run_demo(Card{std::move(root)}, "Pickers · Aurora Demo", 560.0F, 420.0F);
}