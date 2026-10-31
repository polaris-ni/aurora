// tools/verify/run_at_login_live_probe.cpp — 登录项三态真机验收探针（非 CTest）。
//
// 覆盖：`run_at_login()` / `set_run_at_login()`（include/aurora/app/run_at_login.h）在真实用户级
// 数据库上的整条接线：Windows `HKCU\...\CurrentVersion\Run` + `StartupApproved\Run`、Linux
// `$XDG_CONFIG_HOME/autostart` 桌面条目、macOS `SMAppService`。
//
// 为什么必须有本探针（无头单测证明不了的部分）：
//   * 单测只验三态折算与伪后端；「OS 侧真的把条目登记成可自启」「撤销真的拿干净」只有真机可证；
//   * 「用户手动禁用」这一态必须用**独立通道**造出来（探针直接写 StartupApproved 禁用记录 /
//     autostart 条目的 `Hidden=true`），再核对查询报 `Disabled` 而不是 `Unsupported`——这条正是
//     三态相对布尔的核心价值，伪后端上只能验传参；
//   * 「关闭不抹掉用户记录」也必须独立取证：Windows 关闭只删 Run 值、StartupApproved 记录留存；
//     Linux 关闭只翻转 `Hidden=` 且文件实体不删；
//   * 机器上其它登录项一条都不许动：探针在写入前后对整棵 Run 键 / autostart 目录做快照比对。
//
// 构建（AURORA_BUILD_VERIFY_TOOLS=ON）：
//   cmake -S . -B build-verify -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_run_at_login
//   build-verify\aurora_verify_run_at_login.exe [--interactive]
//
// 自动段（无人值守）：
//   1. 能力位 `run_at_login` 为 true；
//   2. 记录基线：三态查询结果 + 独立数据库快照 + 本应用条目的原始值；
//   3. 开启后查询报 `Enabled`，独立数据库里确有指向本应用（含命令行快照）的条目；
//   4. 模拟「用户在系统设置里手动禁用」后查询报 `Disabled`，且条目实体仍在（Disabled ≠ 不存在）；
//   5. 经 API 关闭后查询报 `Disabled`、启用项消失、用户的禁用记录不被抹掉（Windows）/
//      条目文件不被删除且只翻转开关键（Linux）；
//   6. 再次开启覆盖用户禁用记录并回到 `Enabled`；
//   7. 其它登录项快照逐字节不变；最后把基线逐字节还原（原本没有的条目删回没有）。
// 人工段（`--interactive`）：提示人在系统设置 / 任务管理器里核对登录项后确认。
//
// 平台说明：macOS 的登录项服务要求进程运行在 `.app` 包内且未被沙箱化；裸可执行文件上查询如实报
// `Unsupported`（点名不在包内），本探针按环境不可用收尾，不冒充已验收。
//
// 退出码：
//   0  自动段全部通过（未跑人工段时人工项如实打印为待确认）
//   1  自动段断言失败
//   2  环境不可用（无登录项桥 / macOS 裸可执行或沙箱）
//   7  人工段存在未确认项

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/run_at_login.h"
#include "aurora/app/single_instance.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/window/platform.h"
#include "verify_args.h"
#include "verify_shell.h"

#ifdef AURORA_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
// clang-format off
#include <windows.h>
// clang-format on
#endif

