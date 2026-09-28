// DOM 解析出口（公共函数 parse）。
//
// 本文件**不含**任何词法逻辑：解析全部委托给 sax.cpp 的字符级引擎，这里只提供把它的事件
// 装配成 Value 树的 `DomBuilder`。两套出口共用单一引擎，故 parse 与 parse_sax 的行为不可能分叉。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/json.h"

namespace aurora::json {

namespace {

/// @brief 内置 DOM builder：SAX 事件 → Value 树。
/// @note 栈式装配：容器先入栈、子值就地追加，容器闭合时整体上浮一层。嵌套深度受解析器
///       的 max_depth 限制，栈深不会超过它。
class DomBuilder : public SaxHandler {
  public:
    explicit DomBuilder(std::size_t max_depth) {
        // 按深度上限预留，但夹取上限——max_depth 是用户可配的，极端取值不该触发巨量预分配。
        stack_.reserve(max_depth < 64 ? max_depth + 1 : 64);
    }

    auto on_null() -> bool override {
        place(Value(nullptr));
        return true;
    }

    auto on_bool(bool value) -> bool override {
        place(Value(value));
        return true;
    }

    auto on_int(std::int64_t value) -> bool override {
        place(Value(value));
        return true;
    }

    auto on_uint(std::uint64_t value) -> bool override {
        place(Value(value));
        return true;
    }

    auto on_double(double value) -> bool override {
        place(Value(value));
        return true;
    }

    auto on_raw_number(std::string_view digits) -> bool override {
        place(Value::raw_number(digits));
        return true;
    }

    auto on_string(std::string_view decoded) -> bool override {
        place(Value(std::string(decoded)));
        return true;
    }

    auto on_array_start() -> bool override {
        stack_.push_back(Frame{Value::array(), {}});
        return true;
    }

    auto on_array_end(std::size_t /*count*/) -> bool override {
        close_container();
        return true;
    }

    auto on_object_start() -> bool override {
        stack_.push_back(Frame{Value::object(), {}});
        return true;
    }

    auto on_object_key(std::string_view key) -> bool override {
        stack_.back().key.assign(key);  // 必须拷贝：SAX 契约规定视图仅在回调期间有效
        return true;
    }

    auto on_object_end(std::size_t /*count*/) -> bool override {
        close_container();
        return true;
    }

    /// @brief 取走装配完成的根值（移出后本对象不可再用）。
    [[nodiscard]] auto take() -> Value { return std::move(root_); }

  private:
    struct Frame {
        Value container;
        std::string key;  ///< Object 帧：等待装配的成员键
    };

    /// @brief 把刚完成的子值装入当前容器；栈空即顶层值。
    auto place(Value v) -> void {
        if (stack_.empty()) {
            root_ = std::move(v);
            return;
        }
        Frame &frame = stack_.back();
        if (frame.container.is_array()) {
            frame.container.push_back(std::move(v));
        } else {
            frame.container.set(frame.key, std::move(v));  // 重复键后值覆盖，位置保持首次插入处
            frame.key.clear();
        }
    }

    /// @brief 弹出一层已闭合的容器，作为子值交上层装配。
    auto close_container() -> void {
        Value done = std::move(stack_.back().container);
        stack_.pop_back();
        place(std::move(done));
    }

    std::vector<Frame> stack_;
    Value root_;
};

}  // namespace

auto parse(std::string_view input, ParseOptions opts) -> Result<Value> {
    DomBuilder builder(opts.max_depth);
    auto parsed = parse_sax(input, builder, opts);
    if (!parsed.ok()) {
        return parsed.error();
    }
    return builder.take();
}

}  // namespace aurora::json
