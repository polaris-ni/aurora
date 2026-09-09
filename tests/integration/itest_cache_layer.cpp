/// 测试类型: integration
/// 目标单元: include/aurora/modifier/modifier.h
/// 测试说明: 验证 Modifier::cache_layer——子树渲染结果缓存到离屏位图：首次绘制渲染子树、
///           未失效时命中缓存跳过重绘、invalidate_paint_cache 失效后重绘、尺寸变化（配合
///           mark_needs_layout）失效缓存；并对照无 cache_layer 时 Display List 重录行为
///           （用 on_paint 计数器证明缓存命中跳过子树重绘）

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

using au::BuildContext;
using au::Color;
using au::Constraints;
using au::LeafWidget;
using au::Modifier;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;
using au::Stack;
using au::Widget;

namespace aurora::test_cases::itest_cache_layer {

namespace {

class CountingBox : public LeafWidget {
  public:
    Size sz{.width = 100.0F, .height = 20.0F};
    Color color{255, 0, 0, 255};
    static int m_paint_count;

    [[nodiscard]] auto type_name() const -> const char * override { return "CountingBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override {
        ++m_paint_count;
        p.fill_rect(b, color);
    }
};
int CountingBox::m_paint_count = 0;

struct RenderResult {
    std::vector<std::uint8_t> pixels;
    int w = 0;
    int h = 0;
    [[nodiscard]] auto at(int x, const int y, const int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w) + x) * 4;
        return pixels.at(off + ch);
    }
};

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
    r.w = ww;
    r.h = hh;
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    r.pixels.assign(d, d + (static_cast<std::size_t>(ww) * hh * 4));  // NOLINT
    return r;
}

}  // namespace

AURORA_TEST_CASE(cache_layer_first_paint_renders_subtree) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(Modifier{}.cache_layer());

    // 首次绘制：渲染子树（计数+1），像素可见
    const auto r1 = render_in_root(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 1, "first paint renders subtree (count=1)");
    AURORA_TEST_CHECK_MSG(r1.at(50, 10, 0) == 255, "first paint shows red");
}

AURORA_TEST_CASE(cache_layer_second_paint_hits_cache) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(Modifier{}.cache_layer());

    (void)render_in_root(cb, 200, 200);
    // 第二次绘制（同尺寸、未失效）：命中缓存，跳过子树（计数不变）
    const auto r2 = render_in_root(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 1, "second paint hits cache (count still 1)");
    AURORA_TEST_CHECK_MSG(r2.at(50, 10, 0) == 255, "cached paint still shows red");
}

AURORA_TEST_CASE(cache_layer_invalidate_forces_rerender) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(Modifier{}.cache_layer());

    (void)render_in_root(cb, 200, 200);
    // 失效后重绘：计数+1
    cb->invalidate_paint_cache();
    const auto r3 = render_in_root(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 2, "after invalidate, re-renders (count=2)");
    AURORA_TEST_CHECK_MSG(r3.at(50, 10, 0) == 255, "re-rendered paint shows red");
}

AURORA_TEST_CASE(cache_layer_size_change_invalidates_cache) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(Modifier{}.cache_layer());

    (void)render_in_root(cb, 200, 200);
    // 尺寸变化使缓存失效：须先 mark_needs_layout（布局缓存契约——改变影响布局的属性须标脏）。
    cb->sz = Size{.width = 50.0F, .height = 20.0F};
    cb->mark_needs_layout();
    const auto r4 = render_in_root(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 2, "size change invalidates cache (count=2)");
    AURORA_TEST_CHECK_MSG(r4.at(25, 10, 0) == 255, "resized paint shows red at new center");
}

AURORA_TEST_CASE(without_cache_layer_forced_repaint_rerecords) {
    CountingBox::m_paint_count = 0;
    // 无 cache_layer 的对照：Display List 已接管子树绘制缓存，静态重绘经 replay 跳过 on_paint；
    // 强制重绘（mark_needs_paint）后重录，on_paint 再次被调用。
    const auto nb = std::make_shared<CountingBox>();
    (void)render_in_root(nb, 200, 200);
    nb->mark_needs_paint();
    (void)render_in_root(nb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 2,
                          "without cache_layer, forced re-record re-renders (count=2)");
}

}  // namespace aurora::test_cases::itest_cache_layer
