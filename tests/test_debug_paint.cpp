// test_debug_paint.cpp — 可视化调试叠层 + 控件拾取验证。
//
// 覆盖：
//  1) DebugPaintFlags / set_flags / flags / any_flag_enabled 语义（DEBUG 下可切换；Release 全 false）。
//  2) 全树绘制后 paint_debug_overlays 计数（layout_guides == 总控件数、relayout_boundaries == boundary 数、
//     repaint_highlight == layout_guides 首帧全重绘、overdraw == 总控件数 - 1）。
//  3) widget_picker 在已知子控件坐标返回根→最深的命中链，最深层为预期控件。
//  4) Release（未开 DEBUG）：any_flag_enabled()==false、overlay_stats 全 0、picker 返回空。
//
// 宏一致约定：测试 TU 与 aurora 库同配置获得 AURORA_ENABLE_DEBUG；
// success-path 断言用 #ifdef AURORA_ENABLE_DEBUG 分支，与库体编译分支对齐。

#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "aurora/test_helpers.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Column;
using aurora::Constraints;
using aurora::HeadlessSurface;
using aurora::LeafWidget;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::px;
using aurora::Rect;
using aurora::Size;
using aurora::Widget;
using aurora::debug::any_flag_enabled;
using aurora::debug::bump_debug_frame;
using aurora::debug::DebugOverlayStats;
using aurora::debug::DebugPaintFlags;
using aurora::debug::DebugPickResult;
using aurora::debug::flags;
using aurora::debug::overlay_stats;
using aurora::debug::paint_debug_overlays;
using aurora::debug::reset_overlay_stats;
using aurora::debug::widget_picker;
using aurora::test::absolute_bounds;
using aurora::test::init_headless;
using aurora::test::TestEnv;

namespace {

// 调试拾取测试用的极简叶控件：固定 40x20，无字体依赖，命中测试命中自身。
class PickLeaf : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "PickLeaf"; }

  protected:
    [[nodiscard]] auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        return Size{ .width = 40.0f, .height = 20.0f };
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

// 构建一棵已知树：root(Column) -> A(Column,80x40,有背景) -> leaf(PickLeaf,40x20)
//                                      -> B(Column,60x30,有背景)
// 控件总数 = 4；relayout boundary = A、B 两个显式尺寸控件（leaf/root 为 WrapContent）。
auto build_tree(const std::shared_ptr<Column> &root) -> void {
    const auto a = std::make_shared<Column>();
    a->width(px(80.0f));
    a->height(px(40.0f));
    a->modifier.set(Modifier{}.background(Color(200, 200, 200, 255)));
    const auto leaf = std::make_shared<PickLeaf>();
    a->add(Node{ leaf });

    const auto b = std::make_shared<Column>();
    b->width(px(60.0f));
    b->height(px(30.0f));
    b->modifier.set(Modifier{}.background(Color(100, 200, 100, 255)));

    root->add(Node{ a });
    root->add(Node{ b });
}

} // namespace

