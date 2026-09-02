// demo_multitouch.cpp — 多点触控并发 + 原始流演示（v0.10.0-alpha2）。
// 展示：① 单指持发 + 多指并发（每根手指独立路由到各自的命中控件）；
//       ② touch() 修饰器交付完整 TouchEvent 原始流（触点数量可在控制台观察）。
#include "aurora/event/event.h"

#include "demo_common.h"

auto main() -> int {
    // 两个可拖拽方块：每根手指独立路由到各自命中的控件，互不干扰。
    au::Button a;
    a.label.set(au::LocalizedString{ "拖我 A" });
    a.modifier.set(au::Modifier{}
                       .size(160.0f, 120.0f)
                       .background(pal::AURORA_PRIMARY)
                       .draggable([](au::Point, au::Point) -> void { AURORA_LOG_INFO("demo", "[drag] A"); },
                                  []() -> void {}, []() -> void {}));

    au::Button b;
    b.label.set(au::LocalizedString{ "拖我 B" });
    b.modifier.set(au::Modifier{}
                       .size(160.0f, 120.0f)
                       .background(pal::AURORA_ACCENT)
                       .draggable([](au::Point, au::Point) -> void { AURORA_LOG_INFO("demo", "[drag] B"); },
                                  []() -> void {}, []() -> void {}));

    // 原始多点流监听区：每次触摸派发打印触点数量（原始流）。
    au::Button t;
    t.label.set(au::LocalizedString{ "原始流监听区（多指触摸看控制台）" });
    t.modifier.set(
        au::Modifier{}.size(320.0f, 80.0f).background(pal::AURORA_SURFACE).touch([](const au::TouchEvent &e) -> void {
            AURORA_LOG_INFO("demo", "[multitouch] points=", e.points.size(), " active=", e.active_count());
        }));

    au::Node root = au::Column{
        GradientTitle{ "多点触控 / Multi-Touch" },
        gap(12),
        au::Row{ std::move(a), gap(12), std::move(b) },
        gap(12),
        std::move(t),
        au::Text{ au::LocalizedString{ "单指持发 + 多指并发：每根手指独立路由到各自的命中控件；" } },
        au::Text{ au::LocalizedString{ "touch() 修饰器交付完整 TouchEvent 原始流。" } },
    };
    return run_demo(Card{ std::move(root) }, "Multi-Touch · Aurora Demo", 620.0f, 460.0f);
}
