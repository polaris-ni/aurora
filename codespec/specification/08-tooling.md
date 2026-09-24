# 序列化、检查器与工具链（serialization / inspector / tooling / log）

> 覆盖 `widget/serialization.h`、`widget/codegen.h`、`widget/yaml.h`、`widget/inspect.h`、`inspector/`、`core/log.h`、`debug/`、仓库私有 `tests/support/test_helpers.h` 与 `tools/`。
> 本文件是 UI 树线格式、差分补丁、代码生成、运行时检查、MCP / CLI / LSP 工具链与日志通道的**唯一权威**。
> `WidgetDescriptor` / `PropDescriptor` 结构见 [`04-widget.md`](04-widget.md) §2.1；`Result` 与 `Error` 见 [`01-core.md`](01-core.md) §3。

---

## 1 模块范围

| 关注点 | 头文件 / 目标 |
|:---|:---|
| 序列化与补丁 | `widget/serialization.h` |
| 代码生成 | `widget/codegen.h` |
| YAML 输出 | `widget/yaml.h` |
| 控件树检查 | `widget/inspect.h` |
| 检查面板与远程服务 | `widget/inspector_panel.h`、`inspector/inspector_server.h`、`inspector/inspector_api.h` |
| 日志 | `core/log.h`（通道契约见 §9） |
| 测试原语 | 根级 `test_helpers.h` |
| 工具可执行文件 | `aurora_mcp`、`aurora_cli`、`aurora_lsp`、`gen_api_tools`（完整清单与生成器 / 校验器 / 基准见 §7.4；AI 兼容性验证见 `itest_ai_compat` 集成测试，`ctest -R itest_ai_compat`） |

---

## 2 序列化

### 2.1 线格式

节点形态固定为：

```json
{ "type": "<type_name>", "props": { ... }, "children": [ ... ] }
```

`children` 为空时不输出。**属性一律位于 `props` 子对象下**，因此补丁 path 形如 `/children/0/props/show`。

`Json` 类型是 `nlohmann::json` 的别名，定义于 `widget/props_io.h`。

部分控件的序列化 `type` 名与 C++ 类名不同（`Image` → `ImageView` / `ImageViewProps`）。

### 2.2 API

本表函数除 `list_all_schemas` 外均位于 `aurora::serialization` 命名空间（**不是** `aurora`）。`list_all_schemas` 声明在 `serialization` 块之外，属 `aurora` 命名空间（`serialization.h`，与 `list_all_components` / `describe_component` / `search_components` 同区），调用写作 `au::list_all_schemas()`。

| 函数 | 签名 | 位置 |
|:---|:---|:---|
| `to_json` | `[[nodiscard]] auto to_json(const Widget &w) -> Json` | `serialization.h` |
| `from_json` | `[[nodiscard]] auto from_json(const Json &j) -> Result<std::shared_ptr<Widget>>` | `serialization.h` |
| `diff` | `[[nodiscard]] auto diff(const Json &a, const Json &b) -> std::vector<JsonPatchOp>` | `serialization.h` |
| `diff_into` | `auto diff_into(const Json &a, const Json &b, const std::string &path, std::vector<JsonPatchOp> &out) -> void` | `serialization.h` |
| `apply_patch` | `auto apply_patch(Json &target, const std::vector<JsonPatchOp> &patch) -> void` | `serialization.h` |
| `to_yaml` | `[[nodiscard]] auto to_yaml(const Widget &w) -> std::string` | `serialization.h` |
| `component_schema` | `[[nodiscard]] auto component_schema(const std::string &name) -> Json` | `serialization.h` |
| `list_all_schemas` | `[[nodiscard]] auto list_all_schemas() -> std::vector<Json>` | `serialization.h` |

`to_yaml` 有 `Widget` 与 `Json` 两个重载；**YAML 只有输出方向，无 `from_yaml`**。

```cpp
au::Json json = au::serialization::to_json(my_tree);          // 返回 au::Json（非 std::string）
auto restored = au::serialization::from_json(json);           // 返回 Result，使用前判 ok()
if (!restored.ok()) { /* restored.error() 含 code / message / hint */ }

auto patch = au::serialization::diff(before_json, after_json);
// [{op:"replace", path:"/children/1/props/label", value:"Updated"}]
au::serialization::apply_patch(json, patch);                  // 第一参数是 au::Json（非 widget 树）
```

**失败语义**：`from_json` 返回 `Result`，失败经 `Result::Error` 表达，**不抛异常、不产出半合法的树**。节点缺 `type` 或类型未注册即失败；嵌套深度超过 `AURORA_DEFAULT_MAX_WIDGET_DEPTH` 返回 `widget-depth-exceeded`。

组件通过 `serialize_props` / `deserialize_props` 虚钩子声明可序列化属性。

### 2.3 组件工厂注册表

`serialization::WidgetRegistry`（`serialization.h`）:

| 方法 | 签名 |
|:---|:---|
| `instance()` | 静态，返回 `WidgetRegistry&`（`serialization.h`） |
| `register_factory(type, WidgetFactory)` | 注册工厂（`serialization.h`） |
| `make(type, props)` | `[[nodiscard]] auto make(const std::string &type, const Json &props) const -> Result<std::shared_ptr<Widget>>`（`serialization.h`） |
| `list_types()` | `[[nodiscard]] auto list_types() const -> std::vector<std::string>`（`serialization.h`） |

`WidgetFactory = std::function<Result<std::shared_ptr<Widget>>(const Json&)>`（`serialization.h`）。

### 2.4 差分补丁协议

```cpp
struct JsonPatchOp {
    std::string op;    // "replace" | "add" | "remove"
    std::string path;  // JSON Pointer，如 "/children/0/props/show"
    Json value;        // remove 时为空
};
```

协议是 **RFC6902 的子集**，仅支持 `replace` / `add` / `remove` 三类操作，以 JSON Pointer 定位。

**线格式**：`diff` 的返回值是 `std::vector<JsonPatchOp>`，序列化为**一个裸 JSON 数组**，无信封包装。

```json
[
  { "op": "replace", "path": "/children/1/props/label", "value": "Updated" },
  { "op": "add",     "path": "/children/2", "value": { "type": "Text", "props": { "content": "New" } } },
  { "op": "remove",  "path": "/children/0" }
]
```

`apply_patch` 的第一参数是 `au::Json`（**非** widget 树），因此 AI 可只发部分 UI 树的 patch 做增量修改，不必重传整树。

### 2.5 代码生成

`aurora::serialization::to_code`（`widget/codegen.h`）把结构快照还原为 C++ 源码，三个重载：

| 重载 | 位置 |
|:---|:---|
| `to_code(const Json &node, int indent = 0) -> std::string` | `codegen.h` |
| `to_code(const Json &node, CodeStyle style, int indent = 0) -> std::string` | `codegen.h` |
| `to_code(const Widget &w) -> std::string` | `codegen.h` |

```cpp
enum class CodeStyle : std::uint8_t { Fluent, StepByStep, DesignatedInit };   // codegen.h
```

| 枚举值 | 生成形态 |
|:---|:---|
| `Fluent`（默认） | 扁平容器 + `au::Type(au::TypeProps{ … })` 叶形式（多子容器直接罗列子项） |
| `StepByStep` | `auto w = au::Type(au::TypeProps{}); w.prop = val;` 分步赋值形式 |
| `DesignatedInit` | 统一 `au::TypeProps{ .prop = val, .children = { … } }` 指定初始化器形式（仅 `*Props` 聚合支持） |

`emit_props` 按 **JSON 值形态 + 键名**分派（`codegen.h` 的 `emit_prop_value` / `enum_type_for_key`），覆盖基础属性类型：`bool` / `int` / `float` / `double` / `string` / `LocalizedString`（以字符串形态命中 string 分支）/ `Color` / `Length` / `EdgeInsets` / 枚举（经 `enum_type_for_key` 登记表，另含 `font_weight`、`text_decoration` 专用分支）。

枚举登记表**同时覆盖 `to_json` 真实产出的属性名**（`alignment`、`overflow`、`side`、`position`、`decoration`、`text_align`、`font_style`、`main_axis_*`、`cross_axis_alignment` 等），因此真实 UI 树的这些属性可还原为 `Enum::Value` 表达式而非裸字符串。两点例外与一处现状：

- `fit` 与 `orientation` **无法按键名消歧**（`fit` 在 `Stack` 上是 `StackFit`、在 `VideoPlayer` 上是 `BoxFit`；`orientation` 在 `Divider` 上是 `Orientation`、在 `Splitter` 上是 `SplitterOrientation`，取值集还完全同名）。登记表刻意不收录二者，它们仍输出字符串字面量；要正确还原须把 `prop_descriptors[].type` 透传进 `emit_prop_value` 按声明类型分派。
- `Image` / `FlexWeight` / `Flex` / `Json` 这 4 类**暂无专用分派分支**：其序列化形态若未命中任何基础分支，`emit_prop_value` 产出 `/* unknown */`，`emit_props` 随即**静默跳过该属性**——既不报错也不告警，生成的代码中该属性直接缺失。
- 登记表按「键名」而非「类型」工作，故新增枚举属性时必须同步登记真实键名，否则该属性静默退化为字符串（`tests/integration/itest_to_code.cpp` 的 `codegen_enum_keys_emit_enum_expressions` / `codegen_ambiguous_enum_keys_stay_as_strings` 两例守护此点）。

多子扁平容器（`Column` / `Row` / `Stack` / `Grid` / `Scroll` / `Card`）走免 `Props` 包裹的罗列形式。

```cpp
std::string code = au::serialization::to_code(json, au::serialization::CodeStyle::Fluent);
```

---

### 2.6 自然语言 → UI

分三层，**红线是核心零网络依赖**：本库绝不发起 HTTP 请求，调用 LLM 的是外部 Agent。

