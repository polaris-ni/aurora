// tools/verify/win32_os_hotkey_live_probe.cpp — Win32 OS 级全局热键真机验收探针（非 CTest）。
//
// 覆盖：Windows 上 `OsHotkeyRegistry`（include/aurora/app/os_hotkey.h）的整条接线——
// `RegisterHotKey` / `UnregisterHotKey` + 隐藏消息窗口（`HWND_MESSAGE`）接 `WM_HOTKEY`，
// 以及「命中只入队、须由 `drain_pending()` 排空才执行动作」这条核心语义。
//
// 为什么必须有本探针（无头 CI 证明不了的部分）：
//   * 「应用无焦点也触发」只能在真机上由人把焦点切到**别的进程**的窗口后按键才能证明；
//   * 「排空才执行动作」要求一个真正的本机消息泵 + 帧循环节奏，CI 里这两样都没有。
//
// 原理：
//   1) `OsHotkeyRegistry` 构造时经 `internal::add_message_hook` 挂钩子，并首次
//      `ensure_message_window()` 创建属于本线程的隐藏消息窗口——故注册、注销、抽消息必须在
//      **同一个线程**（本探针就是主进程主线程）完成，跨线程收不到 `WM_HOTKEY`；
//   2) `WM_HOTKEY` 到达隐藏窗口的窗口过程后只把命中的 ID 推入内部队列，`drain_pending()`
//      才取出并执行动作。探针因而自己跑「PeekMessage + DispatchMessage + drain_pending」的
//      循环，与真实帧循环同节奏；
//   3) OS 侧是否真的占用 / 真的释放，只能用失败面反证：同一组合重复注册会被本注册表判重；
//      `clear()` 之后以**新 ID**（内部计数器自 1 递增、不复用）重新注册同一组合若仍成功，
//      则旧的 OS 注册确已释放——否则 Windows 会以组合已被占用为由拒绝。
//
// 构建（方式 ① 推荐，Windows 上无需手写编译命令）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_os_hotkey --config Release
//   build-verify/Release/aurora_verify_win32_os_hotkey.exe [--interactive]
// 构建（方式 ② 手写 MinGW 命令，须先有一个已构建好的 Win32 构建目录 build）：
//   g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic
//       -I include -I tools/verify -I tools/include -DAURORA_BACKEND_WIN32 -DNOMINMAX
//       tools/verify/win32_os_hotkey_live_probe.cpp -o probe.exe
//       build/libaurora.a -luser32 -lgdi32 -lole32 -lshell32 -lcomctl32 -lwinpthread
//   （缺符号时按链接报错补 `-l`，具体系统库随本机库内编译进的后端而变。）
//
// 运行：命令行直接跑（不要后台常驻：本探针会占用系统级组合，进程退出才释放）。
//       控制台会话须**已解锁且活动**——锁屏 / 会话隔离时 OS 不投递 `WM_HOTKEY`。
//
// 自动段（无需人工按键）：
//   1. 平台能力核对：`OsHotkeyRegistry::enabled()` 为 true。不为真时打印 FAIL 行并归为环境
//      不可用（退出码 2）——本探针只在 Win32 上构建，该分支在真机上不应出现。
//   2. 合法注册：`add(Ctrl+Alt+Shift+A, 动作)` 返回 Ok，句柄 id 非 0，`count()` 变为 1。
//   3. 重复组合：同一组合第二次 `add()` 返回 `OsHotkeyRegisterFailed`（本注册表内判重）。
//   4. 无效主键：`KeyCode::Unknown` 组合返回 `OsHotkeyRegisterFailed`（详情写明无主键）。
//   5. 注销：`remove(h)` 返回 true、`count()` 回 0；再次 `remove(h)` 返回 false（重复释放安全）。
//   6. 无效句柄：`remove(OsHotkeyHandle{})`（id 为 0）返回 false。
//   7. 批量：两条不同组合 → `count()` 为 2 → `clear()` → `count()` 回 0，且能以新 ID 重新注册
//      同一组合成功（反证 OS 侧确已释放，而非只清了内存表）。
//   8. `drain_pending()` 在无命中时返回 0、且不阻塞。
//   9. 测试注入面：`install_test_backend(false)` → `enabled()` 转 false、且此期间 `add()` 返回
//      `OsHotkeyRegisterFailed`（覆盖「平台不支持」降级路径）→ `remove_test_backend()` 返回
//      true 且 `enabled()` 恢复。该注入点受双宏裁切，返回 false 时按 SKIP 而非 FAIL。
// 人工段（--interactive，必须人按键）：
//   注册 `Ctrl+Alt+Shift+F1` / `F2` / `F3` 三条，提示操作者把焦点切到**别的进程**的窗口后逐个
//   按下；主循环「抽消息 + 排空 + 短睡眠」最多 30s，全部命中即 PASS。每条组合除「是否命中」
//   外还单独验收核心卖点：**命中瞬间前台窗口属于别的进程**（无焦点也触发的机器证据）。
// 退出码：0 全过；1 自动段或人工段断言失败；2 环境不可用（本平台无全局热键后端）。

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_os_hotkey can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
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

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>

