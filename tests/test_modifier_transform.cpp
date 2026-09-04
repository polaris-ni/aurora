// 修饰节点补全验证：Matrix2D 数学、opacity 透明度、rotate/scale 仿射变换渲染与逆矩阵命中。
// 无头 layout + paint（离屏合成）+ hit_test_chain 驱动，不依赖任何 GUI 后端。
// 控件均包裹进根容器（Stack）以按真实布局尺寸绘制/命中，与框架真实用法一致。

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::LeafWidget;
using aurora::Matrix2D;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Stack;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace {

// 实心色块控件：在内容盒内填充纯色，用于确定性像素 / 命中验证。
class SolidBox : public LeafWidget {
  public:
    Size sz_{.width = 100.0F, .height = 20.0F};
    Color color_{255, 0, 0, 255};
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SolidBox"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "SolidBox", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz_); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override { p.fill_rect(b, color_); }
};

struct RenderResult {
    std::vector<std::uint8_t> pixels_;
    int w_ = 0;
    int h_ = 0;
    [[nodiscard]] auto at(int x, int y, int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w_) + x) * 4;
        return pixels_.at(off + ch);
    }
};

// 把控件包裹进根容器渲染，返回主缓冲像素（控件按真实布局尺寸绘制）。
auto render_in_root(std::shared_ptr<Widget> w, const int ww, const int hh) -> RenderResult {
    auto const root = std::make_shared<Stack>(std::vector{Node{std::move(w)}});
    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)};
    root->layout(c, ctx);
    Painter p;
    p.begin(ww, hh);
    root->paint(p,
                Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                     .size = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)}},
                ctx);
    const std::uint8_t *d = p.data();
    RenderResult r;
    r.w_ = ww;
    r.h_ = hh;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    r.pixels_.assign(d, d + (static_cast<std::size_t>(ww) * hh * 4));
    return r;
}

// 把控件包裹进根容器做命中测试，返回命中链中的 widget 指针列表。
auto hit_in_root(std::shared_ptr<Widget> w, const int ww, const int hh, Point pt) -> std::vector<Widget *> {
    auto const root = std::make_shared<Stack>(std::vector{Node{std::move(w)}});
    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)};
    root->layout(c, ctx);
    const auto chain =
        root->hit_test_chain(pt,
                             Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                                  .size = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)}},
                             ctx);
    std::vector<Widget *> out;
    for (const auto &h : chain) {
        // HitNode::get() 兼顾栈对象（裸指针）与 shared_ptr 持有控件（弱引用判活）。
        if (Widget *sp = h.get()) {
            out.push_back(sp);
        }
    }
    return out;
}

}  // namespace

