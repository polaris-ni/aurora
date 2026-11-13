#pragma once

#include <string>
#include <vector>

#include "aurora/app/menu.h"  // MenuModel（安装的就是纯数据模型）
#include "aurora/core/result.h"

namespace aurora {

class CommandRegistry;  // 前向声明：本头不引 commands.h，避免把命令系统拽进所有消费方

/// @brief 原生菜单栏进程级门面：把一份 `MenuModel` 树渲染到 OS 原生菜单栏（Win32 `HMENU` 等）。
///
/// 定位分工（与 `RecentDocuments` 同款进程级单例范式）：
/// - 本门面是**唯一入口**——应用只认它，不感知底层是 Win32 `HMENU` 还是（未来）macOS `NSMenu`；
/// - 数据只吃 `MenuModel`（纯数据）：行为一律经 `MenuModel::id` 反查 `CommandRegistry::invoke`，
///   与自绘路径共用同一个出口（提案「不建平行菜单 API」硬约束）；
/// - **同一进程只认一份菜单栏**：重复 `install` 替换旧树、不叠加、不泄漏。
///
/// 能力归属：是否真有 OS 原生菜单栏机制看 `PlatformCapabilities::native_menu_bar`（当前 Win32 /
/// D3D11 由 `HMENU` 桥接线、macOS 由应用级 `NSMenu` 桥接线；Linux / Wasm / Headless 为 `false`，
/// 回落自绘 `MenuBar`）。位为 `false` 的平台 `install` 返回 `native-menu-unsupported`，应用内菜单
/// 照常工作，不因此降级。
///
/// 点击路由：本门面不直接调命令——它持有一个由 `set_command_registry` 登记的 `CommandRegistry*`
/// （`Application` 构造期自动登记），菜单项被点击时按 `id` 反查并 `invoke`。空 `id` 的项照常建出、
/// 照常显示，但点击不路由并留一条 WARN（不静默丢弃、不伪造 id，见 `06-app-platform.md` §8.4）。
///
/// @note Thread: main-thread only
/// @note Side-effects: `install`/`uninstall` 触碰 OS 原生菜单状态；`set_command_registry` 改进程级指针
/// @note Rebuildable: no（进程级菜单状态）
class NativeMenuBar {
  public:
    /// @brief 进程级单例（懒构造；仿 `RecentDocuments::instance`）。
    /// @return 单例引用（首次调用时构造）。
    [[nodiscard]] static auto instance() -> NativeMenuBar &;

    // 禁复制/移动：进程级唯一门面，语义上不该被搬走。
    NativeMenuBar(const NativeMenuBar &) = delete;
    auto operator=(const NativeMenuBar &) -> NativeMenuBar & = delete;
    NativeMenuBar(NativeMenuBar &&) = delete;
    auto operator=(NativeMenuBar &&) -> NativeMenuBar & = delete;
    ~NativeMenuBar() = default;  // 不持资源（registry_ 非拥有）；默认析构即满足五法则对称

    /// @brief 登记命令注册表（点击路由的出口）。
    ///
    /// 由 `Application` 在构造期自动调用（`&app.commands()`），应用无需手动登记。空指针是合法值
    /// （无头 / 未接线场景）：此时菜单项点击不路由（留 WARN），菜单仍照常构建显示。
    /// @param registry 命令注册表实例；生命周期须覆盖本门面使用期（通常即应用生命周期）。
    auto set_command_registry(CommandRegistry *registry) -> void;

    /// @brief 安装 / 替换菜单栏。
    ///
    /// 契约：
    /// - **进程级单份**：重复调用替换旧树（先卸载旧菜单再装新树），不叠加；
    /// - **只吃纯数据**：`models` 即菜单栏从左到右的顶层菜单；其子项递归成下拉；
    /// - 空 `models` 等同于清空菜单栏（成功，不报错）；
    /// - 平台无原生菜单栏机制（`PlatformCapabilities::native_menu_bar == false` 且本进程无可用桥）
    ///   时返回 `native-menu-unsupported`；机制在但装不上时返回 `native-menu-install-failed`。
    /// @param models 顶层菜单模型列表（顺序即菜单栏自左向右）。
    /// @return 成功（含空表清空）或结构化错误。
    [[nodiscard]] auto install(const std::vector<MenuModel> &models) -> Result<void>;

    /// @brief 卸载菜单栏（销毁原生菜单对象、断开窗口关联）。幂等：未安装时为 no-op。
    auto uninstall() -> void;

    /// @brief 当前是否已安装菜单栏（最近一次 `install` 成功且尚未 `uninstall`）。
    /// @return 已安装为 true。
    [[nodiscard]] auto is_installed() const -> bool;

  private:
    NativeMenuBar() = default;

    /// @brief 点击路由：按 `id` 反查命令注册表并 `invoke`（空 id / 未登记注册表时只留 WARN）。
    auto route_click(const std::string &id) const -> void;

    CommandRegistry *registry_ = nullptr;  ///< 点击路由出口（非拥有；由 Application 登记）
    bool installed_ = false;  ///< 最近一次 install/uninstall 的净结果（进程级单份语义）
};

}  // namespace aurora
