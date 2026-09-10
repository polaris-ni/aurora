#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 断言家族
// ------------------------------------------------------------
// 两族语义：
//   AURORA_TEST_CHECK_*   非致命：记录失败，用例继续执行（对标 GoogleTest EXPECT_*）
//   AURORA_TEST_REQUIRE_* 致命：记录失败后抛 CaseAbort 终止本用例（对标 ASSERT_*）
// 每个谓词只有一个 `*_message` 内核，返回「空串 = 通过」的诊断文本；宏层只负责决定
// 严重级别，故 CHECK/REQUIRE 变体的行为天然一致，不会各自演化。
//
// 两侧操作数**各只求值一次**（内核按 const& 接收，失败时才格式化实际值）。
// 实际值渲染见 value_print.h；作用域追踪见 AURORA_TEST_TRACE。
//
// 断言宏的「表达式侧」参数一律收变参 `__VA_ARGS__`（尾部实参）：花括号初始化列表里的
// 顶层逗号（如 `Rect{{1,2},{3,4}}`）会被预处理器当参数分隔符，变参在展开点原样重组，
// 调用端无须为实参外包圆括号；`#__VA_ARGS__` 还原完整表达式文本用于失败诊断。
// ============================================================

#include <cmath>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "value_print.h"

namespace aurora::testing {

/// @brief 致命断言失败（AURORA_TEST_REQUIRE）：终止当前用例，runner 记为 Failed。
class CaseAbort : public std::exception {
  public:
    explicit CaseAbort(std::string message) : message_(std::move(message)) {}

    [[nodiscard]] auto what() const noexcept -> const char* override { return message_.c_str(); }

  private:
    std::string message_;
};

/// @brief 主动跳过（AURORA_TEST_SKIP）：终止当前用例，runner 记为 Skipped。
class CaseSkipped : public std::exception {
  public:
    explicit CaseSkipped(std::string reason) : reason_(std::move(reason)) {}

    /// @brief 跳过原因（原样进入报告）。
    [[nodiscard]] auto reason() const -> const std::string& { return reason_; }

    [[nodiscard]] auto what() const noexcept -> const char* override { return reason_.c_str(); }

