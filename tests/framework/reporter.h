#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 结果报告
// ------------------------------------------------------------
// `--report=<path>` 由扩展名推断格式：`.xml` → JUnit XML，其余 → JSON。
// 两者都是手写序列化（框架零三方依赖；测试 TU 也不该为报告拉进 json 头）。
//
// 超时路径同样调用本模块：watchdog 触发时把「已完成的部分结果」落盘后再以退出码 3 结束，
// 避免整轮结果随进程一起丢掉（文件级粒度下这一步尤其重要）。
// ============================================================

#include <string>
#include <string_view>
#include <vector>

#include "test_runner.h"

namespace aurora::testing {

/// @brief 写出报告；失败时把原因写入 `error` 并返回 false。
///
/// `results` 允许是「尚未跑完的部分结果」（超时路径）。
[[nodiscard]] auto write_report(std::string_view path, const std::vector<CaseResult>& results,
                                const RunSummary& summary, std::string* error) -> bool;

/// @brief 转义 XML 文本节点 / 属性里的控制字符与实体（供报告与调试使用）。
[[nodiscard]] auto xml_escape(std::string_view text) -> std::string;

/// @brief 转义 JSON 字符串（含控制字符的 \\uXXXX 形式）。
[[nodiscard]] auto json_escape(std::string_view text) -> std::string;

}  // namespace aurora::testing
