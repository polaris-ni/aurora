/// 测试类型: unit
/// 目标单元: include/aurora/render/painter.h
/// 测试说明: 覆盖 Painter 的画布分配与零基底、fill_rect 覆写与 alpha 混合、clear_rect 复位、draw_rect 描边、
/// 裁剪栈（矩形/圆角/边界查询）、global_alpha 乘入、blend_pixel、draw_image 缩放、线性渐变方向、
/// draw_line 覆盖、shift_pixels 垂直搬移、to_image 导出与 get_pixel 越界兜底、draw_text_runs 与逐个
/// draw_text 的逐位一致及空批无操作、带 opts/aa_mode 的批量入口与逐个 draw_text(..., aa_mode, opts) 逐位一致、
/// 圆角填充的行级全覆写快路径与逐像素 coverage 在胶囊/普通圆角（含非整数锚点）上逐位一致

#include <cmath>
#include <span>
#include <string>

#include "aurora/core/image.h"
#include "aurora/render/font_engine.h"
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

/// @brief 某像素是否为红调（R 明显高于 G/B）。
[[nodiscard]] auto color_is_reddish(const Color &c) -> bool { return c.r > 128 && c.g < 64 && c.b < 64; }

/// @brief 某像素是否为蓝调（B 明显高于 R/G）。
[[nodiscard]] auto color_is_bluish(const Color &c) -> bool { return c.b > 128 && c.r < 64 && c.g < 64; }

/// @brief 两画布逐像素不同的格数（文本批量入口用它自证与逐个绘制等价）。
[[nodiscard]] auto count_diff_pixels(const Painter &a, const Painter &b) -> int {
    AURORA_TEST_REQUIRE_EQ(a.width(), b.width());
    AURORA_TEST_REQUIRE_EQ(a.height(), b.height());
    int diff = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            diff += a.get_pixel(x, y) != b.get_pixel(x, y) ? 1 : 0;
        }
    }
    return diff;
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

AURORA_TEST_CASE(text_respects_rect_clip_in_y) {
    // 回归守门：`blend_subpixel_span` 曾只按 X 裁剪、漏裁 Y —— 部分脏区帧的裁剪矩形不含
    // 文本所在行时，背景 fill_rect 被正确裁掉而文字仍逐行写出，同一字形每帧往上一帧墨迹上
    // 再混合，墨量累积致文字逐帧变粗发糙（「过一会儿开始锯齿」，约 1~2s 饱和）。
    auto ink_in_rows = [](const Painter &p, int y0, int y1) -> int {
        int n = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < p.width(); ++x) {
                if (p.get_pixel(x, y).a > 0) {
                    ++n;
                }
            }
        }
        return n;
    };

    // 基线：无裁剪时上半区与下半区都有墨。
    Painter base;
    base.begin(64, 24);
    base.draw_text(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 24.0F}}, "Aurora",
                   Font{.size_pt = 16.0F}, Color::black());
    AURORA_TEST_CHECK_GT(ink_in_rows(base, 0, 10), 0);

    // 裁剪只覆盖下半区（y>=12）：上半区的文字行必须完全不落笔。
    Painter clipped;
    clipped.begin(64, 24);
    clipped.push_clip(Rect{.origin = Point{.x = 0.0F, .y = 12.0F}, .size = Size{.width = 64.0F, .height = 12.0F}});
    clipped.draw_text(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 24.0F}},
                      "Aurora", Font{.size_pt = 16.0F}, Color::black());
    AURORA_TEST_CHECK_EQ(ink_in_rows(clipped, 0, 12), 0);  // 裁剪外（y<12）不得落笔
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

    AURORA_TEST_CHECK_EQ(p.get_pixel(4, 4), Color::red());  // 中心填满
    AURORA_TEST_CHECK_LT(p.get_pixel(0, 0).a, 255);  // 圆角外未填满
}

