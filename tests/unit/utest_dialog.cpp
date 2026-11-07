/// 测试类型: unit
/// 目标单元: include/aurora/widget/dialog.h
/// 测试说明: 覆盖 Dialog 的几何/命中契约——打开态内容盒由 on_layout 落定到 children_[0].bounds_
/// （on_paint 直接读它落笔，不另算居中）、经真实 EventDispatcher 点击内容里的按钮回调触发、
/// 遮罩区点击被 Dialog 自身吸收且不下落穿透、正对照（同一份内容直接挂 Column 亦可点，
/// 排除探针接线错误）、关闭态不参与命中，以及 show/close/set_content 的标脏（布局缓存命中
/// 不得让 on_layout 跳过，否则新内容既不落笔也不可命中）。另覆盖便捷工厂 `alert` / `confirm`
/// 的惯例接线（接线批）：动作按钮排布与标题对齐随 `Theme::conventions` 的风格档变化、
/// 钮间距读 `Theme::spacing` 令牌，且默认档（AcceptFirst + Leading）与接线前逐位同构。
///
/// 回归背景：修复前 Dialog 从不调 Node::set_bounds，视觉用 on_paint 现算的居中盒、命中用
/// 空的默认盒；且 OverlayHost::on_hit_test_chain 对空链不停止、继续问下层基础内容
/// ⇒ 点击穿透到对话框下方的视口。
///
/// 探针取点纪律：命中点取**按钮自身盒的中心**（经 child_nodes() 实读），不取内容盒中心——
/// Column 子项之间有 gap，内容盒中心可能落在间隙里（与被测行为无关）。同理不硬编码任何
/// 坐标：字体度量按平台不同，只断言「点在该按钮盒内 ⇒ 命中该按钮」这一关系。

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/dialog.h"
#include "aurora/widget/popup.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_dialog {

namespace {

constexpr float AURORA_WIDTH = 400.0F;  ///< 对话框宿主视口宽（dp）
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
    auto btn = Button(ButtonProps().set_label("OK"));
    btn.set_on_click([&hits]() -> void { ++hits; });
    auto content =
        Column({std::move(Text("Title").font_size(18).bold()), std::move(btn)}, ColumnProps().set_gap(12.0F));
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
    auto btn = Button(ButtonProps().set_label("OK"));
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
    AURORA_TEST_CHECK_EQ(hits, 0);  // 旧内容已被替换，不应再被点到
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
    auto base_btn = Button(ButtonProps().set_label("Base"));
    base_btn.set_on_click([&base_hits]() -> void { ++base_hits; });
    base->add(std::move(base_btn));

