# H.16 自描述发现 + H.17 MCP/CLI + H.18 偏好 + H.19 性能

> 本文件是「三、特性详细规范」子文档，覆盖 **§H.16**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #H.16 组件运行时自描述与发现 API

每个控件提供编译期确定的完整元数据，供 AI Agent / 工具链在运行时动态发现可用组件及其属性。

**核心数据结构**（`include/aurora/widget/descriptor.h`）：

```cpp
struct PropDescriptor {
    std::string name;          // "label"
    std::string type;          // "LocalizedString"
    std::string default_value; // "\"\""（字符串化默认值）
    bool required = false;
    std::string note;          // 可选说明
};

struct WidgetDescriptor {
    std::string name;              // "Button"
    std::string ns = "aurora";
    std::vector<PropDescriptor> properties;
    std::vector<std::string> events;        // ["on_click"]
    std::string children_policy;            // "none" | "single" | "multiple"
    std::vector<std::string> examples;      // 构造示例
};
```

**Widget 基类接口**（`include/aurora/widget/widget.h`）：

```cpp
/// 子类以 static describe_static() 提供编译期可访问版本，此虚函数供运行时多态调用（非纯虚：基类默认返回 {.name = type_name()}）。
[[nodiscard]] virtual auto describe() const -> WidgetDescriptor;
```

**组件发现 API**（`include/aurora/widget/serialization.h`）：

| API                          | 说明                                                                                                                                  |
|:-----------------------------|:--------------------------------------------------------------------------------------------------------------------------------------|
| `component_schema(type)`     | 返回完整 JSON schema，含 `prop_descriptors`、`events`、`children_policy`、`examples` 增强字段（向后兼容原有 `props`/`default_props`） |
| `list_all_schemas()`         | 返回所有已注册组件的完整 schema 列表（`std::vector<Json>`）                                                                           |
| `describe_component(type)`   | 同 `component_schema`，公共别名                                                                                                       |
| `search_components(keyword)` | 按名称模糊搜索已注册组件                                                                                                              |

> **发现 API 命名说明**：`list_all_components()` 与 `list_all_schemas()` 是两个**真实且不同**的函数，并非笔误——`list_all_components()` 返回 `std::vector<std::string>`（仅组件类型名列表，供 MCP `list_components` 工具消费），`list_all_schemas()` 返回 `std::vector<Json>`（完整 schema，供 MCP `get_schema` 工具消费）；二者并存，无需统一。

**`children_policy` 语义**：

| 值           | 含义                 | 典型控件                          |
|:-------------|:---------------------|:----------------------------------|
| `"none"`     | 叶控件，不接受子节点 | Button、Text、TextInput、Checkbox |
| `"single"`   | 单子容器             | Scroll、Show                      |
| `"multiple"` | 多子容器             | Column、Row、Stack、Grid          |

**JSON 序列化**：`descriptor_to_json(WidgetDescriptor)` / `descriptor_to_json(PropDescriptor)` 输出标准 JSON，供
`gen_api_tools` 消费生成 `aurora_api.json`。

#### #H.17 MCP Server 与 CLI 工具链

AI Agent 集成工具，消费 §H.16 自描述元数据与序列化/渲染/代码生成 API。

**MCP Server（`aurora_mcp`，stdio JSON-RPC 2.0）：**

| MCP Tool             | 输入                             | 输出             | 消费的库 API                   |
|:---------------------|:---------------------------------|:-----------------|:-------------------------------|
| `list_components`    | 无                               | 组件类型名列表   | `list_all_components()`        |
| `describe_component` | `{name}`                         | 完整 schema JSON | `describe_component(name)`     |
| `search_components`  | `{query}`                        | 匹配组件列表     | `search_components(query)`     |
| `validate_tree`      | `{tree}`                         | 校验结果         | `from_json()` + `validate()`   |
| `render_snapshot`    | `{tree, width?, height?}`        | 逻辑快照 JSON    | `render_to_logical_snapshot()` |
| `render_png`         | `{tree, width?, height?, path?}` | PNG 文件路径     | `render_to_png()`              |
| `to_code`            | `{tree, style?}`                 | C++ 代码         | `to_code()`                    |
| `to_yaml`            | `{tree}`                         | YAML 格式字符串  | `serialization::to_yaml(Json)` |
| `get_schema`         | 无                               | 完整 API schema  | `list_all_schemas()` + enums   |

