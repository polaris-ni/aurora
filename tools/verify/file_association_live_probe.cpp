// tools/verify/file_association_live_probe.cpp — 文件关联声明真机验收探针（非 CTest）。
//
// 覆盖：`register_file_association()` / `unregister_file_association()` / `associated_application()`
// （include/aurora/app/file_association.h）在真实用户级数据库上的接线，以及最重要的那条边界：
// **只声明候选、不抢默认处理程序**。
//
// 为什么必须有本探针（无头单测证明不了的部分）：
//   * 单测在伪后端上验归一化与传参；「声明真的进了 OS 的候选数据库」「撤销真的拿干净」只有真机可证；
//   * 「不抢默认」必须用**独立取证**：探针直接读 Windows 的 `.ext` 默认值与 `FileExts\...\UserChoice`
//     （Linux 读 `mimeapps.list` 的 `[Default Applications]`），核对声明前后逐字节不变——库自己的
//     查询 API 返回的是「实际生效的处理程序」，不能用它自证「我没抢默认」。
//
// 扩展名选择：`.log`（Windows / Linux 都有既定 MIME 口径）。它在两平台都不会触发「临时扩展名
// 无法反查 MIME」的边界；探针的全部写入都在本次运行内撤销。
//
// 构建（AURORA_BUILD_VERIFY_TOOLS=ON）：
//   cmake -S . -B build-verify -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_file_association
//   build-verify\aurora_verify_file_association.exe [--interactive]
//
// 自动段（无人值守）：
//   1. 能力位 `file_association` 为 true；
//   2. 非法扩展名（空串 / 孤立点 / 内嵌点）被拒，且不触碰 OS；
//   3. 记录基线：`associated_application("log")` 与默认处理程序证据；
//   4. 声明 `.log`（本次运行独占显示名），并用**独立通道**确认：
//      Windows → `RegisteredApplications` + Capabilities `FileAssociations` 指向本应用的 ProgID；
//      Linux → 桌面条目存在且 `[Added Associations]` 含本应用 id；
//   5. 默认处理程序证据在声明前后逐字节一致（不抢默认）；
//   6. 撤销后用同一独立通道确认声明已消失，且默认证据仍不变、`associated_application` 回到基线；
//   7. 重复撤销幂等成功。
// 人工段（`--interactive`）：提示人在文件管理器里查看「打开方式」候选后确认。
//
// 平台说明：macOS 的文档类型必须在 bundle 的 `CFBundleDocumentTypes` 里静态声明，运行期声明会
// 如实失败（`file-association-failed` 点名缺声明）。探针不在包内运行时按环境不可用收尾。
//
// 退出码：
//   0  自动段全部通过（未跑人工段时人工项如实打印为待确认）
//   1  自动段断言失败
//   2  环境不可用（无文件关联桥 / macOS 裸可执行）
//   7  人工段存在未确认项

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/file_association.h"
#include "aurora/app/mime_types.h"
#include "aurora/app/single_instance.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
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

/// @brief 探针使用的真实扩展名（两平台都有既定 MIME 口径）。
constexpr const char *AURORA_PROBE_EXTENSION = "log";

