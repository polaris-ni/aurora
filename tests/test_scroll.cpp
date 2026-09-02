// 目标源单元：widget/scroll.h + src/aurora/widget/scroll 相关渲染路径
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_scroll.cpp
//   - test_scroll_grid.cpp
//   - test_scroll_reanchor.cpp
//   - test_scroll_regression.cpp
//   - test_scroll_self_driving.cpp
//   - test_scroll_viewport_repaint.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/perf/scroll_bench.h"
#include "aurora/widget/scroll.h"
#include "aurora/window/window.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::Container;
using aurora::GridView;
using aurora::LeafWidget;
using aurora::Length;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::profiling_enabled;
using aurora::Rect;
using aurora::Scroll;
using aurora::ScrollBenchHarness;
using aurora::ScrollEvent;
using aurora::ScrollProps;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Spacer;
using aurora::Text;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace aurora::tests::sec_scroll {
static void run() {
    AURORA_TEST_PRINTF("=== test_scroll ===\n");

    constexpr BuildContext ctx;
    constexpr Constraints c{ .min = Size{ .width = 0.0f, .height = 0.0f },
                             .max = Size{ .width = 200.0f, .height = 100.0f } };

    // 1) 默认构造 type_name 为 "Scroll"。
    const Scroll def;
    AURORA_TEST_CHECK(std::string(def.type_name()) == "Scroll");

    // 2) 默认 step 为 16.0。
    AURORA_TEST_CHECK(near_f(def.step, 16.0f));

    // 3) ScrollProps 构造可设自定义 step。
    const Scroll custom{ ScrollProps{ .child = Node{ Text{ "a" } }, .step = 24.0f } };
    AURORA_TEST_CHECK(near_f(custom.step, 24.0f));

    // 4) initializer_list 构造取首项为唯一子节点。
    const Scroll from_list{ Node{ Text{ "only" } } };
    AURORA_TEST_CHECK(from_list.child_nodes().size() == 1);

    // 5) initializer_list 传多个子节点时仅首项被接管。
    Scroll multi{ Node{ Text{ "first" } }, Node{ Text{ "second" } } };
    AURORA_TEST_CHECK(multi.child_nodes().size() == 1);

    // 6) layout 后宽度受父约束（=200）。
    multi.mount(ctx);
    const Size s = multi.layout(c, ctx);
    AURORA_TEST_CHECK(near_f(s.width, 200.0f));

    // 7) layout 后高度受父约束（=100）。
    AURORA_TEST_CHECK(s.height >= 0.0f && s.height <= 100.0f + 1e-3f);

    // 8) 空 Scroll 也能 layout（不崩溃，尺寸有限）。
    Scroll empty;
    empty.mount(ctx);
    const Size es = empty.layout(c, ctx);
    AURORA_TEST_CHECK(es.width >= 0.0f && es.height >= 0.0f);

    // 9) on_scroll 标记事件为已处理。
    ScrollEvent se;
    se.delta_y = -24.0f;
    se.handled = false;
    multi.on_scroll(se);
    AURORA_TEST_CHECK(se.handled);

    // 10) 连续多次 on_scroll 不崩溃。
    for (int i = 0; i < 10; ++i) {
        ScrollEvent e;
        e.delta_y = 5.0f;
        multi.on_scroll(e);
    }
    AURORA_TEST_CHECK(true);

    // 11) 嵌套 Scroll：内层 child_nodes 为 1。
    const Scroll outer{ Node{ Scroll{ Node{ Text{ "deep" } } } } };
    AURORA_TEST_CHECK(outer.child_nodes().size() == 1);
    AURORA_TEST_CHECK(outer.child_nodes()[0].widget().child_nodes().size() == 1);

    // 12) 高内容（多个子节点）layout 后尺寸仍受视口约束。
    const auto col = std::make_shared<Column>(
        std::initializer_list{ Node{ Text{ "a" } }, Node{ Text{ "b" } }, Node{ Text{ "c" } } });
    Scroll tall{ std::initializer_list{ Node{ col } } };
    tall.mount(ctx);
    const Size ts = tall.layout(c, ctx);
    AURORA_TEST_CHECK(ts.width >= 0.0f && ts.width <= 200.0f);
    AURORA_TEST_CHECK(ts.height >= 0.0f && ts.height <= 100.0f + 1e-3f);

    // 13) on_scroll 在 delta_y=0（边界）时也标记处理且不崩溃。
    ScrollEvent zero;
    zero.delta_y = 0.0f;
    zero.handled = false;
    tall.on_scroll(zero);
    AURORA_TEST_CHECK(zero.handled);

    // 14) 回归：on_scroll 偏移变化必须触发「仅绘制脏」重绘请求（request_frame(false)）。
    //     本框架脏区追踪下，present_root 实际绘制被裁剪到 m_dirty；若滚动不标脏，则只有
    //     自驱动动画控件（如 banner）会跟随滚动，其余静态内容静止——表现即「滚动时只有 banner 在动」。
    //     用确定性定高（Modifier.height）内容，规避 headless 下字体度量缺失导致内容高度不可靠。
    auto tall_child = Node{ Text{ "tall" } };
    tall_child.widget().modifier = Modifier{}.height(300.0f); // 内容高 300 > 视口 100
    Scroll scroll_reg{ std::initializer_list{ tall_child } };
    scroll_reg.mount(ctx);
    scroll_reg.layout(c, ctx); // c 为 200x100 视口约束
    int dirty_calls = 0;
    bool dirty_includes_layout = true;
    scroll_reg.on_dirty = [&](bool layout) -> void {
        ++dirty_calls;
        dirty_includes_layout = layout;
    };
    // 边界（未改变偏移）：不应标脏。
    ScrollEvent edge;
    edge.delta_y = 0.0f;
    scroll_reg.on_scroll(edge);
    AURORA_TEST_CHECK(dirty_calls == 0);
    // 向下滚动（delta_y 为负，符合 delta_y 正=向上滚动约定）使偏移增大、离开 0：
    // 必须标脏且「仅绘制脏」（layout=false）。
    ScrollEvent scroll_down;
    scroll_down.delta_y = -5.0f;
    scroll_reg.on_scroll(scroll_down);
    AURORA_TEST_CHECK(dirty_calls == 1);
    AURORA_TEST_CHECK(dirty_includes_layout == false);
}
} // namespace aurora::tests::sec_scroll

