/// 测试类型: unit
/// 目标单元: include/aurora/widget/rich_text_edit.h
/// 测试说明: 覆盖 RichTextEdit——初始不变量、load_spans/to_spans 往返与相邻同样式合并、
/// 当前输入样式链式 setter 与 toggle、序列化纯文本往返（字符取当前样式）、
/// 布局高度按行计数与自描述元数据、IME 组合输入（preedit 显示 / 上屏落字 / 失焦取消）。
/// RTL 切片：书写方向 API 往返与不落盘、指针命中逻辑↔视觉镜像、RTL 段右对齐像素、
/// 选区高亮镜像、聚焦光标落右缘像素

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/rich_text_edit.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_rich_text_edit {
using aurora::testing::require_field;

using aurora::testing::require_value;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
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
    StateSink out;
    edit.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.sources().size(), 0U);
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
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "text"), "Hi");

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
    commit.committed = "你好";  // CJK-LITERAL: cjk-fixture - the IME commit text must be Han
    edit.on_text_composition(commit);
    AURORA_TEST_CHECK_FALSE(edit.is_composing());
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"你好"});  // CJK-LITERAL: cjk-fixture - same Han commit

    // 序列化只反映已上屏文本，不含 preedit
    TextCompositionEvent pending;
    pending.preedit = "zai";
    edit.on_text_composition(pending);
    Json out = Json::object();
    edit.serialize_props(out);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(out, "text"),
                         "你好");  // CJK-LITERAL: cjk-fixture - Han text serialized
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
// RTL 切片：书写方向解析、指针命中镜像、段右对齐、选区镜像、光标右缘
// ----------------------------------------------------------------------------

AURORA_TEST_CASE(rtl_direction_api_roundtrip_and_unset_omitted) {
    // 显式 RTL：setter 回读、序列化落 "RTL"、反序列化还原。
    RichTextEdit rtl;
    rtl.set_direction(TextDirection::RTL);
    AURORA_TEST_CHECK_TRUE(rtl.direction().has_value());
    AURORA_TEST_CHECK_TRUE(require_value(rtl.direction()) == TextDirection::RTL);

    Json props = Json::object();
    rtl.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "direction"), std::string{"RTL"});

    RichTextEdit back;
    back.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(back.direction().has_value());
    AURORA_TEST_CHECK_TRUE(require_value(back.direction()) == TextDirection::RTL);

    // 继承语义（未显式设置）不落盘——与 Text/TextInput 一致。
    RichTextEdit inherit;
    AURORA_TEST_CHECK_FALSE(inherit.direction().has_value());
    Json plain = Json::object();
    inherit.serialize_props(plain);
    AURORA_TEST_CHECK_FALSE(plain.contains("direction"));
}

