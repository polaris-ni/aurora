/// 测试类型: unit
/// 目标单元: include/aurora/render/snapshot_diff.h
/// 测试说明: 覆盖 compare_snapshots 的尺寸不一致短路、完全一致零差异、单像素差异计数与占比、
/// 容差阈值吸收微小差值、差异可视化图的红/淡化着色，以及空图（0×0）的除零保护；
/// 后段覆盖 cluster_diff_regions 的空间聚合（网格归并、连通跨格、碎块过滤、网格边长只改精度不改区域数）

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/render/snapshot_diff.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_snapshot_diff {

namespace {
/// @brief 构造 w×h 的 RGBA8 图，逐像素按回调填充。
[[nodiscard]] auto make_image(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
    -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * static_cast<std::size_t>(h); ++i) {
        img.pixels[(i * 4U) + 0U] = r;
        img.pixels[(i * 4U) + 1U] = g;
        img.pixels[(i * 4U) + 2U] = b;
        img.pixels[(i * 4U) + 3U] = a;
    }
    return img;
}

/// @brief 把第 (x, y) 个像素的 R 通道改为指定值，用于制造差异。
auto set_px(Image& img, int x, int y, std::uint8_t v) -> void {
    const std::size_t idx =
        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width)) + static_cast<std::size_t>(x)) * 4U;
    img.pixels[idx] = v;
}
}  // namespace

AURORA_TEST_CASE(size_mismatch_short_circuits) {
    const Image baseline = make_image(4, 4, 10, 20, 30);
    const Image current = make_image(4, 5, 10, 20, 30);

    const SnapshotDiff diff = compare_snapshots(baseline, current);
    AURORA_TEST_CHECK_TRUE(diff.size_mismatch);
    AURORA_TEST_CHECK_FALSE(diff.passed());
    // 短路后其余字段无意义：差异占比保持 0，不应被误读为「通过」。
    AURORA_TEST_CHECK_EQ(diff.pixel_diff_count, 0U);
    AURORA_TEST_CHECK_NEAR(diff.diff_ratio, 0.0, 1e-12);
}

