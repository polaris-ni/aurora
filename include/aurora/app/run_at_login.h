#pragma once

/// @brief 登录项：查询 / 设置「登录时自动启动本应用」。状态是三态，不是布尔。
/// @file
///
/// 为什么必须是三态：「OS 侧此刻没让我自启」至少有四种成因——从未设置过、用户手动禁用、
/// 该平台无此机制、查询本身失败（注册表打不开 / 登录项服务状态不明）。布尔会把后三种并成同一个
/// `false`，应用就分不清「我调一下就能开」和「这台机器不可能」。故状态值是
/// `Enabled` / `Disabled` / `Unsupported` 三枚，`Unsupported` 另带 `detail` 点名原因。
///
/// 三态真正的价值在「用户手动禁用」那一条：条目仍然存在且带禁用标记，查询报 `Disabled` 而非
/// `Unsupported`；写入也不得把它当成「没设置过」而抹掉——登录项开关表达在**同一个实体**上，
/// 不用「删文件当禁用」（删掉会同时抹掉用户手工留下的那条记录）。
///
/// 平台落法：
/// - **Windows**：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的本应用值。**不用
///   `HKLM`**——后者需要提权，属动用户机器的越界动作（与 URL 方案 / 文件关联同一条边界）。
///   删除值即关闭，但「值不存在」与「用户用任务管理器在启动项里禁用」是两种形态：后者会在
///   `StartupApproved\Run` 下留一条禁用记录，查询据此报 `Disabled`，写入同向覆盖该记录（如实
///   兑现用户意图，而不是绕过它）；
/// - **Linux**：`$XDG_CONFIG_HOME/autostart` 下的桌面条目，启用 / 禁用表达在同一文件的
///   `Hidden=`（必要时同步 `X-GNOME-Autostart-enabled=`）键上；
/// - **macOS**：系统登录项服务（`SMAppService`，macOS 13+），要求 `.app` bundle；沙箱形态下
///   服务不适用，如实报 `Unsupported` 并点名原因。
///
/// 平台归属与失败口径：能力位见 `PlatformCapabilities::run_at_login`。位为 `false` 时
/// `set_run_at_login` 返回 `run-at-login-unsupported`；机制在但写不进去（注册表只读、服务拒绝、
/// macOS 不在 bundle 内）返回 `run-at-login-failed`，message 带明因。
///
/// @note Thread: main-thread only（注册表 / 桌面条目 / 登录项服务是阻塞 IO）
/// @note Side-effects: 查询读用户级注册表 / 自动启动目录；设置写它们
/// @note Rebuildable: yes（状态完全由 OS 侧登记决定）

#include <cstdint>
#include <string>

#include "aurora/app/single_instance.h"
#include "aurora/core/result.h"

namespace aurora {

/// @brief 登录项三态。
enum class RunAtLoginStatus : std::uint8_t {
    Enabled = 0,  ///< OS 会（或应当会）在用户登录时启动本应用。
    Disabled = 1,  ///< 条目存在但被关闭（含用户手动禁用），或本应用从未设置过。
    Unsupported = 2,  ///< 本机 / 本形态没有可用的登录项机制（未接线、macOS 沙箱、平台缺位等）。
};

/// @brief 登录项查询结果：三态 + 不可用时的原因。
///
/// `detail` 在 `Unsupported` 时必非空——它承载「为什么这台机器不可能」，正是布尔状态值丢掉的那
/// 一层信息；`Disabled` 时可按平台附一句成因（例如「在 Windows 启动项里被禁用」）；`Enabled`
/// 时为空串。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct RunAtLoginState {
    RunAtLoginStatus status = RunAtLoginStatus::Unsupported;  ///< 三态。
    std::string detail;  ///< 不可用或未生效的具体原因（英文）；`Enabled` 时为空串。
};

/// @brief 查询本应用的登录项状态。
///
/// 三态互不顶替：机制缺位 → `Unsupported`；条目启用 → `Enabled`；条目存在但禁用、或从未设置过
/// → `Disabled`。查询故障（注册表打不开等）报 `Unsupported` 并在 `detail` 里点名——它和「平台
/// 没这个机制」在应用动作上同解（都不能靠调用一次开关就成），但原因写在 `detail` 里，不静默。
/// @param identity 应用身份（Windows 按可执行路径定位 Run 值；macOS 认 bundle identifier）。
/// @return 三态查询结果（不抛错）。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 读用户级注册表 / 自动启动目录 / 登录项服务
[[nodiscard]] auto run_at_login(const AppIdentity &identity) -> RunAtLoginState;

/// @brief 开 / 关本应用的登录项。
///
/// 只改本应用自己的条目，机器上其它登录项一条都不触碰。开启时若用户此前手动禁用过，同向覆盖
/// 那条禁用记录（如实兑现这次显式请求）；关闭时保留条目实体并置禁用标记，不删「用户留下的记录」。
/// @param enabled true = 登录时启动，false = 不启动。
/// @param identity 应用身份（命令模板 / bundle identifier）。
/// @return 成功，或 `general-invalid-argument`（身份缺可执行路径 / bundle identifier）、
///         `run-at-login-unsupported`（能力位为 false）、`run-at-login-failed`（写入失败，含
///         macOS 不在 bundle 内）。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 写用户级注册表 / 自动启动桌面条目 / 登录项服务
[[nodiscard]] auto set_run_at_login(bool enabled, const AppIdentity &identity) -> Result<void>;

}  // namespace aurora
