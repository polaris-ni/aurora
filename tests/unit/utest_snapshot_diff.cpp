/// 测试类型: unit
/// 目标单元: include/aurora/render/snapshot_diff.h
/// 测试说明: compare_snapshots 快照像素对比单元测试

#include <cmath>
#include <cstdint>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/render/snapshot_diff.h"
#include "aurora_test_harness.h"

using au::compare_snapshots;
using au::Image;

namespace aurora::test_cases::utest_snapshot_diff {

namespace {

auto make_image(int w, int h, std::uint8_t v) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h * 4, v);
    return img;
}

}  // namespace

AURORA_TEST() {
    // ---- 1. 相同图零差异 ----
    {
        const Image a = make_image(10, 10, 128);
        const Image b = make_image(10, 10, 128);
        const auto diff = compare_snapshots(a, b);
        AURORA_TEST_CHECK(!diff.size_mismatch);
        AURORA_TEST_CHECK(diff.pixel_diff_count == 0);
        AURORA_TEST_CHECK(diff.max_color_delta == 0);
        AURORA_TEST_CHECK(diff.passed());
    }

    // ---- 2. 部分差异 + 容差 ----
    {
        Image a = make_image(10, 10, 100);
        Image b = make_image(10, 10, 100);
        // 改 5 个像素
        for (int i = 0; i < 5; ++i) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            b.pixels[static_cast<std::size_t>(i) * 4] = 200;  // R 通道 +100
        }
        const auto strict = compare_snapshots(a, b, 0);
        AURORA_TEST_CHECK(strict.pixel_diff_count == 5);
        AURORA_TEST_CHECK(strict.max_color_delta == 100);
        AURORA_TEST_CHECK(std::abs(strict.diff_ratio - 0.05) < 1e-9);
        AURORA_TEST_CHECK(!strict.passed());
        AURORA_TEST_CHECK(strict.passed(0.10));  // 10% 阈值内通过

        // 容差 100：全部视为相同
        const auto tolerant = compare_snapshots(a, b, 100);
        AURORA_TEST_CHECK(tolerant.pixel_diff_count == 0);
        AURORA_TEST_CHECK(tolerant.passed());
    }

    // ---- 3. 尺寸不一致 + 差异图可视化 ----
    {
        const Image a = make_image(10, 10, 0);
        const Image c = make_image(8, 8, 0);
        AURORA_TEST_CHECK(compare_snapshots(a, c).size_mismatch);
        AURORA_TEST_CHECK(!compare_snapshots(a, c).passed());

        Image b = make_image(10, 10, 0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        b.pixels[0] = 255;  // 第一个像素差异
        const auto diff = compare_snapshots(a, b);
        // 差异图：像素 0 红色
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(diff.diff_image.pixels[0] == 255);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(diff.diff_image.pixels[1] == 0);
        // 其余淡化
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(diff.diff_image.pixels[7] == 255);  // alpha
    }
}

}  // namespace aurora::test_cases::utest_snapshot_diff
