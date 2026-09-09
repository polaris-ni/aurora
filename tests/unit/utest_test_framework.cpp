/// 测试类型: unit
/// 目标单元: tests/framework/aurora_test.h
/// 测试说明: 测试框架自身契约（用例注册、套件名推导、skip 桩、异常隔离、断言家族、fixture
/// 与参数化组织、用例边界资源隔离）
///

// 框架自检（synthetic 用例）在 runner 的 --selftest 中，不进注册表；
// 本文件验证「真实测试 TU 经宏注册后的可观测行为」，并充当迁移期复制粘贴的写法样板。

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "aurora/app/clipboard.h"
#include "aurora/core/enums.h"
#include "framework/aurora_test.h"
#include "support/paths.h"

namespace aurora::test_cases::utest_test_framework {

namespace testing = aurora::testing;
namespace m = aurora::testing::matchers;  // 匹配器工厂别名

/// @brief 容器断言的实参来源：宏实参里不能写带逗号的花括号初始化列表。
[[nodiscard]] static auto probe_vector() -> std::vector<int> { return {1, 2, 3}; }

[[nodiscard]] static auto probe_empty_vector() -> std::vector<int> { return {}; }

[[nodiscard]] static auto square_area(int side) -> int { return side * side; }

AURORA_TEST_CASE(suite_name_equals_file_stem) {
    // 套件名恒等于文件 stem，CTest 的 --run=<stem> 才能筛中本文件全部用例。
    const auto cases = testing::TestRegistry::instance().cases();
    bool found = false;
    for (const auto* test_case : cases) {
        if (test_case->suite != "utest_test_framework") {
            continue;
        }
        found = true;
        const auto full = test_case->full_name();
        AURORA_TEST_CHECK(full == std::string{"utest_test_framework."} + std::string{test_case->case_name});
    }
    AURORA_TEST_REQUIRE(found);
}

AURORA_TEST_CASE(registered_case_file_matches_this_file) {
    const auto cases = testing::TestRegistry::instance().cases();
    for (const auto* test_case : cases) {
        if (test_case->suite != "utest_test_framework") {
            continue;
        }
        // 注册点记录的文件必须是本文件（suite_from_path 推导正确）。
        AURORA_TEST_CHECK(testing::suite_from_path(test_case->file) == "utest_test_framework");
    }
}

AURORA_TEST_CASE(suite_from_path_handles_both_separators) {
    static_assert(testing::suite_from_path("tests/unit/utest_color.cpp") == "utest_color");
    static_assert(testing::suite_from_path(R"(D:\repo\tests\unit\utest_color.cpp)") == "utest_color");
    AURORA_TEST_CHECK(true);
}

AURORA_TEST_CASE(skip_stub_for_disabled_feature) {
    // 后端 / 平台专属用例的标准写法：feature 宏未开启时注册 skip 桩，
    // 不伪造通过、也不让 CI 误报失败。
#ifndef AURORA_BACKEND_GLFW
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW not enabled");
#else
    AURORA_TEST_CHECK(true);
#endif
}

AURORA_TEST_CASE(assertion_family_smoke) {
    // ⚠️ 注册用例里不得放「故意失败」的断言——那会把本文件的 CTest 条目判红；
    //    失败信息与作用域追踪的可观测行为由 runner `--selftest` 的 synthetic 探针覆盖。
    constexpr int computed = 2 + 2;
    AURORA_TEST_CHECK(computed == 4);
    AURORA_TEST_CHECK_TRUE(computed > 3);
    AURORA_TEST_CHECK_FALSE(computed > 5);
    AURORA_TEST_CHECK_MSG(computed == 4, "arithmetic still works");
    AURORA_TEST_CHECK_EQ(computed, 4);
    AURORA_TEST_CHECK_NE(computed, 5);
    AURORA_TEST_CHECK_LT(computed, 5);
    AURORA_TEST_CHECK_LE(computed, 4);
    AURORA_TEST_CHECK_GT(computed, 3);
    AURORA_TEST_CHECK_GE(computed, 4);
    AURORA_TEST_CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
    AURORA_TEST_CHECK_STREQ(std::string{"aurora"}, "aurora");  // NOLINT(*-string-view-conversions)
    AURORA_TEST_CHECK_STRNE("aurora", "borealis");
    AURORA_TEST_CHECK_STRCASEEQ("Aurora", "aurora");
    AURORA_TEST_CHECK_STRCASENE("Aurora", "borealis");
    AURORA_TEST_CHECK_THROW(throw std::runtime_error{"boom"}, std::runtime_error);
    AURORA_TEST_CHECK_NO_THROW(static_cast<void>(computed));
    AURORA_TEST_CHECK_ANY_THROW(throw 1);
    AURORA_TEST_CHECK_NULL(static_cast<const int*>(nullptr));
    AURORA_TEST_CHECK_NOT_NULL(&computed);

    // 容器与枚举实参可直接比较；实际值渲染见 value_print.h。
    AURORA_TEST_CHECK_EQ(probe_vector(), probe_vector());
    AURORA_TEST_CHECK_EQ(aurora::TextAlign::Center, aurora::TextAlign::Center);

    // 致命断言的通过路径不改变用例状态。
    AURORA_TEST_REQUIRE_EQ(computed, 4);

    AURORA_TEST_TRACE("sample scoped context");
    AURORA_TEST_CHECK(computed != 0);  // 本作用域内若失败，会自动附带上面这行追踪
}

AURORA_TEST_CASE(matcher_family_smoke) {
    AURORA_TEST_CHECK_THAT(std::string{"aurora"}, m::str_eq("aurora"));
    AURORA_TEST_CHECK_THAT(std::string{"aurora"}, m::str_ne("borealis"));
    AURORA_TEST_CHECK_THAT(std::string{"Aurora"}, m::str_case_eq("aurora"));
    AURORA_TEST_CHECK_THAT(std::string{"aurora"}, m::starts_with("au"));
    AURORA_TEST_CHECK_THAT(std::string{"aurora"}, m::ends_with("ra"));
    AURORA_TEST_CHECK_THAT(std::string{"aurora"}, m::has_substr("ror"));
    AURORA_TEST_CHECK_THAT(4, m::all_of(m::ge(2), m::le(5)));
    AURORA_TEST_CHECK_THAT(4, m::any_of(m::lt(0), m::gt(3)));
    AURORA_TEST_CHECK_THAT(4, m::negated(m::eq(5)));
    AURORA_TEST_CHECK_THAT(probe_vector(), m::contains(2));
    AURORA_TEST_CHECK_THAT(probe_vector(), m::size_is(3));
    AURORA_TEST_CHECK_THAT(probe_vector(), m::each(m::gt(0)));
    AURORA_TEST_CHECK_THAT(probe_empty_vector(), m::is_empty());
    AURORA_TEST_REQUIRE_THAT(4, m::eq(4));
}

namespace {
/// @brief fixture 用例样板：状态由 SetUp 复位，TearDown 恰好执行一次。
///
/// fixture 类名按仓库命名规范取 PascalCase（ClassCase），用例名里会带上它。
// fixture 的共享状态放 protected 区（生成的用例类要访问），故按仓库惯例
// 豁免「非私有成员」告警——保护成员被派生用例使用是刻意设计。
// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
class CounterFixture : public testing::Fixture {
  protected:
    auto SetUp() -> void override { count_ = 0; }