AURORA_TEST_CASE(identical_images_have_no_diff) {
    const Image baseline = make_image(4, 4, 10, 20, 30);
    const Image& current = baseline;

    const SnapshotDiff diff = compare_snapshots(baseline, current);
    AURORA_TEST_CHECK_FALSE(diff.size_mismatch);
    AURORA_TEST_CHECK_EQ(diff.pixel_diff_count, 0U);
    AURORA_TEST_CHECK_EQ(diff.max_color_delta, 0);
    AURORA_TEST_CHECK_NEAR(diff.diff_ratio, 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(diff.passed());
}

AURORA_TEST_CASE(single_pixel_diff_counted_and_located) {
    Image baseline = make_image(4, 4, 0, 0, 0);
    Image current = baseline;
    current.pixels[0] = 255;  // 首个像素 R 通道 0 → 255

    const SnapshotDiff diff = compare_snapshots(baseline, current);
    AURORA_TEST_CHECK_EQ(diff.pixel_diff_count, 1U);
    AURORA_TEST_CHECK_EQ(diff.max_color_delta, 255);
    AURORA_TEST_CHECK_NEAR(diff.diff_ratio, 1.0 / 16.0, 1e-12);

    // 差异可视化：差异处标红且完全不透明。
    AURORA_TEST_CHECK_EQ(static_cast<int>(diff.diff_image.pixels[0]), 255);
    AURORA_TEST_CHECK_EQ(static_cast<int>(diff.diff_image.pixels[1]), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(diff.diff_image.pixels[2]), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(diff.diff_image.pixels[3]), 255);
}

AURORA_TEST_CASE(max_color_delta_is_largest_single_channel) {
    Image baseline = make_image(2, 2, 0, 0, 0);
    Image current = baseline;
    current.pixels[0] = 30;  // R 差 30
    current.pixels[4] = 200;  // 第二像素 R 差 200

    const SnapshotDiff diff = compare_snapshots(baseline, current);
    AURORA_TEST_CHECK_EQ(diff.max_color_delta, 200);
    AURORA_TEST_CHECK_EQ(diff.pixel_diff_count, 2U);
}

AURORA_TEST_CASE(tolerance_absorbs_small_deltas) {
    Image baseline = make_image(2, 2, 100, 100, 100);
    Image current = baseline;
    current.pixels[0] = 103;  // 差 3

    AURORA_TEST_CHECK_EQ(compare_snapshots(baseline, current, 0).pixel_diff_count, 1U);
    AURORA_TEST_CHECK_EQ(compare_snapshots(baseline, current, 3).pixel_diff_count, 0U);
    AURORA_TEST_CHECK_TRUE(compare_snapshots(baseline, current, 3).passed());
}

AURORA_TEST_CASE(match_pixels_are_dimmed_in_diff_image) {
    // 相同像素在差异图里淡化到 25%，保留轮廓便于人工核对。
    const Image baseline = make_image(1, 1, 200, 40, 80);
    const SnapshotDiff diff = compare_snapshots(baseline, baseline);

    AURORA_TEST_CHECK_EQ(static_cast<int>(diff.diff_image.pixels[0]), 50);  // 200 / 4
    AURORA_TEST_CHECK_EQ(static_cast<int>(diff.diff_image.pixels[1]), 10);  // 40 / 4
    AURORA_TEST_CHECK_EQ(static_cast<int>(diff.diff_image.pixels[2]), 20);  // 80 / 4
    AURORA_TEST_CHECK_EQ(static_cast<int>(diff.diff_image.pixels[3]), 255);
}

AURORA_TEST_CASE(passed_threshold_uses_ratio) {
    Image baseline = make_image(4, 4, 0, 0, 0);
    Image current = baseline;
    current.pixels[0] = 255;  // 1/16 差异

    const SnapshotDiff diff = compare_snapshots(baseline, current);
    AURORA_TEST_CHECK_FALSE(diff.passed(0.0));  // 严格模式：任一差异即失败
    AURORA_TEST_CHECK_FALSE(diff.passed(0.05));  // 1/16 = 0.0625 > 0.05
    AURORA_TEST_CHECK_TRUE(diff.passed(0.0625));  // 恰好等于阈值 → 通过（<= 语义）
    AURORA_TEST_CHECK_TRUE(diff.passed(0.5));
}

AURORA_TEST_CASE(empty_images_do_not_divide_by_zero) {
    const Image empty;
    const SnapshotDiff diff = compare_snapshots(empty, empty);
    AURORA_TEST_CHECK_FALSE(diff.size_mismatch);
    AURORA_TEST_CHECK_NEAR(diff.diff_ratio, 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(diff.passed());
}

// ───────────────────────── cluster_diff_regions ─────────────────────────

AURORA_TEST_CASE(cluster_size_mismatch_and_empty_images_return_no_regions) {
    // 尺寸不一致：短路，不产出任何区域（也不应尝试读像素）。
    AURORA_TEST_CHECK_TRUE(cluster_diff_regions(make_image(4, 4, 1, 2, 3), make_image(4, 5, 1, 2, 3)).empty());

    // 空图：宽或高非正直接返回空，避免后续除零。
    const Image empty;
    AURORA_TEST_CHECK_TRUE(cluster_diff_regions(empty, empty).empty());
}

AURORA_TEST_CASE(cluster_identical_images_have_no_regions) {
    Image baseline = make_image(16, 16, 7, 8, 9);
    const Image& current = baseline;
    AURORA_TEST_CHECK_TRUE(cluster_diff_regions(baseline, current).empty());
}

AURORA_TEST_CASE(cluster_single_pixel_yields_one_tile_sized_region) {
    // 16×16 / tile 8 → 2×2 网格；差异落在 (2,2) 属左上格 ⇒ 区域是整格 8×8，而非单点 1×1。
    // 这是刻意取舍：区域是「网格包络」，换来确定性与规整矩形（详见 snapshot_diff.h 的算法说明）。
    Image baseline = make_image(16, 16, 0, 0, 0);
    Image current = baseline;
    set_px(current, 2, 2, 255);

    const std::vector<DiffRegion> regions = cluster_diff_regions(baseline, current);
    AURORA_TEST_REQUIRE_EQ(regions.size(), 1U);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(regions[0].bounds.origin.x), 0.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(regions[0].bounds.origin.y), 0.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(regions[0].bounds.size.width), 8.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(regions[0].bounds.size.height), 8.0, 1e-6);
    AURORA_TEST_CHECK_EQ(regions[0].diff_pixels, 1U);
    AURORA_TEST_CHECK_EQ(regions[0].max_color_delta, 255);
    AURORA_TEST_CHECK_NEAR(regions[0].coverage, 1.0 / 64.0, 1e-9);
}

AURORA_TEST_CASE(cluster_merges_edge_adjacent_tiles_into_one_region) {
    // 32×32 / tile 8 → 4×4 网格；两点分别落在格 (0,0) 与 (1,0)，两者共边 ⇒ 四连通归并为一个区域，
    // 横向包络跨到 x=16。注意诊断是「相邻即可并一块」，而非像素级连通。
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    set_px(current, 0, 0, 255);
    set_px(current, 9, 1, 255);

    const std::vector<DiffRegion> regions = cluster_diff_regions(baseline, current);
    AURORA_TEST_REQUIRE_EQ(regions.size(), 1U);
    AURORA_TEST_CHECK_EQ(regions[0].diff_pixels, 2U);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(regions[0].bounds.size.width), 16.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(regions[0].bounds.size.height), 8.0, 1e-6);
}

