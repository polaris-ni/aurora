// file_dialog_win32.cpp — 文件对话框的 Windows 实现（IFileOpenDialog / IFileSaveDialog /
// 文件夹选择器），COM 驱动。非 AURORA_PLATFORM_WINDOWS 平台本文件不含任何定义（实现在 file_dialog.h 内联回退）。
//
// 注意：_WIN32_WINNT / _WIN32_IE 必须在任何 aurora 头文件之前定义，否则 log.h 等会先行引入
// windows.h 并锁死 NTDDI_VERSION，导致 IFileOpenDialog / SHCreateItemFromParsingName 等
// Vista+ 声明不可见。
#include "aurora/core/platform.h"
#ifdef AURORA_PLATFORM_WINDOWS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Vista+：IFileOpenDialog / IFileSaveDialog
#endif
#ifndef _WIN32_IE
#define WIN32_IE 0x0600 // NOLINT(cppcoreguidelines-macro-usage, readability-identifier-naming): Windows SDK 版本宏
#endif
#define WIN32_LEAN_AND_MEAN // NOLINT(readability-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <shobjidl.h>
#include <string>
#include <windows.h>

#include "aurora/app/file_dialog.h"
#include "aurora/core/log.h"
#include "aurora/core/utf8.h" // internal::utf8_to_wstr / wstr_to_utf8（收口 dup-1 重复实现）

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
            if (i != 0u) {
                joined += L';';
            }
            joined += aurora::internal::utf8_to_wstr(f.extensions[i]);
        }
        specs.push_back(joined);
        fspecs.push_back({ .pszName = names.back().c_str(), .pszSpec = specs.back().c_str() });
    }
    dlg->SetFileTypes(static_cast<UINT>(fspecs.size()), fspecs.data());
}

// 统一：显示对话框；取消/失败时返回空结果（不抛异常）。out 经回调收集路径。
template<typename Dlg, typename Collect> auto show_dialog(Dlg *dlg, Collect collect) -> std::vector<std::string> {
    const HRESULT hr = dlg->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == S_FALSE) {
        return {};
    }
    if (FAILED(hr)) {
        AURORA_LOG_WARN("file_dialog", "对话框 Show 失败或被取消");
        return {};
    }
    return collect(dlg);
}

} // namespace
// NOLINTNEXTLINE(readability-function-cognitive-complexity): 对话框结果收集分支较多，复杂度 26 略超阈值 25
auto open_file(const Options &opts) -> Result<std::vector<std::string>> {
    if (!headless_open_result.empty()) {
        return headless_open_result;
    }
    if (!interactive) {
        return std::vector<std::string>{};
    }
    ComInit com;
    if (!com) {
        AURORA_LOG_WARN("file_dialog", "CoInitializeEx 失败，无法打开文件对话框");
        return make_error(ErrorCode::PlatformComInitFailed, "CoInitializeEx failed");
    }
    IFileOpenDialog *pfd = nullptr;
    HRESULT hr =
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                         reinterpret_cast<void **>(&pfd)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr) || (pfd == nullptr)) {
        AURORA_LOG_WARN("file_dialog", "CoCreateInstance(IFileOpenDialog) 失败");
        return make_error(ErrorCode::PlatformDialogCreateFailed, "CoCreateInstance(IFileOpenDialog) failed");
    }
    DWORD flags = 0;
    if (SUCCEEDED(pfd->GetOptions(&flags))) {
        pfd->SetOptions(flags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    }
    if (!opts.title.empty()) {
        pfd->SetTitle(aurora::internal::utf8_to_wstr(opts.title).c_str());
    }
    // 注：initial_dir 预选目录需要 Vista+ 的 SHCreateItemFromParsingName，当前 MinGW 工具链未提供，
    // 故暂不接线（Options 字段保留，向后兼容）；后续可改用 IShellFolder 桌面目录解析。
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
    if (!headless_save_result.empty()) {
        return headless_save_result;
    }
    if (!interactive) {
        return std::string{};
    }
    ComInit com;
    if (!com) {
        AURORA_LOG_WARN("file_dialog", "CoInitializeEx 失败，无法保存文件对话框");
        return make_error(ErrorCode::PlatformComInitFailed, "CoInitializeEx failed");
    }
    IFileSaveDialog *pfs = nullptr;
    HRESULT hr =
        CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog,
                         reinterpret_cast<void **>(&pfs)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr) || (pfs == nullptr)) {
        AURORA_LOG_WARN("file_dialog", "CoCreateInstance(IFileSaveDialog) 失败");
        return make_error(ErrorCode::PlatformDialogCreateFailed, "CoCreateInstance(IFileSaveDialog) failed");
    }
    DWORD flags = 0;
    if (SUCCEEDED(pfs->GetOptions(&flags))) {
        pfs->SetOptions(flags | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
    }
    if (!opts.title.empty()) {
        pfs->SetTitle(aurora::internal::utf8_to_wstr(opts.title).c_str());
    }
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
        return std::vector<std::string>{ path };
    });
    pfs->Release();
    return out.empty() ? std::string{} : out.front();
}

auto open_folder(const Options &opts) -> Result<std::string> {
    if (!headless_folder_result.empty()) {
        return headless_folder_result;
    }
    if (!interactive) {
        return std::string{};
    }
    ComInit com;
    if (!com) {
        AURORA_LOG_WARN("file_dialog", "CoInitializeEx 失败，无法选择文件夹");
        return make_error(ErrorCode::PlatformComInitFailed, "CoInitializeEx failed");
    }
    IFileOpenDialog *pfd = nullptr;
    HRESULT hr =
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                         reinterpret_cast<void **>(&pfd)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr) || (pfd == nullptr)) {
        AURORA_LOG_WARN("file_dialog", "CoCreateInstance(IFileOpenDialog) 失败");
        return make_error(ErrorCode::PlatformDialogCreateFailed, "CoCreateInstance(IFileOpenDialog) failed");
    }
    DWORD flags = 0;
    if (SUCCEEDED(pfd->GetOptions(&flags))) {
        pfd->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    }
    if (!opts.title.empty()) {
        pfd->SetTitle(aurora::internal::utf8_to_wstr(opts.title).c_str());
    }
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
        return std::vector<std::string>{ path };
    });
    pfd->Release();
    return out.empty() ? std::string{} : out.front();
}

} // namespace aurora::file_dialog

#endif // AURORA_PLATFORM_WINDOWS
