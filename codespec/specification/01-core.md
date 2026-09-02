# 核心基础层（core）

> 覆盖 `include/aurora/core/`（30 个头文件）与根级 `todo.h`、`commands.h`。
> 本文件是错误、诊断、日志、异步底座与基础几何类型的**唯一权威**；错误码全量清单见 [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)（生成物），编码层面的错误写法规则见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md) §1。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 几何与尺寸意图 | `types.h`、`dimension.h`、`transform.h`、`math.h` |
| 颜色 | `color.h` |
| 错误与结果 | `result.h`、`error_codes.h`、`error_codes.gen.h`、`expected.h` |
| 诊断与降级 | `diagnostics.h`、`strict_mode.h`、`assert.h` |
| 日志 | `log.h`、`debug.h` |
| 异步底座 | `thread_pool.h`、`thread.h` |
| 时间与周期 | `time.h`、`duration.h`、`event_stream.h`、`file_watcher.h` |
| 文本与编码 | `utf8.h`、`string_util.h` |
| 平台与能力查询 | `platform.h`、`enums.h`、`accessibility.h` |
| 其他 | `immutable.h`、`image.h`、`font.h`、`literals.h`、`version.h` |

根级头文件：`aurora.h`（唯一入口）、`aurora_fwd.h`（仅前向声明，供只需指针/引用的编译单元降低包含成本）、`aurora_pch.h`、`commands.h`、`test_helpers.h`、`todo.h`。

---

## 2 几何与尺寸意图

### 2.1 基础结构

定义于 `include/aurora/core/types.h`。

| 结构 | 字段 / 方法 |
|:---|:---|
| `Point` | `x`、`y`（`types.h:14`） |
| `Size` | `width`、`height`；`Size::infinity()` 表示不限制（`types.h:29`） |
| `Rect` | `origin: Point`、`size: Size`；`right() = x + w`，`bottom() = y + h`（`types.h:58`） |
| `EdgeInsets` | `left` / `top` / `right` / `bottom`；`horizontal()` = 左右之和，`vertical()` = 上下之和（`types.h:87`） |

### 2.2 Length：尺寸意图

`Length` 表达「控件想要的尺寸」，由 AI 直接写在 `width` / `height` 属性上（`types.h:108`）。

```cpp
struct Length {
    LengthKind kind = LengthKind::WrapContent;
    float      value = 0.0f;   // Fixed：像素；Fraction：比例（0~1）
};
```

`LengthKind`（`types.h:98`）是**四个无参枚举值**，尺寸数值存放在 `Length::value` 中：

| 枚举值 | 语义 | 约束求解 |
|:---|:---|:---|
| `WrapContent` | 按内容决定 | `max` = 父剩余空间 |
| `Expand` | 填满父级可用空间 | `min = max = parentSize` |
| `Fixed` | 精准固定尺寸 | `min = max = value` |
| `Fraction` | 占父级比例 | `min = max = parent × value`，`value ∈ [0,1]` |

静态工厂：`Length::wrap()` / `expand()` / `fixed(px)` / `ratio(f)`。`fixed()` 与 `ratio()` 带 `AURORA_ASSERT` 边界校验。

### 2.3 强类型尺寸工厂