  private:
    std::string reason_;
};

/// @brief 断言严重级别：非致命记录后继续，致命记录后抛 CaseAbort。
///
/// 宏层直接书写本类型（`Severity::NonFatal` / `Severity::Fatal`），故置于 `aurora::testing`
/// 而非 `detail`。
enum class Severity : std::uint8_t {
    NonFatal,  ///< 记录失败，用例继续执行
    Fatal,  ///< 记录失败并终止当前用例
};

namespace detail {

/// @brief 断言内核唯一出口：拼接追踪上下文、记账、按级别决定是否抛出。
auto report(Severity severity, const char* file, int line, std::string message) -> void;

/// @brief 抛 CaseSkipped；当前用例立即终止并记为 Skipped。
[[noreturn]] auto skip_case(std::string reason) -> void;

// ---- 谓词内核：返回空串表示通过 ----

[[nodiscard]] auto bool_message(bool satisfied, std::string_view expression) -> std::string;

[[nodiscard]] auto expected_bool_message(bool value, bool expected, std::string_view expression) -> std::string;

[[nodiscard]] auto message_message(bool satisfied, std::string_view expression, std::string_view message)
    -> std::string;

template <typename A, typename B>
[[nodiscard]] auto eq_message(const A& lhs, const B& rhs, std::string_view lhs_text, std::string_view rhs_text)
    -> std::string {
    if (lhs == rhs) {
        return {};
    }
    return std::string{lhs_text} + " == " + std::string{rhs_text} + compare_detail(lhs, rhs);
}

template <typename A, typename B>
[[nodiscard]] auto ne_message(const A& lhs, const B& rhs, std::string_view lhs_text, std::string_view rhs_text)
    -> std::string {
    if (lhs != rhs) {
        return {};
    }
    return std::string{lhs_text} + " != " + std::string{rhs_text} + compare_detail(lhs, rhs);
}

template <typename A, typename B>
[[nodiscard]] auto lt_message(const A& lhs, const B& rhs, std::string_view lhs_text, std::string_view rhs_text)
    -> std::string {
    if (lhs < rhs) {
        return {};
    }
    return std::string{lhs_text} + " < " + std::string{rhs_text} + compare_detail(lhs, rhs);
}

template <typename A, typename B>
[[nodiscard]] auto le_message(const A& lhs, const B& rhs, std::string_view lhs_text, std::string_view rhs_text)
    -> std::string {
    if (lhs <= rhs) {
        return {};
    }
    return std::string{lhs_text} + " <= " + std::string{rhs_text} + compare_detail(lhs, rhs);
}

template <typename A, typename B>
[[nodiscard]] auto gt_message(const A& lhs, const B& rhs, std::string_view lhs_text, std::string_view rhs_text)
    -> std::string {
    if (lhs > rhs) {
        return {};
    }
    return std::string{lhs_text} + " > " + std::string{rhs_text} + compare_detail(lhs, rhs);
}

template <typename A, typename B>
[[nodiscard]] auto ge_message(const A& lhs, const B& rhs, std::string_view lhs_text, std::string_view rhs_text)
    -> std::string {
    if (lhs >= rhs) {
        return {};
    }
    return std::string{lhs_text} + " >= " + std::string{rhs_text} + compare_detail(lhs, rhs);
}

/// @brief 浮点近似：对称差 `|a - b| <= eps`（提升为 double 后比较，避免混型截断）。
template <typename A, typename B, typename E>
[[nodiscard]] auto near_message(const A& lhs, const B& rhs, const E& eps, std::string_view lhs_text,
                                std::string_view rhs_text) -> std::string {
    const auto delta = std::fabs(static_cast<double>(lhs) - static_cast<double>(rhs));
    if (delta <= static_cast<double>(eps)) {
        return {};
    }
    return std::string{lhs_text} + " ~= " + std::string{rhs_text} + compare_detail(lhs, rhs) +
           "\n    Diff: " + print_value(delta) + ", tolerance: " + print_value(eps);
}

// ---- 字符串比较：按内容比较（指针比较是常见误用，故单列一族）----

/// @brief 取字符串视图：C 字符串空指针视作空串，避免构造 `string_view(nullptr)` 的未定义行为；
///        `std::string` / `std::string_view` / 字符字面量经视图构造器统一转换。
/// @note 引用本身永不为空，但**被引用对象**可以是指针且其值为 nullptr —— 故指针分支的空值
///       检查不可省：删掉后 `string_view(nullptr)` 会走 `strlen(nullptr)`（实测段错误）。
///       数组分支则相反：退化后取的是数组首地址，恒非空，比较既恒假又会触发告警。
template <typename T>
[[nodiscard]] auto string_view_of(const T& text) -> std::string_view {
    // 字符数组（字面量 "abc"、char 缓冲区）必须先分流：数组退化为指针后地址恒非空，
    // 再与 nullptr 比较会让 GCC 报 -Wnonnull-compare（该诊断依赖优化期推断，故仅
    // -O1 及以上出现）。数组天然非空，直接走视图构造即可 —— 与指针分支的非空路径
    // 同为 C 串语义（截断到首个 '\0'），行为不变。
    if constexpr (std::is_array_v<std::remove_reference_t<T>>) {
        return std::string_view{text};
    } else if constexpr (std::is_same_v<std::decay_t<T>, const char*> || std::is_same_v<std::decay_t<T>, char*>) {
        return text == nullptr ? std::string_view{} : std::string_view{text};
    } else {
        return std::string_view{text};
    }
}

[[nodiscard]] auto strings_equal(std::string_view lhs, std::string_view rhs, bool case_sensitive) -> bool;

[[nodiscard]] auto string_message(std::string_view lhs_text, std::string_view rhs_text, std::string_view lhs_actual,
                                  std::string_view rhs_actual, bool want_equal, bool case_sensitive) -> std::string;

// ---- 异常判定 ----

/// @brief 语句包装的恒真条件：让「语句」在宏里以表达式形式求值一次，且不可被优化掉。
[[nodiscard]] inline auto always_true() -> bool { return true; }

template <typename Fn>
    requires std::is_invocable_v<Fn&>
[[nodiscard]] auto no_throw_message(Fn&& body, std::string_view statement_text) -> std::string {
    try {
        body();
    } catch (const std::exception& error) {
        return std::string{statement_text} + " threw " + exception_text(error) + ", expected no exception";
    } catch (...) {
        return std::string{statement_text} + " threw a non-standard exception, expected no exception";
    }
    return {};
}

template <typename Expected, typename Fn>
    requires std::is_invocable_v<Fn&>
[[nodiscard]] auto throws_message(Fn&& body, std::string_view statement_text, std::string_view expected_text)
    -> std::string {
    try {
        body();
    } catch (const Expected&) {
        return {};
    } catch (const std::exception& error) {
        return std::string{statement_text} + " threw " + exception_text(error) + ", expected " +
               std::string{expected_text};
    } catch (...) {
        return std::string{statement_text} + " threw a non-standard exception, expected " + std::string{expected_text};
    }
    return std::string{statement_text} + " did not throw; expected " + std::string{expected_text};
}

template <typename Fn>
    requires std::is_invocable_v<Fn&>
[[nodiscard]] auto any_throw_message(Fn&& body, std::string_view statement_text) -> std::string {
    try {
        body();
    } catch (...) {
        // 任意抛出即满足「期望抛出」，具体类型由 CHECK_THROW 族负责区分。
        return {};
    }
    return std::string{statement_text} + " did not throw; expected any exception";
}

// ---- 指针空判定 ----

template <typename T>
[[nodiscard]] auto null_message(const T& pointer, std::string_view expression) -> std::string {
    if (pointer == nullptr) {
        return {};
    }
    return std::string{expression} + " is not null\n    Which is: " + print_value(pointer);
}

template <typename T>
[[nodiscard]] auto not_null_message(const T& pointer, std::string_view expression) -> std::string {
    if (pointer != nullptr) {
        return {};
    }
    return std::string{expression} + " is null";
}

/// @brief 作用域追踪的 RAII 句柄：构造压栈、析构弹栈。
class TraceScope {
  public:
    explicit TraceScope(std::string note);
    TraceScope(const TraceScope&) = delete;
    auto operator=(const TraceScope&) -> TraceScope& = delete;
    TraceScope(TraceScope&&) = delete;
    auto operator=(TraceScope&&) -> TraceScope& = delete;
    ~TraceScope();