MCP 协议方法：`initialize` / `tools/list` / `tools/call` / `ping`。 传输：stdin/stdout，
`Content-Length: <N>\r\n\r\n<JSON-RPC 2.0 body>`。

**CLI 工具（`aurora_cli`）：**

```bash
aurora components                         # 列出所有已注册组件类型
aurora describe <name>                    # 输出单个组件的完整 schema（JSON）
aurora search <keyword>                   # 按名称搜索组件
aurora validate <tree.json>               # 校验 UI 树 JSON，输出诊断
aurora snapshot <tree.json> [-w W] [-h H] # 输出逻辑快照 JSON
aurora render <tree.json> [-w W] [-h H] [-o out.png]  # 离屏渲染为 PNG
aurora to-code <tree.json> [--style fluent|step|di]   # UI 树 → C++ 代码
aurora to-yaml <tree.json>                             # UI 树 → YAML 格式
aurora schema                             # 输出完整 aurora_api.json
```

退出码：成功 0，校验失败 1，用法错误 2。所有输出默认 JSON（机器可读）。

**LSP 服务（`aurora_lsp`，stdio JSON-RPC 2.0）：**

| LSP 方法                                           | 说明                                                                                               |
|:---------------------------------------------------|:---------------------------------------------------------------------------------------------------|
| `initialize` / `initialized` / `shutdown` / `exit` | 生命周期                                                                                           |
| `textDocument/didOpen` · `didChange` · `didClose`  | 文档同步；`didOpen`/`didChange` 后发布 `textDocument/publishDiagnostics`                           |
| `textDocument/completion`                          | `au::` 后补类型/枚举；`XxxProps{` 内 `.` 后补属性（显示默认值/必填/文档）；枚举属性 `=` 后补枚举值 |
| `textDocument/hover`                               | `au::Type` 组件概要（属性数/事件/示例）；`.prop` 类型/默认值/必填/文档                             |
| `textDocument/codeAction`                          | 为缺失必填属性生成「补全缺失必填属性」快速修复（在 `}` 前插入 `.prop = <default>`）                |

传输：stdin/stdout，`Content-Length: <N>\r\n\r\n<JSON-RPC 2.0 body>`（与 MCP 一致）。 schema 来源：库 live API（
`describe_component` + `known_enums`），不读取 `aurora_api.json` 文件，始终与代码同步。

#### #H.18 偏好配置（Preferences / aurora::preferences）

轻量持久化键值配置（对标 Android `SharedPreferences` / iOS `UserDefaults`），以 **单个 JSON 文件**为后端。 属
**状态/存储层扩展**， **不新增任何 UI 控件**。

- **存储位置在初始化时显式指定**：
    - `Preferences()` —— 内存模式（不绑定文件），所有写入仅存内存；`flush`/`reload` 返回 `prefs-not-persistent` 错误。
    - `Preferences(std::filesystem::path file, Options = {})` / `Preferences::at(path)` /
      `Preferences::with_location(name, dir = default_config_dir())` —— 文件模式，构造即加载该 JSON 到内存。
    - `default_config_dir()` 经 `XDG_CONFIG_HOME`/`LOCALAPPDATA`/`HOME` 解析平台默认目录（不依赖窗口后端）。
- **单例（按名注册表，线程安全懒构造）**：`Preferences::instance(name = "app")`（默认名，文件模式、平台默认目录）/
  `instance(name, dir)` / `instance_at(name, file)`。每个 `name` 全局唯一，首次调用按参数创建并缓存，后续同名调用返回同一实例（忽略路径参数）；创建由静态
  `std::mutex` 保护，多线程安全。内存模式与测试仍可用普通构造器。
- **双写语义**：指定文件位置后，数据同时存在于内存与文件。采用业界主流的 **显式提交**模型——`set` 仅更新内存 JSON 与响应式
  `State`（并通知订阅者）， **不**自动写文件；落盘由 `flush()` **主动刷新**完成（对标 `SharedPreferences.edit().commit()` /
  `Settings.Save()`）。