AURORA_TEST_CASE(cluster_separated_blocks_are_distinct_and_sorted_by_size) {
    // 两块差异分别在格 (0,0) 与格 (3,3)，既非共边也非对角相邻 ⇒ 两个独立区域，且按差异像素**降序**输出。
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    for (int x = 0; x < 2; ++x) {
        set_px(current, x, 0, 255);  // 格 (0,0)：2 点
    }
    for (int x = 24; x < 29; ++x) {
        set_px(current, x, 24, 255);  // 格 (3,3)：5 点
    }

    const std::vector<DiffRegion> regions = cluster_diff_regions(baseline, current);
    AURORA_TEST_REQUIRE_EQ(regions.size(), 2U);
    AURORA_TEST_CHECK_EQ(regions[0].diff_pixels, 5U);  // 大的在前
    AURORA_TEST_CHECK_EQ(regions[1].diff_pixels, 2U);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(regions[0].bounds.origin.x), 24.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(regions[0].bounds.origin.y), 24.0, 1e-6);
}

AURORA_TEST_CASE(cluster_min_region_pixels_filters_small_regions) {
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    set_px(current, 0, 0, 255);  // 1 点
    for (int x = 24; x < 27; ++x) {  // 3 点
        set_px(current, x, 24, 255);
    }

    DiffRegionOptions keep_all;
    AURORA_TEST_CHECK_EQ(cluster_diff_regions(baseline, current, 0, keep_all).size(), 2U);

    DiffRegionOptions drop_specks;
    drop_specks.min_region_pixels = 2;
    const std::vector<DiffRegion> kept = cluster_diff_regions(baseline, current, 0, drop_specks);
    AURORA_TEST_REQUIRE_EQ(kept.size(), 1U);
    AURORA_TEST_CHECK_EQ(kept[0].diff_pixels, 3U);
}

AURORA_TEST_CASE(cluster_tile_size_only_changes_bbox_precision_not_region_count) {
    // 同一条竖直差异线：网格边长只影响矩形是否贴边，不影响「这是一块区域」的判断。
    Image baseline = make_image(16, 16, 0, 0, 0);
    Image current = baseline;
    for (int y = 2; y <= 6; ++y) {
        set_px(current, 5, y, 255);
    }

    DiffRegionOptions coarse;
    coarse.tile_size = 8;
    DiffRegionOptions fine;
    fine.tile_size = 1;

    const std::vector<DiffRegion> one_tile = cluster_diff_regions(baseline, current, 0, coarse);
    const std::vector<DiffRegion> per_pixel = cluster_diff_regions(baseline, current, 0, fine);

    AURORA_TEST_REQUIRE_EQ(one_tile.size(), 1U);
    AURORA_TEST_REQUIRE_EQ(per_pixel.size(), 1U);
    AURORA_TEST_CHECK_EQ(one_tile[0].diff_pixels, 5U);
    AURORA_TEST_CHECK_EQ(per_pixel[0].diff_pixels, 5U);

    // 粗网格给出整格包络；细网格给出紧贴像素的矩形 (5,2,1,5)。
    AURORA_TEST_CHECK_NEAR(static_cast<double>(one_tile[0].bounds.size.width), 8.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(per_pixel[0].bounds.origin.x), 5.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(per_pixel[0].bounds.origin.y), 2.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(per_pixel[0].bounds.size.width), 1.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(per_pixel[0].bounds.size.height), 5.0, 1e-6);
}

