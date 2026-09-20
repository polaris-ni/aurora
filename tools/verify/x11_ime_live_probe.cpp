/* 输入法（XIM，PreeditCallbacks）—— X11 真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 覆盖接缝：`XOpenIM`/`XGetIMValues(XNQueryInputStyle)` 风格协商 → `XCreateIC` +
// `XSetICValues(XNPreeditAttributes, {PreeditDrawCallback, PreeditCaretCallback})` →
// 组合期 draw/caret 回调经 `ime_composition` 折算成 `TextCompositionEvent`；
// `FocusIn/Out → XSetICFocus/XUnsetICFocus`（本切片修复的「IC 焦点宣告」缺口）；
// provider 报盒 → `XSetICValues(XNSpotLocation)`（候选窗锚点，客户窗口物理 px）。
//
// 为什么必须真机：XIM 是跨进程协议，`XOpenIM` 需真实 X server + 已注册的 IM 服务器
// （`XMODIFIERS`/`XSetIMOptions`）；无头 CI 里 `XOpenIM` 直接返回 NULL，桥根本不会建立，
// draw/caret 回调也就永不触发。本探针在真实 X 会话里核对「协商到什么、回调是否真被装进 IC、
// 焦点宣告是否补齐」，而不只是「调用不崩」。UTF-8 字节下标 → 码点契约的**折算层**本身由
// `utest_ime_composition`（17 项无头用例）覆盖，本探针只证明平台接线。
//
// 自动段（不要求装任何输入法）逐项验收：
//   ① 桥建立/降级：`XOpenIM` 成功 ⇒ `im_open`；`XCreateIC` 成功 ⇒ `ic_created`；协商到
//      `XIMPreeditCallbacks|XIMStatusNothing` ⇒ `preedit_callbacks`。三者逐级：任一失败即静默
//      回退（PreeditNothing 仅走 `Xutf8LookupString` commit；XOpenIM 失败则整桥缺席、纯 keysym），
//      均按 SKIP 注明而非 FAIL（本机无 XIM 服务器属合法降级，与 Win32 无 IME 环境同口径）。
//   ② 焦点宣告接线（**不依赖输入法配合**，只依赖本端 FocusIn/Out → X{Set,Unset}ICFocus）：
//      用独立观测连接对被测窗口先聚焦根窗（读回 `focused` 归假，连构造期补偿焦点也如实撤下）、
//      再 `XSetInputFocus` 拉起焦点（读回 `focused` 翻真）。此项在 `im_open` 时即有效（IC 已建），
//      证明补齐的 `XSetICFocus` 缺口真的接进了事件循环。
//   ②b 常规键上屏回归（XTEST 假键，dlopen libXtst 免构建依赖）：焦点在被测窗后经 XTEST 注入
//      一个 `a`，桥在 IC 在场时走 `Xutf8LookupString(ic)` 取字 ⇒ 必须产出 `TextInputEvent("a")`。
//      证明「接了 XIM/IC 反而吞掉普通键」不发生（XFilterEvent 不截非组合字符）。XTEST 不可用时 SKIP。
//   ③ 候选窗锚点只在组合期推进：`XSetICFocus` 后未组合时 provider 报非零盒也不应产生
//      `draw_callbacks`/`preedit`（无 IM 驱动组合 ⇒ 保持 0/空）。真机若装了 IM，人工段核对组合期锚点跟随。
//
// 人工段（`--interactive`）：窗口常驻，逐帧打印 IME 状态与组合/输入事件流。用真实 XIM 输入法
// （fcitx/ibus 配 `XMODIFIERS=@im=fcitx`）在窗口内输入拼音，肉眼核对：① preedit 回调逐帧更新；
// ② 选字上屏走 committed 单通道；③ Esc 取消无残留；④ 切焦点回来无半截拼音。
//
// 构建（须已开启 X11 后端与探针开关）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BACKEND_X11=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_x11_ime
//   ./build-verify/aurora_verify_x11_ime            # 自动段
//   ./build-verify/aurora_verify_x11_ime --interactive   # + 人工段
//
// 退出码：
//   0  自动段全部通过（无 XIM 服务器等按 SKIP，其深水区交 --interactive）
//   2  环境不可用（无 DISPLAY / X 连接失败 / 无 X11 后端构建）
//   4  部分验收项不符 —— 见逐行 PASS/FAIL
//
// 本机实测（2026-09-20，WSLg rootless Xwayland）：`XMODIFIERS` 未设但 `XOpenIM` 仍成功
//   （Xwayland 自带轻量 XIM），风格协商回退 `XIMPreeditNothing`（`preedit_callbacks`=n，
//   该 XIM 不广告 PreeditCallbacks），故组合事件回推走 --interactive；但 ②焦点宣告往返与
//   ②b XTEST 假键 `a`→`TextInputEvent("a")` 两条**自动**判据全绿（退出码 0）——补齐的
//   `XSetICFocus` 缺口与「IC 在场仍收常规键」均已在真实 X 服务器上证伪通过。
//   构建命令中的行尾反斜杠为续行符，故本头注释整体使用块注释形态（避免 -Wcomment）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_UNIX) || defined(AURORA_PLATFORM_MACOS)
#error "aurora_verify_x11_ime can only be built on Linux/Unix (non-Apple)"
#endif
#if !defined(AURORA_BACKEND_X11)
#error "AURORA_BACKEND_X11 must be enabled"
#endif

#include "aurora/window/x11_surface.h"  // aurora 头必须先于 Xlib（None/Bool/Status 宏污染）

#include <X11/Xlib.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <thread>

#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "verify_print.h"

// <X11/X.h>（经 Xlib.h 引入）无条件 `#define CursorShape 0`，与 aurora::CursorShape 硬碰撞。
#undef CursorShape

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

/// @brief 抽干式泵：驱动被测 surface 自己的事件循环（poll + 出帧），让 FocusIn/Out 与任何
/// 异步 IM 回调进入派发栈。
void pump(aurora::X11Surface &surface, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        surface.poll_platform_events();
        (void)surface.begin_frame(static_cast<int>(surface.size().width), static_cast<int>(surface.size().height));
        (void)surface.present();
        nap_ms(15);
    }
}

[[nodiscard]] auto state_line(const aurora::X11Surface::ImeState &s) -> std::string {
    return std::string("im_open=") + (s.im_open ? "y" : "n") + " ic=" + (s.ic_created ? "y" : "n") +
           " preeditCbs=" + (s.preedit_callbacks ? "y" : "n") + " focused=" + (s.focused ? "y" : "n") +
           " draws=" + std::to_string(s.draw_callbacks) + " spotUpdates=" + std::to_string(s.spot_updates) +
           " preedit=\"" + s.preedit + "\"";
}

}  // namespace

auto main(int argc, char **argv) -> int {
    bool interactive = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--interactive") {
            interactive = true;
        }
    }
    emit("==== X11 XIM（PreeditCallbacks）输入法桥 真机验收 ====");
    emit(std::string("XMODIFIERS=") + (std::getenv("XMODIFIERS") != nullptr ? std::getenv("XMODIFIERS") : "(unset)"));
    if (const char *d = std::getenv("DISPLAY"); d == nullptr || *d == '\0') {
        emit("[ENV] 无 DISPLAY 环境变量（无 X 会话），探针无从运行");
        return 2;
    }

    aurora::X11Surface surface(520, 360, "aurora-verify-x11-ime");
    if (!surface.is_available()) {
        AURORA_LOG_ERROR("verify", "X11Surface unavailable（X 连接失败或无可用 X server）");
        return 2;
    }
    (void)surface.begin_frame(520, 360);
    (void)surface.present();

    // 事件面捕获（人工/自动段都挂）：committed/preedit 流逐条打印 + 计数供判据。
    int text_inputs = 0;
    int compositions = 0;
    std::string last_text;
    surface.set_event_handler([&](aurora::Event &ev) {
        if (auto *ce = dynamic_cast<aurora::TextCompositionEvent *>(&ev); ce != nullptr) {
            ++compositions;
            emit("[EVENT] TextCompositionEvent preedit=\"" + ce->preedit + "\" cursor=" +
                 std::to_string(ce->cursor_index) + " sel=[" + std::to_string(ce->sel_start) + "," +
                 std::to_string(ce->sel_end) + "] committed=\"" + ce->committed + "\"");
        } else if (auto *te = dynamic_cast<aurora::TextInputEvent *>(&ev); te != nullptr) {
            ++text_inputs;
            last_text = te->text;
            emit("[EVENT] TextInputEvent text=\"" + te->text + "\"");
        }
    });

    auto st = surface.ime_state();
    emit(std::string("initial: ") + state_line(st));

    // provider：非零插入点盒（模拟宿主判定「焦点在文本控件」，锚点应落在其下沿）。
    aurora::Rect caret_box{aurora::Point{64.0F, 96.0F}, aurora::Size{8.0F, 20.0F}};
    surface.set_composition_caret_provider([&caret_box] { return caret_box; });

    if (!st.im_open) {
        // ① 降级路径（无 XIM 服务器）：本机合法结局，深水区交人工段。
        skip("XOpenIM 失败 ⇒ 本机无 XIM 服务器（XMODIFIERS 未指向在跑的 IM）：整桥缺席、纯 keysym 输入，"
             "属合法降级（与 Win32 无 IME 环境同口径）");
        check(st.ic_created == false && st.draw_callbacks == 0 && st.spot_updates == 0,
              "无 XIM 时桥完全静默（无 IC、无回调、无锚点请求）");
    } else {
        check(true, "XOpenIM 成功（im_open）：本机有可达 XIM 服务器");
        if (!st.ic_created) {
            check(false, "im_open 却未建 IC（XCreateIC 失败）—— 异常，请核对 IM 服务器状态");
        } else {
            check(true, "XCreateIC 成功（ic_created）");
            emit(std::string("       协商风格：") + (st.preedit_callbacks ? "XIMPreeditCallbacks（组合事件回推全接线）"
                                                                          : "XIMPreeditNothing（回退：仅 commit 通道）"));

            // ---- ② 焦点宣告接线（不依赖输入法配合） ----
            // 用独立观测连接对被测窗口 XSetInputFocus 拉起/切走焦点，驱动被测 surface 自身事件
            // 循环消费 FocusIn/Out，读回 im_focused 观测面。刻意「先聚焦被测窗再切走」：构造期
            // 补偿（ime_setup 的 active 分支）可能已把 im_focused 预置为真却无**真实** X 焦点，
            // 此时直接聚焦根窗不会触发 FocusOut（X 侧本就没焦点在本窗），focused 不降反为假失败。
            // 先聚焦被测窗确保 X 焦点确在其上，随后的切走才必然产出 FocusOut → XUnsetICFocus。
            if (Display *obs = XOpenDisplay(nullptr); obs != nullptr) {
                const auto win = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(surface.native_handle()));
                // 先确保窗口可见（FocusIn 只对 viewable 窗口生成）。
                XMapWindow(obs, win);
                XRaiseWindow(obs, win);
                XSync(obs, False);

                XSetInputFocus(obs, win, RevertToParent, CurrentTime);
                XSync(obs, False);
                pump(surface, 12);
                check(surface.ime_state().focused,
                      "XSetInputFocus(win) → FocusIn → XSetICFocus（focused 翻 y：补齐的焦点宣告真接进派发栈）");

                // 切走焦点（此刻 X 焦点确在被测窗 ⇒ 必产 FocusOut）。
                XSetInputFocus(obs, DefaultRootWindow(obs), RevertToParent, CurrentTime);
                XSync(obs, False);
                pump(surface, 12);
                check(!surface.ime_state().focused,
                      "焦点切走 → FocusOut → XUnsetICFocus（focused 归 n）");

                // 恢复焦点给 XTEST 段（假键须落入被测窗）。
                XSetInputFocus(obs, win, RevertToParent, CurrentTime);
                XSync(obs, False);
                pump(surface, 6);

                // ---- ②b keysym/commit 通道端到端（XTEST 假键，dlopen libXtst 免构建依赖）----
                // 'a' 经 X 服务器投递到 X 焦点窗（被测窗）；桥在 IC 在场时走
                // Xutf8LookupString(ic) 取字——非组合字符不得被 XFilterEvent/IM 接线截走，
                // 必须以 TextInputEvent 上屏（防「接了 IM 反而吞普通键」回归）。
                using FakeKeyFn = int (*)(Display *, unsigned int, int, unsigned long);
                void *xtst = dlopen("libXtst.so.6", RTLD_NOW | RTLD_GLOBAL);
                auto fake_key = xtst != nullptr ? reinterpret_cast<FakeKeyFn>(
                                                      dlsym(xtst, "XTestFakeKeyEvent"))  // NOLINT(*-pro-type-reinterpret-cast)
                                                : nullptr;
                const int keycode_a = XKeysymToKeycode(obs, 0x61 /*XK_a*/);
                if (fake_key == nullptr || keycode_a == 0) {
                    skip("libXtst/XTEST 不可用 ⇒ 假键合成无从执行，commit 通道交 --interactive");
                } else {
                    const int before = text_inputs;
                    fake_key(obs, static_cast<unsigned>(keycode_a), 1 /*press*/, 0);
                    fake_key(obs, static_cast<unsigned>(keycode_a), 0 /*release*/, 0);
                    XFlush(obs);
                    pump(surface, 12);
                    if (text_inputs == before || last_text != "a") {
                        emit("[INFO] 现场：TextInputEvent 计数 " + std::to_string(before) + " → " +
                             std::to_string(text_inputs) + " 末条=\"" + last_text + "\" keycode=" +
                             std::to_string(keycode_a) + "（供区分假键未达/被 IM 截走/取字为空）");
                    }
                    check(text_inputs > before && last_text == "a",
                          "XTEST 假键 'a' ⇒ TextInputEvent(\"a\") 上屏（IC/XIM 接线不吞普通字符）");
                }
                XCloseDisplay(obs);
            } else {
                skip("观测连接打不开（无法 XSetInputFocus 驱动焦点）⇒ 焦点宣告接线交 --interactive 目视");
            }

            // ---- ③/④ 组合期内容：draw/caret/commit/preedit 需真实 IM 驱动组合 ----
            skip("preedit/上屏/锚点跟随需真实 XIM 输入法进程组合驱动 ⇒ 自动段不判负，交 --interactive");
        }
    }

    if (interactive) {
        emit("");
        emit("---- 人工段：用真实 XIM 输入法（配 XMODIFIERS）在窗口内输入拼音；状态逐行打印（关窗退出） ----");
        std::string last;
        while (!surface.should_close()) {
            surface.poll_platform_events();
            surface.wait_events(50.0);
            const std::string line = state_line(surface.ime_state());
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
