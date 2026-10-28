# 响应式状态与异步（state）

> 覆盖 `include/aurora/state/`（10 个公共头 + 2 个内核细节头）。本文件是状态句柄、派生与订阅、依赖图与异步编程的**唯一权威**。
> 线程模型与所有权不变量见 [`ARCHITECTURE.md`](../ARCHITECTURE.md) §组件树；错误与结果类型见 [`01-core.md`](01-core.md) §3。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 可写状态与只读状态句柄、采集出口 | `state.h`、`read_only_state.h` |
| 派生与副作用工厂 | `computed.h`、`effect.h` |
| 订阅凭证 | `subscription.h` |
| 异步任务 | `async.h`、`coroutine.h` |
| 三态资源绑定与公开快照 | `resource.h`（`Resource<T>` / `StreamResource<T>` / `ResourceSnapshot<T>` / `StreamSnapshot<T>`） |
| 依赖图与撤销 | `state_graph.h`、`undo_stack.h` |
| 内核节点与传播引擎（库内，不属公共契约） | `include/aurora/state/detail/reactor.h`、`include/aurora/state/detail/registry.h` |

旧版的 `state_registry.h` 不再作为公共头存在，注册表实现在 `include/aurora/state/detail/registry.h`；`reactive.h`、`binding.h`、`signal_view.h`、`immutable.h`、`store.h` 已删除，迁移去向见 §2.6 与 §4。

---

## 2 能力选型与信号原语

### 2.1 日常四词

日常面固定为四个词，业务代码只从这里选；其余公共名字按 §2.2 的三档出现，不与这四个词并列成选型项。

```text
State<T>           能改的值        set / update / notify
ReadOnlyState<T>   只读的值        get / subscribe；computed 返回它
computed(fn)       算出来的值      随上游自动重算
effect(fn)         值变时做副作用  调用即首跑，返回 Subscription

State → ReadOnlyState 单向；写权限靠类型，不靠 const
```

### 2.2 三档名字

公共名字按三档管理，文档与示例不得混档：

| 档位 | 内容 | 出现场合 |
| :--- | :--- | :--- |
| 日常面 | `State<T>`、`ReadOnlyState<T>`、`computed(fn)`、`effect(fn)` | 业务代码；GUIDELINE 配方只用这一档 |
| 专项面 | `Resource<T>`、`StreamResource<T>`、`ResourceSnapshot<T>`、`StreamSnapshot<T>`、`ResourcePhase`、`Subscription` | 引入异步、流式或显式取消时 |
| 参考面 | `StateSink`、采集覆写、自定义比较器 | 写自定义控件或调整发布语义时；应用代码不出现 |

新增公共名字须先证明这四个词无法表达，否则归入专项或参考面；不为同一个意图提供第二种写法。日常面出现第五个公共名字而未在三档表登记，视为教学面退化。

### 2.3 纪律五条

1. 句柄按值传、按值捕获；不要保存 `get()` 的引用。
2. 写入不会立刻读到：回调里的 `set/update` 排到下一轮。
3. 订阅与副作用都返回 `Subscription`；想让它一直活着就保留返回值。
4. 值相等不通知；要每次都通知，创建来源时写 `always_publish`。
5. `computed` 的结果可以直接接展示属性，不需要手写中转 `State`。

### 2.4 常见选择

| 我要 | 写 |
| :--- | :--- |
| 一个能改的值 | `au::State<int> n{0};` |
| 一个算出来的值 | `auto m = au::computed([n]{ return n.get() * 2; });` |
| 只读参数或属性 | `void f(au::ReadOnlyState<int> n);` |
| 外部异步来的值 | `au::ReadOnlyState<au::ResourceSnapshot<T>> s = au::Resource<T>::from(producer);` |
| 值变时做副作用 | `auto held = au::effect([n]{ ... });` |

### 2.5 能力与来源（专项面）

| 公共形态 | 权限与职责 |
| :--- | :--- |
| `State<T>` | 拥有来源的读写句柄；读订阅能力加 `set`、`update`、`notify` |
| `ReadOnlyState<T>` | 拥有来源的只读句柄；`get()`、`subscribe(fn)`，没有通用写入口 |
| `computed(fn)` | 创建纯派生来源，返回 `ReadOnlyState<T>`；不再有公共 `Computed<T>` 类 |
| `Resource<T>` | 管理一次性作业，可单向转换为 `ReadOnlyState<ResourceSnapshot<T>>` |
| `StreamResource<T>` | 管理流式生产，可单向转换为 `ReadOnlyState<StreamSnapshot<T>>` |
| `Subscription` | 订阅或副作用的存活凭证：`subscribe` 固定来源，`effect` 自动追踪；析构即取消，不参与容器选型 |

「两词」只指能力类型这一对：`State<T>` 可写，`ReadOnlyState<T>` 只读，二者单向转换。日常面另含 `computed` 与 `effect` 两个工厂；异步、订阅、撤销等概念没有删除，但按 §2.2 的三档出现，不与这对名字并列成选型项。

`ReadOnlyState` 不声称值不变或句柄非拥有——这两点分别与响应式语义和共享所有权冲突。本库不引入 `Property`、`ImmutableState`、`StateView` 或另一种状态容器；`State`、`ReadOnlyState`、两种 Resource 采用**组合与单向转换**，互不公有继承。反向构造、恢复可写句柄和 `is_writable()` 均不提供。擦除协议收进库内节点，不作为公共基类暴露；控件侧只写 `StateSink::push`（§3.4）。