| 层 | 符号（`app/`） | 作用 |
|:---|:---|:---|
| 确定性生成 | `generate_ui(description) -> Result<Json>`<br>`validate_generate_ui(description) -> bool`（`generate_ui.h`） | 关键词匹配（**不依赖 LLM**）。覆盖面由 `serialization::list_all_components()` 派生 —— 新增控件无需改代码即可被识别；另有口语别名表。文案属性写**控件真实读的键**（`Text`→`content`、`Button`→`label`），不再硬编码 `text` |
| prompt 投影 | `build_ui_prompt(types, opt) -> std::string`<br>`ui_prompt_for(description) -> std::string`（`ui_prompt.h`） | 把 schema 压缩成一段紧凑文本供外部 LLM 消费。`UiPromptOptions{include_children_policy, include_defaults, include_examples, max_types}`。`ui_prompt_for` 先用关键词探测相关类型、再补上 `Stack`/`Column`/`Row`/`Text`，避免把全量 schema 塞进上下文 |
| 自修复环 | `repair_ui_tree(tree) -> Json`<br>`generate_ui_repair(description, llm = {}, max_attempts = 3) -> UiRepairResult`（`ui_prompt.h`） | 机修 + 重试。**机修优先**：未知类型（大小写/拼写）、缺属性（按 `default_props` 回填）、`children_policy=none` 却带子节点（丢弃）三类可确定性修好，不浪费一次 LLM 往返。修不了的才把 `ValidationError{path,message,suggestion}` 拼进 prompt 让 LLM 重来 |

`GenerateUiFn = std::function<Json(const std::string &prompt, const std::vector<ValidationError> &errors)>`
由调用方注入；**不注入时只用关键词生成 + 机修**，仍然自洽可测。`UiRepairResult` 带逐轮 `history`
（每轮的产物 / 错误 / 是否机修好），供人或 AI 自省「为什么没修好」。

MCP 侧对应三个工具：`generate_ui` / `build_ui_prompt` / `repair_tree`。

---

### 2.7 JSON 热重载

`HotReload`（`app/hot_reload.h`）：监视 JSON 变化 → `from_json` 重建整棵树，并保留上一棵树里源文件**未显式声明**的属性值。

- **状态保留按「树路径」而非 id**：`Widget::id` 不经 JSON 往返（`serialize_props` 不含 id、`from_json` 也不读），故按 id 匹配在热重载下根本对不上 —— 树路径（`"0"` / `"1/2"`，与 `Inspector::find_node` 同格式）只要结构没变就稳定，且无需给 `Widget` 增加新虚接口。
- **只回填标量**（string / number / boolean）：回调、订阅者、复杂对象无法经 JSON 表达。
- **源文件显式声明的值永远优先**：热重载不该把用户刚改的 JSON 覆盖回去。
- 只对**新旧都存在**的路径回填；结构重排后宁可少恢复几项也不猜。仅保留标量属性，不保留订阅者。
- 逐节点状态经 `Inspector::set_prop` 落回活控件。

---

## 3 控件树检查

UI 树 dump 统一以 `widget/inspect.h` 内的**自由函数**提供，**不提供 `Widget::dump()` 成员方法**——富格式需求由 `dump_tree_rich` 覆盖，避免为每个控件重复实现 dump 逻辑。

| 函数 | 说明 |
|:---|:---|
| `dump_tree(root)` | 人类可读缩进树 |
| `dump_tree_rich(root, depth = 0, tree_chars = true)` | 富格式树，含 `#id` / bounds / visible / text / style / listeners，以 `├─ └─ │` 连接 |
| `dump_tree_json*` / `dump_tree_json_full(root) -> Json` | JSON 快照；`dump_tree_json_full` 含属性（每节点 type / props / children） |
| `widget_tree_to_items(root) -> std::vector<TreeItem>` | Widget 树 → TreeItem 树（供 `TreeView` 消费） |
| `find_node_by_path(root, path) -> Node` | 按索引路径定位节点（如 `"0/2/1"`）。只沿 `child_nodes()` 下降 ⇒ 到不了虚拟化容器的子树 |
| `find_widget_by_path(root, path) -> Widget *` | 同上，但返回裸控件指针、下降全程走**统一子节点遍历** ⇒ 可跨越虚拟化容器；越界或非法路径段返回 `nullptr` |
| `get_widget_props(w) -> Json` | 获取 Widget 属性快照（`describe` + `serialize_props`） |
| `set_widget_prop(w, key, value)` | 单属性回写（经 `deserialize_props`） |
| `collect_widget_boxes(root) -> std::vector<WidgetBox>` | 把控件树拍平成「布局盒表」（`{path, type, bounds}`）。**输出顺序即先序**，是差异归因 tie-break 依赖的契约 |
| `diff_trees(old, new) -> std::vector<WidgetPatchOp>` | 逐节点产出把旧树变成新树的**属性补丁**（最小 patch，非整树替换）；只比较公共前缀，不越界 |
| `trees_differ_structurally(old, new) -> bool` | 是否存在属性补丁**表达不了**的结构差异（类型变了 / 某层子节点数不同）；为真时调用方须整树替换 |

`dump_tree_rich` 输出形态：

```text
Column#root { bounds:[0,0,640,480]; visible:true; listeners:[on_click] }
├─ Text#title { bounds:[20,20,200,24]; text:"Hello"; style:{font_size:24} }
└─ Button#ok  { bounds:[20,52,88,36]; text:"Click"; listeners:[on_click] }
```

`Node` 标识由 `Widget::set_id(std::string_view)` / `id()` 提供，`dump_tree_rich` 经 `#id` 渲染。

**子节点枚举与路径寻址必须同源。** 虚拟化容器（`NavigatorHost`、`LazyList`、`GridView`、`TransitionLayer` 等）把子节点存在 `Node` 之外的私有表中，按约定**不覆写** `child_nodes()` —— 它们没有可交出的 `Node`，只经 `for_each_child` 暴露子树（`widget/a11y_tree.h` 采用同款兜底）。因此树快照的枚举（`dump_tree_json_full`）与按路径的寻址统一走 `for_each_child_unified`：`child_nodes()` 非空则用其一，为空则回退 `for_each_child`。两处若取不同遍历源，同一路径在树快照与单控件查询下会指向不同控件 —— 这是本模块的核心不变量，新增遍历相关能力时须一并遵守。

`find_node_by_path` 只沿 `child_nodes()` 下降，故**到不了**这类容器的子树；且它会把某一层的 `child_nodes()` 拷进临时容器，副本析构会清掉该层**兄弟节点**的 `layout_parent_`，脏标记传播随之断裂。需要跨越虚拟化容器、或不想引入该副作用（如 HTTP / MCP 这类按路径寻址的入口）时，用 `find_widget_by_path`。

`aurora::Inspector`（`inspector/inspector_api.h`，实现 `src/aurora/inspector/inspector_api.cpp`）是操作 UI 树的统一编程门面：全静态方法、仅主线程，各方法委托上表自由函数或组件注册表，无新增运行时开销。除树导出（`tree_text` / `tree_rich` / `tree_json` / `tree_json_full`）外，还提供：

| 能力 | 成员 | 说明 |
|:---|:---|:---|
| 节点查询 | `query(type, root)` / `get_state(path, root)` / `find_node(root, path)` / `find_widget(root, path)` / `widget_info(w)` | 按类型名检索、按路径取状态片段、按索引路径定位节点（`find_node` 返回 `Node` 副本、只走 `child_nodes()`；`find_widget` 返回裸指针、走统一遍历，可跨越虚拟化容器，两条路径的取舍见 §3）、Widget 完整信息 |
| 属性读写 | `get_prop(w)` / `get_prop_value(w, key)` / `set_prop(w, key, val)` / `apply_patch(root, patch)` | 单属性回写返回 `Result<void>`；`apply_patch` 把 JSON Patch 逐条经 `set_prop` 应用到树 |
| 交互模拟 | `simulate_click(w)` / `simulate_scroll(w, dx, dy)` / `simulate_text_input(w, text)` | 合成事件经 `EventDispatcher` 走真实命中测试 + 冒泡派发；派发根与坐标原点均为 `w` 自身、指针取 `w` 中心，故不依赖控件在树中的绝对位置（无需先绘制，但目标须已布局——未布局时尺寸为零、中心退化为自身原点）。目标不可命中时返回 `GeneralNotSupported` 且不派发、不改状态 |
| 组件发现 | `components()` / `component_schema(name)` | 已注册组件 schema 列表 / 单组件 schema |
| 代码生成 | `to_code(root)` | UI 树 → 源码（转发 §2.5） |
| 验证 | `validate(root) -> std::vector<Diagnostic>` | 整树验证（`inspector_api.h`） |
| 变化订阅 | `subscribe_changes(cb)` / `unsubscribe(id)` / `notify_changes(patch)` | `mark_needs_paint` / 布局标脏时向订阅者广播补丁，返回订阅 id |

---

## 4 InspectorPanel

`InspectorPanel`（`widget/inspector_panel.h`）是左右分栏的检查器控件：左侧树形浏览器展示 Widget 层级，右侧属性面板展示选中 Widget 的类型与属性值。支持分隔条拖拽调整比例、运行时属性回写。

| 方法 | 说明 |
|:---|:---|
| `InspectorPanel(root_getter, ratio)` | 构造：接受目标树获取函数 + 左侧树占比 |
| `set_root(getter)` | 设置 / 更新目标树 |
| `refresh()` | 刷新树映射与属性面板 |
| `on_select_widget` | 选中 Widget 回调 |
| `selected_widget()` | 当前选中 Widget 指针 |
| `current_props()` | 属性名值对列表 |
| `export_code()` | 将当前 widget 树导出为 C++ 源码字符串 |
| `on_export_code` | 「Export Code」按钮点击回调，参数为生成的代码字符串 |