namespace {

/// @brief 探针命令行快照的标记参数（写进登录项命令模板，取证用）。
constexpr const char *AURORA_PROBE_ARGUMENT = "--probe-login";
/// @brief 带空格的参数：核对命令模板逐参数安全引用。
constexpr const char *AURORA_PROBE_SPACED_ARGUMENT = "two words";

#ifdef AURORA_PLATFORM_WINDOWS
constexpr const wchar_t *AURORA_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t *AURORA_STARTUP_APPROVED_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

/// @brief 宽串转 ASCII（注册表命令模板里的路径与参数取 ASCII 比较够用）。
/// @param text 宽串。
/// @return ASCII 串（非 ASCII 字符记 `?`）。
[[nodiscard]] auto to_ascii(const std::wstring &text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

/// @brief 读 HKCU 下一个字符串值（独立取证，不复用桥的私有辅助）。
/// @param key 子键路径（相对 HKCU）。
/// @param value 值名（空串 = 默认值）。
/// @return 读到的文本；不存在 / 类型不符为空。
[[nodiscard]] auto read_hkcu_string(const std::wstring &key, const std::wstring &value) -> std::optional<std::wstring> {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size <= sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, buffer.data(), &size) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    buffer.resize((size / sizeof(wchar_t)) - 1U);
    return buffer;
}

/// @brief 读 HKCU 下一个二进制值（`StartupApproved` 的禁用记录）。
/// @param key 子键路径。
/// @param value 值名。
/// @return 字节串；不存在 / 类型不符为空。
[[nodiscard]] auto read_hkcu_binary(const std::wstring &key, const std::wstring &value)
    -> std::optional<std::vector<std::uint8_t>> {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), RRF_RT_REG_BINARY, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size == 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> buffer(size, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), RRF_RT_REG_BINARY, nullptr, buffer.data(), &size) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    return buffer;
}

/// @brief 写 HKCU 字符串值（探针还原基线用）。
/// @param key 子键路径。
/// @param value 值名。
/// @param data 文本。
/// @return 成功为 true。
[[nodiscard]] auto write_hkcu_string(const std::wstring &key, const std::wstring &value, const std::wstring &data)
    -> bool {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &handle, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const auto bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    // `RegSetValueExW` 的数据形参是 `const BYTE *`；宽字符串本来就是字节串。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto *payload = reinterpret_cast<const BYTE *>(data.data());
    const LSTATUS status = RegSetValueExW(handle, value.c_str(), 0, REG_SZ, payload, bytes);
    RegCloseKey(handle);
    return status == ERROR_SUCCESS;
}

/// @brief 写 HKCU 二进制值（模拟任务管理器留下的禁用记录）。
/// @param key 子键路径。
/// @param value 值名。
/// @param data 字节串。
/// @return 成功为 true。
[[nodiscard]] auto write_hkcu_binary(const std::wstring &key, const std::wstring &value,
                                     const std::vector<std::uint8_t> &data) -> bool {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &handle, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status =
        RegSetValueExW(handle, value.c_str(), 0, REG_BINARY, data.data(), static_cast<DWORD>(data.size()));
    RegCloseKey(handle);
    return status == ERROR_SUCCESS;
}

/// @brief 删除单个值（不存在视为成功）。
/// @param key 子键路径。
/// @param value 值名。
/// @return 成功为 true。
[[nodiscard]] auto delete_hkcu_value(const std::wstring &key, const std::wstring &value) -> bool {
    const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

/// @brief 一个注册表值的全部内容（类型 + 原始字节），供快照逐字节比较。
struct RegistryValue {
    DWORD type = 0;  ///< 值类型。
    std::vector<std::uint8_t> data;  ///< 原始字节。
};

/// @brief 读整棵子键的全部值（按值名排序）。
/// @param key 子键路径。
/// @return 值名 → 内容；子键不存在为空表。
[[nodiscard]] auto snapshot_key(const std::wstring &key) -> std::map<std::wstring, RegistryValue> {
    std::map<std::wstring, RegistryValue> values;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) {
        return values;
    }
    for (DWORD index = 0;; ++index) {
        std::wstring name(16384, L'\0');
        auto name_size = static_cast<DWORD>(name.size());
        std::vector<std::uint8_t> data(static_cast<std::size_t>(64U) * 1024U, 0);
        auto data_size = static_cast<DWORD>(data.size());
        DWORD type = 0;
        LSTATUS status = RegEnumValueW(handle, index, name.data(), &name_size, nullptr, &type, data.data(), &data_size);
        if (status == ERROR_MORE_DATA) {
            data.resize(static_cast<std::size_t>(8U) * 1024U * 1024U);
            data_size = static_cast<DWORD>(data.size());
            name.assign(16384, L'\0');
            name_size = static_cast<DWORD>(name.size());
            status = RegEnumValueW(handle, index, name.data(), &name_size, nullptr, &type, data.data(), &data_size);
        }
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            break;
        }
        name.resize(name_size);
        data.resize(data_size);
        values[name] = RegistryValue{.type = type, .data = data};
    }
    RegCloseKey(handle);
    return values;
}

