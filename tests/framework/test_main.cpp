// 测试框架入口：main 由框架唯一提供，测试文件禁止自定义 main()。
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aurora_test.h"
#include "death_test.h"
#include "isolation.h"
#include "reporter.h"
#include "test_selftest.h"

namespace {

using aurora::testing::CaseResult;
using aurora::testing::ExitCode;
using aurora::testing::RunSummary;
using aurora::testing::TestCase;
using aurora::testing::TestRegistry;

/// @brief 命令行解析结果。
struct CliOptions {
    bool list = false;  ///< --list：只列出用例，不执行
    bool verbose = false;  ///< --verbose：输出诊断笔记
    bool selftest = false;  ///< --selftest：跑框架内建自检
    bool help = false;  ///< --help / -h
    std::string list_format{"cases"};  ///< --format=cases|suites
    std::string run_suite;  ///< --run=<suite>：只跑指定套件
    std::string name_filter;  ///< --filter=<substr>：全名子串过滤
    std::string report_path;  ///< --report=<path>：结果报告（.xml → JUnit，其余 JSON）
    std::string death_child;  ///< --death-child=<键>：本进程是死亡测试子进程
    std::string death_capture;  ///< --death-capture=<file>：子进程把 stderr 接到该采集文件
    std::uint64_t shuffle_seed = 0;  ///< --shuffle=<seed>
    bool shuffle = false;
    int repeat = 1;  ///< --repeat=<n>
    int timeout_ms = 0;  ///< --timeout=<ms>：单轮总时限，0 表示不设
};

auto print_usage() -> void {
    std::printf(
        "aurora_test_runner - Aurora test framework runner\n"
        "\n"
        "Usage:\n"
        "  aurora_test_runner [options]\n"
        "\n"
        "Options:\n"
        "  --run=<suite>       run cases of the given suite (suite == test file stem)\n"
        "  --filter=<substr>   filter cases by substring of `Suite.Case`\n"
        "  --list              list registered cases and exit\n"
        "  --format=<fmt>      with --list: cases (default) | suites\n"
        "  --verbose           also print per-case diagnostic notes\n"
        "  --report=<path>     write results: .xml -> JUnit XML, otherwise JSON\n"
        "  --shuffle[=<seed>]  randomize case order (exposes order dependencies)\n"
        "  --repeat=<n>        run the selected cases n times (leaks state?)\n"
        "  --timeout=<ms>      overall deadline; partial results are still reported\n"
        "  --selftest          run the built-in framework self-test\n"
        "  -h, --help          show this help\n"
        "\n"
        "Exit codes:\n"
        "  0  all passed\n"
        "  1  at least one case failed\n"
        "  2  CLI error, no case matched the filter, or report could not be written\n"
        "  3  overall timeout hit (watchdog; partial results flushed to --report)\n");
}

/// @brief 数值参数解析（非数字 / 残留字符 / 越界均视为用法错误）。
///
/// std::from_chars 只接受指针区间，故此处是唯一一处指针算术。
template <typename T>
[[nodiscard]] auto parse_number(std::string_view text, T& destination, int base = 10) -> bool {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars 需要 [first, last)
    const std::pair<const char*, const char*> span{text.data(), text.data() + text.size()};
    const auto result = std::from_chars(span.first, span.second, destination, base);
    return result.ec == std::errc{} && result.ptr == span.second;
}

auto parse_cli(const std::span<char* const> args, CliOptions& options) -> bool {
    for (const auto* raw : args.subspan(1)) {
        const std::string_view arg{raw};
        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--list") {
            options.list = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--selftest") {
            options.selftest = true;
        } else if (arg == "--shuffle") {
            options.shuffle = true;
            options.shuffle_seed = 0;
        } else if (arg.starts_with("--shuffle=")) {
            options.shuffle = true;
            if (!parse_number(arg.substr(10), options.shuffle_seed)) {
                std::fprintf(stderr, "[test] bad --shuffle seed: %s\n", raw);
                return false;
            }
        } else if (arg.starts_with("--run=")) {
            options.run_suite = std::string{arg.substr(6)};
        } else if (arg.starts_with("--filter=")) {
            options.name_filter = std::string{arg.substr(9)};
        } else if (arg.starts_with("--format=")) {
            options.list_format = std::string{arg.substr(9)};
        } else if (arg.starts_with("--report=")) {
            options.report_path = std::string{arg.substr(9)};
        } else if (arg.starts_with("--death-child=")) {
            options.death_child = std::string{arg.substr(14)};
        } else if (arg.starts_with("--death-capture=")) {
            options.death_capture = std::string{arg.substr(16)};
        } else if (arg.starts_with("--repeat=")) {
            if (!parse_number(arg.substr(9), options.repeat) || options.repeat < 1) {
                std::fprintf(stderr, "[test] bad --repeat: %s\n", raw);
                return false;
            }
        } else if (arg.starts_with("--timeout=")) {
            if (!parse_number(arg.substr(10), options.timeout_ms) || options.timeout_ms < 0) {
                std::fprintf(stderr, "[test] bad --timeout: %s\n", raw);
                return false;
            }
        } else {
            std::fprintf(stderr, "[test] unknown argument: %s\n", raw);
            return false;
        }
    }
    if (options.list_format != "cases" && options.list_format != "suites") {
        std::fprintf(stderr, "[test] unknown --format: %s (expected cases|suites)\n", options.list_format.c_str());
        return false;
    }
    return true;
}

