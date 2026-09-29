#pragma once

// ============================================================
// 测试框架（tests/framework/）—— JSON 字面量构造辅助
// ------------------------------------------------------------
// `au::json::Value` 刻意不提供初始化列表构造（避免 nlohmann 那类隐式类型推断），
// 构造对象/数组须显式 `Value::object()` + `set(...)`。若在用例里逐点手写，「造数据」
// 的噪音会淹没断言本身的意图；故这里给出两个薄封装。
//
// 定位：**不是**迁移期的兼容层——它不引用任何第三方 JSON 库，不参与新旧库对拍，
// 只在新 API 之上提供测试专用的构造糖；迁移收尾后照常保留。
// ============================================================

#include <initializer_list>
#include <string_view>
#include <utility>

#include "aurora/core/json.h"

namespace aurora::testing {

/// @brief 由键值对构造 Object（键按给定顺序插入）。
[[nodiscard]] inline auto json_obj(std::initializer_list<std::pair<std::string_view, json::Value>> items)
    -> json::Value {
    auto out = json::Value::object();
    for (const auto &[key, value] : items) {
        out.set(key, value);
    }
    return out;
}

/// @brief 由元素序列构造 Array（元素按给定顺序追加）。
[[nodiscard]] inline auto json_arr(std::initializer_list<json::Value> items) -> json::Value {
    auto out = json::Value::array();
    for (const auto &item : items) {
        out.push_back(item);
    }
    return out;
}

}  // namespace aurora::testing
