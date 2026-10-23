#pragma once

// Win32 OLE 拖放桥：**内部头**（与 `win32_ime.h` / `win32_ua.h` 同列于 src/）。
//
// 为何走 OLE（`RegisterDragDrop` + `IDropTarget`）而非旧的文件拖放消息通路：后者是「落放后一次性给
// 路径清单」的窄通路——没有进入 / 悬停 / 离开三态、没有格式清单、没有动作协商、没有拖出方向；而
// Aurora 的 `SystemDropEvent` 契约是四态 + MIME 清单 + `DragAction` 协商。OLE 是它的严格超集，
// 故本桥落地后该窄通路移除（git 历史即归档，不留双真源）。
//
// 为何不保留该窄通路作为 OLE 失败时的 fallback：① 触发条件（`OleInitialize` 失败）极罕见，
// 为它长期维护第二套坐标换算与格式解析不成比例；② 窄路径无动作协商，Move 回滚在其上是死的；
// ③ 与「PNG 编解码关闭时该格式如实不可得、不假装支持」同条纪律——拖入亦不备语义更弱的备胎。
// OLE 初始化失败时如实报「会话建立失败」，不谎报有通路。
//
// 首批范围：**拖入四态 + 同步载荷 + 拖出（`IDropSource` + `DoDragDrop`）**。大载荷异步通道
// （`read_async` + `pump_reads`）属后续批次；折算层已为其备好阈值常量
// （`system_drag_translate.h`），届时按同一接缝扩展，不改本桥的对外形态。
//
// 拖出的线程形态：规格明文「不引入嵌套消息泵」（`codespec/specification/06-app-platform.md` §8.9），
// 而 Win32 `DoDragDrop` 一旦调用就在**调用线程**上跑 OS 固有的嵌套模态循环。故拖出会话搬到
// **库自管的拖拽线程**上跑：`system_drag::begin`（UI 线程）发起即返回，UI 线程帧循环不被打断，
// OS 嵌套循环随之离开 UI 线程。拖拽线程与事件层之间只经
// `event/detail/system_drag_backend.h` 的两条通道交互（终态登记 + 跨线程要约索取），
// 平台桥不自行决定投递时机——帧循环排空通则因此对四个平台一致。
//
// 门控与 `win32_ime.h` 同款（平台宏 ∧ 后端宏析取），并补 `AURORA_BACKEND_GPU_WGPU`：Win32 / D3D11 /
// WgpuWin32 三类 Surface 共用同一 `Win32Host`，能力位三者同翻（见 `window/platform.h`）。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
#include <ole2.h>    // OleInitialize / OleUninitialize / FORMATETC / STGMEDIUM
#include <oleidl.h>  // IDropTarget / IDataObject / RegisterDragDrop
// clang-format on

#include <functional>
#include <memory>

#include "aurora/core/types.h"
#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief Win32 OLE 拖入桥：把 OS 拖放会话的四个回调翻译成 `SystemDropEvent` 四态并回写光标反馈。
///
/// 生命周期由 **窗口宿主** 持有（`Win32Host::Impl` 唯一持有，Win32 / D3D11 / WgpuWin32 三个 Surface
/// 转发同一实例——仿 §8.5 无障碍桥的持有方式，避免多份 HWND → 窗口映射分裂）。注册后 OS 直接回调
/// 本桥，**不经过 `wnd_proc`**（这正是旧窄通路要移除的原因：同一 HWND 上两套拖入语义
/// 会让应用侧无从知道自己走哪条路）。
///
/// 会话态（当前 `IDataObject` 引用）随 `DragLeave` / `Drop` 释放；窗口销毁路径经 `reset()` 清态，
/// 不再触碰已失效的 HWND。
/// @note Thread: main-thread only（OLE 拖放回调在注册线程上到达）
class Win32DragBridge {
  public:
    /// @brief 上层通道：事件投递与缩放查询，全部由 `Win32Host::Impl` 提供。
    struct Hooks {
        std::function<void(Event &)> emit;  ///< 投给宿主派发器（`allowed` 由派发器在调用内写定）
        std::function<float()> scale_factor;  ///< 当前 DPI 缩放（物理像素 → 逻辑 dp 的除数）
    };

    Win32DragBridge(HWND hwnd, Hooks hooks);
    ~Win32DragBridge();

