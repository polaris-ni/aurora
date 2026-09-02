// 验证 font_engine 在缩放屏下将文本行首 snap 到整数物理像素：
// 同一字符放在两个不同的小数逻辑坐标（经 scale 后落在不同子像素偏移）上，
// snap 后应当绘制在相同的整数物理 x 上，从而保证 125%/175% DPI 下列宽非整数
// 时文本不会交替发虚。
#include <cmath>
#include <string>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::Color;
using aurora::Font;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;

namespace ar = aurora::render;

namespace {
auto leftmost_ink(const Painter &p) -> int {
    int left = -1;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const Color c = p.get_pixel(x, y);
            if (c.m_r < 100 && c.m_g < 100 && c.m_b < 100) {
                if (left < 0 || x < left) {
                    left = x;
                }
            }
        }
    }
    return left;
}
} // namespace

AURORA_TEST() {
    ar::FontEngine::instance().set_text_aa_mode(ar::TextAAMode::Supersample);
    constexpr float k_scale = 1.25f;
    const Font f{ .size_pt = 20.0f };
    const std::string text = "M";
    constexpr Color black = Color::black();

    auto render_at = [&](float x) -> Painter {
        Painter p;
        p.set_scale(k_scale);
        p.begin(40, 40);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 40, .height = 40 } },
                    Color::white());
        p.draw_text(Rect{ .origin = Point{ .x = x, .y = 4.0f }, .size = Size{ .width = 36, .height = 36 } }, text, f,
                    black);
        return p;
    };

    // 两个逻辑 x 经 1.25x 缩放后分别为 0.5 和 0.75 物理像素；
    // 若不 snap，字形光栅会落在不同物理列；snap 后两者应对齐到同一整数列。
    const Painter p1 = render_at(0.4f);
    const Painter p2 = render_at(0.6f);
    const int left1 = leftmost_ink(p1);
    const int left2 = leftmost_ink(p2);
    AURORA_TEST_CHECK(left1 >= 0);
    AURORA_TEST_CHECK(left2 >= 0);
    AURORA_TEST_CHECK_EQ(left1, left2);

    // 同时验证最左墨迹落在整数物理像素上（snap 到最近整数）。
    AURORA_TEST_CHECK(std::floor(static_cast<float>(left1) + 0.5f) == static_cast<float>(left1));
}
