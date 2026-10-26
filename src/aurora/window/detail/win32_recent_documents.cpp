// win32_recent_documents.cpp — OS 级最近文档投影的 Windows 桥（`SHAddToRecentDocs`）。
//
// 机制与边界（用户策略下静默无操作、无逐条撤销 API、撤销靠删除壳自己写出的 `.lnk`）见
// `win32_recent_documents.h` 文件头；本文件只做该口径的落地，不另立口径。
#include "aurora/core/platform.h"
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
#include <objbase.h>    // CoInitializeEx / CoCreateInstance
#include <shlobj.h>     // SHAddToRecentDocs / SHGetFolderPathW / CSIDL_RECENT / SHChangeNotify
#include <shobjidl.h>   // IShellLinkW
#include <windows.h>
// clang-format on
#endif

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/utf8.h"
#include "aurora/window/detail/recent_documents_translate.h"
#include "aurora/window/detail/win32_recent_documents.h"

#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

namespace aurora::detail {
namespace {

/// @brief 进程级桥态：通路引用计数 + 本进程投影记录。
///
/// 两者生命周期**不同**：引用计数归零只注销通路（窗口全关），投影记录仍留着——它是「本进程贡献过
/// 哪些 OS 记录」的账本，撤销必须能在通路重开之后仍算得出上一次投影的范围。
struct BridgeState {
    int refs = 0;  ///< 宿主守卫与自举的引用计数；> 0 时通路已登记
    std::vector<std::string> projected;  ///< 本进程投影记录（UTF-8 原样路径，去重）
};

auto state() -> BridgeState & {
    static BridgeState instance;  // 单线程 UI：无锁（与 RecentDocuments 单例同款纪律）
    return instance;
}

/// @brief ASCII 小写副本（路径比较用）。
///
/// Windows 路径大小写不敏感，而 store 的口径是原样字符串（见 `recent_documents.h`「路径口径」）。
/// 撤销侧按大小写不敏感比较是**放宽召回**（少删不是错删的反面：别的应用写的 `.lnk` 仍靠目标解析
/// 把关），不改 store 侧的字符串口径。
[[nodiscard]] auto ascii_lower(std::string text) -> std::string {
    std::ranges::transform(text, text.begin(), [](unsigned char ch) -> char {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    });
    return text;
}

/// @brief RAII COM 初始化（形态同 `file_dialog_win32.cpp` 的 `ComInit`）。
class ComInit {
  public:
    ComInit() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {
        if (hr_ == RPC_E_CHANGED_MODE) {
            hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        }
    }
    ~ComInit() {
        if (hr_ == S_OK || hr_ == S_FALSE) {
            CoUninitialize();
        }
    }
    ComInit(const ComInit &) = delete;
    auto operator=(const ComInit &) -> ComInit & = delete;
    ComInit(ComInit &&) = delete;
    auto operator=(ComInit &&) -> ComInit & = delete;

    explicit operator bool() const { return SUCCEEDED(hr_); }

  private:
    HRESULT hr_ = E_FAIL;
};

/// @brief 取用户 Recent 目录（`%APPDATA%\Microsoft\Windows\Recent`）。
/// @param out 成功时写入目录宽字符路径。
/// @return 取到为 true；`SHGetFolderPathW` 失败（无用户配置目录等）为 false。
[[nodiscard]] auto recent_folder(std::wstring &out) -> bool {
    wchar_t buffer[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_RECENT, nullptr, SHGFP_TYPE_CURRENT, buffer))) {
        return false;
    }
    out = buffer;
    return !out.empty();
}

/// @brief 解析一条 `.lnk` 的真实目标路径（`IShellLinkW::GetPath`）。
/// @param link_path 快捷方式全路径（宽字符）。
/// @param out 成功时写入目标路径；无法解析（目标已失效、非文件链接、COM 失败、超长路径）时为空。
auto resolve_link_target(const std::wstring &link_path, std::wstring &out) -> void {
    out.clear();
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return;
    }
    IPersistFile *persist = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
        if (SUCCEEDED(persist->Load(link_path.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH] = {};
            if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, SLGP_UNCPRIORITY)) && target[0] != L'\0') {
                out = target;
            }
        }
        persist->Release();
    }
    link->Release();
}

