#pragma once

// ============================================================
// 真实 TLS 服务端对端（tests/framework/tls_peer.h）
// ============================================================
//
// TLS 用例的对端不能是仓内伪造：证书链、主机名核对、记录分帧、close_notify 这几条判据的全部价值，
// 在于它们对着一个**真的会拒绝**的实现说话。本设施用宿主上现成的 `openssl` 命令行拉起一枚真实 TLS
// 服务端（`s_server`，只绑回环），证书资产由同一枚命令行现签（自签、SAN=DNS:localhost），于是：
// - 仓库不自带任何密钥资产（私钥不落 Git），断网可构建的不变量也不受影响——宿主没有 `openssl`
//   命令行时用例走 `AURORA_TEST_SKIP`，报成环境缺项而不是逻辑失败。
// - 对端说的每一句都是真话：握手失败、证书被拒、ALPN 未选中、客户端认证被要求，都由真实 TLS 栈给出。
//
// 与 `scripted_peer.h` 的分工：那一份接管**明文**对端字节（代理握手、隧道类故障、连上但不说话的停滞
// 形态），这一份接管 **TLS 层**对端（加密、证书、记录）。「取消落在未完成的握手」这类用例仍然用
// `scripts::silent()`——那正是「对端不回 TLS」的现场。
//
// 四条纪律：
// - **资产按用例生成、随用例目录一起删除**：全部文件落在 `isolation::temp_dir()`（§3.4 的临时文件纪律），
//   不做跨用例的「进程级共享证书」。共享一份资产的诱惑是省下每条几百毫秒的签发时间，代价是私钥的存活期
//   长过任何一条用例，而它并没有别的理由待在那里。
// - **参数一律经 argv 直送子进程，不经任何 shell**：这不是洁癖——MSYS 那类运行时会把 `/CN=localhost`
//   当成路径参数改写、把带冒号的值当列表拆分，而同一条命令在 CI 的 cmd 里又是另一个样子。只有 argv
//   形态在两腿语义相同。
// - **只绑 127.0.0.1**：`-accept 127.0.0.1:<port>`，端口由 `blocking::Listener` 绑定即取得后归还，
//   不存在把流量送出局外的路径。
// - **应答形态取 `-rev`**：`s_server` 的回显服务把读到的片段反转后写回，故「写出去的字节按 TLS 记录
//   原样回来」是可逐字节断言的，而不必借 HTTP 语义当载体。片段边界由对端的记录读取决定，因此逐字节相等
//   的断言只用**小于一条记录**的载荷；更大载荷只断言总字节数守恒。
//
// 框架属仓库私有设施，不进 include/、不进 aurora_api.json。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/net/blocking.h"
#include "framework/isolation.h"

// clang-format off
#ifdef AURORA_PLATFORM_WINDOWS
// 进程 spawn 是测试侧的宿主设施，不属于 net 层的平台代码（平台 socket 头的唯一落点仍是
// `src/aurora/net/socket.cpp`）；本头因此直接取 Win32 的进程与句柄 API。
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
// clang-format on