#ifdef AURORA_PLATFORM_LINUX
/// @brief 读文件全文（Linux 侧取证用）。
/// @param path 路径。
/// @return 文本；打不开返回空串。
[[nodiscard]] auto read_text(const std::string &path) -> std::string {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/// @brief 取文件里某个 INI 节的内容（`[section]` 到下一个 `[` 之间，原样保留行）。
/// @param text 全文。
/// @param section 节名（不含方括号）。
/// @return 节内容；节不存在返回空串。
[[nodiscard]] auto section_text(const std::string &text, std::string_view section) -> std::string {
    const std::string header = "[" + std::string(section) + "]";
    const auto begin = text.find(header);
    if (begin == std::string::npos) {
        return {};
    }
    const auto content_begin = text.find('\n', begin);
    if (content_begin == std::string::npos) {
        return {};
    }
    const auto next = text.find("\n[", content_begin);
    return text.substr(content_begin + 1U, next == std::string::npos ? std::string::npos : next - content_begin);
}
#endif  // AURORA_PLATFORM_LINUX

#ifdef AURORA_PLATFORM_WINDOWS
/// @brief 探针扩展名的宽字符形式（不含结尾 NUL；拼注册表值名用）。
/// @return 扩展名宽串。
[[nodiscard]] auto wide_extension() -> std::wstring {
    const std::string ext(AURORA_PROBE_EXTENSION);
    return {ext.begin(), ext.end()};
}

/// @brief 读 HKCU 下一个字符串值（独立取证，不复用桥的私有辅助）。
/// @param key 子键路径（相对 HKCU）。
/// @param value 值名（空串 = 默认值）。
/// @param out 读到的 UTF-8 文本。
/// @return 存在且读出为 true；不存在 / 类型不符为 false。
[[nodiscard]] auto read_hkcu_string(const std::wstring &key, const std::wstring &value, std::string &out) -> bool {
    DWORD size = 0;
    const LSTATUS first =
        RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size);
    if (first != ERROR_SUCCESS || size <= sizeof(wchar_t)) {
        return false;
    }
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, buffer.data(), &size) !=
        ERROR_SUCCESS) {
        return false;
    }
    buffer.resize((size / sizeof(wchar_t)) - 1U);
    out.clear();
    for (const wchar_t ch : buffer) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return true;
}

/// @brief 默认处理程序证据：`.ext` 的默认值 + `FileExts\...\UserChoice` 的 ProgId。
/// @param extension 无前导点的扩展名。
/// @return 证据串（不存在时为空段，仍拼接以便逐字比较）。
[[nodiscard]] auto default_handler_evidence(const std::string &extension) -> std::string {
    const std::wstring dotted(1, L'.');
    const std::wstring dotted_ext = dotted + std::wstring(extension.begin(), extension.end());
    std::string class_default;
    std::string user_choice;
    (void)read_hkcu_string(L"Software\\Classes\\" + dotted_ext, L"", class_default);
    (void)read_hkcu_string(
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + dotted_ext + L"\\UserChoice", L"ProgId",
        user_choice);
    return "class-default={" + class_default + "};user-choice={" + user_choice + "}";
}

/// @brief 子键是否存在（探针收尾判定「这条登记是不是本次新造的」）。
/// @param key 子键路径（相对 HKCU）。
/// @return 存在为 true。
[[nodiscard]] auto hkcu_key_exists(const std::wstring &key) -> bool {
    HKEY handle = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle);
    if (status == ERROR_SUCCESS) {
        RegCloseKey(handle);
    }
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

