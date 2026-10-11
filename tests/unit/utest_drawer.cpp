/// 测试类型: unit
/// 目标单元: include/aurora/widget/drawer.h
/// 测试说明: 覆盖 drawer.h 内三个控件（Drawer / ProgressDialog / PageView）的全部内联方法。
/// Drawer：默认/聚合构造（panel_width 非正归一为 240）、type_name、describe、信号收集、is_open/open_state/
/// is_permanent/panel_width 取值、set_open（永久模式早退 + 变更分支触发 on_toggle）、toggle、set_permanent/
/// set_on_toggle、on_pointer_event（打开态点击遮罩外关闭）、wants_click（打开且非永久为真）、panel_rect
/// （左/右停靠两分支）、serialize/deserialize_props、for_each_child/child_nodes（内容+面板）、
/// on_mount/on_unmount/tick 转发子节点。
/// ProgressDialog：默认/聚合构造、type_name/describe/信号收集、show/close/is_open、set_progress（负→-1、
/// 越界夹 [0,1]、中段）、set_message/message、cancel（可取消→关+回调；不可取消→no-op）、set_on_cancel、
/// on_pointer_event（打开态模态吞点击 / 未打开 / 不可取消下沉基类三分支）、wants_click、serialize/deserialize。
/// PageView：默认/聚合构造（初始越界夹取、空列表取 0）、type_name/describe/信号收集、page_count/current/
/// current_page、go_to（有效/same/越界分支）、next/prev（末/首页越界忽略）、set_show_indicator/
/// set_on_page_change、on_pointer_event（Press 记录起点、Release 阈值翻页 next/prev、无 drag 下沉、Move 下沉）、
/// wants_click、serialize/deserialize。
/// 判据：set_open 值变化时回调 on_toggle；打开态点击遮罩外关闭并置 is_handled；进度归一正确；可取消性分流正确；
/// 翻页与拖拽阈值切页符合语义；序列/反序列化往返正确。

#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/drawer.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_drawer {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

class LeafProbe final : public Widget {
  public:
    int mounts = 0;
    int unmounts = 0;
    [[nodiscard]] auto type_name() const -> const char * override { return "LeafProbe"; }
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { (void)out; }
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 10.0F, .height = 10.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*b*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_mount(const BuildContext &ctx) -> void override {
        Widget::on_mount(ctx);
        ++mounts;
    }
    auto on_unmount(const BuildContext &ctx) -> void override {
        Widget::on_unmount(ctx);
        ++unmounts;
    }
};

}  // namespace

AURORA_TEST_CASE(api_surface_and_open_close) {
    auto content = std::make_shared<LeafProbe>();
    auto panel = std::make_shared<LeafProbe>();
    // panel_width 非正 → 归一为 240。
    Drawer d{Node{content}, Node{panel}, DrawerSide::Left, -5.0F};
    AURORA_TEST_CHECK_EQ(std::string(d.type_name()), "Drawer");
    AURORA_TEST_CHECK_EQ(d.panel_width(), 240.0F);
    AURORA_TEST_CHECK_EQ(d.is_open(), false);
    AURORA_TEST_CHECK_EQ(d.is_permanent(), false);
    AURORA_TEST_CHECK_EQ(d.child_nodes().size(), 2U);
    int toggles = 0;
    d.set_on_toggle([&toggles](bool v) { toggles += v ? 1 : -1; });

    // set_open(true)：值变化 → 触发 on_toggle(true)。
    d.set_open(true);
    AURORA_TEST_CHECK_EQ(d.is_open(), true);
    AURORA_TEST_CHECK_EQ(toggles, 1);
    AURORA_TEST_CHECK_EQ(d.wants_click(), true);  // 打开且非永久

    // toggle：翻回关闭 → on_toggle(false)。
    d.toggle();
    AURORA_TEST_CHECK_EQ(d.is_open(), false);
    AURORA_TEST_CHECK_EQ(toggles, 0);
    AURORA_TEST_CHECK_EQ(d.wants_click(), false);

    // 永久模式：set_open 被忽略（无开合语义）。
    d.set_permanent(true);
    AURORA_TEST_CHECK_EQ(d.is_permanent(), true);
    d.set_open(true);
    AURORA_TEST_CHECK_EQ(d.is_open(), false);
    AURORA_TEST_CHECK_EQ(d.wants_click(), false);

    // open_state 引用可双向写。
    d.open_state().set(true);
    AURORA_TEST_CHECK_EQ(d.is_open(), true);
}

