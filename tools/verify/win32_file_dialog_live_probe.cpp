// tools/verify/win32_file_dialog_live_probe.cpp — 文件对话框 Win32 腿投递真机验收探针（非 CTest）。
//
// 覆盖：`aurora::file_dialog::detail::apply_initial_dir()` / `apply_save_name()`
// （src/aurora/app/detail/file_dialog_win32_apply.h，被 `src/aurora/app/file_dialog_win32.cpp`
// 的三个公共入口逐格调用）落到真实 COM 对话框对象上的三处 OS 接线——`IFileDialog::SetFolder`
// （初始目录）、`SetFileName`（建议文件名）、`SetDefaultExtension`（缺省扩展名）。
//
// 为什么必须有本探针（无头 CI 证明不了的部分）：
//   * 折算层（`file_dialog_plan`）能逐值证「该投出去的字符串长什么样」，但**投出去之后 OS 怎么理解**
//     它管不着：`SetDefaultExtension` 要的是不含点的扩展名（给了 `.txt` 会落成 `name..txt`）、
//     `SHCreateItemFromParsingName` 不接受正斜杠（`C:/Windows` 直传回 `E_INVALIDARG`）——这两条都是
//     只有真实 shell 才能证伪的事实；
//   * `Options` 的四格字符串经 `utf8_to_wstr`（`CP_UTF8`）转宽串，非 ASCII 名是否**逐字节**到达名字栏，
//     在无头腿上无从读回（本机活动码页 936，任何一步走 ANSI 都会把 `E4 B8 AD` 折成花码）；
//   * 「解析不了就降级并留 WARN、不返回 Error」这条失败分支要求真实调用 `SetFolder` 才走得到。
//
// 取证口径（关键：证据链**独立于**被测代码）：判据一律取 OS 自己的读回面——`GetFileName` /
//   `GetFolder` + `IShellItem::GetDisplayName(SIGDN_FILESYSPATH)`，不读库内折算结果来自证。库内的
//   `file_dialog_plan` 只用来给出**期望值**，两侧同源字符串一旦在 OS 读回面不一致，就是投递侧的缺陷。
//   唯一取不到读回面的是缺省扩展名：SDK 只声明 `SetDefaultExtension`，**没有** `GetDefaultExtension`
//   （实测 MinGW `x86_64-w64-mingw32/include/shobjidl.h`——IFileDialog / IFileSaveDialog /
//   IFileOpenDialog / IFileDialog2 四个接口各只有 setter）。该腿改用两条独立证据替代（判据 3），
//   不在自动段伪证。
//
// 探针**不调 `Show()`**（自动段）：不弹框、不等用户，只在真实对话框对象上投递并读回。
//   `--interactive` 人工段才真弹一次，由人核对名字栏预填与初始目录。
//
// 构建（方式 ① 推荐）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_file_dialog
//   build-verify\aurora_verify_win32_file_dialog.exe [--interactive]
//
// 自动段（无人值守，逐项判据）：
//   1. COM 可用：`CoInitializeEx` 成功（否则按退出码 2 收尾——环境不可用，不是库的缺陷）。
//   2. 建议名投递：`apply_save_name` 收到 `suggested_name = "dir/sub/report.txt"` ⇒
//      `GetFileName` 读回 `report.txt`（末段，且 OS 没拒）。
//   3. 缺省扩展名投递——**自动段无判据**（OS 无读回面，见取证口径；自动段只如实打印 NOTE 行，不伪证），
//      落点改两条独立证据：折算层单测 `tests/unit/utest_file_dialog_plan.cpp` 逐值证投出去的是不含
//      前导点的 `txt`；人工段把 `suggested_name = "aurora-verify-name"`（无点）+ `default_extension =
//      ".txt"` 投进真对话框、由人按保存，机判 `GetResult` 路径末段**恰为** `aurora-verify-name.txt`——
//      落成无后缀说明扩展名没生效，落成 `..txt` 说明前导点没削掉，一个判据同时抓住两种缺陷。
//   4. 空意图零投递：`Options{}` ⇒ `GetFileName` 读回空串，且不落任何 WARN
//      （证「不表达意图」没有被折成「定成空值再投一次」）。
//   5. 非 ASCII 逐字节往返：`suggested_name` 为转义写死的 UTF-8 中文串 ⇒ `GetFileName` 读回后经
//      `wstr_to_utf8` 还原，与入参**逐字节相等**（不是「看起来能用」）。
//   6. 初始目录投递：`initial_dir = "C:/Windows/system32"`（正斜杠写法）⇒ `GetFolder` 读回的
//      文件系统路径 = `C:\Windows\System32`（大小写不敏感比较；正斜杠不经归一会被 OS 直接拒掉）。
//   7. 解析不了的目录走降级：`initial_dir` 指向不存在的路径 ⇒ 不崩、不返回 Error、`GetFolder` 不落
//      在该路径上，且日志里有一条可归因 WARN（含原文）。
//   8. 另两枚入口共用同一条投递：在 `IFileOpenDialog` 上调 `apply_initial_dir` ⇒ `GetFolder` 同样命中
//      （三入口接的是同一处函数，不是各写一遍）。
//   9. 名字栏与目录可共存：同一对象上先投目录再投名字，两处读回互不覆盖。
// 人工段（`--interactive`）：真弹一次保存对话框（初始目录 = `%LOCALAPPDATA%\aurora_verify` 暂存目录），
//   人核对「标题 / 打开时所在目录 / 名字栏预填」三项后按 y/n，再**按保存**——`GetResult` 返回的路径末段
//   就是判据 3 的机器取证面。对话框只返回选定路径、**不写文件**（实测确认后暂存目录里始终没有该文件），
//   故收尾只摘探针自建的空目录。自动段证的是 OS 记账值，「用户看得见预填的那一行」只有眼睛能证。
//
// 退出码：
//   0  自动段全部通过（未跑人工段时人工项如实打印为待确认）
//   1  自动段断言失败
//   2  环境不可用（COM 初始化失败 / 对话框创建失败 / 暂存目录建不出来）
//   7  人工段存在未确认项（含判据 3 的返回文件名机判失败）

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_file_dialog can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#if !defined(AURORA_BACKEND_WIN32) && !defined(AURORA_BACKEND_D3D11)
#error "AURORA_BACKEND_WIN32 or AURORA_BACKEND_D3D11 must be enabled for aurora_verify_win32_file_dialog"
#endif

