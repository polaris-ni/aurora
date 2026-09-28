/// 测试类型: unit
/// 目标单元: include/aurora/core/json.h（与既有 JSON 库的语义对拍）
/// 测试说明: 共存期正确性锚——同一批语料分别经本库 parse→dump 与既有库 parse，把本库产出的文本
/// 交回既有库解析后应逐值等价；数值域归属与「既有库产出文本」的反向兼容同样如此。值语义
/// （类型判别 / 数值域 / 字符串解码 / 结构）任一处偏差都会在此暴露。
/// ⚠️ 本文件是迁移过渡脚手架（README 无引用，仅供本轮对拍）：波 4 删除 nlohmann 时，连同
///    CMake 侧的 AURORA_HAVE_NLOHMANN 开关一并移除。

#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/json.h"
#include "framework/aurora_test.h"

#ifdef AURORA_HAVE_NLOHMANN
#include <nlohmann/json.hpp>
#endif

namespace aurora::test_cases::utest_json_compare {

namespace aj = aurora::json;

namespace {

/// @brief 共用语料：覆盖标量 / 容器 / 嵌套 / 转义 / 大整数 / 浮点。
constexpr std::string_view kCorpus[] = {
    R"({"a":1,"b":[true,null,3.5],"c":"x"})",
    R"([1,2,3])",
    R"({"i":-42,"u":18446744073709551615})",
    R"({"flat":{},"empty":[]})",
    R"("scalar text")",
    R"(3.5)",
    R"({"nested":{"deep":[{"k":"v"}]}})",
    R"({"esc":"line\nbreak\ttab\"quote\"","u":"\u0041"})",
};

}  // namespace

AURORA_TEST_CASE(matches_the_incumbent_library_on_a_shared_corpus) {
#ifdef AURORA_HAVE_NLOHMANN
    for (const std::string_view text : kCorpus) {
        const auto ours = aj::parse(text);
        AURORA_TEST_REQUIRE(ours.ok());
        const auto dumped = aj::dump(ours.value());
        AURORA_TEST_REQUIRE(dumped.ok());

        const auto incumbent = nlohmann::json::parse(text, nullptr, false);
        AURORA_TEST_REQUIRE_FALSE(incumbent.is_discarded());

        // 对拍锚：本库 dump 出的文本交回既有库解析，须与它直接解析原文等价。
        // nlohmann 的对象相等判定与键序无关，故插入序 vs 字母序不构成差异。
        const auto reparsed = nlohmann::json::parse(dumped.value(), nullptr, false);
        AURORA_TEST_REQUIRE_FALSE(reparsed.is_discarded());
        AURORA_TEST_CHECK(reparsed == incumbent);
    }
#else
    AURORA_TEST_SKIP("nlohmann 语义对拍需要 AURORA_HAVE_NLOHMANN 宏（波 4 后移除）");
#endif
}

AURORA_TEST_CASE(matches_the_incumbent_library_across_number_domains) {
#ifdef AURORA_HAVE_NLOHMANN
    // 三判别（int64 / uint64 / double）的域归属须与既有库一致，否则往返后文本会漂移。
    constexpr std::string_view kNumbers[] = {"0",  "-0",    "1",     "-1",  "9223372036854775807",
                                             "18446744073709551615", "1.5", "-1.5", "1e10"};

    for (const std::string_view text : kNumbers) {
        const auto ours = aj::parse(text);
        AURORA_TEST_REQUIRE(ours.ok());
        const auto dumped = aj::dump(ours.value());
        AURORA_TEST_REQUIRE(dumped.ok());

        const auto reparsed = nlohmann::json::parse(dumped.value(), nullptr, false);
        AURORA_TEST_REQUIRE_FALSE(reparsed.is_discarded());
        AURORA_TEST_CHECK(reparsed == nlohmann::json::parse(text, nullptr, false));
    }
#else
    AURORA_TEST_SKIP("nlohmann 数值对拍需要 AURORA_HAVE_NLOHMANN 宏（波 4 后移除）");
#endif
}

AURORA_TEST_CASE(accepts_text_produced_by_the_incumbent_library) {
#ifdef AURORA_HAVE_NLOHMANN
    // 反向：既有库 dump 出的文本（含其字母序键排列）本库须能解析。本库的相等语义是插入序
    // 敏感的，故不比 Value 本身，而是比「双方产出文本交既有库解析」的结果。
    for (const std::string_view text : kCorpus) {
        const auto incumbent = nlohmann::json::parse(text, nullptr, false);
        AURORA_TEST_REQUIRE_FALSE(incumbent.is_discarded());

        const auto ours = aj::parse(incumbent.dump());
        AURORA_TEST_REQUIRE(ours.ok());
        const auto dumped = aj::dump(ours.value());
        AURORA_TEST_REQUIRE(dumped.ok());

        const auto reparsed = nlohmann::json::parse(dumped.value(), nullptr, false);
        AURORA_TEST_REQUIRE_FALSE(reparsed.is_discarded());
        AURORA_TEST_CHECK(reparsed == incumbent);
    }
#else
    AURORA_TEST_SKIP("nlohmann 反向对拍需要 AURORA_HAVE_NLOHMANN 宏（波 4 后移除）");
#endif
}

}  // namespace aurora::test_cases::utest_json_compare
