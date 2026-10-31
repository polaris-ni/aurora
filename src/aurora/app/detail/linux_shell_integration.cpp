// linux_shell_integration.cpp — Linux 桌面外壳集成桥（URL 方案 / 文件关联 / 登录项 /
// 单实例）。
//
// 全部登记落在 freedesktop 的用户级目录：`$XDG_DATA_HOME/applications` 的桌面条目、
// `$XDG_CONFIG_HOME/mimeapps.list` 与 `$XDG_CONFIG_HOME/autostart`。不用任何需要 root 的位置，
// 也不触碰机器级 `$XDG_DATA_DIRS`。单实例的锁文件与套接字落在用户私有运行时目录（`0700`），
// 不用 `/tmp` 下可预测的名字。
//
// 打印会话本平台不接线：不引入 libcups（本库第三方形态全是 vendored 源码，引入系统动态库会
// 先开一个口子），`print_targets()` 因此如实报 `print-session-unsupported`；替代路径是导出 PDF
// 后交给默认处理程序。结论见 `codespec/specification/06-app-platform.md`。
#include "aurora/app/detail/linux_shell_integration.h"

#ifdef AURORA_PLATFORM_LINUX

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/detail/shell_integration_translate.h"
#include "aurora/app/mime_types.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"

namespace aurora::detail {

namespace {

/// @brief 用户主目录（`HOME`）；缺失返回 `std::nullopt`。
[[nodiscard]] auto home_dir() -> std::optional<std::string> {
    const char *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::nullopt;
    }
    return std::string(home);
}

/// @brief 取 XDG 目录：环境变量优先，否则 `$HOME/<fallback>`。
[[nodiscard]] auto xdg_dir(const char *variable, std::string_view fallback) -> std::optional<std::string> {
    if (const char *value = std::getenv(variable); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    const auto home = home_dir();
    if (!home.has_value()) {
        return std::nullopt;
    }
    return *home + "/" + std::string(fallback);
}

[[nodiscard]] auto data_home() -> std::optional<std::string> { return xdg_dir("XDG_DATA_HOME", ".local/share"); }

[[nodiscard]] auto config_home() -> std::optional<std::string> { return xdg_dir("XDG_CONFIG_HOME", ".config"); }

/// @brief 用户私有运行时目录：`$XDG_RUNTIME_DIR` 优先，退回 `$HOME/.cache/aurora/runtime`。
///
/// **不**退回 `/tmp`：同机其他用户可以预占 `/tmp` 下的可预测名字（拒绝服务或抢先监听）。
[[nodiscard]] auto runtime_home() -> std::optional<std::string> {
    if (const char *value = std::getenv("XDG_RUNTIME_DIR"); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    const auto home = home_dir();
    if (!home.has_value()) {
        return std::nullopt;
    }
    return *home + "/.cache/aurora/runtime";
}

/// @brief 建目录；`private_dir` 为真时把最终目录收紧到 `0700`。
[[nodiscard]] auto ensure_directory(const std::string &path, bool private_dir) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && ec != std::errc::file_exists) {
        return false;
    }
    if (private_dir && ::chmod(path.c_str(), S_IRWXU) != 0) {
        return false;
    }
    return true;
}

/// @brief 读整个文本文件；打不开返回 `std::nullopt`。
[[nodiscard]] auto read_file(const std::string &path) -> std::optional<std::string> {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// @brief 覆盖写文本文件（先写临时文件再 `rename`，避免半截条目被桌面环境读到）。
[[nodiscard]] auto write_file(const std::string &path, std::string_view text) -> bool {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output.good()) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

/// @brief 删除文件（不存在视为成功）。
[[nodiscard]] auto remove_file(const std::string &path) -> bool {
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    return removed || !ec;
}

[[nodiscard]] auto sanitize_token(std::string_view token) -> std::string {
    std::string out;
    out.reserve(token.size());
    for (const char ch : token) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                          ch == '.' || ch == '_' || ch == '-';
        out.push_back(safe ? ch : '_');
    }
    return out.empty() ? std::string("aurora-application") : out;
}

[[nodiscard]] auto executable_stem(const AppIdentity &identity) -> std::string {
    const auto stem = std::filesystem::path(identity.executable_path).stem().string();
    return stem.empty() ? std::string("aurora-application") : stem;
}

[[nodiscard]] auto display_label(std::string_view display_name, const AppIdentity &identity) -> std::string {
    return display_name.empty() ? executable_stem(identity) : std::string(display_name);
}

/// @brief `Exec=` 命令模板：引用可执行路径，再追加字段码（`%u` / `%f`；空 = 不追加）。
[[nodiscard]] auto exec_command(const AppIdentity &identity, std::string_view field_code) -> std::string {
    std::string command = desktop_shell::quote_exec_argument(identity.executable_path);
    if (!field_code.empty()) {
        command.push_back(' ');
        command.append(field_code);
    }
    return command;
}

[[nodiscard]] auto applications_dir() -> std::optional<std::string> {
    const auto data = data_home();
    if (!data.has_value()) {
        return std::nullopt;
    }
    return *data + "/applications";
}

[[nodiscard]] auto autostart_dir() -> std::optional<std::string> {
    const auto config = config_home();
    if (!config.has_value()) {
        return std::nullopt;
    }
    return *config + "/autostart";
}

[[nodiscard]] auto mimeapps_path() -> std::optional<std::string> {
    const auto config = config_home();
    if (!config.has_value()) {
        return std::nullopt;
    }
    return *config + "/mimeapps.list";
}

// ---------- URL 方案 ----------

[[nodiscard]] auto url_scheme_desktop_id(const std::string &scheme) -> std::string {
    return "aurora-url-" + sanitize_token(scheme) + ".desktop";
}

[[nodiscard]] auto url_scheme_key(const std::string &scheme) -> std::string { return "x-scheme-handler/" + scheme; }

auto backend_register_scheme(std::string_view scheme, std::string_view display_name, const AppIdentity &identity)
    -> Result<void> {
    const std::string name(scheme);
    const auto applications = applications_dir();
    if (!applications.has_value()) {
        return make_error(ErrorCode::UrlSchemeFailed,
                          "url-scheme: neither XDG_DATA_HOME nor HOME is set, no user data directory");
    }
    if (!ensure_directory(*applications, false)) {
        return make_error(ErrorCode::UrlSchemeFailed,
                          "url-scheme: cannot create the user applications directory: " + *applications);
    }
    const std::string desktop_id = url_scheme_desktop_id(name);
    desktop_shell::DesktopEntry entry;
    entry.name = display_label(display_name, identity);
    entry.comment = entry.name + " URL handler";
    entry.exec = exec_command(identity, "%u");
    entry.url_scheme = name;
    if (!write_file(*applications + "/" + desktop_id, desktop_shell::build_desktop_entry(entry))) {
        return make_error(ErrorCode::UrlSchemeFailed, "url-scheme: cannot write the desktop entry " + desktop_id);
    }
    const auto config = config_home();
    const auto mimeapps = mimeapps_path();
    if (!config.has_value() || !mimeapps.has_value()) {
        return make_error(ErrorCode::UrlSchemeFailed, "url-scheme: neither XDG_CONFIG_HOME nor HOME is set");
    }
    if (!ensure_directory(*config, false)) {
        return make_error(ErrorCode::UrlSchemeFailed,
                          "url-scheme: cannot create the user config directory: " + *config);
    }
    const std::string original = read_file(*mimeapps).value_or("");
    const std::string updated =
        desktop_shell::ini_set(original, "Default Applications", url_scheme_key(name), desktop_id);
    if (!write_file(*mimeapps, updated)) {
        return make_error(ErrorCode::UrlSchemeFailed, "url-scheme: cannot update mimeapps.list for '" + name + "'");
    }
    return Result<void>{};
}

auto backend_unregister_scheme(std::string_view scheme, const AppIdentity &identity) -> Result<void> {
    const std::string name(scheme);
    const auto mimeapps = mimeapps_path();
    if (!mimeapps.has_value()) {
        return make_error(ErrorCode::UrlSchemeFailed, "url-scheme: neither XDG_CONFIG_HOME nor HOME is set");
    }
    const std::string original = read_file(*mimeapps).value_or("");
    const auto owner = desktop_shell::ini_value(original, "Default Applications", url_scheme_key(name));
    if (!owner.has_value() || owner->empty()) {
        return Result<void>{};  // 归属为空：幂等成功
    }
    const std::string desktop_id = url_scheme_desktop_id(name);
    if (*owner != desktop_id) {
        return make_error(ErrorCode::UrlSchemeFailed,
                          "url-scheme: '" + name + "' is registered by another application: " + *owner);
    }
    const auto applications = applications_dir();
    if (applications.has_value()) {
        if (const auto text = read_file(*applications + "/" + desktop_id); text.has_value()) {
            const auto exec = desktop_shell::desktop_entry_value(*text, "Exec");
            if (exec.has_value() && !exec->empty() &&
                exec->find(desktop_shell::quote_exec_argument(identity.executable_path)) == std::string::npos) {
                return make_error(ErrorCode::UrlSchemeFailed,
                                  "url-scheme: the desktop entry " + desktop_id +
                                      " no longer points at this executable; refusing to remove it");
            }
        }
    }
    const std::string updated = desktop_shell::ini_set(original, "Default Applications", url_scheme_key(name), "");
    if (!write_file(*mimeapps, updated)) {
        return make_error(ErrorCode::UrlSchemeFailed,
                          "url-scheme: cannot remove the mimeapps.list entry for '" + name + "'");
    }
    if (applications.has_value()) {
        (void)remove_file(*applications + "/" + desktop_id);
    }
    return Result<void>{};
}

auto backend_scheme_owner(std::string_view scheme) -> Result<std::string> {
    const auto mimeapps = mimeapps_path();
    if (!mimeapps.has_value()) {
        return make_error(ErrorCode::UrlSchemeFailed, "url-scheme: neither XDG_CONFIG_HOME nor HOME is set");
    }
    const std::string text = read_file(*mimeapps).value_or("");
    const auto owner = desktop_shell::ini_value(text, "Default Applications", url_scheme_key(std::string(scheme)));
    if (!owner.has_value()) {
        return std::string{};  // 没有归属：空集合不是错误
    }
    return *owner;
}

// ---------- 文件关联 ----------

[[nodiscard]] auto file_desktop_id(const AppIdentity &identity) -> std::string {
    return "aurora-file-" + sanitize_token(executable_stem(identity)) + ".desktop";
}

auto backend_query_association(std::string_view extension) -> Result<AssociationInfo> {
    const auto mime = desktop_shell::extension_to_mime(extension);
    if (!mime.has_value()) {
        return AssociationInfo{};  // 未登记扩展名：无关联，不是错误
    }
    const auto mimeapps = mimeapps_path();
    if (!mimeapps.has_value()) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: neither XDG_CONFIG_HOME nor HOME is set");
    }
    const std::string text = read_file(*mimeapps).value_or("");
    const auto owner = desktop_shell::ini_value(text, "Default Applications", *mime);
    if (!owner.has_value() || owner->empty()) {
        return AssociationInfo{};  // 没有关联：空集合不是错误
    }
    AssociationInfo info;
    info.application = *owner;
    info.source = "mimeapps.list";
    return info;
}