- **响应式桥接**：`watch<T>(key, fallback) -> std::shared_ptr<State<T>>` 惰性创建并缓存该键 `State`；
  `binding<T>(key, fallback) -> Binding<T>` 提供非拥有的响应式绑定，供 `Switch`/`TextInput`/`Slider` 直接绑定。
    - 约定：`Binding` 的 `set` 只更新内部 `State`（响应式层）；要将值写入配置存储并落盘，调用方在控件 `on_change` 中执行
      `prefs.set(key, value); prefs.flush();`（见 `examples/demos/demo_preferences.cpp`）。
    - **Binding 删除路径（可靠语义）**：`Preferences::binding(key, fallback)` 返回的 `Binding<T>` 已注入删除回调（封装
      `remove(key)`，走墓碑可靠删除）。控件卸载/失效时可调用 `binding.remove()` 删除对应持久化键；纯 `State` 绑定（未注入回调）的
      `remove()` 为安全空操作，可通过 `removable()` 查询是否可删除。调用 `remove()` 后该 Binding 即失效（其上游 `State`
      可能被销毁），不应再 `get`/`set`。多进程下删除经墓碑持久化与传播，最终一致（见 #H.18）。
- **并发安全**：
    - **线程安全**：实例内部以 `std::shared_mutex` 保护内存 JSON 与 `State` 注册表——读操作（`get`/`contains`/`keys`/
      `watch`/`last_load_error`）走共享锁，写操作（`set`/`remove`/`clear`/`flush`/`reload`）走独占锁，可在多线程下安全并发读写；
      `set` 在释放锁后再向订阅者推送，避免重入死锁。
    - **进程安全**：`flush`/`reload` 期间对 `<file>.lock` 加跨平台 advisory 文件锁（Windows `LockFileEx` 独占 / POSIX
      `flock(LOCK_EX)`），`reload` 用共享锁。写盘采用「 **进程唯一的临时文件**（含 PID）+ 原子 `rename`
      」，避免多进程共用同一临时文件互相覆盖与半写损坏。`flush` 在持有独占锁后会 **先合并磁盘上其他进程已写入的键**
      （本进程内存仅含自身修改 + 加载快照）再写回，解决多进程 read-modify-write 竞争（其他进程的键不被全量覆盖丢失）。删除键采用
      **版本化 LWW + 墓碑（tombstone）+ 全局清空纪元**的可靠语义（非 best-effort）：
        - **墓碑传播**：`remove(key)` 写入墓碑（删除时间戳）并随 `flush` 持久化；其他进程在下次 `flush`/`reload`
          时学习到该墓碑并同步删除，墓碑持续保留以阻止其内存中的旧副本复活——因此删除在多进程下最终一致、可靠。
        - **版本化 LWW**：`set(key, v)` 记录写入版本（时间戳），`flush`/`reload` 时按版本做最后写入者胜出；`remove` 后 `set`
          同一键会取消墓碑并重建（重新创建不受删除影响）。
        - **全局清空纪元**：`clear()` 置全局清空纪元（时间戳），所有版本早于该纪元的键在各进程下次 `flush`/`reload`
          时被删除；清空之后新 `set` 的键（版本晚于纪元）不受影响。`clear` 是全局操作，会清掉所有已知键（含其他进程持有的键，只要其版本早于纪元）。
        - 元数据（versions / tombstones / cleared_at）随配置 JSON 一同落盘，存于保留键 `__aurora_prefs_meta__`
          （用户键空间应避免使用该名）；旧格式（无该键）仍可兼容加载。
- **持久化**：`flush() -> Result<void>`（内存 → 文件，仅文件模式有效）、`reload() -> Result<void>`（文件 → 内存并通知所有订阅者）。
- **读**：`get<T>(key, fallback) -> T`（类型不匹配/缺失回退 `fallback`）。支持 `bool`/整数/浮点/`std::string`/`std::vector`
  /JSON 对象。
