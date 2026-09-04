// test_percent_layout.cpp — 百分比尺寸布局测试。

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "test_harness.h"

using aurora::Column;
using aurora::ColumnProps;
using aurora::Json;
using aurora::Node;
using aurora::Text;

// ---------- 百分比宽度 ----------

static void test_percent_width() {
    // 50% 宽度子项在 800px 父容器中布局为 400px
    auto txt = Text("Hello");
    txt.width(au::percent(0.5F));

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() == 400.0F);
}

static void test_percent_height() {
    // 25% 高度在 600px 视口中 = 150px
    auto txt = Text("Hi");
    txt.height(au::percent(0.25F));

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["h"].get<float>() == 150.0F);
}

static void test_percent_both() {
    // 50% x 50% 在 800x600 = 400x300
    auto txt = Text("Box");
    txt.width(au::percent(0.5F));
    txt.height(au::percent(0.5F));

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() == 400.0F);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["h"].get<float>() == 300.0F);
}

// ---------- 100% = 填满 ----------

static void test_percent_full() {
    auto txt = Text("Full");
    txt.width(au::percent(1.0F));
    txt.height(au::percent(1.0F));

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 640, 480);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() == 640.0F);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["h"].get<float>() == 480.0F);
}

// ---------- Expand (fill) ----------

static void test_expand_width() {
    auto txt = Text("Fill");
    txt.width(au::fill());

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() == 800.0F);
}

// ---------- 嵌套百分比 ----------

static void test_nested_percent() {
    // Column 占 100%，内部 Text 占 50%
    auto txt = Text("Inner");
    txt.width(au::percent(0.5F));

    auto col = Column(ColumnProps{.children = {std::move(txt)}});
    col.width(au::percent(1.0F));

    Node root(std::move(col));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    // Column 应为 800px 宽
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() == 800.0F);
    // 内部 Text 应为 400px（50% of 800）
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["children"][0]["box"]["w"].get<float>() == 400.0F);
}

// ---------- 小比例 ----------

static void test_small_percent() {
    auto txt = Text("Tiny");
    txt.width(au::percent(0.1F));  // 10% of 800 = 80

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() == 80.0F);
}

AURORA_TEST() {
    test_percent_width();
    test_percent_height();
    test_percent_both();
    test_percent_full();
    test_expand_width();
    test_nested_percent();
    test_small_percent();
}