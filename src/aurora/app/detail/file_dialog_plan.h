#pragma once

// 文件对话框折算层：**内部头**（与 `file_picker.h` / `platform_shell_win32.h` 同列于 src/）。
//
// 为何单列且零平台依赖：`Options` 里的「建议文件名」与「缺省扩展名」落到两条真实现腿上是完全不同的
// 动作——Win32 腿把字符串交给 `IFileDialog::SetFileName` / `SetDefaultExtension`（补全由 OS 在名字栏
// 里做），桌面自绘腿把字符串喂给 `TextInput` 初值并在确认时拼扩展名。若「只取末段名字」「前导点怎么
// 处理」「什么时候不补全」这几条规则各写一遍，就会出现两套口径（同一份 `Options` 在两条腿上落出不同
// 的文件名），而文件名正是这条 API 的全部语义。故规则收敛为**纯函数**，两座腿共用同一份换算。
//
// 本头不引任何 `<windows.h>` / POSIX 头：只有 `<string>` / `<string_view>` 与公共面的
// `app/file_dialog.h`、`core/log.h`，因此在任何 CI 作业（含无头 Linux）上都能编译并被逐值单测。
//
// 不做的事：**投递**。折算层只产出字符串（该传什么名字、该补什么扩展名），不调用任何 OS setter，
// 也不决定「哪枚入口消费哪一格」——那是各腿入口的事（见 `file_dialog_win32.cpp` /
// `file_dialog_selfdrawn.cpp` 与 `app/file_dialog.h` 的消费口径注释）。
//
// 门控：无。本头在**所有**平台编译（含 wasm）。

#include <string>
#include <string_view>

namespace aurora::file_dialog_plan {

/// @brief 取路径的末段名字（平台无关，同时认 `/` 与 `\`）。
///
/// 只做「找最后一个分隔符并取其后」，不做规范化、不判存在性、不处理 `.` / `..`（那是文件系统层的
/// 事，本层只关心「要显示在名字栏里的那一段」）。尾部分隔符意味着末段为空——那是目录不是文件名，
/// 按空串交回，调用方据此当作「不表达意图」。
/// @param path 任意路径串（UTF-8 窄串，按字节找分隔符，多字节名字不受影响）。
/// @return 末段子串（指向 `path` 内部，故调用方须保证其存活）；空串输入回空串。
/// @note Thread: main-thread only（纯函数，实际无线程约束）
/// @note Side-effects: pure
[[nodiscard]] auto last_path_segment(std::string_view path) -> std::string_view;

/// @brief 折算「建议文件名」：只取末段，截断与目录形态各留一条可归因 WARN。
///
/// 口径三条：⑴ 传入含分隔符时取末段并留 WARN（不静默丢弃目录部分，也不整串塞进名字栏——那在
/// Win32 上会被 OS 直接拒掉，在自绘腿上会显示成一个带斜杠的怪名字）；⑵ 空串回空串、不留 WARN
/// （空 = 不表达意图，与不传该字段同形）；⑶ 末段为空（尾分隔符）回空串并留 WARN。
/// @param raw 调用方给的原始 `Options::suggested_name`（UTF-8 窄串）。
/// @return 可直接投递给对话框的名字（空串 = 不表达意图）。
/// @note Side-effects: logs a WARN when the input is truncated or names a directory
[[nodiscard]] auto suggested_file_name(std::string_view raw) -> std::string;

/// @brief 归一「缺省扩展名」：去掉前导点与首尾空白，其余原样。
///
/// 存在的理由是一条实测平台事实：Win32 的 `IFileDialog::SetDefaultExtension` 要的是不含点的扩展名
/// （给 `.txt` 会被 OS 再补一个点，落成 `name..txt`），而自绘腿要拼的是含点的后缀。两腿都从这枚
/// 归一结果出发，调用方写 `"txt"` 或 `".txt"` 得到同一个结果。
/// @param ext 调用方给的原始 `Options::default_extension`（UTF-8 窄串）。
/// @return 不含前导点的扩展名（空串 = 不补全）。
/// @note Side-effects: pure
[[nodiscard]] auto normalize_extension(std::string_view ext) -> std::string;

/// @brief 按缺省扩展名补全文件名（自绘腿的确认口径；Win32 腿由 OS 在名字栏里补，不走本函数）。
///
/// 三条不补的情形写死在这里，免得两腿各解释一次：⑴ 名字为空（空名 = 取消，补出个
/// `.txt` 就把取消变成了写入）；⑵ 缺省扩展名为空（不表达意图）；⑶ 名字的最后一段里已有点
/// （用户已经打过扩展名，隐式改写用户的输入是另一回事）。
/// @param name 待确认的文件名（可以是全路径，判定只看最后一段）。
/// @param ext 缺省扩展名（可含前导点，内部归一）。
/// @return 补全后的名字；三条不补情形下逐字节等于入参 `name`。
/// @note Side-effects: pure
[[nodiscard]] auto with_default_extension(std::string_view name, std::string_view ext) -> std::string;

}  // namespace aurora::file_dialog_plan