AURORA_TEST_CASE(cluster_reports_per_region_max_color_delta) {
    // 两块区域的色差不同 ⇒ 区域级 max_color_delta 必须分别记录，不能只留全局最大值。
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    for (int x = 24; x < 27; ++x) {
        set_px(current, x, 24, 50);  // 格 (3,3)：3 点，色差 50
    }
    set_px(current, 0, 0, 200);
    set_px(current, 1, 0, 200);  // 格 (0,0)：2 点，色差 200

    const std::vector<DiffRegion> regions = cluster_diff_regions(baseline, current);
    AURORA_TEST_REQUIRE_EQ(regions.size(), 2U);
    AURORA_TEST_CHECK_EQ(regions[0].max_color_delta, 50);
    AURORA_TEST_CHECK_EQ(regions[1].max_color_delta, 200);
}

AURORA_TEST_CASE(cluster_tolerance_is_shared_with_compare_snapshots) {
    // 容差语义必须与 compare_snapshots 一致：被容差吸收的差异不应产出区域。
    Image baseline = make_image(16, 16, 100, 100, 100);
    Image current = baseline;
    set_px(current, 2, 2, 103);  // 差 3

    AURORA_TEST_CHECK_EQ(cluster_diff_regions(baseline, current, 0).size(), 1U);
    AURORA_TEST_CHECK_TRUE(cluster_diff_regions(baseline, current, 3).empty());
}

// ─────────────────────── attribute_diff_regions ───────────────────────

namespace {
/// @brief 构造一棵三层「假布局盒表」，无需真实控件即可测归因规则。
[[nodiscard]] auto sample_boxes() -> std::vector<WidgetBox> {
    std::vector<WidgetBox> boxes;
    boxes.push_back(WidgetBox{
        .path = "",
        .type = "Column",
        .bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 40.0F, .height = 40.0F}}});
    boxes.push_back(WidgetBox{
        .path = "0",
        .type = "Text",
        .bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 16.0F, .height = 16.0F}}});
    boxes.push_back(WidgetBox{
        .path = "1",
        .type = "Button",
        .bounds = Rect{.origin = Point{.x = 0.0F, .y = 16.0F}, .size = Size{.width = 16.0F, .height = 16.0F}}});
    return boxes;
}
}  // namespace

AURORA_TEST_CASE(attribute_prefers_deeper_widget_on_equal_overlap) {
    // 区域落在左上角 16×16：Column（根）与 Text（第 0 个子）交叠面积都是 16×16 ⇒ 相等。
    // 此时应当判给更深的 Text —— 真正画出像素的是它，而不是「碰巧也覆盖了这块」的祖先容器。
    const std::vector<WidgetBox> boxes = sample_boxes();
    std::vector<DiffRegion> regions;
    DiffRegion r;
    r.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 16.0F, .height = 16.0F}};
    r.diff_pixels = 4;
    regions.push_back(r);

    const std::vector<RegionAttribution> got = attribute_diff_regions(regions, boxes);
    AURORA_TEST_REQUIRE_EQ(got.size(), 1U);
    AURORA_TEST_CHECK_TRUE(got[0].attributed());
    AURORA_TEST_CHECK_EQ(got[0].widget_path, std::string("0"));
    AURORA_TEST_CHECK_EQ(got[0].widget_type, std::string("Text"));
}

