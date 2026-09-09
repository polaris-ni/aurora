/// 测试类型: unit
/// 目标单元: include/aurora/render/offscreen.h
/// 测试说明: 覆盖无头渲染两条产线——render_to_png 写出可解码 PNG、render_to_logical_snapshot 的
/// 平台无关盒模型树结构与确定性；并回归两类 golden 基准：像素基准（golden_basic_column.png，
/// 受 AURORA_GOLDEN_DIR / MAX_DIFF / MAX_PIXELS / UPDATE_GOLDEN 控制）与
/// 逻辑快照基准（logical_snapshots.json，11 场景逐值比对）

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/render/snapshot_diff.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_offscreen {

namespace {

// ---- 场景构建（固定尺寸文本盒：盒模型完全由 px() 决定，与字体度量无关）----

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(px(w));
    t->height(px(h));
    return Node{t};
}

auto fill_box(float w) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(px(w));
    t->height(fill());
    return Node{t};
}

auto hfill_box(float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(fill());
    t->height(px(h));
    return Node{t};
}

struct Scenario {
    const char* name;
    std::function<Node()> build;
};

auto scenarios() -> std::vector<Scenario> {
    return {
        {.name = "text_fixed", .build = []() -> Node { return box(120.0F, 40.0F); }},
        {.name = "column_fixed",
         .build = []() -> Node {
             auto col = std::make_shared<Column>();
             col->add(box(100.0F, 20.0F));
             col->add(box(100.0F, 20.0F));
             col->add(box(100.0F, 20.0F));
             return Node{col};
         }},
        {.name = "row_fixed",
         .build = []() -> Node {
             auto row = std::make_shared<Row>();
             row->add(box(40.0F, 30.0F));
             row->add(box(40.0F, 30.0F));
             row->add(box(40.0F, 30.0F));
             return Node{row};
         }},
        {.name = "column_gap",
         .build = []() -> Node {
             auto col = std::make_shared<Column>();
             col->set_gap(10.0F);
             col->add(box(100.0F, 20.0F));
             col->add(box(100.0F, 20.0F));
             col->add(box(100.0F, 20.0F));
             return Node{col};
         }},
        {.name = "row_flex_fill",
         .build = []() -> Node {
             auto row = std::make_shared<Row>();
             row->add(box(80.0F, 30.0F));
             row->add(box(80.0F, 30.0F));
             row->add(hfill_box(30.0F));
             return Node{row};
         }},
        {.name = "stack_overlay",
         .build = []() -> Node {
             auto st = std::make_shared<Stack>();
             st->add(box(80.0F, 60.0F));
             st->add(box(60.0F, 40.0F));
             return Node{st};
         }},
        {.name = "grid_2x2",
         .build = []() -> Node {
             GridProps props;
             props.columns = 2;
             props.gap = 8.0F;
             props.children = {box(60.0F, 40.0F), box(60.0F, 40.0F), box(60.0F, 40.0F), box(60.0F, 40.0F)};
             return Node{std::make_shared<Grid>(std::move(props))};
         }},
        {.name = "nested_column_row",
         .build = []() -> Node {
             auto col = std::make_shared<Column>();
             auto row = std::make_shared<Row>();
             row->add(box(50.0F, 20.0F));
             row->add(box(50.0F, 20.0F));
             col->add(Node{row});
             col->add(box(100.0F, 20.0F));
             return Node{col};
         }},
        {.name = "flex_expand",
         .build = []() -> Node {
             auto col = std::make_shared<Column>();
             col->add(box(100.0F, 20.0F));
             col->add(fill_box(100.0F));
             return Node{col};
         }},
        {.name = "scroll_tall_content",
         .build = []() -> Node {
             auto inner = std::make_shared<Column>();
             inner->add(box(100.0F, 100.0F));
             inner->add(box(100.0F, 100.0F));
             inner->add(box(100.0F, 100.0F));
             auto scroll = std::make_shared<Scroll>();
             scroll->add(Node{inner});
             return Node{scroll};
         }},
        {.name = "padding_inset",
         .build = []() -> Node {
             auto col = std::make_shared<Column>();
             col->modifier.set(Modifier{}.padding(20.0F));
             col->add(box(100.0F, 50.0F));
             return Node{col};
         }},
    };
}