namespace aurora::test::tls {

/// @brief 自签资产的有效期形态。
///
/// `Expired` 是「证书 Validity 已过期」这条判据的唯一可控制造方式：本设施的资产全部现签，
/// 而有效期判定要的是一张**过去**的证书。宿主 `openssl` 不具备回填日期的旋钮（`-not_before` /
/// `-not_after` 是较新版本才有的选项）时，签法会直接失败，用例据此报成环境缺项而不是伪造一次拒绝。
/// 底型取 `std::uint8_t`：与公共侧的 `TlsVersion` 同形态，枚举只按值比较、不参与算术语境。
enum class Lifetime : std::uint8_t {
    Valid,  ///< 现行有效期（缺省）
    Expired,  ///< 有效期整体落在过去
};

/// @brief 平台无关进程设施的内部件（spawn / 等待 / 强杀），只服务本头自身的对外类型。
namespace detail {

/// @brief 一次进程启动的平台侧产物。
struct PlatformProcess {
    std::intptr_t first{0};  ///< Windows：`HANDLE` 进程对象；POSIX：子进程 pid
    bool started{false};  ///< 子进程是否真的被创建（false = 连 spawn 都没发生）
};

/// @brief 把 UTF-8 文本转成 Windows 侧的宽文本（POSIX 分支不消费本转换）。
/// @param text UTF-8 文本。
/// @return 宽文本；空文本返回空串。
inline auto wide_from_utf8(std::string_view text) -> std::wstring {
#ifdef AURORA_PLATFORM_WINDOWS
    if (text.empty()) {
        return {};
    }
    const auto count = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
#else
    return std::wstring{text.begin(), text.end()};
#endif
}

/// @brief 按 `CommandLineToArgvW` 的逆规则给一枚参数加引号。
///
/// 一律加引号是有意为之：路径里的空格与临时目录的名字由不得用例去挑，而「总是加引号」的规则不需要
/// 判断哪些字符是分隔符——少一条规则就少一处两腿不一致的机会。
/// @param argument 原始参数文本。
/// @return 可拼进 Windows 命令行的形态。
inline auto quote_windows_argument(std::string_view argument) -> std::wstring {
    std::wstring result{L"\""};
    std::size_t backslashes = 0;
    for (const char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        if (character == '"') {
            result.append((backslashes * 2) + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result += wide_from_utf8(std::string_view{&character, 1});
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

/// @brief 启动子进程：argv 直送，stdout 与 stderr 一并写进日志文件。
/// @param argv 参数向量，`argv[0]` 是可执行程序名（经宿主 PATH 解析）。
/// @param log_path 日志落点（覆盖写）。
/// @return 启动产物；`started` 为假表示没有创建任何进程。
inline auto launch(const std::vector<std::string> &argv, const std::filesystem::path &log_path) -> PlatformProcess {
    if (argv.empty()) {
        return {};
    }
#ifdef AURORA_PLATFORM_WINDOWS
    auto attributes = SECURITY_ATTRIBUTES{};
    attributes.nLength = static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES));
    attributes.bInheritHandle = TRUE;
    auto *file = ::CreateFileW(log_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring line;
    for (std::size_t index = 0; index < argv.size(); ++index) {
        if (index != 0) {
            line.push_back(L' ');
        }
        line += quote_windows_argument(argv[index]);
    }
    auto startup = STARTUPINFOW{};
    startup.cb = sizeof(STARTUPINFOW);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = file;
    startup.hStdError = file;
    auto info = PROCESS_INFORMATION{};
    std::vector<wchar_t> mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');
    const auto ok = ::CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &info);
    ::CloseHandle(file);  // 子进程已拿到自己的那份，父侧立即关闭，免得日志文件一直处于打开态
    if (ok == FALSE) {
        return {};
    }
    ::CloseHandle(info.hThread);
    // 进程句柄只能以整型存进平台无关的 `PlatformProcess::first`（POSIX 腿放 pid）：HANDLE 与 intptr_t
    // 之间的往返由 Win32 规定为可逆，改不成类型安全的形式。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return PlatformProcess{.first = reinterpret_cast<std::intptr_t>(info.hProcess), .started = true};
#else
    const int descriptor = ::open(log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (descriptor < 0) {
        return {};
    }
    const auto pid = ::fork();
    if (pid == 0) {
        ::dup2(descriptor, STDOUT_FILENO);
        ::dup2(descriptor, STDERR_FILENO);
        ::close(descriptor);
        std::vector<char *> arguments;
        arguments.reserve(argv.size() + 1);
        for (const auto &argument : argv) {
            arguments.push_back(const_cast<char *>(argument.c_str()));
        }
        arguments.push_back(nullptr);
        ::execvp(arguments.front(), arguments.data());
        ::_exit(127);  // exec 失败：以明确退出码收摊，父侧据此判不可用而不是无限等待
    }
    ::close(descriptor);
    if (pid < 0) {
        return {};
    }
    return PlatformProcess{.first = static_cast<std::intptr_t>(pid), .started = true};
#endif
}

/// @brief 有界等待子进程退出并取回退出码。
/// @param process 启动产物。
/// @param budget 等待上限（超时即强杀并按「没取到」回报）。
/// @return 退出码；未取到为 -1。
inline auto wait_exit(const PlatformProcess &process, std::chrono::milliseconds budget) -> int {
    if (!process.started) {
        return -1;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    // `PlatformProcess::first` 按平台无关形态存整型，取回句柄只能反向 reinterpret（Win32 保证该往返可逆）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    auto *handle = reinterpret_cast<HANDLE>(process.first);
    if (::WaitForSingleObject(handle, static_cast<DWORD>(budget.count())) != WAIT_OBJECT_0) {
        ::TerminateProcess(handle, 1U);
        return -1;
    }
    DWORD code = 0;
    return ::GetExitCodeProcess(handle, &code) != FALSE ? static_cast<int>(code) : -1;
#else
    const auto deadline = std::chrono::steady_clock::now() + budget;
    auto status = 0;
    for (;;) {
        const auto done = ::waitpid(static_cast<pid_t>(process.first), &status, WNOHANG);
        if (done == static_cast<pid_t>(process.first)) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(static_cast<pid_t>(process.first), SIGKILL);
            ::waitpid(static_cast<pid_t>(process.first), &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
#endif
}

/// @brief 强杀子进程并回收（幂等；对已退出的进程同样安全）。
/// @param process 启动产物（可 `started` 为假）。
inline auto terminate(const PlatformProcess &process) -> void {
    if (!process.started) {
        return;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    // 同上：整型反解回 HANDLE 只能靠 Win32 保证的可逆往返。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    auto *handle = reinterpret_cast<HANDLE>(process.first);
    ::TerminateProcess(handle, 1U);
    ::WaitForSingleObject(handle, 2000UL);
#else
    const auto pid = static_cast<pid_t>(process.first);
    ::kill(pid, SIGKILL);
    auto status = 0;
    ::waitpid(pid, &status, 0);
#endif
}

/// @brief 归还平台侧句柄（POSIX 无句柄可还；Windows 关进程句柄）。
/// @param process 启动产物。
inline auto release(const PlatformProcess &process) -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    if (process.first != 0) {
        // 同上：整型反解回 HANDLE 只能靠 Win32 保证的可逆往返。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
        ::CloseHandle(reinterpret_cast<HANDLE>(process.first));
    }
#else
    (void)process;
#endif
}

/// @brief 子进程的 RAII 外壳：析构即强杀并归还。
class ChildProcess {
  public:
    // 句柄型 RAII 外壳：拥有权归本对象，移动只会让「谁负责关闭」变得含糊，故四类特殊成员一律禁掉。
    ChildProcess(const ChildProcess &) = delete;
    auto operator=(const ChildProcess &) -> ChildProcess & = delete;
    ChildProcess(ChildProcess &&) = delete;
    auto operator=(ChildProcess &&) -> ChildProcess & = delete;

    /// @brief 默认构造：未启动状态。
    ChildProcess() = default;
    /// @brief 析构：仍在跑就强杀。
    ~ChildProcess() {
        stop();
        release(process_);
    }

    /// @brief 启动一条子进程。
    /// @param argv 参数向量。
    /// @param log_path stdout/stderr 的落点。
    /// @return 真的创建进程为 true。
    auto start(const std::vector<std::string> &argv, const std::filesystem::path &log_path) -> bool {
        process_ = launch(argv, log_path);
        return process_.started;
    }

    /// @brief 强杀仍在跑的子进程（可重复调用）。
    auto stop() -> void {
        if (process_.started) {
            terminate(process_);
            process_.started = false;
        }
    }

    /// @brief 是否仍在跑。
    /// @return 已启动且未 `stop()` 为 true。
    [[nodiscard]] auto running() const -> bool { return process_.started; }

    /// @brief 有界等待退出。
    /// @param budget 等待上限。
    /// @return 退出码；超时或被杀为 -1。
    [[nodiscard]] auto wait(std::chrono::milliseconds budget) const -> int { return wait_exit(process_, budget); }

  private:
    PlatformProcess process_{};  ///< 平台侧产物
};

/// @brief 本用例的临时目录（`isolation::temp_dir()` 的唯一消费点，本头内部再拼文件名）。
/// @return 已存在的目录路径。
inline auto case_directory() -> std::filesystem::path {
    return std::filesystem::path{aurora::testing::isolation::temp_dir()};
}

/// @brief 在本用例目录里取一个不重名的文件路径。
/// @param stem 文件名前缀。
/// @param extension 扩展名（含点）。
/// @return 可直接创建的路径。
inline auto next_path(std::string_view stem, std::string_view extension) -> std::filesystem::path {
    static std::atomic<unsigned> sequence{0};
    return case_directory() /
           (std::string{stem} + "-" + std::to_string(sequence.fetch_add(1)) + std::string{extension});
}

/// @brief 写一个文本文件。
/// @param path 目标路径。
/// @param text 内容。
/// @return 落盘成功为 true。
inline auto write_text_file(const std::filesystem::path &path, std::string_view text) -> bool {
    auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

/// @brief 读一个文件的全部内容（二进制安全：PKCS#12 也走这一枚）。
/// @param path 目标路径。
/// @return 文件字节；读不到为空串。
inline auto read_file(const std::filesystem::path &path) -> std::string {
    auto stream = std::ifstream{path, std::ios::binary};
    if (!stream) {
        return {};
    }
    auto buffer = std::ostringstream{};
    buffer << stream.rdbuf();
    return buffer.str();
}

/// @brief 一次同步命令执行的结局。
struct CommandResult {
    bool spawned = false;  ///< 是否真的创建了子进程
    int exit_code = -1;  ///< 退出码（未取到为 -1）
    std::string output;  ///< stdout 与 stderr 的合并文本
};

/// @brief 一步资产签发的结局（多命令串成一步，失败原因须能原样带出来）。
struct IssueResult {
    bool ok = false;  ///< 全部步骤是否都成功
    std::string message;  ///< 失败原因（成功为空）
};

/// @brief 同步执行一条命令（argv 直送，输出收进本用例目录后回读）。
/// @param argv 参数向量。
/// @param budget 等待上限。
/// @return 结局。
inline auto run_and_wait(const std::vector<std::string> &argv,
                         std::chrono::milliseconds budget = std::chrono::milliseconds{60000}) -> CommandResult {
    const auto log_path = next_path("cmd", ".log");
    ChildProcess child;
    CommandResult result;
    result.spawned = child.start(argv, log_path);
    if (!result.spawned) {
        result.output = "the command could not be spawned";
        return result;
    }
    result.exit_code = child.wait(budget);
    result.output = read_file(log_path);
    return result;
}

/// @brief 把一次命令结局写成可诊断的文本。
/// @param command 命令名（进文本，便于定位是哪一步）。
/// @param result 结局。
/// @return 一行诊断。
inline auto describe(std::string_view command, const CommandResult &result) -> std::string {
    auto text = std::string{command};
    if (!result.spawned) {
        return text + " could not be spawned";
    }
    text += " exited with " + std::to_string(result.exit_code) + ": ";
    const auto newline = result.output.find('\n');
    text += result.output.substr(0, newline == std::string::npos ? result.output.size() : newline);
    return text;
}

/// @brief 宿主上是否有可用的 `openssl` 命令行（进程内探测一次并缓存）。
///
/// 探测而不是查 PATH：`openssl version` 的退出码就是「这条命令能跑」的判据，比任何路径猜测都准，
/// 且在两腿、两种 shell、有无 vcpkg 的环境里给出同一个答案。
/// @return 可用为 true。
inline auto openssl_available() -> bool {
    // 函数级 static const 按仓内惯例（`socket.cpp` 的 `PINNED`/`SUPPORTED`）用全大写。
    static const bool PRESENT = [] {
        const auto result = run_and_wait({"openssl", "version"});
        return result.spawned && result.exit_code == 0;
    }();
    return PRESENT;
}

/// @brief 签一张自签服务端证书（SAN 含给定名字）。
/// @param certificate 证书落点。
/// @param private_key 私钥落点。
/// @param common_name 主体名与 SAN 的名字。
/// @param lifetime 有效期形态；`Expired` 把整段有效期回填到过去。
/// @return 结局。
inline auto openssl_issue(const std::filesystem::path &certificate, const std::filesystem::path &private_key,
                          std::string_view common_name, Lifetime lifetime = Lifetime::Valid) -> IssueResult {
    auto argv = std::vector<std::string>{"openssl",
                                         "req",
                                         "-x509",
                                         "-newkey",
                                         "rsa:2048",
                                         "-keyout",
                                         private_key.string(),
                                         "-out",
                                         certificate.string(),
                                         "-days",
                                         "30",
                                         "-nodes",
                                         "-subj",
                                         "/CN=" + std::string{common_name},
                                         "-addext",
                                         "subjectAltName=DNS:" + std::string{common_name}};
    if (lifetime == Lifetime::Expired) {
        // `-not_after` 覆盖 `-days`，两枚时刻一起回填才能让「现在」落到区间之外。
        argv.insert(argv.end(), {"-not_before", "20200101000000Z", "-not_after", "20200102000000Z"});
    }
    const auto issued = run_and_wait(argv);
    if (issued.exit_code != 0) {
        return {.ok = false, .message = describe("openssl req", issued)};
    }
    return {.ok = true, .message = {}};
}

/// @brief 以服务端自签证书为 CA 签一张客户端证书并打包成 PKCS#12。
///
/// 同一张证书与同一枚私钥的 **PEM 文本**也留在原地（`client_cert` / `client_key`）：`TlsContext` 的
/// 客户端身份有 PKCS#12 与 PEM 两种入参形态，而 PEM 那一条只有 OpenSSL / mbedTLS 腿支持——两种形态必须
/// 能用同一份材料各验一遍，否则「PEM 在这条腿上通了」就只能是没被证过的说法。
/// @param certificate 服务端证书（充当 CA）。
/// @param private_key 服务端私钥（签名用）。
/// @param passphrase 档案口令。
/// @param identity 档案落点（输出）。
/// @param client_key 客户端私钥的 PEM 落点（输出，未加密）。
/// @param client_cert 客户端证书的 PEM 落点（输出）。
/// @return 结局。
inline auto openssl_issue_client_identity(const std::filesystem::path &certificate,
                                          const std::filesystem::path &private_key, const std::string &passphrase,
                                          const std::filesystem::path &identity,
                                          const std::filesystem::path &client_key,
                                          const std::filesystem::path &client_cert) -> IssueResult {
    const auto request = next_path("client-csr", ".pem");
    const auto extensions = next_path("client-ext", ".cnf");
    if (!write_text_file(extensions, "extendedKeyUsage=clientAuth\nbasicConstraints=CA:FALSE\n")) {
        return {.ok = false, .message = "the client certificate extension file could not be written"};
    }
    const auto requested = run_and_wait({"openssl", "req", "-newkey", "rsa:2048", "-keyout", client_key.string(),
                                         "-out", request.string(), "-nodes", "-subj", "/CN=aurora-client"});
    if (requested.exit_code != 0) {
        return {.ok = false, .message = describe("openssl req", requested)};
    }
    const auto chained = run_and_wait({"openssl", "x509", "-req", "-in", request.string(), "-out", client_cert.string(),
                                       "-CA", certificate.string(), "-CAkey", private_key.string(), "-CAcreateserial",
                                       "-days", "30", "-extfile", extensions.string()});
    if (chained.exit_code != 0) {
        return {.ok = false, .message = describe("openssl x509", chained)};
    }
    const auto packed =
        run_and_wait({"openssl", "pkcs12", "-export", "-out", identity.string(), "-inkey", client_key.string(), "-in",
                      client_cert.string(), "-passout", "pass:" + passphrase});
    if (packed.exit_code != 0) {
        return {.ok = false, .message = describe("openssl pkcs12", packed)};
    }
    if (!std::filesystem::exists(identity)) {
        return {.ok = false, .message = "openssl pkcs12 reported success but left no archive behind"};
    }
    return {.ok = true, .message = {}};
}

}  // namespace detail

/// @brief 一个用例的自签 TLS 资产：服务端证书 + 私钥 +（按需）mTLS 客户端身份。
///
/// 资产文件落在本用例的临时目录里，随 `isolation::end_case()` 一并删除；私钥的存活期不长于用例，
/// 这是「仓库不带密钥、临时目录也不留密钥」这条纪律的落地形态。
class Assets {
  public:
    // 资产是一份临时目录里的私钥材料，拷贝/移动都会让「谁负责清理」变得含糊，四类特殊成员一律禁掉。
    Assets(const Assets &) = delete;
    auto operator=(const Assets &) -> Assets & = delete;
    Assets(Assets &&) = delete;
    auto operator=(Assets &&) -> Assets & = delete;
    // 析构只释放纯值成员；文件清理由用例隔离目录负责，本对象不自持句柄。
    ~Assets() = default;

    /// @brief 现签一份资产。
    /// @param common_name 证书的主体名与 SAN（本设施只用于回环对端，缺省 `localhost`）。
    /// @param with_identity 是否连带生成 mTLS 客户端身份（PKCS#12 与 PEM 两种入参形态同源）。
    /// @param lifetime 有效期形态；`Expired` 用于「Validity 已过期一定被拒」那条判据。
    explicit Assets(std::string_view common_name = "localhost", bool with_identity = false,
                    Lifetime lifetime = Lifetime::Valid) {
        if (!detail::openssl_available()) {
            status_ = "openssl command line is not present on this host";
            return;
        }
        const auto name = std::string{common_name};
        certificate_ = detail::next_path("server-cert", ".pem");
        private_key_ = detail::next_path("server-key", ".pem");
        const auto issued = detail::openssl_issue(certificate_, private_key_, name, lifetime);
        if (!issued.ok) {
            status_ = issued.message;
            return;
        }
        certificate_pem_ = detail::read_file(certificate_);
        if (certificate_pem_.empty()) {
            status_ = "the issued certificate could not be read back";
            return;
        }
        valid_ = true;
        if (with_identity) {
            client_identity_ = detail::next_path("client-identity", ".p12");
            client_key_pem_ = detail::next_path("client-key", ".pem");
            client_cert_pem_ = detail::next_path("client-cert", ".pem");
            const auto packed = detail::openssl_issue_client_identity(
                certificate_, private_key_, passphrase_, client_identity_, client_key_pem_, client_cert_pem_);
            if (!packed.ok) {
                status_ = "server certificate is usable but the client identity could not be packed: " + packed.message;
            } else {
                identity_ = true;
            }
        }
    }

    /// @brief 服务端证书与私钥是否齐备。
    /// @return 可用为 true。
    [[nodiscard]] auto valid() const -> bool { return valid_; }

    /// @brief mTLS 客户端身份是否齐备（未请求生成时恒为 false）。
    /// @return 可用为 true。
    [[nodiscard]] auto identity_valid() const -> bool { return identity_; }

    /// @brief 缺项诊断文本。
    /// @return 诊断；可用态为空串。
    [[nodiscard]] auto status() const -> std::string { return status_; }

    /// @brief 服务端证书路径。
    /// @return 路径。
    [[nodiscard]] auto certificate() const -> std::filesystem::path { return certificate_; }

    /// @brief 服务端私钥路径。
    /// @return 路径。
    [[nodiscard]] auto private_key() const -> std::filesystem::path { return private_key_; }

    /// @brief 客户端身份（PKCS#12）路径。
    /// @return 路径。
    [[nodiscard]] auto client_identity() const -> std::filesystem::path { return client_identity_; }

    /// @brief 客户端身份的 PKCS#12 字节内容（`TlsContext::client_identity` 的入参形态）。
    /// @return 档案字节；不可用为空串。
    [[nodiscard]] auto client_identity_bytes() const -> std::string {
        return identity_ ? detail::read_file(client_identity_) : std::string{};
    }

    /// @brief 保护口令。
    /// @return 口令文本。
    [[nodiscard]] auto passphrase() const -> const std::string & { return passphrase_; }

    /// @brief 客户端证书的 PEM 文本（`TlsContext::client_certificate_pem` 的入参形态）。
    /// @return PEM 文本；不可用为空串。
    [[nodiscard]] auto client_certificate_pem() const -> std::string {
        return identity_ ? detail::read_file(client_cert_pem_) : std::string{};
    }

    /// @brief 客户端私钥的 PEM 文本（未加密；`TlsContext::client_private_key_pem` 的入参形态）。
    /// @return PEM 文本；不可用为空串。
    [[nodiscard]] auto client_private_key_pem() const -> std::string {
        return identity_ ? detail::read_file(client_key_pem_) : std::string{};
    }

    /// @brief 服务端证书的 PEM 文本（`TlsContext::trusted_certificates` 的入参形态）。
    /// @return PEM 文本。
    [[nodiscard]] auto certificate_pem() const -> std::string { return certificate_pem_; }

  private:
    bool valid_{false};  ///< 服务端那一半是否可用
    bool identity_{false};  ///< 客户端身份是否可用
    std::string status_;  ///< 缺项诊断
    std::filesystem::path certificate_;  ///< 服务端证书
    std::filesystem::path private_key_;  ///< 服务端私钥
    std::filesystem::path client_identity_;  ///< 客户端身份档案
    std::filesystem::path client_key_pem_;  ///< 客户端私钥（PEM，同一份身份的另一种入参形态）
    std::filesystem::path client_cert_pem_;  ///< 客户端证书（PEM，同上）
    std::string passphrase_{"aurora-test"};  ///< 档案口令
    std::string certificate_pem_;  ///< 证书 PEM 文本
};

/// @brief 一台真实的 TLS 服务端：回环上的一枚 `openssl s_server` 子进程。
///
/// 一个对象 = 一条服务，可先后服务多条连接（`s_server` 是单连接循环），因此「握手两次取到同一枚 SPKI
/// 摘要」这类跨连接判据可以在同一条服务上跑。构造即完成「挑空闲端口 → 起进程 → 探到端口真的在听」，
/// 返回后用例可直接连接而不必自己等。
/// @note Thread: 单线程构造与析构（用例线程）
/// @note Side-effects: 起子进程、写临时日志、占一个回环端口
class TlsPeer {
  public:
    TlsPeer(const TlsPeer &) = delete;
    auto operator=(const TlsPeer &) -> TlsPeer & = delete;
    TlsPeer(TlsPeer &&) = delete;
    auto operator=(TlsPeer &&) -> TlsPeer & = delete;

    /// @brief 起服务并等到端口可连。
    /// @param assets 本用例的证书资产。
    /// @param extra_arguments 追加给 `s_server` 的参数（`-alpn`、`-Verify`、`-tls1_2` 之类）。
    /// @param echo_reversed 是否用回显模式（`-rev`）；置假即 `-www` 状态页模式。
    explicit TlsPeer(const Assets &assets, std::vector<std::string> extra_arguments = {}, bool echo_reversed = true) {
        if (!assets.valid()) {
            status_ = "tls assets are unavailable: " + assets.status();
            return;
        }
        port_ = reserve_loopback_port();
        if (port_ == 0) {
            status_ = "no free loopback port";
            return;
        }
        std::vector<std::string> argv{"openssl",
                                      "s_server",
                                      "-accept",
                                      "127.0.0.1:" + std::to_string(port_),
                                      "-cert",
                                      assets.certificate().string(),
                                      "-key",
                                      assets.private_key().string(),
                                      echo_reversed ? "-rev" : "-www",
                                      "-quiet"};
        for (auto &argument : extra_arguments) {
            argv.push_back(std::move(argument));
        }
        log_path_ = detail::next_path("s_server", ".log");
        if (!child_.start(argv, log_path_)) {
            status_ = "cannot spawn the tls peer process";
            port_ = 0;
            return;
        }
        if (!wait_until_listening(port_, std::chrono::milliseconds{8000})) {
            status_ = "tls peer never started listening: " + detail::read_file(log_path_);
            child_.stop();
            port_ = 0;
            return;
        }
        valid_ = true;
    }

    /// @brief 析构：强杀子进程。
    ~TlsPeer() { child_.stop(); }

    /// @brief 服务是否真的在听。
    /// @return 可用为 true。
    [[nodiscard]] auto valid() const -> bool { return valid_; }

    /// @brief 服务实际占用的回环端口。
    /// @return 端口；无效态为 0。
    [[nodiscard]] auto port() const -> std::uint16_t { return port_; }

    /// @brief 构造失败的原因（含服务端日志）。
    /// @return 诊断文本；可用态为空串。
    [[nodiscard]] auto status() const -> std::string { return status_; }

    /// @brief 服务端迄今写下的日志文本（边跑边可读，不必等退出）。
    /// @return 日志内容。
    [[nodiscard]] auto log_text() const -> std::string { return detail::read_file(log_path_); }

    /// @brief 提前收摊：强杀服务端子进程（「对端消失之后的读结局」判据的制造者）。
    auto stop() -> void {
        child_.stop();
        valid_ = false;
    }

    /// @brief 子进程是否还活着。
    /// @return 在跑为 true。
    [[nodiscard]] auto running() const -> bool { return child_.running(); }

  private:
    /// @brief 取一个当前没人占的回环端口：绑定即关闭，把号码交给子进程去 bind。
    /// @return 端口；取不到为 0。
    [[nodiscard]] static auto reserve_loopback_port() -> std::uint16_t {
        auto listener = aurora::net::blocking::Listener::listen_loopback(std::uint16_t{0}, 1);
        if (!listener) {
            return 0;
        }
        const auto port = listener.value().port();
        listener.value().close();
        return port;
    }

    /// @brief 轮询探测端口是否开始接受连接（子进程起得慢是常态，不能把它当成逻辑失败）。
    /// @param port 目标端口。
    /// @param budget 等待上限。
    /// @return 连得上为 true。
    [[nodiscard]] static auto wait_until_listening(std::uint16_t port, std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            auto probe = aurora::net::blocking::StreamSocket::connect_loopback(port);
            if (probe) {
                probe.value().close();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    detail::ChildProcess child_;  ///< 服务端子进程
    std::filesystem::path log_path_;
    std::uint16_t port_{0};
    bool valid_{false};
    std::string status_;
};

}  // namespace aurora::test::tls