### 2.6 旧章节迁移

§2 的旧小节曾以 `SignalView<T>` / `Reactive<T>` / `Computed<T>` / `Effect` 为原语清单；对应能力现由 §2.1 的四个词承载，旧类型名不再存在：

| 旧入口 | 迁移去向 |
|:---|:---|
| `SignalView<T>`（只读视图引用） | 按消费方权限改为 `ReadOnlyState<T>` 值句柄；外部只读点不再借引用 |
| `Reactive<T>`（属性容器） | 属性本身即 `ReadOnlyState<T>` 或 `State<T>`；常量经句柄的隐式构造接入 |
| `Computed<T>` / `au::Computed<T>{...}` | `computed(fn)`；需要显式结果类型时写 lambda 的显式返回类型 |
| `Effect` 类 / `run()` / `dispose()` | `auto held = effect(fn)`；调用即首跑，凭证是 `Subscription`（§3.5） |
| `Binding<T>` | 可编辑属性直接持 `State<T>`，展示属性持 `ReadOnlyState<T>`；不再保留 value/binding 双路径 |
| `Immutable<T>` / `Mutable<T>` | 只读处持 `ReadOnlyState<T>`，需要写入处持 `State<T>`；不把 `const State&` 当权限隔离 |

---

## 3 句柄、订阅与传播

### 3.1 句柄语义总表

| 操作 | 目标语义 |
| :--- | :--- |
| 从 T 构造 `ReadOnlyState<T>` | 创建自有常量来源，保留展示 props 的隐式字面量构造；不要求额外中转 State |
| 从 T 构造 `State<T>` | 创建可写来源；允许构造期值隐式转为双向属性的 State |
| 默认构造 ReadOnlyState / State | 仅当 T 可默认构造时可用，创建含 `T{}` 的有效来源，不产生空句柄 |
| 复制或从右值构造、赋值句柄 | 共享来源；采用保源复制语义，源句柄仍有效且身份不变，不因 `std::move` 出现空来源 |
| `a = b` | 仅把 a 重绑到 b 的来源；a 的其他别名与既有订阅仍留在原来源 |
| `State<T>{source.get()}` | 显式复制当前数据到独立来源，要求 T 可复制；不是复制句柄 |
| `get() const -> const T&` | 借用当前值并参与自动依赖追踪；不是历史快照 |
| `set(T) const` | 向同一可写来源提交候选值；所有别名可见 |
| `update(fn) const` | 以当前值计算新 T，经与 set 相同的提交路径发布 |
| `notify() const` | 不替换当前值，强制该来源发出一次变更 |
| `subscribe(fn) const` | 返回 `[[nodiscard]] Subscription`；回调签名为可接受 `const T&` 的可调用体 |

`State` 的 `set/update/notify` 是 `const` 成员：const 限制句柄重绑，不撤销已经授予的写权限，因此 `[state]` 按值捕获的普通 lambda 可以调用 `state.set/update`。只读能力边界必须传 `ReadOnlyState`，而非 `const State&`。`ReadOnlyState` 不承诺 T 内部指针指向对象的深度不可变。

`update(fn)` 调用形态为 `fn(const T&) -> T`，内部读取旧值不登记依赖，updater 与比较器的执行也隔离外层依赖追踪；两者须为纯计算，不得重入状态写入。计算、比较或候选值构造失败时不发布通知、不改变旧值。

`notify()` 只有一个用途：T 内部共享可变对象原地修改后，让该修改对订阅者可见。它不是关闭去重的第二种写法，也不是穿透所有派生节点的全图强刷；下游派生输出若仍等价，可以停止继续传播。需要每次提交都通知时，在创建来源时传 `always_publish`（§3.3）。

### 3.2 共享、保源复制与借用

句柄对象只是来源的读/写外壳：复制、从右值交接都共享同一来源节点，复制句柄不新增来源节点，来源身份不变。`a = b` 是句柄重绑而非数据复制：既有订阅固定原来源，不会随重绑转移；`State<T>{other.get()}` 才是独立数据的复制。

`get()` 返回的引用是**借用**：不得跨可能发生的写入、来源重绑或最后持有者释放保存；跨异步边界应复制所需数据，或持有句柄本身。控件属性、订阅回调和派生计算体都按值捕获句柄，不保存 `get()` 的引用。

`State<T>` 与 `ReadOnlyState<T>` 都要求 `T` 可拷贝：`set/update` 把候选值放进可拷贝的写队列闭包。持有移动-only 载荷的类型只能读取，不能经句柄提交（终止于编译期约束，不会运行期丢写）。

### 3.3 比较与提交

比较策略在**来源创建时**确定并由全部别名共享，不允许从读句柄修改：

- `State<T>{initial}` 与 `computed(fn)` 默认在 T 满足 `std::equality_comparable<T>` 时调用相等比较，否则每次保守发布。
- 关闭去重使用具名标记：`State<T>{initial, au::always_publish}` 与 `computed(fn, au::always_publish)`。不接受空函数对象充当开关。
- 自定义等价使用可调用体作第二参数：`State<T>{initial, eq}`、`computed(fn, eq)`，签名为 `(const T&, const T&) -> bool`，返回 true 表示可观察等价。
- 比较器不进入 `State` 的模板参数。`State<T, Eq>` 无法绑定 `State<T>` 形参，`Checkbox(State<bool>)`、`Provider<State<T>>` 一类入口会静默拒绝自定义比较的来源；实现可为无捕获比较体保留快速路径，不作为公开契约。
- computed 的结果 T 取 callable 返回值去除 cv/ref 后的类型；需要指定结果类型时使用 lambda 的显式返回类型，不再另设 `computed<T>` 入口。比较器参数不参与 T 的推导。
- 比较相等时**不替换存储、不通知**；派生结果也遵循此规则。来源身份不同即应换线，不得用值相等替代身份比较。
- 浮点 NaN 按相等运算结果处理，默认不相等；无比较器的不可比较类型不得猜测相等。外部 T 的比较器必须由作者保证覆盖消费者可观察行为。

