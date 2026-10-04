/// 测试类型: unit
/// 目标单元: include/aurora/widget/dialog.h
/// 测试说明: 覆盖 Dialog 的几何/命中契约——打开态内容盒由 on_layout 落定到 children_[0].bounds_
/// （on_paint 直接读它落笔，不另算居中）、经真实 EventDispatcher 点击内容里的按钮回调触发、
/// 遮罩区点击被 Dialog 自身吸收且不下落穿透、正对照（同一份内容直接挂 Column 亦可点，
/// 排除探针接线错误）、关闭态不参与命中，以及 show/close/set_content 的标脏（布局缓存命中
/// 不得让 on_layout 跳过，否则新内容既不落笔也不可命中）。
///
/// 回归背景：修复前 Dialog 从不调 Node::set_bounds，视觉用 on_paint 现算的居中盒、命中用
/// 空的默认盒；且 OverlayHost::on_hit_test_chain 对空链不停止、继续问下层基础内容
/// ⇒ 点击穿透到对话框下方的视口。
///
/// 探针取点纪律：命中点取**按钮自身盒的中心**（经 child_nodes() 实读），不取内容盒中心——
/// Column 子项之间有 gap，内容盒中心可能落在间隙里（与被测行为无关）。同理不硬编码任何
/// 坐标：字体度量按平台不同，只断言「点在该按钮盒内 ⇒ 命中该按钮」这一关系。

#include <memory>
#include <string>
#include <vector>

#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/dialog.h"
#include "aurora/widget/popup.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_dialog {

namespace {

constexpr float AURORA_WIDTH = 400.0F;   ///< 对话框宿主视口宽（dp）
constexpr float AURORA_HEIGHT = 300.0F;  ///< 对话框宿主视口高（dp）

auto viewport() -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = AURORA_WIDTH, .height = AURORA_HEIGHT}};
}

auto full() -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                       .max = Size{.width = AURORA_WIDTH, .height = AURORA_HEIGHT}};
}

auto mouse(MouseAction action, float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = action;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = y};
    return e;
}

/// @brief 内容根（Column：标题 + 按钮），按钮回调自增外部计数器。
/// @param hits 被按钮回调自增的计数器（引用，生命周期覆盖两次派发）。
/// @return 可直接交给 Dialog / Column 承载的内容节点。
auto make_content(int &hits) -> Node {
    auto btn = Button(ButtonProps{.label = "OK"});
    btn.set_on_click([&hits]() -> void { ++hits; });
    auto content = Column(ColumnProps{
        .children = {std::move(Text("Title").font_size(18).bold()), std::move(btn)},
        .gap = 12.0F,
    });
    return {std::move(content)};
}

/// @brief 在容器子树中找出首个 Button 并返回它的盒（相对容器内容区原点）。
/// @param container 内容根容器（须已完成布局）。
/// @return该按钮的局部盒；子树内无按钮时返回零盒。
[[nodiscard]] auto button_box(const Widget &container) -> Rect {
    for (const Node &n : container.child_nodes()) {
        if (std::string{n.widget().type_name()} == "Button") {
            return n.bounds();
        }
    }
    return Rect{};
}

/// @brief 按钮盒中心（相对内容根原点）。要求容器已完成布局。
/// @param container 内容根容器。
/// @return 按钮中心点；子树内无按钮时返回原点。
[[nodiscard]] auto button_center(const Widget &container) -> Point {
    const Rect b = button_box(container);
    return Point{.x = b.origin.x + (b.size.width * 0.5F), .y = b.origin.y + (b.size.height * 0.5F)};
}