/// @brief 定位 golden 目录：优先环境变量，其次仓库根相对路径。
auto golden_dir() -> std::filesystem::path {
    const char* override_dir = std::getenv("AURORA_GOLDEN_DIR");
    if (override_dir != nullptr && *override_dir != '\0') {
        return {override_dir};
    }
    if (!testing::isolation::repo_root().empty()) {
        return std::filesystem::path(testing::isolation::repo_root()) / "tests" / "golden";
    }
    return {"tests/golden"};
}

/// @brief 读取环境变量；空/未设置返回空视图（避免下标访问裸指针）。
[[nodiscard]] auto env_value(const char* name) -> std::string_view {
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return {};
    }
    return {raw};
}

[[nodiscard]] auto env_flag(const char* name) -> bool { return !env_value(name).empty(); }

[[nodiscard]] auto env_int(const char* name, int fallback) -> int {
    const std::string_view raw = env_value(name);
    if (raw.empty()) {
        return fallback;
    }
    int parsed = fallback;
    // from_chars 需要 [begin, end) 指针区间，末指针只能由 data() + size() 求得，属必要指针算术。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char* last = raw.data() + raw.size();
    // from_chars 不抛异常、不依赖 errno，转换失败时保留 fallback。
    // raw 为 std::string_view，data() 不保证 null 结尾，但已用 size() 推出的 end 界定读取区间，
    // from_chars 仅访问 [begin, end)，不存在越界。
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto [end, ec] = std::from_chars(raw.data(), last, parsed);
    return (ec == std::errc{} && end == last) ? parsed : fallback;
}

}  // namespace

AURORA_TEST_CASE(render_to_png_writes_decodable_output) {
    Node root{Column{
        Text{LocalizedString{"Hello, Aurora"}},
        Text{LocalizedString{"Pixel golden test"}},
    }};

    const std::filesystem::path out = std::filesystem::path(testing::isolation::temp_dir()) / "offscreen.png";
    const auto written = render_to_png(root, 240, 120, out.string().c_str());
    AURORA_TEST_REQUIRE_TRUE(written.ok());

    const auto decoded = Image::load(out.string());
    AURORA_TEST_REQUIRE_TRUE(decoded.ok());
    AURORA_TEST_CHECK_EQ(decoded.value().width, 240);
    AURORA_TEST_CHECK_EQ(decoded.value().height, 120);
    AURORA_TEST_CHECK_EQ(decoded.value().pixels.size(), 240U * 120U * 4U);
}

AURORA_TEST_CASE(render_to_logical_snapshot_describes_tree) {
    Node root{Column{
        Text{LocalizedString{"a"}},
        Text{LocalizedString{"b"}},
    }};
    const Json snapshot = render_to_logical_snapshot(root, 100, 60);

    AURORA_TEST_CHECK_EQ(snapshot["type"].get<std::string>(), std::string{"Column"});
    AURORA_TEST_REQUIRE_TRUE(snapshot.contains("box"));
    AURORA_TEST_CHECK_TRUE(snapshot["box"].contains("x"));
    AURORA_TEST_CHECK_TRUE(snapshot["box"].contains("y"));
    AURORA_TEST_CHECK_TRUE(snapshot["box"].contains("w"));
    AURORA_TEST_CHECK_TRUE(snapshot["box"].contains("h"));
    AURORA_TEST_CHECK_EQ(snapshot["children"].size(), 2U);
    AURORA_TEST_CHECK_EQ(snapshot["children"][0]["type"].get<std::string>(), std::string{"Text"});
}

AURORA_TEST_CASE(logical_snapshot_is_deterministic) {
    // 布局是纯函数（mount → layout）：同输入必得同输出，是 golden 比对成立的前提。
    Node first{Column{Text{LocalizedString{"x"}}}};
    Node second{Column{Text{LocalizedString{"x"}}}};
    AURORA_TEST_CHECK_EQ(render_to_logical_snapshot(first, 100, 60).dump(),
                         render_to_logical_snapshot(second, 100, 60).dump());
}