// 圆角填充的「行级全覆写快路径」必须与逐像素 coverage 逐位一致。胶囊形（radius 等于半高）
// 是行级快路径的边界形态：直边段高度为零，每一行都落在圆弧带里。旧实现把这一形态整行判成
// 「无全覆写区」而退回逐像素 sqrt（结果正确但慢一个数量级），本用例钉住「走出快路径后结果不变」。
AURORA_TEST_CASE(rounded_fill_row_fast_path_matches_per_pixel_coverage) {
    struct Shape {
        Rect rect{};
        float radius = 0.0F;
    };
    const Shape shapes[] = {
        {.rect = rect_at(7.0F, 7.0F, 40.0F, 40.0F), .radius = 20.0F},  // 正胶囊：radius == 半高
        {.rect = rect_at(6.5F, 9.25F, 37.0F, 21.0F), .radius = 10.5F},  // 胶囊 + 非整数锚点
        {.rect = rect_at(10.0F, 12.0F, 44.0F, 30.0F), .radius = 8.0F},  // 普通圆角：直边段非零
        {.rect = rect_at(8.5F, 8.5F, 51.0F, 47.0F), .radius = 6.0F},  // 普通圆角 + 非整数锚点
    };
    // 不透明源色：两路在「覆盖度恒为 1 的内部」必然写出逐值相同的 RGB，于是任何差异都只可能
    // 来自快路径把**覆盖度不足 1** 的像素误判进全覆写区（这正是本用例要钉的东西）。
    // 若改用半透明源色，内部快写走 sRGB 空间朴素 lerp、逐像素路径走线性光伽马混合，
    // 二者本就是两套口径（既有行为），会把覆盖度判据淹没在混色差里。
    const Color color{200, 90, 40, 255};
    const Rect canvas = rect_at(0.0F, 0.0F, 64.0F, 64.0F);
    for (const Shape &s : shapes) {
        // 不透明底：真实绘制总是叠在不透明背景上，边缘像素的 gamma 混合需要非零基底才可观测。
        Painter subject;
        subject.begin(64, 64);
        subject.fill_rect(canvas, Color::white());
        subject.fill_rounded_rect(s.rect, s.radius, color);

        // 独立基准：圆角裁剪下逐像素 blend_pixel（其实现即 set_pixel，走 coverage 的 SDF
        // 精确求解），不使用行级快路径，与 subject 的计算路径正交。
        Painter reference;
        reference.begin(64, 64);
        reference.fill_rect(canvas, Color::white());
        reference.push_clip_rounded(s.rect, s.radius);
        // 迭代域与 fill_rect 的取整口径一致：[floor(左), ceil(右)) × [floor(上), ceil(下))，
        // 否则会拿「边界列是否入选」这种 fill_rect 层面的口径差当覆盖度差。
        const int rx0 = static_cast<int>(std::floor(s.rect.origin.x));
        const int ry0 = static_cast<int>(std::floor(s.rect.origin.y));
        const int rx1 = static_cast<int>(std::ceil(s.rect.right()));
        const int ry1 = static_cast<int>(std::ceil(s.rect.bottom()));
        for (int y = ry0; y < ry1; ++y) {
            for (int x = rx0; x < rx1; ++x) {
                reference.blend_pixel(x, y, color);
            }
        }
        reference.pop_clip();

        AURORA_TEST_CHECK_EQ(count_diff_pixels(subject, reference), 0);
    }
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

AURORA_TEST_CASE(draw_image_one_to_one_blits_exactly) {
    // 1:1 且整数锚点时走整行直拷快路径：每个像素与源逐字节一致（不经采样/混合改写）。
    Image img;
    img.width = 2;
    img.height = 2;
    img.pixels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};

    Painter p;
    p.begin(4, 4);
    p.draw_image(img, rect_at(1.0F, 1.0F, 2.0F, 2.0F));
    AURORA_TEST_CHECK_EQ(p.get_pixel(1, 1), Color(255, 0, 0));
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 1), Color(0, 255, 0));
    AURORA_TEST_CHECK_EQ(p.get_pixel(1, 2), Color(0, 0, 255));
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 2), Color(255, 255, 0));
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, 0).a, 0);  // 目标矩形外不受影响
}

