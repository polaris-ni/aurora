/// 测试类型: unit
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: utest_text_focus 单元测试
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

// （自 utest_text.cpp 拆分：失焦清除选区/高亮段）

namespace aurora::test_cases::utest_text_focus {

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
namespace sec_text_focus_clear {

void run() {
    // 选区高亮为半透明蓝色矩形；ClearType 字形边缘的蓝/红彩色羽化会干扰蓝色检测。
    // 改用与背景无关的超采样抗锯齿，使「失焦后高亮应消失」的判定只反映选区本身。
    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);

    auto txt = std::make_shared<Text>("点击按钮改变计数（运行日志可见）");
    auto btn = std::make_shared<Button>();
    Column col{ColumnProps{.children = {Node{txt}, Node{btn}}}};
    col.set_focusable(false);  // 容器不抢占焦点，焦点应落在叶控件上

    BuildContext ctx;
    col.mount(ctx);
    Constraints cc;
    cc.min = Size{.width = 0.0F, .height = 0.0F};
    cc.max = Size{.width = 640.0F, .height = 480.0F};
    col.layout(cc, ctx);

    Painter p;
    p.begin(640, 480);
    col.paint(p, Rect{.origin = Point{}, .size = Size{.width = 640.0F, .height = 480.0F}},
              ctx);  // 填充各叶控件的显示文本

    FocusManager fm;
    fm.set_root(&col);

    // 1) 点击 Text 建立选区（Press + Move）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    const Rect tb = col.child_nodes()[0].bounds();
    const Point tc{.x = tb.origin.x + 2.0F, .y = tb.origin.y + (tb.size.height / 2.0F)};

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.position = tc;
    EventDispatcher::dispatch(col, press, &fm);
    AURORA_TEST_CHECK(fm.focused() == txt.get());  // 点击 Text 使其获焦

    MouseEvent move;
    move.action = MouseAction::Move;
    move.button = MouseButton::Left;
    move.position = Point{.x = tb.origin.x + tb.size.width - 2.0F, .y = tc.y};
    EventDispatcher::dispatch(col, move, &fm);

    AURORA_TEST_CHECK(txt->has_selection());  // 选区已建立
    AURORA_LOG_INFO("test", "[1] text selection established via dispatch OK");

    // 2) 点击按钮 → 焦点转移到按钮 → Text 失焦、选区清除。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    const Rect bb = col.child_nodes()[1].bounds();
    const Point bc{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};
    MouseEvent click_btn;
    click_btn.action = MouseAction::Press;
    click_btn.button = MouseButton::Left;
    click_btn.position = bc;
    EventDispatcher::dispatch(col, click_btn, &fm);

    AURORA_TEST_CHECK(!txt->has_selection());  // 选区被清除
    AURORA_TEST_CHECK(fm.focused() == btn.get());  // 焦点转移到按钮
    AURORA_LOG_INFO("test", "[2] clicking button blurs text and clears selection OK");

    // 3) 清背景重绘，确认文本选区高亮像素已消失。
    // 仅扫描文本自身包围盒——按钮默认背景为蓝色（Color::blue()），扫全画布会误命中按钮背景。
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 640, .height = 480}}, Color::white());
    col.paint(p, Rect{.origin = Point{}, .size = Size{.width = 640.0F, .height = 480.0F}}, ctx);
    int hl = 0;
    int x0 = static_cast<int>(std::floor(tb.origin.x));
    int y0 = static_cast<int>(std::floor(tb.origin.y));
    int x1 = static_cast<int>(std::ceil(tb.origin.x + tb.size.width));
    int y1 = static_cast<int>(std::ceil(tb.origin.y + tb.size.height));
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, p.width());
    y1 = std::min(y1, p.height());
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Color c = p.get_pixel(x, y);
            if (static_cast<int>(c.b) - static_cast<int>(c.r) > 30) {
                ++hl;
            }
        }
    }
    AURORA_TEST_CHECK(hl == 0);  // 文本选区高亮已清除
    AURORA_LOG_INFO("test", "[3] highlight cleared after blur OK");

    // 4) 点击不可获焦容器（col.focusable=false，命中链无可获焦控件）→ 清焦点、选区消失。
    {
        // 先重新建立选区与焦点。
        MouseEvent p2 = press;
        EventDispatcher::dispatch(col, p2, &fm);
        MouseEvent m2 = move;
        EventDispatcher::dispatch(col, m2, &fm);
        MouseEvent r2 = move;
        r2.action = MouseAction::Release;
        EventDispatcher::dispatch(col, r2, &fm);
        AURORA_TEST_CHECK(txt->has_selection());
        AURORA_TEST_CHECK(fm.focused() == txt.get());
        // 点在容器内、但不在 Text/按钮上（两控件之间/下方的空白带）。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Rect bb2 = col.child_nodes()[1].bounds();
        MouseEvent blank;
        blank.action = MouseAction::Press;
        blank.button = MouseButton::Left;
        blank.position =
            Point{.x = bb2.origin.x + (bb2.size.width / 2.0F), .y = bb2.origin.y + bb2.size.height + 40.0F};
        EventDispatcher::dispatch(col, blank, &fm);
        AURORA_TEST_CHECK(fm.focused() == nullptr);  // 整条命中链不可获焦 → 清焦点
        AURORA_TEST_CHECK(!txt->has_selection());  // 旧选区随失焦清除
        AURORA_LOG_INFO("test", "[4] clicking non-focusable container area blurs text OK");
    }

    // 5) 点击根外空白（命中链为空）→ 同样清焦点、选区消失。
    {
        MouseEvent p3 = press;
        EventDispatcher::dispatch(col, p3, &fm);
        MouseEvent m3 = move;
        EventDispatcher::dispatch(col, m3, &fm);
        MouseEvent r3 = move;
        r3.action = MouseAction::Release;
        EventDispatcher::dispatch(col, r3, &fm);
        AURORA_TEST_CHECK(txt->has_selection());
        MouseEvent outside;
        outside.action = MouseAction::Press;
        outside.button = MouseButton::Left;
        outside.position = Point{.x = col.size().width + 100.0F, .y = col.size().height + 100.0F};
        EventDispatcher::dispatch(col, outside, &fm);
        AURORA_TEST_CHECK(fm.focused() == nullptr);  // 点击空白 → blur
        AURORA_TEST_CHECK(!txt->has_selection());
        AURORA_LOG_INFO("test", "[5] clicking empty space (no hit) blurs text OK");
    }

    AURORA_LOG_INFO("test", "ALL TEXT FOCUS CLEAR TESTS PASSED");
}
}  // namespace sec_text_focus_clear

AURORA_TEST() {
    sec_text_focus_clear::run();
}

}  // namespace aurora::test_cases::utest_text_focus
