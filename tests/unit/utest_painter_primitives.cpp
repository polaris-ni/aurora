/// 测试类型: unit
/// 目标单元: include/aurora/render/painter.h
/// 测试说明: 覆盖图表控件族切片 1 的三个新增矢量原语——stroke_polyline（真 SDF，圆角连接 + 圆帽，
/// 半透明下顶点不得二次合成）、fill_sector（扇形 / 环扇，内外半径与角向四条边 AA）、stroke_arc
/// （环带语义糖）；并验证录制 / 回放（DisplayList）一致性、裁剪与 global_alpha 生效、退化入参
/// 无操作，以及两张原语级像素 golden 基线（painter_polyline.png / painter_sector.png，
/// 受 AURORA_GOLDEN_DIR / MAX_DIFF / MAX_PIXELS / UPDATE_GOLDEN 控制）

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"
#include "aurora/render/png.h"
#include "aurora/render/snapshot_diff.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"

namespace aurora::test_cases::utest_painter_primitives {

namespace golden = aurora::testing::golden;

namespace {

constexpr int AURORA_CANVAS = 40;
constexpr int AURORA_GOLDEN_SIZE = 64;
constexpr float AURORA_HALF_PI = 1.57079632679489661923F;
constexpr float AURORA_TWO_PI = 6.28318530717958647692F;
/// 图例联动把非高亮系列降到的透明度（0.35）对应的 alpha 通道值。
constexpr int AURORA_DIM_ALPHA = 89;

[[nodiscard]] auto alpha_at(const Painter &p, int x, int y) -> int { return static_cast<int>(p.get_pixel(x, y).a); }

/// @brief 取 R 通道：帧缓冲按不透明合成（set_pixel 恒写 alpha=255），故墨量差异看 RGB。
[[nodiscard]] auto red_at(const Painter &p, int x, int y) -> int { return static_cast<int>(p.get_pixel(x, y).r); }

[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 直角折线：(5,5) → (30,5) → (30,30)，线宽 4（半宽 2）。
[[nodiscard]] auto elbow() -> std::vector<Point> {
    return std::vector<Point>{Point{.x = 5.0F, .y = 5.0F}, Point{.x = 30.0F, .y = 5.0F}, Point{.x = 30.0F, .y = 30.0F}};
}

[[nodiscard]] auto count_drawn(const Painter &p) -> int {
    int count = 0;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            if (p.get_pixel(x, y).a != 0) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

AURORA_TEST_CASE(polyline_covers_segments_and_join) {
    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    p.stroke_polyline(elbow(), 4.0F, Color::red());

    AURORA_TEST_CHECK_EQ(alpha_at(p, 15, 5), 255);  // 水平段中部
    AURORA_TEST_CHECK_EQ(alpha_at(p, 30, 15), 255);  // 垂直段中部
    AURORA_TEST_CHECK_EQ(alpha_at(p, 30, 5), 255);  // 顶点（join 无缺口）
    AURORA_TEST_CHECK_EQ(alpha_at(p, 5, 30), 0);  // 折线外
    AURORA_TEST_CHECK_EQ(alpha_at(p, 10, 15), 0);  // 距水平段 10dp > 半宽 + 羽化
    AURORA_TEST_CHECK_GT(count_drawn(p), 0);
}

AURORA_TEST_CASE(polyline_join_has_no_double_composite) {
    // D13：图例联动把系列降到 alpha 0.35。真 SDF 只按几何算一次覆盖度，故顶点与段中部墨量一致；
    // 若退化成「逐段 draw_line + 顶点圆盘」，顶点会二次源覆盖合成而明显更浓（串珠）。
    // 注意：帧缓冲按不透明合成（set_pixel 恒写 alpha=255），覆盖度体现在 RGB 上，故比 R 通道。
    const Color dim{255, 0, 0, static_cast<std::uint8_t>(AURORA_DIM_ALPHA)};

    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    p.stroke_polyline(elbow(), 4.0F, dim);
    const int at_join = red_at(p, 30, 5);
    const int on_segment = red_at(p, 15, 5);
    AURORA_TEST_CHECK_EQ(at_join, on_segment);
    AURORA_TEST_CHECK_EQ(red_at(p, 30, 15), on_segment);

    // 自检：二次合成在本帧缓冲上确实可被检出（否则上面的相等断言是空转）。
    Painter twice;
    twice.begin(AURORA_CANVAS, AURORA_CANVAS);
    twice.stroke_polyline(elbow(), 4.0F, dim);
    twice.stroke_polyline(elbow(), 4.0F, dim);
    AURORA_TEST_CHECK_GT(red_at(twice, 15, 5), on_segment);
    AURORA_TEST_CHECK_LT(on_segment, 255);  // 0.35 覆盖度必弱于不透明
}

AURORA_TEST_CASE(polyline_applies_global_alpha) {
    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    p.set_alpha(0.5);
    p.stroke_polyline(elbow(), 4.0F, Color::red());

    const int r = red_at(p, 15, 5);
    AURORA_TEST_CHECK_GT(r, 0);
    AURORA_TEST_CHECK_LT(r, 255);
}

AURORA_TEST_CASE(polyline_respects_clip) {
    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    p.push_clip(rect_at(0.0F, 0.0F, 10.0F, 40.0F));
    p.stroke_polyline(elbow(), 4.0F, Color::red());

    AURORA_TEST_CHECK_EQ(alpha_at(p, 5, 5), 255);  // 裁剪内
    AURORA_TEST_CHECK_EQ(alpha_at(p, 30, 5), 0);  // 裁剪外
    AURORA_TEST_CHECK_EQ(alpha_at(p, 30, 15), 0);
}

AURORA_TEST_CASE(polyline_records_and_replays_identically) {
    Painter direct;
    direct.begin(AURORA_CANVAS, AURORA_CANVAS);
    direct.stroke_polyline(elbow(), 4.0F, Color::red());
    direct.fill_sector(Point{.x = 20.0F, .y = 20.0F}, 12.0F, 5.0F, -AURORA_HALF_PI, 0.0F, Color::blue());

    DisplayList dl;
    Painter recorded;
    recorded.begin(AURORA_CANVAS, AURORA_CANVAS);
    recorded.record(dl);
    recorded.stroke_polyline(elbow(), 4.0F, Color::red());
    recorded.fill_sector(Point{.x = 20.0F, .y = 20.0F}, 12.0F, 5.0F, -AURORA_HALF_PI, 0.0F, Color::blue());
    recorded.stop();
    AURORA_TEST_CHECK_GT(static_cast<int>(dl.cmd_count()), 0);

    Painter replayed;
    replayed.begin(AURORA_CANVAS, AURORA_CANVAS);
    dl.replay(replayed);

    int diff = 0;
    for (int y = 0; y < AURORA_CANVAS; ++y) {
        for (int x = 0; x < AURORA_CANVAS; ++x) {
            if (!(direct.get_pixel(x, y) == replayed.get_pixel(x, y))) {
                ++diff;
            }
        }
    }
    AURORA_TEST_CHECK_EQ(diff, 0);
}

AURORA_TEST_CASE(sector_fills_wedge_only) {
    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    // 12 点钟 → 3 点钟（y 轴向下）：右上四分之一圆。
    p.fill_sector(Point{.x = 20.0F, .y = 20.0F}, 15.0F, 0.0F, -AURORA_HALF_PI, 0.0F, Color::red());

    AURORA_TEST_CHECK_EQ(alpha_at(p, 26, 14), 255);  // 区内（r≈8.5，角 -45°）
    AURORA_TEST_CHECK_EQ(alpha_at(p, 14, 14), 0);  // 角向区外（左上）
    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 2), 0);  // 半径外（上方 r=18）
    AURORA_TEST_CHECK_EQ(alpha_at(p, 26, 26), 0);  // 角向区外（右下）
}

AURORA_TEST_CASE(sector_inner_radius_forms_ring) {
    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    p.fill_sector(Point{.x = 20.0F, .y = 20.0F}, 15.0F, 8.0F, -AURORA_HALF_PI, 0.0F, Color::red());

    AURORA_TEST_CHECK_EQ(alpha_at(p, 26, 14), 255);  // 环带内
    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 16), 0);  // 内孔（r=4）
    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 20), 0);  // 圆心
    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 2), 0);  // 外半径之外
}

