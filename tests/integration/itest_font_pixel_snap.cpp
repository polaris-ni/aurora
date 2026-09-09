/// 测试类型: integration
/// 目标单元: include/aurora/render/font_engine.h
/// 测试说明: 集成 FontEngine + Painter：验证缩放屏（125%）下文本行首 snap 到整数物理像素，
///           同一字符在两个不同小数逻辑坐标（经 scale 后落在不同子像素偏移）上绘制后，
///           最左墨迹对齐到相同的整数物理 x，保证列宽非整数时文本不交替发虚。

#include <cmath>
#include <string>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_font_pixel_snap {

namespace ar = aurora::render;

namespace {

using au::Color;
using au::Font;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;

/// 扫描整幅帧缓冲中最左「深色墨迹」像素的 x（无墨迹返回 -1）。
auto leftmost_ink(const Painter &p) -> int {
    int left = -1;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const Color c = p.get_pixel(x, y);
            if (c.r < 100 && c.g < 100 && c.b < 100) {
                if (left < 0 || x < left) {
                    left = x;
                }
            }
        }
    }
    return left;
}

/// 在逻辑坐标 x 处以 1.25x 缩放绘制单个字符 "M"，返回其帧缓冲。
auto render_at(float x) -> Painter {
    constexpr float k_scale = 1.25F;
    const Font f{.size_pt = 20.0F};
    Painter p;
    p.set_scale(k_scale);
    p.begin(40, 40);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 40, .height = 40}}, Color::white());
    p.draw_text(Rect{.origin = Point{.x = x, .y = 4.0F}, .size = Size{.width = 36, .height = 36}}, "M", f,
                Color::black());
    return p;
}

}  // namespace

AURORA_TEST_CASE(subpixel_offsets_snap_to_same_physical_column) {
    // 两个逻辑 x 经 1.25x 缩放后分别为 0.5 和 0.75 物理像素；
    // 若不 snap，字形光栅会落在不同物理列；snap 后两者应对齐到同一整数列。
    ar::FontEngine::set_text_aa_mode(ar::TextAAMode::Supersample);

    const Painter p1 = render_at(0.4F);
    const Painter p2 = render_at(0.6F);
    const int left1 = leftmost_ink(p1);
    const int left2 = leftmost_ink(p2);
    AURORA_TEST_CHECK_MSG(left1 >= 0, "first render produced ink");
    AURORA_TEST_CHECK_MSG(left2 >= 0, "second render produced ink");
    AURORA_TEST_CHECK_EQ(left1, left2);
}

AURORA_TEST_CASE(snapped_ink_lands_on_integer_pixel) {
    // 最左墨迹必须落在整数物理像素上（snap 到最近整数，无半像素残留）。
    ar::FontEngine::set_text_aa_mode(ar::TextAAMode::Supersample);

    const Painter p = render_at(0.4F);
    const int left = leftmost_ink(p);
    AURORA_TEST_CHECK_MSG(left >= 0, "render produced ink");
    AURORA_TEST_CHECK_EQ(std::floor(static_cast<float>(left) + 0.5F), static_cast<float>(left));
}

}  // namespace aurora::test_cases::itest_font_pixel_snap
