#pragma once
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
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct Options {
    std::string title;  ///< 对话框标题（空串 = 平台默认标题）
    std::string initial_dir;  ///< 初始目录（空串 = 平台默认位置）
    std::vector<Filter> filters;  ///< 文件筛选器列表（空 = 不限制类型）
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): 故意的 headless 测试可变全局钩子
/// @brief headless 测试预设返回值：置为非空路径列表 → open_file() 直接返回；空 → 进入真实/取消路径。
inline std::vector<std::string> headless_open_result;
/// @brief headless 测试预设返回值：置为非空字符串 → save_file() 直接返回；空 → 进入真实/取消路径。
inline std::string headless_save_result;
/// @brief headless 测试预设返回值：置为非空字符串 → open_folder() 直接返回；空 → 进入真实/取消路径。
inline std::string headless_folder_result;

/// @brief 交互模式开关。
/// - `true`（默认）：弹出真实系统对话框（最终用户场景）。
/// - `false`：在 headless / CTest 等自动化环境中，hook 为空时直接返回空（等价取消），
///   避免 GUI 交互测试卡在等待用户操作（见 `AGENTS.md`：避免引入 GUI 交互测试）。
inline bool interactive = true;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

#ifdef AURORA_PLATFORM_WINDOWS
/// @brief 打开文件选择对话框（可多选）。真实平台（AURORA_PLATFORM_WINDOWS）实现见
/// `src/aurora/app/file_dialog_win32.cpp`；非 Win32 / Headless 用下方内联回退（保留 headless 钩子，便于测试）。
/// @param opts 对话框选项（标题/初始目录/筛选器；headless 回退与钩子路径不消费它）。
/// @return 选中路径列表；取消或未选中为 `Ok(空列表)`（取消不是失败）。仅 COM 初始化或对话框创建失败时返回 `Error`。
[[nodiscard]] auto open_file(const Options &opts = {}) -> Result<std::vector<std::string>>;
/// @brief 打开文件保存对话框（单路径）。Win32 走 IFileSaveDialog，其余平台为内联回退（见上方说明）。
/// @param opts 对话框选项。
/// @return 选定的保存路径；取消或未选中为 `Ok(空串)`；仅 COM 初始化或对话框创建失败时返回 `Error`。
[[nodiscard]] auto save_file(const Options &opts = {}) -> Result<std::string>;
/// @brief 打开文件夹选择对话框（单路径）。Win32 走 IFileOpenDialog 目录模式，其余平台为内联回退（见上方说明）。
/// @param opts 对话框选项。
/// @return 选定的目录路径；取消或未选中为 `Ok(空串)`；仅 COM 初始化或对话框创建失败时返回 `Error`。
[[nodiscard]] auto open_folder(const Options &opts = {}) -> Result<std::string>;
#else
/// @brief 打开文件选择对话框（headless 回退）：优先返回 `headless_open_result` 钩子内容。
/// @param opts 对话框选项（回退实现忽略）。
/// @return 钩子非空时为 `Ok(钩子列表)`；否则 `Ok(空列表)`（等价取消，不弹任何 UI）。
[[nodiscard]] inline auto open_file(const Options &opts = {}) -> Result<std::vector<std::string>> {
    (void)opts;
    if (!headless_open_result.empty()) {
        return headless_open_result;
    }
    return std::vector<std::string>{};
}
/// @brief 打开文件保存对话框（headless 回退）：优先返回 `headless_save_result` 钩子内容。
/// @param opts 对话框选项（回退实现忽略）。
/// @return 钩子非空时为 `Ok(钩子路径)`；否则 `Ok(空串)`（等价取消）。
[[nodiscard]] inline auto save_file(const Options &opts = {}) -> Result<std::string> {
    (void)opts;
    if (!headless_save_result.empty()) {
        return headless_save_result;
    }
    return std::string{};
}
/// @brief 打开文件夹选择对话框（headless 回退）：优先返回 `headless_folder_result` 钩子内容。
/// @param opts 对话框选项（回退实现忽略）。
/// @return 钩子非空时为 `Ok(钩子目录)`；否则 `Ok(空串)`（等价取消）。
[[nodiscard]] inline auto open_folder(const Options &opts = {}) -> Result<std::string> {
    (void)opts;
    if (!headless_folder_result.empty()) {
        return headless_folder_result;
    }
    return std::string{};
}
#endif

}  // namespace aurora::file_dialog