- **批量**：`keys()` / `contains(key)` / `remove(key)` / `clear()`。
- **错误模型**：文件 IO 经 `core/result.h` 的 `Result<T>`（错误码 `prefs-not-persistent`/`prefs-open-failed`/
  `prefs-parse-failed`/`prefs-write-failed`），统一失败路径。

```cpp
#include "aurora/aurora.h"
using namespace aurora;
using namespace aurora::preferences;

// 应用初始化时显式指定配置存储文件位置（也可传绝对路径给 Preferences(path)）
// 单例：同名全局唯一，线程安全懒构造
Preferences &prefs = Preferences::instance("app", Preferences::default_config_dir());

// 订阅：控件可绑定 watch/binding 返回的 State
auto dark = prefs.watch<bool>("dark_mode", false);

prefs.set("volume", 7);          // 写入内存 + 响应式 State（不写文件）
auto r = prefs.flush();          // 主动刷新到文件 → Result<void>（进程锁 + 原子写）
if (!r.ok()) { /* r.error() 含 code / message / hint */ }

prefs.reload();                  // 重新从文件加载并通知订阅者
```

- **键值分组（group）**：在现有扁平 API 之上叠加命名分组，把相关配置组织到嵌套 JSON 对象中持久化，且分组与扁平键可共存于同一实例/文件。
    - `group(name) -> Group` 返回作用域子视图：`Group` 暴露与 `Preferences` 同款的
      `get/set/watch/binding/contains/keys/remove/clear`，且可链式嵌套（
      `prefs.group("ui").group("editor").set("font", 14)`）。`Group` 是轻量视图，须在其所属 `Preferences` 实例存活期内使用。
    - 存储：分组键以 **复合点号路径**（`"ui.theme"`、`"ui.editor.font"`）索引，文件内表现为嵌套 JSON 对象（
      `{"ui":{"theme":"dark","editor":{"font":14}}}`）；顶层扁平键（如 `"theme"`）语义不变，旧版扁平文件仍可正确加载（向后兼容）。
    - **分组删除/清空的可靠语义**：`Group::remove(key)` 与 `Group::clear()` 沿用版本化 LWW + 墓碑 + 全局清空纪元——
      `remove` 对单个分组键打墓碑；`clear` 对该子树所有已知复合键打墓碑（等效逐键可靠删除）， **不影响其他分组与顶层键**。删除随
      `flush` 持久化并在多进程下最终一致。`Group::binding(key, fb)` 注入的删除回调同样走分组墓碑路径。
    - 并发安全：`Group` 方法加的是所属 `Preferences` 的同一把 `std::shared_mutex`，与根 API 共用锁，无死锁与状态分裂；`set`
      锁外推送 `State` 的既有模式不变。

```cpp
// 把界面偏好分组到 "ui" 命名空间（嵌套持久化）
auto ui = prefs.group("ui");
ui.set("theme", std::string("dark"));
ui.set("font_size", 14);

// 控件直接绑定分组内的键
auto b = ui.binding<bool>("dark_mode", false);
b.set(true);          // 更新内存 State
prefs.flush();        // 落盘 → 文件内 {"ui":{"theme":"dark","font_size":14,"dark_mode":true}}

// 链式嵌套分组
prefs.group("ui").group("editor").set("font", std::string("Mono"));

// 仅清空 ui 分组（不动其他分组/顶层键）
ui.clear();
```

#### #H.19 性能检测体系（Performance Profiling）

核心目标：提供帧级运行时指标采集与可视化能力，帮助开发者定位渲染瓶颈与帧率异常。零外部依赖，默认关闭、按需启用。

**FrameStats API 契约**（`include/aurora/app/perf_overlay.h`）：