AURORA_TEST_CASE(pointer_event_closes_when_click_outside_panel) {
    auto content = std::make_shared<LeafProbe>();
    auto panel = std::make_shared<LeafProbe>();
    Drawer d{Node{content}, Node{panel}, DrawerSide::Left, 240.0F};
    d.set_open(true);  // 打开态

    // 点击遮罩外（size_ 为 0 → 面板矩形宽度 0 → 任何点都在面板外）→ 关闭。
    MouseEvent e;
    e.action = MouseAction::Press;
    e.local_position = Point{.x = 5.0F, .y = 5.0F};
    d.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(d.is_open(), false);
    AURORA_TEST_CHECK_EQ(e.is_handled, true);

    // 未打开态点击：下沉基类，不崩溃、不关闭。
    MouseEvent e2;
    e2.action = MouseAction::Press;
    e2.local_position = Point{.x = 5.0F, .y = 5.0F};
    d.on_pointer_event(e2);
    AURORA_TEST_CHECK_EQ(d.is_open(), false);
}

AURORA_TEST_CASE(right_side_panel_rect_branch) {
    Drawer right{Node{std::make_shared<LeafProbe>()}, Node{std::make_shared<LeafProbe>()}, DrawerSide::Right, 200.0F};
    // 右停靠分支（size_ 默认 0 → 面板贴右但宽度夹为 0，仅覆盖分支）。
    const Rect r = right.panel_rect();
    (void)r;
    AURORA_TEST_CHECK_EQ(right.panel_width(), 200.0F);
}

AURORA_TEST_CASE(serialize_and_deserialize_roundtrip) {
    Drawer d{Node{std::make_shared<LeafProbe>()}, Node{std::make_shared<LeafProbe>()}, DrawerSide::Right, 180.0F};
    d.set_open(true);
    d.set_permanent(true);
    Json props = Json::object();
    d.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("open"));
    AURORA_TEST_CHECK_TRUE(props.contains("side"));
    AURORA_TEST_CHECK_TRUE(props.contains("panel_width"));
    AURORA_TEST_CHECK_TRUE(props.contains("permanent"));

    Drawer d2;
    Json p = Json::object();
    p.set("open", Json{true});
    p.set("side", "right");
    p.set("panel_width", 300.0);
    p.set("permanent", Json{true});
    d2.deserialize_props(p);
    AURORA_TEST_CHECK_EQ(d2.is_open(), true);
    AURORA_TEST_CHECK_EQ(d2.panel_width(), 300.0F);
    AURORA_TEST_CHECK_EQ(d2.is_permanent(), true);
}

AURORA_TEST_CASE(mount_unmount_tick_forwards_to_children) {
    auto content = std::make_shared<LeafProbe>();
    auto panel = std::make_shared<LeafProbe>();
    Drawer d{Node{content}, Node{panel}};
    BuildContext ctx;
    d.mount(ctx);
    AURORA_TEST_CHECK_EQ(content->mounts, 1);
    AURORA_TEST_CHECK_EQ(panel->mounts, 1);
    d.tick(std::chrono::steady_clock::now());
    d.unmount();
    AURORA_TEST_CHECK_EQ(content->unmounts, 1);
    AURORA_TEST_CHECK_EQ(panel->unmounts, 1);
    // 遍历子节点：内容 + 面板各一次。
    int visited = 0;
    d.for_each_child([&visited](const Widget &) { ++visited; });
    AURORA_TEST_CHECK_EQ(visited, 2);
}

