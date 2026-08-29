# Aurora 编码规范

> 本文件规定 Aurora 的编码与 API 设计硬规范，所有贡献必须遵守。
> 架构与设计原则见 [`ARCHITECTURE.md`](ARCHITECTURE.md)；功能规格与 API 契约见 [`SPECIFICATIONS.md`](SPECIFICATIONS.md) 及 `specification/`；概念映射见 [`CONCEPTS.md`](CONCEPTS.md)；配方见 [`GUIDELINE.md`](GUIDELINE.md)；编译选项见 [`BUILD_OPTIONS.md`](BUILD_OPTIONS.md)。

---

## 1 错误处理

- **统一错误类型**：全部可失败操作返回 `Result<T>`，不要用裸 `std::optional` 或抛异常表示业务错误。
- **构造即校验**：可失败构造（如 `ImageView::load`、`serialization::from_json`）提供 `make_*()` 工厂，构造时即校验，非法输入立即返回 `Result` 而非延迟崩溃。
- **携带修复建议**：`Error` 必须包含 `suggestion` 字段（人类 / AI 可读的「怎么改」）；`docs` 指向相关文档锚点。
- **部分生成容错**：AI 生成的局部代码片段（单文件 / 单函数）应能独立编译；不要把必须的前置声明 / 类型散落在不可见的上文。`Error` 的 `suggestion` 直接给出缺失件。
- **调试可定位**：`Error::where` 精确到 `文件:行`；`AURORA_ASSERT` 在 debug 触发并附上下文。

错误的设计模型、分类与生成管线见 [`ARCHITECTURE.md`](ARCHITECTURE.md) §12；错误码全量清单见 [`ERROR_CATALOG.md`](ERROR_CATALOG.md)。

---

## 2 命名

- **名序一致**：属性 / 参数名序与 React / Flutter 对齐（回调用 `on_xxx` 如 `on_click`，非 `click_handler`；子节点用 `children`；尺寸用 `width` / `height`）。避免自创词序。
- **默认参数友好**：高频构造提供默认参数 / 便捷工厂（如 `au::colors::AURORA_BLUE`、`dp(16)`），降低记忆负担。
- **顺序容错**：多参数构造优先用 `XxxProps{...}` 具名聚合（如 `ColumnProps{ .children = ... }`），避免位置参数顺序错误。
- **强类型几何**：`Length` / `Color` / `Size` / `Point` 为强类型；禁止 `Length(int)` 隐式转换（裸整数编译失败）。已采纳用户字面量 `au::literals`（`100_dp`、`16_ms`、`0xRRGGBB_rgb`），`px(100)` 与 `100_dp` 互补；**禁止头文件全局 `using`，仅 TU 内 `using namespace au::literals`**。
- **所有权清晰**：资源所有权用 `unique_ptr` / `shared_ptr` 明确；跨边界传递用 `std::move`；`Binding<T>` 为非拥有引用（上游生命周期须更长）。
- **常量命名**：命名空间 / 文件级与类内 `static constexpr` 常量统一 `AURORA_` 前缀 + `UPPER_CASE` 全大写下划线（如 `AURORA_DEFAULT_MAX_WIDGET_DEPTH`），与 `.clang-tidy` 的 `ConstantPrefix` / `GlobalConstantPrefix` 约束一致；禁止 `k` 前缀 CamelCase。
- **生命周期回调强类型**：`au::Lifecycle` 的 `on_mount` / `on_unmount`、窗口级 `WindowState` / `WindowMode` 的 `set_on_*` 回调均为具名 `std::function` 强类型（`MountCb = std::function<void(const BuildContext&)>`、`UnmountCb = std::function<void()>`，`WindowStateHandler` / `WindowModeHandler` 同理）；枚举取值穷尽且按「可见性 / 几何态」正交划分（`WindowState` 不并入 `Maximized`），AI 无需猜测「是否还有隐藏状态」。回调均可空（无副作用时不传），且不走异常捕获（与主线程事件回调一致）。

---

## 3 文档与示例

- **示例即文档**：每个 widget 提供最小可编译示例，集中在 `examples/demos/`（`demo_<组件>.cpp`，1:1）与 [`GUIDELINE.md`](GUIDELINE.md)。
- **API 描述可机读**：`gen_api_tools` 输出 `aurora_api.json`，含类型 / 属性键 / 枚举，供 LSP 与文档生成器消费。
- **零平台魔法**：示例不依赖特定平台 GUI 事件循环；`HeadlessSurface` 可离线渲染 PNG，便于测试与 AI 复现。
- **源—示例—测试 1:1 映射**：每个公共源文件（widget / 子系统头）原则上对应一个 `demo_*.cpp`（`examples/demos/`）与一个 `test_*.cpp`（`tests/`）。允许少量「复杂场景」demo / test（跨控件集成、端到端流程）作为例外，但须明确标注其跨源性质。所有 demo 收敛到 `examples/demos/`（CMake 仅 GLOB 该目录，新增组件 demo 放到此处即自动纳入构建，无需改 CMake）。
- **文件夹区分**：demo 与 test 以目录区分——示例在 `examples/`，测试在 `tests/`；二者不混放。
- **test 文件前缀**：测试文件统一以 `test` 为前缀（`test_xxx.cpp`），与示例的 `demo` 前缀风格一致。聚合多个不相关控件的「catch-all」测试文件视为反模式，应拆为各 `test_<控件>.cpp`。

