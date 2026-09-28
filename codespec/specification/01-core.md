# 核心基础层（core）

> 覆盖 `include/aurora/core/`（36 个头文件）与根级 `todo.h`、`commands.h`。
> 本文件是错误、诊断、日志、异步底座、基础几何类型与 JSON 值容器的**唯一权威**；错误码全量清单见 [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)（生成物），编码层面的错误写法规则见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md) §1。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 几何与尺寸意图 | `types.h`、`dimension.h`、`transform.h`、`math.h` |
| 颜色 | `color.h`、`color_space.h` |
| 错误与结果 | `result.h`、`error_codes.h`、`error_codes.gen.h` |
| 诊断与降级 | `diagnostics.h`、`strict_mode.h`、`aurora_assert.h` |
| 日志 | `log.h`、`debug.h` |
| 异步底座 | `thread_pool.h`、`thread.h` |
| 时间与周期 | `time.h`、`duration.h`、`event_stream.h`、`file_watcher.h` |
| 文本与编码 | `utf8.h`、`string_util.h`、`directionality.h` |
| 平台与能力查询 | `platform.h`、`enums.h`、`native_surface.h` |
| 无障碍（a11y） | `accessibility.h`（角色 / 动作 / 节点 / 事件 / 设置等纯数据类型与指针级钩子）、`a11y_types.h`（状态 / 取值域 / 选区值类型）、`a11y_provider.h`（桥抽象与注册表）、`a11y_text.h`（UTF-8 ↔ UTF-16 偏移映射） |
| JSON 值容器 | `json.h`（公共轻头；实现位于 `src/aurora/core/json/`，契约见 §9） |
| 其他 | `image.h`、`font.h`、`literals.h`、`version.h` |

根级头文件：`aurora.h`（唯一入口）、`aurora_fwd.h`（仅前向声明，供只需指针/引用的编译单元降低包含成本）、`aurora_pch.h`、`commands.h`、`todo.h`、`imperative.h`（命令式逃生舱：就地执行一段命令式代码块）。（原根级 `test_helpers.h` 已迁至 `tests/support/test_helpers.h`，定位为仓库私有测试设施，退出公共 API 与 `aurora_api.json`。）

---

## 2 几何与尺寸意图

### 2.1 基础结构

定义于 `include/aurora/core/types.h`。

| 结构 | 字段 / 方法 |
|:---|:---|
| `Point` | `x`、`y`（`types.h`） |
| `Size` | `width`、`height`；`Size::infinity()` 表示不限制（`types.h`） |
| `Rect` | `origin: Point`、`size: Size`；`right() = x + w`，`bottom() = y + h`（`types.h`） |
| `EdgeInsets` | `left` / `top` / `right` / `bottom`；`horizontal()` = 左右之和，`vertical()` = 上下之和（`types.h`） |

### 2.2 Length：尺寸意图

`Length` 表达「控件想要的尺寸」，由 AI 直接写在 `width` / `height` 属性上（`types.h`）。

```cpp
struct Length {
    LengthKind kind = LengthKind::WrapContent;
    float      value = 0.0F;   // Fixed：像素；Fraction：比例（0~1）
};
```

`LengthKind`（`types.h`）是**四个无参枚举值**，尺寸数值存放在 `Length::value` 中：

| 枚举值 | 语义 | 约束求解 |
|:---|:---|:---|
| `WrapContent` | 按内容决定 | `max` = 父剩余空间 |
| `Expand` | 填满父级可用空间 | `min = max = parentSize` |
| `Fixed` | 精准固定尺寸 | `min = max = value` |
| `Fraction` | 占父级比例 | `min = max = parent × value`，`value ∈ [0,1]` |

静态工厂：`Length::wrap()` / `expand()` / `fixed(px)` / `ratio(f)`。`fixed()` 与 `ratio()` 带 `AURORA_ASSERT` 边界校验。

### 2.3 强类型尺寸工厂

定义于 `include/aurora/core/dimension.h`，全部工厂位于 `namespace aurora` 内（`dimension.h`）；`au` 是 `aurora` 的推荐别名，因此 `au::px(...)` 等写法直接可用。

| 工厂 | 等价 |
|:---|:---|
| `px(float)` | `Length::fixed(v)` |
| `dp(float)` | `Length::fixed(v)`（当前实现与 `px` 同义） |
| `percent(float)` | `Length::ratio(fraction)` |
| `fill()` | `Length::expand()` |
| `auto_length()` | `Length::wrap()` |

用户字面量：`operator""_px`、`operator""_dp`（`long double` 与 `unsigned long long` 两个重载）。

### 2.4 Constraints：布局约束

```cpp
struct Constraints {
    Size min;
    Size max = Size::infinity();
    bool loose_width = false;   // 该轴上限只是「按需剩余空间」，非父级既定槽位
    bool loose_height = false;
    auto constrain(const Size& s) const noexcept -> Size;  // 逐轴 clamp 到 [min, max]
};
```

`operator==` 逐字段比较（含 `loose_*`），用作布局缓存键。`min ≤ max` 逐轴成立是布局求解的前提不变量。

`loose_width` / `loose_height` 是上限的**供给性质**标注，由 `FlexLayouter` 在给孩子施加约束时写入（语义与 Flutter 对照见 [`03-layout-render.md`](03-layout-render.md) §2.3）：`true` 轴的 `max` 只表示「还能给你这么多」，因此「展开自身占满父级」的修饰（`Modifier::align`）必须跳过该轴，否则会吞掉同轴兄弟的空间；纯几何 clamp（`constrain`）与 `fill_max_*` 不看该标记。

---

## 3 错误与结果

### 3.1 Error

`Error`（`core/result.h`）是结构化错误值，字段如下：

`code`（冻结 slug）、`message`、`suggestion`、`docs`、`where`、`hint`、`code_enum`、`severity`、`category`、`auto_fixable`、`retryable`、`fix_category`、`fix_params`。

错误码的**单一声明源**是 [`errors.toml`](../errors.toml)，由 `tools/gen/gen_error_codes.cpp` 生成 `error_codes.gen.h`、[`ERROR_CATALOG.md`](../ERROR_CATALOG.md) 与 `aurora_api.json`。新增或修改错误码只改 `errors.toml` 后重跑生成器，不手改产物。

### 3.2 Result\<T\>

`Result<T>`（`core/result.h`）是值语义的成功/失败二选一，构造 `Result(T value)` 与 `Result(Error err)` 均为隐式。

| 成员 | 签名 |
|:---|:---|
| `ok()` | `[[nodiscard]] auto ok() const -> bool`（`result.h`） |
| `operator bool` | `explicit operator bool() const`（无 `[[nodiscard]]`，`result.h`） |
| `error()` | `[[nodiscard]] auto error() const -> const Error&`（`result.h`） |
| `value()` | `const T&` 与 `T&` 两个重载（`result.h`） |
| `unwrap()` | `[[nodiscard]] auto unwrap() const -> T`（`result.h`） |

