/// 测试类型: unit
/// 目标单元: include/aurora/render/painter.h
/// 测试说明: 覆盖 Painter 的画布分配与零基底、fill_rect 覆写与 alpha 混合、clear_rect 复位、draw_rect 描边、
/// 裁剪栈（矩形/圆角/边界查询）、global_alpha 乘入、blend_pixel、draw_image 缩放、线性渐变方向、
/// draw_line 覆盖、shift_pixels 垂直搬移、to_image 导出与 get_pixel 越界兜底

#include <cstddef>
#include <cstdint>

#include "aurora/core/image.h"
#include "aurora/render/painter.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_painter {

namespace {
[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 统计画布中「红色像素」（R 通道显著高于 G/B）的数量。
[[nodiscard]] auto count_reddish(const Painter &p) -> int {
    int count = 0;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const Color c = p.get_pixel(x, y);
            if (c.r > 128 && c.g < 64 && c.b < 64) {
                ++count;
            }
        }
    }
    return count;
}
}  // namespace

AURORA_TEST_CASE(begin_allocates_zeroed_canvas) {
    Painter p;
    p.begin(8, 8);
    AURORA_TEST_CHECK_EQ(p.width(), 8);
    AURORA_TEST_CHECK_EQ(p.height(), 8);
    AURORA_TEST_REQUIRE_NOT_NULL(p.data());
    // 新帧基底为全零（含 alpha）：脏区重绘依赖该不变量。
    AURORA_TEST_CHECK_EQ(p.get_pixel(4, 4), Color{0, 0, 0, 0});
}

AURORA_TEST_CASE(scale_multiplies_logical_size_into_physical_buffer) {
    Painter p;
    p.set_scale(2.0F);
    p.begin(4, 4);
    AURORA_TEST_CHECK_NEAR(p.scale(), 2.0, 1e-6);
    AURORA_TEST_CHECK_EQ(p.width(), 8);
    AURORA_TEST_CHECK_EQ(p.height(), 8);
}

AURORA_TEST_CASE(fill_rect_overwrites_opaque) {
    Painter p;
    p.begin(4, 4);
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F), Color::red());
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 2), Color::red());
}

AURORA_TEST_CASE(fill_rect_blends_by_source_alpha) {
    Painter p;
    p.begin(4, 4);
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F), Color::red());

    // 全透明源：不产生任何改变。
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F), Color{0, 0, 255, 0});
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 2), Color::red());

    // 半透明源：向源色靠拢但非完全替换。
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F), Color{0, 0, 255, 128});
    const Color blended = p.get_pixel(2, 2);
    AURORA_TEST_CHECK_LT(blended.r, 255);
    AURORA_TEST_CHECK_GT(blended.b, 0);
}

AURORA_TEST_CASE(clear_rect_resets_to_zero_base) {
    Painter p;
    p.begin(4, 4);
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F), Color::red());
    p.clear_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F));
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 2), Color{0, 0, 0, 0});
}

AURORA_TEST_CASE(draw_rect_strokes_border_only) {
    Painter p;
    p.begin(8, 8);
    p.fill_rect(rect_at(0.0F, 0.0F, 8.0F, 8.0F), Color::black());
    p.draw_rect(rect_at(0.0F, 0.0F, 8.0F, 8.0F), Color::red());

    // 内部保持背景色，边框出现红色像素。
    AURORA_TEST_CHECK_EQ(p.get_pixel(4, 4), Color::black());
    AURORA_TEST_CHECK_GT(count_reddish(p), 0);
}

AURORA_TEST_CASE(global_alpha_multiplies_into_draws) {
    Painter p;
    p.begin(4, 4);
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F), Color::red());

    p.set_alpha(0.0);
    AURORA_TEST_CHECK_NEAR(p.global_alpha(), 0.0, 1e-9);
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F), Color::blue());
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 2), Color::red());  // alpha=0 → 无影响

    p.set_alpha(1.0);
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 4.0F), Color::blue());
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 2), Color::blue());
}

AURORA_TEST_CASE(rect_clip_confines_drawing) {
    Painter p;
    p.begin(8, 8);
    p.push_clip(rect_at(0.0F, 0.0F, 2.0F, 8.0F));
    AURORA_TEST_CHECK_TRUE(p.has_clip());

    p.fill_rect(rect_at(0.0F, 0.0F, 8.0F, 8.0F), Color::red());
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, 0).a, 255);
    AURORA_TEST_CHECK_EQ(p.get_pixel(6, 6).a, 0);  // 裁剪外不落笔

    p.pop_clip();
    AURORA_TEST_CHECK_FALSE(p.has_clip());
}