/// @brief 本应用的登录项值名（与桥同口径：可执行文件名去目录与后缀）。
/// @param exe 可执行路径。
/// @return 值名。
[[nodiscard]] auto login_value_name(const std::string &exe) -> std::wstring {
    const auto stem = std::filesystem::path(exe).stem().string();
    return {stem.begin(), stem.end()};
}

/// @brief 写入一条「用户在系统设置里禁用」的记录（首字节最低位为 0）。
/// @param name 值名。
/// @return 成功为 true。
[[nodiscard]] auto write_user_disabled_record(const std::wstring &name) -> bool {
    return write_hkcu_binary(AURORA_STARTUP_APPROVED_KEY, name,
                             std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
}

/// @brief 基线：本应用两个值的原始内容 + 两个键的全量快照。
struct WindowsBaseline {
    std::optional<std::wstring> run_value;  ///< 本应用的 Run 命令模板。
    std::optional<std::vector<std::uint8_t>> approved;  ///< 本应用的 StartupApproved 记录。
    std::map<std::wstring, RegistryValue> run_all;  ///< Run 键全量快照。
    std::map<std::wstring, RegistryValue> approved_all;  ///< StartupApproved 键全量快照。
};

/// @brief 把基线逐字节写回（原本没有的条目删回没有）。
/// @param baseline 基线。
/// @param name 本应用值名。
/// @return 全部写入成功为 true。
[[nodiscard]] auto restore_windows(const WindowsBaseline &baseline, const std::wstring &name) -> bool {
    const bool run_ok = baseline.run_value.has_value() ? write_hkcu_string(AURORA_RUN_KEY, name, *baseline.run_value)
                                                       : delete_hkcu_value(AURORA_RUN_KEY, name);
    const bool approved_ok = baseline.approved.has_value()
                                 ? write_hkcu_binary(AURORA_STARTUP_APPROVED_KEY, name, *baseline.approved)
                                 : delete_hkcu_value(AURORA_STARTUP_APPROVED_KEY, name);
    return run_ok && approved_ok;
}

/// @brief 判定快照里除本应用外的其它值未变。
/// @param before 基线快照。
/// @param after 当前快照。
/// @param ours 本应用值名（比较前从两侧剔除）。
/// @return 其余条目逐字节一致为 true。
[[nodiscard]] auto others_unchanged(std::map<std::wstring, RegistryValue> before,
                                    std::map<std::wstring, RegistryValue> after, const std::wstring &ours) -> bool {
    before.erase(ours);
    after.erase(ours);
    if (before.size() != after.size()) {
        return false;
    }
    for (const auto &[name, value] : before) {
        const auto found = after.find(name);
        if (found == after.end() || found->second.type != value.type || found->second.data != value.data) {
            return false;
        }
    }
    return true;
}
#endif  // AURORA_PLATFORM_WINDOWS

#ifdef AURORA_PLATFORM_LINUX
/// @brief 用户配置目录（XDG_CONFIG_HOME 优先，回落后 HOME/.config）。
/// @return 目录路径；两者都缺返回空串。
[[nodiscard]] auto config_home() -> std::string {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return xdg;
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::string(home) + "/.config";
    }
    return {};
}

