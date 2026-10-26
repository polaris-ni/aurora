// tools/verify/win32_recent_documents_live_probe.cpp — Win32 OS 级最近文档投影真机验收探针（非 CTest）。
//
// 覆盖：`RecentDocuments::project_to_os()` / `clear_os_projection()`（include/aurora/app/
// recent_documents.h）经 Win32 桥（src/aurora/window/detail/win32_recent_documents.cpp）落到
// 「用户最近使用的项目」的整条接线——`SHAddToRecentDocs(SHARD_PATHW, ...)` 投递、进程级投影记账、
// 撤销侧经 `IShellLinkW` 解析目标后删除本进程贡献的那条 `.lnk`。
//
// 为什么必须有本探针（无头 CI 证明不了的部分）：
//   * 单测只能在**假后端**上验折算计划与失败隔离，它无法证明 `SHAddToRecentDocs` 真的写出了快捷方式；
//   * 「OS 侧出现了条目 / 撤销后条目消失」是 shell 状态，只有读真实 `%APPDATA%\...\Recent` 才证得了；
//   * 用户策略（`Start_TrackDocs=0` 时 API 静默无操作）在无头环境不可判别，须在真机上如实分类。
//
// 取证口径（关键：证据链**独立于**被测桥）：
//   探针自带一份 Recent 目录枚举 + `.lnk` 目标解析（`FindFirstFileW` / `IShellLinkW::GetPath`），
//   判据是「OS 目录里现在有几条快捷方式指向探针那个文件」。它不读桥的内部记账来判 OS 状态——
//   否则「桥记了账但 OS 没落记录」这类缺陷会被自证。桥的内部记账（`win32_recent_documents_projected_count()`）
//   只作为**第二条**判据（记账与 OS 实况是否同步）单独核对。
//
// 构建（方式 ① 推荐）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_recent_documents
//   build-verify\aurora_verify_win32_recent_documents.exe [--interactive]
//
// 运行：命令行直接跑即可。探针**不建窗口**——这本身就是被验的一条事实：投影走的是进程级通路，
//   「先记文档、后建窗口」与「完全没有窗口的进程」同样能投影（`win32_recent_documents_ensure_registered()`
//   的自举路径正由本探针覆盖）。
//
// 自动段（无人值守，逐项判据）：
//   1. 能力位：`recent_documents_os_projection` 在当前 Surface 上为 true（本探针只在 Win32/D3D11 构建）。
//   2. 用户策略：读 `HKCU\...\Explorer\Advanced\Start_TrackDocs`；为 0 时后续 OS 侧判据全部无从成立，
//      如实以退出码 2（环境不可用）收尾并给出开启方法，**不改判 PASS**。
//   3. 探针文件落地：在 `%LOCALAPPDATA%\aurora_verify` 下建一个唯一命名的真实文件并 `RecentDocuments::add`
//      该路径。选址原因：实测壳把 `%TEMP%` 排除在最近文档之外（同一最小实验在 `%LOCALAPPDATA%` 立刻产出
//      快捷方式、在 `%TEMP%` 恒不产出），探针文件建在那儿会让判据在库之外失证；用真实文件则把「路径不
//      存在」这个与投影通路无关的变量排除掉。
//   4. 投影成功：`project_to_os()` 返回 Ok；桥内记账 = 1 条；OS 侧出现且仅出现 1 条指向该文件的快捷方式
//      （= 任务栏跳转列表 / 资源管理器「最近使用」的数据源，即任务书要求的「条目出现」）。
//      ⚠️ 该判据按**有界轮询**读（5s 上限、100ms 一采）：实测 `SHAddToRecentDocs` 返回后快捷方式才由壳
//      写出，同步读必然读到 0 条——那是壳的时序，不是库的缺陷，也不是可以省略的等待。
//   5. 幂等：再次 `project_to_os()` 仍 Ok，记账仍 1 条，静置一窗后 OS 侧仍 1 条（不重复投出第二条同名记录）。
//   6. 应用内列表不受投影影响：`list()` 仍恰为该路径（投影既不裁剪也不回写 store）。
//   7. 撤销：`clear_os_projection()` 返回 Ok；记账回 0 条；OS 侧指向该文件的快捷方式归零
//      （= 任务书要求的「clear 后消失」）。
//   8. 撤销不碰 store：`list()` 仍为该路径——「OS 侧清干净」与「应用内最近列表」是两件事。
//   9. 空列表投影：`clear()` 后 `project_to_os()` 返回 Ok 且 OS 侧仍 0 条（空计划是成功的 no-op，不是错误）。
//  10. 清理：删除探针文件（`.lnk` 已由第 7 步撤销），并核对目录里已无指向残留。
// 人工段（`--interactive`）：打印探针文件路径与 Recent 目录，由人打开资源管理器「快速访问 → 最近使用」
//   （及已固定该应用时的任务栏跳转列表）目视确认条目出现/消失后按 y/n——机器判据证的是目录状态，
//   「UI 真的把它显示出来了」这一环只有眼睛能证。
//
// 退出码：
//   0  自动段全部通过（未跑人工段时人工项如实打印为待确认）
//   1  自动段断言失败
//   2  环境不可用（能力位为假 / 用户关闭了最近项目追踪 / Recent 目录取不到 / 探针文件建不出来）
//   7  人工段存在未确认项

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_recent_documents can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#if !defined(AURORA_BACKEND_WIN32) && !defined(AURORA_BACKEND_D3D11)
#error "AURORA_BACKEND_WIN32 or AURORA_BACKEND_D3D11 must be enabled for aurora_verify_win32_recent_documents"
#endif

