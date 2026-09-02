// test_scroll_bench.cpp — `aurora::ScrollBenchHarness` 单测。
//
// 这个 harness 是基准的验收工具，它自己出错的后果比被测代码出错更严重：
// 「测了个寂寞」会给出一组漂亮却无意义的读数，进而让优化验收全盘失真。因此本文件的
// 重点不是性能数字（时间读数天然 flaky，不进 CTest 断言），而是**自证机制本身**：
//
//   1. 纯判据函数（trustworthy / geometry_stable / content_screens / reversal_ratio）
//      —— 手工构造 Result，逐条验证每个子条件都是「load-bearing」的：拿掉任意一条，
//      trustworthy() 必须翻假。这类断言完全确定、零渲染、零耗时。
//   2. 真实 Headless 采样 —— 用几何确定的合成树（N × 固定高 Spacer）跑小规模采样，
//      断言 harness 能定位滚动容器、正确标定 dp/unit、每帧真滚、结果判为可信。
//   3. 反例 —— 内容不足一屏 / 树里没有滚动容器 / 非法输入，必须判为**不可信**。
//   4. 确定性 —— 同一棵树同一份配置跑两次，几何类读数逐位相同（CI 回归锚点的前提）。
//   5. 序列化 —— CSV 表头与数据行列数对齐、JSON 可解析。
//
// 计数器读数（`RenderCounters`）仅在 `AURORA_ENABLE_PROFILING=ON` 的构建下非零，
// 故涉及计数的断言经 `if constexpr (profiling_enabled())` 分流。

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::Column;
using aurora::ColumnProps;
using aurora::Length;
using aurora::Node;
using aurora::PerfReport;
using aurora::profiling_enabled;
using aurora::RenderCounters;
using aurora::Scroll;
using aurora::ScrollBenchHarness;
using aurora::ScrollProps;
using aurora::Size;
using aurora::Spacer;

using Json = nlohmann::json;
using Result_ = ScrollBenchHarness::Result;
using SettleReason = ScrollBenchHarness::Result::SettleReason;