/// @brief 读文件全文。
/// @param path 路径。
/// @return 文本；打不开返回空串。
[[nodiscard]] auto read_text(const std::string &path) -> std::string {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/// @brief 写文件全文。
/// @param path 路径。
/// @param text 文本。
/// @return 成功为 true。
[[nodiscard]] auto write_text(const std::string &path, const std::string &text) -> bool {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

/// @brief 本应用的自动启动条目路径（与桥同口径：aurora-autostart-<stem>.desktop）。
/// @param exe 可执行路径。
/// @return 路径；配置目录不可用为空串。
[[nodiscard]] auto autostart_path(const std::string &exe) -> std::string {
    const std::string home = config_home();
    if (home.empty()) {
        return {};
    }
    const std::string stem = std::filesystem::path(exe).stem().string();
    return home + "/autostart/aurora-autostart-" + (stem.empty() ? std::string("aurora-application") : stem) +
           ".desktop";
}

/// @brief 设置桌面条目某个键的值（键不存在时追加一行）；探针独立实现，不复用桥的折算层。
/// @param text 条目全文。
/// @param key 键名。
/// @param value 键值。
/// @return 更新后的全文。
[[nodiscard]] auto with_key(const std::string &text, std::string_view key, std::string_view value) -> std::string {
    const std::string prefix = std::string(key) + "=";
    bool replaced = false;
    std::string out;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find('\n', begin);
        const bool last = end == std::string::npos;
        std::string line = text.substr(begin, last ? std::string::npos : end - begin);
        if (!replaced && line.rfind(prefix, 0) == 0) {
            line = prefix + std::string(value);
            replaced = true;
        }
        out += line;
        if (last) {
            break;
        }
        out += '\n';
        begin = end + 1U;
    }
    if (!replaced) {
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        out += prefix + std::string(value) + "\n";
    }
    return out;
}

/// @brief 剔除开关键（`Hidden=` / `X-GNOME-Autostart-enabled=`）后的条目正文。
/// @param text 条目全文。
/// @return 剔除后的正文。
[[nodiscard]] auto without_toggle_keys(const std::string &text) -> std::string {
    std::string out;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find('\n', begin);
        const bool last = end == std::string::npos;
        const std::string line = text.substr(begin, last ? std::string::npos : end - begin);
        const bool toggle = line.rfind("Hidden=", 0) == 0 || line.rfind("X-GNOME-Autostart-enabled=", 0) == 0;
        if (!toggle) {
            out += line;
            if (!last) {
                out += '\n';
            }
        }
        if (last) {
            break;
        }
        begin = end + 1U;
    }
    return out;
}

/// @brief 快照 autostart 目录里除本应用条目外的其它文件（名 + 内容）。
/// @param directory 目录。
/// @param ours 本应用条目文件名。
/// @return 文件名 → 内容。
[[nodiscard]] auto snapshot_autostart(const std::string &directory, const std::string &ours)
    -> std::map<std::string, std::string> {
    std::map<std::string, std::string> files;
    std::error_code ignored;
    if (directory.empty()) {
        return files;
    }
    for (const auto &entry : std::filesystem::directory_iterator(directory, ignored)) {
        if (ignored) {
            break;
        }
        if (!entry.is_regular_file(ignored)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name == ours) {
            continue;
        }
        files[name] = read_text(entry.path().string());
    }
    return files;
}
#endif  // AURORA_PLATFORM_LINUX

/// @brief 打印三态查询结果（人工段与诊断用）。
/// @param state 查询结果。
auto note_state(const aurora::RunAtLoginState &state) -> void {
    const char *name = "Unsupported";
    if (state.status == aurora::RunAtLoginStatus::Enabled) {
        name = "Enabled";
    } else if (state.status == aurora::RunAtLoginStatus::Disabled) {
        name = "Disabled";
    }
    aurora_verify::note(std::string("[state] ") + name +
                        (state.detail.empty() ? std::string{} : " (" + state.detail + ")"));
}

