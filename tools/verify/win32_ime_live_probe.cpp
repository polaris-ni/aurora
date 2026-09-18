/* Win32 IMM32 输入法桥 —— 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖接缝：`WM_IME_*` 消息族 → `detail::Win32ImeBridge` → `TextCompositionEvent`
// → `EventDispatcher`（仅派发给焦点控件）→ `TextInput` 组合显示态，以及候选窗定位盒
// （`Widget::composition_caret_bounds()` → `Surface::set_composition_caret_provider`）。
// 后端：`Win32Surface`（GDI）与 `D3D11Surface`（GPU）共用同一 `Win32Window` 宿主与同一份桥，
// 故本探针只跑一路（GDI），另一路无需重复验收。
//
// 为什么必须真机：无头 CI 里没有真实窗口，`ImmGetContext` 取不到上下文、`WM_IME_*` 永不投递，
// 而**事件派发与焦点序本身也不存在**——裸 `Window` 没有 handler，只有 `Application` 登记的
// `WindowHost` 才把后端事件接进 `EventDispatcher`。本探针因此走正规应用路径
// （Scene + Window → Application → run），覆盖「桥与 Windows IMM32 的接缝」与
// 「组合事件是否真的落到焦点控件」。单元测试（`utest_ime_composition`）覆盖的则是平台中立的
// 索引折算层（UTF-16 单元 → 码点、`GCS_COMPATTR` → 待转换选区、代理对夹紧）。
//
// 自动段（无需安装任何第三方输入法）逐项验收：
//   ① 焦点建立：`VK_TAB` 经真实消息路径落焦到 **唯一一个**可聚焦控件 `TextInput`，再用一条
//      普通 `WM_CHAR` 证明输入链路通（`value=="a"`）。
//   ② 组合期吞字纪律：`WM_IME_STARTCOMPOSITION` 之后，`WM_IME_CHAR` 与输入法回发的残余
//      `WM_CHAR` 都不得落字——否则与 `GCS_RESULTSTR` 通道叠加即「同一汉字上屏两次」
//      （DBCS ACP 下 `DefWindowProc` 还会按前导/尾随字节拆分产出乱码）。
//   ③ 组合结束即恢复收字：`WM_IME_ENDCOMPOSITION` 后 `WM_CHAR` 必须重新上屏，证明 ②的抑制窗口
//      有界、不会永久吞键。
//   ④ preedit / 上屏链路：**尽力而为**——需往本窗口 IME 上下文注入组合串
//      （`ImmSetCompositionStringW(SCS_SETSTR|GCS_COMPSTR)`）。Win10/11 的 TSF 型 IME（微软拼音等）
//      不接受外部写入（返回 FALSE），此时该项按 SKIP 处理并注明原因，改由人工段覆盖；
//      注入成功则逐条断言 `preedit()`（UTF-8）、`composition_cursor()`（码点下标）、`value()` 不含
//      preedit、上屏后 committed 单通道落字，以及候选窗定位盒非零（几何链路通）。
//
// 人工段（`--interactive`）：窗口常驻，逐帧轮询并打印组合态变化（preedit/光标/value/插入点）。
// 用微软拼音/五笔输入「你好世界」，肉眼核对：① preedit 带下划线且随拼音更新；② 候选窗出现在
// 插入点旁而非屏幕左上角；③ 选字后只上一次屏；④ Esc 取消后无残留；⑤ 切走焦点再回来无半截拼音。
//
// 构建：
//   cmake -S . -B build-verify-ime -G Ninja -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify-ime --target aurora_verify_win32_ime
//   build-verify-ime\aurora_verify_win32_ime.exe [--interactive]
//
// 退出码：
//   0  自动段全部通过（SKIP 项已注明，其覆盖交由 --interactive 人工段）
//   2  环境不可用（建窗/HWND 取不到）
//   4  部分验收项不符（含焦点未能建立）—— 见逐行 PASS/FAIL
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_WINDOWS)
#error "aurora_verify_win32_ime can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#if !defined(AURORA_BACKEND_WIN32) && !defined(AURORA_BACKEND_D3D11)
#error "AURORA_BACKEND_WIN32 or AURORA_BACKEND_D3D11 must be enabled"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
#include <imm.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/core/types.h"
#include "aurora/widget/node.h"
#include "aurora/widget/text_input.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
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

/// @brief UTF-8 → UTF-16（写 IME 上下文用；本探针自带的独立实现，不复用桥内代码）。
[[nodiscard]] auto to_utf16(const std::string &utf8) -> std::u16string {
    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wide <= 0) {
        return {};
    }
    std::u16string out(static_cast<std::size_t>(wide), u'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        reinterpret_cast<LPWSTR>(out.data()), wide);
    return out;
}

/// @brief 把一段 UTF-16 文本写进窗口 IME 上下文的指定 `GCS_*` 槽位（空串 = 清空该槽）。
///
/// MinGW 把 `ImmSetCompositionStringW` 的 `lpComp` 声明为非 const `LPVOID`，故需脱去 const；
/// 该 API 只读取缓冲内容，不写回。
[[nodiscard]] auto set_comp_string(HIMC himc, DWORD gcs_index, const std::u16string &text) -> bool {
    if (himc == nullptr) {
        return false;
    }
    char16_t *bytes = const_cast<char16_t *>(text.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    return ImmSetCompositionStringW(himc, SCS_SETSTR | gcs_index, bytes,
                                    static_cast<DWORD>(text.size() * sizeof(char16_t)), nullptr, 0) != FALSE;
}

/// @brief 写组合光标位置（UTF-16 单元下标）。`GCS_CURSORPOS` 的值本身即下标，非字符串。
[[nodiscard]] auto set_caret(HIMC himc, std::size_t utf16_index) -> bool {
    if (himc == nullptr) {
        return false;
    }
    SHORT pos = static_cast<SHORT>(utf16_index);
    return ImmSetCompositionStringW(himc, SCS_SETSTR | GCS_CURSORPOS, &pos, sizeof(pos), nullptr, 0) != FALSE;
}

/// @brief 上下文里当前缓存的组合串（用于核对平台侧是否真的被取消）。
[[nodiscard]] auto comp_string_of(HIMC himc) -> std::u16string {
    if (himc == nullptr) {
        return {};
    }
    const LONG need = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::u16string out(static_cast<std::size_t>(need) / 2U, u'\0');
    const LONG written = ImmGetCompositionStringW(himc, GCS_COMPSTR, out.data(), static_cast<DWORD>(need));
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written) / 2U);
    return out;
}

/// @brief 探针的 UI：**只有一个** `TextInput`（默认可聚焦的控件不止输入框，多个可聚焦者会让
///        单次 Tab 落在谁身上取决于 Tab 序，探针要的是确定性）。
struct ProbeUi {
    aurora::Node root;
    std::shared_ptr<aurora::TextInput> input;
};

[[nodiscard]] auto build_ui() -> ProbeUi {
    auto input = std::make_shared<aurora::TextInput>(aurora::TextInputProps{.value = "", .placeholder = "请键入"});
    // Node 拷贝即共享（`shared_ptr<Widget>` 语义），故树内节点与本指针是同一个 TextInput。
    return ProbeUi{.root = aurora::Node{std::static_pointer_cast<aurora::Widget>(input)}, .input = std::move(input)};
}

/// @brief 打印插入点盒（dp）供人工比对候选窗位置。
auto report_caret(const char *phase, const aurora::Rect &box) -> void {
    emit(std::string("caret[") + phase + "] x=" + aurora_verify::pad_right(std::to_string(box.origin.x), 8) +
         " y=" + aurora_verify::pad_right(std::to_string(box.origin.y), 8) +
         " h=" + aurora_verify::pad_right(std::to_string(box.size.height), 8));
}

/// @brief 人工段：逐帧轮询打印组合态，直到窗口关闭（无自动判定，供真实输入法目视）。
auto report_interactive(aurora::TextInput &input, std::string &last_line) -> void {
    const std::string line = "preedit=\"" + input.preedit() + "\" cursor=" +
                             std::to_string(input.composition_cursor()) + " value=\"" + input.value() + "\" caret=(" +
                             std::to_string(input.composition_caret_bounds().origin.x) + "," +
                             std::to_string(input.composition_caret_bounds().origin.y) + ")";
    if (line != last_line) {
        emit(line);
        last_line = line;
    }
}

/// @brief 尽力打开 IME 并请求原生组合模式，然后呈现上下文状态。
///
/// 合成段失败时，这行输出用于区分两种截然不同的原因：「桥未接线」（状态正常却读不回 preedit）
/// 与「本机没有可合成的 IME」（open=no / native=no —— 纯英文键盘布局即如此，须走人工段）。
auto enable_and_report_ime(HIMC himc) -> void {
    (void)ImmSetOpenStatus(himc, TRUE);
    (void)ImmSetConversionStatus(himc, IME_CMODE_NATIVE, 0);
    DWORD conv = 0;
    DWORD sent = 0;
    (void)ImmGetConversionStatus(himc, &conv, &sent);
    emit(std::string("[INFO] IME 上下文：open=") + (ImmGetOpenStatus(himc) != FALSE ? "yes" : "no") +
         " native=" + ((conv & IME_CMODE_NATIVE) != 0U ? "yes" : "no") + " conversion=" + std::to_string(conv));
}

/// @brief 自动段：合成一整套 IMM32 组合序列并逐项断言控件侧读回的契约口径。
auto run_automated(HWND hwnd, aurora::TextInput &input, const char *focused_type) -> void {
    // 焦点：VK_TAB + WM_CHAR 经真实消息路径落焦（组合事件只派发给焦点控件）。
    emit(std::string("focused widget = ") + (focused_type != nullptr ? focused_type : "(none)"));
    if (input.value() != "a") {
        AURORA_LOG_ERROR("verify", std::string("focus not established: value=\"") + input.value() + "\"");
        ++failures;
        return;
    }
    emit("[PASS] 焦点经 VK_TAB 落到 TextInput（普通 WM_CHAR 上屏，value=\"a\"）");
    report_caret("idle", input.composition_caret_bounds());

    // ---- ① 组合期吞字纪律（**不依赖任何 IME 配合**：只依赖桥的 START/END 状态机）----
    // `WM_IME_STARTCOMPOSITION` 后，`WM_IME_CHAR` 与输入法回发的残余 `WM_CHAR` 都不得再落字，
    // 否则与 `GCS_RESULTSTR` 通道叠加即「同一汉字上屏两次」。
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_IME_CHAR, static_cast<WPARAM>('b'), 0);
    SendMessageW(hwnd, WM_CHAR, static_cast<WPARAM>('c'), 0);
    check(input.value() == "a", "组合期：WM_IME_CHAR 与残余 WM_CHAR 均被吞（无上屏重复）");

    // ---- ② 组合结束即恢复收字（抑制窗口有界，不会永久吞键）----
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_CHAR, static_cast<WPARAM>('d'), 0);
    check(input.value() == "ad", "ENDCOMPOSITION 后普通 WM_CHAR 恢复上屏（value=\"ad\"）");

    // ---- ③ preedit / 上屏链路：需往 IME 上下文注入组合串，环境相关（见下方说明）----
    HIMC himc = ImmGetContext(hwnd);
    if (himc == nullptr) {
        skip("无 IME 上下文 ⇒ preedit/上屏 断言交由 --interactive 人工段");
        return;
    }
    enable_and_report_ime(himc);
    const std::u16string comp = to_utf16("你好");
    // Win10/11 的 TSF 型 IME（微软拼音等）不接受第三方**写入**上下文组合串（`SCS_SETSTR` 返回 FALSE
    // 且LastError 置 0），故本项只能尽力而为；被拒时 preedit/上屏 的核对落在人工段。
    if (!set_comp_string(himc, GCS_COMPSTR, comp) || !set_caret(himc, 1)) {
        skip("ImmSetCompositionStringW 被拒（本机 IME 为 TSF 型，不容外部写组合串）⇒ preedit/上屏 交 --interactive");
        ImmReleaseContext(hwnd, himc);
        return;
    }
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, static_cast<LPARAM>(GCS_COMPSTR | GCS_CURSORPOS));
    check(input.is_composing(), "组合期：TextInput 进入组合态（preedit 可见）");
    check(input.preedit() == "你好", "组合期：preedit 为 UTF-8「你好」（GCS_COMPSTR 读回）");
    // 「你好」皆 BMP：UTF-16 单元下标 1 == 码点下标 1（非 BMP 的夹紧由单测覆盖）。
    check(input.composition_cursor() == 1, "组合期：光标折算为码点下标 1");
    check(input.value() == "ad", "组合期：value() 不含 preedit（数据模型保持干净）");
    report_caret("composing", input.composition_caret_bounds());
    check(input.composition_caret_bounds().size.height > 0.0F, "候选窗定位盒有高度（几何链路通）");

    // ---- ④ 上屏：GCS_RESULTSTR 单通道落字，preedit 撤下 ----
    // 真实 IME 上屏时组合串已随结果清空，故先清 `GCS_COMPSTR` 再送结果。
    (void)set_comp_string(himc, GCS_COMPSTR, {});
    if (!set_comp_string(himc, GCS_RESULTSTR, comp)) {
        skip("GCS_RESULTSTR 注入被拒 ⇒ 上屏断言交由 --interactive 人工段（选字后应得 \"ad你好\"）");
    } else {
        SendMessageW(hwnd, WM_IME_COMPOSITION, 0, static_cast<LPARAM>(GCS_RESULTSTR | GCS_COMPSTR | GCS_CURSORPOS));
        check(input.value() == "ad你好", "上屏：committed 文本经组合事件单通道落入 value");
        check(!input.is_composing(), "上屏：preedit 清空（无残留下划线）");
    }

    // ---- ⑤ 失焦取消：桥向 IME 发 CPS_CANCEL。取消由 IME 侧执行、环境相关 ⇒ 只呈现不判负 ----
    if (set_comp_string(himc, GCS_COMPSTR, comp)) {
        SendMessageW(hwnd, WM_IME_COMPOSITION, 0, static_cast<LPARAM>(GCS_COMPSTR));
        SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
        HIMC after = ImmGetContext(hwnd);
        const bool cleared = comp_string_of(after).empty();
        ImmReleaseContext(hwnd, after);
        emit(std::string("[INFO] 失焦后 IME 上下文组合串已清空 = ") + (cleared ? "yes" : "no") +
             "（本库只投递 CPS_CANCEL，取消动作由 IME 执行）");
    }

    ImmReleaseContext(hwnd, himc);
}

}  // namespace

auto main(int argc, char **argv) -> int {
    const bool interactive = argc > 1 && std::string(argv[1]) == "--interactive";
    emit("==== Win32 IMM32 输入法桥 真机验收 ====");

    ProbeUi ui = build_ui();
    auto made = aurora::create_native_window(aurora::WindowOptions{
        .size = aurora::Size{.width = 480.0F, .height = 300.0F}, .title = "Aurora IME verify"});
    if (!made) {
        AURORA_LOG_ERROR("verify", "cannot create native window");
        return 2;
    }
    aurora::Window *window = made.value().get();
    auto *hwnd = static_cast<HWND>(window->surface().native_handle());
    if (hwnd == nullptr) {
        AURORA_LOG_ERROR("verify", "cannot obtain HWND");
        return 2;
    }
    // 事件派发与焦点序由 `Application` 的宿主接线提供（裸 `Window` 无 handler）；
    // 故本探针走正规应用路径：Scene + Window → Application → run()。
    aurora::Application app(aurora::Scene{ui.root}, std::move(made.value()),
                            aurora::WindowOptions{.size = aurora::Size{.width = 480.0F, .height = 300.0F},
                                                  .title = "Aurora IME verify",
                                                  .max_frames = interactive ? -1 : 120});
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    // 焦点建立走真实消息路径：Tab 落焦 → 键入 'a'（自动段据此判定「有焦点且输入链路通」）。
    PostMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>(VK_TAB), 0);
    PostMessageW(hwnd, WM_KEYUP, static_cast<WPARAM>(VK_TAB), 0);
    PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>('a'), 0);

    int frame = 0;
    std::string last_line;
    app.set_on_frame([&]() -> void {
        ++frame;
        if (frame == 3) {
            aurora::Widget *focused = app.focus().focused();
            run_automated(hwnd, *ui.input, focused != nullptr ? focused->type_name() : nullptr);
            if (!interactive) {
                app.quit();
            } else {
                emit("");
                emit("---- 人工段：请用真实输入法（微软拼音/五笔…）在窗口内输入「你好世界」 ----");
                emit("期望：preedit 带下划线更新 → 候选窗贴在插入点旁 → 选字只上一次屏 → Esc 取消无残留");
                emit("（本段无自动判定；关闭窗口即退出）");
            }
        }
        if (interactive) {
            report_interactive(*ui.input, last_line);
        }
    });
    app.run();

    if (!interactive) {
        emit("");
        emit("自动段结束。要核对真实输入法（候选窗位置 / 选字上屏），请加 --interactive 重跑。");
    }

    if (failures > 0) {
        emit("FAILURES PRESENT (" + std::to_string(failures) + ")");
        return 4;
    }
    emit("ALL AUTOMATED CHECKS PASS");
    return 0;
}