/// @brief 删除整棵子键（不存在视为成功）。
/// @param key 子键路径。
/// @return 成功为 true。
[[nodiscard]] auto delete_hkcu_tree(const std::wstring &key) -> bool {
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

/// @brief 独立核对声明确实落到了候选数据库。
/// @param exe 可执行路径（命令模板里应出现）。
/// @param diagnose true 时在缺失处打印一条诊断（只在「应当存在」的那次调用里开）。
/// @return 结构齐全且命令指向本应用为 true。
[[nodiscard]] auto declaration_present(const std::string &exe, bool diagnose) -> bool {
    const std::string stem = std::filesystem::path(exe).stem().string();
    const std::wstring app_id(stem.begin(), stem.end());
    std::string capabilities;
    if (!read_hkcu_string(L"Software\\RegisteredApplications", app_id, capabilities) || capabilities.empty()) {
        if (diagnose) {
            aurora_verify::note("[evidence] no RegisteredApplications value for '" + stem + "'");
        }
        return false;
    }
    std::wstring capabilities_key(capabilities.begin(), capabilities.end());
    capabilities_key += L"\\FileAssociations";  // 能力键下按扩展名登记 ProgID 的子键
    const std::wstring prog_id_key =
        L"Software\\Classes\\Aurora." + std::wstring(stem.begin(), stem.end()) + L"." + std::wstring(wide_extension());
    std::string declared;
    if (!read_hkcu_string(capabilities_key, L"." + wide_extension(), declared)) {
        if (diagnose) {
            aurora_verify::note("[evidence] no capability value for '." + std::string(AURORA_PROBE_EXTENSION) +
                                "' under " + capabilities);
        }
        return false;
    }
    std::string command;
    if (!read_hkcu_string(prog_id_key + L"\\shell\\open\\command", L"", command)) {
        if (diagnose) {
            aurora_verify::note("[evidence] no command under the registered ProgID '" + declared + "'");
        }
        return false;
    }
    if (diagnose) {
        aurora_verify::note("[evidence] declared '" + declared + "', command: " + command);
    }
    return !declared.empty() && aurora_verify::contains_ascii_ci(command, exe);
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

/// @brief 用户数据目录（XDG_DATA_HOME 优先，回落后 HOME/.local/share）。
/// @return 目录路径；两者都缺返回空串。
[[nodiscard]] auto data_home() -> std::string {
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return xdg;
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::string(home) + "/.local/share";
    }
    return {};
}

/// @brief 桌面条目 id（与桥同口径：aurora-file-<exe stem>.desktop）。
/// @param exe 可执行路径。
/// @return 桌面条目 id。
[[nodiscard]] auto desktop_id(const std::string &exe) -> std::string {
    return "aurora-file-" + std::filesystem::path(exe).stem().string() + ".desktop";
}
#endif  // AURORA_PLATFORM_LINUX

/// @brief 断言某次注册以非法参数被拒。
/// @param extension 扩展名。
/// @param identity 应用身份。
/// @param label 判据描述。
auto expect_invalid_extension(const std::string &extension, const aurora::AppIdentity &identity,
                              const std::string &label) -> void {
    aurora::AssociationDeclaration declaration;
    declaration.extension = extension;
    declaration.mime_type = "text/plain";
    declaration.display_name = "Aurora Shell Verify";
    const auto result = aurora::register_file_association(declaration, identity);
    aurora_verify::check(!result.ok() && result.error().code_enum == aurora::ErrorCode::GeneralInvalidArgument, label);
}

/// @brief 自动段：能力位 / 非法参数 / 基线 / 声明取证 / 不抢默认 / 撤销。
/// @param exe 可执行路径。
/// @return 进程退出码（0 / 1 / 2）。
auto run_auto_stage(const std::string &exe) -> int {
    aurora_verify::emit("== Aurora file association live probe ==");
    aurora_verify::emit("executable: " + exe);

    const auto caps = aurora::platform().capabilities();
    aurora_verify::check(caps.file_association, "[capability] file_association is true");
    if (!caps.file_association) {
        aurora_verify::env_fail(
            "[env] this build has no file-association bridge",
            "configure with the platform shell-integration bridge (Windows / Linux / macOS backend)");
        return 2;
    }

    aurora::AppIdentity identity;
    identity.executable_path = exe;
    expect_invalid_extension("", identity, "[grammar] empty extension is rejected before OS work");
    expect_invalid_extension(".", identity, "[grammar] a lone dot is rejected before OS work");
    expect_invalid_extension("a.b", identity, "[grammar] an embedded dot is rejected before OS work");

    const auto baseline = aurora::associated_application(AURORA_PROBE_EXTENSION);
    aurora_verify::check(baseline.ok(), "[baseline] associated_application() succeeds before any declaration");
    if (!baseline.ok()) {
        aurora_verify::note("[baseline] error: " + baseline.error().code + " / " + baseline.error().message);
    }
    const std::string baseline_owner = baseline.ok() ? baseline.value().application : std::string{};
    aurora_verify::note("[baseline] current .log handler: " +
                        (baseline_owner.empty() ? std::string("<none>") : baseline_owner));

#ifdef AURORA_PLATFORM_WINDOWS
    const std::string baseline_defaults = default_handler_evidence(AURORA_PROBE_EXTENSION);
    aurora_verify::note("[baseline] default evidence: " + baseline_defaults);
    const std::string probe_stem = std::filesystem::path(exe).stem().string();
    const std::wstring probe_app_id(probe_stem.begin(), probe_stem.end());
    std::string baseline_registered_value;
    const bool baseline_app_registration =
        read_hkcu_string(L"Software\\RegisteredApplications", probe_app_id, baseline_registered_value);
    const bool baseline_capabilities = hkcu_key_exists(L"Software\\AuroraApplications\\" + probe_app_id);
#elif defined(AURORA_PLATFORM_LINUX)
    const std::string mimeapps = config_home().empty() ? std::string{} : config_home() + "/mimeapps.list";
    const std::string baseline_defaults =
        mimeapps.empty() ? std::string{} : section_text(read_text(mimeapps), "Default Applications");
#endif

    aurora::AssociationDeclaration declaration;
    declaration.extension = AURORA_PROBE_EXTENSION;
    declaration.mime_type = "text/plain";
    declaration.display_name = "Aurora Shell Verify";

    const auto declared = aurora::register_file_association(declaration, identity);
    if (!declared.ok()) {
#ifdef AURORA_PLATFORM_MACOS
        if (aurora_verify::contains_ascii_ci(declared.error().message, "CFBundleDocumentTypes") ||
            aurora_verify::contains_ascii_ci(declared.error().message, "bundle")) {
            aurora_verify::env_fail("[env] macOS document types must be declared statically in CFBundleDocumentTypes",
                                    "run this probe from a probe .app whose Info.plist declares the .log type");
            return 2;
        }
#endif
        aurora_verify::check(false, "[declare] register_file_association() succeeds");
        aurora_verify::note("[declare] error: " + declared.error().code + " / " + declared.error().message);
        return 1;
    }
    aurora_verify::check(true, "[declare] register_file_association() succeeds");

#ifdef AURORA_PLATFORM_WINDOWS
    aurora_verify::check(
        declaration_present(exe, true),
        "[declare] independent registry evidence: RegisteredApplications + Capabilities + ProgID command");
    aurora_verify::check(default_handler_evidence(AURORA_PROBE_EXTENSION) == baseline_defaults,
                         "[declare] the system default handler evidence is byte-identical (no default seized)");
#elif defined(AURORA_PLATFORM_LINUX)
    const std::string entry_path =
        data_home().empty() ? std::string{} : data_home() + "/applications/" + desktop_id(exe);
    const std::string entry = entry_path.empty() ? std::string{} : read_text(entry_path);
    aurora_verify::check(!entry.empty() && aurora_verify::contains_ascii_ci(entry, "text/plain"),
                         "[declare] independent file evidence: the desktop entry declares the MIME type");
    const std::string listed = section_text(read_text(mimeapps), "Added Associations");
    aurora_verify::check(aurora_verify::contains_ascii_ci(listed, desktop_id(exe)),
                         "[declare] independent file evidence: mimeapps.list lists the app under Added Associations");
    aurora_verify::check(section_text(read_text(mimeapps), "Default Applications") == baseline_defaults,
                         "[declare] the [Default Applications] section is byte-identical (no default seized)");
#endif

    const auto revoked = aurora::unregister_file_association(AURORA_PROBE_EXTENSION, identity);
#ifdef AURORA_PLATFORM_MACOS
    aurora_verify::check(!revoked.ok() && revoked.error().code_enum == aurora::ErrorCode::FileAssociationFailed &&
                             aurora_verify::contains_ascii_ci(revoked.error().message, "CFBundleDocumentTypes"),
                         "[revoke] macOS reports file-association-failed naming the static bundle declaration");
#else
    aurora_verify::check(revoked.ok(), "[revoke] unregister_file_association() succeeds");
    if (!revoked.ok()) {
        aurora_verify::note("[revoke] error: " + revoked.error().code + " / " + revoked.error().message);
        return 1;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    aurora_verify::check(!declaration_present(exe, false),
                         "[revoke] independent registry evidence: the declaration is gone");
    aurora_verify::check(default_handler_evidence(AURORA_PROBE_EXTENSION) == baseline_defaults,
                         "[revoke] the system default handler evidence is still byte-identical");
#elif defined(AURORA_PLATFORM_LINUX)
    const std::string listed_after = section_text(read_text(mimeapps), "Added Associations");
    aurora_verify::check(!aurora_verify::contains_ascii_ci(listed_after, desktop_id(exe)),
                         "[revoke] independent file evidence: Added Associations no longer lists the app");
    aurora_verify::check(section_text(read_text(mimeapps), "Default Applications") == baseline_defaults,
                         "[revoke] the [Default Applications] section is still byte-identical");
#endif
    const auto after = aurora::associated_application(AURORA_PROBE_EXTENSION);
    aurora_verify::check(after.ok() && after.value().application == baseline_owner,
                         "[revoke] associated_application() returns to the baseline handler");
    const auto again = aurora::unregister_file_association(AURORA_PROBE_EXTENSION, identity);
    aurora_verify::check(again.ok(), "[revoke] a second unregister is an idempotent success");
#ifdef AURORA_PLATFORM_WINDOWS
    // 收尾卫生：撤销只清「声明」，应用级登记（RegisteredApplications + Capabilities 根）按设计留存；
    // 探针把基线里原本没有的这两处删回没有，避免每次真机验收都在用户注册表里留一份探针残留。
    bool cleanup_ok = true;
    if (!baseline_app_registration) {
        cleanup_ok = delete_hkcu_value(L"Software\\RegisteredApplications", probe_app_id) && cleanup_ok;
    }
    if (!baseline_capabilities) {
        cleanup_ok = delete_hkcu_tree(L"Software\\AuroraApplications\\" + probe_app_id) && cleanup_ok;
    }
    aurora_verify::check(cleanup_ok, "[restore] the probe-specific app registration is removed");
#endif
#endif
    return 0;
}

/// @brief 人工段：提示人在文件管理器里查看「打开方式」候选。
/// @return 0 = 已确认；7 = 未确认 / 无输入。
auto run_interactive_stage() -> int {
    aurora_verify::emit("---- interactive segment ----");
    aurora_verify::emit("Declare the .log type again from an application window, then open the file manager's");
    aurora_verify::emit("'Open with' menu and confirm this app appears as a candidate (not as the default).");
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
        aurora_verify::emit("[PASS] human confirmed the candidate list entry");
        return 0;
    }
    aurora_verify::emit("[PEND] human confirmation not given; the item stays unverified");
    return 7;
}

}  // namespace

// 探针入口不做异常兜底：任何抛出都该让进程带非零码终止，而不是被吞成「通过了」。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("File association declaration live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    aurora::init_console();

    const std::string exe = aurora_verify::executable_path(argc > 0 ? *argv : nullptr);
    const int auto_code = run_auto_stage(exe);
    if (auto_code != 0) {
        return auto_code;
    }
    if (!interactive) {
        aurora_verify::emit("Skipping the interactive stage (re-run with --interactive to confirm visually).");
        aurora_verify::emit(aurora_verify::failures() == 0 ? "PASS (auto stage)" : "FAIL");
        return aurora_verify::failures() == 0 ? 0 : 1;
    }
    const int human_code = run_interactive_stage();
    if (aurora_verify::failures() != 0) {
        return 1;
    }
    if (aurora_verify::env_unavailable()) {
        return 2;
    }
    return human_code;
}
