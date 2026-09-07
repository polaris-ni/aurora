/// 测试类型: unit
/// 目标单元: include/aurora/widget/image_widget.h
/// 测试说明: 图像控件（默认空位图、类型名/自描述、from_file 失败降级、按位图或约束解析布局尺寸）单元测试

#include <optional>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"
#include "aurora/widget/image_widget.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_image_widget {

AURORA_TEST() {
    // ---- 1. 默认构造：空位图、无源路径、类型名为 "Image" ----
    {
        ImageView iv;
        AURORA_TEST_CHECK_EQ(std::string(iv.type_name()), std::string("Image"));
        AURORA_TEST_CHECK_EQ(iv.bitmap.width, 0);
        AURORA_TEST_CHECK_EQ(iv.bitmap.height, 0);
        AURORA_TEST_CHECK(iv.bitmap.pixels.empty());
        AURORA_TEST_CHECK_FALSE(iv.source.has_value());
    }

    // ---- 2. 以 Props 配置块构造：位图尺寸与源路径透传 ----
    {
        Image img;
        img.width = 50;
        img.height = 80;
        ImageView iv{ImageViewProps{.bitmap = std::move(img), .source = std::string("a.png")}};
        AURORA_TEST_CHECK_EQ(iv.bitmap.width, 50);
        AURORA_TEST_CHECK_EQ(iv.bitmap.height, 80);
        AURORA_TEST_CHECK(iv.source.has_value());
        AURORA_TEST_CHECK_EQ(iv.source.value_or("?"), std::string("a.png"));
    }

    // ---- 3. from_file 解码失败：返回空位图占位，不抛异常 ----
    {
        ImageView iv = ImageView::from_file("this_file_does_not_exist_zzz.png");
        AURORA_TEST_CHECK_EQ(iv.bitmap.width, 0);
        AURORA_TEST_CHECK_EQ(iv.bitmap.height, 0);
    }

    // ---- 4. 布局解析：有位图按自然尺寸；空位图回退 100×100，均受约束夹取 ----
    {
        const BuildContext ctx;
        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = 200.0F, .height = 200.0F}};

        Image img;
        img.width = 50;
        img.height = 80;
        ImageView filled{ImageViewProps{.bitmap = std::move(img), .source = std::nullopt}};
        const Size fs = filled.layout(c, ctx);
        AURORA_TEST_CHECK_NEAR(fs.width, 50.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(fs.height, 80.0F, 1e-3F);

        ImageView empty{};
        const Size es = empty.layout(c, ctx);
        AURORA_TEST_CHECK_NEAR(es.width, 100.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(es.height, 100.0F, 1e-3F);
    }

    // ---- 5. 自描述：类型名 Image、叶子（无子）、属性含 source/image_width/image_height ----
    {
        const WidgetDescriptor d = ImageView::describe_static();
        AURORA_TEST_CHECK_EQ(std::string(d.name), std::string("Image"));
        AURORA_TEST_CHECK_EQ(std::string(d.children_policy), std::string("none"));
        bool has_source = false;
        bool has_w = false;
        bool has_h = false;
        for (const auto &p : d.properties) {
            has_source = has_source || (p.name == "source");
            has_w = has_w || (p.name == "image_width");
            has_h = has_h || (p.name == "image_height");
        }
        AURORA_TEST_CHECK(has_source);
        AURORA_TEST_CHECK(has_w);
        AURORA_TEST_CHECK(has_h);
    }
}

}  // namespace aurora::test_cases::utest_image_widget
