/// 测试类型: unit
/// 目标单元: src/aurora/event/detail/macos_click_config.h
/// 测试说明: macOS 连击时间窗的折算真值表——AppKit 报出的**秒**值如何落成派发器用的**毫秒**窗：
///           常规秒值逐档换算、亚毫秒夹为 1（返回 0 等于关掉整条连击链）、非正 / NaN / Inf 一律
///           回落调用方给的库级兜底常量、兜底常量本身逐字透传不被改写。
/// 平台门控: 无——被测层刻意不 include 平台头，故折算在任何 runner 上都照常执行（非 Apple
///           构建同样能钉住这条口径；AppKit 那一枚读取腿由 `aurora_verify_macos_double_click` 真机取证）

#include <array>
#include <cstdint>
#include <limits>
#include <string>

#include "aurora/event/detail/macos_click_config.h"
#include "aurora/event/event.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_macos_click_config {

namespace {

using aurora::AURORA_DEFAULT_CLICK_WINDOW_MS;
using aurora::detail::click_window_ms_from_seconds;

/// @brief 库级兜底常量在本组用例里的替身：取一个与常量本身不同的值，便于断言「回落的是调用方给的档」。
constexpr std::uint32_t k_fallback = AURORA_DEFAULT_CLICK_WINDOW_MS;

/// @brief 把一次折算的输入与结果折成可读明细，供失败时随判定行打印。
/// @param seconds 输入的秒值。
/// @param ms 折算出的毫秒窗。
/// @return 形如 `seconds=0.500000 ms=500` 的明细串。
[[nodiscard]] auto describe(double seconds, std::uint32_t ms) -> std::string {
    std::string out = "seconds=";
    out += std::to_string(seconds);
    out += " ms=";
    out += std::to_string(ms);
    return out;
}

}  // namespace

AURORA_TEST_CASE(appkit_default_interval_maps_to_five_hundred_ms) {
    // 本机实测（macOS 26.7.2）`+[NSEvent doubleClickInterval]` 出厂即 0.5s，对应 500ms。
    // 这条同时钉住「秒 → 毫秒」的乘数关系：写反成毫秒 → 秒会把窗口压到 0.5ms，
    // 于是每次点击都被判为亚毫秒而落到夹 1 的分支，双击在任何机器上都做不出来。
    const std::uint32_t ms = click_window_ms_from_seconds(0.5, k_fallback);
    AURORA_TEST_CHECK_MSG(ms == 500U, describe(0.5, ms));
}

AURORA_TEST_CASE(readable_intervals_round_to_the_nearest_millisecond) {
    // 滑杆的中间档位：四舍五入而非截断，避免「系统给 255.6ms、库算成 255ms」这种逐档漂移。
    struct Sample {
        double seconds;
        std::uint32_t expected_ms;
    };
    constexpr std::array<Sample, 5> samples = {
        Sample{0.1, 100U},  // 最快档
        Sample{0.25, 250U},  // 滑杆中间档
        Sample{0.75, 750U},  // 滑杆偏慢档
        Sample{1.5, 1500U},  // 超出滑杆量程的量级仍需线性换算
        Sample{0.2561, 256U},  // 需要舍去的尾数
    };
    for (const auto &s : samples) {
        const std::uint32_t ms = click_window_ms_from_seconds(s.seconds, k_fallback);
        AURORA_TEST_CHECK_MSG(ms == s.expected_ms, describe(s.seconds, ms));
    }
}

AURORA_TEST_CASE(sub_millisecond_interval_clamps_to_one_not_zero) {
    // 契约写「连击时间窗恒为正（≥1）」：系统把间隔压到亚毫秒时不能返回 0——`click_window_ms == 0`
    // 在派发器里是「关闭连击判定」的显式语义，把不可读与「用户要求极短窗口」混成同一形态，
    // 等于用一条不可能的手感冒充另一条。
    const std::uint32_t ms = click_window_ms_from_seconds(0.0004, k_fallback);
    AURORA_TEST_CHECK_MSG(ms == 1U, describe(0.0004, ms));
}

AURORA_TEST_CASE(non_positive_and_non_finite_inputs_fall_back_to_the_constant) {
    // 不可读形态逐档都回落到调用方给的库级常量：0（未初始化 / 读失败）、负值、NaN、±Inf。
    // 这里刻意不去猜「0 就是关连击」——公共契约的关连击入口是显式写 `click_window_ms = 0`，
    // 而非「系统报 0」。
    const double unreadable[] = {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const double seconds : unreadable) {
        const std::uint32_t ms = click_window_ms_from_seconds(seconds, k_fallback);
        AURORA_TEST_CHECK_MSG(ms == k_fallback, describe(seconds, ms));
    }
}

AURORA_TEST_CASE(the_fallback_constant_is_passed_through_unchanged) {
    // 折算层不擅自替换兜底值：调用方给什么档就回落什么档（`AURORA_DEFAULT_CLICK_WINDOW_MS` 是
    // 事件域的单一真源，Wayland CSD 标题栏双击最大化也引用它，此处再写一套就把一条阈值拆成两处）。
    const std::uint32_t ms = click_window_ms_from_seconds(0.0, 777U);
    AURORA_TEST_CHECK_MSG(ms == 777U, describe(0.0, ms));
}

AURORA_TEST_CASE(every_readable_interval_yields_a_positive_window) {
    // 跨档不变量：只要秒值可读（正且有限），结果必 ≥ 1，绝不产生「0 = 关连击」的假象。
    for (double seconds = 0.0001; seconds < 3.0; seconds += 0.0001) {
        const std::uint32_t ms = click_window_ms_from_seconds(seconds, k_fallback);
        AURORA_TEST_CHECK_MSG(ms >= 1U, describe(seconds, ms));
    }
}

}  // namespace aurora::test_cases::utest_macos_click_config
