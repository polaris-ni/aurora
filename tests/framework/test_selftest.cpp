// 测试框架内建自检：用 synthetic 用例验证断言家族、值打印、追踪、组织设施与退出码协议。
// 探针不进注册表，故 `registry_integrity` 的清单比对不受影响。
#include "test_selftest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "aurora/core/enums.h"
#include "aurora_test.h"
#include "reporter.h"

namespace aurora::testing {

// 值打印探针类型：刻意放在具名命名空间，令期望的类型名文本在 GCC / Clang / MSVC 下一致。

namespace {
/// @brief 既不可流输出、也不可迭代的类型：验证 `<unprintable 类型名>` 兜底。
struct SelftestUnprintable {
    [[maybe_unused]] int payload = 0;
};

/// @brief 定制点验证类型：由下方 `ValuePrinter` 特化接管输出。
struct SelftestCustom {
    int code = 0;
};

}  // namespace

/// @brief `ValuePrinter` 定制点生效验证：测试侧显式特化即接管该类型的实际值渲染。
template <>
struct ValuePrinter<SelftestCustom> {
    static auto print(const SelftestCustom& value) -> std::string {
        return "Custom{" + std::to_string(value.code) + "}";
    }
};

namespace {

using aurora::testing::print_value;

/// @brief 探针用例名（返回指向字面量的视图：TestCase 以 string_view 指向静态存储期文本）。
[[nodiscard]] auto probe_suite() -> std::string_view { return "assertion_probe"; }

/// @brief 探针共享的「是否执行到语句末尾」标记（致命断言须阻止其后的语句）。
auto probe_reached() -> bool& {
    static bool value = false;
    return value;
}

/// @brief 探针共享的操作数副作用计数（验证「两侧各只求值一次」）。
auto probe_operand() -> int& {
    static int value = 0;
    return value;
}

/// @brief 指向静态 int 的指针：指针断言族的「非空」一侧（探针体是无捕获 lambda，故须函数提供）。
[[nodiscard]] auto probe_live_pointer() -> int* {
    static int storage = 7;
    return &storage;
}

/// @brief 两个内容相同、地址不同的静态字符数组：验证 STREQ 按内容而非按指针判定。
[[nodiscard]] auto probe_text_a() -> char* {
    static char storage[4] = {'a', 'b', 'c', '\0'};
    return storage;
}

[[nodiscard]] auto probe_text_b() -> char* {
    static char storage[4] = {'a', 'b', 'c', '\0'};
    return storage;
}

/// @brief 容器断言探针的元素来源（同样的理由：探针体是无捕获 lambda）。
[[nodiscard]] auto probe_vector_small() -> std::vector<int> { return {1, 2}; }

[[nodiscard]] auto probe_vector_large() -> std::vector<int> { return {1, 2, 3}; }

[[nodiscard]] auto probe_vector_other() -> std::vector<int> { return {1, 3}; }

/// @brief 构造一个不注册的探针用例。
[[nodiscard]] auto make_probe(TestBody body) -> TestCase {
    TestCase test_case;
    test_case.suite = probe_suite();
    test_case.case_name = probe_suite();
    test_case.file = "<selftest>";
    test_case.line = 0;
    test_case.body = body;
    return test_case;
}

/// @brief 执行探针：每次先复位 reached 标记，再走真实的 run_case 隔离路径。
[[nodiscard]] auto probe(TestBody body) -> CaseResult {
    probe_reached() = false;
    const auto test_case = make_probe(body);
    return run_case(test_case);
}

/// @brief 自检断言；违反时打印并返回 false（自检自身不得依赖被检设施）。
auto expect(bool condition, const char* what) -> bool {
    if (condition) {
        std::printf("[selftest] ok: %s\n", what);
        return true;
    }
    std::printf("[selftest] FAILED: %s\n", what);
    return false;
}

/// @brief 首条失败记录（无失败时返回空串）。
[[nodiscard]] auto first_failure(const CaseResult& result) -> std::string {
    return result.failures.empty() ? std::string{} : result.failures.front().message;
}

/// @brief 非致命断言的通过向：用例通过且零失败。
auto expect_passes(TestBody body, const char* what) -> bool {
    const auto result = probe(body);
    return expect(result.status == TestStatus::Passed && result.failures.empty(), what);
}

/// @brief 断言的失败向：用例失败且恰好记录一条失败。
auto expect_fails_once(TestBody body, const char* what) -> bool {
    const auto result = probe(body);
    return expect(result.status == TestStatus::Failed && result.failures.size() == 1, what);
}

/// @brief 断言的失败向 + 诊断文本含指定片段（验证「失败即打印实际值」）。
auto expect_fails_with(TestBody body, std::string_view needle, const char* what) -> bool {
    const auto result = probe(body);
    const auto message = first_failure(result);
    return expect(
        result.status == TestStatus::Failed && result.failures.size() == 1 && message.find(needle) != std::string::npos,
        what);
}

/// @brief 致命断言的通过向：用例通过，且其后的语句照常执行。
auto expect_fatal_passes(TestBody body, const char* what) -> bool {
    const auto result = probe(body);
    return expect(result.status == TestStatus::Passed && probe_reached(), what);
}

/// @brief 致命断言的失败向：记一条失败，且其后的语句不得执行。
auto expect_fatal_aborts(TestBody body, const char* what) -> bool {
    const auto result = probe(body);
    return expect(result.status == TestStatus::Failed && result.failures.size() == 1 && !probe_reached(), what);
}

// 探针宏必须吞下「任意语句」，无法降级为 constexpr 变参模板（语句不是表达式），
// 故此处结构性豁免 cppcoreguidelines-macro-usage。
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
/// @brief 把断言语句包成探针用例体（无捕获 lambda 可隐式转函数指针）。
#define AURORA_TEST_PROBE(...) (+[]() -> void { __VA_ARGS__; })

/// @brief 致命断言探针体：断言之后紧跟一行标记，用于判定「是否真的终止了用例」。
#define AURORA_TEST_FATAL_PROBE(...) AURORA_TEST_PROBE(__VA_ARGS__; probe_reached() = true;)
// NOLINTEND(cppcoreguidelines-macro-usage)

// ---- 组织设施探针（fixture / 参数化 / 匹配器）----

/// @brief 生命周期探针的计数器（函数局部 static：避免非 const 全局/静态成员）。
struct LifecycleCounters {
    int setups = 0;
    int teardowns = 0;
    int bodies = 0;
    int teardown_seen_in_body = 0;
};

auto lifecycle_counters() -> LifecycleCounters& {
    static LifecycleCounters counters;
    return counters;
}

/// @brief 生命周期探针 fixture：记录 SetUp / TearDown 次序，供断言核对。
class LifecycleFixture : public Fixture {
  protected:
    auto SetUp() -> void override { ++lifecycle_counters().setups; }

