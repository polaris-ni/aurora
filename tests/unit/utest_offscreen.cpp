/// 测试类型: unit
/// 目标单元: include/aurora/render/offscreen.h
/// 测试说明: 覆盖无头渲染两条产线——render_to_png 写出可解码 PNG、render_to_logical_snapshot 的
/// 平台无关盒模型树结构与确定性；并回归两类 golden 基准：像素基准（golden_basic_column.png，
/// 受 AURORA_GOLDEN_DIR / MAX_DIFF / MAX_PIXELS / UPDATE_GOLDEN 控制）与
/// 逻辑快照基准（logical_snapshots.json，11 场景逐值比对）

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"
#include "framework/json_access.h"
#include "framework/json_literals.h"

namespace aurora::test_cases::utest_offscreen {
using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

namespace golden = aurora::testing::golden;

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
        // WebView 快照模式：无宿主/无引擎时它就是一个 320×240 的普通占位盒——这条钉的是
        // 「双模里帧流那一半」的**结构**面（盒模型与兄弟控件一样参与布局），像素面另有专门用例。
        {.name = "webview_snapshot",
         .build = []() -> Node {
             auto col = std::make_shared<Column>();
             col->add(Node{std::make_shared<webview::WebView>(
                 webview::WebViewProps{.mode = webview::WebViewMode::Snapshot})});
             col->add(box(100.0F, 20.0F));
             return Node{col};
         }},
    };
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

    AURORA_TEST_CHECK_EQ(require_field<std::string>(snapshot, "type"), std::string{"Column"});
    AURORA_TEST_REQUIRE_TRUE(snapshot.contains("box"));
    const auto &root_box = *require_child(snapshot, "box");
    AURORA_TEST_CHECK_TRUE(root_box.contains("x"));
    AURORA_TEST_CHECK_TRUE(root_box.contains("y"));
    AURORA_TEST_CHECK_TRUE(root_box.contains("w"));
    AURORA_TEST_CHECK_TRUE(root_box.contains("h"));
    AURORA_TEST_CHECK_EQ(require_child(snapshot, "children")->size(), 2U);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(*require_child(snapshot, "children"), 0), "type"),
                         std::string{"Text"});
}

AURORA_TEST_CASE(logical_snapshot_is_deterministic) {
    // 布局是纯函数（mount → layout）：同输入必得同输出，是 golden 比对成立的前提。
    Node first{Column{Text{LocalizedString{"x"}}}};
    Node second{Column{Text{LocalizedString{"x"}}}};
    const auto first_dump = json::dump(render_to_logical_snapshot(first, 100, 60));
    const auto second_dump = json::dump(render_to_logical_snapshot(second, 100, 60));
    AURORA_TEST_REQUIRE_TRUE(first_dump.ok());
    AURORA_TEST_REQUIRE_TRUE(second_dump.ok());
    AURORA_TEST_CHECK_EQ(first_dump.value(), second_dump.value());
}

AURORA_TEST_CASE(logical_snapshot_children_stay_within_parent) {
    Node root{Column{
        Text{LocalizedString{"a"}},
        Text{LocalizedString{"b"}},
    }};
    const Json snapshot = render_to_logical_snapshot(root, 100, 60);

    const auto &root_box = *require_child(snapshot, "box");
    const auto parent_w = require_field<float>(root_box, "w");
    const auto parent_h = require_field<float>(root_box, "h");
    const auto &children = *require_child(snapshot, "children");
    for (const auto &child : children) {
        AURORA_TEST_TRACE(std::string{"child "} + require_field<std::string>(child, "type"));
        const auto &child_box = *require_child(child, "box");
        AURORA_TEST_CHECK_LE(require_field<float>(child_box, "w"), parent_w + 0.001F);
        AURORA_TEST_CHECK_LE(require_field<float>(child_box, "h"), parent_h + 0.001F);
    }
}

