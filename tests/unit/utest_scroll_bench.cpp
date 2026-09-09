/// 测试类型: unit
/// 目标单元: include/aurora/perf/scroll_bench.h
/// 测试说明: 覆盖 ScrollBenchHarness——Config 默认配置、Result 默认值与自证派生函数
/// （trustworthy/geometry_stable/content_screens/reversal_ratio）、汇总读数转发单一数据源、
/// run 对非法输入（空树/非正视口）的拒绝路径、结果序列化格式，以及最小离线基准
/// （HeadlessSurface + 固定尺寸内容树 + 程序化滚动，帧数压到 2 帧做快速端到端自证）。

#include <cstddef>
#include <memory>
#include <string>

#include "aurora/perf/scroll_bench.h"
#include "aurora/render/painter.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scroll_bench {

namespace {

/// @brief 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char* override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter& /*p*/, const Rect& /*bounds*/, const BuildContext& /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

/// @brief 内容高 1000dp、视口 200dp 的可滚动树（行程 800dp，5 屏内容）。
auto make_scrollable_tree() -> Node {
    ScrollProps props;
    props.child = Node{std::make_shared<FixedBox>(200.0F, 1000.0F)};
    return Node{Scroll{props}};
}

/// @brief 统计字符出现次数（CSV 列数 = 逗号数 + 1）。
auto count_of(const std::string& text, char ch) -> std::size_t {
    std::size_t n = 0;
    for (const char c : text) {
        if (c == ch) {
            ++n;
        }
    }
    return n;
}

}  // namespace

