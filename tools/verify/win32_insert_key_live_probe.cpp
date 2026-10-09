// tools/verify/win32_insert_key_live_probe.cpp — 主键盘 Insert 键码真机验收探针（非 CTest）。
//
// 覆盖：`KeyCode::Insert`（枚举值 123）在 Win32 后端上真的从物理按键走到事件流。三条判据：
//   (a) 事件流里出现 `KeyCode::Insert`；
//   (b) `key_state` 成对：同一次物理按键产生 **Down 一条 + Up 一条**；
//   (c) 无重复派发：一次按下不产生第二条 Down。
//
// **为什么必须真键盘，不能用注入冒充**：本条的判据是「物理主键盘 Insert → `KeyCode::Insert`」，
// 而 `SendMessage` 注入的 `WM_KEYDOWN` 只改消息流、不经过键盘驱动与扫描码通路；锁屏 / 无人值守
// 会话下 `SendInput` 还会**静默失效**（不报错、什么都不发生），用注入得到的绿无法区分
// 「映射正确」与「根本没收到键」。故：
//   * 自动段只做**通道覆盖**（证明 `wnd_proc` 拿到 `VK_INSERT` 时确实译成 `KeyCode::Insert`），
//     明确标注为「不作验收判据」；
//   * 真正的验收在**人工段**，由操作者在真前台窗口上按一次 Insert。
//
// 未按键 / 锁屏 / 无前台时：三条判据一律记 **PENDING MANUAL**（退出码 3），**不记 PASS**——
// 把「没收到」当成「通过」正是本条要防的自欺。
//
// 构建（方式 ① 推荐，Windows 上无需手写编译命令）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_insert_key --config Release
//   build-verify/Release/aurora_verify_win32_insert_key.exe
// 退出码：0 三条判据全成立；1 有判据失败；2 环境不可用；3 未收到真按键（待人工，非通过）。

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_insert_key can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

#include <windows.h>

#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/win32_host.h"
#include "verify_print.h"