```
record(dt_seconds)         — 记录帧耗时（秒），dt > 100ms 自动视为 idle 段
record_idle()              — idle 跳帧计数
record_phases(l,p,pr)      — 分阶段计时（毫秒）
fps() -> double            — 滑动窗口平均 FPS
avg_frame_ms() -> double   — 滑动窗口平均帧时间（毫秒）
worst_frame_ms() -> double — 窗口内最差帧时间（毫秒）
jitter_ms() -> double      — 帧时间标准差（毫秒）
percentile_ms(p) -> double — 百分位帧时间（P50/P95/P99）
frame_at(i) -> double      — 第 i 帧帧时间（秒），i=0 最新
window_size() -> size_t    — 有效帧数
dropped_frame_count() -> size_t — 掉帧计数
dropped_frame_ratio() -> double — 掉帧率
hitch_count() -> size_t    — hitch 计数
idle_frame_count() -> size_t — idle 帧计数
set_frame_budget_ms(ms)    — 设置帧预算（默认 16.67ms）
avg_layout_ms() -> double  — 平均 layout 耗时
avg_paint_ms() -> double   — 平均 paint 耗时
avg_present_ms() -> double — 平均 present 耗时
reset()                    — 清空所有状态
```

**PerfLog API 契约**（`include/aurora/perf/perf_log.h`）：

```
enable(interval_frames=300) — 启用定期日志
disable()                   — 禁用
enabled() -> bool           — 查询状态
on_frame_end()              — 帧结束调用
snapshot_json() -> string   — JSON 快照
snapshot_csv() -> string    — CSV 快照
```

**Window 新增 API**（`include/aurora/window/window.h`）：

```
is_idle_frame() -> bool     — 当前帧是否为 idle 跳过帧
```

设计要点：

- **环形缓冲区 O (1) 采集**：`FrameStats` 为进程级单例，固定 128 帧缓冲，写入与查询均为 O (1) 或 O (N)（N=128 常数）。
- **分阶段计时**：`present_root()` 内部对 layout / paint / present 三阶段独立计时，64 帧环形缓冲，经 `record_phases` 写入
  `FrameStats`。
- **Idle 帧隔离**：脏区跳帧（§5）经 `is_idle_frame()` 判定，调用 `record_idle()` 单独计数，不进入渲染帧统计窗口，避免 FPS /
  帧时间 / 掉帧率被空闲帧污染。
- **PerfOverlay 叠加**：右上角实时显示多行统计文本 + FPS 颜色告警 + 帧时间条形图，经 `PerfOverlay::enable()` / `disable()`
  按需开关。
- **日志导出**：`PerfLog` 定期（默认每 300 帧）输出指标摘要到 `Log` 子系统，并提供 `snapshot_json()` / `snapshot_csv()`
  快照接口供外部工具消费。

**性能优化效果摘要**（内部实现优化，公共 API 无变化）：

本轮优化覆盖三个优先级层次，均为内部实现改进，不引入新公共 API、不改变既有 API 语义：

- **P0 文本渲染热点**：GlyphAtlas LRU 驱逐从 O (n) 降至 O (1)（`std::list` + `std::unordered_map<key, iterator>`）；
  `make_key` 采用 `uint64_t` 紧凑 key 替代 `std::string`，消除每字形堆分配；`resolve_faces` 结果经 `static unordered_map`
  缓存，重复字形查询跳过字体解析。
- **P1 帧循环与布局**：`wire_dirty` 条件化（仅树结构变化时执行，`for_each_child` 消除 vector 副本）；Button 度量与
  `resolved_text` 在 `on_layout` 时缓存、`on_paint` 直接使用；`tick_gestures` 按需遍历注册手势计时器的 widget；`FlexItem`
  以函数指针 + `void*` 上下文替代 `std::function`，消除堆分配。
- **P2 渲染管线快速路径**：`blend_pixel` 纯矩形裁剪跳过 SDF coverage 遍历；Modifier 以 `kind()` switch 替代 `dynamic_cast`
  ，消除 RTTI 开销；`fill_rect` 圆角按行计算 x 范围、行内快速填充。
- **光栅内核 SIMD 双实现**：`include/aurora/render/detail/painter_simd.inl` 提供标量黄金 `gradient_*_scanline_scalar` 与
  SSE2/AVX2 快路径 `gradient_*_scanline_sse2` / `_avx2`；`gradient_linear_fill` / `gradient_radial_fill`（`painter_simd.h`
  ）按 `g_simd_level` 运行时分发（AVX2→SSE2→标量尾补）。仅作用于不透明双色标渐变（两端 `alpha==255`），`draw_linear_gradient` /
  `draw_radial_gradient` 经 `simd_grad` 门控调用。 **全部 detail 内部 API，不进入 `aurora_api.json`**；与标量黄金逐位一致（见
  `ARCHITECTURE.md` §10.6 与 `BUILD_OPTIONS.md` §3.2）。

