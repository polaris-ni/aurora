#pragma once

/// @brief URL 方案注册：让 OS 知道「本应用能处理 `myapp:` 开头的链接」。
/// @file
///
/// 定位：`Navigator::open_uri` 只负责**应用内**把 `myapp://a/b` 路由到整栈路由，缺的是两端之一
/// ——OS 侧没有任何登记，用户点链接时系统根本不知道该唤起谁。本头补的正是「登记」这一端；
/// 另一端（第二个进程把 URI 交给已在运行的首个实例）见 `single_instance.h`，两者配套才谈得上
/// 「点了链接就进到应用里」。
///
/// 四条形态约束（实现与验收同源）：
/// - **方案文法先在折算层判、再碰 OS**：RFC 3986 `ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`，
///   空串 / 数字开头 / 下划线 / 冒号一律 `general-invalid-argument`，detail 点名是哪一条不合法；
/// - **保留方案拒收**：`http` / `https` / `file` / `mailto` / `ftp` / `tel` / `javascript` 等属于
///   既有生态，应用不得抢占（拒收同样走 `general-invalid-argument`，detail 明说「保留方案」——
///   不是解析崩溃，也不是平台不支持）；
/// - **只落用户级位置**：Windows 写 `HKCU\Software\Classes`，Linux 写用户私有的
///   `$XDG_DATA_HOME/applications`，macOS 靠 bundle 内的 `CFBundleURLTypes`——不碰需要提权的
///   机器级位置（与登录项不用 `HKLM` 同源）；
/// - **撤销先验归属**：归属已是空 → 幂等成功；归属是别的应用 → **一个字节都不改**并返回带原因的
///   `Result`。撤销不得越过别人的登记。
///
/// 平台归属与失败口径：能力位见 `PlatformCapabilities::url_scheme_registration`（机制存在性）。
/// 位为 `false` 的构建（未接线的后端）返回 `url-scheme-unsupported`；机制在但这一回没装进去
/// （注册表写不进、桌面条目落盘失败、macOS 不在 `.app` 包内因而无法登记）返回
/// `url-scheme-failed`，message 带明因。两类不混淆：前者重试无用，后者可能换个位置就成了。
///
/// @note Thread: main-thread only（注册表 / 桌面条目写入是阻塞 IO）
/// @note Side-effects: 写用户级注册表键 / 桌面条目 / Launch Services 数据库；`unregister` 删除它们
/// @note Rebuildable: yes（登记状态完全由 OS 侧数据库决定）

#include <string>
#include <string_view>

#include "aurora/app/single_instance.h"
#include "aurora/core/result.h"

namespace aurora {

/// @brief 登记「本应用处理该 URL 方案」。
///
/// 成功的确切含义是「已写入本机数据库」——Windows 的 `HKCU\Software\Classes\<scheme>`、Linux 的
/// 桌面条目（`MimeType=x-scheme-handler/<scheme>`）与 `mimeapps.list`、macOS 的 Launch Services
/// 刷新。各平台落法不同，但都只影响当前用户。
///
/// 同一方案重复登记是**替换**（幂等）：后一次的命令模板与显示名覆盖前一次，不叠加条目。
/// @param scheme 方案名（大小写不敏感；文法与保留名单见文件头）。
/// @param display_name 显示名（「打开方式」列表里的名字；空串回落为可执行文件名）。
/// @param identity 应用身份（可执行路径进 OS 命令模板；macOS 另认 bundle identifier）。
/// @return 成功，或 `general-invalid-argument`（文法不合法 / 保留方案 / 身份缺可执行路径）、
///         `url-scheme-unsupported`（能力位为 false）、`url-scheme-failed`（机制在但写入失败）。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 写用户级注册表键 / 桌面条目 / Launch Services 登记
[[nodiscard]] auto register_url_scheme(std::string_view scheme, std::string_view display_name,
                                       const AppIdentity &identity) -> Result<void>;

/// @brief 撤销本应用对该 URL 方案的登记。
///
/// 归属口径：
/// - 归属为空 → 幂等成功（本来就没登记，等于已经撤下了）；
/// - 归属是本应用 → 删除条目并返回成功；
/// - 归属是别的应用 → **不改任何字节**，返回 `url-scheme-failed`，detail 点名当前归属。
///
/// macOS 的 Launch Services 没有「按方案撤销」的公开 API（`CFBundleURLTypes` 是包内静态声明），
/// 本平台返回 `url-scheme-unsupported` 并在 detail 里说明；这是如实申报的平台不对称。
/// @param scheme 方案名（大小写不敏感）。
/// @param identity 应用身份（用于判定「归属是不是自己」）。
/// @return 成功（含本来就没有登记），或 `general-invalid-argument` / `url-scheme-unsupported` /
///         `url-scheme-failed`（归属是别人、或删除失败）。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 删用户级注册表键 / 桌面条目
[[nodiscard]] auto unregister_url_scheme(std::string_view scheme, const AppIdentity &identity) -> Result<void>;

/// @brief 该 URL 方案当前是否已登记归属（任一应用）。
///
/// 便捷判定：等价于「`registered_url_scheme_owner` 查询成功且非空」。查询本身失败时也返回
/// `false`——「查不出来」按「此刻不能声称已登记」处理；需要区分故障与真正未登记时用
/// `registered_url_scheme_owner`。
/// @param scheme 方案名（大小写不敏感）。
/// @return 有归属为 true。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 读用户级注册表 / 桌面条目数据库
[[nodiscard]] auto is_url_scheme_registered(std::string_view scheme) -> bool;

/// @brief 查询该 URL 方案当前的归属应用。
/// @param scheme 方案名（大小写不敏感）。
/// @return 成功携归属标识（Windows 为登记的命令模板 / ProgID，Linux 为桌面条目 ID，macOS 为
///         bundle identifier）；未登记时为空串（空集合不是错误）。文法不合法为
///         `general-invalid-argument`；能力位为 false 为 `url-scheme-unsupported`；查询失败为
///         `url-scheme-failed`。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 读用户级注册表 / 桌面条目数据库 / Launch Services
[[nodiscard]] auto registered_url_scheme_owner(std::string_view scheme) -> Result<std::string>;

}  // namespace aurora