    auto TearDown() -> void override { ++lifecycle_counters().teardowns; }
};

/// @brief 把 CaseBase 具体化：case_body 即用户侧「用例体」。
class LifecycleCase : public detail::CaseBase<LifecycleFixture> {
  protected:
    auto case_body() -> void override {
        auto& counters = lifecycle_counters();
        ++counters.bodies;
        // 用例体执行期间尚不应看到清理动作。
        counters.teardown_seen_in_body = (counters.teardowns == 0) ? 1 : 0;
    }
};

/// @brief 用例体抛出致命断言：验证异常路径同样执行 TearDown。
class AbortCase : public detail::CaseBase<LifecycleFixture> {
  protected:
    auto case_body() -> void override { AURORA_TEST_REQUIRE(false); }
};

/// @brief 值参数化探针 fixture（仅需 ParamType 与 param()）。
class ParamFixture : public TestWithParam<int> {};

/// @brief 记录参数化用例体实际看到的取值。
auto observed_param() -> int& {
    static int value = 0;
    return value;
}

/// @brief 值参数化探针用例类：序号 -> 取值表 -> 槽位 -> param()。
class ParamCase : public detail::CaseBase<ParamFixture> {
  protected:
    auto case_body() -> void override { observed_param() = this->param(); }
};

/// @brief 参数化探针的用例体入口（与 AURORA_TEST_P 生成的 shim 同形）。
auto param_probe_run_at(std::size_t index) -> void {
    detail::set_current_param<ParamFixture>(detail::param_at<ParamFixture>(index));
    detail::run_case_instance<ParamCase>();
}

/// @brief 参数化探针的取值表（具名函数，免把带逗号的花括号列表写进调用）。
[[nodiscard]] auto probe_param_values() -> std::vector<int> { return {2, 3}; }

/// @brief 展开产物的可读快照：指定套件下的用例名列表。
[[nodiscard]] auto probe_case_names(std::string_view suite) -> std::vector<std::string> {
    std::vector<std::string> names;
    for (const auto* test_case : TestRegistry::instance().cases()) {
        if (test_case->suite == suite) {
            names.emplace_back(test_case->case_name);
        }
    }
    return names;
}

/// @brief 组织设施自检。
auto selftest_organization() -> bool {
    bool ok = true;

    // 生命周期：SetUp -> 用例体 -> TearDown，次序可观测。
    lifecycle_counters() = {};
    probe(+[]() -> void { detail::run_case_instance<LifecycleCase>(); });
    ok = expect(lifecycle_counters().setups == 1 && lifecycle_counters().bodies == 1 &&
                    lifecycle_counters().teardowns == 1 && lifecycle_counters().teardown_seen_in_body == 1,
                "fixture lifecycle runs SetUp -> body -> TearDown") &&
         ok;

    // 用例体抛致命断言时，TearDown 仍恰好执行一次，异常继续上抛给 runner。
    lifecycle_counters() = {};
    const auto aborted = probe(+[]() -> void { detail::run_case_instance<AbortCase>(); });
    ok = expect(aborted.status == TestStatus::Failed && lifecycle_counters().teardowns == 1,
                "fatal assertion inside a fixture still runs TearDown") &&
         ok;

    // 值参数化：槽位 -> 构造期快照 -> param()。
    detail::set_current_param<ParamFixture>(7);
    const auto parametrised = probe(+[]() -> void { detail::run_case_instance<ParamCase>(); });
    ok = expect(parametrised.status == TestStatus::Passed && observed_param() == 7,
                "TestWithParam::param() sees the injected value") &&
         ok;

    // 展开器：每个取值一条独立用例名；套件名沿用 TEST_P 所在文件。
    // ⚠️ registrar 必须是静态存储期：注册表持有其节点地址，函数局部对象析构即悬垂。
    static detail::ParamFamilyRegistrar family_registrar{probe_suite(),      "ParamFixture", "area", "<selftest>", 1,
                                                         &param_probe_run_at};
    const auto before = probe_case_names(probe_suite()).size();
    detail::register_instantiation<ParamFixture>("edges", "ParamFixture", probe_param_values(),
                                                 detail::DefaultParamName{});
    const auto names = probe_case_names(probe_suite());
    ok = expect(names.size() == before + 2, "one case per value after expansion") && ok;
    ok = expect(std::ranges::find(names, "edges/ParamFixture_area/0") != names.end() &&
                    std::ranges::find(names, "edges/ParamFixture_area/1") != names.end(),
                "expanded case names carry the prefix and the index") &&
         ok;

    // 展开出的用例可执行，且各按自己的序号取值。
    auto expanded = 0;
    auto values_ok = true;
    for (const auto* test_case : TestRegistry::instance().cases()) {
        if (test_case->suite != probe_suite() || test_case->param_body == nullptr) {
            continue;
        }
        const auto result = run_case(*test_case);
        ++expanded;
        values_ok = values_ok && result.status == TestStatus::Passed &&
                    observed_param() == 2 + static_cast<int>(test_case->param_index);
    }
    ok = expect(expanded == 2 && values_ok, "expanded cases run with their own value") && ok;

    // finalize 幂等：钩子只执行一次（首次展开全集，再次调用不得有任何变化）。
    TestRegistry::instance().finalize();
    const auto count_before = TestRegistry::instance().cases().size();
    TestRegistry::instance().finalize();
    ok = expect(TestRegistry::instance().cases().size() == count_before, "finalize is idempotent") && ok;

    // 匹配器：正反两向与描述文本。
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(probe_vector_large(), matchers::size_is(3))),
                       "CHECK_THAT pass") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(probe_vector_large(), matchers::size_is(2))),
                           "Expected: has size 2", "CHECK_THAT fail carries the matcher description") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(4, matchers::all_of(matchers::ge(2), matchers::le(5)))),
                       "all_of composes") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(4, matchers::any_of(matchers::lt(0), matchers::gt(3)))),
                       "any_of composes") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(4, matchers::negated(matchers::eq(5)))),
                       "negated composes") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(probe_vector_large(), matchers::each(matchers::gt(0)))),
                       "each over container elements") &&
         ok;
    ok = expect_fails_with(
             AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(probe_vector_small(), matchers::each(matchers::gt(1)))),
             "every element is", "each fail describes the sub-matcher") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(std::string{"aurora"}, matchers::str_eq("aurora"))),
                       "str_eq matches by content") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THAT(std::string{"aurora"}, matchers::has_substr("ora"))),
                       "has_substr") &&
         ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_THAT(4, matchers::eq(5))),
                             "REQUIRE_THAT aborts the case") &&
         ok;
    return ok;
}