  private:
    bool attached_ = false;
};

}  // namespace detail
}  // namespace aurora::testing

// ---------------------------------------------------------------------------
// 宏层
// ---------------------------------------------------------------------------

/// @brief 标识符拼接（两级间接：`##` 会阻止操作数先展开，`__COUNTER__` 需先取值得到真实计数）。
#define AURORA_TEST_CAT_(a, b) AURORA_TEST_CAT_I_(a, b)  // NOLINT(*-identifier-naming)
#define AURORA_TEST_CAT_I_(a, b) a##b  // NOLINT(*-identifier-naming)
#define AURORA_TEST_UNIQUE_(prefix) AURORA_TEST_CAT_(aurora_test_##prefix, __COUNTER__)  // NOLINT(*-identifier-naming)

/// clang 把 `__COUNTER__` 归为 C2y 扩展并逐点告警（GCC / MSVC 不报），就地把该告警关掉；
/// 非 clang 编译器下展开为空。
#ifdef __clang__
#define AURORA_TEST_NO_C2Y_ _Pragma("clang diagnostic ignored \"-Wc2y-extensions\"")
#else
#define AURORA_TEST_NO_C2Y_  // NOLINT(*-identifier-naming)
#endif

/// @brief 记账原语：`diagnostic` 非空即为失败，严重级别由外层宏决定。
// NOLINTNEXTLINE(*-identifier-naming)
#define AURORA_TEST_REPORT_(severity, diagnostic)                                                                 \
    do {                                                                                                          \
        const std::string aurora_test_diagnostic = (diagnostic);                                                  \
        if (!aurora_test_diagnostic.empty()) {                                                                    \
            ::aurora::testing::detail::report((severity), __FILE__, __LINE__, std::move(aurora_test_diagnostic)); \
        }                                                                                                         \
    } while (false)

