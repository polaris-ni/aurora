# H.xx 日志 + H.yy AI-First

> 本文件是「三、特性详细规范」按功能域/子系统划分出的子文档；返回主线索引见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。
> 前置核心子系统章节（H.1–H.10c）见 [`../subsystems/`](../subsystems)：SUBSYSTEM_SIGNAL_MODIFIER / SUBSYSTEM_ANIMATION / SUBSYSTEM_ENV_THEME / SUBSYSTEM_EVENT_FOCUS_NAV / SUBSYSTEM_RENDER_HEADLESS / SUBSYSTEM_APP_WINDOW / SUBSYSTEM_PLATFORM_SHELL。
> 相关功能域规范（A–G）见 [`../features/`](../features)：FEATURE_API_DESIGN / FEATURE_ARCH_STATE / FEATURE_RUNTIME_SAFETY / FEATURE_LAYOUT_RENDER / FEATURE_CROSS_PLATFORM / FEATURE_AI_INSPECTION / FEATURE_AI_TOOLING / FEATURE_ENGINEERING。

### H.xx 日志（Log）子系统

> 头文件：`include/aurora/core/log.h` + 实现 `src/aurora/core/log.cpp`；经 `include/aurora/aurora.h` 暴露（
> `#include "aurora/core/log.h"`）。
> 编码纪律见 `CODING_STANDARDS.md` §4 三.6： **禁止直接使用标准输出**，所有输出须经本子系统收口。

**双通道设计**：

1. **诊断通道 `AURORA_LOG_*`**：`AURORA_LOG_{TRACE,DEBUG,INFO,WARN,ERROR,FATAL}(category, ...)` 宏，经 `Logger::log` 写
   **默认 stderr**；统一前缀 `[YYYY-MM-DD HH:MM:SS][级别][module@threadId filename:line] > content`，其中 `module`=
   `category`、`threadId`=当前线程 id（`std::this_thread::get_id()`），自动带 file:line，受 `set_level`（默认 `Info`）与
   `set_enabled` 阈值过滤。级别 `LogLevel::{Trace,Debug,Info,Warn,Error,Fatal}`。
2. **功能输出通道 `AURORA_LOG_RAW`**：`AURORA_LOG_RAW(category, ...)` 宏，经 `Logger::raw` 写 **默认 stdout**；
   **无前缀、不受级别阈值/启用开关限制、始终输出**，且 `default_raw_sink` 调 `std::fflush(stdout)` 保证 LSP/MCP 等 stdio
   线协议逐条即时送达。用于 CLI 的 JSON 结果 / usage 文本、benchmark 表格、`Content-Length:`协议帧等「程序产品」输出（区别于诊断日志，避免污染下游解析）。

**重定向与桥接**：

- `Logger::set_sink(LogSink)` / `set_raw_sink(LogSink)`：把两通道重定向到文件或测试捕获；传 `nullptr` 恢复默认（stderr /
  stdout）。
- `test_printf` / `test_printf_err` 宏：先把 `printf` 风格经 `std::snprintf` 写入 **内存缓冲**（非标准输出），再经
  `AURORA_LOG_INFO/ERROR("test", ...)` 输出，作为遗留诊断代码的兼容桥接； **新代码请直接用 `AURORA_LOG_*` /
  `AURORA_LOG_RAW`**，勿新增 `printf` 调用。

**invariant**：库代码唯一允许直接触达标准输出之处是 `log.cpp` 内 `default_sink` / `default_raw_sink` 的 sink 实现（含
`init_console()` 的 UTF-8 代码页设置）；其余源码（含 `tools/`、`examples/`、`tests/`）一律走日志接口。

---

### H.yy AI-First 便利性层

> 增量叠加在既有声明式 `Node`/`Props` 架构之上； **不改动**软件 `Painter`、单一静态库、声明式树内核。使用配方见
> `GUIDELINE.md` §18–§19。

**`aurora::ui` 声明式工厂（`include/aurora/ui/factories.h`，经 `aurora.h` 暴露）**：`label` / `button` / `input` /
`checkbox` / `slider` / `vbox` / `hbox` / `stack` / `grid` / `scroll` 十个自由函数，签名 `(Container& parent, ...)`；构造控件、
`push_back` 到父 `m_children`（`Container::add`）、返回强类型裸指针（`Text*`/`Button*`/`Column*`…）。`vbox`/`hbox` 分别映射到
`Column`/`Row`。容器类第二参数为既有 `XxxProps`（默认 `{}`），`label`/`button` 首参为主文案覆盖对应 Props 字段。指针生命周期由父树
`shared_ptr` 持有。

**`Node` 标识（`include/aurora/widget/widget.h`）**：新增 `set_id(std::string_view)` / `id()`；`dump_tree_rich` 经 `#id` 渲染。

**富格式 dump（`include/aurora/widget/inspect.h`）**：`dump_tree_rich(const Node&, int depth=0, bool tree_chars=true)` 输出规范
#10 形态——`Type#id { bounds:[x,y,w,h]; visible:..; text:".."; style:{..}; listeners:[..] }`，容器递归子节点并以 `├─ └─ │`
连接。既有 `dump_tree` / `dump_tree_json*` 保持不变（向后兼容）。

**`aurora::test` 测试原语（`include/aurora/test_helpers.h`，不进 `aurora.h`）**：`init_headless(w,h)` / `pump(env)`（=
`render_to_logical_snapshot`，确定性 mount+layout）/ `tap(env, widget)`（合成 `MouseEvent` Press+Release）/
`type_text(env, widget, text)`（合成 `TextInputEvent`）/ `expect_text` / `expect_tree_contains` / `expect_bounds` /
`expect_visible` / `expect_count`（内部统一 `TCHECK*`，依赖 `tests/test_harness.h`）。

**`Container` 运行时增子（`include/aurora/widget/widget.h`）**：新增 `add(const Node&)`（尾插 + `mark_needs_layout`）、
`child(size_t)`（可变访问，含设 `id`）、`child_count()`；`SingleChild::set_child(Node)`。

---
