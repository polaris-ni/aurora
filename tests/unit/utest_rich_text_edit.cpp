/// 测试类型: unit
/// 目标单元: include/aurora/widget/rich_text_edit.h
/// 测试说明: 覆盖 RichTextEdit——初始不变量、load_spans/to_spans 往返与相邻同样式合并、
/// 当前输入样式链式 setter 与 toggle、序列化纯文本往返（字符取当前样式）、
/// 布局高度按行计数与自描述元数据、IME 组合输入（preedit 显示 / 上屏落字 / 失焦取消）。
/// A2 RTL 切片：书写方向 API 往返与不落盘、指针命中逻辑↔视觉镜像、RTL 段右对齐像素、
/// 选区高亮镜像、聚焦光标落右缘像素

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/event/keycode.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
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

AURORA_TEST_CASE(composition_preedit_then_commit) {
    // preedit 不进文档（不上屏 → 不参与撤销栈/序列化），上屏时才落字。
    RichTextEdit edit;
    edit.on_focus_change(true);

    TextCompositionEvent typing;
    typing.preedit = "nihao";
    typing.cursor_index = 5;
    edit.on_text_composition(typing);
    AURORA_TEST_CHECK_TRUE(typing.is_handled);
    AURORA_TEST_CHECK_TRUE(edit.is_composing());
    AURORA_TEST_CHECK_EQ(edit.preedit(), std::string{"nihao"});
    AURORA_TEST_CHECK_EQ(edit.composition_cursor(), static_cast<std::size_t>(5));
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{""});  // 未上屏
    AURORA_TEST_CHECK_EQ(edit.accessibility_value(), std::string{"nihao"});

    TextCompositionEvent commit;
    commit.committed = "你好";
    edit.on_text_composition(commit);
    AURORA_TEST_CHECK_FALSE(edit.is_composing());
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"你好"});

    // 序列化只反映已上屏文本，不含 preedit
    TextCompositionEvent pending;
    pending.preedit = "zai";
    edit.on_text_composition(pending);
    Json out;
    edit.serialize_props(out);
    AURORA_TEST_CHECK_EQ(out["text"].get<std::string>(), "你好");
}

AURORA_TEST_CASE(blur_cancels_pending_composition) {
    RichTextEdit edit;
    edit.on_focus_change(true);
    TextCompositionEvent composing;
    composing.preedit = "zhong";
    edit.on_text_composition(composing);
    AURORA_TEST_CHECK_TRUE(edit.is_composing());

    edit.on_focus_change(false);
    AURORA_TEST_CHECK_FALSE(edit.is_composing());
    AURORA_TEST_CHECK_EQ(edit.preedit(), std::string{""});
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{""});  // 取消不落字
}

// ----------------------------------------------------------------------------
// A2 RTL 切片：书写方向解析、指针命中镜像、段右对齐、选区镜像、光标右缘
// ----------------------------------------------------------------------------

AURORA_TEST_CASE(rtl_direction_api_roundtrip_and_unset_omitted) {
    // 显式 RTL：setter 回读、序列化落 "RTL"、反序列化还原。
    RichTextEdit rtl;
    rtl.set_direction(TextDirection::RTL);
    AURORA_TEST_CHECK_TRUE(rtl.direction().has_value());
    AURORA_TEST_CHECK_TRUE(*rtl.direction() == TextDirection::RTL);

    Json props;
    rtl.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["direction"].get<std::string>(), std::string{"RTL"});

    RichTextEdit back;
    back.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(back.direction().has_value());
    AURORA_TEST_CHECK_TRUE(*back.direction() == TextDirection::RTL);

    // 继承语义（未显式设置）不落盘——与 Text/TextInput 一致。
    RichTextEdit inherit;
    AURORA_TEST_CHECK_FALSE(inherit.direction().has_value());
    Json plain;
    inherit.serialize_props(plain);
    AURORA_TEST_CHECK_FALSE(plain.contains("direction"));
}

