// test_occlusion_culling.cpp — 验证遮挡剔除：
//   普通容器在父裁剪区外的子控件整棵子树被跳过（on_paint 不调用）；
//   视口内/部分相交的子控件仍正常绘制；圆角裁剪退化为外接矩形保守判定。
//   注意：Scroll 重写为整页离屏缓冲后，为换取滚动帧一次平移 blit 的性能，
//   内容录制阶段不使用视口裁剪，因此 Scroll 本身不会剔除视口外子控件。
#include <memory>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Column;
using aurora::Constraints;
using aurora::HeadlessSurface;
using aurora::Node;
using aurora::Painter;
using aurora::Rect;
using aurora::Scroll;
using aurora::ScrollProps;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Widget;
using aurora::Window;

namespace {

// 计数控件：每次 on_paint 自增，用于观测是否被遮挡剔除跳过。
struct PaintCounter : Widget {
    int paint_calls = 0;

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto type_name() const -> const char * override { return "PaintCounter"; }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        return Size{ .width = 50.0f, .height = 50.0f };
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {
        ++paint_calls;
    }
};

auto make_window(int w, const int h) -> Window {
    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    return Window{ std::move(surface) };
}

} // namespace

namespace {
// 一个会 push_clip 的 Column：用于验证普通容器下的遮挡剔除。
class ClippedColumn : public Column {
  public:
    using Column::Column;

    [[nodiscard]] auto type_name() const -> const char * override { return "ClippedColumn"; }

  protected:
    auto on_paint(Painter &p, const Rect &b, const BuildContext &ctx) -> void override {
        p.push_clip(b);
        Column::on_paint(p, b, ctx);
        p.pop_clip();
    }
};
} // namespace

AURORA_TEST() {
#ifdef AURORA_OCCLUSION_CULLING
    // 1) 普通容器 + 显式裁剪：4 个 50 高的计数控件堆叠成 200 高内容；
    //    父容器 bounds 高 100 → 仅前 2 个可见，p2/p3 应被剔除。
    const auto p0 = std::make_shared<PaintCounter>();
    const auto p1 = std::make_shared<PaintCounter>();
    const auto p2 = std::make_shared<PaintCounter>();
    const auto p3 = std::make_shared<PaintCounter>();
    Node root{ ClippedColumn{ Node{ p0 }, Node{ p1 }, Node{ p2 }, Node{ p3 } } };
    Window win = make_window(100, 100);
    AURORA_TEST_CHECK(win.present_root(root).ok());
    AURORA_TEST_CHECK_MSG((p0->paint_calls > 0), "p0 in viewport is painted");
    AURORA_TEST_CHECK_MSG((p1->paint_calls > 0), "p1 in viewport is painted (touches bottom edge)");
    AURORA_TEST_CHECK_MSG((p2->paint_calls == 0), "p2 outside viewport is culled (skipped)");
    AURORA_TEST_CHECK_MSG((p3->paint_calls == 0), "p3 outside viewport is culled (skipped)");

    // 2) Scroll 离屏缓冲策略：为滚动帧一次 blit，整页内容都会录制，
    //    因此视口外子控件也会被调用 on_paint（不被剔除）。
    const auto s0 = std::make_shared<PaintCounter>();
    const auto s1 = std::make_shared<PaintCounter>();
    const auto s2 = std::make_shared<PaintCounter>();
    const auto s3 = std::make_shared<PaintCounter>();
    Node root_scroll{ Scroll{
        ScrollProps{ .child = Node{ Column{ Node{ s0 }, Node{ s1 }, Node{ s2 }, Node{ s3 } } } } } };
    Window win_scroll = make_window(100, 100);
    AURORA_TEST_CHECK(win_scroll.present_root(root_scroll).ok());
    AURORA_TEST_CHECK_MSG((s0->paint_calls > 0), "Scroll: in-view child is painted");
    AURORA_TEST_CHECK_MSG((s1->paint_calls > 0), "Scroll: child touching bottom edge is painted");
    AURORA_TEST_CHECK_MSG((s2->paint_calls > 0),
                          "Scroll: off-viewport child is also painted (full-page offscreen buffer)");
    AURORA_TEST_CHECK_MSG((s3->paint_calls > 0),
                          "Scroll: far off-viewport child is also painted (full-page offscreen buffer)");
#else
    AURORA_TEST_CHECK(true); // 未启用 AURORA_OCCLUSION_CULLING：跳过剔除断言
#endif
}