> 所有快速路径必须与慢路径逐位一致（golden 零差异），SIMD 双实现另须通过 `test_simd_parity` 逐位比对（G-13 一票否决），详见
> `ARCHITECTURE.md` §10.6。

**渲染计数器 `RenderCounters`**（`include/aurora/perf/counters.h`）—— 进程级单例，逐帧累加的渲染成本信号：

```
RenderCounters::current()      -> RenderCounters&  进程级单例（持续整个会话）
draw_calls / fill_rects / draw_texts / glyphs_rendered   — 绘制原语计数
pixels_filled / glyph_cache_*(hits|misses) / shape_cache_*(hits|misses) — 像素与缓存
dl_records / dl_replays        — 显示列表录制/重放
layout_nodes / paint_nodes     — 遍历节点数
relayout_boundaries_hit        — 命中 RelayoutBoundary 次数（优化验收用）
dirty_rect_count / dirty_area_ratio — 脏区统计（ratio 为 [0,1] 累加值，需自身除以帧数）
full_redraw (bool)             — 当帧是否整帧重绘
scroll_buffer_bytes (uint64)   — Scroll 离屏内容缓冲常驻字节数（RGBA8888，按帧累加，跨帧取峰值）
add(o) / merge_max(o)          — 逐字段累加 / 取峰值
reset()                        — 全部归零（保留不了任何状态）
to_json() / to_csv_row() / csv_header()  — 序列化（CSV 表头与数据行列数严格一致）
```

- **埋点宏（编译期剪裁）**：`AURORA_PROFILE_COUNT(field, expr)` / `AURORA_PROFILE_SET(field, expr)`。在
  `AURORA_ENABLE_PROFILING` 关闭时展开为 `((void)0)`—— **宏参数一次都不求值**，故参数不得带副作用；开启时参数恰好求值一次并写入
  `current()`。可用 `constexpr bool profiling_enabled()` 在 `if constexpr` 中分流两分支行为（计数器恒 0 vs 累加）。
- 典型埋点：`Scroll::on_paint` 在确保离屏内容缓冲 `m_content` 就绪后写入 `scroll_buffer_bytes = w*h*4`（RGBA8888
  常驻字节数），作为滑窗滚动缓冲优化的验收锚点（门槛 G-8）。

**作用域计时器 `Profiler`**（`include/aurora/perf/profiler.h`）—— 进程级单例，帧内 zone 计时 + 长任务归因：

```
Profiler::instance()           -> Profiler&           进程级单例
begin_frame() / end_frame()    — 帧协议：begin 清空当帧样本，end 推进 frame_index
begin_zone(name) / end_zone()  — 嵌套 zone（按闭合顺序记录，inner depth 更大）
aggregate(name) / aggregates() — 单 zone 聚合（call_count/total_ms/max_ms）/ 全量（按总耗时降序）
long_tasks() / total_long_task_count()  — 当帧长任务 / 跨帧累计（阈值由 set_long_task_threshold_ms 控制）
set_zone_capacity(n)           — 有界内存：超出即丢弃并计数（dropped_zones）
set_enabled(bool)              — 运行时开关（关闭后 begin/end 成对忽略，不误报配对错误）
reset()                        — 清状态、保留容量/阈值配置
report_text()                  — 文本报告
ScopedTimer{name} / FrameScope — RAII：析构产生样本 / 同时推进帧序号并清零 RenderCounters
```

- 与计数器不同，`Profiler` 的成员函数 **始终参与编译与链接**，只有 `AURORA_PROFILE_*` 宏在宏关闭时被裁掉；故直接调用类接口可在
  ON/OFF 两种构建下一致运行。
- 配对错误（`end` 多于 `begin`、帧末未闭合 zone）经 `unbalanced_zones()` 计数，不跨帧累积。

