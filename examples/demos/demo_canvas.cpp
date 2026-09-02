// Canvas 控件 demo：自定义绘制回调（仪表条）。
#include "demo_common.h"

auto main() -> int {
    au::Canvas gauge{ 220, 120, [](au::Painter &p, const au::Rect &b) -> void {
                         constexpr float v = 0.7f;
                         constexpr float pad = 12.0f;
                         const au::Rect track{ .origin = { .x = b.origin.x + pad,
                                                           .y = b.origin.y + (b.size.height * 0.5f) - 8.0f },
                                               .size = { .width = b.size.width - (2.0f * pad), .height = 16.0f } };
                         p.fill_rect(track, au::Color{ 226, 232, 240 });
                         p.fill_rect(
                             au::Rect{ .origin = track.origin,
                                       .size = au::Size{ .width = track.size.width * v, .height = track.size.height } },
                             pal::AURORA_PRIMARY);
                         const float th = aurora::render::FontEngine::measure_height(au::Font{ .size_pt = 14.0f });
                         p.draw_text(au::Rect{ .origin = au::Point{ .x = b.origin.x + pad, .y = b.origin.y + 8.0f },
                                               .size = au::Size{ .width = b.size.width - (2.0f * pad), .height = th } },
                                     "CPU " + std::to_string(static_cast<int>(v * 100)) + "%",
                                     au::Font{ .size_pt = 14.0f }, pal::AURORA_TEXT);
                     } };

    au::Node root = au::Column{
        GradientTitle{ "Canvas 控件" },
        gap(12),
        au::Text{ au::LocalizedString{ "通过 Painter 回调自由绘制" } },
        std::move(gauge),
    };
    return run_demo(Card{ std::move(root) }, "Canvas · Aurora Demo", 520.0f, 380.0f);
}