// 与库内同口径：先在**任何**平台头之前定好版本旋钮与 min/max 宏（`SHCreateItemFromParsingName`
// 的声明由 `_WIN32_IE >= _WIN32_IE_IE70` 门控，口径须与被测 TU 一致，否则探针连被测 API 都拿不到）。
#ifndef _WIN32_WINNT
// NOLINTNEXTLINE(*-macro-usage, *-reserved-identifier, *-identifier-naming): Windows SDK 版本旋钮
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
// NOLINTNEXTLINE(*-macro-usage, *-reserved-identifier, *-identifier-naming): Windows SDK 版本旋钮
#define _WIN32_IE 0x0700
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <objbase.h>   // CoInitializeEx / CoCreateInstance
#include <shlobj.h>    // SHGetFolderPathW / CSIDL_LOCAL_APPDATA
#include <shobjidl.h>  // IFileDialog / IFileOpenDialog / IFileSaveDialog
#include <windows.h>   // CreateDirectoryW / RemoveDirectoryW
// clang-format on

#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/detail/file_dialog_win32_apply.h"
#include "aurora/app/file_dialog.h"
#include "aurora/core/utf8.h"
#include "verify_args.h"

namespace {

/// @brief 累计的断言失败条数（自动段与人工段共用），决定退出码 1。
auto failures() -> int & {
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers): 探针内单例计数，函数级 static 即可
    static int count = 0;
    return count;
}

/// @brief 自动段的「环境不可用」标记：置位后主流程按退出码 2 收尾而非 1。
auto env_unavailable() -> bool & {
    static bool flag = false;
    return flag;
}

