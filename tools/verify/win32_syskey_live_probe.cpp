// tools/verify/win32_syskey_live_probe.cpp — Win32 WM_SYSKEY* 派发链路真机验收探针（非 CTest）。
//
// 覆盖：`Win32Host::handle_syskey`（src/aurora/window/win32_host.cpp）把「按住 Alt 期间的按键」
// 与常规键**同路送进派发链**这一改动——消费即止（`return 0`），未消费原样回落
// `DefWindowProcA`（系统菜单 / 菜单助记键 / Alt+F4 关闭不被掐死）。例外是 `VK_MENU`（左右）
// 与 `VK_F10`：只推进修饰态、不派发（判据见 `detail::syskey_dispatches`）。
//
// 为什么必须有本探针（无头 CI 证明不了的部分）：
//   * `WM_SYSKEY*` 只在**真实消息泵**里由系统投递，`PostMessage` 造出来的消息不经过
//     `DefWindowProcA` 的 accelerator 路径，故「未消费时系统行为仍发生」只能真机证明；
//   * 「消费时 Alt+F4 不关窗」是**否定性**断言，只能真机证明；
//   * Alt 组合在库内的完整闭环（VK → `KeyCode` → `ModifierKey::Alt` 位）要经真实消息流走一遍。
//
// 原理：探针自建一个真实的 `Win32Host`，在其窗口过程里挂一个测试用的事件处理器记录收到的
// `KeyEvent`；随后用 `SendMessageW(hwnd, WM_SYSKEYDOWN, vk, ...)` 注入 —— 注意用 `SendMessage`
// 而非 `PostMessage`：前者让消息**同步**走完整个窗口过程（含 `DefWindowProcA`），后者只入队，
// 无法在断言点读到「本轮是否被 Aurora 消费」。
//
// 构建（方式 ① 推荐，Windows 上无需手写编译命令）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_syskey --config Release
//   build-verify/Release/aurora_verify_win32_syskey.exe [--interactive]
// 构建（方式 ② 手写 MinGW 命令，须先有一个已构建好的 Win32 构建目录 build）：
//   g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic
//       -I include -I tools/verify -I tools/include -DAURORA_BACKEND_WIN32 -DNOMINMAX
//       tools/verify/win32_syskey_live_probe.cpp -o probe.exe
//       build/libaurora.a -luser32 -lgdi32 -lole32 -lshell32 -lcomctl32 -lwinpthread
//
// 运行：命令行直接跑。`--interactive` 段需要操作者亲手按 Alt+F4，故与自动段分开。
//
// 自动段（无需人工按键）：
//   1. Alt+字母（A）经 `WM_SYSKEYDOWN` 投递 → 宿主回调收到 `KeyEvent`，`modifiers` 含
//      `ModifierKey::Alt`，`key` 为 `KeyCode::A`，`action` 为 `KeyAction::Down`。
//   2. 修饰态推进早于派发：先注入 `WM_SYSKEYDOWN(VK_MENU)`（Alt 自身，**不派发**），再注入
//      `WM_SYSKEYDOWN('A')`，后者的事件必须仍带 Alt 位 —— 这是「不推进即错报不带 Alt」的守门。
//   3. `VK_F10`（系统菜单键）不派发：注入后事件处理器不收到任何 `KeyEvent`。
//   4. 消费态路径可达：把处理器切成「恒消费」后注入 Alt+F4，处理器仍应看到该事件。
//   5. **SKIP（非缺陷，是通道限制）**：「消费即止 / 未消费回落 `DefWindowProcA`」这一对在注入
//      路径下不可证 —— `DefWindowProcA` 判「此刻是否按住 Alt」读 `GetKeyState(VK_MENU)` 这一
//      **物理**态，而 `SendMessage` 只改消息流。裸 Win32 对照实测（不经 Aurora）证实注入
//      Alt+F4 / Alt+Space / Alt+Enter 一个 `WM_SYSCOMMAND` 都不产生，故消费与否观感相同。
//      该对语义由 `--interactive` 人工段（真键盘 + 窗口子类观测 `SC_CLOSE`）验收。
//   6. `WM_SYSKEYUP` 同样进派发链（`KeyAction::Up`）。
// 人工段（--interactive）：按提示真的按一次 Alt+F4 —— 未消费时窗口应当关闭；随后重新跑并在
//   消费态下再按一次，窗口应当不关闭。Alt+F4 会真的关掉探针窗口，故本段只观察、不自动断言，
//   结论由操作者按屏幕现象核对后回填 `codespec/manual-test/`。
// 退出码：0 全过；1 自动段断言失败；2 环境不可用（非 Windows 或 Win32 后端未开）。

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_syskey can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

