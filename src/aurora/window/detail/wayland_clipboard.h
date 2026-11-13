#pragma once

// Wayland CLIPBOARD 剪贴板桥：**内部头**（与 `wayland_drag.h` / `x11_drag.h` 同列于 src/）。
//
// 为何单列：Wayland 的剪贴板是**对象协议**——读写都经 `wl_data_device` 的 `selection` / `data_offer`
// 与 `wl_data_source` 的 `send`，把这套时序写进 `WaylandSurface` 的事件泵，会把「协议时序」混进
// 「窗口事件映射」，两边都不可读、也不可单测。故协议态与时序全在本桥内，表层只做「转发条目」。
//
// 为何本头**不引 `<wayland-client.h>`**（与 `wayland_drag.h` 同款纪律）：wayland 头虽不像 Xlib 那样
// 无条件 `#define None`，但把平台协议头关进单个 TU 仍是更小的污染面与更小的依赖闭包。协议对象一律
// 以 `void *` + 版本号传入，在 `.cpp` 内还原类型。
//
// 与 `WaylandDragBridge` 的关键差异（本桥独有）：`wl_data_device` **每 seat 唯一**——拖放与剪贴板
// 共用同一枚。本桥**不复刻** `wl_data_device`（那会触发协议错误），而是复用拖放桥创建的那一枚：
// 拖放桥在 `selection` 事件里经 `set_clipboard_forwarder` 把 selection 的 `wl_data_offer *` 与已收集
// 的名称交给本桥。本桥只额外创建 `wl_data_source`（写侧 / 声明所有权）并用那枚 device 的
// `set_selection`。读侧（`wl_data_offer.receive`）与变更 / 所有权通知（selection 事件）全经转发器。
//
// 剪贴板读取语义（与 CHG-023 为剪贴板定的「不引入异步读取通道」口径一致）：`get_payload` 在原地
// 同步读完（管道读 + 墙钟超时），不挂帧循环排空点——读取时机由应用自己选，不在帧关键路径上。
// 写侧（`wl_data_source.send`）则非阻塞 + `tick()` 逐帧推进（对端节奏不可控，同步写会钉死 UI 线程）。
//
// 门控：Linux 且非 Android 且 Wayland 后端（与 `wayland_surface.cpp` 同款析取）。

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aurora::detail {

/// @brief Wayland CLIPBOARD 剪贴板桥：把 `wl_data_device` / `wl_data_source` 翻译成
/// `clipboard::detail::PlatformBackend` 的九枚入口。
///
/// 生命周期由**窗口宿主**持有（`WaylandSurface::Impl` 唯一持有）：`install()` 向拖放桥登记
/// selection 转发器、注册进程级剪贴板后端；`uninstall()` 撤后端、销毁在途 `wl_data_source`。
/// 所有权本身在 `set_payload` / `set_offer` 时才经 `wl_data_device_set_selection` 声明——
/// `install()` 不抢占别人的剪贴板。
/// @note Thread: main-thread only（Wayland 事件泵与控件回调同线程）
class WaylandClipboardBridge final {
  public:
    /// @brief 上层通道。
    struct Hooks {
        std::function<void()> on_change;  ///< 系统剪贴板内容变化回调（空则无机制）。
        /// @brief 本窗口最近一次输入事件的 serial（`wl_data_device.set_selection` 的协议必需实参）。
        ///
        /// 协议要求该 serial 来自本客户端某次真实输入（指针按键 / 键盘按键）；编造或复用陈旧值
        /// 会被合成器拒绝（选择权静默不生效）。为空即「本窗口没有输入通路」——此时如实报
        /// `clipboard-write-failed`，不声明所有权。
        std::function<std::uint32_t()> input_serial;
    };

    /// @brief 构造：不触碰 Wayland（全部协议调用集中在 `install()`，便于宿主按返回值降级）。
    /// @param display `wl_display *`（以 `void *` 传入：本头不引 wayland 头，见文件头）。
    /// @param manager `wl_data_device_manager *`（`void *` 形态；可为空 ⇒ install 失败）。
    /// @param manager_version 绑 manager 时采用的协议版本。
    /// @param device 共享的 `wl_data_device *`（`void *` 形态；由拖放桥创建并传入）。
    /// @param seat `wl_seat *`（`void *` 形态；声明所有权所需）。
    /// @param hooks 上层通道（含 `set_selection` 所需的输入 serial 来源）。
    WaylandClipboardBridge(void *display, void *manager, std::uint32_t manager_version, void *device, void *seat,
                           Hooks hooks);
    ~WaylandClipboardBridge();

    WaylandClipboardBridge(const WaylandClipboardBridge &) = delete;
    auto operator=(const WaylandClipboardBridge &) -> WaylandClipboardBridge & = delete;
    WaylandClipboardBridge(WaylandClipboardBridge &&) = delete;
    auto operator=(WaylandClipboardBridge &&) -> WaylandClipboardBridge & = delete;

    /// @brief 向拖放桥登记 selection 转发器、注册进程级剪贴板后端。
    ///
    /// 失败来源只有三类：manager / device / seat 缺失、连接已断开。此时如实返回 false，由宿主按
    /// 「该窗口不参与 Wayland 剪贴板」处理——**不谎报已有通路**。
    /// @return 是否成功；重复调用返回 false（不重复注册）。
    [[nodiscard]] auto install() -> bool;

    /// @brief 撤后端、销毁在途 `wl_data_source`、清全部会话态。
    auto uninstall() -> void;

    /// @brief 帧循环看守：写侧 `wl_data_source.send` 的管道写出逐帧推进。
    auto tick() -> void;

    /// @brief 本桥是否持有剪贴板所有权（供宿主判断清空语义）。
    [[nodiscard]] auto owns() const -> bool;

    /// @brief 协议状态机（display / data_source / 读侧 offer），定义在 `wayland_clipboard.cpp`。
    ///
    /// public 而非 private：`wl_data_source_listener` 是**自由函数指针表**，C 回调只能经
    /// `void *user data` 还原对象，故同 TU 内的 thunk 必须在类外以 `Impl *` 命名本类型
    /// （`WaylandDragBridge::Impl` 同款先例）。
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;  ///< 状态机（唯一所有权）

    /// @brief 把 `self` 登记为进程级后端表与 selection 转发器的当前捕获者（最近登记者接管）。
    static auto install_backend_for(Impl *self) -> void;
    /// @brief 存活清单登记：多窗口下后端表捕获最近存活桥，先关的窗口不拆仍在世窗口的通路。
    static auto register_backend(Impl *self) -> void;
    /// @brief 存活清单注销：从清单移除并改挂最近的存活桥（空则撤表并摘转发器）。
    ///
    /// 后端表与转发器都捕获桥的自指针，桥销毁前不撤即悬垂——本函数是那唯一的撤点。
    static auto unregister_backend(Impl *self) -> void;
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && !ANDROID && AURORA_BACKEND_WAYLAND
