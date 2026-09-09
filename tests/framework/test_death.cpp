#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "death_test.h"

#ifdef _WIN32
// 只需要进程与句柄 API。刻意不再定义 WIN32_LEAN_AND_MEAN：自定义宏受本仓库
// 「宏名须 AURORA_ 前缀」的命名门禁约束，而该宏由 SDK 头自行约定。
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aurora::testing::detail {

namespace {

/// @brief 本进程的死亡测试状态（子进程模式下由 CLI 写入）。
struct DeathState {
    std::uint64_t target = 0;  ///< 目标站点键；非 0 即处于子进程模式
    bool reached = false;  ///< 目标站点是否已被执行
};

[[nodiscard]] auto death_state() -> DeathState& {
    static DeathState state;
    return state;
}

/// @brief 自身可执行文件路径（由 main 用 argv[0] 登记；CTest 以绝对路径调用）。
[[nodiscard]] auto executable_slot() -> std::string& {
    static std::string path;
    return path;
}

/// @brief 采集文件名：放当前目录、不含空格与路径分隔符，父子同 cwd 即可对上。
///
/// ⚠️ 刻意不用 temp_directory_path()：MSYS 一类 shell 里 TMP 是 `/tmp`，跨端解释不一致，
/// 而带路径的值还要考虑引号转义。子进程自己 fopen 这个相对名，两边必然同一文件。
[[nodiscard]] auto capture_name(std::uint64_t site_key, int attempt) -> std::string {
    std::ostringstream name;
    name << "aurora_death_" << std::hex << site_key << '_' << attempt << ".log";
    return name.str();
}

/// @brief 读取文本文件（读不到时返回空串）。
[[nodiscard]] auto read_text(const std::string& path) -> std::string {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// @brief 从用例全名 `Suite.Case` 切出套件名（作为子进程的 --run 参数）。
[[nodiscard]] auto suite_of(std::string_view full_name) -> std::string_view {
    const auto dot = full_name.find('.');
    return dot == std::string_view::npos ? full_name : full_name.substr(0, dot);
}

[[nodiscard]] auto hex_of(std::uint64_t value) -> std::string {
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

/// @brief 派发子进程并等待结束；返回其退出码，派发失败返回 -1。
///
/// 刻意**不经 shell**（Windows 用 CreateProcess、POSIX 用 fork+execv）：
/// `std::system` 把命令交给 cmd，而 cmd 的引号剥离规则在多引号命令行上会把
/// 「程序名 + 参数」整体当成一个命令名，报「不是内部或外部命令」。
/// stderr 采集由子进程自己 freopen 完成，因此也不需要 shell 的重定向能力。
[[nodiscard]] auto spawn_and_wait(const std::string& program, const std::vector<std::string>& args) -> int {
#ifdef _WIN32
    std::string line = '"' + program + '"';
    for (const auto& argument : args) {
        line += " \"" + argument + '"';
    }
    std::vector<char> command{line.begin(), line.end()};
    command.push_back('\0');

    STARTUPINFOA info{};
    info.cb = sizeof(info);
    PROCESS_INFORMATION process{};
    const BOOL started =
        CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &info, &process);
    if (started == FALSE) {
        return spawn_failed();
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& argument : args) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        return spawn_failed();
    }
    if (child == 0) {
        execv(program.c_str(), argv.data());
        _exit(127);  // 派发失败：非 0、非哨兵，父进程按「未满足期望」报告
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        // 被信号打断则继续等待子进程结束。
    }
    if (WIFSIGNALED(status)) {
        return 139;  // 信号致死：非 0、非哨兵 → 判为「已致死」
    }
    return WEXITSTATUS(status);
#endif
}

}  // namespace

auto set_executable_path(std::string_view path) -> void { executable_slot() = std::string{path}; }

auto executable_path() -> const std::string& { return executable_slot(); }

auto death_site_not_reached() -> int { return 42; }

/// @brief 派发失败的哨兵退出码（与「未致死 0」「站点未到达 42」区分）。
auto spawn_failed() -> int { return -1; }

auto death_site_key(std::string_view file, int line) -> std::uint64_t {
    // FNV-1a 64：父/子为同一二进制，站点键只依赖「文件:行」文本。
    std::uint64_t hash = 1469598103934665603ULL;
    const auto feed = [&hash](std::string_view text) -> void {
        for (const auto c : text) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            hash *= 1099511628211ULL;
        }
    };
    feed(file);
    feed(":");
    feed(std::to_string(line));
    return hash;
}

auto enter_death_child(std::uint64_t site_key) -> void { death_state().target = site_key; }

auto death_child_mode() -> bool { return death_state().target != 0; }

auto death_child_should_run(std::string_view file, int line) -> bool {
    auto& state = death_state();
    if (state.target == 0 || state.target != death_site_key(file, line)) {
        return false;
    }
    state.reached = true;
    return true;
}

[[noreturn]] auto death_child_survived() -> void { std::_Exit(EXIT_SUCCESS); }

auto death_child_exit_code() -> int { return death_state().reached ? EXIT_SUCCESS : death_site_not_reached(); }

auto death_verdict_for(int status) -> DeathVerdict {
    if (status == EXIT_SUCCESS) {
        return DeathVerdict::Survived;
    }
    if (status == death_site_not_reached()) {
        return DeathVerdict::SiteMissed;
    }
    return DeathVerdict::Died;
}

auto spawn_death_child(const char* file, int line, std::string* output) -> DeathVerdict {
    const auto* context = current_context();
    if (context == nullptr || context->subject().empty()) {
        // 死亡测试必须在用例内执行：子进程要重跑同一个用例，才会走到语句所在站点。
        *output = "death test used outside a running test case";
        return DeathVerdict::SiteMissed;
    }
    if (executable_slot().empty()) {
        *output = "executable path not registered (main must call set_executable_path)";
        return DeathVerdict::SiteMissed;
    }

    const auto site = death_site_key(file, line);
    static int attempt = 0;
    const auto capture = capture_name(site, ++attempt);
    const std::string subject = context->subject();

    std::error_code ignored;
    std::filesystem::remove(capture, ignored);
    const std::vector<std::string> arguments{"--run=" + std::string{suite_of(subject)}, "--filter=" + subject,
                                             "--death-child=" + hex_of(site), "--death-capture=" + capture};
    const int status = spawn_and_wait(executable_slot(), arguments);
    *output = read_text(capture);
    std::filesystem::remove(capture, ignored);

    if (status == spawn_failed()) {
        *output = "failed to spawn the death-test child process";
        return DeathVerdict::SiteMissed;
    }
    return death_verdict_for(status);
}

}  // namespace aurora::testing::detail
