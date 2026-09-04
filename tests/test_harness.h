// test_harness.h — Aurora 轻量级注册式测试框架（v2，单头、零三方依赖）。
//
// 与旧版（全局失败计数 + 各文件自带 main）的差异：
//   - 用例通过 AURORA_TEST() 注册进进程级注册表，main 由框架唯一提供（au_test_main.cpp），
//     全部测试 TU 链入单一 runner 可执行，链接次数从「每文件一次」降为一次。
//   - 断言宏家族更名为 AURORA_TEST_CHECK*（旧 TCHECK* 名称不再提供）；
//     新增致命断言 AURORA_TEST_REQUIRE*（失败即终止当前用例）。
//   - 断言默认「通过静默」：仅在失败时输出 文件:行 + 表达式 + 可打印类型的实际值，
//     --verbose 恢复逐条 [PASS] 输出。
//
// 用法：
//   #include "test_harness.h"
//   AURORA_TEST() {                       // 用例名 = 本文件名 stem（CMake 注入 AURORA_TEST_NAME）
//       AURORA_TEST_CHECK(x == 1);
//       AURORA_TEST_CHECK_EQ(a, b);       // 失败时打印 a/b 实际值（可 operator<< 的类型）
//       AURORA_TEST_REQUIRE(init_ok);     // 致命：失败抛出终止本用例
//   }
//   平台/后端未编译进本构建时：AURORA_TEST_SKIP(原因宏);（注册为 skip 条目，空通过）
//
// 构建/运行（详见 cmake/AuroraTestsV2.cmake）：
//   每个测试 TU 经 set_source_files_properties 注入 AURORA_TEST_NAME="<文件名 stem>"；
//   runner CLI：--run=<名>（CTest 使用）| --filter=<子串> | --list | --verbose | -- <参数透传给用例>。
#pragma once
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/log.h"

// 每个测试 TU 的用例名（默认 = 文件名 stem），由 CMake 按源文件注入；
// 未注入时（如单文件草稿编译）回退为编译期文件名。
#ifndef AURORA_TEST_NAME
#define AURORA_TEST_NAME AURORA_FILE_NAME
#endif

namespace aurora::testing {

namespace detail {

/// 检测类型是否可 operator<< 输出（失败详情打印实际值；不可打印类型只输出表达式原文）。
template <typename T, typename = void>
struct IsStreamable : std::false_type {};  // NOLINT
template <typename T>
struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream &>() << std::declval<const T &>())>>
    : std::true_type {};

template <typename T>
[[nodiscard]] auto debug_value(const T &v) -> std::string {
    if constexpr (IsStreamable<T>::value) {
        std::ostringstream oss;
        oss << v;
        return oss.str();
    } else {
        return "<unprintable>";
    }
}

}  // namespace detail

/// 致命断言（AURORA_TEST_REQUIRE*）失败时抛出，由 runner 捕获并终止当前用例。
/// 派生自 std::exception 以符合「抛出类型应为 std::exception 派生」的通用约定；runner 先按
/// CheckAbort 精确捕获再兜底 std::exception，捕获顺序不变、行为一致。
struct CheckAbort : std::exception {};

/// 测试用例条目：fn 为空表示 skip 桩（skip_reason 非空）。
struct TestCase {
    std::string_view name;
    void (*fn)() = nullptr;
    std::string_view skip_reason;
};

/// 进程级用例注册表（静态注册；重名直接报错终止，防呆于链接期冲突之外的最常见事故）。
class Registry {
  public:
    static auto instance() -> Registry & {
        static Registry reg;
        return reg;
    }

    auto add(std::string_view name, void (*fn)()) -> void {
        assert_unique(name);
        m_tests.push_back({.name = name, .fn = fn, .skip_reason = {}});
    }

    auto add_skip(std::string_view name, std::string_view reason) -> void {
        assert_unique(name);
        m_tests.push_back({.name = name, .fn = nullptr, .skip_reason = reason});
    }