### 3.1 注册式测试 runner

`tests/*.cpp` 全部链入**单一可执行** `aurora_test_runner`（由 `cmake/AuroraTests.cmake` 配置），`main()` 由 `tests/au_test_main.cpp` 唯一提供，**测试文件禁止自定义 `main()`**。

- 用例经 `AURORA_TEST()` 宏静态自注册（用例名 = 文件名 stem，由 CMake 按源文件注入 `AURORA_TEST_NAME`；同文件多片段用 `AURORA_TEST_NAMED("名")`）。
- 断言一律走 `test_harness.h` 的 `AURORA_TEST_CHECK*`（非致命，记录后继续）/ `AURORA_TEST_REQUIRE*`（致命，抛 `CheckAbort` 终止本用例）家族，失败计数汇入框架上下文，由 runner 统一决定退出码；**不得自造 `g_test_failures` / `return 0/1` 式退出码**。
- 后端 / 平台专属用例在 feature 宏未开启的 `#else` 分支以 `AURORA_TEST_SKIP(宏名)` 注册空通过桩。
- 新增文件漏写注册宏不会报链接错误，由 CTest 的 `registry_integrity` 守护（比对 `runner --list` 与配置期 GLOB 清单）兜底。CTest 粒度不变：每条 = `--run=<stem>`（进程隔离）。

### 3.2 覆盖率工作流

行覆盖率以 GCC `--coverage` 构建（CMake 开关 `AURORA_ENABLE_COVERAGE=ON`）并跑完 CTest 后，用 `tools/coverage_report.sh`（Linux / macOS）或 `tools/coverage_report.ps1`（Windows）聚合为终端摘要 + `<build_dir>/coverage.csv`，此为唯一口径。

- 逐源文件阈值 **90%**；低于阈值的文件须在其宿主测试文件头部以「覆盖率说明」注明现状与归类（补测缺口 / 平台门控豁免 / 数据头噪声），不得静默留白。
- 平台门控后端（Win32 / macOS / WASM / D3D11 等）在非对应平台天然 0 覆盖；纯数据 / 生成头（如 `render/bitmap_font.h`、`core/error_codes.gen.h`）的 constexpr 初始化无运行时计数。二者均按豁免处理。
- 公共 API ↔ 测试函数的映射经审计落在各 `test_*.cpp` 文件头部注释块中；内部命名空间（`aurora::internal` / `detail`）不属于对外承诺面，不入映射。

---

## 4 元数据与可观测

- **错误可机读**：`Error::to_json()` 输出结构化错误（`code` / `message` / `suggestion` / `docs` / `where`），供 AI 解析。
- **快照可比对**：`HeadlessSurface` 输出确定性 PNG，CI 用 golden test 比对（见 `tests/test_offscreen.cpp` 的 golden 基准段）。
- **降级而非中止**：非法输入 / 缺失类型产出 `Diagnostics` 并降级到安全默认；`from_json` 含不可重建控件（如 `Repeater` / `Canvas`）时返回预期错误而非崩溃。
- **可观测**：`Logger` 双通道（诊断 `AURORA_LOG_*` / 功能 `AURORA_LOG_RAW`）输出，`Diagnostics` 汇总「做了什么降级」。
- **增量编译友好**：头文件尽量只放声明，实现下沉 `src/aurora/*.cpp`；非模板纯逻辑类实现移 `.cpp`，减少 TU 重编。
- **错误定位**：错误信息精确到 `文件:行`（`Error::where`）；`AURORA_ASSERT` 附上下文。

### 4.1 统一日志与输出纪律

**禁止直接使用 `std::cout` / `std::cerr` / `printf` / `fprintf` / `puts` 等标准输出。**

| 通道 | 宏 | 去向 | 特征 |
|:---|:---|:---|:---|
| 诊断 | `AURORA_LOG_{TRACE,DEBUG,INFO,WARN,ERROR,FATAL}(category, ...)` | stderr | 带 `[时间][级别][module@threadId filename:line] > 消息` 前缀，受级别阈值与启用开关控制 |
| 功能输出 | `AURORA_LOG_RAW(category, ...)` | stdout | 无前缀、不受阈值 / 开关限制、始终输出，且 `fflush` 保证即时送达 |

功能输出通道用于 CLI 的 JSON 结果与 usage 文本、benchmark 表格、LSP / MCP 等基于 stdio 的线协议帧等「程序产品」输出。

遗留 `printf` 风格诊断用 `AURORA_TEST_PRINTF` / `AURORA_TEST_PRINTF_ERR` 桥接宏（先 `snprintf` 入**内存缓冲**再经 `Logger` 输出，非标准输出）。

两通道均可通过 `set_sink` / `set_raw_sink` 重定向到文件或测试捕获。契约见 [`specification/08-tooling.md`](specification/08-tooling.md) §9。

---

## 5 契约与表达

- **显式标注**：纯函数 / 非纯、线程安全、是否可重建，用文档注释显式标注；`serialization` 重建约束在 `include/aurora/widget/serialization.h` 注明。
- **标注格式**：`@note Thread: ...`、`@note Side-effects: ...`、`@note Rebuildable: ...`。全部公共头文件均须带统一契约标注。