/// @brief 按钮中心换算到 Dialog 的局部坐标系（内容盒原点 + 按钮在内容内的位置）。
/// @param dialog 已完成布局且处于打开态的对话框。
/// @return 按钮中心的 Dialog 局部坐标；无子节点时返回原点。
[[nodiscard]] auto button_probe(const Dialog &dialog) -> Point {
    const std::vector<Node> &kids = dialog.child_nodes();
    if (kids.empty()) {
        return Point{};
    }
    const Point inner = button_center(kids.front().widget());
    return Point{.x = kids.front().bounds().origin.x + inner.x, .y = kids.front().bounds().origin.y + inner.y};
}

}  // namespace

AURORA_TEST_CASE(content_bounds_written_and_paint_shares_it) {
    // 回归点：打开态必须把居中后的内容盒写入 children_[0]——几何权威在 Node::bounds_。
    int hits = 0;
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->show();
    LayoutEngine::layout(*dialog, full());

    // Dialog 自身按父约束占满；内容盒则是居中后的小盒。
    AURORA_TEST_CHECK_NEAR(dialog->size().width, AURORA_WIDTH, 1e-3F);
    AURORA_TEST_CHECK_NEAR(dialog->size().height, AURORA_HEIGHT, 1e-3F);

    const std::vector<Node> &kids = dialog->child_nodes();
    AURORA_TEST_REQUIRE(!kids.empty());
    const Rect cb = kids.front().bounds();
    // 修复前此盒为默认 Rect{}（宽高皆 0）。
    AURORA_TEST_CHECK(cb.size.width > 0.0F);
    AURORA_TEST_CHECK(cb.size.height > 0.0F);
    // 居中：左右余量相等、上下余量相等（各留 (self − content) / 2）。
    const float left = cb.origin.x;
    const float right = AURORA_WIDTH - (cb.origin.x + cb.size.width);
    const float top = cb.origin.y;
    const float bottom = AURORA_HEIGHT - (cb.origin.y + cb.size.height);
    AURORA_TEST_CHECK_NEAR(left, right, 1e-3F);
    AURORA_TEST_CHECK_NEAR(top, bottom, 1e-3F);
    // 内容尺寸受 self × 0.8 约束，居中后必留正余量。
    AURORA_TEST_CHECK(left > 0.0F);
    AURORA_TEST_CHECK(top > 0.0F);
}

AURORA_TEST_CASE(press_button_in_content_triggers_callback_via_dispatcher) {
    int hits = 0;
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->show();
    LayoutEngine::layout(*dialog, full());

    const Point probe = button_probe(*dialog);
    AURORA_TEST_REQUIRE(probe.y > 0.0F);  // 子树内确有按钮且盒已落定

    const auto chain = dialog->hit_test_chain(probe, viewport(), BuildContext{});
    // 回归点：修复前该点命中链为空，Press 被当作点击空白。
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(std::string{chain.back().ptr->type_name()}, std::string{"Button"});

    MouseEvent press = mouse(MouseAction::Press, probe.x, probe.y);
    EventDispatcher::dispatch(*dialog, press, nullptr);
    MouseEvent release = mouse(MouseAction::Release, probe.x, probe.y);
    EventDispatcher::dispatch(*dialog, release, nullptr);
    AURORA_TEST_CHECK_EQ(hits, 1);
}

AURORA_TEST_CASE(press_mask_absorbed_by_dialog_not_penetrating) {
    int hits = 0;
    int closed = 0;
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->set_on_close([&closed]() -> void { ++closed; });
    dialog->show();
    LayoutEngine::layout(*dialog, full());

    // 遮罩点：视口左上角（内容居中，必在内容盒外）。
    const Point probe{.x = 4.0F, .y = 4.0F};
    const std::vector<Node> &kids = dialog->child_nodes();
    AURORA_TEST_REQUIRE(!kids.empty());
    AURORA_TEST_REQUIRE(!kids.front().bounds().contains(probe));  // 前提：4,4 确在遮罩区

    const auto chain = dialog->hit_test_chain(probe, viewport(), BuildContext{});
    // 回归点：遮罩区必须返回非空链（仅含 Dialog 自身），否则 OverlayHost 会继续问下层
    // 基础内容、点击穿透到对话框下方的视口。
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(chain.size(), 1U);
    AURORA_TEST_CHECK_EQ(chain.front().ptr, static_cast<Widget *>(dialog.get()));

    MouseEvent press = mouse(MouseAction::Press, probe.x, probe.y);
    EventDispatcher::dispatch(*dialog, press, nullptr);
    // 遮罩吸收但不触发内容回调，也不等于取消（不触发 on_close）。
    AURORA_TEST_CHECK_EQ(hits, 0);
    AURORA_TEST_CHECK_EQ(closed, 0);
}

