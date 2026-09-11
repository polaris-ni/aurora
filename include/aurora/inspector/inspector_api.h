#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/result.h"
#include "aurora/widget/props_io.h"

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

    // ── 交互模拟 ──
    //
    // 三者均为「目标式」语义：以传入控件为派发根与坐标原点、指针取该控件中心，
    // 合成事件经 `EventDispatcher` 走真实命中测试 + 冒泡派发路径。因此不依赖控件
    // 在整棵树中的绝对位置，单独构造并布局（`LayoutEngine::layout`）后即可复现；
    // 代价是事件不会冒泡到该控件的祖先——要驱动祖先的响应须以祖先为目标。
    //
    // 目标不可命中时返回 `GeneralNotSupported`（含原因），便于调用方区分「已派发」
    // 与「无事发生」。

    /// @brief 模拟点击 Widget：以控件 bounds 中心为指针位置，派发 PointerPress + Release。
    /// @param w 目标控件；须为点击目标（`wants_click()`，如 Button / Clickable 修饰）或
    ///          中心处有可命中的后代。
    /// @return 派发成功返回空 Result；中心处无命中目标返回 `GeneralNotSupported`（不派发、不改状态）。
    /// @note Side-effects: 触发控件的点击回调（`on_click` / Clickable），并把焦点交给
    ///       命中链上最近的可获焦控件。
    static auto simulate_click(Widget &w) -> Result<void>;

    /// @brief 模拟滚动：以控件 bounds 中心为指针位置，派发滚轮事件（位移 dx/dy）。
    /// @param w    目标控件（通常是 Scroll / LazyList / GridView 等滚动容器）。
    /// @param dx   水平滚动增量（右为正）。
    /// @param dy   垂直滚动增量（上为正）。
    /// @return 派发成功返回空 Result；中心处无命中目标返回 `GeneralNotSupported`（不派发、不改状态）。
    static auto simulate_scroll(Widget &w, float dx, float dy) -> Result<void>;

    /// @brief 模拟文本输入：把控件置为焦点后向其派发文本输入事件。
    /// @param w    目标控件（须实现 `on_text_input`，如 TextInput / RichTextEdit）。
    /// @param text UTF-8 文本片段；为空时视为完成、不产生副作用。
    /// @return 目标控件消费该输入返回空 Result；未消费（如禁用态）返回 `GeneralNotSupported`。
    /// @note 成功仅表示输入已被目标接纳；是否真正落字仍取决于控件自身状态（如只读态会吞掉输入）。
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

}  // namespace aurora