`Result<void>` 为特化（`result.h`），提供 `ok()` / `error()` / `operator bool`，**不提供** `value()` 与 `unwrap()`。

`Result<T>` 是成功/失败二态的唯一载体，公共 API 一律返回它；不再提供第二套等价的二态包装类型——同义并存只会让读者在两条出口间做无谓选择。

**常见误写**：`Result` **没有** `is_ok()` 成员。判成功一律用 `ok()` 或 `if (r)`。

### 3.3 使用约定

失败是值，不是异常：涉及外部输入（反序列化、文件、构造工厂）的函数返回 `Result<T>`，调用方必须先判 `ok()` 再取 `value()`。

```cpp
auto restored = au::serialization::from_json(json);
if (!restored.ok()) {
    au::Diagnostics::report(restored.error().message, "main.cpp", restored.error().code);
    return;
}
auto widget = restored.value();
```

---

## 4 诊断与降级

### 4.1 Diagnostic

`Diagnostic`（`core/diagnostics.h`）是库在「输入非法 / 部分代码缺失」时产出的结构化记录，而非崩溃或白屏。

| 字段 | 说明 |
|:---|:---|
| `severity` | 与 `Error::severity` 对齐的 `ErrorSeverity`（默认 `Warning`） |
| `category` | `ErrorCategory`（默认 `General`） |
| `message` | 人类可读描述 |
| `where` | 位置（`file:line` 或控件类型） |
| `code` | 机器可读 slug（可为空） |
| `code_enum` | 与 `code` 对应的 `ErrorCode`（无码时 `GeneralUnknown`） |
| `fix` | 可选结构化修复建议 `FixSuggestion` |

`FixSuggestion`（`diagnostics.h`）含 `code`、`description` 与可选 `auto_fix` 回调，`has_auto_fix()` 判断是否可自动修复。

`Diagnostic::to_json_line()` 输出 JSON 行，供工具链消费。`severity_str()` / `category_str()` 供遗留代码按字符串比较，新代码应直接用枚举比较（两个 `using` 别名已标记 `[[deprecated]]`）。

### 4.2 Diagnostics 收集器

`Diagnostics`（`core/diagnostics.h`）是全局诊断收集器，单线程 UI 无需加锁。

| 方法 | 说明 |
|:---|:---|
| `report(message, where, code, is_degraded, fix)` | 上报一条诊断。元数据由 `code` 对应的错误码表驱动注入；`code` 为空时退化为 `Warning` / `General`，并由 `is_degraded` 决定日志级别（`degraded` → `Error`，否则 `Warn`） |
| `warn(message, where, code)` | 便捷：普通警告 |
| `degraded(message, where, code)` | 便捷：降级渲染。严格模式下升级为硬失败 |
| `take()` | 取出并清空累计诊断（测试 / 工具用） |
| `count()` | 当前累计诊断数 |
| `get_last_diagnostics()` | 最近诊断环形缓冲（上限 `AURORA_RECENT_CAP` = 64） |
| `explain_diagnostic(code)` | slug → 人类可读解释，两个重载（字符串 / `ErrorCode`） |
| `collect_fixes()` | 收集最近诊断中携带的修复建议（不消费，同一 `code` 可能多次出现） |
| `apply_fix(code)` | 按 `code` 执行一次自动修复，命中返回 `true`，两个重载（slug 字符串 / `ErrorCode`，`diagnostics.h`） |
| `register_fix(code, fix)` | 注册错误码 → 修复策略映射 |
| `auto_fix_all()` | 应用全部已注册的自动修复，返回成功修复数 |

### 4.3 严格模式

`StrictMode`（`core/strict_mode.h`）取值 `Off`（默认）与 `On`。`On` 时 `Diagnostics::degraded` 升级为硬失败，用于 CI 把「降级渲染 / 深度超限」这类本应容忍的问题变为构建阻断。

- 推荐入口是 `Application` 上下文：`au::App().strict_mode(au::StrictMode::On).run(...)`；`Application` 另提供 `set_strict_mode()` / `strict_mode()`（`app/application.h`）。
- 无 App 上下文的场景（单元测试）用线程级全局开关 `aurora::set_strict_mode(m)` / `aurora::strict_mode()`；该状态是 `thread_local`，避免多线程竞争。
- 硬失败经 `on_strict_failure(message)` 执行：先 `AURORA_LOG_FATAL`，再调用注入的 handler（若有），最后 `std::terminate()`。它**不依赖** `AURORA_ASSERT`，因此 Release / `NDEBUG` 构建同样被阻断。
- 测试可注入 handler 拦截硬失败：`aurora::set_strict_failure_handler(h)`，传空恢复默认。

### 4.4 校验入口

`au::validate(root, max_depth = 64) -> Result<bool>`（`app/validate.h`）把空子节点、深度超限、未知类型报告为结构化 `Error`。子节点合法性是**运行时校验**：容器统一接受任意 `Node`，无编译期白名单。

---

## 5 日志

### 5.1 级别与格式

`LogLevel`（`core/log.h`）取值 `Trace` / `Debug` / `Info` / `Warn` / `Error` / `Fatal`；短标签由 `log_level_label()` 映射为 `TRC` / `DBG` / `INF` / `WRN` / `ERR` / `FTL`。

统一行格式：

```text
[YYYY-MM-DD HH:MM:SS][LEVEL][module@threadId filename:line] > content
```

### 5.2 Logger

`Logger`（`core/log.h`）是单例，默认级别 `Info`，默认输出到 stderr，内部状态无锁（单线程 UI 假设）。

| 方法 | 说明 |
|:---|:---|
| `instance()` | 取得全局唯一实例 |
| `set_level(LogLevel)` / `level()` | 设置 / 读取最低输出级别（低于阈值被丢弃） |
| `set_sink(LogSink)` | 重定向输出目标，传 `nullptr` 恢复默认 stderr |
| `set_enabled(bool)` / `is_enabled()` | 全局开关，禁用后静默丢弃 |
| `log(file, line, level, category, message)` | 记录一条日志（供宏调用） |
| `raw(category, message)` | 无前缀纯文本输出通道 |
| `set_raw_sink(LogSink)` | 设置 raw 通道目标，传 `nullptr` 恢复默认 stdout |

**`raw` 与诊断日志的区分**：`raw` 不过级别阈值、不加时间戳/级别/分类前缀，直接写 stdout，用于「程序产品」输出——CLI 的 JSON 结果与 usage、benchmark 表格、LSP/MCP 的 stdio 线协议帧。诊断、错误、警告一律走 `AURORA_LOG_*` 系列。

### 5.3 宏家族

定义于 `core/log.h`。

| 宏 | 级别 |
|:---|:---|
| `AURORA_LOG_TRACE(category, ...)` | Trace |
| `AURORA_LOG_DEBUG(category, ...)` | Debug |
| `AURORA_LOG_INFO(category, ...)` | Info |
| `AURORA_LOG_WARN(category, ...)` | Warn |
| `AURORA_LOG_ERROR(category, ...)` | Error |
| `AURORA_LOG_FATAL(category, ...)` | Fatal |
| `AURORA_LOG_RAW(category, ...)` | 无前缀，走 `Logger::raw`，始终输出 |

