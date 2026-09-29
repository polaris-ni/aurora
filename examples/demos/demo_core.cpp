// =============================================================================
// core 模块人工验收载体（examples/demos/demo_core.cpp）
// -----------------------------------------------------------------------------
// `core/` 是唯一零 aurora 依赖的基础层，其可观测行为集中在**输出通道**（日志级别阈值、
// stderr/stdout 分流、诊断降级）与**编译期判定**（平台 / 架构宏），而非像素。因此本载体
// 是纯控制台程序：不 include demo_common.h、不创建窗口、不依赖任何 Surface 后端。
//
// 演示分组（逐组对应 codespec/manual-test/01-core.md 的人工用例）：
//   平台与架构   编译期宏判定（AURORA_PLATFORM_* / AURORA_ARCH_*）
//   日志通道     AURORA_LOG_* → stderr 带前缀；AURORA_LOG_RAW → stdout 无前缀
//   级别阈值     --level 控制输出阈值，低于阈值的日志被丢弃
//   诊断与降级   Diagnostics::warn / degraded，降级替换为安全默认值且不中止进程
//   错误码解释   explain_diagnostic：已知 slug 取其 hint，未知 slug 走兜底文案
//   严格模式     --strict 下同一处降级升级为硬失败
//
// 本程序**只演示、不断言**：不输出 PASS/FAIL，判定由人工对照输出完成。用法见 --help。
// =============================================================================

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/core/strict_mode.h"

namespace au = aurora;

