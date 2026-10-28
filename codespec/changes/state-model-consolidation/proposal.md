# CHG-045 响应式状态模型收口：可写状态与只读状态两词化

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-045 |
| 提出日期 | 2026-10-28 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.STATE.SIGNAL-STATE.001 |
| 影响面 | 破坏性公共 API 与响应式内核变更：`State` / `ReadOnlyState` 句柄、派生工厂、订阅、传播、注册表与资源快照；widget 属性权限、运行期换源、序列化；Preferences、Provider、媒体控制、滚动与定时器读点；相关单测、demo、自描述生成物和规格。具体范围、迁移与验收见下文。 |

本提案规定目标契约。实现与权威规格已按本契约迁移；状态按「已提议 → 实施中 → 已归档」流转，全量门禁与验收判据取得证据后归档。本文自包含，不依赖未随仓库分发的评审材料。

## 动机

### 来源无法统一进入属性

已删除的 `Reactive<T>`（include/aurora/state/reactive.h，本次变更后不再存在）始终持有 `std::shared_ptr<State<T>>`；`include/aurora/state/computed.h` 的 `Computed<T>` 则是另一种 `SignalView<T>` 来源，不能直接构造成同类型展示属性。外部读点又使用视图引用，形成「抽象读接口、可写状态、属性包装」三套重叠入口。`codespec/specification/02-state.md` 的派生文案配方因此要求手写中转状态和回调。

真正应区分的是**消费方是否应获得直接写入能力**，不是值会不会变化。派生缓存、异步结果、播放进度都会变化，但消费方不应因此获得任意改写它们的权限。

### 所有权与属性接线存在断层

`include/aurora/state/state.h` 的 `State<T>` 同时承载数据、观察边与对象身份，`shared()` 又要求对象已经由共享指针管理；未满足该要求时实际抛出 `std::bad_weak_ptr`，并非当前注释所称的未定义行为。已删除的非拥有 `Binding<T>`（include/aurora/state/binding.h）另持裸状态指针，迫使部分控件同时维护 binding 与内部 value 两条路径。

`include/aurora/widget/widget.h` 的 `Widget::track` 捕获属性地址，但观察边登记于实际来源。属性句柄被替换不等于新来源已经被订阅。`Text::set_content`、`Button::set_label` 的换源与 `ProgressIndicator::set_value` 在绑定分支的上游写入也不是同一种操作，不能用全局类型替换掩盖差异。

### 传播、比较和生命周期需要成为可验收契约

- `StateBase::notify` 直接遍历观察者并递归执行；当前菱形 `2x + 3x` 在 `x: 1 → 2` 时可以依次暴露 `5 → 7 → 10`，等值短路不能消除这个混合态。
- `Effect::run` 清空反向依赖却未撤销旧来源边；重复订阅去重时又可能不补回反向依赖，导致条件分支继续触发、导出图与通知关系不一致。
- 已删除的 include/aurora/state/state_registry.h 中的注册记录只追加不注销；长期存活来源上的失效观察边也不能只等下次写入才回收。
- `include/aurora/i18n/localized_string.h` 的 `LocalizedString::operator==` 忽略 args/localize。直接把「存在相等运算」当成去重依据会吞掉可见变化。
- `Effect` 当前构造不首跑。两个布局构建器在挂载时显式订阅但没有 `run()`，所以改为构造即执行是行为变更，不能声称所有调用点均无变化。

### 领域事实不能冒充任意写入口

`Preferences::watch` 是存储到只读句柄的单向投递；既有测试已经说明 `binding.set` 不更新配置存储。`Provider<T>` 注入的是 T，不会自动赋予后代修改其来源的能力。`include/aurora/media/video_source.h` 的 `VideoController` 同时提供播放事实与控制命令，修改事实缓存不等于执行 seek 或音量后端操作。

`Store<S>` 把 reducer 纪律、类型擦除 Action 与状态保存捆绑，但它不负责子树作用域；`Immutable<T>` / `Mutable<T>` 的当前调用仅在自身测试。收口应删除重复容器，同时保留领域校验、撤销和作用域各自的职责，不把 `update` 宣称为 reducer、事务或审计系统的等价替代。

## 变更内容

本节按三档组织：日常面是使用者与模型必须记住的全部，见 `### 使用者教学面`；专项面按需查阅；其余小节是内核不变量，只约束实现与验收，不进入使用者的推断路径。

### 能力与来源

| 公共形态 | 权限与职责 |
| :--- | :--- |
| `State<T>` | 拥有来源的读写句柄；读订阅能力加 `set`、`update`、`notify` |
| `ReadOnlyState<T>` | 拥有来源的只读句柄；`get()`、`subscribe(fn)`，没有通用写入口 |
| `computed(fn)` | 创建纯派生来源，返回 `ReadOnlyState<T>`；不再有公共 `Computed<T>` 类 |
| `Resource<T>` | 管理一次性作业，可单向转换为 `ReadOnlyState<ResourceSnapshot<T>>` |
| `StreamResource<T>` | 管理流式生产，可单向转换为 `ReadOnlyState<StreamSnapshot<T>>` |
| `Subscription` | 订阅或副作用的存活凭证：`subscribe` 固定来源，`effect` 自动追踪；析构即取消，不参与容器选型 |

「两词」只指能力类型这一对：`State<T>` 可写，`ReadOnlyState<T>` 只读，二者单向转换。日常面另含 `computed` 与 `effect` 两个工厂，见 `### 使用者教学面`；异步、订阅、撤销等概念没有删除，但按该节的三档出现，不与这对名字并列成选型项。

两个名字共用中心名词、受限能力带限定词：`ReadOnlyState` 在参数与属性位置单独出现时即自解释，不需要与 `State` 成对出现。它也不声称值不变或句柄非拥有——这两点分别与响应式语义和共享所有权冲突。保留 `State` / `ReadOnlyState`；不引入 `Property`、`ImmutableState`、`StateView` 或另一种状态容器。

`State`、`ReadOnlyState`、两种 Resource 采用**组合与单向转换**，互不公有继承。`State<T> → ReadOnlyState<T>` 隐式成立，反向构造、恢复可写句柄和 `is_writable()` 均不提供。Resource 转读句柄同样隐式成立，但读句柄不能恢复控制器。这样无法经可赋值的只读基类子对象重绑 Resource，使读数与控制命令指向不同来源。

擦除协议收进库内节点，不再作为公共基类暴露；`Widget::collect_signals` 改为接受 `StateSink&`，控件侧只写 `out.push(member_)`，不接触擦除基类。不提供可改写来源的基类赋值入口或写权限恢复入口。来源身份、观察边、运行记录属于库内节点，不属于用户句柄对象。锚点如需保留，只表达来源存活，不能当作拥有任意裸消费者对象的证明。

### 使用者教学面（AI First 验收基准）