定义于 `include/aurora/core/dimension.h`，全部工厂位于 `namespace aurora` 内（`dimension.h:5`）；`au` 是 `aurora` 的推荐别名，因此 `au::px(...)` 等写法直接可用。

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
    auto constrain(const Size& s) const noexcept -> Size;  // 逐轴 clamp 到 [min, max]
};
```

`operator==` 逐字段比较，用作布局缓存键。`min ≤ max` 逐轴成立是布局求解的前提不变量。

---

## 3 错误与结果

### 3.1 Error

`Error`（`core/result.h:25`）是结构化错误值，字段如下：

`code`（冻结 slug）、`message`、`suggestion`、`docs`、`where`、`hint`、`code_enum`、`severity`、`category`、`auto_fixable`、`retryable`、`fix_category`、`fix_params`。

错误码的**单一声明源**是 [`errors.toml`](../errors.toml)，由 `tools/gen/gen_error_codes.cpp` 生成 `error_codes.gen.h`、[`ERROR_CATALOG.md`](../ERROR_CATALOG.md) 与 `aurora_api.json`。新增或修改错误码只改 `errors.toml` 后重跑生成器，不手改产物。

### 3.2 Result\<T\>

`Result<T>`（`core/result.h:96`）是值语义的成功/失败二选一，构造 `Result(T value)` 与 `Result(Error err)` 均为隐式。

| 成员 | 签名 |
|:---|:---|
| `ok()` | `[[nodiscard]] auto ok() const -> bool`（`result.h:101`） |
| `operator bool` | `explicit operator bool() const`（无 `[[nodiscard]]`，`result.h:104`） |
| `error()` | `[[nodiscard]] auto error() const -> const Error&`（`result.h:108`） |
| `value()` | `const T&` 与 `T&` 两个重载（`result.h:106-107`） |
| `unwrap()` | `[[nodiscard]] auto unwrap() const -> T`（`result.h:111`） |

`Result<void>` 为特化（`result.h:135`），提供 `ok()` / `error()` / `operator bool`，**不提供** `value()` 与 `unwrap()`。

`core/expected.h` 另有库自带的极简 `expected<T, E>` / `unexpected<E>`（C++23 `std::expected` 落地前的替身实现）：二态（持值或持错误），错误态经 `expected<T, E>{unexpected{err}}` 构造，提供 `operator bool` / `has_value()` / `value()` / `error()` / `value_or(def)`。公共 API 一律返回 `Result<T>`，`expected` 仅作其底层接口底座，新代码不应直接暴露它。

**常见误写**：`Result` **没有** `is_ok()` 成员。判成功一律用 `ok()` 或 `if (r)`。

### 3.3 使用约定

失败是值，不是异常：涉及外部输入（反序列化、文件、构造工厂）的函数返回 `Result<T>`，调用方必须先判 `ok()` 再取 `value()`。

```cpp
auto restored = au::serialization::from_json(json);
if (!restored.ok()) {
    au::Diagnostics::report(restored.error().message, "main.cpp:42", restored.error().code);
    return;
}
auto widget = restored.value();
```

---

## 4 诊断与降级

### 4.1 Diagnostic

`Diagnostic`（`core/diagnostics.h:41`）是库在「输入非法 / 部分代码缺失」时产出的结构化记录，而非崩溃或白屏。

| 字段 | 说明 |
|:---|:---|
| `severity` | 与 `Error::severity` 对齐的 `ErrorSeverity`（默认 `Warning`） |
| `category` | `ErrorCategory`（默认 `General`） |
| `message` | 人类可读描述 |
| `where` | 位置（`file:line` 或控件类型） |
| `code` | 机器可读 slug（可为空） |
| `code_enum` | 与 `code` 对应的 `ErrorCode`（无码时 `GeneralUnknown`） |
| `fix` | 可选结构化修复建议 `FixSuggestion` |

`FixSuggestion`（`diagnostics.h:21`）含 `code`、`description` 与可选 `auto_fix` 回调，`has_auto_fix()` 判断是否可自动修复。

`Diagnostic::to_json_line()` 输出 JSON 行，供工具链消费。`severity_str()` / `category_str()` 供遗留代码按字符串比较，新代码应直接用枚举比较（两个 `using` 别名已标记 `[[deprecated]]`）。

### 4.2 Diagnostics 收集器

`Diagnostics`（`core/diagnostics.h:68`）是全局诊断收集器，单线程 UI 无需加锁。

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
| `apply_fix(code)` | 按 `code` 执行一次自动修复，命中返回 `true`，两个重载（slug 字符串 / `ErrorCode`，`diagnostics.h:106,118`） |
| `register_fix(code, fix)` | 注册错误码 → 修复策略映射 |
| `auto_fix_all()` | 应用全部已注册的自动修复，返回成功修复数 |

### 4.3 严格模式

`StrictMode`（`core/strict_mode.h:22`）取值 `Off`（默认）与 `On`。`On` 时 `Diagnostics::degraded` 升级为硬失败，用于 CI 把「降级渲染 / 深度超限」这类本应容忍的问题变为构建阻断。

- 推荐入口是 `Application` 上下文：`au::App().strict_mode(au::StrictMode::On).run(...)`；`Application` 另提供 `set_strict_mode()` / `strict_mode()`（`app/application.h:132,423`）。
- 无 App 上下文的场景（单元测试）用线程级全局开关 `aurora::set_strict_mode(m)` / `aurora::strict_mode()`；该状态是 `thread_local`，避免多线程竞争。
- 硬失败经 `on_strict_failure(message)` 执行：先 `AURORA_LOG_FATAL`，再调用注入的 handler（若有），最后 `std::terminate()`。它**不依赖** `AURORA_ASSERT`，因此 Release / `NDEBUG` 构建同样被阻断。
- 测试可注入 handler 拦截硬失败：`aurora::set_strict_failure_handler(h)`，传空恢复默认。

### 4.4 校验入口

`au::validate(root, max_depth = 64) -> Result<bool>`（`app/validate.h:35`）把空子节点、深度超限、未知类型报告为结构化 `Error`。子节点合法性是**运行时校验**：容器统一接受任意 `Node`，无编译期白名单。

---

## 5 日志

### 5.1 级别与格式

`LogLevel`（`core/log.h:22`）取值 `Trace` / `Debug` / `Info` / `Warn` / `Error` / `Fatal`；短标签由 `log_level_label()` 映射为 `TRC` / `DBG` / `INF` / `WRN` / `ERR` / `FTL`。

统一行格式：

```text
[YYYY-MM-DD HH:MM:SS][LEVEL][module@threadId filename:line] > content
```

### 5.2 Logger

`Logger`（`core/log.h:91`）是单例，默认级别 `Info`，默认输出到 stderr，内部状态无锁（单线程 UI 假设）。

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

定义于 `core/log.h:171-193`。

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

`ThreadPool`（`core/thread_pool.h:29`）是有界线程池，析构时 stop + join，无悬挂线程。

| 成员 | 说明 |
|:---|:---|
| `ThreadPool(worker_count = default_worker_count())` | 构造并启动 worker；传 0 回落默认值 |
| `default_worker_count()` | `hardware_concurrency()`，下限 2 |
| `worker_count()` | 当前 worker 线程数 |
| `pending_count()` | 当前排队未执行任务数（近似值，仅供诊断） |
| `execute(std::function<void()>)` | fire-and-forget，异常在 worker 内被捕获，不向外传播 |
| `submit(F&&) -> std::future<R>` | 提交并返回 future，异常经 future 传播 |
| `default_pool()` | 进程级默认池（Meyers 单例），`au::async` 与协程均经它调度 |

拷贝与移动均被删除。

### 6.2 使用约定

禁止裸 `std::thread` 执行后台工作，也禁止在 `on_click` 等 UI 回调中直接阻塞。所有耗时操作经 `au::async` / `au::co_async` 提交到默认池（契约见 [`02-state.md`](02-state.md) §5）。

`core/thread.h` 提供单线程 UI 契约的配套守卫：`MainThreadOnly<T>`（`thread.h:20`）包装一个值，debug 下断言读写均发生在构造它的线程，模板参数 `Check = false` 时为零开销特化（不存 owner 线程、不断言）；宏 `AURORA_MAIN_THREAD` 为函数标注「必须在主线程调用」（clang `annotate` 属性，供静态分析 / 文档工具识别；GCC 下为 no-op，运行期契约仍由 `MainThreadOnly` 兜底）。

---

## 7 其他基础能力

| 头文件 | 能力 |
|:---|:---|
| `color.h` | `Color` 结构（`color.h:10`）与具名颜色（`aurora::colors` 子命名空间） |
| `time.h` / `duration.h` | 时间表示与时长 |
| `event_stream.h` | 事件流 |
| `file_watcher.h` | 文件监听 |
| `utf8.h` / `string_util.h` | UTF-8 处理与字符串工具 |
| `platform.h` | 平台能力查询（运行时查询，不靠 `#ifdef`） |
| `enums.h` | 跨模块共享枚举 |
| `immutable.h` | 不可变包装 |
| `image.h` / `font.h` | 图像与字体的基础类型（具体能力见 [`03-layout-render.md`](03-layout-render.md)） |
| `version.h` | 库版本 |
| `accessibility.h` | 无障碍基础 |
| `todo.h`（根级） | `au::TODO` 占位回调 |