auto print_list(const CliOptions& options) -> int {
    const auto& registry = TestRegistry::instance();
    if (options.list_format == "suites") {
        for (const auto& suite : registry.suites()) {
            std::printf("%s\n", suite.c_str());
        }
        return static_cast<int>(ExitCode::AllPassed);
    }
    for (const auto* test_case : registry.cases()) {
        std::printf("%s\n", test_case->full_name().c_str());
    }
    return static_cast<int>(ExitCode::AllPassed);
}

/// @brief 已完成结果的共享槽：主线程逐条追加，watchdog 只读快照。
///
/// 文件级粒度下超时会让整进程退出，若不共享这份数据，「跑了一半超时」就等于「什么都没有」。
class ResultSink {
  public:
    auto push(CaseResult result) -> void {
        const std::scoped_lock<std::mutex> guard{mutex_};
        results_.push_back(std::move(result));
    }

    [[nodiscard]] auto snapshot() const -> std::vector<CaseResult> {
        const std::scoped_lock<std::mutex> guard{mutex_};
        return results_;
    }

    /// @brief 正常路径的唯一读者：直接移交，避免整份结果再复制一遍。
    [[nodiscard]] auto take() -> std::vector<CaseResult> {
        const std::scoped_lock<std::mutex> guard{mutex_};
        return std::move(results_);
    }

  private:
    mutable std::mutex mutex_;
    std::vector<CaseResult> results_;
};

/// @brief 超时看门狗：到点先把已完成结果落盘，再以退出码 3 结束进程。
///
/// 协作式退出（进程内无法强杀死循环线程）；进程级强杀由 CTest 的 TIMEOUT 属性承担，
/// 两层职责不重叠。
class TimeoutWatchdog {
  public:
    TimeoutWatchdog(int timeout_ms, const ResultSink& sink, std::string_view report_path) : sink_(&sink) {
        if (timeout_ms <= 0) {
            return;  // 未启用
        }
        const auto budget = std::chrono::milliseconds{timeout_ms};
        worker_ = std::thread([this, budget, timeout_ms, report_path]() -> void {
            std::unique_lock<std::mutex> lock{mutex_};
            if (done_.wait_for(lock, budget, [this]() -> bool { return stopped_; })) {
                return;  // 正常收尾，无需介入
            }
            const auto partial = sink_->snapshot();
            std::fflush(stdout);  // 先落已打印的用例进度，超时行才按真实顺序出现
            std::fprintf(stderr, "[test] TIMEOUT after %d ms (%zu case(s) finished)\n", timeout_ms, partial.size());
            std::fflush(stderr);
            if (!report_path.empty()) {
                std::string error;
                const auto summary = aurora::testing::summarize(partial);
                if (!aurora::testing::write_report(report_path, partial, summary, &error)) {
                    std::fprintf(stderr, "[test] %s\n", error.c_str());
                }
            }
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(static_cast<int>(ExitCode::Timeout));
        });
    }

    TimeoutWatchdog(const TimeoutWatchdog&) = delete;
    auto operator=(const TimeoutWatchdog&) -> TimeoutWatchdog& = delete;
    TimeoutWatchdog(TimeoutWatchdog&&) = delete;
    auto operator=(TimeoutWatchdog&&) -> TimeoutWatchdog& = delete;

    ~TimeoutWatchdog() { stop(); }

    auto stop() -> void {
        if (!worker_.joinable()) {
            return;
        }
        {
            const std::scoped_lock<std::mutex> guard{mutex_};
            stopped_ = true;
        }
        done_.notify_all();
        worker_.join();
    }

  private:
    std::mutex mutex_;
    std::condition_variable done_;
    std::thread worker_;
    bool stopped_ = false;
    const ResultSink* sink_ = nullptr;
};

/// @brief 报告落盘（正常路径）；写失败返回退出码 2。
auto emit_report(std::string_view path, const std::vector<CaseResult>& results, const RunSummary& summary) -> int {
    if (path.empty()) {
        return static_cast<int>(ExitCode::AllPassed);
    }
    std::string error;
    if (aurora::testing::write_report(path, results, summary, &error)) {
        return static_cast<int>(ExitCode::AllPassed);
    }
    std::fprintf(stderr, "[test] %s\n", error.c_str());
    return static_cast<int>(ExitCode::UsageOrNoMatch);
}

