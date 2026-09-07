/// 测试类型: unit
/// 目标单元: include/aurora/render/offscreen.h
/// 测试说明: golden 逻辑快照比对套件（render_to_logical_snapshot，需求 #15）
///

// 多控件 / 多布局的平台无关逻辑快照基准比对：每个场景经 render_to_logical_snapshot
// 产出 JSON 盒模型树，与 tests/golden/logical_snapshots.json 基准逐场景深度比对。
// 布局是纯函数（mount → layout），同输入必得同输出；场景全部使用显式尺寸意图
// （px()/fill()），不依赖字体度量，保证跨平台逐值一致。
//
// 基准有意变更（布局算法调整）时：设环境变量 AURORA_UPDATE_GOLDEN=1 重跑本用例，
// 基准文件将被重写（用例仍通过），审阅 diff 后提交新基准。

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_golden_snapshots {

using au::Column;
using au::Grid;
using au::GridProps;
using au::Json;
using au::Modifier;
using au::Node;
using au::Row;
using au::Scroll;
using au::Stack;
using au::Text;

namespace {

// 固定尺寸文本盒：盒模型完全由 px() 决定，与字体度量无关，保证跨平台确定性。
auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(au::px(w));
    t->height(au::px(h));
    return Node{t};
}

// 纵向填满剩余空间的文本盒（flex 分配场景用）。
auto fill_box(float w) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(au::px(w));
    t->height(au::fill());
    return Node{t};
}

// 横向填满剩余空间的文本盒（行内 flex 分配场景用）。
auto hfill_box(float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(au::fill());
    t->height(au::px(h));
    return Node{t};
}

struct Scenario {
    const char *name;
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

// 在候选相对路径中定位 golden 目录（兼容 ctest 仓库根 CWD 与手工 build/ 目录直跑）。
auto golden_path() -> std::filesystem::path {
    for (const char *cand : {"tests/golden", "../tests/golden", "../../tests/golden"}) {
        auto p = std::filesystem::path(cand) / "logical_snapshots.json";
        if (std::filesystem::exists(p) || std::filesystem::exists(p.parent_path())) {
            return p;
        }
    }
    return {"tests/golden/logical_snapshots.json"};
}

}  // namespace

AURORA_TEST() {
    constexpr int view_w = 320;
    constexpr int view_h = 240;

    const bool regen = std::getenv("AURORA_UPDATE_GOLDEN") != nullptr;
    const auto path = golden_path();

    Json baseline = Json::object();
    if (!regen) {
        std::ifstream in(path);
        AURORA_TEST_CHECK_MSG(in.good(),
                              "golden baseline logical_snapshots.json must exist "
                              "(run with AURORA_UPDATE_GOLDEN=1 to create)");
        if (in.good()) {
            in >> baseline;
        }
    }

    Json out = Json::object();
    int failures = 0;
    for (const Scenario &sc : scenarios()) {
        Node root = sc.build();
        Json snap = render_to_logical_snapshot(root, view_w, view_h);
        out[sc.name] = snap;

        if (regen) {
            continue;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（json [] 会插入键）
        if (!baseline.contains("scenarios") || !baseline["scenarios"].contains(sc.name)) {
            AURORA_TEST_PRINTF_ERR("[golden] scenario %s missing from baseline\n", sc.name);
            ++failures;
            continue;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json &expected = baseline["scenarios"][sc.name];
        if (expected != snap) {
            AURORA_TEST_PRINTF_ERR("[golden] scenario %s drift:\n  expected: %s\n  actual:   %s\n", sc.name,
                                   expected.dump().c_str(), snap.dump().c_str());
            ++failures;
        }
    }

    if (regen) {
        Json doc = Json::object();
        doc["_about"] =
            "Aurora logical-snapshot golden baseline (requirement #15). Do not hand-edit; regenerate with "
            "AURORA_UPDATE_GOLDEN=1 via aurora_test_runner --run=utest_golden_snapshots.";
        doc["viewport"] = Json{{"w", view_w}, {"h", view_h}};
        doc["scenarios"] = out;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path);
        f << doc.dump(2) << "\n";
        AURORA_TEST_PRINTF("[golden] baseline (re)written: %s\n", path.string().c_str());
        AURORA_TEST_CHECK(f.good());
        return;
    }

    if (failures == 0) {
        AURORA_TEST_PRINTF("[golden] all %zu logical snapshots match baseline\n", scenarios().size());
    }
    AURORA_TEST_CHECK_MSG(failures == 0, "golden logical snapshots match baseline (see drift details above)");
}

}  // namespace aurora::test_cases::utest_golden_snapshots
