/* Windows 系统双击速度真机验收探针（非 CTest）
// ============================================================================
// 为什么必须真机：`src/aurora/event/click_config.cpp` 的 Windows 分支直接调
// `GetDoubleClickTime()`（user32，毫秒），返回系统真实值；失败（极罕见）回退到库级常量。
// 该路径此前只在 Windows/clang-cl 下编译通过，**从未在真机实跑过**——「连击窗口默认跟随系统
// 双击速度」在 Windows 上是否真生效，此前是未经观测的。本探针就是这条路径在真实 Windows 上的
// 机器级物证 + 交叉见证。
//
// 关于「是否真跟随了系统」：公共 API 只暴露 `os_double_click_ms()`（一个标量），不暴露
// 「当前是从哪个来源取的」。故本探针自己 **独立** 再调一遍 `GetDoubleClickTime()` 作为见证值，
// 与库返回值比对：库若被改坏成「恒返回常量」，见证值仍读回真实系统值，二者不一致即 FAIL——
// 这正是无头 CI 抓不到、却决定「连击手感是否跟随系统」的那条回归。
//
// 自动段逐项验收（不需要人工看屏幕）：
//   ① 环境核对：GetDoubleClickTime() 是否可调用、系统设定值是多少——打印作诊断，不作判据。
//   ② 范围合理性：os_double_click_ms() ∈ [1, 10000]，否则 FAIL。
//   ③ 跟随系统断言：见证值可读（>0）时，断言 库返回值 == 见证值（证明真跟随系统）；
//        见证值不可读（GetDoubleClickTime 返回 0，极罕见）时，仅 WARN 提示。
// 人工段（--interactive）：无——本探针全自动化即可得出结论（系统值不依赖屏幕观察）。
//
// 退出码：
//   0  自动段全部通过（WARN 不影响退出码）
//   1  自动段存在 FAIL（范围越界 / 未跟随系统）
//
// 构建（目标由 cmake/AuroraVerify.cmake 注册；需 -DAURORA_BUILD_VERIFY_TOOLS=ON）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release ^
//         -DAURORA_BUILD_VERIFY_TOOLS=ON -DAURORA_BACKEND_WIN32=ON
//   cmake --build build-verify --target aurora_verify_windows_double_click
//   ./build-verify/aurora_verify_windows_double_click               # 自动段
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_WINDOWS)
#error "aurora_verify_windows_double_click can only be built on Windows"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

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

/// @brief 独立见证：再调一遍 `GetDoubleClickTime()`，作为库返回值的对照。
/// @return 系统可读到双击速度（>0）时返回该值；极罕见地返回 0（调用失败）时返回 nullopt。
/// @note 与 `src/aurora/event/click_config.cpp` 的 Windows 分支 **同机制、独立调用栈**，
///       专门用于抓「库被改坏成恒返回常量」这类回归——若库坏了，本函数仍读回真实值，比对即 FAIL。
[[nodiscard]] auto witness_double_click_ms() -> std::optional<std::uint32_t> {
    const UINT ms = ::GetDoubleClickTime();
    if (ms == 0U) {
        return std::nullopt;  // 极罕见：调用失败，库同样回退常量
    }
    return static_cast<std::uint32_t>(ms);
}

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现，捕获反而把失败压成 0。
// 口径与 tools/verify/ 其余探针、examples/ 各 demo 入口同。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Windows system double-click speed live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    static_cast<void>(interactive);  // 本探针全自动化，--interactive 仅保持旗标兼容性

    emit("== Aurora Windows system double-click speed live probe ==");
    emit("verifies aurora::os_double_click_ms() follows the system setting (GetDoubleClickTime)");

    // ---- ① 环境核对 ----
    emit("");
    emit("[ENV] GetDoubleClickTime() availability (the library reads the system value through this at runtime):");
    const std::optional<std::uint32_t> witness = witness_double_click_ms();
    emit(std::string("       GetDoubleClickTime()    : ") + (witness.has_value()
                                                                 ? (std::to_string(*witness) + " ms")
                                                                 : "returned 0 (call failed, unexpected on Windows)"));

    // ---- ② 范围合理性 ----
    emit("");
    const std::uint32_t value = aurora::os_double_click_ms();
    emit("os_double_click_ms() = " + std::to_string(value) + " ms");
    check(value >= 1U && value <= 10000U,
          "returned value is within sane bounds [1, 10000] ms (got " + std::to_string(value) + ")");

    // ---- ③ 跟随系统断言（交叉见证） ----
    emit("");
    if (witness.has_value()) {
        emit("[WITNESS] GetDoubleClickTime() read independently = " + std::to_string(*witness) + " ms");
        check(value == *witness, "os_double_click_ms() follows the system value read from GetDoubleClickTime (" +
                                     std::to_string(value) + " == " + std::to_string(*witness) + ")");
    } else {
        emit(
            "[WITNESS] GetDoubleClickTime() returned 0 on this machine (unexpected) -> "
            "expecting clean fallback to the library constant");
        warn(
            "GetDoubleClickTime() could not be read; cannot witness against the system value "
            "on this machine (investigate the Windows session)");
    }

    emit("");
    if (failures() > 0) {
        emit("result: " + std::to_string(failures()) + " FAILURE(S), " + std::to_string(warnings()) +
             " warning(s) (exit 1)");
        return 1;
    }
    emit("result: ALL PASS, " + std::to_string(warnings()) + " warning(s) (exit 0)");
    return 0;
}