`LocalizedString` 的值等价覆盖 text、id、args、localize 四项，参数值补齐比较所需语义；args-only 与 localize-only 的更新对订阅者可见。

### 3.4 固定来源订阅

`source.subscribe(fn)` **固定订阅调用时的来源**，成功建立后立即交付首值一次，返回 `[[nodiscard]] Subscription`。原变量之后重绑不会转移已建立的订阅；回调读取其他 `ReadOnlyState` 不扩充此订阅的来源，也不污染外层 effect 的依赖。可取消、RAII 释放，取消只有一个动词（`reset()`）。

```cpp
au::State<int> count{0};
int seen = 0;
{
    au::Subscription sub = count.subscribe([&](const int &v) { seen = v; });  // 立即 seen = 0
    count.set(5);                                                             // → seen = 5
}                                                                             // sub 析构 → 自动取消
```

未保留凭证的临时订阅只完成首值交付，不留下后台订阅；`Subscription` 是 move-only 凭证，源句柄离开局部作用域后凭证仍安全。

**采集出口 `StateSink`（参考面）。** 控件在 `collect_signals(StateSink &out)` 中调用 `out.push(member_)` 登记属性来源：只登记来源身份，不附加属性名或作用域标签，也不触发用户回调；同一来源重复登记按身份去重。`watch(on_dirty)` 为全部来源各建一条固定订阅并首跑一次，任一来源提交即调用回调。临时构造的常量属性由采集结果保活。

### 3.5 effect 与首跑

`effect(fn)` **调用即执行一次**并自动追踪执行期间读取的来源，返回 `[[nodiscard]] Subscription`。不保留公共 `Effect` 类、`run()` 或 `dispose()`，也不公开依赖追踪钩子。丢弃返回值的临时凭证在首轮后即取消，编译器可诊断 `[[nodiscard]]`；需要延后启动时改为在启动点调用，或持有可空的 `Subscription` 稍后接管。

```cpp
au::State<int> count{0};
auto held = au::effect([&] { au::Diagnostics::warn("count = " + std::to_string(count.get())); });
count.set(1);   // → effect 重跑
```

两个布局构建器在挂载时以首跑读取 builder 登记依赖，不再手动订阅；首跑与随后的来源变化均由同一凭证收口。

### 3.6 写队列、稳定轮次与重入

- 回调内发出的 `set/update/notify` 排入后续轮，**当前回调及本轮其他回调仍读取本轮值**；最外层写入或首跑入口返回前，排空其同步传播与回调产生的后续轮。两次独立外层 set 是两次提交，不隐含多格原子事务。
- 每个终端消费者在一个稳定轮次最多交付一次，同轮按登记顺序交付；取消发生在执行前则跳过，执行中自取消只阻止后续交付。
- 一次值提交先使受影响的派生节点失效，完成必要重算后再交付终端订阅与自动追踪副作用；终端只能读到本次提交的稳定结果。菱形 `2x + 3x` 的订阅序列必须是 `5 → 10`，不得出现中间混合值。
- 新增固定来源订阅立即交付当前稳定首值，但不再重复参加该来源同轮已发生的通知；首值回调产生的写入仍入后续轮。自动追踪副作用的首次执行同样处于可读取稳定值的上下文。
- computed 求值中禁止写状态、创建订阅或执行领域命令；检测到派生求值重入或终端反馈环时诊断并中止该错误分支，互不相关的合法消费者与后续外部写入仍能工作。可能收敛的自反馈也不作为合法控制流，应移至明确的外部事件。
- 来源值已提交后不能回滚已执行的外部副作用。更新期派生计算失败保留最后成功值与依赖，依赖该失败节点的终端不会收到混合结果，后续依赖变化可以重试。业务回调抛出时诊断、恢复追踪与队列状态，继续其他合法消费者，不永久断订。初次 computed / effect / subscribe 无可用初值时，构造失败向调用者传播并彻底回滚登记。

完整内核契约（身份、轮次、剪枝、注册表）见附录 A。

### 3.7 Subscription

`Subscription`（`subscription.h`）是订阅或副作用的共同凭证：move-only、析构自动取消、`reset()` 是唯一显式取消动词、`active()` 查询是否仍持有有效订阅。来源固定性由创建入口（`subscribe` 固定、`effect` 追踪）决定，不改变类型契约。AI 生成代码无需手动保存或调用取消句柄——把返回值留在作用域即可。

### 3.8 旧 connect / bind 迁移

旧自由函数 `connect(src, fn)`（更早名 `bind`）已删除，统一改为 `source.subscribe(fn)`；两者首值语义一致（建立后立即交付当前值）。旧 `connect(store, fn)` 不交初值的行为随 `Store` 一并下线（§4），需要初值的消费者在迁移时自行读取一次当前值。

