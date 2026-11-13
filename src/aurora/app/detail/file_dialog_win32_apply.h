#pragma once

// 文件对话框 Win32 腿的「投递」内部单元（src/aurora/app/detail/）。
//
// 为什么单列：`Options` 里的四格字符串落到 COM 对话框上是一组 OS setter（`SetTitle` /
// `SetFolder` / `SetFileName` / `SetDefaultExtension`），而这三条公共入口都要投同一批格。
// 收敛成两处可复用的投递函数，一是免把「先归一再解析再 SetFolder」这套次序在三处各写一遍
// （次序错一步就是静默不生效），二是让真机探针能对**同一份 shipping 代码**取证：探针自建
// COM 对话框对象、调用这两枚投递函数，再用 OS 自己的 `GetFolder` / `GetFileName` /
// `GetDefaultExtension` 读回，证据链不经过本库的折算，无头 CI 之外的这条接缝才证得实。
//
// 与折算层的分工：规则（取末段、去前导点、什么时候不补）在
// `src/aurora/app/detail/file_dialog_plan.h`，零平台头、全部 CI 作业逐值单测；本单元只做
// **投递 + 投递失败的留痕**，不解释 `Options` 的语义。
//
// 类型擦除：公共头与本头都不引 `<windows.h>`（口径同 `platform_shell_win32.h`），故对话框
// 指针以 `void *` 传递，实参一律是 `IFileDialog *`（`IFileOpenDialog` / `IFileSaveDialog`
// 都继承自它）；传 nullptr 或平台不符时两枚函数都是 no-op，不写任何状态。
//
// 失败口径：任一步失败只留一条 WARN 并让对话框停在 OS 默认位置——与桌面自绘腿「降级 +
// WARN」同形；不返回 `Error`（`Error` 只留给 COM 初始化 / 对话框创建这类真正开不出框的故障）。

#include "aurora/app/file_dialog.h"

namespace aurora::file_dialog::detail {

/// @brief 把 `Options::initial_dir` 投成对话框的当前目录（窄串转宽串 → `GetFullPathNameW`
///        归一 → `SHCreateItemFromParsingName` → `IFileDialog::SetFolder`）。
///
/// 归一这步是实测必需：`SHCreateItemFromParsingName` 不接受正斜杠形式（`C:/Windows` 直接回
/// `E_INVALIDARG`），而应用侧传来的串完全可能是正斜杠（跨平台代码里的路径写法）。
/// @param dialog `IFileDialog *` 的擦除形式；nullptr（平台不符 / 创建失败）时不投递。
/// @param opts 对话框选项；`initial_dir` 为空 = 不表达意图，原样交给 OS 默认位置。
/// @note Thread: main-thread only（COM 对话框随调用线程的套间）
/// @note Side-effects: mutates the dialog folder; logs a WARN when the path cannot be resolved
void apply_initial_dir(void *dialog, const Options &opts);

/// @brief 把保存形态的两格意图投成对话框初值（`suggested_name` → `SetFileName`，
///        `default_extension` → `SetDefaultExtension`）。
///
/// 名字先经折算层取末段（带目录分隔的串会被 OS 直接拒掉），扩展名去前导点（OS 自己补点，
/// 给了 `.txt` 会落成 `name..txt`）。两格互相独立：只给名字就只投名字，只给扩展名同理。
/// @param dialog `IFileDialog *` 的擦除形式（仅 `IFileSaveDialog` 上这两格有意义）。
/// @param opts 对话框选项；两格任一为空即该格不投递。
/// @note Thread: main-thread only
/// @note Side-effects: mutates the dialog name field and default extension; logs a WARN on rejection
void apply_save_name(void *dialog, const Options &opts);

}  // namespace aurora::file_dialog::detail