/// @brief OS 侧「最近使用的项目」追踪开关（`HKCU\...\Explorer\Advanced\Start_TrackDocs`）。
///
/// 实测：该值为 0（系统设置里关掉「在跳转列表中显示最近打开的项目」）时，`SHAddToRecentDocs`
/// **静默无操作**——API 返回 void、无从回读，但这条注册表是可证的旁证。键不存在按 OS 默认（开启）处理。
/// @return 用户关闭追踪时为 true；开启或读不到键时为 false。
[[nodiscard]] auto tracking_disabled_by_user() -> bool {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                     L"Start_TrackDocs", RRF_RT_DWORD, nullptr, &value, &size);
    return SUCCEEDED(status) && value == 0U;
}

/// @brief 后端回调：逐条 `SHAddToRecentDocs(SHARD_PATHW, ...)`。
auto project_backend(const std::vector<std::string> &plan) -> Result<void> {
    if (tracking_disabled_by_user()) {
        // 可证的「OS 不会落记录」旁证：如实结构化报错（retryable=false，须用户开回系统开关），
        // 不谎报成功。同时不把已记账的条目投出去——API 会静默无操作，记了账也撤不掉。
        return make_error(ErrorCode::PlatformUnavailable,
                          "recent-documents: OS recent-items tracking is disabled (Start_TrackDocs=0)");
    }
    for (const auto &path : plan) {
        const auto wide = internal::utf8_to_wstr(path);
        if (wide.empty()) {
            // 可证的失败路径之一：UTF-8 形态不可转换，OS 侧连一次调用都没发生。
            return make_error(ErrorCode::PlatformUnavailable, "recent-documents: path is not convertible to UTF-16");
        }
        // 投递本身没有回执：API 返回 void，`GetLastError` 也不是它的约定失败通道（壳内部会经由
        // 无关调用置位），故不据它判定成败——判据只在可证的两处：追踪开关（上方）与形态转换。
        // 返回成功的确切含义是「已投递给 OS」，不等于「OS 已落记录」；后者由真机探针直查
        // Recent 目录验收，口径写进 `codespec/specification/06-app-platform.md` §8.10。
        SHAddToRecentDocs(SHARD_PATHW, wide.c_str());
        auto &record = state().projected;
        if (std::ranges::find(record, path) == record.end()) {
            record.emplace_back(path);  // 记账：撤销侧据此定位该条快捷方式
        }
    }
    AURORA_LOG_DEBUG("recent-documents", "projected ", plan.size(), " entries to the OS");
    return Result<void>{};
}