历史备注保留：原名 `bind` 因 `State` 基类链上的 `std::enable_shared_from_this` 使 `std` 进入实参的 ADL 关联命名空间集，无限定 `bind(state, fn)` 会被变参转发的 `std::bind` 吸走（编译通过但回调永不执行）。改为成员入口后该歧义从根上消失；`Animator::bind` 为成员函数，保持原名。

---

## 4 Store：已下线

`Store<S>` / `Action` / `Reducer` / `make_store` / `as_signal` 已从公共面删除。类 Redux 容器把 reducer 纪律、类型擦除 Action 与状态保存捆在一起，却不负责子树作用域；其职责按下列方式拆分：

| 旧入口 | 迁移去向 |
|:---|:---|
| `make_store(initial, reducer)` + 全局单例 | 应用自己的 `State<S>`（或若干领域 `State`）按值/共享指针持有，经 `Provider` 注入子树 |
| `dispatch(Action{"..."})` | 应用领域命令函数：校验、钳位、审计后写对应 `State` |
| reducer 纯函数纪律 | 领域命令内部自持；`update(fn)` 只是纯计算提交，**不是** reducer、事务或审计系统的等价替代 |
| `store.subscribe(fn)`（不交初值） | `state.subscribe(fn)`（立即交初值）；需要跳过首值时在回调内自行判断 |
| `store.as_signal()` | 直接把该 `State` 降级为 `ReadOnlyState` 或按值传入 |
| 撤销 / 重做 | 显式使用 `UndoStack`（§6）；多命令宏组合也不自动成为一次原子提交 |

迁移时不能只把 `dispatch` 机械改成 `update` 就声称纪律不变：校验、审计和撤销责任必须落到明确的领域命令或 `UndoStack` 上。

---

## 5 异步与协程

### 5.1 回调式 Task

`Task<T>`（`async.h`）由 `au::async(fn)` 创建（`async.h`）：在后台线程池执行 `fn`（`fn` 返回 `T` 或 `Result<T>`，抛出的异常捕获为 `runtime-async-exception` 错误）。

| 成员 | 签名 / 说明 |
|:---|:---|
| `then(DoneFn cb)` | 注册完成回调，`DoneFn = std::function<void(const Result<T>&)>`，返回 `Task&` 以便链式（`async.h`） |
| `with_timeout(d)` | 超过 `d` 未完成则向 `then` 回调投递 `async-timeout` 错误（`async.h`）。两条实现路径按线程池模式分流、语义一致：worker 池下看守是**可中断等待 `d` 的后台池任务**（进程退出时由线程池 `stop_` 信号提前唤醒，析构不再卡满超时窗口）；deferred（无 pthreads）下无线程可睡，只登记到期时刻，由 `Application::step_frame()` 帧尾扫描触发，等待中的宿主循环经该期限并入唤醒决策 |
| `cancel()` | 取消结果投递（`async.h`）：置位后 `then` 回调不再被调，已登记的取消观察者在锁外恰触发一次。幂等——第二次调用既不重新置位也不再触发观察者 |
| `on_cancel(hook)` | 登记取消观察者（`async.h`）：`cancel()` 置位那一刻在**调用线程**上同步触发一次。三条契约与 `net::Cancellation::on_cancel` 同构——只保留最后登记的一枚（传空即撤销）、至多触发一次、**登记时若已取消则在当前线程立即触发**（不留「登记即失效」的窗口）。观察者不得阻塞，也不得假设自己跑在哪条线程上。本入口把「取消」补成可订阅的事实，于是承接侧（一条在途连接、一份待归还的句柄）能在同一刻放手，不必轮询取消标志或等自己的超时 |
| `set_main_poster(poster)` | 静态，安装跨线程投递器（`async.h`） |

**`then` 的回调默认在承载 `fn` 的线程池 worker 线程上同步执行**（未安装主线程投递器时直接调用 `fn`，供 headless / 测试；deferred 构建下「承载线程」就是宿主泵任务的线程，即 UI 主线程）；安装 `Task::set_main_poster`（或 `au::async` 的主线程投递器）后，回调才经投递器回到 UI（主）线程执行，此时方可直接更新 UI。安装者与帧循环唤醒的联动契约见 [`06-app-platform.md`](06-app-platform.md)。

```cpp
auto task = au::async([] { return fetch_from_network(); });
task.with_timeout(std::chrono::seconds(5))
    .then([](const au::Result<Data> &r) {          // 安装主线程投递器后在 UI 线程（否则在 worker 线程）
        if (r) labels.set("loaded");
        else   au::Diagnostics::report(r.error().message, {}, r.error().code);
    });
```

**限制**：`with_timeout` 与 `cancel()` 都**无法中断任意 `fn`**，只丢弃或改道结果。deferred 构建另有两点如实口径：① 超时精度为**一帧**（到期只能由帧尾扫描发现，非墙钟即时）；② 任务本身也是排队执行，故期限到点而任务尚未跑完是常态——此时改道获胜，任务事后跑完不再二次回调（`delivered` 单点裁决）。

**口径补充（取消的第二半效果）**：上句的「只丢弃或改道结果」约束的是**作业体自身**——`fn` 仍在承载它的线程上跑到结束，本层无法把它掐掉；它不等于「取消对整条链路毫无副作用」。经 `on_cancel` 登记的观察者把同一刻的取消事实外传，于是承接侧能在作业体之外真正放手：`net/http_binding.h` 就把一次 HTTP 交换的取消接到 `net::Cancellation` 上，在途套接口当场关闭（判据见 `tests/unit/utest_net_http_binding.cpp` 的 `a_reload_cuts_the_socket_of_the_previous_request` 与 `destroying_the_owner_releases_the_in_flight_socket`，证据取在对端：连接被读到 EOF、串行监听口的下一次 accept 照常受理新连接）。§7.2 与 §7.3 里那句旧写法按本段理解，编号与文字一律保留。

