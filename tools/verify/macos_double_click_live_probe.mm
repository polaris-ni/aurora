/* 系统双击速度 —— macOS 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 为什么必须真机：`src/aurora/event/click_config.cpp` 的 macOS 分支把「读系统双击速度」
// 落在 ObjC++ TU（`src/aurora/event/detail/macos_click_config.cpp`）里的
// `+[NSEvent doubleClickInterval]`。该调用在任何 macOS 上都返回一个数，但**返回的是不是
// 系统真值**、以及库那条腿**有没有真被接进 `os_double_click_ms()`**，只有真机能分辨：
// 无头 CI 里若把分支改回「恒回退库级常量」，单元测试（只覆盖秒 → 毫秒折算的纯函数）全绿。
// 本探针就是这条腿在真实 AppKit 上的机器级物证 + 交叉见证。
//
// 关于「是否真跟随了系统」：公共 API 只暴露 `os_double_click_ms()`（一个标量），不暴露
// 「当前取的是系统值还是回退常量」。故本探针**独立**再读一遍 `[NSEvent doubleClickInterval]`
// 作见证，用探针自带的算式（乘 1000 + 四舍五入 + 亚毫秒夹 1）折算后与库返回值比对：
// 库若被改坏成「恒返回常量」，见证值仍在，二者不一致即 FAIL。
//
// 本机取证的边界（如实登记，勿过度解读）：这台 macOS 26.7.2 虚拟机上，出厂 `+[NSEvent
// doubleClickInterval]` 报 0.5s，与库级回退常量 `AURORA_DEFAULT_CLICK_WINDOW_MS = 500` **数值
// 相同**——此时「库值 == 见证值」这条判据不具备区分力（恒回退常量也能蒙对）。为把这一点显式
// 暴露出来，探针在该情形下额外记一条 WARN，并给出两段加强取证：
//   ④ 消费方接线核对：`EventDispatcher::click_window_ms` / `TouchDispatcher::click_window_ms`
//      的**初值**必须等于 `os_double_click_ms()`（这条抓的是「读了但没喂给派发器」）；
//   人工段（--interactive）：现场挪动 System Settings → Mouse → Double-click speed 滑杆，
//      探针按秒轮询 AppKit 与库返回值；任一值随挪动而变即判 PASS，全窗口不动则记 PENDING
//      （退出码 3）——实测本机写入候选用户默认键（NSGlobalDomain 的
//      `com.apple.mouse.doubleClickInterval`、`com.apple.AppleMultitouchMouse` 的
//      `MouseButtonDivision` 等）都**不**改动该读数，故无法把「跟随滑杆」做成自动判据，
//      只有真实滑杆能判。
//
// 自动段逐项验收（不需要人工看屏幕）：
//   ① 环境核对：打印 AppKit 报出的秒值与库返回值，只作诊断。
//   ② 见证合理性：AppKit 秒值 ∈ (0, 10]，否则 FAIL（秒值不可信时比对无意义）。
//   ③ 跟随系统：库返回值 == 探针独立折算值；AppKit 报非正 / 非有限时改判「优雅回退常量」。
//   ④ 范围 + 消费方接线：返回值 ∈ [1, 10000]，且两派发器初值与之相等。
//
// 退出码：
//   0  自动段全部通过（WARN 不影响退出码；未跑人工段亦不判失败）
//   1  自动段存在 FAIL（秒值不可信 / 未跟随系统 / 未优雅回退 / 派发器初值脱钩）
//   3  人工段已请求但滑杆挪动未被观察到（PENDING，须换真机或换会话复跑）
//
// 构建（目标由 cmake/AuroraVerify.cmake 注册；需 -DAURORA_BUILD_VERIFY_TOOLS=ON）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BUILD_VERIFY_TOOLS=ON -DAURORA_BACKEND_MACOS=ON
//   cmake --build build-verify --target aurora_verify_macos_double_click
//   ./build-verify/aurora_verify_macos_double_click                 # 自动段
//   ./build-verify/aurora_verify_macos_double_click --interactive   # 加人工段（挪滑杆）
//   （头注释里的行尾反斜杠为续行符，故整段用块注释形态以避免 -Wcomment。）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_double_click 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#import <AppKit/AppKit.h>
#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "verify_args.h"

namespace {

/// 失败计数：函数局部静态 + 访问器收敛（探针退出码的依据），避免命名空间级可变全局量。
auto failures() -> int & {
    static int count = 0;
    return count;
}

/// 警告计数：同上。
auto warnings() -> int & {
    static int count = 0;
    return count;
}

/// 人工段「未判定」标记：请求了 --interactive 却观察不到滑杆影响时置位（退出码 3 的依据）。
auto pending_manual() -> bool & {
    static bool flag = false;
    return flag;
}

/// @brief 写一行探针输出到 stdout（走 raw 通道：无前缀、不过级别过滤）。
/// @param text 待输出的一行文本。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 记一项自动判据结论。
/// @param ok 判据是否成立。
/// @param label 结论文案（英文，ASCII）。
auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        ++failures();
    }
}

/// @brief 记一条不判失败的提示（环境差异由真机决定，人工复核）。
/// @param label 提示文案。
auto warn(const std::string &label) -> void {
    emit("[WARN] " + label);
    ++warnings();
}

/// @brief AppKit 的出厂双击间隔（秒）——单一读点，便于逐处口径一致。
/// @return `+[NSEvent doubleClickInterval]` 的当前读数。
[[nodiscard]] auto appkit_interval_seconds() -> double { return static_cast<double>([NSEvent doubleClickInterval]); }

/// @brief 独立见证：探针自带算式把秒值折算成毫秒窗，不复用库内折算函数。
/// @param seconds AppKit 秒值。
/// @return 折算出的毫秒窗；秒值非正或非有限时无值（此时库应回退常量）。
/// @note 与 `src/aurora/event/detail/macos_click_config.h` 的纯函数**同口径、不同实现**：
///       刻意不 include 那枚头，否则库把折算写坏成「恒返回入参兜底」时见证也跟着坏，比对失效。
[[nodiscard]] auto witness_double_click_ms(double seconds) -> std::optional<std::uint32_t> {
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return std::nullopt;
    }
    const double ms = seconds * 1000.0;
    if (ms < 1.0) {
        return 1U;
    }
    return static_cast<std::uint32_t>(std::llround(ms));
}

/// @brief 让 AppKit 有机会处理分布式通知（滑杆改动可能经此送达），不阻塞满窗口。
/// @param milliseconds 本轮泵空的时间预算（毫秒）。
auto pump_run_loop(int milliseconds) -> void {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, /*returnAfterCurrentTimer*/ false));
    }
}

