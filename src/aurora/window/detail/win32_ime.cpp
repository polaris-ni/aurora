// Win32 IMM32 组合输入桥实现：`WM_IME_*` 消息族 → Aurora 组合事件 + 候选窗定位。
//
// 索引折算（UTF-16 单元 → 码点下标、GCS_COMPATTR → 待转换选区）不在本文件里手写，
// 一律走 `ime_composition.h` 的纯函数——那层有真机不可自动化的单测覆盖。

#include "win32_ime.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/a11y_text.h"
#include "ime_composition.h"

namespace aurora::detail {

namespace {

/// @brief 读组合串的字符串类 flag（`GCS_COMPSTR` / `GCS_RESULTSTR`）。
///
/// `ImmGetCompositionStringW` 的缓冲语义：首次传空缓冲返回**所需字节数**，二次返回**实际写入字节数**。
/// 按 UTF-16 单元数还原即 `字节数 / 2`。
[[nodiscard]] auto read_comp_string(HIMC himc, DWORD flag) -> std::u16string {
    const LONG need = ImmGetCompositionStringW(himc, flag, nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::u16string out(static_cast<std::size_t>(need) / 2U, u'\0');
    const LONG written = ImmGetCompositionStringW(himc, flag, out.data(), static_cast<DWORD>(need));
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written) / 2U);
    return out;
}

/// @brief 读 `GCS_COMPATTR`：逐 UTF-16 单元一字节属性。
[[nodiscard]] auto read_comp_attrs(HIMC himc) -> std::vector<std::uint8_t> {
    const LONG count = ImmGetCompositionStringW(himc, GCS_COMPATTR, nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::vector<std::uint8_t> attrs(static_cast<std::size_t>(count), 0);
    const LONG written = ImmGetCompositionStringW(himc, GCS_COMPATTR, attrs.data(), static_cast<DWORD>(count));
    if (written <= 0) {
        return {};
    }
    attrs.resize(static_cast<std::size_t>(written));
    return attrs;
}

}  // namespace

auto Win32ImeBridge::handle(UINT msg, LPARAM lp) -> std::optional<LRESULT> {
    switch (msg) {
        case WM_IME_STARTCOMPOSITION:
            composing_ = true;
            comp_.clear();
            position_candidate_window();
            return 0;
        case WM_IME_COMPOSITION: {
            HIMC himc = acquire_context();
            if (himc == nullptr) {
                return 0;  // 无 IME 上下文：本消息无事可做
            }
            std::string committed;
            if ((lp & GCS_RESULTSTR) != 0) {
                committed = a11y::utf16_to_utf8(read_comp_string(himc, GCS_RESULTSTR));
            }
            if ((lp & GCS_COMPSTR) != 0) {
                comp_ = read_comp_string(himc, GCS_COMPSTR);
            }
            // lParam==0：部分输入法用「空标志」表示状态刷新，组合串按缓存重发。
            const bool state_update =
                (lp == 0) || (lp & (GCS_RESULTSTR | GCS_COMPSTR | GCS_CURSORPOS | GCS_COMPATTR)) != 0;
            if (state_update) {
                // GCS_CURSORPOS 的返回值**本身**是组合串内的 WCHAR 下标；-1 = 该 IME 不提供，
                // 退化为串尾（新串通常整体待选，串尾即候选插入点）。
                const LONG pos = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
                const std::size_t caret = (pos >= 0) ? static_cast<std::size_t>(pos) : comp_.size();
                const std::vector<std::uint8_t> attrs =
                    ((lp & GCS_COMPATTR) != 0) ? read_comp_attrs(himc) : std::vector<std::uint8_t>{};
                emit_state(caret, attrs, committed);
                composing_ = !comp_.empty();
            }
            ImmReleaseContext(hwnd_, himc);
            position_candidate_window();
            return 0;
        }
        case WM_IME_ENDCOMPOSITION: {
            // 组合结束（Esc / 取消）：若仍有未上屏 preedit，投空串让控件撤下划线。
            const bool had_preedit = !comp_.empty();
            composing_ = false;
            comp_.clear();
            if (had_preedit) {
                emit_state(0, {}, {});
            }
            return 0;
        }
        case WM_IME_CHAR:
            // 吞掉：见头文件认领集合说明（避免与 GCS_RESULTSTR 双通道重复上屏 / DBCS 拆字节）。
            return 0;
        case WM_KILLFOCUS:
            // 焦点离开窗口：向 IME 取消未上屏组合，否则输入法侧留着半截拼音，
            // 再次点入同一控件时会把旧串一起带出来（控件侧 preedit 由 `on_focus_change` 清）。
            cancel_platform_composition();
            return std::nullopt;  // 交 DefWindowProc 继续常规失焦处理
        default:
            return std::nullopt;
    }
}

auto Win32ImeBridge::cancel_platform_composition() -> void {
    HIMC himc = acquire_context();
    if (himc == nullptr) {
        return;
    }
    ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    ImmReleaseContext(hwnd_, himc);
    composing_ = false;
    comp_.clear();
}

[[nodiscard]] auto Win32ImeBridge::acquire_context() -> HIMC {
    return (hwnd_ == nullptr) ? nullptr : ImmGetContext(hwnd_);
}

auto Win32ImeBridge::emit_state(std::size_t caret_utf16, const std::vector<std::uint8_t> &attrs,
                                const std::string &committed) -> void {
    if (!hooks_.emit) {
        return;
    }
    TextCompositionEvent e = ime::make_preedit_state(comp_, caret_utf16, attrs);
    e.committed = committed;
    hooks_.emit(e);
}

auto Win32ImeBridge::position_candidate_window() -> void {
    if (hwnd_ == nullptr || !hooks_.caret_bounds || !hooks_.scale_factor) {
        return;
    }
    const Rect box = hooks_.caret_bounds();
    // 零盒 = 尚未绘制 / 无焦点控件：不强行指定，退化为系统默认位置（跟随光标）。
    if (box.size.height <= 0.0F) {
        return;
    }
    const float scale = hooks_.scale_factor();
    // 逻辑 dp → 客户区物理像素，再转屏幕坐标；取盒**左下角**（插入点基线处）。
    POINT pt{.x = static_cast<LONG>(box.origin.x * scale),
             .y = static_cast<LONG>((box.origin.y + box.size.height) * scale)};
    if (ClientToScreen(hwnd_, &pt) == 0) {
        return;
    }
    HIMC himc = acquire_context();
    if (himc == nullptr) {
        return;
    }
    CANDIDATEFORM cf{};
    cf.dwStyle = CFS_CANDIDATEPOS;
    cf.ptCurrentPos = pt;
    // rcArea 是候选窗可放置包围盒（部分 IME 只认它）：给插入点右下方一段余量。
    cf.rcArea = {.left = pt.x, .top = pt.y, .right = pt.x + 400, .bottom = pt.y + 240};
    ImmSetCandidateWindow(himc, &cf);
    // CFS_EXCLUDE：preedit 由本应用自绘（带下划线的组合串），声明插入点附近排除 IME 自带绘制，
    // 避免两套预编辑串叠影。区域取插入点盒左右各留 4dp（覆盖下划线串所在行）。
    cf.dwStyle = CFS_EXCLUDE;
    cf.rcArea = {.left = static_cast<LONG>((box.origin.x - 4.0F) * scale),
                 .top = static_cast<LONG>(box.origin.y * scale),
                 .right = static_cast<LONG>((box.origin.x + box.size.width + 4.0F) * scale),
                 .bottom = static_cast<LONG>((box.origin.y + box.size.height) * scale)};
    ImmSetCandidateWindow(himc, &cf);
    ImmReleaseContext(hwnd_, himc);
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
