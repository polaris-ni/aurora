# H.15b Inspector 面板 + H.15c Inspector 远程

> 本文件是「三、特性详细规范」子文档，覆盖 **§H.15b**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #H.15b Inspector 面板（InspectorPanel）

> 新增 `InspectorPanel` 控件（`include/aurora/widget/inspector_panel.h`）：左右分栏布局，左侧树形浏览器展示 Widget
> 层级，右侧属性面板展示选中 Widget 的类型与属性值。支持分隔条拖拽调整比例、运行时属性回写。

**inspect.h 扩展函数**：

| 函数                                             | 说明                                                   |
|:-------------------------------------------------|:-------------------------------------------------------|
| `widget_tree_to_items(root) -> vector<TreeItem>` | Widget 树 → TreeItem 树（供 TreeView 消费）            |
| `dump_tree_json_full(root) -> Json`              | 含属性的完整 JSON 快照（每节点含 type/props/children） |
| `find_node_by_path(root, path) -> Node`          | 按索引路径定位节点（如 "0/2/1"）                       |
| `get_widget_props(w) -> Json`                    | 获取 Widget 属性快照（describe + serialize_props）     |
| `set_widget_prop(w, key, value)`                 | 单属性回写（经 deserialize_props）                     |

**InspectorPanel API**：

| 方法                                 | 说明                                               |
|:-------------------------------------|:---------------------------------------------------|
| `InspectorPanel(root_getter, ratio)` | 构造：接受目标树获取函数 + 左侧树占比              |
| `set_root(getter)`                   | 设置/更新目标树                                    |
| `refresh()`                          | 刷新树映射与属性面板                               |
| `on_select_widget`                   | 选中 Widget 回调                                   |
| `selected_widget()`                  | 当前选中 Widget 指针                               |
| `current_props()`                    | 属性名值对列表                                     |
| `export_code()`                      | 将当前 widget 树导出为 C++ 源码字符串              |
| `on_export_code`                     | “Export Code” 按钮点击回调，参数为生成的代码字符串 |

#### #H.15c Inspector 远程 HTTP 接口（InspectorServer）

> 新增 `InspectorServer`（`include/aurora/inspector/inspector_server.h`）：localhost-only HTTP 服务器，暴露 REST 端点供外部
> Inspector 工具远程访问运行时 widget 树。跨平台（Windows: `ws2_32` / POSIX: `pthread`），CMake 开关 `AURORA_BUILD_INSPECTOR_SERVER`（默认 OFF）。

**InspectorServer API**：

```cpp
#include "aurora/inspector/inspector_server.h"

// 构造：接受一个返回当前 widget 树的函数
InspectorServer server{ []() -> Node { return build_ui(); } };

server.start(6280);   // 启动 HTTP 服务器（默认端口 6280），后台线程运行
server.is_running();  // true
server.port();        // 6280
server.stop();        // 停止并 join 工作线程
```

| 方法                                                           | 说明                                                                                                                 |
|:---------------------------------------------------------------|:---------------------------------------------------------------------------------------------------------------------|
| `InspectorServer(root_getter)`                                 | 构造：接受返回 `Node` 的函数                                                                                         |
| `start(port=6280) -> bool`                                     | 启动 HTTP 服务器，后台工作线程运行，成功返回 true                                                                    |
| `stop()`                                                       | 停止服务器并 join 工作线程                                                                                           |
| `is_running() -> bool`                                         | 查询服务器是否运行中                                                                                                 |
| `port() -> uint16_t`                                           | 返回监听端口（未启动返回 0）                                                                                         |
| `set_surface_getter(std::function<Surface*()> getter) -> void` | 注入 Surface 获取器：调试端点（`snapshot` / `state` / `pick`）借此访问运行时 Surface；可选，未设置时这些端点返回 4xx |

**REST 端点**：