    Win32DragBridge(const Win32DragBridge &) = delete;
    auto operator=(const Win32DragBridge &) -> Win32DragBridge & = delete;
    Win32DragBridge(Win32DragBridge &&) = delete;
    auto operator=(Win32DragBridge &&) -> Win32DragBridge & = delete;

    /// @brief 注册为拖放目标（`OleInitialize` + `RegisterDragDrop`）。
    ///
    /// 两步都可能失败：OLE 初始化失败即该线程没有 COM 单元、`RegisterDragDrop` 失败常见于该 HWND 已
    /// 被注册或窗口已销毁。失败时如实返回 false，由宿主记日志并按「会话建立失败」语义处理——
    /// **不谎报已有通路**。
    /// @return 注册是否成功；已注册时重复调用返回 false（不重复注册）。
    [[nodiscard]] auto install() -> bool;

    /// @brief 注销拖放目标（`RevokeDragDrop` + `OleUninitialize`）并清会话态。
    auto uninstall() -> void;

    /// @brief 清会话态（释放持有的 `IDataObject`）；未注册时为 no-op。
    auto reset() -> void;

    /// @brief 是否已注册（真机探针与自检观测器用）。
    [[nodiscard]] auto installed() const -> bool { return installed_; }

  private:
    // COM 对象的析构语义由 `IUnknown::Release` 与桥的所有权共同决定，虚析构既多余又会在
    // `final` 上引入无谓的 vtable；此处按诊断落点（前置声明所在行）逐行豁免。
    // NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor)
    class DropTargetImpl;  ///< COM `IDropTarget` 实现体（定义在 `win32_drag.cpp`，不进本头）

    /// @brief 把一次 OS 回调折算为事件并派发；返回派发器写定的 `allowed`（供光标回写）。
    /// @param phase 阶段。
    /// @param data 数据对象（`Leave` 时为 nullptr）。
    /// @param key_state `grfKeyState`（`MK_*` 位集）。
    /// @param pt 指针位置（**屏幕坐标**，本方法内换算为客户区像素）。
    /// @param os_suggested OS 报告的源侧建议动作（由 `*pdwEffect` 折算而来）。
    /// @return 派发后写定的允许动作。
    auto dispatch_phase(SystemDropPhase phase, IDataObject *data, DWORD key_state, POINTL pt, DragAction os_suggested)
        -> DragAction;

    HWND hwnd_ = nullptr;
    Hooks hooks_;
    std::unique_ptr<DropTargetImpl> target_;  ///< COM 目标对象（桥持所有权，见实现内引用计数注记）
    IDataObject *data_ = nullptr;  ///< 会话期内持有的数据对象（`DragEnter` 起、`Drop` / `Leave` 止）
    bool installed_ = false;  ///< `RegisterDragDrop` 是否已成功
    bool ole_owned_ = false;  ///< 本桥是否持有一次 `OleInitialize`（配对的 `OleUninitialize` 责任）
};

/// @brief 注册 Win32 拖出后端（进程级单槽；由 `Win32Host::Impl` 构造期调用，重复调用忽略）。
///
/// 拖出机制（OS 会话 + 库自管拖拽线程）与具体窗口句柄无关，故注册面进程级常驻、不随窗口销毁
/// 注销（与事件层注册面同口径，见 `event/detail/system_drag_backend.h`）。多窗口场景下
/// 后到窗口的注册由事件层的「先到者优先」自然作废。
auto register_win32_drag_out_backend() -> void;

/// @brief 登记进程级 DPI 缩放提示（宿主每次 `refresh_scale()` 后调用；任意窗口调用均生效）。
///
/// 存在的理由：拖拽影像热点在逻辑 dp 上声明、像素由源应用合成，折算需要「发起窗口的 DPI」；
/// 而 `SystemDragOutBackend::begin` 只携带 `DragOffer`（无窗口上下文、无缩放），桥无从向
/// 事件层反查发起窗口。若改为捕获首个窗口的缩放查询回调，首窗销毁后该回调即读已释放成员
/// （悬空）。故退一步取**进程级提示槽**：宿主在自己的 DPI 刷新点（唯一写 `scale` 处）顺带
/// 推送，引擎只读该槽、不持有任何窗口指针——多显示器异 DPI 场景取最后一次推送值，
/// 是装饰性热点的次优解，不致语义受损（`DragOffer::icon` 契约）。
/// @param scale 当前 DPI 缩放（dp → 物理像素；≤ 0 或非有限值按 1.0 处理）。
auto win32_drag_out_note_scale(float scale) -> void;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11 || GPU_WGPU)
