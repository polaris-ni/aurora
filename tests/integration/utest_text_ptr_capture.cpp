/// 测试类型: unit
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: utest_text_ptr_capture 单元测试
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

// （自 utest_text.cpp 拆分：指针捕获/RTL 拖选/窗口外释放段）

namespace aurora::test_cases::utest_text_ptr_capture {

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
namespace sec_text_ptr_capture {

namespace {
auto layout_root(Widget &root, const float w, const float h) -> void {
    Constraints c;
    c.min = Size{.width = 0, .height = 0};
    c.max = Size{.width = w, .height = h};
    const BuildContext ctx;
    root.layout(c, ctx);
}
auto paint_root(Widget &root, const float w, const float h) -> void {
    Painter p;
    p.begin(static_cast<int>(w), static_cast<int>(h));
    const BuildContext ctx;
    root.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = w, .height = h}}, ctx);
}
// 扫描各 Text（按显示文本）的可命中盒（哨兵初始化，避免默认 Rect 误判）。
auto scan_texts(Widget &root) -> std::map<std::string, Rect> {
    std::map<std::string, Rect> out;
    for (int y = 0; y < 800; ++y) {
        for (int x = 0; x < 520; ++x) {
            Widget *h = EventDispatcher::hit_test(root, Point{.x = static_cast<float>(x), .y = static_cast<float>(y)});
            auto const *t = dynamic_cast<Text *>(h);
            if (t == nullptr) {
                continue;
            }
            const std::string key = t->display_text();
            const auto ins = out.emplace(
                key, Rect{.origin = Point{.x = 1e9F, .y = 1e9F}, .size = Size{.width = -1e9F, .height = -1e9F}});
            Rect &r = ins.first->second;
            r.origin.x = std::min(r.origin.x, static_cast<float>(x));
            r.origin.y = std::min(r.origin.y, static_cast<float>(y));
            r.size.width = std::max(r.size.width, static_cast<float>(x) - r.origin.x);
            r.size.height = std::max(r.size.height, static_cast<float>(y) - r.origin.y);
        }
    }
    return out;
}
}  // namespace

