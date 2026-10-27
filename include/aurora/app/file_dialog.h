#pragma once
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/core/result.h"

namespace aurora::file_dialog {

/// @brief 文件筛选器（名称 + 扩展名列表，如 `{"\u56fe\u50cf", {"*.png","*.jpg"}}`）。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct Filter {
    std::string name;  ///< 筛选器显示名称（如 "图像"）
    std::vector<std::string> extensions;  ///< 通配模式列表（如 {"*.png","*.jpg"}）
};

/// @brief 对话框选项。
///
/// 四枚字符串格一律 **UTF-8 窄串**（与 `title` 同口径），空串一律读作「不表达意图」而不是「定成空」；
/// 各腿自行转成平台形态（Win32 走 `aurora::internal::utf8_to_wstr`，桌面自绘腿按文本面直接吃窄串）。
/// 逐格的消费面见 `codespec/specification/06-app-platform.md` §8.1.3。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct Options {
    std::string title;  ///< 对话框标题（空串 = 平台默认标题；三腿均消费）
    std::string initial_dir;  ///< 初始目录（空串 = 平台默认位置；不可解析时留 WARN 并按该腿默认位置打开）
    std::string suggested_name;  ///< 建议文件名（仅 save_file 消费；含目录分隔符时取末段并留 WARN）
    std::string default_extension;  ///< 缺省扩展名（仅 save_file 消费；名字没打点才补，"txt" 与 ".txt" 同解）
    std::vector<Filter> filters;  ///< 文件筛选器列表（空 = 不限制类型）
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): 故意的 headless 测试可变全局钩子
/// @brief headless 测试预设返回值：置为非空路径列表 → open_file() 直接返回；空 → 进入真实/取消路径。
inline std::vector<std::string> headless_open_result;
/// @brief headless 测试预设返回值：置为非空字符串 → save_file() 直接返回；空 → 进入真实/取消路径。
inline std::string headless_save_result;
/// @brief headless 测试预设返回值：置为非空字符串 → open_folder() 直接返回；空 → 进入真实/取消路径。
inline std::string headless_folder_result;
/// @brief headless 测试观察面：最后一次传进任一入口的 `Options` 原文（三腿、六个入口一律在函数第一行
///        记录，排在钩子判据之前，故走钩子 / 非交互分支同样可观察）。
///
/// 存在的理由是「建议文件名有没有被消费」这类命题否则没有自动化落点：三枚 `headless_*` 钩子是**预设
/// 返回值**，`interactive == false` 只是回空，传入的 `Options` 今天无处可读。记录的是原文而非折算结果
/// （折算由各腿在投递前做，本面不替它记账）。`std::nullopt` = 本进程从未调用过任一入口，空 `Options`
/// = 调用过但没表达意图——两者不是同一件事。进程级全局，与其余钩子同款复位纪律（用例须自行还原）。
inline std::optional<Options> headless_last_options;

