// tools/verify/glfw_dpi_live_probe.cpp — GLFW 后端 DPI 缩放上报真机探针（非 CTest）。
//
// 覆盖：GLFW 后端 `set_scale_change_handler` 的接线与 `detail/glfw_dpi.h` 的换算在真实
// 窗口 + 真实平台事件泵下的行为——无头 CI 证明不了的部分。与本仓既有真机探针同一口径：
// 判据按「本机是否具备前提」分自动段与人工段，前提不成立时如实记 SKIP，不把恒真的绿当验收。
//
// 为什么必须有本探针（CI 证明不了的部分）：
//   * GLFW 的 content scale 回调由 `WM_DPICHANGED`（Win32）/ 输出 enter-leave（X11、Wayland）
//     驱动，CI 的一次性容器窗口不会被拖到另一块屏 ⇒ 回调在 CI 里恒不触发；
//   * 本仓已确认的教训：这类「接线是否漏掉」的分叉在 100% DPI 下不显形（scale 恰 1.0 时
//     两种单位解读重合），只有真机 ≠100% DPI 才炸（`etest_smoke_render` 的 GLFW 腿曾恒红）。
//   ⇒ 自动段**刻意只用注入造变化**（不依赖真机第二块屏）：它验的是「注册通道真的接通、
//     换算真的被调用、去重真的拦得住重复值」——这三点在 100% DPI 下同样能证伪；
//     真跨屏后的端到端效果由人工段覆盖。
//
// 自动段（无需人工，任何 DPI 环境均可跑）：
//   (e) **上报通道真的通 + 去重真的生效**（Windows）：向窗口注入 `WM_DPICHANGED`（GLFW 在该
//       消息里无条件调 `_glfwInputWindowContentScale`），先注**异值** DPI 断言 handler 被调用、
//       收到的值 == 注入 DPI/96 == `scale_factor()`；再注**同值** DPI 断言 handler **不再被调用**
//       （`on_content_scale` 的 `if (next == self->scale) return;` 是唯一拦截点）。同值这一注是
//       去重分支唯一的驱动手段——GLFW 自身不去重，故不需要第二块显示器。
//   (b) 换算自洽：建窗请求的逻辑 dp 逐位回到 `size()`；`framebuffer_size()` == 物理像素；
//       且 `framebuffer_size() == round(size() × scale)`。这三者同源才说明
//       `detail/glfw_dpi.h` 的换算在线（对照 08-tooling.md §8.2 的 Win32 单一真值源口径）。
//   (c) 无误报：构造期那次「按主显示器预估 → 建窗后按真实 content scale 校正」走的是
//       成员直改，若它误走回调路径，上层会在用户未做任何事时收到一次上报并
//       force_full_redraw —— 一次无谓的全量重排。判据：稳态下上报计数恒为 0。
// 人工段（`--interactive`，需第二块不同缩放的显示器）：
//   (d) 跨屏拖动：把窗口从当前屏拖到缩放比不同的屏 → 断言 (1) handler 收到新的 scale，
//       (2) 该新值 == `scale_factor()`，(3) `(2)` 发生后一帧的帧缓冲物理尺寸 ==
//       新逻辑尺寸 × 新 scale。人工不做则记 PENDING MANUAL（退出码 3），不记 PASS——
//       注入冒充真跨屏会让判据恒真（与 `win32_insert_key_live_probe` 同一纪律）。
//
// 构建：
//   cmake -S . -B build-verify -DAURORA_BACKEND_GLFW=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_glfw_dpi
// 运行：build 目录下 ./aurora_verify_glfw_dpi [--interactive]
//
// 退出码：0 全过；1 有断言失败；2 环境不可用（无显示 / GLFW 初始化失败）；
// 3 人工段待执行（自动段已过，须人工跨屏验收，如实归类而非记 PASS）。

#include <cmath>
#include <iostream>
#include <string>

#include "aurora/aurora.h"
#include "e2e/harness.h"  // E2E 驱动内核：建窗统一经 e2e::open（RAII + 失败翻译）
#include "verify_args.h"

#if defined(AURORA_PLATFORM_WINDOWS)
#include <windows.h>
#endif

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

int failures = 0;

auto check(bool passed, const std::string &label) -> void {
    emit(std::string("[") + (passed ? "PASS" : "FAIL") + "] " + label);
    if (!passed) {
        failures++;
    }
}

auto nearly(float a, float b, float tol = 0.01F) -> bool { return std::fabs(a - b) <= tol; }

}  // namespace

auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("GLFW DPI scale-report live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    emit("== aurora verify: GLFW DPI scale reporting ==");

    // ---- 窗口（建窗经 E2E 内核 e2e::open）----
    // 尺寸取 480x320 dp：非 100% DPI 下窗口有足够像素让「物理 vs 逻辑」的差异可分辨
    // （150% 屏上物理 720x480，差 240x160，远超浮点容差）。
    constexpr int kLogicalW = 480;
    constexpr int kLogicalH = 320;
    aurora::e2e::WindowSpec spec;
    spec.backend = aurora::e2e::Backend::Glfw;
    spec.width = kLogicalW;
    spec.height = kLogicalH;
    spec.title = "aurora-verify-glfw-dpi";
    spec.visibility = aurora::WindowVisibility::Normal;
    auto session = aurora::e2e::open(spec);
    if (!session.ok()) {
        AURORA_LOG_ERROR("verify", "open(Glfw) failed: " + session.reason() + " (no display / GLFW init failed)");
        return 2;
    }
    auto &win = session.window();

    // 挂一棵最小控件树：`Session::pump` 要求已 mount（`require_mounted`），否则每帧直接
    // 返回错误。⚠️ 错误**不可吞**——吞掉后 `begin_frame` 从未执行、`size()` 停在 Impl 成员
    // 初值、帧缓冲恒 0×0，症状看起来像「尺寸换算全错」实则是探针自己没跑起来。
    // 用 `Row`（具体控件）而非抽象基类：根节点只提供可布局实体，不参与任何尺寸判据。
    aurora::Node root = aurora::Node{aurora::Row{}};
    session.mount(root);

    const float scale = win.surface().scale_factor();
    emit("  measured scale_factor() = " + std::to_string(scale));
    emit("  (100% DPI on this host: the scale-*change* criteria cannot rotate; see manual segment)");

    // ---- 判据 (c)：回调注册面 ----
    int report_count = 0;
    float reported_scale = 0.0F;
    win.surface().set_scale_change_handler([&report_count, &reported_scale](float s) -> void {
        report_count++;
        reported_scale = s;
    });

    // 推几帧，覆盖「构造期校正之后」的稳态：此时不应有任何上报。
    // ⚠️ pump 的错误**不可吞**——吞掉后 `begin_frame` 从未执行、`size()` 停在 Impl 成员
    // 初值、帧缓冲恒 0×0，症状看起来像「尺寸换算全错」实则是探针自己没跑起来。
    const auto pumped = session.pump(3);
    check(pumped.ok(), "(c0) frame pump succeeded" + (pumped.ok() ? std::string{} : ": " + pumped.error().message));
    if (!pumped.ok()) {
        emit("== frame pump failed; size criteria would be vacuous, aborting ==");
        return 1;
    }
    check(report_count == 0,
          "(c1) no spurious scale-change report after construction (count=" + std::to_string(report_count) + ")");

    // ---- 判据 (e)：上报通道真的通（注入造变化，验回调被调用）----
    // (c1) 只能证明「注册不抛、且无源时不误报」——把 override 整个删掉它照样全绿，
    // 属空转判据（变异实测）。本条才是接线本身的判据：造一次真实的缩放变化，
    // 断言 handler 收到、且收到的值与 `scale_factor()` 一致。
    //
    // Windows 上 GLFW 由 `WM_DPICHANGED` 驱动 content scale 回调
    // （`third_party/glfw/src/win32_window.c:1198`：先 SetWindowPos 再
    // `_glfwInputWindowContentScale(xscale, yscale)`，其中 scale = DPI / 96）。
    // 故注入该消息即可确定性触发——这是**通道**判据，不需要第二块显示器。
    //
    // ⚠️ 本条**不**证明跨屏时的端到端正确性：注入复现不了「合成器按新 DPI 重新协商
    // 帧缓冲与窗口尺寸」那一段（GLFW 在 Win32 分支会真的按 suggested rect 改窗口）。
    // 那部分由人工段 (d) 覆盖。
#if defined(AURORA_PLATFORM_WINDOWS)
    auto *hwnd = static_cast<HWND>(win.surface().native_handle());
    if (hwnd == nullptr) {
        check(false, "(e0) native_handle() returned HWND (needed to inject WM_DPICHANGED)");
    } else {
        check(true, "(e0) native_handle() returned a usable HWND");
        // 目标 DPI 取一个**与当前不同**的值，否则「变化才上报」的去重会把这次注入
        // 当成无变化而静默返回，判据就变成了对去重的重复断言。
        const UINT current_dpi = static_cast<UINT>(std::lround(scale * 96.0F));
        const UINT inject_dpi = (current_dpi > 120U) ? 96U : 144U;
        // WM_DPICHANGED 的 wParam：HIWORD = X DPI、LOWORD = Y DPI（见上引 GLFW 换算）。
        // lParam 需为有效 RECT —— GLFW 会按它 SetWindowPos，取当前窗口矩形即可（不真移动）。
        RECT cur{};
        if (GetWindowRect(hwnd, &cur) == 0) {
            check(false, "(e0b) GetWindowRect failed (cannot build WM_DPICHANGED lParam)");
        } else {
            const int before_count = report_count;
            SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(inject_dpi, inject_dpi), reinterpret_cast<LPARAM>(&cur));
            // 回调在消息处理期间同步触发；再泵一帧让事件泵把残留消息走完。
            (void)session.pump(1);
            const bool fired = report_count > before_count;
            check(fired, "(e1) scale-change handler fired on injected WM_DPICHANGED (count=" +
                             std::to_string(report_count) + ")");
            if (fired) {
                const float expect = static_cast<float>(inject_dpi) / 96.0F;
                check(nearly(reported_scale, expect), "(e2) reported scale == injected DPI/96 (" +
                                                          std::to_string(reported_scale) + " vs " +
                                                          std::to_string(expect) + ")");
                check(nearly(reported_scale, win.surface().scale_factor()),
                      "(e3) reported scale == scale_factor() after injection (" + std::to_string(reported_scale) +
                          " vs " + std::to_string(win.surface().scale_factor()) + ")");
            }

            // ---- 判据 (e5)：去重分支真的拦下「无变化」的上报 ----
            //
            // 前提核对（决定了这条判据能不能成立，缺了就是空转）：
            //   * `third_party/glfw/src/win32_window.c:1218` 在 `WM_DPICHANGED` 里**无条件**调
            //     `_glfwInputWindowContentScale`；
            //   * `third_party/glfw/src/window.c:142-143` 的派发同样无条件——只要注册了回调
            //     就调，**GLFW 自身不做「值未变则跳过」的去重**。
            // ⇒ 所以再注入一次**当前值**（此刻成员 scale 已是 inject_dpi/96）时，回调**一定会
            // 被递到**，`GlfwSurface::Impl::on_content_scale` 的 `if (next == self->scale) return;`
            // 是唯一能拦住这次上报的地方。这与本探针上一版注释里的推断恰好相反——那一版据
            // 「去重会静默返回」为由把同值注入排除在外，致该分支在自动段永远走不到，
            // 删掉去重后变异全 PASS 暴露（见 memory 2026-10-03「已知缺口」）。
            // 变异自证：删掉 `if (next == self->scale) return;` ⇒ (e5) 转红。
            const int before_dup = report_count;
            RECT same{};
            (void)GetWindowRect(hwnd, &same);
            SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(inject_dpi, inject_dpi), reinterpret_cast<LPARAM>(&same));
            (void)session.pump(1);
            check(report_count == before_dup, "(e5) duplicate-scale WM_DPICHANGED is deduped (count " +
                                                  std::to_string(before_dup) + " -> " + std::to_string(report_count) +
                                                  ")");
            // 顺带钉住「去重不是靠提前 return 吞掉换算」：成员 scale 必须仍是注入后的值。
            check(nearly(win.surface().scale_factor(), static_cast<float>(inject_dpi) / 96.0F),
                  "(e6) dedup kept the adopted scale (" + std::to_string(win.surface().scale_factor()) + " vs " +
                      std::to_string(static_cast<float>(inject_dpi) / 96.0F) + ")");

            // 复位：把 scale 推回原值，避免污染后续 (b) 组判据的读数。
            RECT again{};
            (void)GetWindowRect(hwnd, &again);
            SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(current_dpi, current_dpi), reinterpret_cast<LPARAM>(&again));
            (void)session.pump(1);
        }
    }