/// @brief 构造探针应用身份（带命令行快照，核对命令模板逐参数引用）。
/// @param exe 可执行路径。
/// @return 应用身份。
[[nodiscard]] auto make_identity(const std::string &exe) -> aurora::AppIdentity {
    aurora::AppIdentity identity;
    identity.executable_path = exe;
    identity.arguments = {AURORA_PROBE_ARGUMENT, AURORA_PROBE_SPACED_ARGUMENT};
    return identity;
}

#ifdef AURORA_PLATFORM_WINDOWS
/// @brief Windows 自动段：能力位 / 基线 / 开启 / 用户禁用 / 关闭 / 再开启 / 还原。
/// @param exe 可执行路径。
/// @param identity 应用身份。
/// @return 进程退出码（0 / 1）。
auto run_windows_stage(const std::string &exe, const aurora::AppIdentity &identity) -> int {
    const std::wstring name = login_value_name(exe);
    const std::string ascii_name = to_ascii(name);

    const aurora::RunAtLoginState baseline_state = aurora::run_at_login(identity);
    note_state(baseline_state);
    const WindowsBaseline baseline{.run_value = read_hkcu_string(AURORA_RUN_KEY, name),
                                   .approved = read_hkcu_binary(AURORA_STARTUP_APPROVED_KEY, name),
                                   .run_all = snapshot_key(AURORA_RUN_KEY),
                                   .approved_all = snapshot_key(AURORA_STARTUP_APPROVED_KEY)};
    aurora_verify::note("[baseline] Run value present: " + std::string(baseline.run_value ? "yes" : "no") +
                        ", StartupApproved record present: " + std::string(baseline.approved ? "yes" : "no"));

    const auto enabled = aurora::set_run_at_login(true, identity);
    aurora_verify::check(enabled.ok(), "[enable] set_run_at_login(true) succeeds");
    if (!enabled.ok()) {
        aurora_verify::note("[enable] error: " + enabled.error().code + " / " + enabled.error().message);
        (void)restore_windows(baseline, name);
        return 1;
    }
    aurora_verify::check(aurora::run_at_login(identity).status == aurora::RunAtLoginStatus::Enabled,
                         "[enable] the query reports Enabled");
    const auto command = read_hkcu_string(AURORA_RUN_KEY, name);
    aurora_verify::check(command.has_value(), "[enable] independent evidence: the HKCU Run value exists");
    if (command) {
        const std::string ascii_command = to_ascii(*command);
        aurora_verify::note("[enable] Run command: " + ascii_command);
        aurora_verify::check(aurora_verify::contains_ascii_ci(ascii_command, exe),
                             "[enable] the command points at this executable");
        aurora_verify::check(ascii_command.find(AURORA_PROBE_ARGUMENT) != std::string::npos,
                             "[enable] the command carries the argv snapshot");
        aurora_verify::check(
            ascii_command.find(std::string("\"") + AURORA_PROBE_SPACED_ARGUMENT + "\"") != std::string::npos,
            "[enable] an argument with spaces is quoted as one token");
    }
    aurora_verify::check(!read_hkcu_binary(AURORA_STARTUP_APPROVED_KEY, name).has_value(),
                         "[enable] a stale user-disable record is cleared by an explicit enable");

    // 模拟「用户用任务管理器在启动项里禁用」：Run 值仍在，StartupApproved 留一条首字节为 0 的记录。
    aurora_verify::check(write_user_disabled_record(name), "[user-disable] the probe can plant a user-disable record");
    const auto user_disabled = aurora::run_at_login(identity);
    aurora_verify::check(user_disabled.status == aurora::RunAtLoginStatus::Disabled,
                         "[user-disable] the query reports Disabled, not Unsupported");
    note_state(user_disabled);
    aurora_verify::check(read_hkcu_string(AURORA_RUN_KEY, name).has_value(),
                         "[user-disable] the Run value still exists (Disabled is not 'absent')");

    const auto disabled = aurora::set_run_at_login(false, identity);
    aurora_verify::check(disabled.ok(), "[disable] set_run_at_login(false) succeeds");
    aurora_verify::check(aurora::run_at_login(identity).status == aurora::RunAtLoginStatus::Disabled,
                         "[disable] the query reports Disabled");
    aurora_verify::check(!read_hkcu_string(AURORA_RUN_KEY, name).has_value(),
                         "[disable] independent evidence: the HKCU Run value is gone");
    aurora_verify::check(read_hkcu_binary(AURORA_STARTUP_APPROVED_KEY, name).has_value(),
                         "[disable] the user's StartupApproved record is preserved, not erased");

    aurora_verify::check(others_unchanged(baseline.run_all, snapshot_key(AURORA_RUN_KEY), name),
                         "[isolation] every other Run entry is byte-identical after enable/disable");
    aurora_verify::check(others_unchanged(baseline.approved_all, snapshot_key(AURORA_STARTUP_APPROVED_KEY), name),
                         "[isolation] every other StartupApproved entry is byte-identical");

    const auto reenabled = aurora::set_run_at_login(true, identity);
    aurora_verify::check(reenabled.ok() && aurora::run_at_login(identity).status == aurora::RunAtLoginStatus::Enabled,
                         "[re-enable] enabling again overrides the user-disable record");
    aurora_verify::check(!read_hkcu_binary(AURORA_STARTUP_APPROVED_KEY, name).has_value(),
                         "[re-enable] the user-disable record is gone after an explicit enable");

    aurora_verify::check(restore_windows(baseline, name), "[restore] the baseline values are written back");
    const auto restored_run = read_hkcu_string(AURORA_RUN_KEY, name);
    const auto restored_approved = read_hkcu_binary(AURORA_STARTUP_APPROVED_KEY, name);
    const bool run_restored = baseline.run_value.has_value()
                                  ? restored_run.has_value() && *restored_run == *baseline.run_value
                                  : !restored_run.has_value();
    const bool approved_restored = baseline.approved.has_value()
                                       ? restored_approved.has_value() && *restored_approved == *baseline.approved
                                       : !restored_approved.has_value();
    aurora_verify::check(run_restored && approved_restored, "[restore] the baseline is byte-identical");
    aurora_verify::check(aurora::run_at_login(identity).status == baseline_state.status,
                         "[restore] the query returns to the baseline state");
    aurora_verify::note("[restore] probe value name: " + ascii_name);
    return 0;
}
#endif  // AURORA_PLATFORM_WINDOWS