    auto TearDown() -> void override { ++teardowns_; }

    int count_ = 0;
    int teardowns_ = 0;
};
}  // namespace

AURORA_TEST_F(CounterFixture, starts_from_zero) {
    AURORA_TEST_CHECK_EQ(count_, 0);
    ++count_;
    AURORA_TEST_CHECK_EQ(teardowns_, 0);  // TearDown 尚未执行（用例体先于清理）
}

AURORA_TEST_F(CounterFixture, state_is_per_case) {
    // 上一用例里的 ++count_ 不得泄漏到本用例：每个用例重建 fixture。
    AURORA_TEST_CHECK_EQ(count_, 0);
}

namespace {
/// @brief 值参数化样板：取值经 `param()` 读取，展开后每个取值一条用例。
class SquareFixture : public testing::TestWithParam<int> {};
}  // namespace

AURORA_TEST_P(SquareFixture, area_matches_edge) { AURORA_TEST_CHECK_EQ(square_area(param()), param() * param()); }

AURORA_INSTANTIATE_TEST_SUITE_P(edges, SquareFixture, testing::values_of(1, 2, 4));

AURORA_INSTANTIATE_TEST_SUITE_P_GEN(sides, SquareFixture, testing::values_of(3, 5),
                                    [](int side) -> std::string { return "side" + std::to_string(side); });

namespace {
/// @brief 类型参数化样板：`TestType` 即当前类型，fixture 成员照常可见。
template <typename TestType>
class NumericFixture : public testing::Fixture {
  protected:
    auto SetUp() -> void override { value_ = TestType{2}; }