`export_code()` + 「Export Code」按钮 + `on_export_code` 回调实现「Inspector → 代码」闭环。

---

## 5 InspectorServer（远程 HTTP）

`InspectorServer`（`inspector/inspector_server.h`）是 localhost-only HTTP 服务器，暴露 REST 端点供外部 Inspector 工具远程访问运行时 widget 树。跨平台（Windows `ws2_32` / POSIX `pthread`），CMake 开关 `AURORA_BUILD_INSPECTOR_SERVER`（默认 OFF）。

```cpp
InspectorServer server{ []() -> au::Node { return build_ui(); } };
server.start(6280);   // 启动 HTTP 服务器（默认端口 6280），后台线程运行
server.is_running();
server.port();
server.stop();        // 停止并 join 工作线程
```

| 方法 | 说明 |
|:---|:---|
| `InspectorServer(root_getter)` | 构造：接受返回 `Node` 的函数 |
| `start(port = 6280) -> bool` | 启动，后台工作线程运行，成功返回 `true` |
| `stop()` | 停止服务器并 join 工作线程 |
| `is_running() -> bool` | 查询是否运行中 |
| `port() -> uint16_t` | 返回监听端口（未启动返回 0） |
| `set_surface_getter(std::function<Surface*()>)` | 注入 Surface 获取器：调试端点（`snapshot` / `state`）借此访问运行时 Surface；可选，未设置时这两类端点返回 400。`pick` 不依赖（未设置时以根控件尺寸为命中范围） |
| `set_window_tree_getter(std::function<Node(std::uint32_t)>)` | 注入「按窗口 id 取树根」获取器（多窗口）：`GET /api/tree?window=<id>` 借此返回指定窗口的树；可选，未设置时带 `window` 参数返回 400。回调经主线程 marshal 执行（与 `Surface` 的 main-thread-only 约束一致）；无效 id 应返回空 `Node`，路由层据此回 404 |

### 5.1 REST 端点

| 方法 | 路径 | 说明 |
|:---|:---|:---|
| GET | `/api/tree` | 完整 widget 树 JSON；`?window=<id>` 取指定窗口树（需 `set_window_tree_getter`），无效 id 回 404、未注册 getter 时带 `window` 参数回 400 |
| GET | `/api/widget/{path}` | 单 widget 属性 JSON（`path` 为索引路径如 `0/1/2`） |
| PUT | `/api/widget/{path}/{prop}` | 回写指定属性（请求体为 JSON 值） |
| POST | `/api/patch` | **最小属性补丁**：请求体为 JSON **数组**，每项 `{path, value}`，`path` 形如 `/1/content`（最后一段是属性名，其余为索引路径，根为空）。一次请求批量回写，交由 `Inspector::apply_patch`；非数组返回 400，属性写失败返回 400 并带原因。**只覆盖属性**，结构性增删无法表达（见 `trees_differ_structurally`） |
| GET | `/api/components` | 全部已注册组件 schema 列表 |
| GET | `/api/yaml` | 当前 widget 树的 YAML 格式字符串 |
| POST | `/api/to_code` | UI 树 → C++ 代码。请求体可含 `style` 参数：`0`=Fluent、`1`=StepByStep、`2`=DesignatedInit；`style` 存在但非整数返回 400，越界整数回退 Fluent |
| POST | `/api/input/{click\|scroll\|text}` | 交互模拟：以 `path` 命中的控件为派发根与坐标原点（指针取该控件中心）合成事件，经 `EventDispatcher` 走真实命中测试 + 冒泡派发。请求体须为对象且 `path` 为字符串（空串=树根）；`scroll` 另取数值 `dx`/`dy`（缺省 0），`text` 另取字符串 `text`。经主线程 marshal 执行，成功返回 `{status:"ok", action, widget_path}`；路径不存在 404、目标存在但不可派发 400、字段类型不符 400、方法非 POST 405 |

> `/api/input/*` 为「目标式」语义：落点取目标控件中心，故目标须已布局（未布局时尺寸为零、中心退化为自身原点）。失败（路径不存在 / 不可派发 / 参数不符）一律在派发前返回，**不改变任何控件状态**。滚动只派发事件，偏移量不在响应里（控件虽各自序列化 `offset` / `scroll_offset`，但响应体不回传），需要读回偏移请读控件属性或写 C++ 测试。

### 5.2 调试端点

以下端点需先 `set_surface_getter`（除纯 JSON 状态类外）；所有 Surface / 树 / 全局状态读取经**主线程 marshal**（`marshal_get<T>` 复用 `aurora::detail::main_poster`，无事件循环时直接执行）后返回，与 `Surface` 的 main-thread-only 约束一致。

| 方法 | 路径 | 返回 | 说明 |
|:---|:---|:---|:---|
| GET | `/api/debug/state` | `application/json` | `aurora::debug::surface_state`（Release 返回 `available:false`） |
| GET | `/api/debug/snapshot?source=fb\|win` | `image/png` | `aurora::debug::capture` 写临时 PNG 后返回字节；`source=win` 为真实屏幕窗口（Headless / Wayland unsupported）；Release 返回 500 |
| GET | `/api/debug/perf` | `application/json` | `aurora::debug::perf_snapshot` |
| GET | `/api/debug/timeline` | `application/json` | `aurora::debug::frame_phase_timeline` |
| GET | `/api/debug/diagnostics` | `application/json` | `aurora::debug::diagnostics` |
| GET | `/api/debug/why` | `application/json` | `aurora::debug::why_trace`（含 `propagated` 根因 / 传播区分） |
| GET | `/api/debug/tree` | `application/json` | `aurora::debug::widget_tree`（区别于基础 `/api/tree`） |
| GET | `/api/debug/pick?x=&y=` | `application/json` | `aurora::debug::widget_picker`：`{ hit, chain:[{type_name,bounds}] }`；坐标取窗口逻辑 dp；Release 返回 `{hit:false,chain:[]}` |
| POST | `/api/debug/flags` | `application/json` | 请求体为 `DebugPaintFlags` 子集 JSON，调用 `aurora::debug::set_flags` 实时开关叠层；返回 `{status:"ok", flags:{...}}`。请求体须为对象且字段值须为 boolean（类型不符返回 400；字段缺省保持默认 false） |

### 5.3 线程安全

`InspectorServer` 内部以 `std::mutex` 保护 widget 树访问，`root_getter` 回调在 HTTP 工作线程中被调用。使用者应确保 `root_getter` 返回的 `Node` 是线程安全的（如每次返回新树，或在回调内加锁）。

调试门面的完整能力见 [`06-app-platform.md`](06-app-platform.md) §11。

### 5.4 MCP 如何连到运行中的应用

`aurora_mcp` 是被 AI 客户端 spawn 的**独立进程**，其离线工具（以 `tree` 入参）在自身进程内临时建树，
**与运行中的应用无关**。要操作**正在运行**的 UI，走 `live_*` 工具族 —— 它们是本 HTTP 服务的客户端：

- `live_tree` / `live_widget_get` / `live_widget_set` / `live_patch` / `live_simulate`，分别映射到 §5.1 表的
  `GET /api/tree` / `GET /api/widget/{path}` / `PUT /api/widget/{path}/{prop}` / `POST /api/patch` / `POST /api/input/*`。
- **会话发现按「方案 ③」（不落进程注册表文件）**：优先级为 ① 工具入参 `session`（`"6280"` 或 `"127.0.0.1:6280"`）
  ② 环境变量 `AURORA_INSPECTOR_PORT` ③ 默认 `6280`（与 `InspectorServer::start()` 默认值一致）。
  主机**恒被 pin 到回环**：客户端不做 DNS，只认 `127.0.0.1` / `localhost` / `::1`，且一律连到 `127.0.0.1`。
- HTTP 客户端只落在 `tools/servers/inspector_client.h`（**不进 `include/` / `src/`**），故不改变核心的零依赖承诺。
- 前提：应用需自己 opt-in 启动 `InspectorServer`（CMake 开关 `AURORA_BUILD_INSPECTOR_SERVER`）；
  未启动时 `live_*` 返回传输层错误（连不上）而非空结果。

`InspectorServer::start(0)` 可由系统分配临时端口（`port()` 读回实际值），但 MCP 侧不做端口扫描
—— 需要临时端口时请自行经 `session` 入参或环境变量告知。

---

## 6 自描述发现 API

| API | 说明 |
|:---|:---|
| `component_schema(type)` | 返回完整 JSON schema，含 `prop_descriptors`、`events`、`children_policy`、`examples` 增强字段（向后兼容原有 `props` / `default_props`） |
| `list_all_schemas()` | 返回所有已注册组件的完整 schema 列表（`std::vector<Json>`） |
| `describe_component(type)` | 同 `component_schema`，公共别名 |
| `search_components(keyword)` | 按名称模糊搜索已注册组件 |
| `list_all_components()` | 返回 `std::vector<std::string>`，仅组件类型名列表 |

> `list_all_components()` 与 `list_all_schemas()` 是两个**真实且不同**的函数：`list_all_components()` 返回类型名列表（供 MCP `list_components` 消费），`list_all_schemas()` 返回完整 schema（供 MCP `get_schema` 消费）。二者并存。

**`children_policy` 语义**

| 值 | 含义 | 典型控件 |
|:---|:---|:---|
| `"none"` | 叶控件，不接受子节点 | `Button`、`Text`、`TextInput`、`Checkbox` |
| `"single"` | 单子容器 | `Scroll`、`Show` |
| `"multiple"` | 多子容器 | `Column`、`Row`、`Stack`、`Grid` |

`descriptor_to_json(WidgetDescriptor)` / `descriptor_to_json(PropDescriptor)` 输出标准 JSON，供 `gen_api_tools` 消费生成 `aurora_api.json`。

