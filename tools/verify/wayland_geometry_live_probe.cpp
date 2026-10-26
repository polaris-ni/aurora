// tools/verify/wayland_geometry_live_probe.cpp — Wayland 窗口尺寸提议真机探针（非 CTest）。
//
// 覆盖：Wayland 后端的 `set_size` 是否真的经 xdg-shell 走通了「client 提议 → compositor
// configure 落地」这条链路，而不是只「调用不崩」；以及 `set_position` 如实为 no-op。
//
// 为什么用 configure 驱动尺寸作为判据：xdg-shell 没有「程序化设尺寸」请求，client 只能把
// xdg_toplevel 的 min/max 约束临时收窄到目标值来表达提议，最终尺寸由 compositor 在
// `xdg_toplevel.configure` 里决定。`WaylandSurface::size()` 的内容尺寸**只在 configure
// 处理中更新**（`set_size` 本身不写本地尺寸缓存），故「调用后 size() 变成目标值」只能由
// 真实 configure 往返产生——这是本探针能给出的最强端到端证据。
//
// 覆盖项：
//   (a) 基线：会话可建窗、帧泵成功、`size()` 与建窗请求一致（Wayland 的 size() 是整幅表面
//       口径：内容 + 2×CSD 阴影边距）；
//   (b) `set_size(640×420 dp)` → 轮询真实时间直到 configure 落地，断言 `size()` 等于目标
//       整窗尺寸（容差 ≤1.5dp），且 `frame_count()` 在 configure 后仍能推进；
//   (c) 同值重复 `set_size` 幂等：不再触发尺寸变化（实现里「已是目标尺寸」直接返回）；
//   (d) `set_position` 是显式 no-op：调用后 `position()` 仍为基类默认 (0,0)，`size()` 不变
//       （xdg-shell 协议层没有客户端可用的定位请求，合成器决定位置）。
//
// 判据刻意不依赖合成器私有工具：configure 往返本身就是协议层的真实落地证明。
//
// 构建（Wayland-only）：
// clang-format off
//   cmake -S . -B build-wayland -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_BACKEND_WAYLAND=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-wayland --target aurora_verify_wayland_geometry
// clang-format on
// 运行：WAYLAND_DISPLAY=wayland-0 ./build-wayland/aurora_verify_wayland_geometry
//
// 退出码：0 自动段全过；1 有断言失败；2 环境不可用（无 WAYLAND_DISPLAY / 建窗失败）。
// 本探针全自动，无人工段；`--interactive` 仅为与其余探针共用入口而接受（不改变判据）。

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
#error "aurora_verify_wayland_geometry can only be built on Linux (non-Android)"
#endif
#ifndef AURORA_BACKEND_WAYLAND
#error "AURORA_BACKEND_WAYLAND must be enabled"
#endif

#include "e2e/harness.h"
#include "verify_args.h"
#include "verify_print.h"

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

// 失败计数藏函数内静态：探针不落全局可变状态（与其余 live_probe 同款）。
auto failures() -> int & {
    static int count = 0;
    return count;
}

auto check(bool passed, const std::string &label) -> void {
    emit(std::string("[") + (passed ? "PASS" : "FAIL") + "] " + label);
    if (!passed) {
        failures()++;
    }
}

auto nearly(float a, float b, float tol = 1.5F) -> bool { return std::fabs(a - b) <= tol; }

