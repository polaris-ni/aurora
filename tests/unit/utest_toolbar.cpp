/// 测试类型: unit
/// 目标单元: include/aurora/widget/toolbar.h
/// 测试说明: 覆盖 ToolBar/StatusBar 的类型契约与自描述、构造（vector/初始化列表）、
/// 链式配置与非法值降级、水平排列/垂直居中/尾项右对齐布局行为、序列化往返与无头绘制冒烟

#include <cmath>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/text.h"
#include "aurora/widget/toolbar.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_toolbar {

namespace {

/// 挂载并按给定上限布局，返回测得尺寸（无头环境：BuildContext + 约束）。
auto laid_out(Widget& w, float max_w, float max_h) -> Size {
    BuildContext ctx;
    w.mount(ctx);
    const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = max_w, .height = max_h}};
    return w.layout(c, ctx);
}

}  // namespace

AURORA_TEST_CASE(toolbar_type_contract) {
    // ToolBar/StatusBar 均为 Container 派生、不可复制。
    static_assert(std::is_base_of_v<aurora::Container, aurora::ToolBar>);
    static_assert(std::is_base_of_v<aurora::Container, aurora::StatusBar>);
    static_assert(!std::is_copy_constructible_v<aurora::ToolBar>);
    static_assert(!std::is_copy_constructible_v<aurora::StatusBar>);

    ToolBar tb;
    StatusBar sb;
    AURORA_TEST_CHECK_EQ(std::string{tb.type_name()}, "ToolBar");
    AURORA_TEST_CHECK_EQ(std::string{sb.type_name()}, "StatusBar");

    // 自描述：多子项策略 + 各自属性清单。
    const auto td = ToolBar::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{td.name}, "ToolBar");
    AURORA_TEST_CHECK_EQ(std::string{td.children_policy}, "multiple");
    AURORA_TEST_REQUIRE_EQ(td.properties.size(), 3U);
    AURORA_TEST_CHECK_EQ(std::string{td.properties[0].name}, "bar_height");
    AURORA_TEST_CHECK_EQ(std::string{td.properties[1].name}, "gap");
    AURORA_TEST_CHECK_EQ(std::string{td.properties[2].name}, "padding");

    const auto sd = StatusBar::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{sd.name}, "StatusBar");
    AURORA_TEST_CHECK_EQ(std::string{sd.children_policy}, "multiple");
    AURORA_TEST_REQUIRE_EQ(sd.properties.size(), 2U);
    AURORA_TEST_CHECK_EQ(std::string{sd.properties[0].name}, "bar_height");
    AURORA_TEST_CHECK_EQ(std::string{sd.properties[1].name}, "gap");
}

AURORA_TEST_CASE(toolbar_defaults_and_chained_setters) {
    ToolBar tb;
    AURORA_TEST_CHECK_NEAR(tb.bar_height(), 40.0F, 1e-4F);

    // 链式设置生效。
    tb.set_bar_height(56.0F).set_gap(10.0F);
    AURORA_TEST_CHECK_NEAR(tb.bar_height(), 56.0F, 1e-4F);
    Json props;
    tb.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["gap"].get<float>(), 10.0F, 1e-4F);

    // 非法值降级：bar_height 非正回到 40，gap 负值钳 0。
    ToolBar bad;
    bad.set_bar_height(-1.0F).set_gap(-2.0F);
    AURORA_TEST_CHECK_NEAR(bad.bar_height(), 40.0F, 1e-4F);
    Json bprops;
    bad.serialize_props(bprops);
    AURORA_TEST_CHECK_NEAR(bprops["gap"].get<float>(), 0.0F, 1e-4F);

    // StatusBar 默认 24，非法回落 24。
    StatusBar sb;
    AURORA_TEST_CHECK_NEAR(sb.bar_height(), 24.0F, 1e-4F);
    sb.set_bar_height(28.0F);
    AURORA_TEST_CHECK_NEAR(sb.bar_height(), 28.0F, 1e-4F);
    sb.set_bar_height(-5.0F);
    AURORA_TEST_CHECK_NEAR(sb.bar_height(), 24.0F, 1e-4F);
}

