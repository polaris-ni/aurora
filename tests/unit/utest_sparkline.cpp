/// 测试类型: unit
/// 目标单元: include/aurora/widget/sparkline.h
/// 测试说明: 覆盖 Sparkline（切片 4，最薄图表：无轴 / 无网格 / 无图例 / 无交互）——defaults /
/// describe_static / 序列化往返（values 数组 + 可选 color）/ 工厂 from_json 重建、
/// 空数据与单点 / 全等值不崩、以及像素 golden 基线（chart_sparkline.png，受 AURORA_UPDATE_GOLDEN 控制）

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

namespace aurora::test_cases::utest_sparkline {

namespace {

constexpr int WIDTH = 120;
constexpr int HEIGHT = 40;

[[nodiscard]] auto env_value(const char *name) -> std::string_view {
    const char *raw = std::getenv(name);
    if (raw == nullptr) {
        return {};
    }
    return {raw};
}

[[nodiscard]] auto env_flag(const char *name) -> bool { return !env_value(name).empty(); }

[[nodiscard]] auto env_int(const char *name, int fallback) -> int {
    const std::string_view raw = env_value(name);
    if (raw.empty()) {
        return fallback;
    }
    int parsed = fallback;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char *last = raw.data() + raw.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto [end, ec] = std::from_chars(raw.data(), last, parsed);
    return (ec == std::errc{} && end == last) ? parsed : fallback;
}

[[nodiscard]] auto render_chart(const SparklineProps &props) -> std::filesystem::path {
    auto chart = std::make_shared<Sparkline>(props);
    chart->modifier.set(Modifier{}.width(static_cast<float>(WIDTH)).height(static_cast<float>(HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_sparkline.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, WIDTH, HEIGHT, tmp.string().c_str()).ok());
    return tmp;
}

auto compare_or_update_golden(const std::filesystem::path &current_path, const std::string &base_name) -> void {
    const std::filesystem::path dir = std::filesystem::path(testing::isolation::repo_root()) / "tests" / "golden";
    const std::filesystem::path golden_path = dir / (base_name + ".png");
    const auto current = Image::load(current_path.string());
    AURORA_TEST_REQUIRE_TRUE(current.ok());
    if (env_flag("AURORA_UPDATE_GOLDEN")) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::copy_file(current_path, golden_path, std::filesystem::copy_options::overwrite_existing, ec);
        AURORA_TEST_CHECK_FALSE(static_cast<bool>(ec));
        return;
    }
    const auto golden = Image::load(golden_path.string());
    AURORA_TEST_REQUIRE_MSG(golden.ok(), base_name + ".png missing (run with AURORA_UPDATE_GOLDEN=1)");
    const SnapshotDiff diff =
        compare_snapshots(golden.value(), current.value(), env_int("AURORA_GOLDEN_MAX_DIFF", 0));
    // NOLINTNEXTLINE(modernize-use-integer-sign-comparison)
    const bool within_budget =
        diff.pixel_diff_count <= static_cast<std::size_t>(std::max(0, env_int("AURORA_GOLDEN_MAX_PIXELS", 0)));
    AURORA_TEST_CHECK_MSG(within_budget,
                          "pixel drift vs golden " + base_name + ": " + std::to_string(diff.pixel_diff_count) + " px");
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
        for (const PropDescriptor &p : d.properties) {
            if (p.name == key) {
                return true;
            }
        }
        return false;
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
    compare_or_update_golden(render_chart(p), "chart_sparkline");
}

}  // namespace aurora::test_cases::utest_sparkline