#ifdef AURORA_PLATFORM_LINUX
/// @brief Linux 自动段：能力位 / 基线 / 开启 / 用户禁用 / 关闭 / 开关键差异 / 再开启 / 还原。
/// @param exe 可执行路径。
/// @param identity 应用身份。
/// @return 进程退出码（0 / 1 / 2）。
auto run_linux_stage(const std::string &exe, const aurora::AppIdentity &identity) -> int {
    const std::string path = autostart_path(exe);
    if (path.empty()) {
        aurora_verify::env_fail("[env] neither XDG_CONFIG_HOME nor HOME is set",
                                "set a user config directory before running the probe");
        return 2;
    }
    const std::string directory = std::filesystem::path(path).parent_path().string();
    const std::string ours = std::filesystem::path(path).filename().string();

    const aurora::RunAtLoginState baseline_state = aurora::run_at_login(identity);
    note_state(baseline_state);
    std::error_code ignored;
    const bool existed = std::filesystem::exists(path, ignored);
    const std::string original = existed ? read_text(path) : std::string{};
    const auto baseline_others = snapshot_autostart(directory, ours);
    aurora_verify::note("[baseline] autostart entry present: " + std::string(existed ? "yes" : "no"));

    const auto enabled = aurora::set_run_at_login(true, identity);
    aurora_verify::check(enabled.ok(), "[enable] set_run_at_login(true) succeeds");
    if (!enabled.ok()) {
        aurora_verify::note("[enable] error: " + enabled.error().code + " / " + enabled.error().message);
        return 1;
    }
    aurora_verify::check(aurora::run_at_login(identity).status == aurora::RunAtLoginStatus::Enabled,
                         "[enable] the query reports Enabled");
    const std::string enabled_text = read_text(path);
    aurora_verify::check(!enabled_text.empty(), "[enable] independent evidence: the desktop entry exists");
    aurora_verify::note("[enable] entry: " + path);
    aurora_verify::check(
        aurora_verify::contains_ascii_ci(enabled_text, "Exec=") && aurora_verify::contains_ascii_ci(enabled_text, exe),
        "[enable] the Exec line points at this executable");
    aurora_verify::check(!aurora_verify::contains_ascii_ci(enabled_text, "Hidden=true"),
                         "[enable] the entry is not marked hidden");

    // 模拟用户手工禁用：条目仍在，只把 Hidden 翻成 true。
    aurora_verify::check(write_text(path, with_key(enabled_text, "Hidden", "true")),
                         "[user-disable] the probe can mark the entry hidden");
    const auto user_disabled = aurora::run_at_login(identity);
    aurora_verify::check(user_disabled.status == aurora::RunAtLoginStatus::Disabled,
                         "[user-disable] the query reports Disabled, not Unsupported");
    note_state(user_disabled);
    aurora_verify::check(std::filesystem::exists(path, ignored),
                         "[user-disable] the entry file still exists (Disabled is not 'absent')");

    const auto disabled = aurora::set_run_at_login(false, identity);
    aurora_verify::check(disabled.ok(), "[disable] set_run_at_login(false) succeeds");
    aurora_verify::check(aurora::run_at_login(identity).status == aurora::RunAtLoginStatus::Disabled,
                         "[disable] the query reports Disabled");
    const std::string disabled_text = read_text(path);
    aurora_verify::check(std::filesystem::exists(path, ignored) && !disabled_text.empty(),
                         "[disable] the user's entry file is preserved, not deleted");
    aurora_verify::check(aurora_verify::contains_ascii_ci(disabled_text, "Hidden=true"),
                         "[disable] the entry is marked hidden");
    aurora_verify::check(without_toggle_keys(enabled_text) == without_toggle_keys(disabled_text),
                         "[disable] enable and disable differ only in the toggle keys");

    aurora_verify::check(snapshot_autostart(directory, ours) == baseline_others,
                         "[isolation] every other autostart entry is byte-identical after enable/disable");

    const auto reenabled = aurora::set_run_at_login(true, identity);
    aurora_verify::check(reenabled.ok() && aurora::run_at_login(identity).status == aurora::RunAtLoginStatus::Enabled,
                         "[re-enable] enabling again flips the toggle back");
    aurora_verify::check(!aurora_verify::contains_ascii_ci(read_text(path), "Hidden=true"),
                         "[re-enable] the entry is no longer hidden");

    // 还原基线：原本存在的条目写回原文，原本不存在的删掉。
    bool restored = true;
    if (existed) {
        restored = write_text(path, original);
    } else {
        std::filesystem::remove(path, ignored);
        restored = !std::filesystem::exists(path, ignored);
    }
    aurora_verify::check(restored, "[restore] the baseline autostart state is written back");
    aurora_verify::check(aurora::run_at_login(identity).status == baseline_state.status,
                         "[restore] the query returns to the baseline state");
    return 0;
}
#endif  // AURORA_PLATFORM_LINUX