**同步流程（强制执行）**：

- 新增公共类 / 结构体时，**必须**在类级别 Doxygen 注释中添加契约标注：
  - `@note Thread: main-thread only` / `thread-safe` / `thread-safe with mutex` / `thread-safe with rwlock`
  - `@note Side-effects: pure` / `none` / `paints` / `mutates layout`
  - `@note Rebuildable: yes, via from_json` / `no`
- 修改现有类的线程安全语义、副作用行为或序列化支持时，**必须**同步更新对应标注。
- Code Review 中应将契约标注一致性作为审查项。
- 读写锁（`std::shared_mutex`）仅用于读多写少场景（如缓存表），**禁止在控件 / Props 等主线程对象中使用**；使用 `shared_mutex` 的类必须标注 `@note Thread: thread-safe with rwlock` 并说明为何选择 rwlock 而非普通 mutex。

### 5.1 特殊成员函数豁免

多态基类定义了虚析构后，原则上应删除（或 `= default` 定义）全部拷贝 / 移动特殊成员函数。仅当现有 API **刻意依赖**隐式拷贝 / 移动时（如 `Reactive<T>` 依赖隐式拷贝赋值支持 `content = "Hi";` 写法、`FormField` 按值持有 `State<T>` 并依赖移动语义），才允许用 `// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)` 抑制，且必须用紧邻注释写明依赖原因。

不得默默无视告警，也不得为消除告警而删除仍被使用的拷贝 / 移动（先全量构建验证再决定）。

### 5.2 NOLINT 抑制规则

凡用 `NOLINT` / `NOLINTNEXTLINE` 抑制 Clang-Tidy 告警，必须：

1. 写明具体检查名（禁止裸 `NOLINT` 的新增使用）；
2. 紧邻注释说明「为何不能按建议修复」。

抑制属于显式契约决策，随代码评审、随文档同步。

---

## 6 AI 友好性

本库面向 AI 编码助手友好。以下为**编码规则层**的全部评估条目；架构 / 设计理念层条目见 [`ARCHITECTURE.md`](ARCHITECTURE.md) §13。

### 6.1 部分生成容错

当 AI 生成的代码片段（单文件 / 单函数 / 局部片段）单独编译 / 运行时，应「部分生成也能容错」：缺失的前置声明、类型别名、include 不应导致不可理解的错误。库须保证「复制任一 widget 的构造片段即可独立编译」（`Error::suggestion` 直接给出缺失件）。

### 6.2 默认参数

高频构造提供默认参数 / 便捷工厂，降低记忆负担。所有 `XxxProps` 结构体字段均须有合理默认值。编译期验证（`static_assert`）集中在 `tests/test_default_construct.h`，运行时验证在 `tests/test_default_construct.cpp`。

**同步流程（强制执行）**：

- 新增控件类时，**必须**提供默认构造器（`Xxx() = default` 或自定义默认构造）。
- 新增 `XxxProps` 聚合类型时，**必须**确保所有字段都有合理默认值。
- 新增公共构造函数时，**必须**评估是否可为高频参数提供默认值。
- `tests/test_default_construct.h` 中的 `static_assert` 列表**必须**随新增控件同步更新。
- 新增控件时，**必须**在 `tests/test_default_construct.cpp` 中 `#include "test_default_construct.h"` 以确保编译期校验生效。

### 6.3 元编程边界

模板 / 宏等元编程仅用于「无法用普通函数表达」之处（如 `State` / `Signal` / `Modifier` 的声明式组合）；禁止为「炫技」引入深层模板，避免 AI 难以展开实例化错误。

**控件样板减负同样遵循此原则**：基类以**非模板虚函数默认实现**消除重复——`Widget::describe()` 默认返回 `{ .name = type_name() }`、`Container::collect_signals()` 默认遍历 `m_children` 收集子节点信号，新增控件只需实现 `type_name()`（+ `describe_static()`）。**禁止为减负引入 CRTP / 模板基类**：那会把实现搬进头文件，破坏声明 / 实现分离，并增大 AI 理解单函数所需加载的上下文。

**同步流程（强制执行）**：

- 新增模板 / 概念 / 元编程代码时，**必须**确认属于「无法用普通函数表达」的场景。
- 禁止引入 CRTP、模板基类、深层模板实例化（> 2 层嵌套）；**禁止在头文件中引入 `std::mutex` / `std::shared_mutex` / `std::lock_guard`**。
- 如确需引入，**必须**在代码注释中说明为何无法用普通函数替代。

### 6.4 二层属性划分（固有属性 + 正交 Modifier）

- **固有属性层**：描述控件**自身**视觉 / 行为身份（如 `Text::text_align`、`Button::corner_radius`、`Slider::active_color`），以 `XxxProps` 的 snake_case 字段暴露，随控件序列化，是 Inspector / `aurora_api.json` 可枚举的一等属性。
- **正交修饰层**：跨切面、可叠加、可 `Reactive` 变化的通用装饰，挂在每个 `Widget::modifier`，作用于**任意**控件。
- **重叠规则**：优先控件固有属性；`Modifier` 同类项保留用于「给任意控件套一层」的跨切面场景，绘制时 `Modifier` 在外、固有属性在内，可叠加。

