/// 测试类型: unit
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: utest_text_selection 单元测试
///
// 目标源单元（历史映射，保留供审计）: Text + Text
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// TextAaMode(TextAAMode，经 AA 各段行使)、BitmapFont(BitmapFont 内置字体回退)、
// TextSpan(TextSpan，经 sec_rich_text? 见 test_rich_text.cpp——TextSpan 归属 rich_text 单元)。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora_test_harness.h"

// （自 utest_text.cpp 拆分：选区高亮/端点含入/缩放屏命中段）

namespace aurora::test_cases::utest_text_selection {

namespace render = aurora::render;
using au::Alignment;
using au::BuildContext;
using au::Button;
using au::Clipboard;
using au::Color;
using au::Column;
using au::ColumnProps;
using au::Constraints;
using au::EventDispatcher;
using au::FocusManager;
using au::Font;
using au::FontStyle;
using au::FontWeight;
using au::Json;
using au::KeyAction;
using au::KeyCode;
using au::KeyEvent;
using au::LocalizedString;
using au::Modifier;
using au::ModifierKey;
using au::MouseAction;
using au::MouseButton;
using au::MouseEvent;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Row;
using au::RowProps;
using au::set_current_focus_manager;
using au::Size;
using au::Text;
using au::TextAlign;
using au::TextDecoration;
using au::TextOverflow;
using au::TextProps;
using au::Widget;
namespace sec_text_selection {

void run() {
    // 选区高亮为半透明蓝色矩形；ClearType 会在字形边缘产生红/蓝彩色羽化，
    // 干扰「按 b-r>30 检测蓝色」的判定。改用与背景无关的超采样抗锯齿，使检测只反映选区高亮。
    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);

    // 1) 绘制无选区的 Text，记录是否有蓝色高亮像素（应当没有）。
    {
        Text txt("Hello Selection");
        BuildContext ctx;  // env=nullptr 即可（用默认 Locale）
        Painter p;
        p.begin(400, 60);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, Color::white());
        constexpr Rect bounds{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}};
        txt.paint(p, bounds, ctx);  // 填充 display_text_

