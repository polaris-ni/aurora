// 验证文本拖选只触发「重绘」而非「重排」：最大化/大窗场景下拖选不再每帧全树重排。
// 基于 HeadlessSurface + 间谍控件统计 layout/paint 调用。
#include <memory>

#include "aurora/event/dispatcher.h"
#include "aurora/event/focus.h"
#include "aurora/render/painter.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

#include "test_harness.h"

namespace au = aurora;

namespace {

class SpyWidget : public au::LeafWidget {
  public:
    int layout_calls = 0;
    int paint_calls = 0;

    [[nodiscard]] auto type_name() const -> const char * override { return "SpyWidget"; }
    [[nodiscard]] static auto describe_static() -> au::WidgetDescriptor {
        return au::WidgetDescriptor{ .name = "SpyWidget" };
    }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override { return describe_static(); }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        ++layout_calls;
        return c.constrain(au::Size{ .width = 100.0f, .height = 30.0f });
    }
    auto on_paint(au::Painter &p, const au::Rect &bounds, const au::BuildContext & /*ctx*/) -> void override {
        ++paint_calls;
        p.fill_rect(bounds, au::Color{ 200, 200, 200, 255 });
    }
};

} // namespace

AURORA_TEST() {
    au::HeadlessSurface *raw = nullptr;
    auto surface = std::make_unique<au::HeadlessSurface>();
    (void)surface->begin_frame(512, 512);
    raw = surface.get();
    auto win = au::Window{ std::move(surface) };

    auto text = std::make_shared<au::Text>(au::LocalizedString{ "hello world this is a selection test for drag" });
    auto spy = std::make_shared<SpyWidget>();
    au::Node root = au::Column{ au::Node{ std::static_pointer_cast<au::Widget>(text) },
                                au::Node{ std::static_pointer_cast<au::Widget>(spy) } };

    // 首帧：挂载 + 布局 + 绘制（建立命中几何与文本行缓存）
    auto r1 = win.present_root(root);
    AURORA_TEST_CHECK(r1.ok());
    AURORA_TEST_CHECK(raw->frame_count() == 1);
    AURORA_TEST_CHECK(spy->layout_calls == 1);
    AURORA_TEST_CHECK(spy->paint_calls == 1);

    // 设置焦点管理（命中 Text 用静态派发器）
    au::FocusManager fm;
    fm.set_root(root.operator->());

    const au::Rect tb = text->focus_bounds();
    const float cx = tb.origin.x + (tb.size.width * 0.5f);
    const float cy = tb.origin.y + (tb.size.height * 0.5f);

    // 模拟拖选：Press 起点 → Move 到偏移位置（m_selecting 期间 Move 只 mark_needs_paint）
    au::MouseEvent press;
    press.action = au::MouseAction::Press;
    press.position = au::Point{ .x = cx, .y = cy };
    au::EventDispatcher::dispatch(*root.operator->(), press, &fm);
    au::MouseEvent move;
    move.action = au::MouseAction::Move;
    move.position = au::Point{ .x = cx + 60.0f, .y = cy };
    au::EventDispatcher::dispatch(*root.operator->(), move, &fm);

    // 第二帧：拖选只标 paint 脏 → 应仅重绘，跳过整树重排
    auto r2 = win.present_root(root);
    AURORA_TEST_CHECK(r2.ok());
    AURORA_TEST_CHECK(raw->frame_count() == 2);
    AURORA_TEST_CHECK(spy->layout_calls == 1); // 关键：重排被跳过
    // 选区内子控件(spy)内容未变：Display List 命中直接回放，不再调用 on_paint
    // （frame_count==2 已确认本帧重新上屏；layout_calls==1 确认未重排；此为 DL 优化下的预期行为）。
    AURORA_TEST_CHECK(spy->paint_calls == 1);
}