**为何保留 `Modifier`（AI 友好）**：装饰能力收敛于**单个 `Modifier` 类型**（约 20 个方法 + `Kind` 枚举可枚举），远少于纯包裹控件模型所需的十余个独立 widget 类型（`Padding` / `Container` / `DecoratedBox` / `GestureDetector` / `Align` / `Opacity` / `Transform` / `ClipRRect` / `SizedBox` / `Expanded`…）；扁平 `.modifier` 链比深层 `Container(GestureDetector(Opacity(...)))` 嵌套更不易生成错位；且与现有**扁平** `aurora_api.json` / `diff` / `apply_patch` 序列化模型天然契合。

### 6.5 外观变更只标绘制（不重排）

控件在运行时变更应区分「是否影响布局几何」并调用对应的标脏原语，以配合脏区追踪（默认开启，`Window::present_root` 的 layout / paint 分离）：

- **仅影响外观、不影响几何**的变更（文本选区高亮、颜色 / 主题切换、光标位置、滚动偏移等）**必须**调用 `mark_needs_paint()`（仅置「绘制脏」），让拖选 / 主题切换等帧跳过整树 `layout`。
- **影响布局几何**的变更（尺寸 / 约束 / 子节点增删、结构性 `Modifier` 如 `rotate` / `scale` 重算包围盒）调用 `mark_needs_layout()`（置「布局脏 + 绘制脏」），下一帧 `layout + paint`。
- `Widget::on_dirty` 为 `std::function<void(bool)>`（`true` = 含布局脏），经 `Window::wire_dirty` 递归接线整棵树；子控件的 `State` / `Modifier` 变更通过响应式 `Effect` 自动落到上述两个原语，无需手工标脏。
- **反模式**：外观变更误调 `mark_needs_layout()` 会迫使每帧重排，抵消脏追踪的性能收益。

### 6.6 控件可定制性契约

新增 / 改造交互控件必须遵守四条契约（完整定义见 [`specification/04-widget.md`](specification/04-widget.md) §4）：

1. **强调色主题回退**：`active_color` 类属性用 `std::optional<Color>`，未设置时绘制期经 `inherit_theme(ctx).primary` 解析，且未设置不序列化（保留意图）。
2. **状态色统一派生**：hover / pressed 用 `Color::shaded(k)` 乘性调暗，淡色底 / 选区用 `Color::with_alpha(a)`；不手写逐通道乘法。所有交互控件提供 `set_enabled(bool)`：禁用态灰化、吞事件不冒泡、不改值。
3. **绘制分阶段**：`on_paint` 分解为 protected 虚钩子（状态色经 `resolve_*`）；控件成员用 protected 而非 private，使子类可单点覆盖某阶段而无需重写整个 `on_paint`。
4. **常量属性化**：行高 / 盒高 / 字号 / 圆角等尺寸不得硬编码，升级为可序列化属性；`corner_radius < 0` 统一表示「自动」；影响几何的 setter 调 `mark_needs_layout()`，仅外观的调 `mark_needs_paint()`。

### 6.7 平台后端头必须 pimpl 隔离（硬规则）

任何依赖平台 SDK 的 `Surface` / 窗口宿主类，其**公共头不得包含平台重型头**（`<windows.h>`、`<windowsx.h>`、`<GLFW/glfw3.h>`、`<GL/gl.h>`、`<X11/Xlib.h>`、`wayland-client.h`、AppKit 等），一律以 `std::unique_ptr<Impl> m_pimpl` 形式把细节收进对应 `.cpp`。

- **动机**：① 头依赖收敛——消费者 TU 不因「某后端被开启」而被拉入数万行平台头；② 宏污染隔离——`<windows.h>` 的 `min` / `max` / `ERROR`、Xlib 的 `None` / `Bool` / `Status` 不再泄漏到用户命名空间；③ 增量编译——改后端实现只重编 1 个 TU。
- **句柄暴露**：确需向外暴露原生句柄时，返回 **`void*`** 而非平台类型（如 `hwnd() -> void*`），调用方在自身已含平台头的 TU 内 `static_cast<HWND>(...)` 还原。此类访问器属「平台逃生舱」，其静态类型不计入 API 稳定性承诺。
- **回调归属**：平台 C 回调（GLFW callback、Win32 `WNDPROC`）声明为 `Impl` 的**静态成员**而非文件级自由函数，以便直接访问私有 `Impl`；用户指针（`glfwSetWindowUserPointer` / `GWLP_USERDATA`）存 `Impl*`。
- **现状**：`Win32Window`、`GlfwSurface`、`X11Surface`、`WaylandSurface` 均已合规；新增后端须遵循同一形态。

### 6.8 SIMD 双实现同步修改（硬规则）

凡引入「标量黄金 + SIMD 快路径」双实现的渲染函数（当前 `gradient_*_scanline_*` / `gradient_*_fill`，位于 `aurora::detail`），两条路径必须保持**逐位一致**，且修改任一实现时**必须同步另一份并跑 `test_simd_parity` 全量比对**。