AURORA_TEST_CASE(progress_dialog_api_surface) {
    // 默认构造：空消息、可取消、未显示、进度不确定态(-1)。
    ProgressDialog def;
    AURORA_TEST_CHECK_EQ(std::string(def.type_name()), "ProgressDialog");
    AURORA_TEST_CHECK_EQ(def.is_open(), false);
    AURORA_TEST_CHECK_EQ(def.progress(), -1.0F);
    AURORA_TEST_CHECK_EQ(def.message(), "");
    std::vector<SignalViewBase *> sigs;
    def.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.size(), 1U);  // 仅 progress_
    (void)def.describe();

    // 聚合构造 + 取值 setter + 进度归一。
    ProgressDialog pd{"working", false};
    AURORA_TEST_CHECK_EQ(pd.message(), "working");
    pd.set_message("almost");
    AURORA_TEST_CHECK_EQ(pd.message(), "almost");
    pd.set_progress(-0.5F);  // 负值 → 不确定态 -1
    AURORA_TEST_CHECK_EQ(pd.progress(), -1.0F);
    pd.set_progress(2.0F);  // 越界 → 夹到 1
    AURORA_TEST_CHECK_EQ(pd.progress(), 1.0F);
    pd.set_progress(0.4F);  // 中段
    AURORA_TEST_CHECK_EQ(pd.progress(), 0.4F);

    // show/close 切换显示态 + wants_click。
    pd.show();
    AURORA_TEST_CHECK_EQ(pd.is_open(), true);
    AURORA_TEST_CHECK_EQ(pd.wants_click(), true);
    pd.close();
    AURORA_TEST_CHECK_EQ(pd.is_open(), false);
}

AURORA_TEST_CASE(progress_dialog_cancel_paths) {
    // 可取消：cancel() 关对话框 + 触发回调。
    ProgressDialog pd{"x", true};
    int cancelled = 0;
    pd.set_on_cancel([&cancelled]() { ++cancelled; });
    pd.show();
    pd.cancel();
    AURORA_TEST_CHECK_EQ(pd.is_open(), false);
    AURORA_TEST_CHECK_EQ(cancelled, 1);

    // 不可取消：cancel() 整体 no-op（open_ 不变、回调不触发）。
    ProgressDialog locked{"y", false};
    int c2 = 0;
    locked.set_on_cancel([&c2]() { ++c2; });
    locked.show();
    locked.cancel();
    AURORA_TEST_CHECK_EQ(locked.is_open(), true);
    AURORA_TEST_CHECK_EQ(c2, 0);
}

AURORA_TEST_CASE(progress_dialog_pointer_event_branches) {
    // 打开 + 可取消 + Press：模态吞掉非取消钮点击（未布局 → cancel_rect 空 → 不命中取消钮）。
    ProgressDialog pd{"x", true};
    pd.show();
    MouseEvent e;
    e.action = MouseAction::Press;
    e.local_position = Point{.x = 5.0F, .y = 5.0F};
    pd.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(e.is_handled, true);  // 被模态吞掉
    AURORA_TEST_CHECK_EQ(pd.is_open(), true);  // 非取消钮 → 不关

    // 未打开态：下沉基类链（不消费、不崩溃）。
    ProgressDialog closed{"y", true};
    MouseEvent e2;
    e2.action = MouseAction::Press;
    e2.local_position = Point{.x = 5.0F, .y = 5.0F};
    closed.on_pointer_event(e2);
    AURORA_TEST_CHECK_EQ(closed.is_open(), false);

    // 打开但不可取消：条件短路 → 下沉基类。
    ProgressDialog locked{"z", false};
    locked.show();
    MouseEvent e3;
    e3.action = MouseAction::Press;
    e3.local_position = Point{.x = 5.0F, .y = 5.0F};
    locked.on_pointer_event(e3);
    AURORA_TEST_CHECK_EQ(locked.is_open(), true);
}