公共名字按三档管理，文档与示例不得混档：

| 档位 | 内容 | 出现场合 |
| :--- | :--- | :--- |
| 日常面 | `State<T>`、`ReadOnlyState<T>`、`computed(fn)`、`effect(fn)` | 业务代码；GUIDELINE 配方只用这一档 |
| 专项面 | `Resource<T>`、`StreamResource<T>`、`ResourceSnapshot<T>`、`StreamSnapshot<T>`、`ResourcePhase`、`Subscription` | 引入异步、流式或显式取消时 |
| 参考面 | `StateSink`、采集覆写、自定义比较器 | 写自定义控件或调整发布语义时；应用代码不出现 |

日常面固定为四个词。新增公共名字须先证明这四个词无法表达，否则归入专项或参考面；不为同一个意图提供第二种写法。

```text
State<T>           能改的值        set / update / notify
ReadOnlyState<T>   只读的值        get / subscribe；computed 返回它
computed(fn)       算出来的值      随上游自动重算
effect(fn)         值变时做副作用  调用即首跑，返回 Subscription

State → ReadOnlyState 单向；写权限靠类型，不靠 const
```

纪律五条：

1. 句柄按值传、按值捕获；不要保存 `get()` 的引用。
2. 写入不会立刻读到：回调里的 `set/update` 排到下一轮。
3. 订阅与副作用都返回 `Subscription`；想让它一直活着就保留返回值。
4. 值相等不通知；要每次都通知，创建来源时写 `always_publish`。
5. `computed` 的结果可以直接接展示属性，不需要手写中转 `State`。

| 我要 | 写 |
| :--- | :--- |
| 一个能改的值 | `au::State<int> n{0};` |
| 一个算出来的值 | `auto m = au::computed([n]{ return n.get() * 2; });` |
| 只读参数或属性 | `void f(au::ReadOnlyState<int> n);` |
| 外部异步来的值 | `au::ReadOnlyState<au::ResourceSnapshot<T>> s = au::Resource<T>::from(producer);` |
| 值变时做副作用 | `auto held = au::effect([n]{ ... });` |

### 句柄与值的精确定义

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

保源复制语义也适用于本提案的 Resource 控制句柄；它们仍禁止无作业的默认构造。该选择避免在常规 API 中增加 nullable 状态，代价是从右值交接时仍需共享所有权操作，须纳入性能验证。

const 限制句柄重绑，不撤销已经授予的写权限，因此 `[state]` 按值捕获的普通 lambda 可以调用 `state.set/update`。只读能力边界必须传 ReadOnlyState，而非 `const State&`。ReadOnlyState 不承诺 T 内部指针指向对象的深度不可变。

`get()` 借用不得跨可能发生的写入、来源重绑或最后持有者释放保存；跨异步边界应复制所需数据。移除 `State::shared()` 和公共 `enable_shared_from_this` 继承，不声称这能排除所有模板实例对 std 的 ADL 关联。

### 比较与提交

比较策略在**来源创建时**确定并由全部别名共享，不允许从读句柄修改：

- `State<T>{initial}` 与 `computed(fn)` 默认在 T 满足 `std::equality_comparable<T>` 时调用相等比较，否则每次保守发布。
- 关闭去重使用具名标记：`State<T>{initial, au::always_publish}` 与 `computed(fn, au::always_publish)`。不接受空函数对象充当开关——那不是可读的规则。
- 自定义等价使用可调用体作第二参数：`State<T>{initial, eq}`、`computed(fn, eq)`，签名为 `(const T&, const T&) -> bool`，返回 true 表示可观察等价。
- 比较器不进入 `State` 的模板参数。`State<T, Eq>` 无法绑定 `State<T>` 形参，`Checkbox(State<bool>)`、`set_value_source(State<T>)`、`Provider<State<T>>` 一类入口会静默拒绝自定义比较的来源；这类静默陷阱比一次间接调用贵得多。实现可为无捕获比较体保留快速路径，不作为公开契约。
- computed 的结果 T 取 callable 返回值去除 cv/ref 后的类型；需要指定结果类型时使用 lambda 的显式返回类型，不再另设 `computed<T>` 入口。比较器参数不参与 T 的推导。
- 比较相等时**不替换存储、不通知**；派生结果也遵循此规则。来源身份不同即应换线，不得用值相等替代身份比较。
- `notify()` 只有一个用途：T 内部共享可变对象原地修改后，让该修改对订阅者可见。它不是关闭去重的第二种写法，也不是穿透所有派生节点的全图强刷；下游派生输出若仍等价，可以停止继续传播。

本批修正 `LocalizedString` 的值等价，纳入 text、id、args、localize；为参数值补齐比较所需语义，不把这个内置缺陷留给调用方逐次 `set + notify`。其他内置响应式载荷同批审计；外部 T 的比较器必须由作者保证覆盖消费者可观察行为。浮点 NaN 按相等运算结果处理，默认不相等；无比较器的不可比较类型不得猜测相等。

`update(fn)` 调用形态为 `fn(const T&) -> T`，内部读取旧值不登记依赖，updater 与比较器的执行也隔离外层依赖追踪。两者须为纯计算，不得重入状态写入。计算、比较或候选值构造失败时不发布通知、不改变旧值；提交实现须先完成可抛步骤再切换值存储，或以明确的 noexcept 条件约束原地交换，不能依靠可能部分修改 T 的赋值却宣称强保证。该保证不覆盖 T 内部共享可变对象的外部副作用。

### 单次写入的一致传播

将通知路径与动态依赖剪枝纳入本批，而不是仅优化相同值写入。保持主线程状态模型，不增加公共 scheduler、batch 或跨线程可写 State。

1. 对一个无环纯派生图，一次值提交先使受影响的派生节点失效，完成必要重算后再交付终端订阅与自动追踪副作用。终端只能读到本次提交的稳定结果；菱形 `2x + 3x` 的订阅序列必须是 `5 → 10`，不得出现 7。
2. 保持急切求值：失效派生节点在传播排空前完成求值。实现可用脏标记、版本或内部队列；动态分支中新读到的派生节点也必须先稳定，不能只按旧边的固定顺序计算。
3. 每次成功求值按本轮读取集合替换依赖；相同来源重复读取只保留一条边，不再读取的旧分支退订。正反向关系同步更新，不能用去重分支跳过反向登记。
4. 每个终端消费者在一个稳定轮次最多交付一次；同轮消费者按登记顺序交付。取消发生在某消费者执行前则跳过；执行中自取消仅阻止后续交付，当前 callable 由运行记录保活直到返回。
5. 在终端回调中发出的 `set/update/notify` 排入后续轮，**当前回调及本轮其他回调仍读取本轮值**。待本轮结束按入队顺序提交；update 到执行时才读当前值，连续两次 update 不得覆盖彼此。入队操作及 callable 须由队列拥有，不借用调用栈；嵌套调用返回不代表已提交，这一点属于显式迁移差异。
6. 最外层写入或首跑入口返回前排空其同步传播与回调产生的后续轮。两次独立外层 set 是两次提交，不隐含多格原子事务。多个候选写入不合并为一次，只允许同一轮的消费者去重。
7. 传播中新增固定来源订阅立即交付当前稳定首值，但不再重复参加该来源同轮已发生的通知；首值回调产生的写入仍入后续轮。自动追踪副作用的首次执行也必须处于可读取稳定值的上下文。

