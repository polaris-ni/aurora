/// 测试类型: unit
/// 目标单元: include/aurora/widget/image_widget.h
/// 测试说明: 覆盖 ImageView——默认不变量与自描述、无图占位自然尺寸、位图自然尺寸与约束钳制、
/// Fixed 宽高意图严格等值覆盖（显式盒语义，不受 max 约束钳制）、无图占位描边框/有图栅格化（内存位图，软件 Painter
/// 像素断言）、 source 序列化往返与非字符串防御、from_file 失败回退占位

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/image_widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_image_widget {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 构造 w×h 的内存图像（RGBA8，像素清零，不依赖文件系统）。
auto make_image(int w, int h) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0U);
    return img;
}

}  // namespace

AURORA_TEST_CASE(default_image_view_invariants) {
    ImageView iv;
    AURORA_TEST_CHECK_EQ(std::string{iv.type_name()}, "Image");
    AURORA_TEST_CHECK_EQ(iv.bitmap.width, 0);
    AURORA_TEST_CHECK_TRUE(iv.bitmap.pixels.empty());
    AURORA_TEST_CHECK_FALSE(iv.source.has_value());

    const auto d = ImageView::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Image");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_source = false;
    bool has_image_width = false;
    bool has_image_height = false;
    for (const auto& p : d.properties) {
        if (p.name == "source") {
            has_source = true;
        }
        if (p.name == "image_width") {
            has_image_width = true;
        }
        if (p.name == "image_height") {
            has_image_height = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_source);
    AURORA_TEST_CHECK_TRUE(has_image_width);
    AURORA_TEST_CHECK_TRUE(has_image_height);
    AURORA_TEST_CHECK_EQ(std::string{iv.describe().name}, "Image");
}

AURORA_TEST_CASE(empty_bitmap_uses_placeholder_natural_size) {
    // 无图占位自然尺寸 100×100。
    ImageView iv;
    LayoutEngine::layout(iv, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(iv.size().width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(iv.size().height, 100.0F, 1e-4F);

    // 占位自然尺寸同样受父约束钳制。
    ImageView squeezed;
    LayoutEngine::layout(squeezed, bounded(50.0F, 500.0F));
    AURORA_TEST_CHECK_NEAR(squeezed.size().width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(squeezed.size().height, 100.0F, 1e-4F);
}

AURORA_TEST_CASE(bitmap_natural_size_and_constraint_clamp) {
    ImageView small{make_image(32, 16)};
    LayoutEngine::layout(small, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(small.size().width, 32.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(small.size().height, 16.0F, 1e-4F);

    // 位图大于约束时钳到约束上限。
    ImageView huge{make_image(500, 400)};
    LayoutEngine::layout(huge, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(huge.size().width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(huge.size().height, 100.0F, 1e-4F);
}

AURORA_TEST_CASE(fixed_length_strict_override) {
    ImageView iv{make_image(32, 16)};
    iv.width(Length::fixed(200.0F));
    iv.height(Length::fixed(50.0F));
    LayoutEngine::layout(iv, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(iv.size().width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(iv.size().height, 50.0F, 1e-4F);

    // Fixed 显式盒语义（widget.cpp）：显式轴最终尺寸严格等于设定值，不受 max 约束钳制
    // （约束先被夹成 [v,v]，布局后 size 再被覆写为 v；ImageView::on_layout 内的钳制不构成最终尺寸）。
    ImageView strict{make_image(32, 16)};
    strict.width(Length::fixed(1000.0F));
    LayoutEngine::layout(strict, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(strict.size().width, 1000.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(strict.size().height, 16.0F, 1e-4F);
}

AURORA_TEST_CASE(paint_empty_bitmap_draws_placeholder_rect) {
    ImageView iv;
    Painter p;
    p.begin(64, 64);
    const Rect vp{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 64.0F}};
    iv.paint(p, vp, BuildContext{});
    AURORA_TEST_CHECK_TRUE(iv.paint_bounds() == vp);
    // 占位为 180 灰 1px 描边框（Painter::draw_rect = 四条 1px fill_rect 边带），非实心填充：
    // 边框像素为 {180,180,180,255}，框内保持 begin 后的全零底色 {0,0,0,0}。逐通道断言（Color 不可打印）。
    const Color top = p.get_pixel(32, 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(top.r), 180);
    AURORA_TEST_CHECK_EQ(static_cast<int>(top.g), 180);
    AURORA_TEST_CHECK_EQ(static_cast<int>(top.b), 180);
    AURORA_TEST_CHECK_EQ(static_cast<int>(top.a), 255);

    const Color left = p.get_pixel(0, 32);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.r), 180);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.g), 180);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.b), 180);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.a), 255);

    const Color center = p.get_pixel(32, 32);
    AURORA_TEST_CHECK_EQ(static_cast<int>(center.r), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(center.g), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(center.b), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(center.a), 0);
}

AURORA_TEST_CASE(paint_decoded_bitmap_rasterizes_pixels) {
    auto img = make_image(2, 2);
    for (std::size_t i = 0; i < img.pixels.size(); i += 4U) {
        img.pixels[i] = 255U;  // R
        img.pixels[i + 3U] = 255U;  // A
    }
    ImageView iv{std::move(img)};
    Painter p;
    p.begin(8, 8);
    const Rect vp{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 8.0F, .height = 8.0F}};
    iv.paint(p, vp, BuildContext{});
    // 位图缩放铺满目标矩形，四角均为红色。
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, 0), Color{255, 0, 0, 255});
    AURORA_TEST_CHECK_EQ(p.get_pixel(7, 7), Color{255, 0, 0, 255});
}

AURORA_TEST_CASE(source_serialization_roundtrip) {
    ImageViewProps props_src{.bitmap = make_image(32, 16), .source = "logo.png"};
    ImageView src{std::move(props_src)};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["source"].get<std::string>(), "logo.png");
    AURORA_TEST_CHECK_EQ(props["image_width"].get<int>(), 32);
    AURORA_TEST_CHECK_EQ(props["image_height"].get<int>(), 16);

    ImageView dst;
    dst.deserialize_props(props);
    AURORA_TEST_REQUIRE_TRUE(dst.source.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(dst.source.value(), "logo.png");

    // 未设置 source 时不落盘该键。
    Json bare;
    ImageView{}.serialize_props(bare);
    AURORA_TEST_CHECK_FALSE(bare.contains("source"));

    // 非字符串 source 反序列化被忽略（is_string 防御）。
    Json bad;
    bad["source"] = 42;
    ImageView guarded;
    guarded.deserialize_props(bad);
    AURORA_TEST_CHECK_FALSE(guarded.source.has_value());
}

AURORA_TEST_CASE(from_file_missing_path_falls_back_to_empty) {
    ImageView iv = ImageView::from_file("./aurora_utest_no_such_dir/missing.png");
    AURORA_TEST_CHECK_EQ(iv.bitmap.width, 0);
    AURORA_TEST_CHECK_TRUE(iv.bitmap.pixels.empty());
    // 解码失败回退空图像：布局走占位自然尺寸。
    LayoutEngine::layout(iv, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(iv.size().width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(iv.size().height, 100.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_image_widget
