/// 测试类型: integration
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: 集成 Text + EventDispatcher（实体实例与静态派发双路径）+ FocusManager：
///           RTL 拖选越过左边界仍覆盖到首字、窗口外释放不丢选区且释放后 Move 不再改变选区、
///           相邻 soft_wrap 文本命中盒互不重叠。

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_text_ptr_capture {

namespace {

using au::BuildContext;
using au::Column;
using au::ColumnProps;
using au::Constraints;
using au::EventDispatcher;
using au::FocusManager;
using au::LocalizedString;
using au::MouseButton;
using au::MouseAction;
using au::MouseEvent;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Row;
using au::RowProps;
using au::Size;
using au::Text;
using au::TextAlign;
using au::TextProps;
using au::Widget;

constexpr float kW = 520.0F;
constexpr float kH = 800.0F;

auto make_left_aligned_text() -> std::shared_ptr<Text> {
    return std::make_shared<Text>(
        TextProps{.content = LocalizedString{"默认14pt文本"}, .text_align = TextAlign::Left, .soft_wrap = true});
}

void layout_root(Widget &root, const float w, const float h) {
    const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
    const BuildContext ctx;
    root.layout(c, ctx);
}

void paint_root(Widget &root, const float w, const float h) {
    Painter p;
    p.begin(static_cast<int>(w), static_cast<int>(h));
    const BuildContext ctx;
    root.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = w, .height = h}}, ctx);
}