computed 求值中禁止写状态、创建订阅或执行领域命令。检测派生求值重入时诊断并中止该错误分支；终端写入以因果链追踪，若再次到达该链中已经执行的消费者，即视为反馈环，阻止再次执行并丢弃该循环分支的后续操作。即使可能收敛的自反馈也不作为合法控制流；应移至明确的外部事件。互不相关的合法消费者与后续外部写入仍能工作。

来源值已提交后不能回滚已执行的外部副作用。更新期派生计算失败则保留最后成功值和依赖，并阻止依赖该失败节点的终端收到混合结果；后续依赖变化可以重试。业务回调抛出时诊断、恢复追踪和队列状态，继续其他合法消费者，不永久断订。初次 computed / effect / subscribe 无可用初值时，构造失败向调用者传播并彻底回滚登记。无返回错误通道的运行期错误走既有诊断政策；具体诊断条目随实现登记，不为只读写入设置运行期兜底码。

### 固定订阅、自动副作用与所有权

`source.subscribe(fn)` 固定订阅**调用时的来源**，成功建立后立即交付首值一次。原变量之后重绑不会转移已建立的订阅；回调读取其他 ReadOnlyState 不扩充此订阅的来源，也不污染外层 effect 的依赖。`Subscription` 是两者的共同凭证：来源固定性由创建入口决定，不改变类型契约；可取消、RAII 释放，取消只有一个动词。未保留凭证的临时订阅或临时副作用只完成首值交付，不留下后台订阅。

`effect(fn)` 调用即执行一次并自动追踪读取，返回 `Subscription`；不保留公共 `Effect` 类、`run()` 或 `dispose()`，也不公开依赖追踪钩子。工厂标注 `[[nodiscard]]`：丢弃返回值的临时凭证在第一轮后即取消，编译器须能诊断。需延后启动的调用方改为在启动点调用，或持有可空的 `Subscription` 稍后接管，不再依赖“造好但未运行”的对象。迁移两个布局构建器时用首跑读取 builder 登记依赖，删除原手动订阅，明确其新增首跑标脏并保证不重复构建。

运行记录必须在首次执行前建立，构造失败由已构造成员的 RAII 清理，而非依赖未完成对象的析构函数。执行中的 callable、当前来源和交付值必须保活；只锁住独立空锚点后解引用已销毁运行记录或 Widget 的裸指针不合格。

```text
State / ReadOnlyState ──拥有──> 来源节点
Subscription ──拥有──> 订阅或运行记录及其来源
派生节点 ──拥有──> 计算体与内部运行记录
来源节点 ──弱引用──> 消费者运行记录
计算体 ──按值捕获──> 上游句柄
```

内核不得构造强引用环；派生内部记录不能反向拥有其所属派生节点。用户 callable 若捕获包含自身订阅凭证的宿主，可自行形成共享环，示例须避免。`[&]` 不因句柄化而获得生命周期延长。

### 属性权限与运行期换源

按**属性角色**迁移，不按整个控件批量划分：

| 属性角色 | 类型 | 操作约束 |
| :--- | :--- | :--- |
| 文本、颜色、样式、可见性、布局工厂、纯展示进度 | `ReadOnlyState<T>` | 只读来源；展示 setter 替换来源，不反写上游 |
| Checkbox / Switch / Slider / TextInput 等真正可编辑的模型值 | `State<T>` | 用户输入经原有校验或钳位后写同一格 |
| Preferences、播放进度等领域事实 | `ReadOnlyState<T>` 加领域命令或既有编辑事件 | 不能因控件可编辑就赋予事实缓存通用 set |

**采用收窄运行期修改入口的方案。** 独立 `XxxProps` 保留构建期配置；Widget 内已安装的响应式成员不可从外部任意赋值，经 getter 只读访问，经 setter 或属性更新入口修改。若当前控件公开继承 props，须隔离构建数据与已安装存储，不能保留可变 props 基类引用绕过入口。仅对本次语义必需的响应式成员调整可见性，不扩大到无关成员。

展示换源入口依次退订旧来源、保存新句柄、在已挂载时订阅新来源、失效所需布局与绘制缓存。即使新旧读数相等也必须执行；未挂载时只保存来源，挂载时再订阅，卸载清空订阅，重挂只订当前来源。Provider 换源还须使后代环境缓存失效。

统一语义而非强行统一命名：`Text::set_content(ReadOnlyState<LocalizedString>)`、`Button::set_label(ReadOnlyState<LocalizedString>)` 表示换源，常量由隐式 ReadOnlyState 构造接入。双向控件 `set_value(T)` 写当前 State；确需替换模型时使用独立 `set_value_source(State<T>)`，不重载成语义含混的 `set_value(State<T>)`。不同名的可编辑属性沿用“属性名 + source”的对应入口。

`ProgressIndicator` 是展示件，删除 Binding 写回能力；`set_value(double)` 保留钳位但改为换入自有常量来源，另以 `set_value_source(ReadOnlyState<double>)` 更换动态来源。此为明确的破坏性变化，不把旧绑定分支当成等价迁移。

每个 ReadOnlyState / State 属性必须在 `collect_signals` 中登记，由 `StateSink` 承接；采集继续显式维护。本批不实现属性自动发现，不为普通 ReadOnlyState 赋值偷偷附加控件通知语义。源码内成员 State 经采集协议订阅是合法方式，不再称为必须消灭的“暗道”。

### 序列化与事件

JSON 只保存当前值，不保存共享来源、派生 callable、比较器、订阅或领域命令，但新建与原地回填必须分开：

- 新建 `from_json` 创建自有值来源，双向属性得到私有 State，展示属性得到常量 ReadOnlyState。
- 对已有控件原地回填：State 属性经值 setter 写现有格，保留所有共享关系；ReadOnlyState 属性经换源入口断开旧源并保存新值。
- 程序 setter、来源通知、模型换源和 JSON 回填**不发出用户编辑事件**；真实输入成功改值后按既有控件事件协议发出。内部控件可通过不增加公共 bool 标志的路径区分输入来源。
- `Button::deserialize_props` 由原地写标签源改为换源；`Checkbox::deserialize_props` 保留共享值写入，但不得因调用同一 setter 意外发送用户事件。原有依赖程序赋值触发事件的用法需显式调用领域命令。