#include "aurora/app/os_hotkey.h"
#include "aurora/core/error_codes.h"
#include "verify_args.h"
#include "verify_print.h"

namespace {

// 人工段等待按键的上限（毫秒）。30s 足够操作者切焦点并按三下，又不至于让人干等。
constexpr int AURORA_INTERACTIVE_TIMEOUT_MS = 30000;
// 每轮排空之间的睡眠毫秒数：留出 CPU，同时保证超时窗内仍有数千轮排空机会。
constexpr int AURORA_PUMP_SLICE_MS = 5;
// 人工段取证用的组合条数（同时也是 `hits` / `elsewhere` 两个数组的宽度）。
constexpr std::size_t AURORA_INTERACTIVE_COMBO_COUNT = 3U;

/// @brief 累计的断言失败条数（自动段与人工段共用），决定退出码 1。
///        函数局部静态 + 访问器收敛，避免命名空间级可变全局量。
auto failures() -> int & {
    static int count = 0;
    return count;
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

/// @brief 记录一条 SKIP 结论：环境前提不成立，既不判通过也不判失败。
/// @param label 跳过项的说明（ASCII）。
auto skip(const std::string &label) -> void { emit(std::string("[SKIP] ") + label); }

/// @brief 构造「Ctrl + Alt + Shift + 主键」这一冷门组合。
///
/// 三个修饰键齐上是为了躲开绝大多数 IME / 录屏工具 / IDE 的默认全局快捷键——组合一旦被别的
/// 进程先占，本探针的判别会退化成「无法注册」，那是环境噪声而非库缺陷。
/// @param key 主键。
/// @return 带三修饰键的键组合。
auto ctrl_alt_shift(aurora::KeyCode key) -> aurora::KeyCombo {
    const auto mods = aurora::ModifierKey::Control | aurora::ModifierKey::Alt | aurora::ModifierKey::Shift;
    return aurora::KeyCombo{mods, key};
}

/// @brief 抽取并派发一条本机 Win32 消息（不阻塞）。
///
/// 关键：`WM_HOTKEY` 是**投递**到隐藏消息窗口所属线程队列的，只有 `DispatchMessage` 把它交给
/// 该窗口的窗口过程，其中的消息钩子才会把命中 ID 入队。只 `PeekMessage` 抽出来不派发等于把
/// 命中丢掉；而少了这层钩子也无从验证「`drain_pending()` 才是执行点」这条语义。
/// @return 本轮确实取到并派发过一条消息时为 true；队列已空时为 false。
[[nodiscard]] auto pump_one_message() -> bool {
    MSG msg{};
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) == 0) {
        return false;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    return true;
}

/// @brief 跑一轮「把消息队列抽干 + 排空热键队列」，模拟真实帧循环的一帧。
///
/// 顺序不能颠倒：先派发（`WM_HOTKEY` 入队），再 `drain_pending()`（出队并执行动作）。反过来的
/// 话每条命中都人为延迟一帧；不派发则队列恒空，永远等不到命中。
/// @param registry 待排空的热键注册表。
/// @return 本轮实际执行了动作的热键条数（命中但动作为空的不计）。
auto pump_and_drain(aurora::OsHotkeyRegistry &registry) -> std::size_t {
    while (pump_one_message()) {
        // 队列非空就一直抽：一次按键往往伴随若干条伴随消息，抽一半会留下不均匀的帧节奏。
    }
    return registry.drain_pending();
}

/// @brief 前台窗口是否属于**别的**进程（即「本应用此刻没有焦点」）。
///
/// 用于给人工段一条机器证据：命中发生时若前台窗口属于别的进程，「无焦点也触发」才真的成立，
/// 而不是「操作者在这边的控制台里按了键」。
/// @return 前台窗口属于别的进程时为 true；取不到前台窗口（无桌面会话）时也返回 true——无从
///         归属时不该据此否认「无焦点」。
[[nodiscard]] auto focus_is_elsewhere() -> bool {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return true;
    }
    DWORD owner_pid = 0;
    GetWindowThreadProcessId(foreground, &owner_pid);
    return owner_pid != GetCurrentProcessId();
}