**时间线 `TraceWriter`**（`include/aurora/perf/trace_writer.h`）—— 导出 Chrome `about:tracing` / Perfetto 可读的 Trace
Event JSON：

```
TraceWriter::instance()        -> TraceWriter&
begin_capture() / end_capture()/ clear()  — 录制开关 / 清空（保留容量配置）
add_complete_event / add_instant_event / capture_counters / capture_frame(Profiler&)
to_json() / write_json(path)  — 导出（合法 JSON 数组；write_json 空路径返回 Result 失败而非崩溃）
capacity() / event_count() / dropped_events() / counter_sample_count()  — 有界内存与丢弃统计
```

- 事件相位：`M`（元数据进程/线程名）/ `X`（Complete，ts/dur 以 **微秒**写出，内部 ms×1000）/ `i`（Instant）/ `C`
  （计数器轨道，full_redraw 以 0/1 写出）。
- `AURORA_ENABLE_TRACING` 关闭时 `TraceWriter` 仍可显式驱动（只是 `FrameScope` 不自动喂数据）；开启时 **强制**打开
  `AURORA_ENABLE_PROFILING`。
- 受 `constexpr bool tracing_enabled()` 编译期控制。

**计时原语 `Stopwatch`**（`include/aurora/perf/stopwatch.h`）：`elapsed_ns/us/ms()`（读数自洽、单调不减、非负）、`reset()`、
`lap_ms()`（取读数并重置）、`now_ms()`（进程级共享原点）。仅用于度量，断言不依赖其绝对精度。

**确定性滚动基准 `ScrollBenchHarness`**（`include/aurora/perf/scroll_bench.h`）—— 本专项的核心验收工具，用 Headless 表面 +
程序化滚轮事件序列对任一可滚树做确定性采样，并内置 **自证机制**防止「测了个寂寞」：

```
ScrollBenchHarness::run(root: Node, viewport: Size, cfg: Config) -> Result
Config { name; frames; warmup_frames; delta_per_frame(dp); fling; scale;
         settle_ms; settle_idle_frames; settle_max_frames }
Result 自证字段：
  scrollable_found / moved_frames(应==frames) / idle_frames(应==0)
  reversals / reversal_ratio()  (触边反向占比，阈值 Result::kMaxReversalRatio=0.10)
  max_offset (采样前行程) / max_offset_end (采样后复测行程)
  geometry_stable()  (两者差 < 0.5dp，事后证伪骨架屏中途退场)
  scroll_viewport_h (滚动容器自身视口高，≠ 窗口高，顶/底栏挤占)
  dp_per_unit (运行期标定：一个滚轮单位在被测控件上等于多少 dp)
  settle_reason (Disabled/Idle/TimeBudget=正常落定；FrameCap=未落定)
  trustworthy()  (上述全部满足才判可信；否则性能数不该采信)
  p50/p95/p99/worst_ms、jitter、full_redraw_frames（转发自内嵌 PerfReport）
  counters_sum() / counters_max()  (RenderCounters 汇总/峰值)
  to_markdown() / to_json() / to_csv_row() / csv_header()
```

- 关键设计：`delta_per_frame` 单位是 **dp**（非滚轮单位），运行期先标出 `dp_per_unit`（因 `Scroll::step`/`LazyList`/
  `GridView` 步长各异），再换算回控件滚轮单位下发，保证跨场景可比； **落定判据**接受「连续无脏」 **或**「墙钟达标」任一（永动动画树如自动轮播
  banner 原理上永不 idle，须靠 TimeBudget 收敛）；只有 `FrameCap`（撞帧数上限）才算未落定。
- 验收口径：看 `p99_ms` / `jitter_ms` / `full_redraw_frames` 三项， **不看 avg_frame_ms**。
- 配套 CLI：`tools/bench_scroll.cpp`（`--scene / --delta(dp) / --repeat / --format csv|md`），对 `demo_google_play`
  内容树等真实业务树做基线采集；严格时间口径须 **独立进程多次取最小**（同进程 27MB 离屏缓冲反复申请/释放会产生堆碎片系统性偏慢，首样本最干净）。