静态自描述仍描述值类型与属性键；示例中的公共状态类型名同步更新，由生成目标刷新 API 数据，不手改生成物。

### 异步来源与公开快照

将现有 detail 内的 `ResourceSnapshot<T>`、`StreamSnapshot<T>` 提升为正式只读输出值类型；Resource 提供 `get()` 与 `subscribe(fn)` 的只读委托及到对应 ReadOnlyState 的转换，既有阶段/值/错误便捷读取保留。没有通用 set 或从任意快照构造控制核的入口。

**读句柄保活产生读数的控制核，而不只是快照格。** 临时 Resource 转为 ReadOnlyState 后作业必须继续运行并落定。可使用控制核共享控制块的别名读句柄；控制核不得保存拥有自己的读句柄，完成回调保持弱引用。最后一个控制句柄、读投影、订阅持有或派生闭包中的资源持有者释放，才取消未完成作业。控件卸载仅在释放最后持有者时触发取消。

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

重发保留旧值遵循 `ResourceCore::send` 当前行为；`Resource::value()` 因而在重发 Loading 也可返回旧值，公开注释须纠正“非 Ready 恒为空”的错误。快照各字段一次提交，不能拆成多个独立来源。Resource 与 Stream 的内部快照源显式禁用默认等值短路，以保留每次有效落定/批次的通知语义；下游 computed 可自行去重。

保留现有两类作业入口：`from(producer)` 的 reload 取消前序并产出新作业；`from(Task)` 的 reload 重新等待同一作业，不先取消它。默认 30 秒超时、0 去抖、代次隔离、主线程投递、Stream Append / Replace 与帧内合并保持；状态传播只保证每次快照提交的一致性，不把异步到达改成同步执行。

快照通知在簿记锁外执行。回调中请求 reload/push/fail 等会影响快照的动作也必须遵守后续轮边界，不得绕过状态队列或让先前代次的候选快照迟到覆盖新代次；候选提交须复核代次。producer 的现有锁内调用前置条件单独保留，不扩张为允许其重入本资源。

### 领域观察与命令

**Preferences。** 根与 Group 的 `watch<T>` 返回 `ReadOnlyState<T>`，删除 `binding` / `binding_impl`；修改配置只走 `set/remove/clear`，写入磁盘仍须 `flush`。表单草稿由应用自己的 State 保存，提交事件显式调用 Preferences 命令，不能把 watch 改名为 State 后继续误示持久化双向绑定。

| 操作 | watch 身份、读数与通知 |
| :--- | :--- |
| 同键同类型重复 watch | 返回同一来源；首次创建时的 fallback 保持，不因再次 watch 改写 |
| 同键换类型 watch | 保持现有单缓存槽规则，创建新类型来源；旧读句柄安全保留最后值并脱离该键 |
| set | 更新配置内存，再向该键当前来源投递；同值按其比较规则去重；不自动落盘 |
| 本地 remove | 擦除键、登记墓碑并断开该键来源；旧读句柄保留最后值，不发伪造的“已失效值” |
| 本地 clear / Group::clear | 同样断开范围内全部 watch，包括只读过 fallback、尚未写入存储的键；组外来源不受影响 |
| 删除后重建同名键 | 新 watch 创建新来源；旧句柄不重新附着 |
| reload | 保留仍登记的来源，用磁盘值或缺键/类型不符时的 fallback 更新；文件不存在同样推 fallback |
| flush | 保留现有合并墓碑/清空纪元与写盘语义，不额外推送 watch；要观察合并后的磁盘结果须 reload |
| Preferences 析构 | 旧读句柄仍可读最后值，但不保活文件对象或继续获得更新 |

flush 后配置内存与旧 watch 可能暂时不同，这是保留的既有边界，不描述为完全实时镜像。本地 clear 对 fallback-only watch 的统一脱离是本次补齐的行为，不能仅按存储中存在的键枚举。远端删除经 reload 推 fallback 与本地删除断开来源是不同操作，测试分别覆盖。

set/reload 在锁内记录待投递值与来源版本，锁外发布，支持回调重入 get/set/remove。重入删除、换类型或再次更新后，旧待投递项须重新核对来源身份与版本，不得把较早快照覆盖较新值。发布仍遵守主线程状态契约；跨线程调用方须把会创建或推送 watch 的操作投递到主线程，文件/容器锁不代表状态回调线程安全。

**Provider。** `Provider<T>` 接受 `ReadOnlyState<T>` 注入当前 T，构造期常量与 State 可单向转换；T 为普通数据时，不授予修改原来源的权限。需要后代修改时显式使用 `Provider<State<T>>`，或者注入包含只读 ReadOnlyState 与领域命令的应用模型。环境按类型查找；取出 State 后内层值变化由该 State 的消费者订阅，不期待外层 Provider 自动递归观察嵌套句柄。

删除 Store 后，校验、审计和撤销责任交给应用领域命令与既有 `UndoStack`，不能只把 dispatch 全部机械改成 update 就声称纪律不变。UndoStack 的多命令组合也不自动成为一次原子提交。

**媒体。** `VideoController::playing_signal/progress_signal/volume_signal/muted_signal` 改为返回对应 `ReadOnlyState<T>` 值句柄，停止外借 Reactive/State 裸指针。控制继续使用 `toggle_play/seek_fraction/set_volume/set_muted`。UI 可以有本地编辑 State，真实输入事件发送命令，后端事实回填只更新展示/草稿而不再次触发用户事件，防止递归 seek。读句柄保活读数来源，不承诺单独保活已经销毁的播放器或音频后端。

滚动偏移与定时器 tick 读点同样返回 ReadOnlyState 值句柄；生产宿主释放后保留最后值，不把读投影描述为会自动延长滚动/定时任务寿命。Resource 的作业保活是其明确的额外契约。

### 身份、内省与资源上界

来源使用独立于句柄地址、基类子对象地址和注册表槽位的稳定身份；同一来源的复制/只读降级不新增来源节点。边的两端都引用真实节点身份，重复读取后的反向依赖仍完整。身份不跨新来源复用为同一活节点。

正常销毁、dispose、构造失败均注销相应运行记录并解绑；交换删除若用于注册表，须回填移动项的下标。注册表服务生命周期必须覆盖最后一次注销，不能产生静态销毁次序悬垂。长寿命且不再写入的来源也即时清除已取消消费者的边，不依赖下次 notify 扫墓。

`StateGraph` 保证来源—观察者图正确且有界；不宣称仅转发 anchor 就能导出“哪个 Widget 的哪个属性”。本批不增加控件属性标签或作用域标签 API。节点 id 取引擎分配的稳定身份，不用对象地址，也不挂回公共句柄；需要定位具体节点时用对象创建前后的节点集合差，不为此新开 `id()` 一类访问器。节点与边的 `kind` 按目标节点角色重新取值，旧字符串 `state` / `effect` 随对应公共类型下线。JSON 顶层新增 `schema` 版本字段，首个取值为 `1`；text 首行写成 `StateGraph schema=1:`，使消费者能区分“格式不符”与“图内容变化”。导出结构变化同批同步 Inspector 与快照测试。

