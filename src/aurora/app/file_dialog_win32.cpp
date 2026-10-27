// file_dialog_win32.cpp — 文件对话框的 Windows 实现（IFileOpenDialog / IFileSaveDialog /
// 文件夹选择器），COM 驱动。非 AURORA_PLATFORM_WINDOWS 平台本文件不含任何定义（实现在 file_dialog.h 内联回退）。
//
// 注意：_WIN32_WINNT / _WIN32_IE 必须在任何 aurora 头文件之前定义，否则 log.h 等会先行引入
// windows.h 并锁死 NTDDI_VERSION，导致 IFileOpenDialog / SHCreateItemFromParsingName 等
// Vista+ 声明不可见。
//
// _WIN32_IE 取 0x0700 是 `SHCreateItemFromParsingName`（`initial_dir` 的预选通路）声明所需：
// 该原型在 SDK 里由 `_WIN32_IE >= _WIN32_IE_IE70` 门控，取 0x0600 时声明不可见。实链接的
// `shell32` 已随库 PUBLIC 链接（见 cmake/AuroraBackends.cmake），故抬旋钮只是让声明可见。
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
#define _WIN32_IE 0x0700
#endif
#define WIN32_LEAN_AND_MEAN  // NOLINT(*-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <shobjidl.h>
#include <windows.h>

#include <string>
#include <vector>

#include "aurora/app/detail/file_dialog_plan.h"
#include "aurora/app/detail/file_dialog_win32_apply.h"
#include "aurora/app/file_dialog.h"
#include "aurora/core/log.h"
#include "aurora/core/utf8.h"

namespace aurora::file_dialog {
namespace {

// RAII COM 初始化：优先 STA，遇到进程已为 MTA 时回退 MTA；仅在本作用域首次初始化时才 Uninitialize。
struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {
        if (hr == RPC_E_CHANGED_MODE) {
            hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        }
    }
    ~ComInit() {
        if (hr == S_OK || hr == S_FALSE) {
            CoUninitialize();
        }
    }
    explicit operator bool() const { return SUCCEEDED(hr); }

    ComInit(const ComInit &) = delete;
    auto operator=(const ComInit &) -> ComInit & = delete;
    ComInit(ComInit &&) = delete;
    auto operator=(ComInit &&) -> ComInit & = delete;
};

// 把筛选器列表写入对话框；names/specs 须保持存活（COMDLG_FILTERSPEC 只存指针）。
void apply_filters(IFileDialog *dlg, const std::vector<Filter> &filters) {
    if (filters.empty()) {
        return;
    }
    std::vector<std::wstring> names;
    std::vector<std::wstring> specs;
    std::vector<COMDLG_FILTERSPEC> fspecs;
    fspecs.reserve(filters.size());
    for (const auto &f : filters) {
        names.push_back(aurora::internal::utf8_to_wstr(f.name));
        std::wstring joined;
        for (size_t i = 0; i < f.extensions.size(); ++i) {
            if (i != 0U) {
                joined += L';';
            }
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            joined += aurora::internal::utf8_to_wstr(f.extensions[i]);
        }
        specs.push_back(joined);
        fspecs.push_back({.pszName = names.back().c_str(), .pszSpec = specs.back().c_str()});
    }
    (void)dlg->SetFileTypes(static_cast<UINT>(fspecs.size()), fspecs.data());
}

// 统一：显示对话框；取消/失败时返回空结果（不抛异常）。out 经回调收集路径。
template <typename Dlg, typename Collect>
auto show_dialog(Dlg *dlg, Collect collect) -> std::vector<std::string> {
    const HRESULT hr = dlg->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == S_FALSE) {
        return {};
    }
    if (FAILED(hr)) {
        AURORA_LOG_WARN("file_dialog", "Dialog Show failed or was cancelled");
        return {};
    }
    return collect(dlg);
}

}  // namespace

// ---------------------------------------------------------------------------
// 投递（内部接缝，声明见 detail/file_dialog_win32_apply.h）
// ---------------------------------------------------------------------------

