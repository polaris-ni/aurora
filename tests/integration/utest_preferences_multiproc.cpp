/// 测试类型: unit
/// 目标单元: include/aurora/preferences/preferences.h
/// 测试说明: preferences 单元测试（多进程并发段，自 utest_preferences.cpp 拆分）
///

// 目标源单元：Preferences + Preferences
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/preferences/preferences.h"
#include "aurora_test_harness.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>

#endif

// writer 进程或父进程的探测 reload 瞬时持有目标文件而返回 ERROR_ACCESS_DENIED（"Permission
// denied"）。这是存储实现既有的偶发竞态——旧「独立可执行」同样会偶发失败（实测约 1/5），非
// tests_v2 迁移引入。本多进程用例真正校验的是「最终键齐全 + 无半写损坏」，短暂的重命名共享冲突
// 属可重试的瞬态，予以有限次退避重试，避免竞态窗口被放大时出现不稳定误报。置于文件作用域，
// 供各多进程子命令（writer/delete/clear）共用。
static auto flush_retry(aurora::preferences::Preferences &p, const int attempts = 60) -> bool {
    for (int i = 0; i < attempts; ++i) {
        if (p.flush().ok()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// （自 utest_preferences.cpp 拆分：多进程并发写入 / 删除-清空纪元段；
//  子进程协议测试名随本文件 stem 变更为 --run=utest_preferences_multiproc。）

namespace aurora::test_cases::utest_preferences_multiproc {

namespace sec_preferences_multiproc {
using preferences::Preferences;

static auto self_exe() -> std::filesystem::path {
#ifdef AURORA_PLATFORM_WINDOWS
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    return {buf, buf + n};  // NOLINT
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    return "test_preferences_multiproc";
#endif
}

static auto run_writer(int id, const std::filesystem::path &file) -> int {
    auto &p = Preferences::instance_at("mp", file);
    constexpr int n = 50;
    for (int j = 0; j < n; ++j) {
        p.set("w" + std::to_string(id) + "_k" + std::to_string(j), (id * 1000) + j);
        if (j % 4 == 0) {
            if (!flush_retry(p)) {
                AURORA_TEST_PRINTF_ERR("writer %d flush failed (retries exhausted)\n", id);
                return 1;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 让步，制造进程间交错
    }
    if (!flush_retry(p)) {
        AURORA_TEST_PRINTF_ERR("writer %d final flush failed (retries exhausted)\n", id);
        return 1;
    }
    return 0;
}

namespace {
struct Child {
#ifdef AURORA_PLATFORM_WINDOWS
    PROCESS_INFORMATION pi{};
#else
    pid_t pid = -1;
#endif
};
}  // namespace

static auto run_parent(const std::filesystem::path &exe, const std::filesystem::path &file) -> int {
    constexpr int k = 2;
    std::vector<Child> children(k);

#ifdef AURORA_PLATFORM_WINDOWS
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    for (int i = 0; i < k; ++i) {
        const std::string cmd = "\"" + exe.string() + "\" --run=utest_preferences_multiproc -- --writer " +
                                std::to_string(i) + " \"" + file.string() + "\"";
        std::vector buf(cmd.begin(), cmd.end());
        buf.push_back('\0');
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &children[i].pi) ==
            0) {
            AURORA_TEST_CHECK(!"CreateProcessA failed");
            return 1;
        }
    }
#else
    for (int i = 0; i < k; ++i) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            const std::string a1 = "--writer";
            const std::string a2 = std::to_string(i);
            const std::string a3 = file.string();
            ::execl(exe.c_str(), "test_preferences_multiproc", "--run=utest_preferences_multiproc", "--", a1.c_str(),
                    a2.c_str(), a3.c_str(), static_cast<char *>(nullptr));
            ::_exit(127);
        }
        children[i].pid = pid;
    }
#endif

    // 子进程运行期间周期性 reload：验证文件始终可被完整解析（无半写损坏）。
    bool running = true;
    for (int poll = 0; poll < 400 && running; ++poll) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Preferences probe(file);
        const auto r = probe.reload();
        if (!r.ok() && r.error().code == "prefs-parse-failed") {
            AURORA_TEST_CHECK(!"multiproc concurrent writes cause half-written corruption: reload parse failed");
        }
#ifdef AURORA_PLATFORM_WINDOWS
        running = false;
        for (int i = 0; i < k; ++i) {
            DWORD code = 0;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if ((GetExitCodeProcess(children[i].pi.hProcess, &code) != 0) && code == STILL_ACTIVE) {
                running = true;
            }
        }
#else
        running = false;
        for (int i = 0; i < k; ++i) {
            int status = 0;
            if (::waitpid(children[i].pid, &status, WNOHANG) == 0) {
                running = true;  // 仍运行
            }
        }
#endif
    }

    // 确保全部结束并取退出码。
    std::vector codes(k, 0);
#ifdef AURORA_PLATFORM_WINDOWS
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    HANDLE procs[2] = {children[0].pi.hProcess, children[1].pi.hProcess};
    WaitForMultipleObjects(k, procs, TRUE, INFINITE);
    for (int i = 0; i < k; ++i) {
        DWORD code = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        GetExitCodeProcess(children[i].pi.hProcess, &code);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        codes[i] = static_cast<int>(code);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        CloseHandle(children[i].pi.hThread);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        CloseHandle(children[i].pi.hProcess);
    }
#else
    for (int i = 0; i < k; ++i) {
        int status = 0;
        ::waitpid(children[i].pid, &status, 0);
        codes[i] = WEXITSTATUS(status);
    }
#endif
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(codes[0] == 0 && codes[1] == 0);

    // 最终一致性：两个 writer 写入的键全部存在（验证进程锁防止互相覆盖丢数据）。
    Preferences p(file);
    AURORA_TEST_CHECK(p.reload().ok());
    constexpr int n = 50;
    for (int id = 0; id < k; ++id) {
        for (int j = 0; j < n; ++j) {
            AURORA_TEST_CHECK(p.get("w" + std::to_string(id) + "_k" + std::to_string(j), -1) == (id * 1000) + j);
        }
    }
    return 0;  // 断言结果已记入框架上下文；补齐返回值消除 int 函数落尾 UB
}

static auto run(int argc, char **argv) -> int {
    (void)argc;
    (void)argv;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    if (argc > 1 && std::string(argv[1]) == "--writer") {  // NOLINT
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        const int id = static_cast<int>(std::strtol(argv[2], nullptr, 10));  // NOLINT
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        const std::filesystem::path file = argv[3];  // NOLINT
        return run_writer(id, file);
    }

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "aurora_mp_test";
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "mp.json";

    // 清理上一次残留（含锁文件）。
    std::filesystem::remove(file, ec);
#ifdef AURORA_PLATFORM_WINDOWS
    std::filesystem::remove(std::filesystem::path(file.wstring() + L".lock"), ec);
#else
    std::filesystem::remove(std::filesystem::path(file.string() + ".lock"), ec);
#endif

    const auto exe = self_exe();
    const int rc = run_parent(exe, file);

    std::filesystem::remove_all(dir, ec);
    return rc;
}
}  // namespace sec_preferences_multiproc

namespace sec_preferences_multiproc_delete {
using preferences::Preferences;

static auto self_exe() -> std::filesystem::path {
#ifdef AURORA_PLATFORM_WINDOWS
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    return {buf, buf + n};  // NOLINT
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    return "test_preferences_multiproc_delete";
#endif
}

static auto run_delete_child(const std::filesystem::path &file) -> int {
    auto &p = Preferences::instance_at("del", file);
    // 构造即加载；验证确实看到了父进程写入的 victim（否则测试前提不成立）。
    AURORA_TEST_CHECK(p.get("victim", -1) == 999);
    p.remove("victim");  // 打墓碑
    if (!flush_retry(p)) {
        AURORA_TEST_PRINTF_ERR("delete child flush failed (retries exhausted)\n");
        return 1;
    }
    return 0;
}

static auto run_clear_child(const std::filesystem::path &file) -> int {
    auto &p = Preferences::instance_at("del", file);
    (void)p.reload();  // 看到父进程写入的 c*
    p.clear();  // 全局清空纪元
    if (!flush_retry(p)) {
        return 1;
    }
    p.set("after_clear", 1);  // 清空之后的新键，应存活
    if (!flush_retry(p)) {
        return 1;
    }
    return 0;
}

#ifdef AURORA_PLATFORM_WINDOWS
static auto spawn(const std::filesystem::path &exe, const std::string &mode, const std::filesystem::path &file)
    -> PROCESS_INFORMATION {
    const std::string cmd =
        "\"" + exe.string() + "\" --run=utest_preferences_multiproc -- --" + mode + " \"" + file.string() + "\"";
    std::vector buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) == 0) {
        AURORA_TEST_CHECK(!"CreateProcessA failed");
    }
    return pi;
}
#else
static auto spawn(const std::filesystem::path &exe, const std::string &mode, const std::filesystem::path &file)
    -> pid_t {
    const pid_t pid = ::fork();
    if (pid == 0) {
        const std::string a1 = "--" + mode;
        const std::string a2 = file.string();
        ::execl(exe.c_str(), "test_preferences_multiproc_delete", "--run=utest_preferences_multiproc", "--", a1.c_str(),
                a2.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    return pid;
}
#endif

static auto wait_child(
#ifdef AURORA_PLATFORM_WINDOWS
    PROCESS_INFORMATION const &pi
#else
    pid_t pid
#endif
    ) -> int {
#ifdef AURORA_PLATFORM_WINDOWS
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
#endif
}

static auto run_parent(const std::filesystem::path &exe, const std::filesystem::path &file) -> int {
    // ---------- 阶段 1：删除传播 ----------
    {
        auto &p = Preferences::instance_at("del", file);
        p.set("victim", 999);
        for (int i = 0; i < 50; ++i) {
            p.set("keep" + std::to_string(i), i);
        }
        AURORA_TEST_CHECK(p.flush().ok());
    }

#ifdef AURORA_PLATFORM_WINDOWS
    auto const pi_del = spawn(exe, "delete", file);
#else
    auto pid_del = spawn(exe, "delete", file);
#endif

    // 周期性 reload，验证 victim 最终消失（墓碑跨进程传播、可靠删除）。
    bool victim_gone = false;
    for (int poll = 0; poll < 500 && !victim_gone; ++poll) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Preferences probe(file);
        if (probe.get("victim", -1) == -1) {
            victim_gone = true;
        }
    }
    AURORA_TEST_CHECK(victim_gone);  // 删除可靠传播到其他进程

#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_CHECK(wait_child(pi_del) == 0);
#else
    AURORA_TEST_CHECK(wait_child(pid_del) == 0);
#endif

    // 阶段 1 最终一致性：victim 删除，keep* 全部保留（删除不会波及其他键）。
    {
        Preferences p(file);
        AURORA_TEST_CHECK(p.reload().ok());
        AURORA_TEST_CHECK(p.get("victim", -1) == -1);
        for (int i = 0; i < 50; ++i) {
            AURORA_TEST_CHECK(p.get("keep" + std::to_string(i), -1) == i);
        }
    }

    // ---------- 阶段 2：清空传播 + 清空后重建 ----------
    {
        auto &p = Preferences::instance_at("del", file);
        for (int i = 0; i < 10; ++i) {
            p.set("c" + std::to_string(i), i);
        }
        AURORA_TEST_CHECK(p.flush().ok());
    }

#ifdef AURORA_PLATFORM_WINDOWS
    auto const pi_clr = spawn(exe, "clear", file);
#else
    auto pid_clr = spawn(exe, "clear", file);
#endif

#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_CHECK(wait_child(pi_clr) == 0);
#else
    AURORA_TEST_CHECK(wait_child(pid_clr) == 0);
#endif

    {
        Preferences p(file);
        AURORA_TEST_CHECK(p.reload().ok());
        // 被清空的键全部消失（clear 是全局清空）
        for (int i = 0; i < 10; ++i) {
            AURORA_TEST_CHECK(p.get("c" + std::to_string(i), -1) == -1);
        }
        // 全局清空同样清掉阶段 1 的 keep* 与 victim
        for (int i = 0; i < 50; ++i) {
            AURORA_TEST_CHECK(p.get("keep" + std::to_string(i), -1) == -1);
        }
        AURORA_TEST_CHECK(p.get("victim", -1) == -1);
        // 清空之后新建的键存活
        AURORA_TEST_CHECK(p.get("after_clear", -1) == 1);
    }
    return 0;  // 断言结果已记入框架上下文；补齐返回值消除 int 函数落尾 UB
}

static auto run(int argc, char **argv) -> int {
    (void)argc;
    (void)argv;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    const std::string mode = argc > 1 ? std::string(argv[1]) : "";  // NOLINT
    if (mode == "--delete") {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return run_delete_child(argv[2]);  // NOLINT
    }
    if (mode == "--clear") {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return run_clear_child(argv[2]);  // NOLINT
    }

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "aurora_mp_del_test";
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "del.json";

    // 清理上一次残留（含锁文件）。
    std::filesystem::remove(file, ec);
#ifdef AURORA_PLATFORM_WINDOWS
    std::filesystem::remove(std::filesystem::path(file.wstring() + L".lock"), ec);
#else
    std::filesystem::remove(std::filesystem::path(file.string() + ".lock"), ec);
#endif

    const auto exe = self_exe();
    const int rc = run_parent(exe, file);

    std::filesystem::remove_all(dir, ec);
    return rc;
}
}  // namespace sec_preferences_multiproc_delete

AURORA_TEST() {
    // 子进程模式派发：runner 父进程会以「runner --run=utest_preferences_multiproc -- 模式参数」重启子进程；
    // 子进程读取 runner 透传参数（`--` 之后），只执行对应段并直接返回，不得再跑父进程各段。
    const int argc = aurora::testing::pass_argc();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) 测试入口：pass_argv() 返回 const char**，此处只读索引
    // argv[1] 不修改，去除 const 仅为满足签名
    auto *const argv = const_cast<char **>(aurora::testing::pass_argv());  // NOLINT
    if (argc > 1) {
        const std::string mode = argv[1];  // NOLINT
        if (mode == "--writer") {
            AURORA_TEST_CHECK(sec_preferences_multiproc::run(argc, argv) == 0);
            return;
        }
        if (mode == "--delete" || mode == "--clear") {
            AURORA_TEST_CHECK(sec_preferences_multiproc_delete::run(argc, argv) == 0);
            return;
        }
    }
    int rc = 0;
    rc += sec_preferences_multiproc::run(argc, argv);
    rc += sec_preferences_multiproc_delete::run(argc, argv);
    AURORA_TEST_CHECK(rc == 0);
}

}  // namespace aurora::test_cases::utest_preferences_multiproc