消息支持任意数量的类型安全可变参数，按 `operator<<` 折叠拼接；宏自动附加 `file:line`。

**硬规则**：项目中禁止直接使用 `std::cout` / `std::cerr` / `printf` / `fprintf` / `puts`。唯一允许直接触达标准输出的是 `src/aurora/core/log.cpp` 内的 sink 实现。`tools/`、`examples/`、`tests/` 中的遗留诊断打印经 `AURORA_TEST_PRINTF` / `AURORA_TEST_PRINTF_ERR` 桥接。

`init_console()`（Windows 生效，其它平台空实现）把控制台切到 UTF-8 代码页；`Logger` 首次使用时亦会自动触发。

---

## 6 异步执行底座

### 6.1 ThreadPool

`ThreadPool`（`core/thread_pool.h`）是有界线程池，析构时 stop + join，无悬挂线程。

| 成员 | 说明 |
|:---|:---|
| `explicit ThreadPool(worker_count = default_worker_count(), force_deferred = false)` | 构造并启动 worker；传 0 回落默认值；`force_deferred=true` 任意平台显式开延迟排空模式 |
| `default_worker_count()` | `hardware_concurrency()`，下限 2 |
| `worker_count()` | 当前 worker 线程数（deferred 模式恒为 0） |
| `is_deferred()` | 是否处于「任务只入队、由宿主 `pump()` 排空」的延迟模式 |
| `pump() -> std::size_t` | 延迟模式专用：在**当前线程**执行至多「进入时已入队」的任务（新入队留待下次，防饿死宿主帧），返回实际执行数；非 deferred 恒 0 |
| `pending_count()` | 当前排队未执行任务数（近似值，仅供诊断） |
| `execute(std::function<void()>)` | fire-and-forget，异常在 worker 内被捕获，不向外传播 |
| `submit(F&&) -> std::future<R>` | 提交并返回 future，异常经 future 传播 |
| `default_pool()` | 进程级默认池（Meyers 单例），`au::async` 与协程均经它调度 |

拷贝与移动均被删除。

**延迟排空（deferred）模式**：`AURORA_COMPILE_TIME_DEFERRED` 仅在 Emscripten 且未启用 pthreads（`__EMSCRIPTEN_PTHREADS__` 未定义）时为 true——浏览器单线程下 `std::thread` 不可用，池不启动 worker，任务只入队，由宿主在安全点 `pump()` 于当前线程排空。宿主接线点是 `Application::step_frame()` 的**帧尾**（`pump_deferred_work()`，见 [`06-app-platform.md`](06-app-platform.md) §2.4），刻意**不放在 `present()` 上屏路径**：空闲帧被脏区决策整段跳过就没有 present，挂那里会让续体饿死。故 `au::async` / 协程续体在浏览器下随帧回写、不开线程也不丢任务；真并行经构建开关 `AURORA_ENABLE_WASM_PTHREADS`（`-pthread`，见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)）回到普通 worker 池语义，但宿主页面须跨源隔离（该开关的取舍与前提同条）。注意：deferred 下 `submit()` 的 `future.get()` 不可与 `pump()` 同线程互等（会自锁），消费续体应经 `Task::then` / 协程或帧尾泵。deferred 池析构同样排空剩余队列，与 worker 池「drain-until-empty」语义对齐。`is_deferred()` 是运行期口径的唯一查询面（消费者勿散写 `#ifdef`）。

### 6.2 使用约定

禁止裸 `std::thread` 执行后台工作，也禁止在 `on_click` 等 UI 回调中直接阻塞。所有耗时操作经 `au::async` / `au::co_async` 提交到默认池（契约见 [`02-state.md`](02-state.md) §5）。

`core/thread.h` 提供单线程 UI 契约的配套守卫：`MainThreadOnly<T>`（`thread.h`）包装一个值，debug 下断言读写均发生在构造它的线程，模板参数 `Check = false` 时为零开销特化（不存 owner 线程、不断言）；宏 `AURORA_MAIN_THREAD` 为函数标注「必须在主线程调用」（clang `annotate` 属性，供静态分析 / 文档工具识别；GCC 下为 no-op，运行期契约仍由 `MainThreadOnly` 兜底）。

---

## 7 其他基础能力

| 头文件 | 能力 |
|:---|:---|
| `color.h` | `Color` 结构（`color.h`）与具名颜色（`aurora::colors` 子命名空间） |
| `time.h` / `duration.h` | 时间表示与时长 |
| `event_stream.h` | 事件流 |
| `file_watcher.h` | 文件监听 |
| `utf8.h` / `string_util.h` | UTF-8 处理与字符串工具 |
| `platform.h` | 编译期平台 / 架构 / 能力宏探测（零运行时成本） |
| `enums.h` | 跨模块共享枚举 |
| `image.h` / `font.h` | 图像与字体的基础类型（具体能力见 [`03-layout-render.md`](03-layout-render.md)） |
| `version.h` | 库版本 |
| `accessibility.h` | 无障碍基础（语义树 / 事件 / 设置；详见 §7.2） |
| `todo.h`（根级） | `au::TODO` 占位回调 |

### 7.1 au::TODO

`au::TODO`（根级 `todo.h`）是占位回调，用于标记尚未实现的事件处理——编译通过，运行时触发时经 `Diagnostics::warn("TODO", what)` 输出警告。它是占位回调而非错误码。

```cpp
auto btn = au::Button(au::ButtonProps{ .label = "OK" });
btn.set_on_click(au::TODO("handle_click"));   // 编译通过，运行时留可读警告
```

### 7.2 无障碍基础（a11y）

无障碍能力按**依赖方向**分居两层：`core/` 提供平台中立的**类型与指针级钩子**，需要遍历控件树的构建与快照逻辑落在 `widget/`（见 [`04-widget.md`](04-widget.md) §1）。以下四头全部**平台中立**（不含任何平台头，不依赖 GUI 后端）：

| 头文件 | 内容 |
|:---|:---|
| `accessibility.h` | 角色（`AccessibilityRole`）、动作（`AccessibilityAction` / `AccessibilityActionRequest`）、节点（`AccessibilityNode`）、事件（`AccessibilityEvent` / `AccessibilityEventKind`）、设置（`AccessibilitySettings`）、语义裁剪（`apply_semantic_pruning`）、角色推断与默认动作（`infer_accessibility_role` / `default_actions`）。**只以指针持有 `Widget`**（前置声明），树遍历入口见 `widget/a11y_tree.h` |
| `a11y_types.h` | 纯值类型：`AccessibilityState`（13 位状态集）、`AccessibilityRange`（min/max/step/value）、`AccessibilityTextSelection`（UTF-8 字节半开区间）、`AccessibilityScrollRange` |
| `a11y_provider.h` | 桥抽象 `a11y::Provider` + 进程级 `a11y::detail::ProviderRegistry`（注册 / 广播 / 注销） |
| `a11y_text.h` | `UtfOffsetMap`（UTF-8 ↔ UTF-16 偏移换算，代理对按码点起点夹取）、`utf8_to_utf16` / `utf16_to_utf8`、`TextUnit` / `expand_to_unit` |

