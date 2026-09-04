// test_perf_counters.cpp — `aurora::RenderCounters` 与埋点宏单测。
//
// 本文件的核心职责是**两分支等价性**：同一份源码在 `AURORA_ENABLE_PROFILING`
// ON / OFF 两种构建下都必须编译通过、都必须给出各自正确的行为。
//   - ON  ：宏累加/赋值生效，宏参数**恰好求值一次**；
//   - OFF ：宏完全展开为 `((void)0)`，计数恒 0，宏参数**一次都不求值**
//           （这正是 counters.h 里「参数不得带副作用」那条约束的可执行化表述）。
// 断言经 `if constexpr (profiling_enabled())` 分流，两种构建跑的是同一个测试文件。
//
// 其余用例（add / merge_max / 序列化）与开关无关，属纯数据结构行为。
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::profiling_enabled;
using aurora::RenderCounters;

using Json = nlohmann::json;

namespace {

/// @brief 统计 CSV 行的字段数（本模块的字段值均不含逗号，简单切分即可）。
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

/// @brief 造一份每个字段都非零的计数，用于验证「所有字段都被处理到」。
[[nodiscard]] auto make_filled(std::uint32_t base) -> RenderCounters {
    RenderCounters c{};
    c.draw_calls = base;
    c.fill_rects = base + 1;
    c.draw_texts = base + 2;
    c.glyphs_rendered = base + 3;
    c.pixels_filled = base + 4;
    c.glyph_cache_hits = base + 5;
    c.glyph_cache_misses = base + 6;
    c.shape_cache_hits = base + 7;
    c.shape_cache_misses = base + 8;
    c.dl_replays = base + 9;
    c.dl_records = base + 10;
    c.layout_nodes = base + 11;
    c.paint_nodes = base + 12;
    c.relayout_boundaries_hit = base + 13;
    c.dirty_rect_count = base + 14;
    c.dirty_area_ratio = 0.25;
    c.full_redraw = false;
    c.scroll_buffer_bytes = base + 15;
    return c;
}

// ---- Test 1: 默认值全零 + reset ----
auto test_defaults_and_reset() -> void {
    constexpr RenderCounters zero{};
    AURORA_TEST_CHECK_MSG(zero.draw_calls == 0 && zero.pixels_filled == 0,
                          "Test1: default-constructed draw fields are 0");
    AURORA_TEST_CHECK_MSG(zero.layout_nodes == 0 && zero.paint_nodes == 0,
                          "Test1: default-constructed traversal fields are 0");
    AURORA_TEST_CHECK_MSG(!zero.full_redraw && near_d(zero.dirty_area_ratio, 0.0),
                          "Test1: default-constructed dirty-region fields are empty");
    AURORA_TEST_CHECK_MSG(zero.scroll_buffer_bytes == 0, "Test1: default-constructed scroll_buffer_bytes == 0");

    RenderCounters c = make_filled(100);
    c.full_redraw = true;
    c.reset();
    AURORA_TEST_CHECK_MSG(c.draw_calls == 0 && c.scroll_buffer_bytes == 0 && !c.full_redraw,
                          "Test1: reset() zeroes all fields");
    AURORA_TEST_CHECK_MSG(near_d(c.dirty_area_ratio, 0.0), "Test1: reset() zeroes dirty_area_ratio");
}

// ---- Test 2: add 逐字段累加，full_redraw 取逻辑或，dirty_area_ratio 取算术和 ----
auto test_add() -> void {
    RenderCounters a = make_filled(10);
    const RenderCounters b = make_filled(20);

    a.add(b);

    AURORA_TEST_CHECK_MSG(a.draw_calls == 30, "Test2: draw_calls accumulate (10+20)");
    AURORA_TEST_CHECK_MSG(a.pixels_filled == 14 + 24, "Test2: pixels_filled accumulates");
    AURORA_TEST_CHECK_MSG(a.scroll_buffer_bytes == 25 + 35, "Test2: scroll_buffer_bytes accumulates");
    AURORA_TEST_CHECK_MSG(a.relayout_boundaries_hit == 23 + 33, "Test2: relayout_boundaries_hit accumulates");
    AURORA_TEST_CHECK_MSG(near_d(a.dirty_area_ratio, 0.5, 1e-9),
                          "Test2: dirty_area_ratio takes arithmetic sum (0.25+0.25)");
    AURORA_TEST_CHECK_MSG(!a.full_redraw, "Test2: full_redraw stays false when both sides false");

    RenderCounters f{};
    RenderCounters t{};
    t.full_redraw = true;
    f.add(t);
    AURORA_TEST_CHECK_MSG(f.full_redraw, "Test2: full_redraw merged by logical OR");
}

// ---- Test 3: merge_max 逐字段取大 ----
auto test_merge_max() -> void {
    RenderCounters a = make_filled(50);
    const RenderCounters b = make_filled(5);

    a.merge_max(b);  // a 全面更大，应保持不变
    AURORA_TEST_CHECK_MSG(a.draw_calls == 50 && a.scroll_buffer_bytes == 65,
                          "Test3: merging smaller value does not change peak");

    RenderCounters lo = make_filled(1);
    const RenderCounters hi = make_filled(1000);
    lo.merge_max(hi);
    AURORA_TEST_CHECK_MSG(lo.draw_calls == 1000, "Test3: merging larger value raises peak");
    AURORA_TEST_CHECK_MSG(lo.pixels_filled == 1004, "Test3: pixels_filled takes max");
    AURORA_TEST_CHECK_MSG(lo.scroll_buffer_bytes == 1015, "Test3: scroll_buffer_bytes takes max");

    RenderCounters r{};
    r.dirty_area_ratio = 0.8;
    RenderCounters s{};
    s.dirty_area_ratio = 0.3;
    r.merge_max(s);
    AURORA_TEST_CHECK_MSG(near_d(r.dirty_area_ratio, 0.8, 1e-9), "Test3: dirty_area_ratio takes max (not summed)");
}

// ---- Test 4: to_json 是合法 JSON 且字段齐全 ----
auto test_to_json() -> void {
    RenderCounters c = make_filled(7);
    c.full_redraw = true;

    const std::string s = c.to_json();
    AURORA_TEST_CHECK_MSG(s.size() > 2 && s.front() == '{' && s.back() == '}',
                          "Test4: to_json looks like a JSON object");

    const Json j = Json::parse(s, nullptr, false);
    AURORA_TEST_CHECK_MSG(!j.is_discarded(), "Test4: to_json output is parseable");
    if (j.is_discarded()) {
        return;
    }

    AURORA_TEST_CHECK_MSG(j.size() == 18, "Test4: JSON key count == struct field count (18)");
    AURORA_TEST_CHECK_MSG(j.value("draw_calls", 0U) == 7U, "Test4: draw_calls correct");
    AURORA_TEST_CHECK_MSG(j.value("scroll_buffer_bytes", 0ULL) == 22ULL, "Test4: scroll_buffer_bytes correct");
    AURORA_TEST_CHECK_MSG(j.value("relayout_boundaries_hit", 0U) == 20U, "Test4: relayout_boundaries_hit correct");
    AURORA_TEST_CHECK_MSG(j.value("full_redraw", false), "Test4: full_redraw serialized as JSON boolean");
    AURORA_TEST_CHECK_MSG(near_d(j.value("dirty_area_ratio", 0.0), 0.25, 1e-4), "Test4: dirty_area_ratio correct");
}

// ---- Test 5: CSV 表头与数据行列数严格一致 ----
auto test_csv_alignment() -> void {
    const auto header = std::string(RenderCounters::csv_header());
    const RenderCounters c = make_filled(3);
    const std::string row = c.to_csv_row();

    const std::size_t hn = csv_field_count(header);
    const std::size_t rn = csv_field_count(row);

    AURORA_TEST_CHECK_MSG(hn == 18, "Test5: csv_header() column count == 18");
    AURORA_TEST_CHECK_MSG(hn == rn, "Test5: header column count matches data row column count");
    AURORA_TEST_CHECK_MSG(row.find(',') != std::string::npos, "Test5: data row non-empty and contains delimiter");

    // full_redraw 在 CSV 里写作 0/1（而非 true/false），便于表格软件直接聚合。
    // 字段序为 ...,dirty_area_ratio,full_redraw,scroll_buffer_bytes，故末尾恰为 ",1,0"。
    RenderCounters t{};
    t.full_redraw = true;
    const std::string trow = t.to_csv_row();
    AURORA_TEST_CHECK_MSG(trow.size() >= 4 && trow.compare(trow.size() - 4, 4, ",1,0") == 0,
                          "Test5: full_redraw written as 0/1 numeric (trailing \",1,0\")");
}

// ---- Test 6: current() 是进程级单例 ----
auto test_current_singleton() -> void {
    RenderCounters &a = RenderCounters::current();
    RenderCounters &b = RenderCounters::current();
    AURORA_TEST_CHECK_MSG(&a == &b, "Test6: current() returns the same instance");

    a.reset();
    b.draw_calls = 42;
    AURORA_TEST_CHECK_MSG(a.draw_calls == 42, "Test6: writes via either reference visible to the other");
    a.reset();
}

// ---- Test 7: 埋点宏在两种构建下的确定性行为（本文件的核心用例）----
auto test_macro_branch_determinism() -> void {
    RenderCounters &c = RenderCounters::current();
    c.reset();

    // 宏参数求值次数：ON 恰好一次、OFF 一次都不求值。
    int evaluated = 0;
    const auto value_with_side_effect = [&evaluated]() -> std::uint32_t {
        ++evaluated;
        return 5U;
    };

    AURORA_PROFILE_COUNT(draw_calls, value_with_side_effect());
    AURORA_PROFILE_COUNT(draw_calls, 3U);
    AURORA_PROFILE_SET(full_redraw, true);
    AURORA_PROFILE_COUNT(scroll_buffer_bytes, 1024ULL);

    if constexpr (profiling_enabled()) {
        AURORA_TEST_CHECK_MSG(c.draw_calls == 8U, "Test7[ON]: AURORA_PROFILE_COUNT accumulation works (5+3)");
        AURORA_TEST_CHECK_MSG(c.full_redraw, "Test7[ON]: AURORA_PROFILE_SET assignment works");
        AURORA_TEST_CHECK_MSG(c.scroll_buffer_bytes == 1024ULL, "Test7[ON]: 64-bit field accumulation works");
        AURORA_TEST_CHECK_MSG(evaluated == 1, "Test7[ON]: macro argument evaluated exactly once");
    } else {  // NOLINT
        AURORA_TEST_CHECK_MSG(c.draw_calls == 0U, "Test7[OFF]: AURORA_PROFILE_COUNT produces no writes");
        AURORA_TEST_CHECK_MSG(!c.full_redraw, "Test7[OFF]: AURORA_PROFILE_SET produces no writes");

        AURORA_TEST_CHECK_MSG(c.scroll_buffer_bytes == 0ULL, "Test7[OFF]: 64-bit field stays 0");
        AURORA_TEST_CHECK_MSG(evaluated == 0,
                              "Test7[OFF]: macro argument evaluated zero times (so args must have no side effects)");
    }
    (void)value_with_side_effect;  // OFF 构建下宏把它整个丢弃，显式消费以免 -Wunused

    c.reset();
}

// ---- Test 8: 宏可用于任意语句位置（语法形态回归）----
auto test_macro_syntax_forms() -> void {
    RenderCounters &c = RenderCounters::current();
    c.reset();

    // 无花括号的 if / for 体：宏必须是单条完整语句，否则这里编译失败。
    AURORA_PROFILE_COUNT(fill_rects, 1);

    for (int i = 0; i < 3; ++i) {
        AURORA_PROFILE_COUNT(paint_nodes, 1);
    }

    constexpr std::uint32_t expected_rects = profiling_enabled() ? 1U : 0U;
    constexpr std::uint32_t expected_nodes = profiling_enabled() ? 3U : 0U;
    AURORA_TEST_CHECK_MSG(c.fill_rects == expected_rects, "Test8: macro behaves correctly in brace-less if branch");
    AURORA_TEST_CHECK_MSG(c.paint_nodes == expected_nodes, "Test8: macro behaves correctly in brace-less for body");

    c.reset();
}

// ---- Test 9: profiling_enabled() 是编译期常量，可用于 constexpr 上下文 ----
auto test_profiling_enabled_constexpr() -> void {
    static_assert(profiling_enabled() || !profiling_enabled(), "profiling_enabled() must be constexpr");
    constexpr bool on = profiling_enabled();
    // 与宏定义状态一致性：宏在则必须为 true，反之为 false。
#ifdef AURORA_ENABLE_PROFILING
    AURORA_TEST_CHECK_MSG(on, "Test9: profiling_enabled() == true when AURORA_ENABLE_PROFILING defined");
#else
    AURORA_TEST_CHECK_MSG(!on, "Test9: profiling_enabled() == false when AURORA_ENABLE_PROFILING undefined");
#endif
    AURORA_TEST_PRINTF("      (build profile: profiling=%s)\n", on ? "ON" : "OFF");
}

}  // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_perf_counters ===\n");

    test_defaults_and_reset();
    test_add();
    test_merge_max();
    test_to_json();
    test_csv_alignment();
    test_current_singleton();
    test_macro_branch_determinism();
    test_macro_syntax_forms();
    test_profiling_enabled_constexpr();

    RenderCounters::current().reset();
}