namespace aurora::tests::sec_scroll_grid {

namespace {

class RedCell : public LeafWidget {
  public:
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "RedCell"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "RedCell", .children_policy = "none" };
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override {
        p.fill_rect(b, Color{ 255, 0, 0 });
    }
};

struct RenderResult {
    std::vector<std::uint8_t> pixels;
    int w = 0, h = 0;
    [[nodiscard]] auto at(int x, int y, const int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w) + x) * 4;
        return pixels[off + ch];
    }
};

class WideBody : public Container {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "WideBody"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "WideBody", .children_policy = "multiple" };
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        m_children.clear();
        const auto grid =
            std::make_shared<GridView>(100, 3, [](int) -> Node { return Node{ std::make_shared<RedCell>() }; }, 50.0f);
        grid->set_cache_extent(0.0f);
        grid->mount(ctx);
        Constraints gc;
        gc.min = Size{ .width = 0.0f, .height = 0.0f };
        gc.max = Size{ .width = c.max.width, .height = 300.0f };
        grid->layout(gc, ctx);
        grid->set_layout_parent(this);
        Node grid_node{ grid };
        grid_node.set_bounds(
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = c.max.width, .height = 300.0f } });
        add(grid_node);
        return c.constrain(Size{ .width = c.max.width, .height = 300.0f });
    }
    void on_paint(Painter &p, const Rect &b, const BuildContext &ctx) override {
        for (auto &n : m_children) {
            const Rect cb = n.bounds();
            const Rect gb{ .origin = Point{ .x = b.origin.x + cb.origin.x, .y = b.origin.y + cb.origin.y },
                           .size = cb.size };
            n.widget().paint(p, gb, ctx);
        }
    }
};

auto render_scroll_with_grid(int ww, const int hh) -> RenderResult {
    const auto scroll = std::make_shared<Scroll>();
    scroll->add(Node{ std::make_shared<WideBody>() });

    constexpr BuildContext ctx;
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

static void run() {
    const auto r = render_scroll_with_grid(300, 300);

    // 视口顶部应有红色单元格（第 0 行）
    AURORA_TEST_CHECK_MSG(r.at(10, 10, 0) == 255, "first row cell visible (red)");
    // 关键回归点：第 2 行、第 3 行也必须是红色，而不是黑色。
    // 若 Scroll 传无限宽约束，离屏缓冲宽度异常，下方行会未绘制而呈黑色。
    AURORA_TEST_CHECK_MSG(r.at(10, 110, 0) == 255, "second row cell visible (red)");
    AURORA_TEST_CHECK_MSG(r.at(10, 210, 0) == 255, "third row cell visible (red)");
}
} // namespace aurora::tests::sec_scroll_grid

