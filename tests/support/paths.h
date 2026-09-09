#pragma once

// ============================================================
// 测试公共设施（tests/support/paths.h）—— 仓库相对路径解析
// ------------------------------------------------------------
// 框架启动时已把 cwd 统一切到仓库根（isolation::setup），用例内相对路径本可直接用；
// 本头提供显式的仓库根定位与拼接，供需要绝对路径的场景（如把路径传给子进程、
// 或与「相对路径可能被别处 chdir」的库代码交互）使用。
// 仓库根定位失败（安装到仓库外的 runner）时 under_repo 原样退回相对路径。
// ============================================================

#include <filesystem>
#include <string>
#include <string_view>

#include "framework/isolation.h"

namespace aurora::testing::paths {

/// @brief 仓库根绝对路径（从可执行文件位置向上定位；失败返回空串）。
[[nodiscard]] inline auto repo_root() -> const std::string& { return isolation::repo_root(); }

/// @brief 仓库根下的绝对路径；仓库根不可定位时原样返回相对路径（保持可运行）。
[[nodiscard]] inline auto under_repo(std::string_view relative) -> std::string {
    const auto& root = repo_root();
    if (root.empty()) {
        return std::string{relative};
    }
    return (std::filesystem::path{root} / std::string_view{relative}).string();
}

}  // namespace aurora::testing::paths