// 与库内同口径：先在**任何**平台头之前定好这两个宏，避免 <windows.h> 的 min/max 宏污染
// 标准库用法（src/aurora/app/detail/platform_shell_win32.cpp 内同样带守卫）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/win32_host.h"
#include "verify_args.h"
#include "verify_print.h"

namespace {

/// @brief 累计的断言失败条数，决定退出码 1。
int failures = 0;

/// @brief 宿主回调侧收到的全部 `KeyEvent` 记录（本探针的观察面）。
struct KeyRecord {
    int key = 0;  ///< `KeyEvent::key`（`KeyCode` 的整数值）。
    std::uint8_t modifiers = 0;  ///< `KeyEvent::modifiers` 按位转 uint8_t 后的位集。
    bool is_down = false;  ///< `action == KeyAction::Down`。

    /// @return 该记录的修饰位是否含 Alt。
    [[nodiscard]] auto has_alt() const -> bool {
        return (modifiers & static_cast<std::uint8_t>(aurora::ModifierKey::Alt)) != 0U;
    }
};

/// @brief 事件处理器开关：`consume` 为真时把每条 `KeyEvent` 标记为已消费。
struct KeySink {
    std::vector<KeyRecord> records;
    bool consume = false;

    /// @brief 事件处理器本体：只观察 `KeyEvent`，其余事件类型忽略。
    /// @param e 宿主上抛的事件引用。
    auto operator()(aurora::Event &e) -> void {
        auto *k = dynamic_cast<aurora::KeyEvent *>(&e);
        if (k == nullptr) {
            return;
        }
        records.push_back(KeyRecord{
            .key = k->key,
            .modifiers = static_cast<std::uint8_t>(k->modifiers),
            .is_down = k->action == aurora::KeyAction::Down,
        });
        if (consume) {
            k->is_handled = true;
        }
    }
};

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

/// @brief 记录一条 SKIP 结论：该判据在当前通道下不可证，既不判通过也不判失败。
/// @param label 跳过项的说明（ASCII）。
auto skip(const std::string &label) -> void { emit(std::string("[SKIP] ") + label); }

/// @brief 向宿主窗口同步投递一条键盘系统消息（`SendMessage` 而非 `PostMessage`）。
///
/// 同步是判据成立的前提：只有同步派发才能在本行读到 `DefWindowProcA` 的结果，从而区分
/// 「Aurora 消费掉了（返回 0）」与「交回系统处理（返回系统结果）」。
/// @param hwnd 目标窗口句柄。
/// @param syskey_down true = `WM_SYSKEYDOWN`，false = `WM_SYSKEYUP`。
/// @param vk 虚拟键码。
/// @return 窗口过程的返回值（`handle_syskey` 的返回值 / 系统默认结果）。
auto send_syskey(HWND hwnd, bool syskey_down, int vk) -> LRESULT {
    return SendMessageW(hwnd, syskey_down ? WM_SYSKEYDOWN : WM_SYSKEYUP, static_cast<WPARAM>(vk), 0);
}

/// @brief 取窗口句柄（探针需要向真实的宿主窗口注入消息）。
/// @param host 宿主引用。
/// @return 宿主窗口句柄。
[[nodiscard]] auto hwnd_of(aurora::Win32Host &host) -> HWND { return static_cast<HWND>(host.hwnd()); }

// ---- 窗口子类：观测系统菜单 / 关闭请求是否真的到达（**仅人工段**）----
//
// 为什么不看 `handle_syskey` 的返回值：`DefWindowProcA` 对 `WM_SYSKEYDOWN` 的返回值**本来就是 0**
// （它只把 accelerator 查一遍，无匹配即返回 0），故「返回值非零」不是「已回落系统」的判据。
// 真正的机器证据在下游：Alt+F4 走完 `DefWindowProcA` 会发 `WM_SYSCOMMAND(SC_CLOSE)`，
// Alt+Space 会发 `WM_SYSCOMMAND(SC_SYSMENU)`。子类记下它即「系统行为确实发生了」的正面证据。
//
// **为什么自动段（注入）用不上**：`DefWindowProcA` 判「此刻是否按住 Alt」读的是
// `GetKeyState(VK_MENU)` 这一**物理**态，而 `SendMessage` 注入只改消息流、不改物理键态，
// 于是恒走「没按 Alt」分支。裸 Win32 对照实测（不经 Aurora、直接 `DefWindowProcA`）证实：
// 注入 Alt+F4 / Alt+Space / Alt+Enter 一个 `WM_SYSCOMMAND` 都不产生。故自动段对此如实记
// SKIP，只有真键盘（`--interactive`）能产生 `SC_*`。
//
// 子类里**不把 WM_SYSCOMMAND 交回**（直接返回 0）：`SC_CLOSE` 交回即销毁窗口，探针就跑不到
// 后续断言了。记录本身就是我们要的证据，交回与否不影响判读。
//
// `SC_CLOSE` 在 MinGW 的 `windows.h` 里虽有定义，但同族的 `SC_SYSMENU` 没有（只有 MSVC SDK 的
// `winuser.h` 给了），故这一族统一按 Win32 文档的官方数值写死并注明来源，避免为探针引入 SDK 依赖。
constexpr WORD kScClose = 0xF060;  ///< `SC_CLOSE`（Alt+F4 关闭窗口）。
constexpr WORD kMaskCommand = 0xFFF0U;  ///< `WM_SYSCOMMAND` 的 wParam 低 4 位为保留位，须掩掉。

WNDPROC g_original_wndproc = nullptr;  ///< Aurora 自己的窗口过程（子类化前的原值）。
std::vector<WORD> g_syscommands;  ///< 收到的 `WM_SYSCOMMAND` 命令码序列（`SC_CLOSE` 等）。

/// @brief 子类窗口过程：只拦 `WM_SYSCOMMAND` 做记录，其余原样转交 Aurora 的窗口过程。
/// @param hwnd 窗口句柄。
/// @param msg 消息码。
/// @param wp 消息 wParam（对 `WM_SYSCOMMAND` 即命令码）。
/// @param lp 消息 lParam。
/// @return 子类自身消费时返回 0；否则为原窗口过程的返回值。
auto CALLBACK subclass_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    if (msg == WM_SYSCOMMAND) {
        g_syscommands.push_back(static_cast<WORD>(wp & kMaskCommand));
        return 0;  // 记录即可，不交回：交回 SC_CLOSE 会销毁窗口，后续断言就没了
    }
    return CallWindowProcA(g_original_wndproc, hwnd, msg, wp, lp);
}

