#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "aurora/app/shortcuts.h"
#include "aurora/core/result.h"

namespace aurora {

/// @brief OS 级全局热键的注册句柄（specification/06-app-platform.md §9.1）。
///
/// 值语义句柄：只持一个 ID，不持有资源——热键的生命周期由 `OsHotkeyRegistry` 掌管，
/// 句柄失效（对应热键被 `remove` / 注册表销毁）后仅表现为「ID 查不到」，不会悬垂。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct OsHotkeyHandle {
    std::uint32_t id = 0;  ///< 热键 ID；0 恒为无效句柄，有效 ID 自 1 起且进程内唯一

    /// @brief 默认构造：无效句柄（id == 0）。
    OsHotkeyHandle() = default;
    /// @brief 由 ID 构造（供注册表内部发放；调用方不应自行造号）。
    /// @param id_ 热键 ID。
    explicit OsHotkeyHandle(std::uint32_t id_) : id(id_) {}

    /// @brief 相等比较：ID 相同即同一热键。
    /// @param other 另一个句柄。
    /// @return ID 相等时为 `true`。
    [[nodiscard]] auto operator==(const OsHotkeyHandle &other) const noexcept -> bool { return id == other.id; }
    /// @brief 不等比较（`operator==` 的取反）。
    /// @param other 另一个句柄。
    /// @return ID 不等时为 `true`。
    [[nodiscard]] auto operator!=(const OsHotkeyHandle &other) const noexcept -> bool { return id != other.id; }
};

/// @brief OS 级全局热键注册表：把键组合注册到操作系统，**应用无焦点时也触发**。
///
/// 与 `ShortcutRegistry` 的分工：后者是应用内快捷键（只在窗口有焦点、经事件派发命中），
/// 本类走 OS 接口（Windows `RegisterHotKey` / `WM_HOTKEY`），焦点在别的进程里同样生效。
///
/// **失败口径**（一律机器可见，不做静默 no-op）：
///
/// | 情形 | 结果 |
/// |:---|:---|
/// | 本平台没有全局热键后端（Wayland / GLFW / macOS / WASM 等） | `OsHotkeyRegisterFailed` |
/// | 主键无法映射到原生虚拟键，或组合未指定主键 | `OsHotkeyRegisterFailed` |
/// | 同一组合在本注册表内重复注册 | `OsHotkeyRegisterFailed` |
/// | OS 侧注册失败（组合已被别的进程抢占 / 无桌面会话） | `OsHotkeyRegisterFailed` |
///
/// **触发时机**：命中不在消息泵内同步执行回调。`WM_HOTKEY` 到达时只把命中的 ID 推进内部
/// 队列，由 `drain_pending()` 在帧循环里（主线程、栈上没有窗口过程帧）排空并调用动作。
/// 回调可能重建页面 / 触发重排，在消息泵内重入会把布局与绘制切到半途的状态。
///
/// @note Thread: main-thread only（注册、注销、排空必须在建窗与跑消息泵的同一线程）
/// @note Side-effects: registers/unregisters OS-level hotkeys
/// @note Rebuildable: no
class OsHotkeyRegistry {
  public:
    /// @brief 构造：接上本库共享的隐藏消息窗口（平台不支持时为空注册表，`enabled()` 为 false）。
    OsHotkeyRegistry();
    /// @brief 禁止拷贝：注册表持有 OS 资源与钩子 ID，拷贝会导致双重注销。
    OsHotkeyRegistry(const OsHotkeyRegistry &) = delete;
    /// @brief 禁止拷贝赋值：同拷贝构造的理由。
    /// @return 恒不返回（已 delete）。
    auto operator=(const OsHotkeyRegistry &) -> OsHotkeyRegistry & = delete;
    /// @brief 析构：注销本注册表注册的全部热键，并摘掉消息钩子。
    ~OsHotkeyRegistry();

    /// @brief 注册一条 OS 级全局热键。
    /// @param combo 触发键组合（修饰键位掩码 + 主键）；主键不得为 `KeyCode::Unknown`。
    /// @param action 命中时执行的动作（可空；空动作仍占住该组合，排空时跳过）。
    /// @return 成功为 `Ok(句柄)`（ID 自 1 起）；失败为结构化 `Error`，恒为
    /// `ErrorCode::OsHotkeyRegisterFailed`，`detail` 给出具体成因。
    auto add(const KeyCombo &combo, std::function<void()> action) -> Result<OsHotkeyHandle>;

    /// @brief 注销一条热键并释放其 OS 注册。
    /// @param h `add()` 返回的句柄；无效句柄或不属于本注册表的 ID 返回 false（不抛、不报错）。
    /// @return 确有热键被注销时为 `true`。
    auto remove(OsHotkeyHandle h) -> bool;

    /// @brief 注销本注册表内的全部热键（析构前主动释放，或整批撤掉重建时用）。
    auto clear() -> void;

    /// @brief 排空待处理队列：把自上次排空以来命中的热键逐个交给其动作。
    /// 由帧循环每帧调用一次；动作在本调用内同步执行，其间再次命中会留到下一次排空。
    /// @return 本次实际执行了动作的热键条数（命中但动作为空的不计）。
    auto drain_pending() -> std::size_t;

    /// @brief 本平台的 OS 级全局热键后端是否可用。
    /// @return 平台提供全局热键能力时为 `true`（如 Windows）；Wayland / GLFW / macOS 等
    /// 无后端时为 `false`，此时 `add()` 恒返回错误、`remove()` 恒返回 false。
    [[nodiscard]] auto enabled() const -> bool;

    /// @brief 当前已注册的热键条数（不含已排空但尚未注销的命中）。
    /// @return 注册表中的热键条数。
    [[nodiscard]] auto count() const -> std::size_t;

    // ---- 测试注入点（test-only，口径同 `Clipboard` 的注入面）----
    // 声明常驻（消费端调用始终可编译），实现体按 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS`
    // 双宏裁切，任一关闭（含 Release 下 DEBUG 自动关闭）时返回 false / no-op，平台行为不变。
    // 为何需要：单测不得真的抢占系统热键（CI 机器上会干扰系统与别的用例），
    // 但又必须覆盖「平台不支持」的降级路径——那在受支持的平台上没有别的办法复现。

    /// @brief 安装进程内 inert 后端：此后 add/remove 全部走内存，不触碰 OS 热键接口。
    /// @param simulated_platform_supported 模拟的平台支持位（即 `enabled()` 的返回值）。
    /// @return 注入是否生效（双宏未齐备时为 `false`）。
    [[nodiscard]] static auto install_test_backend(bool simulated_platform_supported) -> bool;

    /// @brief 卸载 inert 后端，恢复平台实现（未安装时为 no-op）。
    /// @return 是否确有后端被卸载（双宏未齐备时为 `false`）。
    [[nodiscard]] static auto remove_test_backend() -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  ///< 注册表的实现体（已注册项、待排空队列、消息钩子 ID）
};

}  // namespace aurora