namespace detail {
namespace {

/// @brief `GetFullPathNameW` 归一一串宽字符路径（正斜杠转反斜杠、相对转绝对、收掉 `.` / `..`
///        与尾分隔符）；解析名 API 不接受正斜杠形式，故投递前必经这一步。
/// @param path 待归一的宽字符路径。
/// @return 归一结果；调用失败或长度异常时为空串（调用方据此留痕降级）。
[[nodiscard]] auto normalize_wide_path(const std::wstring &path) -> std::wstring {
    std::wstring buffer(path.size() + 4U, L'\0');
    // 首调用返回写入字符数（不含终止符）；缓冲区不足时返回「所需字符数（含终止符）」，据此重试一次。
    DWORD need = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (need == 0U) {
        return {};
    }
    if (need >= buffer.size()) {
        buffer.resize(need);
        need = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (need == 0U || need >= buffer.size()) {
            return {};
        }
    }
    buffer.resize(need);
    return buffer;
}

}  // namespace

void apply_initial_dir(void *dialog, const Options &opts) {
    if (dialog == nullptr || opts.initial_dir.empty()) {
        return;  // 平台不符 / 未创建成功 / 不表达意图：停在 OS 默认位置，不留痕
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 头侧类型擦除的还原点
    auto *const dlg = reinterpret_cast<IFileDialog *>(dialog);
    const std::wstring wide = aurora::internal::utf8_to_wstr(opts.initial_dir);
    if (wide.empty()) {
        AURORA_LOG_WARN("file_dialog",
                        "initial_dir is not valid UTF-8, keeping the OS default location: ", opts.initial_dir);
        return;
    }
    const std::wstring full = normalize_wide_path(wide);
    if (full.empty()) {
        AURORA_LOG_WARN("file_dialog",
                        "initial_dir cannot be made absolute, keeping the OS default location: ", opts.initial_dir);
        return;
    }
    IShellItem *item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(
        full.c_str(), nullptr, IID_IShellItem,
        reinterpret_cast<void **>(&item));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr) || (item == nullptr)) {
        AURORA_LOG_WARN("file_dialog",
                        "initial_dir cannot be resolved, keeping the OS default location: ", opts.initial_dir,
                        ", hr=", hr);
        return;
    }
    hr = dlg->SetFolder(item);
    item->Release();
    if (FAILED(hr)) {
        AURORA_LOG_WARN("file_dialog",
                        "SetFolder rejected initial_dir, keeping the OS default location: ", opts.initial_dir,
                        ", hr=", hr);
    }
}

void apply_save_name(void *dialog, const Options &opts) {
    if (dialog == nullptr) {
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 头侧类型擦除的还原点
    auto *const dlg = reinterpret_cast<IFileDialog *>(dialog);
    // 名字先过折算层：带目录分隔的串 OS 会直接拒掉，末段才是「名字栏」该放的东西。
    const std::string name = aurora::file_dialog_plan::suggested_file_name(opts.suggested_name);
    if (!name.empty()) {
        const HRESULT hr = dlg->SetFileName(aurora::internal::utf8_to_wstr(name).c_str());
        if (FAILED(hr)) {
            AURORA_LOG_WARN("file_dialog", "SetFileName rejected the suggested name: ", name, ", hr=", hr);
        }
    }
    // 缺省扩展名交 OS 补全（故传不含点的那一份，`with_default_extension` 是自绘腿的形态）。
    const std::string ext = aurora::file_dialog_plan::normalize_extension(opts.default_extension);
    if (!ext.empty()) {
        const HRESULT hr = dlg->SetDefaultExtension(aurora::internal::utf8_to_wstr(ext).c_str());
        if (FAILED(hr)) {
            AURORA_LOG_WARN("file_dialog", "SetDefaultExtension rejected the default extension: ", ext, ", hr=", hr);
        }
    }
}

}  // namespace detail

auto open_file(const Options &opts) -> Result<std::vector<std::string>> {
    headless_last_options = opts;
    if (!headless_open_result.empty()) {
        return headless_open_result;
    }
    if (!interactive) {
        return std::vector<std::string>{};
    }
    ComInit com;
    if (!com) {
        AURORA_LOG_WARN("file_dialog", "CoInitializeEx failed, cannot open file dialog");
        return make_error(ErrorCode::PlatformComInitFailed, "CoInitializeEx failed");
    }
    IFileOpenDialog *pfd = nullptr;
    HRESULT hr =
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                         reinterpret_cast<void **>(&pfd));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr) || (pfd == nullptr)) {
        AURORA_LOG_WARN("file_dialog", "CoCreateInstance(IFileOpenDialog) failed");
        return make_error(ErrorCode::PlatformDialogCreateFailed, "CoCreateInstance(IFileOpenDialog) failed");
    }
    DWORD flags = 0;
    if (SUCCEEDED(pfd->GetOptions(&flags))) {
        (void)pfd->SetOptions(flags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    }
    if (!opts.title.empty()) {
        (void)pfd->SetTitle(aurora::internal::utf8_to_wstr(opts.title).c_str());
    }
    // initial_dir 经归一 + 解析 + SetFolder 落进对话框；不可解析时留 WARN 并停在 OS 默认位置。
    detail::apply_initial_dir(pfd, opts);
    apply_filters(pfd, opts.filters);
    auto out = show_dialog(pfd, [](IFileOpenDialog *d) -> std::vector<std::string> {
        IShellItemArray *items = nullptr;
        std::vector<std::string> result;
        if (FAILED(d->GetResults(&items)) || !items) {
            return result;
        }
        DWORD count = 0;
        if (SUCCEEDED(items->GetCount(&count))) {
            for (DWORD i = 0; i < count; ++i) {
                IShellItem *si = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &si)) && si) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        result.push_back(aurora::internal::wstr_to_utf8(path));
                        CoTaskMemFree(path);
                    }
                    si->Release();
                }
            }
        }
        items->Release();
        return result;
    });
    pfd->Release();
    return out;
}

