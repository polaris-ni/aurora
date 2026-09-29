#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/result.h"
#include "aurora/event/event.h"
#include "aurora/widget/props_io.h"

namespace aurora {

class Node;
class Widget;

/// @brief Inspector 统一编程接口（AI Agent 操作 UI 树的标准入口）。
/// 所有查询方法委托到 inspect.h 自由函数，零新运行时开销。
/// 新增 simulate_* 交互模拟和 subscribe_changes 变化订阅能力。
///
/// @note Thread: main-thread only
/// @note Side-effects: simulate_*/set_prop/apply_patch 有副作用
class Inspector {
  public:
    // ── 树查询（委托 inspect.h，零新开销）──

    /// @brief 人类可读缩进树。
    /// @param root 树根；子节点按其 `child_nodes()` 顺序递归，故虚拟化容器的子树不入此表。
    /// @return 每个节点一行的缩进文本（内容为 `type_name`，每层两个空格，行尾 `\n`）。
    static auto tree_text(const Node &root) -> std::string;

    /// @brief 富格式文本树（含 bounds/text/style/listeners）。
    /// @param root 根节点；建议在布局/绘制之后取，否则 bounds 为空盒。
    /// @return 首行为根、其余每行一个节点的树形文本（`├─ └─` 连接符，单行内不含换行）。
    static auto tree_rich(const Node &root) -> std::string;

    /// @brief 结构化 JSON 树（仅 type + children）。
    /// @param root 树根；只经 `child_nodes()` 下降，虚拟化容器的子树不入 JSON。
    /// @return `{type, children}` 对象；叶子节点 `children` 为空数组。
    static auto tree_json(const Node &root) -> Json;

    /// @brief 完整 JSON 快照（type + props + children）。
    /// @param root 树根；子节点枚举走统一遍历，虚拟化容器的子树同样可见。
    /// @return `{type, props, children}` 对象；持焦节点另含 `focused: true`。
    static auto tree_json_full(const Node &root) -> Json;

    /// @brief Widget 完整信息（descriptor + values）。
    /// @param w 目标控件；元数据取其 `describe()`（只列 name 与属性名），值取其 `serialize_props()`。
    /// @return `{descriptor: {name, property_names}, values: {...}}` 对象。
    static auto widget_info(const Widget &w) -> Json;

    /// @brief 按类型名查询节点。
    /// @param type 与 `Widget::type_name()` 逐字符比对的类型名（区分大小写，不做前缀/通配）。
    /// @param root 检索起点；自身也参与匹配（即命中含 root）。
    /// @return 先序命中的节点表（副本）；无命中为空表。
    static auto query(std::string_view type, const Node &root) -> std::vector<Node>;

    /// @brief 按路径获取状态片段。
    /// @param path 以 `/` 分隔的键/下标序列，沿 JSON 树寻址（如 "children/0/type"）；空路径取整棵树。
    /// @param root 树根。
    /// @return 命中片段的拷贝；路径不可达时为空 Json。
    static auto get_state(std::string_view path, const Node &root) -> Json;

    /// @brief 按索引路径定位节点。
    /// @param root 树根；路径段为子节点下标序列（如 "0/2/1"），空路径即根自身。
    /// @param path 子节点下标路径；无效路径返回空 `Node`。
    /// @return 命中节点的 `Node` 副本（内部 shared_ptr 指向同一控件）。
    /// @note 只沿 `child_nodes()` 下降 ⇒ 到不了虚拟化容器（`NavigatorHost` / `LazyList` 等
    /// `child_nodes()` 恒空者）的子树；返回的 `Node` 是副本。HTTP / MCP 等按路径寻址的入口
    /// 应改用 `find_widget`。
    static auto find_node(const Node &root, std::string_view path) -> Node;

    /// @brief 按索引路径定位控件（空路径即根自身）；越界或非法路径返回 `nullptr`。
    /// 与 `find_node` 的差别：① 返回裸 `Widget *`，下降全程走统一遍历
    /// （`child_nodes()`，为空时回退 `for_each_child`），因此**能寻址虚拟化容器的子树**；
    /// ② 不构造任何 `Node` 副本，故不会因副本析构清掉兄弟节点的 `layout_parent_`。
    ///
    /// 与 `tree_json_full` 的枚举口径一致 —— 按路径寻址的入口须与本函数同源，否则
    /// 同一路径在树快照与单控件查询下会指向不同控件。
    ///
    /// @param root 遍历起点控件；空路径即其自身。
    /// @param path 索引路径（子节点在统一遍历序中的下标序列）；非法段或越界视为未命中。
    /// @return 命中控件指针（生命周期由树持有的 shared_ptr 保证）；非法路径或越界返回 `nullptr`。
    static auto find_widget(Widget &root, std::string_view path) -> Widget *;

    // ── 属性读写 ──

    /// @brief 获取 Widget 当前属性快照（descriptor + values）。
    /// @param w 目标控件。
    /// @return `{descriptor: {name, property_names}, values: {...}}` 对象，与 `widget_info` 同源。
    static auto get_prop(const Widget &w) -> Json;

    /// @brief 获取单个属性值。
    /// @param w 目标控件；属性表取 `serialize_props()` 的当前输出。
    /// @param key 属性名。
    /// @return 该属性的当前值；属性不存在时为空 Json。
    static auto get_prop_value(const Widget &w, std::string_view key) -> Json;