---

## 7 工具链

### 7.1 MCP Server（`aurora_mcp`）

stdio JSON-RPC 2.0。传输格式：`Content-Length: <N>\r\n\r\n<JSON-RPC 2.0 body>`。协议方法：`initialize` / `tools/list` / `tools/call` / `ping`。

| MCP Tool | 输入 | 输出 | 消费的库 API |
|:---|:---|:---|:---|
| `list_components` | 无 | 组件类型名列表 | `list_all_components()` |
| `describe_component` | `{name}` | 完整 schema JSON | `describe_component(name)` |
| `search_components` | `{query}` | 匹配组件列表 | `search_components(query)` |
| `validate_tree` | `{tree}` | 校验结果 | `from_json()` + `validate()` |
| `validate_ui` | `{tree}` | 结构化诊断 | `validate_ui_tree_json()` |
| `render_snapshot` | `{tree, width?, height?}` | 逻辑快照 JSON | `render_to_logical_snapshot()` |
| `render_png` | `{tree, width?, height?, path?}` | PNG 文件路径 | `render_to_png()` |
| `to_code` | `{tree, style?}` | C++ 代码 | `to_code()` |
| `to_yaml` | `{tree}` | YAML 格式字符串 | `serialization::to_yaml(Json)` |
| `get_schema` | 无 | 完整 API schema | `list_all_schemas()` + enums |
| `simulate_interaction` | `{tree, path, action, dx?, dy?, text?, width?, height?}` | 目标控件属性 + 交互后逻辑快照 | `from_json()` + `render_to_logical_snapshot()`（派发前布局）+ `find_node()` + `simulate_click/scroll/text_input()` + `get_prop()` |
| `list_commands` | `{commands, query?, limit?, include_disabled?}` | 过滤排序后的命令描述符 + 计数 | `command_fuzzy_score()`（与命令面板同一打分与排序） |
| `invoke_command` | `{commands, id}` | 命中信息与可调用性 `status` | 描述符解析 + 启用 / 可调用判定（**不执行**宿主动作） |

> `list_commands` / `invoke_command` 是**无状态**工具：命令描述符由调用方随请求传入（宿主 `CommandRegistry::to_json()` 的产物，接受 `{"commands":[…]}` 信封或裸数组），服务器不持有运行中的应用状态。`list_commands` 复用库的 `command_fuzzy_score()` 与「得分降序、标题升序」排序，故 AI 侧检索次序与用户看到的命令面板一致。`invoke_command` **只解析与校验**——`status` 取 `invocable` / `not-found` / `disabled` / `not-invocable`，并返回调用意图；真正的调用由宿主完成。对无法远程调用的命令（无动作体 / 启用条件不满足）如实报出状态，**不伪报成功**。

> `simulate_interaction` 把「生成 → 交互 → 断言」闭环搬到无头环境：`action` 取 `click`/`scroll`/`text`，`path` 为索引路径（空串=树根），返回目标控件的属性快照与整棵树的交互后逻辑快照；目标未找到或中心不可命中时置 `isError`（此时不改状态）。**只能验证可观测状态**：JSON 树不带用户回调，故点击须经状态变化（如 `Checkbox.checked`、焦点转移）而非回调副作用来确认；滚动偏移不经此通道暴露（`Scroll` 序列化 `offset`、`LazyList`/`GridView` 序列化 `scroll_offset`，但后两者的工厂是 `reg_no_props` 空占位、静态 JSON 树给不出），偏移须由 C++ 测试读回。

### 7.2 CLI（`aurora_cli`）

命令面是**一棵 `aurora::cli::CommandSpec` 声明表**（`tools/servers/aurora_cli.cpp` 的 `build_spec()`），由
[`09-cli.md`](09-cli.md) 的 `aurora::cli::parse` 消费：解析、每层 `--help`、usage 行与 `schema_json` 全部由同一份声明派生。
`-w W` / `-H H` / `-o out.png` / `--style` 是**按子命令声明**的局部选项（cobra 式「选项属于当前命令」），尺寸取值域
`[1, 8192]`，越界即 `cli-range-violated`（退出码 `2`）。

```bash
aurora_cli components                         # 列出所有已注册组件类型
aurora_cli describe <name>                    # 输出单个组件的完整 schema（JSON）
aurora_cli search <keyword>                   # 按名称搜索组件
aurora_cli validate <tree.json>               # 校验 UI 树 JSON，输出诊断
aurora_cli snapshot <tree.json> [-w W] [-H H] # 输出逻辑快照 JSON
aurora_cli render <tree.json> [-w W] [-H H] [-o out.png]  # 离屏渲染为 PNG
aurora_cli preview <tree.json> [-w W] [-H H]  # 快速预览 UI（启动临时窗口；无显示后端回退无头渲染一帧退出）
aurora_cli to-code <tree.json> [--style fluent|step|di]   # UI 树 → C++ 代码
aurora_cli to-yaml <tree.json>                            # UI 树 → YAML 格式
aurora_cli schema                             # 输出运行时重建的 API 骨架（非仓库内 aurora_api.json）
aurora_cli --help    (-h)                      # 显示用法帮助（每一层都可用，如 `render --help`）
aurora_cli --version (-V)                      # 显示版本号
```

退出码：成功 `0`，校验失败 `1`，用法错误 `2`（含缺子命令、未知子命令、未知选项、取值越界）。所有输出默认 JSON（机器可读）。
`-h` 因内建 help 占用不可用作高度短名，故高度为 `-H`。

### 7.3 LSP（`aurora_lsp`）

stdio JSON-RPC 2.0 语言服务，对 `au::<Type>Props{ .prop = ... }` 等声明式写法提供 completion / hover / diagnostics / codeAction 四件套。

| LSP 方法 | 说明 |
|:---|:---|
| `initialize` / `initialized` / `shutdown` / `exit` | 生命周期 |
| `textDocument/didOpen` · `didChange` · `didClose` | 文档同步；`didOpen` / `didChange` 后发布 `textDocument/publishDiagnostics` |
| `textDocument/completion` | `au::` 后补类型 / 枚举；`XxxProps{` 内 `.` 后补属性（显示默认值 / 必填 / 文档）；枚举属性 `=` 后补枚举值 |
| `textDocument/hover` | `au::Type` 组件概要（属性数 / 事件 / 示例）；`.prop` 类型 / 默认值 / 必填 / 文档 |
| `textDocument/codeAction` | 为缺失必填属性生成「补全缺失必填属性」快速修复（在 `}` 前插入 `.prop = <default>`） |

`publishDiagnostics` 报告：未知组件类型、未知属性、非法枚举值、缺失必填属性（warning）。

**schema 来源**：库 live API（`describe_component` + `known_enums`），**不读取 `aurora_api.json` 文件**，始终与代码同步。

### 7.4 工具可执行与自定义目标

工具 target 分两类：**可执行**（`add_executable`，直接运行）与**自定义目标**（`add_custom_target`，经 `cmake --build build --target <name>` 触发）。全部定义见 `cmake/AuroraTools.cmake`。

**可执行目标**

| 目标 | 源 | 说明 |
|:---|:---|:---|
| `gen_error_codes` | `tools/gen/gen_error_codes.cpp` | 读 [`errors.toml`](../errors.toml) → 生成 `error_codes.gen.h`、[`ERROR_CATALOG.md`](../ERROR_CATALOG.md) 与 `aurora_api.json` 的 `error_codes` 段（**不链接 aurora**） |
| `gen_api_tools` | `tools/gen/gen_api.cpp` | 反射公共 API 生成 `aurora_api.json`（schema / 类型 / 属性键 / 枚举），并保留既有 `error_codes` / `debug` 段 |
| `gen_debug_api` | `tools/gen/gen_debug_api.cpp` | 读 [`debug_api.toml`](../debug_api.toml) → 合并 `aurora_api.json` 的 `debug` 段（**不链接 aurora**） |
| `aurora_mcp` | `tools/servers/aurora_mcp.cpp` | MCP Server（stdio JSON-RPC 2.0），见 §7.1 |
| `aurora_cli` | `tools/servers/aurora_cli.cpp` | CLI 工具链，见 §7.2 |
| `aurora_lsp` | `tools/servers/aurora_lsp.cpp` | LSP 语言服务，见 §7.3 |
| `bench_render` | `tools/bench/bench_render.cpp` | 渲染基准（HeadlessSurface + Painter 计时，非 CTest 断言） |
| `bench_scroll` | `tools/bench/bench_scroll.cpp` | 滚动基准（确定性滚动序列，依赖 google_play 头） |
| `bench_win32_present` | `tools/bench/bench_win32_present.cpp` | Win32 上屏诊断基准（无 Win32 后端时跳过） |
| `bench_idle_cpu` | `tools/bench/bench_idle_cpu.cpp` | 空闲 CPU 基准（无 Win32 后端时跳过） |

**自定义目标**

| 目标 | 说明 |
|:---|:---|
| `generate_error_codes` | 触发 `gen_error_codes` 重跑（errors.toml 变更时） |
| `aurora_api_json` | 运行 `gen_api_tools` 直写 `aurora_api.json`，随后 `gen_debug_api` 再合并 `debug` 段（单跑即得完整文件） |
| `gen_debug_api_json` | 仅刷新 `aurora_api.json` 的 `debug` 段 |
| `perf_gates` | 本机时间类门槛校验（`tools/check/check_perf_gates.ps1`，仅 Windows，不进 CI） |

> AI 兼容性批量验证**不是** cmake 目标，而是 CTest 集成用例 `itest_ai_compat`（`tests/integration/itest_ai_compat.cpp`）：遍历 `tests/fixtures/ai_compat/` 下的 JSON fixture，无 LLM 调用；`valid_*` 期望通过、`error_*` 期望报错、`interact_*` 为「静态树 → TestController 交互 → 状态断言」回归脚本（`itest_ai_compat.cpp` 中段消费）。运行：`ctest -R itest_ai_compat`。

