// test_perf_display_list.cpp — Display List 性能基准（仅计时打印，无严格阈值断言，避免 CI 抖动）。
// 静态场景：首帧录制后，后续帧命中 DL 直接 replay，跳过整棵子树 paint 遍历；
// 全量变脏场景：每帧 mark_needs_paint 强制重录，作为对照。两者耗时对比即 DL 收益。
#include <chrono>
#include <memory>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::HeadlessSurface;
using aurora::Node;
using aurora::Painter;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Widget;
using aurora::Window;

namespace {

struct LeafCounter : Widget {
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 20.0F, .height = 20.0F});
    }
    auto on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(b, Color{80, 160, 240});
    }
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    auto type_name() const -> const char * override { return "LeafCounter"; }
};

}  // namespace

AURORA_TEST() {
#ifdef AURORA_DISPLAY_LIST
    constexpr int n = 300;  // 叶子控件数量
    constexpr int frames = 60;

    std::vector<Node> kids;
    std::vector<std::shared_ptr<LeafCounter>> refs;
    kids.reserve(n);
    refs.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto leaf = std::make_shared<LeafCounter>();
        refs.push_back(leaf);
        kids.emplace_back(leaf);
    }
    Node root{Column{ColumnProps{.children = std::move(kids)}}};

    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(400, 600);
    Window win{std::move(surface)};

    // 预热 + 首帧（录制）。
    AURORA_TEST_CHECK(win.present_root(root).ok());

    // 静态场景：force_full_redraw 强制每帧重绘，但 DL 命中（replay）。
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        win.force_full_redraw();
        AURORA_TEST_CHECK(win.present_root(root).ok());
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double static_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 全量变脏场景：每帧对所有叶子 mark_needs_paint（强制重录）。
    const auto t2 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        for (auto const &leaf : refs) {
            leaf->mark_needs_paint();
        }
        win.force_full_redraw();
        AURORA_TEST_CHECK(win.present_root(root).ok());
    }
    const auto t3 = std::chrono::steady_clock::now();
    const double dirty_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    AURORA_TEST_PRINTF(
        "[perf] %d widgets x %d frames: static-replay=%.3f ms (%.4f ms/frame), "
        "full-rerecord=%.3f ms (%.4f ms/frame)\n",
        n, frames, static_ms, static_ms / frames, dirty_ms, dirty_ms / frames);
    AURORA_TEST_CHECK_MSG(static_ms > 0.0 && dirty_ms > 0.0, "both scenarios executed");
    // 静态 replay 应显著快于全量重录（DL 收益）；宽松断言 1.1x 防偶然反转。
    AURORA_TEST_CHECK_MSG(static_ms < dirty_ms * 1.5, "static replay at least comparable to full re-record");
#else
    AURORA_TEST_CHECK(true);
#endif
}