    TestType value_{};
};
}  // namespace
// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

AURORA_TYPED_TEST_SUITE(NumericFixture, int, float, double);

AURORA_TYPED_TEST(NumericFixture, keeps_identity) {
    // ⚠️ fixture 是生成类的依赖基，成员须经 this-> 引用（普通查找不进依赖基）。
    AURORA_TEST_CHECK_EQ(this->value_, TestType{2});
    AURORA_TEST_CHECK_THAT(this->value_, m::gt(TestType{1}));
}

AURORA_TEST_CASE(death_test_detects_fatal_statement) {
    // 死亡测试走完整 spawn 路径：子进程重跑本用例，只有这里的语句真正执行。
    AURORA_TEST_CHECK_DEATH(throw std::runtime_error{"fatal by design"}, "fatal by design");
    AURORA_TEST_CHECK_DEATH(std::abort(), "");
}

// ---- 隔离（tmpdir / 仓库根 / 剪贴板注入）------------------------------------

AURORA_TEST_CASE(isolation_temp_dir_wired_to_env) {
    // 每个用例开始时框架创建唯一临时目录并接管 TMPDIR/TMP/TEMP，
    // 偏好 / 存储类用例的临时文件写入因此彼此隔离（并行安全的关键一环）。
    const auto& tmp = testing::isolation::temp_dir();
    AURORA_TEST_REQUIRE(!tmp.empty());
    AURORA_TEST_CHECK(std::filesystem::exists(tmp));
    const char* env_tmpdir = std::getenv("TMPDIR");
    AURORA_TEST_REQUIRE(env_tmpdir != nullptr);
    AURORA_TEST_CHECK(tmp == env_tmpdir);
}

AURORA_TEST_CASE(isolation_repo_root_resolves) {
    // cwd 已被统一切到仓库根；paths::under_repo 在其上给出绝对路径。
    const auto& root = testing::paths::repo_root();
    AURORA_TEST_REQUIRE(!root.empty());
    AURORA_TEST_CHECK(std::filesystem::exists(std::filesystem::path{root} / "codespec"));
    AURORA_TEST_CHECK(std::filesystem::exists(testing::paths::under_repo("codespec")));
    AURORA_TEST_CHECK(std::filesystem::path{testing::paths::under_repo("codespec")}.is_absolute());
}

AURORA_TEST_CASE(clipboard_test_backend_roundtrip) {
    // 双宏齐备时注入生效（不触碰系统剪贴板）；任一关闭（含 Release 下 DEBUG 自动关）
    // 返回 false → 运行时 skip，不伪造通过也不误报失败。
    if (!aurora::Clipboard::install_test_backend()) {
        AURORA_TEST_SKIP("test hooks disabled (AURORA_ENABLE_DEBUG / AURORA_ENABLE_TEST_HOOKS off)");
    }
    aurora::Clipboard::set_text("aurora-clipboard-roundtrip");
    AURORA_TEST_CHECK_EQ(aurora::Clipboard::get_text(), "aurora-clipboard-roundtrip");

    aurora::Clipboard::reset_test_backend();
    AURORA_TEST_CHECK(aurora::Clipboard::get_text().empty());

    aurora::Image image;
    image.width = 1;
    image.height = 1;
    image.pixels = {10U, 20U, 30U, 255U};
    aurora::Clipboard::set_image(image);
    const auto restored = aurora::Clipboard::get_image();
    AURORA_TEST_CHECK_EQ(restored.width, 1);
    AURORA_TEST_CHECK_EQ(restored.height, 1);
    AURORA_TEST_CHECK(restored.pixels == image.pixels);

    AURORA_TEST_REQUIRE(aurora::Clipboard::remove_test_backend());
    AURORA_TEST_CHECK(!aurora::Clipboard::remove_test_backend());  // 无后端可再卸
}

}  // namespace aurora::test_cases::utest_test_framework