AURORA_TEST() {
    BuildContext ctx{};

    // ---- 1. flags 语义 ----
    {
        set_flags(DebugPaintFlags{}); // 复位
        AURORA_TEST_CHECK(!any_flag_enabled());
        AURORA_TEST_CHECK(!flags().layout_guides);
        set_flags(DebugPaintFlags{ .layout_guides = true, .overdraw = true });
#ifdef AURORA_ENABLE_DEBUG
        // DEBUG：set_flags 生效，可切换各叠层开关。
        AURORA_TEST_CHECK(any_flag_enabled());
        AURORA_TEST_CHECK(flags().layout_guides);
        AURORA_TEST_CHECK(flags().overdraw);
        AURORA_TEST_CHECK(!flags().repaint_highlight);
#else
        // Release：set_flags 为 no-op，flags 恒全 false。
        AURORA_TEST_CHECK(!flags().layout_guides);
        AURORA_TEST_CHECK(!flags().overdraw);
#endif
        set_flags(DebugPaintFlags{});
    }

    // ---- 2. paint_debug_overlays 计数 ----
    {
        auto root = std::make_shared<Column>();
        build_tree(root);

        // 先 layout（不 paint），保证下一步是「首帧」：所有控件走 render_into 实际重绘。
        root->layout(Constraints{ .min = Size{ .width = 0.0f, .height = 0.0f },
                                  .max = Size{ .width = 200.0f, .height = 200.0f } },
                     ctx);

        HeadlessSurface surf("", Size{ .width = 200.0f, .height = 200.0f });
        auto bf = surf.begin_frame(200, 200);
        AURORA_TEST_CHECK(bf.ok());
        auto &p = surf.painter();

        set_flags(DebugPaintFlags{
            .layout_guides = true, .relayout_boundaries = true, .repaint_highlight = true, .overdraw = true });

#ifdef AURORA_ENABLE_DEBUG
        bump_debug_frame(); // 模拟 present_root 的帧前移
        reset_overlay_stats();
        root->paint(
            p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 200.0f, .height = 200.0f } }, ctx);
        paint_debug_overlays(
            p, *root,
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 200.0f, .height = 200.0f } }, ctx);
        const DebugOverlayStats st = overlay_stats();

        // 总控件数 = 4（root + A + leaf + B）。
        AURORA_TEST_CHECK(st.layout_guides_drawn == 4);
        // relayout boundary = A、B（显式尺寸）；leaf/root 为 WrapContent → 非 boundary。
        AURORA_TEST_CHECK(st.relayout_boundaries_drawn == 2);
        // 首帧：所有控件经 render_into 实际重绘 → repaint_highlight 计数 == 全部已绘制控件。
        AURORA_TEST_CHECK(st.repaint_highlight_drawn == st.layout_guides_drawn);
        AURORA_TEST_CHECK(st.repaint_highlight_drawn > 0);
        // overdraw 跳过根背景，统计参与叠加的非根控件 = 3。
        AURORA_TEST_CHECK(st.overdraw_regions_drawn == 3);
#else
        // Release：paint_debug_overlays 为零开销 no-op，统计全 0。
        bump_debug_frame();
        reset_overlay_stats();
        root->paint(
            p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 200.0f, .height = 200.0f } }, ctx);
        paint_debug_overlays(
            p, *root,
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 200.0f, .height = 200.0f } }, ctx);
        const DebugOverlayStats st = overlay_stats();
        AURORA_TEST_CHECK(st.layout_guides_drawn == 0);
        AURORA_TEST_CHECK(st.relayout_boundaries_drawn == 0);
        AURORA_TEST_CHECK(st.repaint_highlight_drawn == 0);
        AURORA_TEST_CHECK(st.overdraw_regions_drawn == 0);
#endif
        set_flags(DebugPaintFlags{});
    }

    // ---- 3. widget_picker 命中链 ----
    {
        TestEnv env = init_headless(200, 200);
        build_tree(env.root_widget);
        pump(env); // 布局（含挂载）；绘制/缓存不影响拾取（走 hit_test_chain）。

        // 定位树中 type_name == "PickLeaf" 的控件（自定义叶控件）。
        Widget *leaf_w = nullptr;
        std::function<void(Node &)> find = [&](Node &n) -> void {
            if (leaf_w) {
                return;
            }
            if (n.widget().type_name() == std::string("PickLeaf")) {
                leaf_w = &n.widget();
            }
            for (const Node &c : n.widget().child_nodes()) {
                find(const_cast<Node &>(c));
            }
        };
        find(env.root);
        AURORA_TEST_CHECK(leaf_w != nullptr);

        const auto box = absolute_bounds(env.root, *leaf_w);
        AURORA_TEST_REQUIRE(box.has_value());
        const Point center{ .x = box->origin.x + (box->size.width / 2.0f),
                            .y = box->origin.y + (box->size.height / 2.0f) };

        const DebugPickResult res = widget_picker(
            *env.root_widget,
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 200.0f, .height = 200.0f } }, ctx,
            center);

#ifdef AURORA_ENABLE_DEBUG
        AURORA_TEST_CHECK(res.hit);
        // 链 = [root Column, A Column, PickLeaf]（根→最深）。
        AURORA_TEST_CHECK(res.chain.size() >= 3);
        AURORA_TEST_CHECK(res.chain.front().type_name == std::string("Column"));  // 根
        AURORA_TEST_CHECK(res.chain.back().type_name == std::string("PickLeaf")); // 最深
        // 最深层控件盒应包含拾取点。
        AURORA_TEST_CHECK(res.chain.back().bounds.contains(center));
#else
        AURORA_TEST_CHECK(!res.hit);
        AURORA_TEST_CHECK(res.chain.empty());
#endif
    }
}
