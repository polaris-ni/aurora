// win32_shell_integration.cpp — Win32 桌面外壳集成桥（URL 方案 / 文件关联 / 登录项 /
// 单实例 / 打印会话）。
//
// 全部写入都落在用户级位置（`HKCU`），不碰需要提权的 `HKLM`；撤销前先验归属，归属是别人时
// 一个字节都不改。字段口径与边界见各公共头与 `specification/06-app-platform.md`。
#include "aurora/app/detail/win32_shell_integration.h"

#ifdef AURORA_PLATFORM_WINDOWS

#ifndef _WIN32_WINNT
// Windows SDK 版本旋钮，不可改名（见 platform.h 例外说明）
// NOLINTNEXTLINE(*-macro-usage, *-reserved-identifier, *-identifier-naming)
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
// Windows SDK 版本旋钮，不可改名
// NOLINTNEXTLINE(*-macro-usage, *-reserved-identifier, *-identifier-naming)
#define _WIN32_IE 0x0600
#endif
#define WIN32_LEAN_AND_MEAN  // NOLINT(*-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <shlobj.h>     // SHChangeNotify
#include <shlwapi.h>    // AssocQueryStringW
#include <windows.h>
#include <winspool.h>   // EnumPrintersW / GetDefaultPrinterW
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/detail/shell_integration_translate.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/core/utf8.h"

