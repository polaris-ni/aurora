/// @file utest_bench_result.cpp
/// 测试类型: unit
/// 目标单元: tools/include/bench_result.h
/// 测试说明: 覆盖统一结果契约的字段齐备性与 JSON 往返、跳过与不可信状态的原因码、
/// 非有限读数拒绝、落盘往返、重复指标的后写覆盖语义与结果文件命名规则

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include "aurora/core/json.h"
#include "bench_result.h"
#include "framework/aurora_test.h"
#include "framework/isolation.h"

namespace aurora::test_cases::utest_bench_result {

namespace aj = aurora::json;
namespace ab = aurora::bench;

namespace {

[[nodiscard]] auto string_at(const aj::Value &doc, const char *pointer) -> std::string {
    const aj::Value *node = aj::find_pointer(doc, pointer);
    AURORA_TEST_REQUIRE(node != nullptr);
    const auto text = node->as<std::string>();
    AURORA_TEST_REQUIRE(text.ok());
    return text.value();
}

[[nodiscard]] auto double_at(const aj::Value &doc, const char *pointer) -> double {
    const aj::Value *node = aj::find_pointer(doc, pointer);
    AURORA_TEST_REQUIRE(node != nullptr);
    const auto number = node->as<double>();
    AURORA_TEST_REQUIRE(number.ok());
    return number.value();
}

[[nodiscard]] auto int_at(const aj::Value &doc, const char *pointer) -> int {
    const aj::Value *node = aj::find_pointer(doc, pointer);
    AURORA_TEST_REQUIRE(node != nullptr);
    const auto number = node->as<int>();
    AURORA_TEST_REQUIRE(number.ok());
    return number.value();
}

[[nodiscard]] auto bool_at(const aj::Value &doc, const char *pointer) -> bool {
    const aj::Value *node = aj::find_pointer(doc, pointer);
    AURORA_TEST_REQUIRE(node != nullptr);
    const auto flag = node->as<bool>();
    AURORA_TEST_REQUIRE(flag.ok());
    return flag.value();
}

[[nodiscard]] auto parse(const ab::BenchResult &result) -> aj::Value {
    const std::string text = result.to_json();
    AURORA_TEST_REQUIRE_FALSE(text.empty());
    const auto parsed = aj::parse(text);
    AURORA_TEST_REQUIRE(parsed.ok());
    return parsed.value();
}

}  // namespace

AURORA_TEST_CASE(complete_contract_round_trips) {
    auto meta = ab::make_meta("bench_unit", "unit-scene");
    meta.warmup_frames = 30;
    meta.sampled_frames = 300;
    meta.interleave_seed = 42;
    ab::BenchResult result{meta};
    result.metric("p99_ms", 4.25);
    result.metric("frames", 300.0, "count");
    result.flag("scrollable_found", true);
    result.scope("layout", 12.5, 300);
    result.trust(true, "all self-checks passed");

    const aj::Value doc = parse(result);
    AURORA_TEST_CHECK_EQ(int_at(doc, "/schema_version"), ab::AURORA_BENCH_RESULT_SCHEMA);
    AURORA_TEST_CHECK_EQ(string_at(doc, "/status"), std::string{"ok"});
    AURORA_TEST_CHECK_EQ(string_at(doc, "/data_status"), std::string{"complete"});
    AURORA_TEST_CHECK_EQ(string_at(doc, "/meta/tool"), std::string{"bench_unit"});
    AURORA_TEST_CHECK_EQ(string_at(doc, "/meta/scene"), std::string{"unit-scene"});
    AURORA_TEST_CHECK_EQ(int_at(doc, "/meta/warmup_frames"), 30);
    AURORA_TEST_CHECK_EQ(int_at(doc, "/meta/sampled_frames"), 300);
    AURORA_TEST_CHECK_EQ(int_at(doc, "/meta/interleave_seed"), 42);
    AURORA_TEST_CHECK_EQ(double_at(doc, "/metrics/p99_ms/value"), 4.25);
    AURORA_TEST_CHECK_EQ(string_at(doc, "/metrics/p99_ms/unit"), std::string{"ms"});
    AURORA_TEST_CHECK_EQ(string_at(doc, "/metrics/frames/unit"), std::string{"count"});
    AURORA_TEST_CHECK(bool_at(doc, "/flags/scrollable_found"));
    AURORA_TEST_CHECK_EQ(string_at(doc, "/scopes/0/name"), std::string{"layout"});
    AURORA_TEST_CHECK_EQ(double_at(doc, "/scopes/0/total_ms"), 12.5);
    AURORA_TEST_CHECK_EQ(int_at(doc, "/scopes/0/calls"), 300);
    AURORA_TEST_CHECK(bool_at(doc, "/trust/trustworthy"));
    AURORA_TEST_CHECK_EQ(string_at(doc, "/trust/reason"), std::string{"all self-checks passed"});
}

AURORA_TEST_CASE(skipped_run_records_reason_and_missing_data) {
    ab::BenchResult result{ab::make_meta("bench_unit", "no-backend")};
    result.skip("backend-unavailable");

    const aj::Value doc = parse(result);
    AURORA_TEST_CHECK_EQ(string_at(doc, "/status"), std::string{"skipped"});
    AURORA_TEST_CHECK_EQ(string_at(doc, "/data_status"), std::string{"data-missing"});
    AURORA_TEST_CHECK_EQ(string_at(doc, "/reason"), std::string{"backend-unavailable"});
    AURORA_TEST_CHECK_FALSE(bool_at(doc, "/trust/trustworthy"));
    const aj::Value *metrics = aj::find_pointer(doc, "/metrics");
    AURORA_TEST_REQUIRE(metrics != nullptr);
    AURORA_TEST_CHECK_EQ(metrics->type(), aj::Type::Object);
}

AURORA_TEST_CASE(untrusted_run_stays_readable_but_cannot_pass) {
    ab::BenchResult result{ab::make_meta("bench_unit", "unit-scene")};
    result.metric("p99_ms", 1.0);
    result.trust(false, "self-check-failed");

    const aj::Value doc = parse(result);
    AURORA_TEST_CHECK_EQ(string_at(doc, "/status"), std::string{"untrusted"});
    AURORA_TEST_CHECK_EQ(string_at(doc, "/data_status"), std::string{"data-missing"});
    AURORA_TEST_CHECK_EQ(double_at(doc, "/metrics/p99_ms/value"), 1.0);
    AURORA_TEST_CHECK_FALSE(bool_at(doc, "/trust/trustworthy"));
    AURORA_TEST_CHECK_EQ(string_at(doc, "/trust/reason"), std::string{"self-check-failed"});
}

AURORA_TEST_CASE(non_finite_metric_is_rejected) {
    ab::BenchResult result{ab::make_meta("bench_unit", "unit-scene")};
    result.metric("p99_ms", std::numeric_limits<double>::quiet_NaN());
    AURORA_TEST_CHECK(result.to_json().empty());

    const std::string &dir = aurora::testing::isolation::temp_dir();
    AURORA_TEST_REQUIRE_FALSE(dir.empty());
    const std::filesystem::path path = std::filesystem::path{dir} / "non-finite.json";
    AURORA_TEST_CHECK_FALSE(result.write(path));
}

AURORA_TEST_CASE(write_persists_a_parseable_document) {
    ab::BenchResult result{ab::make_meta("bench_unit", "disk-scene")};
    result.metric("p99_ms", 2.75);

    const std::string &dir = aurora::testing::isolation::temp_dir();
    AURORA_TEST_REQUIRE_FALSE(dir.empty());
    const std::filesystem::path path = std::filesystem::path{dir} / "nested" / "result.json";
    AURORA_TEST_REQUIRE(result.write(path));

    std::ifstream in(path, std::ios::binary);
    AURORA_TEST_REQUIRE(in.good());
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto parsed = aj::parse(text);
    AURORA_TEST_REQUIRE(parsed.ok());
    AURORA_TEST_CHECK_EQ(string_at(parsed.value(), "/meta/scene"), std::string{"disk-scene"});
    AURORA_TEST_CHECK_EQ(double_at(parsed.value(), "/metrics/p99_ms/value"), 2.75);
}

AURORA_TEST_CASE(duplicate_metric_keeps_last_reading) {
    ab::BenchResult result{ab::make_meta("bench_unit", "unit-scene")};
    result.metric("p99_ms", 1.0);
    result.metric("p99_ms", 2.0);
    AURORA_TEST_CHECK_EQ(double_at(parse(result), "/metrics/p99_ms/value"), 2.0);
}

AURORA_TEST_CASE(result_file_name_carries_scene) {
    const std::filesystem::path with_scene = ab::result_path("bench_scroll", "app");
    const std::filesystem::path without_scene = ab::result_path("bench_scroll", "");
    AURORA_TEST_CHECK_EQ(with_scene.filename().string(), std::string{"bench_scroll.app.json"});
    AURORA_TEST_CHECK_EQ(without_scene.filename().string(), std::string{"bench_scroll.json"});
}

}  // namespace aurora::test_cases::utest_bench_result