// ---- 自检分区 ----

/// @brief 执行 / 注册 / 退出码协议 / 异常隔离。
auto selftest_execution() -> bool {
    // 套件名推导：constexpr，编译期即校验（含 Windows 路径分隔符）
    static_assert(suite_from_path("tests/unit/utest_color.cpp") == "utest_color");
    static_assert(suite_from_path(R"(D:\repo\tests\unit\utest_color.cpp)") == "utest_color");
    static_assert(suite_from_path("utest_color") == "utest_color");
    // 退出码协议位固定，改动须同步 BUILD_OPTIONS.md 与计划文档
    static_assert(static_cast<int>(ExitCode::AllPassed) == 0);
    static_assert(static_cast<int>(ExitCode::HasFailures) == 1);
    static_assert(static_cast<int>(ExitCode::UsageOrNoMatch) == 2);
    static_assert(static_cast<int>(ExitCode::Timeout) == 3);

    const std::vector<TestCase> synthetic {
        make_probe(AURORA_TEST_PROBE(AURORA_TEST_CHECK(1 + 1 == 2); AURORA_TEST_REQUIRE(true))),
            make_probe(AURORA_TEST_PROBE(AURORA_TEST_CHECK(1 + 1 == 3))),
            make_probe(AURORA_TEST_PROBE(AURORA_TEST_SKIP("selftest demo skip"))),
    };
    std::vector<const TestCase*> view;
    view.reserve(synthetic.size());
    for (const auto& test_case : synthetic) {
        view.push_back(&test_case);
    }

    std::vector<CaseResult> results;
    results.reserve(view.size());
    for (const auto* test_case : view) {
        results.push_back(run_case(*test_case));
    }
    const auto summary = summarize(results);

    bool ok = true;
    ok = expect(summary.total == 3, "3 probe cases executed") && ok;
    ok = expect(summary.passed == 1, "1 passed") && ok;
    ok = expect(summary.failed == 1, "1 failed") && ok;
    ok = expect(summary.skipped == 1, "1 skipped") && ok;
    ok =
        expect(exit_code_for(summary) == static_cast<int>(ExitCode::HasFailures), "exit code 1 when any case failed") &&
        ok;
    ok = expect(exit_code_for(RunSummary{.total = 1, .passed = 1}) == static_cast<int>(ExitCode::AllPassed),
                "exit code 0 when all passed") &&
         ok;
    ok = expect(exit_code_for(RunSummary{.total = 1, .skipped = 1}) == static_cast<int>(ExitCode::AllPassed),
                "skipped does not count as failure") &&
         ok;

    // 筛选语义：套件精确匹配 + 全名子串下钻
    ok = expect(select_cases(view, probe_suite(), "").size() == 3, "suite filter selects all 3") && ok;
    ok = expect(select_cases(view, "no_such_suite", "").empty(), "suite filter miss yields empty") && ok;
    ok = expect(select_cases(view, "", "assertion_probe").size() == 3, "name filter matches full name substring") && ok;
    ok = expect(select_cases(view, "", "no_such_case").empty(), "name filter miss yields empty") && ok;

    // 上下文必须在用例结束后复位（防止相邻用例互相污染）
    ok = expect(current_context() == nullptr, "context slot restored after each case") && ok;

    // 未捕获的标准异常必须被隔离为失败，而非终止整个 runner
    const auto thrown = probe(AURORA_TEST_PROBE(throw std::runtime_error{"boom"}));
    ok = expect(thrown.status == TestStatus::Failed && thrown.failures.size() == 1 &&
                    first_failure(thrown).find("boom") != std::string::npos,
                "unexpected exception -> single failure carrying what()") &&
         ok;

    const auto skipped = probe(AURORA_TEST_PROBE(AURORA_TEST_SKIP("not on this platform")));
    ok = expect(skipped.status == TestStatus::Skipped && skipped.skip_reason == "not on this platform",
                "SKIP records reason and counts as skipped") &&
         ok;
    return ok;
}