AURORA_TEST_CASE(config_defaults) {
    // 采样配置默认值：匀速 12dp/帧、warmup 30 + 采样 300、落定墙钟 1500ms。
    const ScrollBenchHarness::Config cfg;
    AURORA_TEST_CHECK_EQ(cfg.frames, 300);
    AURORA_TEST_CHECK_NEAR(cfg.delta_per_frame, 12.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(cfg.fling);
    AURORA_TEST_CHECK_EQ(cfg.warmup_frames, 30);
    AURORA_TEST_CHECK_NEAR(cfg.scale, 1.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(cfg.auto_reverse);
    AURORA_TEST_CHECK_NEAR(cfg.frame_budget_ms, 16.67, 1e-9);
    AURORA_TEST_CHECK_STREQ(cfg.name.c_str(), "scroll");
    AURORA_TEST_CHECK_NEAR(cfg.settle_ms, 1500.0, 1e-9);
    AURORA_TEST_CHECK_EQ(cfg.settle_idle_frames, 24);
    AURORA_TEST_CHECK_EQ(cfg.settle_max_frames, 4000);
    AURORA_TEST_CHECK_NEAR(cfg.fling_boost, 4.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(cfg.fling_decay, 0.94F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(cfg.fling_cutoff, 0.5F, 1e-4F);
}

AURORA_TEST_CASE(result_defaults_are_untrusted) {
    // 默认 Result：全部自证字段为否 —— 读数不可信（防「测了个寂寞」的缺省安全态）。
    const ScrollBenchHarness::Result r{};
    AURORA_TEST_CHECK_FALSE(r.scrollable_found);
    AURORA_TEST_CHECK_EQ(r.moved_frames, std::size_t{0});
    AURORA_TEST_CHECK_EQ(r.idle_frames, std::size_t{0});
    AURORA_TEST_CHECK_FALSE(r.settled);
    AURORA_TEST_CHECK_EQ(r.settle_reason, ScrollBenchHarness::Result::SettleReason::FrameCap);
    AURORA_TEST_CHECK_EQ(r.report.frame_count, std::size_t{0});

    // 派生函数在默认值下安全回退：几何视为稳定、0 屏内容、0 反向占比、不可信。
    AURORA_TEST_CHECK_TRUE(r.geometry_stable());
    AURORA_TEST_CHECK_NEAR(r.content_screens(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(r.reversal_ratio(), 0.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(r.trustworthy());
    AURORA_TEST_CHECK_NEAR(ScrollBenchHarness::Result::kMaxReversalRatio, 0.10, 1e-9);
}

AURORA_TEST_CASE(derived_readers_forward_to_report) {
    // 汇总读数转发 report（单一数据源，转发访问器不另存副本）。
    ScrollBenchHarness::Result r{};
    r.report.avg_frame_ms = 5.0;
    r.report.p99_ms = 9.0;
    r.report.jitter_ms = 1.5;
    r.report.worst_ms = 12.0;
    r.report.long_task_count = 2;
    r.report.full_redraw_frames = 3;
    r.report.frame_count = 4;
    r.reversals = 1;

    AURORA_TEST_CHECK_NEAR(r.avg_frame_ms(), 5.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.p99_ms(), 9.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.jitter_ms(), 1.5, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.worst_ms(), 12.0, 1e-9);
    AURORA_TEST_CHECK_EQ(r.long_task_count(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(r.full_redraw_frames(), std::size_t{3});
    AURORA_TEST_CHECK_NEAR(r.reversal_ratio(), 0.25, 1e-9);
}

AURORA_TEST_CASE(run_rejects_empty_root) {
    // 空树：提前返回默认 Result，scrollable_found = false，由 trustworthy() 识别。
    const ScrollBenchHarness::Result r = ScrollBenchHarness::run(Node{}, Size{.width = 200.0F, .height = 200.0F});
    AURORA_TEST_CHECK_FALSE(r.scrollable_found);
    AURORA_TEST_CHECK_FALSE(r.trustworthy());
    AURORA_TEST_CHECK_FALSE(r.settled);
    AURORA_TEST_CHECK_EQ(r.report.frame_count, std::size_t{0});
    AURORA_TEST_CHECK_EQ(r.moved_frames, std::size_t{0});
}

AURORA_TEST_CASE(run_rejects_nonpositive_viewport) {
    // 非正视口（0 宽/负高）：同样走拒绝路径，不触碰渲染流程。
    const ScrollBenchHarness::Result zero_w =
        ScrollBenchHarness::run(make_scrollable_tree(), Size{.width = 0.0F, .height = 200.0F});
    AURORA_TEST_CHECK_FALSE(zero_w.scrollable_found);
    AURORA_TEST_CHECK_FALSE(zero_w.trustworthy());

    const ScrollBenchHarness::Result negative_h =
        ScrollBenchHarness::run(make_scrollable_tree(), Size{.width = 200.0F, .height = -1.0F});
    AURORA_TEST_CHECK_FALSE(negative_h.scrollable_found);
    AURORA_TEST_CHECK_FALSE(negative_h.trustworthy());
}

AURORA_TEST_CASE(run_measures_scrollable_tree_offline) {
    // 最小离线端到端：Headless + 程序化滚动，2 帧采样（warmup 0、落定关闭）。
    ScrollBenchHarness::Config cfg;
    cfg.frames = 2;
    cfg.warmup_frames = 0;
    cfg.settle_ms = 0.0;  // 显式关闭落定：settle_reason = Disabled 且 settled = true
    cfg.name = "utest-scroll";

    const ScrollBenchHarness::Result r =
        ScrollBenchHarness::run(make_scrollable_tree(), Size{.width = 200.0F, .height = 200.0F}, cfg);

    // 定位与落定自证：树里有 Scroll、落定阶段显式关闭视为正常。
    AURORA_TEST_CHECK_TRUE(r.scrollable_found);
    AURORA_TEST_CHECK_TRUE(r.settled);
    AURORA_TEST_CHECK_EQ(r.settle_reason, ScrollBenchHarness::Result::SettleReason::Disabled);

    // 行程自证：内容 1000 - 视口 200 = 800dp；步长标定 = Scroll::step 16dp/单位。
    AURORA_TEST_CHECK_NEAR(r.max_offset, 800.0F, 0.5F);
    AURORA_TEST_CHECK_TRUE(r.geometry_stable());
    AURORA_TEST_CHECK_NEAR(r.max_offset_end, 800.0F, 0.5F);
    AURORA_TEST_CHECK_GT(r.dp_per_unit, 0.0F);

    // 采样自证：2 帧全部真实滚动、无 idle 跳帧，读数可信。
    AURORA_TEST_CHECK_EQ(r.report.frame_count, std::size_t{2});
    AURORA_TEST_CHECK_EQ(r.moved_frames, std::size_t{2});
    AURORA_TEST_CHECK_EQ(r.idle_frames, std::size_t{0});
    AURORA_TEST_CHECK_TRUE(r.trustworthy());
    // 每帧 12dp × 2 帧 = 24dp 累计位移。
    AURORA_TEST_CHECK_NEAR(r.scrolled_px, 24.0, 0.5);
}

AURORA_TEST_CASE(result_serialization_shapes) {
    // 序列化格式：JSON 含自证字段；CSV 行列数与表头严格对应；Markdown 含自证表。
    const ScrollBenchHarness::Result r{};

    const std::string markdown = r.to_markdown();
    AURORA_TEST_CHECK_FALSE(markdown.empty());
    AURORA_TEST_CHECK_THAT(markdown, ::aurora::testing::matchers::has_substr("trustworthy"));
    AURORA_TEST_CHECK_THAT(markdown, ::aurora::testing::matchers::has_substr("scrollable found"));

    const std::string json = r.to_json();
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::starts_with("{"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("scrollable_found":false)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("settled":false)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("trustworthy":false)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("report":)"));

    const std::string header = ScrollBenchHarness::Result::csv_header();
    const std::string row = r.to_csv_row();
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::starts_with(PerfReport::csv_header()));
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::has_substr("scrollable_found"));
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::ends_with("trustworthy"));
    AURORA_TEST_CHECK_EQ(count_of(header, ','), count_of(row, ','));
}

}  // namespace aurora::test_cases::utest_scroll_bench
