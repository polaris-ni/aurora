/// 测试类型: integration
/// 目标单元: include/aurora/widget/image_widget.h
/// 测试说明: ImageView 控件与布局引擎/解码链路集成——位图自然尺寸与约束钳制、固定宽高覆盖、
///           0x0 占位回退、from_file 失败安全、source 序列化、真实 golden PNG 文件解码进控件

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/image_widget.h"
#include "framework/aurora_test.h"
#include "support/paths.h"

namespace aurora::test_cases::itest_image_view {

namespace {

/// 无约束下限、给定上限的布局约束。
auto bounded(float w, float h) -> aurora::Constraints {
    return aurora::Constraints{
        .min = aurora::Size{.width = 0.0F, .height = 0.0F},
        .max = aurora::Size{.width = w, .height = h},
    };
}

/// 构造 w×h 全不透明像素图像（RGBA8，长度 = w*h*4）。
auto make_image(int w, int h) -> aurora::Image {
    aurora::Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 255U);
    return img;
}

}  // namespace

AURORA_TEST_CASE(image_view_natural_size_and_bitmap) {
    // 合成 32x24 图像：布局走位图自然尺寸，类型名与位图字段一致。
    aurora::ImageView iv{make_image(32, 24)};
    aurora::LayoutEngine::layout(iv, bounded(100.0F, 100.0F));

    AURORA_TEST_CHECK_NEAR(iv.size().width, 32.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(iv.size().height, 24.0F, 1e-3F);
    AURORA_TEST_CHECK_EQ(std::string{iv.type_name()}, std::string{"Image"});
    AURORA_TEST_CHECK_EQ(iv.bitmap.width, 32);
    AURORA_TEST_CHECK_EQ(iv.bitmap.height, 24);
}

AURORA_TEST_CASE(image_view_fixed_width_keeps_natural_height) {
    // 固定宽度覆盖：高度仍按内容自然尺寸。
    aurora::ImageView iv_w{make_image(32, 24)};
    iv_w.width(aurora::Length::fixed(80.0F));
    aurora::LayoutEngine::layout(iv_w, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(iv_w.size().width, 80.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(iv_w.size().height, 24.0F, 1e-3F);

    // 固定高度覆盖：宽度仍按内容自然尺寸。
    aurora::ImageView iv_h{make_image(32, 24)};
    iv_h.height(aurora::Length::fixed(60.0F));
    aurora::LayoutEngine::layout(iv_h, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(iv_h.size().height, 60.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(iv_h.size().width, 32.0F, 1e-3F);
}

AURORA_TEST_CASE(image_view_zero_size_bitmap_falls_back_to_placeholder) {
    // 0x0 图像回退到 100x100 自然尺寸（占位语义）。
    aurora::ImageView iv0{aurora::Image{}};
    aurora::LayoutEngine::layout(iv0, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(iv0.size().width, 100.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(iv0.size().height, 100.0F, 1e-3F);
}

AURORA_TEST_CASE(image_view_from_file_missing_path_is_safe) {
    // from_file 缺失文件 → 返回空图像（不抛异常），布局走占位自然尺寸。
    auto missing = aurora::ImageView::from_file("does_not_exist.png");
    AURORA_TEST_CHECK_TRUE(missing.bitmap.pixels.empty());
    AURORA_TEST_CHECK_EQ(missing.bitmap.width, 0);
    AURORA_TEST_CHECK_EQ(missing.bitmap.height, 0);
    aurora::LayoutEngine::layout(missing, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(missing.size().width, 100.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(missing.size().height, 100.0F, 1e-3F);
}

AURORA_TEST_CASE(image_view_source_props_serialization) {
    // source 序列化：props 含 source / image_width / image_height。
    aurora::ImageViewProps props{.bitmap = make_image(32, 24), .source = std::string("logo.png")};
    aurora::ImageView iv_src{std::move(props)};
    aurora::Json js;
    iv_src.serialize_props(js);
    AURORA_TEST_CHECK_EQ(js["source"].get<std::string>(), std::string{"logo.png"});
    AURORA_TEST_CHECK_EQ(js["image_width"].get<int>(), 32);
    AURORA_TEST_CHECK_EQ(js["image_height"].get<int>(), 24);
}

AURORA_TEST_CASE(image_view_golden_png_file_decodes_into_widget) {
    // 真实 PNG 加载（仓库 golden 基准图，走 stb）：解码成功且尺寸、像素长度正确，
    // 控件持有同一尺寸位图。
    const std::string path = aurora::testing::paths::under_repo("tests/golden/golden_basic_column.png");
    const auto gr = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_MSG(gr.ok(), "golden_basic_column.png missing or undecodable");
    const aurora::Image &g = gr.value();
    AURORA_TEST_CHECK_TRUE(g.width > 0);
    AURORA_TEST_CHECK_TRUE(g.height > 0);
    AURORA_TEST_CHECK_EQ(g.pixels.size(),
                         static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.height) * 4U);

    aurora::ImageView ivg{g};
    AURORA_TEST_CHECK_EQ(ivg.bitmap.width, g.width);
    AURORA_TEST_CHECK_EQ(ivg.bitmap.height, g.height);
}

AURORA_TEST_CASE(image_view_constraint_clamps_oversized_bitmap) {
    // 约束上限裁剪：max=(20,20)，自然尺寸(32,24) 被夹到 (20,20)。
    aurora::ImageView iv_big{make_image(32, 24)};
    aurora::LayoutEngine::layout(iv_big, bounded(20.0F, 20.0F));
    AURORA_TEST_CHECK_NEAR(iv_big.size().width, 20.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(iv_big.size().height, 20.0F, 1e-3F);
}

}  // namespace aurora::test_cases::itest_image_view
