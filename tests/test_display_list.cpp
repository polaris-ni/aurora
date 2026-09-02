// test_display_list.cpp — 验证 Display List 录制/回放：
//   ① replay 像素保真（首帧「录制+回放」与强制重绘帧「仅回放」逐位一致）；
//   ② 命中回放时整棵子树 paint 遍历被跳过（on_paint 不再调用）；
//   ③ 内容变脏（mark_needs_paint）触发祖先 DL 失效并重录，像素与全绘一致；
//   ④ 离屏合成（cache_layer）的 Composite 命令录制/回放正确。

#include <cstring>
#include <memory>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Column;
using aurora::Constraints;
using aurora::HeadlessSurface;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Widget;
using aurora::Window;

namespace {

// 计数控件：on_paint 自增并填充纯色，用于观测 DL 是否跳过子树遍历。
struct PaintCounter : Widget {
    int paint_calls = 0;
    Color fill = Color{ 200, 30, 30 };
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = 40.0f, .height = 40.0f });
    }
    auto on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) -> void override {
        ++paint_calls;
        p.fill_rect(b, fill);
    }
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    auto type_name() const -> const char * override { return "PaintCounter"; }
};

auto const make_window(int w, const int h) -> Window {
    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    return Window{ std::move(surface) };
}

auto copy_pixels(const std::uint8_t *src, const size_t n) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out(n);
    if (src != nullptr) {
        std::memcpy(out.data(), src, n);
    }
    return out;
}

auto pixel_diff(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) -> size_t {
    size_t d = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a[i] != b[i]) {
            ++d;
        }
    }
    return d;
}

} // namespace

AURORA_TEST() {
#ifdef AURORA_DISPLAY_LIST
    constexpr int w = 100;
    constexpr int h = 100;
    constexpr size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

    // 场景 1：replay 保真 + 跳过子树遍历。
    {
        auto c = std::make_shared<PaintCounter>();
        Node root{ Column{ Node{ c } } };
        Window win = make_window(w, h);
        AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧1：录制 + 回放
        auto &hs = dynamic_cast<HeadlessSurface &>(win.surface());
        AURORA_TEST_CHECK(hs.data() != nullptr);
        auto f1 = copy_pixels(hs.data(), n);
        AURORA_TEST_CHECK_MSG((c->paint_calls == 1), "frame1 records subtree (on_paint once)");

        win.force_full_redraw();                        // 强制重绘（布局脏 + 脏区全窗），但 DL 仍有效
        AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧2：仅回放（命中 DL）
        auto f2 = copy_pixels(hs.data(), n);
        AURORA_TEST_CHECK_MSG((c->paint_calls == 1),
                              "frame2 DL-hit skips subtree paint traversal (on_paint not re-called)");
        AURORA_TEST_CHECK_MSG((pixel_diff(f1, f2) == 0), "replay pixels identical to record+replay");
    }

    // 场景 2：内容变脏触发祖先 DL 失效并重录。
    {
        auto c = std::make_shared<PaintCounter>();
        Node root{ Column{ Node{ c } } };
        Window win = make_window(w, h);
        AURORA_TEST_CHECK(win.present_root(root).ok());
        auto &hs = dynamic_cast<HeadlessSurface &>(win.surface());
        auto before = copy_pixels(hs.data(), n);
        AURORA_TEST_CHECK_MSG((c->paint_calls == 1), "before-change records once");

        c->mark_needs_paint(); // 内容脏：须沿布局父链失效祖先 DL 并重录
        AURORA_TEST_CHECK(win.present_root(root).ok());
        auto after = copy_pixels(hs.data(), n);
        AURORA_TEST_CHECK_MSG((c->paint_calls == 2), "dirty triggers re-record (on_paint called again)");
        AURORA_TEST_CHECK_MSG((pixel_diff(before, after) == 0), "same content re-record is pixel-consistent");

        win.force_full_redraw();
        AURORA_TEST_CHECK(win.present_root(root).ok());
        auto ffull = copy_pixels(hs.data(), n);
        AURORA_TEST_CHECK_MSG((pixel_diff(after, ffull) == 0), "re-record matches full redraw");
    }

    // 场景 3：离屏合成（cache_layer）Composite 命令录制/回放。
    {
        auto c = std::make_shared<PaintCounter>();
        c->modifier = Modifier{}.cache_layer(); // 走离屏像素缓存 + composite
        Node root{ Column{ Node{ c } } };
        Window win = make_window(w, h);
        AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧1：录制（含 Composite）+ 回放
        auto &hs = dynamic_cast<HeadlessSurface &>(win.surface());
        auto f1 = copy_pixels(hs.data(), n);
        AURORA_TEST_CHECK_MSG((c->paint_calls == 1), "cache_layer frame1 records once");

        win.force_full_redraw();
        AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧2：仅回放（命中 DL，Composite 命令重贴离屏缓冲）
        auto f2 = copy_pixels(hs.data(), n);
        AURORA_TEST_CHECK_MSG((c->paint_calls == 1), "cache_layer frame2 DL-hit skips subtree");
        AURORA_TEST_CHECK_MSG((pixel_diff(f1, f2) == 0), "cache_layer replay composites offscreen buffer correctly");
    }
#else
    AURORA_TEST_CHECK(true); // 未启用 AURORA_DISPLAY_LIST：跳过断言
#endif
}
