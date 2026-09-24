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
// 本机实测（2026-09-20，WSLg Weston）：registry globals **未发布** zwp_text_input_manager_v3
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
    emit("==== Wayland text-input-unstable-v3 输入法桥 真机验收 ====");
    if (const char *wdpy = std::getenv("WAYLAND_DISPLAY"); wdpy == nullptr || *wdpy == '\0') {
        emit("[ENV] 无 WAYLAND_DISPLAY（无 Wayland 会话），探针无从运行");
        return 2;
    }

    aurora::WaylandSurface surface(520, 360, "aurora-verify-wayland-ime");
    if (!surface.is_available()) {
        AURORA_LOG_ERROR("verify", "WaylandSurface unavailable（合成器连接失败或缺 compositor/shm/xdg_wm_base）");
        return 2;
    }
    // 先出帧：xdg_toplevel 带缓冲 commit 后才被合成器视为 mapped，才可能获得键盘焦点。
    if (!surface.begin_frame(520, 360)) {
        AURORA_LOG_ERROR("verify", "begin_frame 失败，窗口进不了 mapped 态");
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
            "[ENV] 本次构建缺 text-input-unstable-v3 XML（AURORA_HAVE_WL_TEXT_INPUT=0）⇒ 桥未编译，"
            "装 wayland-protocols 后重配置构建再跑");
        return 2;
    }
    check(true, "协议代码生成在场（protocol_disabled=false，listener/请求静态合法已由编译证明）");

    // ---- ② 未发布 manager：优雅降级（WSLg Weston 即此路） ----
    if (!st.manager_bound) {
        pump_until(surface, [] { return false; }, 400);  // 纯泵 400ms：全局若晚到也能绑上
        st = surface.text_input_state();
        emit(std::string("after pump: ") + state_line(st));
        check(!st.manager_bound && !st.input_created && !st.enabled && st.commits == 0,
              "合成器未发布 v3 ⇒ 桥零请求（无 enable/commit，provider 接线不产生协议流量）");
        check(!surface.should_close(), "多帧泵送后连接仍健康（降级未触发协议错误/断连）");
        emit("[INFO] WSLg Weston 长期不发布 v3：本段即降级路证明。enable 判据与 enter 链需");
        emit("       提供 text-input 的合成器（KDE/mutter/weston 带输入法扩展）真机复核。");
        if (interactive) {
            emit("");
            emit("---- 人工段：本合成器无 v3，输入法事件不会到达；仅观察帧循环无扰动 ----");
            while (!surface.should_close()) {
                surface.poll_platform_events();
                surface.wait_events(50.0);
            }
        }
        emit(failures > 0 ? "FAILURES PRESENT (" + std::to_string(failures) + ")" : "ALL AUTOMATED CHECKS PASS");
        return failures > 0 ? 4 : 0;
    }

    // ---- ③ 绑定链 ----
    check(true, "合成器发布 zwp_text_input_manager_v3（manager_bound），进入完整判据段");
    check(st.input_created, "seat 键盘能力到达 ⇒ zwp_text_input_v3 已建（input_created）");
    if (!st.input_created) {
        emit("[INFO] input_created=false：本 seat 无键盘（平板会话？），后续 enter/enable 判据无从执行。");
    }

    // provider：非零插入点盒（=宿主判定「焦点在文本控件」）。真实应用里由 WindowHost 接线，
    // 探针直接模拟该激励——enable 判据只依赖「provider 报盒」，不依赖控件树。
    aurora::Rect caret_box{aurora::Point{64.0F, 96.0F}, aurora::Size{8.0F, 20.0F}};
    surface.set_composition_caret_provider([&caret_box] { return caret_box; });

    const bool entered =
        st.input_created && pump_until(surface, [&surface] { return surface.text_input_state().entered; }, 2000);
    if (!entered) {
        skip(
            "未收到 zwp_text_input_v3.enter（合成器未把键盘焦点给本表面）⇒ enable 判据交 --interactive "
            "（点一下窗口即可）");
    } else {
        check(true, "键盘焦点落入 ⇒ text_input.enter 到达（服务端 listener 表签名合法）");
        // ---- ④ enable 判据双向 + 去重 ----
        // ti_on_enter 内即刻按 provider 刷新：entered 观察到时 enable 应已随 commit 下发。
        const auto en_st = surface.text_input_state();
        check(en_st.enabled && en_st.commits > 0,
              "entered + 非零插入点盒 ⇒ enable+content_type+commit 已下发（enabled 翻真、commits>0）");
        const int commits_after_enable = surface.text_input_state().commits;
        (void)surface.begin_frame(520, 360);
        (void)surface.present();
        (void)surface.begin_frame(520, 360);
        (void)surface.present();
        check(surface.text_input_state().commits == commits_after_enable,
              "同状态重复刷新去重（每帧 present 零协议开销）");
        caret_box = aurora::Rect{};  // 零盒 = 焦点离开文本控件
        const bool disabled = pump_until(surface, [&surface] { return !surface.text_input_state().enabled; }, 500);
        check(disabled && surface.text_input_state().commits > commits_after_enable,
              "插入点盒归零 ⇒ disable+commit 下发（enabled 翻假）");
        caret_box = aurora::Rect{aurora::Point{64.0F, 96.0F}, aurora::Size{8.0F, 20.0F}};
        const bool re_enabled = pump_until(surface, [&surface] { return surface.text_input_state().enabled; }, 500);
        check(re_enabled, "重新非零盒 ⇒ 再次 enable（判据可逆，无粘滞）");
    }

    // ---- preedit/commit/delete：需真实输入法进程 ----
    skip(
        "preedit/上屏/delete_surrounding 需合成器侧输入法进程配合 ⇒ 交 --interactive 人工段"
        "（判据：preedit 更新 → 选字单通道上屏 → Esc 无残留）");

    if (interactive) {
        emit("");
        emit("---- 人工段：请用真实输入法在窗口内键入拼音；状态/事件逐行打印（关窗退出） ----");
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