#### 7.4.1 空闲 / 叠加层刷新实测基线与复现

空闲期分两种场景，必须分别度量：**无叠加层**（事件驱动深睡）与**叠加层可见**（每 500 ms 一帧）。所有 CPU 占比均为**占单核百分比**（进程 CPU 时间 ÷ 墙钟时间 × 100），非整机百分比；采样窗口跳过启动与首帧构图，只取稳态区间。两类场景的设计语义（停帧陈旧口径、脏决策三态、HUD 刷新周期）见 [`ARCHITECTURE.md`](../ARCHITECTURE.md) §10.1 / §10.2 / §10.4。

载具与配置：

| 项 | 无叠加层 | 叠加层可见 |
|:---|:---|:---|
| 载具 | `bench_idle_cpu` | `examples/app/google_play/demo_google_play.cpp` |
| 后端 | Win32 软件渲染（`AURORA_BACKEND_WIN32`） | 同左 |
| 构建 | `build/` | `build-inspector/`（调试开启 + Inspector 服务开启） |
| 帧率上限 | 默认（60） | `opts.max_fps = 60` |
| 观测窗口 | 3 s | 静止 6 s 采样窗口 |

**无叠加层：事件驱动深睡**（基准自带判定，人工用例见 [`../manual-test/17-perf.md`](../manual-test/17-perf.md) TC-PERF-006）：

| 观测 | 值 | 判定 |
|:---|:---|:---|
| 空闲 CPU 占比 | 0.5% | PASS（门槛 < 5%） |
| 空闲「渲染帧率」 | 0.3 fps | 几乎不出帧，符合预期 |
| 空闲唤醒频率 | 4.3 / 秒 | 事件驱动，非轮询 |

- 门槛与判定由 `bench_idle_cpu` 自身给出（空闲 < 5%；旧实现忙轮询时该值会顶到约 100%）。活跃场景同次实测为 CPU 6.7%、帧率 35.8（被夹在 60 附近），PASS。
- 该场景**未挂叠加层** ⇒ 不触发 HUD-only 帧（态 ②），因而空闲帧语义的改动对它无影响；改动后已实测复验通过。