### 实施范围与破坏性迁移

state 仍按头文件交付，内核模板放在 state 层；非模板的传播引擎单独成实现 TU，符合「非模板纯逻辑离头」。计划新增公共头 `read_only_state.h`（`ReadOnlyState<T>` 与 `StateSink`）；`state.h` 保留 `State<T>` 与单向转换实现，`computed.h` 保留工厂；擦除基类与来源节点放 `detail/`，不新增公共基类头；`state_registry.h` 一并移入 `detail/`，不留公共头，伞头与清单同批更新。删除旧头时同步唯一伞头与 `tools/check/umbrella_manifest.txt`，不留兼容别名或转发头。

| 旧入口或行为 | 目标与迁移 |
| :--- | :--- |
| `Reactive<T>` / `SignalView<T>` | 按使用者权限迁为 ReadOnlyState 或 State，外部只读点从引用改为值句柄 |
| `SignalViewBase` | 收进库内节点；采集覆写改走 `StateSink`，不保留公共擦除基类 |
| `Computed<T>` | 使用 `computed(fn)`；显式结果类型放到 lambda 返回类型；保留派生行为测试 |
| `Binding<T>` | 可编辑属性直接持 State，删除双份 value/binding 路径；展示属性不保留隐式写回 |
| `Immutable<T>` / `Mutable<T>` | 只读持 ReadOnlyState，需要写入持 State，不把 const State 作为权限隔离 |
| `Store<S>` / 状态模块 Action、Reducer、make_store | State 加领域命令；需要撤销显式包 UndoStack，作用域交 Provider |
| `shared()` / 为接属性而创建 `shared_ptr<State<T>>` | 按值传 State；共享指针仍可用于应用自己的对象所有权，但不再是属性接线前提 |
| 状态模块自由 `connect` | `source.subscribe(fn)`；统一首值立即交付，旧 Store 不交初值的消费者须重审初始化副作用 |
| `Effect{fn}` / 构造后 `run()` / 先造后延迟 run | 改为 `auto s = effect(fn)`；调用即首跑，需要延后则移动调用时机；两个布局构建器明确新增首跑 |
| 挂载后的公开响应式成员赋值 | 改走控件换源入口；值 setter 与 source setter 按职责区分 |
| Progress 的 Binding 构造与写回 | 展示只读 ReadOnlyState；要改上游显式调用原 State，不由进度条代写 |
| 原地 JSON 回填 | 可编辑模型保留来源并写值，展示属性换源；两者均不产生用户输入事件 |
| `Preferences::binding` / 可写 watch | watch 只读；保存/删除/落盘走领域命令，旧删除视图规则按本文明确 |
| VideoController 读数指针 | 只读值句柄；用户操作调用控制命令，事实通知不当作输入事件 |
| 回调中写入后立即读取 | 后续轮才提交；当前轮读数不变，需要新值的逻辑放到对应订阅或外部命令完成点 |
| 相同值 set / 复制句柄 / 右值交接 | 分别是去重提交、共享身份、保源复制；依赖旧重复通知的地方按语义选择 `notify()`（原地可变载荷）或 `always_publish`（创建来源时禁用去重） |

迁移扫描仅针对 Aurora 状态 API、活动示例与实现，不删除网络 `connect`、其他领域 Action 或 Animator 内部非模板 Binding。历史变更记录与迁移表可保留旧名；不得以全仓字符串零命中替代符号级验收。

消费方覆盖 widget 的全部响应式属性及采集覆写，form、动画、滚动、定时器、Provider、媒体控制、Preferences、序列化、测试控制器、debug/inspector，相关 tests 与 demos 同批迁移。非模板 `Animator::Binding` 不因名字相同而改名。

### 明确不做

- 不新增惰性求值、公共事务/batch、跨线程 State 写入或用户调度器；未来 eager/lazy 转换即使签名不变，也要重新审视计算、异常与持有时机。
- 不新增自动属性采集、作用域标签和控件属性级图溯源；保留显式采集并以测试约束。
- 不提供通用可写 Resource、Preferences watch 或媒体事实，不用运行期告警替代能力隔离。
- 不为减少名字把读写合并成带运行期权限位的单一类型；那不是减少概念，而是把类型检查换成记忆。
- 不保留独立的 `Effect` 公共类型：副作用凭证与订阅凭证是同一契约，取消只保留一个动词。
- 不重写另一套 Store，不提供 Action 字符串分派兼容层，不承诺 update 自动恢复 reducer 纪律。
- 不改变平台后端矩阵、不调整性能门槛、不以扩大豁免方式让迁移过门禁。

### 拟议 API 用法

以下是**实施后的目标示例**，不是当前代码可编译的证明；正向片段应转为可编译用例。同类型能力反例由静态断言独立验证。

```cpp
au::State<int> count{0};
auto label = au::computed([count]() -> au::LocalizedString {
    return au::LocalizedString{std::to_string(count.get())};
});
au::Text text{au::TextProps{.content = label}};
auto increment = [count] {
    count.update([](const int& old) { return old + 1; });
};

au::State<bool> enabled{false};
au::Checkbox editor{enabled};
au::ReadOnlyState<bool> derived_enabled = au::computed([enabled] { return !enabled.get(); });
static_assert(std::is_convertible_v<au::State<bool>, au::ReadOnlyState<bool>>);
static_assert(!std::is_constructible_v<au::State<bool>, au::ReadOnlyState<bool>>);
static_assert(!std::is_constructible_v<au::Checkbox, decltype(derived_enabled)>);
static_assert(!requires(au::ReadOnlyState<bool> s) { s.set(true); });
```

Provider 的读值与写权限显式区分；下例后代在挂载时取得回调，真实输入时才调用，不能在 layout/paint 中调用写操作。后代若展示该值，须把取得的 State 降级为 ReadOnlyState 并登记自己的属性订阅。

```cpp
auto make_scope(au::State<bool> enabled, au::Node child) -> au::Node {
    return au::Provider<au::State<bool>>{enabled, std::move(child)};
}

auto make_child_toggle(const au::BuildContext& ctx) -> std::function<void()> {
    auto enabled = au::env_of<au::State<bool>>(ctx);
    return [enabled] {
        enabled.update([](const bool& old) { return !old; });
    };
}
```

异步展示按值捕获控制句柄，或者先转换到只读快照句柄；两种方式均保活作业。下例 producer 由应用提供并返回 `Task<std::string>`。