AURORA_TEST_CASE(progress_dialog_serialize_roundtrip) {
    ProgressDialog pd{"hello", true};
    pd.show();
    pd.set_progress(0.5F);
    Json props = Json::object();
    pd.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("message"));
    AURORA_TEST_CHECK_TRUE(props.contains("progress"));
    AURORA_TEST_CHECK_TRUE(props.contains("open"));
    AURORA_TEST_CHECK_TRUE(props.contains("cancellable"));

    ProgressDialog restored;
    Json p = Json::object();
    p.set("message", std::string{"restored"});
    p.set("progress", 0.25);
    p.set("open", Json{true});
    p.set("cancellable", Json{false});
    restored.deserialize_props(p);
    AURORA_TEST_CHECK_EQ(restored.message(), "restored");
    AURORA_TEST_CHECK_EQ(restored.progress(), 0.25F);
    AURORA_TEST_CHECK_EQ(restored.is_open(), true);
    // cancellable 还原为 false：cancel() 不再关闭。
    restored.show();
    restored.cancel();
    AURORA_TEST_CHECK_EQ(restored.is_open(), true);
}

AURORA_TEST_CASE(page_view_api_surface) {
    auto a = std::make_shared<LeafProbe>();
    auto b = std::make_shared<LeafProbe>();
    auto c = std::make_shared<LeafProbe>();
    // 初始页码越界夹取；空列表取 0。
    PageView pv{std::vector<Node>{Node{a}, Node{b}, Node{c}}, 10};
    AURORA_TEST_CHECK_EQ(std::string(pv.type_name()), "PageView");
    AURORA_TEST_CHECK_EQ(pv.page_count(), 3U);
    AURORA_TEST_CHECK_EQ(pv.current_page(), 2);  // 夹到 size-1
    AURORA_TEST_CHECK_EQ(pv.wants_click(), true);
    std::vector<SignalViewBase *> psigs;
    pv.collect_signals(psigs);
    AURORA_TEST_CHECK_EQ(psigs.size(), 1U);  // 仅 current_
    (void)pv.describe();

    PageView under{std::vector<Node>{Node{a}, Node{b}, Node{c}}, -5};
    AURORA_TEST_CHECK_EQ(under.current_page(), 0);  // 负 → 夹到 0

    PageView empty{std::vector<Node>{}, 9};
    AURORA_TEST_CHECK_EQ(empty.page_count(), 0U);
    AURORA_TEST_CHECK_EQ(empty.current_page(), 0);
}

AURORA_TEST_CASE(page_view_navigation) {
    auto a = std::make_shared<LeafProbe>();
    auto b = std::make_shared<LeafProbe>();
    auto c = std::make_shared<LeafProbe>();
    PageView pv{std::vector<Node>{Node{a}, Node{b}, Node{c}}, 0};
    int changes = 0;
    pv.set_on_page_change([&changes](int) { ++changes; });
    pv.set_show_indicator(false);

    // go_to 有效页：改页码 + 回调。
    pv.go_to(2);
    AURORA_TEST_CHECK_EQ(pv.current_page(), 2);
    AURORA_TEST_CHECK_EQ(changes, 1);

    // go_to 同页 / 越界：整体忽略（不回调、不改）。
    pv.go_to(2);
    pv.go_to(99);
    pv.go_to(-1);
    AURORA_TEST_CHECK_EQ(pv.current_page(), 2);
    AURORA_TEST_CHECK_EQ(changes, 1);

    // next/prev 跨页；末页 next / 首页 prev 越界忽略。
    pv.go_to(0);
    pv.next();
    AURORA_TEST_CHECK_EQ(pv.current_page(), 1);
    pv.next();
    AURORA_TEST_CHECK_EQ(pv.current_page(), 2);
    pv.next();  // 末页 → 忽略
    AURORA_TEST_CHECK_EQ(pv.current_page(), 2);
    pv.prev();
    AURORA_TEST_CHECK_EQ(pv.current_page(), 1);
    pv.prev();
    AURORA_TEST_CHECK_EQ(pv.current_page(), 0);
    pv.prev();  // 首页 → 忽略
    AURORA_TEST_CHECK_EQ(pv.current_page(), 0);
}