auto backend_declare_association(const AssociationDeclaration &declaration, std::string_view mime,
                                 const AppIdentity &identity) -> Result<void> {
    const auto applications = applications_dir();
    if (!applications.has_value()) {
        return make_error(ErrorCode::FileAssociationFailed, "file-association: neither XDG_DATA_HOME nor HOME is set");
    }
    if (!ensure_directory(*applications, false)) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: cannot create the user applications directory: " + *applications);
    }
    const std::string desktop_id = file_desktop_id(identity);
    const std::string path = *applications + "/" + desktop_id;
    const std::string label = display_label(declaration.display_name, identity);

    std::string text = read_file(path).value_or("");
    if (text.empty()) {
        desktop_shell::DesktopEntry entry;
        entry.name = label;
        entry.comment = label + " document handler";
        entry.exec = exec_command(identity, "%f");
        entry.mime_types = desktop_shell::list_add("", mime);
        text = desktop_shell::build_desktop_entry(entry);
    } else {
        const std::string merged =
            desktop_shell::list_add(desktop_shell::desktop_entry_value(text, "MimeType").value_or(""), mime);
        text = desktop_shell::desktop_entry_set_value(text, "Name", label);
        text = desktop_shell::desktop_entry_set_value(text, "Exec", exec_command(identity, "%f"));
        text = desktop_shell::desktop_entry_set_value(text, "MimeType", merged);
        text = desktop_shell::desktop_entry_set_value(text, "Hidden", "false");
    }
    if (!write_file(path, text)) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: cannot write the desktop entry " + desktop_id);
    }

    const auto config = config_home();
    const auto mimeapps = mimeapps_path();
    if (!config.has_value() || !mimeapps.has_value()) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: neither XDG_CONFIG_HOME nor HOME is set");
    }
    if (!ensure_directory(*config, false)) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: cannot create the user config directory: " + *config);
    }
    // 只写 `[Added Associations]`（候选），不写 `[Default Applications]`（默认处理程序归用户）。
    const std::string original = read_file(*mimeapps).value_or("");
    const std::string added = desktop_shell::list_add(
        desktop_shell::ini_value(original, "Added Associations", std::string(mime)).value_or(""), desktop_id);
    const std::string updated = desktop_shell::ini_set(original, "Added Associations", std::string(mime), added);
    if (!write_file(*mimeapps, updated)) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: cannot update mimeapps.list for '" + std::string(mime) + "'");
    }
    return Result<void>{};
}

