#pragma once

// Win32 剪贴板桥：**内部头**（与 `win32_drag.h` / `win32_recent_documents.h` 同列于 src/）。
//
// 承载的三件平台语义都以**剪贴板 owner 窗口**为投递目标，这是本桥存在的全部理由：
// - 惰性提供：`SetClipboardData(cf, NULL)` 声明延迟渲染后，请求方到达时 `WM_RENDERFORMAT`
//   只发给 owner 窗口；`OpenClipboard(nullptr)` 不建立 owner，该消息没有投递目标；
// - 所有权丢失：`WM_DESTROYCLIPBOARD` 同样只发 owner；
// - 变更监听：`AddClipboardFormatListener(hwnd)` 强制要窗口。
// 故 **无 `Win32Host` 存活即无剪贴板 owner 窗口**，三件在此形态下于平台层就不成立；桥未登记时
// 门面沿用 `clipboard-format-unsupported` / `clipboard-listen-unsupported`，不谎报。
//
// 富格式（文件清单 / HTML / 自定义 MIME）则不同：它们是一次性的 `SetClipboardData`，不依赖 owner，
// 但**每次写入都以 `EmptyClipboard` 开头**——既有 `set_text` 就是这么写的，故平台路径的语义是
// 「后写者接管整块剪贴板内容」，与 memory 后端「多格式并存」刻意不同（那是内存态的自由，不是
// 系统剪贴板的语义）。这点在规格侧并列写明，免得调用方按 memory 后端的直觉写码。
//
// 门控与 `win32_drag.h` 同款（平台宏 ∧ 后端宏析取）：Win32 / D3D11 / WgpuWin32 三类 Surface 共用
// 同一宿主，剪贴板能力三者同翻。

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

namespace aurora::detail {

/// @brief Win32 剪贴板桥的**宿主守卫**：构造即登记 owner 窗口与进程级后端，析构即注销。
///
/// 与 `Win32RecentDocumentsBridge` 同款的引用计数纪律：多窗口场景下每个宿主各持一份守卫
/// （进程级桥状态只有一份），先关的窗口不会把仍在世的窗口的剪贴板能力一起拆掉。
/// owner 取**最近登记的存活窗口**（`attach()` 覆盖式登记）；该窗口销毁即 `detach()`，
/// 此后本桥失去 owner，`WM_RENDERFORMAT` 无从投递，故一并触发一次所有权丢失回调——如实反映
/// 「本进程已不再持有剪贴板」，而不是让调用方继续以为要约还活着。
/// @note Thread: main-thread only（剪贴板是单线程 UI 契约下的进程态）
class Win32ClipboardBridge {
  public:
    Win32ClipboardBridge();
    ~Win32ClipboardBridge();

    Win32ClipboardBridge(const Win32ClipboardBridge &) = delete;
    auto operator=(const Win32ClipboardBridge &) -> Win32ClipboardBridge & = delete;
    Win32ClipboardBridge(Win32ClipboardBridge &&) = delete;
    auto operator=(Win32ClipboardBridge &&) -> Win32ClipboardBridge & = delete;

    /// @brief 登记 owner 窗口（`HWND` 的 `void*` 形态），覆盖式：最近登记者生效。
    /// @param owner_hwnd 宿主窗口句柄；`nullptr` 为空登记（桥仍存在但无 owner）。
    auto attach(void *owner_hwnd) -> void;

    /// @brief 提前释放本守卫的引用（窗口销毁路径显式调用；析构会再调一次，幂等）。
    ///
    /// 声明 `noexcept`：退位动作里会调用应用的「所有权丢失」回调，而本函数同时是**析构与窗口
    /// 销毁路径**的一环——异常穿出析构不可接受，故在内部捕获并只留日志。
    auto detach() noexcept -> void;

  private:
    bool installed_ = false;  ///< 本实例是否持有一份进程级引用
    void *hwnd_ = nullptr;    ///< 本实例登记的 owner 窗口（detach 时据此判定是否仍由它持有 owner）
};

/// @brief `WM_RENDERFORMAT`：按格式号找到要约声明的 MIME，调 `provide` 取字节并提交给剪贴板。
/// @param hwnd 收到消息的窗口（用于诊断与校验它确是当前 owner）。
/// @param format 被请求的剪贴板格式号。
/// @return 是否已兑现（false = 该格式不由本进程要约声明，交 `DefWindowProc`）。
[[nodiscard]] auto win32_clipboard_render_format(void *hwnd, unsigned format) -> bool;

/// @brief `WM_DESTROYCLIPBOARD`：所有权被夺，清要约并触发一次所有权丢失回调。
auto win32_clipboard_destroy_clipboard() -> void;

/// @brief `WM_CLIPBOARDUPDATE`：内容变化，触发一次变更回调。
auto win32_clipboard_clipboard_update() -> void;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11 || GPU_WGPU)