AURORA_TEST_CASE(draw_image_one_to_one_translucent_blends) {
    // 半透明 1:1 贴图不得走直拷（否则透明通道被硬写成不透明）：须与底色混合。
    Image img;
    img.width = 1;
    img.height = 1;
    img.pixels = {255, 0, 0, 128};

    Painter p;
    p.begin(1, 1);
    p.fill_rect(rect_at(0.0F, 0.0F, 1.0F, 1.0F), Color(0, 0, 255));
    p.draw_image(img, rect_at(0.0F, 0.0F, 1.0F, 1.0F));
    const Color c = p.get_pixel(0, 0);
    AURORA_TEST_CHECK_GT(static_cast<int>(c.r), 0);
    AURORA_TEST_CHECK_GT(static_cast<int>(c.b), 0);
}

AURORA_TEST_CASE(linear_gradient_interpolates_along_axis) {
    Painter p;
    p.begin(8, 1);
    p.draw_linear_gradient(rect_at(0.0F, 0.0F, 8.0F, 1.0F), Point{.x = 0.0F, .y = 0.0F}, Point{.x = 8.0F, .y = 0.0F},
                           {Color::black(), Color::white()}, {0.0F, 1.0F});

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
    AURORA_TEST_CHECK_EQ(p.get_pixel(2, 0).a, 0);  // 让出的条带归零
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
    AURORA_TEST_CHECK_EQ(p.get_pixel(-1, 0), Color{0, 0, 0, 0});
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, -1), Color{0, 0, 0, 0});
    AURORA_TEST_CHECK_EQ(p.get_pixel(4, 0), Color{0, 0, 0, 0});
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, 4), Color{0, 0, 0, 0});
}

AURORA_TEST_CASE(draw_text_runs_matches_individual_draw_text_calls) {
    // 批量化只省每次调用的字体面解析与行高度量，不得改变任何落笔坐标：整屏逐位比对。
    // 第三段的字重与前后不同，覆盖「换 Font 即重建发射上下文」那条分支。
    const Font regular{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    const Font bold{.family = "sans-serif", .size_pt = 14.0F, .weight = 700};
    const auto runs = {
        render::TextRun{
            .text = "alpha", .box = rect_at(4.0F, 6.0F, 60.0F, 20.0F), .font = regular, .color = Color::red()},
        render::TextRun{
            .text = "0-9 \xC3\xA9", .box = rect_at(70.0F, 6.0F, 60.0F, 20.0F), .font = regular, .color = Color::blue()},
        render::TextRun{
            .text = "beta", .box = rect_at(4.0F, 30.0F, 60.0F, 20.0F), .font = bold, .color = Color::green()}};

    Painter batch;
    batch.begin(140, 56);
    batch.draw_text_runs(std::span<const render::TextRun>{runs});

    Painter single;
    single.begin(140, 56);
    for (const auto &run : runs) {
        single.draw_text(run.box, std::string{run.text}, run.font, run.color);
    }
    AURORA_TEST_REQUIRE_EQ(count_diff_pixels(batch, single), 0);
}

AURORA_TEST_CASE(draw_text_runs_paints_each_run_with_its_own_color) {
    Painter p;
    p.begin(140, 24);
    const Font f{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    const auto runs = {
        render::TextRun{.text = "left", .box = rect_at(2.0F, 2.0F, 60.0F, 20.0F), .font = f, .color = Color::red()},
        render::TextRun{.text = "right", .box = rect_at(72.0F, 2.0F, 60.0F, 20.0F), .font = f, .color = Color::blue()}};
    p.draw_text_runs(std::span<const render::TextRun>{runs});

    // 两段各自着色：换色不要求重建绘制上下文，也不得让后一段覆盖前一段的色调。
    auto reddish = 0;
    auto bluish = 0;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < 64; ++x) {
            reddish += color_is_reddish(p.get_pixel(x, y)) ? 1 : 0;
        }
        for (int x = 68; x < p.width(); ++x) {
            bluish += color_is_bluish(p.get_pixel(x, y)) ? 1 : 0;
        }
    }
    AURORA_TEST_CHECK_GT(reddish, 0);
    AURORA_TEST_CHECK_GT(bluish, 0);
}

AURORA_TEST_CASE(empty_draw_text_runs_batch_is_a_no_op) {
    Painter p;
    p.begin(8, 8);
    p.draw_text_runs(std::span<const render::TextRun>{});
    AURORA_TEST_CHECK_EQ(p.get_pixel(4, 4), Color{0, 0, 0, 0});
}