- **动机**：① 双实现随时间演化漂移会产生「SIMD 开启 / 关闭给出不同像素」的静默正确性 bug；② parity 测试覆盖不到的边界输入（alpha = 0 / 255、非 8 倍数宽、负坐标、裁剪边界）是漂移高发区。
- **约束**：SIMD 路径须沿用标量浮点运算序列（同序、`-ffp-contract=off` 禁 FMA），整型截断统一用 `cvtt`（`_mm_cvttps_epi32` / `_mm256_cvttps_epi32`），sRGB 转换沿用同一 LUT；禁止为「提速」引入标量未做的近似或重排。
- **验收**：每次改动后 `test_simd_parity` 必须 0 failure（一票否决）；若某函数无法在保持逐位一致前提下向量化，停下来单独提出，不擅自放宽。
- **开关**：`AURORA_ENABLE_SIMD` 默认 ON，OFF 时仅编译标量路径；双实现均属 `aurora::detail` 内部，不计入 `aurora_api.json`。

### 6.9 错误可机读与局部修复建议

`Error` 提供 `to_json()`，结构含 `code` / `message` / `suggestion` / `docs` / `where`；`Error::suggestion` 直接给出「改哪一行 / 加哪个 include」级别的可执行建议。

### 6.10 无障碍 lint

不提供对比度 / 标签缺失 / 焦点可达性等无障碍 lint（用户明确移出范围）。

### 6.11 Schema 强制与工具链

- **Schema 强制**：公共 API 的入参 / 出参有 Schema 校验；`gen_api_tools` 输出 `aurora_api.json` 即机器可读 Schema。
- **代码补全 / LSP**：`aurora_lsp` 消费库 live API（`describe_component` + `known_enums`）对 `au::XxxProps{ .prop = ... }` 等声明式写法做 completion / hover / diagnostics / codeAction。
- **MCP / CLI**：stdio JSON-RPC 2.0 MCP Server（`aurora_mcp`，10 个 tools）+ CLI 工具（`aurora_cli`），消费 `describe()` / `list_all_schemas()` / `validate()` / `render_to_png()` / `to_code()` 等库 API。
- **序列化 diff**：`to_json` / `from_json` / `diff` / `apply_patch` 提供树级 diff，便于 AI 推断「改了什么」。
- **示例文档化**：每个 widget 的最小可编译示例随 API 文档发布（`examples/` + [`GUIDELINE.md`](GUIDELINE.md)）。
- **零平台魔法**：示例不依赖特定平台事件循环；`HeadlessSurface` 可离线渲染 PNG。
- **版本稳定**：次要版本只增不删（见 §7）；破坏性变更进主版本并写迁移指南。
- **Playground / REPL**：不提供在线 Playground / REPL（用户明确移出范围）。

契约细节见 [`specification/08-tooling.md`](specification/08-tooling.md) §7。

### 6.12 组件发现

每个控件实现静态 `describe_static()`（注册表 / 工具链消费的完整 `WidgetDescriptor`，含属性 / 事件 / 子节点策略 / 示例）与必写的 `type_name()`；`describe()` 现由 `Widget` 提供默认实现（无富描述控件可省略 override），仅当需要额外 properties / events / children_policy 时才覆写。

`component_schema()` 消费 `describe()` 输出并增强 `prop_descriptors` / `events` / `children_policy` / `examples` 字段；`list_all_schemas()` 批量返回全部已注册组件 schema；`aurora_api.json` 自动包含新字段。

### 6.13 评分方法

以「AI 在有限上下文窗口内能否一次生成可编译、可运行代码」为验收标准，而非仅人类可读。

---

## 7 版本与变更管理

公开 API 的稳定性直接影响生成代码的可用性，故单列。Aurora 采用**语义化版本（SemVer 2.0）**：`主.次.补`（MAJOR.MINOR.PATCH）。

### 7.1 规则

- **次要版本只增不删**：新增 API / 类型 / 属性可在 MINOR 增加；不得删除或破坏既有公开 API（保持向后兼容）。
- **主版本允许破坏性变更**：破坏性修改（删除 / 改语义）只能进 MAJOR，并附迁移指南。
- **补丁版本**：缺陷修复、文档、性能，不引入 API 变更。
- **变更追踪**：所有公开 API 变更写入 `CHANGELOG.json`（类型化，版本真相以 `currentVersion` 为准）；`aurora_api.json` 无顶层版本字段，仅按条目 `since` 标注引入版本。
- **AI 友好性影响**：API 删除会让依赖旧签名的生成代码失效，因此非 MAJOR 不删。

### 7.2 流程

1. 改动公开 API → 更新 `CHANGELOG.json` 条目。
2. 运行 `cmake --build build --target aurora_api_json`（内部执行 `gen_api_tools aurora_api.json`）刷新 API 描述。
3. 破坏性变更 → 写迁移说明并 bump MAJOR。

---

## 8 函数签名

### 8.1 优先使用引用

当参数不需要转移所有权、且为必传（不可空）时，优先使用左值引用（`const T&` / `T&`）而非裸指针；仅当需表达「可选 / 可空 / 可重绑定」或 C 互操作边界时才使用指针。引用消除空指针歧义、约束调用方传入有效对象，提升静态分析与 AI 可读性。

### 8.2 未使用形参保留名称注释

在虚函数重写 / 接口实现等「签名必须保留但实现内不使用」的场景，不得省略参数名称（不写成 `void f(int)`），而应以注释保留其语义名称，例如 `void on_event(Event /* ctx */)`。此写法既保留 API 自描述性，又避免未使用参数告警。