/// @brief 给窗口装上子类（记录 `WM_SYSCOMMAND`）。
/// @param hwnd 目标窗口。
/// @return 装类是否成功。
[[nodiscard]] auto install_subclass(HWND hwnd) -> bool {
    SetLastError(0);
    g_original_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&subclass_wnd_proc)));
    return g_original_wndproc != nullptr;
}

/// @brief 自动段：全部判据见本文件头注释的「自动段」清单。
///
/// 本探针只在 `WIN32 ∧ (AURORA_BACKEND_WIN32 ∨ AURORA_BACKEND_D3D11)` 下注册构建
/// （`cmake/AuroraVerify.cmake` 已守卫），故此处不再自带平台宏分支——那只会成为不可达死代码。
/// @return 该段退出码：0 通过；2 环境不可用（取不到窗口句柄）。
auto run_auto_stage() -> int {
    emit("---- auto segment ----");

    KeySink sink;
    aurora::WindowStyleOptions style{};
    // frameless + 不可调大小：探针不需要非客户区，且 Alt+F4 人工段的观察对象更干净。
    style.frameless = true;
    style.resizable = false;
    aurora::Win32Host host{320, 240, "Aurora Win32 syskey live probe", style};
    host.set_event_handler([&sink](aurora::Event &e) { sink(e); });
    HWND hwnd = hwnd_of(host);
    if (hwnd == nullptr) {
        emit("result: no native window handle (exit 2)");
        return 2;
    }
    check(true, "Win32Host created with a valid HWND");

    // 1. Alt+字母：宿主回调收到 KeyEvent，modifiers 含 Alt、键码为 A、动作为 Down。
    sink.records.clear();
    send_syskey(hwnd, true, 'A');
    const bool got = sink.records.size() == 1U;
    check(got, "WM_SYSKEYDOWN(Alt+A) reaches the handler as exactly one KeyEvent: " +
                   aurora_verify::format_uint(sink.records.size()));
    if (got) {
        const KeyRecord &r = sink.records.front();
        check(r.is_down, "the KeyEvent action is KeyAction::Down");
        check(r.key == static_cast<int>(aurora::KeyCode::A),
              "the KeyEvent key is KeyCode::A (raw=" + aurora_verify::format_int(r.key) + ")");
    }

    // 2. 修饰态推进早于派发：先按 Alt 自身（不派发，只推进位），再按字母，事件必须带 Alt。
    //
    // 这里刻意**不**靠「此刻物理 Alt 确实是按下的」——注入路径不经过物理键盘，跟踪器只认消息流。
    // 若 `handle_syskey` 忘了对 `VK_MENU` 调 `mods.apply`，紧随其后的字母就会错报不带 Alt。
    sink.records.clear();
    send_syskey(hwnd, true, VK_MENU);  // Alt 自身：只推进态
    const std::size_t after_alt = sink.records.size();
    check(after_alt == 0U, "WM_SYSKEYDOWN(VK_MENU) does NOT dispatch a KeyEvent (Alt itself is modifier-only): " +
                               aurora_verify::format_uint(after_alt));
    send_syskey(hwnd, true, 'B');
    const bool b_got = sink.records.size() == 1U;
    check(b_got, "the letter after Alt still reaches the handler: " + aurora_verify::format_uint(sink.records.size()));
    if (b_got) {
        check(sink.records.front().has_alt(),
              "modifier advance precedes dispatch: the letter KeyEvent carries the Alt bit");
    }
    send_syskey(hwnd, false, VK_MENU);  // 松开 Alt，为下一段清场

    // 3. F10（系统菜单键）不派发。
    sink.records.clear();
    send_syskey(hwnd, true, VK_F10);
    check(sink.records.size() == 0U,
          "WM_SYSKEYDOWN(VK_F10) does NOT dispatch a KeyEvent (system-menu key is system semantics): " +
              aurora_verify::format_uint(sink.records.size()));

    // 4~5. 「消费即止 / 未消费回落 DefWindowProcA」在**注入路径下不可证**，如实记 SKIP。
    //
    //    裸 Win32 对照实测（不经 Aurora、直接 `DefWindowProcA`）：注入 `WM_SYSKEYDOWN` 的
    //    Alt+F4 / Alt+Space / Alt+Enter 一个 `WM_SYSCOMMAND` 都不产生。原因是
    //    `DefWindowProcA` 判「当前是否按住 Alt」读的是 `GetKeyState(VK_MENU)` 这一**物理**态，
    //    而 `SendMessage` 注入只改消息流、不改物理键态 ⇒ 恒走「没按 Alt」分支。
    //    故无论 Aurora 消费与否，回落与否在注入下观感相同 —— 这不是库的缺陷，是该判据的
    //    通道限制。硬凑一个「返回值非零」之类的判据只会得到恒真的空转断言，故此处记 SKIP
    //    而非 PASS，并把该语义整体交给 --interactive 人工段（真键盘 + 真前台）。
    //
    //    可注入证明的部分已在上面覆盖：Alt 组合确实进入了派发链（(1)~(3) 与 (6)），
    //    故「未消费时交回系统」的前半段成立；后半段「消费时不交回」只能真机证明。
    skip(
        "consumed-blocks-SC_CLOSE / unconsumed-reaches-SC_SYSMENU: DefWindowProcA reads "
        "GetKeyState(VK_MENU) (physical Alt state), which SendMessage injection cannot set; verified with a "
        "bare Win32 control that no WM_SYSCOMMAND is produced on this path either way. Covered by the "
        "--interactive Alt+F4 stage instead.");
    // 消费开关本身仍需被走到一次，证明「消费态下事件仍到达处理器」（判据不成立但路径要覆盖）。
    sink.consume = true;
    sink.records.clear();
    send_syskey(hwnd, true, VK_F4);
    check(sink.records.size() == 1U, "consumed Alt+F4: the handler still saw the event (consume path is reached): " +
                                         aurora_verify::format_uint(sink.records.size()));
    sink.consume = false;

    // 6. 释放阶段同样进派发链。
    sink.records.clear();
    send_syskey(hwnd, false, 'D');
    const bool up_got = sink.records.size() == 1U;
    check(up_got,
          "WM_SYSKEYUP reaches the handler as one KeyEvent: " + aurora_verify::format_uint(sink.records.size()));
    if (up_got) {
        check(!sink.records.front().is_down, "the release KeyEvent action is KeyAction::Up");
    }

    return failures == 0 ? 0 : 1;
}

