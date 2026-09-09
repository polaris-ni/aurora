/// 测试类型: integration
/// 目标单元: include/aurora/widget/widget.h
/// 测试说明: 验证遮挡剔除——普通容器在父裁剪区外的子控件整棵子树被跳过（on_paint 不调用），
///           视口内/部分相交的子控件仍正常绘制；Scroll 重写为滑动窗口离屏缓冲后，内容录制阶段
///           不使用视口裁剪，视口外子控件也会被调用 on_paint（剔除部分受
///           AURORA_ENABLE_OCCLUSION_CULLING 门控，Headless 部分受 AURORA_BACKEND_HEADLESS 门控）

#include <memory>
#include <utility>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_occlusion_culling {

namespace {

// 计数控件：每次 on_paint 自增，用于观测是否被遮挡剔除跳过。
class PaintCounter : public au::LeafWidget {
  public:
    int paint_calls = 0;

    [[nodiscard]] auto type_name() const -> const char * override { return "PaintCounter"; }

  protected:
    auto on_layout(const au::Constraints & /*c*/, const au::BuildContext & /*ctx*/) -> au::Size override {
        return au::Size{.width = 50.0F, .height = 50.0F};
    }
    auto on_paint(au::Painter & /*p*/, const au::Rect & /*bounds*/, const au::BuildContext & /*ctx*/) -> void override {
        ++paint_calls;
    }
};

#ifdef AURORA_BACKEND_HEADLESS
auto make_window(int w, const int h) -> au::Window {
    auto surface = std::make_unique<au::HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    return au::Window{std::move(surface)};
}
#endif

}  // namespace

namespace {

// 一个会 push_clip 的 Column：用于验证普通容器下的遮挡剔除。
class ClippedColumn : public au::Column {
  public:
    using Column::Column;

    [[nodiscard]] auto type_name() const -> const char * override { return "ClippedColumn"; }

  protected:
    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) -> void override {
        p.push_clip(b);
        au::Column::on_paint(p, b, ctx);
        p.pop_clip();
    }
};

}  // namespace

AURORA_TEST_CASE(clipped_column_culls_children_outside_clip) {
#ifdef AURORA_BACKEND_HEADLESS
#ifdef AURORA_ENABLE_OCCLUSION_CULLING
    // 4 个 50 高的计数控件堆叠成 200 高内容；父容器 bounds 高 100 → 仅前 2 个可见，p2/p3 应被剔除。
    const auto p0 = std::make_shared<PaintCounter>();
    const auto p1 = std::make_shared<PaintCounter>();
    const auto p2 = std::make_shared<PaintCounter>();
    const auto p3 = std::make_shared<PaintCounter>();
    au::Node root{ClippedColumn{au::Node{p0}, au::Node{p1}, au::Node{p2}, au::Node{p3}}};
    au::Window win = make_window(100, 100);
    AURORA_TEST_CHECK(win.present_root(root).ok());
    AURORA_TEST_CHECK_MSG(p0->paint_calls > 0, "p0 in viewport is painted");
    AURORA_TEST_CHECK_MSG(p1->paint_calls > 0, "p1 in viewport is painted (touches bottom edge)");
    AURORA_TEST_CHECK_MSG(p2->paint_calls == 0, "p2 outside viewport is culled (skipped)");
    AURORA_TEST_CHECK_MSG(p3->paint_calls == 0, "p3 outside viewport is culled (skipped)");
#else
    AURORA_TEST_SKIP("AURORA_ENABLE_OCCLUSION_CULLING 未开启，剔除逻辑未编译");
#endif
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，HeadlessSurface 未编译");
#endif
}

AURORA_TEST_CASE(scroll_offscreen_buffer_paints_offscreen_children) {
#ifdef AURORA_BACKEND_HEADLESS
    // Scroll 离屏缓冲策略：滚动帧一次 blit，有界缓冲内容都会录制，
    // 因此视口外子控件也会被调用 on_paint（不被剔除）。
    const auto s0 = std::make_shared<PaintCounter>();
    const auto s1 = std::make_shared<PaintCounter>();
    const auto s2 = std::make_shared<PaintCounter>();
    const auto s3 = std::make_shared<PaintCounter>();
    au::Node root_scroll{
        au::Scroll{au::ScrollProps{.child = au::Node{au::Column{au::Node{s0}, au::Node{s1}, au::Node{s2},
                                                                au::Node{s3}}}}}};
    au::Window win_scroll = make_window(100, 100);
    AURORA_TEST_CHECK(win_scroll.present_root(root_scroll).ok());
    AURORA_TEST_CHECK_MSG(s0->paint_calls > 0, "Scroll: in-view child is painted");
    AURORA_TEST_CHECK_MSG(s1->paint_calls > 0, "Scroll: child touching bottom edge is painted");
    AURORA_TEST_CHECK_MSG(s2->paint_calls > 0, "Scroll: off-viewport child is also painted (offscreen buffer)");
    AURORA_TEST_CHECK_MSG(s3->paint_calls > 0, "Scroll: far off-viewport child is also painted (offscreen buffer)");
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，HeadlessSurface 未编译");
#endif
}

}  // namespace aurora::test_cases::itest_occlusion_culling