/// @brief 布尔断言与无条件失败原语。
auto selftest_bool_assertions() -> bool {
    bool ok = true;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK(2 + 2 == 4)), "CHECK pass") && ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK(2 + 2 == 5)), "is false",
                           "CHECK fail prints the expression") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_MSG(2 + 2 == 5, "arithmetic broke")), "arithmetic broke",
                           "CHECK_MSG carries the custom message") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_TRUE(2 + 2 == 5)), "Actual: false",
                           "CHECK_TRUE prints Actual / Expected") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_TRUE(2 + 2 == 4)), "CHECK_TRUE pass") && ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_FALSE(2 + 2 == 4)), "Actual: true",
                           "CHECK_FALSE prints Actual / Expected") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_FALSE(2 + 2 == 5)), "CHECK_FALSE pass") && ok;

    ok = expect_fatal_passes(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE(2 + 2 == 4)),
                             "REQUIRE pass continues the case") &&
         ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE(2 + 2 == 5)),
                             "REQUIRE failure aborts the case") &&
         ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_MSG(false, "stop now")),
                             "REQUIRE_MSG aborts the case") &&
         ok;
    ok =
        expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_TRUE(false)), "REQUIRE_TRUE aborts the case") &&
        ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_FALSE(true)),
                             "REQUIRE_FALSE aborts the case") &&
         ok;

    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_FAIL("manual failure")), "manual failure",
                           "FAIL records the given message") &&
         ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_FAIL_FATAL("stop")), "FAIL_FATAL aborts the case") &&
         ok;
    return ok;
}