/// @brief 交互模式开关。
/// - `true`（默认）：弹出真实系统对话框（最终用户场景）。
/// - `false`：在 headless / CTest 等自动化环境中，hook 为空时直接返回空（等价取消），
///   避免 GUI 交互测试卡在等待用户操作（见 `AGENTS.md`：避免引入 GUI 交互测试）。
inline bool interactive = true;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// 平台族三分支（specification/06-app-platform.md §8 文件 / 目录选择器）：
//   - AURORA_PLATFORM_WINDOWS：COM 对话框（IFileDialog 系），实现见
//     `src/aurora/app/file_dialog_win32.cpp`；`title` 走 `SetTitle`、`initial_dir` 走
//     `GetFullPathNameW` 归一 + `SHCreateItemFromParsingName` + `SetFolder`、`save_file` 另走
//     `SetFileName`（建议名）与 `SetDefaultExtension`（缺省扩展名，补全由 OS 在名字栏里做）；
//   - AURORA_PLATFORM_LINUX / AURORA_PLATFORM_MACOS：框架自绘选择器（Dialog + ListView +
//     TextInput 装配，std::filesystem 枚举，经 `Application::run_modal` 嵌套帧循环驱动），
//     实现见 `src/aurora/app/file_dialog_selfdrawn.cpp`；无 Application 上下文（无头 / 纯库
//     调用 / 尚未 run）时 WARN 后回空——不静默说谎、不挂起。同一份 `Options` 在此腿的落点是：
//     `initial_dir` 定初始目录、`suggested_name`（末段）同时喂状态机的保存名与文件名输入框初值、
//     `default_extension` 在**确认时**补（名字没打点才补）；
//   - 其余平台（WASM / Android / BSD 等）：保留下方内联回退——这些平台不做真实选择器
//     （WASM 主线程不可阻塞、文件系统是 MEMFS 而非用户真实磁盘），仅保钩子语义，
//     `Options` 五格一概不消费（但同另两腿一样在入口记录 `headless_last_options`）。
// `suggested_name` 与 `default_extension` 只属保存形态：`open_file` / `open_folder` 不消费它们——
// 打开态预选一个名字与「选一个已存在的文件」相互冲突，目录形态根本没有名字概念。
// 三个 headless 钩子与 `interactive` 开关的优先级次序三支共用：钩子优先 → 非交互回空。
#ifdef AURORA_PLATFORM_WINDOWS
/// @brief 打开文件选择对话框（可多选）。Win32 实现见 `src/aurora/app/file_dialog_win32.cpp`。
/// @param opts 对话框选项（`title` / `initial_dir` / `filters` 消费，`suggested_name` 与
///        `default_extension` 不消费；钩子路径不投递任何一格，但仍记录到 `headless_last_options`）。
/// @return 选中路径列表；取消或未选中为 `Ok(空列表)`（取消不是失败）。仅 COM 初始化或对话框创建失败时返回 `Error`。
[[nodiscard]] auto open_file(const Options &opts = {}) -> Result<std::vector<std::string>>;
/// @brief 打开文件保存对话框（单路径）。Win32 走 IFileSaveDialog。
/// @param opts 对话框选项（`title` / `initial_dir` / `filters` 之外，本入口另消费 `suggested_name`
///        作名字栏初值、`default_extension` 作无扩展名时的补全；空串一律 = 不表达意图）。
/// @return 选定的保存路径；取消或未选中为 `Ok(空串)`；仅 COM 初始化或对话框创建失败时返回 `Error`。
[[nodiscard]] auto save_file(const Options &opts = {}) -> Result<std::string>;
/// @brief 打开文件夹选择对话框（单路径）。Win32 走 IFileOpenDialog 目录模式。
/// @param opts 对话框选项（`title` / `initial_dir` 消费；目录无类型与文件名语义，`filters` /
///        `suggested_name` / `default_extension` 不消费）。
/// @return 选定的目录路径；取消或未选中为 `Ok(空串)`；仅 COM 初始化或对话框创建失败时返回 `Error`。
[[nodiscard]] auto open_folder(const Options &opts = {}) -> Result<std::string>;
#elif defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
/// @brief 打开文件选择对话框（可多选）。桌面平台（Linux / macOS）为框架自绘选择器，
///        实现见 `src/aurora/app/file_dialog_selfdrawn.cpp`；真交互路径在调用线程内阻塞到
///        用户作答（与 `QFileDialog::exec()` 同口径），允许一枚选择器叠在另一枚内层打开。
/// @param opts 对话框选项：`title` 作浮层标题（空 = 形态默认标题）、`initial_dir` 作初始
///        位置（空 = 当前工作目录；不存在 / 不可读降级到根并留 WARN）、`filters` 过滤文件条目；
///        `suggested_name` / `default_extension` 属保存形态，本入口不消费。
/// @return 选中文件的绝对路径列表；取消或未选中为 `Ok(空列表)`（取消不是失败）。本腿系统级
///         失败一律降级 + WARN，不返回 `Error`。
[[nodiscard]] auto open_file(const Options &opts = {}) -> Result<std::vector<std::string>>;
/// @brief 打开文件保存对话框（单路径）。桌面平台为框架自绘选择器（带文件名输入框）。
/// @param opts 对话框选项（口径同 `open_file`；`filters` 仅影响列表展示；`suggested_name` 取末段
///        后同时作状态机的保存名与输入框初值，`default_extension` 在确认时补——名字里没打点才补）。
/// @return 选定的保存路径（当前目录 + 文件名，绝对路径）；取消或未选中为 `Ok(空串)`。
[[nodiscard]] auto save_file(const Options &opts = {}) -> Result<std::string>;
/// @brief 打开文件夹选择对话框（单路径）。桌面平台为框架自绘选择器；目录无类型语义，
///         `filters` 被忽略。
/// @param opts 对话框选项（`filters` / `suggested_name` / `default_extension` 忽略，口径见 `save_file`）。
/// @return 选定的目录绝对路径；取消或未选中为 `Ok(空串)`。
[[nodiscard]] auto open_folder(const Options &opts = {}) -> Result<std::string>;
#else
/// @brief 打开文件选择对话框（无真实选择器平台的内联回退）：优先返回 `headless_open_result` 钩子内容。
/// @param opts 对话框选项（回退实现不投递任何一格，仅在入口记录到 `headless_last_options`）。
/// @return 钩子非空时为 `Ok(钩子列表)`；否则 `Ok(空列表)`（等价取消，不弹任何 UI）。
[[nodiscard]] inline auto open_file(const Options &opts = {}) -> Result<std::vector<std::string>> {
    headless_last_options = opts;
    if (!headless_open_result.empty()) {
        return headless_open_result;
    }
    return std::vector<std::string>{};
}
/// @brief 打开文件保存对话框（无真实选择器平台的内联回退）：优先返回 `headless_save_result` 钩子内容。
/// @param opts 对话框选项（回退实现不投递任何一格，仅在入口记录到 `headless_last_options`）。
/// @return 钩子非空时为 `Ok(钩子路径)`；否则 `Ok(空串)`（等价取消）。
[[nodiscard]] inline auto save_file(const Options &opts = {}) -> Result<std::string> {
    headless_last_options = opts;
    if (!headless_save_result.empty()) {
        return headless_save_result;
    }
    return std::string{};
}
/// @brief 打开文件夹选择对话框（无真实选择器平台的内联回退）：优先返回 `headless_folder_result` 钩子内容。
/// @param opts 对话框选项（回退实现不投递任何一格，仅在入口记录到 `headless_last_options`）。
/// @return 钩子非空时为 `Ok(钩子目录)`；否则 `Ok(空串)`（等价取消）。
[[nodiscard]] inline auto open_folder(const Options &opts = {}) -> Result<std::string> {
    headless_last_options = opts;
    if (!headless_folder_result.empty()) {
        return headless_folder_result;
    }
    return std::string{};
}
#endif

}  // namespace aurora::file_dialog
