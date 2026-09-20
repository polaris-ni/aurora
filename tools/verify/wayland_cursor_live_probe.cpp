/* 光标形状 —— Wayland 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 用途：在**真实 Wayland 合成器**上验证 `aurora::WaylandSurface::set_cursor(CursorShape)`
//       确实把「该形状对应的光标位图」提交给了合成器（端到端接线），而不只是「调用不崩」。
//
// 与 Win32/X11 探针的关键差别（务必如实申报）：
//   Wayland **没有任何客户端可达的「屏幕上当前显示的光标」读回 API**（Win32 有
//   `GetCursorInfo`，X11 有 XFIXES `XFixesGetCursorImage`）。合成器不广播、也不允许客户端
//   查询自己正在用哪张光标。因此本探针的判据只能落在**本端提交了什么**：
//   `WaylandSurface::cursor_state()` 暴露每次提交的「主题命中名 / 位图尺寸 / 热点 /
//   wl_buffer 身份 / 提交次数」，逐项与 `cursor_rfc_name(shape)` 请求比对。
//   「合成器接受并在屏幕上画出该位图」不在本探针证明范围内，改由 --interactive 人工目视段。
//
// 原理：
//   1) 建真实窗口并出帧（xdg_toplevel 首次带缓冲 commit 才被合成器视为 mapped）；
//   2) 取得 `wl_pointer.set_cursor` 必需的 serial：Wayland 客户端**没有** warp 指针的 API，
//      指针位置由合成器持有。故把窗口铺满输出，使静止的物理指针必然落在表面内，逼合成器回
//      一次 `wl_pointer.enter`（携带 serial）；后端在 enter 内自动补下发一次。铺满分两级策略：
//      先最大化，拿不到 enter 再全屏（WSLg 实测最大化不含顶部面板带，指针停在那儿即落不进）。
//   3) 对 11 个 `CursorShape` 逐个 `set_cursor`，逐形状读回 `cursor_state()` 断言（见下）。
//
// 逐项判据（每形状）：
//   a. `applied` 为真（本次请求确实提交到了 cursor 表面）；
//   b. `resolved_name` 逐字等于 `cursor_rfc_name(shape)`（主题里确有该 freedesktop 规范名，
//      未走 default/left_ptr 回退）；
//   c. 位图尺寸 > 0 且热点落在位图内（热点折错缩放会立刻越界）；
//   d. `commits` 相对上一形状 +1（每形状恰一次提交，无重复提交）；
//   e. `buffer_id` 与上一形状互异（提交了不同的位图）。
// 汇总判据：
//   f. 同形状重复下发**幂等**（`commits` 不再增长）；
//   g. 11 形状提交的 `wl_buffer` 身份至少 10 个互异（判据留一处余量：某些主题里 `default`
//      与 `move` 的光标文件字节相同，若实现按内容去重则会共用同一位图，属主题巧合）；
//   h. `theme_size == 24 × scale`（主题按设备像素加载，缩放接线正确）。
//
// 构建（须已开启 Wayland 后端与探针开关）：
//   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BACKEND_WAYLAND=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build --target aurora_verify_wayland_cursor
//   ./build/aurora_verify_wayland_cursor            # 自动段
//   WAYLAND_DISPLAY=wayland-0 ./build/aurora_verify_wayland_cursor --interactive   # + 人工目视
//
// 退出码：
//   0  全部判据通过（自动段）—— 未跑人工段时不代表「屏幕像素已证明」
//   1  自动断言存在失败项
//   2  环境不可用（无 WAYLAND_DISPLAY / 合成器连接失败 / 无 Wayland 后端构建）
//   3  拿不到 wl_pointer.enter（最大化与全屏两级铺满后指针仍未落入表面）—— 无合法 serial，
//      判据无从执行，须人工在真实桌面下复核；随场打印的「累计鼠标事件」用于区分
//      「本会话根本没有指针设备」（0 条）与「有指针但落不进窗口」
//   4  光标主题不可用（wl_cursor_theme_load 失败，通常是图标主题未安装）
//
// 注意：本探针会把被测窗口先最大化再全屏（见「原理」2），退出即随窗口析构恢复；不移动用户
//   物理指针，不改系统主题。
//   本机实测（2026-09-20，WSLg Weston 3840x2160 + Adwaita 主题，scale=1）：**策略 1（最大化）
//       拿不到 enter**——WSLg 下最大化后表面只有 3840x2088（顶部留出面板带），静止指针停在
//       带上；策略 2（全屏 3840x2160）随即取得 enter。11/11 形状的主题命中名逐字等于请求的
//       freedesktop 规范名、位图均 24x24、热点互不相同（仅 default/move 同为 (3,1)）、
//       11 个互异 `wl_buffer`（Adwaita 的 default 与 move 文件字节相同，但 libwayland-cursor
//       各自建缓冲故未重合）、`wait` 的 image_count=60（动画光标，本端取首帧）；同形状重复
//       下发 commits 不变（14 → 14）；全程提交 14 次，自动段 9 条 [PASS] 全绿，退出码 0。
//   构建命令中的行尾反斜杠为续行符，故本头注释整体使用块注释形态（避免 -Wcomment）。
// ============================================================================ */

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/window/cursor_map.h"
#include "aurora/window/native_surfaces.h"
#include "verify_print.h"

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