/// @brief 关系比较、浮点近邻与容器比较。
auto selftest_comparison_assertions() -> bool {
    bool ok = true;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_EQ(2, 2)), "CHECK_EQ pass") && ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_EQ(2, 3)), "Which is: 2 vs 3",
                           "CHECK_EQ fail prints both values") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NE(2, 3)), "CHECK_NE pass") && ok;
    ok = expect_fails_once(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NE(2, 2)), "CHECK_NE fail") && ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_LT(1, 2)), "CHECK_LT pass") && ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_LT(2, 1)), "2 < 1", "CHECK_LT fail") && ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_LE(2, 2)), "CHECK_LE pass") && ok;
    ok = expect_fails_once(AURORA_TEST_PROBE(AURORA_TEST_CHECK_LE(3, 2)), "CHECK_LE fail") && ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_GT(3, 2)), "CHECK_GT pass") && ok;
    ok = expect_fails_once(AURORA_TEST_PROBE(AURORA_TEST_CHECK_GT(2, 3)), "CHECK_GT fail") && ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_GE(3, 3)), "CHECK_GE pass") && ok;
    ok = expect_fails_once(AURORA_TEST_PROBE(AURORA_TEST_CHECK_GE(2, 3)), "CHECK_GE fail") && ok;

    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9)),
                       "CHECK_NEAR pass within tolerance") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NEAR(1.0, 1.02, 0.001)),
                           "Diff:", "CHECK_NEAR fail prints diff and tolerance") &&
         ok;

    ok = expect_fatal_passes(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_EQ(2, 2)), "REQUIRE_EQ pass continues") && ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_EQ(std::size_t{1}, std::size_t{2})),
                             "REQUIRE_EQ aborts the case") &&
         ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_NEAR(1.0, 2.0, 0.1)),
                             "REQUIRE_NEAR aborts the case") &&
         ok;

    // 操作数只求值一次：失败向里自增只发生一次。
    probe_operand() = 0;
    const auto once = probe(AURORA_TEST_PROBE(AURORA_TEST_CHECK_EQ(++probe_operand(), 9)));
    ok = expect(once.failures.size() == 1 && probe_operand() == 1 &&
                    first_failure(once).find("Which is: 1 vs 9") != std::string::npos,
                "operands evaluated exactly once") &&
         ok;

    // 容器比较：两侧元素逐个打印（实参走函数，避免花括号里的逗号被预处理器切成多个宏参数）。
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_EQ(probe_vector_large(), probe_vector_large())),
                       "CHECK_EQ on containers pass") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_EQ(probe_vector_small(), probe_vector_other())),
                           "Which is: { 1, 2 } vs { 1, 3 }", "CHECK_EQ on containers prints elements") &&
         ok;
    return ok;
}

/// @brief 字符串内容比较（C 数组 / std::string 混用同样按内容判定）。
auto selftest_string_assertions() -> bool {
    bool ok = true;
    // 两个独立数组内容相同：按指针比较会误判，按内容比较必须通过。
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_STREQ(probe_text_a(), probe_text_b())),
                       "CHECK_STREQ compares content, not address") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_STREQ(std::string{"abc"}, "abc")),
                       "CHECK_STREQ mixes std::string and C string") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_STREQ("abc", "abd")), R"(Which is: "abc" vs "abd")",
                           "CHECK_STREQ fail prints quoted values") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_STRNE("abc", "abd")), "CHECK_STRNE pass") && ok;
    ok = expect_fails_once(AURORA_TEST_PROBE(AURORA_TEST_CHECK_STRNE("abc", "abc")), "CHECK_STRNE fail") && ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_STRCASEEQ("AbC", "abc")),
                       "CHECK_STRCASEEQ ignores ASCII case") &&
         ok;
    ok = expect_fails_once(AURORA_TEST_PROBE(AURORA_TEST_CHECK_STRCASEEQ("AbC", "xyz")), "CHECK_STRCASEEQ fail") && ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_STRCASENE("AbC", "xyz")), "CHECK_STRCASENE pass") && ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_STREQ("a", "b")),
                             "REQUIRE_STREQ aborts the case") &&
         ok;
    return ok;
}