/// @brief 判定一次 `add()` 的结果是不是预期的 `OsHotkeyRegisterFailed`，并打印一行结论。
///
/// 失败口径集中在 errors.toml 的 `os-hotkey-register-failed` 一项：`add()` 的**全部**失败路径
/// （无后端 / 无主键 / 注册表内重复 / OS 侧拒绝）都走同一个错误码，区别只在 `message` 的
/// detail，故这里只读 code_enum、把 message 原样打出来供人判因。
/// @param scenario 场景描述（打印在结论行头部）。
/// @param result 待判定的注册结果。
auto expect_register_failed(const std::string &scenario, const aurora::Result<aurora::OsHotkeyHandle> &result) -> void {
    const bool expected = !result.ok() && result.error().code_enum == aurora::ErrorCode::OsHotkeyRegisterFailed;
    const std::string detail = result.ok() ? std::string("got Ok instead") : result.error().message;
    check(expected, scenario + ": rejected with OsHotkeyRegisterFailed (" + detail + ")");
}

/// @brief 自动段：不需要人工按键的全部判据（见本文件头注释的「自动段」清单）。
/// @return 该段的退出码：0 通过；1 有断言失败；2 环境不可用（无 OS 全局热键后端）。
auto run_auto_stage() -> int {
    emit("---- auto segment ----");

    aurora::OsHotkeyRegistry registry;

    // 1. 平台能力核对。Win32 上 `enabled()` 恒真；为假意味着走了非 Windows 的降级实现，
    //    此时后面 8 项全部失去意义（每条都会以「无后端」为由拒绝），故就地按环境不可用收尾。
    const bool supported = registry.enabled();
    check(supported, "platform capability: OsHotkeyRegistry::enabled() is true on Win32");
    if (!supported) {
        emit("result: no OS global hotkey backend on this platform (exit 2)");
        return 2;
    }

    // 2. 合法注册。
    std::size_t fired = 0;
    const auto first = registry.add(ctrl_alt_shift(aurora::KeyCode::A), [&fired]() { ++fired; });
    const bool registered = first.ok();
    check(registered, "valid registration: add(Ctrl+Alt+Shift+A) returns Ok");
    if (registered) {
        check(first.value().id != 0U, "handle id is non-zero (0 is reserved as the invalid handle)");
        check(registry.count() == 1U,
              "count() is 1 after the first successful add(): " + aurora_verify::format_uint(registry.count()));
    } else {
        emit(
            "Hint: the OS refused Ctrl+Alt+Shift+A. Typical cause: another process already owns the combination "
            "(IMEs, screen recorders, IDE global shortcuts) or this session has no interactive desktop.");
    }

    // 3. 重复组合（本注册表内判重，与 OS 侧无关）。
    expect_register_failed("duplicate combination",
                           registry.add(ctrl_alt_shift(aurora::KeyCode::A), std::function<void()>{}));

    // 4. 无效主键：`Unknown` 无法映射到任何原生虚拟键。
    expect_register_failed("invalid primary key (KeyCode::Unknown)",
                           registry.add(ctrl_alt_shift(aurora::KeyCode::Unknown), std::function<void()>{}));

    // 5. 注销 + 重复释放。
    if (registered) {
        const aurora::OsHotkeyHandle handle = first.value();
        check(registry.remove(handle), "remove(handle) returns true for a registered hotkey");
        check(registry.count() == 0U,
              "count() is back to 0 after removing the only hotkey: " + aurora_verify::format_uint(registry.count()));
        check(!registry.remove(handle), "remove(handle) again returns false (repeated release is safe)");
    }

    // 6. 无效句柄（id 为 0 的哨兵）。
    check(!registry.remove(aurora::OsHotkeyHandle{}), "remove(OsHotkeyHandle{}) with id 0 returns false");

    // 7. 批量注册 + clear() 的 OS 侧释放反证。
    const auto combo_b = ctrl_alt_shift(aurora::KeyCode::B);
    const auto combo_c = ctrl_alt_shift(aurora::KeyCode::C);
    const auto second = registry.add(combo_b, std::function<void()>{});
    const auto third = registry.add(combo_c, std::function<void()>{});
    check(second.ok() && third.ok(), "batch: two distinct combinations register Ok");
    check(registry.count() == 2U,
          "count() is 2 with two live registrations: " + aurora_verify::format_uint(registry.count()));
    registry.clear();
    check(registry.count() == 0U, "clear() brings count() back to 0: " + aurora_verify::format_uint(registry.count()));
    // 反证的核心：`next_hotkey_id()` 只递增不复用，故这条 `add()` 一定用的是新 ID。若 OS 侧仍占
    // 着该组合，`RegisterHotKey` 会以组合已被占用为由失败，amina 这里就该拿到错误而非 Ok。
    expect_register_failed("none", aurora::OsHotkeyHandle{});  // 占位：真实断言见下一行
    const auto reuse = registry.add(combo_b, std::function<void()>{});
    check(reuse.ok(), "clear() really released the OS-side registration (re-adding the same combination succeeds)");
    if (!reuse.ok()) {
        emit("  detail: " + reuse.error().message);
    }

    // 8. `drain_pending()` 在无命中时返回 0 且不阻塞（到这里为止没有任何按键发生）。
    check(registry.drain_pending() == 0U, "drain_pending() returns 0 and does not block when nothing hit");
    check(fired == 0U, "no action executed before any hotkey was pressed");

    // 9. 测试注入面：「平台不支持」这条降级路径在受支持的平台上只能靠注入复现。
    // 注意：注入期间不得调 `clear()`——inert 后端会跳过 OS 侧注销，那会把真实注册漏在系统里。
    const bool injected = aurora::OsHotkeyRegistry::install_test_backend(false);
    if (!injected) {
        skip(
            "test backend injection is unavailable in this build (both AURORA_ENABLE_DEBUG and "
            "AURORA_ENABLE_TEST_HOOKS are required); the degraded platform path is not covered here");
    } else {
        check(!registry.enabled(), "install_test_backend(false) turns enabled() off");
        expect_register_failed("degraded platform path (no backend)",
                               registry.add(ctrl_alt_shift(aurora::KeyCode::D), std::function<void()>{}));
        check(aurora::OsHotkeyRegistry::remove_test_backend(), "remove_test_backend() returns true");
        check(registry.enabled(), "enabled() is restored to true after remove_test_backend()");
    }

    return failures() == 0 ? 0 : 1;
}