AURORA_TEST_CASE(sector_full_sweep_covers_all_quadrants) {
    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    p.fill_sector(Point{.x = 20.0F, .y = 20.0F}, 15.0F, 0.0F, -AURORA_HALF_PI, -AURORA_HALF_PI + AURORA_TWO_PI,
                  Color::red());

    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 8), 255);  // 上
    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 32), 255);  // 下
    AURORA_TEST_CHECK_EQ(alpha_at(p, 8, 20), 255);  // 左
    AURORA_TEST_CHECK_EQ(alpha_at(p, 32, 20), 255);  // 右
    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 2), 0);  // 外
}

AURORA_TEST_CASE(stroke_arc_spans_thickness_band) {
    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    // 半径 10、厚度 4 → 环带 [8, 12]，角度 12 点 → 3 点。
    p.stroke_arc(Point{.x = 20.0F, .y = 20.0F}, 10.0F, 4.0F, -AURORA_HALF_PI, 0.0F, Color::red());

    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 10), 255);  // r=10，带内
    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 14), 0);  // r=6，内孔
    AURORA_TEST_CHECK_EQ(alpha_at(p, 20, 5), 0);  // r=15，带外
    AURORA_TEST_CHECK_EQ(alpha_at(p, 26, 14), 255);  // r≈8.5，带内且角向内
    AURORA_TEST_CHECK_EQ(alpha_at(p, 14, 14), 0);  // 角向区外
}

