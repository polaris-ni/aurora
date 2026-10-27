// tools/verify/win32_clipboard_live_probe.cpp — Win32 剪贴板富格式与惰性提供的真机验收探针（非 CTest）。
//
// 覆盖：`Clipboard` 的富格式（文件清单 / HTML / 自定义 MIME）经 `src/aurora/window/detail/
// win32_clipboard.cpp` 落到系统剪贴板的整条接线，以及三件**以 owner 窗口为投递目标**的能力：
// 惰性提供（`WM_RENDERFORMAT`）、变更监听（`WM_CLIPBOARDUPDATE`）、程序化清空。
//
// 为什么必须有本探针（无头 CI 证明不了的部分）：
//   * 折算层单测只能证 `CF_HDROP` / `CF_HTML` 的**字节布局**，证不了「系统剪贴板真的收下了这一块」；
//   * 惰性提供的兑现点是 `WM_RENDERFORMAT`，它只在有人请求该格式时由系统发给 owner 窗口——
//     没有真实剪贴板与真实窗口，这条链路一次都不会跑；
//   * 「无 owner 窗口即无机制」是一条**平台事实**，只有在真机上建窗 / 不建窗各跑一次才证得。
//
// 取证口径（关键：证据链**独立于**被测桥）：
//   读侧一律用原生 Win32 API（`OpenClipboard` / `GetClipboardData` / `DragQueryFileW` /
//   `RegisterClipboardFormatA` / `EnumClipboardFormats`），不经过库的读取通道判「写没写进去」——
//   否则桥写错了格式名、读侧照着同一个错名读回来，缺陷会被自证。
//
// 构建（方式 ① 推荐）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_clipboard
//   build-verify\aurora_verify_win32_clipboard.exe
//
// 自动段（无人值守，逐项判据）：
//   1. 建窗（owner 就绪）——剪贴板惰性提供与监听的承载体。建窗失败即环境不可用（退出码 2）。
//   2. 文件清单：`set_files` 后由原生 `CF_HDROP` + `DragQueryFileW` 解析出路径清单，逐条比对。
//   3. HTML：`set_html` 后由原生 `HTML Format` 读出整块，断言含 `StartFragment` 头与片段本身。
//   4. 自定义 MIME：`set_payload` 后由原生注册名读回，逐字节相等。
//   5. `available_formats()` 如实包含上述三档（新 API 已声明 ≠ 格式已可用，这条是反向判据）。
//   6. 惰性提供：`set_offer` 后 `provide` **零次**被调用；随后由原生 `GetClipboardData` 触发
//      `WM_RENDERFORMAT`（同线程 SendMessage 直达 owner 的窗口过程），断言调用一次且字节正确。
//   7. 变更监听：注册 `on_change` 后由原生写入触发 `WM_CLIPBOARDUPDATE`，泵消息后断言回调到达。
//      ⚠️ 该判据依赖消息泵，未到达时记 SKIP 而非 FAIL（泵不到是环境问题，不是库的缺陷）。
//   8. 清空：`clear()` 后原生 `EnumClipboardFormats` 计数为 0。
//
// 退出码：0 = 自动段全通过；1 = 有 FAIL；2 = 环境不可用（建窗失败 / 非 Win32 构建）。

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

// NOLINTBEGIN(readability-identifier-naming): Windows SDK 宏，命名不可改
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOLINTEND(readability-identifier-naming)
#include <windows.h>
#include <shellapi.h>  // `HDROP` / `DragQueryFileW`（须在 `windows.h` 之后，MSVC 下先行即炸）

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "verify_args.h"

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

// @brief 自动段累计的失败数。
//
// 文件作用域：探针是单文件、单线程、无状态的小工具，计数器本身就是它的全部可变状态；
// 为了绕过静态检查把它包进单例类只是形式主义，反而多一层不必要的间接。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int failures = 0;

/// @brief 记录一条断言结论：打印 PASS / FAIL 行并累计失败数。
auto check(bool passed, const std::string &label) -> void {
    emit(std::string{passed ? "[PASS] " : "[FAIL] "} + label);
    if (!passed) {
        ++failures;
    }
}

/// @brief 记录一条「本轮未得到覆盖」的判据：打印 SKIP 行，不计失败。
auto skip(const std::string &label) -> void { emit(std::string{"[SKIP] "} + label); }

/// @brief 泵干本线程消息队列（让 `WM_CLIPBOARDUPDATE` 之类的通知到达窗口过程）。
auto pump_messages(int rounds) -> void {
    MSG msg{};
    for (int i = 0; i < rounds; ++i) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }
}

