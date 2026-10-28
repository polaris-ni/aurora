#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "aurora/state/detail/reactor.h"

/// @brief 内部实现细节：状态来源与终端的全局登记表，不属于公共 API 契约面。
namespace aurora::detail {

/// @brief 登记一个来源节点（来源构造完成后调用，供 `StateGraph` 枚举存活来源）。
///
/// 注册表只持弱引用，节点析构即自然失效；失效条目在后续查询或后续登记时按摊还代价
/// 压缩，因此长期运行也不会随来源创建/销毁单调增长。
/// @param node 新创建的来源节点（共享句柄，用于取得弱引用）。
auto register_source(const std::shared_ptr<SourceNode> &node) -> void;

/// @brief 登记一个终端运行记录（订阅或副作用建立后调用）。
/// @param record 新创建的运行记录（共享句柄，用于取得弱引用）。
auto register_terminal(const std::shared_ptr<RunRecord> &record) -> void;

/// @brief 当前存活的来源节点快照（已剔除失效条目）。
/// @return 存活来源的共享句柄列表，顺序为登记顺序。
[[nodiscard]] auto live_sources() -> std::vector<std::shared_ptr<SourceNode>>;

/// @brief 当前存活的终端运行记录快照（已剔除失效条目）。
/// @return 存活终端的共享句柄列表，顺序为登记顺序。
[[nodiscard]] auto live_terminals() -> std::vector<std::shared_ptr<RunRecord>>;

/// @brief 创建值来源并登记：构造体内无 `shared_from_this`，故登记只能在构造之后。
/// @tparam T 值类型。
/// @tparam Args 来源构造实参类型。
/// @param args 转发给 `Source<T>` 构造的实参。
/// @return 新建来源的共享句柄。
template <typename T, typename... Args>
[[nodiscard]] auto make_source(Args &&...args) -> std::shared_ptr<Source<T>> {
    auto node = std::make_shared<Source<T>>(std::forward<Args>(args)...);
    register_source(node);
    return node;
}

/// @brief 创建派生来源并登记。
/// @tparam T 派生值类型。
/// @tparam Args 派生构造实参类型（计算体与等价判定）。
/// @param args 转发给 `Derived<T>` 构造的实参。
/// @return 新建派生节点的共享句柄。
template <typename T, typename... Args>
[[nodiscard]] auto make_derived(Args &&...args) -> std::shared_ptr<Derived<T>> {
    auto node = std::make_shared<Derived<T>>(std::forward<Args>(args)...);
    register_source(node);
    return node;
}

/// @brief 创建终端运行记录并登记（订阅、副作用、控件信号接线统一经此建立）。
/// @tparam F 回调类型。
/// @param fn 回调体，执行期间的读取成为依赖。
/// @return 新建运行记录的共享句柄。
template <typename F>
[[nodiscard]] auto make_record(F &&fn) -> std::shared_ptr<RunRecord> {
    auto record = std::make_shared<RunRecord>(std::forward<F>(fn));
    register_terminal(record);
    return record;
}

}  // namespace aurora::detail