**叠加层可见：每 500 ms 一帧**（人工用例见 [`../manual-test/17-perf.md](../manual-test/17-perf.md) TC-PERF-008）：

| 观测 | 值 |
|:---|:---|
| 稳态 CPU（静止 6 s 采样窗口） | **3.4%**（占单核） |
| `idle` 计数逐秒增量 | `2, 2, 2, 16, 2, 2, 19, 2, 2, 2` |
| `(stale)` 标记出现率 | 6 / 11 个打印秒 |
| 静止期 FPS 取值 | 保持末值（如 39.499）而非归零 |
| 空闲 1.2 s 的叠加层重绘像素差 | 18716 字节 |

- 增量基线恒为 **+2 / 秒**，与 500 ms 叠加层刷新周期一致 ⇒ 空闲唤醒确实由叠加层驱动，且**没有**退化成「一帧都不出」。
- 增量为 16 / 19 的两秒是轮播动画在推进（动画期内本就应出帧），不属于空闲异常。
- 若叠加层唤醒退化成忙轮询，该 CPU 值会顶到约 100%；实测 3.4% 说明唤醒是按截止时间等待的。
- 像素差来自集成用例 `tests/integration/itest_perf_overlay_refresh.cpp` 的空闲场景：1.2 s 内只推进帧循环、不强制全量重绘，断言叠加层像素确实发生变化。

**复现步骤**

无叠加层（基准自带判定，一条命令即可）：

1. 构建基准：`cmake --build build --target bench_idle_cpu`。
2. 直接运行该基准，读它打印的两行结果与 PASS / FAIL：空闲场景要求 CPU 占比 < 5%，活跃场景要求帧率被夹在 60 附近。

叠加层可见（基准族未覆盖此场景，需自建观测）：

1. 构建调试开启 + Inspector 服务开启的构建目录，得到载具 `demo_google_play`。
2. 以分离进程方式启动载具，并把标准输出重定向到文件（载具的帧率摘要行经 `AURORA_LOG_RAW` 输出，即标准输出；标准错误恒为空）。
3. 间隔数秒两次采样该进程的 CPU 时间，跳过启动阶段，按上方口径计算占比。
4. 结束进程后读回输出文件：统计 `idle` 计数的逐秒增量与 `(stale)` 出现的行数。
5. 确认该载具进程已退出，避免占用目标导致下一次重编报占用错误。

> 上述数字为单次实测（Windows / Win32 软件后端），与机型、构建配置强相关；引用时必须连同口径一起复述。**尚未纳入 CI 门禁**：时间类门槛历史上只作本地趋势对照（见 `tools/check/perf_gates.json` 的 `source` 字段），若要新增门槛，需先固定载具、采样窗口与判定阈值。另：叠加层空闲场景此前只有自动化用例覆盖，人工用例 TC-PERF-008 已补齐。

### 7.5 真机验收探针（`tools/verify/`）

探针证明的是**无头 CI 无法证明**的平台接线：单元测试只能断言到「平台中立层」（快照 / diff / 偏移映射 / 动作路由），而「平台回调是否真的到达、平台侧对象是否真的可查」只能建真实窗口、在真实桌面会话里验收。全部由 `cmake/AuroraVerify.cmake` 定义、`AURORA_BUILD_VERIFY_TOOLS` 门控、**不进 CTest**（会创建真实窗口 / 读取屏幕状态，非确定且干扰用户桌面）。

| 探针 | 验证目标 | 读回方式 |
|:---|:---|:---|
| `aurora_verify_win32_cursor` / `aurora_verify_x11_cursor` / `aurora_verify_macos_cursor` / `aurora_verify_glfw_cursor` | `Surface::set_cursor` 是否真的改变了屏幕上显示的**光标**（Win32 一支同时验 `Win32Surface`(GDI)、`D3D11Surface` 与 `WgpuWin32Surface` 三个宿主调用点，共用 `detail::set_win32_cursor` 映射） | 平台查询读回（Win32 `GetCursorInfo` / X11 XFIXES `XFixesGetCursorImage` / macOS `[NSCursor currentCursor]` 单例同一性；GLFW 无查询 API → 能力核对 + 人工目视） |
| `aurora_verify_wayland_cursor` | `WaylandSurface::set_cursor` 是否真的把该形状的光标位图**提交给了合成器**（客户端主题光标三步：`wl_cursor_theme_load` → cursor `wl_surface` attach 主题自有 `wl_buffer` → `wl_pointer_set_cursor`） | **本端提交事实读回**（`WaylandSurface::cursor_state()`：主题命中名 / 位图尺寸 / 热点 / `wl_buffer` 身份 / 提交次数）——Wayland 协议**没有**客户端可达的「屏幕当前光标」查询，故「合成器画出来了」由 `--interactive` 人工目视段负责；实测（2026-09-20，WSLg Weston + Adwaita）自动段 9 条 `[PASS]`：11/11 形状命中名逐字等于 `cursor_rfc_name`、位图均 24x24、11 个互异 `wl_buffer`、`wait` 动画 60 帧取首帧、同形状重复下发幂等（14→14） |
| `aurora_verify_win32_ua` | Win32 UIA 无障碍桥与 Windows 的接缝：根对象能否应答、语义树能否 `Navigate`、必需属性是否有值、各 pattern 能否 QueryInterface 到 | **COM UIA 客户端**（`CUIAutomation8`，与 NVDA / Narrator 同路径）`ElementFromHandle` 取根 → 控件视图遍历器先序下钻 → 逐节点读属性与 pattern 并与期望表比对；`FrameworkId == "Aurora"` 区分桥投影元素与 UIA 默认 HWND provider 合成的非客户区 |
| `aurora_verify_atspi` | Linux AT-SPI2 桥与真实 a11y 总线的接缝：`Socket.Embed` 握手、树可见性、状态集 / 动作 / 几何 / 文本读回、`DoAction` 回灌宿主（`on_click` 真实执行）、**事件推送段**（客户端注册 7 类精确事件监听，父进程按时间表做声明式变更：标题/输入值/ReorderableList 收缩再增长/播报，断言 `focus:`、`state-changed:focused`、`property-change:accessible-{value,name}`、`children-changed:{add,remove}`、`announcement` 全部被动收到） | **libatspi 客户端**（探针内起 `python3-gi` 子进程，与 Orca 同路径）从桌面树按 app/frame/button/entry/static 逐检查项断言并打印 `RES|pass/fail`；实测（2026-09-20，WSLg）Wayland 与 X11 两路 20/20 ALL PASS。运行需 `GI_TYPELIB_PATH`/`PYTHONPATH` 指向解包好的 gir 仓库（见源文件头注释） |

各探针的验收范围、逐项期望与退出码语义写在对应源文件头注释内（`tools/verify/*.cpp|.mm`）；真机验收须在**对应平台**手工执行。

**由后台进程执行时的物理前提**（不是软件缺陷，探针按退出码如实申报而非假通过）：凡判据落在「屏幕上真实显示的指针/光标」上的探针（`aurora_verify_win32_cursor`，以及各探针的 `--interactive` 人工段），要求**已解锁且处于活动状态的交互桌面**——探针会把被测窗口置顶（`HWND_TOPMOST`）并依次试摆「屏幕中心 → 四角内侧」共 5 个落点（同处置顶带内他人窗口可长期压住中心点，`SetForegroundWindow` 又受前台锁约束），多次不中才以退出码 3 报出「期望落点 / 实际指针位置 / 该点上的窗口类名 / 试过的落点数」现场证据后终止（远程桌面会话隔离、锁屏时的 `LockScreenBackstopFrame` 同理）。纯逻辑/句柄类判据（如 GLFW 探针自动段：`glfwCreateStandardCursor` 句柄互异计数）不受此约束，可在任意会话内跑通。

**读回屏幕光标前必须真正派发平台事件**（探针侧时序约束，非库缺陷）：`Surface::wait_events` 只等待、不派发（Win32 侧派发在 `poll_platform_events` 的 `PeekMessage`/`DispatchMessage`），而 `GetCursorInfo` 读回的共享光标随 WM_SETCURSOR 走完 wndproc 才刷新——只 wait 不 poll 会让读回恒停在上一手的值，本线程 `GetCursor()` 却逐形状命中，极易误判成「本会话读不回」。故 Win32 探针每形状按时序「1px 位移 → 派发 → `set_cursor` → 立刻读回（不再派发，避免 DefWindowProc 用窗口类光标覆盖）」执行；实测（2026-09-20，Windows 11 + MinGW 构建）`Win32Surface` / `D3D11Surface` / `WgpuWin32Surface` 三路各 11/11 读回命中且两两互异。

**Wayland 侧取 `wl_pointer.enter` 的 serial 只能靠铺满窗口**（同上类约束）：`wl_pointer.set_cursor` 的 serial **必须**来自本表面的 `wl_pointer.enter`，而 Wayland 客户端**没有** warp 指针的 API（指针位置由合成器持有），故探针只能把窗口铺满输出、让静止的物理指针必然落入表面。铺满分两级：先最大化，拿不到 enter 再全屏——实测（2026-09-20，WSLg 3840x2160）**最大化不够**：其最大化表面只有 3840x2088（顶部让出面板带），静止指针恰好停在带上，enter 永不到达；全屏（3840x2160）随即取到。判据无从执行时以退出码 3 报出，并打印「累计鼠标事件」区分「本会话根本没有指针设备」（0 条）与「有指针但落不进窗口」。

**X11 侧落点必须按窗口自身几何求，不能按请求坐标**（同上类约束）：`XMoveResizeWindow` 只是请求，rootless Xwayland 下合成器会把窗口安放到别处（实测请求全屏 `(0,0,7680x2160)` 后窗口报回左上角 `(639,1214)`），故「按硬编码点 warp + 按另一份几何判」两套口径互斥、恒判「指针放不过去」。探针改为等窗口几何稳定且完整落在 root 内、按**该几何中心** warp、再用同一次读回判命中（强判据 `XQueryPointer` 的 `child==win`，Xwayland 常不回 child 故按几何包含放行）。放行不是假阳性——判据仍是 XFIXES 读回；实测（2026-09-20，WSLg）11 形状读回 11/11 互异且逐行等于 `x11_cursor_glyph` 期望字形名。

---

## 8 测试原语（`aurora::test`）

`tests/support/test_helpers.h`（**仓库私有设施**：位于 `tests/` 下，不进 `include/`、不进 `aurora.h`、
不进 `aurora_api.json`，不受公共 API 兼容承诺约束），使用方须显式包含：

| 原语 | 说明 |
|:---|:---|
| `init_headless(w, h)` | 初始化无头环境 |
| `pump(env)` | 确定性 mount + layout（内部即 `render_to_logical_snapshot`） |
| `tap(env, widget)` | 合成 `MouseEvent` Press + Release |
| `type_text(env, widget, text)` | 合成 `TextInputEvent` |
| `expect_text` / `expect_tree_contains` / `expect_bounds` / `expect_visible` / `expect_count` | 断言辅助（内部统一 `AURORA_TEST_CHECK*` 宏族，依赖 `tests/framework/aurora_test.h`） |

`tests/support/fake_gl.h`（同为**仓库私有设施**）：`aurora::testing::FakeGl` 全量 GLFn 驱动桩——
模拟「3.3 core 完整实现」并记录 draw / clear / blit / 上传与纹理分配/删除计数，供
`utest_gpu_gl_rhi`（GpuGlRhi 契约断言）与 `tools/bench/bench_gpu.cpp`（GPU 特性基准的
确定性计数器，无需真实 GL 上下文）共用。
`bench_gpu` 的**两套口径**同档并存：场景一/二用 `FakeGl` 计数器隔离「CPU 侧翻译与纹理生命周期策略」
（分配/删除次数、上传字节，跨机器确定），场景三/四/五/六在 `AURORA_BACKEND_GPU_WGPU` 配置下改用
`WgpuRhi` **真 GPU 离屏**端到端帧成本（流式槽 vs content_hash 缓存、`cache_layer` 层缓存、大图 compute
mip 链、区域效果 compute vs 片元两路——后者经 `set_compute_effects_enabled` 切换），无可用 adapter 时整段
如实跳过。真 GPU 段每帧报「提交（CPU）」与「端到端（批尾一次排空）」两列：
后者靠 `WgpuRhi::set_readback_enabled(false)` 关掉逐帧读回、末尾一帧重开并单次 `read_pixels` 排空队列
（逐帧 `read_pixels` 会让每帧都吃一次平台睡眠量子，Windows 实测把全部场景压成同一地板值，见
[`specification/03-layout-render.md`](03-layout-render.md) §8.8 运行时契约注意）。基准数为**本机相对量**
（不进 CTest，跨机器不可比）。

**无障碍桥签（`RecordingProvider`，`tests/unit/utest_a11y_bridge.cpp`）**：沿 `FakeSink` / `FakeGl`
同款「假实现记录调用序列」范式，`RecordingProvider : a11y::Provider` 只观测「桥侧收到了什么」
（激活 / 置脏 / 事件 / 播报 / 控件销毁通知），不触任何平台 API——使桥注册表语义、事件广播与宿主
处理器并存、播报直投、`screen_reader_active` 回填、语义根销毁通知等**平台中立契约可在无头环境
端到端断言**。平台实现（Win32 UIA COM provider / 平台事件 API）不在单测射程内，由 `tools/verify/`
的真机探针覆盖（§7.5）。

### 8.1 框架设施（`tests/framework/`）

| 设施 | 说明 |
|:---|:---|
| `AURORA_TEST_CASE(<Case>)` | 静态注册用例；全名 `<文件 stem>.<Case>`，套件名由 `__FILE__` 推导、不可自定义 |
| `AURORA_TEST_CHECK_*` / `AURORA_TEST_REQUIRE_*` | 非致命 / 致命两族断言，谓词清单与语义见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md) §3.1 |
| `AURORA_TEST_F(<Fixture>, <Case>)` | fixture 用例：`SetUp` → 用例体 → `TearDown`（用例抛异常同样清理），每用例重建实例 |
| `AURORA_TEST_P` + `AURORA_INSTANTIATE_TEST_SUITE_P[_GEN]` | 值参数化：fixture 派生自 `TestWithParam<V>`，体内 `param()` 取值；取值表 `values_of(...)` / `values_in(container)`，`_GEN` 变体带名字生成器 |
| `AURORA_TYPED_TEST_SUITE` + `AURORA_TYPED_TEST` | 类型参数化：对 `TypeList<...>` 逐类型展开成独立用例 |
| `AURORA_TEST_CHECK_THAT(value, matcher)` | 匹配器断言；工厂与组合器在 `aurora::testing::matchers::`（`eq` / `str_eq` / `contains` / `each` / `size_is` / `all_of` / `any_of` / `negated` …） |
| `AURORA_TEST_SKIP(原因)` | 运行时跳过（后端 / 平台未编译时注册 skip 桩，计入 Skipped 不算失败） |
| `AURORA_TEST_TRACE(说明)` | 作用域追踪栈，随作用域自动出栈；本作用域内失败自动附带 |
| `aurora::testing::ValuePrinter<T>` | 值打印定制点：测试侧显式特化即接管该类型在失败信息里的渲染 |
| `AURORA_TEST_CHECK_DEATH(stmt, expectation)` | 死亡测试：spawn 子进程重跑同一用例，只有该站点执行语句；`expectation` 为子串或匹配器（校验子进程 stderr），空串=只要求致死 |
| `aurora::testing::write_report` | 结果报告：`.xml` → JUnit XML，其余 JSON；超时路径同样落盘已完成部分（`tests/framework/reporter.h`） |
| `aurora_test_runner` | 唯一 `main`（`tests/framework/test_main.cpp`），测试 TU 禁止自定义 `main()`；起手执行 `TestRegistry::finalize()` 统一展开参数化用例，故 `--list` / `--run` 即展开后全集；`--report` / `--shuffle` / `--repeat` / `--timeout`（看门狗，退出码 3）见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) §7.1 |

### 8.2 真实后端 E2E 驱动内核（`tools/include/e2e/harness.h`）

真实后端端到端测试（`tests/e2e/` 下 `etest_` 用例，受 `AURORA_BUILD_E2E` 门控）的**框架无关**驱动
内核。它不是新增开发，而是**既有真实后端测试代码的抽取与统一**：建窗 / 帧推进 / 像素读回在仓库内
只有这一份实现，不得出现第二套容差常量或第二套 skip 约定。

| 项 | 约定 |
|:---|:---|
| 位置与依赖 | `tools/include/e2e/harness.h`，**只依赖 aurora 公共头**，不含任何 `AURORA_TEST_*` 宏——故 `tests/`（`etest_` 用例）与 `tools/verify/`（真机验收探针）可共用同一份实现 |
| 分层 | 场景层（`examples/demos/scenes/` 下 header-only 场景头，与 demo 同源）→ 进程内驱动内核（本头）→ 进程外客户端（`InspectorServer` REST，见 §5） |
| 后端标识 | `e2e::Backend` 枚举器**无条件出现**（不随 `AURORA_BACKEND_*` 裁剪）：未编译的后端请求得到明确的「不可用 + 原因」，而非编译失败。`backend_compiled()` 是纯编译期事实，`open()` 的运行期失败以 `ok()` / `reason()` 如实上报 |
| 建窗 | `e2e::open(WindowSpec)` 经类型安全的 `create_window(XxxOptions)` 工厂；**不静默降级**——请求的后端未编译或初始化失败即失败。「跳过还是失败」是测试侧策略，落在 `tests/e2e/e2e_expect.h`（`AURORA_E2E_EXPECT` 期望集），内核不裁决 |
| 窗口可见性 | `WindowSpec::visibility` 默认 `Hidden`（与公共 `WindowOptions::visibility` 的默认 `Normal` 刻意不同）：E2E 默认不把窗口推入用户视野；三档语义与各宿主落地见 [`specification/03-layout-render.md`](03-layout-render.md) §8.3 与 [`specification/06-app-platform.md`](06-app-platform.md) §3.3 |
| 帧推进 | 帧序复用 `TestController` 已验证的既有序列（泵平台事件 → `Widget::tick` 手势计时 → `Animator::tick` → `Scheduler::tick` → `Window::present_root`）：内核自持 `Animator` / `Scheduler` 并经其 `set_current` 挂为进程内当前实例，使 mount 期注册的动画与定时任务可被逐帧推进。**不新增任何公共单帧 API**（`Application::step_frame()` 保持 `private`） |
| 收敛与超时 | `pump_until_settled(max_frames)` 以「`Window::is_idle_frame()` 且无运行中动画」收敛；预算内未收敛返回 `RuntimeAsyncTimeout`，消息含已推进帧数、最后脏区状态、idle 帧状态与活跃动画数（可直接作为失败原因） |
| 像素读回 | `capture_frame(const Surface &)` = `Surface::data()` + `Surface::framebuffer_size()` 组合，返回帧缓冲**物理像素**的 RGBA 帧；**未新增 `Surface` 公共读回虚方法**。`data()` 为 `nullptr`（后端未覆写读回，或该后端的读回受 `AURORA_ENABLE_DEBUG` 门控且未生效）时返回 `GeneralNotSupported`，消息沿用 `save_snapshot` 既有的 "framebuffer capture unavailable"；不返回空帧、不伪造内容 |
| 查询面 | `collect_preorder` / `find_by_key` / `find_by_type` / `find_by_text` / `read_prop` 全部建立在**公共自描述通道**上（`Node::id()` / `Widget::type_name()` / `Widget::serialize_props` / `Widget::child_nodes()`），**不依赖 `TestController`**——后者整头受 `AURORA_BACKEND_HEADLESS` 门控，若查询面依赖它，「关掉无头后端但开真实后端」的构建里 E2E 恰好失去查询能力 |
| 输入注入 | `tap` / `drag` / `scroll` / `enter_text` 经 `Inspector::simulate_*` 走真实命中测试与冒泡派发（目标式语义），而非直接改控件状态；注入成功后登记「下一帧全量重绘」，模拟真实平台输入事件唤醒帧循环 |
| 生命周期 | `Session` 以 RAII 兜底窗口生命周期：用例中途断言失败、抛出异常或提前 return 时析构即关闭窗口并回收宿主，不残留幽灵窗口；该保证不依赖用例显式调用清理函数，也不依赖测试框架的断言宏 |

内核自测在 `tests/unit/utest_e2e_harness.cpp`（以 `HeadlessSurface` 为后端运行，不依赖真实显示环境）。

**CI 能力边界（GitHub Actions 三平台实测）**：真实建窗 + 像素读回的能力因 runner 环境而异，
E2E 的 CI 期望集（`AURORA_E2E_EXPECT`）须按平台声明：`windows-latest` 上 Win32 与 D3D11 可用，
GLFW 不可用（VM 无 OpenGL 3.3，WGL 通用驱动无法建 GPU 上下文）；`ubuntu-latest` + `xvfb-run` 上
X11 与 GLFW 可用（llvmpipe 软件 GL 可建 3.3 上下文；GLFW 源码构建需 `wayland-scanner` 等
Wayland 依赖）；`macos-latest` 无人值守会话无窗口系统，全部后端不可用（期望集留空 = 全部
skipped by policy，编译与注册面仍被覆盖）。Wayland（CI 无 compositor）与 wgpu（默认不编译）不进
CI 默认范围，由真机或本地会话 opt-in。另有两点硬约束：CI 的 E2E 步骤必须以
`AURORA_ENABLE_DEBUG=ON` 或 Debug 配置构建（部分后端读回受该宏门控，宏未注入时读回一律按
「能力不可用」记账）；期望集内环境不可用即 FAIL_FATAL 红灯，因此期望集声明的是「该环境必须
可跑」的最小集，宁可留空也不声明未实测的后端。

场景库与组件 demo **同源**：被 E2E 引用的组件在 `examples/demos/scenes/` 下建 header-only 场景头
（`scene_<组件>.h`，inline 构建函数返回根 `Node`），对应 `demo_<组件>.cpp` 退化为「薄 `main()` +
`run_demo(...)`」，人类可见的 demo 行为（尺寸 / 标题 / 渲染结果）逐字节不变；**未被引用的 demo 零改动**，
全量抽取（所有 demo 薄 `main()` 化）留作后续增量。使用 `Application` 装配的 demo 只抽 UI 构建，
装配留在其 `main()` 内，E2E 用例以同一构建函数取根节点经内核自行驱动。场景枚举由场景库自带：
`examples/demos/scenes/scene_registry.h` 注册表头 + `examples/demos/scene_tool.cpp` 小工具（`--list` 列场景 /
`--render` 以 HeadlessSurface 软件路径渲染 PNG——后者同时是 golden 基线的软件 SSOT 渲染器），
**不占用 runner 的 `--list`**（用例/套件面与场景面是不同 CLI 面）。进入 golden 用例集的场景须满足
确定性渲染契约：不依赖墙钟时间、不依赖随机数（或固定种子）、动画在捕获前推进到静止态；
含文本场景（demo 房风含 GradientTitle 与标签）须按字体依赖单列更大的差异像素预算。

**golden 容差层**（`tests/e2e/etest_smoke_render.cpp` 第二用例组，场景 × 后端矩阵展开）：

- 基线：`scene_tool --render` 生成的软件 SSOT PNG（`tests/golden/e2e_<场景>.png`，scale=1）。软件读回
  路径（Win32/GDI、GLFW-软件、X11）与基线同一条 Painter 链路，漂移恒 0；预算只为 GPU 路径的跨驱动
  AA 差异买单。
- 背景口径：基线渲染与真实窗口上屏必须同底色——`render_to_image` / `render_to_png` /
  `Scene::render_to_png` 提供 `std::optional<Color> background` 默认参（默认不填 = 零初始化透明黑，
  行为不变），`scene_tool --render` 传 `Surface::clear_color()` 同款 `{245,245,247,255}`。
- 判据单源：`golden::compare_gpu_tolerance`（差异像素数 ≤ 场景预算 + 单通道容差 tol=48，失败消息含
  差异区域与控件归因）。预算**按场景级申报**（`SceneGoldenBudget`：预算 + 覆盖形态 + `requires_scale_one`），
  不读全局旋钮 `AURORA_GOLDEN_MAX_*`；预算为防御值——CI 探针实测默认矩阵全部后端（含 D3D11 增量上屏
  偏置路径）与基线同一条 Painter 软件栅格链路、漂移恒 0，收紧或放宽须以新的实测证据为准。
- DPI 口径分两层：**帧尺寸维度**——读回帧是帧缓冲物理像素，帧尺寸 ≠ 请求逻辑尺寸即环境缩放生效，
  逐位比对不适用，记 SKIP；**内容域维度**——帧尺寸一致也可能不等：经物理域离屏缓冲的控件（如
  Scroll 滑动窗口按 `ctx.scale_factor` 高清录制、composite 下采样回逻辑缓冲）在 scale ≠ 1 环境下
  字形光栅与 scale=1 基线不同，此类场景以 `requires_scale_one` 申报，surface scale ≠ 1 时记 SKIP
  （CI 100% DPI 环境全跑保证门禁有效性，本地高 DPI 环境诚实跳过）。
- 度量 artifact：`AURORA_E2E_METRICS_FILE` 置定时逐用例追加 JSONL（差异像素数 / 最大单通道差 /
  帧与基线尺寸 / 预算与判定 / 失败帧 PNG 路径），供 CI 上传与容差校准；未置不写、写失败不影响用例。
- 失败落盘：比对超预算时把实际帧 PNG 写 `build/e2e-failures/`（预判用 `compare_snapshots`，判据仍以
  `compare_gpu_tolerance` 单源）。

---

## 9 日志通道

### 9.1 双通道

| 通道 | 宏 | 目标 | 特征 |
|:---|:---|:---|:---|
| 诊断通道 | `AURORA_LOG_{TRACE,DEBUG,INFO,WARN,ERROR,FATAL}(category, ...)` | 默认 **stderr** | 统一前缀 `[YYYY-MM-DD HH:MM:SS][级别][module@threadId filename:line] > content`；受 `set_level`（默认 `Info`）与 `set_enabled` 阈值过滤 |
| 功能输出通道 | `AURORA_LOG_RAW(category, ...)` | 默认 **stdout** | 无前缀、不受级别阈值 / 启用开关限制、**始终输出**；`default_raw_sink` 调 `std::fflush(stdout)` 保证 stdio 线协议逐条即时送达 |

`module` 即 `category`，`threadId` 为当前线程 id，自动带 file:line。

功能输出通道用于 CLI 的 JSON 结果与 usage 文本、benchmark 表格、LSP / MCP 的 `Content-Length:` 协议帧等「程序产品」输出，避免污染下游解析。

### 9.2 重定向与桥接

- `Logger::set_sink(LogSink)` / `set_raw_sink(LogSink)`：把两通道重定向到文件或测试捕获；传 `nullptr` 恢复默认。
- `AURORA_TEST_PRINTF` / `AURORA_TEST_PRINTF_ERR`：先把 `printf` 风格经 `std::snprintf` 写入**内存缓冲**（非标准输出），再经 `AURORA_LOG_INFO/ERROR("test", ...)` 输出，作为遗留诊断代码的兼容桥接。**新代码请直接用 `AURORA_LOG_*` / `AURORA_LOG_RAW`**，勿新增 `printf` 调用。

### 9.3 不变量

库代码唯一允许直接触达标准输出之处是 `src/aurora/core/log.cpp` 内 `default_sink` / `default_raw_sink` 的 sink 实现（含 `init_console()` 的 UTF-8 代码页设置）；其余源码（含 `tools/`、`examples/`、`tests/`）一律走日志接口。

---

## 10 需求规格

### 10.1 #9 结构化错误信息（JSON 可解析）

**核心目标：** AI 易调试——错误必须可被 AI 直接解析，而不只是被人读懂。

**验收标准：**

- 运行时错误以**值语义**返回 `au::Result<T>`（内含 `au::Error`），**不抛异常**；错误对象自带定位与建议，使 AI 能在一次迭代内读到「哪里错了 + 怎么改」。
- `Error` 字段：`code`（冻结对外 slug）/ `message` / `suggestion` / `docs` / `where`（`file:line`）/ `hint`，外加表驱动元数据 `code_enum` / `severity` / `category` / `auto_fixable` / `retryable` / `fix_category`。
- **机器可解析形态由 `Error::to_json()` 提供**：`{ "code", "message", "suggestion"?, "docs"?, "where"?, "hint"? }`，可选字段仅非空时输出。本库只产出修复**建议**，不自动修改用户代码。
- 渲染前的整树静态检查走 `au::validate(const Node& root, int max_depth = 64) -> Result<bool>`；UI 树 JSON 校验走 `au::validate_ui_tree_json()`（MCP `validate_ui` 工具）。
- **两条输出流分工**：诊断日志走 `AURORA_LOG_*`；CLI 的 JSON 诊断结果 / usage、LSP 线协议帧等「程序产品」输出走 `AURORA_LOG_RAW`，两者互不污染，保证下游管道可直接解析。
- 编译期诊断用自定义 `static_assert` 消息（C++20/23），必须在第一个错误点给出、不级联。
- 运行期降级（#21）必须同时产生结构化警告，让 AI 从渲染快照与日志两侧都能识别「此处被降级」。

**设计决策：** 不采用 SARIF。SARIF 面向静态分析工具，而 Aurora 的错误涵盖运行时场景（空子元素、非法属性值），故采用自定义 JSON 错误格式，同时覆盖编译期与运行时。

**错误码权威：** 所有错误码的真实产生点与语义见 [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)（由代码 `make_error(...)` 调用逐项核对），本文不复述清单。

### 10.2 #10 内置 UI Inspector

**核心目标：** AI 可观测运行时。

```cpp
std::string dump = au::dump_tree_json(root).dump();
// {
//   "type": "Column",
//   "children": [
//     {"type": "Text", "props": {"content": "Hello", "font_size": 24}},
//     {"type": "Button", "props": {"label": "Click", "enabled": true}}
//   ]
// }
```

**关键约束：**

- 运行时 Inspector 提供 HTTP 服务接口（§5）。
- 不仅人类可用，AI Agent 也能通过 MCP 协议查询组件树、属性、状态快照。
- 形成「代码 → 运行 → 检查 → 修复」的反馈闭环。
- UI 树 dump 统一以 `widget/inspect.h` 内的自由函数提供，**不提供 `Widget::dump()` 成员方法**。

**验收标准：** 任一运行时 UI 树可经 `dump_tree_json*` 导出且能被 `from_json` 重建；`InspectorPanel` 可浏览与回写属性。

### 10.3 #12 机器可读 API Schema

**核心目标：** AI 工具链直接消费。

随库发布结构化的 API 描述文件 `aurora_api.json`，供 LLM 直接作为 function calling 的 schema。

```json
{
  "component": "Button",
  "namespace": "aurora",
  "properties": [
    { "name": "label", "type": "LocalizedString", "required": true, "default": "" },
    { "name": "color", "type": "Color", "default": "blue",
      "note": "背景色（视觉变体靠背景色区分，无 variant 枚举）" },
    { "name": "corner_radius", "type": "float", "default": "6.0" },
    { "name": "padding", "type": "EdgeInsets", "default": "12,6,12,6" },
    { "name": "enabled", "type": "bool", "default": "true" },
    { "name": "on_click", "type": "callback<void>", "required": false }
  ],
  "events": ["on_click"],
  "children_policy": "none",
  "examples": [
    "au::Button(au::ButtonProps{ .label = \"Submit\" }).set_on_click(handle_submit)"
  ]
}
```

**关键约束：**

- 格式为 JSON Schema / OpenAPI 风格接口描述。
- 不仅包含函数签名，还要包含**类型约束、默认值、组件组合规则**。
- Schema 从代码**自动生成**（`gen_api_tools`），保证与实现同步。

**验收标准：** `aurora_api.json` 覆盖全部已注册控件；新增 / 删除 widget 或类型后重跑生成器即可同步，无手工维护项。

### 10.4 #13 UI 树序列化 + 差分 Patch 协议

**核心目标：** AI 可增量修改 UI——整树可往返，局部改动不必重传全树。

```cpp
au::Json json = au::serialization::to_json(my_widget_tree);
auto restored = au::serialization::from_json(json);   // 返回 Result，使用前判 ok()

// YAML 输出（当前仅输出方向，无 from_yaml）
std::string yaml = au::serialization::to_yaml(my_widget_tree);  // Widget 树 → YAML
std::string yaml2 = au::serialization::to_yaml(json_value);     // Json → YAML
```

**验收标准：**

- 任何 UI 树都可以双向转换（`to_json` ↔ `from_json`，形态与失败语义见 §2.2），并支持从结构化描述直接构建 UI 树。
- 差分协议基于 JSON Pointer 定位 + `replace` / `add` / `remove` 三类操作，允许 AI 只发送部分 UI 树 patch 而不是整树（这是「AI 编辑现有界面」的成本下限：改动越小，token 越少）。
- 反序列化失败必须是**值语义的失败**（`Result` + 结构化 `Error`），不得抛异常或产出一棵「半合法」树（与 #9 / #21 的错误与降级策略一致）。

### 10.5 #16 示例驱动文档（Recipe 形式）

**核心目标：** AI 从示例高效学习。

```cpp
int main() {
    auto root = au::Column(au::ColumnProps{
        .gap = 8,
        .children = {
            std::move(au::Text("Button Examples").font_size(20).bold()),
            au::Button(au::ButtonProps{ .label = "Primary" }),
            std::move(au::Button(au::ButtonProps{ .label = "Disabled" }).set_enabled(false)),
        },
    });
    root.modifier = au::Modifier{}.padding(20);
    auto win_res = au::create_window(au::Win32Options{ .title = "Button Example" });
    au::Application app{ au::Scene{ std::move(root) },
                         win_res ? std::move(win_res.value()) : nullptr,
                         au::WindowOptions{ .title = "Button Example" } };
    app.run();
}
```

**关键约束：**

- 每个示例 **≤ 30 行**，可独立编译运行。
- 示例覆盖所有组件的所有常见用法。
- 示例本身就是集成测试。
- 示例被组织成配方形式（见 [`GUIDELINE.md`](../GUIDELINE.md)），AI 可以通过检索示例直接拼接出目标代码。

### 10.6 #17 LSP / MCP Server / CLI 工具链

**核心目标：** AI Agent 直接集成。

三件套的 API 契约见 §7：

- **MCP Server（`aurora_mcp`）**：stdio JSON-RPC 2.0，暴露 13 个 MCP tools。
- **CLI（`aurora_cli`）**：子命令 `components` / `describe` / `search` / `validate` / `snapshot` / `render` / `preview` / `to-code` / `to-yaml` / `schema`，以及全局选项 `--help`（`-h`）/ `--version`（`-V`）。
- **LSP（`aurora_lsp`）**：stdio JSON-RPC 2.0 语言服务，对声明式写法提供 completion / hover / diagnostics / codeAction 四件套，消费库 live API（`describe_component` + `known_enums`），无需读取 `aurora_api.json` 文件，始终与代码同步。

**验收标准：** AI Agent 可仅凭工具链完成「发现控件 → 校验树 → 渲染快照 → 生成代码」全链路，无需读取源码。

### 10.7 #22 可逆性：UI → 代码的参考还原

**核心目标：** AI 可分析现有界面并重构。定位是「结构化往返」而非「完全可逆」。

**往返语义：**

- 正向：代码 → UI 树（JSON）**必须支持**（`serialization::to_json`）。
- 反向：UI 树（JSON）→ 代码**只提供参考实现**（`serialization::to_code`），生成的是**规范形式**，不保留原始代码的变量名、注释与结构。
- 序列化格式（JSON）是 **canonical form**，不含代码风格信息——风格由调用方通过 `CodeStyle` 选择。
- `to_code` 生成的代码**必须可编译**。
- 语义等价判定标准：`to_json(from_json(to_code(json))) == json`（往返一致性）。

**关键约束：**

- 这是**工具层**功能（CLI / MCP / InspectorPanel），不是库核心 API。
- 与 #10 Inspector 集成：`to_code(dump_tree_json_full(root))` 可直接获取当前 UI 的代码表示。
- 与 #17 CLI 集成：`aurora_cli to-code tree.json --style fluent`。
- `InspectorPanel` 已支持导出代码：`export_code()` + 「Export Code」按钮 + `on_export_code` 回调，实现 Inspector → 代码闭环。
