// demo_multitouch.cpp — 多点触控并发 + 原始流演示。
// 展示：① 单指持发 + 多指并发（每根手指独立路由到各自的命中控件）；
//       ② touch() 修饰器交付完整 TouchEvent 原始流（触点数量可在控制台观察）。
#include "aurora/event/event.h"
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    // 两个可拖拽方块：每根手指独立路由到各自命中的控件，互不干扰。
    au::Button a;
    a.label.set(au::LocalizedString{"Drag me A"});
    a.modifier.set(au::Modifier{}
                       .size(160.0F, 120.0F)
                       .background(pal::AURORA_PRIMARY)
                       .draggable([](au::Point, au::Point) -> void { AURORA_LOG_INFO("demo", "[drag] A"); },
                                  []() -> void {}, []() -> void {}));

    au::Button b;
    b.label.set(au::LocalizedString{"Drag me B"});
    b.modifier.set(au::Modifier{}
                       .size(160.0F, 120.0F)
                       .background(pal::AURORA_ACCENT)
                       .draggable([](au::Point, au::Point) -> void { AURORA_LOG_INFO("demo", "[drag] B"); },
                                  []() -> void {}, []() -> void {}));

    // 原始多点流监听区：每次触摸派发打印触点数量（原始流）。
    au::Button t;
    t.label.set(au::LocalizedString{"Raw stream listener (multi-touch; watch console)"});
    t.modifier.set(
        au::Modifier{}.size(320.0F, 80.0F).background(pal::AURORA_SURFACE).touch([](const au::TouchEvent& e) -> void {
            AURORA_LOG_INFO("demo", "[multitouch] points=", e.points.size(), " active=", e.active_count());
        }));

    au::Node root = au::Column{
        GradientTitle{"Multi-Touch"},
        gap(12),
        au::Row{std::move(a), gap(12), std::move(b)},
        gap(12),
        std::move(t),
        au::Text{au::LocalizedString{"Single-finger hold-release + multi-finger concurrent: each finger routed "
                                     "independently to its hit widget;"}},
        au::Text{au::LocalizedString{"touch() modifier delivers the full TouchEvent raw stream."}},
    };
    return run_demo(Card{std::move(root)}, "Multi-Touch · Aurora Demo", 620.0F, 460.0F);
}