    /// 按名称排序后的稳定视图（首次调用时排序；输出顺序与链接顺序无关）。
    [[nodiscard]] auto sorted() -> const std::vector<TestCase> & {
        if (!m_sorted) {
            std::ranges::sort(m_tests, [](const TestCase &a, const TestCase &b) -> bool { return a.name < b.name; });
            m_sorted = true;
        }
        return m_tests;
    }

  private:
    Registry() = default;

    auto assert_unique(std::string_view name) const -> void {
        for (const auto &t : m_tests) {
            if (t.name == name) {
                AURORA_LOG_FATAL("test", "duplicate test name: ", name);
                std::abort();
            }
        }
    }

    std::vector<TestCase> m_tests;
    bool m_sorted = false;
};

/// 注册桩：静态对象的构造期副作用完成自注册（无需各文件手写 main / 调用清单）。
struct Registrar {
    Registrar(std::string_view name, void (*fn)()) { Registry::instance().add(name, fn); }
};

/// skip 桩注册：对应平台/后端 feature 宏未启用时注册一条空通过的 skip 条目。
struct SkipRegistrar {
    SkipRegistrar(std::string_view name, std::string_view reason) { Registry::instance().add_skip(name, reason); }
};

/// 当前正在运行的用例上下文（单线程 UI 假设，无锁）。
struct TestContext {
    std::string_view name;
    int failures = 0;
};

[[nodiscard]] inline auto current() -> TestContext & {
    static TestContext ctx;
    return ctx;
}

/// --verbose 时逐条输出 [PASS]（默认静默，避免数千行噪音）。
/// 刻意的可变框架全局：单线程测试 runner 在跑用例前置位，检查建议「改 const」不适用。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline bool g_verbose = false;

/// runner 透传给用例的参数（CLI 中 `--` 之后的参数；如 test_preferences 的多进程 writer 模式）。
/// 同上：单线程 runner 一次性写入、用例只读，属刻意的进程级可变状态，不宜 const 化。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline int g_pass_argc = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline const char *const *g_pass_argv = nullptr;

[[nodiscard]] inline auto pass_argc() -> int { return g_pass_argc; }
[[nodiscard]] inline auto pass_argv() -> const char *const * { return g_pass_argv; }

namespace detail {

inline auto record_fail(const char *file, int line, std::string_view what) -> void {
    auto &ctx = current();
    ++ctx.failures;
    // 直接调 Logger::log 以携带断言调用点的 file:line（AURORA_LOG_* 宏会带本函数的位置）。
    Logger::instance().log(AURORA_FILE_NAME, __LINE__, LogLevel::Error, "test",
                           aurora::detail::log_concat("[FAIL] ", ctx.name, " ", file, ":", line, ": ", what));
}

template <typename A, typename B>
[[nodiscard]] auto values_detail(const A &a, const B &b) -> std::string {
    if constexpr (IsStreamable<A>::value && IsStreamable<B>::value) {
        return aurora::detail::log_concat(" [actual: ", debug_value(a), " vs ", debug_value(b), "]");
    } else {
        return {};
    }
}

}  // namespace detail

/// 布尔断言核心：失败记录到当前用例上下文（非致命，继续执行后续断言）。
inline auto check_bool(bool ok, const char *file, int line, std::string_view what) -> void {
    if (ok) {
        if (g_verbose) {
            AURORA_LOG_INFO("test", "[PASS] ", current().name, ": ", what);
        }
        return;
    }
    detail::record_fail(file, line, what);
}

/// 致命断言核心：先记录失败，再抛 CheckAbort 终止当前用例。
[[noreturn]] inline void check_fatal(const char *file, int line, std::string_view what) {
    detail::record_fail(file, line, what);
    throw CheckAbort{};
}