### 8.3 尾置返回类型

函数（成员函数、自由函数）统一使用尾置返回类型 `auto f(...) -> Ret`（lambda 与显然的短返回可省略）。尾置写法使参数列表首屏完整可见、复杂 / 模板返回类型更易对齐，利于 AI 生成与 diff 比对。

### 8.4 控制语句大括号不可省略

`if` / `for` / `while` / `do-while` 等控制语句，**即使受控体仅一行也必须使用 `{}` 包裹**，禁止「尾随单语句省略大括号」的写法。

该约束由 `.clang-format` 机械强制——已设置 `AllowShortIfStatementsOnASingleLine: false` 与 `AllowShortLoopsOnASingleLine: false`，运行 `clang-format` 会自动补齐缺失大括号。此规则对 `switch` 的 `case` 标签（无受控体括号语义）不适用。

---

## 9 内部工具层约定

重构抽取的内部通用函数统一收口于 `include/aurora/core/`，避免新增命名空间与额外公共导出面。

| 助手 | 位置 | 说明 |
|:---|:---|:---|
| `utf8_encode` / `utf8_cp_len` / `utf8_cp_count` / `utf8_cp_slice` | `core/utf8.h` | header-only、零依赖、可被常量上下文使用，供 widget / window 统一调用以消除重复编码实现 |
| `aurora::internal::string_format` | `core/string_util.h` + `.cpp` | printf 风格、`vsnprintf` 自动扩容；**仅内部使用、不进 `aurora.h` 公共导出**，用于收敛各模块 `std::snprintf` 进栈缓冲的重复样板 |
| `aurora::saturate(float)` / `saturate_u8(float)` | `core/math.h` | header-only，收口渲染热路径中散落的 `std::clamp(x, 0, 255)` / `std::clamp(x, 0.0f, 1.0f)` 样板；**不抽取与 `std` 重复的通用 `clamp` / `lerp`**。`saturate_u8` 保留 `static_cast<uint8_t>` 的截断（向零）语义 |

**归属原则**：跨多模块高频且语义中立的纯函数收口 `core/`；单领域复用就近归入所属模块；平台特定逻辑保留在对应后端（如 `window/swizzle.h`），不污染 `core/`。

---

## 10 提交信息规范

本规范约定仓库提交信息的统一写法，确保 `git log` 可读、可自动归类，并与 §7 的 SemVer 版本策略和 `CHANGELOG.json` 变更追踪对齐。提交信息支持**简体中文或英文**（二选一，同一仓库内保持一致即可）。

### 10.1 格式

采用 Conventional Commits 结构：头部一行 + 空行 + 可选正文 + 空行 + 可选脚注。

```text
<type>(<scope>): <subject>

<body>

<footer>
```

- `type`：提交类型（英文小写，见 §10.3）。
- `scope`：受影响的模块 / 子系统（中文或英文均可，见 §10.4）。可省略。
- `subject`：一句话简述，**动词开头、不加句号、≤ 50 字 / 词**。
- `body`：说明**为什么**做此改动、**做了什么**。每行 ≤ 72 字，可多段。
- `footer`：破坏性变更、关联 Issue / PR、对应 `CHANGELOG.json` 条目等。

### 10.2 示例

```text
feat(painter): 新增圆角矩形填充接口

为支持卡片阴影与气泡背景，Painter 新增 fill_rounded_rect，
内部走 SDF 慢路径，与现有 fill_rect 共享裁剪逻辑。

Ref #142
对应 CHANGELOG.json: added Painter.fill_rounded_rect
```

破坏性变更：

```text
refactor(widget)!: 统一 on_paint 入参为全局坐标

on_paint 接收的 bounds 现统一为相对窗口客户区的全局坐标，
子控件需基于 bounds.origin 计算绘制位置。

BREAKING CHANGE: 自定义 Widget 的 on_paint 实现须改用全局坐标，
否则绘制偏移错误。
```

### 10.3 Type 列表

| type | 含义 | 版本影响 |
|:---|:---|:---|
| `feat` | 新增功能 / 公共 API | MINOR |
| `fix` | 缺陷修复 | PATCH |
| `docs` | 仅文档（`codespec/`、`README`、注释型文档） | PATCH |
| `perf` | 性能优化（不改变对外行为） | PATCH |
| `refactor` | 重构（不改变对外行为） | PATCH |
| `test` | 新增 / 修正测试（`tests/`） | PATCH |
| `build` | 构建系统 / 依赖（`CMakeLists.txt`、`cmake/`、`third_party/`） | PATCH |
| `ci` | CI / 工作流（`.github/`） | PATCH |
| `style` | 格式化（不影响逻辑，如 clang-format） | PATCH |
| `chore` | 杂务（版本号 bump、清理、非代码改动） | PATCH |
| `revert` | 回滚某次提交 | 视被回滚内容 |

> 破坏性变更：在 `type` 或 `type(scope)` 后加 `!`，并在 footer 写 `BREAKING CHANGE: <说明>`。破坏性变更须触发 **MAJOR** 版本 bump。

### 10.4 Scope 取值

优先取受影响的子系统名，保持与代码目录 / 命名空间一致：

- 渲染：`painter`、`render`、`image`、`font`
- 控件：`widget`、`button`、`scroll`、`lazy_list`、`chip`、`navigator`
- 布局：`layout`、`flex`
- 后端：`surface`、`win32`、`x11`、`wayland`、`glfw`、`wasm`、`macos`、`headless`
- 子系统：`storage`、`animation`、`event`、`core`、`logger`
- 工程：`cmake`、`tests`、`examples`、`tools`、`docs`、`ci`

多个模块受影响时可省略 scope，或取最关键的一个。

### 10.5 编写要求

1. **中文或英文**：subject 与 body 使用简体中文或英文（二选一）；技术专有名词（`Painter`、API 名、CMake 选项）保留英文。
2. **动词开头**：中文用「新增 / 修复 / 重构 / 优化 / 移除 / 调整」等祈使句；英文用祈使句。
3. **不写句号**：中文 subject 末尾不加 `。`；英文 subject 末尾不加 `.`。
4. **一行一个语义**：一次提交聚焦一件事；若含多类改动（如 feat + fix），拆成多次提交。
5. **关联可追溯**：涉及 Issue / PR 时在 footer 写 `Ref #<id>` / `Close #<id>`；API 变更须注明对应 `CHANGELOG.json` 条目。
6. **与版本策略对齐**：`feat` → 升 MINOR；`fix` / `perf` / `docs` 等 → 升 PATCH；带 `!` 或 `BREAKING CHANGE:` → 升 MAJOR，并写迁移说明。
7. **不提交无关文件**：仅纳入本次实际改动的业务文件；构建产物（`build*/`）与本地 AI 工具目录（`.codebuddy/` 等）已由 `.gitignore` 忽略，勿 `git add -A` 强行纳入。

### 10.6 禁止事项

- 禁止 subject 为空或仅写 `update` / `fix bug` 这类无信息内容。
- 禁止把不相关的多个 feature / 修复混在同一提交（破坏 bisect 与 revert 粒度）。
- 禁止在 `style` 提交里夹带逻辑改动；格式化与逻辑改动分开提交。
- 禁止提交被 `.gitignore` 忽略的产物。

---

## 11 需求规格

### 11.1 #1 声明式双模 API（链式 / 分步 / 配置块等价）

**核心目标：** AI 易生成。

**问题背景：** Fluent API 往往产生长链式调用，AI 容易在链中间遗漏括号或参数顺序错误。

**需求陈述：** 同一控件必须提供三种**语义完全等价**的构建形态，AI 可按复杂度选择——① 链式 setter（简短组件）、② `*Props` 具名聚合（嵌套结构，可分块生成）、③ 对已构造对象逐行赋值（复杂组件）。等价性由「控件继承其 Props」保证：`class Xxx : public XxxProps`，`XxxProps` 的字段即控件自身的公有字段，不再用私有 `m_*` 重复声明同一属性。

```cpp
// 形态一：链式 setter（setter 返回引用）
auto btn = au::Button(au::ButtonProps{ .label = "OK" });
btn.set_on_click(fn);

// 形态二：*Props 具名聚合
auto btn2 = au::Button(au::ButtonProps{ .label = "OK" });
btn2.on_click = fn;

// 形态三：对已构造对象逐行赋值
auto btn3 = au::Button();
btn3.label = au::LocalizedString{ "OK" };
btn3.on_click = fn;
```

**关键约束：**

- **控件类不是聚合类型**，不能用 `au::Button{ .label = ... }` 这类指定初始化器构造控件（编译失败）。合法写法只有三种：① `*Props` 聚合；② 初始化列表 `au::Column{ au::Text("A"), au::Text("B") }`（`Column` / `Row` 接受 `std::initializer_list<Node>`）；③ 对已构造对象直接成员赋值。
- 指定初始化器**仅适用于 `*Props` 聚合结构**与 `Theme` 等纯数据聚合；其中字段顺序无关，遗漏字段回退默认值。
- 链式 setter 返回引用；作为子节点放入 `children` 时必须用 `std::move` 包裹（`Widget` 拷贝构造被删除，`Node` 仅移动派生对象）。
- 嵌套声明推荐 `au::Column(au::ColumnProps{ .children = { ... } })` 的树形结构。
- 控件可用 `au::` 或 `aurora::` 前缀；推荐 `namespace au = aurora;`。

**验收标准：** 同一界面用三种形态各写一遍，编译产物与运行时行为一致；AI 只需记住「属性名 + `*Props`」即可生成整棵嵌套树，无需记忆顺序位置参数。

### 11.2 #2 极致命名一致性 + 扁平命名空间

**核心目标：** AI 易补全。

**需求陈述：**

- **相同语义在所有组件使用完全相同的名称**（`on_click` 在所有可点击控件同名同参），杜绝 `setCaption` / `setValue` / `size().x` 一类同义异名；属性设置器一律为「属性直接赋值」风格。
- **命名与类型拼写规则**（属性用名词、事件 `on_` 前缀、布尔取 `show` / `enabled` 语义、杜绝缩写、名序与 React / Flutter 对齐、`XxxProps{...}` 具名聚合优先、强类型几何、常量前缀等）以 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §2 为唯一权威。
- **扁平命名空间**：所有公共组件、类型、自由函数直接位于 `aurora`，禁止深层嵌套（`aurora::widgets::buttons::MaterialButton` 列为反模式）；变体通过属性区分而非类型区分。公共子命名空间仅 `colors`（具名颜色）与 `platform`（平台查询 API）；`render` / `detail` / `ui` / `preferences` 等为内部或辅助命名空间，**不属对外承诺稳定的公共 API 表面**。
- 前缀与别名写法见 [`SPECIFICATIONS.md`](SPECIFICATIONS.md) §命名速查。

