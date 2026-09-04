// test_layout_cache.cpp — 验证布局约束缓存：
//   1) 稳态帧（无变更）命中缓存，整棵子树跳过 on_layout；
//   2) 单点脏（叶子 mark_needs_layout）仅重排脏链，兄弟子树缓存命中被跳过；
//   3) 约束未变时缓存键相等；
//   4) RelayoutBoundary 语义（固定/撑满宽高的控件为边界）。
#include <memory>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Column;
using aurora::Constraints;
using aurora::HeadlessSurface;
using aurora::Length;
using aurora::Node;
using aurora::Painter;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Widget;
using aurora::Window;

namespace {

// 计数叶子：每次 on_layout 自增，用于观测缓存是否跳过布局。
struct LeafCounter : Widget {
    int layout_calls_ = 0;

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    auto type_name() const -> const char * override { return "LeafCounter"; }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        ++layout_calls_;
        return Size{.width = 50.0F, .height = 50.0F};
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

auto make_window(int w, int h) -> Window {
    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    return Window{std::move(surface)};
}

}  // namespace

AURORA_TEST() {
    const auto a = std::make_shared<LeafCounter>();
    const auto b = std::make_shared<LeafCounter>();
    Node root{Column{Node{a}, Node{b}}};

    Window win = make_window(300, 300);
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 1：首帧全量布局
    AURORA_TEST_CHECK_EQ(a->layout_calls_, 1);
    AURORA_TEST_CHECK_EQ(b->layout_calls_, 1);

    // 稳态帧：无任何变更 → 根缓存命中，整棵子树 on_layout 被跳过。
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 2
    AURORA_TEST_CHECK_EQ(a->layout_calls_, 1);
    AURORA_TEST_CHECK_EQ(b->layout_calls_, 1);

    // 单点脏：仅叶子 A 标脏 → A 重排（count=2），兄弟 B 缓存命中被跳过（count 不变）。
    a->mark_needs_layout();
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 3
    AURORA_TEST_CHECK_EQ(a->layout_calls_, 2);
    AURORA_TEST_CHECK_EQ(b->layout_calls_, 1);

    // 约束未变时缓存键相等（派生验证：再次稳态帧仍跳过）。
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 4
    AURORA_TEST_CHECK_EQ(a->layout_calls_, 2);
    AURORA_TEST_CHECK_EQ(b->layout_calls_, 1);

    // RelayoutBoundary 语义：默认 WrapContent 不是边界；固定/撑满宽高是边界。
    const LeafCounter probe;
    AURORA_TEST_CHECK_FALSE(probe.is_relayout_boundary());
    const auto fixed = std::make_shared<LeafCounter>();
    fixed->width(Length::fixed(40.0F));
    AURORA_TEST_CHECK_TRUE(fixed->is_relayout_boundary());
    const auto expanded = std::make_shared<LeafCounter>();
    expanded->height(Length::expand());
    AURORA_TEST_CHECK_TRUE(expanded->is_relayout_boundary());

    // RelayoutBoundary 控件标脏后仍能正确失效并重排（缓存不污染固定尺寸控件）。
    {
        const auto boundary = std::make_shared<LeafCounter>();
        boundary->width(Length::fixed(60.0F));
        Node r2{Column{Node{boundary}}};
        auto w = make_window(200, 200);
        AURORA_TEST_CHECK(w.present_root(r2).ok());  // 首帧
        AURORA_TEST_CHECK_EQ(boundary->layout_calls_, 1);
        boundary->mark_needs_layout();  // 固定尺寸边界标脏
        AURORA_TEST_CHECK(w.present_root(r2).ok());  // 重排
        AURORA_TEST_CHECK_EQ(boundary->layout_calls_, 2);
    }
}