        bool saw_highlight = false;
        for (int y = 0; y < p.height() && !saw_highlight; ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if (static_cast<int>(c.b) - static_cast<int>(c.r) > 30) {
                    saw_highlight = true;  // 蓝色染色 = 高亮
                    break;
                }
            }
        }
        AURORA_TEST_CHECK(!saw_highlight);  // 无选区时不应有高亮
        AURORA_LOG_INFO("test", "[1] no highlight without selection OK");
    }

    // 1b) 只读 Text 点击获焦后不应绘制编辑光标（caret）：
    // 文本字形本身为黑（text_color），caret 亦为黑；故以「获焦 vs 未获焦的墨迹像素数差」
    // 做差分断言——旧实现下获焦会多出 caret 竖条像素，修复后应相等。
    {
        Text txt("readonly text no caret");
        BuildContext ctx;
        Painter base;
        base.begin(400, 60);
        base.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, Color::white());
        txt.paint(base, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                  ctx);  // 未获焦基线

        auto count_ink = [](Painter const &pp) -> int {
            int n = 0;
            for (int y = 0; y < pp.height(); ++y) {
                for (int x = 0; x < pp.width(); ++x) {
                    const Color c = pp.get_pixel(x, y);
                    if (c.r < 40 && c.g < 40 && c.b < 40) {
                        ++n;  // 近黑 = 字形墨迹
                    }
                }
            }
            return n;
        };
        const int ink_unfocused = count_ink(base);

        Painter focused;
        focused.begin(400, 60);
        focused.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                          Color::white());
        txt.paint(focused, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, ctx);
        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.local_position = Point{.x = 50.0F, .y = 0.0F};
        txt.on_pointer_event(press);  // 点击获焦
        focused.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                          Color::white());
        txt.paint(focused, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                  ctx);  // 获焦后重绘
        const int ink_focused = count_ink(focused);

        AURORA_TEST_CHECK(ink_focused == ink_unfocused);  // 获焦不应新增 caret 墨迹
        AURORA_LOG_INFO("test", "[1b] no caret drawn on focus (unfocused=", ink_unfocused, " focused=", ink_focused,
                        ") OK");
    }

    // 2) 制造选区（Press@0 + Move@300）后重绘，选区区域应出现蓝色高亮像素。
    {
        Text txt("Hello Selection");
        BuildContext ctx;
        Painter p;
        p.begin(400, 60);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, Color::white());
        constexpr Rect bounds{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}};
        txt.paint(p, bounds, ctx);  // 先填充 display_text_

        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.local_position = Point{.x = 0.0F, .y = 0.0F};
        txt.on_pointer_event(press);

        MouseEvent move;
        move.action = MouseAction::Move;
        move.button = MouseButton::Left;
        move.local_position = Point{.x = 300.0F, .y = 0.0F};
        txt.on_pointer_event(move);

        AURORA_TEST_CHECK(txt.has_selection());  // 选区已建立
        AURORA_LOG_INFO("test", "[2] selection established OK");

        // 重绘（高亮应在文本之后绘制，不会被文本包围盒填充覆盖）
        txt.paint(p, bounds, ctx);

        int hl_count = 0;
        for (int y = 0; y < p.height(); ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if (static_cast<int>(c.b) - static_cast<int>(c.r) > 30) {
                    ++hl_count;
                }
            }
        }
        AURORA_TEST_CHECK(hl_count > 50);  // 高亮应覆盖相当区域（非单点噪点）
        AURORA_LOG_INFO("test", "[3] selection highlight visible (pixels=", hl_count, ") OK");

        // 4) 失焦应取消选区高亮（点击别处）
        txt.on_focus_change(false);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                    Color::white());  // 清掉旧高亮
        txt.paint(p, bounds, ctx);
        int hl_after_blur = 0;
        for (int y = 0; y < p.height(); ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if (static_cast<int>(c.b) - static_cast<int>(c.r) > 30) {
                    ++hl_after_blur;
                }
            }
        }
        AURORA_TEST_CHECK(!txt.has_selection());
        AURORA_TEST_CHECK(hl_after_blur == 0);  // 高亮已清除
        AURORA_LOG_INFO("test", "[4] highlight cleared on blur OK");
    }

    // 5) 选中某一行 Text，其选区高亮不得渗入下方相邻 Text 的绘制区域
    //    （回归：相邻控件仅余极小间隙时，半透明高亮矩形曾渗入邻行，造成假选中）。
    {
        auto line_a = std::make_shared<Text>("AAAA line one selected fully here");
        auto line_b = std::make_shared<Text>("BBBB line two must stay unselected");
        Column col{au::Node{line_a}, Node{line_b}};
        BuildContext lctx;
        Constraints lc;
        lc.min = Size{.width = 0, .height = 0};
        lc.max = Size{.width = 400, .height = 200};
        col.layout(lc, lctx);

        // 先绘制一次，填充两个 Text 的 display_text_
        Painter warm;
        warm.begin(400, 200);
        warm.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}},
                       Color::white());
        col.paint(warm, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}}, lctx);

        // 定位 lineB 的像素盒
        Rect r_b{.origin = Point{.x = 1e9F, .y = 1e9F}, .size = Size{.width = -1e9F, .height = -1e9F}};
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 400; ++x) {
                Widget *h =
                    EventDispatcher::hit_test(col, Point{.x = static_cast<float>(x), .y = static_cast<float>(y)});
                auto *t = dynamic_cast<Text *>(h);
                if (t != nullptr && t->display_text().find("BBBB") != std::string::npos) {
                    r_b.origin.x = std::min(r_b.origin.x, static_cast<float>(x));
                    r_b.origin.y = std::min(r_b.origin.y, static_cast<float>(y));
                    r_b.size.width = std::max(r_b.size.width, static_cast<float>(x) - r_b.origin.x);
                    r_b.size.height = std::max(r_b.size.height, static_cast<float>(y) - r_b.origin.y);
                }
            }
        }
        AURORA_TEST_CHECK(r_b.size.width > 0 && r_b.size.height > 0);  // lineB 确实被布局出来

        FocusManager fm;
        fm.set_root(&col);
        auto press = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Press;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };
        auto move = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Move;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };
        auto release = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Release;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };

        // 选满 lineA：右端按下 -> 拖到左端
        const float y_a = r_b.origin.y - 10.0F;  // lineA 在 lineB 上方
        press(360.0F, y_a);
        move(4.0F, y_a);
        release(4.0F, y_a);

        Painter p2;
        p2.begin(400, 200);
        p2.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}}, Color::white());
        col.paint(p2, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}}, lctx);

        int blue_in_b = 0;
        for (int yy = static_cast<int>(r_b.origin.y); yy < static_cast<int>(r_b.origin.y + r_b.size.height); ++yy) {
            for (int xx = static_cast<int>(r_b.origin.x); xx < static_cast<int>(r_b.origin.x + r_b.size.width); ++xx) {
                const Color c = p2.get_pixel(xx, yy);
                if (static_cast<int>(c.b) - static_cast<int>(c.r) > 30) {
                    ++blue_in_b;
                }
            }
        }
        AURORA_TEST_CHECK(blue_in_b == 0);  // lineA 的高亮不得渗入 lineB
        AURORA_TEST_CHECK(!line_b->has_selection());  // lineB 自身也未被选中
        AURORA_LOG_INFO("test", "[5] selection highlight does not bleed into neighbor (blue_in_B=", blue_in_b, ") OK");
    }

    // 6) 默认 ClearType 渲染模式下（与真实 Win32 app 一致），拖选上一行不得让相邻行
    //    既被「真实选中」也不得被「视觉染色」。ClearType 文本边缘自带彩色羽化，故以
    //    「选中前后邻行蓝像素数不变」作为染色判据，而非简单断言为 0。
    {
        // 显式使用默认 ClearType，避免受其他用例改过的 AA 模式影响，贴合真实 app。
        render::FontEngine::set_text_aa_mode(render::TextAAMode::ClearType);

        auto line1 = std::make_shared<Text>("curve@0.5 = 0.500000");
        auto line2 = std::make_shared<Text>("spring value = 1.000017");
        auto line3 = std::make_shared<Text>("keyframe@0.5 = rgb(236,72,153)");
        Column col{au::Node{line1}, au::Node{line2}, Node{line3}};
        BuildContext lctx;
        Constraints lc;
        lc.min = Size{.width = 0, .height = 0};
        lc.max = Size{.width = 520, .height = 520};
        col.layout(lc, lctx);

        auto paint_all = [&](Painter &p) -> void {
            p.begin(520, 520);
            p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 520, .height = 520}},
                        Color::white());
            col.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 520, .height = 520}}, lctx);
        };
        Painter warm;
        paint_all(warm);  // 填充 display_text_

        auto find_box = [&](const std::string &needle) -> Rect {
            Rect r{.origin = Point{.x = 1e9F, .y = 1e9F}, .size = Size{.width = -1e9F, .height = -1e9F}};
            for (int y = 0; y < 520; ++y) {
                for (int x = 0; x < 520; ++x) {
                    Widget *h =
                        EventDispatcher::hit_test(col, Point{.x = static_cast<float>(x), .y = static_cast<float>(y)});
                    auto const *t = dynamic_cast<Text *>(h);
                    if (t && t->display_text().find(needle) != std::string::npos) {
                        r.origin.x = std::min(r.origin.x, static_cast<float>(x));
                        r.origin.y = std::min(r.origin.y, static_cast<float>(y));
                        r.size.width = std::max(r.size.width, static_cast<float>(x) - r.origin.x);
                        r.size.height = std::max(r.size.height, static_cast<float>(y) - r.origin.y);
                    }
                }
            }
            return r;
        };
        auto count_blue = [&](const Painter &p, const Rect &r) -> int {
            int n = 0;
            for (int y = static_cast<int>(r.origin.y); y < static_cast<int>(r.origin.y + r.size.height); ++y) {
                for (int x = static_cast<int>(r.origin.x); x < static_cast<int>(r.origin.x + r.size.width); ++x) {
                    const Color c = p.get_pixel(x, y);
                    if (static_cast<int>(c.b) - static_cast<int>(c.r) > 30) {
                        ++n;
                    }
                }
            }
            return n;
        };

        Rect r1 = find_box("curve@0.5");
        Rect r2 = find_box("spring value");
        Rect r3 = find_box("keyframe@0.5");
        AURORA_TEST_CHECK(r1.size.height > 0 && r2.size.height > 0 && r3.size.height > 0);

        // 邻行蓝像素 baseline（未选中 line1 时）
        Painter base_p;
        paint_all(base_p);
        const int blue2_base = count_blue(base_p, r2);
        const int blue3_base = count_blue(base_p, r3);

        FocusManager fm;
        fm.set_root(&col);
        auto press = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Press;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };
        auto move = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Move;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };
        auto release = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Release;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };

        // 拖选 line1：右端按下 -> 拖到左端 'c' -> 继续向左下漂移进入 line2 区域
        const float y1c = r1.origin.y + (r1.size.height * 0.5F);
        press(r1.origin.x + r1.size.width - 3.0F, y1c);
        move(r1.origin.x + 2.0F, y1c);
        move(r1.origin.x + 2.0F, r2.origin.y + (r2.size.height * 0.5F));
        release(r1.origin.x + 2.0F, r2.origin.y + (r2.size.height * 0.5F));

        AURORA_TEST_CHECK(line1->has_selection());  // line1 应被选中（sanity）
        AURORA_TEST_CHECK(!line2->has_selection());  // line2 不得被真实选中
        AURORA_TEST_CHECK(!line3->has_selection());  // line3 不得被真实选中

        Painter post_p;
        paint_all(post_p);
        const int blue2_post = count_blue(post_p, r2);
        const int blue3_post = count_blue(post_p, r3);
        // 选中 line1 后，邻行蓝像素数不应增加（高亮不得渗入邻行）
        AURORA_TEST_CHECK(blue2_post == blue2_base);
        AURORA_TEST_CHECK(blue3_post == blue3_base);
        AURORA_LOG_INFO("test", "[6] neighbor unselected & uncolored under ClearType (blue2 base=", blue2_base,
                        " post=", blue2_post, ", blue3 base=", blue3_base, " post=", blue3_post, ") OK");
    }

    // 7) 回归：拖选时，按下与松开所在的字符都应计入选区（含头含尾）。
    //    此前若按下落在首字符右半、或松开落在末字符左半，端点字符会被漏选，
    //    表现为「行首/行尾存在若干字符没有被高亮选中」。
    {
        Text txt("Hello World");
        txt.font_size(24).set_soft_wrap(false).set_align(TextAlign::Left);
        BuildContext ctx;
        txt.mount(ctx);
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = 1000, .height = 100};
        txt.layout(cc, ctx);
        const Font f = txt.font;
        render::TextLayoutOpts o{};
        const std::string s = "Hello World";
        const size_t total = s.size();  // 全 ASCII：字节数 == 码点数
        auto right_half = [&](size_t idx) -> float {
            const float l = render::FontEngine::caret_x(s, idx, f, o);
            const float r = render::FontEngine::caret_x(s, idx + 1, f, o);
            return l + (0.75F * (r - l));
        };
        auto left_half = [&](size_t idx) -> float {
            const float l = render::FontEngine::caret_x(s, idx, f, o);
            const float r = render::FontEngine::caret_x(s, idx + 1, f, o);
            return l + (0.25F * (r - l));
        };

        // 按下首字符 'H'(idx0) 右半，拖到 'o'(idx4) 右半：首字符必须被选中。
        MouseEvent p;
        p.action = MouseAction::Press;
        p.local_position = Point{.x = right_half(0), .y = 5.0F};
        txt.on_pointer_event(p);
        MouseEvent mv;
        mv.action = MouseAction::Move;
        mv.local_position = Point{.x = right_half(4), .y = 5.0F};
        txt.on_pointer_event(mv);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 0);  // 首字符 'H' 被选中
        AURORA_TEST_CHECK(txt.selection().second == 5);  // 含 idx0..4 共 5 码点
        AURORA_LOG_INFO("test", "[7a] first-char inclusive on right-half press OK");

        // 按下首字符左半，拖到末字符 'd'(idx10) 左半：末字符必须被选中。
        txt.on_focus_change(false);
        MouseEvent p2;
        p2.action = MouseAction::Press;
        p2.local_position = Point{.x = left_half(0), .y = 5.0F};
        txt.on_pointer_event(p2);
        MouseEvent mv2;
        mv2.action = MouseAction::Move;
        mv2.local_position = Point{.x = left_half(10), .y = 5.0F};
        txt.on_pointer_event(mv2);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 0);
        AURORA_TEST_CHECK(txt.selection().second == total);  // 整段（末字符含入）
        AURORA_LOG_INFO("test", "[7b] last-char inclusive on left-half release OK");
    }

    // 8) 回归：多行（换行）选区中，行尾与行首的端点字符都应计入选区。
    {
        Text txt("Hello World");
        txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Left);
        BuildContext ctx;
        txt.mount(ctx);
        const Font f = txt.font;
        render::TextLayoutOpts o{};
        const float w_hello = render::FontEngine::measure_width("Hello", f, o);
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = w_hello + 2.0F, .height = 100};
        txt.layout(cc, ctx);
        // line0="Hello"(cp0-4)，line1="World"(cp6-10)。选 line0 的 'l'(idx3) 到 line1 的 'r'(idx8)。
        // 在字符内部（右半/左半）点击，端点含入无歧义。
        auto rh = [&](const std::string &line, size_t idx) -> float {
            const float l = render::FontEngine::caret_x(line, idx, f, o);
            const float r = render::FontEngine::caret_x(line, idx + 1, f, o);
            return l + (0.75F * (r - l));
        };
        auto lh = [&](const std::string &line, size_t idx) -> float {
            const float l = render::FontEngine::caret_x(line, idx, f, o);
            const float r = render::FontEngine::caret_x(line, idx + 1, f, o);
            return l + (0.25F * (r - l));
        };
        const float x0 = rh("Hello", 3);  // line0 内 idx3 右半
        const float x1 = lh("World", 8 - 6);  // line1 内 idx8 的相对位置(=2) 左半
        MouseEvent p;
        p.action = MouseAction::Press;
        p.local_position = Point{.x = x0, .y = 5.0F};
        txt.on_pointer_event(p);
        MouseEvent mv;
        mv.action = MouseAction::Move;
        mv.local_position = Point{.x = x1, .y = 50.0F};
        txt.on_pointer_event(mv);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 3);  // line0 端点 'l'(idx3) 选中
        AURORA_TEST_CHECK(txt.selection().second == 9);  // line1 端点 'r'(idx8) 选中 -> [3,9)
        AURORA_LOG_INFO("test", "[8] line-end/line-start endpoints inclusive in multi-line OK");
    }

    // 9) 严格回归：选中整段后，逐行检查高亮蓝色像素的最左/最右 x 边界，
    //    必须等于 caret_x(行, 0) 与 caret_x(行, 行码点数) —— 即每行行首与行尾字符
    //    都必须被高亮。此前若端点字符未高亮，本块会暴露（端点 x 比期望短）。
    {
        auto is_blue = [](const Color &c) -> bool { return static_cast<int>(c.b) - static_cast<int>(c.r) > 30; };
        constexpr render::TextLayoutOpts o{};

        // --- 9A：单行整选，验证行尾字符被高亮 ---
        {
            const std::string s = "Hello World";
            Text txt(s);
            txt.font_size(24).set_soft_wrap(false).set_align(TextAlign::Left);
            BuildContext ctx;
            txt.mount(ctx);
            const Font f = txt.font;
            const float full_w = render::FontEngine::measure_width(s, f, o);
            [[maybe_unused]] const float line_h = render::FontEngine::measure_height(f);
            Constraints cc;
            cc.min = Size{.width = 0, .height = 0};
            cc.max = Size{.width = 400, .height = 100};
            const Size sz = txt.layout(cc, ctx);

            MouseEvent press;
            press.action = MouseAction::Press;
            press.button = MouseButton::Left;
            press.local_position = Point{.x = 0.0F, .y = 5.0F};
            txt.on_pointer_event(press);
            MouseEvent move;
            move.action = MouseAction::Move;
            move.button = MouseButton::Left;
            move.local_position = Point{.x = 399.0F, .y = 5.0F};  // 拖到远超文本右侧
            txt.on_pointer_event(move);
            MouseEvent rel;
            rel.action = MouseAction::Release;
            rel.button = MouseButton::Left;
            rel.local_position = Point{.x = 399.0F, .y = 5.0F};
            txt.on_pointer_event(rel);
            AURORA_TEST_CHECK(txt.has_selection());

            Painter p;
            p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
            p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                        Color::white());
            txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                      ctx);

            int minx = 1e9;
            int maxx = -1e9;
            for (int y = 0; y < static_cast<int>(sz.height); ++y) {
                for (int x = 0; x < static_cast<int>(sz.width); ++x) {
                    if (is_blue(p.get_pixel(x, y))) {
                        minx = std::min(minx, x);
                        maxx = std::max(maxx, x);
                    }
                }
            }
            const bool left_ok = (minx <= 2);
            const bool right_ok = (maxx >= static_cast<int>(full_w) - 2);
            if (!left_ok || !right_ok) {
                AURORA_LOG_INFO("test", "  [9A] minx=", minx, " maxx=", maxx, " exp_right=", full_w,
                                " left_ok=", left_ok, " right_ok=", right_ok);
            }
            AURORA_TEST_CHECK(left_ok && right_ok);
            AURORA_LOG_INFO("test", "[9A] single-line full-selection endpoint highlight OK (full_w=", full_w, ")");
        }

        // --- 9B：干净词折行（无 char-split），逐行验证端点高亮 ---
        {
            const std::string s = "Hello World";
            Text txt(s);
            txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Left);
            BuildContext ctx;
            txt.mount(ctx);
            const Font f = txt.font;
            const float w_hello = render::FontEngine::measure_width("Hello", f, o);
            const float w_world = render::FontEngine::measure_width("World", f, o);
            const float line_h = render::FontEngine::measure_height(f);
            // 宽度需 >= max("Hello","World")，确保按词折行、不触发 char-split。
            const float w = std::max(w_hello, w_world) + 3.0F;
            Constraints cc;
            cc.min = Size{.width = 0, .height = 0};
            cc.max = Size{.width = w, .height = 100};
            const Size sz = txt.layout(cc, ctx);
            AURORA_TEST_CHECK(line_h > 0);

            MouseEvent press;
            press.action = MouseAction::Press;
            press.button = MouseButton::Left;
            press.local_position = Point{.x = 0.0F, .y = 5.0F};
            txt.on_pointer_event(press);
            MouseEvent move;
            move.action = MouseAction::Move;
            move.button = MouseButton::Left;
            move.local_position = Point{.x = w - 1.0F, .y = line_h + 5.0F};
            txt.on_pointer_event(move);
            MouseEvent rel;
            rel.action = MouseAction::Release;
            rel.button = MouseButton::Left;
            rel.local_position = Point{.x = w - 1.0F, .y = line_h + 5.0F};
            txt.on_pointer_event(rel);
            AURORA_TEST_CHECK(txt.has_selection());

            Painter p;
            p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
            p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                        Color::white());
            txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                      ctx);

            auto line_blue_extent = [&](int li) -> std::pair<int, int> {
                int mn = 1e9;
                int mx = -1e9;
                const int y0 = static_cast<int>(static_cast<float>(li) * line_h);
                const int y1 = static_cast<int>(static_cast<float>(li + 1) * line_h);
                for (int y = y0; y < y1; ++y) {
                    for (int x = 0; x < static_cast<int>(sz.width); ++x) {
                        if (is_blue(p.get_pixel(x, y))) {
                            mn = std::min(mn, x);
                            mx = std::max(mx, x);
                        }
                    }
                }
                return {mn, mx};
            };
            bool ep_ok = true;
            {
                auto [mn, mx] = line_blue_extent(0);
                const bool left_ok = (mn <= 2);
                const bool right_ok = (mx >= static_cast<int>(w_hello) - 2);
                if (!left_ok || !right_ok) {
                    ep_ok = false;
                    AURORA_LOG_INFO("test", "  [9B] line0 minx=", mn, " maxx=", mx, " exp=", w_hello);
                }
            }
            {
                auto [mn, mx] = line_blue_extent(1);
                const bool left_ok = (mn <= 2);
                const bool right_ok = (mx >= static_cast<int>(w_world) - 2);
                if (!left_ok || !right_ok) {
                    ep_ok = false;
                    AURORA_LOG_INFO("test", "  [9B] line1 minx=", mn, " maxx=", mx, " exp=", w_world);
                }
            }
            AURORA_TEST_CHECK(ep_ok);
            AURORA_LOG_INFO("test", "[9B] clean word-wrap per-line endpoint highlight OK (wHello=", w_hello,
                            " wWorld=", w_world, ")");
        }
    }

    // 10) 回归：Justify 两端对齐段落 —— 非末行按逐词均分拉伸铺满整行，选中该行后高亮
    //     必须延伸到行右缘（此前高亮按自然宽度计算，多行选中时行尾未被高亮）；
    //     且命中测试与拉伸后的词位一致（词间拉伸间隙归属其空格字符）。
    {
        render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
        auto is_blue = [](const Color &c) -> bool { return static_cast<int>(c.b) - static_cast<int>(c.r) > 30; };
        constexpr render::TextLayoutOpts o{};

        // 构造确定性两行：line0="aa bb"（Justify 拉伸行），line1="cccccccc"（末行不拉伸）。
        Text txt("aa bb cccccccc");
        txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Justify);
        BuildContext ctx;
        txt.mount(ctx);
        const Font f = txt.font;
        const float w_aa = render::FontEngine::measure_width("aa", f, o);
        const float w_bb = render::FontEngine::measure_width("bb", f, o);
        const float w_c = render::FontEngine::measure_width("cccccccc", f, o);
        const float line_h = render::FontEngine::measure_height(f);
        const float w = w_c + 20.0F;  // "aa bb" 后挤不下 "cccccccc" → 折两行；"cccccccc" 单独成行
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = w, .height = 200};
        const Size sz = txt.layout(cc, ctx);
        AURORA_TEST_CHECK(sz.height > 1.5F * line_h);  // 确已折成两行

        // 10a：跨行全选后，line0（拉伸行）高亮必须达到行右缘。
        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.local_position = Point{.x = 1.0F, .y = 2.0F};
        txt.on_pointer_event(press);
        MouseEvent move;
        move.action = MouseAction::Move;
        move.button = MouseButton::Left;
        move.local_position = Point{.x = w - 1.0F, .y = line_h * 1.5F};
        txt.on_pointer_event(move);
        MouseEvent rel;
        rel.action = MouseAction::Release;
        rel.button = MouseButton::Left;
        rel.local_position = Point{.x = w - 1.0F, .y = line_h * 1.5F};
        txt.on_pointer_event(rel);
        AURORA_TEST_CHECK(txt.has_selection());

        Painter p;
        p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                    Color::white());
        txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}}, ctx);

        int minx0 = static_cast<int>(1e9);
        int maxx0 = -1;
        for (int y = 0; y < static_cast<int>(line_h); ++y) {
            for (int x = 0; x < static_cast<int>(sz.width); ++x) {
                if (is_blue(p.get_pixel(x, y))) {
                    minx0 = std::min(minx0, x);
                    maxx0 = std::max(maxx0, x);
                }
            }
        }
        const bool left_ok = (minx0 <= 2);
        const bool right_ok = (maxx0 >= static_cast<int>(sz.width) - 3);  // 行尾必须高亮到右缘
        if (!left_ok || !right_ok) {
            AURORA_LOG_INFO("test", "  [10a] line0 minx=", minx0, " maxx=", maxx0, " W=", sz.width);
        }
        AURORA_TEST_CHECK(left_ok && right_ok);
        AURORA_LOG_INFO("test", "[10a] justify line highlight reaches line right edge OK (W=", sz.width, ")");

        // 10b：点击拉伸间隙中点应命中词间空格（cp=2），而非按自然宽度误判为行尾字符。
        txt.on_focus_change(false);  // 清选区
        const float gap_mid = (w_aa + (w - w_bb)) * 0.5F;  // 间隙 = [wAA, W-wBB]
        MouseEvent p2;
        p2.action = MouseAction::Press;
        p2.button = MouseButton::Left;
        p2.local_position = Point{.x = gap_mid, .y = 2.0F};
        txt.on_pointer_event(p2);
        MouseEvent m2;
        m2.action = MouseAction::Move;
        m2.button = MouseButton::Left;
        m2.local_position = Point{.x = gap_mid, .y = 2.0F};
        txt.on_pointer_event(m2);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 2);  // 命中的是空格（"aa bb" 的 cp2）
        AURORA_TEST_CHECK(txt.selection().second == 3);
        AURORA_LOG_INFO("test", "[10b] justify gap hit-test maps to space char OK");
    }

    // 11) 回归：贴合 run_demo 的真实派发路径 —— 鼠标派发必须携带 FocusManager，
    //     点击拖选获焦后 Ctrl+C 才能复制选中文本（此前 demo 鼠标派发不带 fm，
    //     request_focus 静默 no-op，键盘事件到不了 Text，Ctrl+C 无效）。
    {
        const std::string src = "copy me via dispatcher";
        auto t = std::make_shared<Text>(src);
        Column col{Node{t}};
        BuildContext lctx;
        Constraints lc;
        lc.min = Size{.width = 0, .height = 0};
        lc.max = Size{.width = 400, .height = 100};
        col.layout(lc, lctx);

        FocusManager fm;
        fm.set_root(&col);
        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.position = Point{.x = 2.0F, .y = 5.0F};
        EventDispatcher::dispatch(col, press, &fm);
        MouseEvent move;
        move.action = MouseAction::Move;
        move.button = MouseButton::Left;
        move.position = Point{.x = 399.0F, .y = 5.0F};
        EventDispatcher::dispatch(col, move, &fm);
        MouseEvent rel;
        rel.action = MouseAction::Release;
        rel.button = MouseButton::Left;
        rel.position = Point{.x = 399.0F, .y = 5.0F};
        EventDispatcher::dispatch(col, rel, &fm);

        AURORA_TEST_CHECK(t->has_selection());
        AURORA_TEST_CHECK(t->is_focused());  // 鼠标派发带 fm → 点击获焦
        AURORA_TEST_CHECK(fm.focused() == t.get());

        Clipboard::set_text("__PROBE__");
        if (Clipboard::get_text() != "__PROBE__") {
            AURORA_LOG_INFO("test", "[11][SKIP] system clipboard unavailable in this env");
        } else {
            KeyEvent ke;
            ke.action = KeyAction::Down;
            ke.key = static_cast<int>(KeyCode::C);
            ke.modifiers = ModifierKey::Control;
            EventDispatcher::dispatch(col, ke, fm);
            AURORA_TEST_CHECK(ke.is_handled);
            AURORA_TEST_CHECK(Clipboard::get_text() == src);  // 整段拖选 → 复制全文
            AURORA_LOG_INFO("test", "[11] click-focus + Ctrl+C copy via dispatcher pipeline OK");
        }
    }

    // 12) 回归：缩放屏（如 150%，Painter::scale=1.5）下多行全选后，末行（整串按物理 DPI 绘制）
    //     行尾墨迹必须被高亮覆盖 —— GDI hinting 在 96dp 测量与物理 DPI 实绘间取整不成比例，
    //     误差在行尾累计，此前全选后末行尾部欠出「半个字符 + 标点」宽度的高亮。
    //     （高亮/命中按实显 caret（物理 DPI 前缀 extent）计算，与实绘像素对齐。）
    {
        render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
        constexpr float k_scale = 1.5F;  // 150% 缩放：字形按 144 DPI 光栅
        // 长段落软折两行：末行需足够长（≈半段），hinting 取整误差才能在行尾累计出可见宽度。
        const std::string k_para =
            "The pale moon rises above the sleeping town, and the silver light spills gently across the rooftops.";
        Text txt(k_para);
        txt.font_size(15).set_soft_wrap(true).set_align(TextAlign::Justify);
        BuildContext ctx;
        txt.mount(ctx);
        const Font f = txt.font;
        constexpr render::TextLayoutOpts o{};
        const float full = render::FontEngine::measure_width(k_para, f, o);
        const float line_h = render::FontEngine::measure_height(f);
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = full * 0.52F, .height = 300};  // 折成两行：末行≈半段长度，以句号结尾
        const Size sz = txt.layout(cc, ctx);
        AURORA_TEST_CHECK(sz.height > 1.5F * line_h);
        // 此处 +0.5 四舍五入为既有断言口径（行数为小正数、无负值/半数值边界），改 lround 可能移动取整边界，故保留
        // NOLINTNEXTLINE(bugprone-incorrect-roundings)
        const auto n_lines = static_cast<size_t>(((sz.height - 2.0F) / line_h) + 0.5F);

        auto paint_once = [&](Painter &p) -> void {
            p.set_scale(k_scale);
            p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
            p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                        Color::white());
            txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                      ctx);
        };
        // 末行像素带（物理坐标）：[(n-1)*line_h, n*line_h) * scale
        const int y0 = static_cast<int>(static_cast<float>(n_lines - 1) * line_h * k_scale);
        const int y1 = static_cast<int>(static_cast<float>(n_lines) * line_h * k_scale);

        // 基线：无选区绘制，找末行墨迹（近黑）最右 x。
        Painter base;
        paint_once(base);
        int ink_max = -1;
        for (int y = y0; y < std::min(y1, base.height()); ++y) {
            for (int x = 0; x < base.width(); ++x) {
                const Color c = base.get_pixel(x, y);
                if (c.r < 100 && c.g < 100 && c.b < 100) {
                    ink_max = std::max(ink_max, x);
                }
            }
        }
        AURORA_TEST_CHECK(ink_max > 0);  // 末行确有墨迹

        // 拖选全文后重绘，末行蓝色高亮最右 x 必须覆盖墨迹最右 x。
        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.local_position = Point{.x = 0.5F, .y = 2.0F};
        txt.on_pointer_event(press);
        MouseEvent move;
        move.action = MouseAction::Move;
        move.button = MouseButton::Left;
        move.local_position = Point{.x = sz.width * 2.0F, .y = sz.height * 2.0F};  // 远超末行末字符
        txt.on_pointer_event(move);
        MouseEvent rel;
        rel.action = MouseAction::Release;
        rel.button = MouseButton::Left;
        rel.local_position = move.local_position;
        txt.on_pointer_event(rel);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 0);
        AURORA_TEST_CHECK(txt.selection().second == k_para.size());  // 纯 ASCII：字节数即码点数，含末尾句号

        Painter sel;
        paint_once(sel);
        int blue_max = -1;
        for (int y = y0; y < std::min(y1, sel.height()); ++y) {
            for (int x = 0; x < sel.width(); ++x) {
                const Color c = sel.get_pixel(x, y);
                if (static_cast<int>(c.b) - static_cast<int>(c.r) > 30) {
                    blue_max = std::max(blue_max, x);
                }
            }
        }
        if (blue_max < ink_max - 1) {
            AURORA_LOG_INFO("test", "  [12] last-line ink_max=", ink_max, " blue_max=", blue_max, " (scale=", k_scale,
                            ")");
        }
        AURORA_TEST_CHECK(blue_max >= ink_max - 1);  // 高亮必须覆盖到末行墨迹右缘（含末尾标点）
        AURORA_LOG_INFO("test", "[12] scaled-display last-line tail fully highlighted OK (ink=", ink_max,
                        " blue=", blue_max, ")");
    }

    // 13) 回归：缩放屏（scale=1.5）下逐字符命中往返 —— 在每个字符的「实显中心」按下拖选，
    //     选中的必须正是该字符。此前命中按整行实显/自然宽度比线性换算，但 hinting 取整
    //     偏差在行内非线性，相邻窄字符边界处会跨界，造成「从第一个字符开始选择、
    //     实际选中的却是第二个」的 off-by-one（现改用 display_caret_x 逐字符精确边界）。
    {
        render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
        constexpr float k_scale = 1.5F;
        // 含大量窄字符（i/l/t）：窄字符处半字宽仅 1–2dp，线性近似残差最易跨边界。
        const std::string k_line = "The pale illimitable moonlit hills still fill the silent little mill.";
        Text txt(k_line);
        txt.font_size(15).set_soft_wrap(false);  // 单行，默认左对齐（line_off=0）
        BuildContext ctx;
        txt.mount(ctx);
        const Font f = txt.font;
        constexpr render::TextLayoutOpts o{};
        const float full = render::FontEngine::measure_width(k_line, f, o);
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = full + 20.0F, .height = 100};
        const Size sz = txt.layout(cc, ctx);

        // 绘制一次以记录 paint_scale_=1.5（实显命中与绘制同源）。
        Painter p;
        p.set_scale(k_scale);
        p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                    Color::white());
        txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}}, ctx);

        auto click_sel = [&](float cx) -> void {
            txt.on_focus_change(false);  // 清上一轮选区
            MouseEvent pr;
            pr.action = MouseAction::Press;
            pr.button = MouseButton::Left;
            pr.local_position = Point{.x = cx, .y = 2.0F};
            txt.on_pointer_event(pr);
            MouseEvent mv;
            mv.action = MouseAction::Move;
            mv.button = MouseButton::Left;
            mv.local_position = Point{.x = cx, .y = 2.0F};
            txt.on_pointer_event(mv);
            MouseEvent rl;
            rl.action = MouseAction::Release;
            rl.button = MouseButton::Left;
            rl.local_position = Point{.x = cx, .y = 2.0F};
            txt.on_pointer_event(rl);
        };

        size_t mismatches = 0;
        const size_t n = k_line.size();  // 纯 ASCII：字节数即码点数
        for (size_t i = 0; i < n; ++i) {
            // 用户肉眼对准的是实绘字形 → 在该字符实显宽度内左/中/右三点采样（dp）：
            // 「从字符起始处按下」对应左采样点，选中的必须正是该字符。
            const float x0 = render::FontEngine::display_caret_x(k_line, i, f, o, k_scale);
            const float x1 = render::FontEngine::display_caret_x(k_line, i + 1, f, o, k_scale);
            if (x1 - x0 <= 1.0F) {
                continue;  // 零宽/极窄字形不采边缘
            }
            const float probes[3] = {x0 + 0.4F, (x0 + x1) * 0.5F, x1 - 0.4F};
            for (const float cx : probes) {
                click_sel(cx);
                if (!txt.has_selection() || txt.selection().first != i || txt.selection().second != i + 1) {
                    ++mismatches;
                    if (mismatches <= 5) {
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                        AURORA_LOG_INFO("test", "  [13] cp=", i, " ('", k_line[i], "') x=", cx, " got [",
                                        txt.selection().first, ", ", txt.selection().second, ")");
                    }
                }
            }
        }
        AURORA_TEST_CHECK(mismatches == 0);
        AURORA_LOG_INFO("test", "[13] per-char display-space hit round-trip OK (n=", n, ")");
    }

    // 14) 回归：实显度量必须按物理像素尺寸真算，不得退化为自然度量的伪转发 ——
    //     FT hinting 把 advance 取整到整像素，px=20 与 px=30 的逐字形 advance 不成 1.5 比，
    //     长串累计后实显宽度与自然宽度必然分叉；若有人把 display_* 改回转发别名，
    //     本用例的分叉断言与墨迹对齐断言都会失败（对应症状：缩放屏按 'a' 选中 'b'）。
    {
        constexpr float k_scale = 1.5F;
        const std::string k_line = "The pale illimitable moonlit hills still fill the silent little mill.";
        auto f = Font{.size_pt = 15.0F};
        constexpr render::TextLayoutOpts o{};
        const std::size_t n_cp = k_line.size();  // 纯 ASCII：字节数即码点数

        // 14a) 分叉：实显行宽与自然行宽在 1.5x 下必须不同（伪转发时两者恒等）。
        const float natural_w = render::FontEngine::caret_x(k_line, n_cp, f, o);
        const float display_w = render::FontEngine::display_caret_x(k_line, n_cp, f, o, k_scale);
        if (std::abs(display_w - natural_w) <= 0.1F) {
            AURORA_LOG_INFO("test", "  [14a] natural=", natural_w, " display=", display_w);
        }
        AURORA_TEST_CHECK(std::abs(display_w - natural_w) > 0.1F);
        // scale=1 退化：与自然度量逐位相等（Headless/golden 路径不受影响）。
        AURORA_TEST_CHECK(render::FontEngine::instance().display_caret_x(k_line, n_cp, f, o, 1.0F) == natural_w);

        // 14b) 墨迹对齐：1.5x 实绘整行墨迹右缘（物理 px）必须落在 display_w*scale 附近，
        //     且不得更贴近 natural_w*scale（否则说明实显度量没有与实绘同源）。
        const float phys_w = display_w * k_scale;
        Painter p;
        p.set_scale(k_scale);
        const int w = static_cast<int>(phys_w / k_scale) + 40;
        p.begin(w, 40);
        p.fill_rect(
            Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = static_cast<float>(w), .height = 40.0F}},
            Color::white());
        p.draw_text(
            Rect{.origin = Point{.x = 0.0F, .y = 2.0F}, .size = Size{.width = static_cast<float>(w), .height = 30.0F}},
            k_line, f, Color::black());
        int ink_max = -1;
        for (int y = 0; y < p.height(); ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if (c.r < 100 && c.g < 100 && c.b < 100) {
                    ink_max = std::max(ink_max, x);
                }
            }
        }
        AURORA_TEST_CHECK(ink_max > 0);
        // 末字符 '.' 右侧承距小；容差留足字形右边距与 AA 扩散（实测典型偏差 < 4px）。
        const float err_display = std::abs(static_cast<float>(ink_max) - phys_w);
        const float err_natural = std::abs(static_cast<float>(ink_max) - (natural_w * k_scale));
        if (err_display >= err_natural || err_display > 8.0F) {
            AURORA_LOG_INFO("test", "  [14b] ink_max=", ink_max, " display*s=", phys_w,
                            " natural*s=", natural_w * k_scale);
        }
        AURORA_TEST_CHECK(err_display < err_natural);  // 实显度量必须比自然度量更贴近实绘像素
        AURORA_TEST_CHECK(err_display <= 8.0F);  // 且绝对误差在字形右边距量级内
        AURORA_LOG_INFO("test", "[14] display metrics diverge from natural & align with drawn ink OK (ink=", ink_max,
                        " display*s=", phys_w, " natural*s=", natural_w * k_scale, ")");
    }

    AURORA_LOG_INFO("test", "ALL TEXT SELECTION TESTS PASSED");
}
}  // namespace sec_text_selection

AURORA_TEST() {
    sec_text_selection::run();
}

}  // namespace aurora::test_cases::utest_text_selection