| 方法 | 路径                        | 说明                                                                                                                                     |
|:-----|:----------------------------|:-----------------------------------------------------------------------------------------------------------------------------------------|
| GET  | `/api/tree`                 | 完整 widget 树 JSON                                                                                                                      |
| GET  | `/api/widget/{path}`        | 单 widget 属性 JSON（`path` 为索引路径如 `0/1/2`）                                                                                       |
| PUT  | `/api/widget/{path}/{prop}` | 回写指定属性（请求体为 JSON 值）                                                                                                         |
| GET  | `/api/components`           | 全部已注册组件 schema 列表                                                                                                               |
| GET  | `/api/yaml`                 | 当前 widget 树的 YAML 格式字符串                                                                                                         |
| POST | `/api/to_code`              | UI 树 → C++ 代码（请求体可含 `style` 参数：0=Fluent, 1=StepByStep, 2=DesignatedInit；`style` 存在但非整数返回 400，越界整数回退 Fluent） |

**调试端点（收编 §H.10c `aurora::debug` 门面）**：以下端点需先 `set_surface_getter`（除纯 JSON 状态类外）；所有 Surface / 树 /
全局状态读取经 **主线程 marshal**（`marshal_get<T>` 复用 `aurora::detail::main_poster`，无事件循环时直接执行）后返回，与
`Surface` 的 main-thread-only 约束一致。

| 方法 | 路径                                 | 返回               | 说明                                                                                                                                                                                                  |
|:-----|:-------------------------------------|:-------------------|:------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| GET  | `/api/debug/state`                   | `application/json` | `aurora::debug::surface_state`（Surface 运行时状态；Release 返回 `available:false`）                                                                                                                  |
| GET  | `/api/debug/snapshot?source=fb\|win` | `image/png`        | `aurora::debug::capture` 写临时 PNG 后返回字节；`source=win` 为真实屏幕窗口（Headless/Wayland unsupported）；Release 返回 500                                                                         |
| GET  | `/api/debug/perf`                    | `application/json` | `aurora::debug::perf_snapshot`                                                                                                                                                                        |
| GET  | `/api/debug/timeline`                | `application/json` | `aurora::debug::frame_phase_timeline`（L/P/R 三相位 + 近帧 + ASCII flamegraph）                                                                                                                       |
| GET  | `/api/debug/diagnostics`             | `application/json` | `aurora::debug::diagnostics`                                                                                                                                                                          |
| GET  | `/api/debug/why`                     | `application/json` | `aurora::debug::why_trace`（重排 / 重绘因果链，含 `propagated` 根因/传播区分）                                                                                                                        |
| GET  | `/api/debug/tree`                    | `application/json` | `aurora::debug::widget_tree`（`aurora::debug` 门面，区别于基础 `/api/tree`）                                                                                                                          |
| GET  | `/api/debug/pick?x=&y=`              | `application/json` | `aurora::debug::widget_picker`：返回 `{ hit, chain:[{type_name,bounds}] }`；坐标取窗口逻辑 dp；Release 返回 `{hit:false,chain:[]}`                                                                    |
| POST | `/api/debug/flags`                   | `application/json` | 请求体为 `DebugPaintFlags` 子集 JSON，调用 `aurora::debug::set_flags` 实时开关叠层；返回 `{status:"ok", flags:{...}}`。请求体须为对象且字段值须为 boolean（类型不符返回 400；字段缺省保持默认 false） |

**线程安全说明**：`InspectorServer` 内部以 `std::mutex`（`tree_mutex`）保护 widget 树访问，`root_getter` 回调在 HTTP
工作线程中被调用；`/api/debug/*` 的 Surface / 树 / 全局状态读取经 `marshal_get<T>` 入队主线程后阻塞回收结果（无
`main_poster` 时直接执行，兼容测试 / 无头）。使用者应确保 `root_getter` 返回的 `Node` 是线程安全的（如每次返回新树，或在回调内加锁）。

**CMake 开关**：`AURORA_BUILD_INSPECTOR_SERVER`（默认 `OFF`），开启后编译 `aurora_inspector_server` 静态库，链接 `ws2_32`
（Winsock2）。详见 `BUILD_OPTIONS.md`。