/// @brief 人工段：注册三条组合，由操作者在**别的进程**的窗口里逐个按下。
///
/// 这是唯一能证明「OS 级」而非「应用内快捷键」的手段——后者只在自家窗口有焦点时命中。每条组合
/// 除「是否命中」外，还单独取证「命中瞬间前台窗口属于别的进程」，即本能力的核心卖点。
/// @return 该段的退出码：0 三条全部命中且均有无焦点证据；1 否则。
auto run_interactive_stage() -> int {
    emit("---- interactive segment ----");

    aurora::OsHotkeyRegistry registry;
    const std::array<aurora::KeyCode, AURORA_INTERACTIVE_COMBO_COUNT> keys{aurora::KeyCode::F1, aurora::KeyCode::F2,
                                                                           aurora::KeyCode::F3};
    std::array<std::size_t, AURORA_INTERACTIVE_COMBO_COUNT> hits{};
    std::array<bool, AURORA_INTERACTIVE_COMBO_COUNT> elsewhere{};

    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto combo = ctrl_alt_shift(keys.at(i));
        auto *hits_ptr = &hits;
        auto *elsewhere_ptr = &elsewhere;
        const auto result = registry.add(combo, [hits_ptr, elsewhere_ptr, i]() {
            ++hits_ptr->at(i);
            // 动作在 `drain_pending()` 内执行，此刻取前台窗口即最接近按键时刻的机器证据。
            elsewhere_ptr->at(i) = elsewhere_ptr->at(i) || focus_is_elsewhere();
        });
        if (!result.ok()) {
            emit("[FAIL] interactive: cannot register " + combo.to_string() + " (" + result.error().message + ")");
            emit(
                "Hint: the combination is likely owned by another process (IME / screen recorder / IDE global "
                "shortcut). Free it or switch to a colder combination, then re-run with --interactive.");
            return 1;
        }
    }

    emit("");
    emit("Please switch focus to ANOTHER application window (Notepad or another terminal), then press these");
    emit("combinations one by one while this console is NOT the foreground window:");
    for (const aurora::KeyCode key : keys) {
        emit("  - " + ctrl_alt_shift(key).to_string());
    }
    emit(std::string("Waiting up to ") + aurora_verify::format_int(AURORA_INTERACTIVE_TIMEOUT_MS / 1000) +
         "s; a global hotkey must fire even though this application has no focus.");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(AURORA_INTERACTIVE_TIMEOUT_MS);
    std::size_t pending_total = keys.size();
    while (pending_total > 0U && std::chrono::steady_clock::now() < deadline) {
        pump_and_drain(registry);
        pending_total = 0U;
        for (const std::size_t n : hits) {
            if (n == 0U) {
                ++pending_total;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(AURORA_PUMP_SLICE_MS));
    }

    int missed = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto combo = ctrl_alt_shift(keys.at(i));
        const bool hit = hits.at(i) > 0U;
        if (!hit) {
            ++missed;
        }
        check(hit, combo.to_string() + " fired " + aurora_verify::format_uint(hits.at(i)) + " time(s)");
        check(hit && elsewhere.at(i),
              combo.to_string() + " fired while the foreground window belonged to another process (no-focus evidence)");
    }
    if (missed > 0) {
        emit(
            "Hint: a missing combination usually means another process grabbed it (common causes: an IME, a screen "
            "recorder, or an IDE global shortcut). Re-run after freeing it, or choose a colder combination.");
    }
    return missed == 0 ? 0 : 1;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与 demo 入口同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Win32 OS global hotkey live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    aurora::init_console();  // Windows 控制台切 UTF-8，避免表格错行

    emit("== Aurora Win32 OS global hotkey live probe ==");
    emit("auto segment: platform capability / register / duplicate / invalid key / remove / clear / drain / injection");

    const int auto_code = run_auto_stage();
    if (auto_code != 0) {
        return auto_code;
    }
    if (!interactive) {
        emit(
            "Skipping the interactive stage: the no-focus claim needs a human to press the keys elsewhere "
            "(re-run with --interactive).");
        emit("PASS: Win32 OS global hotkey wiring acceptance passed (auto stage)");
        return 0;
    }

    const int human_code = run_interactive_stage();
    if (human_code == 0) {
        emit("PASS: Win32 OS global hotkey wiring acceptance passed (auto + interactive)");
    }
    return human_code;
}

// ---------------------------------------------------------------------------
// 已知差距（供后续任务决策，本探针不掩盖）
//   1. 自动段只能验证「注册 / 注销 / 判重 / 降级」这些 API 面，「OS 真的把按键投到了本进程」
//      这一环只能由人工段证明——这正是本探针保留人工段的唯一理由。
//   2. 冷门组合仍可能被别的进程先占（IME / 录屏 / IDE）。此时注册会以
//      `OsHotkeyRegisterFailed` 失败，探针如实报 FAIL 并给出成因提示，不静默改判。
//   3. 测试注入面受双宏裁切：未同时开启两个宏时该项只能 SKIP，降级路径此时未在本机得到覆盖。
// ---------------------------------------------------------------------------