### 5.2 协程路径

| 符号 | 说明 | 位置 |
|:---|:---|:---|
| `co_async(fn)` | 返回 awaitable，`co_await` 在后台线程池执行 `fn`，续体经主线程投递器回到主线程 | `coroutine.h` |
| `CoroTask<T>` | 协程返回类型 | `coroutine.h` |
| `CoroTask<T>::on_done(hook)` | 登记完成观察者：帧收尾那一刻以协程的 `Result<T>` 触发一次，三条契约与 `Task<T>::on_cancel` 同构（最后登记的一枚生效、至多一次、登记时帧已收尾则当前线程立即触发）。本库的 promise 永不首挂也不终挂，帧只靠 `resume` 前进，收尾历来只有 `is_done()` 一条轮询问法——本入口把「结束即交付」补成订阅问法，适配层因此不必自带节拍。**`void` 特化不提供**：它能交出的只有「已结束」与一枚可选错误，而有值形态那条契约的载荷在这里没有对应物 | `coroutine.h` |
| `launch(task)` | 启动顶层协程 | `coroutine.h` |

```cpp
au::CoroTask<void> load() {
    au::Result<Data> r = co_await au::co_async([] { return fetch_from_network(); });
    if (r) labels.set("loaded");
}
au::launch(load());
```

### 5.3 使用约定

- 禁止裸 `std::thread`；后台底座是有界线程池 `ThreadPool::default_pool()`（[`01-core.md`](01-core.md) §6）。
- 禁止在 `on_click` 等 UI 回调中直接阻塞。
- 异步错误通过 `Result<T>` 传递，回调必须处理成功与失败两个分支。
- 定时与周期任务**不属于**本模块，由 `Scheduler` / `Timer` 承担，见 [`06-app-platform.md`](06-app-platform.md)。

### 5.4 三态资源绑定 `Resource<T>` 与 `StreamResource<T>`

`resource.h` 把「一个会异步落定的结果」变成 widget 可直接读的只读状态：三态 `Loading / Ready / Failed`，读取即登记依赖、落定即定点刷新（对位业界 `FutureBuilder`）。任意异步计算、文件读取、进程间调用都能产出资源，故 `net/` 不进 `state/` 的依赖图。

| 成员 | 签名 / 说明 |
|:---|:---|
| `Resource<T>::from(Task<T> job, opts = {})` | 由**已启动作业**创建并立即等待其结果（`au::async(fn)` 的产物）。该形态没有可重发来源，`reload()` 只复位到 `Loading` 并重新等待**同一**作业 |
| `Resource<T>::from(std::function<Task<T>()> producer, opts = {})` | 由**作业来源**创建并立即发送第一条作业；每次发送调 `producer` 产出新作业，故 `reload()` 是真重发并取消前序。`producer` 在簿记锁内被调用，不得回调回本资源 |
| `reload()` | 重发；去抖窗口非 0 时窗口内的多次调用合并为一次发送（最后一次生效），窗口内不改动状态 |
| `phase()` / `value()` / `error()` | 阶段 / 成功值（`const T *`，无值 `nullptr`）/ 失败详情；读取即登记依赖 |
| `get()` | 整份 `ResourceSnapshot<T>` 的常引用；下一次落定写入后失效 |
| `subscribe(fn)` | 订阅落定事件，建立后立即交付当前快照一次；回调保活控制核，作业不会因资源句柄被释放而静默停摆 |
| 到 `ReadOnlyState<ResourceSnapshot<T>>` 的隐式转换 | **别名**读句柄：与控制核共享所有权并指向同一来源，作业随读句柄保活并继续落定 |
| `StreamResource<T>::open(opts = {})` | 打开等待写入的流槽位（初始 `Loading`） |
| `StreamResource<T>::push(item, wire_bytes = 0)` | 写入端交付一条数据并计入在途批次；`wire_bytes` 供 `batch_bytes()` 累计，非线上流传 0 |
| `StreamResource<T>::fail(err)` | 报错终止：丢弃未提交批次，状态进 `Failed`，已提交数据保留；终态之后的 `push()` 被丢弃（续用请另开一个流资源） |
| `items()` / `batch_bytes()` | 已提交列表的只读引用（下一次提交后失效）/ 最近一次提交批次的累计线上字节数 |
| `ResourceOptions` | `{ timeout = 30s, debounce = 0 }`；`timeout <= 0` = 不设超时。默认值在声明处可见（`SPEC.API.SENSIBLE-DEFAULTS.001`） |
| `StreamOptions` | `{ policy = StreamPolicy::Append }`；`Append` 累计、`Replace` 只留最新一条 |

**快照有效性**（字段一次提交，不拆成多个独立来源）：

| 来源与阶段 | 快照有效性 |
| :--- | :--- |
| Resource 初始 Loading | value 与 error 均空 |
| Resource 重发 Loading | error 清空；value 可保留最近已提交的旧值，供等待新结果期间展示 |
| Resource Ready | value 有值，error 空 |
| Resource Failed | value 空，error 有值 |
| Stream Loading | items 空，error 空，batch_bytes 为 0 |
| Stream Ready | items 为已提交数据，error 空，batch_bytes 是最近一批累计线上字节 |
| Stream Failed | 保留已提交 items，丢弃未提交批次，error 有值；batch_bytes 保留最近成功批次值 |

