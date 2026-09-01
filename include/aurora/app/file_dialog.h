#pragma once
#include "aurora/core/platform.h"


#include <string>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {
namespace file_dialog {

/// @brief 文件筛选器（名称 + 扩展名列表，如 `{"\u56fe\u50cf", {"*.png","*.jpg"}}`）。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct Filter {
    std::string name;
    std::vector<std::string> extensions;
};

/// @brief 对话框选项。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct Options {
    std::string title;
    std::string initial_dir;
    std::vector<Filter> filters;
};

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

// 真实平台（AURORA_PLATFORM_WINDOWS）实现见 src/aurora/app/file_dialog_win32.cpp；
// 非 Win32 / Headless 用下方内联回退（保留 headless 钩子，便于测试）。
#if defined(AURORA_PLATFORM_WINDOWS)
[[nodiscard]] auto open_file(const Options &opts = {}) -> Result<std::vector<std::string>>;
[[nodiscard]] auto save_file(const Options &opts = {}) -> Result<std::string>;
[[nodiscard]] auto open_folder(const Options &opts = {}) -> Result<std::string>;
#else
[[nodiscard]] inline auto open_file(const Options &opts = {}) -> Result<std::vector<std::string>> {
    (void)opts;
    if (!headless_open_result.empty())
        return headless_open_result;
    return std::vector<std::string>{};
}
[[nodiscard]] inline auto save_file(const Options &opts = {}) -> Result<std::string> {
    (void)opts;
    if (!headless_save_result.empty())
        return headless_save_result;
    return std::string{};
}
[[nodiscard]] inline auto open_folder(const Options &opts = {}) -> Result<std::string> {
    (void)opts;
    if (!headless_folder_result.empty())
        return headless_folder_result;
    return std::string{};
}
#endif

} // namespace file_dialog
} // namespace aurora