AURORA_TEST_CASE(logical_snapshot_children_stay_within_parent) {
    Node root{Column{
        Text{LocalizedString{"a"}},
        Text{LocalizedString{"b"}},
    }};
    const Json snapshot = render_to_logical_snapshot(root, 100, 60);

    const float parent_w = snapshot["box"]["w"].get<float>();
    const float parent_h = snapshot["box"]["h"].get<float>();
    for (const Json& child : snapshot["children"]) {
        AURORA_TEST_TRACE(std::string{"child "} + child["type"].get<std::string>());
        AURORA_TEST_CHECK_LE(child["box"]["w"].get<float>(), parent_w + 0.001F);
        AURORA_TEST_CHECK_LE(child["box"]["h"].get<float>(), parent_h + 0.001F);
    }
}

AURORA_TEST_CASE(logical_snapshots_match_golden_baseline) {
    constexpr int view_w = 320;
    constexpr int view_h = 240;
    const std::filesystem::path path = golden_dir() / "logical_snapshots.json";
    const bool regen = env_flag("AURORA_UPDATE_GOLDEN");

    Json baseline = Json::object();
    if (!regen) {
        std::ifstream in(path);
        AURORA_TEST_REQUIRE_MSG(in.good(),
                                "golden baseline logical_snapshots.json must exist "
                                "(run with AURORA_UPDATE_GOLDEN=1 to create)");
        in >> baseline;
        AURORA_TEST_REQUIRE_TRUE(baseline.contains("scenarios"));
    }

    Json out = Json::object();
    for (const Scenario& sc : scenarios()) {
        AURORA_TEST_TRACE(std::string{"scenario "} + sc.name);
        Node root = sc.build();
        Json snap = render_to_logical_snapshot(root, view_w, view_h);
        out[sc.name] = snap;

        if (regen) {
            continue;
        }
        AURORA_TEST_REQUIRE_TRUE(baseline["scenarios"].contains(sc.name));
        AURORA_TEST_CHECK_MSG(baseline["scenarios"][sc.name] == snap,
                              std::string{"logical snapshot drift: "} + sc.name);
    }

    if (regen) {
        Json doc = Json::object();
        doc["_about"] =
            "Aurora logical-snapshot golden baseline (requirement #15). Do not hand-edit; regenerate with "
            "AURORA_UPDATE_GOLDEN=1 via aurora_test_runner --run=utest_offscreen.";
        doc["viewport"] = Json{{"w", view_w}, {"h", view_h}};
        doc["scenarios"] = out;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path);
        f << doc.dump(2) << "\n";
        AURORA_TEST_CHECK_TRUE(f.good());
    }
}

AURORA_TEST_CASE(pixel_snapshot_matches_golden_baseline) {
    constexpr int w = 240;
    constexpr int h = 120;
    const std::filesystem::path dir = golden_dir();
    const std::filesystem::path golden_path = dir / "golden_basic_column.png";
    const std::filesystem::path current_path =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_render.png";

    Node root{Column{
        Text{LocalizedString{"Hello, Aurora"}},
        Text{LocalizedString{"Pixel golden test"}},
    }};
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, w, h, current_path.string().c_str()).ok());

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
    AURORA_TEST_REQUIRE_MSG(golden.ok(),
                            "golden_basic_column.png missing or undecodable "
                            "(run with AURORA_UPDATE_GOLDEN=1 to regenerate)");
    AURORA_TEST_REQUIRE_EQ(golden.value().width, w);
    AURORA_TEST_REQUIRE_EQ(golden.value().height, h);

    // 默认严格：逐像素零容差；容差仅用于吸收抗锯齿/字体 hinting 的跨平台抖动。
    const int tolerance = env_int("AURORA_GOLDEN_MAX_DIFF", 0);
    const int max_pixels = env_int("AURORA_GOLDEN_MAX_PIXELS", 0);

    const SnapshotDiff diff = compare_snapshots(golden.value(), current.value(), tolerance);
    // 两侧已显式同型比较；tidy 对含显式 cast 的操作数仍误报混比（C++20 无 ssize 转换惯用法）。
    // NOLINTNEXTLINE(modernize-use-integer-sign-comparison)
    const bool within_budget = diff.pixel_diff_count <= static_cast<std::size_t>(std::max(0, max_pixels));
    AURORA_TEST_CHECK_MSG(within_budget, "pixel drift vs golden: " + std::to_string(diff.pixel_diff_count) +
                                             " px, max delta " + std::to_string(diff.max_color_delta));
}

}  // namespace aurora::test_cases::utest_offscreen