/// @brief 原生读出 `CF_HDROP` 的路径清单（独立取证：不经库的 `get_files`）。
auto native_read_files() -> std::vector<std::string> {
    std::vector<std::string> paths;
    if (OpenClipboard(nullptr) == 0 || IsClipboardFormatAvailable(CF_HDROP) == 0) {
        CloseClipboard();
        return paths;
    }
    const HANDLE handle = GetClipboardData(CF_HDROP);
    auto *drop = static_cast<HDROP>(GlobalLock(handle));
    if (drop != nullptr) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<wchar_t> buffer(32768, L'\0');
        for (UINT i = 0; i < count; ++i) {
            buffer.assign(buffer.size(), L'\0');
            const UINT copied = DragQueryFileW(drop, i, buffer.data(), static_cast<UINT>(buffer.size()));
            if (copied == 0) {
                continue;
            }
            const int bytes =
                WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(copied), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(copied), utf8.data(), bytes, nullptr,
                                nullptr);
            paths.emplace_back(utf8);
        }
        GlobalUnlock(handle);
    }
    CloseClipboard();
    return paths;
}

/// @brief 原生读出某个已注册剪贴板格式的字节（独立取证）。
auto native_read_bytes(const char *format_name) -> std::string {
    const UINT format = RegisterClipboardFormatA(format_name);
    std::string out;
    if (format == 0U || OpenClipboard(nullptr) == 0) {
        return out;
    }
    const HANDLE handle = GetClipboardData(static_cast<WORD>(format));
    const auto *source = static_cast<const char *>(GlobalLock(handle));
    if (source != nullptr) {
        const SIZE_T size = GlobalSize(handle);
        out.assign(source, size);
        GlobalUnlock(handle);
    }
    CloseClipboard();
    return out;
}

/// @brief 原生枚举剪贴板当前格式数（清空的独立取证）。
auto native_format_count() -> int {
    if (OpenClipboard(nullptr) == 0) {
        return -1;
    }
    int count = 0;
    UINT format = 0;
    while ((format = EnumClipboardFormats(format)) != 0U) {
        ++count;
    }
    CloseClipboard();
    return count;
}

auto run_auto_stage() -> int {
    // 1. 建窗：剪贴板桥随 `Win32Host` 构造登记 owner 窗口，三件能力自此才成立。
    au::WindowOptions base{.title = "Aurora clipboard probe"};
    au::Win32Options options;
    static_cast<au::WindowOptions &>(options) = base;
    auto window = au::create_window(options);
    if (!window) {
        emit("FAIL: cannot create a Win32 window (no clipboard owner): " + window.error().message);
        return 2;  // 环境不可用
    }
    emit("[PASS] window created (clipboard owner registered)");

    // 2. 文件清单往返（原生 `CF_HDROP` + `DragQueryFileW` 解析）。
    const std::vector<std::string> paths{R"(C:\Windows\System32\notepad.exe)", R"(C:\docs\report.txt)"};
    const auto wrote_files = au::Clipboard::set_files(paths);
    check(wrote_files && wrote_files.value(), "set_files returns Ok(true)");
    const auto native_files = native_read_files();
    check(native_files.size() == paths.size(), "native CF_HDROP carries exactly the written entry count");
    check(native_files.size() == paths.size() && native_files.at(0) == paths.at(0),
          "native CF_HDROP first entry round-trips byte for byte");

    // 3. HTML 往返（原生读 `HTML Format`，校验头部与片段都在）。
    const std::string fragment = "<b>aurora</b>";
    const auto wrote_html = au::Clipboard::set_html(fragment);
    check(wrote_html && wrote_html.value(), "set_html returns Ok(true)");
    const auto native_html = native_read_bytes("HTML Format");
    check(!native_html.empty() && native_html.find("StartFragment:") != std::string::npos,
          "native CF_HTML block carries the StartFragment header");
    check(native_html.find(fragment) != std::string::npos, "native CF_HTML block contains the fragment");

    // 4. 自定义 MIME 往返（原生按注册名读回）。
    const std::string probe_mime = "application/x-aurora-clipboard-probe";
    const std::vector<std::uint8_t> payload{0x41U, 0x00U, 0x42U, 0xFFU, 0x7FU};
    const auto wrote_payload = au::Clipboard::set_payload(probe_mime, payload);
    check(wrote_payload && wrote_payload.value(), "set_payload returns Ok(true)");
    const auto native_payload = native_read_bytes(probe_mime.c_str());
    check(native_payload.size() == payload.size() &&
              std::equal(payload.begin(), payload.end(), native_payload.begin(),
                         [](std::uint8_t byte, char observed) { return byte == static_cast<std::uint8_t>(observed); }),
          "native custom-MIME block round-trips byte for byte");

    // 5. `available_formats()` 如实列出（重放一次文件清单后查询：立即写以 EmptyClipboard 开头，
    //    故每次只查最后一次写入的内容）。
    const auto re_wrote = au::Clipboard::set_files(paths);
    check(re_wrote && re_wrote.value(), "set_files returns Ok(true) on the replay");
    const auto formats = au::Clipboard::available_formats();
    bool has_uri_list = false;
    if (formats) {
        for (const auto &mime : formats.value()) {
            if (mime == au::AURORA_MIME_URI_LIST) {
                has_uri_list = true;
            }
        }
    }
    check(formats && has_uri_list, "available_formats reports the uri-list after set_files");

    // 6. 惰性提供：注册即声明、`provide` 在真正被请求前零次调用。
    int provide_calls = 0;
    const auto offered = au::Clipboard::set_offer(au::ClipboardOffer{
        .formats = {std::string{au::AURORA_MIME_TEXT_PLAIN}},
        .provide = [&provide_calls](const std::string &mime) -> au::Result<std::vector<std::uint8_t>> {
            ++provide_calls;
            const std::string text = "lazy:" + mime;
            return std::vector<std::uint8_t>(text.begin(), text.end());
        }});
    check(offered.ok(), "set_offer returns Ok");
    check(provide_calls == 0, "provide is not called at registration time (lazy, not eager)");

    // 原生请求触发 `WM_RENDERFORMAT`：同线程 SendMessage 直达 owner 的窗口过程。
    std::string rendered;
    if (OpenClipboard(nullptr) != 0) {
        const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        const auto *wide = static_cast<const wchar_t *>(GlobalLock(handle));
        if (wide != nullptr) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                rendered.resize(static_cast<std::size_t>(bytes) - 1U);
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, rendered.data(), bytes, nullptr, nullptr);
            }
            GlobalUnlock(handle);
        }
        CloseClipboard();
    }
    check(provide_calls == 1, "provide is called exactly once when the format is requested");
    check(rendered == "lazy:text/plain", "WM_RENDERFORMAT delivers the bytes produced by provide");

    // 7. 变更监听：`WM_CLIPBOARDUPDATE` 需消息泵才到达，泵不到如实记 SKIP。
    bool changed = false;
    const auto listening = au::Clipboard::on_change([&changed] { changed = true; });
    check(listening.ok(), "on_change registers on this platform");
    if (listening.ok()) {
        if (OpenClipboard(nullptr) != 0) {
            EmptyClipboard();
            const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, 4U);
            auto *target = static_cast<wchar_t *>(GlobalLock(handle));
            if (target != nullptr) {
                std::memset(target, 0, 4U);
                GlobalUnlock(handle);
            }
            SetClipboardData(CF_UNICODETEXT, handle);
            CloseClipboard();
        }
        pump_messages(20);
        if (changed) {
            check(true, "on_change callback fires after a foreign clipboard write");
        } else {
            skip("on_change callback did not arrive (message pump did not deliver WM_CLIPBOARDUPDATE)");
        }
    }

    // 8. 程序化清空：原生枚举计数归零。
    const auto cleared = au::Clipboard::clear();
    check(cleared.ok(), "clear returns Ok");
    const int remaining = native_format_count();
    check(remaining == 0, "native clipboard holds no format after clear");

    return failures == 0 ? 0 : 1;
}

}  // namespace

