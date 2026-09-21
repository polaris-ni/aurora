# 响应式状态与异步（state）

> 覆盖 `include/aurora/state/`（14 个头文件）。本文件是信号原语、订阅生命周期、`Store` 与异步编程序的**唯一权威**。
> 线程模型与所有权不变量见 [`ARCHITECTURE.md`](../ARCHITECTURE.md) §组件树；错误与结果类型见 [`01-core.md`](01-core.md) §3。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 状态源与属性容器 | `state.h`、`reactive.h`、`binding.h` |
| 作用域权限包装 | `immutable.h`（`Immutable<T>` 只读 / `Mutable<T>` 读写，类型层面约束状态作用域） |
| 派生与副作用 | `computed.h`、`effect.h` |
| 信号视图与观察图 | `signal_view.h` |
| 订阅句柄 | `subscription.h` |
| 类 Redux 单向流 | `store.h` |
| 异步任务 | `async.h`、`coroutine.h` |
| 依赖图与撤销 | `state_graph.h`、`undo_stack.h`、`state_registry.h` |

---

## 2 信号原语

### 2.1 类型总览

| 类型 | 角色 | 头文件 |
|:---|:---|:---|
| `SignalView<T>` | 只读信号视图，定义 `get()`；在活跃 `Effect` 作用域内读取时自动登记依赖 | `signal_view.h` |
| `State<T>` | 可变状态源。`get()` 登记依赖，`set()` 通知依赖者重跑 | `state.h` |
| `Reactive<T>` | 属性容器，可持有常量或信号引用；所有可被信号驱动的属性都是 `Reactive<T>` | `reactive.h` |
| `Binding<T>` | `State<T>` 的双向绑定包装（读写都同步回源） | `binding.h` |
| `Computed<T>` | 派生信号，只读、可缓存 | `computed.h` |
| `Effect` | 响应式副作用作用域 | `effect.h` |
| `Immutable<T>` | 只读权限包装：包装 `State<T>&` 但删除写入路径，在类型层面强制「某作用域只读该状态」 | `immutable.h` |
| `Mutable<T>` | 读写权限包装：在裸 `State<T>` 之上附加显式 scope 标签，用于把「谁在读 / 谁在写」汇入 `StateGraph` | `immutable.h` |

`SignalViewBase`（`signal_view.h`）是非模板基类，提供 `subscribe(Effect&)` 与 `read()` 两个纯虚方法，供 `Effect` 做依赖追踪。

### 2.2 State 与 Reactive

`State<T>` 继承 `SignalView<T>`、`StateBase` 与 `std::enable_shared_from_this<State<T>>`（`state.h`）。

- `get()` 在 `Effect` 或渲染作用域内自动登记依赖。
- `set()` 通知依赖者重跑；固定在 UI 线程派发，无需加锁。
- 属性（如 `Text.content`）的类型是 `Reactive<T>`：既可持有常量（`.content = "Hi"`）也可持有信号。从 `State<T>` 构造为**显式**构造 `Reactive(std::shared_ptr<State<T>>)`，须经 `state()` / `shared()` 包装后传入；两种来源对组件透明。

### 2.3 Computed

派生信号，依赖变化时急切重算，`get()` 返回最近一次结果。两种等价构造：

```cpp
auto is_even = au::computed([&] { return count.get() % 2 == 0; });  // T 由返回类型推导
auto label   = au::Computed<au::LocalizedString>{ [&] {             // T 显式指定
    return au::LocalizedString{ std::to_string(count.get()) };
} };
```

工厂 `computed(F&&)` 定义于 `computed.h`。

### 2.4 Effect

`Effect`（`effect.h`）是响应式副作用作用域：**构造函数不执行 `fn`**，创建后须显式调用 `run()` 首次运行，运行期间 `get()` 到的信号登记为依赖；之后任一依赖 `set()` 时自动重跑。`Computed` 与 `connect` 内部创建 `Effect` 后会自行补一次 `run()`（`computed.h`、`subscription.h`），控件 `collect_signals` 驱动的刷新路径亦由框架内部发起——只有裸 `Effect` 需要手动 `run()`，漏调则回调一次都不会执行。