```cpp
auto request = au::Resource<std::string>::from(producer);
au::ReadOnlyState<au::ResourceSnapshot<std::string>> snapshot = request;
auto message = au::computed([snapshot]() -> au::LocalizedString {
    const auto& current = snapshot.get();
    if (current.phase == au::ResourcePhase::Failed) {
        return au::LocalizedString{"Failed"};
    }
    if (current.value) {
        return au::LocalizedString{*current.value};
    }
    return au::LocalizedString{"Loading"};
});
au::Text status{au::TextProps{.content = message}};
request.reload();
```

比较策略的默认、定制和显式禁用均在创建来源时表达；同一个来源的全部句柄共享该策略。

```cpp
au::State<int> ordinary{0};
au::State<int> always{0, au::always_publish};
auto parity = au::computed([ordinary] { return ordinary.get() % 2; },
                          [](const int& left, const int& right) { return left == right; });
```

## 验收判据

### 类型、接线与值语义

1. **同类型能力隔离**：bool 载荷下 State 可隐式降级 ReadOnlyState，ReadOnlyState 与 computed 的结果不能构造可写属性；`ReadOnlyState<T>` 上不存在 `set/update/notify`（`requires` 负向断言），Resource 不具有通用 set，也不存在可修改的公共基类子对象。比较器、字面量构造、`StateSink` 均不能绕过这些负向判据。
2. **共享与重绑**：复制、右值交接后源与目的都能读取同一来源；一侧 set 为全部别名可见；句柄赋值只改该句柄，既有订阅固定原源。独立数据复制拥有不同身份；非默认可构造载荷不能得到假空默认句柄。
3. **派生直通**：computed 文案直接填展示属性，上游修改后相关控件失效与绘制计数正确；未读取该来源的同级不重算、不重绘。离屏像素验证显示，独立计数器验证没有额外工作，不能仅凭像素相同推断未重绘。
4. **等值换源**：A/B 初值相同，展示属性 A→B 后写 B 刷新、写 A 不刷新；卸载后均不通知，重挂只订 B。验证公开字段/props 基类不能绕过入口，Provider 后代缓存随换源更新。
5. **值写入与序列化**：双向 set_value 保持原 State 身份，set_value_source 换线；Progress set_value 不反写上游。新建 JSON 为私有源，原地 State 回填保留共享、ReadOnlyState 回填退旧换新；程序回填无用户事件，真实输入事件不丢不重。

### 传播、异常与资源回收

6. **稳定轮次**：菱形从 5 只到 10；读取原始值与多级派生的 effect 也不见混合态。动态分支新读入另一失效派生时仍得稳定值；旧分支边消失，重复读取后图与通知边一致。
7. **重入与交付顺序**：回调内两次 update 逐次执行且不丢增量，当前轮读取不提前变化；新增订阅首值一次且不重复当前通知，执行前取消跳过，自取消不悬垂。外层入口返回时后续轮排空；独立外层两次 set 不被合并。
8. **失败与恢复**：初次 callable 抛出后注册表与边恢复基线；更新期计算/比较/候选构造失败不发布半值；派生失败阻止其下游读取混合态。一个订阅回调失败不阻断独立消费者；循环求值/反馈有诊断且有限停止，后续外部合法更新可恢复。
9. **比较**：同值 set 不替换对象、不通知；显式空策略每次通知；不可比较载荷保守发布。args-only、localize-only 的 LocalizedString 更新可见，NaN 行为按契约，共享可变载荷经 notify 更新，等价派生结果阻止继续传播。
10. **effect 与固定订阅**：effect 调用首跑一次并返回可取消凭证；两个布局构建器无漏订、重复首跑或重复构建。订阅 A 的回调读取 B 不转成 A+B 订阅，也不污染外层 effect；来源句柄离开局部作用域后 Subscription 仍安全。
11. **有界与保活**：在长期静默来源上反复创建/取消订阅，注册表与观察边最终回到活对象基线；反复挂载/卸载不按历史次数累积。回调释放最后外壳仍安全完成；静态销毁、构造失败和交换注销无悬垂。所有图边端点存在，句柄复制不增加来源节点，节点 id 不复用为另一活节点；图 JSON 带 `schema` 版本且不含对象地址，旧 `kind` 字符串不再出现，Inspector 按 schema 判读。

### 异步、领域与迁移回归

12. **异步所有权**：临时 Resource 直接转 ReadOnlyState 后可从 Loading 落定；仅剩订阅或派生持有时作业仍存活；释放最后持有者取消，销毁任一非最后控件不取消。晚到旧代次不能覆盖新结果，包括通知内重入 reload 的情形。
13. **快照与流式**：初始/重发 Loading、Ready、Failed 字段符合表格，重发保留旧值有专门用例；每次有效落定仅发布一份快照。复跑 `codespec/specification/02-state.md` 异步判据中与本文一致的超时、去抖、单作业/producer、Append/Replace、帧内批次与字节累计测试；冲突注释按本文修正，不能“一字不变”带入。
14. **Preferences**：watch 在编译期不可写；set 先改配置内存、flush 才落盘。同键/类型、换类型、删后重建、本地 clear 的 fallback-only watch、组外不受影响、远端 reload 缺键与 flush 不通知分别验证。回调重入 get/set/remove 无死锁，旧待投递项不覆盖新来源或较新版本。
15. **领域权限**：Provider 普通数据不授予原源写权限；显式注入 State 的后代按真实输入写回且刷新正确。媒体后端事实回填不触发 seek/音量命令，真实输入恰发一次命令；Timer/Scroll/播放器宿主释放后读句柄保留最后值，不继续访问宿主。
16. **迁移覆盖**：按源文件维护 utest，套件名等于文件 stem；删除旧包装类的专属用例时把仍有意义的行为迁移到新归属，不删 computed 工厂行为测试。受影响 integration 与 demos 可编译运行；活动公共头、源码与自描述示例没有已撤销的 Aurora 状态 API，历史说明与无关同名符号不误删。

### 门禁与变异自证

实施验收须先重建库、测试、受影响 demos 与 `aurora_api_json`，构建启用最大可用并行线程，再运行全量 ctest；定向覆盖 Debug / Release 与 wasm 的状态、资源、订阅用例，不假定线程型异步路径等同于 wasm deferred 路径。性能门禁 `check_perf_gates` 复核句柄分配、右值交接、比较与传播开销，不放宽现有门槛掩盖退化。

必跑静态门禁：`check_api_schema_sync`、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`check_umbrella_header`、`check_core_layer_boundary`、`check_arch_module_map`、`check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format`、`check_change_proposals`、`check_version_consistency`、`check_no_cjk_literals`、`check_no_hardcoded_paths`、`check_test_registry`、`check_nolint_layout`、`format-check`；lint 覆盖实际修改文件。新诊断按错误目录生成与校验流程执行。

文档验收与实现验收同批判红：`GUIDELINE.md` 每条配方只出现一个公共状态类型；`02-state.md` 开头的选型段只使用日常面四词，内核不变量移入附录；教学面表格与本文逐字一致。日常面出现第五个公共名字而未在三档表登记，视为教学面退化。

