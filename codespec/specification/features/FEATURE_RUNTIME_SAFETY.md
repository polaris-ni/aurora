# C. 运行时安全层（#18,#19,#21,#23）

> 本文件是「三、特性详细规范」按功能域/子系统划分出的子文档；返回主线索引见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。
> 相关核心子系统实现（H 系列）见 [`../subsystems/`](../subsystems)（H.1–H.10c 信号/动画/环境/事件/渲染/窗口/平台）与 [`../subsystems_api/`](../subsystems_api)（H.11–H.17 + Log + AI-First 序列化/布局/控件/Inspector/工具/日志）。

#### #18 安全的内存与所有权模型

**核心目标：** AI 生成无悬空指针/泄漏的代码

**规范：**

```cpp
// 规则 1：组件由 Node 持有，Node 内部是 shared_ptr<Widget>，拷贝即共享
au::Node btn1 = au::Button(au::ButtonProps{ .label = "OK" });
au::Node btn2 = btn1;            // 共享同一实现，复制成本低，AI 可安全复制组件

// 规则 2：父子关系 = 树内拥有，移动语义转移节点
auto parent = au::Column{};
parent.children.push_back(std::move(child));  // child 节点移入 parent
// 整棵树随根 Node 析构而析构，AI 无需手动管理生命周期

// 规则 3：跨组件引用 = 弱引用 ID，非裸指针
struct AppState {
    au::WidgetId focused_field;  // 不是 Widget*，是稳定 ID
};
auto widget = tree.find(state.focused_field);  // optional<Widget*>

// 规则 4：禁止裸指针出现在公开 API 中
// ❌ void on_click(Button* sender, Event* e);
// ✅ void on_click(au::WidgetId sender, const au::Event& e);
```

**关键约束：**

- UI 树通过 `std::shared_ptr<Widget>` 管理所有权，明确父子关系
- 禁止裸指针传递，组件树自动管理生命周期
- AI 不需要手动 `delete` 或担心 use-after-free
- 事件回调中需要引用"触发者" → 用 `WidgetId` + 查询，而非引用/指针

**单线程 UI 说明：**

> Aurora 是 **单线程 UI**：所有 UI 构建、状态变更、事件派发、重绘都在 UI 线程进行，无锁。
> - `State<T>` 的读/写、`Node` 的复制/移动都只在 UI 线程发生，天然无数据竞争
> - 耗时操作通过 `au::async`（见 #19）在后台线程执行，结果经主线程投递器回到 UI 线程
> - `shared_ptr` 仅用于简化树的生命周期管理，并非为多线程共享；快照等只读场景可安全跨线程共享
>


---

#### #19 结构化异步与并发模型

**核心目标：** AI 轻松处理耗时操作

**规范：**

```cpp
// 规则 1：UI 线程是唯一的，所有 UI 更新必须在 UI 线程
// 规则 2：耗时操作通过 Aurora 提供的异步原语执行，禁止裸 std::thread

// 模式 A：回调式 Task + 链式 then（后台执行，结果回到主线程）
auto task = au::async([] { return fetch_from_network(); });  // 返回 au::Task<T>
task.with_timeout(std::chrono::seconds(5))                  // opt-in 超时
    .then([](const au::Result<Data> &r) {                   // 回调在 UI 线程执行
        if (r) store->set(r.value());
        else   Diagnostics::error(r.error().message);       // 含 async-timeout
    });

// 模式 B：协程式 co_await（续体回到主线程）
au::CoroTask<void> load() {
    au::Result<Data> r = co_await au::co_async([] { return fetch_from_network(); });
    if (r) store->set(r.value());
}
au::launch(load());
```

**关键约束：**

- **后台执行底座是有界线程池 `aurora::ThreadPool`**（`ThreadPool::default_pool()`，worker 数 = `hardware_concurrency()`，下限
  2）；`au::async` / `co_async` 均向该池提交任务， **不再为每次调用 `std::thread().detach()`**，杜绝线程爆炸与悬挂线程（池析构
  stop+join）。
- `au::async(fn)` 返回的 `au::Task<T>` 在后台线程执行 `fn`（`fn` 返回 `T` 或 `Result<T>`，异常捕获为 `async-exception`错误）。
- `Task::then` 的回调 **保证在 UI（主）线程**执行，可直接更新 UI；跨线程投递由 `Task::set_main_poster` 安装（无 poster
  时直接调用，供 headless / 测试）。
