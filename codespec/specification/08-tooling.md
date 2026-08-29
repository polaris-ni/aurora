# 序列化、检查器与工具链（serialization / inspector / tooling / log）

> 覆盖 `widget/serialization.h`、`widget/codegen.h`、`widget/yaml.h`、`widget/inspect.h`、`inspector/`、`core/log.h`、`debug/`、根级 `test_helpers.h` 与 `tools/`。
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
| 工具可执行文件 | `aurora_mcp`、`aurora_cli`、`aurora_lsp`、`gen_api_tools`、`ai_compat_test` |

---

## 2 序列化

### 2.1 线格式

节点形态固定为：

```json
{ "type": "<type_name>", "props": { ... }, "children": [ ... ] }
```

`children` 为空时不输出。**属性一律位于 `props` 子对象下**，因此补丁 path 形如 `/children/0/props/show`。

`Json` 类型是 `nlohmann::json` 的别名，定义于 `widget/props_io.h:15`。

部分控件的序列化 `type` 名与 C++ 类名不同（`Image` → `ImageView` / `ImageViewProps`）。

### 2.2 API

本表函数除 `list_all_schemas` 外均位于 `aurora::serialization` 命名空间（**不是** `aurora`）。`list_all_schemas` 声明在 `serialization` 块之外，属 `aurora` 命名空间（`serialization.h:105`，与 `list_all_components` / `describe_component` / `search_components` 同区），调用写作 `au::list_all_schemas()`。

| 函数 | 签名 | 位置 |
|:---|:---|:---|
| `to_json` | `[[nodiscard]] auto to_json(const Widget &w) -> Json` | `serialization.h:45` |
| `from_json` | `[[nodiscard]] auto from_json(const Json &j) -> Result<std::shared_ptr<Widget>>` | `serialization.h:73` |
| `diff` | `[[nodiscard]] auto diff(const Json &a, const Json &b) -> std::vector<JsonPatchOp>` | `serialization.h:79` |
| `diff_into` | `auto diff_into(const Json &a, const Json &b, const std::string &path, std::vector<JsonPatchOp> &out) -> void` | `serialization.h:76` |
| `apply_patch` | `auto apply_patch(Json &target, const std::vector<JsonPatchOp> &patch) -> void` | `serialization.h:83` |
| `to_yaml` | `[[nodiscard]] auto to_yaml(const Widget &w) -> std::string` | `serialization.h:91` |
| `component_schema` | `[[nodiscard]] auto component_schema(const std::string &name) -> Json` | `serialization.h:87` |
| `list_all_schemas` | `[[nodiscard]] auto list_all_schemas() -> std::vector<Json>` | `serialization.h:105` |

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

`serialization::WidgetRegistry`（`serialization.h:51`）:

| 方法 | 签名 |
|:---|:---|
| `instance()` | 静态，返回 `WidgetRegistry&`（`serialization.h:53`） |
| `register_factory(type, WidgetFactory)` | 注册工厂（`serialization.h:58`） |
| `make(type, props)` | `[[nodiscard]] auto make(const std::string &type, const Json &props) const -> Result<std::shared_ptr<Widget>>`（`serialization.h:60`） |
| `list_types()` | `[[nodiscard]] auto list_types() const -> std::vector<std::string>`（`serialization.h:63`） |

`WidgetFactory = std::function<Result<std::shared_ptr<Widget>>(const Json&)>`（`serialization.h:48`）。

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
| `to_code(const Json &node, int indent = 0) -> std::string` | `codegen.h:297` |
| `to_code(const Json &node, CodeStyle style, int indent = 0) -> std::string` | `codegen.h:342` |
| `to_code(const Widget &w) -> std::string` | `codegen.h:357` |

```cpp
enum class CodeStyle : std::uint8_t { Fluent, StepByStep, DesignatedInit };   // codegen.h:19-23
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
- 登记表按「键名」而非「类型」工作，故新增枚举属性时必须同步登记真实键名，否则该属性静默退化为字符串（`tests/test_serialization.cpp` 的真实键名用例守护此点）。

多子扁平容器（`Column` / `Row` / `Stack` / `Grid` / `Scroll` / `Card`）走免 `Props` 包裹的罗列形式。

```cpp
std::string code = au::serialization::to_code(json, au::serialization::CodeStyle::Fluent);
```

---

## 3 控件树检查

UI 树 dump 统一以 `widget/inspect.h` 内的**自由函数**提供，**不提供 `Widget::dump()` 成员方法**——富格式需求由 `dump_tree_rich` 覆盖，避免为每个控件重复实现 dump 逻辑。

| 函数 | 说明 |
|:---|:---|
| `dump_tree(root)` | 人类可读缩进树 |
| `dump_tree_rich(root, depth = 0, tree_chars = true)` | 富格式树，含 `#id` / bounds / visible / text / style / listeners，以 `├─ └─ │` 连接 |
| `dump_tree_json*` / `dump_tree_json_full(root) -> Json` | JSON 快照；`dump_tree_json_full` 含属性（每节点 type / props / children） |
| `widget_tree_to_items(root) -> std::vector<TreeItem>` | Widget 树 → TreeItem 树（供 `TreeView` 消费） |
| `find_node_by_path(root, path) -> Node` | 按索引路径定位节点（如 `"0/2/1"`） |
| `get_widget_props(w) -> Json` | 获取 Widget 属性快照（`describe` + `serialize_props`） |
| `set_widget_prop(w, key, value)` | 单属性回写（经 `deserialize_props`） |

`dump_tree_rich` 输出形态：

```text
Column#root { bounds:[0,0,640,480]; visible:true; listeners:[on_click] }
├─ Text#title { bounds:[20,20,200,24]; text:"Hello"; style:{font_size:24} }
└─ Button#ok  { bounds:[20,52,88,36]; text:"Click"; listeners:[on_click] }
```

