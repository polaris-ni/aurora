// test_display.cpp — 多显示器枚举：list_displays 非空、primary_display 标记主屏、工作区合理。
#include <cstddef>

#include "aurora/app/display.h"
#include "aurora/window/window.h"
#include "test_harness.h"

namespace app = aurora::app;
using aurora::Display;
using aurora::HeadlessOptions;
using aurora::Point;
using aurora::Window;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_display ===\n");

    const std::vector<Display> all = app::list_displays();
    AURORA_TEST_CHECK(!all.empty());  // 至少一块显示器（无头回退为单默认屏）

    // 恰有一块主显示器。
    std::size_t primaries = 0;
    for (const auto &d : all) {
        if (d.is_primary) {
            ++primaries;
        }
    }
    AURORA_TEST_CHECK(primaries == 1);

    // 主显示器尺寸合理（>0），且工作区不超出整屏。
    const Display primary = app::primary_display();
    AURORA_TEST_CHECK(primary.is_primary);
    AURORA_TEST_CHECK(primary.bounds.size.width > 0.0F && primary.bounds.size.height > 0.0F);
    AURORA_TEST_CHECK(primary.work_area.size.width > 0.0F && primary.work_area.size.height > 0.0F);
    AURORA_TEST_CHECK(primary.work_area.size.width <= primary.bounds.size.width + 1e-3F);
    AURORA_TEST_CHECK(primary.work_area.size.height <= primary.bounds.size.height + 1e-3F);

    // display_containing：落点在屏内返回某显示器；屏外回退主屏（无头下均为默认屏）。
    const Display inside = app::display_containing(Point{.x = 100.0F, .y = 100.0F});
    AURORA_TEST_CHECK(inside.bounds.size.width > 0.0F);
    const Display outside = app::display_containing(Point{.x = 1.0e9F, .y = 1.0e9F});
    AURORA_TEST_CHECK(outside.bounds.size.width > 0.0F);

    // move_window_to_display：无头下 native_handle 为 nullptr → no-op，不崩溃（含未知 id 回退主屏）。
    {
        auto win_res = create_window(HeadlessOptions{});
        AURORA_TEST_CHECK(static_cast<bool>(win_res));
        if (win_res) {
            Window &win = *win_res.value();
            app::move_window_to_display(win, 0);  // 不存在的 id → 回退主屏
            app::move_window_to_display(win, inside.id);  // 存在的 id
        }
    }
}