// NOLINTNEXTLINE(*-identifier-naming)
#define AURORA_TEST_CHECK_REPORT_(diagnostic) AURORA_TEST_REPORT_(::aurora::testing::Severity::NonFatal, diagnostic)
// NOLINTNEXTLINE(*-identifier-naming)
#define AURORA_TEST_REQUIRE_REPORT_(diagnostic) AURORA_TEST_REPORT_(::aurora::testing::Severity::Fatal, diagnostic)

/// @brief 语句包装：把「语句」包成可调用体交给异常判定内核执行（内核需亲自捕获抛出），
/// 内层 `if (always_true())` 既保证语句只执行一次，也吞掉不可达代码等告警。
/// // NOLINTNEXTLINE(*-identifier-naming)
#define AURORA_TEST_STATEMENT_(stmt)                    \
    [&]() -> void {                                     \
        if (::aurora::testing::detail::always_true()) { \
            stmt;                                       \
        }                                               \
    }

// ---- 布尔 / 消息 ----

/// @brief 非致命布尔断言。
#define AURORA_TEST_CHECK(...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::bool_message(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__))
/// @brief 致命布尔断言。
#define AURORA_TEST_REQUIRE(...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::bool_message(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__))

/// @brief 带自定义说明的布尔断言（复杂判定无法用谓词宏表达时使用）。
#define AURORA_TEST_CHECK_MSG(expr, ...) \
    AURORA_TEST_CHECK_REPORT_(           \
        ::aurora::testing::detail::message_message(static_cast<bool>((expr)), #expr, (__VA_ARGS__)))
/// @brief 带自定义说明的致命布尔断言。
#define AURORA_TEST_REQUIRE_MSG(expr, ...) \
    AURORA_TEST_REQUIRE_REPORT_(           \
        ::aurora::testing::detail::message_message(static_cast<bool>((expr)), #expr, (__VA_ARGS__)))

/// @brief 断言表达式为真（失败时打印 Actual / Expected）。
#define AURORA_TEST_CHECK_TRUE(...) \
    AURORA_TEST_CHECK_REPORT_(      \
        ::aurora::testing::detail::expected_bool_message(static_cast<bool>((__VA_ARGS__)), true, #__VA_ARGS__))
/// @brief 断言表达式为假。
#define AURORA_TEST_CHECK_FALSE(...) \
    AURORA_TEST_CHECK_REPORT_(       \
        ::aurora::testing::detail::expected_bool_message(static_cast<bool>((__VA_ARGS__)), false, #__VA_ARGS__))
/// @brief 致命版本：断言表达式为真。
#define AURORA_TEST_REQUIRE_TRUE(...) \
    AURORA_TEST_REQUIRE_REPORT_(      \
        ::aurora::testing::detail::expected_bool_message(static_cast<bool>((__VA_ARGS__)), true, #__VA_ARGS__))
/// @brief 致命版本：断言表达式为假。
#define AURORA_TEST_REQUIRE_FALSE(...) \
    AURORA_TEST_REQUIRE_REPORT_(       \
        ::aurora::testing::detail::expected_bool_message(static_cast<bool>((__VA_ARGS__)), false, #__VA_ARGS__))

// ---- 关系比较 ----

#define AURORA_TEST_CHECK_EQ(lhs, ...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::eq_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_CHECK_NE(lhs, ...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::ne_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_CHECK_LT(lhs, ...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::lt_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_CHECK_LE(lhs, ...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::le_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_CHECK_GT(lhs, ...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::gt_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_CHECK_GE(lhs, ...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::ge_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_CHECK_NEAR(lhs, rhs, ...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::near_message((lhs), (rhs), (__VA_ARGS__), #lhs, #rhs))

#define AURORA_TEST_REQUIRE_EQ(lhs, ...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::eq_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_REQUIRE_NE(lhs, ...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::ne_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_REQUIRE_LT(lhs, ...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::lt_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_REQUIRE_LE(lhs, ...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::le_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_REQUIRE_GT(lhs, ...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::gt_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_REQUIRE_GE(lhs, ...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::ge_message((lhs), (__VA_ARGS__), #lhs, #__VA_ARGS__))
#define AURORA_TEST_REQUIRE_NEAR(lhs, rhs, ...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::near_message((lhs), (rhs), (__VA_ARGS__), #lhs, #rhs))

// ---- 字符串内容比较（避免指针相等误判）----

#define AURORA_TEST_CHECK_STREQ(lhs, ...)                                   \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::string_message(    \
        #lhs, #__VA_ARGS__, ::aurora::testing::detail::string_view_of(lhs), \
        ::aurora::testing::detail::string_view_of((__VA_ARGS__)), true, true))
#define AURORA_TEST_CHECK_STRNE(lhs, ...)                                   \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::string_message(    \
        #lhs, #__VA_ARGS__, ::aurora::testing::detail::string_view_of(lhs), \
        ::aurora::testing::detail::string_view_of((__VA_ARGS__)), false, true))
#define AURORA_TEST_CHECK_STRCASEEQ(lhs, ...)                               \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::string_message(    \
        #lhs, #__VA_ARGS__, ::aurora::testing::detail::string_view_of(lhs), \
        ::aurora::testing::detail::string_view_of((__VA_ARGS__)), true, false))
#define AURORA_TEST_CHECK_STRCASENE(lhs, ...)                               \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::string_message(    \
        #lhs, #__VA_ARGS__, ::aurora::testing::detail::string_view_of(lhs), \
        ::aurora::testing::detail::string_view_of((__VA_ARGS__)), false, false))

#define AURORA_TEST_REQUIRE_STREQ(lhs, ...)                                 \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::string_message(  \
        #lhs, #__VA_ARGS__, ::aurora::testing::detail::string_view_of(lhs), \
        ::aurora::testing::detail::string_view_of((__VA_ARGS__)), true, true))
#define AURORA_TEST_REQUIRE_STRNE(lhs, ...)                                 \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::string_message(  \
        #lhs, #__VA_ARGS__, ::aurora::testing::detail::string_view_of(lhs), \
        ::aurora::testing::detail::string_view_of((__VA_ARGS__)), false, true))