AURORA_TEST_CASE(rtl_pointer_hit_test_same_level_runs_keep_logical_order) {
    // 完整 UBA（UAX #9）语义：RTL 段内两色拉丁 run "a"+"b" 同属嵌入层 2（连续同层 span），
    // L2 层叠反转在段落层与 span 层各反转一次而相互抵消 → span 内保持逻辑序，显示 "ab"
    // （a 左 b 右）。点击映射因此与 LTR 相同（旧 UBA-lite 的「整体翻转」会把同层 run
    // 错误地互换为 "ba"，已被完整 UBA 修正）。跨层 run 的翻转见 utest_bidi 的
    // arabic_richtextedit_cross_level_runs_reorder（阿文 run 层 1 vs 拉丁 run 层 2）。
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
    // 同层 span 逻辑序保持：RTL 段右对齐但不改变 run 间逻辑序 → 点击映射与 LTR 一致。
    click(rtl, 0.0F);  // 视觉左缘 → run[0]="a" 左缘 → caret 0
    AURORA_TEST_CHECK_EQ(rtl.caret(), 0U);
    click(rtl, rtl.size().width - 0.5F);  // 视觉右缘 → run[1]="b" 右缘 → caret 1
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
        constexpr BuildContext ctx;
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
        constexpr BuildContext ctx;
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
    AURORA_TEST_CHECK_TRUE(ltr_lo < 30.0F);  // LTR 选区贴左缘
    AURORA_TEST_CHECK_TRUE(rtl_lo > 100.0F);  // RTL 选区贴右缘（镜像）
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
        constexpr BuildContext ctx;
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

AURORA_TEST_CASE(accessibility_hooks_and_text_pattern) {
    // 无障碍语义组：state(multiline) / text / selection / set_selection / replace_text /
    // perform_action(Value) / cursor_shape / wants_*_keys / on_text_input。
    RichTextEdit edit;
    edit.load_spans({TextSpan{.text = LocalizedString{"hello"}, .font = Font{.size_pt = 14.0F}}});

    // 多行语义：multilinie 恒为 true（UIA 侧映射 Document）。
    const auto state = edit.accessibility_state();
    AURORA_TEST_CHECK_TRUE(state.multiline);

    // 全文视图（缓存中转）与纯文本一致。
    AURORA_TEST_CHECK_EQ(std::string{edit.accessibility_text()}, std::string{"hello"});

    // 选区视图：初始为零宽区间（光标态），恒有值。
    const auto sel = require_value(edit.accessibility_selection());
    AURORA_TEST_CHECK_EQ(sel.start, 0U);
    AURORA_TEST_CHECK_EQ(sel.end, 0U);

    // set_selection：设置后光标置于区间终点。
    edit.accessibility_set_selection(1, 4);
    AURORA_TEST_CHECK_TRUE(edit.has_selection());
    AURORA_TEST_CHECK_EQ(edit.selected_text(), std::string{"ell"});
    const auto sel2 = require_value(edit.accessibility_selection());
    AURORA_TEST_CHECK_EQ(sel2.start, 1U);
    AURORA_TEST_CHECK_EQ(sel2.end, 4U);
    AURORA_TEST_CHECK_EQ(edit.caret(), 4U);
    // 越界夹紧到文档长度。
    edit.accessibility_set_selection(0, 999);
    AURORA_TEST_CHECK_EQ(require_value(edit.accessibility_selection()).end, 5U);

    // replace_text：区间替换后按当前输入样式重建，光标落插入串尾。
    edit.set_current_color(Color::red());
    edit.accessibility_replace_text(1, 3, "EY");
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"hEYlo"});
    const auto spans = edit.to_spans();
    AURORA_TEST_REQUIRE_EQ(spans.size(), 1U);  // 重建走单 span
    AURORA_TEST_CHECK_TRUE(spans[0].color == Color::red());
    AURORA_TEST_CHECK_EQ(edit.caret(), 3U);  // a + utf8.size() = 1 + 2
    AURORA_TEST_CHECK_FALSE(edit.has_selection());

    // perform_accessibility_action：Value 整值替换；其余动作回落基类。
    AccessibilityActionRequest value_req;
    value_req.action = AccessibilityAction::Value;
    value_req.text = "replaced";
    AURORA_TEST_CHECK_TRUE(edit.perform_accessibility_action(value_req));
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"replaced"});
    AccessibilityActionRequest toggle_req;
    toggle_req.action = AccessibilityAction::Toggle;
    AURORA_TEST_CHECK_FALSE(edit.perform_accessibility_action(toggle_req));  // 非编辑动作不认领

    // 组合态 accessibility_value：preedit 插在光标处播报。
    edit.on_focus_change(true);
    TextCompositionEvent typing;
    typing.preedit = "XY";
    edit.on_text_composition(typing);
    AURORA_TEST_CHECK_EQ(edit.accessibility_value(), std::string{"replacedXY"});

    // 游标语义常量。
    AURORA_TEST_CHECK_TRUE(require_value(edit.cursor_shape()) == CursorShape::IBeam);
    AURORA_TEST_CHECK_TRUE(edit.wants_activation_keys());
    AURORA_TEST_CHECK_TRUE(edit.wants_navigation_keys());

    // on_text_input：非空文本走 do_insert 并消费；空文本不消费。
    RichTextEdit ins;
    ins.on_focus_change(true);
    TextInputEvent seed;
    seed.text = "abc";
    ins.on_text_input(seed);
    AURORA_TEST_CHECK_TRUE(seed.is_handled);
    AURORA_TEST_CHECK_EQ(ins.plain_text(), std::string{"abc"});
    TextInputEvent blank;
    blank.text = "";
    ins.on_text_input(blank);
    AURORA_TEST_CHECK_FALSE(blank.is_handled);
}