auto backend_undeclare_association(std::string_view extension, const AppIdentity &identity) -> Result<void> {
    const auto mime = desktop_shell::extension_to_mime(extension);
    if (!mime.has_value()) {
        return Result<void>{};  // 本库不可能声明过未登记扩展名：幂等成功
    }
    const std::string desktop_id = file_desktop_id(identity);
    const auto mimeapps = mimeapps_path();
    if (!mimeapps.has_value()) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: neither XDG_CONFIG_HOME nor HOME is set");
    }
    const std::string original = read_file(*mimeapps).value_or("");
    const auto added = desktop_shell::ini_value(original, "Added Associations", std::string(*mime));
    if (!added.has_value() || !desktop_shell::list_contains(*added, desktop_id)) {
        return Result<void>{};  // 本应用没有声明过：幂等成功，一个字节都不改
    }
    const std::string remaining = desktop_shell::list_remove(*added, desktop_id);
    const std::string updated = desktop_shell::ini_set(original, "Added Associations", std::string(*mime), remaining);
    if (!write_file(*mimeapps, updated)) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: cannot update mimeapps.list for '" + std::string(*mime) + "'");
    }
    if (const auto applications = applications_dir(); applications.has_value()) {
        const std::string path = *applications + "/" + desktop_id;
        if (const auto text = read_file(path); text.has_value()) {
            const std::string reduced =
                desktop_shell::list_remove(desktop_shell::desktop_entry_value(*text, "MimeType").value_or(""), *mime);
            if (reduced.empty()) {
                (void)remove_file(path);  // 本条目只为关联而生：没有 MIME 了就连条目一起撤
            } else {
                (void)write_file(path, desktop_shell::desktop_entry_set_value(*text, "MimeType", reduced));
            }
        }
    }
    return Result<void>{};
}

