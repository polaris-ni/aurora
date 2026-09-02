// png_image_test.cpp — 覆盖 PNG 编解码往返与无效输入处理（render::write_png / core::Image::load）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// image/image_codec.h(编解码器注册表 IImageCodec；各格式编解码测试共同行使注册路由)。

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/image.h"
#include "aurora/render/png.h"

#include "test_harness.h"

using aurora::Image;
using aurora::write_png;

static void test_png_round_trip() {
    constexpr int w = 4;
    constexpr int h = 4;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    for (int i = 0; i < w * h; ++i) {
        px[(static_cast<size_t>(i) * 4) + 0] = 255; // 红
        px[(static_cast<size_t>(i) * 4) + 3] = 255; // 不透明
    }
    auto wr = write_png("png_test.png", w, h, px.data());
    AURORA_TEST_CHECK_MSG(wr.ok(), "write_png: encodes 4x4 RGBA");

    auto lr = Image::load("png_test.png");
    AURORA_TEST_CHECK_MSG(lr.ok(), "Image::load: decodes written PNG");
    const Image &img = lr.value();
    AURORA_TEST_CHECK_MSG(img.width == w && img.height == h, "Image::load: dimensions preserved");
    AURORA_TEST_CHECK_MSG(img.pixels[0] == 255 && img.pixels[3] == 255, "Image::load: pixel values preserved");
}

static void test_invalid_dims() {
    const auto bad = write_png("png_bad.png", 0, 0, nullptr);
    AURORA_TEST_CHECK_MSG(!bad.ok(), "write_png: zero dimensions returns error");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== png_image_test ===\n");
    test_png_round_trip();
    test_invalid_dims();
}
