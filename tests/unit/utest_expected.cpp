/// 测试类型: unit
/// 目标单元: include/aurora/core/expected.h
/// 测试说明: unexpected 的构造与错误访问、expected 值态/错误态访问、拷贝/移动/自赋值语义、value_or 回退，以及以 aurora::Error 为错误类型的表驱动元数据

#include <string>
#include <type_traits>
#include <utility>

#include "aurora/core/expected.h"
#include "aurora/core/result.h"  // aurora::Error：expected 设计上的错误类型
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_expected {

namespace m = aurora::testing::matchers;

AURORA_TEST_CASE(unexpected_wraps_and_exposes_error) {
    const unexpected u{Error{}};  // CTAD 推导 unexpected<Error>
    static_assert(std::is_same_v<std::decay_t<decltype(u)>, unexpected<Error>>);
    AURORA_TEST_CHECK(u.error().code_enum == ErrorCode::GeneralUnknown);

    // 非 const error() 可写。
    unexpected<Error> mutable_u{Error{}};
    mutable_u.error() = make_error(ErrorCode::StorageIoError);
    AURORA_TEST_CHECK(mutable_u.error().code_enum == ErrorCode::StorageIoError);

    // 右值 error()：移动取出。
    unexpected<std::string> taken{std::string{"moved out"}};
    const std::string out = std::move(taken).error();
    AURORA_TEST_CHECK_THAT(out, m::str_eq("moved out"));
}

AURORA_TEST_CASE(expected_value_state_access) {
    const expected<int, Error> e{42};
    AURORA_TEST_CHECK_TRUE(e.has_value());
    AURORA_TEST_CHECK(e);  // explicit operator bool
    AURORA_TEST_CHECK_EQ(e.value(), 42);

    // 非 const value() 可写。
    expected<std::string, Error> s{std::string{"aurora"}};
    s.value() += "-ok";
    AURORA_TEST_CHECK_THAT(s.value(), m::str_eq("aurora-ok"));
}

AURORA_TEST_CASE(expected_error_state_access) {
    const expected<int, Error> e{unexpected{make_error(ErrorCode::NavDepthExceeded, ErrorParams{{"max", "4"}})}};
    AURORA_TEST_CHECK_FALSE(e.has_value());
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(e));
    AURORA_TEST_CHECK_STREQ(e.error().code, "nav-depth-exceeded");
}

AURORA_TEST_CASE(expected_copy_semantics_independent_and_cross_state) {
    // 值态拷贝：拷贝后互不影响。
    expected<std::string, Error> src{std::string{"one"}};
    auto copied = src;
    copied.value() = "two";
    AURORA_TEST_CHECK_THAT(src.value(), m::str_eq("one"));
    AURORA_TEST_CHECK_THAT(copied.value(), m::str_eq("two"));

    // 错误态拷贝。
    expected<int, Error> err_src{unexpected{make_error(ErrorCode::JsonParseError)}};
    const auto& err_copy = err_src;
    AURORA_TEST_CHECK_FALSE(err_copy.has_value());
    AURORA_TEST_CHECK_STREQ(err_copy.error().code, "json-parse-error");

    // 跨态拷贝赋值：值态 ← 错误态。
    expected<int, Error> from_value{1};
    from_value = err_src;
    AURORA_TEST_CHECK_FALSE(from_value.has_value());
    AURORA_TEST_CHECK_STREQ(from_value.error().code, "json-parse-error");

    // 跨态拷贝赋值：错误态 ← 值态。
    expected<int, Error> from_error{unexpected{make_error(ErrorCode::JsonParseError)}};
    from_error = expected<int, Error>{99};
    AURORA_TEST_CHECK_TRUE(from_error.has_value());
    AURORA_TEST_CHECK_EQ(from_error.value(), 99);
}

AURORA_TEST_CASE(expected_move_semantics) {
    // 移动构造：值所有权转移。
    expected<std::string, Error> src{std::string{"payload"}};
    auto dst{std::move(src)};
    AURORA_TEST_CHECK_THAT(dst.value(), m::str_eq("payload"));

    // 跨态移动赋值：错误态 ← 值态右值。
    expected<std::string, Error> err_state{unexpected{make_error(ErrorCode::StorageIoError)}};
    err_state = expected<std::string, Error>{std::string{"recovered"}};
    AURORA_TEST_CHECK_TRUE(err_state.has_value());
    AURORA_TEST_CHECK_THAT(err_state.value(), m::str_eq("recovered"));

    // 移动 unexpected 构造错误态。
    unexpected<std::string> u{std::string{"plain error"}};
    expected<int, std::string> plain{std::move(u)};
    AURORA_TEST_CHECK_FALSE(plain.has_value());
    AURORA_TEST_CHECK_THAT(plain.error(), m::str_eq("plain error"));
}

AURORA_TEST_CASE(expected_value_or_returns_value_or_default) {
    // 值态：返回持有值。
    const expected<int, Error> has{42};
    AURORA_TEST_CHECK_EQ(has.value_or(99), 42);

    // 错误态：返回默认值。
    const expected<int, Error> lacks{unexpected{make_error(ErrorCode::GeneralUnknown)}};
    AURORA_TEST_CHECK_EQ(lacks.value_or(99), 99);

    // 非平凡类型同样适用。
    const expected<std::string, Error> lacks_str{unexpected{make_error(ErrorCode::GeneralUnknown)}};
    AURORA_TEST_CHECK_THAT(lacks_str.value_or(std::string{"fallback"}), m::str_eq("fallback"));
}

AURORA_TEST_CASE(expected_with_aurora_error_table_metadata) {
    // expected<int, Error> 组合：错误侧携带 errors.toml 表驱动元数据。
    const expected<int, Error> e{unexpected{make_error(ErrorCode::StorageRecordNotFound)}};
    AURORA_TEST_CHECK_THAT(e.error().code, m::str_eq("storage-record-not-found"));
    AURORA_TEST_CHECK(e.error().code_enum == ErrorCode::StorageRecordNotFound);
    AURORA_TEST_CHECK(e.error().severity == ErrorSeverity::Error);
    AURORA_TEST_CHECK(e.error().category == ErrorCategory::Io);
}

AURORA_TEST_CASE(expected_self_assignment_preserves_state) {
    // 自赋值（经引用别名规避编译器自赋值告警）：值态保持不变。
    expected<std::string, Error> value_state{std::string{"stable"}};
    auto& value_self = value_state;
    value_state = value_self;
    AURORA_TEST_CHECK_TRUE(value_state.has_value());
    AURORA_TEST_CHECK_THAT(value_state.value(), m::str_eq("stable"));

    // 自赋值：错误态保持不变。
    expected<int, Error> error_state{unexpected{make_error(ErrorCode::SurfaceLost)}};
    auto& error_self = error_state;
    error_state = error_self;
    AURORA_TEST_CHECK_FALSE(error_state.has_value());
    AURORA_TEST_CHECK(error_state.error().code_enum == ErrorCode::SurfaceLost);
}

AURORA_TEST_CASE(expected_wrong_state_access_aborts_process) {
    // 失败路径（死亡测试）：错误态取 value / 值态取 error 属 UB，AURORA_CHECK 常开拦截
    // （所有构建配置生效，fail-fast 优于返回垃圾引用）。
    expected<int, Error> value_state{42};
    AURORA_TEST_CHECK_DEATH((void)value_state.error(), "expected::error()");
    expected<int, Error> error_state{unexpected{make_error(ErrorCode::SurfaceLost)}};
    AURORA_TEST_CHECK_DEATH((void)error_state.value(), "expected::value()");
}

}  // namespace aurora::test_cases::utest_expected