    /// @brief 设置单个属性值。
    /// @param w 目标控件。
    /// @param key 属性名。
    /// @param val 新值；经 `set_widget_prop` 写入并触发重绘。
    /// @return 恒为成功的空 Result（当前实现无失败路径）。
    static auto set_prop(Widget &w, std::string_view key, const Json &val) -> Result<void>;

    /// @brief 应用 JSON Patch 批量修改 UI 树（逐条 set_prop）。
    /// @param root 树根；各条目 path 相对该树寻址。
    /// @param patch JSON 数组，每项为 `{path, value}`；path 形如 "/控件路径/属性名"，末段为属性名。
    /// @return patch 非数组时返回 `GeneralNotSupported` 错误；否则逐条应用（条目非法或路径未命中则跳过）后返回成功。
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

    /// @brief 模拟拖拽：Press（控件中心）→ Move（中心 + 位移）→ Release（终点）。
    /// @param w  目标控件（文本拖选、Slider 拖动等以 Move 为有效输入的控件）。
    /// @param dx 终点相对中心的水平位移（右为正）。
    /// @param dy 终点相对中心的垂直位移（下为正）。
    /// @return 派发成功返回空 Result；中心处无命中目标返回 `GeneralNotSupported`（不派发、不改状态）。
    /// @note 与 `simulate_click` 同为目标式语义：三个事件均以控件自身为派发根、坐标相对控件顶点；
    ///       位移超出控件自身 bounds 后不再有后代可命中（运动被限制在本控件几何内），
    ///       需要跨控件拖拽时以共同祖先为目标自行合成事件。
    /// @note Side-effects: 触发 Press/Move/Release 对应的手势响应（如 Text 建立选区、Slider 改值）。
    static auto simulate_drag(Widget &w, float dx, float dy) -> Result<void>;

    /// @brief 模拟单步指针动作：以给定点为指针位置，派发一个 Press / Move / Release。
    /// @param w       派发根（坐标即相对该控件顶点）。
    /// @param position 指针位置（`w` 的局部坐标，逻辑单位）。
    /// @param action  `Press` / `Move` / `Release` 之一；其余动作返回 `GeneralNotSupported`。
    /// @return 已派发返回空 Result；`Press` 落点无命中目标返回 `GeneralNotSupported`（不派发、不改状态）。
    /// @note 与 `simulate_drag` 的「目标式」语义互补：本函数按**坐标**寻址，可落在控件的任一子区域
    ///       （Splitter 的分隔条、Scrollbar 的滑块、Slider 的某个刻度），也可由调用方分多次调用
    ///       合成连续拖拽——派发器自带指针捕获，Press 之后的 Move/Release 会持续送达按下时的目标，
    ///       因此中间步可让指针越出分隔条而拖拽不中断。
    /// @note Side-effects: 等价于真实指针的单步输入，会触发命中回调与焦点转移（仅 Press）。
    static auto simulate_pointer(Widget &w, const Point &position, MouseAction action) -> Result<void>;

    /// @brief 模拟文本输入：把控件置为焦点后向其派发文本输入事件。
    /// @param w    目标控件（须实现 `on_text_input`，如 TextInput / RichTextEdit）。
    /// @param text UTF-8 文本片段；为空时视为完成、不产生副作用。
    /// @return 目标控件消费该输入返回空 Result；未消费（如禁用态）返回 `GeneralNotSupported`。
    /// @note 成功仅表示输入已被目标接纳；是否真正落字仍取决于控件自身状态（如只读态会吞掉输入）。
    static auto simulate_text_input(Widget &w, std::string_view text) -> Result<void>;

    // ── 组件发现 ──

    /// @brief 列出所有已注册组件的 Schema。
    /// @return 全部已注册组件的完整 schema 表（含 describe 元数据与 props/children/thread）。
    static auto components() -> std::vector<Json>;

    /// @brief 获取指定组件的完整 Schema。
    /// @param name 组件类型名。
    /// @return 该组件的完整 schema；未注册类型返回空 Json 对象。
    static auto component_schema(std::string_view name) -> Json;

    // ── 代码生成 ──

    /// @brief 将 UI 树转换为源码。
    /// @param root 树根；序列化取其 `widget()`。
    /// @return 生成的源码文本。
    static auto to_code(const Node &root) -> std::string;

    // ── 验证 ──

    /// @brief 验证 UI 树，返回诊断列表。
    /// @param root 待验证的 UI 树根。
    /// @return 诊断列表：验证通过为空表，失败时含一条 Error 级诊断（携错误码与出错位置）。
    static auto validate(const Node &root) -> std::vector<Diagnostic>;

    // ── 变化订阅 ──

    /// @brief 变化通知的回调类型：接收描述本次 UI 树变化的 JSON Patch。
    using ChangeCallback = std::function<void(const Json &patch)>;

    /// @brief 订阅 UI 树变化通知。
    /// @param cb 变化回调；此后每次变化通知都会调用它，直至注销。
    /// @return 订阅 id（单调递增分配，供 `unsubscribe` 注销）。
    static auto subscribe_changes(ChangeCallback cb) -> std::size_t;

    /// @brief 取消订阅。
    /// @param id `subscribe_changes` 返回的订阅 id；未注册的 id 无效果。
    static auto unsubscribe(std::size_t id) -> void;

    /// @brief 通知所有订阅者（内部调用，在 mark_needs_paint/layout 时触发）。
    /// @param patch 描述本次 UI 树变化的 JSON Patch；原样转发给每个已注册回调。
    static void notify_changes(const Json &patch);
};

}  // namespace aurora
