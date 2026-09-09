/// 测试类型: integration
/// 目标单元: include/aurora/preferences/preferences.h
/// 测试说明: Preferences 多进程并发语义——双 writer 子进程交错写入后键齐全且
///           无半写损坏（进程锁 + 原子 rename）、墓碑删除跨进程可靠传播、
///           全局清空纪元传播且清空后新键存活。子进程经「环境变量派发 +
///           --filter=child_entry」复用同一 runner 拉起（新 runner 不接受位置参数）；
///           无注入环境时 child_entry 直接 SKIP

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/preferences/preferences.h"
#include "framework/aurora_test.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aurora::test_cases::itest_preferences_multiproc {

namespace prefs = aurora::preferences;

namespace {

constexpr const char *kSuiteName = "itest_preferences_multiproc";
constexpr const char *kChildCase = "child_entry";

// writer 进程 flush 时瞬时持有目标文件可返回 ERROR_ACCESS_DENIED（存储实现既有的
// 偶发竞态，短暂的重命名共享冲突属可重试瞬态）：有限次退避重试，避免放大竞态窗口。
auto flush_retry(prefs::Preferences &p, const int attempts = 60) -> bool {
    for (int i = 0; i < attempts; ++i) {
        if (p.flush().ok()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

auto self_exe() -> std::filesystem::path {
#ifdef AURORA_PLATFORM_WINDOWS
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    // 测试助手：缓冲区长度已知，指针算术等价于 span 索引。
    return std::string{buf, buf + n};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        // 测试助手：readlink 返回字节数即写入长度。
        return std::string{buf, buf + n};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    return {};
#endif
}

/// @brief 子进程句柄：RAII 回收（wait 返回退出码并释放资源）。
struct ChildProcess {
#ifdef AURORA_PLATFORM_WINDOWS
    PROCESS_INFORMATION pi{};
#else
    pid_t pid = -1;
#endif

    auto wait() -> int {
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
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

#ifdef AURORA_PLATFORM_WINDOWS
    [[nodiscard]] auto still_active() const -> bool {
        DWORD code = 0;
        return (GetExitCodeProcess(pi.hProcess, &code) != 0) && code == STILL_ACTIVE;
    }
#else
    [[nodiscard]] auto still_active() const -> bool {
        int status = 0;
        return ::waitpid(pid, &status, WNOHANG) == 0;
    }
#endif
};

/// @brief 注入环境变量后拉起子进程：同一 runner，仅跑 child_entry 用例。
/// 环境在 CreateProcess/fork 时刻快照，两次派发间改 env 互不影响。
auto spawn_child(const std::string &mode, const std::string &id, const std::filesystem::path &file) -> ChildProcess {
#ifdef AURORA_PLATFORM_WINDOWS
    SetEnvironmentVariableA("AURORA_ITEST_MP_MODE", mode.c_str());
    SetEnvironmentVariableA("AURORA_ITEST_MP_ID", id.c_str());
    SetEnvironmentVariableA("AURORA_ITEST_MP_FILE", file.string().c_str());

    const std::string cmd =
        "\"" + self_exe().string() + "\" --run=" + kSuiteName + " --filter=" + kChildCase;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    ChildProcess child;
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &child.pi) == 0) {
        AURORA_TEST_FAIL_FATAL("CreateProcessA failed to spawn child runner");
    }
    return child;
#else
    ::setenv("AURORA_ITEST_MP_MODE", mode.c_str(), 1);
    ::setenv("AURORA_ITEST_MP_ID", id.c_str(), 1);
    ::setenv("AURORA_ITEST_MP_FILE", file.string().c_str(), 1);

    const std::string exe = self_exe().string();
    const std::string run_arg = std::string{"--run="} + kSuiteName;
    const std::string filter_arg = std::string{"--filter="} + kChildCase;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exe.c_str(), "aurora_test_runner", run_arg.c_str(), filter_arg.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ChildProcess child;
    child.pid = pid;
    return child;
#endif
}

// ---------- 子进程侧实现 ----------

auto run_writer_child(const std::filesystem::path &file) -> int {
    const char *id_s = std::getenv("AURORA_ITEST_MP_ID");
    AURORA_TEST_REQUIRE_MSG(id_s != nullptr, "writer child requires AURORA_ITEST_MP_ID");
    const int id = std::atoi(id_s);

    auto &p = prefs::Preferences::instance_at("itest_mp", file);
    constexpr int kKeys = 50;
    for (int j = 0; j < kKeys; ++j) {
        p.set("w" + std::to_string(id) + "_k" + std::to_string(j), (id * 1000) + j);
        if (j % 4 == 0 && !flush_retry(p)) {
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 让步，制造进程间交错
    }
    return flush_retry(p) ? 0 : 1;
}

auto run_delete_child(const std::filesystem::path &file) -> int {
    auto &p = prefs::Preferences::instance_at("itest_mp", file);
    // 构造即加载；验证确实看到了父进程写入的 victim（否则测试前提不成立）。
    AURORA_TEST_CHECK_EQ(p.get("victim", -1), 999);
    p.remove("victim");  // 打墓碑
    return flush_retry(p) ? 0 : 1;
}

auto run_clear_child(const std::filesystem::path &file) -> int {
    auto &p = prefs::Preferences::instance_at("itest_mp", file);
    (void)p.reload();  // 看到父进程写入的 c*
    p.clear();         // 全局清空纪元
    if (!flush_retry(p)) {
        return 1;
    }
    p.set("after_clear", 1);  // 清空之后的新键，应存活
    return flush_retry(p) ? 0 : 1;
}

}  // namespace

// ---------- 子进程派发入口（父进程以 --filter=child_entry 拉起本用例） ----------

AURORA_TEST_CASE(child_entry) {
    const char *mode = std::getenv("AURORA_ITEST_MP_MODE");
    const char *file_s = std::getenv("AURORA_ITEST_MP_FILE");
    if (mode == nullptr || file_s == nullptr) {
        AURORA_TEST_SKIP("父进程编排用例的子进程入口：直接运行（无注入环境）时无意义");
    }
    const std::filesystem::path file{file_s};
    const std::string m{mode};

    int rc = 0;
    if (m == "writer") {
        rc = run_writer_child(file);
    } else if (m == "delete") {
        rc = run_delete_child(file);
    } else if (m == "clear") {
        rc = run_clear_child(file);
    } else {
        AURORA_TEST_FAIL_FATAL("unknown AURORA_ITEST_MP_MODE: " + m);
    }
    AURORA_TEST_CHECK_EQ(rc, 0);
}

AURORA_TEST_CASE(concurrent_writers_final_consistency) {
    // 每用例唯一临时目录：文件随用例结束由框架清理。
    const std::filesystem::path file =
        std::filesystem::path{aurora::testing::isolation::temp_dir()} / "mp.json";

    // 拉起 2 个 writer 子进程。
    constexpr int kWriters = 2;
    std::vector<ChildProcess> children;
    children.reserve(kWriters);
    for (int i = 0; i < kWriters; ++i) {
        children.push_back(spawn_child("writer", std::to_string(i), file));
    }

    // 子进程运行期间周期性 reload：验证文件始终可被完整解析（无半写损坏）。
    bool running = true;
    for (int poll = 0; poll < 400 && running; ++poll) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        prefs::Preferences probe(file);
        const auto r = probe.reload();
        AURORA_TEST_CHECK_MSG(!(!r.ok() && r.error().code == "prefs-parse-failed"),
                              "multiproc concurrent writes must not cause half-written corruption");
        running = false;
        for (const auto &c : children) {
            if (c.still_active()) {
                running = true;
            }
        }
    }

    // 确保全部结束并取退出码。
    for (auto &c : children) {
        AURORA_TEST_CHECK_EQ(c.wait(), 0);
    }

    // 最终一致性：两个 writer 写入的键全部存在（验证进程锁防止互相覆盖丢数据）。
    prefs::Preferences p(file);
    AURORA_TEST_REQUIRE(p.reload().ok());
    constexpr int kKeys = 50;
    for (int id = 0; id < kWriters; ++id) {
        for (int j = 0; j < kKeys; ++j) {
            AURORA_TEST_CHECK_EQ(p.get("w" + std::to_string(id) + "_k" + std::to_string(j), -1), (id * 1000) + j);
        }
    }
}

AURORA_TEST_CASE(delete_and_clear_epoch_propagation) {
    const std::filesystem::path file =
        std::filesystem::path{aurora::testing::isolation::temp_dir()} / "del.json";

    // ---------- 阶段 1：删除传播 ----------
    {
        prefs::Preferences p(file);
        p.set("victim", 999);
        for (int i = 0; i < 50; ++i) {
            p.set("keep" + std::to_string(i), i);
        }
        AURORA_TEST_REQUIRE(flush_retry(p));
    }

    ChildProcess child_del = spawn_child("delete", "0", file);

    // 周期性 reload，验证 victim 最终消失（墓碑跨进程传播、可靠删除）。
    bool victim_gone = false;
    for (int poll = 0; poll < 500 && !victim_gone; ++poll) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        prefs::Preferences probe(file);
        if (probe.get("victim", -1) == -1) {
            victim_gone = true;
        }
    }
    AURORA_TEST_CHECK_MSG(victim_gone, "tombstone delete must propagate to other processes");
    AURORA_TEST_CHECK_EQ(child_del.wait(), 0);

    // 阶段 1 最终一致性：victim 删除，keep* 全部保留（删除不波及其他键）。
    {
        prefs::Preferences p(file);
        AURORA_TEST_REQUIRE(p.reload().ok());
        AURORA_TEST_CHECK_EQ(p.get("victim", -1), -1);
        for (int i = 0; i < 50; ++i) {
            AURORA_TEST_CHECK_EQ(p.get("keep" + std::to_string(i), -1), i);
        }
    }

    // ---------- 阶段 2：清空传播 + 清空后重建 ----------
    {
        prefs::Preferences p(file);
        for (int i = 0; i < 10; ++i) {
            p.set("c" + std::to_string(i), i);
        }
        AURORA_TEST_REQUIRE(flush_retry(p));
    }

    ChildProcess child_clr = spawn_child("clear", "0", file);
    AURORA_TEST_CHECK_EQ(child_clr.wait(), 0);

    {
        prefs::Preferences p(file);
        AURORA_TEST_REQUIRE(p.reload().ok());
        // 被清空的键全部消失（clear 是全局清空）。
        for (int i = 0; i < 10; ++i) {
            AURORA_TEST_CHECK_EQ(p.get("c" + std::to_string(i), -1), -1);
        }
        // 全局清空同样清掉阶段 1 的 keep* 与 victim。
        for (int i = 0; i < 50; ++i) {
            AURORA_TEST_CHECK_EQ(p.get("keep" + std::to_string(i), -1), -1);
        }
        AURORA_TEST_CHECK_EQ(p.get("victim", -1), -1);
        // 清空之后新建的键存活。
        AURORA_TEST_CHECK_EQ(p.get("after_clear", -1), 1);
    }
}

}  // namespace aurora::test_cases::itest_preferences_multiproc