三态不外扩：刷新中且已有旧值写作 `Loading` 且 value 非空；超时是 `Failed` 且 error 的编号为 `async-timeout`；「从没启动」不可达，因为 Resource 禁止无作业的默认构造；「已取消」不可观察，取消只表现为最后一个持有者释放或 `reload` 换作业。故不增加 Idle、Refreshing、TimedOut 或 Cancelled 相位。读取顺序固定为先相位后值，避免 value 非空掩盖失败；失败后仍要展示旧内容的界面自持一份最近成功值，快照不在 Failed 保留 value。

五条不变量：

- **一个资源 = 一个信号源**：阶段、值、错误打包成一份快照写进同一个 `State<...>`，一次落定只通知一次——三分支渲染必然同时读这三者，拆成三个信号只会让每次迁移多跑一轮重建；粒度因此收在「每个资源一个信号」，不同资源之间依旧互不牵连。Resource 与 Stream 的内部快照源显式禁用默认等值短路，以保留每次有效落定/批次的通知语义；下游 computed 可自行去重。
- **读句柄保活控制核**：临时 Resource 转为 `ReadOnlyState` 后作业必须继续运行并落定；最后一个控制句柄、读投影、订阅持有或派生闭包中的资源持有者释放，才取消未完成作业。控件卸载仅在释放最后持有者时触发取消。控制核不得保存拥有自己的读句柄，完成回调保持弱引用。
- **结果不串台**：每次发送推进一个代次（generation），完成回调只认自己那一代。`Task::cancel()` 只能让尚未投递的结果闭嘴，**已排进主线程队列的那次投递照样抵达**，故 `reload()` 之后前序作业的迟到结果由代次守卫丢弃，不覆盖新状态。快照通知在簿记锁外执行；回调中请求 reload/push/fail 等动作也遵守后续轮边界，候选提交须复核代次。`producer` 的锁内调用前置条件单独保留，不扩张为允许其重入本资源。
- **帧内合并（流式）**：`push()` 只累加批次，并把「提交」经主线程投递器排队；队列排空前到达的全部交付合并成**一次**状态写入。故宿主卡顿期间的多次交付损失的是提交次数，不是数据——`batch_bytes()` 给出的是该批累计量，不是末条大小。
- **默认值与通道复用**：默认 30 秒超时、0 去抖；`timeout` 复用 `Task<T>::with_timeout` 的两条分流（worker 池下是可中断等待 `d` 的池任务；deferred 下登记到期看守、由帧尾扫描触发），去抖延时的通道与之相同，精度口径同 §5.1。`from(Task)` 的 reload 重新等待同一作业，不先取消它；`from(producer)` 的 reload 取消前序并产出新作业。

**必须先安装主线程投递器**（`Task<T>::set_main_poster`，真实事件循环已装）：状态写入发生在完成回调所在线程，未装投递器时回调在承接作业的 worker 线程直调，写 `State` 即违反「main-thread only」。deferred（无 pthreads）构建下承载线程就是宿主泵任务的线程，自装与否都不越约定。

```cpp
auto request = au::Resource<std::string>::from(producer);
au::ReadOnlyState<au::ResourceSnapshot<std::string>> snapshot = request;
auto message = au::computed([snapshot]() -> au::LocalizedString {
    const auto &current = snapshot.get();
    if (current.phase == au::ResourcePhase::Failed) {
        return au::LocalizedString{"Failed"};
    }
    if (current.value) {
        return au::LocalizedString{*current.value};
    }
    return au::LocalizedString{"Loading"};
});
au::Text status{au::TextProps{.content = message}};
request.reload();  // 前序请求自动取消；旧结果不会盖住新状态
```

---

## 6 依赖图与撤销栈

| 类型 | 说明 | 位置 |
|:---|:---|:---|
| `StateGraph` | 状态依赖图，含 `Node` 与 `Edge` 结构；`to_json()` / `to_text()` 导出，便于 AI 做快照测试 | `state_graph.h` |
| `UndoStack` | 撤销/重做栈，命令结构为 `UndoCommand`（成对 redo/undo） | `undo_stack.h` |

`State<T>` 的**值本身不直接序列化**；要导出结构用 `StateGraph::to_json()`。

`StateGraph` 保证来源—观察者图正确且有界，但不宣称仅转发 anchor 就能导出「哪个 Widget 的哪个属性」；本库不提供控件属性标签或作用域标签。节点身份取引擎分配的稳定编号（来源 `s<id>`、终端 `t<seq>`），不取对象地址，也不挂回公共句柄；复制句柄或只读降级不新增节点，同一编号不复用为另一活节点。节点 `kind` 按目标节点角色取值：`source`（值来源）、`derived`（派生来源）、`terminal`（终端运行记录）；边类别恒为 `reads`。JSON 顶层带 `schema: 1`，text 首行为 `StateGraph schema=1:`，使消费者能区分「格式不符」与「图内容变化」；旧 `state` / `effect` 类别字符串随对应公共类型下线，Inspector 与快照测试按 schema 判读。