AURORA_TEST_CASE(logical_snapshots_match_golden_baseline) {
    constexpr int view_w = 320;
    constexpr int view_h = 240;
    const std::filesystem::path path = golden::dir() / "logical_snapshots.json";
    const bool regen = golden::env_flag("AURORA_UPDATE_GOLDEN");

    Json baseline = Json::object();
    if (!regen) {
        std::ifstream in(path);
        AURORA_TEST_REQUIRE_MSG(in.good(),
                                "golden baseline logical_snapshots.json must exist "
                                "(run with AURORA_UPDATE_GOLDEN=1 to create)");
        const std::string text{(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
        const auto parsed = json::parse(text);
        AURORA_TEST_REQUIRE_TRUE(parsed.ok());
        baseline = parsed.value();
        AURORA_TEST_REQUIRE_TRUE(baseline.contains("scenarios"));
        // 色彩管理注记：golden 基准唯一色彩空间为 sRGB（软件参考路径 SSOT，逐位确定性红线）。
        // 基线尚无此字段时跳过（向后兼容）；再生成路径会写入。
        if (baseline.contains("colorspace")) {
            AURORA_TEST_CHECK_MSG(baseline.as_or<std::string>("colorspace", "") == std::string{"sRGB"},
                                  "golden baseline colorspace must be sRGB");
        }
    }

    Json out = Json::object();
    for (const Scenario &sc : scenarios()) {
        AURORA_TEST_TRACE(std::string{"scenario "} + sc.name);
        Node root = sc.build();
        Json snap = render_to_logical_snapshot(root, view_w, view_h);
        out.set(sc.name, snap);

        if (regen) {
            continue;
        }
        const auto *scenarios = require_child(baseline, "scenarios");
        AURORA_TEST_REQUIRE_TRUE(scenarios->contains(sc.name));
        AURORA_TEST_CHECK_MSG(*require_child(*scenarios, sc.name) == snap,
                              std::string{"logical snapshot drift: "} + sc.name);
    }

    if (regen) {
        Json doc = Json::object();
        doc.set("_about",
                "Aurora logical-snapshot golden baseline (requirement SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001). Do not "
                "hand-edit; regenerate with "
                "AURORA_UPDATE_GOLDEN=1 via aurora_test_runner --run=utest_offscreen.");
        doc.set("colorspace", "sRGB");  // 注记：golden 基准唯一色彩空间（软件参考路径 SSOT）
        doc.set("viewport", testing::json_obj({{"w", view_w}, {"h", view_h}}));
        doc.set("scenarios", out);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path);
        const auto text = json::dump(doc, {.indent = 2});
        AURORA_TEST_REQUIRE_TRUE(text.ok());
        f << text.value() << "\n";
        AURORA_TEST_CHECK_TRUE(f.good());
    }
}

AURORA_TEST_CASE(pixel_snapshot_matches_golden_baseline) {
    constexpr int w = 240;
    constexpr int h = 120;
    const std::filesystem::path current_path =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_render.png";

    Node root{Column{
        Text{LocalizedString{"Hello, Aurora"}},
        Text{LocalizedString{"Pixel golden test"}},
    }};
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, w, h, current_path.string().c_str()).ok());

    // 重生成模式下基线可能尚不存在，故先跑更新分支再回读做尺寸校验。
    if (golden::env_flag("AURORA_UPDATE_GOLDEN")) {
        golden::compare_or_update("golden_basic_column", current_path);
        return;
    }

    const auto golden = Image::load((golden::dir() / "golden_basic_column.png").string());
    AURORA_TEST_REQUIRE_MSG(golden.ok(),
                            "golden_basic_column.png missing or undecodable "
                            "(run with AURORA_UPDATE_GOLDEN=1 to regenerate)");
    AURORA_TEST_REQUIRE_EQ(golden.value().width, w);
    AURORA_TEST_REQUIRE_EQ(golden.value().height, h);

    // 默认严格：逐像素零容差；容差仅用于吸收抗锯齿/字体 hinting 的跨平台抖动。
    // 传入 root 以启用归因：失败时报告会说清差异落在哪个控件的盒子里。
    golden::compare_or_update("golden_basic_column", current_path, &root);
}

// ---- WebView 快照模式：像素真的经引擎流进 Painter 帧流（判据 3 的像素面） ----

namespace {

/// @brief 一张确定性的「假页面」：与控件盒同尺寸（320×240），故 `draw_image` 是 1:1 blit，
///        不引入任何缩放滤波 ⇒ 像素逐位可复现。左半纯红、右半纯绿、顶三行纯白、底三行纯蓝。
/// @return 320×240 RGBA8 位图。
auto stub_page_image() -> Image {
    constexpr int page_w = 320;
    constexpr int page_h = 240;
    Image img;
    img.width = page_w;
    img.height = page_h;
    img.pixels.assign(static_cast<std::size_t>(page_w) * static_cast<std::size_t>(page_h) * 4U, 0U);
    for (int y = 0; y < page_h; ++y) {
        for (int x = 0; x < page_w; ++x) {
            std::uint8_t r = 0;
            std::uint8_t g = 0;
            std::uint8_t b = 0;
            if (y < 3) {
                r = g = b = 255;  // 顶带：白
            } else if (y >= page_h - 3) {
                b = 255;  // 底带：蓝
            } else if (x < page_w / 2) {
                r = 255;  // 左半：红
            } else {
                g = 255;  // 右半：绿
            }
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(page_w) +
                                    static_cast<std::size_t>(x)) *
                                   4U;
            img.pixels[at] = r;
            img.pixels[at + 1] = g;
            img.pixels[at + 2] = b;
            img.pixels[at + 3] = 255;
        }
    }
    return img;
}

