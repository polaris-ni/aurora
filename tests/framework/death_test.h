#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 死亡测试
// ------------------------------------------------------------
// `AURORA_TEST_CHECK_DEATH(statement, expectation)` 判定「statement 会让进程异常终止」
// （abort / 未捕获异常 / 段错误等），并可选地要求子进程的 stderr 命中期望片段。
//
// 实现取 GoogleTest 的 **重跑自身 + 站点匹配** 模型，因此 Windows 与 POSIX 同一套代码
// （POSIX 的 fork 快路径是可选优化，未实现——少一条分叉路径，跨平台行为更一致）：
//
//   父进程  ── spawn ──▶ 子进程（同一个 runner，带 --death-child=<站点键> --run --filter）
//      │                        │ 只有键相符的那条死亡断言会真正执行 statement
//      │ 等待 + 读 stderr 副本   │ 语句竟未致死 → _exit(0)
//      ▼                        │ 站点没走到（被 if 挡住等）→ _exit(42)
//   按退出码判定「是否致死」      ┘ 致死 → 进程异常终止，退出码非 0/42
//
// 站点键用 FNV-1a 64 散列 `__FILE__:__LINE__`，父/子为同一二进制故必然一致；
// 用散列而非把路径搬上命令行，避开空格与引号转发的坑，也无需为死亡站点单独建注册表。
// ============================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "assertions.h"
#include "aurora/core/platform.h"

namespace aurora::testing::detail {

/// @brief 子进程判定结果。
enum class DeathVerdict {
    Died,  ///< 进程异常终止（死亡测试通过的前提）
    Survived,  ///< 站点已到达但 statement 正常返回
    SiteMissed,  ///< 站点未被执行（statement 不该无条件致死，或被条件挡住）
};

/// @brief 子进程「站点未到达」的约定退出码（0=未致死，其他=已致死）。
///
/// 用函数而非命名空间级常量：本仓库要求全局常量为 `AURORA_` 前缀全大写。
[[nodiscard]] auto death_site_not_reached() -> int;

/// @brief 子进程**派发失败**的哨兵退出码（既不是「未致死」也不是「已致死」，须单独判定）。
[[nodiscard]] auto spawn_failed() -> int;

/// @brief 死亡测试站点键。
[[nodiscard]] auto death_site_key(std::string_view file, int line) -> std::uint64_t;

/// @brief 把 CLI 传入的十六进制站点键登记到本进程（仅子进程调用）。
auto enter_death_child(std::uint64_t site_key) -> void;

/// @brief 本进程是否处于死亡测试子进程模式。
[[nodiscard]] auto death_child_mode() -> bool;

/// @brief 子进程内：本站点是否就是目标站点。
[[nodiscard]] auto death_child_should_run(std::string_view file, int line) -> bool;

/// @brief 子进程内：目标站点已到达但 statement 未致死，直接以 0 结束进程。
[[noreturn]] auto death_child_survived() -> void;

/// @brief 子进程收尾用退出码：站点未到达 → 约定码；已到达但未致死 → 0。
[[nodiscard]] auto death_child_exit_code() -> int;

/// @brief 子进程退出码 → 判定（0=站点到达但未致死；约定码=站点没走到；其余=已致死）。
[[nodiscard]] auto death_verdict_for(int status) -> DeathVerdict;

/// @brief 父进程侧：spawn 子进程执行同一用例，收集其 stderr，返回判定。
[[nodiscard]] auto spawn_death_child(const char* file, int line, std::string* output) -> DeathVerdict;

/// @brief 登记自身可执行文件路径（main 用 argv[0] 调用；子进程据此组装命令行）。
auto set_executable_path(std::string_view path) -> void;

/// @brief 已登记的可执行文件路径（未登记时为空；隔离层据此定位仓库根）。
[[nodiscard]] auto executable_path() -> const std::string&;

/// @brief 期望描述（匹配器走 describe，字符串形态按原样引用）。
template <typename Expectation>
[[nodiscard]] auto death_expectation_text(const Expectation& expectation) -> std::string {
    if constexpr (requires { expectation.describe(); }) {
        return expectation.describe();
    } else {
        return "stderr contains " + std::string{std::string_view{expectation}};
    }
}

/// @brief 期望判定（匹配器走 matches，字符串形态按子串查找）。
template <typename Expectation>
[[nodiscard]] auto death_expectation_matches(const Expectation& expectation, const std::string& output) -> bool {
    using Plain = std::remove_cv_t<Expectation>;
    if constexpr (std::is_convertible_v<Plain, std::string_view>) {
        // 空期望 = 只要求「致死」，不校验输出内容。
        if (std::string_view{expectation}.empty()) {
            return true;
        }
    }
    if constexpr (requires { expectation.matches(output); }) {
        return expectation.matches(output);
    } else {
        return output.find(std::string_view{expectation}) != std::string::npos;
    }
}

/// @brief 父进程侧实现：spawn 子进程、收集 stderr、判定并记账。
///
/// 子进程模式里直接返回（嵌套死亡测试在子进程中被禁用，避免递归 spawn）。
template <typename Expectation>
auto check_death(const char* file, int line, std::string_view statement, const Expectation& expectation) -> void {
    if (death_child_mode()) {
        return;
    }
    std::string output;
    const auto verdict = spawn_death_child(file, line, &output);
    std::string message;
    switch (verdict) {
        case DeathVerdict::Survived:
            message = std::string{statement} + " did not terminate the process; expected death (" +
                      death_expectation_text(expectation) + ")";
            break;
        case DeathVerdict::SiteMissed:
            message = "death test site was never reached by " + std::string{statement} +
                      " (the statement is conditional, so a re-run child cannot reach it)";
            break;
        case DeathVerdict::Died:
            if (!death_expectation_matches(expectation, output)) {
                message = std::string{statement} + " died but stderr did not satisfy " +
                          death_expectation_text(expectation) + "\n    stderr: " + output;
            }
            break;
    }
    if (!message.empty()) {
        report(Severity::NonFatal, file, line, std::move(message));
    }
}

}  // namespace aurora::testing::detail

