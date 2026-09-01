#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/result.h"
#include "aurora/widget/props_io.h" // Json

namespace aurora {

class Node;
class Widget;

/// @brief Inspector 统一编程接口（AI Agent 操作 UI 树的标准入口）。
///
/// 所有查询方法委托到 inspect.h 自由函数，零新运行时开销。
/// 新增 simulate_* 交互模拟和 subscribe_changes 变化订阅能力。
///
/// @note Thread: main-thread only
/// @note Side-effects: simulate_*/set_prop/apply_patch 有副作用
class Inspector {
  public:
    // ── 树查询（委托 inspect.h，零新开销）──

    /// @brief 人类可读缩进树。
    static auto tree_text(const Node &root) -> std::string;

    /// @brief 富格式文本树（含 bounds/text/style/listeners）。
    static auto tree_rich(const Node &root) -> std::string;

    /// @brief 结构化 JSON 树（仅 type + children）。
    static auto tree_json(const Node &root) -> Json;

    /// @brief 完整 JSON 快照（type + props + children）。
    static auto tree_json_full(const Node &root) -> Json;

    /// @brief Widget 完整信息（descriptor + values）。
    static auto widget_info(const Widget &w) -> Json;

    /// @brief 按类型名查询节点。
    static auto query(std::string_view type, const Node &root) -> std::vector<Node>;

    /// @brief 按路径获取状态片段。
    static auto get_state(std::string_view path, const Node &root) -> Json;

    /// @brief 按索引路径定位节点。
    static auto find_node(const Node &root, std::string_view path) -> Node;

    // ── 属性读写 ──

    /// @brief 获取 Widget 当前属性快照（descriptor + values）。
    static auto get_prop(const Widget &w) -> Json;

    /// @brief 获取单个属性值。
    static auto get_prop_value(const Widget &w, std::string_view key) -> Json;

    /// @brief 设置单个属性值。
    static auto set_prop(Widget &w, std::string_view key, const Json &val) -> Result<void>;

    /// @brief 应用 JSON Patch 批量修改 UI 树（逐条 set_prop）。
    static auto apply_patch(Node &root, const Json &patch) -> Result<void>;

    // ── 交互模拟（新增）──

    /// @brief 模拟点击 Widget（需要事件系统支持，当前返回 GeneralNotSupported）。
    static auto simulate_click(Widget &w) -> Result<void>;

    /// @brief 模拟滚动（需要事件系统支持，当前返回 GeneralNotSupported）。
    static auto simulate_scroll(Widget &w, float dx, float dy) -> Result<void>;

    /// @brief 模拟文本输入（需要事件系统支持，当前返回 GeneralNotSupported）。
    static auto simulate_text_input(Widget &w, std::string_view text) -> Result<void>;

    // ── 组件发现 ──

    /// @brief 列出所有已注册组件的 Schema。
    static auto components() -> std::vector<Json>;

    /// @brief 获取指定组件的完整 Schema。
    static auto component_schema(std::string_view name) -> Json;

    // ── 代码生成 ──

    /// @brief 将 UI 树转换为源码。
    static auto to_code(const Node &root) -> std::string;

    // ── 验证 ──

    /// @brief 验证 UI 树，返回诊断列表。
    static auto validate(const Node &root) -> std::vector<Diagnostic>;

    // ── 变化订阅（新增）──

    using ChangeCallback = std::function<void(const Json &patch)>;

    /// @brief 订阅 UI 树变化通知。返回 subscription id。
    static auto subscribe_changes(ChangeCallback cb) -> std::size_t;

    /// @brief 取消订阅。
    static auto unsubscribe(std::size_t id) -> void;

    /// @brief 通知所有订阅者（内部调用，在 mark_needs_paint/layout 时触发）。
    static void notify_changes(const Json &patch);
};

} // namespace aurora