AURORA_TEST_CASE(positive_control_column_content_also_clickable) {
    // 正对照：同一份内容直接挂 Column（Column 会写 bounds）时同一派发路径能命中。
    // 用于排除「探针接线错误判成通过」——若此对照红，说明问题在探针而非 Dialog。
    int hits = 0;
    auto root = std::make_shared<Column>();
    auto btn = Button(ButtonProps{.label = "OK"});
    btn.set_on_click([&hits]() -> void { ++hits; });
    root->add(std::move(btn));
    LayoutEngine::layout(*root, full());

    const std::vector<Node> &kids = root->child_nodes();
    AURORA_TEST_REQUIRE(!kids.empty());
    const Rect bb = kids.front().bounds();
    AURORA_TEST_REQUIRE(bb.size.width > 0.0F);

    const Point probe{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.origin.y + (bb.size.height * 0.5F)};
    MouseEvent press = mouse(MouseAction::Press, probe.x, probe.y);
    EventDispatcher::dispatch(*root, press, nullptr);
    MouseEvent release = mouse(MouseAction::Release, probe.x, probe.y);
    EventDispatcher::dispatch(*root, release, nullptr);
    AURORA_TEST_CHECK_EQ(hits, 1);
}

AURORA_TEST_CASE(closed_dialog_never_enters_hit_chain) {
    int hits = 0;
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->show();
    LayoutEngine::layout(*dialog, full());
    const Point probe = button_probe(*dialog);
    AURORA_TEST_REQUIRE(probe.y > 0.0F);

    dialog->close();
    LayoutEngine::layout(*dialog, full());
    // 关闭态：既不命中内容，也不该以遮罩身份入链（不可见即不参与命中）。
    AURORA_TEST_CHECK(dialog->hit_test_chain(probe, viewport(), BuildContext{}).empty());
    AURORA_TEST_CHECK(dialog->hit_test_chain(Point{.x = 4.0F, .y = 4.0F}, viewport(), BuildContext{}).empty());
}