`UndoStack` 的 `push(cmd)` 入栈并立即执行 redo、清空重做历史；`undo()` / `redo()` 逐条执行；`merge(cmds)` 把多个命令合并为单个宏命令（一次 undo/redo 整组执行）。`Store` 下线后，校验、审计和撤销责任交给应用领域命令与 `UndoStack`，不能只把 dispatch 全部机械改成 update 就声称纪律不变。

---

## 7 需求规格

### 7.1 SPEC.FEAT.STATE.SIGNAL-STATE.001 单向数据流与细粒度信号状态模型

**核心目标：** AI 易理解状态。

**需求陈述：** UI 是一棵静态构建的组件树，状态保存在 `State<T>` 中；组件在渲染期读取句柄时自动登记依赖；状态变化只重跑依赖它的局部计算，不重建整棵树、不做 key/diff。只读消费者得到 `ReadOnlyState<T>`，写权限由类型表达而非 `const` 修饰。

```cpp
au::State<int> count{0};
auto label = au::computed([count]() -> au::LocalizedString {
    return au::LocalizedString{std::to_string(count.get())};
});
au::Text text{au::TextProps{.content = label}};   // 派生直通展示属性，无需中转 State
auto increment = [count] { count.update([](const int &old) { return old + 1; }); };
```

**关键原则：**

- **State → View 是纯函数映射**：组件的绘制是句柄当前值的纯函数。
- **事件 → State 通过普通赋值**：`count.set(...)` / `update(...)`，无需 Action / Reducer 样板。
- **细粒度、定点刷新**：只有读取过该来源的组件与计算会被重跑；一次提交按稳定轮次传播，终端本轮至多交付一次（§3.6）。
- **派生直通属性**：`computed` 返回 `ReadOnlyState<T>`，可直接填展示属性；常量经隐式构造接入。
- **状态可快照**：`StateGraph::to_json()` 导出依赖图结构（`schema: 1`）；`State<T>` 值本身不直接序列化。

**约束：**

- 默认状态方式是 `State<T>`，AI 只需学这一条即可覆盖绝大多数场景。
- `State<T>` 变化固定在 UI 线程派发，无需锁。
- 禁止在渲染（构建）期产生副作用（写状态）。
- 句柄按值传递与捕获，不保存 `get()` 的引用。

**验收标准：** 一次 `set()` 只重绘读取过该来源的控件；未读取该来源的控件不产生任何重绘或重算；菱形派生订阅序列为 `5 → 10`，不出现中间混合值；`ReadOnlyState<T>` 无 `set/update/notify`（`requires` 负向断言）；`StateGraph::to_json()` 能导出完整依赖图且带 `schema` 版本。

### 7.2 SPEC.FEAT.CORE.ASYNC-CONCURRENCY.001 结构化异步与并发模型（异步侧契约）

**核心目标：** AI 轻松处理耗时操作。

**需求陈述：** UI 线程唯一，所有 UI 更新必须在 UI 线程；耗时操作必须经 Aurora 提供的异步原语执行，禁止裸 `std::thread`，也禁止在 UI 回调中直接阻塞。

**API 与不变量：** 见 §5（`au::async` / `Task<T>` / `then` / `with_timeout` / `co_async` / `CoroTask<T>` / `launch`）。

**验收标准：** 全部后台工作都可在无 GUI 环境（headless）下完成并回到主线程；`ThreadPool` 存活期间不出现新建的游离 `std::thread`；超时与取消路径只改变结果的投递去向，绝不中断用户函数。

> **口径补充（与 §5.1 同源，编号与上句文字一律保留）**：上句约束的是**作业体**，不是整条链路。取消同刻还经 `Task<T>::on_cancel` 外传给承接侧，故「绝不中断用户函数」与「取消把在途的对外作业掐停」并存——网络交付面（`net/http_binding.h`，见 [`06-app-platform.md`](06-app-platform.md) §8.8.30）就是这条外传的消费者。协程侧的对称入口是 `CoroTask<T>::on_done`（§5.2），它把「帧收尾」从轮询补成订阅。

> 内存与所有权侧的所有权模型见 [`01-core.md`](01-core.md) §8.1（SPEC.QUALITY.CORE.MEMORY-SAFETY.001）；跨线程回投与帧循环唤醒见 [`06-app-platform.md`](06-app-platform.md)。

### 7.3 SPEC.FEAT.NET.DECLARATIVE-BINDING.001 异步结果三态响应式绑定

**核心目标：** AI 少写胶水——请求即状态来源。

**需求陈述：** 一次异步作业（网络请求、文件读取、进程间调用）可以直接退化成一个 widget 可读的三态只读状态源：`loading / ready / failed` 定点刷新，重发自动取消前序且旧结果不覆盖新状态，宿主失效后回调被丢弃。绑定层不认识网络，故 `net/` 不进入 `state/` 的依赖图。

**API 与不变量：** 见 §5.4（`au::Resource<T>` / `au::StreamResource<T>` / `ResourceSnapshot<T>` / `StreamSnapshot<T>` / `ResourcePhase` / `StreamPolicy` / `ResourceOptions` / `StreamOptions`）。把一次 HTTP 交换送进这三态的接线件在 net 侧（`net/http_binding.h` 的 `deliver` / `bind_resource` / `deliver_stream` / `image_fetcher`），形态与判据见 [`06-app-platform.md`](06-app-platform.md) §8.8.30——本模块为它多出的只有 §5.1 的 `Task<T>::on_cancel` 与 §5.2 的 `CoroTask<T>::on_done` 两枚订阅口。