**语义树节点（`AccessibilityNode`）** 在既有 `role / name / value / hint / bounds / actions` 之外新增：

| 字段 | 说明 |
|:---|:---|
| `id` | `Widget::runtime_id()` 取值——进程级原子自增、构造时分配、生命周期内恒定。**不用**路径字符串（虚拟化列表复用 / 重排下不稳定）也不用指针值（无跨事件可比性） |
| `state` | `AccessibilityState` 位集 |
| `range` | `std::optional<AccessibilityRange>` |
| `level` | `std::optional<int>`，标题层级（文档结构导航用） |
| `is_control` / `is_content` | 裁剪结论：是否进入平台的「控件视图」/「内容视图」 |
| `stable_key` | 宿主经 `Widget::set_stable_key()` 声明的**跨重建**稳定键（对标 HTML `id`；空 = 未设） |
| `labelled_by` | 名字来源的**键**（`Widget::set_labelled_by()`，对标 `aria-labelledby`；空 = 未声明） |
| `labelled_by_id` | 上述键在**本次投影内**解析出的目标 `id`（0 = 未声明 / 未命中 / 环上 / 目标无名）；三桥据此投影关系 |

新增字段一律带默认成员初始化，且**追加在既有字段之后**——既有聚合初始化与序列化面零变化。

`id` 与 `stable_key` 是**两套身份**，各司其职、不可互替：`id = runtime_id()` 进程级自增、重建即变，是平台侧元素句柄（事件目标、缓存键）；`stable_key` 由宿主命名、跨重建不变，是**宿主持久引用**的唯一合法形式（`labelled_by` 只能引它，否则同名重排的树一对不上）。

**名称回退链（对标 ARIA accessible name computation）**：**引用式标签关联**（宿主经 `Widget::set_labelled_by()` 声明，对标 `aria-labelledby`）→ `explicit_accessibility_label()`（宿主经 `Widget::set_accessibility_label()` 声明，对标 `aria-label`）→ `accessibility_label()` 覆写（`Button` 取 label、`Text` 取内容…）→ （`Text` / `TextInput` 角色）控件文本 / 取值 → **兄弟标签关联** → **唯一文本子节点**之标签。第 2–6 级求值实现在 `a11y_tree.h` 的 `detail::resolve_accessibility_name`，与上述顺序逐字一致；**引用式关联**一级**不在**该函数内求值（见下）。

- **兄弟标签关联**（`detail::sibling_label_name`）是**几何启发式**：同容器直接子节点中取与本控件「垂直重叠 + 水平相邻（间隙 ≤ 12 DIP）」的最近文本兄弟，且只对 `Checkbox` / `Switch` / `Slider` 三种角色生效；未绘制、无父、纵向堆叠（`Column { Slider, Text }`）一律不命中——**宁可不念也不猜错**。
- **唯一文本子节点**只在该子树恰有一个 `Text` / `RichText` / `Label` 子节点时生效（多个候选即弃权，避免把整段内容拼成名字）。
- 标签来源（兄弟级与子节点级各读一段文本）统一走 `detail::declared_label`，故「钩子覆写取到名的控件」与「显式声明取到名的控件」互为可用的标签来源，两条路同源。
- `set_accessibility_label(std::string)` 返回 `Widget &`（可链式），键名 `accessibility_label` 随基类 props 往返（**未声明不写键**，空串语义是「撤除声明、回落回退链」而非「名字为空」）；同值重复设置不上报事件，值真变化上报 `NameChanged`。
- **引用式标签关联**（`set_stable_key` + `set_labelled_by`，对标 HTML `id` + `aria-labelledby`）是回退链的**最高优先级**，也是唯一不在 `resolve_accessibility_name` 内求值的一级：引用可指向树序上**更靠前**的控件（先声明的标题），单趟递归建名时无从查起，故由 `detail::apply_labelled_by_relations` 在整棵节点树建完后做**两趟**后置解析（一趟收集键索引 + 一趟解析，成本 O(节点数)，无人声明时除空遍历外零成本）。生产路径唯一入口是 `build_accessibility_tree`，三桥（UIA / AT-SPI / ARIA）与 `a11y_diff::build_tree_snapshot` 共用其结果，故 `labelled_by_id` 与跟随后的 `name` 天然同值。
- 引用的降级判据（每种原因**每进程提示一次**，经 `Diagnostics::degraded`，读屏在线时不逐帧刷屏）：① 键不在本次投影的树内（含目标 `show == false` 未入树）⇒ 保留自身名字、`labelled_by_id = 0`；② 目标自身无可读名字 ⇒ 同上（**宁念旧名也不念空**，且 ARIA 的 `aria-labelledby` 会压制 `aria-label`，投影空目标等于抹掉名字）；③ 链式引用（A→B→C）逐层展开，菱形引用经已解析集合去重、**环**（含自引用）在闭合处断掉，断环者不投影关系 ⇒ 三桥侧投影出的关系图始终无环；④ 同树多个控件共用一个 `stable_key` ⇒ 取先序第一个并提示一次。
- `set_stable_key` / `set_labelled_by` 同样「未声明不写键」，键名 `stable_key` / `labelled_by` 随基类 props 往返（可过 `to_json` / `from_json`）；`set_stable_key` 不动名字故不上报事件，`set_labelled_by` 变化上报 `NameChanged`。名字跟随的失效面：目标改名 ⇒ 引用者的 `name` 随之变化，`TreeDiff` 对**两侧节点各报一次** `FieldChange::Name`（判据在 `utest_a11y_diff.referenced_label_rename_propagates_name_to_dependent`）。
- **动态性的边界（如实申报）**：名字跟随发生在「下一次语义树投影」，而投影是拉取式惰性的（事件只 `mark_dirty()`）。`Text::content` 等控件自带文案的变化**不上报** a11y 事件，故若一帧内再无其他脏源，读屏读到的仍是旧名。要即时跟随，请用 `set_accessibility_label()` 改标签源（上报 `NameChanged`）或让目标随任一已接线事件（取值 / 焦点 / 结构变化）一起更新。

**剩余缺口（如实申报）**：`set_labelled_by` 的**关系对象**只在 Windows UIA（`UIA_LabeledByPropertyId`，真机判据见 `tools/verify/win32_ua_live_probe.cpp` #21）与浏览器 ARIA（`aria-labelledby` IDREF）两桥投影；Linux AT-SPI 的 `GetRelationSet` 仍申报为空——其 `(ii)` 索引对须由 a11y 注册表分配的应用序号推出，属独立增量。名字本身在三桥一致（语义树内已解析完毕），故 Linux 读屏念到的仍是目标标签文案，缺的只是「由 … 标注」这条可导航关系。