namespace aurora::tests::sec_scroll_reanchor {

namespace {

/// @brief 行号灰度编码：content 第 row 行 → 灰度 = (row*13+7) & 0xFF（唯一、可复现）。
auto row_gray(int row) -> std::uint8_t { return static_cast<std::uint8_t>(((row * 13U) + 7U) & 0xFFU); }

/// @brief 测试用单子：高度可热改（用于触发 Scroll 的缓冲失效），逐行填可辨识灰度。
struct ResizableStack : Widget {
    int m_rows = 1000; ///< 逻辑高度（px），可随时改以触发内容尺寸变化

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ResizableStack"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float w = (c.max.width > 0.0f && c.max.width < Size::infinity().width) ? c.max.width : 100.0f;
        return Size{ .width = w, .height = static_cast<float>(m_rows) };
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        const auto w = static_cast<float>(p.width());
        const float oy = bounds.origin.y; // Scroll 以 -m_buffer_origin_y 偏移把子控件绘进缓冲
        for (int y = 0; y < m_rows; ++y) {
            const int by = y + static_cast<int>(oy);
            if (by < 0) {
                continue; // 落在缓冲顶之上的内容行不绘制
            }
            const auto g = row_gray(y);
            p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = static_cast<float>(by) },
                              .size = Size{ .width = w, .height = 1.0f } },
                        Color{ g, g, g, 255 });
        }
    }
};

/// @brief 把 Scroll 渲染进一个 100×100、scale=1 的画布，返回画布（供逐像素读取）。
auto paint_scroll(Node &root, BuildContext const &ctx, const int off = 0) -> Painter {
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = 100.0f, .height = 100.0f };
    root->layout(c, ctx); // 重新布局以刷新 m_content_valid / 缓冲尺寸
    Painter p;
    p.begin(100, 100);
    root->paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 100.0f, .height = 100.0f } },
                ctx);
    (void)off;
    return p;
}

/// @brief 断言视口第 y 行灰度等于 content 第 (offset+y) 行。
auto check_viewport_row(const Painter &p, int y, int offset, const char *msg) -> void {
    const int expected = row_gray(offset + y);
    AURORA_TEST_CHECK_MSG(std::cmp_equal(p.get_pixel(5, y).m_r, expected), msg);
}

} // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_scroll_reanchor ===\n");

    BuildContext ctx;
    auto child = std::make_shared<ResizableStack>();
    child->m_rows = 1000;
    auto scroll = std::make_shared<Scroll>(ScrollProps{ .child = Node{ child } });
    Node root{ scroll };
    root->mount(ctx);

    // ---- (1) 初始帧：offset=0，视口应显示 content[0,100) ----
    {
        Painter p = paint_scroll(root, ctx);
        check_viewport_row(p, 0, 0, "offset=0: top row = content row 0");
        check_viewport_row(p, 50, 0, "offset=0: middle row = content row 50");
        check_viewport_row(p, 99, 0, "offset=0: bottom row = content row 99");
    }

    // ---- (2) reanchor 路径：滚动到 offset=500 后整帧重绘，视口须显示 content[500,600) ----
    // step 默认 16，scroll_by(-31.25) → m_offset_y += 31.25*16 = 500。
    scroll->scroll_by(-31.25f);
    {
        Painter p = paint_scroll(root, ctx);
        check_viewport_row(p, 0, 500, "reanchor: top row = content row 500 after scrolling 500");
        check_viewport_row(p, 50, 500, "reanchor: middle row = content row 550 after scrolling 500");
        check_viewport_row(p, 99, 500, "reanchor: bottom row = content row 599 after scrolling 500");
    }

    // ---- (3) else 分支锚点重对齐（修复验证）：
    //     再滚动到 offset=300（设置 m_offset_y，但不立即重绘），随后改内容高度触发缓冲失效，
    //     模拟「已滚动但缓冲未重锚点 + 内容尺寸变化」的到达态。m_buffer_origin_y 仍停留在
    //     上一帧的 400（offset=500 时），与当前 m_offset_y=300 脱钩。
    //     若缺少 else 分支的对齐行：composite dy=400-300=100，视口映射到缓冲负行 → 整片空白；
    //     修复后：m_buffer_origin_y 重算为 200，视口显示 content[300,400)。
    scroll->scroll_by(12.5f); // m_offset_y: 500 → 500 - 12.5*16 = 300
    child->m_rows = 1200;     // 内容尺寸变化：下帧 m_content_valid=false，但 m_buffer_origin_y 未更新
    {
        Painter p = paint_scroll(root, ctx);
        check_viewport_row(p, 0, 300, "reanchor-invalidation: top row = content row 300 (non-blank)");
        check_viewport_row(p, 50, 300, "reanchor-invalidation: middle row = content row 350 (non-blank)");
        check_viewport_row(p, 99, 300, "reanchor-invalidation: bottom row = content row 399 (non-blank)");
        // 反向：若落回缓冲外，整片会是 0（透明黑）。这里显式确认非全零。
        bool any_nonzero = false;
        for (int y = 0; y < 100; ++y) {
            if (p.get_pixel(5, y).m_r != 0) {
                any_nonzero = true;
                break;
            }
        }
        AURORA_TEST_CHECK_MSG(any_nonzero,
                              "reanchor-invalidation: viewport has non-blank pixels (buffer not misaligned)");
    }
}
} // namespace aurora::tests::sec_scroll_reanchor