**验收标准：** ① 一次落定只通知一份快照、只通知一次；② `reload()` 取消前序作业，且其迟到结果不覆盖新状态（含通知内重入 reload）；③ 宿主析构后抵达的回调被丢弃，无悬垂访问；临时资源转 `ReadOnlyState` 后仍能落定，释放最后持有者才取消；④ `StreamResource` 的 `Append` / `Replace` 两策略与帧内合并批次（`batch_bytes()` 为累计量）；⑤ 快照字段符合 §5.4 的有效性表（重发保留旧值、Failed 清空 value）；⑥ 默认超时 30 s、`reload()` 不去抖，默认值在声明处可见。单测见 `tests/unit/utest_state_resource.cpp`。

**约束：** 状态写入限主线程（须先装主线程投递器）；取消只改结果的投递去向，不中断作业体（作业体之外那一半见 §5.1 的口径补充：来源登记了 `on_cancel` 时，取消同时掐停来源的在途作业——HTTP 交付面即如此，判据在 `tests/unit/utest_net_http_binding.cpp`）；去抖与超时复用 `Task<T>` 的既有通道，精度口径随其构建模式（见 §5.1）。**持续态事实不进这三态容器**：`Resource` / `StreamResource` 折算的是「一次作业的落定」，而链路态（[`06-app-platform.md`](06-app-platform.md) §8.8.41）没有落定时刻、也不由本容器驱动——宿主在 `ConnectivityMonitor::subscribe()` 的回调里写自己的 store（回调已回投主线程），故 `state/` 仍不认识网络。

---

## 附录 A 内核不变量（实现与验收）

本附录只约束实现与验收，不进入使用者的推断路径；日常选型只看 §2。

### A.1 身份与所有权

```text
State / ReadOnlyState ──拥有──> 来源节点
Subscription ──拥有──> 订阅或运行记录及其来源
派生节点 ──拥有──> 计算体与内部运行记录
来源节点 ──弱引用──> 消费者运行记录
计算体 ──按值捕获──> 上游句柄
```

来源使用独立于句柄地址、基类子对象地址和注册表槽位的稳定身份；同一来源的复制/只读降级不新增来源节点，复制句柄不推进身份。边的两端都引用真实节点身份，重复读取后的反向依赖仍完整。内核不得构造强引用环；派生内部记录不能反向拥有其所属派生节点。用户 callable 若捕获包含自身订阅凭证的宿主，可自行形成共享环，示例须避免；`[&]` 不因句柄化而获得生命周期延长。

运行记录必须在首次执行前建立，执行中的 callable、当前来源和交付值必须保活；构造失败由已构造成员的 RAII 清理，而非依赖未完成对象的析构函数。

### A.2 轮次与交付

一次值提交（`write_committed`）先推进来源版本，再从提交来源标脏可达节点：派生节点只置脏，终端进入待交付集合；随后按登记序号排序、逐条交付。每次成功求值按本轮读取集合替换依赖；相同来源重复读取只保留一条边，不再读取的旧分支退订；正反向关系同步更新，不能用去重分支跳过反向登记。派生节点在传播排空前按需完成求值（失效即重算），动态分支中新读到的派生节点也必须先稳定。每个终端在一个稳定轮次最多交付一次；取消发生在某消费者执行前则跳过；执行中自取消仅阻止后续交付，当前 callable 由运行记录保活直到返回。

回调中的 `set/update/notify` 经写队列排入后续轮，当前轮其余消费者仍读本轮值；最外层入口返回前排空后续轮。连续两次 update 到执行时才读当前值，不覆盖彼此；入队操作及 callable 由队列拥有，不借用调用栈。

终端写入以因果链追踪：若再次到达该链中已经执行的消费者，即视为反馈环，记录诊断并丢弃该循环分支的后续操作；即使可能收敛的自反馈也不作为合法控制流。派生求值重入同类处理：诊断并中止该错误分支。

### A.3 依赖剪枝与注册表

每次成功求值/回调按本轮读取集合替换依赖边：同源重复读取保留一条边，旧分支解边。`StateSink` 的采集同样按来源身份去重。正常销毁、取消、构造失败均注销相应运行记录并解绑；注册表服务生命周期必须覆盖最后一次注销，不能产生静态销毁次序悬垂——传播引擎为进程级常驻对象。长寿命且不再写入的来源也即时清除已取消消费者的边，不依赖下次 notify 扫墓。交换删除若用于注册表，须回填移动项的下标。

### A.4 失败与恢复

来源值已提交后不能回滚已执行的外部副作用。更新期派生计算失败保留最后成功值和依赖，并阻止依赖该失败节点的终端收到混合结果；后续依赖变化可以重试。业务回调抛出时诊断、恢复追踪与队列状态，继续其他合法消费者，不永久断订。初次 computed / effect / subscribe 无可用初值时，构造失败向调用者传播并彻底回滚登记；无返回错误通道的运行期错误走既有诊断政策，不为只读写入设置运行期兜底码。

### A.5 图身份与导出

`StateGraph` 的节点取引擎分配的稳定身份（来源 `s<id>`、终端 `t<seq>`），边只连接真实节点；节点与边的 `kind` 按目标节点角色重新取值（`source` / `derived` / `terminal`；边恒为 `reads`），旧类别字符串随对应公共类型下线。JSON 顶层带 `schema: 1`，text 首行 `StateGraph schema=1:`；导出结构变化同批同步 Inspector 与快照测试。需要定位具体节点时用对象创建前后的节点集合差，不为此新开 `id()` 一类公共访问器。
