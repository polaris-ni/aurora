/// 测试类型: integration
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: 集成 Text + Button + Column + EventDispatcher + FocusManager + Painter：
///           点击 Text 获焦并拖选建立选区、点击按钮转移焦点且选区清除、失焦后高亮像素消失、
///           点击不可获焦区域与根外空白均清焦点与选区。

#include <algorithm>
#include <cmath>
#include <memory>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_text_focus {

namespace {

using au::BuildContext;
using au::Button;
using au::Color;
using au::Column;
using au::ColumnProps;
using au::Constraints;
using au::EventDispatcher;
using au::FocusManager;
using au::MouseAction;
using au::MouseButton;
using au::MouseEvent;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;
using au::Text;

constexpr float AURORA_TEST_TEXT_FOCUS_W = 640.0F;
constexpr float AURORA_TEST_TEXT_FOCUS_H = 480.0F;

/// 选区高亮为半透明蓝色矩形；ClearType 字形边缘的红/蓝彩色羽化会干扰蓝色检测，
/// 统一改用与背景无关的超采样抗锯齿，使「失焦后高亮应消失」的判定只反映选区本身。
void use_supersample_aa() { render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample); }

/// 一套已挂载/布局/绘制过的 Text+Button 舞台：填充 display_text_ 并备好焦点管理与派发。
struct Stage {
    std::shared_ptr<Text> txt;
    std::shared_ptr<Button> btn;
    std::shared_ptr<Column> col;
    BuildContext ctx;
    Painter p;
    FocusManager fm;

    Stage() {
        txt = std::make_shared<Text>("点击按钮改变计数（运行日志可见）");
        btn = std::make_shared<Button>();
        col = std::make_shared<Column>(ColumnProps{.children = {Node{txt}, Node{btn}}});
        col->set_focusable(false);  // 容器不抢占焦点，焦点应落在叶控件上
        col->mount(ctx);
        const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F},
                             .max = Size{.width = AURORA_TEST_TEXT_FOCUS_W, .height = AURORA_TEST_TEXT_FOCUS_H}};
        col->layout(cc, ctx);
        p.begin(static_cast<int>(AURORA_TEST_TEXT_FOCUS_W), static_cast<int>(AURORA_TEST_TEXT_FOCUS_H));
        repaint_on_white();
        fm.set_root(col.get());
    }

    /// 白底重绘整棵子树。
    void repaint_on_white() {
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0},
                         .size = Size{.width = AURORA_TEST_TEXT_FOCUS_W, .height = AURORA_TEST_TEXT_FOCUS_H}},
                    Color::white());
        col->paint(p,
                   Rect{.origin = Point{},
                        .size = Size{.width = AURORA_TEST_TEXT_FOCUS_W, .height = AURORA_TEST_TEXT_FOCUS_H}},
                   ctx);
    }

    void press(float x, float y) { dispatch(MouseAction::Press, x, y); }
    void move(float x, float y) { dispatch(MouseAction::Move, x, y); }
    void release(float x, float y) { dispatch(MouseAction::Release, x, y); }

    void dispatch(MouseAction action, float x, float y) {
        MouseEvent e;
        e.action = action;
        e.button = MouseButton::Left;
        e.position = Point{.x = x, .y = y};
        EventDispatcher::dispatch(*col, e, &fm);
    }

    /// 在 Text 上「右端按下 → 左侧拖动 → 抬起」建立完整选区。
    void establish_selection() {
        const Rect tb = text_bounds();
        const float yc = tb.origin.y + (tb.size.height / 2.0F);
        press(tb.origin.x + tb.size.width - 2.0F, yc);
        move(tb.origin.x + 2.0F, yc);
        release(tb.origin.x + 2.0F, yc);
    }

    [[nodiscard]] auto text_bounds() const -> Rect { return col->child_nodes().at(0).bounds(); }
    [[nodiscard]] auto button_bounds() const -> Rect { return col->child_nodes().at(1).bounds(); }
};

/// 统计 [r] 盒内「蓝色染色」像素数（b - r > 30，即选区高亮）。
auto count_blue_in(const Painter& p, const Rect& r) -> int {
    int x0 = std::max(static_cast<int>(std::floor(r.origin.x)), 0);
    int y0 = std::max(static_cast<int>(std::floor(r.origin.y)), 0);
    const int x1 = std::min(static_cast<int>(std::ceil(r.origin.x + r.size.width)), p.width());
    const int y1 = std::min(static_cast<int>(std::ceil(r.origin.y + r.size.height)), p.height());
    int hl = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Color c = p.get_pixel(x, y);
            if (static_cast<int>(c.b) - static_cast<int>(c.r) > 30) {
                ++hl;
            }
        }
    }
    return hl;
}

}  // namespace