#define AURORA_TEST_REQUIRE_STRCASEEQ(lhs, ...)                             \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::string_message(  \
        #lhs, #__VA_ARGS__, ::aurora::testing::detail::string_view_of(lhs), \
        ::aurora::testing::detail::string_view_of((__VA_ARGS__)), true, false))
#define AURORA_TEST_REQUIRE_STRCASENE(lhs, ...)                             \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::string_message(  \
        #lhs, #__VA_ARGS__, ::aurora::testing::detail::string_view_of(lhs), \
        ::aurora::testing::detail::string_view_of((__VA_ARGS__)), false, false))

// ---- 异常判定 ----

#define AURORA_TEST_CHECK_THROW(statement, exception_type)                               \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::throws_message<exception_type>( \
        AURORA_TEST_STATEMENT_(statement), #statement, #exception_type))
#define AURORA_TEST_CHECK_NO_THROW(statement) \
    AURORA_TEST_CHECK_REPORT_(                \
        ::aurora::testing::detail::no_throw_message(AURORA_TEST_STATEMENT_(statement), #statement))
#define AURORA_TEST_CHECK_ANY_THROW(statement) \
    AURORA_TEST_CHECK_REPORT_(                 \
        ::aurora::testing::detail::any_throw_message(AURORA_TEST_STATEMENT_(statement), #statement))

#define AURORA_TEST_REQUIRE_THROW(statement, exception_type)                               \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::throws_message<exception_type>( \
        AURORA_TEST_STATEMENT_(statement), #statement, #exception_type))
#define AURORA_TEST_REQUIRE_NO_THROW(statement) \
    AURORA_TEST_REQUIRE_REPORT_(                \
        ::aurora::testing::detail::no_throw_message(AURORA_TEST_STATEMENT_(statement), #statement))
#define AURORA_TEST_REQUIRE_ANY_THROW(statement) \
    AURORA_TEST_REQUIRE_REPORT_(                 \
        ::aurora::testing::detail::any_throw_message(AURORA_TEST_STATEMENT_(statement), #statement))

// ---- 指针空判定 ----

#define AURORA_TEST_CHECK_NULL(...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::null_message((__VA_ARGS__), #__VA_ARGS__))
#define AURORA_TEST_CHECK_NOT_NULL(...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::not_null_message((__VA_ARGS__), #__VA_ARGS__))
#define AURORA_TEST_REQUIRE_NULL(...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::null_message((__VA_ARGS__), #__VA_ARGS__))
#define AURORA_TEST_REQUIRE_NOT_NULL(...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::not_null_message((__VA_ARGS__), #__VA_ARGS__))

// ---- 无条件失败 / 跳过 / 追踪 ----

/// @brief 无条件记一次非致命失败（用于断言族无法表达的复杂判定）。
#define AURORA_TEST_FAIL(message) \
    ::aurora::testing::detail::report(::aurora::testing::Severity::NonFatal, __FILE__, __LINE__, (message))

/// @brief 无条件终止本用例并记为失败。
#define AURORA_TEST_FAIL_FATAL(message) \
    ::aurora::testing::detail::report(::aurora::testing::Severity::Fatal, __FILE__, __LINE__, (message))

/// @brief 无条件跳过本用例。用于后端 / 平台 feature 宏未开启的 `#else` 分支。
#define AURORA_TEST_SKIP(reason) ::aurora::testing::detail::skip_case((reason))

// ---- 平台能力守卫（能力缺失时跳过，而非伪装失败）----
//
// 判定集中在编译期，语义边界明确：跳过的是**平台能力**，不是被测逻辑。
// 与 `#if 特性宏` 的 `#else` 分支 skip 桩同一契约——计入 Skipped，不算失败、不伪造通过。
//
// 线程：Emscripten 未开 `-pthread` 时 `std::thread` 构造直接抛 "Not supported"；
//   开 pthread 会让产物要求 SharedArrayBuffer（浏览器侧需 COOP/COEP），属产品级取舍，
//   不由测试决定，故此处如实跳过。`__EMSCRIPTEN_PTHREADS__` 仅由 `-pthread` 定义（实测）。
// 子进程：Emscripten 运行时没有 fork/exec（`CreateProcess` 同理），跨进程语义用例
//   （多进程偏好一致性等）无法在 wasm 内等价构造。
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define AURORA_TEST_HAS_THREADS 0
#else
#define AURORA_TEST_HAS_THREADS 1
#endif

#if defined(__EMSCRIPTEN__)
#define AURORA_TEST_HAS_SUBPROCESS 0
#else
#define AURORA_TEST_HAS_SUBPROCESS 1
#endif

/// @brief 用例依赖 std::thread / 线程池：无线程能力时跳过本用例。
#if AURORA_TEST_HAS_THREADS
#define AURORA_TEST_REQUIRE_THREADS() static_cast<void>(0)
#else
#define AURORA_TEST_REQUIRE_THREADS() \
    AURORA_TEST_SKIP("std::thread 需 Emscripten 的 -pthread，本 wasm 构建未开启")
#endif

/// @brief 用例依赖派发子进程（fork/exec 或 CreateProcess）：无子进程能力时跳过本用例。
#if AURORA_TEST_HAS_SUBPROCESS
#define AURORA_TEST_REQUIRE_SUBPROCESS() static_cast<void>(0)
#else
#define AURORA_TEST_REQUIRE_SUBPROCESS() \
    AURORA_TEST_SKIP("跨进程用例需 fork/exec 派发子进程，Emscripten 运行时不可用")
#endif

/// @brief 作用域追踪：本作用域内的所有失败都附带这条上下文（对标 SCOPED_TRACE）。
#define AURORA_TEST_TRACE(message) \
    AURORA_TEST_NO_C2Y_ const ::aurora::testing::detail::TraceScope AURORA_TEST_UNIQUE_(trace_scope_) { (message) }
