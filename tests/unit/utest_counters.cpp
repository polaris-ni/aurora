/// 测试类型: unit
/// 目标单元: include/aurora/perf/counters.h
/// 测试说明: 覆盖 RenderCounters——默认全零、进程级单例语义、reset 归零、add 逐字段累加
/// （full_redraw 逻辑或、dirty_area_ratio 算术和）、merge_max 逐字段取最大、to_json 字段序列化、
/// to_csv_row 与 csv_header 列数严格对应。

#include <algorithm>
#include <string>
#include <string_view>

#include "aurora/perf/counters.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_counters {

namespace {

/// @brief 统计字符出现次数（CSV 列数 = 逗号数 + 1）。
auto count_of(std::string_view text, char ch) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count(text, ch));
}

}  // namespace

AURORA_TEST_CASE(default_constructed_counters_are_zero) {
    // 默认构造：全部字段零值 / false，不产生 NaN。
    const RenderCounters c{};
    AURORA_TEST_CHECK_EQ(c.draw_calls, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.fill_rects, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.glyphs_rendered, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.pixels_filled, std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(c.dl_records, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.layout_nodes, std::uint32_t{0});
    AURORA_TEST_CHECK_NEAR(c.dirty_area_ratio, 0.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(c.full_redraw);
    AURORA_TEST_CHECK_EQ(c.scroll_buffer_bytes, std::uint64_t{0});
}

AURORA_TEST_CASE(current_is_process_wide_singleton) {
    // current() 返回进程级单例：多次取址一致，写入读回生效。
    auto& c = RenderCounters::current();
    AURORA_TEST_CHECK_EQ(&RenderCounters::current(), &c);

    c.reset();
    AURORA_TEST_CHECK_EQ(c.fill_rects, std::uint32_t{0});
    c.fill_rects = 5;
    AURORA_TEST_CHECK_EQ(RenderCounters::current().fill_rects, std::uint32_t{5});
    c.reset();  // 恢复：不把脏状态泄漏给后续用例
    AURORA_TEST_CHECK_EQ(RenderCounters::current().fill_rects, std::uint32_t{0});
}

AURORA_TEST_CASE(reset_zeroes_every_field) {
    // 填满非零值后 reset：逐字段归零（帧起点语义）。
    RenderCounters c;
    c.draw_calls = 9;
    c.fill_rects = 8;
    c.glyph_cache_hits = 7;
    c.dl_replays = 6;
    c.layout_nodes = 5;
    c.dirty_rect_count = 4;
    c.dirty_area_ratio = 0.5;
    c.full_redraw = true;
    c.scroll_buffer_bytes = 123456;

    c.reset();
    AURORA_TEST_CHECK_EQ(c.draw_calls, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.fill_rects, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.glyph_cache_hits, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.dl_replays, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.layout_nodes, std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(c.dirty_rect_count, std::uint32_t{0});
    AURORA_TEST_CHECK_NEAR(c.dirty_area_ratio, 0.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(c.full_redraw);
    AURORA_TEST_CHECK_EQ(c.scroll_buffer_bytes, std::uint64_t{0});
}

AURORA_TEST_CASE(add_accumulates_fields_fieldwise) {
    // add：逐字段累加；dirty_area_ratio 按算术和；full_redraw 按逻辑或。
    RenderCounters a{};
    a.fill_rects = 3;
    a.pixels_filled = 100;
    a.dl_records = 2;
    a.dirty_area_ratio = 0.25;
    a.full_redraw = false;

    RenderCounters b{};
    b.fill_rects = 4;
    b.pixels_filled = 50;
    b.dl_records = 1;
    b.dirty_area_ratio = 0.5;
    b.full_redraw = true;

    a.add(b);
    AURORA_TEST_CHECK_EQ(a.fill_rects, std::uint32_t{7});
    AURORA_TEST_CHECK_EQ(a.pixels_filled, std::uint64_t{150});
    AURORA_TEST_CHECK_EQ(a.dl_records, std::uint32_t{3});
    AURORA_TEST_CHECK_NEAR(a.dirty_area_ratio, 0.75, 1e-9);
    AURORA_TEST_CHECK_TRUE(a.full_redraw);
}

AURORA_TEST_CASE(merge_max_takes_fieldwise_maximum) {
    // merge_max：逐字段取最大；full_redraw 仍按逻辑或（峰值合并不丢整帧重绘标记）。
    RenderCounters a{};
    a.draw_calls = 5;
    a.layout_nodes = 10;
    a.dirty_area_ratio = 0.8;
    a.scroll_buffer_bytes = 999;
    a.full_redraw = false;

    RenderCounters b{};
    b.draw_calls = 7;
    b.layout_nodes = 3;
    b.dirty_area_ratio = 0.2;
    b.scroll_buffer_bytes = 1234;
    b.full_redraw = true;

    a.merge_max(b);
    AURORA_TEST_CHECK_EQ(a.draw_calls, std::uint32_t{7});
    AURORA_TEST_CHECK_EQ(a.layout_nodes, std::uint32_t{10});
    AURORA_TEST_CHECK_NEAR(a.dirty_area_ratio, 0.8, 1e-9);
    AURORA_TEST_CHECK_EQ(a.scroll_buffer_bytes, std::uint64_t{1234});
    AURORA_TEST_CHECK_TRUE(a.full_redraw);
}

AURORA_TEST_CASE(to_json_contains_serialized_fields) {
    // JSON 序列化：字段名与值逐一出现；full_redraw 布尔、dirty_area_ratio 四位小数。
    RenderCounters c{};
    c.draw_calls = 12;
    c.fill_rects = 34;
    c.dirty_area_ratio = 0.25;
    c.full_redraw = true;
    c.scroll_buffer_bytes = 4096;

    const std::string json = c.to_json();
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("draw_calls":12)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("fill_rects":34)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("dirty_area_ratio":0.2500)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("full_redraw":true)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("scroll_buffer_bytes":4096)"));

    // 零值对象同样可序列化为合法对象形态。
    const std::string zero_json = RenderCounters{}.to_json();
    AURORA_TEST_CHECK_THAT(zero_json, ::aurora::testing::matchers::has_substr(R"("draw_calls":0)"));
    AURORA_TEST_CHECK_THAT(zero_json, ::aurora::testing::matchers::starts_with("{"));
}

AURORA_TEST_CASE(csv_row_column_count_matches_header) {
    // CSV 表头与数据行列数严格对应（字段顺序一致），便于趋势表直接拼接。
    const std::string header{RenderCounters::csv_header()};
    const std::string row = RenderCounters{}.to_csv_row();
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::starts_with("draw_calls,"));
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::ends_with("scroll_buffer_bytes"));
    AURORA_TEST_CHECK_EQ(count_of(header, ','), count_of(row, ','));

    // 填充值后列数不变。
    RenderCounters filled{};
    filled.draw_calls = 1;
    filled.dirty_area_ratio = 0.5;
    AURORA_TEST_CHECK_EQ(count_of(filled.to_csv_row(), ','), count_of(header, ','));
}

}  // namespace aurora::test_cases::utest_counters