int failures = 0;  // NOLINT

auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        failures++;
    }
}

void nap_ms(long ms) {
    struct timespec ts{};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000L * 1000L;
    nanosleep(&ts, nullptr);
}

}  // namespace

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)

auto main(int argc, char **argv) -> int {
    bool interactive = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--interactive") {
            interactive = true;
        }
    }
    if (const char *wdpy = std::getenv("WAYLAND_DISPLAY"); wdpy == nullptr || *wdpy == '\0') {
        emit("[SKIP] 无 WAYLAND_DISPLAY 环境变量（无 Wayland 合成器），Wayland 光标探针无法运行");
        return 2;
    }

    aurora::WaylandSurface surface(640, 420, "aurora-verify-wayland-cursor");
    if (!surface.is_available()) {
        AURORA_LOG_ERROR("verify", "WaylandSurface unavailable（合成器连接失败或缺 compositor/shm/xdg_wm_base）");
        return 2;
    }
    // 先出帧：xdg_toplevel 只有带缓冲 commit 后才会被合成器视为 mapped，未映射不会收到 enter。
    auto frame_out = surface.begin_frame(640, 420);
    if (!frame_out) {
        AURORA_LOG_ERROR("verify", "begin_frame 失败，无法让窗口进入 mapped 态");
        return 2;
    }
    (void)surface.present();

    // ---- 取 enter serial：Wayland 客户端**没有** warp 指针的 API（对比 X11 探针的 XWarpPointer），
    // 指针位置完全由合成器持有。故只能把窗口铺满整块输出，使静止的物理指针必然落在表面内，
    // 逼合成器回一次 wl_pointer.enter。两级策略：先最大化（WSLg 下最大化高度可能不含顶部面板，
    // 指针若停在 (0,0) 仍在窗外）→ 再全屏（覆盖整个输出）。同时计数收到的鼠标事件，作为
    // 「本会话究竟有没有指针设备」的现场证据（无指针时 rc=3 的诊断靠它区分）。
    std::int64_t mouse_events = 0;
    surface.set_event_handler([&mouse_events](aurora::Event &ev) {
        if (dynamic_cast<aurora::MouseEvent *>(&ev) != nullptr || dynamic_cast<aurora::ScrollEvent *>(&ev) != nullptr) {
            ++mouse_events;
        }
    });
    bool entered = false;
    int strategy = 0;
    for (int attempt = 0; attempt < 2 && !entered; ++attempt) {
        if (attempt == 0) {
            strategy = 1;
            surface.toggle_maximize();
        } else {
            strategy = 2;
            surface.set_fullscreen(true);
        }
        for (int i = 0; i < 150 && !entered; ++i) {
            surface.poll_platform_events();
            entered = surface.cursor_state().pointer_entered;
            if (entered) {
                break;
            }
            (void)surface.begin_frame(static_cast<int>(surface.size().width), static_cast<int>(surface.size().height));
            (void)surface.present();
            nap_ms(20);
        }
    }
    emit("WAYLAND_DISPLAY=" +
         std::string(std::getenv("WAYLAND_DISPLAY") != nullptr ? std::getenv("WAYLAND_DISPLAY") : "") +
         "，scale=" + aurora_verify::format_uint(static_cast<unsigned>(surface.scale_factor())) +
         "，落点策略=" + aurora_verify::format_int(strategy) +
         "，窗口尺寸=" + aurora_verify::format_int(static_cast<long long>(surface.size().width)) + "x" +
         aurora_verify::format_int(static_cast<long long>(surface.size().height)) +
         "，累计鼠标事件=" + aurora_verify::format_int(static_cast<long long>(mouse_events)));
    if (!entered) {
        AURORA_LOG_ERROR("verify",
                         "拿不到 wl_pointer.enter：最大化与全屏两级铺满后指针仍未落入本表面"
                         "（累计鼠标事件 " +
                             aurora_verify::format_int(static_cast<long long>(mouse_events)) +
                             " 条；0 条即本会话根本没有指针设备）。无合法 serial 可供 "
                             "wl_pointer.set_cursor，判据无从执行，须人工在真实桌面复核。");
        return 3;
    }
    check(surface.cursor_state().pointer_entered, "已取得 wl_pointer.enter（set_cursor 有合法 serial）");

    // 首帧 enter 时后端会自动补下发一次（ptr_enter 内 force 重下发），提交计数含那一次。
    // 基线前先切到一个必与被测形状不同的形状，好让下面 11 形状逐个 +1 的判据成立
    // （否则第 1 个形状 Arrow 会与 enter 时的自动提交撞上同形状去重）。
    surface.set_cursor(aurora::CursorShape::Crosshair);
    surface.poll_platform_events();
    const int baseline = surface.cursor_state().commits;
    check(baseline >= 2, "enter 自动提交 + 基线切形状各一次（commits=" + aurora_verify::format_int(baseline) + "）");

    // ---- 11 形状逐个下发 + 逐项读回 ----
    emit("");
    emit(aurora_verify::pad_right("shape(rfc name)", 20) + aurora_verify::pad_right("resolved", 16) +
         aurora_verify::pad_right("w", 5) + aurora_verify::pad_right("h", 5) + aurora_verify::pad_right("bscale", 7) +
         aurora_verify::pad_right("xhot", 6) + aurora_verify::pad_right("yhot", 6) +
         aurora_verify::pad_right("frames", 8) + aurora_verify::pad_right("commits", 9) + "buffer_id");

    const int total = static_cast<int>(aurora::AURORA_CURSOR_SHAPE_COUNT);
    std::set<std::uint64_t> buffers;
    std::set<std::string> names;
    int prev_commits = baseline;
    bool all_applied = true;
    bool all_named = true;
    bool all_geom_ok = true;
    bool all_single_commit = true;
    for (int i = 0; i < total; ++i) {
        const auto shape = static_cast<aurora::CursorShape>(i);
        const std::string rfc = aurora::cursor_rfc_name(shape);
        surface.set_cursor(shape);
        surface.poll_platform_events();
        const auto st = surface.cursor_state();
        emit(aurora_verify::pad_right(rfc, 20) + aurora_verify::pad_right(st.resolved_name, 16) +
             aurora_verify::pad_right(aurora_verify::format_int(st.image_width), 5) +
             aurora_verify::pad_right(aurora_verify::format_int(st.image_height), 5) +
             aurora_verify::pad_right(aurora_verify::format_int(st.buffer_scale), 7) +
             aurora_verify::pad_right(aurora_verify::format_int(st.hotspot_x), 6) +
             aurora_verify::pad_right(aurora_verify::format_int(st.hotspot_y), 6) +
             aurora_verify::pad_right(aurora_verify::format_int(st.image_count), 8) +
             aurora_verify::pad_right(aurora_verify::format_int(st.commits), 9) +
             aurora_verify::format_handle(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(st.buffer_id))));
        all_applied = all_applied && st.applied;
        all_named = all_named && (st.resolved_name == rfc);
        const bool geom_ok = st.image_width > 0 && st.image_height > 0 && st.hotspot_x >= 0 && st.hotspot_y >= 0 &&
                             st.hotspot_x * st.buffer_scale <= st.image_width &&
                             st.hotspot_y * st.buffer_scale <= st.image_height;
        all_geom_ok = all_geom_ok && geom_ok;
        all_single_commit = all_single_commit && (st.commits == prev_commits + 1);
        prev_commits = st.commits;
        buffers.insert(st.buffer_id);
        names.insert(st.resolved_name);
    }
    check(all_applied, "11/11 形状均提交到 cursor 表面（cursor_state().applied 恒真）");
    check(all_named, "11/11 形状的主题命中名逐字等于 cursor_rfc_name（未走 default/left_ptr 回退）");
    check(all_geom_ok, "11/11 形状的位图尺寸 > 0 且热点落在位图内（缩放折算无误）");
    check(all_single_commit, "11/11 形状各自恰好提交一次（commits 逐形状 +1，无重复提交）");
    if (names.empty() || *names.begin() == "") {
        AURORA_LOG_ERROR("verify", "光标主题不可用（无 resolved name），判据无从执行");
        return 4;
    }

    // ---- 幂等：同形状连续两次下发不得再提交 ----
    surface.set_cursor(aurora::CursorShape::IBeam);
    const int c1 = surface.cursor_state().commits;
    surface.set_cursor(aurora::CursorShape::IBeam);
    const int c2 = surface.cursor_state().commits;
    check(c1 == c2, "同形状重复下发幂等（commits " + aurora_verify::format_int(c1) + " → " +
                        aurora_verify::format_int(c2) + "）");

    // ---- 主题尺寸按设备像素加载 ----
    const auto st = surface.cursor_state();
    check(st.theme_size == 24 * static_cast<int>(surface.scale_factor()),
          "主题加载尺寸 == 24 × scale（实得 " + aurora_verify::format_int(st.theme_size) + "）");

    // ---- 位图互异（Adwaita 的 default/move 同图，允许一处重合）----
    check(buffers.size() >= static_cast<std::size_t>(total) - 1,
          "提交的互异光标位图 >= 10 / 11（实得 " + aurora_verify::format_uint(buffers.size()) +
              "；判据留一处余量：某些主题 default 与 move 的光标文件字节相同）");

    // ---- 人工段 ----
    if (interactive) {
        emit("\n[人工段] 窗口常驻，每 1.2s 轮换一个光标形状。请把指针停在窗口内，目视确认屏幕上");
        emit("的光标确实随之一一改变（text/pointer/ns-resize/…/fleur 等），并核对热点位置正确");
        emit("（例如 IBeam 的竖线正对指针尖、缩放箭头正对边角）。关闭窗口退出。");
        emit("注意：本探针的自动段只证明「提交了什么」，屏幕上真实显示的光标只能由本段人眼确认。");
        int idx = 0;
        while (!surface.should_close()) {
            surface.set_cursor(static_cast<aurora::CursorShape>(idx % total));
            (void)surface.begin_frame(static_cast<int>(surface.size().width), static_cast<int>(surface.size().height));
            (void)surface.present();
            surface.poll_platform_events();
            nap_ms(1200);
            ++idx;
        }
    } else {
        emit("\n提示：加 --interactive 进入常驻窗口人工目视段（屏幕上真实光标只能由人眼确认）。");
    }

    emit(std::string("\n结果：") + (failures == 0 ? "ALL PASS" : std::to_string(failures) + " FAILURES"));
    return failures == 0 ? 0 : 1;
}

#else

auto main(int /*argc*/, char ** /*argv*/) -> int {
    emit("[SKIP] 本探针仅在 Linux(非 Android) + AURORA_BACKEND_WAYLAND=ON 构建下有效");
    return 2;
}

#endif  // AURORA_PLATFORM_LINUX && AURORA_BACKEND_WAYLAND