/// @brief 自动段：能力位分派到各平台段。
/// @param exe 可执行路径。
/// @param identity 应用身份。
/// @return 进程退出码（0 / 1 / 2）。
auto run_auto_stage(const std::string &exe, const aurora::AppIdentity &identity) -> int {
    aurora_verify::emit("== Aurora run-at-login live probe ==");
    aurora_verify::emit("executable: " + exe);

    const auto caps = aurora::platform().capabilities();
    aurora_verify::check(caps.run_at_login, "[capability] run_at_login is true");
    if (!caps.run_at_login) {
        aurora_verify::env_fail(
            "[env] this build has no run-at-login bridge",
            "configure with the platform shell-integration bridge (Windows / Linux / macOS backend)");
        return 2;
    }

#ifdef AURORA_PLATFORM_WINDOWS
    return run_windows_stage(exe, identity);
#elif defined(AURORA_PLATFORM_LINUX)
    return run_linux_stage(exe, identity);
#elif defined(AURORA_PLATFORM_MACOS)
    (void)exe;
    const aurora::RunAtLoginState state = aurora::run_at_login(identity);
    note_state(state);
    if (state.status == aurora::RunAtLoginStatus::Unsupported) {
        aurora_verify::env_fail("[env] macOS login items need a non-sandboxed .app bundle",
                                "run this probe from a probe .app bundle; detail: " + state.detail);
        return 2;
    }
    const auto enabled = aurora::set_run_at_login(true, identity);
    if (!enabled.ok()) {
        if (aurora_verify::contains_ascii_ci(enabled.error().message, "bundle") ||
            aurora_verify::contains_ascii_ci(enabled.error().message, "sandbox")) {
            aurora_verify::env_fail("[env] macOS login items need a non-sandboxed .app bundle",
                                    "run this probe from a probe .app bundle");
            return 2;
        }
        aurora_verify::check(false, "[enable] set_run_at_login(true) succeeds");
        aurora_verify::note("[enable] error: " + enabled.error().code + " / " + enabled.error().message);
        return 1;
    }
    aurora_verify::check(aurora::run_at_login(identity).status == aurora::RunAtLoginStatus::Enabled,
                         "[enable] the query reports Enabled");
    const auto disabled = aurora::set_run_at_login(false, identity);
    aurora_verify::check(disabled.ok() && aurora::run_at_login(identity).status == aurora::RunAtLoginStatus::Disabled,
                         "[disable] the query reports Disabled");
    return 0;
#else
    aurora_verify::env_fail("[env] this platform has no run-at-login bridge", "no remediation on this platform");
    return 2;
#endif
}