/// @brief 输出一行探针结论到 stdout（经 `AURORA_LOG_RAW`：无日志前缀、不过级别过滤）。
/// @param text 待输出的一行文本（调用方不传行尾换行）。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 记录一条断言结论：打印 PASS / FAIL 行并累计失败数。
/// @param passed 断言是否成立。
/// @param label 断言的人类可读描述（ASCII）。
auto check(bool passed, const std::string &label) -> void {
    emit(std::string("[") + (passed ? "PASS" : "FAIL") + "] " + label);
    if (!passed) {
        ++failures();
    }
}

/// @brief 记一条环境不可用并说明补救方式，随后由主流程按退出码 2 收尾。
/// @param label 判据描述。
/// @param hint 用户侧补救方式（ASCII）。
auto env_fail(const std::string &label, const std::string &hint) -> void {
    emit(std::string("[ENV ] ") + label);
    emit("Hint: " + hint);
    env_unavailable() = true;
}

/// @brief RAII COM 初始化（探针侧独立一份，不复用库内同名内部类型）。
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
    [[nodiscard]] auto ok() const -> bool { return SUCCEEDED(hr_); }

  private:
    HRESULT hr_ = E_FAIL;
};

/// @brief RAII 日志捕获：接管 Logger sink，统计含给定片段的行数（判据 7 要证「降级留了痕」）。
class CapturedLog {
  public:
    CapturedLog() : level_(aurora::Logger::instance().level()) {
        aurora::Logger::instance().set_sink([this](std::string_view line) -> void { lines.emplace_back(line); });
    }
    ~CapturedLog() {
        auto &logger = aurora::Logger::instance();
        logger.set_level(level_);
        logger.set_sink(nullptr);
    }
    CapturedLog(const CapturedLog &) = delete;
    auto operator=(const CapturedLog &) -> CapturedLog & = delete;
    CapturedLog(CapturedLog &&) = delete;
    auto operator=(CapturedLog &&) -> CapturedLog & = delete;

    /// @brief 含全部给定片段的行数（级别标签 + 分类 + 文案片段）。
    [[nodiscard]] auto count(std::initializer_list<const char *> fragments) const -> std::size_t {
        std::size_t hits = 0;
        for (const std::string &line : lines) {
            bool all = true;
            for (const char *f : fragments) {
                if (line.find(f) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) {
                ++hits;
            }
        }
        return hits;
    }

    std::vector<std::string> lines;  // NOLINT(*-non-private-member-variables-in-classes) 捕获到的日志行

  private:
    aurora::LogLevel level_;
};

/// @brief ASCII 小写副本（Windows 路径大小写不敏感，取证比较按此放宽）。
[[nodiscard]] auto ascii_lower(std::string text) -> std::string {
    for (char &ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

/// @brief 创建一枚真实对话框对象。
/// @param save true = `IFileSaveDialog`（名字栏与缺省扩展名只在保存形态有意义）。
/// @return 成功时返回 `IFileDialog *`（两形态都继承自它）；失败返回 nullptr。
[[nodiscard]] auto create_dialog(bool save) -> IFileDialog * {
    IFileDialog *dlg = nullptr;
    const CLSID &clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IFileDialog,
                                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                                        reinterpret_cast<void **>(&dlg));
    if (FAILED(hr) || (dlg == nullptr)) {
        return nullptr;
    }
    return dlg;
}

/// @brief `GetFileName` 读回（UTF-8 窄串）。
/// @param dlg 对话框对象。
/// @return 名字栏当前值；读回失败为空串。
/// @note 缺省扩展名没有对称的读回面——SDK 只有 `SetDefaultExtension`、没有 `GetDefaultExtension`
///       （实测 MinGW `shobjidl.h` 四个对话框接口各自只声明 setter），故该腿的证据见判据 3。
[[nodiscard]] auto read_file_name(IFileDialog *dlg) -> std::string {
    PWSTR raw = nullptr;
    if (FAILED(dlg->GetFileName(&raw)) || (raw == nullptr)) {
        return {};
    }
    std::string out = aurora::internal::wstr_to_utf8(raw);
    CoTaskMemFree(raw);
    return out;
}

/// @brief `GetFolder` 读回的文件系统路径（UTF-8 窄串）。
/// @param dlg 对话框对象。
/// @return 当前目录；未设 / 非文件系统项 / 读回失败为空串。
[[nodiscard]] auto read_folder(IFileDialog *dlg) -> std::string {
    IShellItem *item = nullptr;
    if (FAILED(dlg->GetFolder(&item)) || (item == nullptr)) {
        return {};
    }
    PWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && (raw != nullptr)) {
        out = aurora::internal::wstr_to_utf8(raw);
        CoTaskMemFree(raw);
    }
    item->Release();
    return out;
}

/// @brief `GetResult` 读回的选定路径（宽字符）——人工段唯一能证「OS 按这份意图把名字定成了什么」的面。
/// @param dlg 已 `Show()` 且用户按下保存的对话框对象。
/// @return 文件系统路径；取消 / 非文件系统项 / 读回失败为空串。
[[nodiscard]] auto read_result_path(IFileDialog *dlg) -> std::wstring {
    IShellItem *item = nullptr;
    if (FAILED(dlg->GetResult(&item)) || (item == nullptr)) {
        return {};
    }
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && (raw != nullptr)) {
        out = raw;
        CoTaskMemFree(raw);
    }
    item->Release();
    return out;
}

