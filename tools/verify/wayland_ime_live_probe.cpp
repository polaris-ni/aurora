/* 输入法（text-input-unstable-v3）—— Wayland 真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 覆盖接缝：`zwp_text_input_manager_v3` 全局绑定 → seat 首个键盘到达时建
// `zwp_text_input_v3` + listener 表 → enter/leave → `ti_refresh_enable`（provider
// 非零盒判据）→ `set_content_type`/`set_cursor_rectangle`/`commit` 请求序列，以及
// preedit/commit/delete_surrounding 服务端的接收面（`text_input_state()` 观测）。
//
// 为什么必须真机：v3 的「输入焦点」由合成器授予并投递 enter/事件，无头 CI 既无合成器
// 也无协议对象；桥的 listener 表若签名错位，Weston 会在运行期直接杀连接——这类
// 「协议合法性」只能在真实会话里证明。UTF-8 字节下标 → 码点契约的**折算层**本身由
// `utest_ime_composition`（17 项无头用例）覆盖，本探针只证明接线。
//
// 自动段（无需任何输入法进程）逐项验收：
//   ① 代码生成门：`protocol_disabled` 为 true 即构建缺 v3 XML（退出码 2，属构建环境项）。
//   ② 优雅降级：合成器**未发布** v3 manager（WSLg Weston 实测如此）⇒ provider 接线 +
//      连续出帧后 `input_created/enabled/commits` 恒零、连接无错——「桥缺席不影响帧循环」。
//   ③ 绑定链（manager 在场的合成器）：seat 有键盘 ⇒ `input_created`；键盘焦点落入本表面
//      ⇒ `entered`（`zwp_text_input_v3.enter` 真到达，listener 表合法）。
//   ④ enable 判据（本切片核心逻辑，双向）：entered 后 provider 报**非零盒** ⇒
//      `enable + set_content_type + commit` 下发（`enabled` 翻真、`commits` 增长）；
//      报**零盒**（焦点离开文本控件）⇒ `disable + commit`；同状态重复刷新**去重**
//      （每帧 present 调用零协议开销）。拿不到键盘焦点（合成器激活策略）则该段 SKIP。
//   preedit/commit/delete_surrounding 需真实输入法进程（weston-keyboard、fcitx5-wayland…）
//   配合 ⇒ 自动段 SKIP，落在 --interactive 人工段。
//
// 人工段（`--interactive`）：窗口常驻，逐帧打印 text-input 状态与组合事件流。在提供 v3
// 的合成器会话下用真实输入法于窗口内键入拼音，肉眼核对：① preedit 随拼音更新；② 候选窗
// 贴在插入点盒旁；③ 选字上屏走 committed 单通道；④ Esc 取消无残留；⑤ 切走键盘焦点后
// 桥 disable 且控件无半截拼音残留。
//
// 构建（须已开启 Wayland 后端与探针开关）：
//   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BACKEND_WAYLAND=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build --target aurora_verify_wayland_ime
//   ./build/aurora_verify_wayland_ime            # 自动段
//   ./build/aurora_verify_wayland_ime --interactive   # + 人工段
//
// 退出码：
//   0  自动段全部通过（环境相关项按 SKIP 注明，交由 --interactive 人工段）
//   2  环境不可用（无 WAYLAND_DISPLAY / 合成器连接失败 / 构建缺 v3 协议代码生成）
//   4  部分验收项不符 —— 见逐行 PASS/FAIL
//
// 本机实测（WSLg Weston）：registry globals **未发布** zwp_text_input_manager_v3
//   ⇒ 走②优雅降级路全绿（退出码 0）：provider 接线 + 多帧出帧后桥零请求
//   （`input_created/enabled/commits` 恒零）、连接无错。③/④（enter→enable 判据双向+去重）
//   代码路径经编译与静态监听表验证，运行期证明需换发布 v3 的合成器（KDE/mutter 桌面会话）。
//   构建命令中的行尾反斜杠为续行符，故本头注释整体使用块注释形态（避免 -Wcomment）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
#error "aurora_verify_wayland_ime can only be built on Linux (AURORA_PLATFORM_LINUX)"
#endif
#if !defined(AURORA_BACKEND_WAYLAND)
#error "AURORA_BACKEND_WAYLAND must be enabled"
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/window/wayland_surface.h"
#include "verify_args.h"
#include "verify_print.h"

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

int failures = 0;

auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        ++failures;
    }
}

auto skip(const std::string &label) -> void { emit("[SKIP] " + label); }

void nap_ms(long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/// @brief 抽干式泵事件：poll + 出帧（present 内含 IME 状态刷新）+ 小睡，直至判据成立或超时。
/// @return 判据是否在超时前成立。
auto pump_until(aurora::WaylandSurface &surface, const std::function<bool()> &done, int timeout_ms) -> bool {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 20) {
        surface.poll_platform_events();
        if (done()) {
            return true;
        }
        (void)surface.begin_frame(static_cast<int>(surface.size().width), static_cast<int>(surface.size().height));
        (void)surface.present();
        nap_ms(20);
    }
    return done();
}

/// @brief 状态单行摘要（--interactive 差分打印用）。
[[nodiscard]] auto state_line(const aurora::WaylandSurface::TextInputState &s) -> std::string {
    return std::string("manager=") + (s.manager_bound ? "y" : "n") + " input=" + (s.input_created ? "y" : "n") +
           " entered=" + (s.entered ? "y" : "n") + " enabled=" + (s.enabled ? "y" : "n") +
           " commits=" + std::to_string(s.commits) + " deletes=" + std::to_string(s.delete_requests) + " preedit=\"" +
           s.preedit + "\"";
}

}  // namespace

auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Wayland text-input-v3 IME bridge live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    emit("==== Wayland text-input-unstable-v3 IME bridge live acceptance ====");
    if (const char *wdpy = std::getenv("WAYLAND_DISPLAY"); wdpy == nullptr || *wdpy == '\0') {
        emit("[ENV] WAYLAND_DISPLAY is not set (no Wayland session), the probe cannot run");
        return 2;
    }

    aurora::WaylandSurface surface(520, 360, "aurora-verify-wayland-ime");
    if (!surface.is_available()) {
        AURORA_LOG_ERROR("verify", "WaylandSurface unavailable (compositor connect failed or shm/xdg_wm_base missing)");
        return 2;
    }
    // 先出帧：xdg_toplevel 带缓冲 commit 后才被合成器视为 mapped，才可能获得键盘焦点。
    if (!surface.begin_frame(520, 360)) {
        AURORA_LOG_ERROR("verify", "begin_frame failed, the window cannot reach the mapped state");
        return 2;
    }
    (void)surface.present();

    // 事件面捕获（人工/自动段都挂）：committed/preedit 流逐条打印，供 --interactive 目视。
    surface.set_event_handler([](aurora::Event &ev) {
        if (auto *ce = dynamic_cast<aurora::TextCompositionEvent *>(&ev); ce != nullptr) {
            emit("[EVENT] TextCompositionEvent preedit=\"" + ce->preedit +
                 "\" cursor=" + std::to_string(ce->cursor_index) + " sel=[" + std::to_string(ce->sel_start) + "," +
                 std::to_string(ce->sel_end) + "] committed=\"" + ce->committed + "\"");
        } else if (auto *te = dynamic_cast<aurora::TextInputEvent *>(&ev); te != nullptr) {
            emit("[EVENT] TextInputEvent text=\"" + te->text + "\"");
        }
    });

    auto st = surface.text_input_state();
    emit(std::string("initial: ") + state_line(st));

    // ---- ① 代码生成门 ----
    if (st.protocol_disabled) {
        emit(
            "[ENV] this build lacks the text-input-unstable-v3 XML (AURORA_HAVE_WL_TEXT_INPUT=0), the bridge is not "
            "compiled; install wayland-protocols, reconfigure the build and run again");
        return 2;
    }
    check(true,
          "protocol code generation present (protocol_disabled=false; listener and request signatures are "
          "statically valid, already proven by compilation)");

    // ---- ② 未发布 manager：优雅降级（WSLg Weston 即此路） ----
    if (!st.manager_bound) {
        pump_until(surface, [] { return false; }, 400);  // 纯泵 400ms：全局若晚到也能绑上
        st = surface.text_input_state();
        emit(std::string("after pump: ") + state_line(st));
        check(!st.manager_bound && !st.input_created && !st.enabled && st.commits == 0,
              "compositor does not advertise v3, so the bridge issues zero requests (no enable/commit; the provider "
              "wiring creates no protocol traffic)");
        check(!surface.should_close(),
              "connection still healthy after pumping frames (degradation raised no protocol error or disconnect)");
        emit("[INFO] WSLg Weston never advertises v3, so this section is the degraded-path proof; the enable and");
        emit("       enter criteria need a live check on a compositor offering text-input (KDE/mutter/weston).");
        if (interactive) {
            emit("");
            emit("---- Manual: this compositor has no v3, IME events will not arrive; watch the frame loop ----");
            while (!surface.should_close()) {
                surface.poll_platform_events();
                surface.wait_events(50.0);
            }
        }
        emit(failures > 0 ? "FAILURES PRESENT (" + std::to_string(failures) + ")" : "ALL AUTOMATED CHECKS PASS");
        return failures > 0 ? 4 : 0;
    }

    // ---- ③ 绑定链 ----
    check(true, "compositor advertises zwp_text_input_manager_v3 (manager_bound), entering the full criteria section");
    check(st.input_created, "seat keyboard capability arrived -> zwp_text_input_v3 created (input_created)");
    if (!st.input_created) {
        emit(
            "[INFO] input_created=false: this seat has no keyboard (tablet session?), so the later enter/enable "
            "criteria cannot run");
    }

    // provider：非零插入点盒（=宿主判定「焦点在文本控件」）。真实应用里由 WindowHost 接线，
    // 探针直接模拟该激励——enable 判据只依赖「provider 报盒」，不依赖控件树。
    aurora::Rect caret_box{aurora::Point{64.0F, 96.0F}, aurora::Size{8.0F, 20.0F}};
    surface.set_composition_caret_provider([&caret_box] { return caret_box; });

    const bool entered =
        st.input_created && pump_until(surface, [&surface] { return surface.text_input_state().entered; }, 2000);
    if (!entered) {
        skip(
            "no zwp_text_input_v3.enter received (the compositor gave this surface no keyboard focus) -> the enable "
            "criterion is left to --interactive (one click on the window is enough)");
    } else {
        check(true, "keyboard focus landed -> text_input.enter arrived (server listener signatures are valid)");
        // ---- ④ enable 判据双向 + 去重 ----
        // ti_on_enter 内即刻按 provider 刷新：entered 观察到时 enable 应已随 commit 下发。
        const auto en_st = surface.text_input_state();
        check(en_st.enabled && en_st.commits > 0,
              "entered + non-zero caret rectangle -> enable+content_type+commit sent (enabled true, commits>0)");
        const int commits_after_enable = surface.text_input_state().commits;
        (void)surface.begin_frame(520, 360);
        (void)surface.present();
        (void)surface.begin_frame(520, 360);
        (void)surface.present();
        check(surface.text_input_state().commits == commits_after_enable,
              "repeated refresh in the same state deduplicates (zero protocol cost per presented frame)");
        caret_box = aurora::Rect{};  // 零盒 = 焦点离开文本控件
        const bool disabled = pump_until(surface, [&surface] { return !surface.text_input_state().enabled; }, 500);
        check(disabled && surface.text_input_state().commits > commits_after_enable,
              "caret rectangle zeroed -> disable+commit sent (enabled flips false)");
        caret_box = aurora::Rect{aurora::Point{64.0F, 96.0F}, aurora::Size{8.0F, 20.0F}};
        const bool re_enabled = pump_until(surface, [&surface] { return surface.text_input_state().enabled; }, 500);
        check(re_enabled, "non-zero rectangle again -> enable sent once more (reversible, no stickiness)");
    }

    // ---- preedit/commit/delete：需真实输入法进程 ----
    skip(
        "preedit/commit/delete_surrounding need a compositor-side input method process -> left to the --interactive "
        "manual section (preedit updates -> the candidate commits through one channel -> Esc leaves no residue)");

    if (interactive) {
        emit("");
        emit(
            "---- Manual: type pinyin in the window with a real input method; state/events print line by line "
            "(close the window to exit) ----");
        std::string last;
        while (!surface.should_close()) {
            surface.poll_platform_events();
            surface.wait_events(50.0);
            const std::string line = state_line(surface.text_input_state());
            if (line != last) {
                emit(line);
                last = line;
            }
        }
    }

    if (failures > 0) {
        emit("FAILURES PRESENT (" + std::to_string(failures) + ")");
        return 4;
    }
    emit("ALL AUTOMATED CHECKS PASS");
    return 0;
}
