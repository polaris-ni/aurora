// demo_timers.cpp — 定时任务模块演示：时钟 / 倒计时 / 自动显现。
#include <chrono>
#include <ctime>
#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"

#include "demo_common.h"

using namespace std::chrono_literals;
using au::App;

/// @brief 当前本地时间字符串 HH:MM:SS。
static auto now_string() -> std::string {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::array<char, 16> buf = {};
    std::tm tm_buf{};
#ifdef AURORA_PLATFORM_WINDOWS
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::strftime(buf.data(), sizeof(buf), "%H:%M:%S", &tm_buf);
    return std::string{ buf.data() };
}

/// @brief 时钟：每秒经 on_tick 写入响应式字符串，Text 自动刷新。
static auto build_clock() -> au::Node {
    auto clock_str = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "clock: --:--:--" });
    return au::Timer(
        1s,
        [clock_str](const au::SignalView<int> &) -> au::Text {
            return au::Text{ au::TextProps{ .content = au::Reactive{ clock_str } } };
        },
        [clock_str](int) -> void { clock_str->set(au::LocalizedString{ "clock: " + now_string() }); });
}

/// @brief 倒计时：每秒递减，归零显示 done。
static auto build_countdown() -> au::Node {
    auto cd = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "countdown: 10" });
    return au::Timer(
        1s, [cd](const au::SignalView<int> &) -> au::Text { return au::Text{ au::TextProps{ .content = au::Reactive{ cd } } }; },
        [cd](int n) -> void {
            const int left = 10 - n;
            cd->set(au::LocalizedString{ left > 0 ? "countdown: " + std::to_string(left) : "countdown: done" });
        });
}

/// @brief 自动显现：2 秒后由 on_tick 置位 shown，Show 响应式显示提示。
static auto build_auto_reveal() -> au::Node {
    auto shown = std::make_shared<au::State<bool>>(false);
    return au::Timer(
        2s,
        [shown](const au::SignalView<int> &) -> au::Show {
            return au::Show(shown, au::Text{ au::LocalizedString{ "auto-revealed after 2s!" } });
        },
        [shown](int) -> void { shown->set(true); });
}

static auto build_root() -> au::Node {
    return au::Column{
        GradientTitle{ "定时任务 / Timer" }, gap(12), build_clock(), build_countdown(), build_auto_reveal(),
    };
}

auto main() -> int { return run_demo(build_root(), "Timer · Aurora Demo", 520.0f, 440.0f); }
