#pragma once

// ============================================================
// 测试框架（tests/framework/）—— `au::json::Value` 的失败诊断打印
// ------------------------------------------------------------
// `value_print.h` 的正文刻意不依赖任何 aurora 头（唯一例外是零成本的
// `aurora/core/platform.h`，见其自身说明）。故 `json::Value` 的打印以显式特化单独
// 接入此处：走其自身序列化，而不是落进「自引用退化容器」分支后再去要求一个并不
// 存在的 `operator<<`。需要它的测试 TU 显式包含本头——依赖 `au::json` 的事实被
// 隔离在这里，框架正文的零依赖约定不受影响。
// ============================================================

#include <string>
#include <utility>

#include "aurora/core/json.h"
#include "value_print.h"

namespace aurora::testing {

/// @brief `json::Value` 定制打印：紧凑 JSON 文本；含非有限 Double 时给出说明性文本。
template <>
struct ValuePrinter<aurora::json::Value> {
    static auto print(const aurora::json::Value &value) -> std::string {
        auto text = aurora::json::dump(value);
        if (!text.ok()) {
            return "<json non-serializable: " + text.error().message + ">";
        }
        return std::move(text).value();
    }
};

}  // namespace aurora::testing
