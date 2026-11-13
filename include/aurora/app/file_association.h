#pragma once

/// @brief 文件关联：查询某扩展名当前归谁处理，以及声明「本应用能处理」。
/// @file
///
/// 立场：**查询 + 声明候选，不抢占默认**。查询侧尽力做全平台；声明侧把「本应用可处理」写进
/// 各自的本机数据库，但**不主动把默认处理程序改成自己**——默认程序属用户偏好且抢占不可逆，
/// 是越界动作（与 `tools/servers/aurora_mcp.cpp` 拒绝写自启动目录同源：写到 OS 会自动生效的
/// 位置是需要显式授权的事，不是库该代劳的事）。Windows 自 Win8 起也不允许以编程方式设置默认
/// 处理程序，本节口径与平台约束同向。
///
/// 词汇表不另起炉灶：扩展名 → MIME 的方向映射引用 `aurora/app/mime_types.h`（剪贴板与系统拖放
/// 共用的 MIME 唯一真源；该头只差「扩展名 → MIME」这一侧，已补 `mime_type_for_extension`）。
/// 声明侧仍允许调用方自带任意 MIME 字符串——本表的「未登记」不等于「不可用」。
///
/// 三平台的落法（细节见 `specification/06-app-platform.md`）：
/// - **Windows**：写 `HKCU\Software\Classes` 的 ProgID 与 `RegisteredApplications` +
///   Capabilities（只写 ProgID 会在「打开方式」里查不到本应用——常见假阴性）；查询走
///   `AssocQueryStringW`，只答「当前实际生效的归属」，不把系统默认与用户偏好混成一次查询；
/// - **Linux**：声明写桌面条目的 `MimeType=` 与 `mimeinfo.cache` / `mimeapps.list`；查询解析这两张
///   纯文本表（解析层无头可测）；
/// - **macOS**：由 `CFBundleDocumentTypes`（UTI）声明、Launch Services 查询，要求 bundle 身份。
///
/// 平台归属与失败口径：能力位见 `PlatformCapabilities::file_association`。位为 `false` 返回
/// `file-association-unsupported`；机制在但这一回没装进去（注册表写不进、数据库落盘失败、
/// macOS 不在 `.app` 包内）返回 `file-association-failed`，message 带明因。
///
/// @note Thread: main-thread only（注册表 / 数据库读写是阻塞 IO）
/// @note Side-effects: 查询读用户级数据库；注册写用户级注册表键 / 桌面条目 / `mimeapps.list`
/// @note Rebuildable: yes（归属完全由 OS 侧数据库决定）

#include <string>
#include <string_view>

#include "aurora/app/single_instance.h"
#include "aurora/core/result.h"

namespace aurora {

/// @brief 扩展名的当前归属。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct AssociationInfo {
    std::string application;  ///< 归属应用标识（平台各异）；空串 = 无关联
    std::string source;  ///< 结论来源（查询命中层），不参与归属比较
};

/// @brief 查询某扩展名当前实际生效的归属（不改变任何登记）。
/// @param extension 扩展名（可带前导点，任意大小写；空串与孤立点被拒）。
/// @return 成功携归属（无关联时 `application` 为空串，不是错误）；扩展名非法为
///         `general-invalid-argument`；能力位为 false 为 `file-association-unsupported`；
///         查询失败（数据库打不开等）为 `file-association-failed`。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 读用户级注册表 / `mimeapps.list` / Launch Services
[[nodiscard]] auto associated_application(std::string_view extension) -> Result<AssociationInfo>;

/// @brief 一条文件关联声明：本应用可处理某扩展名。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct AssociationDeclaration {
    std::string extension;  ///< 扩展名（可带前导点，任意大小写；**必填**）。
    std::string mime_type;  ///< MIME（可空：按扩展名映射；与扩展名皆缺时非法）。
    std::string display_name;  ///< 显示名（可空：回落为可执行文件名）。
};

/// @brief 声明「本应用可处理该扩展名」。
///
/// **不代表**把本应用设成默认处理程序——那属用户偏好，本库不做（见文件头立场）。声明之后本应用
/// 会出现在系统「打开方式」候选里，默认仍由用户决定。
///
/// 同一扩展名重复声明是替换（幂等）。声明不覆盖别的应用已登记的默认归属。
/// @param declaration 扩展名 / MIME / 显示名。
/// @param identity 应用身份（可执行路径进 OS 命令模板；macOS 另认 bundle identifier）。
/// @return 成功，或 `general-invalid-argument`（扩展名非法 / MIME 缺失 / 身份缺可执行路径）、
///         `file-association-unsupported`（能力位为 false）、`file-association-failed`（写入失败，
///         含 macOS 不在 bundle 内）。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 写用户级注册表键 / 桌面条目 / `mimeapps.list`
[[nodiscard]] auto register_file_association(const AssociationDeclaration &declaration, const AppIdentity &identity)
    -> Result<void>;

/// @brief 撤销本应用对该扩展名的声明。
///
/// 归属口径同 URL 方案：无登记 → 幂等成功；登记是本应用 → 删除；登记是别的应用 →
/// **不改任何字节**并返回 `file-association-failed`，detail 点名当前归属。
///
/// 本函数只撤销**声明**，不触碰默认处理程序（本库从未设过默认，也不做「还原默认」——
/// 那是用户偏好的领域）。
/// @param extension 扩展名（可带前导点，任意大小写）。
/// @param identity 应用身份（用于判定归属是不是自己）。
/// @return 成功（含本来就没声明），或 `general-invalid-argument` / `file-association-unsupported` /
///         `file-association-failed`。
/// @note Thread: main-thread only（阻塞 IO）
/// @note Side-effects: 删用户级注册表键 / 桌面条目
[[nodiscard]] auto unregister_file_association(std::string_view extension, const AppIdentity &identity) -> Result<void>;

}  // namespace aurora
