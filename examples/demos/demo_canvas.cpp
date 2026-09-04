// Canvas 控件 demo：自定义绘制回调（仪表条）。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Canvas gauge{
        220, 120, [](au::Painter& p, const au::Rect& b) -> void {
            constexpr float v = 0.7F;
            constexpr float pad = 12.0F;
            const au::Rect track{.origin = {.x = b.origin.x + pad, .y = b.origin.y + (b.size.height * 0.5F) - 8.0F},
                                 .size = {.width = b.size.width - (2.0F * pad), .height = 16.0F}};
            p.fill_rect(track, au::Color{226, 232, 240});
            p.fill_rect(au::Rect{.origin = track.origin,
                                 .size = au::Size{.width = track.size.width * v, .height = track.size.height}},
                        pal::AURORA_PRIMARY);
            const float th = aurora::render::FontEngine::measure_height(au::Font{.size_pt = 14.0F});
            p.draw_text(au::Rect{.origin = au::Point{.x = b.origin.x + pad, .y = b.origin.y + 8.0F},
                                 .size = au::Size{.width = b.size.width - (2.0F * pad), .height = th}},
                        "CPU " + std::to_string(static_cast<int>(v * 100)) + "%", au::Font{.size_pt = 14.0F},
                        pal::AURORA_TEXT);
        }};

    au::Node root = au::Column{
        GradientTitle{"Canvas widget"},
        gap(12),
        au::Text{au::LocalizedString{"Free drawing via Painter callback"}},
        std::move(gauge),
    };
    return run_demo(Card{std::move(root)}, "Canvas · Aurora Demo", 520.0F, 380.0F);
}