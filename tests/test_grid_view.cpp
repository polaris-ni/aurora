// 目标源单元：widget/grid_view.h
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_grid_view.cpp
//   - test_grid_view_clip.cpp
//   - test_grid_view_scroll_rows.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/widget/grid_view.h"
#include "aurora/window/window.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::Container;
using aurora::EdgeInsets;
using aurora::GridView;
using aurora::Json;
using aurora::LeafWidget;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Scroll;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Stack;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace sec_grid_view {

namespace {

class Cell : public LeafWidget {
  public:
    int idx = 0;
    explicit Cell(int i) : idx(i) {}
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override {
        p.fill_rect(b, Color{ 255, 0, 0 });
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "Cell"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "Cell", .children_policy = "none" };
    }
};

struct RenderResult {
    std::vector<std::uint8_t> pixels;
    int w = 0, h = 0;
    [[nodiscard]] auto const at(int x, const int y, const int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w) + x) * 4;
        return pixels[off + ch];
    }
};

auto render_in_root(std::shared_ptr<Widget> w, const int ww, const int hh) -> RenderResult {
    auto const root = std::make_shared<Stack>(std::vector{ Node{ std::move(w) } });
    const BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = static_cast<float>(ww), .height = static_cast<float>(hh) };
    root->layout(c, ctx);
    Painter p;
    p.begin(ww, hh);
    root->paint(p,
                Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                      .size = Size{ .width = static_cast<float>(ww), .height = static_cast<float>(hh) } },
                ctx);
    const std::uint8_t *d = p.data();
    RenderResult r;
    r.w = ww;
    r.h = hh;
    r.pixels.assign(d, d + (static_cast<std::size_t>(ww) * hh * 4));
    return r;
}
} // namespace

void run() {
    // 1000 项 / 3 列 / 单元格 50dp，视口 300x300 → 可见行 ~6 → 存活实例应远小于 1000
    const auto gv =
        std::make_shared<GridView>(1000, 3, [](int i) -> Node { return Node{ std::make_shared<Cell>(i) }; }, 50.0f);
    gv->set_cache_extent(0.0f); // 关闭预取，便于精确断言

    const auto r = render_in_root(gv, 300, 300);
    AURORA_TEST_CHECK_MSG(gv->count() == 1000, "count 1000");
    AURORA_TEST_CHECK_MSG(gv->columns() == 3, "columns 3");
    AURORA_TEST_CHECK_MSG(gv->row_count() == 334, "row_count = ceil(1000/3) = 334");
    AURORA_TEST_CHECK_MSG(gv->content_height() == 334.0f * 50.0f, "content_height = 334*50");
    AURORA_TEST_CHECK_MSG(gv->live_item_count() <= static_cast<std::size_t>(3 * 7),
                          "live instances bounded (<= 3 cols * 7 rows)");
    AURORA_TEST_CHECK_MSG(gv->live_item_count() > 0, "live instances > 0");
    // 第一个单元格红色可见
    AURORA_TEST_CHECK_MSG(r.at(10, 10, 0) == 255, "first cell renders red at top-left");

    // 滚动到底：偏移 = content_height - viewport = 16700-300=16400
    gv->set_scroll_offset(99999.0f);
    render_in_root(gv, 300, 300);
    AURORA_TEST_CHECK_MSG(gv->scroll_offset() == gv->max_scroll_offset(), "scroll clamped to max");
    AURORA_TEST_CHECK_MSG(gv->max_scroll_offset() == (334.0f * 50.0f) - 300.0f, "max_scroll_offset correct");

    // 滚回顶部：存活实例应回收早期、重建早期
    gv->set_scroll_offset(0.0f);
    render_in_root(gv, 300, 300);
    AURORA_TEST_CHECK_MSG(gv->live_item_count() <= static_cast<std::size_t>(3 * 7),
                          "after scroll to top, live bounded again");

    // 序列化
    Json props = Json::object();
    gv->serialize_props(props);
    AURORA_TEST_CHECK_MSG(props["count"] == 1000, "serialize count");
    AURORA_TEST_CHECK_MSG(props["columns"] == 3, "serialize columns");
    AURORA_TEST_CHECK_MSG(props["cell_extent"] == 50.0f, "serialize cell_extent");
}
} // namespace sec_grid_view

