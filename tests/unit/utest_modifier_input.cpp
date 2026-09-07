/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_input.h
/// 测试说明: 输入修饰节点（Clickable / Draggable / LongPress / TouchListener / Tooltip / ContextMenu）单元测试

#include <chrono>
#include <optional>
#include <vector>

#include "aurora/modifier/modifier_input.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_modifier_input {

namespace {

using au::Clickable;
using au::ContextMenuNode;
using au::Draggable;
using au::LongPress;
using au::MenuItem;
using au::ModifierNode;
using au::Point;
using au::TouchEvent;
using au::TouchListener;
using au::TooltipNode;

using Clock = std::chrono::steady_clock;

}  // namespace

AURORA_TEST() {
    // ---- 1. Clickable：fire_click 与 on_tap 等价触发 ----
    {
        int taps = 0;
        const Clickable c{[&]() -> void { ++taps; }};
        AURORA_TEST_CHECK(c.kind() == ModifierNode::Kind::Input);

        c.on_tap();
        AURORA_TEST_CHECK(taps == 1);
        c.fire_click();  // 派发器入口，复用同一回调
        AURORA_TEST_CHECK(taps == 2);
    }

    // ---- 2. Draggable：指针绑定与匹配 ----
    {
        const Draggable d{[](Point /*delta*/, Point /*pos*/) -> void {}, []() -> void {}, []() -> void {}};
        AURORA_TEST_CHECK(d.matches(1));  // 未绑定时视为通配
        d.bind(7);
        AURORA_TEST_CHECK(d.matches(7));
        AURORA_TEST_CHECK(!d.matches(8));
        d.release();
        AURORA_TEST_CHECK(d.matches(8));  // 解绑后重新通配
    }

    // ---- 3. Draggable：start / drag / end 回调序列 ----
    {
        std::vector<int> seq;
        Point seen_delta{};
        const Draggable d{[&](Point delta, Point /*pos*/) -> void {
                              seen_delta = delta;
                              seq.push_back(2);
                          },
                          [&]() -> void { seq.push_back(1); },
                          [&]() -> void { seq.push_back(3); }};
        d.fire_start();
        d.fire_drag(Point{.x = 3.0F, .y = 4.0F}, Point{.x = 10.0F, .y = 20.0F});
        d.fire_end();

        AURORA_TEST_CHECK(seq.size() == 3);
        AURORA_TEST_CHECK(seq.at(0) == 1 && seq.at(1) == 2 && seq.at(2) == 3);
        AURORA_TEST_CHECK(seen_delta.x == 3.0F && seen_delta.y == 4.0F);
    }

    // ---- 4. LongPress：未达阈值不触发，超过阈值触发一次（幂等） ----
    {
        int fired = 0;
        LongPress lp{[&]() -> void { ++fired; }, 500.0F};
        const auto t0 = Clock::now();

        lp.press_at(t0);
        AURORA_TEST_CHECK(!lp.long_press_fired());

        lp.tick(t0 + std::chrono::milliseconds(100));
        AURORA_TEST_CHECK(fired == 0);

        lp.tick(t0 + std::chrono::milliseconds(600));
        AURORA_TEST_CHECK(fired == 1);
        AURORA_TEST_CHECK(lp.long_press_fired());

        lp.tick(t0 + std::chrono::milliseconds(1200));  // 幂等，不重复触发
        AURORA_TEST_CHECK(fired == 1);
    }

    // ---- 5. LongPress：cancel 后不再触发 ----
    {
        int fired = 0;
        LongPress lp{[&]() -> void { ++fired; }, 100.0F};
        const auto t0 = Clock::now();
        lp.press_at(t0);
        lp.cancel();
        lp.tick(t0 + std::chrono::milliseconds(500));
        AURORA_TEST_CHECK(fired == 0);
        AURORA_TEST_CHECK(!lp.long_press_fired());
    }

    // ---- 6. TouchListener：完整事件透传 ----
    {
        int seen = 0;
        const TouchListener tl{[&](const TouchEvent & /*e*/) -> void { ++seen; }};
        const TouchEvent ev{};
        tl.on_touch(ev);
        AURORA_TEST_CHECK(seen == 1);
    }

    // ---- 7. TooltipNode：悬停达延迟后可见，离开后隐藏 ----
    {
        const TooltipNode tip{"保存", 200.0F};
        AURORA_TEST_CHECK(tip.text() == "保存");
        AURORA_TEST_CHECK(tip.delay_ms() == 200.0F);
        AURORA_TEST_CHECK(!tip.is_visible());

        const auto t0 = Clock::now();
        tip.hover_start(t0);
        tip.tick(t0 + std::chrono::milliseconds(50));
        AURORA_TEST_CHECK(!tip.is_visible());

        tip.tick(t0 + std::chrono::milliseconds(250));
        AURORA_TEST_CHECK(tip.is_visible());

        tip.hover_end();
        AURORA_TEST_CHECK(!tip.is_visible());
    }

    // ---- 8. ContextMenuNode：开合与位置记录 ----
    {
        std::vector<MenuItem> items;
        items.emplace_back("复制");
        items.emplace_back("粘贴");
        const ContextMenuNode menu{std::move(items)};
        AURORA_TEST_CHECK(menu.items().size() == 2);
        AURORA_TEST_CHECK(!menu.is_open());

        menu.open_at(Point{.x = 12.0F, .y = 34.0F});
        AURORA_TEST_CHECK(menu.is_open());
        AURORA_TEST_CHECK(menu.position().x == 12.0F);
        AURORA_TEST_CHECK(menu.position().y == 34.0F);

        menu.close();
        AURORA_TEST_CHECK(!menu.is_open());
    }
}

}  // namespace aurora::test_cases::utest_modifier_input