AURORA_TEST_CASE(rtl_pointer_hit_test_flips_run_order) {
    // RTL 段落的核心 RichTextEdit 逻辑：跨 run 顺序翻转——逻辑首 run 落视觉右缘、逻辑尾 run 落视觉左缘
    // （run 内仍按各自内容脚本整形，A2 设计）。用两色 "a"+"b" 构造两 run 验证：
    //   LTR：run[0]="a"(begin 0) 在左、run[1]="b"(begin 1) 在右 → 点左缘 caret 0、点右缘 caret 1。
    //   RTL：run[0]="a"(begin 0) 在右、run[1]="b"(begin 1) 在左 → 点左缘 caret 1、点右缘 caret 0。
    // 拉丁字形在 headless 可用，且短 run 不会溢出 300px 约束，故不依赖阿拉伯字体整形。
    auto click = [&](RichTextEdit &e, float x) -> void {
        MouseEvent ev;
        ev.action = MouseAction::Press;
        ev.button = MouseButton::Left;
        ev.local_position = Point{.x = x, .y = 0.0F};
        e.on_pointer_event(ev);
    };
    const std::vector<TextSpan> spans{
        TextSpan{.text = LocalizedString{"a"}, .color = Color::red()},
        TextSpan{.text = LocalizedString{"b"}, .color = Color::blue()},
    };

    RichTextEdit ltr;
    ltr.load_spans(spans);
    LayoutEngine::layout(ltr, bounded(300.0F, 300.0F));
    click(ltr, 0.0F);  // 视觉左缘 → run[0]="a" 左缘 → caret 0
    AURORA_TEST_CHECK_EQ(ltr.caret(), 0U);
    click(ltr, ltr.size().width - 0.5F);  // 视觉右缘 → run[1]="b" 右缘 → caret 1
    AURORA_TEST_CHECK_EQ(ltr.caret(), 1U);

    RichTextEdit rtl;
    rtl.load_spans(spans);
    rtl.set_direction(TextDirection::RTL);
    LayoutEngine::layout(rtl, bounded(300.0F, 300.0F));
    click(rtl, rtl.size().width - 0.5F);  // 视觉右缘 → run[0]="a" 右缘 → caret 0
    AURORA_TEST_CHECK_EQ(rtl.caret(), 0U);
    click(rtl, 0.0F);  // 视觉左缘 → run[1]="b" 左缘 → caret 1
    AURORA_TEST_CHECK_EQ(rtl.caret(), 1U);
}

AURORA_TEST_CASE(rtl_block_right_aligns_within_line) {
    // 段落基准方向为 RTL 时，整段右对齐：最左墨迹显著靠右。用相对断言吸收字体 hinting 抖动。
    auto leftmost_ink = [&](bool rtl) -> float {
        RichTextEdit t;
        t.load_spans({TextSpan{.text = LocalizedString{"MMMM MMMM"}}});
        if (rtl) {
            t.set_direction(TextDirection::RTL);
        }
        LayoutEngine::layout(t, bounded(300.0F, 300.0F));
        Painter p;
        p.begin(308, static_cast<int>(t.size().height) + 4);
        const BuildContext ctx;
        t.paint(p, Rect{.origin = Point{.x = 2.0F, .y = 2.0F}, .size = t.size()}, ctx);
        const int y0 = 2 + static_cast<int>(t.size().height / 2.0F);
        for (int x = 0; x < 308; ++x) {
            if (p.get_pixel(x, y0) != Color{0, 0, 0, 0}) {
                return static_cast<float>(x);
            }
        }
        return -1.0F;
    };

    const float ltr_left = leftmost_ink(false);
    const float rtl_left = leftmost_ink(true);
    AURORA_TEST_CHECK_TRUE(ltr_left > 0.0F);
    AURORA_TEST_CHECK_TRUE(rtl_left > 0.0F);
    AURORA_TEST_CHECK_TRUE(rtl_left > ltr_left + 100.0F);
}