namespace {

/// @brief 每个内容块的固定高（dp）。用 `Length::fixed` 而非文本，几何完全脱离字体度量。
constexpr float AURORA_K_BLOCK_H = 40.0f;

/// @brief 合成可滚动树：`Scroll` 包 `Column`，Column 内 `rows` 个固定高块。
/// @note 用 `Spacer(false)` 作块体：它不绘制、不测字，`height(fixed)` 后尺寸严格可预测。
[[nodiscard]] auto build_scrollable(int rows) -> Node {
    std::vector<Node> items;
    items.reserve(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        auto block = std::make_shared<Spacer>(false);
        block->height(Length::fixed(AURORA_K_BLOCK_H));
        block->width(Length::fixed(200.0f));
        items.emplace_back(std::move(block));
    }
    auto col = std::make_shared<Column>(ColumnProps{ .children = std::move(items) });
    return Node{ std::make_shared<Scroll>(ScrollProps{ .child = Node{ std::move(col) } }) };
}

/// @brief 无滚动容器的树（纯 Column），用于验证 `scrollable_found == false`。
[[nodiscard]] auto build_static_tree() -> Node {
    std::vector<Node> items;
    for (int i = 0; i < 3; ++i) {
        auto block = std::make_shared<Spacer>(false);
        block->height(Length::fixed(AURORA_K_BLOCK_H));
        items.emplace_back(std::move(block));
    }
    return Node{ std::make_shared<Column>(ColumnProps{ .children = std::move(items) }) };
}

/// @brief 单测口径的小规模配置：静态树能秒收敛，不必等默认的 1500ms / 300 帧。
[[nodiscard]] auto small_config(std::string name) -> ScrollBenchHarness::Config {
    ScrollBenchHarness::Config cfg;
    cfg.name = std::move(name);
    cfg.frames = 30;
    cfg.warmup_frames = 5;
    cfg.delta_per_frame = 12.0f;
    cfg.settle_ms = 200.0;
    cfg.settle_idle_frames = 3;
    cfg.settle_max_frames = 400;
    return cfg;
}

/// @brief 造一份「各项都合格」的 Result，供逐条翻假验证 trustworthy() 的每个子条件。
[[nodiscard]] auto make_valid_result() -> Result_ {
    Result_ r;
    r.report.frame_count = 100;
    r.viewport = Size{ .width = 400.0f, .height = 300.0f };
    r.scrollable_found = true;
    r.moved_frames = 100;
    r.idle_frames = 0;
    r.reversals = 0;
    r.scrolled_px = 1200.0;
    r.final_offset = 1200.0f;
    r.max_offset = 3700.0f;
    r.max_offset_end = 3700.0f;
    r.scroll_viewport_h = 300.0f;
    r.dp_per_unit = 16.0f;
    r.settle_frames = 10;
    r.settle_ms = 50.0;
    r.settled = true;
    r.settle_reason = SettleReason::Idle;
    return r;
}

/// @brief 统计 CSV 字段数（本模块字段值不含逗号）。
[[nodiscard]] auto csv_field_count(const std::string &row) -> std::size_t {
    if (row.empty()) {
        return 0;
    }
    std::size_t n = 1;
    for (const char ch : row) {
        if (ch == ',') {
            ++n;
        }
    }
    return n;
}

// =========================================================================
// 一、纯判据函数（确定性，零渲染）
// =========================================================================

// ---- Test 1: geometry_stable —— 采样前后行程差 < 0.5dp ----
auto test_geometry_stable() -> void {
    Result_ r = make_valid_result();

    AURORA_TEST_CHECK_MSG(r.geometry_stable(), "Test1: travel identical before/after -> stable");

    r.max_offset_end = r.max_offset + 0.4f;
    AURORA_TEST_CHECK_MSG(r.geometry_stable(), "Test1: diff 0.4dp (< 0.5 tolerance) still stable");

    r.max_offset_end = r.max_offset + 0.6f;
    AURORA_TEST_CHECK_MSG(!r.geometry_stable(), "Test1: diff 0.6dp judged unstable");

    // 骨架屏中途退场的典型形态：采样后内容变高，行程随之变大。
    r.max_offset = 364.0f;
    r.max_offset_end = 2200.0f;
    AURORA_TEST_CHECK_MSG(!r.geometry_stable(), "Test1: content grew during sampling (skeleton exit) judged unstable");
}

// ---- Test 2: content_screens —— 内容是滚动容器视口的多少倍 ----
auto test_content_screens() -> void {
    Result_ r = make_valid_result();

    r.scroll_viewport_h = 300.0f;
    r.max_offset = 300.0f; // 内容 = 视口 + 行程 = 600 = 2 屏
    AURORA_TEST_CHECK_MSG(near_f(r.content_screens(), 2.0f, 1e-4f),
                          "Test2: travel = one screen -> content 2.00 screens");

    r.max_offset = 0.0f;
    AURORA_TEST_CHECK_MSG(near_f(r.content_screens(), 1.0f, 1e-4f), "Test2: travel 0 -> content exactly 1 screen");

    r.scroll_viewport_h = 0.0f;
    AURORA_TEST_CHECK_MSG(near_f(r.content_screens(), 0.0f, 1e-4f),
                          "Test2: returns 0 when viewport height unknown (0), no division by zero");

    // 用「滚动容器自身视口」而非窗口视口：AppShell 的顶栏/底栏会挤占上百 dp，
    // 用窗口高算会把「内容不足两屏」误判成「够滚」。
    r.scroll_viewport_h = 640.0f;
    r.max_offset = 364.0f;
    r.viewport = Size{ .width = 1100.0f, .height = 760.0f };
    AURORA_TEST_CHECK_MSG(r.content_screens() < 2.0f,
                          "Test2: google_play metric (640dp viewport / 364dp travel) judged under two screens");
}

// ---- Test 3: reversal_ratio ----
auto test_reversal_ratio() -> void {
    Result_ r = make_valid_result();

    r.reversals = 0;
    AURORA_TEST_CHECK_MSG(near_d(r.reversal_ratio(), 0.0, 1e-9), "Test3: no reversal -> ratio 0");

    r.reversals = 5; // frame_count = 100
    AURORA_TEST_CHECK_MSG(near_d(r.reversal_ratio(), 0.05, 1e-9), "Test3: 5/100 → 0.05");

    r.report.frame_count = 0;
    AURORA_TEST_CHECK_MSG(near_d(r.reversal_ratio(), 0.0, 1e-9),
                          "Test3: returns 0 when frame count is 0, no division by zero");
}

// ---- Test 4: trustworthy 的每个子条件都是 load-bearing（逐条翻假）----
auto test_trustworthy_conditions() -> void {
    AURORA_TEST_CHECK_MSG(make_valid_result().trustworthy(), "Test4: all-valid baseline sample judged trustworthy");

    {
        Result_ r = make_valid_result();
        r.scrollable_found = false;
        AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test4: scroll control not located -> untrustworthy");
    }
    {
        Result_ r = make_valid_result();
        r.settled = false;
        AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test4: hit frame cap during settle -> untrustworthy");
    }
    {
        Result_ r = make_valid_result();
        r.report.frame_count = 0;
        r.moved_frames = 0;
        AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test4: zero sampled frames -> untrustworthy");
    }
    {
        Result_ r = make_valid_result();
        r.moved_frames = 99; // 有一帧没滚起来
        AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test4: a sampled frame produced no movement -> untrustworthy");
    }
    {
        Result_ r = make_valid_result();
        r.idle_frames = 1;
        AURORA_TEST_CHECK_MSG(!r.trustworthy(),
                              "Test4: idle frame skip present -> untrustworthy (measured skip, not render)");
    }
    {
        Result_ r = make_valid_result();
        r.max_offset = 0.0f;
        r.max_offset_end = 0.0f;
        AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test4: travel 0 (tree not scrollable at all) -> untrustworthy");
    }
    {
        Result_ r = make_valid_result();
        r.max_offset_end = 2200.0f; // 采样期内容还在长
        AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test4: geometry unstable -> untrustworthy");
    }
    {
        Result_ r = make_valid_result();
        r.reversals = 11; // 11/100 = 11% > 10%
        AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test4: reversal ratio over 10% -> untrustworthy (content too short)");

        r.reversals = 10; // 恰好 10%，边界取闭区间
        AURORA_TEST_CHECK_MSG(r.trustworthy(), "Test4: reversal ratio exactly 10% still trustworthy (closed interval)");
    }
    AURORA_TEST_CHECK_MSG(near_d(Result_::kMaxReversalRatio, 0.10, 1e-9),
                          "Test4: reversal ratio threshold constant is 0.10");
}

// ---- Test 5: SettleReason 中只有 FrameCap 代表失败 ----
auto test_settle_reason_semantics() -> void {
    // settled 与 settle_reason 是两个字段，但语义上一一对应：
    // Disabled / Idle / TimeBudget 都是正常落定，只有 FrameCap 是未落定。
    for (const SettleReason reason : { SettleReason::Disabled, SettleReason::Idle, SettleReason::TimeBudget }) {
        Result_ r = make_valid_result();
        r.settle_reason = reason;
        r.settled = true;
        AURORA_TEST_CHECK_MSG(r.trustworthy(), "Test5: Disabled / Idle / TimeBudget all treated as normal settle");
    }
    Result_ capped = make_valid_result();
    capped.settle_reason = SettleReason::FrameCap;
    capped.settled = false;
    AURORA_TEST_CHECK_MSG(!capped.trustworthy(), "Test5: FrameCap treated as not settled -> untrustworthy");
}

// ---- Test 6: 序列化——CSV 列对齐、JSON 可解析、Markdown 含判定行 ----
auto test_serialization() -> void {
    Result_ r = make_valid_result();
    r.report.name = "unit";

    const std::size_t hn = csv_field_count(Result_::csv_header());
    const std::size_t rn = csv_field_count(r.to_csv_row());
    AURORA_TEST_CHECK_MSG(hn == rn, "Test6: CSV header column count matches data row column count");
    AURORA_TEST_CHECK_MSG(hn > csv_field_count(PerfReport::csv_header()),
                          "Test6: scroll self-check columns appended after PerfReport columns");

    const Json j = Json::parse(r.to_json(), nullptr, false);
    AURORA_TEST_CHECK_MSG(!j.is_discarded(), "Test6: to_json output is parseable");
    if (!j.is_discarded()) {
        AURORA_TEST_CHECK_MSG(j.value("trustworthy", false), "Test6: JSON contains trustworthy verdict");
        AURORA_TEST_CHECK_MSG(j.value("settle_reason", std::string{}) == "idle",
                              "Test6: JSON contains human-readable settle_reason");
        AURORA_TEST_CHECK_MSG(near_d(j.value("dp_per_unit", 0.0), 16.0, 1e-3),
                              "Test6: JSON contains calibration factor");
        AURORA_TEST_CHECK_MSG(j.contains("report") && j["report"].is_object(),
                              "Test6: JSON embeds complete report object");
    }

    const std::string md = r.to_markdown();
    AURORA_TEST_CHECK_MSG(md.find("trustworthy") != std::string::npos, "Test6: Markdown contains trustworthy line");
    AURORA_TEST_CHECK_MSG(md.find("step calibration") != std::string::npos,
                          "Test6: Markdown contains calibration line");
    AURORA_TEST_CHECK_MSG(md.find("geometry stable") != std::string::npos,
                          "Test6: Markdown contains geometry-stable line");

    // 不可信时 Markdown 必须显式喊出来，不能只在数字里体现。
    Result_ bad = make_valid_result();
    bad.scrollable_found = false;
    AURORA_TEST_CHECK_MSG(bad.to_markdown().find("FAIL") != std::string::npos,
                          "Test6: untrustworthy result marked FAIL in Markdown");
}

// =========================================================================
// 二、真实 Headless 采样
// =========================================================================

// ---- Test 7: 正常场景——定位、标定、每帧真滚、判为可信 ----
auto test_run_scrollable() -> Result_ {
    const auto cfg = small_config("unit-scrollable");
    const Result_ r = ScrollBenchHarness::run(build_scrollable(100), Size{ .width = 400.0f, .height = 300.0f }, cfg);

    AURORA_TEST_CHECK_MSG(r.scrollable_found, "Test7: located Scroll container");
    AURORA_TEST_CHECK_MSG(r.settled, "Test7: settle phase ended normally");
    AURORA_TEST_CHECK_MSG(r.settle_reason == SettleReason::Idle,
                          "Test7: static tree converges by 'consecutive no-dirty' (not wall-clock fallback)");
    AURORA_TEST_CHECK_MSG(r.settle_frames >= 3, "Test7: ran at least settle_idle_frames frames");

    AURORA_TEST_CHECK_MSG(r.report.frame_count == 30, "Test7: sampled frame count == cfg.frames (warmup excluded)");
    AURORA_TEST_CHECK_MSG(r.moved_frames == 30, "Test7: every sampled frame produced real movement");
    AURORA_TEST_CHECK_MSG(r.idle_frames == 0, "Test7: no idle frame skips");
    AURORA_TEST_CHECK_MSG(r.reversals == 0, "Test7: content long enough, no edge reversal throughout");

    // 几何：100 块 × 40dp = 4000dp 内容，300dp 视口 → 行程 ≈ 3700dp（Column 间距可能微调）。
    AURORA_TEST_CHECK_MSG(r.max_offset > 3000.0f, "Test7: measured travel > 3000dp (content 4000dp / viewport 300dp)");
    AURORA_TEST_CHECK_MSG(r.geometry_stable(), "Test7: travel consistent before/after sampling (content static)");
    AURORA_TEST_CHECK_MSG(near_f(r.scroll_viewport_h, 300.0f, 1.0f),
                          "Test7: scroll container viewport height == window height (no top/bottom bar squeeze)");
    AURORA_TEST_CHECK_MSG(r.content_screens() > 2.0f, "Test7: content exceeds two screens");

    // 标定：`Scroll::step` 默认 16dp/滚轮单位，harness 应实测出这个系数。
    AURORA_TEST_CHECK_MSG(near_f(r.dp_per_unit, 16.0f, 0.01f),
                          "Test7: dp_per_unit calibrated to 16.0 (Scroll::step default)");

    // 位移量：30 帧 × 12dp/帧 = 360dp（标定生效后，配置里的 dp 就是真实 dp）。
    AURORA_TEST_CHECK_MSG(near_d(r.scrolled_px, 360.0, 1.0),
                          "Test7: accumulated offset ~= 30x12 = 360dp (dp-calibrated)");
    AURORA_TEST_CHECK_MSG(r.final_offset > 0.0f, "Test7: offset positive at end");

    AURORA_TEST_CHECK_MSG(r.trustworthy(), "Test7: overall judged trustworthy");

    // 时间读数不做阈值断言（会 flaky），只校验「确实测到了东西」。
    AURORA_TEST_CHECK_MSG(r.report.total_ms > 0.0, "Test7: non-zero total duration collected");
    AURORA_TEST_CHECK_MSG(r.p99_ms() >= r.p50_ms(), "Test7: p99 >= p50 (percentiles monotonic)");
    AURORA_TEST_CHECK_MSG(r.worst_ms() >= r.p99_ms(), "Test7: worst >= p99");

    if constexpr (profiling_enabled()) {
        AURORA_TEST_CHECK_MSG(r.counters_sum().paint_nodes > 0, "Test7[PROFILING=ON]: counters have readings");
        AURORA_TEST_CHECK_MSG(
            r.counters_max().scroll_buffer_bytes > 0,
            "Test7[PROFILING=ON]: Scroll offscreen-buffer byte count is instrumented (anchor for gate G-8)");
    } else { // NOLINT
        AURORA_TEST_CHECK_MSG(r.counters_sum().paint_nodes == 0,
                              "Test7[PROFILING=OFF]: counters stay 0 (instrumentation compiled out)");
    }
    return r;
}

// ---- Test 8: 内容不足一屏——必须判为不可信 ----
auto test_run_too_short() -> void {
    auto cfg = small_config("unit-too-short");
    // 2 块 × 40dp = 80dp 内容，300dp 视口 → 根本没得滚。
    const Result_ r = ScrollBenchHarness::run(build_scrollable(2), Size{ .width = 400.0f, .height = 300.0f }, cfg);

    AURORA_TEST_CHECK_MSG(r.scrollable_found,
                          "Test8: still located Scroll container (control present, just nothing scrollable)");
    AURORA_TEST_CHECK_MSG(near_f(r.max_offset, 0.0f, 0.01f), "Test8: measured travel is 0");
    AURORA_TEST_CHECK_MSG(near_f(r.dp_per_unit, 0.0f, 1e-6f),
                          "Test8: skip calibration when not scrollable, dp_per_unit stays 0");
    AURORA_TEST_CHECK_MSG(r.moved_frames == 0, "Test8: no frame produced any movement");
    AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test8: judged untrustworthy (this is exactly why the harness exists)");
    AURORA_TEST_CHECK_MSG(r.to_markdown().find("FAIL") != std::string::npos, "Test8: Markdown clearly marks FAIL");
}

// ---- Test 9: 树里没有滚动容器 ----
auto test_run_no_scrollable() -> void {
    const auto cfg = small_config("unit-no-scrollable");
    const Result_ r = ScrollBenchHarness::run(build_static_tree(), Size{ .width = 400.0f, .height = 300.0f }, cfg);

    AURORA_TEST_CHECK_MSG(!r.scrollable_found, "Test9: scrollable_found == false");
    AURORA_TEST_CHECK_MSG(near_f(r.scroll_viewport_h, 0.0f, 1e-6f),
                          "Test9: viewport height 0 when no scroll container");
    AURORA_TEST_CHECK_MSG(r.moved_frames == 0, "Test9: no movement");
    AURORA_TEST_CHECK_MSG(!r.trustworthy(), "Test9: judged untrustworthy");
    AURORA_TEST_CHECK_MSG(r.report.frame_count == 30,
                          "Test9: still samples normally (exposes problem instead of silently skipping)");
}

// ---- Test 10: 非法输入不崩溃、直接判伪 ----
auto test_run_invalid_input() -> void {
    const auto cfg = small_config("unit-invalid");

    const Result_ empty = ScrollBenchHarness::run(Node{}, Size{ .width = 400.0f, .height = 300.0f }, cfg);
    AURORA_TEST_CHECK_MSG(!empty.scrollable_found && !empty.trustworthy(),
                          "Test10: empty node -> untrustworthy, no crash");
    AURORA_TEST_CHECK_MSG(empty.report.frame_count == 0, "Test10: empty node does not enter sampling");

    const Result_ zero = ScrollBenchHarness::run(build_scrollable(50), Size{ .width = 0.0f, .height = 0.0f }, cfg);
    AURORA_TEST_CHECK_MSG(!zero.trustworthy(), "Test10: zero-size viewport -> untrustworthy, no crash");

    const Result_ neg = ScrollBenchHarness::run(build_scrollable(50), Size{ .width = 400.0f, .height = -10.0f }, cfg);
    AURORA_TEST_CHECK_MSG(!neg.trustworthy(), "Test10: negative-height viewport -> untrustworthy, no crash");
}

// ---- Test 11: 关闭落定阶段 ----
auto test_settle_disabled() -> void {
    auto cfg = small_config("unit-no-settle");
    cfg.settle_ms = 0.0;

    const Result_ r = ScrollBenchHarness::run(build_scrollable(100), Size{ .width = 400.0f, .height = 300.0f }, cfg);
    AURORA_TEST_CHECK_MSG(r.settled, "Test11: settle_ms = 0 treated as settled (caller explicitly disabled)");
    AURORA_TEST_CHECK_MSG(r.settle_reason == SettleReason::Disabled, "Test11: exit reason annotated as Disabled");
    AURORA_TEST_CHECK_MSG(r.settle_frames == 0, "Test11: no settle frames consumed");
    AURORA_TEST_CHECK_MSG(r.trustworthy(),
                          "Test11: readings still trustworthy after disabling settle phase on static tree");
}

// ---- Test 12: 确定性——同树同配置两次运行，几何类读数逐位相同 ----
auto test_determinism(const Result_ &first) -> void {
    const auto cfg = small_config("unit-scrollable");
    const Result_ second =
        ScrollBenchHarness::run(build_scrollable(100), Size{ .width = 400.0f, .height = 300.0f }, cfg);

    AURORA_TEST_CHECK_MSG(second.moved_frames == first.moved_frames, "Test12: moved_frames reproducible");
    AURORA_TEST_CHECK_MSG(second.reversals == first.reversals, "Test12: reversals reproducible");
    AURORA_TEST_CHECK_MSG(second.max_offset == first.max_offset, "Test12: max_offset bit-identical");
    AURORA_TEST_CHECK_MSG(second.dp_per_unit == first.dp_per_unit, "Test12: dp_per_unit bit-identical");
    AURORA_TEST_CHECK_MSG(second.final_offset == first.final_offset, "Test12: final_offset bit-identical");
    AURORA_TEST_CHECK_MSG(near_d(second.scrolled_px, first.scrolled_px, 1e-6), "Test12: scrolled_px reproducible");

    if constexpr (profiling_enabled()) {
        // 计数器是 CI 回归锚点，跨运行必须逐位相同，否则不能拿来锁基线。
        AURORA_TEST_CHECK_MSG(second.counters_max().paint_nodes == first.counters_max().paint_nodes,
                              "Test12[PROFILING=ON]: paint_nodes 峰值逐位相同");
        AURORA_TEST_CHECK_MSG(second.counters_sum().paint_nodes == first.counters_sum().paint_nodes,
                              "Test12[PROFILING=ON]: paint_nodes cumulative bit-identical");
        AURORA_TEST_CHECK_MSG(second.counters_max().scroll_buffer_bytes == first.counters_max().scroll_buffer_bytes,
                              "Test12[PROFILING=ON]: scroll_buffer_bytes peak bit-identical");
        AURORA_TEST_CHECK_MSG(second.counters_sum().pixels_filled == first.counters_sum().pixels_filled,
                              "Test12[PROFILING=ON]: pixels_filled cumulative bit-identical");
    }
}

// ---- Test 13: fling 模式跑得通且仍判可信 ----
auto test_fling_mode() -> void {
    auto cfg = small_config("unit-fling");
    cfg.fling = true;

    const Result_ r = ScrollBenchHarness::run(build_scrollable(400), Size{ .width = 400.0f, .height = 300.0f }, cfg);
    AURORA_TEST_CHECK_MSG(r.scrollable_found, "Test13: fling mode still locates scroll container");
    AURORA_TEST_CHECK_MSG(r.moved_frames == 30, "Test13: fling mode still produces movement each frame");
    AURORA_TEST_CHECK_MSG(r.scrolled_px > 360.0,
                          "Test13: fling starts faster, accumulated offset larger than constant-speed");
    AURORA_TEST_CHECK_MSG(r.trustworthy(), "Test13: fling readings judged trustworthy");
}

// ---- Test 14: scale 参数真实生效（不改变逻辑几何）----
auto test_scale() -> void {
    auto cfg = small_config("unit-scale2x");
    cfg.scale = 2.0f;

    const Result_ r = ScrollBenchHarness::run(build_scrollable(100), Size{ .width = 400.0f, .height = 300.0f }, cfg);
    AURORA_TEST_CHECK_MSG(r.trustworthy(), "Test14: readings still trustworthy under scale = 2.0");
    // 视口尺寸是**逻辑 dp**，缩放只影响物理像素，逻辑几何必须保持不变。
    AURORA_TEST_CHECK_MSG(near_f(r.scroll_viewport_h, 300.0f, 1.0f),
                          "Test14: logical viewport height independent of scale");
    AURORA_TEST_CHECK_MSG(near_f(r.dp_per_unit, 16.0f, 0.01f), "Test14: calibration factor independent of scale");

    if constexpr (profiling_enabled()) {
        AURORA_TEST_CHECK_MSG(r.counters_sum().pixels_filled > 0,
                              "Test14[PROFILING=ON]: pixels are actually filled under 2x scaling");
    }
}

} // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_scroll_bench ===\n");

    // 一、纯判据（零渲染，确定性）
    test_geometry_stable();
    test_content_screens();
    test_reversal_ratio();
    test_trustworthy_conditions();
    test_settle_reason_semantics();
    test_serialization();

    // 二、真实 Headless 采样
    const Result_ baseline = test_run_scrollable();
    test_run_too_short();
    test_run_no_scrollable();
    test_run_invalid_input();
    test_settle_disabled();
    test_determinism(baseline);
    test_fling_mode();
    test_scale();

    RenderCounters::current().reset();
}
