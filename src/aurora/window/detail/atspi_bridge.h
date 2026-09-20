#pragma once

// AT-SPI2 平台桥（Linux）：**内部头**（与 `win32_ua.h` 同列于 src/，非公共 API）。
//
// 形态裁定（对齐设计 D9/D14/D15/D16 的 Win32 先例，按 AT-SPI2 现实修订处如实申报）：
//  * **每窗口一条 a11y 总线连接**（区别于 GTK 的进程级单连接）：本进程多窗口共用一个
//    会话总线连接会让两条窗口树撞同一个规范 Cache 路径 `/org/a11y/atspi/cache`；
//    分连接则路径天然唯一，代价是桌面树里一号多窗呈现为多个 application 节点（申报）。
//  * **dlopen("libdbus-1.so.3") 运行时绑定**：零构建期依赖（无 dev 包也可编译），
//    与 ALSA 后端同款；libdbus 缺失 / 无会话总线 / a11y 总线不可达 ⇒ 工厂返回 nullptr，
//    宿主按「无桥」继续运行。环境变量 `NO_AT_BRIDGE=1` 显式免提（GNOME 惯例）。
//  * **激活时机**：AT-SPI 没有 `WM_GETOBJECT` 式的「查询即激活」信号（连接建立本身就是
//    被查询的前提），故首个语义树根注入时尝试建连 + Embed；但**语义树仍是拉取式惰性**：
//    无客户端方法调用到达前零建树成本（与 D14 的初衷一致）。
//  * **事件信号（Object:/Cache: Add/Remove/children-changed 等）为后续增量**：注册表经
//    Cache.GetItems 全量拉取即可建树，Orca 浏览/读取可用；动态播报跟随该增量（如实申报）。

#include "aurora/core/platform.h"

// 门控与 `win32_ua.h` 同款：平台宏 ∧ 后端宏析取，且在守卫之前引入平台宏。
#if defined(AURORA_PLATFORM_LINUX) && (defined(AURORA_BACKEND_X11) || defined(AURORA_BACKEND_WAYLAND))

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/a11y_provider.h"
#include "aurora/window/detail/atspi_protocol.h"

namespace aurora::detail {

/// @brief Linux AT-SPI2 桥：libdbus 连接 + Embed 握手 + 方法面应答（折算全走 AtspiModel）。
///
/// 所有权：由宿主 Surface（`X11Surface` / `WaylandSurface`）持有；`set_accessibility_root`
/// 注入语义树根。帧循环集成：`poll_watches()` 的 fd 并入 `wait_events` 的 poll 集，
/// 可届时调 `pump()` 派发入站消息（UI 线程单线程模型，与 UIA 桥一致）。
/// @note Thread: main-thread only
class AtspiBridge final : public a11y::Provider {
  public:
    /// @brief 建连 + Embed 握手（尽力而为，含短超时）；返回 nullptr = 本窗口无桥可用。
    ///
    /// 失败降级面：`NO_AT_BRIDGE=1`、libdbus 缺失、会话总线不可达、org.a11y.Bus 无应答、
    /// a11y 总线打不开、注册表 Embed 失败/超时。失败一次即不再重试（诊断 warn 一次）。
    [[nodiscard]] static auto create(AtspiEnv env) -> std::unique_ptr<AtspiBridge>;

    ~AtspiBridge() override;

    AtspiBridge(const AtspiBridge &) = delete;
    auto operator=(const AtspiBridge &) -> AtspiBridge & = delete;
    AtspiBridge(AtspiBridge &&) = delete;
    auto operator=(AtspiBridge &&) -> AtspiBridge & = delete;

    // ---- a11y::Provider ----
    auto activate() -> void override;
    auto deactivate() -> void override;
    auto sync_if_dirty() -> void override;
    auto mark_dirty() -> void override;
    [[nodiscard]] auto is_active() const -> bool override;
    [[nodiscard]] auto name() const -> std::string override;
    auto set_root(Widget *root) -> void override;
    auto on_announcement(const std::string &text, const Widget *target) -> void override;
    auto on_widget_destroying(const Widget *w) -> void override;
    auto set_rtl(bool rtl) -> void override { rtl_ = rtl; }

    // ---- 帧循环 fd 集成（宿主 wait_events 调用）----
    /// @brief poll fd 描述（与 `<poll.h>` 的 pollfd 前两个字段同语义：events 用 POLLIN/POLLOUT 位值）。
    struct WatchFd {
        int fd = -1;
        short events = 0;
    };
    /// @brief 当前需要监听的 D-Bus 传输 fd 集（连接存活期间基本恒定）。
    [[nodiscard]] auto poll_watches() const -> std::vector<WatchFd>;
    /// @brief 非阻塞收取传输字节并派发全部已入队消息（方法调用 → 本桥应答）。
    auto pump() -> void;

    /// @brief 窗口客户区原点的屏幕物理 px（`Component.GetExtents(SCREEN)` 平移量；宿主更新）。
    auto set_window_origin(std::int32_t x, std::int32_t y) -> void;
    /// @brief 标题（FRAME 节点 Name；宿主在窗口标题变化时更新并置脏）。
    auto set_window_title(std::string title) -> void;

  private:
    struct Impl;
    explicit AtspiBridge(std::unique_ptr<Impl> d) : d_(std::move(d)) {}

    std::unique_ptr<Impl> d_;
    bool rtl_ = false;
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && (AURORA_BACKEND_X11 || AURORA_BACKEND_WAYLAND)
