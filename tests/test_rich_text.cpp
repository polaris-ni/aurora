// test_rich_text.cpp — RichText / TextSpan 1:1 测试：富文本测量与换行。

#include <algorithm>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

namespace render = aurora::render;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::Font;
using aurora::Locale;
using aurora::LocalizedString;
using aurora::Reactive;
using aurora::RichText;
using aurora::Size;
using aurora::TextSpan;

static void test_rich_text() {
    const TextSpan bold{ .text = LocalizedString{ "Hello " },
                         .font = Font{ .size_pt = 14.0f },
                         .color = Color{ 200, 30, 30 } };
    const TextSpan plain{ .text = LocalizedString{ "World" },
                          .font = Font{ .size_pt = 20.0f },
                          .color = Color{ 30, 80, 200 } };
    AURORA_TEST_CHECK_MSG(near_f(bold.font.size_pt, 14.0f), "RichText: span font size");
    AURORA_TEST_CHECK_MSG(bold.color.m_r == 200, "RichText: span color red");
    AURORA_TEST_CHECK_MSG(bold.color.m_b == 30, "RichText: span color blue");

    const std::vector spans = { bold, plain };
    const float single_w = measure_rich_text(spans, 1e9f, Locale{}).width;
    const float sum_no_space = render::FontEngine::instance().measure_width("Hello", Font{ .size_pt = 14.0f }) +
                               render::FontEngine::instance().measure_width("World", Font{ .size_pt = 20.0f });
    const float max_space = std::max(render::FontEngine::instance().measure_width(" ", Font{ .size_pt = 14.0f }),
                                     render::FontEngine::instance().measure_width(" ", Font{ .size_pt = 20.0f }));
    AURORA_TEST_CHECK_MSG(single_w > sum_no_space, "RichText width includes inter-run space");
    AURORA_TEST_CHECK_MSG(single_w <= sum_no_space + max_space + 0.001f, "RichText width has at most one space");
    const float single_h = measure_rich_text(spans, 1e9f, Locale{}).height;
    AURORA_TEST_CHECK_MSG(single_h > 0.0f, "RichText has positive height");

    const float wrapped_h = measure_rich_text(spans, 20.0f, Locale{}).height;
    AURORA_TEST_CHECK_MSG(wrapped_h > single_h, "RichText wraps to multiple lines under narrow width");
}

static void test_rich_text_widget() {
    const std::vector spans = {
        TextSpan{
            .text = LocalizedString{ "Hello " }, .font = Font{ .size_pt = 14.0f }, .color = Color{ 200, 30, 30 } },
        TextSpan{ .text = LocalizedString{ "World" }, .font = Font{ .size_pt = 20.0f }, .color = Color{ 30, 80, 200 } }
    };
    RichText rt{ Reactive{ spans } };
    const BuildContext ctx;
    rt.mount(ctx);
    rt.layout(Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 200, .height = 200 } }, ctx);
    AURORA_TEST_CHECK_MSG(rt.size().width > 0.0f && rt.size().height > 0.0f, "RichText widget lays out");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_rich_text ===\n");
    test_rich_text();
    test_rich_text_widget();
}