/// @brief 人工段：真键盘按 Alt+F4，由窗口子类**自动判读**「消费则不关窗、未消费则关窗」。
///
/// 注入通道证不了这一段（`DefWindowProcA` 读 `GetKeyState(VK_MENU)` 物理态，见上方子类注释），
/// 故必须真键盘。子类把 `WM_SYSCOMMAND` 记下但**不交回**，于是「关窗」不会真的发生，
/// 操作者按错一次也不必重跑 —— 判据由 `g_syscommands` 自动给出。
/// @return 该段退出码：0 两段判据均成立；1 否则。
auto run_interactive_stage() -> int {
    emit("---- interactive segment ----");

    KeySink sink;
    aurora::WindowStyleOptions style{};
    style.frameless = true;
    style.resizable = false;
    aurora::Win32Host host{320, 240, "Aurora Win32 syskey live probe (interactive)", style};
    host.set_event_handler([&sink](aurora::Event &e) { sink(e); });
    HWND hwnd = hwnd_of(host);
    if (hwnd == nullptr) {
        emit("result: no native window handle (exit 2)");
        return 2;
    }
    if (!install_subclass(hwnd)) {
        emit("result: SetWindowLongPtr(GWLP_WNDPROC) failed, cannot observe system commands (exit 2)");
        return 2;
    }

    // 第 1 段：未消费态。系统关闭请求必须真的产生 —— 这证明「未消费时回落 DefWindowProcA」。
    sink.consume = false;
    g_syscommands.clear();
    emit("Step 1 (unconsumed path): press Alt+F4 NOW on the probe window.");
    emit("  Expected: the window stays open (the probe intercepts SC_CLOSE), and the log below shows it arrived.");
    host.wait_events(30.0);  // 阻塞等消息，最多 30s；操作者按键即提前返回
    const bool close_seen = std::find(g_syscommands.begin(), g_syscommands.end(), kScClose) != g_syscommands.end();
    check(close_seen,
          "unconsumed Alt+F4: WM_SYSCOMMAND(SC_CLOSE) reached the system (fell through to DefWindowProcA), "
          "commands seen = " +
              aurora_verify::format_uint(g_syscommands.size()));
    if (!close_seen) {
        emit(
            "  Hint: no SC_CLOSE within 30s. Either the keypress did not reach this window, or the session is "
            "locked (locked sessions deliver no keyboard input). Also verify the window has focus.");
    }

    // 第 2 段：消费态。系统关闭请求必须**不**产生 —— 这证明「消费即止」。
    sink.consume = true;
    g_syscommands.clear();
    sink.records.clear();
    emit("Step 2 (consumed path): the handler now consumes every KeyEvent. Press Alt+F4 NOW again.");
    emit("  Expected: the handler sees the event, but NO SC_CLOSE is produced (Aurora short-circuited).");
    host.wait_events(30.0);
    const bool close_seen_again =
        std::find(g_syscommands.begin(), g_syscommands.end(), kScClose) != g_syscommands.end();
    check(sink.records.size() >= 1U,
          "consumed Alt+F4: the handler saw the event: " + aurora_verify::format_uint(sink.records.size()));
    check(!close_seen_again,
          "consumed Alt+F4: no WM_SYSCOMMAND(SC_CLOSE) reached the system (Aurora short-circuited), commands seen = " +
              aurora_verify::format_uint(g_syscommands.size()));
    sink.consume = false;

    return failures == 0 ? 0 : 1;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与 demo 入口同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Win32 syskey live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    aurora::init_console();  // Windows 控制台切 UTF-8，避免表格错行

    emit("== Aurora Win32 syskey live probe ==");
    emit("auto segment: Alt+letter dispatch / Alt-self is modifier-only / F10 exempt / Up also dispatches;");
    emit("             the consumed-vs-fallthrough pair is SKIP here (GetKeyState is physical) -- see --interactive");

    const int auto_code = run_auto_stage();
    if (auto_code != 0) {
        return auto_code;
    }
    if (!interactive) {
        emit("Skipping the interactive stage: Alt+F4 behaviour needs a human keypress (re-run with --interactive).");
        emit("PASS: Win32 syskey dispatch acceptance passed (auto stage)");
        return 0;
    }

    const int human_code = run_interactive_stage();
    if (human_code == 0) {
        emit("PASS: Win32 syskey dispatch acceptance passed (auto stage; interactive stage observed by hand)");
    }
    return human_code;
}

// ---------------------------------------------------------------------------
// 已知差距（供后续任务决策，本探针不掩盖）
//   1. `SendMessageW` 注入不经过物理键盘，故「修饰态推进早于派发」靠消息流自身的顺序证明
//      （先 `VK_MENU` 后字母），不依赖操作者真的按住 Alt。
//   2. Alt+F4 的人工段只能目视核对：消费态下窗口不关是**否定性**断言，自动段无法替代。
//   3. 菜单助记键（Alt+ underlines）与系统菜单的**视觉**行为（菜单是否弹出）本探针不核，
//      只核「消息是否交回 DefWindowProcA」这一层。
//   4. 本轮仅 Win32 收敛；X11 / Wayland / GLFW 侧的 Alt 组合是否已作为常规 key press 派发
//      需各自复核（见 codespec/specification/05-event-navigation.md §2.2）。
// ---------------------------------------------------------------------------