AURORA_TEST_CASE(page_view_drag_flips_page) {
    auto a = std::make_shared<LeafProbe>();
    auto b = std::make_shared<LeafProbe>();
    auto c = std::make_shared<LeafProbe>();
    PageView pv{std::vector<Node>{Node{a}, Node{b}, Node{c}}, 1};
    // 无布局 → size_.width=0 → 阈值 0：左滑(dx<0) 进下一页，右滑(dx>0) 进上一页。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 200.0F, .y = 5.0F};
    pv.on_pointer_event(press);
    AURORA_TEST_CHECK_EQ(press.is_handled, true);

    // Release 左滑到 x=10：dx=-190 → next()。
    MouseEvent release_left;
    release_left.action = MouseAction::Release;
    release_left.local_position = Point{.x = 10.0F, .y = 5.0F};
    pv.on_pointer_event(release_left);
    AURORA_TEST_CHECK_EQ(pv.current_page(), 2);  // 1 → next

    // 新一轮：右滑 dx=+190 → prev()。
    MouseEvent press2;
    press2.action = MouseAction::Press;
    press2.local_position = Point{.x = 10.0F, .y = 5.0F};
    pv.on_pointer_event(press2);
    MouseEvent release_right;
    release_right.action = MouseAction::Release;
    release_right.local_position = Point{.x = 200.0F, .y = 5.0F};
    pv.on_pointer_event(release_right);
    AURORA_TEST_CHECK_EQ(pv.current_page(), 1);  // 2 → prev

    // Release 无前置 Press（drag_active_=false）→ 下沉基类，不翻页。
    MouseEvent release_orphan;
    release_orphan.action = MouseAction::Release;
    release_orphan.local_position = Point{.x = 5.0F, .y = 5.0F};
    pv.on_pointer_event(release_orphan);
    AURORA_TEST_CHECK_EQ(pv.current_page(), 1);

    // Move 动作：下沉基类，不翻页。
    MouseEvent move;
    move.action = MouseAction::Move;
    move.local_position = Point{.x = 5.0F, .y = 5.0F};
    pv.on_pointer_event(move);
    AURORA_TEST_CHECK_EQ(pv.current_page(), 1);
}

AURORA_TEST_CASE(page_view_serialize_roundtrip) {
    auto a = std::make_shared<LeafProbe>();
    auto b = std::make_shared<LeafProbe>();
    PageView pv{std::vector<Node>{Node{a}, Node{b}}, 1};
    pv.set_show_indicator(false);
    Json props = Json::object();
    pv.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("current"));
    AURORA_TEST_CHECK_TRUE(props.contains("show_indicator"));

    PageView restored{std::vector<Node>{Node{a}, Node{b}}, 0};
    Json p = Json::object();
    p.set("current", 1);
    p.set("show_indicator", Json{true});
    restored.deserialize_props(p);
    AURORA_TEST_CHECK_EQ(restored.current_page(), 1);  // 反序列化不夹取
}