namespace sec_grid_view_clip {
namespace au = aurora;

void run() {
    auto grid = std::make_shared<GridView>(
        40, 3,
        [](int i) -> Node {
            auto const c = std::make_shared<au::Canvas>(100.0f, 100.0f, [i](au::Painter &p, const Rect &b) -> void {
                p.fill_rect(b, Color{ static_cast<uint8_t>(i * 6 % 256), 0x60, 0x90, 0xFF });
            });
            return Node{ c };
        },
        100.0f);

    auto wrap = std::make_shared<au::Row>();
    wrap->add(Node{ grid });
    wrap->modifier = au::Modifier{}.clip_rounded(20.0f).background(Color{ 0xFF, 0xFF, 0xFF, 0xFF });

    au::HeadlessOptions opts;
    opts.size = Size{ .width = 340.0f, .height = 520.0f };
    opts.png_path = "build/test_grid_view_clip.png";
    auto res = au::create_window(opts);
    AURORA_TEST_CHECK(static_cast<bool>(res));
    if (res) {
        auto win = std::move(res.value());
        // 若 GridView 未裁剪导致 Painter 慢路径越界，此调用会段错误（测试失败）。
        Node root_node{ wrap };
        auto r = win->present_root(root_node);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(win->surface().frame_count() == 1);
    }
}
} // namespace sec_grid_view_clip

namespace sec_grid_view_scroll_rows {

namespace {

class GreenCell : public LeafWidget {
  public:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override {
        p.fill_rect(b, Color{ 0, 255, 0 });
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "GreenCell"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "GreenCell", .children_policy = "none" };
    }
};

struct RenderResult {
    std::vector<std::uint8_t> pixels;
    int w = 0, h = 0;
    [[nodiscard]] auto const at(int x, int y, const int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w) + x) * 4;
        return pixels[off + ch];
    }
};

// 模拟 Google Play BodyView：给 GridView 套 fill_max_width + padding 修饰器，
// 并把它限制在固定高度区域内（BodyView 知道 GridView 区高 480）。
class PaddedGridBody : public Container {
  public:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        m_children.clear();
        const auto grid = std::make_shared<GridView>(
            40, 4, [](int) -> Node { return Node{ std::make_shared<GreenCell>() }; }, 140.0f);
        grid->set_cache_extent(300.0f);
        grid->modifier = Modifier{}.fill_max_width().padding(
            EdgeInsets{ .left = 16.0f, .top = 0.0f, .right = 16.0f, .bottom = 0.0f });
        grid->mount(ctx);

        const float w = c.max.width;
        constexpr float grid_h = 480.0f;
        grid->layout(
            Constraints{ .min = Size{ .width = 0.0f, .height = 0.0f }, .max = Size{ .width = w, .height = grid_h } },
            ctx);
        grid->set_layout_parent(this);

        Node grid_node{ grid };
        grid_node.set_bounds(
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = w, .height = grid_h } });
        add(grid_node);

        return c.constrain(Size{ .width = w, .height = grid_h });
    }
    void on_paint(Painter &p, const Rect &b, const BuildContext &ctx) override {
        for (auto &n : m_children) {
            const Rect cb = n.bounds();
            const Rect gb{ .origin = Point{ .x = b.origin.x + cb.origin.x, .y = b.origin.y + cb.origin.y },
                           .size = cb.size };
            n.widget().paint(p, gb, ctx);
        }
    }
    [[nodiscard]] auto type_name() const -> const char * override { return "PaddedGridBody"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "PaddedGridBody", .children_policy = "multiple" };
    }
};

auto const render_scroll_with_padded_grid(int ww, const int hh) -> RenderResult {
    const auto scroll = std::make_shared<Scroll>();
    scroll->add(Node{ std::make_shared<PaddedGridBody>() });

    const BuildContext ctx;
    scroll->mount(ctx);
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = static_cast<float>(ww), .height = static_cast<float>(hh) };
    scroll->layout(c, ctx);

    Painter p;
    p.begin(ww, hh);
    scroll->paint(p,
                  Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                        .size = Size{ .width = static_cast<float>(ww), .height = static_cast<float>(hh) } },
                  ctx);

    RenderResult r;
    r.w = ww;
    r.h = hh;
    const std::uint8_t *d = p.data();
    r.pixels.assign(d, d + (static_cast<std::size_t>(ww) * hh * 4));
    return r;
}

} // namespace

void run() {
    const auto r = render_scroll_with_padded_grid(1280, 600);

    // 第 0 行应有绿色单元格（在 padding 后，x 从 16 开始）
    AURORA_TEST_CHECK_MSG(r.at(30, 10, 1) == 255, "first row cell visible (green)");
    // 第 1 行（y ~ 140）必须有绿色，不能只渲染第一行
    AURORA_TEST_CHECK_MSG(r.at(30, 150, 1) == 255, "second row cell visible (green)");
    // 第 2 行（y ~ 280）必须有绿色
    AURORA_TEST_CHECK_MSG(r.at(30, 290, 1) == 255, "third row cell visible (green)");
}
} // namespace sec_grid_view_scroll_rows

AURORA_TEST() {
    sec_grid_view::run();
    sec_grid_view_clip::run();
    sec_grid_view_scroll_rows::run();
}