/// @brief 人工段：请现场挪动系统双击速度滑杆，按秒轮询 AppKit 读数与库返回值。
///
/// 判据取「随挪动而变」：任一读数变化即证明该值来自系统设置而非库内常量，且库那条腿实时反映之。
/// 观察不到变化时不判 FAIL——本机实测写入候选用户默认键并不改动 AppKit 读数，故自动段无法构造
/// 出「变化」；把「没观察到」冒充「不跟随」同样是假绿。此时记 PENDING（退出码 3）。
auto run_manual_stage() -> void {
    emit("");
    emit("[MANUAL] Open System Settings -> Mouse -> Double-click speed.");
    emit("[MANUAL] Drag the slider to either end and hold it there while this probe polls (30s).");
    const double baseline_seconds = appkit_interval_seconds();
    const std::uint32_t baseline_ms = aurora::os_double_click_ms();
    emit("[MANUAL] baseline: appkit_seconds=" + std::to_string(baseline_seconds) +
         " library_ms=" + std::to_string(baseline_ms));

    bool moved = false;
    for (int tick = 0; tick < 30 && !moved; ++tick) {
        pump_run_loop(1000);
        const double seconds = appkit_interval_seconds();
        const std::uint32_t value = aurora::os_double_click_ms();
        emit("         t=" + std::to_string(tick + 1) + "s appkit_seconds=" + std::to_string(seconds) +
             " library_ms=" + std::to_string(value));
        if (std::abs(seconds - baseline_seconds) > 1e-6 || std::cmp_not_equal(value, baseline_ms)) {
            moved = true;
        }
    }
    if (moved) {
        check(true, "system double-click value moved while the slider was dragged (follows the OS setting)");
    } else {
        emit("[PENDING] no change observed within the polling window; the launch-time equality check is the "
             "only evidence available on this machine (drag the slider in a live GUI session and re-run)");
        pending_manual() = true;
    }
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("macOS system double-click speed live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    emit("== Aurora macOS system double-click speed live probe ==");
    emit("verifies aurora::os_double_click_ms() reads +[NSEvent doubleClickInterval] (or falls back cleanly)");

    // ---- ① 环境核对 ----
    const double seconds = appkit_interval_seconds();
    emit("");
    emit("[ENV] AppKit +[NSEvent doubleClickInterval] = " + std::to_string(seconds) + " seconds");
    const std::uint32_t value = aurora::os_double_click_ms();
    emit("[ENV] aurora::os_double_click_ms()        = " + std::to_string(value) + " ms");

    // ---- ② 见证合理性 ----
    const bool seconds_sane = std::isfinite(seconds) && seconds > 0.0 && seconds <= 10.0;
    check(seconds_sane, "AppKit reports a sane interval within (0, 10] seconds (got " + std::to_string(seconds) + ")");

    // ---- ③ 跟随系统 / 优雅回退二选一断言 ----
    emit("");
    const std::optional<std::uint32_t> witness = witness_double_click_ms(seconds);
    if (witness.has_value()) {
        emit("[WITNESS] independently converted AppKit interval = " + std::to_string(*witness) + " ms");
        check(std::cmp_equal(value, *witness), "os_double_click_ms() follows the AppKit system value (" +
                                                   std::to_string(value) + " == " + std::to_string(*witness) + ")");
        if (std::cmp_equal(*witness, aurora::AURORA_DEFAULT_CLICK_WINDOW_MS)) {
            warn("system value coincides with the library fallback constant on this machine: the equality check "
                 "is not discriminating here (run --interactive to watch the slider move it)");
        }
    } else {
        emit("[WITNESS] AppKit interval is unreadable (non-positive / non-finite) -> expecting clean fallback");
        check(value == aurora::AURORA_DEFAULT_CLICK_WINDOW_MS,
              "os_double_click_ms() falls back to AURORA_DEFAULT_CLICK_WINDOW_MS when AppKit is unreadable (" +
                  std::to_string(value) + " == " + std::to_string(aurora::AURORA_DEFAULT_CLICK_WINDOW_MS) + ")");
    }

    // ---- ④ 范围 + 消费方接线 ----
    emit("");
    check(value >= 1U && value <= 10000U,
          "returned value is within sane bounds [1, 10000] ms (got " + std::to_string(value) + ")");
    const aurora::EventDispatcher mouse;
    const aurora::TouchDispatcher touch;
    check(std::cmp_equal(mouse.click_window_ms, value),
          "EventDispatcher::click_window_ms is seeded from os_double_click_ms() (" +
              std::to_string(mouse.click_window_ms) + " == " + std::to_string(value) + ")");
    check(std::cmp_equal(touch.click_window_ms, value),
          "TouchDispatcher::click_window_ms is seeded from os_double_click_ms() (" +
              std::to_string(touch.click_window_ms) + " == " + std::to_string(value) + ")");

    // ---- 人工段 ----
    if (interactive) {
        run_manual_stage();
    }

    emit("");
    if (failures() > 0) {
        emit("result: " + std::to_string(failures()) + " FAILURE(S), " + std::to_string(warnings()) +
             " warning(s) (exit 1)");
        return 1;
    }
    if (pending_manual()) {
        emit("result: auto stage PASS, manual stage PENDING, " + std::to_string(warnings()) + " warning(s) (exit 3)");
        return 3;
    }
    emit("result: ALL PASS, " + std::to_string(warnings()) + " warning(s) (exit 0)");
    return 0;
}