```cpp
au::State<int> count{0};
au::Effect watch{ [&]{ au::Diagnostics::warn("count = " + std::to_string(count.get())); } };
watch.run();    // 首次显式运行，同时登记对 count 的依赖
count.set(1);   // → watch 重跑
```

### 2.5 设计要点

树只构建一次：组件在绘制期读取信号时登记依赖，信号变化定点刷新依赖它的局部计算。**无虚拟 DOM、无 key、无 diff**。组件经 `Widget::collect_signals` 把属性里的信号登记到所属信号作用域，信号变化时仅重绘该组件。

---

## 3 订阅与生命周期安全

### 3.1 Subscription

`Subscription`（`subscription.h`）是 RAII 订阅句柄：包装 `State` / `Reactive` / `Computed` / `Store` 订阅返回的取消句柄，析构自动取消，杜绝监听器泄漏。AI 生成代码无需手动保存或调用取消句柄——把返回值留在作用域即可。

### 3.2 connect

`connect` 有两个重载，首值行为**不同**：

- `connect(src, fn)`（信号重载）：每次变化调用 `fn(最新值)`，**首次立即应用当前值**（内部 `Effect` 构造后显式 `run()`）。`src` 可为 `State<T>` / `Reactive<T>` / `Computed<T>`（均继承 `SignalView<T>`）。
- `connect(store, fn)`（`Store<S>` 重载）：仅转接 `Store::subscribe`，**不应用初始值**——只有下一次 `dispatch` 产生新状态才调用 `fn`。需要同步初值时自行取 `store.get_state()`（或改用 `connect(*store.as_signal(), fn)` 走信号重载）。

两者均返回 `Subscription`。

```cpp
au::State<int> count{0};
int seen = 0;
{
    au::Subscription sub = au::connect(count, [&](int v){ seen = v; });  // 立即 seen = 0
    count.set(5);                                                        // → seen = 5
}                                                                        // sub 析构 → 自动取消
```

**命名契约（v1.0.0-alpha.1）**：本函数原名 `bind`，已改名 `connect`——`State` 基类链上的 `std::enable_shared_from_this` 使 `std` 进入实参的 ADL 关联命名空间集，无限定 `bind(state, fn)` 会被变参转发的 `std::bind` 吸走（编译通过但回调永不执行的静默错误）。`connect` 与任何 `std` 名字无重叠，从根上排除该歧义；`Animator::bind` 为成员函数，不经 ADL，保持原名。

### 3.3 观察图的失效安全

响应式内核以 `Connection`（`signal_view.h`）维护「信号源 ↔ Effect」观察图：双方均以 `weak_ptr` 引用彼此的锚点 `ReactiveAnchor`（`signal_view.h`）。

`State::notify()` 在遍历时探测并**惰性摘除失效边**，因此任一侧先析构都不会再解引用失效对象。`Subscription` / `Effect` / `Computed` 与控件 `track` 内部的 `Effect` 销毁后，信号源继续 `set()` 是安全的——不会重跑已死的观察者，也不会崩溃。

两项确定性行为约定（v1 实现，契约如下）：

- **观察边叠加式（不剪枝）**：`Effect` 每轮 `run()` 清空的是自身 `deps_` 记录，信号源侧的观察边不随之撤销——某轮不再读取的信号，其后 `set()` 仍会触发重跑（重跑幂等，值正确，仅存在冗余触发）。边只在 `Effect` 析构/dispose 时经失效探测摘除。
- **`set()` 无等值短路**：写入与旧值相等的值同样全量通知依赖者重跑；如需抑制请自行比较后再写。

---

## 4 Store：可选的类 Redux 单向流

`Store`（`store.h`）内部持有 `State<S>`，用 `dispatch(Action)` 加纯函数 `Reducer` 产生新状态，并暴露 `as_signal()` 供组件像订阅普通信号一样订阅。

