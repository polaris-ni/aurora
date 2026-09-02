#include <memory>
#include <string>

#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/event/keycode.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text_input.h"
#include "aurora/widget/widget.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::EventDispatcher;
using aurora::FocusDirection;
using aurora::FocusManager;
using aurora::KeyAction;
using aurora::KeyCode;
using aurora::KeyEvent;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::TextInput;
using aurora::TextInputEvent;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace {
/// @brief 焦点变更追踪控件（验证 FocusManager 的获焦/失焦通知）。
class FocusSpy : public aurora::LeafWidget {
  public:
    int gained = 0;
    int lost = 0;
    void on_focus_change(bool focused) override {
        if (focused) {
            ++gained;
        } else {
            ++lost;
        }
    }
    void collect_signals(std::vector<aurora::SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FocusSpy"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "FocusSpy", .children_policy = "none" };
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = 40.0f, .height = 20.0f });
    }
    void on_paint(aurora::Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) override {}
};
} // namespace

AURORA_TEST() {
    // 构造一颗含多个可聚焦叶控件的树；容器本身不参与焦点序。
    auto ti_a = std::make_shared<TextInput>(aurora::TextInputProps{ .value = "", .placeholder = "A" });
    auto ti_b = std::make_shared<TextInput>(aurora::TextInputProps{ .value = "", .placeholder = "B" });
    auto btn = std::make_shared<Button>();
    auto spy = std::make_shared<FocusSpy>();

    Column col{ ColumnProps{ .children = { Node{ ti_a }, Node{ ti_b }, Node{ btn }, Node{ spy } } } };
    col.set_focusable(false); // 容器不抢占焦点

    BuildContext ctx;
    col.mount(ctx);
    Constraints cc;
    cc.min = Size{ .width = 0.0f, .height = 0.0f };
    cc.max = Size{ .width = 640.0f, .height = 480.0f };
    col.layout(cc, ctx);

    aurora::Painter p;
    p.begin(640, 480);
    col.paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 640.0f, .height = 480.0f } },
              ctx);

    FocusManager fm;
    fm.set_root(&col);

    // 1) 初始无焦点。
    AURORA_TEST_CHECK(fm.focused() == nullptr);

    // 2) 首次 move_focus 落到第一个候选（tiA，遍历序在前、tabIndex 相同）。
    AURORA_TEST_CHECK(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK(fm.focused() == ti_a.get());
    AURORA_TEST_CHECK(ti_a->is_focused());

    // 3) 沿 Tab 序前进：tiA -> tiB -> btn -> spy -> tiA(循环)。
    fm.move_focus(FocusDirection::Forward);
    AURORA_TEST_CHECK(fm.focused() == ti_b.get());
    fm.move_focus(FocusDirection::Forward);
    AURORA_TEST_CHECK(fm.focused() == btn.get());
    fm.move_focus(FocusDirection::Forward);
    AURORA_TEST_CHECK(fm.focused() == spy.get());
    fm.move_focus(FocusDirection::Forward);
    AURORA_TEST_CHECK(fm.focused() == ti_a.get()); // 循环回起点

    // 4) 焦点变更通知：spy 在序列中获焦一次、失焦一次。
    AURORA_TEST_CHECK(spy->gained == 1);
    AURORA_TEST_CHECK(spy->lost == 1);

    // 5) Shift+Tab 后退（spy 再次获焦，gained 增为 2）。
    fm.move_focus(FocusDirection::Backward);
    AURORA_TEST_CHECK(fm.focused() == spy.get());
    AURORA_TEST_CHECK(spy->gained == 2);

    // 6) 键盘经 FocusManager 派发到焦点 widget：文本输入 + 退格。
    fm.set_focus(ti_b.get());
    TextInputEvent te;
    te.text = "x";
    AURORA_TEST_CHECK(aurora::EventDispatcher::dispatch(col, te, fm));
    te.text = "y";
    AURORA_TEST_CHECK(aurora::EventDispatcher::dispatch(col, te, fm));
    AURORA_TEST_CHECK(ti_b->value() == "xy");

    KeyEvent back;
    back.action = KeyAction::Down;
    back.key = static_cast<int>(KeyCode::Backspace);
    AURORA_TEST_CHECK(aurora::EventDispatcher::dispatch(col, back, fm)); // 派发到 tiB
    AURORA_TEST_CHECK(ti_b->value() == "x");

    // 7) 点击 TextInput 经 request_focus 获焦。
    const Rect ab = col.child_nodes()[0].bounds();
    const Point ac{ .x = ab.origin.x + (ab.size.width / 2.0f), .y = ab.origin.y + (ab.size.height / 2.0f) };
    MouseEvent click;
    click.position = ac;
    click.action = MouseAction::Press;
    click.button = MouseButton::Left;
    EventDispatcher::dispatch(col, click, &fm);
    AURORA_TEST_CHECK(fm.focused() == ti_a.get());

    // 8) Tab 键经事件分发器触发焦点移动（而非被焦点 widget 消费）。
    KeyEvent tab;
    tab.action = KeyAction::Down;
    tab.key = static_cast<int>(KeyCode::Tab);
    AURORA_TEST_CHECK(aurora::EventDispatcher::dispatch(col, tab, fm)); // 焦点前进到 tiB
    AURORA_TEST_CHECK(fm.focused() == ti_b.get());

    // 9) Shift+Tab 后退（带修饰键位）。
    KeyEvent shift_tab = tab;
    shift_tab.modifiers = aurora::ModifierKey::Shift;
    AURORA_TEST_CHECK(aurora::EventDispatcher::dispatch(col, shift_tab, fm));
    AURORA_TEST_CHECK(fm.focused() == ti_a.get());

    // 10) 不可见控件不参与焦点序：隐藏 tiA 后移动应跳过它。
    ti_a->show = false;
    fm.set_focus(ti_b.get());
    fm.move_focus(FocusDirection::Forward); // tiB -> btn -> spy -> tiB（跳过隐藏的 tiA）
    AURORA_TEST_CHECK(fm.focused() != ti_a.get());

    AURORA_LOG_INFO("test", "focus_test: OK");
}