AURORA_TEST_CASE(drawer_layout_and_paint_branches) {
    auto content = std::make_shared<LeafProbe>();
    auto panel = std::make_shared<LeafProbe>();
    BuildContext ctx;
    Painter painter;

    // 左停靠 + 打开：面板可见、内容占满、面板宽 240 贴左、绘制遮罩+阴影+面板+内容。
    Drawer left{Node{content}, Node{panel}, DrawerSide::Left, 240.0F};
    left.set_open(true);
    LayoutEngine::layout(left, bounded(800.0F, 600.0F));
    AURORA_TEST_CHECK_EQ(left.size().width, 800.0F);
    AURORA_TEST_CHECK_EQ(left.size().height, 600.0F);
    left.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = left.size()}, ctx);

    // 左停靠 + 永久：无遮罩分支、面板仍绘制。
    Drawer perm{Node{content}, Node{panel}, DrawerSide::Left, 240.0F};
    perm.set_permanent(true);
    LayoutEngine::layout(perm, bounded(800.0F, 600.0F));
    perm.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = perm.size()}, ctx);

    // 右停靠 + 打开：面板贴右、阴影 delta 取负。
    Drawer right{Node{content}, Node{panel}, DrawerSide::Right, 240.0F};
    right.set_open(true);
    LayoutEngine::layout(right, bounded(800.0F, 600.0F));
    right.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = right.size()}, ctx);

    // 非有限约束 → 回退 480x320。
    Drawer finite_fb{Node{content}, Node{panel}, DrawerSide::Left, 240.0F};
    finite_fb.set_open(true);
    LayoutEngine::layout(finite_fb,
                         Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                                     .max = Size{.width = std::numeric_limits<float>::infinity(),
                                                 .height = std::numeric_limits<float>::infinity()}},
                         ctx);
    AURORA_TEST_CHECK_EQ(finite_fb.size().width, 480.0F);
    AURORA_TEST_CHECK_EQ(finite_fb.size().height, 320.0F);
}

AURORA_TEST_CASE(drawer_hit_test_branches) {
    auto content = std::make_shared<LeafProbe>();
    auto panel = std::make_shared<LeafProbe>();
    BuildContext ctx;
    Drawer d{Node{content}, Node{panel}, DrawerSide::Left, 240.0F};
    d.set_open(true);
    LayoutEngine::layout(d, bounded(800.0F, 600.0F));
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = d.size()};

    // 面板内点击 → 命中（面板子树或自身）。
    Widget *in_panel = d.hit_test(Point{.x = 10.0F, .y = 10.0F}, box, ctx);
    AURORA_TEST_CHECK_TRUE(in_panel != nullptr);
    // 内容区点击 → 命中内容子树。
    Widget *in_content = d.hit_test(Point{.x = 400.0F, .y = 300.0F}, box, ctx);
    AURORA_TEST_CHECK_TRUE(in_content != nullptr);

    // 空内容 + 打开：点击遮罩区（不在面板内）→ 返回自身（模态关闭语义）。
    Drawer scrim{Node{}, Node{std::make_shared<LeafProbe>()}, DrawerSide::Left, 240.0F};
    scrim.set_open(true);
    LayoutEngine::layout(scrim, bounded(800.0F, 600.0F));
    Widget *on_scrim = scrim.hit_test(Point{.x = 400.0F, .y = 300.0F},
                                      Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = scrim.size()}, ctx);
    AURORA_TEST_CHECK_TRUE(on_scrim != nullptr);

    // 关闭且空：无命中。
    Drawer closed{Node{}, Node{}};
    LayoutEngine::layout(closed, bounded(800.0F, 600.0F));
    Widget *none = closed.hit_test(Point{.x = 400.0F, .y = 300.0F},
                                   Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = closed.size()}, ctx);
    AURORA_TEST_CHECK_EQ(none, nullptr);
}

