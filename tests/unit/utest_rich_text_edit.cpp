/// 测试类型: unit
/// 目标单元: include/aurora/widget/rich_text_edit.h
/// 测试说明: 覆盖 RichTextEdit——初始不变量、load_spans/to_spans 往返与相邻同样式合并、
/// 当前输入样式链式 setter 与 toggle、序列化纯文本往返（字符取当前样式）、
/// 布局高度按行计数与自描述元数据

#include <string>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/rich_text_edit.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_rich_text_edit {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(fresh_edit_initial_invariants) {
    RichTextEdit edit;
    AURORA_TEST_CHECK_EQ(std::string{edit.type_name()}, "RichTextEdit");
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{});
    AURORA_TEST_CHECK_EQ(edit.caret(), 0U);
    AURORA_TEST_CHECK_FALSE(edit.has_selection());
    AURORA_TEST_CHECK_EQ(edit.selected_text(), std::string{});
    AURORA_TEST_CHECK_TRUE(edit.to_spans().empty());
    AURORA_TEST_CHECK_EQ(edit.undo_stack(), nullptr);

    // 无自有响应式信号。
    std::vector<SignalViewBase *> out;
    edit.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.size(), 0U);
}

AURORA_TEST_CASE(load_spans_roundtrips_via_to_spans) {
    const Font font_small{.size_pt = 12.0F};
    const Font font_big{.size_pt = 24.0F};

    RichTextEdit edit;
    edit.load_spans({
        TextSpan{.text = LocalizedString{"ab"}, .font = font_small, .color = Color::red()},
        TextSpan{.text = LocalizedString{"cd"}, .font = font_big, .color = Color::blue()},
    });
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"abcd"});
    // 加载后光标归零、无选区。
    AURORA_TEST_CHECK_EQ(edit.caret(), 0U);
    AURORA_TEST_CHECK_FALSE(edit.has_selection());

    const auto spans = edit.to_spans();
    AURORA_TEST_REQUIRE_EQ(spans.size(), 2U);
    AURORA_TEST_CHECK_EQ(spans[0].text.text, std::string{"ab"});
    AURORA_TEST_CHECK_TRUE(spans[0].font == font_small);
    AURORA_TEST_CHECK_TRUE(spans[0].color == Color::red());
    AURORA_TEST_CHECK_EQ(spans[1].text.text, std::string{"cd"});
    AURORA_TEST_CHECK_TRUE(spans[1].font == font_big);
    AURORA_TEST_CHECK_TRUE(spans[1].color == Color::blue());
}

AURORA_TEST_CASE(to_spans_merges_adjacent_same_style) {
    // 连续同样式（font 与 color 均相等）的字符合并为一个 span。
    const Font font_a{.size_pt = 14.0F};
    const Font font_b{.size_pt = 20.0F};

    RichTextEdit edit;
    edit.load_spans({
        TextSpan{.text = LocalizedString{"a"}, .font = font_a, .color = Color::red()},
        TextSpan{.text = LocalizedString{"b"}, .font = font_a, .color = Color::red()},
        TextSpan{.text = LocalizedString{"c"}, .font = font_b, .color = Color::red()},
    });
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"abc"});

    const auto spans = edit.to_spans();
    AURORA_TEST_REQUIRE_EQ(spans.size(), 2U);
    AURORA_TEST_CHECK_EQ(spans[0].text.text, std::string{"ab"});
    AURORA_TEST_CHECK_TRUE(spans[0].font == font_a);
    AURORA_TEST_CHECK_EQ(spans[1].text.text, std::string{"c"});
    AURORA_TEST_CHECK_TRUE(spans[1].font == font_b);
}

AURORA_TEST_CASE(current_style_setters_and_toggles) {
    RichTextEdit edit;
    edit.set_current_font(Font{.size_pt = 18.0F, .weight = 400})
        .set_current_color(Color::red())
        .set_current_underline(false);
    AURORA_TEST_CHECK_TRUE(edit.current_font() == Font{.size_pt = 18.0F});
    AURORA_TEST_CHECK_TRUE(edit.current_color() == Color::red());
    AURORA_TEST_CHECK_FALSE(edit.current_underline());

    // toggle_bold：weight 400 ↔ 700。
    edit.toggle_bold();
    AURORA_TEST_CHECK_EQ(edit.current_font().weight, 700);
    edit.toggle_bold();
    AURORA_TEST_CHECK_EQ(edit.current_font().weight, 400);

    // toggle_italic：family 尾部 "/I" 标记可逆。
    edit.toggle_italic();
    AURORA_TEST_CHECK_EQ(edit.current_font().family, std::string{"sans-serif/I"});
    edit.toggle_italic();
    AURORA_TEST_CHECK_EQ(edit.current_font().family, std::string{"sans-serif"});

    // toggle_underline：布尔翻转。
    edit.toggle_underline();
    AURORA_TEST_CHECK_TRUE(edit.current_underline());
    edit.toggle_underline();
    AURORA_TEST_CHECK_FALSE(edit.current_underline());
}

AURORA_TEST_CASE(serialize_deserialize_plain_text_roundtrip) {
    RichTextEdit src;
    src.load_spans({TextSpan{.text = LocalizedString{"Hi"}, .font = Font{.size_pt = 16.0F}, .color = Color::red()}});
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["text"].get<std::string>(), "Hi");

    // 反序列化字符取接收方「当前输入样式」（默认字体 + 显式设置的蓝色）。
    RichTextEdit dst;
    dst.set_current_color(Color::blue());
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.plain_text(), std::string{"Hi"});
    AURORA_TEST_CHECK_EQ(dst.caret(), 0U);

    const auto spans = dst.to_spans();
    AURORA_TEST_REQUIRE_EQ(spans.size(), 1U);
    AURORA_TEST_CHECK_EQ(spans[0].text.text, std::string{"Hi"});
    AURORA_TEST_CHECK_TRUE(spans[0].font == Font{});
    AURORA_TEST_CHECK_TRUE(spans[0].color == Color::blue());
}

AURORA_TEST_CASE(layout_height_counts_lines) {
    // 空文档 1 行；"a\nb" 两行；行高一致 → 总高恰为 2 倍（宽度取约束宽）。
    RichTextEdit single;
    LayoutEngine::layout(single, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(single.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(single.size().height > 0.0F);

    RichTextEdit two_lines;
    two_lines.load_spans({TextSpan{.text = LocalizedString{"a\nb"}}});
    LayoutEngine::layout(two_lines, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(two_lines.size().height, 2.0F * single.size().height, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata_and_events) {
    const auto d = RichTextEdit::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, std::string{"RichTextEdit"});
    AURORA_TEST_CHECK_EQ(d.children_policy, std::string{"none"});

    bool has_text = false;
    for (const auto &p : d.properties) {
        if (p.name == "text") {
            has_text = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_text);

    bool has_text_input_event = false;
    for (const auto &ev : d.events) {
        if (ev == "on_text_input") {
            has_text_input_event = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_text_input_event);
}

}  // namespace aurora::test_cases::utest_rich_text_edit