AURORA_TEST_CASE(toolbar_layout_orders_and_centers_children) {
    std::vector<Node> kids;
    kids.emplace_back(Button{"A"});
    kids.emplace_back(Button{"B"});
    ToolBar tb{std::move(kids)};

    const Size s = laid_out(tb, 640.0F, 480.0F);
    AURORA_TEST_CHECK_NEAR(s.width, 640.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(s.height, 40.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(s.height, tb.bar_height(), 1e-3F);

    const auto& nodes = tb.child_nodes();
    AURORA_TEST_REQUIRE_EQ(nodes.size(), 2U);
    const Rect b0 = nodes[0].bounds();
    const Rect b1 = nodes[1].bounds();

    // 第一项从 padding(6) 起；第二项 = 第一项宽 + gap(4)。
    AURORA_TEST_CHECK_NEAR(b0.origin.x, 6.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(b1.origin.x, 6.0F + b0.size.width + 4.0F, 1e-3F);

    // 垂直居中：y == (bar_height - 子高) / 2。
    AURORA_TEST_CHECK_NEAR(b0.origin.y, (40.0F - b0.size.height) * 0.5F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(b0.origin.y > 0.0F);
}

AURORA_TEST_CASE(toolbar_gap_zero_joins_children) {
    std::vector<Node> kids;
    kids.emplace_back(Button{"A"});
    kids.emplace_back(Button{"B"});
    ToolBar tb{std::move(kids)};
    tb.set_gap(0.0F);

    laid_out(tb, 640.0F, 480.0F);
    const auto& nodes = tb.child_nodes();
    AURORA_TEST_REQUIRE_EQ(nodes.size(), 2U);
    // gap=0 时第二项紧贴第一项（仅隔 padding 起点）。
    AURORA_TEST_CHECK_NEAR(nodes[1].bounds().origin.x, nodes[0].bounds().origin.x + nodes[0].bounds().size.width,
                           1e-3F);
}

AURORA_TEST_CASE(toolbar_constructors_and_empty_bar) {
    // 初始化列表构造。
    ToolBar listed{Node{Button{"A"}}, Node{Button{"B"}}};
    AURORA_TEST_CHECK_EQ(listed.child_nodes().size(), 2U);

    // 默认构造无子项；空栏布局仍返回整栏尺寸。
    ToolBar empty;
    AURORA_TEST_CHECK_EQ(empty.child_nodes().size(), 0U);
    const Size s = laid_out(empty, 640.0F, 480.0F);
    AURORA_TEST_CHECK_NEAR(s.width, 640.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(s.height, 40.0F, 1e-3F);

    // StatusBar 同构：vector 构造 + 尾项语义由布局用例覆盖。
    std::vector<Node> texts;
    texts.emplace_back(Text{"Ready"});
    texts.emplace_back(Text{"Ln 1"});
    StatusBar sb{std::move(texts)};
    AURORA_TEST_CHECK_EQ(sb.child_nodes().size(), 2U);
}

AURORA_TEST_CASE(toolbar_json_roundtrip) {
    ToolBar src;
    src.set_bar_height(48.0F).set_gap(8.0F);
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["bar_height"].get<float>(), 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["gap"].get<float>(), 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["padding"].get<float>(), 6.0F, 1e-4F);

    ToolBar dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.bar_height(), 48.0F, 1e-4F);
    Json dprops;
    dst.serialize_props(dprops);
    AURORA_TEST_CHECK_NEAR(dprops["gap"].get<float>(), 8.0F, 1e-4F);

    StatusBar ssrc;
    ssrc.set_bar_height(28.0F);
    Json sprops;
    ssrc.serialize_props(sprops);
    AURORA_TEST_CHECK_NEAR(sprops["bar_height"].get<float>(), 28.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sprops["gap"].get<float>(), 12.0F, 1e-4F);

    StatusBar sdst;
    sdst.deserialize_props(sprops);
    AURORA_TEST_CHECK_NEAR(sdst.bar_height(), 28.0F, 1e-4F);
}

AURORA_TEST_CASE(statusbar_layout_tail_right_aligned) {
    std::vector<Node> kids;
    kids.emplace_back(Text{"Ready"});
    kids.emplace_back(Text{"UTF-8"});
    kids.emplace_back(Text{"Ln 1, Col 1"});
    StatusBar sb{std::move(kids)};

    const Size s = laid_out(sb, 640.0F, 480.0F);
    AURORA_TEST_CHECK_NEAR(s.width, 640.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(s.height, 24.0F, 1e-3F);

    const auto& nodes = sb.child_nodes();
    AURORA_TEST_REQUIRE_EQ(nodes.size(), 3U);
    const Rect b0 = nodes[0].bounds();
    const Rect b1 = nodes[1].bounds();
    const Rect b2 = nodes[2].bounds();

    // 前两项自 padding(8) 起左排，尾项右对齐到 640-8。
    AURORA_TEST_CHECK_NEAR(b0.origin.x, 8.0F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(b1.origin.x > b0.origin.x);
    const float tail_right = b2.origin.x + b2.size.width;
    AURORA_TEST_CHECK_NEAR(tail_right, 632.0F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(b2.origin.x > b1.origin.x);

    // 垂直居中。
    AURORA_TEST_CHECK_NEAR(b0.origin.y, (24.0F - b0.size.height) * 0.5F, 1e-3F);
}

AURORA_TEST_CASE(statusbar_single_child_left_aligned) {
    std::vector<Node> kids;
    kids.emplace_back(Text{"Only"});
    StatusBar sb{std::move(kids)};

    laid_out(sb, 640.0F, 480.0F);
    const auto& nodes = sb.child_nodes();
    AURORA_TEST_REQUIRE_EQ(nodes.size(), 1U);
    // 单子项不右对齐：从 padding 起左排。
    AURORA_TEST_CHECK_NEAR(nodes[0].bounds().origin.x, 8.0F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(nodes[0].bounds().origin.x + nodes[0].bounds().size.width < 632.0F);
}

AURORA_TEST_CASE(toolbar_statusbar_paint_smoke) {
    std::vector<Node> tkids;
    tkids.emplace_back(Button{"Run"});
    ToolBar tb{std::move(tkids)};

    std::vector<Node> skids;
    skids.emplace_back(Text{"OK"});
    skids.emplace_back(Text{"v1.0"});
    StatusBar sb{std::move(skids)};

    laid_out(tb, 320.0F, 240.0F);
    laid_out(sb, 320.0F, 240.0F);

    Painter p;
    p.begin(320, 240);
    BuildContext ctx;
    tb.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 320.0F, .height = 40.0F}}, ctx);
    sb.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 216.0F}, .size = Size{.width = 320.0F, .height = 24.0F}}, ctx);
    AURORA_TEST_CHECK_EQ(p.width(), 320);
    AURORA_TEST_CHECK_EQ(p.height(), 240);
}

}  // namespace aurora::test_cases::utest_toolbar
