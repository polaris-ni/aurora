// 目标源单元：event/dispatcher.h + src/aurora/event/dispatcher.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_event.cpp
//   - test_click.cpp
//   - test_hover.cpp
//   - test_file_drop.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "test_harness.h"

using aurora::Application;
using aurora::BuildContext;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::EventDispatcher;
using aurora::FileDropEvent;
using aurora::LocalizedString;
using aurora::Modifier;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Scene;
using aurora::Size;
using aurora::Text;
using aurora::Widget;

namespace aurora::tests::sec_event {

static void run() {
    BuildContext ctx;
    Constraints c;
    c.max = Size{.width = 300, .height = 300};

    // A) Text + Clickable 修饰：此前 Clickable.on_click 从未被调用，本测试验证其修复。
    {
        bool clicked = false;
        Text txt;
        txt.content.set(LocalizedString{"tap"});
        txt.modifier.set(Modifier{}.clickable([&clicked]() -> void { clicked = true; }));

        Column col{ColumnProps{.children = {Node{std::move(txt)}}}};
        col.layout(c, ctx);

        MouseEvent e;
        e.position = Point{.x = 5.0F, .y = 5.0F};
        e.action = MouseAction::Press;
        const bool hit = EventDispatcher::dispatch(col, e);
        AURORA_TEST_CHECK(hit);
        AURORA_TEST_CHECK(e.is_handled_);

        // Clickable.on_click 在「按下+抬起」完整点击的「抬起」时触发（tap 语义，同 Button）。
        MouseEvent r;
        r.position = Point{.x = 5.0F, .y = 5.0F};
        r.action = MouseAction::Release;
        EventDispatcher::dispatch(col, r);
        AURORA_TEST_CHECK(clicked);
        AURORA_LOG_INFO("test", "[A] Text+Clickable dispatch OK");
    }

    // B) Button.on_click 仍经 activate 路径触发（回归保护）。
    {
        bool clicked = false;
        aurora::Button btn;
        btn.label.set(LocalizedString{"go"});
        btn.on_click = [&clicked]() -> void { clicked = true; };

        Column col{ColumnProps{.children = {Node{std::move(btn)}}}};
        col.layout(c, ctx);

        MouseEvent e;
        e.position = Point{.x = 5.0F, .y = 5.0F};
        e.action = MouseAction::Press;
        const bool hit_press = EventDispatcher::dispatch(col, e);
        // 点击在「按下+抬起」完整序列的抬起时触发（spec：释放 → 触发 click）。
        MouseEvent r;
        r.position = Point{.x = 5.0F, .y = 5.0F};
        r.action = MouseAction::Release;
        EventDispatcher::dispatch(col, r);
        AURORA_TEST_CHECK(hit_press);
        AURORA_TEST_CHECK(clicked);
        AURORA_LOG_INFO("test", "[B] Button.on_click dispatch OK");
    }

    // C) 未命中空白区域：无目标，返回 false、is_handled_ 保持 false。
    {
        Text txt;
        txt.content.set(LocalizedString{"x"});
        Column col{ColumnProps{.children = {Node{std::move(txt)}}}};
        col.layout(c, ctx);

        MouseEvent e;
        e.position = Point{.x = 299.0F, .y = 299.0F};
        e.action = MouseAction::Press;
        const bool hit = EventDispatcher::dispatch(col, e);
        AURORA_TEST_CHECK(!hit);
        AURORA_TEST_CHECK(!e.is_handled_);
        AURORA_LOG_INFO("test", "[C] miss returns false OK");
    }

    AURORA_LOG_INFO("test", "ALL EVENT TESTS PASSED");
}
}  // namespace aurora::tests::sec_event

namespace aurora::tests::sec_click {
using aurora::BuildContext;
using aurora::Button;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::EventDispatcher;
using aurora::LocalizedString;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Point;
using aurora::Reactive;
using aurora::Rect;
using aurora::Size;
using aurora::State;
using aurora::Text;

static void run() {
    State count{0};

    auto t = std::make_shared<Text>();
    t->content = LocalizedString{"Aurora GLFW Demo"};
    auto c = std::make_shared<Text>();
    c->content = LocalizedString{"count = 0"};
    auto inp = std::make_shared<Text>();
    inp->content = LocalizedString{"type here..."};
    auto btn = std::make_shared<Button>();
    btn->label = Reactive{LocalizedString{"+1"}};
    btn->on_click = [&count]() -> void { count.set(count.get() + 1); };

    Column col{ColumnProps{.children = {Node{t}, Node{c}, Node{inp}, Node{btn}}}};

    BuildContext ctx;
    col.mount(ctx);
    Constraints cc;
    cc.min = Size{.width = 0.0F, .height = 0.0F};
    cc.max = Size{.width = 640.0F, .height = 480.0F};
    col.layout(cc, ctx);

    Painter p;
    p.begin(640, 480);
    col.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 640.0F, .height = 480.0F}}, ctx);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    const Rect bb = col.child_nodes()[3].bounds();
    AURORA_TEST_PRINTF("button bounds: (%.1f, %.1f) %.1fx%.1f\n", bb.origin.x, bb.origin.y, bb.size.width,
                       bb.size.height);

    const Point center{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};

    MouseEvent press;
    press.position = center;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    const bool hp = EventDispatcher::dispatch(col, press);

    // 悬停移动：不应触发点击（此前该 bug 会让 count 在鼠标移过按钮时递增）
    MouseEvent move = press;
    move.action = MouseAction::Move;
    const bool hm = EventDispatcher::dispatch(col, move);

    MouseEvent release = press;
    release.action = MouseAction::Release;
    const bool hr = EventDispatcher::dispatch(col, release);

    AURORA_TEST_PRINTF("hit press=%d move=%d release=%d count=%d\n", hp, hm, hr, count.get());

    // 一次完整点击（按下→抬起）应仅递增一次，且悬停移动不得递增。
    AURORA_TEST_CHECK(hp && hm && hr && count.get() == 1);
}
}  // namespace aurora::tests::sec_click