AURORA_TEST_CASE(attribute_prefers_largest_overlap_even_if_shallower) {
    // 区域 30×30：根 Column 交叠 900，子 Text 只交叠 256 ⇒ 深度不足以翻盘，取交叠更大者。
    std::vector<DiffRegion> regions;
    DiffRegion r;
    r.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 30.0F, .height = 30.0F}};
    regions.push_back(r);

    const std::vector<RegionAttribution> got = attribute_diff_regions(regions, sample_boxes());
    AURORA_TEST_REQUIRE_EQ(got.size(), 1U);
    AURORA_TEST_CHECK_EQ(got[0].widget_path, std::string(""));
    AURORA_TEST_CHECK_EQ(got[0].widget_type, std::string("Column"));
}

AURORA_TEST_CASE(attribute_root_widget_has_empty_path_but_counts_as_attributed) {
    // 陷阱回归：根控件的 `path` 按约定就是空串（可被 find_node / REST 解析）。
    // 用路径判空会把「已归因到根」误判成「未归因」，故判据必须看类型名。
    std::vector<WidgetBox> boxes;
    boxes.push_back(WidgetBox{
        .path = "",
        .type = "Column",
        .bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 40.0F, .height = 40.0F}}});
    std::vector<DiffRegion> regions;
    DiffRegion r;
    r.bounds = Rect{.origin = Point{.x = 4.0F, .y = 4.0F}, .size = Size{.width = 8.0F, .height = 8.0F}};
    regions.push_back(r);

    const std::vector<RegionAttribution> got = attribute_diff_regions(regions, boxes);
    AURORA_TEST_REQUIRE_EQ(got.size(), 1U);
    AURORA_TEST_CHECK_TRUE(got[0].attributed());
    AURORA_TEST_CHECK_TRUE(got[0].widget_path.empty());
    AURORA_TEST_CHECK_EQ(got[0].widget_type, std::string("Column"));
    // JSON 里此时必须是真字符串而非 null，否则下游读不到「是根控件画的」。
    const SnapshotDiffReport report = SnapshotDiffReport{.attributed = got};
    AURORA_TEST_CHECK_EQ(report.to_json()["attributed"][0]["widget_path"].get<std::string>(), std::string(""));
}

AURORA_TEST_CASE(attribute_unattributed_when_no_widget_covers_region) {
    std::vector<DiffRegion> regions;
    DiffRegion r;
    r.bounds = Rect{.origin = Point{.x = 100.0F, .y = 100.0F}, .size = Size{.width = 8.0F, .height = 8.0F}};
    regions.push_back(r);

    const std::vector<RegionAttribution> got = attribute_diff_regions(regions, sample_boxes());
    AURORA_TEST_REQUIRE_EQ(got.size(), 1U);
    AURORA_TEST_CHECK_FALSE(got[0].attributed());
    AURORA_TEST_CHECK_TRUE(got[0].widget_path.empty());
    AURORA_TEST_CHECK_TRUE(got[0].widget_type.empty());
}

AURORA_TEST_CASE(attribute_with_empty_box_list_is_unattributed) {
    std::vector<DiffRegion> regions;
    DiffRegion r;
    r.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 4.0F, .height = 4.0F}};
    regions.push_back(r);

    const std::vector<RegionAttribution> got = attribute_diff_regions(regions, std::vector<WidgetBox>{});
    AURORA_TEST_REQUIRE_EQ(got.size(), 1U);
    AURORA_TEST_CHECK_FALSE(got[0].attributed());
}