AURORA_TEST_CASE(preedit_inverted_selection_degenerates_to_point) {
    // preedit 选区端点倒置（sel_end < sel_start）→ 收敛为单点。
    RichTextEdit edit;
    edit.on_focus_change(true);
    TextCompositionEvent typing;
    typing.preedit = "abcd";
    typing.cursor_index = 2;
    typing.sel_start = 3;
    typing.sel_end = 1;
    edit.on_text_composition(typing);
    AURORA_TEST_CHECK_TRUE(typing.is_handled);
    AURORA_TEST_CHECK_EQ(edit.preedit(), std::string{"abcd"});
    AURORA_TEST_CHECK_EQ(edit.composition_cursor(), 2U);
    // sel_start/sel_end 无公开 getter，经绘制路径间接覆盖（组合态 paint 走 preedit 选区分支）。
    Painter p;
    p.begin(200, 80);
    edit.layout(bounded(200.0F, 80.0F), BuildContext{});
    edit.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 80.0F}},
               BuildContext{});
    edit.on_focus_change(false);  // 收尾：取消组合
    AURORA_TEST_CHECK_FALSE(edit.is_composing());
}

AURORA_TEST_CASE(keyboard_shortcuts_style_undo_and_select_all) {
    RichTextEdit edit;
    UndoStack stack;
    edit.set_undo_stack(&stack);
    edit.on_focus_change(true);

    auto press = [](KeyCode code, ModifierKey mods) -> KeyEvent {
        KeyEvent k;
        k.key = static_cast<int>(code);
        k.action = KeyAction::Down;
        k.modifiers = mods;
        return k;
    };

    // 非 Down 动作直接放行。
    KeyEvent up = press(KeyCode::A, ModifierKey::None);
    up.action = KeyAction::Up;
    edit.on_key_event(up);
    AURORA_TEST_CHECK_FALSE(up.is_handled);

    TextInputEvent seed;
    seed.text = "abc";
    edit.on_text_input(seed);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"abc"});

    // Ctrl+B/I/U 样式切换（二次切换回退）。
    KeyEvent b = press(KeyCode::B, ModifierKey::Control);
    edit.on_key_event(b);
    AURORA_TEST_CHECK_TRUE(b.is_handled);
    AURORA_TEST_CHECK_TRUE(edit.current_font().weight >= 700);
    KeyEvent b2 = press(KeyCode::B, ModifierKey::Control);
    edit.on_key_event(b2);
    AURORA_TEST_CHECK_TRUE(edit.current_font().weight < 700);
    KeyEvent i = press(KeyCode::I, ModifierKey::Control);
    edit.on_key_event(i);
    AURORA_TEST_CHECK_TRUE(edit.current_font().family.size() >= 2 &&
                           edit.current_font().family.substr(edit.current_font().family.size() - 2) == "/I");
    KeyEvent i2 = press(KeyCode::I, ModifierKey::Control);
    edit.on_key_event(i2);
    AURORA_TEST_CHECK_FALSE(edit.current_font().family.size() >= 2 &&
                            edit.current_font().family.substr(edit.current_font().family.size() - 2) == "/I");
    KeyEvent u = press(KeyCode::U, ModifierKey::Control);
    edit.on_key_event(u);
    AURORA_TEST_CHECK_TRUE(edit.current_underline());
    KeyEvent u2 = press(KeyCode::U, ModifierKey::Control);
    edit.on_key_event(u2);
    AURORA_TEST_CHECK_FALSE(edit.current_underline());

    // Ctrl+A 全选；Ctrl+Z 撤销删除插入；Ctrl+Shift+Z 重做恢复。
    KeyEvent a = press(KeyCode::A, ModifierKey::Control);
    edit.on_key_event(a);
    AURORA_TEST_CHECK_TRUE(edit.has_selection());
    AURORA_TEST_CHECK_EQ(edit.selected_text(), std::string{"abc"});
    AURORA_TEST_CHECK_EQ(edit.caret(), 3U);

    KeyEvent z = press(KeyCode::Z, ModifierKey::Control);
    edit.on_key_event(z);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{""});
    AURORA_TEST_CHECK_EQ(edit.caret(), 0U);
    KeyEvent rz = press(KeyCode::Z, ModifierKey::Control | ModifierKey::Shift);
    edit.on_key_event(rz);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"abc"});
    KeyEvent y = press(KeyCode::Y, ModifierKey::Control);
    edit.on_key_event(y);
    // 无可重做时 Y 仍消费但不改文档。
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"abc"});

    // 未识别的 Ctrl 组合不认领。
    KeyEvent q = press(KeyCode::Q, ModifierKey::Control);
    edit.on_key_event(q);
    AURORA_TEST_CHECK_FALSE(q.is_handled);
}