/// @brief 只回确定性假页面的快照桩视图。
class StubPageSink final : public webview::WebViewSink {
  public:
    auto set_native_rect(const webview::NativeViewRect & /*rect*/) -> void override {}
    auto set_native_visible(bool /*visible*/) -> void override {}
    auto capture(std::function<void(Image)> done) -> void override {
        if (done) {
            done(stub_page_image());
        }
    }
    auto detach() -> void override {}

    // 内容通道在本套件不参与判据（几何/像素另有专测），按契约留空实现。
    auto navigate(const std::string & /*uri*/) -> void override {}
    auto navigate_to_string(const std::string & /*html*/, const std::string & /*base_uri*/) -> void override {}
    auto reload() -> void override {}
    auto stop() -> void override {}
    auto go_back() -> void override {}
    auto go_forward() -> void override {}
    auto set_zoom_factor(float /*zoom*/) -> void override {}
    auto set_javascript_enabled(bool /*enabled*/) -> void override {}
    auto evaluate_script(const std::string & /*js*/, std::function<void(std::string)> /*done*/) -> void override {}
    auto post_message(const std::string & /*json*/) -> void override {}
    auto set_callbacks(std::weak_ptr<webview::WebViewCallbacks> /*callbacks*/) -> void override {}
};

/// @brief 只提供快照能力的桩引擎（交互恒不可用 ⇒ 视图不会被授予原生可见）。
class StubPageEngine final : public webview::NativeViewEngine {
  public:
    [[nodiscard]] auto id() const -> webview::WebViewEngine override { return webview::WebViewEngine::Null; }
    [[nodiscard]] auto probe() const -> webview::WebViewCapabilities override {
        return webview::WebViewCapabilities{.interactive = false,
                                           .snapshot = true,
                                           .engine = webview::WebViewEngine::Null,
                                           .availability = webview::WebViewAvailability::Available};
    }
    [[nodiscard]] auto create_view(void * /*parent_window*/) -> std::unique_ptr<webview::WebViewSink> override {
        return std::make_unique<StubPageSink>();
    }
};

/// @brief 宿主桩窗口句柄（只作非空判据，桩引擎从不解引用）。
/// @return 指向进程内静态存储的地址。
auto stub_host_window() -> void * {
    static int handle = 0;
    return &handle;
}

}  // namespace

AURORA_TEST_CASE(webview_snapshot_blits_into_the_painter_frame_stream) {
    // 判据 3：快照模式必须**在帧流里**——像素来自引擎 `capture`、由 `on_paint` 画进 Painter，
    // 因而可被逐像素 golden 比对（交互模式的原生视图恰恰做不到这件事，它不在 Painter 里）。
    //
    // 独立基准纪律：golden 文件本身可以空转通过（控件什么都没画，基线也就什么都没画），故先用
    // **手算的具体像素颜色**钉住「画出来的确实是那张假页面」，再比 golden。
    webview::install_engine(std::make_shared<StubPageEngine>());
    webview::NativeViewHost host;
    host.bind_window(stub_host_window(), []() -> float { return 1.0F; }, []() -> EdgeInsets { return EdgeInsets{}; });
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>(
        webview::WebViewProps{.mode = webview::WebViewMode::Snapshot, .background = Color{9, 9, 9, 255}});
    auto col = std::make_shared<Column>();
    col->add(Node{std::static_pointer_cast<Widget>(wv)});

    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = 320.0F, .height = 240.0F};
    LayoutEngine::layout(*col, c);
    AURORA_TEST_REQUIRE_FALSE(wv->degraded());

    wv->request_snapshot({});
    AURORA_TEST_REQUIRE_EQ(wv->snapshot_bitmap().pixels.size(), 320U * 240U * 4U);

    Painter p;
    p.begin(320, 240);
    col->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 320.0F, .height = 240.0F}},
               BuildContext{});

    // 手算取样：四象限 + 顶/底带各一点，颜色必须来自那张假页面。
    AURORA_TEST_CHECK_TRUE(p.get_pixel(10, 1) == Color(255, 255, 255, 255));  // 顶带白
    AURORA_TEST_CHECK_TRUE(p.get_pixel(10, 120) == Color(255, 0, 0, 255));    // 左半红
    AURORA_TEST_CHECK_TRUE(p.get_pixel(300, 120) == Color(0, 255, 0, 255));   // 右半绿
    AURORA_TEST_CHECK_TRUE(p.get_pixel(10, 238) == Color(0, 0, 255, 255));    // 底带蓝
    // 反向守卫：占位底色一次都不许出现（出现即说明画的仍是降级占位而非快照）。
    AURORA_TEST_CHECK_TRUE(p.get_pixel(160, 120) != Color(9, 9, 9, 255));

    golden::compare_or_update_painter("webview_snapshot", p);

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

}  // namespace aurora::test_cases::utest_offscreen