template <typename A, typename B>
auto check_eq(const A &a, const B &b, const char *file, int line, std::string_view what) -> void {
    if (a == b) {
        if (g_verbose) {
            AURORA_LOG_INFO("test", "[PASS] ", current().name, ": ", what);
        }
        return;
    }
    detail::record_fail(file, line, aurora::detail::log_concat(what, detail::values_detail(a, b)));
}

template <typename A, typename B, typename Eps>
auto check_near(const A &a, const B &b, const Eps &eps, const char *file, int line, std::string_view what) -> void {
    if (a - b <= eps && b - a <= eps) {  // 对称差 <= eps（免 <cmath> 依赖，等价 |a-b| <= eps）
        if (g_verbose) {
            AURORA_LOG_INFO("test", "[PASS] ", current().name, ": ", what);
        }
        return;
    }
    detail::record_fail(
        file, line,
        aurora::detail::log_concat(what, detail::values_detail(a, b), " [eps ", detail::debug_value(eps), "]"));
}

// ---------------------------------------------------------------------------
// 浮点近似比较：规范定义在 aurora::testing 内（供新测试限定调用），
// 同时以 using 声明引入全局作用域，兼容既有测试代码里的裸 near_f/near_d 调用
// （旧框架即在头里以全局函数提供）。using 声明非定义，多 TU 包含无 ODR 问题。
inline auto near_f(float a, float b, float eps = 1e-3f) -> bool { return a - b <= eps && b - a <= eps; }
inline auto near_d(double a, double b, double eps = 1e-9) -> bool { return a - b <= eps && b - a <= eps; }

}  // namespace aurora::testing

using aurora::testing::near_d;
using aurora::testing::near_f;

// ---------------------------------------------------------------------------
// 宏层：注册 / 断言
// ---------------------------------------------------------------------------

/// 注册一个用例（用例名 = AURORA_TEST_NAME，即文件名 stem）。每个测试 TU 至多一次（与旧版每文件一个 main 对应）。
// 标识符拼接需两级间接：直接 `##` 会阻止操作数的宏展开（`__COUNTER__` 被当字面量贴入，
// 生成 `au_test_body___COUNTER__` 而非递增编号），经一层「先展开后粘贴」才能拿到真实计数值，
// 从而支持同文件内多次 AURORA_TEST()/AURORA_TEST_NAMED() 注册而不产生重定义。
#define AURORA_TEST_CAT(a, b) AURORA_TEST_CAT_I(a, b)
#define AURORA_TEST_CAT_I(a, b) a##b

// `__COUNTER__` 被 clang 归为 C2y 扩展（-Wc2y-extensions），在每个注册宏展开点报一次；
// GCC / MSVC 不报。计数器是 TU 内标识符唯一性的必要手段（换 __LINE__ 会让同一行多条注册
// 重定义），故在宏展开处就地发一条 diagnostic ignored 关掉它：仅 clang 生效，其余编译器展开
// 为空，避免 -Wunknown-pragmas。代价是该 TU 自首个注册点起不再报 c2y 类扩展告警。
#ifdef __clang__
#define AURORA_TEST_NO_C2Y _Pragma("clang diagnostic ignored \"-Wc2y-extensions\"")
#else
#define AURORA_TEST_NO_C2Y
#endif

#define AURORA_TEST_IMPL(name, ctr)                                               \
    static void AURORA_TEST_CAT(au_test_body_, ctr)();                            \
    namespace {                                                                   \
    /* NOLINTNEXTLINE */                                                          \
    static const ::aurora::testing::Registrar AURORA_TEST_CAT(au_test_reg_, ctr){ \
        name, &AURORA_TEST_CAT(au_test_body_, ctr)};                              \
    }                                                                             \
    /* NOLINTNEXTLINE */                                                          \
    static void AURORA_TEST_CAT(au_test_body_, ctr)()

#define AURORA_TEST_COUNTER __COUNTER__