AURORA_TEST_CASE(rtl_selection_highlight_mirrors_to_right) {
    // 选中逻辑首字符（空文档经 load 后 caret 归零；RTL 用 Shift+ArrowLeft、LTR 用 Shift+ArrowRight
    // 均使逻辑前进 +1 → sel [0,1)）。空格无字形墨迹，故选区半透明蓝完整可见，便于像素扫描。
    auto highlight_left = [&](bool rtl) -> float {
        RichTextEdit t;
        t.load_spans({TextSpan{.text = LocalizedString{" "}}});
        if (rtl) {
            t.set_direction(TextDirection::RTL);
        }
        LayoutEngine::layout(t, bounded(300.0F, 300.0F));

        KeyEvent k;
        k.action = KeyAction::Down;
        k.key = static_cast<int>(rtl ? KeyCode::ArrowLeft : KeyCode::ArrowRight);
        k.modifiers = ModifierKey::Shift;
        t.on_key_event(k);
        AURORA_TEST_REQUIRE_TRUE(t.has_selection());

        Painter p;
        p.begin(308, static_cast<int>(t.size().height) + 4);
        const BuildContext ctx;
        t.paint(p, Rect{.origin = Point{.x = 2.0F, .y = 2.0F}, .size = t.size()}, ctx);
        const int y0 = 2 + static_cast<int>(t.size().height / 2.0F);
        float lo = 1e9F;
        for (int x = 0; x < 308; ++x) {
            const Color c = p.get_pixel(x, y0);
            // 选区半透明蓝：蓝通道占优且非透明（区别于黑字与透明背景）。
            if (c.a > 0 && c.b > c.r && c.b > c.g) {
                lo = std::min(lo, static_cast<float>(x));
            }
        }
        return lo;
    };

    const float ltr_lo = highlight_left(false);
    const float rtl_lo = highlight_left(true);
    AURORA_TEST_CHECK_TRUE(ltr_lo < 30.0F);    // LTR 选区贴左缘
    AURORA_TEST_CHECK_TRUE(rtl_lo > 100.0F);   // RTL 选区贴右缘（镜像）
}

AURORA_TEST_CASE(rtl_focused_caret_paints_at_right_edge) {
    // 聚焦 + 逻辑首字符：LTR 光标落左缘，RTL 落右缘（FontEngine::caret_x 在 RTL 下返回逻辑 0
    // 于右缘的视觉偏移，调用方无需再镜像）。空格无字形墨迹，避免与光标像素混淆。
    auto caret_x_pos = [&](bool rtl) -> float {
        RichTextEdit t;
        t.load_spans({TextSpan{.text = LocalizedString{" "}}});
        if (rtl) {
            t.set_direction(TextDirection::RTL);
        }
        LayoutEngine::layout(t, bounded(300.0F, 300.0F));
        t.on_focus_change(true);  // 设置焦点以绘制光标
        AURORA_TEST_REQUIRE_TRUE(t.is_focused());

        Painter p;
        p.begin(308, static_cast<int>(t.size().height) + 4);
        const BuildContext ctx;
        t.paint(p, Rect{.origin = Point{.x = 2.0F, .y = 2.0F}, .size = t.size()}, ctx);
        const int y0 = 2 + static_cast<int>(t.size().height / 2.0F);
        for (int x = 0; x < 308; ++x) {
            const Color c = p.get_pixel(x, y0);
            // 黑色光标像素（空格无墨迹，唯一深色像素）。
            if (c.a > 0 && c.r < 50 && c.g < 50 && c.b < 50) {
                return static_cast<float>(x);
            }
        }
        return -1.0F;
    };

    const float ltr_cx = caret_x_pos(false);
    const float rtl_cx = caret_x_pos(true);
    AURORA_TEST_CHECK_TRUE(ltr_cx >= 0.0F);
    AURORA_TEST_CHECK_TRUE(rtl_cx >= 0.0F);
    AURORA_TEST_CHECK_TRUE(rtl_cx > ltr_cx + 50.0F);  // RTL 光标在右缘
}

}  // namespace aurora::test_cases::utest_rich_text_edit