AURORA_TEST() {
    // ---------- A. Matrix2D 数学 ----------
    {
        const Matrix2D r = Matrix2D::from_rotate(90.0F);
        const Point p1 = r.apply_to_point(Point{.x = 1.0F, .y = 0.0F});
        AURORA_TEST_CHECK_MSG(std::abs(p1.x) < 1e-3F && std::abs(p1.y - 1.0F) < 1e-3F, "rotate90 maps (1,0) -> (0,1)");

        const Matrix2D inv = r.inverse();
        const Point p2 = inv.apply_to_point(Point{.x = 0.0F, .y = 1.0F});
        AURORA_TEST_CHECK_MSG(std::abs(p2.x - 1.0F) < 1e-3F && std::abs(p2.y) < 1e-3F,
                              "inverse(rotate90) maps (0,1) -> (1,0)");

        const Matrix2D id = r.compose(r.inverse());
        AURORA_TEST_CHECK_MSG(id.is_identity(), "rotate90 ∘ inverse(rotate90) = identity");

        const Matrix2D t = Matrix2D::from_translate(5.0F, 7.0F);
        const Point p3 = t.apply_to_point(Point{.x = 2.0F, .y = 3.0F});
        AURORA_TEST_CHECK_MSG(std::abs(p3.x - 7.0F) < 1e-3F && std::abs(p3.y - 10.0F) < 1e-3F, "translate(5,7)");

        const Matrix2D s = Matrix2D::from_scale(2.0F, 3.0F);
        const Point p4 = s.apply_to_point(Point{.x = 4.0F, .y = 5.0F});
        AURORA_TEST_CHECK_MSG(std::abs(p4.x - 8.0F) < 1e-3F && std::abs(p4.y - 15.0F) < 1e-3F, "scale(2,3)");

        // 退化：缩放 0 求逆降级为单位矩阵（不崩溃）。
        const Matrix2D degenerate = Matrix2D::from_scale(0.0F, 1.0F);
        AURORA_TEST_CHECK_MSG(degenerate.inverse().is_identity(), "degenerate matrix inverse degrades to identity");

        // 绕中心旋转：点 (cx+1, cy) 绕 (cx,cy) 旋转 90 -> (cx, cy+1)。
        constexpr Point c{.x = 40.0F, .y = 25.0F};
        const Matrix2D rc = Matrix2D::from_rotate_about(90.0F, c);
        const Point p5 = rc.apply_to_point(Point{.x = c.x + 1.0F, .y = c.y});
        AURORA_TEST_CHECK_MSG(std::abs(p5.x - c.x) < 1e-3F && std::abs(p5.y - (c.y + 1.0F)) < 1e-3F,
                              "rotate_about center maps (cx+1,cy) -> (cx,cy+1)");
    }

    // ---------- B. opacity 透明度（与不透明对照，背景为不透明黑）----------
    {
        auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.opacity(0.5F));
        const auto r = render_in_root(sb, 200, 200);
        // 控件布局为 (0,0,100,20)，中心 (50,10)。
        const std::uint8_t red = r.at(50, 10, 0);
        const std::uint8_t a = r.at(50, 10, 3);
        AURORA_TEST_CHECK_MSG(red > 100 && red < 160, "opacity 0.5 reduces red (~127)");
        AURORA_TEST_CHECK_MSG(a == 255, "over opaque background result stays opaque (a=255)");
        AURORA_LOG_INFO("test", "  [debug] opacity outside(150,100)=(", static_cast<int>(r.at(150, 100, 0)), ",",
                        static_cast<int>(r.at(150, 100, 1)), ",", static_cast<int>(r.at(150, 100, 2)), ",",
                        static_cast<int>(r.at(150, 100, 3)), ")  inside(50,10)=(", static_cast<int>(r.at(50, 10, 0)),
                        ",", static_cast<int>(r.at(50, 10, 3)), ")");
        AURORA_TEST_CHECK_MSG(r.at(150, 100, 3) == 0,
                              "outside box is transparent (headless buffer cleared transparent)");

        auto sb2 = std::make_shared<SolidBox>();
        sb2->modifier.set(Modifier{}.opacity(1.0F));
        const auto r2 = render_in_root(sb2, 200, 200);
        AURORA_TEST_CHECK_MSG(r2.at(50, 10, 0) == 255, "opacity 1.0 fully red");
        AURORA_TEST_CHECK_MSG(r2.at(50, 10, 0) > red, "opacity 0.5 is less red than opacity 1.0");
    }

    // ---------- C. rotate 旋转渲染（离屏合成 + 逆采样）----------
    {
        auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.rotate(90.0F));
        const auto r = render_in_root(sb, 200, 200);
        // 宽 100 高 20 的色块绕中心 (50,10) 旋转 90° -> 视觉上 x∈[40,60], y∈[-40,60]。
        AURORA_TEST_CHECK_MSG(r.at(50, 30, 3) > 100, "rotated interior (50,30) is painted");
        AURORA_TEST_CHECK_MSG(r.at(10, 10, 3) == 0, "unrotated region (10,10) stays empty");
    }

    // ---------- D. scale 缩放渲染 ----------
    {
        auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.scale(0.5F));
        const auto r = render_in_root(sb, 200, 200);
        // 绕中心 (50,10) 缩放 0.5：视觉上 x∈[25,75], y∈[5,15]。
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 3) > 100, "scaled interior (50,10) is painted");
        AURORA_TEST_CHECK_MSG(r.at(10, 10, 3) == 0, "scaled exterior (10,10) stays empty");
    }

    // ---------- E. 逆矩阵命中测试 ----------
    {
        auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.rotate(90.0F).draggable([](Point, Point) -> void {}));
        const auto hit = hit_in_root(sb, 200, 200, Point{.x = 50.0F, .y = 10.0F});
        AURORA_LOG_INFO("test", "  [debug] hit chain size=", hit.size(), " sb=", sb.get());
        for (auto *w : hit) {
            AURORA_LOG_INFO("test", "    widget=", w);
        }
        bool found = false;
        for (auto *w : hit) {
            if (w == sb.get()) {
                found = true;
            }
        }
        AURORA_TEST_CHECK_MSG(found, "rotated center hits via inverse matrix");

        const auto miss = hit_in_root(sb, 200, 200, Point{.x = 10.0F, .y = 10.0F});
        bool found2 = false;
        for (auto *w : miss) {
            if (w == sb.get()) {
                found2 = true;
            }
        }
        AURORA_TEST_CHECK_MSG(!found2, "rotated exterior misses via inverse matrix");
    }

    // ---------- F. Painter::composite 直接验证（隔离 widget 接线）----------
    {
        Painter src;
        src.set_scale(1.0F);
        src.begin(100, 100);
        src.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 100.0F}},
                      Color{255, 0, 0, 255});

        Painter dst;
        dst.set_scale(1.0F);
        dst.begin(200, 200);
        dst.composite(src, Matrix2D{});  // 恒等
        const std::uint8_t *dd = dst.data();
        constexpr int idx = ((50 * 200) + 50) * 4;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        AURORA_TEST_CHECK_MSG(dd[idx] > 100, "composite identity paints red at (50,50)");

        Painter dst2;
        dst2.set_scale(1.0F);
        dst2.begin(200, 200);
        dst2.composite(src, Matrix2D::from_rotate_about(90.0F, Point{.x = 50.0F, .y = 50.0F}));
        const std::uint8_t *dd2 = dst2.data();
        constexpr int idx2 = ((50 * 200) + 50) * 4;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        AURORA_TEST_CHECK_MSG(dd2[idx2] > 100, "composite rotate paints red after rotation");
    }
}