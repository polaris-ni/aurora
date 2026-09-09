/// 测试类型: unit
/// 目标单元: include/aurora/render/snapshot_diff.h
/// 测试说明: 覆盖 compare_snapshots 的尺寸不一致短路、完全一致零差异、单像素差异计数与占比、
/// 容差阈值吸收微小差值、差异可视化图的红/淡化着色，以及空图（0×0）的除零保护

#include <cstdint>
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

}  // namespace aurora::test_cases::utest_snapshot_diff