    auto host = std::make_shared<OverlayHost>(Node{base});
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->show();
    // 有基础内容 ⇒ 追加得到可移除序号（≥ 1）；空宿主会返回 nullopt（见 utest_overlay_host）。
    const std::optional<std::size_t> overlay_index = host->add_overlay(Node{std::static_pointer_cast<Widget>(dialog)});
    AURORA_TEST_REQUIRE(overlay_index.has_value());
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

AURORA_TEST_CASE(dialog_paint_draws_mask_and_content) {
    // 覆盖 on_paint：打开态铺半透明遮罩 + 按居中盒落笔内容；关闭态直接 return。
    int hits = 0;
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(make_content(hits));
    dialog->show();
    LayoutEngine::layout(*dialog, full());

    Painter painter;
    dialog->paint(painter, viewport(), BuildContext{});  // 打开态：遮罩 + 内容

    dialog->close();
    dialog->paint(painter, viewport(), BuildContext{});  // 关闭态：on_paint 早返回
}

// ---- 便捷工厂的惯例接线（接线批）：alert / confirm 的动作按钮排布随主题风格档变化 ----

namespace {

/// @brief 深度优先收集子树内全部 Button 的盒，换算到 Dialog 局部坐标，按文档序返回。
///
/// 盒权威取自 `Node::bounds_`（父容器写入），逐层累加原点即得 Dialog 局部坐标——与
/// `button_probe` 同一套折算，不硬编码任何坐标（字体度量按平台不同）。
/// @param n 当前节点。
/// @param origin 当前节点父侧原点的累计值。
/// @param out 收集容器。
auto collect_button_boxes(const Node &n, const Point &origin, std::vector<Rect> &out) -> void {
    const Point here{.x = origin.x + n.bounds().origin.x, .y = origin.y + n.bounds().origin.y};
    if (std::string{n.widget().type_name()} == "Button") {
        out.push_back(Rect{.origin = here, .size = n.bounds().size});
        return;
    }
    for (const Node &child : n.widget().child_nodes()) {
        collect_button_boxes(child, here, out);
    }
}

/// @brief 打开承载指定内容的对话框并完成整树布局。
/// @param content 工厂产出的内容节点。
/// @return 持有 Dialog 的共享指针（调用方持有即保证子树存活）。
auto open_dialog(Node content) -> std::shared_ptr<Dialog> {
    auto dialog = std::make_shared<Dialog>();
    dialog->set_content(std::move(content));
    dialog->show();
    LayoutEngine::layout(*dialog, full());
    return dialog;
}

/// @brief 对话框内动作按钮的盒（Dialog 局部坐标，文档序）。
/// @param dialog 已完成布局且处于打开态的对话框。
/// @return 按钮盒数组；无按钮时为空。
auto button_boxes(const Dialog &dialog) -> std::vector<Rect> {
    std::vector<Rect> out;
    const std::vector<Node> &kids = dialog.child_nodes();
    if (!kids.empty()) {
        collect_button_boxes(kids.front(), Point{}, out);
    }
    return out;
}

/// @brief 卡片（内容根 Column）在 Dialog 局部坐标下的右缘。
/// @param dialog 已完成布局且处于打开态的对话框。
/// @return 右缘 x；无子节点时返回 0。
auto card_right(const Dialog &dialog) -> float {
    const std::vector<Node> &kids = dialog.child_nodes();
    if (kids.empty()) {
        return 0.0F;
    }
    const Rect &box = kids.front().bounds();
    return box.origin.x + box.size.width;
}

/// @brief 在给定盒中心派发一次完整点击，回传该钮经 `on_result` 给出的布尔（Yes=true、No=false）。
///
/// 「点在哪 ⇒ 回调传出什么」即按钮身份的判据：不需要读 label 文案，也不依赖两钮宽度差异。
/// @param dialog 已完成布局且处于打开态的对话框。
/// @param box 按钮盒（Dialog 局部坐标）。
/// @param results 工厂回调收集容器（按触发顺序追加）。
/// @return 本次点击新增的结果；命中链不含按钮或未触发回调时为空。
auto click_box(Dialog &dialog, const Rect &box, std::vector<bool> &results) -> std::optional<bool> {
    const Point probe{.x = box.origin.x + (box.size.width * 0.5F), .y = box.origin.y + (box.size.height * 0.5F)};
    const auto chain = dialog.hit_test_chain(probe, viewport(), BuildContext{});
    if (chain.empty() || std::string{chain.back().ptr->type_name()} != "Button") {
        return std::nullopt;
    }
    const std::size_t before = results.size();
    MouseEvent press = mouse(MouseAction::Press, probe.x, probe.y);
    EventDispatcher::dispatch(dialog, press, nullptr);
    MouseEvent release = mouse(MouseAction::Release, probe.x, probe.y);
    EventDispatcher::dispatch(dialog, release, nullptr);
    if (results.size() == before) {
        return std::nullopt;
    }
    return results.back();
}

/// @brief 一段必定宽于动作按钮行的正文：右靠判据的前提，各用例内以实测复核。
constexpr auto AURORA_LONG_MESSAGE = "The quick brown fox jumps over the lazy dog near the open window";

}  // namespace

AURORA_TEST_CASE(confirm_accept_first_arms_put_accept_on_the_left) {
    // Material / Windows / KDE 三档同取 AcceptFirst：接受钮在行首（左）、整行贴卡片左缘——
    // 与接线批之前的工厂行为逐位一致（零回归锚点）。
    for (const ThemeStyle style : {ThemeStyle::Material, ThemeStyle::Windows, ThemeStyle::Kde}) {
        std::vector<bool> results;
        auto dialog = open_dialog(confirm(
            "Title", AURORA_LONG_MESSAGE, [&results](bool r) -> void { results.push_back(r); },
            Theme::from_style(style, ColorScheme::Light)));
        const std::vector<Rect> boxes = button_boxes(*dialog);
        AURORA_TEST_REQUIRE_EQ(boxes.size(), 2U);
        AURORA_TEST_REQUIRE(card_right(*dialog) > boxes[1].origin.x + boxes[1].size.width);  // 前提：正文更宽
        AURORA_TEST_CHECK_NEAR(boxes[0].origin.y, boxes[1].origin.y, 1e-3F);  // 同一水平行
        AURORA_TEST_CHECK(boxes[0].origin.x < boxes[1].origin.x);  // 接受钮在左
        AURORA_TEST_CHECK(click_box(*dialog, boxes[0], results) == std::optional{true});
        AURORA_TEST_CHECK(click_box(*dialog, boxes[1], results) == std::optional{false});
        // 默认档不包层：按钮的直接父级即内容根 Column，其盒原点与卡片左缘同值。
        AURORA_TEST_CHECK_NEAR(boxes[0].origin.x, dialog->child_nodes().front().bounds().origin.x, 0.5F);
    }
}

AURORA_TEST_CASE(confirm_accept_last_arm_moves_accept_to_the_right_edge) {
    // GNOME/Adwaita 档：[拒绝] 左、[接受] 右，且整行靠卡片右缘（与 AcceptFirst 档可区分的判据）。
    std::vector<bool> results;
    auto dialog = open_dialog(confirm(
        "Title", AURORA_LONG_MESSAGE, [&results](bool r) -> void { results.push_back(r); },
        Theme::from_style(ThemeStyle::Gnome, ColorScheme::Light)));
    const std::vector<Rect> boxes = button_boxes(*dialog);
    AURORA_TEST_REQUIRE_EQ(boxes.size(), 2U);
    AURORA_TEST_CHECK_NEAR(boxes[0].origin.y, boxes[1].origin.y, 1e-3F);  // 同一水平行
    AURORA_TEST_CHECK(boxes[0].origin.x < boxes[1].origin.x);  // 行首已是拒绝钮
    AURORA_TEST_CHECK(click_box(*dialog, boxes[0], results) == std::optional{false});
    AURORA_TEST_CHECK(click_box(*dialog, boxes[1], results) == std::optional{true});
    // 右靠：末钮右缘贴卡片右缘（AcceptFirst 档下该式不成立，故上一条用例反向成立）。
    AURORA_TEST_CHECK_NEAR(boxes[1].origin.x + boxes[1].size.width, card_right(*dialog), 0.5F);
}

AURORA_TEST_CASE(confirm_vertical_arm_stacks_accept_above_reject) {
    // macOS 档（VerticalRightAligned）：竖排、接受钮在上、两钮右缘对齐并整体贴卡片右缘。
    std::vector<bool> results;
    const Theme themed = Theme::from_style(ThemeStyle::MacOs, ColorScheme::Light);
    auto dialog = open_dialog(
        confirm("Title", AURORA_LONG_MESSAGE, [&results](bool r) -> void { results.push_back(r); }, themed));
    const std::vector<Rect> boxes = button_boxes(*dialog);
    AURORA_TEST_REQUIRE_EQ(boxes.size(), 2U);
    AURORA_TEST_CHECK(boxes[0].origin.y < boxes[1].origin.y);  // 接受钮在上
    const float right_0 = boxes[0].origin.x + boxes[0].size.width;
    const float right_1 = boxes[1].origin.x + boxes[1].size.width;
    AURORA_TEST_CHECK_NEAR(right_0, right_1, 0.5F);  // 两钮右缘对齐
    AURORA_TEST_CHECK_NEAR(right_1, card_right(*dialog), 0.5F);  // 整列靠卡片右缘
    // 钮间距读 `spacing.medium` 令牌（Material 基线档实测 8，与接线前的字面量同值）。
    AURORA_TEST_CHECK_NEAR(boxes[1].origin.y - (boxes[0].origin.y + boxes[0].size.height), themed.spacing.medium, 0.5F);
    AURORA_TEST_CHECK(click_box(*dialog, boxes[0], results) == std::optional{true});
    AURORA_TEST_CHECK(click_box(*dialog, boxes[1], results) == std::optional{false});
}

AURORA_TEST_CASE(confirm_spacing_gap_follows_theme_token) {
    // 令牌消费：把 `spacing.medium` 改成 18 后实测行内间距随之变化（证明读的是主题而非字面量）。
    const auto measure = [](float medium) -> float {
        Theme themed = Theme::light();
        themed.spacing.medium = medium;
        auto dialog = open_dialog(confirm("Title", AURORA_LONG_MESSAGE, {}, themed));
        const std::vector<Rect> boxes = button_boxes(*dialog);
        if (boxes.size() < 2U) {
            return -1.0F;
        }
        return boxes[1].origin.x - (boxes[0].origin.x + boxes[0].size.width);
    };
    AURORA_TEST_CHECK_NEAR(measure(8.0F), 8.0F, 0.5F);  // 基线档 = 接线前的字面量
    AURORA_TEST_CHECK_NEAR(measure(18.0F), 18.0F, 0.5F);  // 改令牌即改实测
}

AURORA_TEST_CASE(alert_single_button_wraps_only_in_non_default_arms) {
    // AcceptFirst 档：按钮直接作 Column 子项（与接线前同构，不多包一层）；
    // 其余档：包一层 End 容器，把单钮推到卡片右缘。
    struct Arm {
        ThemeStyle style;
        bool hugs_left;
    };
    static constexpr Arm ARMS[] = {
        {.style = ThemeStyle::Material, .hugs_left = true}, {.style = ThemeStyle::Windows, .hugs_left = true},
        {.style = ThemeStyle::Kde, .hugs_left = true},      {.style = ThemeStyle::Gnome, .hugs_left = false},
        {.style = ThemeStyle::MacOs, .hugs_left = false},
    };
    for (const Arm &arm : ARMS) {
        auto dialog =
            open_dialog(alert("Title", AURORA_LONG_MESSAGE, {}, Theme::from_style(arm.style, ColorScheme::Light)));
        const std::vector<Rect> boxes = button_boxes(*dialog);
        AURORA_TEST_REQUIRE_EQ(boxes.size(), 1U);
        const float left = dialog->child_nodes().front().bounds().origin.x;
        if (arm.hugs_left) {
            AURORA_TEST_CHECK_NEAR(boxes[0].origin.x, left, 0.5F);  // 未包层：与卡片左缘同值
        } else {
            AURORA_TEST_CHECK_NEAR(boxes[0].origin.x + boxes[0].size.width, card_right(*dialog), 0.5F);  // 右靠档
            AURORA_TEST_CHECK(boxes[0].origin.x > left + 1.0F);
        }
    }
}

AURORA_TEST_CASE(confirm_title_alignment_follows_convention) {
    // 标题居中惯例：`dialog_title_alignment = Centered` 时标题盒被拉到卡片同宽（居中因此才有自由
    // 空间可用），`Leading` 档保持自身文本宽。五套标定档目前全取 Leading（无出处不改，见
    // src/aurora/theming/theme_presets.cpp 的惯例覆写行），故本用例用手工改过的主题验证消费点真实存在。
    const auto widths = [](const Theme &themed) -> std::pair<float, float> {
        auto dialog = open_dialog(confirm("Title", AURORA_LONG_MESSAGE, {}, themed));
        const std::vector<Node> &kids = dialog->child_nodes();
        if (kids.empty()) {
            return {-1.0F, -1.0F};
        }
        const Node &column = kids.front();
        const std::vector<Node> &inner = column.widget().child_nodes();
        if (inner.empty()) {
            return {-1.0F, column.bounds().size.width};
        }
        return {inner.front().bounds().size.width, column.bounds().size.width};  // 标题宽 / 卡片宽
    };

    const auto [leading_width, card_width] = widths(Theme::light());
    AURORA_TEST_REQUIRE(leading_width > 0.0F);
    AURORA_TEST_REQUIRE(card_width > 0.0F);

    Theme centered = Theme::light();
    centered.conventions.dialog_title_alignment = DialogTitleAlignment::Centered;
    const auto [centered_width, centered_card] = widths(centered);
    AURORA_TEST_REQUIRE(centered_width > 0.0F);

    AURORA_TEST_CHECK(leading_width < card_width - 1.0F);  // Leading：标题只占自身文本宽
    AURORA_TEST_CHECK_NEAR(centered_width, centered_card, 0.5F);  // Centered：标题盒 = 卡片宽，居中才有落点
}

}  // namespace aurora::test_cases::utest_dialog
