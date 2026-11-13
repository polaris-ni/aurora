#pragma once

// 桌面外壳集成折算层：**内部头**（src/aurora/app/detail/，与 file_dialog_plan.h 同列）。
//
// 收敛四类「字符串 / 路径 / 文本」语义，三平台桥共用同一份判定：
// - URL 方案文法（RFC 3986）与保留方案拒收；
// - 文件扩展名规范化与平台无关的 MIME 方向映射；
// - 应用身份（可执行路径规范化 → 稳定实例键）与命令行中的 URI 提取；
// - Linux 桌面条目文本（URL 方案 / 文件关联 / 自动启动共用一份拼装与开关键翻转）。
//
// 本头不引任何 `<windows.h>` / Cocoa / Xlib 头，也不触碰 OS：只有标准库与 core 头，
// 因此在任何 CI 作业（含无头 Linux / wasm）上都能编译并被单测逐值覆盖。
//
// 门控：无。本头在**所有**平台编译。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::desktop_shell {

/// @brief URL 方案校验结论。
enum class SchemeCheck : std::uint8_t {
    Valid = 0,  ///< 文法合法且不是保留方案。
    Empty = 1,  ///< 空串（调用方笔误）。
    InvalidSyntax = 2,  ///< 不满足 RFC 3986 `ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`。
    Reserved = 3,  ///< 文法合法但属于保留方案（http/https/file/mailto 等），不得被应用抢占。
};

/// @brief 规范化方案名（ASCII 小写；不做文法校验）。
/// @param scheme 原始方案名。
/// @return 小写副本。
[[nodiscard]] auto normalize_url_scheme(std::string_view scheme) -> std::string;

/// @brief 校验 URL 方案（文法 + 保留名单），纯字符串判定。
/// @param scheme 待校验方案（大小写不敏感）。
/// @return 三态结论；`Reserved` 与 `InvalidSyntax` 分开，调用方可给出不同 detail。
[[nodiscard]] auto check_url_scheme(std::string_view scheme) -> SchemeCheck;

/// @brief 是否属于保留方案（大小写不敏感）。
/// @param scheme 方案名。
/// @return 命中保留名单为 true。
[[nodiscard]] auto is_reserved_scheme(std::string_view scheme) -> bool;

/// @brief 规范化文件扩展名：去掉一个前导点、ASCII 小写、拒绝空串 / 孤立点 / 路径分隔符 / 内嵌点。
/// @param extension 原始扩展名（`.PNG` / `png` / `PNG` 折叠为同一键）。
/// @return 成功给出规范化后的扩展名（无前导点）；非法输入为 `std::nullopt`。
[[nodiscard]] auto normalize_extension(std::string_view extension) -> std::optional<std::string>;

/// @brief 扩展名 → MIME 的方向映射（大小写/前导点不敏感）。
///
/// 词汇一律取自 `include/aurora/app/mime_types.h`（MIME 唯一真源，见该头文件头注）；
/// 未登记的扩展名返回 `std::nullopt`，调用方仍可自行提供 MIME 字符串声明关联。
/// @param extension 扩展名（可带前导点、任意大小写）。
/// @return 命中的 MIME（指向静态常量）；未登记为 `std::nullopt`。
[[nodiscard]] auto extension_to_mime(std::string_view extension) -> std::optional<std::string_view>;

/// @brief 由可执行路径派生稳定实例键（FNV-1a 64 十六进制）。
///
/// 规范化口径：分隔符统一为 `/`、词法消解 `.` / `..`、Windows / macOS 上 ASCII 大小写折叠、
/// 存在时经 `weakly_canonical` 解析符号链接。**不**混入版本号、mtime 或命令行内容——键一变，
/// 单实例的互斥量与锁文件就认不出自己，表现为「每次都当成首个实例」。
/// @param executable_path 可执行文件路径（可为绝对或相对；空串得到固定空键前缀的哈希）。
/// @return 16 位小写十六进制实例键（空输入亦非空串）。
[[nodiscard]] auto derive_instance_key(std::string_view executable_path) -> std::string;

/// @brief 从命令行参数（不含 argv[0]）中取第一条 URI。
///
/// 判定：`scheme:` 前缀且方案合法、长度 ≥ 2（避开 Windows 盘符 `C:`）、`:` 后不是反斜杠。
/// 不做 URI 解码、不校验 host/path——转交语义由 `Navigator::open_uri` 负责。
/// @param arguments 命令行参数快照（不含可执行文件名）。
/// @return 第一条命中的 URI 原样串；没有则为 `std::nullopt`。
[[nodiscard]] auto extract_uri_argument(const std::vector<std::string> &arguments) -> std::optional<std::string>;

/// @brief 为 `Exec=` / 命令行模板安全引用一枚参数（含空格或引号时加双引号并转义）。
/// @param argument 原始参数（路径 / `%u` 等）。
/// @return 可直接拼进 `Exec=` 的片段。
[[nodiscard]] auto quote_exec_argument(std::string_view argument) -> std::string;