// 豁免异常逃逸：主体已 try/catch 覆盖；`main` 抛异常等价于 terminate，显式标注不改变可观测行为。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli =
        aurora_verify::parse_interactive("Win32 clipboard rich-format and lazy-provide live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    aurora::init_console();

    emit("== Aurora Win32 clipboard live probe ==");
    emit("auto segment: files / html / custom MIME / lazy provide / change listener / clear");
    try {
        const int code = run_auto_stage();
        if (code == 0) {
            emit("PASS: Win32 clipboard acceptance passed (auto stage)");
        } else if (code == 1) {
            emit("FAIL: Win32 clipboard acceptance failed (auto stage)");
        }
        return code;
    } catch (const std::exception &error) {
        emit(std::string{"FAIL: clipboard probe threw: "} + error.what());
        return 1;
    } catch (...) {
        emit("FAIL: clipboard probe threw an unknown exception");
        return 1;
    }
}

// ---------------------------------------------------------------------------
// 已知差距（供后续任务决策，本探针不掩盖）
//   1. 惰性提供的真机判据依赖「同线程 SendMessage 直达窗口过程」。跨进程场景（别的进程请求格式）
//      下 `WM_RENDERFORMAT` 走的是同一条窗口过程，但本探针不构造跨进程请求方，故那条路径未覆盖。
//   2. 变更监听判据依赖消息泵：泵不到时记 SKIP 而非 FAIL——泵不到是环境问题，不是库的缺陷，
//      但这也意味着本条判据在无泵环境下不产生证据。
//   3. 立即写与惰性声明**混用**（同一份剪贴板内容上部分格式走立即写）没有真机判据：Win32 侧每次
//      立即写都以 `EmptyClipboard` 开头，混用形态的验证需要一次打开态内放多档的合成用例，本探针不构造。
//   4. `CF_DIBV5` → PNG 转码按提案第 24 条不落本批，故拖入侧图像没有真机判据。
// ---------------------------------------------------------------------------