/// @brief 路径末段（最后一个 `\\` 或 `/` 之后的那一段；无分隔符时原样返回）。
/// @param path 待取末段的路径。
/// @return 末段（不含分隔符）。
[[nodiscard]] auto last_segment(std::wstring_view path) -> std::wstring {
    const std::size_t sep = path.find_last_of(L"\\/");
    return std::wstring(sep == std::wstring_view::npos ? path : path.substr(sep + 1));
}

/// @brief 建人工段暂存目录 `%LOCALAPPDATA%` 下的 `aurora_verify`——真存一次只允许往这里落。
/// @param out 成功时写入目录宽字符路径。
/// @return 取到 LOCALAPPDATA 且目录已就位（新建成功或本就存在）为 true。
[[nodiscard]] auto make_scratch_dir(std::wstring &out) -> bool {
    wchar_t base[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) {
        return false;
    }
    out = std::wstring(base) + L"\\aurora_verify";
    if (CreateDirectoryW(out.c_str(), nullptr)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/// @brief 自动段：逐格投进真实对话框，再用 OS 自己的读回面判一致性。
/// @return 退出码（0 通过 / 1 断言失败 / 2 环境不可用）。
auto run_auto_stage() -> int {
    ComInit com;
    if (!com.ok()) {
        env_fail("CoInitializeEx failed; no COM dialog available", "run the probe on an interactive desktop session");
        return env_unavailable() ? 2 : 1;
    }

    // 判据 2 / 3 / 9：建议名（带目录分隔）与缺省扩展名投进保存对话框后各自被 OS 认下。
    {
        IFileDialog *dlg = create_dialog(true);
        if (dlg == nullptr) {
            env_fail("CoCreateInstance(IFileSaveDialog) failed", "check shell32 / ole32 availability on this machine");
            return 2;
        }
        CapturedLog log;
        const aurora::file_dialog::Options opts{.suggested_name = "dir/sub/report.txt", .default_extension = ".txt"};
        aurora::file_dialog::detail::apply_save_name(dlg, opts);
        check(read_file_name(dlg) == "report.txt", "SetFileName received the last path segment");
        // 缺省扩展名这一腿 OS **只有 setter、没有 getter**（`GetDefaultExtension` 不存在，见文件头取证
        // 口径），自动段无从读回「OS 记下了什么」。这里只证能证的那半：投出去的那一份被 OS 接受——桥在
        // `SetDefaultExtension` 的 hr 失败时落一条可归因 WARN，零命中即 OS 认下了不含点的 `txt`。
        // 「削点」本身由 `utest_file_dialog_plan` 逐值证；「确认后返回的名字真带 `.txt`」由人工段机判。
        check(log.count({"WRN", "file_dialog", "SetDefaultExtension rejected"}) == 0U,
              "the dot-free extension was accepted by the OS (no rejected WARN)");
        emit("[NOTE ] the extension append is read from the confirmed path, not this object (see --interactive)");

        // 判据 6 + 9：同一对象上再投初始目录，两处读回互不覆盖。
        const aurora::file_dialog::Options with_dir{
            .initial_dir = "C:/Windows/system32", .suggested_name = "dir/sub/report.txt", .default_extension = ".txt"};
        aurora::file_dialog::detail::apply_initial_dir(dlg, with_dir);
        check(ascii_lower(read_folder(dlg)) == ascii_lower("C:\\Windows\\System32"),
              "SetFolder resolved the forward-slash initial_dir");
        check(read_file_name(dlg) == "report.txt", "the folder leg did not clear the name field");
        dlg->Release();
    }

    // 判据 4：空 Options = 不表达意图（不投递、不留痕）。
    {
        IFileDialog *dlg = create_dialog(true);
        if (dlg == nullptr) {
            env_fail("CoCreateInstance(IFileSaveDialog) failed (empty-options leg)", "shell dialog unavailable");
            return 2;
        }
        CapturedLog log;
        const aurora::file_dialog::Options plain;
        aurora::file_dialog::detail::apply_save_name(dlg, plain);
        aurora::file_dialog::detail::apply_initial_dir(dlg, plain);
        check(read_file_name(dlg).empty(), "empty suggested_name leaves the name field empty");
        // 「空 default_extension ⇒ 根本不投」在 OS 侧不可读回（无 getter），落在折算层单测：
        // `normalize_extension("")` 为空 ⇒ 桥的 `if (!ext.empty())` 门不走进去。这里只证留痕面。
        check(log.count({"WRN", "file_dialog"}) == 0U, "no intent produces no WARN line");
        dlg->Release();
    }

    // 判据 5：非 ASCII 名字逐字节往返（转义写死，不经 argv、不经活动码页）。
    {
        IFileDialog *dlg = create_dialog(true);
        if (dlg == nullptr) {
            env_fail("CoCreateInstance(IFileSaveDialog) failed (utf-8 leg)", "shell dialog unavailable");
            return 2;
        }
        const std::string utf8_name = "\xe4\xb8\xad\xe6\x96\x87\xe6\x8a\xa5\xe5\x91\x8a.txt";
        const aurora::file_dialog::Options opts{.title = "\xe5\x8f\xa6\xe5\xad\x98\xe4\xb8\xba",
                                                .suggested_name = "/\xe7\x9b\xae\xe5\xbd\x95/" + utf8_name};
        aurora::file_dialog::detail::apply_save_name(dlg, opts);
        const std::string back = read_file_name(dlg);
        check(back == utf8_name, "SetFileName round-trips the UTF-8 name byte for byte");
        if (back != utf8_name) {
            emit("  expected size=" + std::to_string(utf8_name.size()) + " actual size=" + std::to_string(back.size()));
        }
        dlg->Release();
    }

    // 判据 7：解析不了的目录走降级——不崩、不返回 Error、GetFolder 不落在该路径，且留一条可归因 WARN。
    {
        IFileDialog *dlg = create_dialog(true);
        if (dlg == nullptr) {
            env_fail("CoCreateInstance(IFileSaveDialog) failed (fallback leg)", "shell dialog unavailable");
            return 2;
        }
        CapturedLog log;
        const std::string bogus = "C:/aurora-verify/does-not-exist";
        const aurora::file_dialog::Options opts{.initial_dir = bogus};
        aurora::file_dialog::detail::apply_initial_dir(dlg, opts);
        check(ascii_lower(read_folder(dlg)).find("does-not-exist") == std::string::npos,
              "an unresolvable initial_dir is not adopted as the dialog folder");
        check(log.count({"WRN", "file_dialog", "initial_dir cannot be resolved"}) == 1U,
              "the degrade branch leaves exactly one attributable WARN");
        dlg->Release();
    }

    // 判据 8：另两枚入口共用同一条投递（在 IFileOpenDialog 上投初始目录同样命中）。
    {
        IFileDialog *dlg = create_dialog(false);
        if (dlg == nullptr) {
            env_fail("CoCreateInstance(IFileOpenDialog) failed", "shell dialog unavailable");
            return 2;
        }
        const aurora::file_dialog::Options opts{.initial_dir = "C:/Windows/system32"};
        aurora::file_dialog::detail::apply_initial_dir(dlg, opts);
        check(ascii_lower(read_folder(dlg)) == ascii_lower("C:\\Windows\\System32"),
              "the open dialog shares the same folder leg");
        dlg->Release();
    }

    return failures() == 0 ? (env_unavailable() ? 2 : 0) : 1;
}

/// @brief 人工段：真弹一次保存对话框——人核对预填的三项，再按保存读回 OS 选定的路径。
///
/// `GetResult` 返回的路径末段是**判据 3 唯一的机器取证面**：SDK 只有 `SetDefaultExtension`、没有
/// getter，对话框对象上读不回「缺省扩展名被记成了什么」，只有走一次确认才知道 OS 有没有把扩展名补上。
/// 故建议名**不带点**，返回名字里的 `.txt` 才只可能来自这一腿——落成无后缀是没生效，落成 `..txt` 是
/// 前导点没削掉。实测确认后**没有文件被写出**（`IFileDialog` 只返回选定路径，落盘归应用），暂存目录
/// 只用来接住位置、收尾按空目录摘掉；返回路径落在别处时一律不动。
/// @return 退出码（0 机判通过且三项全确认 / 1 机判失败 / 2 暂存目录不可用 / 7 存在未确认项）。
auto run_interactive_stage() -> int {
    ComInit com;
    if (!com.ok()) {
        emit("[ENV ] interactive stage needs a COM apartment; skipping");
        return 7;
    }
    std::wstring scratch;
    if (!make_scratch_dir(scratch)) {
        emit("[ENV ] interactive stage could not prepare the scratch dir under %LOCALAPPDATA%");
        emit("Hint: check that LOCALAPPDATA resolves and is writable, then re-run with --interactive");
        return 2;
    }
    IFileDialog *dlg = create_dialog(true);
    if (dlg == nullptr) {
        emit("[ENV ] interactive stage could not create the save dialog; skipping");
        return 7;
    }
    const std::string scratch_utf8 = aurora::internal::wstr_to_utf8(scratch.c_str());
    const aurora::file_dialog::Options opts{.title = "Aurora verify: save as",
                                            .initial_dir = scratch_utf8,
                                            .suggested_name = "aurora-verify-name",
                                            .default_extension = ".txt"};
    (void)dlg->SetTitle(aurora::internal::utf8_to_wstr(opts.title).c_str());
    aurora::file_dialog::detail::apply_initial_dir(dlg, opts);
    aurora::file_dialog::detail::apply_save_name(dlg, opts);

    emit("A real save dialog is about to open inside the scratch dir printed below.");
    emit("Check three things, then press SAVE as-is (do not type an extension, do not change the folder):");
    emit("  1. the title reads: Aurora verify: save as");
    emit("  2. it opens inside: " + scratch_utf8);
    emit("  3. the file name field is prefilled with: aurora-verify-name (no extension)");
    const HRESULT hr = dlg->Show(nullptr);
    emit("dialog closed (hr=" + std::to_string(static_cast<long>(hr)) + ")");

    int unconfirmed = 0;
    const std::wstring saved = read_result_path(dlg);
    if (saved.empty()) {
        emit("[PEND ] the dialog was cancelled; the default-extension leg is only readable from a confirmed save");
        ++unconfirmed;
    } else {
        const std::wstring name = last_segment(saved);
        emit("returned path: " + aurora::internal::wstr_to_utf8(saved.c_str()));
        check(name == L"aurora-verify-name.txt",
              "the OS appended the dot-free default extension to the confirmed path");
        // 只回收探针自己建的那一处：人改了位置或改了名字时一律不动。
        const std::wstring parent = saved.substr(0, saved.find_last_of(L"\\/"));
        const bool in_scratch =
            ascii_lower(aurora::internal::wstr_to_utf8(parent.c_str())) == ascii_lower(scratch_utf8);
        const std::string saved_name = aurora::internal::wstr_to_utf8(name.c_str());
        const bool ours = in_scratch && (saved_name.rfind("aurora-verify-name", 0) == 0);
        if (!ours) {
            emit("[SKIP ] saved outside the scratch dir or under an unexpected name; leaving it alone");
        } else {
            // `IFileDialog` **不写文件**：确认后只返回选定的路径，落盘由应用自己完成（实测本段返回
            // 路径后目录里始终没有该文件，`GetFileAttributesW` 报 error=2）。故这里没有文件要回收，
            // 只摘掉探针自建的那枚空暂存目录；目录非空（人往里放了东西）就留着不动。
            emit(RemoveDirectoryW(scratch.c_str()) ? "cleanup: scratch dir removed"
                                                   : "cleanup: scratch dir kept (not empty; nothing was deleted)");
        }
    }

    for (const char *question :
         {"Did the title read as printed above? (y/n)", "Did it open inside the scratch dir printed above? (y/n)",
          "Was the name field prefilled with aurora-verify-name? (y/n)"}) {
        emit(std::string(question));
        std::string answer;
        // 只有明确答 y 才算确认：EOF / 空答 / 其它字符一律计入未确认项（退出码 7）。
        if (!std::getline(std::cin, answer) || answer.empty() || (answer[0] != 'y')) {
            ++unconfirmed;
        }
    }
    dlg->Release();
    if (failures() != 0) {
        return 1;  // 返回文件名的机判失败：这是缺陷，不是「没确认」
    }
    if (unconfirmed != 0) {
        emit("[PEND ] " + std::to_string(unconfirmed) + " interactive item(s) left unconfirmed");
        return 7;
    }
    return 0;
}

}  // namespace

auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Win32 file-dialog delivery live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    aurora::init_console();  // Windows 控制台切 UTF-8，避免路径行错行

    emit("== Aurora Win32 file-dialog delivery live probe ==");
    emit("auto segment: name / folder read back via the OS getters, extension via the confirmed path");

    const int auto_code = run_auto_stage();
    if (auto_code != 0) {
        return auto_code;
    }
    if (!interactive) {
        emit("Skipping the interactive stage.");
        emit("The default-extension append is only readable from a confirmed path, and whether the");
        emit("prefilled name shows on screen is a human judgement. Re-run with --interactive for both.");
        emit("PASS: Win32 file-dialog delivery acceptance passed (auto stage)");
        return 0;
    }
    const int human_code = run_interactive_stage();
    if (human_code == 0) {
        emit("PASS: Win32 file-dialog delivery acceptance passed (auto + interactive)");
    }
    return human_code;
}