// 与库内同口径：先在**任何**平台头之前定好这两个宏，避免 <windows.h> 的 min/max 宏污染标准库用法。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <objbase.h>   // CoInitializeEx / CoCreateInstance
#include <shlobj.h>    // SHGetFolderPathW / CSIDL_RECENT
#include <shobjidl.h>  // IShellLinkW
#include <windows.h>
// clang-format on

#include <iostream>
#include <string>
#include <vector>

#include "aurora/app/recent_documents.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/utf8.h"
#include "aurora/window/detail/win32_recent_documents.h"
#include "aurora/window/platform.h"
#include "verify_args.h"
#include "verify_print.h"

namespace {

/// @brief 累计的断言失败条数（自动段与人工段共用），决定退出码 1。
auto failures() -> int & {
    static int count = 0;
    return count;
}

/// @brief 自动段的「环境不可用」标记：置位后主流程按退出码 2 收尾而非 1。
auto env_unavailable() -> bool & {
    static bool flag = false;
    return flag;
}

// OS 侧快捷方式写出的等待上限（毫秒）。实测迟到在毫秒级，5s 是「足够覆盖 + 不至于像卡死」的折中；
// 超时仍不达标即如实 FAIL，不放宽判据。
constexpr unsigned long long AURORA_OS_SETTLE_TIMEOUT_MS = 5000ULL;
// 有界轮询的间隔（毫秒）：100ms 让最坏情况下的额外延迟远小于迟到量级，又不 busy-poll 抢 CPU。
constexpr unsigned long long AURORA_OS_POLL_SLICE_MS = 100ULL;
// 幂等段观察「第二次投递是否多写一条」的静置窗（毫秒）：取等待上限的五分之一，
// 足够让迟到者现身，又不把整段拖成两次满窗等待。
constexpr unsigned long long AURORA_OS_SETTLE_WINDOW_MS = 1000ULL;

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
/// @param hint 用户侧补救方式（ASCII；环境策略类必须给，否则跑第二次仍是同样结果）。
auto env_fail(const std::string &label, const std::string &hint) -> void {
    emit(std::string("[ENV ] ") + label);
    emit("Hint: " + hint);
    env_unavailable() = true;
}

/// @brief RAII COM 初始化（探针侧独立一份，不复用桥内的同名内部类型）。
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

/// @brief ASCII 小写副本（Windows 路径大小写不敏感，取证比较按此放宽）。
[[nodiscard]] auto ascii_lower(std::string text) -> std::string {
    for (char &ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

/// @brief 取用户 Recent 目录（取证用的独立通道，与桥的 `recent_folder()` 同 API、不同实现）。
/// @param out 成功时写入目录宽字符路径。
/// @return 取到为 true。
[[nodiscard]] auto recent_folder(std::wstring &out) -> bool {
    wchar_t buffer[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_RECENT, nullptr, SHGFP_TYPE_CURRENT, buffer))) {
        return false;
    }
    out = buffer;
    return !out.empty();
}

/// @brief 统计 Recent 目录里目标指向指定文件的快捷方式条数（探针的**主取证面**）。
///
/// 逐条 `IShellLinkW::GetPath` 解析目标后按大小写不敏感比较；解析不出目标的（目标已失效、非文件
/// 链接）不计——它们本就不该被算作「指向探针文件的记录」。计数而非「有无」是判据形态：幂等段要能
/// 区分「仍是一条」与「投出了两条」。
/// @param folder Recent 目录（宽字符）。
/// @param target_utf8 待匹配的目标路径（UTF-8，原样探针路径）。
/// @return 命中的快捷方式条数；COM 初始化失败或目录不可枚举时返回 -1（无从取证）。
auto count_links_targeting(const std::wstring &folder, const std::string &target_utf8) -> int {
    ComInit com;
    if (!com.ok()) {
        return -1;
    }
    const std::string wanted = ascii_lower(target_utf8);
    int hits = 0;
    WIN32_FIND_DATAW find_data = {};
    const std::wstring pattern = folder + L"\\*.lnk";
    const HANDLE search = FindFirstFileW(pattern.c_str(), &find_data);
    if (search == INVALID_HANDLE_VALUE) {
        return 0;  // 目录里没有任何快捷方式：计数为 0，不是取证失败
    }
    do {
        const std::wstring link_path = folder + L"\\" + find_data.cFileName;
        IShellLinkW *link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
            continue;
        }
        IPersistFile *persist = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
            if (SUCCEEDED(persist->Load(link_path.c_str(), STGM_READ))) {
                wchar_t target[MAX_PATH] = {};
                if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, SLGP_UNCPRIORITY)) && target[0] != L'\0') {
                    if (ascii_lower(aurora::internal::wstr_to_utf8(target)) == wanted) {
                        ++hits;
                    }
                }
            }
            persist->Release();
        }
        link->Release();
    } while (FindNextFileW(search, &find_data) != 0);
    FindClose(search);
    return hits;
}