auto run_selected(const std::vector<const TestCase*>& selected, const CliOptions& options) -> int {
    // 死亡测试子进程：安静重跑同一用例，只按「站点是否到达 / 语句是否致死」给退出码。
    const bool silent = aurora::testing::detail::death_child_mode();
    auto order = selected;
    if (options.shuffle && order.size() > 1) {
        std::mt19937_64 engine{options.shuffle_seed};
        std::shuffle(order.begin(), order.end(), engine);
    }

    ResultSink sink;
    TimeoutWatchdog watchdog{silent ? 0 : options.timeout_ms, sink, options.report_path};

    const int rounds = silent ? 1 : options.repeat;
    for (int round = 0; round < rounds; ++round) {
        for (const auto* test_case : order) {
            if (!silent) {
                std::printf("[ RUN      ] %s\n", test_case->full_name().c_str());
            }
            // 用例边界资源隔离：进用例前开新临时目录并接管 TMP/TMPDIR/TEMP、兜底卸载
            // 剪贴板注入残留；出用例后清理临时目录、卸载注入。死亡测试子进程同样生效
            // （幂等，且子进程短暂存在的临时目录由自身 end_case 清理）。
            aurora::testing::isolation::begin_case();
            auto result = aurora::testing::run_case(*test_case);
            aurora::testing::isolation::end_case();
            if (!silent) {
                aurora::testing::print_case_result(result, options.verbose);
            }
            if (rounds > 1) {
                result.full_name += "#" + std::to_string(round);
            }
            sink.push(std::move(result));
        }
    }
    watchdog.stop();

    const auto results = sink.take();
    const auto summary = aurora::testing::summarize(results);
    if (!silent) {
        aurora::testing::print_summary(summary);
    }
    if (silent) {
        // 走到这里说明目标站点从未被执行（语句被条件挡住，或根本不在本用例里）。
        std::_Exit(aurora::testing::detail::death_child_exit_code());
    }
    const auto report_code = emit_report(options.report_path, results, summary);
    const auto run_code = aurora::testing::exit_code_for(summary);
    return run_code != static_cast<int>(ExitCode::AllPassed) ? run_code : report_code;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    const std::span<char* const> args{argv, static_cast<std::size_t>(argc)};
    if (argc > 0) {
        aurora::testing::detail::set_executable_path(*argv);  // argc > 0 已判，指针解引用而非下标
    }
    // 统一 cwd → 仓库根（可定位时）：相对路径（--report、用例内 golden/fixtures）以仓库根为基准。
    // 须在解析 CLI 之前完成，使所有相对路径解释一致；死亡测试子进程重跑 main 时同样生效。
    aurora::testing::isolation::setup();
    CliOptions options;
    if (!parse_cli(args, options)) {
        print_usage();
        return static_cast<int>(ExitCode::UsageOrNoMatch);
    }
    if (options.help) {
        print_usage();
        return static_cast<int>(ExitCode::AllPassed);
    }
    if (!options.death_child.empty()) {
        std::uint64_t key = 0;
        if (!parse_number(options.death_child, key, 16) || key == 0) {
            std::fprintf(stderr, "[test] bad --death-child: %s\n", options.death_child.c_str());
            return static_cast<int>(ExitCode::UsageOrNoMatch);
        }
        aurora::testing::detail::enter_death_child(key);
        if (!options.death_capture.empty()) {
            // 由子进程自己接管 stderr：命令行里就不需要任何 shell 重定向（cmd 的引号/路径规则最易出错）。
            if (std::freopen(options.death_capture.c_str(), "w", stderr) == nullptr) {
                std::fprintf(stderr, "[test] cannot capture stderr into %s\n", options.death_capture.c_str());
            }
        }
    }
    if (options.selftest) {
        return aurora::testing::run_framework_selftest();
    }
    // 参数化用例（TEST_P / TYPED_TEST）在静态初始化期只登记描述符，展开后才能被读取；
    // 所有静态初始化均先于 main，故此处一次展开即得全集（finalize 幂等）。
    TestRegistry::instance().finalize();
    if (options.list) {
        return print_list(options);
    }

    const auto& registry = TestRegistry::instance();
    const auto selected = aurora::testing::select_cases(registry.cases(), options.run_suite, options.name_filter);
    if (selected.empty()) {
        std::fprintf(stderr, "[test] no test case matched (run='%s', filter='%s')\n", options.run_suite.c_str(),
                     options.name_filter.c_str());
        return static_cast<int>(ExitCode::UsageOrNoMatch);
    }
    return run_selected(selected, options);
}