/// @brief 人工段：提示人在系统设置里核对登录项。
/// @param identity 应用身份。
/// @return 0 = 已确认；7 = 未确认 / 无输入。
auto run_interactive_stage(const aurora::AppIdentity &identity) -> int {
    aurora_verify::emit("---- interactive segment ----");
    note_state(aurora::run_at_login(identity));
    aurora_verify::emit("Enable the login item again from an application window, then check the OS login-item");
    aurora_verify::emit("settings (Windows: Task Manager startup tab; Linux: autostart settings).");
    aurora_verify::emit("Type y to confirm, anything else to keep it pending: ");
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return 7;
    }
    for (char &ch : answer) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    if (!answer.empty() && answer.front() == 'y') {
        aurora_verify::emit("[PASS] human confirmed the OS-side login item");
        return 0;
    }
    aurora_verify::emit("[PEND] human confirmation not given; the item stays unverified");
    return 7;
}

}  // namespace

// 探针入口不做异常兜底：任何抛出都该让进程带非零码终止，而不是被吞成「通过了」。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Run-at-login tri-state live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    aurora::init_console();

    const std::string exe = aurora_verify::executable_path(argc > 0 ? *argv : nullptr);
    const auto identity = make_identity(exe);
    const int auto_code = run_auto_stage(exe, identity);
    if (auto_code != 0) {
        return auto_code;
    }
    if (!interactive) {
        aurora_verify::emit("Skipping the interactive stage (re-run with --interactive to confirm visually).");
        aurora_verify::emit(aurora_verify::failures() == 0 ? "PASS (auto stage)" : "FAIL");
        return aurora_verify::failures() == 0 ? 0 : 1;
    }
    const int human_code = run_interactive_stage(identity);
    if (aurora_verify::failures() != 0) {
        return 1;
    }
    if (aurora_verify::env_unavailable()) {
        return 2;
    }
    return human_code;
}