// ---------------------------------------------------------------------------
// 已知差距（供后续任务决策，本探针不掩盖）
//   1. 「投进去的字符串被 OS 记下了」与「用户确认后返回的路径就是它」是两段：前者自动段用 OS getters
//      证，后者只在走一次确认后可见，故压在 `--interactive` 段。`IFileDialog` 只返回选定路径、不写
//      文件（实测确认后暂存目录里空着），所以这段不往磁盘留东西，收尾只摘那枚自建空目录。
//   2. `GetFolder` 的读回值在从未设置过时是壳给的默认位置（常是「上次使用」或用户目录），因此判据 7
//      写成「不落在坏路径上」而不是「等于空」——后者会把壳自己的默认策略当成库的缺陷。
//   3. 缺省扩展名**没有 getter**（SDK 只声明 `SetDefaultExtension`，四个对话框接口皆然），自动段无从
//      读回，只能证「投出去的那一份被 OS 接受」这半边；「前导点削得对不对」靠确认后 `GetResult`
//      返回的名字，故判据 3 在非交互运行下如实打印为未证，不写成通过。
//   4. 自动段不调 `Show()`，故「对话框真的能开、名字栏真的显示」不在其覆盖内；自动段的读回面是 OS 的
//      记账值，与显示层是否把它画出来之间仍有一段（那段只有眼睛能证）。
// ---------------------------------------------------------------------------