namespace aurora::detail {

namespace {

// 【豁免说明】Win32 接线：句柄须以 reinterpret_cast 在 HWND 与 LPARAM 之间往返，且 SDK 宏
// 不可改名；逐点抑制不成比例，故按区间豁免（名单取自去掉本区间后 clang-tidy 实测报出的检查集）。
// NOLINTBEGIN(*-pro-type-*, performance-no-int-to-ptr, *-macro-usage, *-reserved-identifier, *-identifier-naming)

/// @brief 取可执行文件名（去目录与后缀），用于 ProgID / 注册表值名。
[[nodiscard]] auto executable_stem(const AppIdentity &identity) -> std::string {
    const std::filesystem::path path(identity.executable_path);
    const auto stem = path.stem().string();
    return stem.empty() ? std::string("AuroraApplication") : stem;
}

/// @brief UTF-8 → 宽字符；空串返回空。
[[nodiscard]] auto widen(std::string_view text) -> std::wstring { return internal::utf8_to_wstr(std::string(text)); }

/// @brief 宽字符 → UTF-8；空串返回空。
[[nodiscard]] auto narrow(const wchar_t *text) -> std::string {
    return text == nullptr ? std::string() : internal::wstr_to_utf8(text);
}

/// @brief ASCII 小写副本（命令模板比较用）。
[[nodiscard]] auto lower_copy(std::string text) -> std::string {
    std::ranges::transform(text, text.begin(), [](unsigned char ch) -> char {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    });
    return text;
}

/// @brief 命令模板是否引用本可执行文件（大小写不敏感的子串判定）。
[[nodiscard]] auto command_is_ours(std::string_view command, const AppIdentity &identity) -> bool {
    if (command.empty() || identity.executable_path.empty()) {
        return false;
    }
    return lower_copy(std::string(command)).find(lower_copy(identity.executable_path)) != std::string::npos;
}

/// @brief 读注册表字符串值（`value` 为空 = 默认值）。
[[nodiscard]] auto read_string(HKEY root, const std::wstring &subkey, const std::wstring &value)
    -> std::optional<std::wstring> {
    DWORD size = 0;
    const wchar_t *name = value.empty() ? nullptr : value.c_str();
    if (RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size == 0) {
        return std::nullopt;
    }
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    buffer.resize(size / sizeof(wchar_t));
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

/// @brief 写注册表字符串值（`value` 为空 = 默认值；`data` 为空写零长度 REG_SZ）。
[[nodiscard]] auto write_string(HKEY root, const std::wstring &subkey, const std::wstring &value,
                                const std::wstring &data) -> bool {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const wchar_t *name = value.empty() ? nullptr : value.c_str();
    const auto bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    const auto result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(data.c_str()), bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

/// @brief 读注册表二进制值（`StartupApproved` 的禁用记录）。
[[nodiscard]] auto read_binary(HKEY root, const std::wstring &subkey, const std::wstring &value)
    -> std::optional<std::vector<std::uint8_t>> {
    DWORD size = 0;
    if (RegGetValueW(root, subkey.c_str(), value.c_str(), RRF_RT_REG_BINARY, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size == 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> buffer(size, 0);
    if (RegGetValueW(root, subkey.c_str(), value.c_str(), RRF_RT_REG_BINARY, nullptr, buffer.data(), &size) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    return buffer;
}

/// @brief 删除整棵子键（不存在视为成功）。
[[nodiscard]] auto delete_tree(HKEY root, const std::wstring &subkey) -> bool {
    const LSTATUS status = RegDeleteTreeW(root, subkey.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

/// @brief 删除单个值（不存在视为成功）。
[[nodiscard]] auto delete_value(HKEY root, const std::wstring &subkey, const std::wstring &value) -> bool {
    const LSTATUS status = RegDeleteKeyValueW(root, subkey.c_str(), value.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

/// @brief 通知壳关联已变化（资源管理器据此刷新「打开方式」缓存）。
auto notify_association_changed() -> void { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

/// @brief `HKCU\Software\RegisteredApplications` 的本应用登记名（诊断与撤销定位用）。
[[nodiscard]] auto capabilities_path(const AppIdentity &identity) -> std::wstring {
    return L"Software\\AuroraApplications\\" + widen(executable_stem(identity)) + L"\\Capabilities";
}

// ---------- URL 方案 ----------

[[nodiscard]] auto scheme_key(const std::string &scheme) -> std::wstring {
    return L"Software\\Classes\\" + widen(scheme);
}

auto backend_register_scheme(std::string_view scheme, std::string_view display_name, const AppIdentity &identity)
    -> Result<void> {
    const std::string name(scheme);
    const std::wstring key = scheme_key(name);
    const std::wstring label = display_name.empty() ? widen(executable_stem(identity)) : widen(display_name);
    const std::wstring command = L"\"" + widen(identity.executable_path) + L"\" \"%1\"";
    if (!write_string(HKEY_CURRENT_USER, key, L"", L"URL:" + label + L" Protocol") ||
        !write_string(HKEY_CURRENT_USER, key, L"URL Protocol", L"") ||
        !write_string(HKEY_CURRENT_USER, key + L"\\DefaultIcon", L"",
                      L"\"" + widen(identity.executable_path) + L"\",0") ||
        !write_string(HKEY_CURRENT_USER, key + L"\\shell\\open\\command", L"", command)) {
        return make_error(ErrorCode::UrlSchemeFailed, "url-scheme: failed to write HKCU\\" + narrow(key.c_str()));
    }
    notify_association_changed();
    return Result<void>{};
}

/// @brief 读系统实际生效的命令模板（`AssocQueryStringW`），失败回落 `HKCU` 直接读。
[[nodiscard]] auto query_scheme_command(const std::string &scheme) -> std::optional<std::string> {
    const std::wstring uri = widen(scheme) + L":";
    DWORD size = 0;
    // 首调用（pszOut == nullptr）按文档回 S_FALSE 并给出所需字节数；S_FALSE 同样是「探到尺寸」。
    const HRESULT probe = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, uri.c_str(), nullptr, nullptr, &size);
    if ((probe == S_OK || probe == S_FALSE) && size > 0) {
        std::wstring buffer(size, L'\0');
        if (AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, uri.c_str(), nullptr, buffer.data(), &size) == S_OK) {
            return narrow(buffer.c_str());
        }
    }
    if (const auto direct = read_string(HKEY_CURRENT_USER, scheme_key(scheme) + L"\\shell\\open\\command", L"");
        direct.has_value()) {
        return narrow(direct->c_str());
    }
    return std::nullopt;
}

auto backend_unregister_scheme(std::string_view scheme, const AppIdentity &identity) -> Result<void> {
    const std::string name(scheme);
    const auto direct = read_string(HKEY_CURRENT_USER, scheme_key(name) + L"\\shell\\open\\command", L"");
    if (!direct.has_value()) {
        return Result<void>{};  // 归属为空（HKCU 下没有本用户的登记）：幂等成功
    }
    const auto command = narrow(direct->c_str());
    if (!command_is_ours(command, identity)) {
        return make_error(ErrorCode::UrlSchemeFailed,
                          "url-scheme: '" + name + "' is registered by another application: " + command);
    }
    if (!delete_tree(HKEY_CURRENT_USER, scheme_key(name))) {
        return make_error(ErrorCode::UrlSchemeFailed,
                          "url-scheme: failed to delete HKCU\\" + narrow(scheme_key(name).c_str()));
    }
    notify_association_changed();
    return Result<void>{};
}

auto backend_scheme_owner(std::string_view scheme) -> Result<std::string> {
    const std::string name(scheme);
    if (const auto command = query_scheme_command(name); command.has_value()) {
        return *command;
    }
    return std::string{};
}

// ---------- 文件关联 ----------

[[nodiscard]] auto prog_id(const AppIdentity &identity, const std::string &extension) -> std::wstring {
    return L"Aurora." + widen(executable_stem(identity)) + L"." + widen(extension);
}

auto backend_query_association(std::string_view extension) -> Result<AssociationInfo> {
    const std::wstring dotted = L"." + widen(std::string(extension));
    DWORD size = 0;
    const HRESULT first = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, dotted.c_str(), nullptr, nullptr, &size);
    // 首调用（pszOut == nullptr）按文档回 S_FALSE 并给出所需字节数，不是失败；只有真错误才进下面的
    // 归属判定。把 S_FALSE 当失败会让每个「有处理程序」的扩展名都报 file-association-failed。
    if (first != S_OK && first != S_FALSE) {
        const DWORD error = HRESULT_CODE(first);
        if (first == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) || error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_NO_ASSOCIATION) {
            return AssociationInfo{};  // 没有关联：空集合不是错误
        }
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: AssocQueryString failed for " + std::string(extension));
    }
    if (size == 0) {
        return AssociationInfo{};
    }
    std::wstring buffer(size, L'\0');
    if (AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, dotted.c_str(), nullptr, buffer.data(), &size) != S_OK) {
        return make_error(
            ErrorCode::FileAssociationFailed,
            "file-association: AssocQueryString could not read the handler for " + std::string(extension));
    }
    AssociationInfo info;
    info.application = narrow(buffer.c_str());
    info.source = "windows-assocquery";
    return info;
}

auto backend_declare_association(const AssociationDeclaration &declaration, std::string_view mime,
                                 const AppIdentity &identity) -> Result<void> {
    const std::wstring pid = prog_id(identity, declaration.extension);
    const std::wstring label =
        declaration.display_name.empty() ? widen(executable_stem(identity)) : widen(declaration.display_name);
    const std::wstring app_id = widen(executable_stem(identity));
    const std::wstring command = L"\"" + widen(identity.executable_path) + L"\" \"%1\"";
    const std::wstring icon = L"\"" + widen(identity.executable_path) + L"\",0";
    const std::wstring classes = L"Software\\Classes\\" + pid;
    const std::wstring capabilities = capabilities_path(identity);
    const bool ok =
        write_string(HKEY_CURRENT_USER, classes, L"", label + L" file") &&
        write_string(HKEY_CURRENT_USER, classes + L"\\shell\\open\\command", L"", command) &&
        write_string(HKEY_CURRENT_USER, classes + L"\\DefaultIcon", L"", icon) &&
        write_string(HKEY_CURRENT_USER, capabilities, L"ApplicationName", label) &&
        write_string(HKEY_CURRENT_USER, capabilities, L"ApplicationDescription", label + L" (Aurora application)") &&
        write_string(HKEY_CURRENT_USER, capabilities + L"\\FileAssociations", L"." + widen(declaration.extension),
                     pid) &&
        write_string(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", app_id, capabilities);
    (void)mime;  // Windows 的 ProgID 归属不按 MIME 记录；MIME 只用于 Linux 侧声明
    if (!ok) {
        return make_error(
            ErrorCode::FileAssociationFailed,
            "file-association: failed to write the user-level registration for ." + declaration.extension);
    }
    notify_association_changed();
    return Result<void>{};
}

auto backend_undeclare_association(std::string_view extension, const AppIdentity &identity) -> Result<void> {
    const std::string ext(extension);
    const std::wstring capabilities = capabilities_path(identity);
    const auto registered = read_string(HKEY_CURRENT_USER, capabilities + L"\\FileAssociations", L"." + widen(ext));
    if (!registered.has_value()) {
        return Result<void>{};  // 本应用没声明过这个扩展名：幂等成功
    }
    if (!delete_tree(HKEY_CURRENT_USER, L"Software\\Classes\\" + *registered)) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: failed to delete the ProgID for ." + ext);
    }
    if (!delete_value(HKEY_CURRENT_USER, capabilities + L"\\FileAssociations", L"." + widen(ext))) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: failed to remove the capability entry for ." + ext);
    }
    notify_association_changed();
    return Result<void>{};
}

// ---------- 登录项 ----------

constexpr const wchar_t *AURORA_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t *AURORA_STARTUP_APPROVED_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

/// @brief 登录项命令模板：可执行路径 + 命令行快照（逐参数安全引用）。
[[nodiscard]] auto login_command(const AppIdentity &identity) -> std::wstring {
    std::wstring command = L"\"" + widen(identity.executable_path) + L"\"";
    for (const auto &argument : identity.arguments) {
        command.append(L" ");
        command.append(widen(desktop_shell::quote_exec_argument(argument)));
    }
    return command;
}

[[nodiscard]] auto backend_run_at_login_query(const AppIdentity &identity) -> RunAtLoginState {
    const std::wstring name = widen(executable_stem(identity));
    const auto value = read_string(HKEY_CURRENT_USER, AURORA_RUN_KEY, name);
    if (!value.has_value()) {
        return RunAtLoginState{.status = RunAtLoginStatus::Disabled};  // 从未设置过
    }
    if (const auto approved = read_binary(HKEY_CURRENT_USER, AURORA_STARTUP_APPROVED_KEY, name);
        approved.has_value() && !approved->empty() && (approved->front() & 0x01U) == 0U) {
        return RunAtLoginState{.status = RunAtLoginStatus::Disabled,
                               .detail = "run-at-login: the entry is disabled in Windows startup settings"};
    }
    return RunAtLoginState{.status = RunAtLoginStatus::Enabled};
}

auto backend_run_at_login_set(bool enabled, const AppIdentity &identity) -> Result<void> {
    const std::wstring name = widen(executable_stem(identity));
    if (enabled) {
        if (!write_string(HKEY_CURRENT_USER, AURORA_RUN_KEY, name, login_command(identity))) {
            return make_error(ErrorCode::RunAtLoginFailed, "run-at-login: failed to write the HKCU Run value");
        }
        // 用户此前在任务管理器里禁用过时，StartupApproved 会留一条禁用记录；显式开启要覆盖它，
        // 否则 Run 值写了也不生效（删掉记录即回到 OS 默认的「启用」）。
        (void)delete_value(HKEY_CURRENT_USER, AURORA_STARTUP_APPROVED_KEY, name);
        return Result<void>{};
    }
    if (!delete_value(HKEY_CURRENT_USER, AURORA_RUN_KEY, name)) {
        return make_error(ErrorCode::RunAtLoginFailed, "run-at-login: failed to remove the HKCU Run value");
    }
    return Result<void>{};
}

// ---------- 打印会话 ----------

[[nodiscard]] auto backend_print_targets() -> Result<std::vector<PrintTarget>> {
    DWORD needed = 0;
    DWORD returned = 0;
    if (EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &needed, &returned) == 0) {
        const DWORD error = GetLastError();
        if (error == ERROR_INSUFFICIENT_BUFFER) {
            // 正常：先取所需字节数
        } else if (error == ERROR_FILE_NOT_FOUND || needed == 0) {
            return std::vector<PrintTarget>{};  // 没有打印机：空集合不是错误
        } else {
            return make_error(ErrorCode::PrintSessionFailed,
                              "print: EnumPrinters failed with error " + std::to_string(error));
        }
    }
    std::vector<std::uint8_t> buffer(needed, 0);
    if (needed > 0 && EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), &needed, &returned) == 0) {
        return make_error(ErrorCode::PrintSessionFailed,
                          "print: EnumPrinters failed with error " + std::to_string(GetLastError()));
    }
    auto *infos = reinterpret_cast<PRINTER_INFO_4W *>(buffer.data());
    const std::span<const PRINTER_INFO_4W> info_view(infos, returned);
    std::wstring default_name;
    DWORD default_size = 0;
    if (GetDefaultPrinterW(nullptr, &default_size) != 0 && default_size > 0) {
        std::wstring buffer_name(default_size, L'\0');
        if (GetDefaultPrinterW(buffer_name.data(), &default_size) != 0) {
            buffer_name.resize(default_size > 0 ? default_size - 1 : 0);
            default_name = buffer_name;
        }
    }
    std::vector<PrintTarget> targets;
    targets.reserve(returned);
    for (DWORD index = 0; index < returned; ++index) {
        if (info_view[index].pPrinterName == nullptr) {
            continue;
        }
        PrintTarget target;
        target.name = narrow(info_view[index].pPrinterName);
        target.id = target.name;
        target.is_default = !default_name.empty() && _wcsicmp(default_name.c_str(), info_view[index].pPrinterName) == 0;
        targets.push_back(std::move(target));
    }
    return targets;
}

/// @brief Win32 打印会话：GDI 设备上下文 + `StartDoc` / `StartPage` / `EndDoc`。
class Win32PrintSession final : public desktop_shell::PrintSessionBackend {
  public:
    Win32PrintSession(HDC dc, PageSpec spec) : dc_(dc), spec_(spec) {}
    ~Win32PrintSession() override { abort(); }

    Win32PrintSession(const Win32PrintSession &) = delete;
    auto operator=(const Win32PrintSession &) -> Win32PrintSession & = delete;
    Win32PrintSession(Win32PrintSession &&) = delete;
    auto operator=(Win32PrintSession &&) -> Win32PrintSession & = delete;

    [[nodiscard]] auto submit_page(const Image &page) -> Result<void> override {
        if (dc_ == nullptr || finished_) {
            return make_error(ErrorCode::PrintSessionFailed, "print: session is no longer open");
        }
        if (page.width <= 0 || page.height <= 0 ||
            page.pixels.size() != static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height) * 4U) {
            return make_error(ErrorCode::GeneralInvalidArgument, "print: page image must be RGBA8");
        }
        const int horizontal = GetDeviceCaps(dc_, HORZRES);
        const int vertical = GetDeviceCaps(dc_, VERTRES);
        if (horizontal <= 0 || vertical <= 0) {
            return make_error(ErrorCode::PrintSessionFailed, "print: printer device reports no drawable area");
        }
        const auto left = static_cast<int>(spec_.margins.left_mm / spec_.width_mm * horizontal);
        const auto top = static_cast<int>(spec_.margins.top_mm / spec_.height_mm * vertical);
        const auto right = horizontal - static_cast<int>(spec_.margins.right_mm / spec_.width_mm * horizontal);
        const auto bottom = vertical - static_cast<int>(spec_.margins.bottom_mm / spec_.height_mm * vertical);
        const int target_width = right - left;
        const int target_height = bottom - top;
        if (target_width <= 0 || target_height <= 0) {
            return make_error(ErrorCode::PrintSessionFailed, "print: margins leave no drawable area");
        }
        // RGBA（已合成不透明）→ BGR 自底向上、每行 4 字节对齐。
        const auto stride = static_cast<std::size_t>(((page.width * 3) + 3) & ~3);
        std::vector<std::uint8_t> bgr(stride * static_cast<std::size_t>(page.height), 0);
        for (int y = 0; y < page.height; ++y) {
            const auto src_row = static_cast<std::size_t>(page.height - 1 - y);
            const auto dst_row = static_cast<std::size_t>(y);
            for (int x = 0; x < page.width; ++x) {
                const auto src = ((src_row * static_cast<std::size_t>(page.width)) + static_cast<std::size_t>(x)) * 4U;
                const auto dst = (dst_row * stride) + (static_cast<std::size_t>(x) * 3U);
                bgr[dst] = page.pixels[src + 2U];
                bgr[dst + 1U] = page.pixels[src + 1U];
                bgr[dst + 2U] = page.pixels[src];
            }
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = page.width;
        info.bmiHeader.biHeight = page.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;
        if (StartPage(dc_) <= 0) {
            return make_error(ErrorCode::PrintSessionFailed, "print: StartPage failed");
        }
        const int copied = StretchDIBits(dc_, left, top, target_width, target_height, 0, 0, page.width, page.height,
                                         bgr.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        const int ended = EndPage(dc_);
        if (copied <= 0 || ended <= 0) {  // 成功时返回拷贝的扫描行数（> 0），失败为 GDI_ERROR
            return make_error(ErrorCode::PrintSessionFailed, "print: GDI page transfer failed");
        }
        return Result<void>{};
    }

    [[nodiscard]] auto finish() -> Result<void> override {
        if (dc_ == nullptr || finished_) {
            return Result<void>{};
        }
        finished_ = true;
        const int result = EndDoc(dc_);
        DeleteDC(dc_);
        dc_ = nullptr;
        if (result <= 0) {
            return make_error(ErrorCode::PrintSessionFailed, "print: EndDoc failed");
        }
        return Result<void>{};
    }

    auto abort() -> void override {
        if (dc_ != nullptr) {
            if (!finished_) {
                AbortDoc(dc_);
            }
            DeleteDC(dc_);
            dc_ = nullptr;
        }
        finished_ = true;
    }

  private:
    HDC dc_ = nullptr;
    PageSpec spec_;
    bool finished_ = false;
};

auto backend_open_print_session(std::string_view target_id, const PageSpec &spec)
    -> Result<std::unique_ptr<desktop_shell::PrintSessionBackend>> {
    const std::wstring printer = widen(std::string(target_id));
    HDC dc = CreateDCW(L"WINSPOOL", printer.c_str(), nullptr, nullptr);
    if (dc == nullptr) {
        return make_error(ErrorCode::PrintSessionFailed,
                          "print: cannot open a device context for printer '" + std::string(target_id) + "'");
    }
    DOCINFOW doc{};
    doc.cbSize = sizeof(doc);
    doc.lpszDocName = L"Aurora Document";
    doc.lpszDatatype = L"RAW";
    if (StartDocW(dc, &doc) <= 0) {
        DeleteDC(dc);
        return make_error(ErrorCode::PrintSessionFailed, "print: StartDoc was rejected by the spooler");
    }
    return std::unique_ptr<desktop_shell::PrintSessionBackend>(std::make_unique<Win32PrintSession>(dc, spec));
}

// ---------- 单实例 ----------

constexpr const wchar_t *AURORA_INSTANCE_WINDOW_CLASS = L"AuroraSingleInstanceWindow";
constexpr std::uintptr_t AURORA_INSTANCE_MAGIC = 0x41555241U;  // 'AURA'
constexpr std::size_t AURORA_INSTANCE_MAX_URI = 16384U;

struct InstanceState {
    HANDLE mutex = nullptr;  ///< 首实例持有的有名互斥量
    HWND window = nullptr;  ///< 首实例的消息窗口（`WM_COPYDATA` 收件人）
    HANDLE ack = nullptr;  ///< 首实例的确认事件（第二实例等待它）
    std::function<void(std::string)> sink;  ///< 收到的 URI 入队回调（公共层提供）
    bool class_registered = false;  ///< 窗口类是否已注册
};

/// @brief 进程级单实例状态（单线程 UI：无锁）。
[[nodiscard]] auto instance_state() -> InstanceState & {
    static InstanceState state;
    return state;
}

/// @brief 消息窗口过程：只处理 `WM_COPYDATA`，其余交默认过程。
LRESULT CALLBACK instance_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COPYDATA) {
        auto *copy = reinterpret_cast<COPYDATASTRUCT *>(lparam);
        auto &state = instance_state();
        if (copy != nullptr && copy->dwData == AURORA_INSTANCE_MAGIC && copy->lpData != nullptr && copy->cbData > 0 &&
            copy->cbData <= AURORA_INSTANCE_MAX_URI) {
            const auto *text = static_cast<const char *>(copy->lpData);
            std::string uri(text, copy->cbData);
            while (!uri.empty() && uri.back() == '\0') {
                uri.pop_back();
            }
            if (!uri.empty() && state.sink) {
                state.sink(std::move(uri));
            }
            if (state.ack != nullptr) {
                SetEvent(state.ack);
            }
            return TRUE;
        }
        return FALSE;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/// @brief 注册窗口类（幂等）。
[[nodiscard]] auto register_instance_class() -> bool {
    auto &state = instance_state();
    if (state.class_registered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &instance_window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = AURORA_INSTANCE_WINDOW_CLASS;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    state.class_registered = true;
    return true;
}

/// @brief 等待首实例确认（超时即视为失败）。
[[nodiscard]] auto wait_for_ack(HANDLE ack, DWORD timeout_ms) -> bool {
    if (ack == nullptr) {
        return false;
    }
    const DWORD waited = WaitForSingleObject(ack, timeout_ms);
    return waited == WAIT_OBJECT_0;
}

auto backend_acquire_instance(const AppIdentity &identity, const std::vector<std::string> &uris,
                              const std::function<void(std::string)> &sink) -> Result<SingleInstanceOutcome> {
    const std::wstring key = widen(aurora::resolve_instance_key(identity));
    const std::wstring mutex_name = L"Local\\AuroraInstance_" + key;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (mutex == nullptr) {
        return make_error(ErrorCode::PlatformUnavailable, "single-instance: CreateMutex failed");
    }
    const bool primary = GetLastError() != ERROR_ALREADY_EXISTS;
    if (primary) {
        auto &state = instance_state();
        state.mutex = mutex;
        state.sink = sink;
        if (!register_instance_class()) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
            state.mutex = nullptr;
            return make_error(ErrorCode::PlatformUnavailable, "single-instance: RegisterClassEx failed");
        }
        state.window = CreateWindowExW(0, AURORA_INSTANCE_WINDOW_CLASS, key.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE,
                                       nullptr, GetModuleHandleW(nullptr), nullptr);
        if (state.window == nullptr) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
            state.mutex = nullptr;
            state.sink = nullptr;
            return make_error(ErrorCode::PlatformUnavailable, "single-instance: CreateWindowEx(HWND_MESSAGE) failed");
        }
        const std::wstring ack_name = L"Local\\AuroraInstanceAck_" + key;
        state.ack = CreateEventW(nullptr, FALSE, FALSE, ack_name.c_str());
        return SingleInstanceOutcome::Primary;
    }

    // 已有实例：把命令行 URI 交给它并等待确认；找不到窗口 / 超时 / 明确失败即回落为新实例。
    HWND existing = FindWindowW(AURORA_INSTANCE_WINDOW_CLASS, key.c_str());
    if (existing == nullptr) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return SingleInstanceOutcome::FallbackNewInstance;
    }
    if (uris.empty()) {
        // 没有 URI 可转交：视为已交给首实例（窗口激活由应用自己的单实例逻辑决定）。
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return SingleInstanceOutcome::Forwarded;
    }
    const std::wstring ack_name = L"Local\\AuroraInstanceAck_" + key;
    HANDLE ack = CreateEventW(nullptr, FALSE, FALSE, ack_name.c_str());
    bool forwarded = true;
    for (const auto &uri : uris) {
        if (uri.size() > AURORA_INSTANCE_MAX_URI) {
            forwarded = false;
            break;
        }
        COPYDATASTRUCT copy{};
        copy.dwData = AURORA_INSTANCE_MAGIC;
        copy.cbData = static_cast<DWORD>(uri.size() + 1U);
        copy.lpData = const_cast<char *>(uri.c_str());
        SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy));
        if (!wait_for_ack(ack, 3000)) {
            forwarded = false;
            break;
        }
    }
    if (ack != nullptr) {
        CloseHandle(ack);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return forwarded ? SingleInstanceOutcome::Forwarded : SingleInstanceOutcome::FallbackNewInstance;
}

auto backend_poll_instance() -> void {
    // Windows 的唤起由 OS 消息泵推进；`WM_COPYDATA` 收件在窗口过程里，本函数无事可做。
}

auto backend_release_instance() -> void {
    auto &state = instance_state();
    if (state.window != nullptr) {
        DestroyWindow(state.window);
        state.window = nullptr;
    }
    if (state.ack != nullptr) {
        CloseHandle(state.ack);
        state.ack = nullptr;
    }
    if (state.mutex != nullptr) {
        ReleaseMutex(state.mutex);
        CloseHandle(state.mutex);
        state.mutex = nullptr;
    }
    state.sink = nullptr;
}

}  // namespace

auto win32_shell_integration_ensure() -> bool {
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
    desktop_shell::register_print_backend(desktop_shell::PrintBackend{
        .targets = &backend_print_targets,
        .open = &backend_open_print_session,
    });
    desktop_shell::register_single_instance_backend(desktop_shell::SingleInstanceBackend{
        .acquire = &backend_acquire_instance,
        .poll = &backend_poll_instance,
        .release = &backend_release_instance,
    });
    AURORA_LOG_DEBUG("shell", "win32 desktop shell integration bridges registered");
    return true;
}

// NOLINTEND(*-pro-type-*, performance-no-int-to-ptr, *-macro-usage, *-reserved-identifier, *-identifier-naming)

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS
