# CODING_ERRORS_NAMING

> 本文件由 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md) 划分而出（错误处理 / 命名 / 文档与示例 / 元数据与可观测 / 契约与表达 / 约束总结）。章节编号保持原样。
> 返回主线见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md)。

**本文包含章节：**

- [1. 错误处理（Error Handling，对应规格 §9）](#1-错误处理error-handling对应规格-9)
- [2. 命名（Naming，对应规格 §2）](#2-命名naming对应规格-2)
- [3. 文档与示例（对应规格 §12）](#3-文档与示例对应规格-12)
- [4. 元数据与可观测（对应规格 §21）](#4-元数据与可观测对应规格-21)
- [5. 契约与表达（对应规格 §23）](#5-契约与表达对应规格-23)
- [6. 约束总结（Invariants 链接）](#6-约束总结invariants-链接)

## 1. 错误处理（Error Handling，对应规格 §9）

- **统一错误类型**：全部可失败操作返回 `Result<T>`，不要用裸 `std::optional` 或抛异常表示业务错误。
- **构造即校验**：可失败构造（如 `ImageView::load`、`from_json`）提供 `make_*()` 工厂（如 `make_node`/`make_element`
  ），构造时即校验，非法输入立即返回 `Result` 而非延迟崩溃。
- **携带修复建议**：`Error` 必须包含 `suggestion` 字段（人类/AI 可读的「怎么改」）；`docs` 指向相关文档锚点。
- **0.3 部分生成容错**：AI 生成的局部代码片段（如单文件、单函数）应能独立编译；不要把必须的前置声明/类型散落在不可见的上文。
  `Error` 的 `suggestion` 直接给出缺失件。
- **0.3 调试可定位**：`Error::where` 精确到文件:行；`AURORA_ASSERT` 在 debug 触发并附上下文。

## 2. 命名（Naming，对应规格 §2）

- **名序一致（1.3）**：属性/参数名序与 React/Flutter 对齐（如 `on_click` 而非 `click_handler`）。
- **默认参数友好（1.4）**：高频构造提供默认参数 / 便捷工厂（如 `au::colors::Blue`、`dp(16)`），降低记忆负担。
- **顺序容错（1.5）**：多参数构造优先用 `XxxProps{...}` 具名聚合（如 `ColumnProps{.children=...}`），避免位置参数顺序错误。
- **强类型几何（1.6，规格 §4）**：`Length`/`Color`/`Size`/`Point` 为强类型；禁止 `Length(int)` 隐式转换（裸整数编译失败）。已采纳用户字面量
  `au::literals`（如 `100_dp`、`16_ms`、`0xRRGGBB_rgb`），`px(100)` 与 `100_dp` 互补； **禁止头文件全局 `using`，仅 TU 内
  `using namespace au::literals`**。
- **所有权清晰（1.7）**：资源所有权用 `unique_ptr`/`shared_ptr` 明确；跨边界传递用 `std::move`；`Binding<T>`
  为非拥有引用（上游生命周期须更长）。
- **常量命名（1.8）**：命名空间/文件级与类内 `static constexpr` 常量统一 `AURORA_` 前缀 + `UPPER_CASE` 全大写下划线（如
  `AURORA_DEFAULT_MAX_WIDGET_DEPTH`），与 `.clang-tidy` 的 `ConstantPrefix`/`GlobalConstantPrefix` 约束一致；禁止 `k` 前缀
  CamelCase（历史别名 `kDefaultMaxWidgetDepth`/`kDefaultMaxNavDepth` 已于 v0.25.0 移除）。
- **生命周期回调强类型（1.9）**：`au::Lifecycle` 的 `on_mount`/`on_unmount`、窗口级 `WindowState`/`WindowMode` 的 `set_on_*`
  回调均为具名 `std::function` 强类型（`MountCb = std::function<void(const BuildContext&)>`、
  `UnmountCb = std::function<void()>`、`WindowStateHandler`/`WindowModeHandler` 同理）；枚举取值穷尽且按「可见性 / 几何态」正交划分（
  `WindowState` 不并入 `Maximized`），AI 无需猜测「是否还有隐藏状态」。回调均可空（无副作用时不传），且不走异常捕获（与主线程事件回调一致）。

## 3. 文档与示例（对应规格 §12）

- **示例即文档（4.6）**：每个 widget 提供最小可编译示例，集中在 `examples/demos/`（`demo_<组件>.cpp`，1:1）与 `GUIDELINE.md`。
- **API 描述可机读（4.1）**：`gen_api_tools` 输出 `aurora_api.json`，含类型/属性键/枚举；供 LSP、文档生成器消费。
- **零平台魔法（4.7）**：示例不依赖特定平台 GUI 事件循环；`HeadlessSurface` 可离线渲染 PNG，便于测试与 AI 复现。
- **源—示例—测试 1:1 映射（4.8）**：每个公共源文件（widget/子系统头）原则上对应一个 `demo_*.cpp`（位于 `examples/demos/`）与一个
  `test_*.cpp`（位于 `tests/`），便于按需定位与独立验证。允许少量「复杂场景」demo/test（跨控件集成、端到端流程）作为例外，但须明确标注其跨源性质。顶层
  `examples/*.cpp` 的聚合/重复 demo 已清理，所有 demo 收敛到 `examples/demos/`（CMake 仅 `GLOB` 该目录，新增组件 demo
  放到此处即自动纳入构建，无需改 CMake）。
- **文件夹区分（4.9）**：demo 与 test 以目录区分——示例在 `examples/`（含 `examples/demos/`），测试在 `tests/`；二者不混放。
- **test 文件前缀（4.10）**：测试文件统一以 `test` 为前缀（`test_xxx.cpp`），与示例的 `demo` 前缀（`demo_xxx.cpp`）风格一致，便于
  GLOB 收集与一眼区分源/示例/测试三者。聚合多个不相关控件的「catch-all」测试文件视为反模式，应拆为各 `test_<控件>.cpp`。
- **覆盖率工作流（4.11）**：行覆盖率以 GCC `--coverage` 构建（CMake 开关 `AURORA_ENABLE_COVERAGE=ON`，见 `BUILD_OPTIONS.md`）
  并跑完 ctest 后，用 `tools/coverage_report.sh`（Linux/macOS）或 `tools/coverage_report.ps1`（Windows）聚合为终端摘要 +
  `<build_dir>/coverage.csv`，此为唯一口径（gcov 按源文件路径跨目标求和）。逐源文件阈值 **90%**；低于阈值的文件须在其宿主
  测试文件头部以「覆盖率说明」注明现状与归类（补测缺口 / 平台门控豁免 / 数据头噪声），不得静默留白。平台门控后端
  （Win32/macOS/WASM/D3D11 等）在非对应平台天然 0 覆盖、纯数据/生成头（如 `render/bitmap_font.h`、`core/error_codes.gen.h`）
  的 constexpr 初始化无运行时计数，均按豁免处理。公共 API ↔ 测试函数的映射经审计落在各 `test_*.cpp` 文件头部注释块中，
  内部命名空间（`aurora::internal` / `detail`）不属于对外承诺面，不入映射；覆盖率口径 / 阈值 / 豁免归类的规则见上文本节约述。
## 4. 元数据与可观测（对应规格 §21）

- **错误可机读（3.1）**：`Error.to_json()` 输出结构化错误（code/message/suggestion/docs/where），供 AI 解析。
- **快照可比对（3.2）**：`HeadlessSurface` 输出确定性 PNG，CI 用 golden test 比对（见 `tests/test_offscreen.cpp` 的 golden 基准段，原 golden_test 已并入）。
- **降级而非中止（3.3）**：非法输入/缺失类型产出 `Diagnostics` 并降级到安全默认；`from_json` 含不可重建控件（如 `Repeater`/
  `Canvas`）时返回预期错误而非崩溃。
- **可观测（3.4）**：`Logger` 双通道（诊断 `AURORA_LOG_*` / 功能 `AURORA_LOG_RAW`）输出，`Diagnostics` 汇总「做了什么降级」；通道划分、输出纪律与重定向见 `3.6`。
- **增量编译友好（3.5）**：头文件尽量只放声明，实现下沉 `src/aurora/*.cpp`；非模板纯逻辑类实现移 `.cpp`，减少 TU 重编。
- **统一日志与输出纪律（3.6）**： **禁止直接使用 `std::cout` / `std::cerr` / `printf` / `fprintf` / `puts` 等标准输出**
  。诊断/日志统一走 `AURORA_LOG_{TRACE,DEBUG,INFO,WARN,ERROR,FATAL}(category, ...)` 宏（写 stderr，带
  `[时间][级别][module@threadId filename:line] > 消息` 前缀，受级别阈值与启用开关控制）；CLI 的 JSON 结果 / usage 文本、benchmark
  表格、LSP/MCP 等基于 stdio 的线协议帧等「程序产品」功能输出走 `AURORA_LOG_RAW(category, ...)`（写
  stdout，无前缀、不受阈值/开关限制，始终输出，且 `fflush` 保证即时送达）；遗留 `printf` 风格诊断用 `AURORA_TEST_PRINTF` /
  `AURORA_TEST_PRINTF_ERR` 桥接宏（先 `snprintf` 入 **内存缓冲**再经 `Logger` 输出，非标准输出）。两通道均可通过 `set_sink` /
  `set_raw_sink` 重定向到文件或测试捕获。详见 `SPECIFICATIONS.md` 日志（Log）子系统小节。
- **错误定位（3.7）**：错误信息精确到 `文件:行`（`Error::where`）；`AURORA_ASSERT` 附上下文。

## 5. 契约与表达（对应规格 §23）

- **显式标注（五·标注与契约表达）**：纯函数/非纯、线程安全、是否可重建，用文档注释显式标注；`serialization` 重建约束在
  `include/aurora/widget/serialization.h` 注明。
- **评分方法（六）**：以「AI 在有限上下文窗口内能否一次生成可编译、可运行代码」为验收标准，而非仅人类可读。

## 6. 约束总结（Invariants 链接）

- 架构与运行时不变量见 `../architecture/ARCHITECTURE_PERF.md` §11（设计不变量 Invariants）。
- 线程安全边界：`State::set` 可跨线程；订阅回调/重绘只在主线程。
- 强类型几何：尺寸/颜色/长度使用强类型，禁止裸整数隐式转换。

---