/// @brief 后端回调：撤销本进程投影记录内的条目。
auto clear_backend() -> Result<void> {
    auto &self = state();
    if (self.projected.empty()) {
        return Result<void>{};  // 无账可撤：空操作而非错误
    }
    std::wstring folder;
    if (!recent_folder(folder)) {
        return make_error(ErrorCode::PlatformUnavailable, "recent-documents: user Recent folder is unavailable");
    }
    ComInit com;
    if (!com) {
        return make_error(ErrorCode::PlatformComInitFailed, "CoInitializeEx failed");
    }
    // 枚举该目录下全部 .lnk，逐条解析目标；命中投影记录者删除并通知壳刷新。
    // 之所以枚举而非按文件名直算：壳的快捷方式名由目标显示名派生，同名不同目录会撞名，
    // 只有解析目标才能区分「本进程投影的那一条」与「别的应用的同名那一条」。
    const std::size_t recorded = self.projected.size();
    // `keep`：撤销后仍需记账的条目（默认留账，仅「确认删除成功」者销账，见下方收敛段）。
    std::vector<char> keep(recorded, 1);
    std::size_t removed = 0;
    std::size_t failed = 0;
    WIN32_FIND_DATAW find_data = {};
    auto pattern = folder + L"\\*.lnk";
    const HANDLE search = FindFirstFileW(pattern.c_str(), &find_data);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD first_error = GetLastError();
        if (first_error != ERROR_FILE_NOT_FOUND && first_error != ERROR_NO_MORE_FILES) {
            return make_error(ErrorCode::PlatformUnavailable, "recent-documents: enumerating the Recent folder failed");
        }
        // 目录里没有任何快捷方式：本次无可撤销，账目按下方 `keep` 原样留着——壳的写出晚于投递（见头注），
        // 当场销账会把那条迟到的记录变成无主残留，留账才认得它。
    } else {
        do {
            const std::wstring link_path = folder + L"\\" + find_data.cFileName;
            std::wstring target;
            resolve_link_target(link_path, target);
            if (target.empty()) {
                continue;  // 解析不出目标即无从判断归属：宁可不删，也不误删别的应用的记录
            }
            const auto target_utf8 = ascii_lower(internal::wstr_to_utf8(target.c_str()));
            for (std::size_t index = 0; index < recorded; ++index) {
                if (ascii_lower(self.projected[index]) != target_utf8) {
                    continue;  // 目标不是本进程投影的那条：那是别的应用的贡献，不动
                }
                if (DeleteFileW(link_path.c_str()) != 0) {
                    keep[index] = 0;  // 确认删除成功：这一条销账
                    SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, link_path.c_str(), nullptr);
                    ++removed;
                } else {
                    ++failed;  // 删除失败（只读 / 占用）：条目仍在 OS 上，账不能销
                }
                break;
            }
        } while (FindNextFileW(search, &find_data) != 0);
        FindClose(search);
    }
    // 记账收敛：**只销「确认删掉了」的那几条**，其余（含本次没找到对应快捷方式的）一律留账。
    // 留账的理由是实测到的壳侧时序：`SHAddToRecentDocs` 返回后快捷方式可能**稍后**才落盘（投递与
    // 写出解耦）。据此「此刻没有」不等于「永远不会出现」，当场销账会把那条迟到的记录变成无主残留；
    // 留账则让下一次撤销仍认得了它。代价是多留几条无从兑现的账（上限受 store 容量约束），可接受。
    std::vector<std::string> retained;
    for (std::size_t index = 0; index < recorded; ++index) {
        if (keep[index] != 0) {
            retained.emplace_back(self.projected[index]);
        }
    }
    self.projected = std::move(retained);
    if (failed > 0) {
        AURORA_LOG_WARN("recent-documents", "revoked ", removed, " entries but ", failed, " could not be deleted");
    } else {
        AURORA_LOG_DEBUG("recent-documents", "revoked ", removed, " OS recent-document entries");
    }
    return Result<void>{};
}

}  // namespace

auto win32_recent_documents_project(const std::vector<std::string> &plan) -> Result<void> {
    return project_backend(plan);
}

auto win32_recent_documents_clear() -> Result<void> { return clear_backend(); }

auto win32_recent_documents_projected_count() -> std::size_t { return state().projected.size(); }

Win32RecentDocumentsBridge::Win32RecentDocumentsBridge() {
    auto &self = state();
    if (self.refs == 0) {
        recent_documents::register_projection_backend(recent_documents::ProjectionBackend{
            .max_entries = AURORA_WIN32_RECENT_DOCUMENTS_MAX_ENTRIES,
            .project = &project_backend,
            .clear = &clear_backend,
        });
    }
    ++self.refs;
    installed_ = true;
}

auto Win32RecentDocumentsBridge::detach() -> void {
    if (!installed_) {
        return;  // 幂等：窗口销毁路径显式调用与析构调用共用一条出口
    }
    installed_ = false;
    auto &self = state();
    self.refs = self.refs > 0 ? self.refs - 1 : 0;
    if (self.refs == 0) {
        recent_documents::unregister_projection_backend();
    }
}

Win32RecentDocumentsBridge::~Win32RecentDocumentsBridge() { detach(); }

auto win32_recent_documents_ensure_registered() -> bool {
    // 进程级长存守卫：登记一次即不再释放（投影记录与通路都与窗口无关）。
    static Win32RecentDocumentsBridge bootstrap;
    return bootstrap.installed();
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11 || GPU_WGPU)
