/// 测试类型: unit
/// 目标单元: src/aurora/widget/widgets_paint.cpp (Text / Button 绘制分支)
/// 测试说明: 覆盖 Text::on_paint 的对齐(Right/End/Start/Center/Left)、文本底色、Justify 多行逐词均分、
/// 单行 Justify 兜底、装饰线(上划线/下划线/删除线)、选区高亮(多行含非交叠跳行、各对齐 line_off 分支)；
/// Button::paint_border 禁用态(圆角/直角)与启用态。

#include <string>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_widgets_paint {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 在窄盒内对文本做「行内拖拽选区」（按下行首、移动到行内右侧），形成仅落于第 0 行的选区。
auto select_line0(Text &t, float width) -> void {
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 4.0F, .y = 2.0F};
    t.on_pointer_event(press);
    MouseEvent move;
    move.action = MouseAction::Move;
    move.local_position = Point{.x = width - 6.0F, .y = 2.0F};
    t.on_pointer_event(move);
}

}  // namespace

AURORA_TEST_CASE(text_paint_alignment_and_selection) {
    BuildContext ctx;
    Painter p;
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 160.0F, .height = 120.0F}};
    const float width = 160.0F;

    // 各对齐均建立行内选区（覆盖对齐绘制 switch + 选区高亮 line_off switch + 非交叠跳行）。
    for (const TextAlign align :
         {TextAlign::Left, TextAlign::Right, TextAlign::Center, TextAlign::End, TextAlign::Start}) {
        Text t{"the quick brown fox jumps over the lazy dog and then some more text"};
        t.set_align(align);
        LayoutEngine::layout(t, bounded(width, 120.0F));
        select_line0(t, width);
        AURORA_TEST_CHECK_TRUE(t.has_selection());
        t.paint(p, box, ctx);
    }

    // RTL 下 End/Start 取反向偏移（覆盖对齐 switch 的 rtl 分支）。
    Text end_rtl{"RTL text content here"};
    end_rtl.set_align(TextAlign::End);
    end_rtl.set_direction(TextDirection::RTL);
    LayoutEngine::layout(end_rtl, bounded(width, 120.0F));
    select_line0(end_rtl, width);
    end_rtl.paint(p, box, ctx);

    Text start_rtl{"RTL text content here"};
    start_rtl.set_align(TextAlign::Start);
    start_rtl.set_direction(TextDirection::RTL);
    LayoutEngine::layout(start_rtl, bounded(width, 120.0F));
    select_line0(start_rtl, width);
    start_rtl.paint(p, box, ctx);
}

AURORA_TEST_CASE(text_paint_background_color) {
    BuildContext ctx;
    Painter p;
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 60.0F}};
    Text t{"highlighted"};
    t.set_background_color(Color{255, 255, 0, 255});  // alpha > 0 → 覆盖底色分支
    LayoutEngine::layout(t, bounded(200.0F, 60.0F));
    t.paint(p, box, ctx);
}

AURORA_TEST_CASE(text_paint_justify_multiline) {
    BuildContext ctx;
    Painter p;
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 120.0F}};
    Text t{"one two three four five six seven eight nine ten"};
    t.set_align(TextAlign::Justify);  // 多行非末行 → 逐词均分
    LayoutEngine::layout(t, bounded(200.0F, 120.0F));
    t.paint(p, box, ctx);
}

AURORA_TEST_CASE(text_paint_justify_single_word_fallback) {
    BuildContext ctx;
    Painter p;
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 60.0F, .height = 120.0F}};
    // 无空格长串在窄盒内折成多行，每行仅 1 个「词」→ 走 justify 单行兜底分支。
    Text t{"supercalifragilisticexpialidociouslylongtokenwithoutspaces"};
    t.set_align(TextAlign::Justify);
    LayoutEngine::layout(t, bounded(60.0F, 120.0F));
    t.paint(p, box, ctx);
}

AURORA_TEST_CASE(text_paint_decoration_lines) {
    BuildContext ctx;
    Painter p;
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 60.0F}};
    Text t{"decorated"};
    t.set_decoration(TextDecoration::Underline | TextDecoration::Overline | TextDecoration::LineThrough);
    LayoutEngine::layout(t, bounded(200.0F, 60.0F));
    t.paint(p, box, ctx);  // 覆盖 decoration != None + 上/下/删除线三分支
}

AURORA_TEST_CASE(button_paint_border_branches) {
    BuildContext ctx;
    Painter p;
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 120.0F, .height = 40.0F}};

    // 圆角 + 禁用 + 边框色：禁用态淡化(with_alpha) + 圆角描边。
    Button round;
    round.set_label("X");
    round.set_corner_radius(8.0F);
    round.set_enabled(false);
    round.set_border(Color{10, 20, 30, 255}, 2.0F);
    round.paint(p, box, ctx);  // 222(with_alpha) + 223-224

    // 直角 + 禁用：走 draw_rect 兜底。
    Button square;
    square.set_label("Y");
    square.set_corner_radius(0.0F);
    square.set_enabled(false);
    square.set_border(Color{10, 20, 30, 255}, 2.0F);
    square.paint(p, box, ctx);  // 222 + 226

    // 启用态：边框不淡化。
    Button enabled;
    enabled.set_label("Z");
    enabled.set_corner_radius(8.0F);
    enabled.set_enabled(true);
    enabled.set_border(Color{10, 20, 30, 255}, 2.0F);
    enabled.paint(p, box, ctx);  // 222(无 with_alpha) + 223-224
}

}  // namespace aurora::test_cases::utest_widgets_paint
