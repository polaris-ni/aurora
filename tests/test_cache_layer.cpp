// Modifier::cache_layer 验证：子树渲染结果缓存到离屏位图，尺寸不变且未失效时直接复用。
// 用 on_paint 计数器证明缓存命中跳过子树重绘。无头 layout + paint，不依赖 GUI 后端。
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::LeafWidget;
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

class CountingBox : public LeafWidget {
  public:
    Size sz_{.width = 100.0F, .height = 20.0F};
    Color color_{255, 0, 0, 255};
    static int m_paint_count;
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz_); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override {
        ++m_paint_count;
        p.fill_rect(b, color_);
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "CountingBox"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "CountingBox", .children_policy = "none"};
    }
};
int CountingBox::m_paint_count = 0;

struct RenderResult {
    std::vector<std::uint8_t> pixels_;
    int w_ = 0, h_ = 0;
    [[nodiscard]] auto at(int x, const int y, const int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w_) + x) * 4;
        return pixels_.at(off + ch);
    }
};

auto render_in_root(std::shared_ptr<Widget> w, const int ww, const int hh) -> RenderResult {
    auto const root = std::make_shared<Stack>(std::vector{Node{std::move(w)}});
    const BuildContext ctx;
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
}  // namespace

AURORA_TEST() {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(Modifier{}.cache_layer());

    // 首次绘制：渲染子树（计数+1），像素可见
    const auto r1 = render_in_root(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 1, "first paint renders subtree (count=1)");
    AURORA_TEST_CHECK_MSG(r1.at(50, 10, 0) == 255, "first paint shows red");

    // 第二次绘制（同尺寸、未失效）：命中缓存，跳过子树（计数不变）
    const auto r2 = render_in_root(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 1, "second paint hits cache (count still 1)");
    AURORA_TEST_CHECK_MSG(r2.at(50, 10, 0) == 255, "cached paint still shows red");

    // 失效后重绘：计数+1
    cb->invalidate_paint_cache();
    const auto r3 = render_in_root(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 2, "after invalidate, re-renders (count=2)");
    AURORA_TEST_CHECK_MSG(r3.at(50, 10, 0) == 255, "re-rendered paint shows red");

    // 尺寸变化使缓存失效：须先 mark_needs_layout（布局缓存契约——改变影响布局的属性须标脏）。
    cb->sz_ = Size{.width = 50.0F, .height = 20.0F};
    cb->mark_needs_layout();
    const auto r4 = render_in_root(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 3, "size change invalidates cache (count=3)");
    AURORA_TEST_CHECK_MSG(r4.at(25, 10, 0) == 255, "resized paint shows red at new center");

    // 无 cache_layer 的对照：Display List 已接管子树绘制缓存，静态重绘经 replay 跳过 on_paint；
    // 强制重绘（mark_needs_paint）后重录，on_paint 再次被调用。
    CountingBox::m_paint_count = 0;
    const auto nb = std::make_shared<CountingBox>();
    render_in_root(nb, 200, 200);
    nb->mark_needs_paint();
    render_in_root(nb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 2,
                          "without cache_layer, forced re-record re-renders (count=2)");
}