// ---------- 登录项 ----------

[[nodiscard]] auto autostart_entry_path(const AppIdentity &identity) -> std::optional<std::string> {
    const auto autostart = autostart_dir();
    if (!autostart.has_value()) {
        return std::nullopt;
    }
    return *autostart + "/aurora-autostart-" + sanitize_token(executable_stem(identity)) + ".desktop";
}

[[nodiscard]] auto backend_run_at_login_query(const AppIdentity &identity) -> RunAtLoginState {
    const auto path = autostart_entry_path(identity);
    if (!path.has_value()) {
        return RunAtLoginState{.status = RunAtLoginStatus::Unsupported,
                               .detail = "run-at-login: neither XDG_CONFIG_HOME nor HOME is set"};
    }
    const auto text = read_file(*path);
    if (!text.has_value()) {
        // 文件不存在有两种成因：从未设置过，或用户手工删掉了。两者动作同解（开启会重建），
        // 但这里只答「当前没有启用的条目」——即 Disabled。
        std::error_code ec;
        if (std::filesystem::exists(*path, ec)) {
            return RunAtLoginState{.status = RunAtLoginStatus::Unsupported,
                                   .detail = "run-at-login: the autostart entry exists but cannot be read"};
        }
        return RunAtLoginState{.status = RunAtLoginStatus::Disabled};
    }
    return RunAtLoginState{.status = desktop_shell::desktop_entry_disabled(*text) ? RunAtLoginStatus::Disabled
                                                                                  : RunAtLoginStatus::Enabled};
}