/// @brief 异常判定族。
auto selftest_exception_assertions() -> bool {
    bool ok = true;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THROW(throw std::runtime_error{"bad"}, std::runtime_error)),
                       "CHECK_THROW matches the expected type") &&
         ok;
    ok = expect_fails_with(
             AURORA_TEST_PROBE(AURORA_TEST_CHECK_THROW(throw std::logic_error{"nope"}, std::runtime_error)),
             "std::logic_error", "CHECK_THROW fail names the actual exception type") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_THROW(static_cast<void>(0), std::runtime_error)),
                           "did not throw", "CHECK_THROW fail when nothing is thrown") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NO_THROW(static_cast<void>(42))), "CHECK_NO_THROW pass") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NO_THROW(throw std::runtime_error{"oops"})),
                           "expected no exception", "CHECK_NO_THROW fail reports the thrown exception") &&
         ok;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_ANY_THROW(throw 7)),
                       "CHECK_ANY_THROW accepts non-standard throws") &&
         ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_ANY_THROW(static_cast<void>(0))), "did not throw",
                           "CHECK_ANY_THROW fail when nothing is thrown") &&
         ok;
    ok = expect_fatal_aborts(
             AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_THROW(static_cast<void>(0), std::runtime_error)),
             "REQUIRE_THROW aborts the case") &&
         ok;
    return ok;
}

/// @brief 指针空判定族。
auto selftest_pointer_assertions() -> bool {
    bool ok = true;
    ok = expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NULL(static_cast<int*>(nullptr))), "CHECK_NULL pass") && ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NULL(probe_live_pointer())), "is not null",
                           "CHECK_NULL fail wording") &&
         ok;
    ok =
        expect_passes(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NOT_NULL(probe_live_pointer())), "CHECK_NOT_NULL pass") && ok;
    ok = expect_fails_with(AURORA_TEST_PROBE(AURORA_TEST_CHECK_NOT_NULL(static_cast<int*>(nullptr))), "is null",
                           "CHECK_NOT_NULL fail wording") &&
         ok;
    ok = expect_fatal_aborts(AURORA_TEST_FATAL_PROBE(AURORA_TEST_REQUIRE_NOT_NULL(static_cast<int*>(nullptr))),
                             "REQUIRE_NOT_NULL aborts the case") &&
         ok;
    return ok;
}

/// @brief 值打印：各类型的渲染形态。
auto selftest_value_printing() -> bool {
    bool ok = true;
    ok = expect(print_value(true) == "true", "bool prints true") && ok;
    ok = expect(print_value('a') == "'a'", "char prints quoted") && ok;
    ok = expect(print_value(42) == "42", "int prints decimal") && ok;
    ok = expect(print_value(0.5) == "0.5", "double prints shortest exact form") && ok;
    ok = expect(print_value(std::string{"ab\ncd"}) == R"("ab\ncd")", "std::string escapes newline") && ok;
    ok = expect(print_value(std::string_view{"xy"}) == "\"xy\"", "string_view prints quoted") && ok;
    ok = expect(print_value("lit") == "\"lit\"", "C string literal prints quoted") && ok;
    const char* null_text = nullptr;
    ok = expect(print_value(null_text) == "nullptr", "null C string prints nullptr") && ok;
    const int* null_pointer = nullptr;
    ok = expect(print_value(null_pointer) == "nullptr", "null pointer prints nullptr") && ok;
    // 枚举：密集枚举走 known_enums 反查给值名，稀疏枚举退回底层数值（避免张冠李戴）。
    ok = expect(print_value(aurora::TextAlign::Center) == "TextAlign::Center",
                "dense enum prints registered value name") &&
         ok;
    ok = expect(print_value(aurora::FontWeight::Bold) == "FontWeight(700)",
                "sparse enum falls back to the underlying value") &&
         ok;
    ok = expect(print_value(std::vector<int>{}) == "{ }", "empty container prints braces") && ok;
    ok = expect(print_value(std::vector<std::uint8_t>{0x00, 0xA1, 0xFF}) == "<3 bytes: 00 A1 FF>",
                "byte sequence prints a hex dump") &&
         ok;
    ok = expect(print_value(std::optional<int>{5}) == "5", "optional prints its value") && ok;
    ok = expect(print_value(std::optional<int>{}) == "«empty»", "empty optional prints «empty»") && ok;
    ok = expect(print_value(std::pair{1, 2}) == "(1, 2)", "pair prints its members") && ok;
    ok = expect(print_value(SelftestUnprintable{}) == "<unprintable aurora::testing::SelftestUnprintable>",
                "unprintable type falls back to its type name") &&
         ok;
    ok = expect(print_value(SelftestCustom{3}) == "Custom{3}",
                "ValuePrinter specialization wins over the generic path") &&
         ok;
    return ok;
}

