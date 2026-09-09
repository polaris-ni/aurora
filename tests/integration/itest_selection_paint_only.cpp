/// 测试类型: integration
/// 目标单元: include/aurora/event/dispatcher.h
/// 测试说明: 验证文本拖选只触发「重绘」而非「重排」：最大化/大窗场景下拖选不再每帧全树重排。
///           HeadlessSurface + 间谍控件统计 layout/paint 调用：拖选 Move 仅 mark_needs_paint，
///           第二帧跳过整树重排（layout_calls 不变）；未变子控件经 Display List 缓存回放，
///           on_paint 不再调用（该断言受 AURORA_ENABLE_DISPLAY_LIST 门控，关闭时仅提示跳过）。

#include <memory>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_selection_paint_only {

namespace {

using au::BuildContext;
using au::Color;
using au::Column;
using au::Constraints;
using au::EventDispatcher;
using au::FocusManager;
#ifdef AURORA_BACKEND_HEADLESS
using au::HeadlessSurface;  // 仅 AURORA_BACKEND_HEADLESS 下编译；宏关闭时本文件走 SKIP 桩
#endif
using au::MouseButton;
using au::MouseAction;
using au::MouseEvent;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;
using au::Text;
using au::Window;

/// 间谍控件：统计 layout/paint 调用次数，观测拖选帧是否跳过重排/重绘。
class SpyWidget : public au::LeafWidget {
  public:
    int layout_calls = 0;
    int paint_calls = 0;

    [[nodiscard]] auto type_name() const -> const char * override { return "SpyWidget"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        ++layout_calls;
        return c.constrain(Size{.width = 100.0F, .height = 30.0F});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        ++paint_calls;
        p.fill_rect(bounds, Color{200, 200, 200, 255});
    }
};

}  // namespace

AURORA_TEST_CASE(drag_selection_paint_only_skips_relayout) {
#ifdef AURORA_BACKEND_HEADLESS
    // 512×512 模拟最大化/大窗：拖选若引发全树重排会被本用例暴露。
    constexpr int w = 512;
    constexpr int h = 512;

    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(w, h);  // 先确立尺寸（present_root 首帧读取 size 布局整树）
    auto *raw = surface.get();
    Window win{std::move(surface)};

    const auto text = std::make_shared<Text>("hello world this is a selection test for drag");
    const auto spy = std::make_shared<SpyWidget>();
    Node root{Column{Node{text}, Node{spy}}};

    // 帧 1：挂载 + 布局 + 绘制（建立命中几何与文本行缓存）。
    AURORA_TEST_CHECK(win.present_root(root).ok());
    AURORA_TEST_CHECK_EQ(raw->frame_count(), 1);
    AURORA_TEST_CHECK_EQ(spy->layout_calls, 1);
    AURORA_TEST_CHECK_EQ(spy->paint_calls, 1);

    // 命中 Text 中心：拖选经 EventDispatcher 静态派发（携带 FocusManager）。
    FocusManager fm;
    fm.set_root(&root.widget());

    const Rect tb = text->focus_bounds();
    const float cx = tb.origin.x + (tb.size.width * 0.5F);
    const float cy = tb.origin.y + (tb.size.height * 0.5F);

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.position = Point{.x = cx, .y = cy};
    (void)EventDispatcher::dispatch(root.widget(), press, &fm);

    // Move 期间（选区拖拽中）只 mark_needs_paint：paint-only 脏。
    MouseEvent move;
    move.action = MouseAction::Move;
    move.button = MouseButton::Left;
    move.position = Point{.x = cx + 60.0F, .y = cy};
    (void)EventDispatcher::dispatch(root.widget(), move, &fm);
    AURORA_TEST_CHECK_MSG(text->has_selection(), "drag established a selection (dispatch reached Text)");

    // 帧 2：拖选只标 paint 脏 → 应仅重绘，跳过整树重排。
    AURORA_TEST_CHECK(win.present_root(root).ok());
    AURORA_TEST_CHECK_EQ(raw->frame_count(), 2);
    AURORA_TEST_CHECK_EQ(spy->layout_calls, 1);  // 关键：重排被跳过

#ifdef AURORA_ENABLE_DISPLAY_LIST
    // 选区内无关子控件（spy）内容未变：Display List 命中直接回放，不再调用 on_paint。
    // （frame_count==2 已确认本帧重新上屏；layout_calls==1 确认未重排。）
    AURORA_TEST_CHECK_EQ(spy->paint_calls, 1);
#else
    AURORA_TEST_TRACE("AURORA_ENABLE_DISPLAY_LIST 未开启：无 DL 回放，跳过 paint 计数断言");
#endif
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，HeadlessSurface 未编译");
#endif
}

}  // namespace aurora::test_cases::itest_selection_paint_only