auto backend_run_at_login_set(bool enabled, const AppIdentity &identity) -> Result<void> {
    const auto path = autostart_entry_path(identity);
    if (!path.has_value()) {
        return make_error(ErrorCode::RunAtLoginFailed, "run-at-login: neither XDG_CONFIG_HOME nor HOME is set");
    }
    const auto autostart = autostart_dir();
    if (!autostart.has_value() || !ensure_directory(*autostart, false)) {
        return make_error(ErrorCode::RunAtLoginFailed, "run-at-login: cannot create the autostart directory");
    }
    if (const auto existing = read_file(*path); existing.has_value()) {
        // 启停表达在同一文件的 `Hidden=` 上：用户手工禁用的条目保留实体，不删文件。
        const std::string updated = desktop_shell::set_desktop_entry_hidden(*existing, !enabled);
        if (!write_file(*path, updated)) {
            return make_error(ErrorCode::RunAtLoginFailed, "run-at-login: cannot rewrite the autostart entry");
        }
        return Result<void>{};
    }
    if (!enabled) {
        return Result<void>{};  // 条目本就不存在：关闭是幂等成功
    }
    desktop_shell::DesktopEntry entry;
    entry.name = executable_stem(identity);
    entry.comment = entry.name + " autostart entry";
    entry.exec = exec_command(identity, "");
    entry.hidden = false;
    entry.autostart = true;
    if (!write_file(*path, desktop_shell::build_desktop_entry(entry))) {
        return make_error(ErrorCode::RunAtLoginFailed, "run-at-login: cannot write the autostart entry");
    }
    return Result<void>{};
}

// ---------- 单实例 ----------

constexpr std::size_t AURORA_INSTANCE_MAX_URI = 16384U;
constexpr int AURORA_INSTANCE_ACK_TIMEOUT_SECONDS = 3;
constexpr int AURORA_INSTANCE_CONNECT_RETRIES = 20;
constexpr long AURORA_INSTANCE_RETRY_SLEEP_NS = 100L * 1000L * 1000L;

/// @brief 一条已接入的连接及其未消费的帧缓冲。
struct InstanceConnection {
    int fd = -1;  ///< 连接描述符
    std::string buffer;  ///< 尚未凑成完整帧的字节
};

/// @brief 进程级单实例状态（单线程 UI：无锁）。
struct InstanceState {
    int lock_fd = -1;  ///< 首实例持有的锁文件描述符
    int listen_fd = -1;  ///< 首实例的监听套接字
    std::string directory;  ///< 私有运行时子目录
    std::string socket_path;  ///< 套接字路径
    std::function<void(std::string)> sink;  ///< 收到的 URI 入队回调（公共层提供）
    std::vector<InstanceConnection> connections;  ///< 已接入但未关闭的对端
};

[[nodiscard]] auto instance_state() -> InstanceState & {
    static InstanceState state;
    return state;
}

