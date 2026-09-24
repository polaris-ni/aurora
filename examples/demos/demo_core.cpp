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
    emit("\n--- 平台与架构（编译期宏判定）---");
    emit("平台宏: ", platform_macro_name());
    emit("架构宏: ", arch_macro_name());
    emit("指针宽度: ", sizeof(void *), " 字节");
    emit("注：以上由预处理期确定；若与本机构建平台不符，说明构建配置异常。");
}

// ---------------------------------------------------------------------------
// 日志级别：阈值过滤
// ---------------------------------------------------------------------------

/// @brief 解析级别名；无法识别时返回 false 且不修改 out。
/// @note 刻意用顺序判定而非常量查表：级别名到枚举的对应关系在此处即唯一权威，
/// 少一张需与本函数同步维护的表。
[[nodiscard]] auto parse_level(std::string_view name, au::LogLevel &out) -> bool {
    if (name == "trace") {
        out = au::LogLevel::Trace;
        return true;
    }
    if (name == "debug") {
        out = au::LogLevel::Debug;
        return true;
    }
    if (name == "info") {
        out = au::LogLevel::Info;
        return true;
    }
    if (name == "warn") {
        out = au::LogLevel::Warn;
        return true;
    }
    if (name == "error") {
        out = au::LogLevel::Error;
        return true;
    }
    if (name == "fatal") {
        out = au::LogLevel::Fatal;
        return true;
    }
    return false;
}

/// @brief 按声明顺序发出 6 个级别的日志，供人工核对阈值截断位置。
auto emit_all_levels() -> void {
    AURORA_LOG_TRACE("core-demo", "TRC 级：最低优先级，通常用于最详细的执行轨迹");
    AURORA_LOG_DEBUG("core-demo", "DBG 级：内部状态快照");
    AURORA_LOG_INFO("core-demo", "INF 级：常规运行信息");
    AURORA_LOG_WARN("core-demo", "WRN 级：可容忍的异常情况");
    AURORA_LOG_ERROR("core-demo", "ERR 级：操作失败，但进程可继续");
    AURORA_LOG_FATAL("core-demo", "FTL 级：不可恢复错误（本演示不中止进程）");
}

/// @brief 级别阈值组：先设阈值，再发全部级别，观察哪些被丢弃。
auto demo_log_level(au::LogLevel level) -> void {
    emit("\n--- 日志级别阈值（诊断日志应只出现在 stderr）---");
    au::Logger::instance().set_level(level);
    emit("阈值已设为: ", au::log_level_label(level), "（低于该级别的日志应被丢弃，不出现在 stderr）");
    emit("AURORA_LOG_RAW 走 stdout 且不受阈值影响，故上面这行在任何阈值下都应可见。");
    emit("[stderr 分界] 以下按 TRC→FTL 顺序发出 6 条诊断日志，低于阈值的不会出现在 stderr：");
    emit_all_levels();
}

// ---------------------------------------------------------------------------
// 诊断与降级
// ---------------------------------------------------------------------------

/// @brief 诊断收集组：warn 与 degraded 的级别差异、内存收集器、降级不中止。
auto demo_diagnostics() -> void {
    emit("\n--- 诊断收集与降级（桥接到 stderr，每行一条 JSON）---");

    // 无 code → 退化为 Warning / General，桥接 WRN 级。
    au::Diagnostics::warn("字体族缺少指定字重，已回退到 Regular", "demo_core.cpp");
    // code 命中 errors.toml（severity=error）→ 桥接 ERR 级；降级语义 = 非法输入替换为安全默认值。
    au::Diagnostics::degraded("布局约束 width 为负值，已钳制为 0", "demo_core.cpp", "general-invalid-argument");

    const auto recent = au::Diagnostics::get_last_diagnostics();
    emit("环形缓冲内诊断条数: ", recent.size());
    for (const auto &item : recent) {
        emit("  severity=", item.severity_str(), " category=", item.category_str(),
             " code=", item.code.empty() ? std::string_view{"<empty>"} : std::string_view{item.code});
    }
    emit("注：两条诊断均已发出而进程未中止 —— 这正是「降级而非终止」的行为。");
}