namespace aurora::tests::sec_scroll_regression {
using Result_ = ScrollBenchHarness::Result;

namespace {

constexpr float AURORA_ITEM_H = 48.0f;       ///< 固定行高（dp），与项高一致
constexpr int AURORA_BASE_ROWS = 300;        ///< 基线项数（视口 760dp / 48dp ≈ 15 可见项）
constexpr float AURORA_VIEWPORT_W = 1100.0f; ///< 约定口径：与 demo_google_play 窗口一致
constexpr float AURORA_VIEWPORT_H = 760.0f;
constexpr float AURORA_SCROLL_OVERSCAN = 1.0f; ///< Scroll 默认 overscan（上/下各一屏预取）

/// @brief 合成滚动树：`Scroll` 包 `rows` 个固定高 `Spacer`（不绘制、不测字，几何完全可预测）。
[[nodiscard]] auto build_scroll_column(int rows) -> Node { // NOLINT
    std::vector<Node> items;
    items.reserve(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        auto sp = std::make_shared<Spacer>(false);
        sp->height(Length::fixed(AURORA_ITEM_H));
        sp->width(Length::fixed(200.0f));
        items.emplace_back(std::move(sp));
    }
    auto col = std::make_shared<Column>(ColumnProps{ .children = std::move(items) });
    return Node{ std::make_shared<Scroll>(ScrollProps{ .child = Node{ std::move(col) } }) };
}

/// @brief CI 口径配置：轻量但足以让 harness 收敛（落定 + 真实滚动）。
[[nodiscard]] auto gate_cfg() -> ScrollBenchHarness::Config { // NOLINT
    ScrollBenchHarness::Config c;
    c.name = "ci-gates";
    c.frames = 120;
    c.warmup_frames = 20;
    c.delta_per_frame = 12.0f;
    c.settle_ms = 300.0;
    c.settle_idle_frames = 5;
    c.settle_max_frames = 400;
    return c;
}

/// @brief 跑一对（基线 / 内容×10），用于「×10 不增长」类门槛。
struct Pair {
    Result_ base;
    Result_ x10;
};
[[nodiscard]] auto run_pair(int base_rows) -> Pair {
    Pair p;
    p.base = ScrollBenchHarness::run(build_scroll_column(base_rows),
                                     Size{ .width = AURORA_VIEWPORT_W, .height = AURORA_VIEWPORT_H }, gate_cfg());
    p.x10 = ScrollBenchHarness::run(build_scroll_column(base_rows * 10),
                                    Size{ .width = AURORA_VIEWPORT_W, .height = AURORA_VIEWPORT_H }, gate_cfg());
    return p;
}

} // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_scroll_regression (CI count gates G-5/G-6/G-7/G-8) ===\n");

    const Pair p = run_pair(AURORA_BASE_ROWS);

    // 前置：harness 自证（任何构建都校验，先证伪再看性能数）。
    AURORA_TEST_CHECK_MSG(
        p.base.trustworthy(),
        "base: harness readings trustworthy (located Scroll, truly scrolls each frame, geometry stable)");
    AURORA_TEST_CHECK_MSG(p.x10.trustworthy(), "x10: harness readings trustworthy");