AURORA_TEST_CASE(click_text_focuses_and_drag_establishes_selection) {
    use_supersample_aa();
    Stage st;

    // 点击 Text 使其获焦。
    const Rect tb = st.text_bounds();
    const Point tc{.x = tb.origin.x + 2.0F, .y = tb.origin.y + (tb.size.height / 2.0F)};
    st.press(tc.x, tc.y);
    AURORA_TEST_CHECK_MSG(st.fm.focused() == st.txt.get(), "clicking Text focuses it");

    // 按下后拖动 → 选区建立。
    st.move(tb.origin.x + tb.size.width - 2.0F, tc.y);
    AURORA_TEST_CHECK_MSG(st.txt->has_selection(), "drag after press establishes selection");
}

AURORA_TEST_CASE(click_button_blurs_text_and_clears_selection) {
    use_supersample_aa();
    Stage st;

    st.establish_selection();
    AURORA_TEST_REQUIRE_MSG(st.txt->has_selection(), "precondition: selection established on Text");

    // 点击按钮 → 焦点转移到按钮 → Text 失焦、选区清除。
    const Rect bb = st.button_bounds();
    st.press(bb.origin.x + (bb.size.width / 2.0F), bb.origin.y + (bb.size.height / 2.0F));
    AURORA_TEST_CHECK_MSG(!st.txt->has_selection(), "selection cleared after button press");
    AURORA_TEST_CHECK_MSG(st.fm.focused() == st.btn.get(), "focus transferred to button");
}

AURORA_TEST_CASE(blur_clears_selection_highlight_pixels) {
    use_supersample_aa();
    Stage st;

    st.establish_selection();
    AURORA_TEST_REQUIRE_MSG(st.txt->has_selection(), "precondition: selection established on Text");

    // 点击按钮使 Text 失焦，清背景重绘，确认文本选区高亮像素已消失。
    // 仅扫描文本自身包围盒——按钮默认背景为蓝色，扫全画布会误命中按钮背景。
    const Rect bb = st.button_bounds();
    st.press(bb.origin.x + (bb.size.width / 2.0F), bb.origin.y + (bb.size.height / 2.0F));
    AURORA_TEST_REQUIRE_MSG(!st.txt->has_selection(), "precondition: text blurred by button press");

    st.repaint_on_white();
    AURORA_TEST_CHECK_EQ(count_blue_in(st.p, st.text_bounds()), 0);
}

AURORA_TEST_CASE(click_non_focusable_area_blurs_text) {
    use_supersample_aa();
    Stage st;

    st.establish_selection();
    AURORA_TEST_REQUIRE_MSG(st.txt->has_selection(), "precondition: selection established on Text");
    AURORA_TEST_REQUIRE_MSG(st.fm.focused() == st.txt.get(), "precondition: Text focused");

    // 点在容器内容之外、Text/按钮下方的空白带：整条命中链不可获焦 → 清焦点、选区随失焦清除。
    const Rect bb = st.button_bounds();
    st.press(bb.origin.x + (bb.size.width / 2.0F), bb.origin.y + bb.size.height + 40.0F);
    AURORA_TEST_CHECK_MSG(st.fm.focused() == nullptr, "clicking non-focusable area clears focus");
    AURORA_TEST_CHECK_MSG(!st.txt->has_selection(), "stale selection cleared on blur");
}

AURORA_TEST_CASE(click_outside_root_blurs_text) {
    use_supersample_aa();
    Stage st;

    st.establish_selection();
    AURORA_TEST_REQUIRE_MSG(st.txt->has_selection(), "precondition: selection established on Text");

    // 点击根外空白（命中链为空）→ 同样清焦点、选区消失。
    st.press(st.col->size().width + 100.0F, st.col->size().height + 100.0F);
    AURORA_TEST_CHECK_MSG(st.fm.focused() == nullptr, "clicking empty space outside root clears focus");
    AURORA_TEST_CHECK_MSG(!st.txt->has_selection(), "selection cleared after blur");
}

}  // namespace aurora::test_cases::itest_text_focus
