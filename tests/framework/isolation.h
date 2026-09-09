#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 用例边界资源隔离
// ------------------------------------------------------------
// 并行模型（CTest 进程隔离 + 资源虚拟化）的进程内侧：
//   - cwd：runner 启动时统一切到仓库根（经可执行文件位置向上定位），
//     相对路径解析不再依赖 CTest 的 WORKING_DIRECTORY 白名单；
//   - tmpdir：每个用例开始时创建唯一临时目录并接管 TMPDIR/TMP/TEMP，
//     用例结束后清理，偏好 / 存储类用例的临时文件写入彼此隔离；
//   - 剪贴板：用例结束兜底卸载库侧 memory 注入后端（Clipboard::remove_test_backend），
//     防止用例把注入状态泄漏到后续用例。
//
// 库侧注入点（memory 剪贴板后端）见 include/aurora/app/clipboard.h 的 test-only 段，
// 受 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏控制，关闭时为 no-op。
// ============================================================

#include <string>

namespace aurora::testing::isolation {

/// @brief runner 启动期装配：把 cwd 切到仓库根（可定位时；失败保持原 cwd 不变）。
///
/// 由 main 在解析 CLI 之前调用一次，保证 `--report=<相对路径>` 与用例内相对路径
/// 全部以仓库根为基准；死亡测试子进程重跑 main 时同样生效（无害幂等）。
auto setup() -> void;

/// @brief 用例开始：创建本轮唯一临时目录并接管 TMPDIR/TMP/TEMP；兜底卸载剪贴板注入残留。
auto begin_case() -> void;

/// @brief 用例结束：清理本轮临时目录；卸载库侧剪贴板 memory 后端（未安装为 no-op）。
auto end_case() -> void;

/// @brief 当前用例的临时目录（begin_case 之后有效；未启用时为空串）。
[[nodiscard]] auto temp_dir() -> const std::string&;

/// @brief 仓库根绝对路径（从可执行文件位置向上定位；定位失败返回空串，cwd 保持不变）。
[[nodiscard]] auto repo_root() -> const std::string&;

}  // namespace aurora::testing::isolation