/// @brief OS 侧「最近使用的项目」追踪开关（与桥读同一注册表键，用途不同：分类 SKIP 与 FAIL）。
/// @return 用户关闭追踪时为 true。
[[nodiscard]] auto tracking_disabled_by_user() -> bool {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                     L"Start_TrackDocs", RRF_RT_DWORD, nullptr, &value, &size);
    return SUCCEEDED(status) && value == 0U;
}

/// @brief 在 `%LOCALAPPDATA%\\aurora_verify` 下建探针用的真实文件（唯一命名，免与残留相撞）。
///
/// 选址是实测逼出来的，不是随手挑的：`%TEMP%` 里的文件**不会**被写进最近文档——同一份直连
/// `SHAddToRecentDocs` 的最小实验（无 aurora 参与）对 `%TEMP%` 下的新建文件恒不产出 `.lnk`，
/// 而对 `%LOCALAPPDATA%` 下的文件立刻产出，说明壳自己排除了临时目录。探针若把文件建在 `%TEMP%`，
/// 「OS 侧出现条目」这条判据会在库之外失证。故取一个应用私有、非临时、可清理的目录。
/// @param folder 输出：所在目录（宽字符，不含尾部反斜杠）。
/// @param name 输出：文件名（宽字符）。
/// @return 创建成功为 true。
[[nodiscard]] auto make_probe_file(std::wstring &folder, std::wstring &name) -> bool {
    wchar_t base[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) {
        return false;
    }
    folder = std::wstring(base) + L"\\aurora_verify";
    // 目录不存在时 `SHCreateDirectoryExW` 逐级建出；已存在返回 `ERROR_FILE_EXISTS` 系，一律按可用处理。
    const int made = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS) {
        return false;
    }
    name = L"recent_docs_probe_" + std::to_wstring(GetTickCount64()) + L".txt";
    const HANDLE file = CreateFileW((folder + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    static constexpr char AURORA_PROBE_PAYLOAD[] = "aurora recent documents probe\n";
    DWORD written = 0;
    WriteFile(file, AURORA_PROBE_PAYLOAD, static_cast<DWORD>(sizeof(AURORA_PROBE_PAYLOAD) - 1U), &written, nullptr);
    CloseHandle(file);
    return true;
}

/// @brief 判定一次投影 / 撤销结果是否 Ok，失败时打印错误码与详情。
/// @param label 判据描述前缀。
/// @param result 待判定的结果。
/// @return 结果为 Ok 时为 true。
auto expect_ok(const std::string &label, const aurora::Result<void> &result) -> bool {
    if (result.ok()) {
        return true;
    }
    check(false, label + ": expected Ok, got " + result.error().code + " (" + result.error().message + ")");
    return false;
}

/// @brief 把桥的记账条数与预期值比对，并把实际值写进结论行。
/// @param label 判据描述。
/// @param expected 预期记账条数。
auto check_bookkeeping(const std::string &label, std::size_t expected) -> void {
    const std::size_t actual = aurora::detail::win32_recent_documents_projected_count();
    check(actual == expected, label + ": bridge projection record = " + aurora_verify::format_uint(expected) +
                                  " (got " + aurora_verify::format_uint(actual) + ")");
}

/// @brief 有界轮询 Recent 目录，直到指向探针文件的快捷方式条数达到预期（或超时）。
///
/// 为什么不能「调用后立读」：实测 `SHAddToRecentDocs` **返回后**快捷方式才由壳写出（投递与写出解耦，
/// 迟到量级为毫秒），同步读会读到 0 条——那是壳的时序，不是库的缺陷。判据因此按「有界等待到落定」
/// 构造，超时仍不达标即如实 FAIL。（同一时序事实决定了桥的撤销只销「确认删掉了」的账。）
/// @param folder Recent 目录（宽字符）。
/// @param target_utf8 待匹配的探针文件路径。
/// @param wanted 预期条数：> 0 时等到「至少这么多」，0 时等到「一条也没有」。
/// @return 末次实际计数；-1 表示取证失败（COM 初始化不了）。
auto wait_for_count(const std::wstring &folder, const std::string &target_utf8, int wanted) -> int {
    const unsigned long long deadline = GetTickCount64() + AURORA_OS_SETTLE_TIMEOUT_MS;
    int hits = count_links_targeting(folder, target_utf8);
    while (hits >= 0 && (wanted > 0 ? hits < wanted : hits != 0) && GetTickCount64() < deadline) {
        Sleep(AURORA_OS_POLL_SLICE_MS);
        hits = count_links_targeting(folder, target_utf8);
    }
    return hits;
}

/// @brief 自动段：全部无人值守判据（清单见本文件头注释）。
/// @param folder_out 输出：Recent 目录（供人工段打印）。
/// @param probe_path_utf8 输出：探针文件全路径（UTF-8）。
/// @return 该段的退出码：0 通过；1 断言失败；2 环境不可用。
auto run_auto_stage(std::wstring &folder_out, std::string &probe_path_utf8) -> int {
    emit("---- auto segment ----");

    // 1. 能力位核对。为假说明本腿没接桥（编译宏或 Surface 判定不符），后面的判据都会以
    //    「projection-unsupported」失败收场——那是同一件事的重复报错，故就地按环境不可用收尾。
    const aurora::Platform snapshot = aurora::platform();
    const bool supported = snapshot.capabilities().recent_documents_os_projection;
    check(supported, "platform capability: recent_documents_os_projection is true for this surface");
    if (!supported) {
        env_fail("no OS recent-documents projection for the detected surface",
                 "Build with AURORA_BACKEND_WIN32=ON (or D3D11) and re-run.");
        return 2;
    }

    // 2. 用户策略前置核对。`SHAddToRecentDocs` 在该开关为 0 时静默无操作（API 返回 void、无回执），
    //    桥此时如实返回 platform-unavailable；探针若继续跑判据，报的会是「库有缺陷」的假 FAIL。
    if (tracking_disabled_by_user()) {
        env_fail("Windows is not tracking recent items (Start_TrackDocs=0); no OS-side entry can be produced",
                 "Turn Settings > Privacy > 'Show recently opened items in Jump Lists' ON, or set "
                 "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\\Start_TrackDocs to 1, then "
                 "re-run.");
        return 2;
    }

    // 3. 探针文件 + store 记账。用真实文件是为排除「路径不存在」这个与投影通路无关的变量；
    //    store 侧只认字符串，故这里 add 的是探针自己造的绝对路径。
    std::wstring probe_folder;
    std::wstring probe_name;
    if (!make_probe_file(probe_folder, probe_name)) {
        env_fail("cannot create the probe file under %LOCALAPPDATA%\\aurora_verify",
                 "Check that the local app-data directory is writable.");
        return 2;
    }
    probe_path_utf8 = aurora::internal::wstr_to_utf8((probe_folder + L"\\" + probe_name).c_str());
    if (!recent_folder(folder_out) || folder_out.empty()) {
        env_fail("the user Recent folder is unavailable", "Run inside an interactive desktop session.");
        return 2;
    }
    const std::string folder_utf8 = aurora::internal::wstr_to_utf8(folder_out.c_str());
    emit("probe file : " + probe_path_utf8);
    emit("recent dir : " + folder_utf8);

    auto &store = aurora::RecentDocuments::instance();
    store.clear_all();  // 单例跨段共享：先复位，判据才不受历史状态影响
    store.add(probe_path_utf8);
    check(store.size() == 1U, "store: the probe path is the single recent entry");

    // 4. 投影：OS 侧条目出现（任务书判据 ①）。
    const auto first = store.project_to_os();
    const bool projected = expect_ok("project_to_os (first call)", first);
    check_bookkeeping("after the first projection", 1U);
    const int first_hits = wait_for_count(folder_out, probe_path_utf8, 1);
    check(projected && first_hits == 1,
          "OS evidence: exactly one Recent shortcut targets the probe file after projecting (got " +
              aurora_verify::format_int(static_cast<long long>(first_hits)) + ")");
    if (first_hits < 0) {
        env_fail("cannot read the Recent folder (COM init failed)", "Re-run in a normal desktop session.");
        return 2;
    }

    // 5. 幂等：重复投影不重复落记录（任务书判据 ②）。
    const int before_repeat = count_links_targeting(folder_out, probe_path_utf8);
    const auto repeated = store.project_to_os();
    expect_ok("project_to_os (repeat, idempotency)", repeated);
    check_bookkeeping("after the repeated projection", 1U);
    // 静置一窗再读：第二次投递若真多写一条，迟到者应当在这一窗内现身；立即读会把「还没写」当成
    // 「没有多写」，判据就恒真了。
    Sleep(AURORA_OS_SETTLE_WINDOW_MS);
    const int repeat_hits = count_links_targeting(folder_out, probe_path_utf8);
    check(repeat_hits == before_repeat, "idempotency: repeating the projection adds no second shortcut (before " +
                                            aurora_verify::format_int(static_cast<long long>(before_repeat)) +
                                            ", after " +
                                            aurora_verify::format_int(static_cast<long long>(repeat_hits)) + ")");

    // 6. 投影不回写 store：应用内列表与投影是两件事（任务书「失败隔离 / 不影响应用内列表」口径）。
    const auto listed = store.list();
    check(listed.size() == 1U && listed.front() == probe_path_utf8,
          "store untouched: list() still holds exactly the probe path after projection");

    // 7. 撤销：OS 侧条目消失（任务书判据 ③）。有界等到「一条也没有」，再静置一窗确认没有迟到者
    //    ——撤销删得比壳写得晚时，壳仍可能补写回来，那属于第 5 条已知差距，此处如实报 FAIL。
    const auto cleared = store.clear_os_projection();
    expect_ok("clear_os_projection", cleared);
    const int clear_hits = wait_for_count(folder_out, probe_path_utf8, 0);
    Sleep(AURORA_OS_SETTLE_WINDOW_MS);
    const int clear_final = count_links_targeting(folder_out, probe_path_utf8);
    check(cleared && clear_hits == 0 && clear_final == 0,
          "OS evidence: no Recent shortcut targets the probe file after revoking (settled at " +
              aurora_verify::format_int(static_cast<long long>(clear_final)) + ")");
    check_bookkeeping("after revoking", 0U);

    // 8. 撤销不碰 store：应用内最近列表保留（撤销的是 OS 记录，不是用户的最近文档账目）。
    const auto after_clear = store.list();
    check(after_clear.size() == 1U && after_clear.front() == probe_path_utf8,
          "store untouched: list() still holds the probe path after revoking the OS record");

    // 9. 空列表投影是成功的 no-op，不产生 OS 记录、也不报错。
    store.clear();
    const auto empty = store.project_to_os();
    expect_ok("project_to_os on an empty list", empty);
    check_bookkeeping("after the empty projection", 0U);
    check(count_links_targeting(folder_out, probe_path_utf8) == 0,
          "empty plan: still no shortcut targets the probe file");

    // 10. 清理探针文件（`.lnk` 已由第 7 步撤销；残留时由人自行删除，探针不强改用户 Recent 目录）。
    const auto probe_file_wide = probe_folder + L"\\" + probe_name;
    const bool removed = DeleteFileW(probe_file_wide.c_str()) != 0;
    emit(std::string(removed ? "[INFO] probe file deleted: " : "[WARN] probe file left behind (delete it manually): ") +
         probe_path_utf8);
    const int residue = count_links_targeting(folder_out, probe_path_utf8);
    if (residue > 0) {
        emit("[WARN] " + aurora_verify::format_uint(static_cast<unsigned long long>(residue)) +
             " shortcut(s) still target the probe file; they belong to other applications' records and are not revoked "
             "by "
             "this process's projection record.");
    }

    return failures() > 0 ? 1 : 0;
}

/// @brief 人工段：由人目视核对系统入口的显示与消失（机器判据证的是目录，不是 UI）。
/// @param folder Recent 目录。
/// @return 该段退出码：0 全部确认；7 存在未确认项。
auto run_interactive_stage(const std::wstring &folder, const std::string &probe_path_utf8) -> int {
    emit("---- interactive segment ----");
    const std::string folder_utf8 = aurora::internal::wstr_to_utf8(folder.c_str());
    emit("This stage only a human can settle: the auto segment proves the Recent folder, the");
    emit("Explorer 'Quick access' / 'Recent items' view and the taskbar Jump List are one layer");
    emit("above it. Re-run is needed to re-project, so the entries are already revoked here.");
    emit("Recent folder : " + folder_utf8);
    emit("Probe file    : " + probe_path_utf8 + " (deleted at the end of the auto segment)");
    emit("");
    emit("Check 1: open Explorer > Quick access > Recent items. The probe file must NOT be listed");
    emit("         (it was projected and then revoked). Type y when verified.");
    emit("Check 2: open Windows Settings > Privacy > 'Recent items' and confirm tracking is ON, or");
    emit("         note that a disabled switch makes the auto segment exit with code 2.");
    int unconfirmed = 0;
    for (int index = 1; index <= 2; ++index) {
        emit("Confirm check " + aurora_verify::format_int(index) + " [y/n]:");
        std::string answer;
        if (!std::getline(std::cin, answer) || answer.empty() || answer[0] != 'y') {
            ++unconfirmed;
            emit(std::string("[MANUAL] check ") + aurora_verify::format_int(index) + " not confirmed");
        } else {
            emit(std::string("[PASS  ] check ") + aurora_verify::format_int(index) + " confirmed by eye");
        }
    }
    return unconfirmed == 0 ? 0 : 7;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与同目录探针入口同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Win32 OS recent-documents projection live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    aurora::init_console();  // Windows 控制台切 UTF-8，避免路径行错行

    emit("== Aurora Win32 OS recent-documents projection live probe ==");
    emit("auto segment: capability bit / tracking switch / project / idempotency / revoke / store isolation");

    std::wstring folder;
    std::string probe_path;
    const int auto_code = run_auto_stage(folder, probe_path);
    if (auto_code != 0) {
        return auto_code;
    }
    if (!interactive) {
        emit("Skipping the interactive stage: whether the OS UI lists the entry is a human judgement");
        emit("(re-run with --interactive).");
        emit("PASS: Win32 OS recent-documents projection acceptance passed (auto stage)");
        return 0;
    }
    const int human_code = run_interactive_stage(folder, probe_path);
    if (human_code == 0) {
        emit("PASS: Win32 OS recent-documents projection acceptance passed (auto + interactive)");
    }
    return human_code;
}

// ---------------------------------------------------------------------------
// 已知差距（供后续任务决策，本探针不掩盖）
//   1. 撤销只删「本进程投影记录命中的」快捷方式。同一文件若也被别的应用打开过，那条 `.lnk` 是
//      共享记录，撤销对别的应用同样生效；探针第 10 步会把这类残留如实打出来，不据此判 FAIL。
//   2. 注册表侧按扩展名的 MRU（`HKCU\...\Explorer\RecentDocs`）是用户全局二进制 MRU，桥按设计不碰，
//      故撤销后该 MRU 里仍可能留着探针文件的记录片段——本探针不把它当判据（改了反而污染用户数据）。
//   3. `Start_TrackDocs=0` 时自动段以退出码 2 收尾：这是用户策略，不是库的缺陷，但此时「OS 落记录」
//      这条判据在本机没有得到覆盖，需在开启追踪的机器上补跑。
//   4. 人工段确认为 n 或未确认时退出码 7：OS 的 UI 呈现层级（跳转列表是否刷新）受壳的缓存与时序影响，
//      机器判据无法覆盖，保留人工段是为了不让「目录里有」冒充「用户看得见」。
//   5. 壳写出快捷方式**晚于** `SHAddToRecentDocs` 返回（实测毫秒级迟到）。探针据此把判据写成有界轮询；
//      桥侧不能照做（撤销在 UI 线程上，绝不为了等一条记录而阻塞），故记账只销「确认删掉了」的那几条，
//      本次没找到的条目留账待下次撤销认领——代价是最坏情况下留一条无从兑现的账，好于无主残留。
// ---------------------------------------------------------------------------