#else
    check(false,
          "(e) channel-injection criterion requires Windows (GLFW drives content-scale from WM_DPICHANGED); "
          "non-Windows hosts get the manual cross-monitor segment only");
#endif

    // ---- 判据 (b)：换算自洽（frame / 逻辑 / scale 三者同源）----
    // 这是本探针在 100% DPI 下仍能证伪的核心：即便 scale 恰 1.0，「构造期按主显示器预估
    // → 建窗后按真实 content scale 校正」这条路径是否真的跑过、`framebuffer_size()`
    // 是否真的返回物理像素，仍是可观测的。100% DPI 下三者恒等，故本条只证明接线在位，
    // 不证明换算在 ≠100% DPI 下的正确性——后者由人工段 (d3) 覆盖。
    const auto pumped2 = session.pump(1);
    check(pumped2.ok(), "(b0) second frame pump succeeded");
    const auto logical = win.surface().size();
    const auto physical = win.surface().framebuffer_size();
    const bool size_matches_request = (static_cast<int>(std::lround(logical.width)) == kLogicalW) &&
                                      (static_cast<int>(std::lround(logical.height)) == kLogicalH);
    check(size_matches_request, "(b1) size() == requested dp (" + std::to_string(logical.width) + "x" +
                                    std::to_string(logical.height) + " vs " + std::to_string(kLogicalW) + "x" +
                                    std::to_string(kLogicalH) + ")");
    const bool fb_is_physical = nearly(static_cast<float>(physical.width), static_cast<float>(kLogicalW) * scale) &&
                                nearly(static_cast<float>(physical.height), static_cast<float>(kLogicalH) * scale);
    check(fb_is_physical,
          "(b2) framebuffer_size() == requested dp x scale (" + std::to_string(physical.width) + "x" +
              std::to_string(physical.height) + " vs " +
              std::to_string(static_cast<int>(std::lround(static_cast<float>(kLogicalW) * scale))) + "x" +
              std::to_string(static_cast<int>(std::lround(static_cast<float>(kLogicalH) * scale))) + ")");
    check(nearly(logical.width * scale, static_cast<float>(physical.width)) &&
              nearly(logical.height * scale, static_cast<float>(physical.height)),
          "(b3) frame / logical / scale are consistent (logical x scale == framebuffer)");
    // 不退化为逻辑尺寸：GLFW 后端的 painter 按物理分辨率分配，若 framebuffer_size 漏 override
    // 则此处返回逻辑值，≠100% DPI 下 (b2) 转红。
    check(!(nearly(static_cast<float>(physical.width), logical.width) && scale > 1.01F) ||
              nearly(static_cast<float>(physical.width), static_cast<float>(kLogicalW) * scale),
          "(b4) framebuffer_size() is not silently falling back to logical size");

    if (failures == 0) {
        emit("== automatic segment passed ==");
    } else {
        emit("== automatic segment FAILED (" + std::to_string(failures) + ") ==");
    }

    // ---- 人工段 (d)：真跨屏拖动 ----
    if (!interactive) {
        emit("");
        emit("[PENDING MANUAL] criterion (d) not executed: drag this window to a monitor with a");
        emit("  different scale, then observe: the handler must receive the new scale, and");
        emit("  scale_factor() must agree with it.");
        emit("  Re-run with --interactive to perform it, or record it in the manual-test doc.");
        return 3;  // 如实归类：人工段未做，不是通过
    }

    emit("");
    emit("== manual segment (d): drag the window to a monitor with a different scale ==");
    emit("   watching for a scale-change report...");
    // 人工观察窗：这里不做任何注入。真跨屏只能由人拖动窗口发生；注入 WM_DPICHANGED
    // 会让判据恒真（GLFW 的 Win32 分支在收到该消息时先 SetWindowPos 再推回调，
    // 注入可复现回调但复现不了「合成器按新 DPI 重新协商帧缓冲」这一段）。
    // 观察窗保持运行直到用户回车。
    emit("   (press Enter in this console after the drag completes)");
    {
        std::string line;
        (void)std::getline(std::cin, line);
    }

    const bool report_seen = report_count > 0;
    if (!report_seen) {
        emit("[FAIL] (d1) no scale-change report received after cross-monitor drag");
        emit("        if this host has only one monitor, or all monitors share the same scale,");
        emit("        this criterion cannot rotate — record it as SKIP, not as a pass.");
        ++failures;
    } else {
        emit("[PASS] (d1) scale-change report received " + std::to_string(report_count) + " time(s)");
        check(nearly(reported_scale, win.surface().scale_factor()),
              "(d2) reported scale == scale_factor() (" + std::to_string(reported_scale) + " vs " +
                  std::to_string(win.surface().scale_factor()) + ")");
        const auto pumped3 = session.pump(2);
        check(pumped3.ok(), "(d4) post-drag frame pump succeeded");
        const auto post = win.surface().framebuffer_size();
        const auto post_logical = win.surface().size();
        check(nearly(post_logical.width * win.surface().scale_factor(), static_cast<float>(post.width)) &&
                  nearly(post_logical.height * win.surface().scale_factor(), static_cast<float>(post.height)),
              "(d3) framebuffer rescaled to the new scale (" + std::to_string(post.width) + "x" +
                  std::to_string(post.height) + " at scale " + std::to_string(win.surface().scale_factor()) + ")");
    }

    return failures == 0 ? 0 : 1;
}