**事件通道（两条并列，互不覆盖）。** 事件处理器在 `accessibility.h` 内是**进程级单槽**（`current_accessibility_event_handler()`，宿主用）；桥另经 `detail::a11y_broadcast_hook` 独立接收同一批事件。两条通道**并列**而非链式：宿主处理器永远被调用，桥广播独立生效，安装顺序无关（历史上「保存旧处理器 + 链式包裹」会让先安装者失效）。另有第三条并列通道 `detail::a11y_widget_destroy_hook`：控件实例销毁前**带上其指针**广播一次，供桥判定「我缓存的根是不是没了」（语义树事件只能给出宿主容器，无法承载这一判定）。

`AccessibilityEventKind` 含 `Announcement`（动态播报 / Live Region）：`announce_accessibility(text, target)` 走独立事件通道，**不经**语义树 diff——toast / 状态提示 / 异步结果这类临时文本没有焦点或取值变化，只有此通道能被读屏感知。

`screen_reader_active`（`AccessibilitySettings`）的语义是 **heuristic**：以「读屏主动取根对象」近似「有读屏在线」，由桥激活时回填 `true`、桥去激活时回填 `false`；读屏退出无反向信号，故不保证及时复位。

---

## 8 需求规格

### 8.1 #18 安全的内存与所有权模型

**核心目标：** AI 生成无悬空指针、无泄漏的代码。

**需求陈述：**

- UI 树通过 `std::shared_ptr<Widget>` 管理所有权，父子关系即「树内拥有」：拷贝即共享、移动即转移，整棵树随根 `Node` 析构而析构。AI 不需要手动 `delete`，也不必担心 use-after-free。
- 禁止**拥有语义**的裸指针进入公开 API；非拥有的观察指针 / 句柄（如 `Binding<T>::target()` 返回的 `State<T>*`）除外。事件回调需要引用「触发者」时，用稳定标识 `Node::id()` 加查询，而非引用或指针。
- **例外**：Inspector / 调试内部句柄（如 `selected_widget()`、`set_surface_getter`）可返回裸 `Widget*` / `Surface*`，但必须配弱引用守卫（生命周期由树 `shared_ptr` 持有），且不得进入业务公开 API。

```cpp
// 规则：跨组件引用 = 稳定标识，非裸指针
struct AppState {
    std::string focused_field_id;   // 对应 Node::set_id() / id() 的稳定标识
};
auto node = au::find_node_by_path(root, path);  // widget/inspect.h，位于 aurora 命名空间（无 inspect 子命名空间）；返回 Node，生命周期由树 shared_ptr 持有

// ❌ void on_click(Button* sender, Event* e);
// ✅ void on_click(std::string_view sender_id, const au::Event& e);
```

**单线程 UI（不变量，非并发 API）：** 所有 UI 构建、状态变更、事件派发、重绘都在 UI 线程进行，无锁无原子。`State<T>` 的读写与 `Node` 的复制移动只在 UI 线程发生，天然无数据竞争。`shared_ptr` 仅用于简化树的生命周期管理，并非为多线程共享；快照等只读场景可安全跨线程共享。

**验收标准：** 公开头文件中不出现裸 `Widget*` 子节点或回调参数；AI 生成的树代码没有任何 `delete`；耗时工作的结果只经 #19 的回投路径写回状态。

### 8.2 #19 结构化异步与并发模型

**核心目标：** AI 轻松处理耗时操作。

**需求陈述：** UI 线程唯一，所有 UI 更新必须在 UI 线程；耗时操作必须经 Aurora 提供的异步原语执行，禁止裸 `std::thread`，也禁止在 UI 回调中直接阻塞。后台执行底座是 `aurora::ThreadPool`（§6.1），`au::async` / `au::co_async` 均向该池提交任务。

**验收标准：** 全部后台工作都可在无 GUI 环境（headless）下完成并回到主线程；`ThreadPool` 存活期间不出现新建的游离 `std::thread`；超时与取消路径只改变结果的投递去向，绝不中断用户函数。

> 异步 API 契约（`au::async`、`Task<T>::then`、`co_async`、`CoroTask<T>`、`launch`）见 [`02-state.md`](02-state.md) §5；定时与周期任务由 `Scheduler` / `Timer` 承担，见 [`06-app-platform.md`](06-app-platform.md)。

### 8.3 #21 错误恢复与降级渲染

**核心目标：** AI 生成的错误 UI 不会崩溃。

**规则：**

1. 任何组件在任何非法状态下都不崩溃。无 `source` 的 `ImageView` 渲染为带虚线框的占位符；非法 `font_size` 回退默认值并告警；负 `gap` 钳到 0 并告警。
2. 降级渲染有统一视觉语言：边框 + 灰色背景 + 说明文字。可直接使用 `au::Placeholder`（`widget/placeholder.h`）作为通用降级占位盒，AI 通过快照即可识别「这个位置降级了」。
3. 所有降级产生结构化警告（§4.1），典型如：

```text
[render-degraded] Image.source:
  - Received: "" (empty string)
  - Fallback: placeholder rendered (200x150, dashed border)
  - Fix: provide a valid file path or URL
  - Location: main.cpp
```

4. 严格模式用于生产与 CI（§4.3）下，降级即致命失败。默认宽松语义由各组件内置降级：非法或缺失属性 → 渲染占位框 + 结构化警告。

**验收标准：** 对任意非法输入构造的树，`HeadlessSurface` 渲染不崩溃、产出占位像素与结构化警告；开启严格模式后同一输入返回致命失败。

### 8.4 #23 部分代码容错（半成品可编译可运行）

**核心目标：** AI 可增量开发。

**需求陈述：** 任何组件在任何「半成品」状态下都不应崩溃，而是优雅降级。这对 AI 的增量开发循环至关重要：先生成骨架 → 编译通过 → 逐步填充 → 每步都可运行。

- 编译期：未完成的组件不应导致整个项目编译失败；以 `au::TODO`（§7.1）占位标记尚未实现的事件处理。
- 运行时：缺少必要属性的组件渲染为占位框而非崩溃（降级视觉语言见 #21）。

**验收标准：** 只填了 label 的控件、带 `au::TODO` 回调的界面，均可编译、可渲染、可运行，并留下可读警告指明未完成处。

---

## 9 JSON 值容器（`core/json.h`）

> 覆盖 `include/aurora/core/json.h`（公共轻头）与 `src/aurora/core/json/`（`value.cpp` / `parse.cpp` /
> `sax.cpp` / `pointer.cpp` / `dump.cpp`）。
> 本模块是 Aurora 自有 JSON **值容器与编解码器**的唯一权威；UI 树线格式、差分补丁与工具链侧的
> 序列化契约见 [`08-tooling.md`](08-tooling.md) §2，复制即用配方见 [`GUIDELINE.md`](../GUIDELINE.md)。