    if constexpr (profiling_enabled()) {
        constexpr auto vp_px = static_cast<std::uint64_t>(AURORA_VIEWPORT_W * AURORA_VIEWPORT_H);
        // 允许上限 = 视口像素 × 4 × (1 + 2 × overscan)，overscan 取 Scroll 默认 1.0。
        constexpr auto buf_allowed = static_cast<std::uint64_t>(vp_px * 4.0 * (1.0 + (2.0 * AURORA_SCROLL_OVERSCAN)));
        // 绝对上限 = 可见项节点数 × 1.25（滚动帧本不该重排，余量仅容容器节点）。
        const double visible = std::floor(static_cast<double>(AURORA_VIEWPORT_H) / static_cast<double>(AURORA_ITEM_H));
        const double g6_bound = visible * 1.25;

        // ---- 滚动期间不得退化为整帧重绘（Scroll 离屏缓冲 composite）----
        AURORA_TEST_CHECK_MSG(p.base.full_redraw_frames() == 0,
                              "G-5: full_redraw_frames == 0/120 (offscreen-buffer composite)");
        AURORA_TEST_CHECK_MSG(p.x10.full_redraw_frames() == 0, "G-5[x10]: full_redraw_frames == 0");

        // ---- 滚动帧 layout_nodes ≤ 可见项×1.25，且内容×10 不增长 ----
        // 保持既有阈值语义，改写 lround 会翻转边界期望值。
        // NOLINTNEXTLINE(bugprone-incorrect-roundings)
        AURORA_TEST_CHECK_MSG(p.base.counters_max().layout_nodes <= static_cast<std::uint32_t>(g6_bound + 0.5),
                              "G-6: layout_nodes_max <= visible items x1.25");
        AURORA_TEST_CHECK_MSG(p.x10.counters_max().layout_nodes <= p.base.counters_max().layout_nodes,
                              "G-6[x10]: layout_nodes does not grow with content x10");

        // ---- dl_records ×10 波动 ≤ 10%（须正比于滚动距离而非内容总量）----
        const auto dl_base = p.base.counters_sum().dl_records;
        const auto dl_x10 = p.x10.counters_sum().dl_records;
        AURORA_TEST_CHECK_MSG(dl_x10 <=
                                  dl_base + static_cast<std::uint32_t>((static_cast<double>(dl_base) * 0.10) + 1.0),
                              "G-7: dl_records x10 fluctuation <= 10%");

        // ---- Scroll 缓冲字节 ≤ 视口×4×(1+2×overscan)，且 ×10 不增长（滑动窗口）----
        AURORA_TEST_CHECK_MSG(p.base.counters_max().scroll_buffer_bytes <= buf_allowed,
                              "G-8: scroll_buffer <= viewport x4 x(1+2xoverscan)");
        AURORA_TEST_CHECK_MSG(p.x10.counters_max().scroll_buffer_bytes <= p.base.counters_max().scroll_buffer_bytes,
                              "G-8[x10]: scroll_buffer does not grow with content x10");

        AURORA_TEST_PRINTF("  G-5  full_redraw base/x10 = %zu / %zu\n", p.base.full_redraw_frames(),
                           p.x10.full_redraw_frames());
        AURORA_TEST_PRINTF("  G-6  layout_nodes_max base/x10 = %u / %u (bound=%.1f)\n",
                           p.base.counters_max().layout_nodes, p.x10.counters_max().layout_nodes, g6_bound);
        AURORA_TEST_PRINTF("  G-7  dl_records_sum base/x10 = %u / %u\n", dl_base, dl_x10);
        AURORA_TEST_PRINTF("  G-8  scroll_buffer base/x10 = %llu / %llu (allowed=%llu)\n",
                           static_cast<unsigned long long>(p.base.counters_max().scroll_buffer_bytes),
                           static_cast<unsigned long long>(p.x10.counters_max().scroll_buffer_bytes),
                           static_cast<unsigned long long>(buf_allowed));
    } else { // NOLINT
        AURORA_TEST_PRINTF(
            "[PROFILING=OFF] counters stay 0, count-based assertions skipped (only harness self-check validated);"
            "CI anchor should run under build-prof (Release + PROFILING=ON).\n");
    }
}
} // namespace aurora::tests::sec_scroll_regression

namespace aurora::tests::sec_scroll_self_driving {
namespace au = aurora;

namespace {

constexpr float kLeafH = 60.0f;
constexpr Color kSkeletonColor{ 210, 210, 210, 255 };
constexpr Color kContentColor{ 255, 64, 0, 255 }; ///< 高饱和：与骨架灰/底色可判别

/// @brief 自驱动动画叶控件：每次绘制都在 on_paint 内 mark_needs_paint 请求下一帧
///        （与 demo 的骨架微光 / banner 入场 / 轮播同构）。paints 记录真实绘制次数。
struct SelfDrivingLeaf : LeafWidget {
    int paints = 0;
    Color color = kSkeletonColor;