[[nodiscard]] auto write_all(int fd, const void *data, std::size_t size) -> bool {
    const auto *bytes = static_cast<const char *>(data);
    while (size > 0) {
        const ssize_t written = ::send(fd, bytes, size, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

/// @brief 帧编码：4 字节小端长度 + UTF-8 URI。
[[nodiscard]] auto encode_frame(std::string_view uri) -> std::string {
    const auto size = static_cast<std::uint32_t>(uri.size());
    std::string frame(4, '\0');
    frame[0] = static_cast<char>(size & 0xFFU);
    frame[1] = static_cast<char>((size >> 8U) & 0xFFU);
    frame[2] = static_cast<char>((size >> 16U) & 0xFFU);
    frame[3] = static_cast<char>((size >> 24U) & 0xFFU);
    frame.append(uri);
    return frame;
}

/// @brief 帧长度解码（小端）。
[[nodiscard]] auto decode_frame_length(const char *bytes) -> std::uint32_t {
    const auto byte = [bytes](std::size_t index) -> std::uint32_t {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
    };
    return byte(0) | (byte(1) << 8U) | (byte(2) << 16U) | (byte(3) << 24U);
}

/// @brief 对端是否与本进程同一用户（纵深防御：目录 0700 已挡住别的用户）。
[[nodiscard]] auto peer_is_same_user(int fd) -> bool {
#if defined(SO_PEERCRED)
    struct ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return false;
    }
    return credentials.uid == ::getuid();
#else
    (void)fd;
    return true;
#endif
}

[[nodiscard]] auto set_nonblocking(int fd) -> bool {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

/// @brief 把待转交的 URI 写入首实例的套接字并逐条等确认。
[[nodiscard]] auto forward_uris(const std::string &socket_path, const std::vector<std::string> &uris) -> bool {
    int fd = -1;
    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        return false;  // 套接字路径超出平台上限：无法转交
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size());
    for (int attempt = 0; attempt < AURORA_INSTANCE_CONNECT_RETRIES; ++attempt) {
        fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return false;
        }
        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
        const struct timespec pause{0, AURORA_INSTANCE_RETRY_SLEEP_NS};
        (void)::nanosleep(&pause, nullptr);
    }
    if (fd < 0) {
        return false;  // 首实例的套接字还没就绪 / 已退出：按新实例正常启动
    }
    const struct timeval timeout{AURORA_INSTANCE_ACK_TIMEOUT_SECONDS, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    bool forwarded = true;
    for (const auto &uri : uris) {
        if (uri.size() > AURORA_INSTANCE_MAX_URI) {
            forwarded = false;
            break;
        }
        const std::string frame = encode_frame(uri);
        if (!write_all(fd, frame.data(), frame.size())) {
            forwarded = false;
            break;
        }
        char ack = '\0';
        const ssize_t got = ::recv(fd, &ack, 1, 0);
        if (got != 1 || ack != 'A') {
            forwarded = false;
            break;
        }
    }
    ::close(fd);
    return forwarded;
}

auto backend_acquire_instance(const AppIdentity &identity, const std::vector<std::string> &uris,
                              const std::function<void(std::string)> &sink) -> Result<SingleInstanceOutcome> {
    const auto runtime = runtime_home();
    if (!runtime.has_value()) {
        return make_error(ErrorCode::PlatformUnavailable, "single-instance: neither XDG_RUNTIME_DIR nor HOME is set");
    }
    const std::string directory = *runtime + "/aurora-" + aurora::resolve_instance_key(identity);
    if (!ensure_directory(directory, true)) {
        return make_error(ErrorCode::PlatformUnavailable,
                          "single-instance: cannot create the private runtime directory " + directory);
    }
    const std::string lock_path = directory + "/lock";
    const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (lock_fd < 0) {
        return make_error(ErrorCode::PlatformUnavailable, "single-instance: cannot open the lock file");
    }
    if (::flock(lock_fd, LOCK_EX | LOCK_NB) == 0) {
        const std::string socket_path = directory + "/single.sock";
        ::unlink(socket_path.c_str());
        struct sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (socket_path.size() >= sizeof(address.sun_path)) {
            ::close(lock_fd);
            return make_error(ErrorCode::PlatformUnavailable,
                              "single-instance: the runtime socket path is too long for AF_UNIX");
        }
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size());
        const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd < 0 || ::bind(listen_fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listen_fd, 8) != 0 || !set_nonblocking(listen_fd)) {
            if (listen_fd >= 0) {
                ::close(listen_fd);
            }
            ::close(lock_fd);
            return make_error(ErrorCode::PlatformUnavailable, "single-instance: cannot listen on the URI socket");
        }
        (void)::chmod(socket_path.c_str(), S_IRUSR | S_IWUSR);
        auto &state = instance_state();
        state.lock_fd = lock_fd;
        state.listen_fd = listen_fd;
        state.directory = directory;
        state.socket_path = socket_path;
        state.sink = sink;
        return SingleInstanceOutcome::Primary;
    }
    const int flock_error = errno;
    ::close(lock_fd);
    if (flock_error != EWOULDBLOCK && flock_error != EAGAIN) {
        return make_error(ErrorCode::PlatformUnavailable, "single-instance: flock failed");
    }
    // 已有实例持有锁：本进程是第二实例。
    if (uris.empty()) {
        return SingleInstanceOutcome::Forwarded;
    }
    return forward_uris(directory + "/single.sock", uris) ? SingleInstanceOutcome::Forwarded
                                                          : SingleInstanceOutcome::FallbackNewInstance;
}