/// @brief 作用域追踪：块内失败携带上下文，离开块自动复原。
auto selftest_tracing() -> bool {
    bool ok = true;
    const auto traced = probe(AURORA_TEST_PROBE(AURORA_TEST_TRACE("outer context"); {
        AURORA_TEST_TRACE("inner context");
        AURORA_TEST_CHECK(false);
    } AURORA_TEST_CHECK(false);));
    ok = expect(traced.failures.size() == 2, "traced case records two failures") && ok;
    if (traced.failures.size() == 2) {
        const auto& inner = traced.failures[0].message;
        const auto& outer = traced.failures[1].message;
        ok =
            expect(inner.find("outer context") != std::string::npos && inner.find("inner context") != std::string::npos,
                   "failure inside the block carries both trace levels") &&
            ok;
        ok =
            expect(outer.find("inner context") == std::string::npos && outer.find("outer context") != std::string::npos,
                   "leaving the block pops exactly one trace level") &&
            ok;
    }
    const auto untraced = probe(AURORA_TEST_PROBE(AURORA_TEST_CHECK(false)));
    ok = expect(untraced.failures.size() == 1 && first_failure(untraced).find("trace") == std::string::npos,
                "no trace stack means no trace section") &&
         ok;
    return ok;
}

// ---- 诊断设施探针（死亡测试 / 报告）----

/// @brief 报告写出的临时文件路径。
[[nodiscard]] auto scratch_path(std::string_view name) -> std::string {
    return (std::filesystem::temp_directory_path() / std::string{name}).string();
}