AURORA_TEST_CASE(draw_text_runs_with_opts_matches_individual_draw_text_calls) {
    // 带 opts 的批量入口必须与逐个 `draw_text(..., opts)` 逐位一致：字距/词距/斜体整批同值。
    const Font f{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    const auto runs = {
        render::TextRun{.text = "spacing", .box = rect_at(4.0F, 6.0F, 130.0F, 20.0F), .font = f, .color = Color::red()},
        render::TextRun{
            .text = "wrap it", .box = rect_at(4.0F, 30.0F, 130.0F, 20.0F), .font = f, .color = Color::blue()}};
    const render::TextLayoutOpts opts{.letter_spacing = 1.5F, .word_spacing = 2.0F, .italic = true};

    Painter batch;
    batch.begin(160, 56);
    batch.draw_text_runs(std::span<const render::TextRun>{runs}, opts);

    Painter single;
    single.begin(160, 56);
    for (const auto &run : runs) {
        single.draw_text(run.box, std::string{run.text}, run.font, run.color, opts);
    }
    AURORA_TEST_REQUIRE_EQ(count_diff_pixels(batch, single), 0);
}

AURORA_TEST_CASE(draw_text_runs_with_aa_mode_and_opts_matches_individual_calls) {
    // 显式 aa_mode 的批量入口同样逐位等价于逐个 `draw_text(..., aa_mode, opts)`。
    const Font f{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    const auto runs = {
        render::TextRun{.text = "Aa", .box = rect_at(2.0F, 2.0F, 80.0F, 20.0F), .font = f, .color = Color::black()}};
    const render::TextLayoutOpts opts{.letter_spacing = 0.0F, .word_spacing = 0.0F, .italic = false};
    constexpr auto aa = render::TextAAMode::ClearType;

    Painter batch;
    batch.begin(96, 28);
    batch.draw_text_runs(std::span<const render::TextRun>{runs}, aa, opts);

    Painter single;
    single.begin(96, 28);
    for (const auto &run : runs) {
        single.draw_text(run.box, std::string{run.text}, run.font, run.color, aa, opts);
    }
    AURORA_TEST_REQUIRE_EQ(count_diff_pixels(batch, single), 0);
}

AURORA_TEST_CASE(draw_text_runs_italic_changes_pixels_but_keeps_layout) {
    // 斜体只改变字形倾角，不得改变排版盒/行高：非斜体批与斜体批在**整盒**上必有像素差，
    // 而段落盒高度（排版盒）由调用方给定、不受 italic 影响。
    const Font f{.family = "sans-serif", .size_pt = 16.0F, .weight = 400};
    const auto runs = {render::TextRun{
        .text = "Italic", .box = rect_at(2.0F, 2.0F, 120.0F, 24.0F), .font = f, .color = Color::black()}};

    Painter upright;
    upright.begin(128, 32);
    upright.draw_text_runs(std::span<const render::TextRun>{runs}, render::TextLayoutOpts{});

    Painter slanted;
    slanted.begin(128, 32);
    slanted.draw_text_runs(std::span<const render::TextRun>{runs},
                           render::TextLayoutOpts{.letter_spacing = 0.0F, .word_spacing = 0.0F, .italic = true});

    AURORA_TEST_CHECK_GT(count_diff_pixels(upright, slanted), 0);
}

AURORA_TEST_CASE(draw_text_runs_letter_spacing_matches_single_fragment) {
    // 正/负字距的批量结果须分别等于同字距的逐个绘制：收紧不得被批量路径夹掉。
    const Font f{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    const auto runs = {render::TextRun{
        .text = "tracking", .box = rect_at(2.0F, 2.0F, 160.0F, 20.0F), .font = f, .color = Color::black()}};

    for (const float ls : {-1.0F, 2.5F}) {
        const render::TextLayoutOpts opts{.letter_spacing = ls};
        Painter batch;
        batch.begin(168, 28);
        batch.draw_text_runs(std::span<const render::TextRun>{runs}, opts);

        Painter single;
        single.begin(168, 28);
        single.draw_text(runs.begin()->box, std::string{runs.begin()->text}, f, Color::black(), opts);

        AURORA_TEST_REQUIRE_EQ(count_diff_pixels(batch, single), 0);
    }
}

}  // namespace aurora::test_cases::utest_painter