    [[nodiscard]] auto type_name() const -> const char * override { return "SelfDrivingLeaf"; }
    /// 内容每帧变化，不参与 DL 缓存（否则回放会跳过 on_paint，掩盖被测行为）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = c.max.width, .height = kLeafH });
    }

    auto on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) -> void override {
        ++paints;
        p.fill_rect(b, color);
        mark_needs_paint(); // 自驱动：绘制中请求下一帧（脏须经布局父链上溯到 Scroll / 窗口）
    }
};

/// @brief 首屏骨架 → 真实内容切换体：由**帧驱动**（绘制若干帧后切换），切换在 on_layout 中
///        重建子树（新建控件实例）。这正是旧「接线快照」覆盖不到的模式。
struct SwitchingBody : Container {
    int phase = 0;        ///< 0=骨架 1=真实内容
    int paint_ticks = 0;  ///< 骨架期已绘制帧数
    int layout_calls = 0; ///< on_layout 真实执行次数
    std::shared_ptr<SelfDrivingLeaf> skeleton;
    std::shared_ptr<SelfDrivingLeaf> content; ///< 切换后**动态新建**，旧接线快照覆盖不到

    [[nodiscard]] auto type_name() const -> const char * override { return "SwitchingBody"; }
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }
    /// on_layout 依赖外部状态重建子树（非纯函数）→ 必须退出布局缓存，否则约束不变时被短路冻结。
    [[nodiscard]] auto can_cache_layout() const -> bool override { return false; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        ++layout_calls;
        const float w = (c.max.width < Size::infinity().width) ? c.max.width : 200.0f;
        m_children.clear();
        const auto leaf = std::make_shared<SelfDrivingLeaf>();
        if (phase == 0) {
            leaf->color = kSkeletonColor;
            skeleton = leaf;
        } else {
            leaf->color = kContentColor;
            content = leaf;
        }
        Node n{ leaf };
        n.widget().mount(ctx);
        // 经 Widget::layout() 入口重排 → 布局父链自动登记（脏标记得以上溯到 Scroll / 根）。
        n.widget().layout(
            Constraints{ .min = Size{ .width = w, .height = 0.0f }, .max = Size{ .width = w, .height = kLeafH } }, ctx);
        n.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = w, .height = kLeafH } });
        m_children.push_back(std::move(n));
        return c.constrain(Size{ .width = w, .height = kLeafH });
    }

    auto on_paint(Painter &p, const Rect &b, const BuildContext &ctx) -> void override {
        // 帧/时钟驱动的「延期布局」：骨架期结束后切真实内容（不经外部显式标脏，与 demo 同构）。
        if (phase == 0 && ++paint_ticks >= 3) {
            phase = 1;
            mark_needs_layout();
        }
        Container::on_paint(p, b, ctx);
    }
};