/// @brief 轮询等待 configure 把尺寸真正落地。
///
/// configure 由合成器在收到约束 commit 后异步产生，`Session::pump` 只推帧不等真实时间；
/// 故按真实时间轮询平台事件泵，而不是「推两帧即断言」。
/// @param session E2E 会话：轮询期间持续泵平台事件（Wayland 的 configure 在此被派发）。
/// @param done 落地判据（满足即返回）。
/// @param timeout_ms 轮询上限（毫秒）。
/// @return 是否在超时前满足判据。
template <typename Done>
auto wait_until(aurora::e2e::Session &session, Done done, int timeout_ms = 3000) -> bool {
    for (int waited = 0; waited <= timeout_ms; waited += 50) {
        session.pump_events();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return done();
}

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现（口径同 tools/verify/ 其余探针）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Wayland geometry proposal live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    (void)cli.arguments->flag("interactive");  // 本探针全自动，旗标仅为统一入口而接受

    emit("== aurora verify: Wayland size proposal (xdg_toplevel) ==");

    if (const char *wdpy = std::getenv("WAYLAND_DISPLAY"); wdpy == nullptr || *wdpy == '\0') {
        emit("[SKIP] WAYLAND_DISPLAY is not set (no Wayland compositor); the geometry probe cannot run");
        return 2;
    }

    aurora::e2e::WindowSpec spec;
    spec.backend = aurora::e2e::Backend::Wayland;
    spec.width = 480;
    spec.height = 320;
    spec.title = "aurora-verify-wayland-geometry";
    spec.visibility = aurora::WindowVisibility::Normal;  // 尺寸提议需要合成器真实参与
    auto session = aurora::e2e::open(spec);
    if (!session.ok()) {
        AURORA_LOG_ERROR("verify", "open(Wayland) failed: " + session.reason());
        return 2;
    }
    auto &win = session.window();

    // 挂最小控件树：`Session::pump` 要求已 mount；`Row` 只提供可布局实体，不参与尺寸判据。
    aurora::Node root = aurora::Node{aurora::Row{}};
    session.mount(root);

    // ---- (a) 基线 ----
    const auto settled = session.pump_until_settled(10);
    check(settled.ok(),
          "(a0) baseline frames settled" + (settled.ok() ? std::string{} : ": " + settled.error().message));
    const aurora::Size base = win.size();
    emit("  base window size = " + std::to_string(base.width) + "x" + std::to_string(base.height) + " dp");
    check(base.width >= static_cast<float>(spec.width) - 1.5F && base.height >= static_cast<float>(spec.height) - 1.5F,
          "(a1) baseline size() covers the requested content (Wayland size() = content + 2*margin)");

    // ---- (b) set_size 经 configure 真实落地 ----
    const aurora::Size target{.width = 640.0F, .height = 420.0F};
    const int frames_before = session.frame_count();
    win.surface().set_size(target);
    const bool landed = wait_until(session, [&win, &target]() -> bool {
        const aurora::Size now = win.size();
        return nearly(now.width, target.width) && nearly(now.height, target.height);
    });
    // configure 处理会经 present_request_ 标脏（重排 + 全量重绘），推两帧让它真正出帧。
    (void)session.pump(2);
    const aurora::Size after = win.size();
    check(landed, "(b0) set_size proposal landed via configure (got " + std::to_string(after.width) + "x" +
                      std::to_string(after.height) + ", want " + std::to_string(target.width) + "x" +
                      std::to_string(target.height) + ")");
    // 内容尺寸 = 整窗 - 2×margin：若实现漏了边距换算，configure 落地后 size() 会多出 2×margin，
    // b0 即转红。此处再断言新帧确实产出，证明重排/重绘链路也接通了。
    check(session.frame_count() > frames_before, "(b1) configure triggered a new frame (frame_count " +
                                                     std::to_string(frames_before) + " -> " +
                                                     std::to_string(session.frame_count()) + ")");

    // ---- (c) 同值重复提议幂等 ----
    win.surface().set_size(target);
    (void)session.pump(2);
    session.pump_events();
    check(nearly(win.size().width, after.width) && nearly(win.size().height, after.height),
          "(c0) repeated set_size with the same target is idempotent");

    // ---- (d) set_position 如实 no-op ----
    const aurora::Point before = win.surface().position();
    win.surface().set_position(aurora::Point{.x = 120.0F, .y = 90.0F});
    (void)session.pump(2);
    session.pump_events();
    const aurora::Point reported = win.surface().position();
    check(nearly(before.x, 0.0F, 0.001F) && nearly(before.y, 0.0F, 0.001F) && nearly(reported.x, 0.0F, 0.001F) &&
              nearly(reported.y, 0.0F, 0.001F),
          "(d0) set_position is a no-op and position() stays (0,0) (xdg-shell has no client positioning)");
    check(nearly(win.size().width, after.width) && nearly(win.size().height, after.height),
          "(d1) set_position does not disturb the size");

    if (failures() == 0) {
        emit("== automatic segment passed ==");
        return 0;
    }
    emit("== automatic segment FAILED (" + std::to_string(failures()) + ") ==");
    return 1;
}
