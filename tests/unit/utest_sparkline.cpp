/// 测试类型: unit
/// 目标单元: include/aurora/widget/sparkline.h
/// 测试说明: 覆盖 Sparkline（切片 4，最薄图表：无轴 / 无网格 / 无图例 / 无交互）——defaults /
/// describe_static / 序列化往返（values 数组 + 可选 color）/ 工厂 from_json 重建、
/// 空数据与单点 / 全等值不崩、以及像素 golden 基线（chart_sparkline.png，受 AURORA_UPDATE_GOLDEN 控制）

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/render/snapshot_diff.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"

namespace aurora::test_cases::utest_sparkline {

namespace golden = aurora::testing::golden;

namespace {

constexpr int AURORA_WIDTH = 120;
constexpr int AURORA_HEIGHT = 40;

[[nodiscard]] auto render_chart(const SparklineProps &props) -> std::filesystem::path {
    auto chart = std::make_shared<Sparkline>(props);
    chart->modifier.set(Modifier{}.width(static_cast<float>(AURORA_WIDTH)).height(static_cast<float>(AURORA_HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_sparkline.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_WIDTH, AURORA_HEIGHT, tmp.string().c_str()).ok());
    return tmp;
}

}  // namespace

AURORA_TEST_CASE(defaults_and_identity) {
    const SparklineProps d = Sparkline::defaults();
    AURORA_TEST_CHECK_TRUE(d.values.empty());
    AURORA_TEST_CHECK_FALSE(d.color.has_value());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.line_width), 1.5, 1e-6);
    AURORA_TEST_CHECK_TRUE(d.show_end_dot);

    Sparkline s{};
    AURORA_TEST_CHECK_TRUE(s.type_name() == std::string{"Sparkline"});
    AURORA_TEST_CHECK_TRUE(s.accessibility_label() == std::string{"Sparkline, 0 points"});
    AURORA_TEST_CHECK_TRUE(s.accessibility_value().empty());
}

AURORA_TEST_CASE(describe_static_is_complete) {
    const WidgetDescriptor d = Sparkline::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "Sparkline");
    AURORA_TEST_CHECK_TRUE(d.events.empty());  // 无交互 ⇒ 无事件
    auto has = [&d](const std::string &key) -> bool {
        return std::ranges::any_of(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
    };
    for (const auto &key : {"values", "color", "line_width", "show_end_dot", "dot_radius", "padding"}) {
        AURORA_TEST_CHECK_TRUE(has(key));
    }
}

AURORA_TEST_CASE(props_roundtrip_and_factory) {
    SparklineProps p{};
    p.values = {1.0, 3.0, 2.0, 5.0};
    p.color = Color{10, 20, 30, 255};
    p.line_width = 2.5F;
    p.show_end_dot = false;
    Sparkline src{p};
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["values"].size(), 4U);

    Sparkline dst{};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.values == std::vector<double>{1.0, 3.0, 2.0, 5.0});
    AURORA_TEST_REQUIRE_TRUE(dst.color.has_value());
    AURORA_TEST_CHECK_TRUE(*dst.color == Color{10, 20, 30, 255});
    AURORA_TEST_CHECK_FALSE(dst.show_end_dot);

    // 未设色 ⇒ 不输出 color 键（保留「按色板取色」语义）
    Sparkline plain{};
    plain.set_values({1.0, 2.0});
    Json plain_props = Json::object();
    plain.serialize_props(plain_props);
    AURORA_TEST_CHECK_FALSE(plain_props.contains("color"));

    serialization::register_core_widgets();
    Json node = Json::object();
    node["type"] = "Sparkline";
    node["props"] = props;
    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    AURORA_TEST_REQUIRE_NOT_NULL(dynamic_cast<const Sparkline *>(built.value().get()));
}

AURORA_TEST_CASE(degenerate_data_is_safe) {
    // 空值 / 单点 / 全等值：均不得崩溃，golden 路径亦应呈现空或平线
    Sparkline empty{};
    empty.set_values({});
    AURORA_TEST_CHECK_TRUE(empty.accessibility_label() == std::string{"Sparkline, 0 points"});
    AURORA_TEST_CHECK_TRUE(render_chart(SparklineProps{}).empty() == false);
    AURORA_TEST_CHECK_TRUE(render_chart(SparklineProps{.values = {2.0}}).empty() == false);
    AURORA_TEST_CHECK_TRUE(render_chart(SparklineProps{.values = {2.0, 2.0, 2.0}}).empty() == false);
}

AURORA_TEST_CASE(golden_sparkline_matches_baseline) {
    SparklineProps p{};
    p.values = {4.0, 6.0, 3.0, 8.0, 5.0, 9.0, 7.0, 11.0};
    p.line_width = 2.0F;
    p.dot_radius = 2.5F;
    golden::compare_or_update("chart_sparkline", render_chart(p));
}

}  // namespace aurora::test_cases::utest_sparkline
