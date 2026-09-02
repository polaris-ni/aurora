// DatePicker / TimePicker / ColorPicker 选择器 demo。
#include <format>

#include "demo_common.h"

auto main() -> int {
    au::DatePicker date{ au::Date{ 2026, 7, 25 } };
    date.set_on_change([](au::Date d) -> void { AURORA_LOG_INFO("demo", "日期: ", d.to_string()); });

    au::TimePicker time{ au::TimeOfDay{ 14, 30 } };
    time.set_on_change([](au::TimeOfDay t) -> void { AURORA_LOG_INFO("demo", "时间: ", t.to_string()); });

    au::ColorPicker color{ au::Color(0, 122, 255, 255) };
    color.set_on_change([](au::Color c) -> void {
        AURORA_LOG_INFO("demo", std::format("颜色: #{:02X}{:02X}{:02X}", c.m_r, c.m_g, c.m_b));
    });

    au::Node root = au::Column{
        GradientTitle{ "Pickers 选择器三件套" },
        gap(12),
        au::Row{ std::move(date), au::Column{ std::move(time), gap(8), std::move(color) } },
    };
    return run_demo(Card{ std::move(root) }, "Pickers · Aurora Demo", 560.0f, 420.0f);
}