### 7.1 au::TODO

`au::TODO`（根级 `todo.h:23`）是占位回调，用于标记尚未实现的事件处理——编译通过，运行时触发时经 `Diagnostics::warn("TODO", what)` 输出警告。它是占位回调而非错误码。

```cpp
auto btn = au::Button(au::ButtonProps{ .label = "OK" });
btn.set_on_click(au::TODO("handle_click"));   // 编译通过，运行时留可读警告
```

---

## 8 需求规格

### 8.1 #18 安全的内存与所有权模型

**核心目标：** AI 生成无悬空指针、无泄漏的代码。

**需求陈述：**

- UI 树通过 `std::shared_ptr<Widget>` 管理所有权，父子关系即「树内拥有」：拷贝即共享、移动即转移，整棵树随根 `Node` 析构而析构。AI 不需要手动 `delete`，也不必担心 use-after-free。
- 禁止裸指针出现在公开 API 中。事件回调需要引用「触发者」时，用稳定标识 `Node::id()` 加查询，而非引用或指针。
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
2. 降级渲染有统一视觉语言：边框 + 灰色背景 + 说明文字。可直接使用 `au::Placeholder`（`widget/placeholder.h:24`）作为通用降级占位盒，AI 通过快照即可识别「这个位置降级了」。
3. 所有降级产生结构化警告（§4.1），典型如：

```text
[render-degraded] Image.source:
  - Received: "" (empty string)
  - Fallback: placeholder rendered (200x150, dashed border)
  - Fix: provide a valid file path or URL
  - Location: main.cpp:42
```

4. 严格模式用于生产与 CI（§4.3）下，降级即致命失败。默认宽松语义由各组件内置降级：非法或缺失属性 → 渲染占位框 + 结构化警告。

**验收标准：** 对任意非法输入构造的树，`HeadlessSurface` 渲染不崩溃、产出占位像素与结构化警告；开启严格模式后同一输入返回致命失败。

### 8.4 #23 部分代码容错（半成品可编译可运行）

**核心目标：** AI 可增量开发。

**需求陈述：** 任何组件在任何「半成品」状态下都不应崩溃，而是优雅降级。这对 AI 的增量开发循环至关重要：先生成骨架 → 编译通过 → 逐步填充 → 每步都可运行。

- 编译期：未完成的组件不应导致整个项目编译失败；以 `au::TODO`（§7.1）占位标记尚未实现的事件处理。
- 运行时：缺少必要属性的组件渲染为占位框而非崩溃（降级视觉语言见 #21）。

**验收标准：** 只填了 label 的控件、带 `au::TODO` 回调的界面，均可编译、可渲染、可运行，并留下可读警告指明未完成处。