/// @brief 读回文本文件（读不到时返回空串）。
[[nodiscard]] auto read_back(const std::string& path) -> std::string {
    std::ifstream input{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// @brief 造一份含通过 / 失败 / 跳过三种状态的报告样本（消息里带需要转义的字符）。
[[nodiscard]] auto report_sample() -> std::vector<CaseResult> {
    std::vector<CaseResult> results;
    results.push_back(CaseResult{.full_name = "utest_probe.passes", .file = "probe.cpp", .line = 1});
    results.push_back(
        CaseResult{.full_name = "utest_probe.fails",
                   .file = "probe.cpp",
                   .line = 2,
                   .status = TestStatus::Failed,
                   .failures = {Failure{.file = "probe.cpp", .line = 2, .message = "expected <true> & \"x\""}}});
    results.push_back(CaseResult{.full_name = "other_probe.skips",
                                 .file = "probe.cpp",
                                 .line = 3,
                                 .status = TestStatus::Skipped,
                                 .skip_reason = "backend off"});
    return results;
}

/// @brief 诊断设施自检。
auto selftest_diagnostics() -> bool {
    bool ok = true;

    // 站点键：同一站点稳定、不同站点可区分（父/子进程靠它对齐站点）。
    const auto key = detail::death_site_key("probe/death.cpp", 10);
    ok = expect(key == detail::death_site_key("probe/death.cpp", 10) &&
                    key != detail::death_site_key("probe/death.cpp", 11),
                "death site key is stable per file:line") &&
         ok;

    // 退出码 → 判定：0=站点到达但未致死、约定码=站点没走到、其余=已致死。
    ok = expect(detail::death_verdict_for(0) == detail::DeathVerdict::Survived &&
                    detail::death_verdict_for(detail::death_site_not_reached()) == detail::DeathVerdict::SiteMissed &&
                    detail::death_verdict_for(3) == detail::DeathVerdict::Died,
                "child exit code maps to the death verdict") &&
         ok;

    // 期望判定：子串、匹配器、空期望（只要求致死）三种形态。
    const std::string stderr_text = "terminate called after throwing: boom";
    ok =
        expect(detail::death_expectation_matches("boom", stderr_text), "string expectation matches by substring") && ok;
    ok = expect(!detail::death_expectation_matches("boom", std::string{"unrelated"}),
                "string expectation rejects non-matching stderr") &&
         ok;
    ok = expect(detail::death_expectation_matches(matchers::has_substr("boom"), stderr_text),
                "matcher expectation is supported") &&
         ok;
    ok = expect(detail::death_expectation_matches("", std::string{"whatever"}) &&
                    !detail::death_expectation_matches("anything", std::string{}),
                "empty expectation only requires death") &&
         ok;

    // 子进程门控：只有键相符的站点执行语句；未到达时退出码用约定值。
    // ⚠️ 必须放在本分区末尾：此后本进程仍处于子进程模式，自检随即结束。
    detail::enter_death_child(key);
    ok = expect(detail::death_child_mode() && !detail::death_child_should_run("probe/death.cpp", 11),
                "child mode ignores non-target sites") &&
         ok;
    ok = expect(detail::death_child_exit_code() == detail::death_site_not_reached(),
                "unreached site exits with the sentinel code") &&
         ok;
    ok = expect(detail::death_child_should_run("probe/death.cpp", 10) && detail::death_child_exit_code() == 0,
                "reached site reports survival as exit 0") &&
         ok;

    // 报告转义。
    ok = expect(xml_escape("a<b>&\"x\"") == "a&lt;b&gt;&amp;&quot;x&quot;", "xml escaping") && ok;
    ok = expect(json_escape("line1\nline2\ttab") == "line1\\nline2\\ttab",
                "json escaping keeps control characters escaped") &&
         ok;
    ok = expect(json_escape(std::string{"a\x01", 2}) == R"(a\u0001)", "json escaping renders C0 controls as \\uXXXX") &&
         ok;

    // 报告落盘：JSON 与 JUnit XML 各一份，内容可回读且分组 / 转义正确。
    const auto results = report_sample();
    const auto summary = summarize(results);
    const auto json_path = scratch_path("aurora_selftest_report.json");
    const auto xml_path = scratch_path("aurora_selftest_report.xml");
    std::string error;
    ok = expect(write_report(json_path, results, summary, &error) && error.empty(), "JSON report written") && ok;
    ok = expect(write_report(xml_path, results, summary, &error), "JUnit XML report written") && ok;
    const auto json_text = read_back(json_path);
    const auto xml_text = read_back(xml_path);
    ok = expect(json_text.find("\"total\": 3") != std::string::npos &&
                    json_text.find("\"failed\": 1") != std::string::npos &&
                    json_text.find("utest_probe.fails") != std::string::npos &&
                    json_text.find(R"(\"x\")") != std::string::npos,
                "JSON report carries counters, case names and escaped messages") &&
         ok;
    ok = expect(xml_text.find("<testsuites") != std::string::npos &&
                    xml_text.find("failures=\"1\"") != std::string::npos &&
                    xml_text.find("<testsuite name=\"other_probe\"") != std::string::npos &&
                    xml_text.find("expected &lt;true&gt; &amp;") != std::string::npos &&
                    xml_text.find("<skipped message=") != std::string::npos,
                "JUnit XML report groups by suite and escapes failure text") &&
         ok;
    ok = expect(!write_report("Z:/definitely/not/writable/report.json", results, summary, &error) && !error.empty(),
                "unwritable report path reports an error") &&
         ok;

    std::error_code ignored;
    std::filesystem::remove(json_path, ignored);
    std::filesystem::remove(xml_path, ignored);
    return ok;
}

#undef AURORA_TEST_PROBE
#undef AURORA_TEST_FATAL_PROBE

}  // namespace

auto run_framework_selftest() -> int {
    bool ok = true;
    ok = selftest_execution() && ok;
    ok = selftest_bool_assertions() && ok;
    ok = selftest_comparison_assertions() && ok;
    ok = selftest_string_assertions() && ok;
    ok = selftest_exception_assertions() && ok;
    ok = selftest_pointer_assertions() && ok;
    ok = selftest_value_printing() && ok;
    ok = selftest_tracing() && ok;
    ok = selftest_organization() && ok;
    // 诊断分区放在最后：它会把本进程切进死亡测试子进程模式。
    ok = selftest_diagnostics() && ok;

    if (!ok) {
        std::printf("[selftest] RESULT: FAILED\n");
        return static_cast<int>(ExitCode::HasFailures);
    }
    std::printf("[selftest] RESULT: PASSED\n");
    return static_cast<int>(ExitCode::AllPassed);
}

}  // namespace aurora::testing