AURORA_TEST_CASE(primitives_ignore_degenerate_inputs) {
    Painter p;
    p.begin(AURORA_CANVAS, AURORA_CANVAS);
    const Point center{.x = 20.0F, .y = 20.0F};
    p.stroke_polyline(std::vector<Point>{Point{.x = 5.0F, .y = 5.0F}}, 4.0F, Color::red());  // 单点
    p.stroke_polyline(elbow(), 0.0F, Color::red());  // 零宽
    p.stroke_polyline(elbow(), 4.0F, Color{255, 0, 0, 0});  // 全透明
    p.fill_sector(center, 0.0F, 0.0F, 0.0F, AURORA_HALF_PI, Color::red());  // 零外径
    p.fill_sector(center, 10.0F, 10.0F, 0.0F, AURORA_HALF_PI, Color::red());  // 内径 ≥ 外径
    p.fill_sector(center, 10.0F, 0.0F, AURORA_HALF_PI, 0.0F, Color::red());  // 角差 ≤ 0
    p.stroke_arc(center, 10.0F, 0.0F, 0.0F, AURORA_HALF_PI, Color::red());  // 零厚度

    AURORA_TEST_CHECK_EQ(count_drawn(p), 0);
}

AURORA_TEST_CASE(golden_polyline_matches_baseline) {
    Painter p;
    p.begin(AURORA_GOLDEN_SIZE, AURORA_GOLDEN_SIZE);
    p.fill_rect(rect_at(0.0F, 0.0F, static_cast<float>(AURORA_GOLDEN_SIZE), static_cast<float>(AURORA_GOLDEN_SIZE)),
                Color::white());
    // 直角折线（join）+ 三点折线（细）+ 半透明折线（D13：顶点不得串珠）
    p.stroke_polyline(
        std::vector<Point>{Point{.x = 6.0F, .y = 8.0F}, Point{.x = 30.0F, .y = 8.0F}, Point{.x = 30.0F, .y = 28.0F}},
        4.0F, Color::red());
    p.stroke_polyline(std::vector<Point>{Point{.x = 6.0F, .y = 40.0F}, Point{.x = 18.0F, .y = 52.0F},
                                         Point{.x = 30.0F, .y = 40.0F}, Point{.x = 42.0F, .y = 52.0F}},
                      1.5F, Color::blue());
    p.stroke_polyline(
        std::vector<Point>{Point{.x = 40.0F, .y = 8.0F}, Point{.x = 56.0F, .y = 8.0F}, Point{.x = 56.0F, .y = 24.0F}},
        4.0F, Color{255, 0, 0, static_cast<std::uint8_t>(AURORA_DIM_ALPHA)});
    golden::compare_or_update_painter("painter_polyline", p);
}

AURORA_TEST_CASE(golden_sector_matches_baseline) {
    Painter p;
    p.begin(AURORA_GOLDEN_SIZE, AURORA_GOLDEN_SIZE);
    p.fill_rect(rect_at(0.0F, 0.0F, static_cast<float>(AURORA_GOLDEN_SIZE), static_cast<float>(AURORA_GOLDEN_SIZE)),
                Color::white());
    // 实心扇形（12 点起，90°）+ 环扇（donut）+ 整圆 + 弧线描边
    p.fill_sector(Point{.x = 16.0F, .y = 16.0F}, 14.0F, 0.0F, -AURORA_HALF_PI, 0.0F, Color::red());
    p.fill_sector(Point{.x = 46.0F, .y = 16.0F}, 14.0F, 7.0F, -AURORA_HALF_PI, AURORA_HALF_PI, Color::blue());
    p.fill_sector(Point{.x = 16.0F, .y = 46.0F}, 12.0F, 0.0F, 0.0F, AURORA_TWO_PI, Color::green());
    p.stroke_arc(Point{.x = 46.0F, .y = 46.0F}, 10.0F, 3.0F, -AURORA_HALF_PI, AURORA_HALF_PI, Color{0, 0, 0, 255});
    golden::compare_or_update_painter("painter_sector", p);
}

}  // namespace aurora::test_cases::utest_painter_primitives