/// @brief 统计表面上与 `c` 近似相等（各通道差 ≤ tol）的不透明像素数。
auto count_color(const Surface &s, Color c, const int tol) -> long {
    const int w = static_cast<int>(s.size().width);
    const int h = static_cast<int>(s.size().height);
    const std::uint8_t *buf = s.data();
    long n = 0;
    for (int i = 0; i < w * h; ++i) {
        const int r = buf[static_cast<std::size_t>(i) * 4];
        const int g = buf[(i * 4) + 1];
        const int b = buf[(i * 4) + 2];
        const int a = buf[(i * 4) + 3];
        if (a > 0 && std::abs(r - c.m_r) <= tol && std::abs(g - c.m_g) <= tol && std::abs(b - c.m_b) <= tol) {
            ++n;
        }
    }
    return n;
}

} // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_scroll_self_driving ===\n");

    HeadlessOptions opts;
    opts.size = Size{ .width = 200.0f, .height = 100.0f };
    auto res = create_window(opts);
    AURORA_TEST_CHECK(static_cast<bool>(res));
    auto &win = *res.value();

    auto body = std::make_shared<SwitchingBody>();
    auto scroll = std::make_shared<Scroll>(ScrollProps{ .child = Node{ body } });
    Node root{ scroll };

    // 连续推进若干帧：不注入任何外部标脏，全靠子树自驱动 + 延期布局驱动。
    constexpr int kFrames = 20;
    for (int i = 0; i < kFrames; ++i) {
        AURORA_TEST_CHECK(win.present_root(root).ok());
    }

    // 1) 延期布局真的发生了：骨架 → 真实内容切换（on_layout 至少重跑过一次）。
    AURORA_TEST_PRINTF("layout_calls=%d phase=%d\n", body->layout_calls, body->phase);
    AURORA_TEST_CHECK_MSG(body->phase == 1, "frame-driven skeleton->real-content switch should have happened");
    AURORA_TEST_CHECK_MSG(body->layout_calls >= 2,
                          "switch must trigger re-layout, on_layout should run more than once");
    AURORA_TEST_CHECK_MSG(static_cast<bool>(body->content),
                          "real content widget should be dynamically created after switch");

    // 2) 核心：切换后**动态新建**的叶控件必须持续被绘制，而不是只画 1 帧就被离屏缓冲冻结。
    //    旧实现（接线快照 / 重录后清脏）下此值恒为 1。
    const int content_paints = body->content->paints;
    AURORA_TEST_PRINTF("content leaf paints=%d (total frames %d)\n", content_paints, kFrames);
    AURORA_TEST_CHECK_MSG(
        content_paints > 5,
        "dynamically-created self-driving widget should keep painting every frame (offscreen buffer not frozen)");

    // 3) 帧循环没有退化成 idle 跳帧（自驱动脏必须每帧上达窗口）。
    AURORA_TEST_CHECK_MSG(!win.is_idle_frame(),
                          "self-driving animation in progress should not be judged as idle frame skip");

    // 4) 像素证据：真实内容颜色确实出现在窗口表面（不只是计数器在动）。
    const long content_px = count_color(win.surface(), kContentColor, 8);
    AURORA_TEST_PRINTF("content color pixels=%ld\n", content_px);
    AURORA_TEST_CHECK_MSG(content_px > 1000, "real content color should be composited onto window surface");
}
} // namespace aurora::tests::sec_scroll_self_driving

namespace aurora::tests::sec_scroll_viewport_repaint {

namespace {

constexpr float AURORA_TOP_H = 56.0f;  ///< Scroll 视口在屏幕上的垂直偏移（模拟 AppShell 顶栏高度）
constexpr float AURORA_LEAF_H = 40.0f; ///< 叶控件高度（填满 Scroll 视口宽度方向）

/// @brief 外部帧计数：测试每帧前写入，叶控件据此选色——使「屏幕应显示的颜色」与
///        「叶自身被绘次数」解耦，从而能区分「视口被真正重绘」与「首帧冻结」。
std::atomic g_tick{ 0 };

/// @brief 高区分度调色板：相邻帧颜色不同，便于判定屏幕是否显示「最新帧」。
// 测试常量表，抛出即终止可接受。
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const std::vector AURORA_PALETTE = {
    Color{ 255, 0, 0, 255 },   // 0 红
    Color{ 0, 255, 0, 255 },   // 1 绿
    Color{ 0, 0, 255, 255 },   // 2 蓝
    Color{ 255, 255, 0, 255 }, // 3 黄
    Color{ 0, 255, 255, 255 }, // 4 青
};

/// @brief 自驱动 + 按外部帧计数循环变色的叶控件。paints 记录真实绘制次数。
struct SelfDrivingLeaf : LeafWidget {
    int paints = 0;

    [[nodiscard]] auto type_name() const -> const char * override { return "SelfDrivingLeaf"; }
    /// 内容每帧变化，不参与 DL 缓存（否则回放会跳过 on_paint，掩盖被测行为）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = c.max.width, .height = AURORA_LEAF_H });
    }

    auto on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) -> void override {
        ++paints;
        const int idx = g_tick.load() % static_cast<int>(AURORA_PALETTE.size());
        p.fill_rect(b, AURORA_PALETTE[idx]); // 绘制「当前帧」颜色
        mark_needs_paint();                  // 自驱动：请求下一帧（脏须经布局父链上溯到 Scroll / 窗口）
    }
};

/// @brief 根容器：把 Scroll 放在偏移 (0, AURORA_TOP_H) 处（与 demo 的 AppShell 结构同构），
///        Scroll 内含自驱动叶控件。手动布局 + 设 bounds（Aurora 容器惯例，见 google_play_ui.h）。
struct RootShell : Container {
    std::shared_ptr<Scroll> m_scroll;
    std::shared_ptr<SelfDrivingLeaf> m_leaf;