#define AURORA_TEST() AURORA_TEST_NO_C2Y AURORA_TEST_IMPL(AURORA_TEST_NAME, AURORA_TEST_COUNTER)

/// 注册一个显式命名的用例（用于单文件多用例拆分；注意名字不得与其他 TU 重复）。
#define AURORA_TEST_NAMED(test_name) AURORA_TEST_NO_C2Y AURORA_TEST_IMPL(test_name, __COUNTER__)

/// 注册一条 skip 桩：对应 feature 宏未编译进本构建时空通过（放在 #else 分支）。
/// skip_reason 传宏标识符（如 AURORA_BACKEND_GLFW），沿用旧 TEST_SKIP 的字符串化语义（# 取字面量）。
#define AURORA_TEST_SKIP(skip_reason)                                                                           \
    AURORA_TEST_NO_C2Y                                                                                          \
    namespace {                                                                                                 \
    /* NOLINTNEXTLINE */                                                                                        \
    static const ::aurora::testing::SkipRegistrar AURORA_TEST_CAT(au_test_skip_, __COUNTER__){AURORA_TEST_NAME, \
                                                                                              #skip_reason};    \
    }

// 非致命断言（CHECK 族）：失败记录后继续执行。
#define AURORA_TEST_CHECK(cond) ::aurora::testing::check_bool((cond), __FILE__, __LINE__, #cond)
#define AURORA_TEST_CHECK_MSG(cond, msg) ::aurora::testing::check_bool((cond), __FILE__, __LINE__, (msg))
#define AURORA_TEST_CHECK_EQ(a, b) ::aurora::testing::check_eq((a), (b), __FILE__, __LINE__, #a " == " #b)
#define AURORA_TEST_CHECK_NE(a, b) ::aurora::testing::check_bool(((a) != (b)), __FILE__, __LINE__, #a " != " #b)
#define AURORA_TEST_CHECK_LT(a, b) ::aurora::testing::check_bool(((a) < (b)), __FILE__, __LINE__, #a " < " #b)
#define AURORA_TEST_CHECK_LE(a, b) ::aurora::testing::check_bool(((a) <= (b)), __FILE__, __LINE__, #a " <= " #b)
#define AURORA_TEST_CHECK_GT(a, b) ::aurora::testing::check_bool(((a) > (b)), __FILE__, __LINE__, #a " > " #b)
#define AURORA_TEST_CHECK_GE(a, b) ::aurora::testing::check_bool(((a) >= (b)), __FILE__, __LINE__, #a " >= " #b)
#define AURORA_TEST_CHECK_TRUE(a) ::aurora::testing::check_bool(!!(a), __FILE__, __LINE__, #a)
#define AURORA_TEST_CHECK_FALSE(a) ::aurora::testing::check_bool(!(a), __FILE__, __LINE__, "!" #a)
#define AURORA_TEST_CHECK_NEAR(a, b, eps) \
    ::aurora::testing::check_near((a), (b), (eps), __FILE__, __LINE__, #a " ~ " #b " (eps " #eps ")")

// 致命断言（REQUIRE 族）：失败即终止当前用例（抛 CheckAbort，runner 捕获）。
#define AURORA_TEST_REQUIRE(cond)                                      \
    do {                                                               \
        if (!(cond)) {                                                 \
            ::aurora::testing::check_fatal(__FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)
#define AURORA_TEST_REQUIRE_MSG(cond, msg)                             \
    do {                                                               \
        if (!(cond)) {                                                 \
            ::aurora::testing::check_fatal(__FILE__, __LINE__, (msg)); \
        }                                                              \
    } while (0)
#define AURORA_TEST_REQUIRE_EQ(a, b)                                          \
    do {                                                                      \
        if (!((a) == (b))) {                                                  \
            ::aurora::testing::check_fatal(__FILE__, __LINE__, #a " == " #b); \
        }                                                                     \
    } while (0)