AURORA_TEST_CASE(move_keys_text_keys_and_undo_roundtrip) {
    RichTextEdit edit;
    UndoStack stack;
    edit.set_undo_stack(&stack);
    edit.on_focus_change(true);

    auto press = [](KeyCode code, ModifierKey mods) -> KeyEvent {
        KeyEvent k;
        k.key = static_cast<int>(code);
        k.action = KeyAction::Down;
        k.modifiers = mods;
        return k;
    };

    TextInputEvent seed;
    seed.text = "hello";
    edit.on_text_input(seed);

    // End/Home 与 Shift 扩选。
    KeyEvent end_k = press(KeyCode::End, ModifierKey::None);
    edit.on_key_event(end_k);
    AURORA_TEST_CHECK_EQ(edit.caret(), 5U);
    KeyEvent home_k = press(KeyCode::Home, ModifierKey::None);
    edit.on_key_event(home_k);
    AURORA_TEST_CHECK_EQ(edit.caret(), 0U);
    KeyEvent sh_end = press(KeyCode::End, ModifierKey::Shift);
    edit.on_key_event(sh_end);
    AURORA_TEST_CHECK_TRUE(edit.has_selection());
    AURORA_TEST_CHECK_EQ(edit.selected_text(), std::string{"hello"});
    // 带选区时无 Shift 方向键塌缩：← 塌到选区起点（min）。
    KeyEvent left = press(KeyCode::ArrowLeft, ModifierKey::None);
    edit.on_key_event(left);
    AURORA_TEST_CHECK_FALSE(edit.has_selection());
    AURORA_TEST_CHECK_EQ(edit.caret(), 0U);
    KeyEvent left2 = press(KeyCode::ArrowLeft, ModifierKey::None);
    edit.on_key_event(left2);
    AURORA_TEST_CHECK_EQ(edit.caret(), 0U);  // 已在文首，钳制不动
    KeyEvent right_in = press(KeyCode::ArrowRight, ModifierKey::None);
    edit.on_key_event(right_in);
    AURORA_TEST_CHECK_EQ(edit.caret(), 1U);
    // Shift+→ 扩选（无既有选区时建立锚点）。
    KeyEvent sh_right = press(KeyCode::ArrowRight, ModifierKey::Shift);
    edit.on_key_event(sh_right);
    AURORA_TEST_CHECK_EQ(edit.selected_text(), std::string{"e"});
    // ← 塌缩回选区起点。
    KeyEvent left3 = press(KeyCode::ArrowLeft, ModifierKey::None);
    edit.on_key_event(left3);
    AURORA_TEST_CHECK_EQ(edit.caret(), 1U);
    KeyEvent end_k2 = press(KeyCode::End, ModifierKey::None);
    edit.on_key_event(end_k2);
    AURORA_TEST_CHECK_EQ(edit.caret(), 5U);

    // Delete：caret < n 时删光标后字符；caret == n 时消费但无变化。
    KeyEvent del_end = press(KeyCode::Delete, ModifierKey::None);
    edit.on_key_event(del_end);
    AURORA_TEST_CHECK_TRUE(del_end.is_handled);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"hello"});
    KeyEvent left4 = press(KeyCode::ArrowLeft, ModifierKey::None);
    edit.on_key_event(left4);
    KeyEvent del = press(KeyCode::Delete, ModifierKey::None);
    edit.on_key_event(del);
    AURORA_TEST_CHECK_TRUE(del.is_handled);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"hell"});
    KeyEvent del2 = press(KeyCode::Delete, ModifierKey::None);
    edit.on_key_event(del2);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"hell"});

    // Backspace：删光标前字符并记撤销。
    KeyEvent bs = press(KeyCode::Backspace, ModifierKey::None);
    edit.on_key_event(bs);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"hel"});
    KeyEvent z = press(KeyCode::Z, ModifierKey::Control);
    edit.on_key_event(z);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"hell"});

    // Enter 插入换行。
    KeyEvent enter = press(KeyCode::Enter, ModifierKey::None);
    edit.on_key_event(enter);
    AURORA_TEST_CHECK_EQ(edit.plain_text(), std::string{"hell\n"});

    // 非编辑键不消费。
    KeyEvent tab = press(KeyCode::Tab, ModifierKey::None);
    edit.on_key_event(tab);
    AURORA_TEST_CHECK_FALSE(tab.is_handled);
}