| 临时变异 | 必须转红的判据 |
| :--- | :--- |
| 删除 computed 到展示属性所需来源接入能力 | 派生直通的正向编译/渲染用例 |
| 将双向值属性放宽为 ReadOnlyState | 同为 bool 的构造负向断言与用户输入写回用例 |
| 改回复制数据而非共享来源 | 别名可见性、身份与订阅用例 |
| 换源时遗漏退旧或订新 | 等值 A→B、卸载/重挂用例 |
| 恢复递归即刻交付或不剪旧分支 | 菱形稳定序列、动态依赖与图一致性用例 |
| 删除等值短路或仍忽略本地化参数 | 同值通知计数、args/localize 变更用例 |
| effect 改回不首跑 | 首值计数与挂载接线用例 |
| 注销或来源退边缺失 | 静默长寿命来源的边数/注册表增长用例 |
| 图导出退回地址 id、去掉 schema 版本或复用已销毁节点的 id | 节点身份、schema 判读与 id 复用拒绝用例 |
| Resource 投影只保活快照 | 临时资源转换后仍须落定的所有权用例 |
| Resource 公有继承 ReadOnlyState 或新增通用 set | 类型结构与能力负向断言，不拿代次用例冒充权限证明 |
| 媒体回填走用户编辑事件 | 后端事实更新时命令计数必须为零的用例 |

变异只在临时验证副本实施，逐项恢复，记录确实失败的测试；不能用“不存在某诊断字符串”或未受影响的代次测试证明类型隔离。

### 提案时证据与未验证边界

2026-10-28 提案阶段已通过当时的源码与一次性 C++20 探针核实：MinGW GCC 16.1.0 下，LocalizedString 的 args/localize 差异仍被旧比较判等；旧传播交付 `5 → 7 → 10`；切换条件后旧分支继续触发；栈 State 调 shared 抛 `std::bad_weak_ptr`。这些是**现状反例证据**，不是目标模型已通过验收。

提案交付时没有编译本节拟议 API，没有完成实现、全量 ctest、性能或跨平台验收。以上边界已由同批次实施逐条关闭，取证见下一节「验收证据」。

## 验收证据

实施批次（2026-10-28 至 2026-10-29，本机 MinGW GCC 16.1.0 / Ninja / ccache，构建一律启用最大并行）取得下列证据。口径写在各行之上，未取证的写「未取证」。

| 判据 | 取证载体 | 结果 |
| :--- | :--- | :--- |
| 1 同类型能力隔离 | `utest_state_resource` / `utest_checkbox` 的 `requires` 负向断言与 `static_assert` 组 | 通过；变异行 11 证明该判据可转红（见下表） |
| 2 共享与重绑 | `utest_state.state_copy_shares_source_and_assignment_rebinds` 及同族句柄用例 | 通过 |
| 3 派生直通 | `utest_computed` 的接线用例（`PaintProbe` 以派生结果构造）+ 离屏像素与独立计数器双判据 | 通过；变异行 1 使其编译期转红 |
| 4 等值换源 | `utest_state_graph` 与 widget 换源用例（A→B 写侧刷新、卸载不通知、重挂只订新） | 通过 |
| 5 值写入与序列化 | `utest_checkbox` / `utest_slider` / `utest_progress` / `utest_text_input` 的 `set_value` 与 `deserialize_props` 用例 | 通过；变异行 2b 使 4 例转红 |
| 6 稳定轮次 | `utest_reactor`（菱形只到 10、动态分支剪旧边、图与通知边一致） | 通过 |
| 7 重入与交付顺序 | `utest_effect` / `utest_subscription` 的回调内逐次写入、首值一次、执行前取消跳过、外层排空 | 通过 |
| 8 失败与恢复 | `utest_reactor` 异常路径与 `utest_computed` 求值重入诊断 | 通过 |
| 9 比较 | `utest_state` 等值去重、`always_publish` 每次通知、`LocalizedString` 的 args/localize 差异可见、NaN 与共享可变载荷按契约 | 通过 |
| 10 effect 与固定订阅 | `utest_effect.effect_runs_immediately_and_tracks_dependencies`、`utest_layout_builder` 首跑与重建、订阅读 B 不污染 A | 通过 |
| 11 有界与保活 | `utest_subscription` 反复挂卸后注册表与边回到基线、`utest_state_graph` 节点身份/id 不复用与 `schema` 版本 | 通过 |
| 12 异步所有权 | `utest_state_resource`（含新增 `read_only_projection_keeps_job_alive_until_landing`）与 wasm deferred 腿 | 通过；变异行 10 使新增用例转红 |
| 13 快照与流式 | `utest_state_resource` 的 Loading/Ready/Failed 字段、重发保留旧值、单份快照；`02-state.md` 异步判据中与本文一致者已复跑 | 通过 |
| 14 Preferences | `utest_preferences` 的 watch 编译期只读、set/flush 分离、换类型/删键/clear/reload 与回调重入 | 通过 |
| 15 领域权限 | `utest_video_controls.backend_facts_do_not_fire_user_commands`（回填命令计数为零）、`utest_navigator_host` / Provider 注入写回、宿主释放后读最后值 | 通过；变异行 12 使该用例转红 |
| 16 迁移覆盖 | 六套旧用例（`utest_reactive` / `utest_signal_view` / `utest_binding` / `utest_immutable` / `utest_store` / `utest_state_registry`）删除后行为迁到 `utest_reactor` 等新归属；`compute --list` 套件名=文件 stem 由 `check_test_registry` 把守；demos 全量构建通过；活动公共头与示例无已撤销 API | 通过 |

构建与门禁矩阵：Release 全量构建（库 + `tools/` + 测试 + demos）通过；`ctest --preset ninja-test` **448/448** 通过（含全部 `check_*` 与 `format-check`，在最终源码状态上复跑）；Debug（`build-debug/`）定向跑状态域 7 套件 **68 例**通过；wasm（`build-wasm/`，emsdk + node）重建后同 7 套件 **7/7** 通过，线程型异步与 deferred 两条腿分别取证。lint：`run_clang_tidy.py --fail-on warning` 覆盖 89 个变更 TU，**0 findings**（首轮 20 条 blocking 以改名、补特殊成员函数、`std::forward`、成员初始化列表真修；仅 `always_publish` 的命名与三处测试判据用具名豁免并就地写理由）。性能：计数类 G-5~G-8 由 `utest_scroll.scroll_regression_counter_gates` 在全量 ctest 内实断言、阈值仍取自 `tools/check/perf_gates.json` 未放宽；时间类在安静机器单跑 `check_perf_gates.ps1`（best-of 3 进程）得 p99 4.98 ms、worst 6.20 ms、jitter 0.94 ms、长任务 0、CJK 12 行 0.86 ms、线性渐变 3.11 ms、径向渐变 3.41 ms，全部达标。