namespace {

/// @brief stdout 功能输出（无前缀，不受级别阈值影响）。
/// @note 与 `AURORA_LOG_*` 的 stderr 分流是本载体的观测点之一，两者刻意不复用通道。
template <typename... Args>
auto emit(Args &&...args) -> void {
    AURORA_LOG_RAW("core-demo", std::forward<Args>(args)..., '\n');
}

// ---------------------------------------------------------------------------
// 平台与架构：编译期宏判定，取值由构建平台决定
// ---------------------------------------------------------------------------

/// @brief 当前生效的平台宏名（用于人工核对该构建是否为预期平台）。
[[nodiscard]] auto platform_macro_name() -> std::string_view {
#ifdef AURORA_PLATFORM_WINDOWS
    return "AURORA_PLATFORM_WINDOWS";
#elif defined(AURORA_PLATFORM_MACOS)
    return "AURORA_PLATFORM_MACOS";
#elif defined(AURORA_PLATFORM_LINUX)
    return "AURORA_PLATFORM_LINUX";
#elif defined(AURORA_PLATFORM_BSD)
    return "AURORA_PLATFORM_BSD";
#elif defined(AURORA_PLATFORM_WASM)
    return "AURORA_PLATFORM_WASM";
#elif defined(AURORA_PLATFORM_ANDROID)
    return "AURORA_PLATFORM_ANDROID";
#else
    return "<none>";
#endif
}

/// @brief 当前生效的架构宏名。
[[nodiscard]] auto arch_macro_name() -> std::string_view {
#ifdef AURORA_ARCH_X64
    return "AURORA_ARCH_X64";
#elif defined(AURORA_ARCH_X86)
    return "AURORA_ARCH_X86";
#elif defined(AURORA_ARCH_AARCH64)
    return "AURORA_ARCH_AARCH64";
#elif defined(AURORA_ARCH_ARM32)
    return "AURORA_ARCH_ARM32";
#elif defined(AURORA_ARCH_RISCV64)
    return "AURORA_ARCH_RISCV64";
#elif defined(AURORA_ARCH_WASM)
    return "AURORA_ARCH_WASM";
#else
    return "<none>";
#endif
}

/// @brief 平台与架构判定组。
auto demo_platform() -> void {
    emit("\n--- Platform and architecture (compile-time macro detection)---");
    emit("Platform macro: ", platform_macro_name());
    emit("Architecture macro: ", arch_macro_name());
    emit("Pointer width: ", sizeof(void *), " bytes");
    emit("Note: determined at preprocessing; a mismatch with the local build platform means a broken build.");
}

// ---------------------------------------------------------------------------
// 日志级别：阈值过滤
// ---------------------------------------------------------------------------

/// @brief 按声明顺序发出 6 个级别的日志，供人工核对阈值截断位置。
auto emit_all_levels() -> void {
    AURORA_LOG_TRACE("core-demo", "TRC level: lowest priority, typically the most detailed execution trace");
    AURORA_LOG_DEBUG("core-demo", "DBG level: internal state snapshot");
    AURORA_LOG_INFO("core-demo", "INF level: routine runtime information");
    AURORA_LOG_WARN("core-demo", "WRN level: tolerable abnormal condition");
    AURORA_LOG_ERROR("core-demo", "ERR level: operation failed, but the process can continue");
    AURORA_LOG_FATAL("core-demo", "FTL level: unrecoverable error (this demo does not abort the process)");
}

/// @brief 级别阈值组：先设阈值，再发全部级别，观察哪些被丢弃。
auto demo_log_level(au::LogLevel level) -> void {
    emit("\n--- Log level threshold (diagnostic logs should appear only on stderr)---");
    au::Logger::instance().set_level(level);
    emit("Threshold set to: ", au::log_level_label(level), " (logs below it are dropped, never on stderr)");
    emit("AURORA_LOG_RAW goes to stdout, unaffected by the threshold, so this line stays visible at any level.");
    emit("[stderr split] Next 6 diagnostic logs in TRC->FTL order; those below the threshold stay off stderr:");
    emit_all_levels();
}

// ---------------------------------------------------------------------------
// 诊断与降级
// ---------------------------------------------------------------------------

/// @brief 诊断收集组：warn 与 degraded 的级别差异、内存收集器、降级不中止。
auto demo_diagnostics() -> void {
    emit("\n--- Diagnostic collection and degradation (bridged to stderr, one JSON per line)---");

    // 无 code → 退化为 Warning / General，桥接 WRN 级。
    au::Diagnostics::warn("Font family lacks the requested weight, fell back to Regular", "demo_core.cpp");
    // code 命中 errors.toml（severity=error）→ 桥接 ERR 级；降级语义 = 非法输入替换为安全默认值。
    au::Diagnostics::degraded("Layout constraint width is negative, clamped to 0", "demo_core.cpp",
                              "general-invalid-argument");

    const auto recent = au::Diagnostics::get_last_diagnostics();
    emit("Diagnostics in the ring buffer: ", recent.size());
    for (const auto &item : recent) {
        emit("  severity=", item.severity_str(), " category=", item.category_str(),
             " code=", item.code.empty() ? std::string_view{"<empty>"} : std::string_view{item.code});
    }
    emit("Note: both diagnostics were emitted without aborting - degrade, not terminate, as designed.");
}

// ---------------------------------------------------------------------------
// 错误码解释
// ---------------------------------------------------------------------------

/// @brief 演示用已知 slug（取自 codespec/errors.toml）。
constexpr std::array<std::string_view, 2> AURORA_KNOWN_SLUGS{"general-unknown", "general-invalid-argument"};

/// @brief 解释组：已知码取 hint，未知码取兜底文案。
auto demo_error_explain() -> void {
    emit("\n--- Error code explanation (explain_diagnostic)---");
    for (const auto slug : AURORA_KNOWN_SLUGS) {
        emit("Known code ", slug, " -> ", au::Diagnostics::explain_diagnostic(slug));
    }
    emit("Unknown code no-such-code -> ", au::Diagnostics::explain_diagnostic("no-such-code"));
    emit("Note: unknown codes yield fallback text pointing to codespec/ERROR_CATALOG.md, not empty output/crash.");
}

// ---------------------------------------------------------------------------
// 严格模式
// ---------------------------------------------------------------------------

/// @brief 严格模式硬失败信号。
/// @note `on_strict_failure` 本身不可返回（生产路径为 `std::terminate()`）。本载体注入
/// 一个抛异常的 handler，把该路径转为可在同一进程内捕获的信号，使人工用例能并列对比
/// 「降级」与「硬失败」两种结果，而不必依赖进程终止码。
struct StrictFailureSignal {
    std::string message;
};

/// @brief 严格模式组：On 时同一处降级应升级为硬失败。
auto demo_strict_mode() -> void {
    emit("\n--- Strict mode: the same degradation should escalate to a hard failure ---");
    au::set_strict_failure_handler([](std::string_view message) { throw StrictFailureSignal{std::string{message}}; });
    au::set_strict_mode(au::StrictMode::On);

    try {
        au::Diagnostics::degraded("Degradation under strict mode should trigger a hard failure", "demo_core.cpp",
                                  "general-invalid-argument");
        emit("Result: no hard failure triggered - unexpected");
    } catch (const StrictFailureSignal &signal) {
        emit("Result: hard failure signal captured -> ", signal.message);
        emit("Note: in production (no handler injected) this calls std::terminate() and ends the process.");
    }

    au::set_strict_mode(au::StrictMode::Off);
    au::set_strict_failure_handler(nullptr);
}

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------

/// @brief 载体命令行声明表：解析、`--help` 文本与用法行全部由它派生（`aurora::cli`）。
/// @note `--level` 取 `ValueKind::LogLevel`，故「级别名 → 枚举」的权威是库内字面量表
/// （specification/09-cli.md §5），本文件不再自建一份需要同步维护的查表。
[[nodiscard]] auto build_spec() -> const au::cli::CommandSpec & {
    static const au::cli::CommandSpec ROOT_SPEC = [] {
        au::cli::CommandSpec root;
        root.name = "demo_core";
        root.about = "Manual acceptance carrier for the core module (pure console, no GUI dependency)";
        root.options = {
            au::cli::OptionSchema{
                .long_name = "level",
                .kind = au::cli::ValueKind::LogLevel,
                .help = "Log level threshold (defaults to info)",
                .value_hint = "LEVEL",
                .default_text = "info",
            },
            au::cli::OptionSchema{
                .long_name = "strict",
                .kind = au::cli::ValueKind::Bool,
                .arity = au::cli::Arity::flag(),
                .help = "Append the strict mode demo",
            },
        };
        root.epilog =
            R"(What to observe: diagnostic logs go to stderr (with timestamp/level/category prefixes),
functional output goes to this program's stdout (no prefix).
Redirect both separately and compare the two files, e.g.:
  demo_core --level=debug 1> out.txt 2> err.txt
Exit codes: 0 = normal end or --help, 2 = usage error.)";
        return root;
    }();
    return ROOT_SPEC;
}

}  // namespace

// 入口允许库异常逃逸到 main（terminate 即失败路径）；示例不做 try/catch 包装。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    au::init_console();  // 最早设置 UTF-8 控制台代码页，避免中文乱码

    const auto parsed = au::cli::parse(build_spec(), argc, argv);
    if (!parsed) {
        AURORA_LOG_ERROR("core-demo", parsed.error().message);
        return 2;
    }
    const au::cli::Invocation &invocation = parsed.value();
    if (invocation.shows_display()) {
        AURORA_LOG_RAW("core-demo", invocation.display_text);  // --help 已由声明表渲染
        return 0;
    }
    // 声明表带 default_text 且 kind 为 LogLevel，故该值必然存在且必然可读（不变量，无需判错）。
    const auto level = invocation.arguments.get<au::LogLevel>("level").value();
    const bool strict = invocation.arguments.flag("strict");

    demo_platform();
    demo_log_level(level);
    demo_diagnostics();
    demo_error_explain();
    if (strict) {
        demo_strict_mode();
    }

    emit("\n--- core carrier demo finished: process exited normally (no abort) ---");
    return 0;
}
