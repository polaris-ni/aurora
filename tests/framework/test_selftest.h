#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 内建自检入口
// ------------------------------------------------------------
// 自检用**内建 synthetic 用例**验证断言家族、值打印、追踪与退出码协议，
// 不进注册表，故不影响 `registry_integrity` 的清单比对。
// 由 runner 的 `--selftest` 调用（见 `test_main.cpp`）。
// ============================================================

namespace aurora::testing {

/// @brief 运行框架内建自检；全部通过返回退出码 0，否则返回 1。
[[nodiscard]] auto run_framework_selftest() -> int;

}  // namespace aurora::testing