AURORA_TEST_CASE(pointer_click_positions_caret) {
    RichTextEdit edit;
    edit.load_spans({TextSpan{.text = LocalizedString{"ab\ncd"}, .font = Font{.size_pt = 14.0F}}});
    edit.layout(bounded(200.0F, 100.0F), BuildContext{});

    auto click = [&edit](float x, float y) {
        MouseEvent e;
        e.action = MouseAction::Press;
        e.button = MouseButton::Left;
        e.local_position = Point{.x = x, .y = y};
        edit.on_pointer_event(e);
        return e;
    };

    // 第一行点击：消费并把光标收敛到命中字符。
    MouseEvent e0 = click(10.0F, 5.0F);
    AURORA_TEST_CHECK_TRUE(e0.is_handled);
    AURORA_TEST_CHECK_FALSE(edit.has_selection());
    AURORA_TEST_CHECK_TRUE(edit.caret() <= 2U);  // 第一行内
    // 第二行点击（跨行下标累加路径；y 取足够大以落入第二行或行尾兜底）。
    MouseEvent e1 = click(30.0F, 45.0F);
    AURORA_TEST_CHECK_TRUE(e1.is_handled);
    AURORA_TEST_CHECK_TRUE(edit.caret() >= 3U);
    // 点击超出行数范围 → 光标落文档尾。
    MouseEvent e2 = click(30.0F, 90.0F);
    AURORA_TEST_CHECK_TRUE(e2.is_handled);
    AURORA_TEST_CHECK_EQ(edit.caret(), 5U);

    // 未布局（无行）时按下：消费并直接返回。
    RichTextEdit fresh;
    MouseEvent e3;
    e3.action = MouseAction::Press;
    e3.button = MouseButton::Left;
    e3.local_position = Point{.x = 5.0F, .y = 5.0F};
    fresh.on_pointer_event(e3);
    AURORA_TEST_CHECK_TRUE(e3.is_handled);

    // 非「左键按下」不处理。
    MouseEvent e4;
    e4.action = MouseAction::Move;
    e4.button = MouseButton::Left;
    e4.local_position = Point{.x = 5.0F, .y = 5.0F};
    edit.on_pointer_event(e4);
    AURORA_TEST_CHECK_FALSE(e4.is_handled);
}

AURORA_TEST_CASE(paint_selection_underline_preedit_and_cursor) {
    auto paint = [](Widget &w, float wpx, float hpx) {
        Painter p;
        p.begin(static_cast<int>(wpx), static_cast<int>(hpx));
        w.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = wpx, .height = hpx}},
                BuildContext{});
    };

    // 空文档直接绘制：懒重排路径。
    RichTextEdit empty;
    empty.on_focus_change(true);
    paint(empty, 120.0F, 40.0F);

    // 带下划线的混合样式文档（逐字下划线绘制路径）。
    RichTextEdit edit;
    UndoStack stack;
    edit.set_undo_stack(&stack);
    edit.on_focus_change(true);
    edit.set_current_underline(true);
    TextInputEvent part1;
    part1.text = "ab";
    edit.on_text_input(part1);
    edit.set_current_underline(false);
    TextInputEvent part2;
    part2.text = "cd";
    edit.on_text_input(part2);

    // 全选后绘制：选区高亮路径。
    KeyEvent a;
    a.key = static_cast<int>(KeyCode::A);
    a.modifiers = ModifierKey::Control;
    edit.on_key_event(a);
    paint(edit, 120.0F, 40.0F);

    // 非组合聚焦态绘制：文档光标路径。
    KeyEvent right;
    right.key = static_cast<int>(KeyCode::ArrowRight);
    edit.on_key_event(right);
    paint(edit, 120.0F, 40.0F);

    // 组合态绘制：preedit 内选区 + 下划线 + 候选插入点光标（聚焦）。
    TextCompositionEvent typing;
    typing.preedit = "ni";
    typing.cursor_index = 1;
    typing.sel_start = 0;
    typing.sel_end = 0;
    edit.on_text_composition(typing);
    paint(edit, 120.0F, 40.0F);

    // composition_caret_bounds：组合 + preedit_cursor_ > 0 → 候选窗 = 文档光标 + preedit 前缀宽。
    (void)edit.caret();  // 触发惰性断言无副作用；仅保持调用形态
    const Rect bounds = edit.composition_caret_bounds();
    AURORA_TEST_CHECK_TRUE(bounds.size.width >= 0.0F);
    AURORA_TEST_CHECK_TRUE(bounds.size.height >= 0.0F);

    // 从未绘制过的新控件（focus_bounds 为零盒）：caret_box_at 返回空 → 回退基类控件盒。
    RichTextEdit bare;
    TextCompositionEvent pend;
    pend.preedit = "x";
    bare.on_text_composition(pend);
    const Rect fb = bare.composition_caret_bounds();
    AURORA_TEST_CHECK_EQ(fb.size.width, 0.0F);

    // 布局：字号 0 测高异常回退路径（能产出有效行高即可）。
    RichTextEdit tiny;
    tiny.set_current_font(Font{.size_pt = 0.0F});
    const Size sz = tiny.layout(bounded(100.0F, 100.0F), BuildContext{});
    AURORA_TEST_CHECK_TRUE(sz.height >= 1.0F);
}