    [[nodiscard]] auto type_name() const -> const char * override { return "RootShell"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const float W = c.max.width;
        const float H = c.max.height;
        if (!m_scroll) {
            m_leaf = std::make_shared<SelfDrivingLeaf>();
            m_scroll = std::make_shared<Scroll>(ScrollProps{ .child = Node{ m_leaf } });
            m_children.clear();
            m_children.emplace_back(m_scroll);
            for (auto &n : m_children) {
                n.widget().mount(ctx);
                n.widget().set_layout_parent(this);
            }
        }
        const float scrollH = std::max(0.0f, H - AURORA_TOP_H);
        m_children[0].widget().layout(
            Constraints{ .min = Size{ .width = W, .height = scrollH }, .max = Size{ .width = W, .height = scrollH } },
            ctx);
        // Scroll 视口位于屏幕 (0, AURORA_TOP_H) —— 内容坐标 (0,0) 的后代 ≠ 此屏幕坐标。
        m_children[0].set_bounds(
            Rect{ .origin = Point{ .x = 0.0f, .y = AURORA_TOP_H }, .size = Size{ .width = W, .height = scrollH } });
        return c.constrain(Size{ .width = W, .height = H });
    }
};

/// @brief 统计表面上与 `c` 各通道差 ≤ tol 的不透明像素数。
auto count_color(const Surface &s, Color c, const int tol) -> long {
    const int w = static_cast<int>(s.size().width);
    const int h = static_cast<int>(s.size().height);
    const std::uint8_t *buf = s.data();
    long n = 0;
    for (int i = 0; i < w * h; ++i) {
        const int r = buf[static_cast<std::size_t>(i) * 4];
        const int g = buf[(i * 4) + 1];
        const int b = buf[(i * 4) + 2];
        const int a = buf[(i * 4) + 3];
        if (a > 0 && std::abs(r - c.m_r) <= tol && std::abs(g - c.m_g) <= tol && std::abs(b - c.m_b) <= tol) {
            ++n;
        }
    }
    return n;
}

} // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_scroll_viewport_repaint ===\n");

    HeadlessOptions opts;
    opts.size = Size{ .width = 200.0f, .height = 200.0f };
    auto res = create_window(opts);
    AURORA_TEST_CHECK(static_cast<bool>(res));
    auto &win = *res.value();

    auto shell = std::make_shared<RootShell>();
    Node root{ shell };

    constexpr int kFrames = 12;
    for (int i = 0; i < kFrames; ++i) {
        g_tick = i; // 本帧叶控件应绘制 palette[i % N]
        AURORA_TEST_CHECK(win.present_root(root).ok());
    }

    // 1) 离屏缓冲确实在持续重录（Scroll::on_paint 每帧重绘内容）—— 证明链路与脏标记工作正常。
    AURORA_TEST_PRINTF("leaf paints=%d (total frames %d)\n", shell->m_leaf->paints, kFrames);
    AURORA_TEST_CHECK_MSG(shell->m_leaf->paints > kFrames / 2,
                          "offscreen buffer should keep re-recording (leaf widget keeps being painted)");

    // 2) 核心：屏幕视口应显示「最新帧」颜色 palette[(kFrames-1) % N]，而非首帧冻结色 palette[0]。
    //    修复前：视口被增量裁剪排除，屏幕冻结在首帧 → 最新色像素为 0。
    const int last = (kFrames - 1) % static_cast<int>(AURORA_PALETTE.size());
    const Color expect = AURORA_PALETTE[last];
    const long px = count_color(win.surface(), expect, 8);
    AURORA_TEST_PRINTF("expect latest color idx=%d pixels=%ld\n", last, px);
    AURORA_TEST_CHECK_MSG(
        px > 500, "viewport should keep repainting and show latest frame color (not first-frame freeze -> blank)");

    // 3) 反向校验：若 last≠0，首帧冻结色不应仍独占视口（确认确有「更新」发生，而非误判）。
    if (last != 0) {
        const long first_px = count_color(win.surface(), AURORA_PALETTE[0], 8);
        AURORA_TEST_PRINTF("first-frame color idx=0 pixels=%ld\n", first_px);
        // 最新色应明显多于首帧色（视口主要显示最新帧）。
        AURORA_TEST_CHECK_MSG(px > first_px,
                              "viewport should mainly show latest frame rather than first-frame frozen color");
    }
}
} // namespace aurora::tests::sec_scroll_viewport_repaint

AURORA_TEST() {
    aurora::tests::sec_scroll::run();
    aurora::tests::sec_scroll_grid::run();
    aurora::tests::sec_scroll_reanchor::run();
    aurora::tests::sec_scroll_regression::run();
    aurora::tests::sec_scroll_self_driving::run();
    aurora::tests::sec_scroll_viewport_repaint::run();
}