/// @brief 死亡测试断言：statement 必须使进程异常终止（可选：stderr 命中期望）。
///
/// `expectation` 可以是子串字面量，也可以是 `matchers::` 匹配器（如 `has_substr("...")`）。
/// 传空串表示不校验输出。
/// 子分支以 [[noreturn]] 的 death_child_survived() 收尾，控制流不会落入后续语句，故不写 else
/// （readability-else-after-return）。
///
/// ⚠️ Emscripten（wasm）下整条断言退化为 AURORA_TEST_SKIP：死亡测试依赖「重跑自身子进程」，
///    而 wasm 运行时没有 fork/exec（spawn_death_child 的 fork 直接失败），子进程无从派发，
///    硬跑只会恒定报 SiteMissed。跨编译下如实跳过，交由原生 job 守护。
#if defined(AURORA_PLATFORM_WASM)
#define AURORA_TEST_CHECK_DEATH(statement, ...) \
    AURORA_TEST_SKIP("死亡测试需 fork/exec 重跑自身进程，Emscripten 下不可用")
#else
#define AURORA_TEST_CHECK_DEATH(statement, ...)                                                  \
    do {                                                                                         \
        if (::aurora::testing::detail::death_child_should_run(__FILE__, __LINE__)) {             \
            /* 子进程：真正执行语句（此模式不做异常隔离，抛出即终止进程）；正常返回即未致死。 */ \
            AURORA_TEST_STATEMENT_(statement)();                                                 \
            ::aurora::testing::detail::death_child_survived();                                   \
        }                                                                                        \
        ::aurora::testing::detail::check_death(__FILE__, __LINE__, #statement, (__VA_ARGS__));   \
    } while (false)
#endif