AURORA_TEST_CASE(rich_text_edit_color_follows_theme_when_unset) {
    // 未显式设置当前输入色时，resolve_text_color 经 ctx 取主题 on_surface：
    // 裸 ctx 回落 light（黑），注入 dark 主题翻面为白。
    constexpr BuildContext bare;
    const RichTextEdit edit;
    AURORA_TEST_CHECK_FALSE(edit.current_color().has_value());
    AURORA_TEST_CHECK(edit.resolve_text_color(bare) == Color{0, 0, 0});  // light on_surface = 黑

    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(edit.resolve_text_color(dark) == Color{0xFF, 0xFF, 0xFF});  // dark on_surface = 白
}

AURORA_TEST_CASE(rich_text_edit_explicit_color_overrides_theme) {
    // 显式设置当前输入色后，resolve_text_color 忽略主题（即便注入 dark）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    RichTextEdit edit;
    edit.set_current_color(Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK(edit.resolve_text_color(dark) == Color{0xE0, 0x1B, 0x24});
}

AURORA_TEST_CASE(inline_home_end_move_within_current_line) {
    // 多行文档：L0 "abc"[0,3) | L1 "def"[4,7) | L2 "ghij"[8,12)。
    // 行内 Home/End 跳到当前行首/行尾；Ctrl+Home/Ctrl+End 跳到文档头/尾。
    RichTextEdit edit;
    edit.load_spans({TextSpan{.text = LocalizedString{"abc\ndef\nghij"}}});
    edit.layout(bounded(200.0F, 100.0F), BuildContext{});  // 触发 on_layout 以填充 lines_
    AURORA_TEST_CHECK_EQ(edit.caret(), 0U);

    auto press = [&](KeyCode k, bool ctrl = false, bool shift = false) {
        KeyEvent e;
        e.action = KeyAction::Down;
        e.key = static_cast<int>(k);
        unsigned m = 0U;
        if (ctrl) {
            m |= static_cast<unsigned>(ModifierKey::Control);
        }
        if (shift) {
            m |= static_cast<unsigned>(ModifierKey::Shift);
        }
        e.modifiers = static_cast<ModifierKey>(m);
        edit.on_key_event(e);
    };

    // 移到 L1 的 'e' 之后（doc 下标 5）：5 次右移。
    for (int i = 0; i < 5; ++i) {
        press(KeyCode::ArrowRight);
    }
    AURORA_TEST_CHECK_EQ(edit.caret(), 5U);

    // 行内 Home（无修饰）：跳到当前行首 doc 下标 4。
    press(KeyCode::Home);
    AURORA_TEST_CHECK_EQ(edit.caret(), 4U);

    // 行内 End（无修饰）：跳回当前行尾 doc 下标 7。
    press(KeyCode::End);
    AURORA_TEST_CHECK_EQ(edit.caret(), 7U);

    // Ctrl+Home：跳到文档头（下标 0）。
    press(KeyCode::Home, /*ctrl=*/true);
    AURORA_TEST_CHECK_EQ(edit.caret(), 0U);

    // Ctrl+End：跳到文档尾（下标 12 = 文档长度）。
    press(KeyCode::End, /*ctrl=*/true);
    AURORA_TEST_CHECK_EQ(edit.caret(), 12U);

    // 选区：Shift+End 从当前行首扩到当前行尾（行内语义，非整篇）。
    press(KeyCode::Home);  // 回到 L2 行首（下标 8）
    AURORA_TEST_CHECK_EQ(edit.caret(), 8U);
    press(KeyCode::End, false, true);  // Shift+End：选区 8..12
    AURORA_TEST_CHECK_TRUE(edit.has_selection());
    AURORA_TEST_CHECK_EQ(edit.selected_text(), std::string{"ghij"});
}

}  // namespace aurora::test_cases::utest_rich_text_edit