auto backend_poll_instance() -> void {
    auto &state = instance_state();
    if (state.listen_fd < 0) {
        return;
    }
    for (;;) {
        const int fd = ::accept(state.listen_fd, nullptr, nullptr);
        if (fd < 0) {
            break;  // EAGAIN：没有更多待接入连接
        }
        if (!peer_is_same_user(fd) || !set_nonblocking(fd)) {
            ::close(fd);
            continue;
        }
        state.connections.push_back(InstanceConnection{.fd = fd});
    }
    for (std::size_t index = 0; index < state.connections.size();) {
        auto &connection = state.connections[index];
        bool closed = false;
        char bytes[4096];
        for (;;) {
            const ssize_t got = ::recv(connection.fd, bytes, sizeof(bytes), 0);
            if (got > 0) {
                connection.buffer.append(bytes, static_cast<std::size_t>(got));
                continue;
            }
            if (got == 0) {
                closed = true;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                closed = true;
            }
            break;
        }
        while (connection.buffer.size() >= 4) {
            const auto length = decode_frame_length(connection.buffer.data());
            if (length > AURORA_INSTANCE_MAX_URI) {
                closed = true;  // 超长帧：按恶意输入丢弃整条连接
                break;
            }
            if (connection.buffer.size() < 4U + length) {
                break;
            }
            std::string uri = connection.buffer.substr(4U, length);
            connection.buffer.erase(0, 4U + length);
            if (state.sink) {
                state.sink(std::move(uri));
            }
            const char ack = 'A';
            (void)write_all(connection.fd, &ack, 1);
        }
        if (closed) {
            ::close(connection.fd);
            state.connections.erase(state.connections.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
}

auto backend_release_instance() -> void {
    auto &state = instance_state();
    for (auto &connection : state.connections) {
        ::close(connection.fd);
    }
    state.connections.clear();
    if (state.listen_fd >= 0) {
        ::close(state.listen_fd);
        state.listen_fd = -1;
    }
    if (!state.socket_path.empty()) {
        ::unlink(state.socket_path.c_str());
        state.socket_path.clear();
    }
    if (state.lock_fd >= 0) {
        (void)::flock(state.lock_fd, LOCK_UN);
        ::close(state.lock_fd);
        state.lock_fd = -1;
    }
    if (!state.directory.empty()) {
        (void)::rmdir(state.directory.c_str());
        state.directory.clear();
    }
    state.sink = nullptr;
}

}  // namespace

auto linux_shell_integration_ensure() -> bool {
    static bool registered = false;
    if (registered) {
        return true;
    }
    registered = true;
    desktop_shell::register_url_scheme_backend(desktop_shell::UrlSchemeBackend{
        .register_scheme = &backend_register_scheme,
        .unregister_scheme = &backend_unregister_scheme,
        .owner = &backend_scheme_owner,
    });
    desktop_shell::register_file_association_backend(desktop_shell::FileAssociationBackend{
        .query = &backend_query_association,
        .declare = &backend_declare_association,
        .undeclare = &backend_undeclare_association,
    });
    desktop_shell::register_run_at_login_backend(desktop_shell::RunAtLoginBackend{
        .query = &backend_run_at_login_query,
        .set = &backend_run_at_login_set,
    });
    desktop_shell::register_single_instance_backend(desktop_shell::SingleInstanceBackend{
        .acquire = &backend_acquire_instance,
        .poll = &backend_poll_instance,
        .release = &backend_release_instance,
    });
    // 打印槽位刻意不登记：本平台不声称原生打印会话（不引入 libcups）。
    AURORA_LOG_DEBUG("shell", "linux desktop shell integration bridges registered");
    return true;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX
