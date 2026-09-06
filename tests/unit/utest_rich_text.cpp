/// 测试类型: unit
/// 目标单元: include/aurora/aurora.h
/// 测试说明: rich_text 单元测试
///

// RichText / TextSpan 1:1 测试：富文本测量与换行。

#include <algorithm>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_rich_text {


namespace render = aurora::render;

static void test_rich_text() {
    const TextSpan bold{.text = LocalizedString{"Hello "}, .font = Font{.size_pt = 14.0F}, .color = Color{200, 30, 30}};
    const TextSpan plain{.text = LocalizedString{"World"}, .font = Font{.size_pt = 20.0F}, .color = Color{30, 80, 200}};
    AURORA_TEST_CHECK_MSG(near_f(bold.font.size_pt, 14.0F), "RichText: span font size");
    AURORA_TEST_CHECK_MSG(bold.color.r == 200, "RichText: span color red");
    AURORA_TEST_CHECK_MSG(bold.color.b == 30, "RichText: span color blue");

    const std::vector spans = {bold, plain};
    const float single_w = measure_rich_text(spans, 1e9F, Locale{}).width;
    const float sum_no_space = render::FontEngine::measure_width("Hello", Font{.size_pt = 14.0F}) +
                               render::FontEngine::measure_width("World", Font{.size_pt = 20.0F});
    const float max_space = std::max(render::FontEngine::measure_width(" ", Font{.size_pt = 14.0F}),
                                     render::FontEngine::measure_width(" ", Font{.size_pt = 20.0F}));
    AURORA_TEST_CHECK_MSG(single_w > sum_no_space, "RichText width includes inter-run space");
    AURORA_TEST_CHECK_MSG(single_w <= sum_no_space + max_space + 0.001F, "RichText width has at most one space");
    const float single_h = measure_rich_text(spans, 1e9F, Locale{}).height;
    AURORA_TEST_CHECK_MSG(single_h > 0.0F, "RichText has positive height");

    const float wrapped_h = measure_rich_text(spans, 20.0F, Locale{}).height;
    AURORA_TEST_CHECK_MSG(wrapped_h > single_h, "RichText wraps to multiple lines under narrow width");
}

static void test_rich_text_widget() {
    const std::vector spans = {
        TextSpan{.text = LocalizedString{"Hello "}, .font = Font{.size_pt = 14.0F}, .color = Color{200, 30, 30}},
        TextSpan{.text = LocalizedString{"World"}, .font = Font{.size_pt = 20.0F}, .color = Color{30, 80, 200}}};
    RichText rt{Reactive{spans}};
    const BuildContext ctx;
    rt.mount(ctx);
    rt.layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 200, .height = 200}}, ctx);
    AURORA_TEST_CHECK_MSG(rt.size().width > 0.0F && rt.size().height > 0.0F, "RichText widget lays out");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_rich_text ===\n");
    test_rich_text();
    test_rich_text_widget();
}


}  // namespace aurora::test_cases::utest_rich_text