/// 扫描各 Text（按显示文本）的可命中盒（哨兵初始化，避免默认 Rect 误判）。
auto scan_texts(Widget &root) -> std::map<std::string, Rect> {
    std::map<std::string, Rect> out;
    for (int y = 0; y < static_cast<int>(kH); ++y) {
        for (int x = 0; x < static_cast<int>(kW); ++x) {
            Widget *h = EventDispatcher::hit_test(root, Point{.x = static_cast<float>(x), .y = static_cast<float>(y)});
            const auto *t = dynamic_cast<Text *>(h);
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

/// 鼠标序列发射器：dispatch 闭包决定走实体实例（指针捕获域独立）还是静态持久单例。
using Sink = std::function<void(MouseAction, float, float)>;

auto press_at(Sink &s, float x, float y) -> void { s(MouseAction::Press, x, y); }
auto move_to(Sink &s, float x, float y) -> void { s(MouseAction::Move, x, y); }
auto release_at(Sink &s, float x, float y) -> void { s(MouseAction::Release, x, y); }

}  // namespace

AURORA_TEST_CASE(rtl_drag_select_reaches_leftmost_char) {
    // 1) RTL 拖选最左字：从右端按下向左拖（越过左边界），须包含索引 0（首字'默'）。
    auto a = make_left_aligned_text();
    Column col{ColumnProps{.children = {Node{a}}}};
    layout_root(col, kW, kH);
    paint_root(col, kW, kH);

    const auto boxes = scan_texts(col);
    const auto it = boxes.find("默认14pt文本");
    AURORA_TEST_REQUIRE_MSG(it != boxes.end(), "text hit box found by scanning");
    const Rect &r = it->second;

    EventDispatcher ed;  // 实体派发器：独立指针捕获域
    FocusManager fm;
    fm.set_root(&col);
    Sink dispatch = [&](MouseAction action, float x, float y) -> void {
        MouseEvent e;
        e.action = action;
        e.button = MouseButton::Left;
        e.position = Point{.x = x, .y = y};
        (void)ed.dispatch_mouse(col, e, &fm);
    };

    const float yc = r.origin.y + (r.size.height * 0.5F);
    press_at(dispatch, r.origin.x + r.size.width - 2.0F, yc);
    move_to(dispatch, r.origin.x + 1.0F, yc);
    move_to(dispatch, r.origin.x - 5.0F, yc);  // 越过左边界
    const auto sel = a->selection();
    const std::size_t lo = std::min(sel.first, sel.second);
    const std::size_t hi = std::max(sel.first, sel.second);
    AURORA_TEST_CHECK_EQ(hi - lo, std::size_t{8});  // "默认14pt文本" 共 8 码点全选
    AURORA_TEST_CHECK_EQ(lo, std::size_t{0});       // 含最左 '默'
    release_at(dispatch, r.origin.x - 5.0F, yc);
}

AURORA_TEST_CASE(release_outside_window_retains_selection) {
    // 2) 窗口外释放：拖选时光标移出根/窗口，释放事件仍须送达并按捕获路径结束选择；
    //    释放后的 Move 不得再改变选区（selecting_ 已结束）。
    auto a = std::make_shared<Text>(TextProps{.content = LocalizedString{"默认14pt文本"}, .soft_wrap = true});
    Column col{ColumnProps{.children = {Node{a}}}};
    layout_root(col, kW, kH);
    paint_root(col, kW, kH);

    const auto boxes = scan_texts(col);
    const auto it = boxes.find("默认14pt文本");
    AURORA_TEST_REQUIRE_MSG(it != boxes.end(), "text hit box found by scanning");
    const Rect &r = it->second;

    EventDispatcher ed;
    FocusManager fm;
    fm.set_root(&col);
    Sink dispatch = [&](MouseAction action, float x, float y) -> void {
        MouseEvent e;
        e.action = action;
        e.button = MouseButton::Left;
        e.position = Point{.x = x, .y = y};
        (void)ed.dispatch_mouse(col, e, &fm);
    };

    const float yc = r.origin.y + (r.size.height * 0.5F);
    press_at(dispatch, r.origin.x + r.size.width - 2.0F, yc);
    move_to(dispatch, r.origin.x + 30.0F, yc);
    AURORA_TEST_CHECK_MSG(a->has_selection(), "selection exists after drag");
    release_at(dispatch, 9999.0F, yc);  // 窗口外释放
    AURORA_TEST_CHECK_MSG(a->has_selection(), "selection retained after release outside window (not lost)");
    const auto s1 = a->selection();
    move_to(dispatch, r.origin.x + 5.0F, yc);  // 释放后再次 move
    AURORA_TEST_CHECK_MSG(a->selection() == s1, "re-move after release does not change selection");
}

AURORA_TEST_CASE(adjacent_soft_wrap_texts_do_not_overlap) {
    // 3) 相邻两 soft_wrap 文本不应重叠：默认 soft_wrap=true 时 Text 仅当确需换行才填满，
    //    短文本按内容宽度上报，兄弟控件可并排且各自可选中。
    auto a = make_left_aligned_text();
    auto b = std::make_shared<Text>(
        TextProps{.content = LocalizedString{"Text控件"}, .text_align = TextAlign::Left, .soft_wrap = true});
    Row row{RowProps{.children = {Node{a}, Node{b}}}};
    layout_root(row, kW, kH);
    paint_root(row, kW, kH);

    const auto boxes = scan_texts(row);
    const auto it_a = boxes.find("默认14pt文本");
    const auto it_b = boxes.find("Text控件");
    AURORA_TEST_REQUIRE_MSG(it_a != boxes.end() && it_b != boxes.end(), "both Text widgets are hittable");
    const Rect &ra = it_a->second;
    const Rect &rb = it_b->second;
    const bool overlap = rb.origin.x < ra.origin.x + ra.size.width && ra.origin.x < rb.origin.x + rb.size.width;
    AURORA_TEST_CHECK_MSG(!overlap, "two Text hit boxes do not overlap (second is selectable)");
}

AURORA_TEST_CASE(static_dispatch_path_rtl_drag_reaches_leftmost_char) {
    // 4) 静态 EventDispatcher::dispatch 路径（每个事件委托进程内持久实例 → 保留指针捕获）：
    //    与 run_demo 一致的真实派发路径下，RTL 拖选越过左边界仍延伸到索引 0（含最左'默'）。
    auto a = make_left_aligned_text();
    Column col{ColumnProps{.children = {Node{a}}}};
    layout_root(col, kW, kH);
    paint_root(col, kW, kH);

    const auto boxes = scan_texts(col);
    const auto it = boxes.find("默认14pt文本");
    AURORA_TEST_REQUIRE_MSG(it != boxes.end(), "text hit box found by scanning");
    const Rect &r = it->second;

    FocusManager fm;
    fm.set_root(&col);
    Sink dispatch = [&](MouseAction action, float x, float y) -> void {
        MouseEvent e;
        e.action = action;
        e.button = MouseButton::Left;
        e.position = Point{.x = x, .y = y};
        (void)EventDispatcher::dispatch(col, e, &fm);
    };

    const float yc = r.origin.y + (r.size.height * 0.5F);
    press_at(dispatch, r.origin.x + r.size.width - 2.0F, yc);
    move_to(dispatch, r.origin.x + 1.0F, yc);
    move_to(dispatch, r.origin.x - 5.0F, yc);  // 越过左边界
    const auto sel = a->selection();
    const std::size_t lo = std::min(sel.first, sel.second);
    const std::size_t hi = std::max(sel.first, sel.second);
    AURORA_TEST_CHECK_EQ(hi - lo, std::size_t{8});  // 静态派发：8 码点全选
    AURORA_TEST_CHECK_EQ(lo, std::size_t{0});       // 静态派发：含最左 '默'
    release_at(dispatch, r.origin.x - 5.0F, yc);
}

}  // namespace aurora::test_cases::itest_text_ptr_capture