namespace aurora::tests::sec_hover {
namespace au = aurora;

namespace {
auto make_move(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Move;
    e.position = Point{.x = x, .y = y};
    return e;
}
}  // namespace

static void run() {
    // 布局：Row 内两个 20dp Checkbox（位于 [0,20) 与 [20,40)）。
    auto cb1 = std::make_shared<Checkbox>(Reactive{false});
    auto cb2 = std::make_shared<Checkbox>(Reactive{true});
    Node root{Row{Node{cb1}, Node{cb2}}};
    BuildContext ctx;
    root.widget().layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 200, .height = 100}},
                         ctx);
    root.set_bounds(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 200, .height = 100}});

    EventDispatcher d;

    // ---- 1. 初始无悬停 ----
    AURORA_TEST_CHECK_FALSE(cb1->hovered());
    AURORA_TEST_CHECK_FALSE(cb2->hovered());

    // ---- 2. 移到 cb1 上：cb1 悬停、cb2 不 ----
    auto m1 = make_move(10.0F, 10.0F);
    (void)d.dispatch_mouse(root.widget(), m1);
    AURORA_TEST_CHECK_TRUE(cb1->hovered());
    AURORA_TEST_CHECK_FALSE(cb2->hovered());

    // ---- 3. 移到 cb2 上：cb1 离开、cb2 进入 ----
    auto m2 = make_move(30.0F, 10.0F);
    (void)d.dispatch_mouse(root.widget(), m2);
    AURORA_TEST_CHECK_FALSE(cb1->hovered());
    AURORA_TEST_CHECK_TRUE(cb2->hovered());

    // ---- 4. 移到空白（窗外合成 Move）：全部清除 ----
    auto m3 = make_move(-10000.0F, -10000.0F);
    (void)d.dispatch_mouse(root.widget(), m3);
    AURORA_TEST_CHECK_FALSE(cb1->hovered());
    AURORA_TEST_CHECK_FALSE(cb2->hovered());
}
}  // namespace aurora::tests::sec_hover

namespace aurora::tests::sec_file_drop {

namespace {
// 捕获文件拖放事件的测试控件。
class DropCatcher : public Widget {
  public:
    bool got_ = false;
    std::vector<std::string> paths_;
    Point pos_{};

    [[nodiscard]] auto type_name() const -> const char * override { return "DropCatcher"; }

    auto on_file_drop(FileDropEvent &e) -> void override {
        got_ = true;
        paths_ = e.paths;
        pos_ = e.position;
        e.is_handled_ = true;
    }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        return Size{.width = 100, .height = 100};
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
    // 命中测试：作为可拖放目标，落点在自身盒内即命中（默认 Widget 返回 nullptr 不命中）。
    auto on_hit_test(const Point & /*local*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/)
        -> Widget * override {
        return this;
    }
};
}  // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_file_drop ===\n");

    constexpr BuildContext ctx;
    Constraints c;
    c.max = Size{.width = 300, .height = 300};

    const auto catcher = std::make_shared<DropCatcher>();
    Column col{ColumnProps{.children = {Node{catcher}}}};
    col.layout(c, ctx);

    // 1) 命中落点 (5,5) 处控件应收到事件，路径与坐标正确。
    {
        FileDropEvent e;
        e.position = Point{.x = 5.0F, .y = 5.0F};
        e.paths = {"/tmp/a.png", "/tmp/b.txt"};
        const bool hit = EventDispatcher::dispatch(col, e);
        AURORA_TEST_CHECK(hit);
        AURORA_TEST_CHECK(catcher->got_);
        AURORA_TEST_CHECK(catcher->paths_.size() == 2);
        AURORA_TEST_CHECK(catcher->paths_.at(0) == "/tmp/a.png");
        AURORA_TEST_CHECK(catcher->paths_.at(1) == "/tmp/b.txt");
        AURORA_TEST_CHECK(catcher->pos_.x == 5.0F && catcher->pos_.y == 5.0F);
    }

    // 2) 落点在外（(500,500)）应未命中（返回 false、控件未收到）。
    {
        FileDropEvent e;
        e.position = Point{.x = 500.0F, .y = 500.0F};
        e.paths = {"/tmp/x"};
        catcher->got_ = false;
        const bool miss = EventDispatcher::dispatch(col, e);
        AURORA_TEST_CHECK(!miss);
        AURORA_TEST_CHECK(!catcher->got_);
    }

    // 3) Application 级便捷派发：经 dispatch 路径触发 on_file_drop。
    {
        const auto catcher2 = std::make_shared<DropCatcher>();
        const auto col2 = std::make_shared<Column>(ColumnProps{.children = {Node{catcher2}}});
        col2->layout(c, ctx);
        Application app(Scene{Node{col2}}, 200, 200);
        catcher2->got_ = false;
        app.dispatch_file_drop({"/home/file.dat"}, 5.0F, 5.0F);
        AURORA_TEST_CHECK(catcher2->got_);
        AURORA_TEST_CHECK(catcher2->paths_.size() == 1);
        AURORA_TEST_CHECK(catcher2->paths_.at(0) == "/home/file.dat");
    }
}
}  // namespace aurora::tests::sec_file_drop

AURORA_TEST() {
    aurora::tests::sec_event::run();
    aurora::tests::sec_click::run();
    aurora::tests::sec_hover::run();
    aurora::tests::sec_file_drop::run();
}