AURORA_TEST_CASE(clip_bounds_covers_canvas_without_clip) {
    Painter p;
    p.begin(8, 8);
    const Rect bounds = p.clip_bounds();
    AURORA_TEST_CHECK_NEAR(bounds.size.width, 8.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.size.height, 8.0, 1e-6);
}

AURORA_TEST_CASE(clip_bounds_intersects_pushed_rect) {
    Painter p;
    p.begin(8, 8);
    p.push_clip(rect_at(2.0F, 2.0F, 4.0F, 4.0F));
    const Rect bounds = p.clip_bounds();
    AURORA_TEST_CHECK_NEAR(bounds.origin.x, 2.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.origin.y, 2.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.size.width, 4.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.size.height, 4.0, 1e-6);
}

AURORA_TEST_CASE(rounded_fill_cuts_corners) {
    Painter p;
    p.begin(8, 8);
    p.fill_rounded_rect(rect_at(0.0F, 0.0F, 8.0F, 8.0F), 4.0F, Color::red());

    AURORA_TEST_CHECK_EQ(p.get_pixel(4, 4), Color::red());       // 中心填满
    AURORA_TEST_CHECK_LT(p.get_pixel(0, 0).a, 255);              // 圆角外未填满
}

AURORA_TEST_CASE(blend_pixel_writes_single_pixel) {
    Painter p;
    p.begin(4, 4);
    p.blend_pixel(1, 1, Color::red());
    AURORA_TEST_CHECK_EQ(p.get_pixel(1, 1), Color::red());
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, 0).a, 0);
}

AURORA_TEST_CASE(draw_image_scales_to_destination) {
    Image img;
    img.width = 1;
    img.height = 1;
    img.pixels = {255, 0, 0, 255};

    Painter p;
    p.begin(4, 4);
    p.draw_image(img, rect_at(0.0F, 0.0F, 4.0F, 4.0F));
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, 0), Color::red());
    AURORA_TEST_CHECK_EQ(p.get_pixel(3, 3), Color::red());
}

AURORA_TEST_CASE(linear_gradient_interpolates_along_axis) {
    Painter p;
    p.begin(8, 1);
    p.draw_linear_gradient(rect_at(0.0F, 0.0F, 8.0F, 1.0F), Point{.x = 0.0F, .y = 0.0F},
                           Point{.x = 8.0F, .y = 0.0F}, {Color::black(), Color::white()}, {0.0F, 1.0F});

    // 沿轴单调变亮：起点暗于终点。
    AURORA_TEST_CHECK_LT(p.get_pixel(0, 0).r, p.get_pixel(7, 0).r);
}

AURORA_TEST_CASE(draw_line_covers_diagonal) {
    Painter p;
    p.begin(6, 6);
    p.draw_line(Point{.x = 0.0F, .y = 0.0F}, Point{.x = 5.0F, .y = 5.0F}, 1.0F, Color::red());
    AURORA_TEST_CHECK_GT(count_reddish(p), 0);
}

AURORA_TEST_CASE(shift_pixels_moves_content_down) {
    Painter p;
    p.begin(4, 4);
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 1.0F), Color::red());
    p.shift_pixels(1.0F);

    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 1), Color::red());  // 内容下移一行
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 0).a, 0);           // 让出的条带归零
}

AURORA_TEST_CASE(to_image_exports_canvas) {
    Painter p;
    p.begin(4, 3);
    p.fill_rect(rect_at(0.0F, 0.0F, 4.0F, 3.0F), Color::blue());

    const Image exported = p.to_image();
    AURORA_TEST_CHECK_EQ(exported.width, 4);
    AURORA_TEST_CHECK_EQ(exported.height, 3);
    AURORA_TEST_CHECK_EQ(exported.pixels.size(), 4U * 3U * 4U);
}

AURORA_TEST_CASE(get_pixel_out_of_range_returns_transparent) {
    Painter p;
    p.begin(4, 4);
    // 越界一律返回透明色（a=0），而非不透明黑：调用方可据此区分「未绘制」与「绘制为黑」。
    AURORA_TEST_CHECK_EQ(p.get_pixel(-1, 0), (Color{0, 0, 0, 0}));
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, -1), (Color{0, 0, 0, 0}));
    AURORA_TEST_CHECK_EQ(p.get_pixel(4, 0), (Color{0, 0, 0, 0}));
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, 4), (Color{0, 0, 0, 0}));
}

}  // namespace aurora::test_cases::utest_painter