AURORA_TEST_CASE(attribute_area_ratio_and_partial_overlap_flag) {
    // 单一 Text（16×16 位于原点），区域 (8,8,16,16) 只有左上角 8×8 落在它里面 ⇒
    // 命中 Text、覆盖比 64/256，且区域明显溢出到控件之外 ⇒ partial_overlap 为真。
    std::vector<WidgetBox> boxes;
    boxes.push_back(WidgetBox{
        .path = "0",
        .type = "Text",
        .bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 16.0F, .height = 16.0F}}});
    std::vector<DiffRegion> regions;
    DiffRegion r;
    r.bounds = Rect{.origin = Point{.x = 8.0F, .y = 8.0F}, .size = Size{.width = 16.0F, .height = 16.0F}};
    regions.push_back(r);

    const std::vector<RegionAttribution> got = attribute_diff_regions(regions, boxes);
    AURORA_TEST_REQUIRE_EQ(got.size(), 1U);
    AURORA_TEST_CHECK_EQ(got[0].widget_type, std::string("Text"));
    AURORA_TEST_CHECK_TRUE(got[0].partial_overlap);
    AURORA_TEST_CHECK_NEAR(got[0].widget_area_ratio, 64.0 / 256.0, 1e-9);

    // 反向对照：区域完全落在控件内时不应打溢出标记。
    std::vector<DiffRegion> contained;
    DiffRegion c;
    c.bounds = Rect{.origin = Point{.x = 2.0F, .y = 2.0F}, .size = Size{.width = 4.0F, .height = 4.0F}};
    contained.push_back(c);
    AURORA_TEST_CHECK_FALSE(attribute_diff_regions(contained, boxes)[0].partial_overlap);
}

AURORA_TEST_CASE(attribute_converts_widget_bounds_by_device_scale) {
    // Node 的 bounds 是 dp，区域是像素：同一块文本在 2x 渲染下覆盖两倍像素。
    std::vector<WidgetBox> boxes;
    boxes.push_back(
        WidgetBox{.path = "0",
                  .type = "Text",
                  .bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 8.0F, .height = 8.0F}}});
    std::vector<DiffRegion> regions;
    DiffRegion r;
    r.bounds = Rect{.origin = Point{.x = 12.0F, .y = 12.0F}, .size = Size{.width = 4.0F, .height = 4.0F}};
    regions.push_back(r);

    // 1x 下该区域在控件之外 ⇒ 归因失败；2x 下控件覆盖到 16px ⇒ 归因成功。
    AURORA_TEST_CHECK_FALSE(attribute_diff_regions(regions, boxes, 1.0F)[0].attributed());
    AURORA_TEST_CHECK_TRUE(attribute_diff_regions(regions, boxes, 2.0F)[0].attributed());
}

// ─────────────────────── SnapshotDiffReport ───────────────────────

AURORA_TEST_CASE(report_size_mismatch_short_circuits_without_regions) {
    const SnapshotDiffReport report = build_snapshot_diff_report(make_image(4, 4, 1, 2, 3), make_image(4, 5, 1, 2, 3));
    AURORA_TEST_CHECK_TRUE(report.raw.size_mismatch);
    AURORA_TEST_CHECK_TRUE(report.regions.empty());
    AURORA_TEST_CHECK_TRUE(report.attributed.empty());
    AURORA_TEST_CHECK_FALSE(report.passed());
}

AURORA_TEST_CASE(report_without_boxes_still_reports_regions_as_unattributed) {
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    set_px(current, 0, 0, 255);

    const SnapshotDiffReport report = build_snapshot_diff_report(baseline, current);
    AURORA_TEST_REQUIRE_EQ(report.regions.size(), 1U);
    AURORA_TEST_REQUIRE_EQ(report.attributed.size(), 1U);
    AURORA_TEST_CHECK_FALSE(report.attributed[0].attributed());
    AURORA_TEST_CHECK_NEAR(report.attributed_ratio, 0.0, 1e-12);

    // JSON 里未归因用 null（而非空串），避免下游把「背景上的差异」误读成根控件。
    const Json j = report.to_json();
    AURORA_TEST_CHECK_TRUE(j["attributed"][0]["widget_path"].is_null());
    AURORA_TEST_CHECK_TRUE(j["attributed"][0]["widget_type"].is_null());
}