### 9.1 定位与边界

`aurora::json`（别名 `au::json`）提供纯 DOM 的 JSON 值容器与解析 / 序列化，取代此前对第三方单头
JSON 库的全量依赖。四条边界约束：

| 约束 | 内容 |
|:---|:---|
| **公共模块** | 头位于 `include/aurora/core/json.h`，随 semver 承诺稳定性；值类型判别 `aurora::json::Type` 经 `tools/include/known_enums.h` 以 `JsonType` 进 `aurora_api.json` 的 `enums` 段（AI 可枚举） |
| **轻头承诺** | 目标 < 500 行；头内不落模板实现与大段逻辑，实现全部在 `src/aurora/core/json/`。这是编译收益的前提——伞头 `aurora.h` 经 `widget/props_io.h` 传递包含本头，其体量直接进入每个消费者 TU |
| **层边界** | 只允许 `#include "aurora/core/..."` 与标准库，由 `tools/check/check_core_layer_boundary.py` 守护 |
| **零外部依赖** | 纯计算模块：不触网络、不触 GPU、不触平台 API |

值类型与读写语义刻意区别于常见的第三方 JSON 库：**读缺失键绝不隐式插入**、**无隐式出向类型转换**、
**对象键序为插入序**、**数值三判别 + 超域保真**、**const 与非 const 访问行为一致**。

### 9.2 类型系统（9 值）

| `Type` | 序数 | 承载类型 | 语义 |
|:---|--:|:---|:---|
| `Null` | 0 | — | `null` |
| `Bool` | 1 | `bool` | `true` / `false` |
| `Int` | 2 | `std::int64_t` | 有符号整数（int64 域内） |
| `UInt` | 3 | `std::uint64_t` | 无符号整数（> `INT64_MAX` 的正整数） |
| `Double` | 4 | `double` | 双精度浮点 |
| `RawNumber` | 5 | `RawNumber` | 保真数字（超 int64/uint64 域，或与 double 最短往返不一致的原字面量） |
| `String` | 6 | `std::string` | UTF-8 字符串 |
| `Array` | 7 | `std::vector<Value>` | 数组（插入序） |
| `Object` | 8 | `std::vector<std::pair<std::string, Value>>` | 对象（插入序） |

**序数与内部 `std::variant` 的 alternative index 强制同构**（模块内部契约）：`type()` 实现为
`static_cast<Type>(data_.index())`，零分支。两表任一侧重排即由 `tests/unit/utest_json.cpp` 的逐
枚举量断言捕获，不存在静默漂移窗口。

类型谓词：`is_null()` / `is_bool()` / `is_int()` / `is_uint()` / `is_double()` / `is_raw_number()` /
`is_string()` / `is_array()` / `is_object()`，外加合成谓词 `is_number()`（Int ∪ UInt ∪ Double ∪
RawNumber）与 `is_integer()`（Int ∪ UInt）。

### 9.3 读出口三族

| 族 | 入口 | 缺失 / 类型不符时的行为 |
|:---|:---|:---|
| **宽容** | `as_or(fallback)`、`as_or(key, fallback)`、`as_or_at(index, fallback)` | 回退 `fallback`；空安全、零 UB、不返回指针 |
| **指针** | `at(key)`、`at(index)`、`find(key)` | 返回 `nullptr`；**不抛异常** |
| **严格** | `as<T>()`、`get<T>(key)` | 返回 `Result<T>`，失败码 `json-type-mismatch` |

宽容族是「取不到就用默认」场景的官方入口，指针族用于需要区分「缺失」与「类型不符」的场合，
严格族用于必须成功且需要结构化错误的位置。另有**无模板族**（免实例化，供热路径与
`widget/props_io.h` 的宽容解析复用）：`as_bool()` / `as_int()` / `as_double()` / `as_string()` /
`as_raw_number()`，统一返回 `std::optional`。

**`at` 不抛是对 STL 的刻意偏离**：STL 与常见 JSON 库的 `at` 一律「缺失或越界即抛
`std::out_of_range`」，本模块的 `at` 返回指针、缺失返回 `nullptr`，使调用方在 `noexcept` 路径上
也能安全探测。`find(key)` / `contains(key)` 是 `at(key)` 的等价入口，保留供键语义自述。

**`as<T>` 支持封闭类型集**（无用户特化点；域外类型编译期拒绝）：`bool` / `int` / `std::int64_t` /
`std::uint64_t` / `std::size_t` / `float` / `double` / `std::string` / `std::string_view`。数值族按
「值域内可提升、收窄可丢精度」规则转换。**`RawNumber` 全程不参与数值转换**——`is_string()` 对其为
false，`as_string()` 亦不返回其文本，唯一读出口是 `as_raw_number()`；保真语义与「可被数值消费」
互斥，混入会掩盖精度损失。

### 9.4 写接口与引用失效

| 接口 | 适用 | 语义 |
|:---|:---|:---|
| `set(key, v)` | Object | 插入或覆盖；**重复键后值覆盖前值，位置保持首次插入处** |
| `push_back(v)` | Array | 追加（不提供 `emplace_back`） |
| `reserve(n)` | Array / Object | 容量预留（批量构造热路径） |
| `erase(key)` / `erase_at(index)` | Object / Array | 删除，返回是否命中 |
| `clear()` | 容器 | 清空为对应空容器 |

**写操作使既有引用与指针失效**：Array / Object 由 `std::vector` 承载，故 `set` / `push_back` /
`erase` / `clear` 之后，先前取得的 `Value&` 与指针均不可再用。这是写接口返回 `void` 而非引用的
原因——嵌套写入统一走指针重载，失效点显式可见，不提供「返回引用且可链式持有」的接口。

**禁隐式出向转换**：算术与字符串构造为**隐式入向**（按 `T` 的精确类别落域，绝不跨类别），但
**不提供** `operator T()` / `operator bool()`。判空与取值一律走显式谓词与读族，避免
「`if (value)` 编译通过」这类静默错误。

### 9.5 相等语义

**同 `Type` 严格比较**：跨数值类型不相等（`Int(1) != Double(1.0)`）。这比「按值跨类型相等」的
第三方惯例更严格，代价是差分把 `1` → `1.0` 视为一次真实变更，收益是类型变更不再被吞掉；
`RawNumber` 按文本比较。

### 9.6 解析

```cpp
struct ParseOptions {
    std::size_t max_depth = 512;   // 嵌套深度上限，超限 → json-depth-exceeded
    bool validate_utf8 = true;     // 字符串与文档级 UTF-8 校验
};

// 解析完整 RFC 8259 文档；失败持结构化 Error（含 line / column / offset）
[[nodiscard]] auto parse(std::string_view input, ParseOptions opts = {}) -> Result<Value>;
```

