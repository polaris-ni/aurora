/// 测试类型: unit
/// 目标单元: include/aurora/core/json.h（SAX 出口 parse_sax / SaxHandler）
/// 测试说明: 事件序列正确性（标量 / 容器 / 嵌套混合文档）、转义与 Unicode 在发事件前已还原、
/// 容器闭合事件的成员计数、handler 返回 false 时提前终止且按成功返回、失败口径与 DOM 出口一致、
/// 深度上限同样生效

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/json.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_json_sax {

namespace aj = aurora::json;

using aurora::ErrorCode;

namespace {

/// @brief 把 SAX 回调序列录成可断言的文本轨迹。
class Recorder : public aj::SaxHandler {
  public:
    auto on_null() -> bool override {
        trace.emplace_back("null");
        return true;
    }

    auto on_bool(bool value) -> bool override {
        trace.emplace_back(value ? "bool:true" : "bool:false");
        return true;
    }

    auto on_int(std::int64_t value) -> bool override {
        trace.emplace_back("int:" + std::to_string(value));
        return true;
    }

    auto on_uint(std::uint64_t value) -> bool override {
        trace.emplace_back("uint:" + std::to_string(value));
        return true;
    }

    auto on_double(double value) -> bool override {
        trace.emplace_back("double:" + std::to_string(value));
        return true;
    }

    auto on_raw_number(std::string_view digits) -> bool override {
        trace.emplace_back("raw:" + std::string(digits));
        return true;
    }

    auto on_string(std::string_view decoded) -> bool override {
        trace.emplace_back("string:" + std::string(decoded));
        return true;
    }

    auto on_array_start() -> bool override {
        trace.emplace_back("array_start");
        return true;
    }

    auto on_array_end(std::size_t count) -> bool override {
        trace.emplace_back("array_end:" + std::to_string(count));
        return true;
    }

    auto on_object_start() -> bool override {
        trace.emplace_back("object_start");
        return true;
    }

    auto on_object_key(std::string_view key) -> bool override {
        trace.emplace_back("key:" + std::string(key));
        return true;
    }

    auto on_object_end(std::size_t count) -> bool override {
        trace.emplace_back("object_end:" + std::to_string(count));
        return true;
    }

    std::vector<std::string> trace;
};

/// @brief 消费到第 N 个事件后返回 false，记录实际收到的事件数。
class EarlyStop : public aj::SaxHandler {
  public:
    explicit EarlyStop(std::size_t stop_after) noexcept : stop_after_(stop_after) {}

    auto on_null() -> bool override { return tick(); }
    auto on_bool(bool /*value*/) -> bool override { return tick(); }
    auto on_int(std::int64_t /*value*/) -> bool override { return tick(); }
    auto on_uint(std::uint64_t /*value*/) -> bool override { return tick(); }
    auto on_double(double /*value*/) -> bool override { return tick(); }
    auto on_raw_number(std::string_view /*digits*/) -> bool override { return tick(); }
    auto on_string(std::string_view /*decoded*/) -> bool override { return tick(); }
    auto on_array_start() -> bool override { return tick(); }
    auto on_array_end(std::size_t /*count*/) -> bool override { return tick(); }
    auto on_object_start() -> bool override { return tick(); }
    auto on_object_key(std::string_view /*key*/) -> bool override { return tick(); }
    auto on_object_end(std::size_t /*count*/) -> bool override { return tick(); }

    std::size_t seen = 0;

  private:
    auto tick() -> bool {
        ++seen;
        return seen < stop_after_;
    }

    std::size_t stop_after_;
};

}  // namespace

// ============================================================================
// 事件序列
// ============================================================================

AURORA_TEST_CASE(records_the_event_sequence_of_a_mixed_document) {
    Recorder rec;
    const auto r = aj::parse_sax(R"({"a":1,"b":[true,null],"c":"x"})", rec);
    AURORA_TEST_REQUIRE(r.ok());

    const std::vector<std::string> expected{"object_start", "key:a",     "int:1",       "key:b",
                                            "array_start",  "bool:true", "null",        "array_end:2",
                                            "key:c",        "string:x",  "object_end:3"};
    AURORA_TEST_CHECK_EQ(rec.trace.size(), expected.size());
    for (std::size_t i = 0; i < rec.trace.size() && i < expected.size(); ++i) {
        AURORA_TEST_CHECK_EQ(rec.trace[i], expected[i]);
    }
}

AURORA_TEST_CASE(decodes_escapes_and_unicode_before_emitting) {
    Recorder rec;
    const auto r = aj::parse_sax(R"({"k":"a\nb\u0041\uD83D\uDE00"})", rec);
    AURORA_TEST_REQUIRE(r.ok());

    // "a\nbA😀"：转义与代理对在发事件前已还原成 UTF-8。
    AURORA_TEST_REQUIRE(rec.trace.size() == 4);
    AURORA_TEST_CHECK_EQ(rec.trace[2], std::string("string:a\nbA\xF0\x9F\x98\x80"));
    AURORA_TEST_CHECK_EQ(rec.trace[3], std::string("object_end:1"));
}

AURORA_TEST_CASE(reports_member_and_element_counts) {
    Recorder rec;
    const auto r = aj::parse_sax(R"([[1,2,3],{"a":1,"b":2,"c":3}])", rec);
    AURORA_TEST_REQUIRE(r.ok());

    // 内层数组 3 个元素、内层对象 3 个成员、外层数组 2 个元素。
    AURORA_TEST_CHECK_EQ(rec.trace.back(), std::string("array_end:2"));
    bool saw_three_array = false;
    bool saw_three_object = false;
    for (const auto &event : rec.trace) {
        if (event == "array_end:3") {
            saw_three_array = true;
        }
        if (event == "object_end:3") {
            saw_three_object = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(saw_three_array);
    AURORA_TEST_CHECK_TRUE(saw_three_object);
}

AURORA_TEST_CASE(emits_distinct_events_for_every_number_domain) {
    Recorder rec;
    const auto r = aj::parse_sax(R"([1,-2,3.5,18446744073709551615,1e999])", rec);
    AURORA_TEST_REQUIRE(r.ok());

    AURORA_TEST_CHECK_EQ(rec.trace[1], std::string("int:1"));
    AURORA_TEST_CHECK_EQ(rec.trace[2], std::string("int:-2"));
    AURORA_TEST_CHECK_EQ(rec.trace[3], std::string("double:3.500000"));
    AURORA_TEST_CHECK_EQ(rec.trace[4], std::string("uint:18446744073709551615"));
    AURORA_TEST_CHECK_EQ(rec.trace[5], std::string("raw:1e999"));  // 上溢保真
}

// ============================================================================
// 提前终止
// ============================================================================

AURORA_TEST_CASE(stops_when_the_handler_requests_termination) {
    EarlyStop stop{2};  // 收到第 2 个事件后即要求停止
    const auto r = aj::parse_sax(R"([1,2,3,4,5])", stop);

    // 主动终止按成功返回，且后续事件不再派发。
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(stop.seen, 2U);
}

AURORA_TEST_CASE(termination_before_the_first_scalar_is_still_success) {
    EarlyStop stop{1};  // 数组起始事件后立刻停止
    const auto r = aj::parse_sax(R"([1,2,3])", stop);

    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(stop.seen, 1U);
}

// ============================================================================
// 失败口径与配置
// ============================================================================

AURORA_TEST_CASE(reports_the_same_failures_as_the_dom_outlet) {
    Recorder rec;

    // 同一批非法输入：SAX 与 DOM 出口都判 json-parse-error。
    for (const std::string_view bad : {"", "   ", "{} {}", "\xEF\xBB\xBF{}", "[1,]", "\"\\uD800\""}) {
        const auto via_sax = aj::parse_sax(bad, rec);
        const auto via_dom = aj::parse(bad);
        AURORA_TEST_REQUIRE_FALSE(via_sax.ok());
        AURORA_TEST_REQUIRE_FALSE(via_dom.ok());
        AURORA_TEST_CHECK_EQ(via_sax.error().code_enum, ErrorCode::JsonParseError);
        AURORA_TEST_CHECK_EQ(via_dom.error().code_enum, ErrorCode::JsonParseError);
        AURORA_TEST_CHECK_EQ(via_sax.error().message, via_dom.error().message);
    }
}

AURORA_TEST_CASE(enforces_the_depth_limit) {
    Recorder rec;
    aj::ParseOptions opts;
    opts.max_depth = 3;

    const auto within = aj::parse_sax("[[[[1]]]]", rec, opts);  // 深度 4 层
    AURORA_TEST_REQUIRE_FALSE(within.ok());
    AURORA_TEST_CHECK_EQ(within.error().code_enum, ErrorCode::JsonDepthExceeded);

    const auto shallow = aj::parse_sax("[[1]]", rec, opts);
    AURORA_TEST_CHECK_TRUE(shallow.ok());
}

AURORA_TEST_CASE(accepts_a_top_level_scalar) {
    Recorder rec;
    AURORA_TEST_CHECK_TRUE(aj::parse_sax("42", rec).ok());
    AURORA_TEST_CHECK_EQ(rec.trace.back(), std::string("int:42"));
}

}  // namespace aurora::test_cases::utest_json_sax