`Node` 标识由 `Widget::set_id(std::string_view)` / `id()` 提供，`dump_tree_rich` 经 `#id` 渲染。

`aurora::Inspector`（`inspector/inspector_api.h:25`，实现 `src/aurora/inspector/inspector_api.cpp`）是操作 UI 树的统一编程门面：全静态方法、仅主线程，各方法委托上表自由函数或组件注册表，无新增运行时开销。除树导出（`tree_text` / `tree_rich` / `tree_json` / `tree_json_full`）外，还提供：

| 能力 | 成员 | 说明 |
|:---|:---|:---|
| 节点查询 | `query(type, root)` / `get_state(path, root)` / `find_node(root, path)` / `widget_info(w)` | 按类型名检索、按路径取状态片段、按索引路径定位节点、Widget 完整信息 |
| 属性读写 | `get_prop(w)` / `get_prop_value(w, key)` / `set_prop(w, key, val)` / `apply_patch(root, patch)` | 单属性回写返回 `Result<void>`；`apply_patch` 把 JSON Patch 逐条经 `set_prop` 应用到树 |
| 交互模拟 | `simulate_click(w)` / `simulate_scroll(w, dx, dy)` / `simulate_text_input(w, text)` | 事件系统尚未接线，当前三者均返回 `GeneralNotSupported` |
| 组件发现 | `components()` / `component_schema(name)` | 已注册组件 schema 列表 / 单组件 schema |
| 代码生成 | `to_code(root)` | UI 树 → 源码（转发 §2.5） |
| 验证 | `validate(root) -> std::vector<Diagnostic>` | 整树验证（`inspector_api.h:94`） |
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

### 5.1 REST 端点

| 方法 | 路径 | 说明 |
|:---|:---|:---|
| GET | `/api/tree` | 完整 widget 树 JSON |
| GET | `/api/widget/{path}` | 单 widget 属性 JSON（`path` 为索引路径如 `0/1/2`） |
| PUT | `/api/widget/{path}/{prop}` | 回写指定属性（请求体为 JSON 值） |
| GET | `/api/components` | 全部已注册组件 schema 列表 |
| GET | `/api/yaml` | 当前 widget 树的 YAML 格式字符串 |
| POST | `/api/to_code` | UI 树 → C++ 代码。请求体可含 `style` 参数：`0`=Fluent、`1`=StepByStep、`2`=DesignatedInit；`style` 存在但非整数返回 400，越界整数回退 Fluent |

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

### 7.2 CLI（`aurora_cli`）

```bash
aurora components                         # 列出所有已注册组件类型
aurora describe <name>                    # 输出单个组件的完整 schema（JSON）
aurora search <keyword>                   # 按名称搜索组件
aurora validate <tree.json>               # 校验 UI 树 JSON，输出诊断
aurora snapshot <tree.json> [-w W] [-h H] # 输出逻辑快照 JSON
aurora render <tree.json> [-w W] [-h H] [-o out.png]  # 离屏渲染为 PNG
aurora preview <tree.json> [-w W] [-h H]  # 快速预览 UI（启动临时窗口；无显示后端回退无头渲染一帧退出）
aurora to-code <tree.json> [--style fluent|step|di]   # UI 树 → C++ 代码
aurora to-yaml <tree.json>                            # UI 树 → YAML 格式
aurora schema                             # 输出完整 aurora_api.json
```

退出码：成功 `0`，校验失败 `1`，用法错误 `2`。所有输出默认 JSON（机器可读）。

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

### 7.4 生成器与校验工具

| 目标 | 说明 |
|:---|:---|
| `gen_api_tools` | 生成 `aurora_api.json`（schema / 类型 / 属性键 / 枚举）。新增或删除 widget / 类型后须重跑 |
| `generate_error_codes` | 读 [`errors.toml`](../errors.toml) → 生成 `error_codes.gen.h`、[`ERROR_CATALOG.md`](../ERROR_CATALOG.md) 与 `aurora_api.json` 的错误段 |
| `gen_debug_api_json` | 读 [`debug_api.toml`](../debug_api.toml) → 更新 `aurora_api.json` 的 `"debug"` 段 |
| `ai_compat_test` | AI 兼容性批量验证：遍历 `tests/fixtures/ai_compat/` 下的 JSON fixture（`valid_*` 期望通过、`error_*` 期望报错），无 LLM 调用 |

---

## 8 测试原语（`aurora::test`）

`include/aurora/test_helpers.h`（**不进 `aurora.h`**，需显式包含）：

| 原语 | 说明 |
|:---|:---|
| `init_headless(w, h)` | 初始化无头环境 |
| `pump(env)` | 确定性 mount + layout（内部即 `render_to_logical_snapshot`） |
| `tap(env, widget)` | 合成 `MouseEvent` Press + Release |
| `type_text(env, widget, text)` | 合成 `TextInputEvent` |
| `expect_text` / `expect_tree_contains` / `expect_bounds` / `expect_visible` / `expect_count` | 断言辅助（内部统一 `TCHECK*` 宏族，依赖 `tests/test_harness.h`） |

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

- **MCP Server（`aurora_mcp`）**：stdio JSON-RPC 2.0，暴露 10 个 MCP tools。
- **CLI（`aurora_cli`）**：子命令 `components` / `describe` / `search` / `validate` / `snapshot` / `render` / `preview` / `to-code` / `to-yaml` / `schema`。
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
- 与 #17 CLI 集成：`aurora to-code tree.json --style fluent`。
- `InspectorPanel` 已支持导出代码：`export_code()` + 「Export Code」按钮 + `on_export_code` 回调，实现 Inspector → 代码闭环。