// ---------------------------------------------------------------------------
// 错误码解释
// ---------------------------------------------------------------------------

/// @brief 演示用已知 slug（取自 codespec/errors.toml）。
constexpr std::array<std::string_view, 2> AURORA_KNOWN_SLUGS{"general-unknown", "general-invalid-argument"};

/// @brief 解释组：已知码取 hint，未知码取兜底文案。
auto demo_error_explain() -> void {
    emit("\n--- 错误码解释（explain_diagnostic）---");
    for (const auto slug : AURORA_KNOWN_SLUGS) {
        emit("已知码 ", slug, " → ", au::Diagnostics::explain_diagnostic(slug));
    }
    emit("未知码 no-such-code → ", au::Diagnostics::explain_diagnostic("no-such-code"));
    emit("注：未知码应返回兜底文案并指路 codespec/ERROR_CATALOG.md，而非空串或崩溃。");
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
    emit("\n--- 严格模式：同一处降级应升级为硬失败 ---");
    au::set_strict_failure_handler([](std::string_view message) { throw StrictFailureSignal{std::string{message}}; });
    au::set_strict_mode(au::StrictMode::On);

    try {
        au::Diagnostics::degraded("严格模式下降级应触发硬失败", "demo_core.cpp", "general-invalid-argument");
        emit("结果: 未触发硬失败 —— 与预期不符");
    } catch (const StrictFailureSignal &signal) {
        emit("结果: 已捕获硬失败信号 → ", signal.message);
        emit("注：生产环境（未注入 handler）此处为 std::terminate()，进程即终止。");
    }

    au::set_strict_mode(au::StrictMode::Off);
    au::set_strict_failure_handler(nullptr);
}

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------

/// @brief 命令行选项。
struct Options {
    au::LogLevel level = au::LogLevel::Info;
    bool strict = false;
    bool show_help = false;
};

/// @brief 解析命令行；返回 false 表示用法错误（调用方以退出码 2 结束）。
[[nodiscard]] auto parse_args(int argc, char **argv, Options &out) -> bool {
    constexpr std::string_view level_prefix = "--level=";
    // 入口参数是 C 风格 argc/argv，按下标取值属本文件的既定边界，不做越界传播；
    // 口径与 tools/servers/aurora_cli.cpp 的入口一致。
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
            return true;
        }
        if (arg == "--strict") {
            out.strict = true;
            continue;
        }
        if (arg.starts_with(level_prefix)) {
            const auto name = arg.substr(level_prefix.size());
            if (!parse_level(name, out.level)) {
                AURORA_LOG_ERROR("core-demo", "无法识别的日志级别: ", name);
                return false;
            }
            continue;
        }
        AURORA_LOG_ERROR("core-demo", "无法识别的参数: ", arg);
        return false;
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return true;
}

/// @brief 用法说明。
auto print_usage() -> void {
    emit("demo_core — core 模块人工验收载体（纯控制台，无 GUI 依赖）");
    emit("");
    emit("用法: demo_core [选项]");
    emit("  --level=<trace|debug|info|warn|error|fatal>  日志级别阈值（缺省 info）");
    emit("  --strict                                     追加严格模式演示");
    emit("  --help, -h                                   显示本帮助");
    emit("");
    emit("观测要点：诊断日志在 stderr（带时间戳/级别/分类前缀），功能输出在本程序 stdout");
    emit("（无前缀）。建议分别重定向后用两个文件对照，例如：");
    emit("  demo_core --level=debug 1> out.txt 2> err.txt");
}

}  // namespace

// 入口允许库异常逃逸到 main（terminate 即失败路径）；示例不做 try/catch 包装。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    au::init_console();  // 最早设置 UTF-8 控制台代码页，避免中文乱码

    Options options;
    if (!parse_args(argc, argv, options)) {
        return 2;
    }
    if (options.show_help) {
        print_usage();
        return 0;
    }

    demo_platform();
    demo_log_level(options.level);
    demo_diagnostics();
    demo_error_explain();
    if (options.strict) {
        demo_strict_mode();
    }

    emit("\n--- core 载体演示结束：进程正常退出（未发生中止）---");
    return 0;
}
