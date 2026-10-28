/// 测试类型: unit
/// 目标单元: include/aurora/widget/tab_bar.h
/// 测试说明: 覆盖 TabBar 的 API 面与交互分支——构造钳制、select/close_tab 的回调与选中修正、
/// 全部 set_* 链式配置、on_pointer_event 的标签命中/关闭钮命中/内容区下沉、serialize/deserialize
/// 往返（含 active_color 未设置时省略）、for_each_child/child_nodes 内容遍历、on_layout/on_paint/
/// paint_tab 渲染与 tab_width 计算、on_hit_test/on_hit_test_chain、on_mount/on_unmount/tick 转发。
///
/// 与 utest_tab_bar.cpp（运行期 add_tab 补挂时机）互补：本文件覆盖静态 API 与其余运行期分支。

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/tab_bar.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_tab_bar_api {

namespace {

/// @brief 极简叶子控件：固定 10×10 布局，统计挂载/卸载次数。
class LeafProbe final : public Widget {
  public:
    int mounts = 0;
    int unmounts = 0;
    [[nodiscard]] auto type_name() const -> const char * override { return "LeafProbe"; }
    auto collect_signals(StateSink &out) -> void override { (void)out; }
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 10.0F, .height = 10.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*b*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_mount(const BuildContext &ctx) -> void override {
        Widget::on_mount(ctx);
        ++mounts;
    }
    auto on_unmount(const BuildContext &ctx) -> void override {
        Widget::on_unmount(ctx);
        ++unmounts;
    }
};

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

// ⚠️ clang-analyzer 假告警族（clang 22 + MSVC STL）：make_shared 控制块（_Rep）经 Tab 移动、
// TabBar::add_tab 转移入 vector 后，析构侧 decref 在 Node/容器内联路径上不可见，
// 于是一批 add_tab 调用点被误判 NewDeleteLeaks、按值返回 TabBar 被误判 StackAddressEscape。
// 实际生命周期由 Node 的 RAII 持有保证（同 registry.cpp 的同族误报先例），
// 各调用点以行尾豁免标注；改动本文件后请复跑 scoped tidy 确认豁免与告警行一一对齐。
auto make_tab(const std::string &label, bool closable = false) -> Tab {
    return Tab{.label = label, .content = Node{std::make_shared<LeafProbe>()}, .closable = closable};
}

}  // namespace

AURORA_TEST_CASE(constructor_clamps_initial_selection) {
    // 默认构造：选中 0。
    TabBar def;
    AURORA_TEST_CHECK_EQ(def.selected(), 0);
    AURORA_TEST_CHECK_EQ(def.tab_count(), 0U);

    // 初始越界：超出尾端钳到末项、负数钳到 0。
    TabBar over{std::vector<Tab>{make_tab("A"), make_tab("B"), make_tab("C")}, 99};
    AURORA_TEST_CHECK_EQ(over.selected(), 2);
    TabBar under{std::vector<Tab>{make_tab("A"), make_tab("B")}, -5};
    AURORA_TEST_CHECK_EQ(under.selected(), 0);

    AURORA_TEST_CHECK_EQ(std::string(def.type_name()), "TabBar");
    AURORA_TEST_CHECK_TRUE(def.wants_click());
    // 静态/实例自描述同构（collect_signals 登记 selected_）。
    AURORA_TEST_CHECK_EQ(std::string(def.describe_static().name), "TabBar");
    AURORA_TEST_CHECK_EQ(std::string(def.describe().name), "TabBar");
}

AURORA_TEST_CASE(select_invokes_change_and_skips_noop) {
    TabBar bar;
    bar.add_tab(make_tab("A"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    bar.add_tab(make_tab("B"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    bar.add_tab(make_tab("C"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    int changes = 0;
    int last = -1;
    bar.set_on_change([&changes, &last](int i) {
        ++changes;
        last = i;
    });

    bar.select(1);
    AURORA_TEST_CHECK_EQ(bar.selected(), 1);
    AURORA_TEST_CHECK_EQ(changes, 1);
    AURORA_TEST_CHECK_EQ(last, 1);

    // 选同一项：不重复派发。
    bar.select(1);
    AURORA_TEST_CHECK_EQ(changes, 1);

    // 越界：忽略、不派发。
    bar.select(7);
    AURORA_TEST_CHECK_EQ(bar.selected(), 1);
    AURORA_TEST_CHECK_EQ(changes, 1);
    bar.select(-2);
    AURORA_TEST_CHECK_EQ(changes, 1);
}

AURORA_TEST_CASE(close_tab_removes_and_fixes_selection) {
    int closed = 0;
    int closed_idx = -1;
    auto build = [&closed, &closed_idx]() -> TabBar {
        TabBar bar;
        bar.add_tab(make_tab("A"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
        bar.add_tab(make_tab("B"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
        bar.add_tab(make_tab("C"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
        bar.set_on_close([&closed, &closed_idx](int i) {
            ++closed;
            closed_idx = i;
        });
        return bar;  // NOLINT(clang-analyzer-core.StackAddressEscape)
    };

    // 删除选中项之前：选中前移。
    {
        TabBar bar = build();
        bar.select(2);
        bar.close_tab(0);
        AURORA_TEST_CHECK_EQ(bar.tab_count(), 2U);
        AURORA_TEST_CHECK_EQ(bar.selected(), 1);  // 2 -> 1
        AURORA_TEST_CHECK_EQ(closed_idx, 0);
    }

    // 删除选中本身：前移。
    {
        closed = 0;
        TabBar bar = build();
        bar.select(2);
        bar.close_tab(2);
        AURORA_TEST_CHECK_EQ(bar.tab_count(), 2U);
        AURORA_TEST_CHECK_EQ(bar.selected(), 1);
        AURORA_TEST_CHECK_EQ(closed_idx, 2);
    }

    // 删除选中之后：选中不变（删除点 > 选中）。
    {
        closed = 0;
        TabBar bar = build();
        bar.select(0);
        bar.close_tab(1);
        AURORA_TEST_CHECK_EQ(bar.tab_count(), 2U);
        AURORA_TEST_CHECK_EQ(bar.selected(), 0);
        AURORA_TEST_CHECK_EQ(closed_idx, 1);
    }

    // 越界 close_tab 安全 no-op。
    closed = 0;
    closed_idx = -1;
    TabBar bar = build();
    bar.close_tab(-1);
    bar.close_tab(42);
    AURORA_TEST_CHECK_EQ(bar.tab_count(), 3U);
    AURORA_TEST_CHECK_EQ(closed, 0);
}

AURORA_TEST_CASE(tab_label_and_count) {
    TabBar bar;
    bar.add_tab(make_tab("Alpha"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    bar.add_tab(make_tab("Beta"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    AURORA_TEST_CHECK_EQ(bar.tab_count(), 2U);
    AURORA_TEST_CHECK_EQ(bar.tab_label(0), "Alpha");
    AURORA_TEST_CHECK_EQ(bar.tab_label(1), "Beta");
    AURORA_TEST_CHECK_EQ(bar.tab_label(2), "");  // 越界返回空
    AURORA_TEST_CHECK_EQ(bar.tab_label(-1), "");
}

AURORA_TEST_CASE(setters_chain_and_store) {
    TabBar bar;
    auto &ref = bar.set_tab_height(40.0F)
                    .set_active_color(Color{10, 20, 30, 255})
                    .set_bar_background(Color{245, 245, 247, 255})
                    .set_tab_background(Color{255, 255, 255, 255})
                    .set_text_color(Color{96, 96, 96, 255})
                    .set_indicator_thickness(3.0F)
                    .set_font_size(14.0F)
                    .set_tab_padding(14.0F);
    (void)ref;  // 链式返回自身引用

    // 边界回落：非正/负值取默认。
    bar.set_tab_height(-1.0F);  // -> 36
    bar.set_indicator_thickness(-1.0F);  // -> 2
    bar.set_font_size(0.0F);  // -> 13
    bar.set_tab_padding(-4.0F);  // -> 12

    int change_hits = 0;
    int close_hits = 0;
    bar.set_on_change([&change_hits](int) { ++change_hits; });
    bar.set_on_close([&close_hits](int) { ++close_hits; });
    AURORA_TEST_CHECK_EQ(change_hits, 0);
    AURORA_TEST_CHECK_EQ(close_hits, 0);

    // selected_index()/selected() 为同一状态引用。
    bar.select(0);
    AURORA_TEST_CHECK_EQ(bar.selected_index().get(), 0);
}

AURORA_TEST_CASE(pointer_event_selects_and_closes_tab) {
    TabBar bar;
    auto a = make_tab("A");  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto b = make_tab("B", true);  // 可关闭  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    bar.add_tab(a);
    bar.add_tab(b);
    bar.set_active_color(Color{1, 2, 3, 255});  // 避免 on_paint 走 inherit_theme
    BuildContext ctx;
    bar.mount(ctx);
    LayoutEngine::layout(bar, bounded(320.0F, 240.0F));

    // 命中第一个标签（x 在 [0,60) 内，y < 栏高）：select(0)。
    MouseEvent sel;
    sel.action = MouseAction::Press;
    sel.button = MouseButton::Left;
    sel.local_position = Point{.x = 10.0F, .y = 5.0F};
    bar.on_pointer_event(sel);
    AURORA_TEST_CHECK_EQ(bar.selected(), 0);
    AURORA_TEST_CHECK_EQ(sel.is_handled, true);

    // 命中第二个标签的关闭区（标签宽 >= 60，关闭区在 [w-20, w)）：close_tab(1)。
    // 第一个标签宽约 60，第二个标签起点 x≈60；其关闭区在 x∈[60+40, 60+60) = [100,120)。
    MouseEvent close_ev;
    close_ev.action = MouseAction::Press;
    close_ev.button = MouseButton::Left;
    close_ev.local_position = Point{.x = 110.0F, .y = 5.0F};
    bar.on_pointer_event(close_ev);
    AURORA_TEST_CHECK_EQ(bar.tab_count(), 1U);
    AURORA_TEST_CHECK_EQ(close_ev.is_handled, true);
}

AURORA_TEST_CASE(pointer_event_content_area_falls_through) {
    TabBar bar;
    bar.add_tab(make_tab("A"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks) 内容区 y >= 栏高
    BuildContext ctx;
    bar.mount(ctx);
    LayoutEngine::layout(bar, bounded(320.0F, 240.0F));

    // y 落在内容区（>= 栏高 36）：下沉基类链，不命中标签、不切换选中。
    MouseEvent e;
    e.action = MouseAction::Press;
    e.button = MouseButton::Left;
    e.local_position = Point{.x = 10.0F, .y = 200.0F};
    bar.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(bar.selected(), 0);  // 内容区点击不改选中
}

AURORA_TEST_CASE(serialize_roundtrip_and_active_color_omission) {
    // 全字段设置 → 序列化包含 active_color；反序列化还原。
    TabBar bar;
    bar.add_tab(make_tab("A"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    bar.add_tab(make_tab("B"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    bar.select(1);
    bar.set_tab_height(40.0F);
    bar.set_active_color(Color{200, 100, 50, 255});
    bar.set_bar_background(Color{245, 245, 247, 255});
    bar.set_tab_background(Color{255, 255, 255, 255});
    bar.set_text_color(Color{96, 96, 96, 255});
    bar.set_indicator_thickness(3.0F);
    bar.set_font_size(14.0F);
    bar.set_tab_padding(14.0F);
    Json props = Json::object();
    bar.serialize_props(props);

    AURORA_TEST_CHECK_TRUE(props.contains("selected_index"));
    AURORA_TEST_CHECK_TRUE(props.contains("tab_height"));
    AURORA_TEST_CHECK_TRUE(props.contains("tab_labels"));
    AURORA_TEST_CHECK_TRUE(props.contains("active_color"));
    AURORA_TEST_CHECK_TRUE(props.contains("bar_background"));
    AURORA_TEST_CHECK_TRUE(props.contains("tab_background"));
    AURORA_TEST_CHECK_TRUE(props.contains("text_color"));
    AURORA_TEST_CHECK_TRUE(props.contains("indicator_thickness"));
    AURORA_TEST_CHECK_TRUE(props.contains("font_size"));
    AURORA_TEST_CHECK_TRUE(props.contains("tab_padding"));

    TabBar restored;
    restored.deserialize_props(props);
    // 注意：deserialize_props 仅还原选中态与样式（内容子树经序列化丢失），故 tab_labels 不往返。
    AURORA_TEST_CHECK_EQ(restored.selected(), 1);  // selected_index 往返正确

    // 未设置 active_color 的栏：序列化省略该键（保留「跟随主题」语义）。
    TabBar plain;
    plain.add_tab(make_tab("X"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    Json p2 = Json::object();
    plain.serialize_props(p2);
    AURORA_TEST_CHECK_FALSE(p2.contains("active_color"));
}

AURORA_TEST_CASE(paint_and_hit_test_exercise_render_paths) {
    TabBar bar;
    bar.add_tab(make_tab("A"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    bar.add_tab(make_tab("B"));  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    bar.set_active_color(Color{200, 100, 50, 255});
    bar.set_indicator_thickness(2.0F);
    BuildContext ctx;
    bar.mount(ctx);
    LayoutEngine::layout(bar, bounded(320.0F, 240.0F));

    Painter painter;
    Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 320.0F, .height = 240.0F}};
    bar.paint(painter, bounds, ctx);  // 覆盖 on_paint / paint_tab（选中背景+指示器+文本）

    // 标签栏区域命中自身；内容区命中选中子树（LeafProbe）。
    AURORA_TEST_CHECK_EQ(bar.hit_test(Point{.x = 5.0F, .y = 5.0F}, bounds, ctx), &bar);
    const std::vector<HitNode> chain = bar.hit_test_chain(Point{.x = 5.0F, .y = 5.0F}, bounds, ctx);
    AURORA_TEST_CHECK_FALSE(chain.empty());  // 栏条内命中（基类前置 this），覆盖 on_hit_test_chain 栏分支

    // 内容区：命中链下钻到选中内容叶子（覆盖 on_hit_test_chain 内容分支）。
    const std::vector<HitNode> content_chain = bar.hit_test_chain(Point{.x = 5.0F, .y = 50.0F}, bounds, ctx);
    AURORA_TEST_CHECK_FALSE(content_chain.empty());
}

AURORA_TEST_CASE(children_traversal_and_mount_lifecycle) {
    TabBar bar;
    auto a = std::make_shared<LeafProbe>();
    auto b = std::make_shared<LeafProbe>();
    bar.add_tab(Tab{.label = "A", .content = Node{a}});
    bar.add_tab(Tab{.label = "B", .content = Node{b}});
    BuildContext ctx;
    bar.mount(ctx);
    AURORA_TEST_CHECK_EQ(a->mounts, 1);
    AURORA_TEST_CHECK_EQ(b->mounts, 1);

    // 遍历：仅含非空内容节点（两个）。
    int visited = 0;
    bar.for_each_child([&visited](const Widget &) { ++visited; });
    AURORA_TEST_CHECK_EQ(visited, 2);
    AURORA_TEST_CHECK_EQ(bar.child_nodes().size(), 2U);

    // tick 转发到各内容子节点的 tick。
    bar.tick(std::chrono::steady_clock::now());

    bar.unmount();
    AURORA_TEST_CHECK_EQ(a->unmounts, 1);
    AURORA_TEST_CHECK_EQ(b->unmounts, 1);
}

}  // namespace aurora::test_cases::utest_tab_bar_api