- 异步错误通过 `au::Result<T>` 传递，回调必须处理 `if (r) ... else ...`；超时经 `async-timeout` 错误回传。
- `Task::with_timeout(d)`：超过 `d` 任务仍未完成则向 `then` 回调投递 `make_error(ErrorCode::RuntimeAsyncTimeout, ...)`
  （slug 为 `"async-timeout"`）；与 `cancel()` 同限制—— **无法中断任意 `fn`**，仅丢弃 / 改道结果。
- 协程路径：`au::co_async(fn)` 返回 awaitable，`co_await` 在后台线程池执行 `fn`、续体经主线程投递器回到主线程；
  `au::CoroTask<T>` 为协程返回类型，`au::launch` 启动顶层协程。
- 禁止在 `on_click` 等 UI 回调中直接阻塞：

```cpp
// ❌ on_click([]{ auto data = blocking_fetch(); });  // 阻塞 UI 线程
// ✅ 必须通过 au::async 将耗时工作移到后台
```

---

#### #21 错误恢复与降级渲染

**核心目标：** AI 生成的错误 UI 不会崩溃

**规范：**

```cpp
// 规则 1：任何组件在任何非法状态下都不崩溃
au::ImageView{};                    // 无 source → 渲染为带虚线框的占位符
au::Text{}.font_size(-1);           // → 使用默认 font_size(14) + 警告
au::Column(au::ColumnProps{ .gap = -10 });  // → gap 钳到 0 + 警告

// 规则 2：降级渲染有统一的视觉语言
// 所有降级组件渲染为：边框 + 灰色背景 + 说明文字；可直接使用 au::Placeholder 控件
// （au::Placeholder{ .message = "..." }，见 widget/placeholder.h）作为通用降级占位盒
// AI 通过快照可以识别"这个位置降级了"

// 规则 3：所有降级产生结构化警告（与 #9 配合）
// [Aurora::Degraded] Image.source:
//   - Received: "" (empty string)
//   - Fallback: placeholder rendered (200x150, dashed border)
//   - Fix: provide a valid file path or URL
//   - Location: main.cpp:42

// 规则 4：提供"严格模式"用于生产环境
// 注：au::App().strict_mode() 见 application.h（App/Application 构建器与运行期套用）；
//     严格模式下降级即致命失败（见 strict_mode.h，NDEBUG 下也不依赖被剥离的 assert），
//     默认（宽松）语义仍由各组件内置降级：
//     非法/缺失属性 → 渲染占位框 + 控制台结构化警告（默认即"宽松/降级"语义）。
```

**设计原则：** 当组件属性非法或布局冲突时，Aurora 显示占位符或回退到安全默认，并记录结构化警告，而不是 crash 或白屏。这给 AI
提供迭代修正的机会。

---

#### #23 部分代码容错（半成品可编译可运行）

**核心目标：** AI 可增量开发

**规范：**

```cpp
// 编译期：未完成的组件不应导致整个项目编译失败
auto btn = au::Button(au::ButtonProps{ .label = "OK" });
btn.set_on_click(au::TODO("handle_click"));  // 编译通过，运行时显示占位提示

// 运行时：缺少必要属性的组件应渲染为"占位框"而非崩溃
au::ImageView();  // 无 source → 渲染为带虚线框的占位符，控制台输出警告
```

**`au::TODO` 的定义：**

```cpp
namespace aurora {
    // au::TODO 是一个占位回调，用于标记尚未实现的事件处理
    // 类型：可转换为任何回调签名的占位对象
    struct TODO {
        std::string description;
        explicit TODO(std::string desc) : description(std::move(desc)) {}
    };
    // 运行时行为：触发时输出警告 "[Aurora::TODO] handle_click not implemented"
    // 并在 UI 上显示一个黄色占位提示条（仅 strict_mode=false 时）
}
```

**设计原则：** 任何组件在任何"半成品"状态下都不应崩溃，而是优雅降级。

这对 AI 的增量开发循环至关重要：AI 先生成骨架 → 编译通过 → 逐步填充 → 每步都可运行。

---