namespace {

/// @brief 累计的断言失败条数，决定退出码 1。
int failures = 0;

/// @brief 一次物理按键的记录（整数键码 + 动作），供三条判据读取。
struct KeyRecord {
    int key = 0;
    aurora::KeyAction action = aurora::KeyAction::Down;
};

std::vector<KeyRecord> g_records;  ///< 人工段收到的按键序列（换段时清空）。

/// @brief 输出一行探针结论到 stdout（经 `AURORA_LOG_RAW`：无日志前缀、不过级别过滤）。
/// @param text 待输出的一行文本（调用方不传行尾换行）。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 记录一条断言结论：打印 PASS / FAIL 行并累计失败数。
/// @param passed 断言是否成立。
/// @param label 断言的人类可读描述（ASCII）。
auto check(bool passed, const std::string &label) -> void {
    emit(std::string("[") + (passed ? "PASS" : "FAIL") + ") " + label);
    if (!passed) {
        ++failures;
    }
}

/// @brief 取窗口句柄。
/// @param host 宿主引用。
/// @return 宿主窗口句柄。
[[nodiscard]] auto hwnd_of(aurora::Win32Host &host) -> HWND { return static_cast<HWND>(host.hwnd()); }

/// @brief 建一个前台探针窗口并挂上按键记录回调。
/// @param host 待构造的宿主（构造失败时 `hwnd()` 为 nullptr）。
/// @param title 窗口标题（ASCII）。
void open_probe_window(aurora::Win32Host &host, const char *title) {
    host.set_event_handler([](aurora::Event &e) {
        auto *k = dynamic_cast<aurora::KeyEvent *>(&e);
        if (k == nullptr) {
            return;
        }
        g_records.push_back(KeyRecord{.key = k->key, .action = k->action});
    });
    (void)title;
}

/// @brief 自动段：**通道覆盖**，不是验收判据。
///
/// 注入 `WM_KEYDOWN` / `WM_KEYUP`（`wParam = VK_INSERT`）证明宿主的 `wnd_proc` 拿到该虚拟键码时
/// 确实译成 `KeyCode::Insert`——即新加的 `case VK_INSERT` 分支被真实消息路径走到。它证不了
/// 「物理主键盘 Insert 会产生 `VK_INSERT`」（那是操作系统的事，也是本条真机探针的意义所在），
/// 故结果只作通道自检打印，计入失败但不算验收通过。
/// @param host 宿主引用。
/// @return 通道是否按要求产出 `KeyCode::Insert`。
auto check_injected_path_reaches_insert(aurora::Win32Host &host) -> bool {
    emit("---- auto segment (channel coverage ONLY, not the acceptance criterion) ----");
    g_records.clear();
    const HWND hwnd = hwnd_of(host);
    SendMessageA(hwnd, WM_KEYDOWN, VK_INSERT, 0);
    SendMessageA(hwnd, WM_KEYUP, VK_INSERT, 0);
    MSG msg{};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    const auto want = static_cast<int>(aurora::KeyCode::Insert);
    bool saw_insert = false;
    for (const auto &r : g_records) {
        if (r.key == want) {
            saw_insert = true;
        }
    }
    emit(std::string("[NOTE] injected WM_KEYDOWN/WM_KEYUP(VK_INSERT) produced ") +
         aurora_verify::format_uint(g_records.size()) +
         " KeyEvent(s); KeyCode::Insert present = " + (saw_insert ? "yes" : "no"));
    emit("[NOTE] this only proves the mapping table is reached by the message path; it does NOT prove");
    emit("       a physical Insert keypress yields VK_INSERT. That is what the manual stage is for.");
    return saw_insert;
}

/// @brief 人工段：操作者在探针窗口上按一次主键盘 Insert，三条判据自动判读。
/// @param host 宿主引用。
/// @return 0 三条判据成立；1 有判据失败；3 未收到任何按键（待人工）。
auto run_manual_stage(aurora::Win32Host &host) -> int {
    emit("---- manual segment (this is the real acceptance) ----");
    g_records.clear();
    emit("Press the MAIN-KEYBOARD Insert key ONCE on the probe window now (press and release).");
    emit("  Expected: exactly two KeyEvents, one Down then one Up, both key == KeyCode::Insert.");
    host.wait_events(30.0);

    if (g_records.empty()) {
        emit("[PENDING MANUAL] no key event within 30s - NOT counted as a pass.");
        emit("       Causes: the window is not focused, the session is locked (locked sessions deliver");
        emit("       no keyboard input), or the key was not pressed. Re-run with the probe window focused.");
        return 3;
    }

    const auto want = static_cast<int>(aurora::KeyCode::Insert);
    std::size_t downs = 0;
    std::size_t ups = 0;
    std::size_t insert_hits = 0;
    for (const auto &r : g_records) {
        if (r.key == want) {
            ++insert_hits;
        }
        if (r.action == aurora::KeyAction::Down) {
            ++downs;
        } else {
            ++ups;
        }
    }
    // (a) 事件流出现 KeyCode::Insert。
    check(insert_hits >= 1U, "(a) KeyEvent carries KeyCode::Insert: hits = " + aurora_verify::format_uint(insert_hits) +
                                 " of " + aurora_verify::format_uint(g_records.size()) + " events");
    // (b) 成对：Down 一条 + Up 一条。
    check(downs == 1U && ups == 1U,
          "(b) one Down and one Up (key_state pairs): downs = " + aurora_verify::format_uint(downs) +
              " ups = " + aurora_verify::format_uint(ups));
    // (c) 无重复派发：一次按下只一条 Down、总条数为二。
    check(g_records.size() == 2U, "(c) no duplicate dispatch: total events = " +
                                      aurora_verify::format_uint(g_records.size()) + " (expected 2)");
    if (g_records.size() >= 1U) {
        emit("       first event: key=" + aurora_verify::format_int(g_records.front().key) +
             " action=" + (g_records.front().action == aurora::KeyAction::Down ? "Down" : "Up"));
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与 demo 入口同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    aurora::init_console();  // Windows 控制台切 UTF-8，避免表格错行
    emit("== Aurora Win32 main-keyboard Insert live probe ==");

#ifndef AURORA_BACKEND_WIN32
    emit("result: Win32 backend is off in this build (exit 2)");
    return 2;
#else
    aurora::WindowStyleOptions style{};
    style.resizable = false;
    aurora::Win32Host host{320, 240, "Aurora Win32 Insert key live probe", style};
    if (hwnd_of(host) == nullptr) {
        emit("result: no native window handle (exit 2)");
        return 2;
    }
    open_probe_window(host, "probe");
    // 窗口需真的在前台才能收到物理按键：先激活一次。
    SetForegroundWindow(hwnd_of(host));

    const bool channel_ok = check_injected_path_reaches_insert(host);
    if (!channel_ok) {
        emit("[FAIL] injected VK_INSERT did not reach the handler as KeyCode::Insert (mapping table broken)");
        return 1;
    }

    const int manual = run_manual_stage(host);
    if (manual == 3) {
        emit("result: no real keypress observed - recorded as PENDING MANUAL, not a pass (exit 3)");
        return 3;
    }
    if (manual == 0) {
        emit("PASS: main-keyboard Insert produced key_state-paired KeyCode::Insert events (all three criteria)");
        return 0;
    }
    emit("result: one or more criteria failed (exit 1)");
    return 1;
#endif
}
