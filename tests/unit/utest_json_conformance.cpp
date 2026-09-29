/// 测试类型: unit
/// 目标单元: include/aurora/core/json.h（RFC 8259 合规验收，对照 nst/JSONTestSuite 语料）
/// 测试说明: 必须接受的语料全部接受、必须拒绝的语料全部拒绝、实现自定语料的行为与登记的策略
///           快照一致（漂移须显式改表）、test_transform 语料往返（值相等 + dump 幂等）、
///           语料清单与 pin 的版本一致。语料来源与更新方式见
///           tests/fixtures/json_test_suite/README.md。

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/json.h"
#include "framework/aurora_test.h"
#include "paths.h"

namespace aurora::test_cases::utest_json_conformance {

namespace aj = aurora::json;
namespace fs = std::filesystem;

namespace {

/// @brief 策略表行：语料文件名 + 期望是否被接受。
struct PolicyRow {
    const char *file;
    bool accepted;
};

/// @brief 语料根：默认仓库内 `tests/fixtures/json_test_suite`，可经环境变量覆盖。
///        覆盖口径与 golden 一致，便于把同一套用例指向本机另一份 checkout。
auto suite_root() -> fs::path {
    if (const char *override_dir = std::getenv("AURORA_JSON_TEST_SUITE_DIR")) {
        return fs::path{override_dir};
    }
    return fs::path{testing::paths::under_repo("tests/fixtures/json_test_suite")};
}

/// @brief 语料必须按字节读：其中含无效 UTF-8 序列与 NUL 字节，文本模式会破坏原貌。
auto read_bytes(const fs::path &path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief 某子目录下的 `.json` 文件名（按字节序升序，与策略表的书写顺序解耦）。
auto list_corpus(const fs::path &dir) -> std::vector<std::string> {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            names.emplace_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

/// @brief 按文件名前缀筛选语料条目。
auto with_prefix(const std::vector<std::string> &names, std::string_view prefix) -> std::vector<std::string> {
    std::vector<std::string> picked;
    std::copy_if(names.begin(), names.end(), std::back_inserter(picked),
                 [prefix](const std::string &name) { return name.starts_with(prefix); });
    return picked;
}

/// @brief 语料根下的子目录；资产随仓库分发，缺失即环境错误（不静默跳过）。
auto corpus_dir(std::string_view name) -> fs::path {
    const fs::path dir = suite_root() / fs::path{name};
    AURORA_TEST_REQUIRE_MSG(fs::is_directory(dir), "corpus directory is missing: " + dir.string() +
                                                       " (override with AURORA_JSON_TEST_SUITE_DIR)");
    return dir;
}

/// @brief 把策略表里的文件名取出来（用于与语料清单双向比对）。
template <std::size_t N>
auto table_files(const PolicyRow (&rows)[N]) -> std::vector<std::string> {
    std::vector<std::string> names;
    names.reserve(N);
    for (const auto &row : rows) {
        names.emplace_back(row.file);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

// ============================================================================
// 快照表
// ----------------------------------------------------------------------------
// 语料是 pin 住的快照（见 tests/fixtures/json_test_suite/README.md 的 commit 锚点），
// 表里的期望值就是「当前实现的有意行为」。改动任一行都必须在提交信息里说明理由 ——
// 这张表的作用是让行为漂移需要一次显式的、可评审的修改，而不是无声通过。
// ============================================================================

/// @brief 语料清单规模，随 pin 的 commit 固定。
constexpr std::size_t AURORA_MUST_ACCEPT_COUNT = 95;
constexpr std::size_t AURORA_MUST_REJECT_COUNT = 188;
constexpr std::size_t AURORA_IMPLEMENTATION_DEFINED_COUNT = 35;
constexpr std::size_t AURORA_TRANSFORM_COUNT = 22;

/// @brief 实现自定语料的处置策略：超大指数 / 超大整数走保真数字域接受且保留原文；
///        无效 UTF-8、孤立代理、非 UTF-8 编码文本与 BOM 一律拒绝。
constexpr PolicyRow AURORA_IMPLEMENTATION_DEFINED_POLICY[] = {
    {.file = "i_number_double_huge_neg_exp.json", .accepted = true},
    {.file = "i_number_huge_exp.json", .accepted = true},
    {.file = "i_number_neg_int_huge_exp.json", .accepted = true},
    {.file = "i_number_pos_double_huge_exp.json", .accepted = true},
    {.file = "i_number_real_neg_overflow.json", .accepted = true},
    {.file = "i_number_real_pos_overflow.json", .accepted = true},
    {.file = "i_number_real_underflow.json", .accepted = true},
    {.file = "i_number_too_big_neg_int.json", .accepted = true},
    {.file = "i_number_too_big_pos_int.json", .accepted = true},
    {.file = "i_number_very_big_negative_int.json", .accepted = true},
    {.file = "i_object_key_lone_2nd_surrogate.json", .accepted = false},
    {.file = "i_string_1st_surrogate_but_2nd_missing.json", .accepted = false},
    {.file = "i_string_1st_valid_surrogate_2nd_invalid.json", .accepted = false},
    {.file = "i_string_UTF-16LE_with_BOM.json", .accepted = false},
    {.file = "i_string_UTF-8_invalid_sequence.json", .accepted = false},
    {.file = "i_string_UTF8_surrogate_U+D800.json", .accepted = false},
    {.file = "i_string_incomplete_surrogate_and_escape_valid.json", .accepted = false},
    {.file = "i_string_incomplete_surrogate_pair.json", .accepted = false},
    {.file = "i_string_incomplete_surrogates_escape_valid.json", .accepted = false},
    {.file = "i_string_invalid_lonely_surrogate.json", .accepted = false},
    {.file = "i_string_invalid_surrogate.json", .accepted = false},
    {.file = "i_string_invalid_utf-8.json", .accepted = false},
    {.file = "i_string_inverted_surrogates_U+1D11E.json", .accepted = false},
    {.file = "i_string_iso_latin_1.json", .accepted = false},
    {.file = "i_string_lone_second_surrogate.json", .accepted = false},
    {.file = "i_string_lone_utf8_continuation_byte.json", .accepted = false},
    {.file = "i_string_not_in_unicode_range.json", .accepted = false},
    {.file = "i_string_overlong_sequence_2_bytes.json", .accepted = false},
    {.file = "i_string_overlong_sequence_6_bytes.json", .accepted = false},
    {.file = "i_string_overlong_sequence_6_bytes_null.json", .accepted = false},
    {.file = "i_string_truncated-utf-8.json", .accepted = false},
    {.file = "i_string_utf16BE_no_BOM.json", .accepted = false},
    {.file = "i_string_utf16LE_no_BOM.json", .accepted = false},
    {.file = "i_structure_500_nested_arrays.json", .accepted = true},
    {.file = "i_structure_UTF-8_BOM_empty_object.json", .accepted = false},
};

/// @brief `test_transform` 语料的处置快照：可解析者必须往返自洽，其余为正确拒绝
///        （含无效码点的字符串语料）。
constexpr PolicyRow AURORA_TRANSFORM_POLICY[] = {
    {.file = "number_-9223372036854775808.json", .accepted = true},
    {.file = "number_-9223372036854775809.json", .accepted = true},
    {.file = "number_1.0.json", .accepted = true},
    {.file = "number_1.000000000000000005.json", .accepted = true},
    {.file = "number_1000000000000000.json", .accepted = true},
    {.file = "number_10000000000000000999.json", .accepted = true},
    {.file = "number_1e-999.json", .accepted = true},
    {.file = "number_1e6.json", .accepted = true},
    {.file = "number_9223372036854775807.json", .accepted = true},
    {.file = "number_9223372036854775808.json", .accepted = true},
    {.file = "object_key_nfc_nfd.json", .accepted = true},
    {.file = "object_key_nfd_nfc.json", .accepted = true},
    {.file = "object_same_key_different_values.json", .accepted = true},
    {.file = "object_same_key_same_value.json", .accepted = true},
    {.file = "object_same_key_unclear_values.json", .accepted = true},
    {.file = "string_1_escaped_invalid_codepoint.json", .accepted = false},
    {.file = "string_1_invalid_codepoint.json", .accepted = false},
    {.file = "string_2_escaped_invalid_codepoints.json", .accepted = false},
    {.file = "string_2_invalid_codepoints.json", .accepted = false},
    {.file = "string_3_escaped_invalid_codepoints.json", .accepted = false},
    {.file = "string_3_invalid_codepoints.json", .accepted = false},
    {.file = "string_with_escaped_NULL.json", .accepted = true},
};

// ============================================================================
// 用例
// ============================================================================

AURORA_TEST_CASE(matches_the_pinned_corpus_inventory) {
    const auto parsing = list_corpus(corpus_dir("test_parsing"));
    AURORA_TEST_CHECK_EQ(with_prefix(parsing, "y_").size(), AURORA_MUST_ACCEPT_COUNT);
    AURORA_TEST_CHECK_EQ(with_prefix(parsing, "n_").size(), AURORA_MUST_REJECT_COUNT);
    AURORA_TEST_CHECK_EQ(with_prefix(parsing, "i_").size(), AURORA_IMPLEMENTATION_DEFINED_COUNT);
    for (const auto &name : parsing) {
        const bool known_prefix = name.starts_with("y_") || name.starts_with("n_") || name.starts_with("i_");
        if (!known_prefix) {
            AURORA_TEST_FAIL("unrecognised corpus entry (prefix must be y_ / n_ / i_): " + name);
        }
    }
    AURORA_TEST_CHECK_EQ(list_corpus(corpus_dir("test_transform")).size(), AURORA_TRANSFORM_COUNT);
}

AURORA_TEST_CASE(accepts_the_whole_must_accept_corpus) {
    const fs::path dir = corpus_dir("test_parsing");
    const auto names = with_prefix(list_corpus(dir), "y_");
    AURORA_TEST_REQUIRE_EQ(names.size(), AURORA_MUST_ACCEPT_COUNT);
    for (const auto &name : names) {
        const auto parsed = aj::parse(read_bytes(dir / name));
        if (!parsed.ok()) {
            AURORA_TEST_FAIL(name + " must be accepted, but was rejected: " + parsed.error().code);
        }
    }
}

AURORA_TEST_CASE(rejects_the_whole_must_reject_corpus) {
    const fs::path dir = corpus_dir("test_parsing");
    const auto names = with_prefix(list_corpus(dir), "n_");
    AURORA_TEST_REQUIRE_EQ(names.size(), AURORA_MUST_REJECT_COUNT);
    for (const auto &name : names) {
        if (aj::parse(read_bytes(dir / name)).ok()) {
            AURORA_TEST_FAIL(name + " must be rejected, but was accepted");
        }
    }
}

AURORA_TEST_CASE(keeps_the_recorded_policy_for_implementation_defined_inputs) {
    const fs::path dir = corpus_dir("test_parsing");
    const auto names = with_prefix(list_corpus(dir), "i_");
    AURORA_TEST_REQUIRE_EQ(names.size(), AURORA_IMPLEMENTATION_DEFINED_COUNT);
    AURORA_TEST_REQUIRE_EQ(std::size(AURORA_IMPLEMENTATION_DEFINED_POLICY), AURORA_IMPLEMENTATION_DEFINED_COUNT);
    AURORA_TEST_CHECK_MSG(table_files(AURORA_IMPLEMENTATION_DEFINED_POLICY) == names,
                          "policy table does not match the corpus inventory (a corpus entry was added or removed)");
    for (const auto &row : AURORA_IMPLEMENTATION_DEFINED_POLICY) {
        const bool accepted = aj::parse(read_bytes(dir / row.file)).ok();
        if (accepted != row.accepted) {
            AURORA_TEST_FAIL(std::string{row.file} + " changed policy: recorded=" +
                             (row.accepted ? "accept" : "reject") + " actual=" + (accepted ? "accept" : "reject") +
                             " (update the table only if the new behaviour is intended)");
        }
    }
}

AURORA_TEST_CASE(round_trips_the_transform_corpus_through_dump) {
    const fs::path dir = corpus_dir("test_transform");
    const auto names = list_corpus(dir);
    AURORA_TEST_REQUIRE_EQ(names.size(), AURORA_TRANSFORM_COUNT);
    AURORA_TEST_REQUIRE_EQ(std::size(AURORA_TRANSFORM_POLICY), AURORA_TRANSFORM_COUNT);
    AURORA_TEST_CHECK_MSG(table_files(AURORA_TRANSFORM_POLICY) == names,
                          "transform policy table does not match the corpus inventory");
    for (const auto &row : AURORA_TRANSFORM_POLICY) {
        const auto once = aj::parse(read_bytes(dir / row.file));
        if (once.ok() != row.accepted) {
            AURORA_TEST_FAIL(std::string{row.file} + " changed policy: recorded=" +
                             (row.accepted ? "accept" : "reject") + " actual=" + (once.ok() ? "accept" : "reject"));
            continue;
        }
        if (!once.ok()) {
            continue;
        }
        const auto text = aj::dump(once.value());
        if (!text.ok()) {
            AURORA_TEST_FAIL(std::string{row.file} + " parsed but could not be serialised: " + text.error().code);
            continue;
        }
        const auto twice = aj::parse(text.value());
        if (!twice.ok()) {
            AURORA_TEST_FAIL(std::string{row.file} + " dump output failed to parse back: " + twice.error().code);
            continue;
        }
        AURORA_TEST_CHECK_MSG(once.value() == twice.value(),
                              std::string{row.file} + " lost information across parse -> dump -> parse");
        const auto again = aj::dump(twice.value());
        AURORA_TEST_CHECK_MSG(again.ok() && again.value() == text.value(),
                              std::string{row.file} + " dump is not idempotent");
    }
}

}  // namespace aurora::test_cases::utest_json_conformance