实现为手写递归下降字符级引擎（位于 `src/aurora/core/json/sax.cpp`，见 §9.10），`parse.cpp` 只是
「引擎 + 内置 DOM 装配器」的薄壳。`parse` 的唯一出口是 `Result`——**无抛出式重载、无 discarded
双模式**，`Error` 携带 line / column / offset 与 ≤ 24 字节的上下文片段。

| 边界情形 | 规则 |
|:---|:---|
| 前导 UTF-8 BOM | **拒绝**（RFC 8259 §8.1 明禁），无宽容开关 |
| `\u0000` | **合法**：解出内嵌 NUL 原样存入 `std::string`，视图长度不被截断 |
| 孤立代理项（未配对 / 配对顺序错） | **拒绝** |
| 顶层标量（`42` / `"s"` / `true`） | **合法**（`JSON-text = ws value ws`） |
| 顶层尾随内容（`{} {}`） | 拒绝 |
| 重复键 | 后值覆盖前值，位置保持首次插入处 |
| 空输入 / 仅空白 | 拒绝 |
| 嵌套深度超限 | `json-depth-exceeded` |

**数字分派（三判别 + 保真）**：整数字面量先试 `int64`、再试 `uint64`，两者皆溢出则存为
`RawNumber`；小数字面量经 `std::from_chars` 解析为 `double`，若域外（如 `1e999`）或
`std::to_chars` 最短往返与原文不一致（忽略符号、小数点、指数与前后零后比对）则同样存为
`RawNumber`。单一规则，无特例分支。`-0` 归 `Int(0)`（JSON 无负零整数语义），`-0.0` 归
`Double(-0.0)`。

### 9.7 序列化

```cpp
struct DumpOptions {
    int indent = -1;             // < 0 紧凑单行；≥ 0 为每层缩进空格数
    bool ensure_ascii = false;   // true 时非 ASCII 转 \uXXXX（含代理对编码）
};

[[nodiscard]] auto dump(const Value &v, DumpOptions opts = {}) -> Result<std::string>;
auto dump_into(const Value &v, std::string &out, DumpOptions opts = {}) -> Result<void>;
```

- 键序 = 插入序；
- `Double` 文本化走 `std::to_chars` 最短往返，整值补 `.0` 后缀（`3.0` 不退化为 `3`，类型可辨识）；
- `RawNumber` 原样输出存储的字面量（保真闭环）；
- **非有限值硬失败**：`NaN` / `Inf` 无 RFC 8259 文本形态，返回 `json-value-not-serializable`，
  不宽容转 `null`；
- 转义：`"` 与 `\` 必转，`< 0x20` 控制字符转 `\u00XX`，`/` 不转；`ensure_ascii` 时非 ASCII 转
  `\uXXXX`（BMP 之外经代理对）；
- `dump_into` 复用调用方缓冲，供响应帧热路径避免每帧分配。

### 9.8 错误码

复用既有 `JsonParseError` / `json-parse-error` 作为解析失败与 DOM 访问失败的统一码位；新增三条
（`category` 一律 `io`，元数据见 [`errors.toml`](../errors.toml)，全量清单见
[`ERROR_CATALOG.md`](../ERROR_CATALOG.md)）：

| enum | slug | 触发 |
|:---|:---|:---|
| `JsonDepthExceeded` | `json-depth-exceeded` | 解析嵌套超 `ParseOptions::max_depth` |
| `JsonTypeMismatch` | `json-type-mismatch` | `as<T>()` / `get(key)` 类型不符或域外 |
| `JsonValueNotSerializable` | `json-value-not-serializable` | `dump` 遇 `NaN` / `Inf` |

错误消息模板含多个占位符（`{max}` / `{line}` / `{column}` / `{expected}` / `{actual}`），而
`format_message` 对**缺失占位符原样保留**，故这些模板每次调用都必须传齐全部键——实现以「构造错误
消息的唯一私有工厂」集中保证，不在调用点散落手拼。

### 9.9 与序列化层的值类型关系

`widget/` 层的序列化接口（`to_json` / `from_json` / `diff` / `apply_patch` 等）当前仍以第三方单头
JSON 库的别名 `Json` 作为值类型；本模块提供其收敛目标类型 `aurora::json::Value`。收敛顺序为
「库内改写 → 工具与测试改写 → 删除别名与第三方单头」，属尚未执行的动作；收敛完成前两者并存，
`widget/` 层的签名以现状为准（别名定义点与迁移约束见 [`08-tooling.md`](08-tooling.md) §2.1）。

本模块的覆盖用例分三处（套件名恒等于文件 stem）：`tests/unit/utest_json.cpp`——语法合规矩阵、
边界语义、全类型往返、键序与重复键、错误位置与占位符填充、类型判别与内部存储的同构、封闭读类型集、
保真数字不参与数值转换、严格相等语义、容器读写与迭代视图、序列化转义与非有限值拒绝、Pointer 寻址
与写路径补齐；`tests/unit/utest_json_sax.cpp`——SAX 出口（§9.10）；`tests/unit/utest_json_conformance.cpp`
——外部语料合规验收（§9.12）。迁移期另有 `tests/unit/utest_json_compare.cpp`，与第三方单头做语义
对拍，属过渡脚手架，随该单头移除一并删除。

### 9.10 SAX 出口（`parse_sax`）

```cpp
class SaxHandler {
  public:
    SaxHandler() = default;
    virtual ~SaxHandler() = default;
    SaxHandler(const SaxHandler &) = delete;            // 不可拷贝
    SaxHandler &operator=(const SaxHandler &) = delete;
    SaxHandler(SaxHandler &&) = delete;                 // 不可移动
    SaxHandler &operator=(SaxHandler &&) = delete;

    virtual auto on_null() -> bool = 0;
    virtual auto on_bool(bool value) -> bool = 0;
    virtual auto on_int(std::int64_t value) -> bool = 0;
    virtual auto on_uint(std::uint64_t value) -> bool = 0;
    virtual auto on_double(double value) -> bool = 0;
    virtual auto on_raw_number(std::string_view digits) -> bool = 0;
    virtual auto on_string(std::string_view decoded) -> bool = 0;
    virtual auto on_array_start() -> bool = 0;
    virtual auto on_array_end(std::size_t count) -> bool = 0;
    virtual auto on_object_start() -> bool = 0;
    virtual auto on_object_key(std::string_view key) -> bool = 0;
    virtual auto on_object_end(std::size_t count) -> bool = 0;
};