**验收标准：** AI 在只见过组件名与属性名的情况下，能凭直觉拼出正确 API；`ai_compat_test` 不出现因「同义异名」导致的生成失败。

### 11.3 #3 正交可组合的最小核心 API

**核心目标：** AI 少幻觉。

**问题背景：** 「最小」可能导致 API 功能缺失，AI 为补全功能会自行虚构 API（幻觉）。因此目标定为「**正交且可组合的最小核心 API**」，而非单纯求小。

**需求陈述：**

- 核心概念控件与每组件核心方法保持精简（Token 经济优先，避免方法无序膨胀；**不预设硬性数量上限**）。控件总清单见 [`specification/04-widget.md`](specification/04-widget.md) §3。
- 高级功能通过**组合**而非继承实现：横切能力由 `Modifier` 表达（见 [`specification/07-environment-modifier.md`](specification/07-environment-modifier.md) §7），布局能力由正交原语表达（见 [`specification/03-layout-render.md`](specification/03-layout-render.md) §3）。
- 高频模式必须以**可从原语推导的组合配方**形式直接提供：

| 配方 | 由何推导 | 头文件 |
|:---|:---|:---|
| `au::Form()` / `au::FormField()` | 表单：label 列宽 + 控件自动对齐 | `widget/form.h` |
| `au::ToolBar()` | `Row` + 固定高度 + 溢出折叠 | `widget/toolbar.h` |
| `au::Drawer()` | 可折叠侧边面板 | `widget/drawer.h` |
| `au::TabBar()` + `au::PageView()` | 顶部 Tab + 页体切换 | `widget/tab_bar.h` / `widget/drawer.h` |
| `au::MenuBar()` | `Row` + 下拉菜单 | `widget/menu_bar.h` |

**验收标准：** 任一常见界面（表单、工具栏、抽屉、Tab 页、菜单条）都有具名配方；不存在「只有某个控件才有、别处需要自行虚构」的孤立能力。

### 11.4 #4 强类型 + 单位标注 + 编译期校验

**核心目标：** AI 生成的代码编译即验证。

**需求陈述：**

- 枚举与尺寸均为强类型：错误取值与缺失单位必须在编译期失败，而不是运行期静默降级。强类型尺寸 / 颜色工厂与用户字面量的具体规则见 §2。
- **编译错误信息需直接指出单位错误，而不是模板爆栈**：

```cpp
button.alignment(au::Alignment::Cenetr);   // 编译错误：拼写错误立刻发现
// [widget-invalid-prop] Button.width():
//   - Expected: au::Length (px / dp / percent / auto)
//   - Received: int (120)
//   - Fix: Use au::px(120) or au::dp(120)
```

- **子节点合法性采用运行时校验**（容器统一接受任意 `Node`，无编译期白名单）：`au::validate(root)` 把空子节点 / 深度超限 / 未知类型报告为结构化 `Error`；公开 API 不接受裸指针作子节点参数，`Node` 化是唯一入口。
- **约束：** 模板深度 ≤ 3 层，避免模板元编程导致的编译爆炸与不可读错误。

**验收标准：** 典型误用（错枚举值、裸整数当尺寸、忘写单位）全部在编译期失败并给出可读修复建议。

### 11.5 #5 合理默认值（声明处可见）

**核心目标：** AI 少写少错。

**需求陈述：** 只需写「与默认值不同」的部分；默认值须适用于绝大多数场景，避免 AI 每次都要显式指定。

```cpp
au::Button(au::ButtonProps{ .label = "OK" });
// 默认：enabled=true, visible=true,
//       alignment=Center, font_size=14,
//       padding={12,6,12,6}, corner_radius=6
```

**关键约束：**

- 默认值文档化在**声明处**：代码中可省略（保持简洁），但 LSP hover 时可见（保持透明），AI 工具无需查阅外部文档。
- 新增 `XxxProps` 的每个字段必须有合理默认值（规则见 §6.2）。
- 运行时可查询默认值：各控件提供静态 `defaults()`（如 `au::Button::defaults()` 返回默认 `ButtonProps`）。

**验收标准：** 空构造 + 单一必填属性即可得到可用控件；`defaults()` 与头文件声明的默认值一致。

---

## 12 约束总结

- 架构与运行时不变量见 [`ARCHITECTURE.md`](ARCHITECTURE.md) §11。
- 线程安全边界：`State::set` 与控件树操作只在主线程；异步结果经 `au::async` 回投主线程后再写状态。
- 强类型几何：尺寸 / 颜色 / 长度使用强类型，禁止裸整数隐式转换。
- 输出纪律：禁止直接使用标准输出，一律走 `Logger` 双通道（§4.1）。
- 更多设计原则见 [`ARCHITECTURE.md`](ARCHITECTURE.md) §13（AI-first 设计原则）；「显式优于隐式（含样式继承）」的需求规格见 [`specification/05-event-navigation.md`](specification/05-event-navigation.md) §8.1（#8）。