AURORA_TEST_CASE(set_content_while_open_marks_layout_dirty) {
    // 回归点：布局缓存命中的条件是「约束相等」，`set_content` 不标脏则 on_layout 被跳过、
    // 新子节点停在零盒（既不落笔也不可命中）。
    //
    // 场景取「**已打开**后换内容」而非「先 set_content 再 show」：后者会被 show() 自身的
    // 标脏覆盖掉，标脏缺失时仍全绿 ⇒ 判据结构性不可观测（变异自证时实测踩到过一次：
    // 去掉 set_content 的 mark_needs_layout 后本用例仍绿，改测打开态才转红）。
    // 真实消费者（borealis 复用同一 dialog 实例换文案）走的正是打开态这一条。
    int hits = 0;
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->show();
    LayoutEngine::layout(*dialog, full());  // 建立打开态布局缓存

    // 打开态换内容：约束与上一帧完全相同 ⇒ 只有 set_content 自己标脏才能让 on_layout 重跑。
    int new_hits = 0;
    dialog->set_content(make_content(new_hits));
    LayoutEngine::layout(*dialog, full());

    const std::vector<Node> &kids = dialog->child_nodes();
    AURORA_TEST_REQUIRE(!kids.empty());
    // 新子节点的盒必须已落定（未标脏时为默认零盒）。
    AURORA_TEST_CHECK(kids.front().bounds().size.width > 0.0F);
    AURORA_TEST_CHECK(kids.front().bounds().size.height > 0.0F);

    const Point probe = button_probe(*dialog);
    AURORA_TEST_REQUIRE(probe.y > 0.0F);
    const auto chain = dialog->hit_test_chain(probe, viewport(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(std::string{chain.back().ptr->type_name()}, std::string{"Button"});

    // Press+Release 成对：on_click 在 Release 分支 activate()（见 Widget::on_pointer_event）。
    MouseEvent press = mouse(MouseAction::Press, probe.x, probe.y);
    EventDispatcher::dispatch(*dialog, press, nullptr);
    MouseEvent release = mouse(MouseAction::Release, probe.x, probe.y);
    EventDispatcher::dispatch(*dialog, release, nullptr);
    AURORA_TEST_CHECK_EQ(new_hits, 1);  // 新内容的回调触发
    AURORA_TEST_CHECK_EQ(hits, 0);      // 旧内容已被替换，不应再被点到
}

AURORA_TEST_CASE(close_then_reopen_restores_content_hit) {
    // 开→关→开：open_ 变化须标布局脏，否则关闭态的零尺寸布局会被缓存复用。
    int hits = 0;
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->show();
    LayoutEngine::layout(*dialog, full());
    dialog->close();
    LayoutEngine::layout(*dialog, full());
    AURORA_TEST_CHECK(dialog->hit_test_chain(Point{.x = 4.0F, .y = 4.0F}, viewport(), BuildContext{}).empty());

    dialog->show();
    LayoutEngine::layout(*dialog, full());
    const std::vector<Node> &kids = dialog->child_nodes();
    AURORA_TEST_REQUIRE(!kids.empty());
    AURORA_TEST_CHECK(kids.front().bounds().size.width > 0.0F);

    const Point probe = button_probe(*dialog);
    // Press+Release 成对：on_click 在 Release 分支 activate()（见 Widget::on_pointer_event），
    // 只发 Press 只会置按下态、不会触发回调。
    MouseEvent press = mouse(MouseAction::Press, probe.x, probe.y);
    EventDispatcher::dispatch(*dialog, press, nullptr);
    MouseEvent release = mouse(MouseAction::Release, probe.x, probe.y);
    EventDispatcher::dispatch(*dialog, release, nullptr);
    AURORA_TEST_CHECK_EQ(hits, 1);
}

AURORA_TEST_CASE(overlay_host_dialog_does_not_penetrate_to_base) {
    // 端到端对照 borealis 的真实挂载形态：Dialog 作为 OverlayHost 的浮层。
    // 修复前该场景下点击内容会穿透到基础内容（终端视口）——本例钉住「不再穿透」。
    int hits = 0;
    int base_hits = 0;
    auto base = std::make_shared<Column>();
    auto base_btn = Button(ButtonProps{.label = "Base"});
    base_btn.set_on_click([&base_hits]() -> void { ++base_hits; });
    base->add(std::move(base_btn));

    auto host = std::make_shared<OverlayHost>(Node{base});
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->show();
    host->add_overlay(Node{std::static_pointer_cast<Widget>(dialog)});
    LayoutEngine::layout(*host, full());

    const Point probe = button_probe(*dialog);
    MouseEvent press = mouse(MouseAction::Press, probe.x, probe.y);
    EventDispatcher::dispatch(*host, press, nullptr);
    MouseEvent release = mouse(MouseAction::Release, probe.x, probe.y);
    EventDispatcher::dispatch(*host, release, nullptr);
    // 浮层内容吃掉这次点击，基础内容的按钮一次都不该响（这正是 borealis 的原始症状）。
    AURORA_TEST_CHECK_EQ(hits, 1);
    AURORA_TEST_CHECK_EQ(base_hits, 0);
}

}  // namespace aurora::test_cases::utest_dialog