[[nodiscard]] auto parse_sax(std::string_view input, SaxHandler &handler, ParseOptions opts = {}) -> Result<void>;
```

**单引擎不分叉**：`parse_sax` 与 `parse` 共用同一字符级引擎。DOM 出口的实质是「本引擎 + 内置装配器
（`DomBuilder`，一个 `SaxHandler` 实现）」，故词法、数字分派、转义还原、边界语义与错误消息全仓只有
一份实现，不存在 DOM 与 SAX 行为漂移的可能。

| 事件 | 触发时机与载荷 |
|:---|:---|
| `on_null` | 遇到 `null` |
| `on_bool` | 遇到 `true` / `false`，载荷为已判别的 `bool` |
| `on_int` / `on_uint` / `on_double` | 数字按 §9.6 的三判别落域后，以**对应域**回调 |
| `on_raw_number` | 数字落 `RawNumber`（超域或往返失真），载荷为**原字面量文本** |
| `on_string` | 字符串：**转义与 `\uXXXX`（含代理对）已还原为 UTF-8** 后才回调 |
| `on_array_start` / `on_array_end(count)` | 数组开闭；闭合事件载荷为元素个数 |
| `on_object_start` / `on_object_key(key)` / `on_object_end(count)` | 对象开 / 键 / 闭合；闭合载荷为成员个数 |

三条语义约束：

- **提前终止即成功**：回调返回 `false` 表示消费者主动停止（如只关心前几个字段），`parse_sax` 立即
  停止解析并返回**成功**——这是消费者的决定而非错误。DOM 路径的装配器恒返回 `true`，故 DOM 出口
  永不触发该分支。
- **视图仅在回调期间有效**：`on_string` / `on_object_key` / `on_raw_number` 收到的 `string_view`
  别名引擎内部的可复用缓冲，下一次回调即被覆写；需要留存必须自行拷贝。
- **失败口径与 DOM 出口逐字一致**：非法文档的 `Error`（code / message）与 `parse` 完全相同，由
  `tests/unit/utest_json_sax.cpp` 对同一批非法输入逐字比对 message 守住。

覆盖用例见 `tests/unit/utest_json_sax.cpp`：事件序列（标量 / 容器 / 混合嵌套）、转义与代理对在发
事件前已还原、闭合事件的成员与元素计数、提前终止（含首个事件前终止）按成功返回且后续事件不再
派发、失败口径与 DOM 出口一致、深度上限同样生效、顶层标量。

### 9.11 JSON Pointer（RFC 6901 最小集）

```cpp
[[nodiscard]] auto find_pointer(const Value &root, std::string_view pointer) -> const Value *;
[[nodiscard]] auto find_pointer(Value &root, std::string_view pointer) -> Value *;
[[nodiscard]] auto resolve_for_write(Value &root, std::string_view pointer) -> Result<Value *>;
[[nodiscard]] auto erase_pointer(Value &root, std::string_view pointer) -> Result<bool>;
```

| 入口 | 语义 |
|:---|:---|
| `find_pointer`（const / 非 const 重载） | 只读寻址。命中返回子值指针；未命中返回 `nullptr` |
| `resolve_for_write` | 写路径寻址：**自动补齐缺失的中间容器**，返回可写槽位 |
| `erase_pointer` | 删除末段所指成员；成功时 `bool` 表示**是否命中**（未命中不是错误） |

**路径语法**：段以 `/` 分隔（`"/a/0/b"`）；**空串指向 `root` 自身**；段内 `~1` 还原为 `/`、`~0`
还原为 `~`。**非空且不以 `/` 开头即语法非法**——不存在「相对路径」或省略前导斜杠的宽容形式。

**错误归属二分**（错误码见 §9.8）：

| 情形 | 结果 |
|:---|:---|
| `pointer` 语法非法（非空且不以 `/` 开头） | `find_pointer` 返 `nullptr`；写路径返 `json-parse-error` |
| 段不存在 / 数组索引越界 / 段与值类型不符 / 对非容器取子项 | `find_pointer` 返 `nullptr`；写路径返 `json-type-mismatch` |
| 空 `pointer` 传入 `erase_pointer`（指向根自身） | 恒失败——根不可删除 |
| 写路径 `pointer` 为空 | 成功，返回 `&root` |

**写路径补齐规则**：中间段遇 `null` 占位时，按**本段**的形态决定建 `Object` 还是 `Array`（本段形如
十进制数字或 `-` 则建 Array，否则建 Object；已存在的对象成员不覆盖，仅在缺失时补 `null` 占位）。
末段落在数组上时，段为 `-`（RFC 6901 追加记号）或数字等于当前长度均按**追加**处理；数字大于当前
长度不补齐空位，报 `json-type-mismatch`。**`-` 与「等于长度的索引」都只能出现在末段**，出现在中间
段一律报 `json-type-mismatch`（不存在「追加后再往下走」的目标）。

**与 §9.4 的引用失效规则叠加**：`resolve_for_write` 补齐容器会触发 `push_back` / `set`，故返回的
指针在**后续任何写操作**后即失效——路径化写入必须「取指针 → 立即写」成对使用，不得跨写持有。

覆盖用例见 `tests/unit/utest_json.cpp` 的 Pointer 段落：成员寻址、未命中返空、写路径补齐缺失层级、
非法写路径拒绝、路径化删除。

### 9.12 RFC 8259 合规验收

正确性由**仓库之外**的权威语料背书，而不是自产断言。语料快照位于
`tests/fixtures/json_test_suite/`（来源、commit 锚点与更新流程见该目录 `README.md`），唯一消费方是
`tests/unit/utest_json_conformance.cpp`。

| 语料 | 条数 | 断言口径 |
|:---|:---|:---|
| `test_parsing/` 下 `y_` | 95 | **硬断言**：必须全部接受，任一条被拒即失败 |
| `test_parsing/` 下 `n_` | 188 | **硬断言**：必须全部拒绝，任一条被收即失败 |
| `test_parsing/` 下 `i_` | 35 | **行为快照**：接受或拒绝均合规范，但必须与测试内登记的处置表一致 |
| `test_transform/` | 22 | 可解析者做 `parse → dump → parse` 值相等 + `dump` 幂等三重断言 |

`i_` 与 `test_transform` 各有一张**处置表**逐文件名登记期望值，且与语料清单双向比对：语料增删或
行为漂移都必须显式改表，不会静默通过。改表的唯一正当理由是「有意的实现策略变更」——实现缺陷一律
改实现。表的作用是把行为变更变成一次可评审的动作，而不是让测试自动跟随实现。

当前处置（随语料的 commit 锚点固定）：

- `i_` 中 11 条接受、24 条拒绝。接受的是超大指数与超域整数——走保真数字域（§9.6）保留原字面量，
  不做数值转换；拒绝的是无效 UTF-8 序列、孤立代理项、非 UTF-8 编码文本与 UTF-8 BOM。
  **BOM 的拒绝是有意选择**：RFC 8259 §8.1 允许实现忽略 BOM，也允许视其为错误；本模块选择报错，
  理由是「静默吞掉前导字节」会让下游把编码问题误判为文档问题。
- `test_transform/` 中 16 条可解析且往返自洽，其余 6 条是含无效码点的字符串语料，属正确拒绝。

语料根默认取仓库内 `tests/fixtures/json_test_suite`，可经环境变量 `AURORA_JSON_TEST_SUITE_DIR`
覆盖以指向另一份 checkout；语料目录缺失时用例**失败而非跳过**——该资产随仓库分发，缺失即环境错误。