auto save_file(const Options &opts) -> Result<std::string> {
    headless_last_options = opts;
    if (!headless_save_result.empty()) {
        return headless_save_result;
    }
    if (!interactive) {
        return std::string{};
    }
    ComInit com;
    if (!com) {
        AURORA_LOG_WARN("file_dialog", "CoInitializeEx failed, cannot save file dialog");
        return make_error(ErrorCode::PlatformComInitFailed, "CoInitializeEx failed");
    }
    IFileSaveDialog *pfs = nullptr;
    HRESULT hr =
        CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog,
                         reinterpret_cast<void **>(&pfs));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr) || (pfs == nullptr)) {
        AURORA_LOG_WARN("file_dialog", "CoCreateInstance(IFileSaveDialog) failed");
        return make_error(ErrorCode::PlatformDialogCreateFailed, "CoCreateInstance(IFileSaveDialog) failed");
    }
    DWORD flags = 0;
    if (SUCCEEDED(pfs->GetOptions(&flags))) {
        (void)pfs->SetOptions(flags | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
    }
    if (!opts.title.empty()) {
        (void)pfs->SetTitle(aurora::internal::utf8_to_wstr(opts.title).c_str());
    }
    detail::apply_initial_dir(pfs, opts);
    // 本入口是 suggested_name / default_extension 的唯一消费点（打开态预选名字与「选一个已存在的
    // 文件」相互冲突，见 app/file_dialog.h 的消费口径注释）。
    detail::apply_save_name(pfs, opts);
    apply_filters(pfs, opts.filters);
    auto out = show_dialog(pfs, [](IFileDialog *d) -> std::vector<std::string> {
        IShellItem *result = nullptr;
        std::string path;
        if (SUCCEEDED(d->GetResult(&result)) && result) {
            PWSTR p = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                path = aurora::internal::wstr_to_utf8(p);
                CoTaskMemFree(p);
            }
            result->Release();
        }
        return std::vector<std::string>{path};
    });
    pfs->Release();
    return out.empty() ? std::string{} : out.front();
}

auto open_folder(const Options &opts) -> Result<std::string> {
    headless_last_options = opts;
    if (!headless_folder_result.empty()) {
        return headless_folder_result;
    }
    if (!interactive) {
        return std::string{};
    }
    ComInit com;
    if (!com) {
        AURORA_LOG_WARN("file_dialog", "CoInitializeEx failed, cannot select folder");
        return make_error(ErrorCode::PlatformComInitFailed, "CoInitializeEx failed");
    }
    IFileOpenDialog *pfd = nullptr;
    const HRESULT hr =
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                         reinterpret_cast<void **>(&pfd));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr) || (pfd == nullptr)) {
        AURORA_LOG_WARN("file_dialog", "CoCreateInstance(IFileOpenDialog) failed");
        return make_error(ErrorCode::PlatformDialogCreateFailed, "CoCreateInstance(IFileOpenDialog) failed");
    }
    DWORD flags = 0;
    if (SUCCEEDED(pfd->GetOptions(&flags))) {
        (void)pfd->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    }
    if (!opts.title.empty()) {
        (void)pfd->SetTitle(aurora::internal::utf8_to_wstr(opts.title).c_str());
    }
    detail::apply_initial_dir(pfd, opts);
    auto out = show_dialog(pfd, [](IFileDialog *d) -> std::vector<std::string> {
        IShellItem *result = nullptr;
        std::string path;
        if (SUCCEEDED(d->GetResult(&result)) && result) {
            PWSTR p = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                path = aurora::internal::wstr_to_utf8(p);
                CoTaskMemFree(p);
            }
            result->Release();
        }
        return std::vector<std::string>{path};
    });
    pfd->Release();
    return out.empty() ? std::string{} : out.front();
}

}  // namespace aurora::file_dialog

#endif  // AURORA_PLATFORM_WINDOWS