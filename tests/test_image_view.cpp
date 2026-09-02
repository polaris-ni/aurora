// test_image_view.cpp — ImageView 控件 1:1 测试：图像尺寸 / 约束裁剪 / 固定宽高 /
// 0x0 回退 / from_file 失败安全 / source 序列化 / 真实 PNG 解码。
// ── API 覆盖映射 ─────────────────────────────
// core/image.h(Image 载入/像素缓冲，经 ImageView 解码用例行使)。

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::BuildContext;
using aurora::Constraints;
using aurora::Image;
using aurora::ImageView;
using aurora::ImageViewProps;
using aurora::Length;
using aurora::Size;

// 在多个候选相对路径中尝试加载 golden PNG（兼容 ctest build 目录与仓库根 CWD）。
static auto try_load_golden() -> bool {
    const char *candidates[] = {
        "tests/golden/golden_basic_column.png",
        "../tests/golden/golden_basic_column.png",
        "../../tests/golden/golden_basic_column.png",
    };
    return std::ranges::any_of(candidates, [](const char *p) -> bool { return Image::load(p).ok(); });
}

static void test_image_view() {
    // 合成 32x24 图像（全不透明像素，长度 = w*h*4）。
    Image img;
    img.width = 32;
    img.height = 24;
    img.pixels = std::vector<uint8_t>(static_cast<size_t>(img.width) * img.height * 4, 255);

    ImageView iv{ img };
    BuildContext ctx;
    iv.mount(ctx);
    Constraints c{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 100, .height = 100 } };
    iv.layout(c, ctx);
    const Size s = iv.size();

    AURORA_TEST_CHECK(near_f(s.width, 32.0f));
    AURORA_TEST_CHECK(near_f(s.height, 24.0f));
    AURORA_TEST_CHECK(std::string(iv.type_name()) == "Image");
    AURORA_TEST_CHECK(iv.bitmap.width == 32 && iv.bitmap.height == 24);

    // 固定宽度覆盖：高度仍按内容自然尺寸。
    ImageView iv_w{ img };
    BuildContext ctx_w;
    iv_w.mount(ctx_w);
    iv_w.width(Length::fixed(80));
    iv_w.layout(c, ctx_w);
    AURORA_TEST_CHECK(near_f(iv_w.size().width, 80.0f));
    AURORA_TEST_CHECK(near_f(iv_w.size().height, 24.0f));

    // 固定高度覆盖：宽度仍按内容自然尺寸。
    ImageView iv_h{ img };
    BuildContext ctx_h;
    iv_h.mount(ctx_h);
    iv_h.height(Length::fixed(60));
    iv_h.layout(c, ctx_h);
    AURORA_TEST_CHECK(near_f(iv_h.size().height, 60.0f));
    AURORA_TEST_CHECK(near_f(iv_h.size().width, 32.0f));

    // 0x0 图像回退到 100x100 自然尺寸。
    ImageView iv0{ Image{} };
    BuildContext ctx0;
    iv0.mount(ctx0);
    iv0.layout(c, ctx0);
    AURORA_TEST_CHECK(near_f(iv0.size().width, 100.0f));
    AURORA_TEST_CHECK(near_f(iv0.size().height, 100.0f));

    // from_file 缺失文件 → 返回空图像（不抛异常）。
    auto missing = ImageView::from_file("does_not_exist.png");
    AURORA_TEST_CHECK(missing.bitmap.pixels.empty());
    AURORA_TEST_CHECK(missing.bitmap.width == 0 && missing.bitmap.height == 0);

    // source 序列化：props 含 source / image_width / image_height。
    ImageView iv_src{ ImageViewProps{ .bitmap = img, .source = std::string("logo.png") } };
    auto js = serialization::to_json(iv_src);
    AURORA_TEST_CHECK(js.contains("type") && js["type"].get<std::string>() == "Image");
    AURORA_TEST_CHECK(js["props"].contains("source") && js["props"]["source"].get<std::string>() == "logo.png");
    AURORA_TEST_CHECK(js["props"]["image_width"].get<int>() == 32);
    AURORA_TEST_CHECK(js["props"]["image_height"].get<int>() == 24);

    // 真实 PNG 加载（走 stb）：解码成功且尺寸、像素长度正确。
    AURORA_TEST_CHECK(try_load_golden());
    auto gr = Image::load("tests/golden/golden_basic_column.png");
    if (!gr.ok()) {
        gr = Image::load("../tests/golden/golden_basic_column.png");
    }
    if (gr.ok()) {
        const Image &g = gr.value();
        AURORA_TEST_CHECK(g.width > 0 && g.height > 0);
        AURORA_TEST_CHECK(g.pixels.size() == static_cast<size_t>(g.width) * g.height * 4);
        ImageView ivg{ g };
        AURORA_TEST_CHECK(ivg.bitmap.width == g.width && ivg.bitmap.height == g.height);
    }

    // 约束上限裁剪：max=(20,20)，自然尺寸(32,24) 被夹到 (20,20)。
    ImageView iv_big{ img };
    BuildContext ctx_b;
    iv_big.mount(ctx_b);
    Constraints tight{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 20, .height = 20 } };
    iv_big.layout(tight, ctx_b);
    AURORA_TEST_CHECK(near_f(iv_big.size().width, 20.0f));
    AURORA_TEST_CHECK(near_f(iv_big.size().height, 20.0f));
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_image_view ===\n");
    test_image_view();
}