| 成员 | 签名 / 说明 |
|:---|:---|
| `Action` | `{ std::string type; std::shared_ptr<void> payload; }`；构造 `Action(std::string)` 或 `Action(std::string, T value)`（`store.h`） |
| `Action::payload_as<T>()` | 取回载荷，为空返回 `nullptr`；类型由调用方保证匹配（本函数不做类型校验）（`store.h`） |
| `dispatch(const Action&)` | 派发动作（`store.h`） |
| `subscribe(Listener)` | 订阅，返回取消函数（`store.h`） |
| `as_signal()` | `-> std::shared_ptr<State<S>>`（`store.h`） |
| `make_store(initial, reducer)` | `-> std::shared_ptr<Store<S>>`（`store.h`） |

```cpp
struct AppState { int counter = 0; };
auto store = au::make_store(AppState{},
    [](const AppState& s, const au::Action& a) -> AppState {
        if (a.type == "increment") return { s.counter + 1 };
        return s;
    });
au::Button(au::ButtonProps{ .label = "+1" })
    .set_on_click([&]{ store->dispatch(au::Action{"increment"}); });
```

`State<T>` 与 `Store<S>` 可并存：高频局部状态用 `State<T>`，全局应用状态用 `Store<S>`。

---

## 5 异步与协程

### 5.1 回调式 Task

`Task<T>`（`async.h`）由 `au::async(fn)` 创建（`async.h`）：在后台线程池执行 `fn`（`fn` 返回 `T` 或 `Result<T>`，抛出的异常捕获为 `runtime-async-exception` 错误）。

| 成员 | 签名 / 说明 |
|:---|:---|
| `then(DoneFn cb)` | 注册完成回调，`DoneFn = std::function<void(const Result<T>&)>`，返回 `Task&` 以便链式（`async.h`） |
| `with_timeout(d)` | 超过 `d` 未完成则向 `then` 回调投递 `async-timeout` 错误（`async.h`） |
| `cancel()` | 取消结果投递（`async.h`） |
| `set_main_poster(poster)` | 静态，安装跨线程投递器（`async.h`） |

**`then` 的回调默认在承载 `fn` 的线程池 worker 线程上同步执行**（未安装主线程投递器时直接调用 `fn`，供 headless / 测试）；安装 `Task::set_main_poster`（或 `au::async` 的主线程投递器）后，回调才经投递器回到 UI（主）线程执行，此时方可直接更新 UI。安装者与帧循环唤醒的联动契约见 [`06-app-platform.md`](06-app-platform.md)。

```cpp
auto task = au::async([] { return fetch_from_network(); });
task.with_timeout(std::chrono::seconds(5))
    .then([](const au::Result<Data> &r) {          // 安装主线程投递器后在 UI 线程（否则在 worker 线程）
        if (r) store->dispatch(au::Action{"loaded", r.value()});
        else   au::Diagnostics::report(r.error().message, {}, r.error().code);
    });
```

**限制**：`with_timeout` 与 `cancel()` 都**无法中断任意 `fn`**，只丢弃或改道结果。

### 5.2 协程路径

| 符号 | 说明 | 位置 |
|:---|:---|:---|
| `co_async(fn)` | 返回 awaitable，`co_await` 在后台线程池执行 `fn`，续体经主线程投递器回到主线程 | `coroutine.h` |
| `CoroTask<T>` | 协程返回类型 | `coroutine.h` |
| `launch(task)` | 启动顶层协程 | `coroutine.h` |

```cpp
au::CoroTask<void> load() {
    au::Result<Data> r = co_await au::co_async([] { return fetch_from_network(); });
    if (r) store->dispatch(au::Action{"loaded", r.value()});
}
au::launch(load());
```

### 5.3 使用约定

