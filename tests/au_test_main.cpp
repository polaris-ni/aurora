// au_test_main.cpp — 测试 runner 的唯一 main（框架私有，不属于任何用例）。
//
// 职责：解析 CLI → 从注册表筛选用例 → 逐条运行（每条用例重置上下文 + 异常隔离）→ 汇总与退出码。
// CLI：
//   --run=<名>     精确运行单个用例（CTest 每条 add_test 即此形态，进程隔离与旧版逐 exe 等价）
//   --filter=<子串> 运行名字含子串的全部用例
//   --list         按运行顺序列出全部用例名（供完整性守护脚本比对，每行一个名字）
//   --verbose      逐条输出 [PASS]（默认仅失败时输出详情）
//   -- <args...>   其余参数透传给用例（aurora::testing::pass_argc()/pass_argv()），如 test_preferences
//                  的多进程 writer 子模式：runner --run=test_preferences -- --writer <id> <file>
// 退出码：任一用例失败 → 1，否则 0（与旧版 g_test_failures 协议一致）。
#include <span>
#include <string>
#include <vector>

#include "test_harness.h"

namespace {

namespace ut = aurora::testing;

/// CLI 解析结果。
struct CliOptions {
    std::string run_name_;
    std::string filter_;
    bool list_only_ = false;
    int pass_start_ = 0;  ///< `--` 之后的首个参数下标（默认无透传时 = argc）
};

/// 解析命令行选项；`--verbose` 就地置位框架全局。返回 false 表示遇到未知选项（调用方退出码 2）。
/// 用 std::span 承载 argv 并以 subspan + range-for 遍历，避免裸指针算术与 operator[] 下标访问。
auto parse_cli(std::span<char *> args, CliOptions &opt) -> bool {
    const int argc = static_cast<int>(args.size());
    opt.pass_start_ = argc;
    if (args.empty()) {
        return true;
    }
    int i = 1;
    for (char const *p : args.subspan(1)) {  // 跳过 argv[0]，range-for 免下标/指针算术
        const std::string arg(p);
        if (arg == "--") {
            opt.pass_start_ = i + 1;
            break;
        }
        if (arg.starts_with("--run=")) {
            opt.run_name_ = arg.substr(6);
        } else if (arg.starts_with("--filter=")) {
            opt.filter_ = arg.substr(9);
        } else if (arg == "--list") {
            opt.list_only_ = true;
        } else if (arg == "--verbose") {
            ut::g_verbose = true;
        } else {
            AURORA_LOG_ERROR("test", "unknown option: ", arg, " (use --run= / --filter= / --list / --verbose / --)");
            return false;
        }
        ++i;
    }
    return true;
}

/// 合成 argv：以 [0]=程序名 起始，其后拼接 `--` 之后的透传参数，令用例看到与「独立 exe」完全一致的
/// argv 布局（argv[0]=可执行名、argv[1]=子命令/模式、argv[2..]=实参）。test_preferences 等跨进程用例
/// 据此把 argv[1] 当作 --writer/--delete 模式分派；其自身可执行路径由 self_exe() 取得，不依赖 argv[0]。
/// 直接暴露 runner 的原始 pass_argv（其 [0] 即模式）会导致索引错位，且在无 `--` 的父进程模式下 argv
/// 落到空槽而越界。
auto set_pass_args(std::span<char *> args, int pass_start) -> void {
    static std::vector<const char *> s_pass;
    s_pass.clear();
    s_pass.push_back(args.empty() ? "aurora_test_runner" : *args.begin());  // argv[0] 经 *begin() 取，免下标
    if (static_cast<size_t>(pass_start) <= args.size()) {
        for (char const *p : args.subspan(static_cast<size_t>(pass_start))) {  // `--` 之后的透传参数
            s_pass.push_back(p);
        }
    }
    ut::g_pass_argc = static_cast<int>(s_pass.size());
    ut::g_pass_argv = s_pass.data();
}

/// 按 --run / --filter 选择用例：--run 优先（精确匹配），其次 --filter（子串），二者皆无 → 全量。
auto select_tests(const std::vector<ut::TestCase> &tests, const CliOptions &opt) -> std::vector<const ut::TestCase *> {
    std::vector<const ut::TestCase *> selected;
    for (const auto &t : tests) {
        if (!opt.run_name_.empty() && t.name != opt.run_name_) {
            continue;
        }
        if (!opt.filter_.empty() && t.name.find(opt.filter_) == std::string_view::npos) {
            continue;
        }
        selected.push_back(&t);
    }
    return selected;
}

/// 重置上下文并执行用例主体，隔离异常：致命断言抛出的 CheckAbort 属预期控制流（失败已记录），
/// 其余未捕获异常统一折算为一次失败记录，避免单条用例崩溃拖垮整个 runner 进程。
auto invoke_test(const ut::TestCase &t) -> void {
    ut::current() = ut::TestContext{.name = t.name, .failures = 0};
    try {
        t.fn();
    }
    // NOLINTBEGIN(bugprone-empty-catch) 致命断言（AURORA_TEST_REQUIRE*）已在 record_fail 记入上下文，
    // 此处刻意吞掉 CheckAbort 让本用例正常收尾——空 catch 是有意的控制流终点，非疏漏。
    catch (const ut::CheckAbort &) {
    }
    // NOLINTEND(bugprone-empty-catch)
    catch (const std::exception &e) {
        ut::detail::record_fail("(runner)", 0, std::string("uncaught exception: ") + e.what());
    } catch (...) {
        ut::detail::record_fail("(runner)", 0, "uncaught non-standard exception");
    }
}

/// 运行单条用例（skip 桩直接计 skipped；否则执行并按失败数计 passed / failed）。
auto run_one(const ut::TestCase &t, int &passed, int &failed, int &skipped) -> void {
    if (t.fn == nullptr) {  // skip 桩（平台/后端未编译进本构建）
        AURORA_TEST_PRINTF("[SKIP] %.*s: %.*s not compiled into this build\n", static_cast<int>(t.name.size()),
                           t.name.data(), static_cast<int>(t.skip_reason.size()), t.skip_reason.data());
        ++skipped;
        return;
    }
    invoke_test(t);
    if (ut::current().failures == 0) {
        ++passed;
        if (ut::g_verbose) {
            AURORA_TEST_PRINTF("[ OK ] %.*s\n", static_cast<int>(t.name.size()), t.name.data());
        }
    } else {
        ++failed;
        AURORA_TEST_PRINTF("[FAIL] %.*s (%d check(s) failed)\n", static_cast<int>(t.name.size()), t.name.data(),
                           ut::current().failures);
    }
}

auto parse_and_run(int argc, char **argv) -> int {
    const std::span args(argv, argc);
    CliOptions opt;
    if (!parse_cli(args, opt)) {
        return 2;
    }
    set_pass_args(args, opt.pass_start_);

    const auto &tests = ut::Registry::instance().sorted();
    if (opt.list_only_) {
        for (const auto &t : tests) {
            AURORA_LOG_RAW("test", t.name, "\n");
        }
        return 0;
    }

    const auto selected = select_tests(tests, opt);
    if (!opt.run_name_.empty() && selected.empty()) {
        AURORA_LOG_ERROR("test", "test not found: ", opt.run_name_);
        return 2;
    }

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const auto *t : selected) {
        run_one(*t, passed, failed, skipped);
    }
    AURORA_TEST_PRINTF("=== %d test(s): %d passed, %d failed, %d skipped ===\n", static_cast<int>(selected.size()),
                       passed, failed, skipped);
    return failed > 0 ? 1 : 0;
}

}  // namespace

// 测试入口 main 允许把用例体内抛出的异常传播出去终止进程，无需在此吞掉异常
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int { return parse_and_run(argc, argv); }