/// @brief Linux 桌面条目的平台无关描述（三处注册共用同一拼装器）。
struct DesktopEntry {
    std::string name;  ///< `Name=` 显示名（必填；空串由拼装器回落为可执行名）。
    std::string exec;  ///< `Exec=` 命令模板（已由 `quote_exec_argument` 处理）。
    std::string icon;  ///< `Icon=`（可空）。
    std::string mime_types;  ///< `MimeType=`（分号分隔，可空）。
    std::string url_scheme;  ///< `X-Aurora-Url-Scheme=`（可空；方案注册条目用）。
    std::string comment;  ///< `Comment=`（可空）。
    bool hidden = false;  ///< `Hidden=` 开关键（禁用态由它表达，不删文件）。
    bool autostart = false;  ///< 追加 `X-GNOME-Autostart-enabled` / `Type=Application` 等自动启动键。
};

/// @brief 拼装 `.desktop` 文本（确定性：固定键序、LF 结尾）。
/// @param entry 条目描述。
/// @return 完整桌面条目文本。
[[nodiscard]] auto build_desktop_entry(const DesktopEntry &entry) -> std::string;

/// @brief 读取桌面条目的禁用态。
///
/// `Hidden=true` 或 `X-GNOME-Autostart-enabled=false` 任一命中即禁用；两键都缺或为启用值则为启用。
/// @param text 桌面条目全文。
/// @return 禁用为 true；启用为 false。
[[nodiscard]] auto desktop_entry_disabled(std::string_view text) -> bool;

/// @brief 翻转桌面条目的开关键（启用 ↔ 禁用），**其余行原样保留**。
///
/// 启用态写 `Hidden=false`；禁用态写 `Hidden=true`。若原文件带 `X-GNOME-Autostart-enabled`，
/// 同向同步该键（保留用户留下的那条记录，不删文件、不抹其它键）。
/// @param text 原条目全文。
/// @param hidden true = 禁用，false = 启用。
/// @return 翻转后的全文（未出现对应键时追加）。
[[nodiscard]] auto set_desktop_entry_hidden(std::string_view text, bool hidden) -> std::string;

/// @brief 取桌面条目某个键的值（首个命中；键名大小写敏感，符合 Desktop Entry 规范）。
/// @param text 条目全文。
/// @param key 键名（如 `Exec`）。
/// @return 值（去首尾空白）；未出现为 `std::nullopt`。
[[nodiscard]] auto desktop_entry_value(std::string_view text, std::string_view key) -> std::optional<std::string>;

/// @brief 改写桌面条目某个键的值（首个命中行替换；缺键时追加；其余行原样保留）。
/// @param text 条目全文。
/// @param key 键名。
/// @param value 新值（空串表示删除该键所在行）。
/// @return 改写后的全文（LF 结尾）。
[[nodiscard]] auto desktop_entry_set_value(std::string_view text, std::string_view key, std::string_view value)
    -> std::string;

/// @brief 取 INI 文本某节内某个键的值（首个命中；节名 / 键名大小写敏感）。
///
/// `mimeapps.list` 与 `mimeinfo.cache` 是 freedesktop 的纯文本 INI 形态，解析层放这里
/// 以便无头单测；本函数不触碰文件系统。
/// @param text INI 全文。
/// @param section 节名（不含方括号，如 `Default Applications`）。
/// @param key 键名。
/// @return 值（去首尾空白）；节或键缺失为 `std::nullopt`。
[[nodiscard]] auto ini_value(std::string_view text, std::string_view section, std::string_view key)
    -> std::optional<std::string>;

/// @brief 写入 / 删除 INI 文本某节内的键（其余行原样保留；缺节时在末尾追加）。
/// @param text INI 全文（可空）。
/// @param section 节名（不含方括号）。
/// @param key 键名。
/// @param value 新值（空串表示删除该键；删除时保留空节）。
/// @return 改写后的全文（LF 结尾）。
[[nodiscard]] auto ini_set(std::string_view text, std::string_view section, std::string_view key,
                           std::string_view value) -> std::string;

/// @brief 把一项并入分号分隔列表（保序、去重、忽略空项）。
/// @param list 原列表（如 `image/png;image/jpeg;`，可空）。
/// @param item 待并入项。
/// @return 规范化列表（恒以 `;` 结尾）。
[[nodiscard]] auto list_add(std::string_view list, std::string_view item) -> std::string;

/// @brief 从分号分隔列表中移除一项（保序；不存在时返回规范化后的原列表）。
/// @param list 原列表。
/// @param item 待移除项。
/// @return 移除后的列表（为空时返回空串）。
[[nodiscard]] auto list_remove(std::string_view list, std::string_view item) -> std::string;

/// @brief 分号分隔列表是否含某项。
/// @param list 原列表。
/// @param item 待查项。
/// @return 命中为 true。
[[nodiscard]] auto list_contains(std::string_view list, std::string_view item) -> bool;

}  // namespace aurora::desktop_shell