- 禁止裸 `std::thread`；后台底座是有界线程池 `ThreadPool::default_pool()`（[`01-core.md`](01-core.md) §6）。
- 禁止在 `on_click` 等 UI 回调中直接阻塞。
- 异步错误通过 `Result<T>` 传递，回调必须处理成功与失败两个分支。
- 定时与周期任务**不属于**本模块，由 `Scheduler` / `Timer` 承担，见 [`06-app-platform.md`](06-app-platform.md)。

---

## 6 依赖图与撤销栈

| 类型 | 说明 | 位置 |
|:---|:---|:---|
| `StateGraph` | 状态依赖图，含 `Node` 与 `Edge` 结构；`to_json()` 可导出依赖图结构，便于 AI 做快照测试 | `state_graph.h` |
| `UndoStack` | 撤销栈，命令结构为 `UndoCommand` | `undo_stack.h` |
| `state_registry.h` | `StateRegEntry` / `EffectRegEntry`，运行时注册表 | `state_registry.h` |

`State<T>` 的**值本身不直接序列化**；要导出结构用 `StateGraph::to_json()`。

---

## 7 需求规格

### 7.1 #6 单向数据流与细粒度信号状态模型

**核心目标：** AI 易理解状态。

**需求陈述：** UI 是一棵静态构建的组件树，状态保存在 `State<T>` 中；组件在渲染期读取信号时自动登记依赖；信号变化只重跑依赖它的局部计算，不重建整棵树、不做 key/diff。

```cpp
// Reactive<T> 只持有 State<T>（reactive.h），Computed<T> 不能直接流入属性；
// 派生显示文本走「共享 State<LocalizedString> + 事件回调写入」的惯用形态（同 demo_button）。
auto count = std::make_shared<au::State<int>>(0);
auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "Count: 0" });
au::Column(au::ColumnProps{
    .children = {
        au::Text{ au::TextProps{ .content = au::Reactive{ label } } },  // 属性与 label 共享同一信号源
        std::move(au::Button{ au::ButtonProps{ .label = "+1" } }
                      .set_on_click([count, label] {
                          const int next = count->get() + 1;
                          count->set(next);
                          label->set(au::LocalizedString{ "Count: " + std::to_string(next) });
                      })),
    },
});
```

**关键原则：**

- **State → View 是纯函数映射**：组件的绘制是信号当前值的纯函数。
- **事件 → State 通过普通赋值**：`count.set(...)`，无需 Action / Reducer 样板。
- **细粒度、定点刷新**：只有读取过该信号的组件与计算会被重跑（§2.5）。
- **属性即信号**：`Reactive<T>` 既可接受常量也可接受信号，对组件透明。
- **状态可快照**：`StateGraph::to_json()` 导出依赖图结构；`State<T>` 值本身不直接序列化。

**约束：**

- 默认状态方式是细粒度 `State<T>`，AI 只需学这一条即可覆盖绝大多数场景。
- `State<T>` 变化固定在 UI 线程派发，无需锁。
- 禁止在渲染（构建）期产生副作用（写信号）。

**验收标准：** 一次 `set()` 只重绘读取过该信号的控件；未读取该信号的控件不产生任何重绘或重算；`StateGraph::to_json()` 能导出完整依赖图。

### 7.2 #19 结构化异步与并发模型（异步侧契约）

**核心目标：** AI 轻松处理耗时操作。

**需求陈述：** UI 线程唯一，所有 UI 更新必须在 UI 线程；耗时操作必须经 Aurora 提供的异步原语执行，禁止裸 `std::thread`，也禁止在 UI 回调中直接阻塞。

**API 与不变量：** 见 §5（`au::async` / `Task<T>` / `then` / `with_timeout` / `co_async` / `CoroTask<T>` / `launch`）。

**验收标准：** 全部后台工作都可在无 GUI 环境（headless）下完成并回到主线程；`ThreadPool` 存活期间不出现新建的游离 `std::thread`；超时与取消路径只改变结果的投递去向，绝不中断用户函数。

> 内存与所有权侧的所有权模型见 [`01-core.md`](01-core.md) §8.1（#18）；跨线程回投与帧循环唤醒见 [`06-app-platform.md`](06-app-platform.md)。