AURORA_TEST_CASE(report_json_carries_global_and_per_region_facts) {
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    set_px(current, 0, 0, 255);
    set_px(current, 24, 24, 255);

    const SnapshotDiffReport report = build_snapshot_diff_report(baseline, current, sample_boxes());
    const Json j = report.to_json();
    AURORA_TEST_CHECK_FALSE(j["size_mismatch"].get<bool>());
    AURORA_TEST_CHECK_EQ(j["pixel_diff_count"].get<std::size_t>(), 2U);
    AURORA_TEST_CHECK_EQ(j["max_color_delta"].get<int>(), 255);
    AURORA_TEST_CHECK_EQ(j["regions"].size(), 2U);
    AURORA_TEST_CHECK_EQ(j["attributed"].size(), 2U);
    // 逐条区域必须有几何，否则 AI 无从定位。
    AURORA_TEST_CHECK_TRUE(j["regions"][0].contains("x"));
    AURORA_TEST_CHECK_TRUE(j["attributed"][0].contains("region"));
    AURORA_TEST_CHECK_GT(report.attributed_ratio, 0.0);
}

AURORA_TEST_CASE(report_text_mentions_widget_path_and_type) {
    // to_text 的价值就在这一句：AI 拿到文本就能知道该去改哪个控件，不必自己遍历 accumulated JSON。
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    set_px(current, 0, 0, 255);

    const SnapshotDiffReport report = build_snapshot_diff_report(baseline, current, sample_boxes());
    const std::string text = report.to_text();
    AURORA_TEST_CHECK_NE(text.find("snapshot diff"), std::string::npos);
    AURORA_TEST_CHECK_NE(text.find("Text"), std::string::npos);  // 归因到第 0 个子控件
    AURORA_TEST_CHECK_NE(text.find("max_delta"), std::string::npos);
}

AURORA_TEST_CASE(report_text_truncates_regions_and_says_so) {
    Image baseline = make_image(64, 64, 0, 0, 0);
    Image current = baseline;
    // 制造多块互不相连的差异（每块落在独立网格，彼此不四连通）。
    set_px(current, 2, 2, 255);
    set_px(current, 20, 2, 255);
    set_px(current, 2, 20, 255);
    set_px(current, 40, 40, 255);

    const SnapshotDiffReport report = build_snapshot_diff_report(baseline, current, sample_boxes());
    AURORA_TEST_REQUIRE_GE(report.regions.size(), 2U);
    const std::string truncated = report.to_text(1);
    AURORA_TEST_CHECK_NE(truncated.find("[1]"), std::string::npos);
    AURORA_TEST_CHECK_NE(truncated.find("more region(s) omitted"), std::string::npos);
    // 不截断时不该出现折叠行。
    AURORA_TEST_CHECK_EQ(report.to_text(report.regions.size()).find("omitted"), std::string::npos);
}

AURORA_TEST_CASE(report_text_marks_unattributed_regions) {
    // 只给一个位于原点的小控件，把差异放在右下角 —— 那里没有任何控件覆盖，
    // 报告必须诚实地标记为未归因，而不是硬凑一个「看起来最接近」的控件。
    std::vector<WidgetBox> boxes;
    boxes.push_back(
        WidgetBox{.path = "0",
                  .type = "Text",
                  .bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 8.0F, .height = 8.0F}}});
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    set_px(current, 30, 30, 255);

    const SnapshotDiffReport report = build_snapshot_diff_report(baseline, current, boxes);
    AURORA_TEST_REQUIRE_EQ(report.attributed.size(), 1U);
    AURORA_TEST_CHECK_FALSE(report.attributed[0].attributed());
    AURORA_TEST_CHECK_NE(report.to_text().find("<unattributed>"), std::string::npos);
}

AURORA_TEST_CASE(attribute_output_parallels_input_regions) {
    // 输出与输入等长同序 —— 调用方依赖按索引回配到区域。
    Image baseline = make_image(32, 32, 0, 0, 0);
    Image current = baseline;
    set_px(current, 0, 0, 255);
    set_px(current, 24, 24, 255);

    const std::vector<DiffRegion> regions = cluster_diff_regions(baseline, current);
    const std::vector<RegionAttribution> got = attribute_diff_regions(regions, sample_boxes());
    AURORA_TEST_REQUIRE_EQ(got.size(), regions.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        AURORA_TEST_CHECK_EQ(got[i].region.diff_pixels, regions[i].diff_pixels);
    }
}

}  // namespace aurora::test_cases::utest_snapshot_diff