AURORA_TEST_CASE(progress_dialog_layout_paint_hit_test) {
    BuildContext ctx;
    Painter p;
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 400.0F, .height = 300.0F}};

    // 打开 + 确定进度：布局算盒 + 绘制进度填充 + 取消按钮。
    ProgressDialog pd{"working", true};
    pd.show();
    pd.set_progress(0.5F);
    LayoutEngine::layout(pd, bounded(400.0F, 300.0F));
    pd.paint(p, box, ctx);  // 遮罩+盒+消息+进度填充(214-217)+取消按钮(227-235)

    // 打开 + 不确定态：中段高亮块。
    ProgressDialog ind{"thinking", true};
    ind.show();
    ind.set_progress(-1.0F);
    LayoutEngine::layout(ind, bounded(400.0F, 300.0F));
    ind.paint(p, box, ctx);  // 218-223

    // 打开但不可取消：跳过取消按钮绘制。
    ProgressDialog nocancel{"x", false};
    nocancel.show();
    nocancel.set_progress(0.3F);
    LayoutEngine::layout(nocancel, bounded(400.0F, 300.0F));
    nocancel.paint(p, box, ctx);  // 跳过 227

    // 未打开：绘制提前返回。
    ProgressDialog hidden{"y", true};
    hidden.paint(p, box, ctx);  // 192-193

    // 布局分支：非有限 → 400x300；未打开 → 0x0。
    ProgressDialog fb{"z", true};
    fb.show();
    LayoutEngine::layout(fb,
                         Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                                     .max = Size{.width = std::numeric_limits<float>::infinity(),
                                                 .height = std::numeric_limits<float>::infinity()}},
                         ctx);
    AURORA_TEST_CHECK_EQ(fb.size().width, 400.0F);
    AURORA_TEST_CHECK_EQ(fb.size().height, 300.0F);
    ProgressDialog shut{"w", true};
    LayoutEngine::layout(shut, bounded(400.0F, 300.0F), ctx);
    AURORA_TEST_CHECK_EQ(shut.size().width, 0.0F);
    AURORA_TEST_CHECK_EQ(shut.size().height, 0.0F);

    // 命中测试：打开态盒内 → 自身；盒外 → nullptr；未打开 → nullptr。
    ProgressDialog ht{"v", true};
    ht.show();
    LayoutEngine::layout(ht, bounded(400.0F, 300.0F), ctx);
    AURORA_TEST_CHECK_TRUE(ht.hit_test(Point{.x = 200.0F, .y = 150.0F}, box, ctx) != nullptr);
    AURORA_TEST_CHECK_EQ(ht.hit_test(Point{.x = 999.0F, .y = 999.0F}, box, ctx), nullptr);
    ProgressDialog ht2{"u", true};  // 未打开
    AURORA_TEST_CHECK_EQ(ht2.hit_test(Point{.x = 200.0F, .y = 150.0F}, box, ctx), nullptr);
}

AURORA_TEST_CASE(page_view_layout_paint_hit_test) {
    auto a = std::make_shared<LeafProbe>();
    auto b = std::make_shared<LeafProbe>();
    auto c = std::make_shared<LeafProbe>();
    BuildContext ctx;
    Painter p;

    // 多页 + 指示器：当前页绘制 + 圆点(active=0, 其余 inactive)。
    PageView pv{std::vector<Node>{Node{a}, Node{b}, Node{c}}, 0};
    pv.set_show_indicator(true);
    LayoutEngine::layout(pv, bounded(400.0F, 300.0F));
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = pv.size()};
    pv.paint(p, box, ctx);  // 293-298 + 301-314
    Widget *hit = pv.hit_test(Point{.x = 200.0F, .y = 150.0F}, box, ctx);
    AURORA_TEST_CHECK_TRUE(hit != nullptr);  // 当前页命中

    // 单页：指示器跳过（>1 不成立）。
    PageView single{std::vector<Node>{Node{a}}, 0};
    single.set_show_indicator(true);
    LayoutEngine::layout(single, bounded(400.0F, 300.0F));
    single.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = single.size()}, ctx);

    // 非有限约束 → 回退 320x240。
    PageView fb{std::vector<Node>{Node{a}}, 0};
    LayoutEngine::layout(fb,
                         Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                                     .max = Size{.width = std::numeric_limits<float>::infinity(),
                                                 .height = std::numeric_limits<float>::infinity()}},
                         ctx);
    AURORA_TEST_CHECK_EQ(fb.size().width, 320.0F);
    AURORA_TEST_CHECK_EQ(fb.size().height, 240.0F);
}

}  // namespace aurora::test_cases::utest_drawer