void run() {
    int fails = 0;
    auto ck = [&](bool c, const char *m) -> void {
        if (!c) {
            AURORA_TEST_PRINTF("  [FAIL] %s\n", m);
            ++fails;
        } else {
            AURORA_TEST_PRINTF("  [PASS] %s\n", m);
        }
    };

    // 1) RTL 拖选最左字：从右端按下向左拖，须包含索引 0（首字 '默'）。
    {
        AURORA_TEST_PRINTF("[1] RTL drag-select leftmost char:\n");
        auto a = std::make_shared<Text>(
            TextProps{.content = LocalizedString{"默认14pt文本"}, .text_align = TextAlign::Left, .soft_wrap = true});
        Column col{ColumnProps{.children = {Node{a}}}};
        layout_root(col, 520, 800);
        paint_root(col, 520, 800);
        auto boxes = scan_texts(col);
        auto it = boxes.find("默认14pt文本");
        ck(it != boxes.end(), "text hit");
        if (it != boxes.end()) {
            const Rect &r = it->second;
            EventDispatcher ed;
            FocusManager fm;
            fm.set_root(&col);
            auto press = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Press;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                ed.dispatch_mouse(col, e, &fm);
            };
            auto move = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Move;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                ed.dispatch_mouse(col, e, &fm);
            };
            auto release = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Release;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                ed.dispatch_mouse(col, e, &fm);
            };
            const float yc = r.origin.y + (r.size.height * 0.5F);
            press(r.origin.x + r.size.width - 2.0F, yc);
            move(r.origin.x + 1.0F, yc);
            move(r.origin.x - 5.0F, yc);  // 越过左边界
            const auto sel = a->selection();
            const std::size_t lo = std::min(sel.first, sel.second);
            const std::size_t hi = std::max(sel.first, sel.second);
            ck(hi - lo == 8, "RTL drag selection covers all 8 codepoints");
            ck(lo == 0, "RTL drag selection starts at index 0 (incl. leftmost '默')");
            release(r.origin.x - 5.0F, yc);
        }
    }

    // 2) 窗口外释放：拖选时光标移出根/窗口，释放事件仍须送达并按捕获路径结束选择。
    {
        AURORA_TEST_PRINTF("[2] release outside window ends selection:\n");
        auto a = std::make_shared<Text>(TextProps{.content = LocalizedString{"默认14pt文本"}, .soft_wrap = true});
        Column col{ColumnProps{.children = {Node{a}}}};
        layout_root(col, 520, 800);
        paint_root(col, 520, 800);
        auto boxes = scan_texts(col);
        Rect r{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 520, .height = 30}};
        if (auto it = boxes.find("默认14pt文本"); it != boxes.end()) {
            r = it->second;
        }
        EventDispatcher ed;
        FocusManager fm;
        fm.set_root(&col);
        auto press = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Press;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            ed.dispatch_mouse(col, e, &fm);
        };
        auto move = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Move;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            ed.dispatch_mouse(col, e, &fm);
        };
        auto release = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Release;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            ed.dispatch_mouse(col, e, &fm);
        };
        const float yc = r.origin.y + (r.size.height * 0.5F);
        press(r.origin.x + r.size.width - 2.0F, yc);
        move(r.origin.x + 30.0F, yc);
        ck(a->has_selection(), "selection exists after drag");
        release(9999.0F, yc);  // 窗口外释放
        const auto s1 = a->selection();
        ck(a->has_selection(), "selection retained after release outside window (not lost)");
        move(r.origin.x + 5.0F, yc);  // 释放后再次 move
        const auto s2 = a->selection();
        ck(s2 == s1, "re-move after release does not change selection (selection ended, m_selecting=false)");
    }

    // 3) 相邻两 soft_wrap 文本不应重叠：默认 soft_wrap=true 时 Text 仅当确需换行才填满，
    //    短文本按内容宽度上报，兄弟控件可并排且各自可选中。
    {
        AURORA_TEST_PRINTF("[3] adjacent soft_wrap texts do not overlap:\n");
        auto a = std::make_shared<Text>(
            TextProps{.content = LocalizedString{"默认14pt文本"}, .text_align = TextAlign::Left, .soft_wrap = true});
        auto b = std::make_shared<Text>(
            TextProps{.content = LocalizedString{"Text控件"}, .text_align = TextAlign::Left, .soft_wrap = true});
        Row row{RowProps{.children = {au::Node{a}, Node{b}}}};
        layout_root(row, 520, 800);
        paint_root(row, 520, 800);
        auto boxes = scan_texts(row);
        auto it_a = boxes.find("默认14pt文本");
        auto it_b = boxes.find("Text控件");
        ck(it_a != boxes.end() && it_b != boxes.end(), "both Text widgets are hittable");
        if (it_a != boxes.end() && it_b != boxes.end()) {
            const Rect &ra = it_a->second;
            const Rect &rb = it_b->second;
            const bool overlap = rb.origin.x < ra.origin.x + ra.size.width && ra.origin.x < rb.origin.x + rb.size.width;
            ck(!overlap, "two Text hit boxes do not overlap (second is selectable)");
        }
    }

    // 4) 复现「demo 经静态 EventDispatcher::dispatch（每个事件新建实例→无捕获）导致 RTL 拖选越过左边界后丢选区」：
    //    修复后静态 dispatch 内部委托持久实例，越过左边界仍延伸到索引 0（含最左'默'）。此路径与 run_demo 一致。
    {
        AURORA_TEST_PRINTF("[4] static dispatch path RTL drag-select leftmost char:\n");
        auto a = std::make_shared<Text>(
            TextProps{.content = LocalizedString{"默认14pt文本"}, .text_align = TextAlign::Left, .soft_wrap = true});
        Column col{ColumnProps{.children = {Node{a}}}};
        layout_root(col, 520, 800);
        paint_root(col, 520, 800);
        auto boxes = scan_texts(col);
        auto it = boxes.find("默认14pt文本");
        ck(it != boxes.end(), "text hit");
        if (it != boxes.end()) {
            const Rect &r = it->second;
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
            const float yc = r.origin.y + (r.size.height * 0.5F);
            press(r.origin.x + r.size.width - 2.0F, yc);
            move(r.origin.x + 1.0F, yc);
            move(r.origin.x - 5.0F, yc);  // 越过左边界
            const auto sel = a->selection();
            const std::size_t lo = std::min(sel.first, sel.second);
            const std::size_t hi = std::max(sel.first, sel.second);
            ck(hi - lo == 8, "static dispatch: RTL drag selection covers all 8 codepoints");
            ck(lo == 0, "static dispatch: RTL drag selection starts at index 0 (incl. leftmost '默')");
            release(r.origin.x - 5.0F, yc);
        }
    }

    AURORA_TEST_PRINTF(fails == 0 ? "text_ptr_capture: ALL PASS\n" : "text_ptr_capture: %d FAIL\n", fails);
    AURORA_TEST_CHECK_EQ(fails, 0);
}
}  // namespace sec_text_ptr_capture

AURORA_TEST() {
    sec_text_ptr_capture::run();
}

}  // namespace aurora::test_cases::utest_text_ptr_capture