变异自证按下表逐行在临时副本实施、逐项还原，还原后与母本 `cmp` 逐字节相同。转红记录只写**实际观察到的失败**，未失败的连带发现一并写下，不用相邻用例冒充。

| 临时变异 | 实际转红 |
| :--- | :--- |
| 删除 computed 到展示属性所需来源接入能力 | `utest_computed` 编译期转红：探针控件无法以派生结果构造（正向接线即判据） |
| 将双向值属性放宽为 ReadOnlyState | `utest_checkbox` 的「可编辑值属性不得接受只读句柄」`static_assert` 转红，并连带 `State<bool>` 构造不匹配使 `gen_api`、`aurora_cli` 等 TU 编译失败 |
| `Checkbox::set_value` 摘掉对上游的写入 | 4 例转红，含 `utest_checkbox.checkbox_shared_source_writes_through_to_upstream` |
| 改回复制数据而非共享来源 | `utest_state` 句柄共享组转红（别名可见性、身份与订阅三条判据同时失守） |
| 换源时遗漏退旧或订新 | `utest_state_graph` 与 widget 换源用例转红（等值 A→B 后写旧源仍刷新 / 卸载后仍通知） |
| 恢复递归即刻交付或不剪旧分支 | `utest_reactor.reactor_dynamic_branch_prunes_old_dependency` 转红。**同族的 `utest_computed.computed_conditional_reads_replace_dependencies` 实测不转红**——该用例只统计回调运行次数、不统计求值次数，不足以证明依赖剪枝，故本行证据只认前者 |
| 删除等值短路或仍忽略本地化参数 | `utest_state` 等值去重（同值不通知）与 `LocalizedString` 的 args/localize 变更用例转红 |
| effect 改回不首跑 | `utest_effect.effect_runs_immediately_and_tracks_dependencies` 转红（回调零次） |
| 注销或来源退边缺失 | `utest_subscription` 的「反复挂卸后注册表与观察边回到基线」与 `utest_state_graph` 的边端点存在性用例转红 |
| 图导出退回地址 id、去掉 schema 版本或复用已销毁节点的 id | `utest_state_graph` 的节点身份、`schema` 判读与 id 复用拒绝用例转红 |
| Resource 投影只保活快照 | 新增所有权用例 `utest_state_resource.read_only_projection_keeps_job_alive_until_landing` 转红（临时资源句柄析构后作业不落定） |
| Resource 公有继承 ReadOnlyState 或新增通用 set | `utest_state_resource` 的负向能力断言编译期转红（`HasSet` / `HasNotify` / `is_base_of` / `is_convertible` 四项），不用代次用例冒充权限证明 |
| 媒体回填走用户编辑事件 | `utest_video_controls.backend_facts_do_not_fire_user_commands` 转红（后端事实更新时 seek/set_volume 命令计数非零） |

「恢复递归即刻交付」一行另附**提案阶段的反例证据**：旧内核的交付序列为 `5 → 7 → 10`（混合态可见），切换条件分支后旧分支继续触发。该行无法做成单行变异（惰性重算与依赖剪枝是同一处实现的两面，摘掉任一面即退回旧内核整体形态），故以反例 + 上表 `reactor_dynamic_branch_prunes_old_dependency` 的转红共同支撑。

未取证项：**PROFILING=ON 下的帧内预算实测（G-9~G-11）未在本批次采集**——该三项按 `check_perf_gates.ps1` 口径属信息项、不计入判定，且已知 paint 基线超出 10 ms 预算；如需读数，另配 PROFILING 构建单跑 `-ProfilingBuild`。除此之外本节日判据均已取证。

## 回写落点

以下为实施同批回写清单；提案状态为「已提议」期间不把权威文档改写成代码已经具备这些能力。

- `codespec/specification/02-state.md`：重写能力选型、共享/保源复制、借用、比较策略、固定订阅与首跑、轮次传播/重入/异常、依赖剪枝、资源快照与生命周期、图身份；开头只教日常面四词，内核不变量移入附录；撤销旧包装与 Store 条目，旧章节编号保留迁移去向，避免伪死链；`Effect` 类下线，副作用由 `effect(fn)` 返回 `Subscription`。
- `codespec/CONCEPTS.md`：选型问题改为“消费方是否应得到直接写权限”；区分普通数据、共享 State 与领域命令注入；重写生命周期和跨框架映射，不再把只读解释为永不变化、把 Store 解释为全局状态。
- `codespec/ARCHITECTURE.md`：模块清单、状态所有权、不变量、同步稳定轮次、重入队列、主线程边界、注册表/边规模、图导出的稳定身份与 schema 版本、资源取消条件与验收矩阵；明确 ReadOnlyState/State 组合而非继承。
- `codespec/CODING_STANDARDS.md`：句柄 const 与复制语义、按值捕获、构造期回滚、响应式属性可见性调整依据；新增属性必须采集，并测试上游变化与等值换源的纪律；更新对应特殊成员豁免理由。
- `codespec/SPECIFICATIONS.md`：状态需求 `SPEC.FEAT.STATE.SIGNAL-STATE.001`、模块职责与文档地图，列明破坏性迁移和代码/规格同步要求。
- `codespec/GUIDELINE.md`：每条配方只出现一个公共状态类型；派生值直接接展示属性、同类型只读/可写反例、显式 Provider 写权限、领域提交、固定来源订阅、异步只读投影与保活的可编译配方。
- `codespec/specification/04-widget.md`：按属性的权限表、运行期换源与可见性、`StateSink` 采集协议、值 setter/source setter、Progress 写回移除、程序赋值与用户事件、新建/原地 JSON 两种语义。
- `codespec/specification/03-layout-render.md`：布局构建器首跑、滚动读点、媒体只读事实与控制命令、回填不重发命令的约束。
- `codespec/specification/06-app-platform.md`：Preferences watch 的只读所有权、fallback/删键/clear/换类型/重建表、flush 与 reload 差异、锁外且主线程投递的可重入规则。
- `codespec/specification/07-environment-modifier.md`：Provider 普通数据与显式 State 注入、子树换源失效、ReadOnlyState 样式与本地化可观察等价。
- `codespec/manual-test/07-state.md`：按该文件下一个可用编号补充派生直通、换源、领域编辑、异步保活、图规模走查；记录真实执行结果，不预填通过。

实施同时同步公共头注释、伞头/manifest、测试与 demos、Inspector 消费点；重建根 API 数据文件，不手改生成物。有新诊断时同步错误定义及生成目录。仓库根变更记录的 Unreleased 节加入 breaking 迁移表，版本调整遵循既有发布规则，本提案不自行决定升版。

实施开始后状态改为「实施中」，全部契约与门禁取得证据、回写完成后改为「已归档」。不得用文档检查通过代替实现验收。
