#pragma once

// Win32 IMM32 组合输入桥：**内部头**（与 `win32_ua.h` / `win32_cursor.h` 同列于 src/）。
//
// 为何走 IMM32 而非 TSF：TSF（`ITextStoreACPServices` + `ITfTextInputProcessor` + range 锁）
// 是 Win10/11 的正统接口，但要落 ~1500 行 COM 实现并接管整个文本存储；而 Win10/11 的
// CTF 装载器对**未注册 TSF 文本存储**的窗口提供 IME32 兼容通道——微软拼音 / 五笔 / 百度等
// 在 `WM_IME_*` + `ImmGetCompositionString` 下能完整给出 preedit、待转换段与上屏串
// （SDL2 / GLFW 生态多年实证）。故本切片取 IMM32 首桥：零 COM 依赖、零三方依赖、
// 与既有 `Win32Host` pimpl 同构。TSF 作为后续增量（需要接管 `ITextProvider` 时再上）。
//
// 门控与 `win32_ua.h` 同款：平台宏 ∧ 后端宏析取（Win32 GDI 与 D3D11 共用宿主）。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
#include <imm.h>  // ImmGetContext / ImmGetCompositionStringW / ImmSetCandidateWindow / ImmNotifyIME
// clang-format on

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief Win32 IMM32 组合输入桥：`WM_IME_*` → `TextCompositionEvent`，并把候选窗摆到插入点旁。
///
/// 生命周期与**窗口宿主**一致（非惰性）：桥自身零成本——没有输入法激活时一条 `WM_IME_*`
/// 都不会到达，也就零 `ImmGetContext` 调用，不同于 UIA 桥需要「无读屏即零构建」的门闩。
/// 组合态（preedit / 是否组合中）由本桥缓存，控件侧的显示态由 `TextCompositionEvent` 驱动。
/// @note Thread: main-thread only（消息泵线程）
class Win32ImeBridge {
  public:
    /// @brief 上层通道：事件投递与焦点控件查询，全部由 `Win32Host::Impl` 提供。
    struct Hooks {
        std::function<void(Event &)> emit;  ///< 投给宿主派发器（组合事件 / 上屏文本事件）
        std::function<Rect()> caret_bounds;  ///< 候选窗定位盒（**窗口逻辑 dp**）；空 = 无定位
        std::function<float()> scale_factor;  ///< 当前 DPI 缩放（dp → 物理像素）
    };

    explicit Win32ImeBridge(HWND hwnd, Hooks hooks) : hwnd_(hwnd), hooks_(std::move(hooks)) {}

    /// @brief 处理本桥认领的消息；返回 `nullopt` = 不认领（交 `DefWindowProc`）。
    ///
    /// 认领集合：`WM_IME_STARTCOMPOSITION` / `WM_IME_COMPOSITION` / `WM_IME_ENDCOMPOSITION` /
    /// `WM_IME_CHAR` / `WM_KILLFOCUS`。其中 `WM_IME_CHAR` **必须**吞掉：`DefWindowProc` 会把它
    /// 转成逐字 `WM_CHAR`，与 `GCS_RESULTSTR` 通道叠加即**同一汉字上屏两次**（且在 DBCS ACP 下
    /// 按前导/尾随字节拆开发送，产出乱码）。`WPARAM` 在认领集内均无含义，故不入参。
    [[nodiscard]] auto handle(UINT msg, LPARAM lp) -> std::optional<LRESULT>;

    /// @brief 复位组合态（窗口销毁路径调用；仅清本地缓存，不再触碰已失效的 HWND）。
    auto reset() -> void {
        composing_ = false;
        comp_.clear();
    }

    /// @brief 是否处于组合中（真机探针与自检观测器用）。
    [[nodiscard]] auto is_composing() const -> bool { return composing_; }

  private:
    /// @brief 取本窗口 IME 上下文（无输入法上下文时为 nullptr；调用方负责 `ImmReleaseContext`）。
    [[nodiscard]] auto acquire_context() -> HIMC;

    /// @brief 向 IME 取消未上屏的组合（`NI_COMPOSITIONSTR` + `CPS_CANCEL`）并清本地缓存。
    auto cancel_platform_composition() -> void;

    /// @brief 把候选窗/组合窗定位到焦点控件插入点（`CFS_CANDIDATEPOS` + `CFS_EXCLUDE`）。
    auto position_candidate_window() -> void;

    /// @brief 投出一条组合事件：缓存的 UTF-16 组合串 + WCHAR 光标位 + 属性数组 → 契约口径
    ///        （UTF-8 preedit + 码点下标，折算见 `ime_composition.h`）。
    auto emit_state(std::size_t caret_utf16, const std::vector<std::uint8_t> &attrs, const std::string &committed)
        -> void;

    HWND hwnd_ = nullptr;
    Hooks hooks_;
    bool composing_ = false;  ///< 已收到 STARTCOMPOSITION 且尚未 END
    std::u16string comp_;  ///< 最近一次 `GCS_COMPSTR`（未随状态更新消息重发时复用）
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11)
