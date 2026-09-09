/// 测试类型: unit
/// 目标单元: include/aurora/widget/rich_text.h
/// 测试说明: 覆盖 RichText——多 span 组装与逐 span 样式落词、空 span 尺寸为零、
/// 布局尺寸来自 span 度量且与 measure_rich_text 一致、窄约束确定性换行、
/// 序列化拼接文本往返与 split_words 纯函数行为

#include <string>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/rich_text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_rich_text {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_rich_text_metadata_and_signal) {
    RichText rt;
    AURORA_TEST_CHECK_EQ(std::string{rt.type_name()}, "RichText");

    const auto d = RichText::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, std::string{"RichText"});
    AURORA_TEST_CHECK_EQ(d.children_policy, std::string{"none"});
    AURORA_TEST_CHECK_TRUE(d.events.empty());
    bool has_text = false;
    for (const auto &p : d.properties) {
        if (p.name == "text") {
            has_text = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_text);

    // spans_ 是本控件唯一响应式信号。
    std::vector<SignalViewBase *> out;
    rt.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.size(), 1U);
}

AURORA_TEST_CASE(empty_spans_measure_and_layout_to_zero) {
    RichText empty;
    LayoutEngine::layout(empty, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(empty.size().width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(empty.size().height, 0.0F, 1e-4F);

    const Size m = measure_rich_text(std::vector<TextSpan>{}, 300.0F);
    AURORA_TEST_CHECK_NEAR(m.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(m.height, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(widget_layout_matches_measure_rich_text) {
    // 控件 on_layout 的尺寸必须与自由函数 measure_rich_text 一致（同约束同输入）。
    const std::vector<TextSpan> spans{
        TextSpan{.text = LocalizedString{"Hello rich text"}, .font = Font{.size_pt = 18.0F}}};
    RichText rt{Reactive<std::vector<TextSpan>>{spans}};
    LayoutEngine::layout(rt, bounded(400.0F, 400.0F));

    const Size m = measure_rich_text(spans, 400.0F);
    AURORA_TEST_CHECK_TRUE(m.width > 0.0F);
    AURORA_TEST_CHECK_TRUE(m.height > 0.0F);
    AURORA_TEST_CHECK_NEAR(rt.size().width, m.width, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rt.size().height, m.height, 1e-4F);
}

AURORA_TEST_CASE(narrow_width_wraps_deterministically) {
    // 5 个词在 1px 宽约束下逐词成行：总高 = 5 × 单行高（行高只依赖字号）。
    const std::vector<TextSpan> spans{
        TextSpan{.text = LocalizedString{"one two three four five"}, .font = Font{.size_pt = 16.0F}}};

    RichText one_line{Reactive<std::vector<TextSpan>>{spans}};
    LayoutEngine::layout(one_line, bounded(400.0F, 400.0F));
    RichText wrapped{Reactive<std::vector<TextSpan>>{spans}};
    LayoutEngine::layout(wrapped, bounded(1.0F, 10000.0F));

    AURORA_TEST_CHECK_NEAR(wrapped.size().height, 5.0F * one_line.size().height, 1e-4F);
    // 换行后宽度收窄为最宽单词。
    AURORA_TEST_CHECK_TRUE(wrapped.size().width < one_line.size().width);
    AURORA_TEST_CHECK_TRUE(wrapped.size().width > 0.0F);
}

AURORA_TEST_CASE(layout_carries_per_span_style) {
    // 每个 word 携带其所属 span 的字体/颜色与实测宽度。
    const Font font_plain{.size_pt = 16.0F, .weight = 400};
    const Font font_bold{.size_pt = 16.0F, .weight = 700};
    const std::vector<TextSpan> spans{
        TextSpan{.text = LocalizedString{"Red"}, .font = font_plain, .color = Color::red()},
        TextSpan{.text = LocalizedString{"Blue"}, .font = font_bold, .color = Color::blue()},
    };

    const std::vector<detail::RichLine> lines = layout_rich_text(spans, 200.0F, Locale{});
    AURORA_TEST_REQUIRE_EQ(lines.size(), 1U);
    AURORA_TEST_REQUIRE_EQ(lines[0].words.size(), 2U);
    AURORA_TEST_CHECK_NEAR(lines[0].y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(lines[0].height > 0.0F);

    const detail::RichWord &red = lines[0].words[0];
    const detail::RichWord &blue = lines[0].words[1];
    AURORA_TEST_CHECK_EQ(red.text, std::string{"Red"});
    AURORA_TEST_CHECK_TRUE(red.font == font_plain);
    AURORA_TEST_CHECK_TRUE(red.color == Color::red());
    AURORA_TEST_CHECK_TRUE(red.width > 0.0F);
    AURORA_TEST_CHECK_EQ(blue.text, std::string{"Blue"});
    AURORA_TEST_CHECK_TRUE(blue.font == font_bold);
    AURORA_TEST_CHECK_TRUE(blue.color == Color::blue());
}

AURORA_TEST_CASE(serialize_concatenates_and_deserialize_reloads) {
    // 序列化把全部 span 的字面文本拼接为 "text"；反序列化重建为单 span。
    RichText src{Reactive<std::vector<TextSpan>>{std::vector<TextSpan>{
        TextSpan{.text = LocalizedString{"Hel"}, .color = Color::red()},
        TextSpan{.text = LocalizedString{"lo"}, .color = Color::blue()},
    }}};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["text"].get<std::string>(), "Hello");

    RichText dst;
    dst.deserialize_props(props);
    Json back;
    dst.serialize_props(back);
    AURORA_TEST_CHECK_EQ(back["text"].get<std::string>(), "Hello");
}

AURORA_TEST_CASE(split_words_collapses_repeated_spaces) {
    // 按空格拆词：连续空格折叠、全空白输入得到空序列。
    using aurora::detail::split_words;

    AURORA_TEST_CHECK_TRUE(split_words("").empty());
    AURORA_TEST_CHECK_TRUE(split_words("   ").empty());

    const auto two = split_words("a b");
    AURORA_TEST_REQUIRE_EQ(two.size(), 2U);
    AURORA_TEST_CHECK_EQ(two[0], std::string{"a"});
    AURORA_TEST_CHECK_EQ(two[1], std::string{"b"});

    const auto collapsed = split_words("a   b");
    AURORA_TEST_REQUIRE_EQ(collapsed.size(), 2U);
    AURORA_TEST_CHECK_EQ(collapsed[0], std::string{"a"});
    AURORA_TEST_CHECK_EQ(collapsed[1], std::string{"b"});
}

}  // namespace aurora::test_cases::utest_rich_text
