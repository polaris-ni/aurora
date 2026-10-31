# CHG-048 动画引擎能力补齐：循环播放、时间缩放、交互式进度、弹簧封装与关键帧分段曲线

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-048 |
| 提出日期 | 2026-10-29 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.API.EXPLICIT-FIRST.001 |
| 影响面 | 动画公共头 `include/aurora/animation/animator.h`（`AnimationController` / `Animator` / `AnimatedValue`）、`include/aurora/animation/spring.h`（新增 `SpringAnimation`）、`include/aurora/animation/timeline.h`（`Keyframes<T>::Stop`）；`easing.h` 的 `Curve` 仅被引用、本提案不改动它。实现 `src/aurora/animation/animator.cpp`。因「真全局缩放」与「弹簧去重」两项裁决，波及面扩到驱动层消费点：`include/aurora/event/gesture.h`（`DragToDismiss` 重构为复用 `SpringAnimation`）、`include/aurora/widget/dismissible.h`（消费点，接口不变）、`include/aurora/widget/scroll_viewport.h`（缩放下沉进 `ScrollGlide::tick` 还是留在各消费点的 dt 取值——二者只可选其一，双层乘会重复缩放，实施时须就地定案）与 `include/aurora/widget/scroll.h` / `lazy_list.h` / `grid_view.h` / `pull_to_refresh.h` 的自驱动收位路径、`include/aurora/widget/reorderable_list.h` 的 `tick_settle` 弹簧 dt（其私有弹簧实现本提案不收口，见「关联限制与后续」）、`src/aurora/app/application.cpp`（空闲帧决策与缩放为 0 时的停手语义）、视冻结重挂形态而定 `include/aurora/widget/widget.h`（`needs_gesture_tick_` 是逐控件字段，全局 `set_time_scale` 若要重挂需一处树遍历入口）。受影响测试与 demo 与上述同源；规格回写见末节（`05-event-navigation.md` §6.1–§6.7 与 §7.1、ARCHITECTURE、CONCEPTS、GUIDELINE、SPECIFICATIONS、manual-test/08-animation）。本提案不改动导航层转场接线（逐路由 curve / pop 类型 / `TransitionKind` 扩展）。 |

本提案规定目标契约：在现有显式动画内核（`AnimationController` + `Animator` + `Tween`/`Keyframes` + `SpringSimulation`）上补齐五类业界常见、当前引擎层缺失的原语，并把已存在但私有、且以 `State` 为落点的那份弹簧驱动（`DragToDismiss`）收口到新原语上。动机与现状证据来自对源码与 `codespec/` 文档的交叉核对；本提案自包含，不依赖未随仓库分发的评审材料。状态按「已提议 → 实施中 → 已归档」流转，全量门禁与验收判据取得证据后归档。

## 动机

对照 Flutter / Qt / SwiftUI / WPF 的动画体系，Aurora 的显式动画内核已具备：`AnimationController`（时间线，输出原始进度）、`Animator`（帧循环 + 绑定到 `State<T>`）、`Tween`/`Keyframes`（补间与关键帧）、`SpringSimulation`（纯弹簧求解器）、`TimelinePlayer`（多轨编排）、`AnimatedValue`/`TweenAnimation`（便捷封装），以及导航层的 `Hero` + `TransitionLayer` + `NavigatorHost` 转场链路。交叉核对确认下列结论：

- **「无隐式动画」不是缺口**：`codespec/specification/05-event-navigation.md` §8.1（需求 `SPEC.API.EXPLICIT-FIRST.001` 的规格落点）把「显式优于隐式」列为正式设计原则，属性赋值即补间是刻意不做，不在本提案范围。
- **「无页面/共享元素转场」是过时断言**：`Hero`、`TransitionLayer`、`NavigatorHost` 已实现并接入帧循环，链路完整。该断言（曾出现在缺口盘点里）与代码不符，本提案不为其开列变更项。
- **「无路径/形变动画」是合理的 non-goal**：与纯软件栅格能力一致，`Hero` 的矩形 lerp + 交叉淡变已覆盖共享元素切片，任意 `PathMotion` 不在计划内。
- **「完全没有弹簧到状态的封装」是错的，须改口径**：`include/aurora/event/gesture.h` 的 `DragToDismiss` 已经在事件层实现了「累积模拟时间 → `SpringSimulation::value` 写 `State<double> progress_` → `is_settled` 定格终点」这一整套驱动层，只是它是私有的、走 `Widget::tick_gestures` 自驱、且不在 `Animator` 登记面上。所以本提案的弹簧项不是「从无到有」，而是**把已存在的手写样板上提为公共原语，并让 `DragToDismiss` 复用它**——否则新增 `SpringAnimation` 会让仓库里同构的弹簧驱动从两份变成三份。第二处私有弹簧在 `ReorderableList`，它落位写的不是 `State` 而是普通成员，收口它需要另一项决策，故本提案只清点、不收口（见「关联限制与后续」）。

在剔除上述三项过时断言后，下列五类能力构成引擎层当前的真实缺口（弹簧项按上条修正后的口径），即本提案的变更内容。

### 循环 / 重复 / 往复播放缺位

`AnimationController` 仅提供 `forward` / `reverse` / `reset` / `stop` / `tick`，到达端点即钳制并置对应终态（`Completed` / `Dismissed`），没有 `repeat` / `loop` / `alternate` 计数（实测：动画域对 `repeat` / `loop_count` / `alternate` / `set_value` 零命中）。需要循环播放（加载指示、呼吸态、背景动效）时，调用方只能自行在帧回调里翻转方向——现证即 `examples/demos/demo_animation.cpp`：其背景色呼吸与来回缩放是**在帧回调里手动到端点反向**实现的，这正是本项要消除的样板。该 workaround 与 `Animator::bind` 的写回时机耦合，调用方还须自行保证控制器在端点被重新起播，无有限计数、无「本循环不触发 completed」的语义承诺。这与 Flutter `AnimationController.repeat(reverse:)`、Qt `loopCount` 直接冲突。

### 应用内全局时间缩放缺位

帧步长有**两条入口**，本提案的核对必须把两条都算进来：`Animator::tick(dt)` 直接消费帧步长推进全部登记控制器；控件自驱动路径（`ScrollGlide::tick(dt)` 服务于 `Scroll` / `LazyList` / `GridView` / `PullToRefresh` 的收位滑动，`DragToDismiss::tick(dt)` 服务于松手弹簧，`ReorderableList` 的 `tick_settle(dt)` 服务于落位弹簧，`AnimatedValue::tick` / `TimelinePlayer::tick` / `TweenAnimation::tick` 为无 `Animator` 时的手动自驱）各自从 `steady_clock` 取 dt，不经 `Animator`。因此「逐控制器改 `duration`」不是唯一堵点，**真正的堵点是 dt 口径没有单一漏斗**：只在 `Animator` 上挂一个倍率，控件自驱动的动画与 Dismissible 弹簧仍然照常全速跑，调试慢放会呈现「一半慢一半快」的错误画面。实测 `include/aurora/` 与 `src/aurora/` 对 `time_scale` / `dilation` 零命中。Flutter `timeDilation`、QML 动画调试器给的都是单一漏斗语义。

### 手势驱动进度的干净 setter 缺位

`AnimationController` 对外只暴露只读的 `value()`；唯一能写入进度的 `reset(double v)` 会按值改写 `status_`（≥1 → `Completed`，否则 `Dismissed`），即「静止复位并改状态语义」。这意味着手势拖拽（抽屉、滑删、可中断页面转场）无法直接把 `dx / width` 这类拖拽进度灌入控制器而不触发状态翻转。§6.7 现状是绕开控制器：`DragToDismiss` 自己维护一个 `progress_` State 并逐事件 1:1 写入。这条私有路径可用，但**引擎层没有与控制器/补间/`Animator::bind` 写回链路对接的进度入口**，于是「拖拽跟手 → 松手由 `forward`/`reverse` 接管收尾」这一惯用法在 `AnimationController` 面上没有等价物，是交互式转场缺失的先决堵点。

### 弹簧驱动层的公共原语缺位（并存在同构私有实现）

`spring.h` 仅有 `SpringDescription` + `SpringSimulation`（含 `is_settled` 收敛判定）这一纯求解器，而驱动样板已在两处各自手写：`DragToDismiss`（写 `State<double>`，本提案收口）与 `ReorderableList` 的落位弹簧（写普通成员，本提案不收口，见「关联限制与后续」）。调用方若要用弹簧驱动一个值，仍须自行每帧 `SpringSimulation::value(t)` 写 `State`、并在 `is_settled` 后手动停帧——重复且易漏收敛判定；漏掉时表现为「定格前多写若干帧」或「永不停帧的空转」。对照 iOS `UISpringTimingParameters` 的即插封装与 Flutter `SpringSimulation` 之上的驱动层，Aurora 缺的是**公共**这一层。

### 关键帧段间分段曲线缺位

`Keyframes<T>::Stop` 仅含 `time` 与 `value`，`value(t)` 在相邻 Stop 间做线性插值，段间曲线固定为线性（`codespec/specification/05-event-navigation.md` §6.1 现文明写「插值严格线性」）。虽然 `TimelinePlayer::track` 每段 `Tween<T>` 自带 `Curve`、多段带曲线编排可走 TimelinePlayer，但单层 `Keyframes` 路径无法表达「每段不同曲线」。Flutter `TweenSequence` 每段带 `curve`，对应能力在 `Keyframes` 单层缺失。

### 复审修订（2026-10-30）

本版相对首稿的实质变更，均源于对源码的复核推翻首稿结论：

1. 推翻「完全没有弹簧到状态的封装」——`DragToDismiss` 已有同构私有实现；据裁决**本提案同时重构 `DragToDismiss` 复用新原语**，不留第二份实现。
2. 推翻「`set_progress` 置 `dirty_` 即可让 `Animator::bind` 当帧写回」——`AnimationController::tick` 入口第一句即 `dirty_ = false` 且静止态立刻早退，手势先写进度再进帧循环时脏位被抹掉，首稿判据按现契约必然落空。改为**外部脏位 + `dirty()` 取或**机制，并加「既有 stop 用例继续绿」的负向判据。
3. 推翻「缩放只落在 `Animator` 即等价于 Flutter `timeDilation`」——控件自驱动路径不经 `Animator`。据裁决**升级为真全局缩放**：以单一 dt 漏斗收口全部动画推进点，并补齐「缩放为 0 时自驱动路径不得持续自标脏」的空闲纪律。
4. 推翻「`SpringAnimation` 可直接 `attach` 到现有 `Animator`」——`add_binding` 登记的回调 `owner` 恒为空、`remove()` 摘不掉，且回调形参不含 dt，弹簧取不到帧步长；不登记 `AnimationController` 时 `has_active()` 为假，`Application::wait_once` 会直接睡住，弹簧半途冻结。据裁决**新增可注销、带 dt 的绑定入口**。
5. 修正过时断言与「表中该行」这类悬空指称，改为直接点名被推翻的断言本身。
6. 修正拟议用法片段里三处不可编译写法（`AnimationController{0.3}` 的第一参数是时长不是进度；`Keyframes<double>` 无 initializer-list 构造；片段引用了未声明的 `ctrl`），并把「关键帧段曲线归属哪一段」这一未定义语义补成契约。
7. 回写落点纠偏：测试面改为扩展既有 `utest_animator.cpp` / `utest_spring.cpp` / `utest_timeline.cpp`（首稿新增的三个测试文件名违背源—测试 1:1）；导航层已知限制的落点从 §7.4 移到 §7.1；按 `AGENTS.md` §5 规则 14 删除「在权威文档里引用提案编号」的指令；门禁清单补 `check_api_budget` / `check_doc_comments` 等；补 `aurora_api.json` 与 `known_enums.h` 的实际影响说明。
8. 自驱动消费点归类纠偏（本轮二次核验）：`ReorderableList` **不是** `ScrollGlide` 消费者——`ScrollGlide` 消费点实测只有四处（`Scroll` / `LazyList` / `GridView` / `PullToRefresh`），`ReorderableList` 自持第三处私有弹簧（`spring_active_` / `settle_t_` / `tick_settle`）。首版把它并入 glide 清单，会让「重复实现归零」的判据落在错误的文件上，也会让该控件被误当作本提案收口对象。据此把「归零」拆成两处（`gesture.h` 收口 / `reorderable_list.h` 明确不收口并给阻塞点），并就地裁决自动滚边（`DragAutoScroll` / `tick_auto_scroll`）为缩放例外——它是速度型手感，无进度与终态。
9. 冻结纪律补第二条停手机制：首版只盯 `Animator` 侧脏位与 `has_active()`，漏掉控件侧 `tick_gestures` 的每帧计时在途标志；「dt 归零但照常进帧」会让在途滑动永不抵达终点、计时永不摘除，冻结退化成满帧唤醒。判据 11 据此拆成 (a)/(b) 两项、变异表加对应行，并把「恢复缩放时重新武装计时」写成实施契约。

## 变更内容

本提案新增引擎原语并把 `DragToDismiss` 的弹簧驱动收口到该原语；除下述明示项外，既有 `forward` / `reverse` / `reset` / `stop` / `tick`、`Animator::bind` / `drive` / `remove` 与 §6.7 的对外语义保持不变。

### 循环 / 重复 / 往复播放

`AnimationController` 新增播放模式与循环参数：

- 新增 `enum class PlaybackMode { Once, Repeat }`，默认 `Once`。
- 新增 `repeat(int count = -1, bool alternate = false) -> void`：置 `Repeat` 模式；`count <= 0` 表示无限循环（`0` 与 `-1` 同义，须在注释里写明，避免被读成「播 0 次」），`count > 0` 表示有限次数（到达端点 `count` 次后停止）；`alternate` 为真时每次到达端点翻转方向（往复），否则保持方向重启。
- **方向重启的落点必须定义**（首稿未定义，实施时会被自由发挥）：`alternate` 为真时到达 `1.0` 翻为 `Reverse`、到达 `0.0` 翻为 `Forward`，进度停在被到达的端点、下一帧从该端点反向续跑；`alternate` 为假时同向重启，即 `Forward` 到达 `1.0` 后把进度重置为 `0.0` 续跑、`Reverse` 到达 `0.0` 后重置为 `1.0`。两种形态在重启帧都置脏（进度相对上一帧有变化时）。
- `tick` 到端点时的新增分支：处于 `Repeat` 模式且未达终止条件（无限，或有限且剩余次数 > 0）时，不钳制为永久 `Completed`/`Dismissed`，而按上条规则续播；有限计数在每次到达端点时递减，归零后在最后一次到达的端点置终态并停止。
- `reset` / `stop` 清除 `Repeat` 模式、回到 `Once`，其余行为与现状逐位一致（不引入回归）。`forward` / `reverse` 只定方向与起播，**不清除** `Repeat` 模式（与 `repeat` 正交）。
- **`reduce_motion` 优先于循环**：`AnimationController::tick` 现有短路（直接落本次播放的目标端点并置终态）先于 `Repeat` 分支生效——即无障碍模式下不进入循环，一次落端点即止。该取舍必须写进注释与 §6.1/§6.5，否则「呼吸态在无障碍模式下静默停住」会被当成缺陷报回。
- **无限循环使 `has_active()` 恒真**：`Application::wait_once` 据 `Animator::has_active()` 决定能否深睡，`repeat(-1)` 意味着该应用永不进入空闲等待。这是能力本身的代价，必须在 `repeat` 的注释与 §6.1 明示「无限循环 = 常驻活跃帧」，并给出退出方式（`stop()` / `reset()` 即清模式）。
- **completed 语义**：`AnimationController` 本身没有 completed 回调（回调在 `AnimatedValue` / `TimelinePlayer` 层，且为一次性 `fired_completed` 守卫）。本提案定义：`Repeat` 循环期间不触发 `on_completed`，仅有限计数耗尽置终态那一帧触发一次。
- 本提案**不给 `AnimatedValue` / `TimelinePlayer` 加 `repeat` 转发**（API 面收敛）：调用方经既有的 `AnimatedValue::controller()` 可变引用设置模式。

### 应用内全局时间缩放

裁决为**真全局**：缩放作用于「动画时间轴」这一条 dt 口径，覆盖全部推进路径，而不是只覆盖 `Animator`。

- `Animator` 新增 `set_time_scale(double scale) -> void` 与 `time_scale() const -> double`。`scale` 为速度倍率：`1.0` 正常，`2.0` 两倍速，`0.0` 冻结，`< 0` 与非有限值（NaN / inf）一律夹取为 `0.0`；默认 `1.0`。倍率存放在 `Animator`，`Animator::tick` 保持 `const`（只读该成员，不改 `Animator` 自身状态）。
- **单一漏斗**：新增 `auto scaled_frame_dt(double raw_seconds) -> double`（自由函数，落 `include/aurora/animation/animator.h`），按 `Animator::current()` 的倍率返回缩放后的帧步长；`Animator::current()` 为空（无头渲染 / golden / 未跑 `Application`）时**原样返回入参**，以保持既有 golden 的确定性。内部推进点一律改走该漏斗，不得再直接吃裸 dt：
  - `Animator::tick`（全部登记控制器，含经新绑定入口登记的弹簧）；
  - `AnimatedValue::tick` / `TimelinePlayer::tick` / `TweenAnimation::tick`（无 `Animator` 时的手动自驱）；
  - `ScrollGlide::tick` 的全部消费点（实测四处，均在控件自有帧计时里以墙钟差作 dt）：`Scroll`、`LazyList`、`GridView`、`PullToRefresh` 的收位/回弹短滑动；
  - `ReorderableList::tick_settle`（该控件**不消费** `ScrollGlide`，而是自持 `SpringSimulation` + 累积时间做落位弹簧，见「关联限制与后续」）；
  - `DragToDismiss::tick`（重构后经 `SpringAnimation` 走同一漏斗）。
- **不纳入缩放的例外（就地裁决）**：`DragAutoScroll::advance` 与 `ReorderableList::tick_auto_scroll`（近边缘自动滚边）**不算动画时间轴**——滚边速度由「落点距边缘的远近」实时决定，是拖拽手感而非一段有始有终的时序；缩放它会让画面滚动与手的位置脱钩，且它没有「进度 / 终态」可供加速或冻结。判据：缩放倍率下自动滚边的每帧增量不变。
- **冻结时的空闲纪律（两条停手机制都要管）**：`scale == 0.0` 时 `Animator` 侧登记控制器不推进，且**不得继续自标脏**；控件自驱侧还有一条独立的停手机制——`Widget::tick_gestures` 家族靠「在途」标志维持每帧计时（实测 `Scroll::tick_gestures` 即 `needs_gesture_tick_` + `glide_last_`：`glide_.active` 为真就每帧重挂，抵达终点才摘）。若冻结只是「进帧但 dt 恒 0」，滑动/弹簧永不抵达终点，该计时永不摘除，表现为**停着不动却满帧唤醒**——`Animator::has_active()` 之外第二条破坏「完全空闲 → 无限等待」不变量的路径。裁决：冻结路径必须在 dt 归零的同一帧摘除控件侧每帧计时，恢复缩放（`0 → > 0`）时由 `set_time_scale` 重新武装；若公共 API 无法触达各控件的私有在途标志，则改为「dt==0 的帧不重挂、下一次 `set_time_scale` 显式重挂」的实现形态并在 ARCHITECTURE §5.2（事件驱动帧循环）就近记录取舍。约束须一并写明：`needs_gesture_tick_` 是 `Widget` 上的逐控件字段（各控件在自己的 `tick_gestures` 里置真/摘除），`set_time_scale` 是全局入口、拿不到控件指针，因此「重挂」要么走一次全树遍历（成本与「冻结/解冻」这一调试动作的频率相称，可接受），要么由 `Animator` 广播一条「帧计时重新武装」的既有重绘路径——两种形态都不得把缩放做成每帧全树扫描。两条形态都须由判据 11 证伪，不接受「冻结就是满帧跑但不动」。
- **`reduce_motion` 优先于缩放**：短路落端点的路径不乘倍率（无障碍语义是「一步到位」，缩放不得把它变成「慢速播放」）。
- 缩放只影响动画时间轴：不改 `duration`、不改目标 `State` 终值、不改曲线，也不影响事件、布局、输入与 `Scheduler` 定时任务。
- 该入口定位为调试 / 演示 / 降速用途，不进入业务动画契约；文档标注其「非业务 API」性质，并列出**不覆盖**的例外（无 `Application` 在跑时的手动 `tick`：此时 `Animator::current()` 为空、按恒等返回）。
- 命名与等价关系：本提案的 `time_scale` 是 Flutter `timeDilation` 的倒数语义（`timeDilation = 2` 慢放两倍 ⇔ `time_scale = 0.5`）。文档写清映射，不混淆两套命名；措辞统一为「应用内全局时间缩放」，不再用无「应用内」限定的「全局」，以免被读成进程级 static。

### 交互式进度 setter

`AnimationController` 新增 `set_progress(double t) -> void`：

- 把 `t` 夹取进入 `[0, 1]`，写入 `value_`，**保留 `status_` 与 `PlaybackMode` 不变**——不重写为 `Dismissed` / `Completed`，也不改变播放/静止语义。
- **写回机制（本版新增的硬约束）**：`AnimationController` 的脏位拆成两位——推进脏 `dirty_` 与外部脏位（如 `external_dirty_`）。`set_progress` 只置外部位；`tick()` 入口**只清推进脏**（现状是第一句清掉唯一脏位，会吞掉外部写入）；`clear_dirty()` 同时清两位；`dirty()` 返回两者之或。该拆分是必要的：既有单测 `utest_animator.cpp` 的 `controller_reverse_reset_stop_semantics` 依赖「静止控制器 `tick` 后 `dirty()` 为假」，直接把入口清脏删掉会让该用例转红并改掉既有语义，故必须在 `Animator` 帧末统一 `clear_dirty()` 的前提下引入外部位。
- 与 `reset` 明确区分：`reset(v)` 静止复位并改写 `status_`；`set_progress(t)` 仅定位、不改「是否在播放」。
- 播放中（`Forward`/`Reverse`）调用 = 重定位后从新进度续跑；静止态调用 = 只定位，不会使 `has_active()` 变真。因此调用次序契约是「手势事件在帧边界前写入，同一帧 `Animator::tick` 的绑定阶段经补间/关键帧写回目标 `State`」，`State` 写入自带定点刷新，无需额外起播。
- 保留一处既有的不对称并写明：构造函数的初值**不夹取**（越界原样返回，见 `AnimationController` 注释），`set_progress` **夹取**。本提案不改构造语义。

这是交互式转场（抽屉 / 滑删 / 可中断页面转场）的先决能力：导航层可在手势 delta 直接驱动转场进度，而无需经 `reset` 的状态副作用或绕过控制器直写 `State`。

### 弹簧到状态的即插封装，并收口 DragToDismiss 的重复实现

新增 `SpringAnimation`（标量 `double` 专用，落 `include/aurora/animation/spring.h`，**不新增公共头**——新增头会连锁 `codespec/manual-test/08-animation.md` §1.1 的「动画层 4 个公共头」计数声称、`codespec/ARCHITECTURE.md` §4.5 模块地图与 `tools/check/umbrella_manifest.txt`）：

- 持有 `SpringSimulation`（纯求解器，不替换、签名与语义不动）、目标 `State<double>`（**句柄副本**，与来源共享同一响应式节点，故不要求实参比弹簧存活更久；实施时据此把首稿的「非拥有引用」改掉，句柄拷贝正是 `attach` 后帧循环持副本不悬垂的前提）、内部累积时间、可选收敛容差与 settled 回调。
- 公共形态（落码即此，参数名与默认值按头文件）：`explicit SpringAnimation(const State<double> &target)`；`animate_to(double target, const SpringDescription &spring, double initial_velocity = 0.0, double tolerance = 0.01) const -> void`（起点取目标 `State` 当前值，构造 `SpringSimulation` 并起播）；`set_range(double lo, double hi) const -> void`（写出值夹取窗口，弹簧过冲时不越出目标值域）；`tick(double raw_dt_seconds) const -> void`（自驱动一帧，步长在此经漏斗折算）；`attach(Animator &anim) const -> void`（接入帧循环）；`is_running()`；`target()`（最近一次起播的收敛目标）；`on_settled(std::function<void()>)`；`clock()`（作 `Animator::remove` 的注销键）。`SpringSimulation::is_settled` 的容差是**值域单位**（不是归一化时间），封装须把它作为参数暴露并沿用默认 `0.01`，文档写明单位随目标量纲变化。
- **`Animator` 侧需要先补一个入口**（首稿漏掉的依赖）：现状 `add_binding(fn)` 登记的回调 `owner` 恒为空、`remove(controller)` 摘不掉它，且回调形参不含 dt。本提案新增**带 dt 且按控制器归属可注销**的绑定入口（如 `add_frame_binding(std::function<void(double)>, AnimationController &owner)`，最终名以 `check_naming_conventions` 为准）：`SpringAnimation::attach` 自持一条 `AnimationController` 作为登记键与活跃性来源（`drive()` 登记，使 `has_active()` 在弹簧在途期间为真、`Application::wait_once` 不会深睡），帧步长经该入口下发，`remove(controller)` 连同该绑定一起摘除，满足 §6.2 的可注销不变量。弹簧的时间轴只由该 dt 累积，不使用控制器的进度值。
- 每帧：`elapsed += scaled_frame_dt(dt)`，以 `simulation.value(elapsed)` 写目标 `State`；`simulation.is_settled(elapsed, tolerance)` 为真时定格于 `target`（`State::set(target)`）、停掉登记用的控制器（`has_active()` 转假）、触发一次 `on_settled`，之后不再增长 `elapsed`、不再写 `State`，也不再有幽灵帧。载荷 `SpringAnimation::Payload` 的字段按本仓 pimpl 约定取名（同 `AnimatedValue` / `TimelinePlayer` 的 `Payload`：公开、`lower_case`、不带尾下划线），故这里叫 `elapsed` 而不是 `t_sim_`。
- `reduce_motion` 生效时单帧直接落 `target` 并结束（与 §6.7 现弹簧阶段的语义一致，不能因为换成公共原语而丢失）。
- 不把 `SpringSimulation` 改成驱动件；`SpringAnimation` 是它之上的即插封装。类型就是 `SpringAnimation`（非模板，仅 `double`），本提案不引入 `SpringAnimation<T>` 形态；向量 / 矩形弹簧由调用方组合多个标量实例，不在本提案范围。
- **`DragToDismiss` 重构（同批落地，不让仓库里出现第三份同构弹簧驱动）**：`include/aurora/event/gesture.h` 里 `DragToDismiss` 的私有 `std::optional<SpringSimulation>` + `spring_t_` + `spring_end_` 三件套改为持有一个 `SpringAnimation`（或共用其内部驱动），`on_release()` 只负责阈值裁决与把拖动速度按方向规则交作 `initial_velocity`，收敛与定格交回原语；`include/aurora/widget/dismissible.h` 的对外接口与调用点不变。重构后 §6.7 的四条语义必须逐条保持：跟手 1:1（`tick` 为 no-op、`reduce_motion` 不干预跟手）、松手按 `threshold` 二选一（回位静默 / 飞出后触发 `on_dismissed` 一次）、`reduce_motion` 单帧落端点、spring 阶段仍由使用方 `tick(dt)` 自驱（不强制改为 `Animator` 登记，避免把事件层耦合进 `NavigatorHost` 那类生命周期问题）。既有 `tests/unit/utest_gesture.cpp` 与 `tests/unit/utest_dismissible.cpp` 的全部用例须逐条继续绿，任何因重构产生的用例语义变化都要就地写明理由而不是改断言凑绿。

### 关键帧分段曲线

`Keyframes<T>::Stop` 新增可选 `Curve`（默认 `Curve{}` 即线性）：

- `Stop` 由 `{time, value}` 扩展为 `{time, value, curve}`，`curve` 缺省线性；`Stop` 仍是聚合类型，既有 `Stop{time, value}` 写法不变。
- **段归属必须定义**（首稿未定义）：第 *i* 个 `Stop` 的 `curve` 作用于 `[stop_i, stop_{i+1}]` 这一**向后**段；最后一个 `Stop` 的 `curve` 不参与求值（该处给值只作数据保留，须在注释与 §6.1 明写，避免实现自由选「向前段」口径）。`Keyframes<T>::value(t)` 定位到区间后按 `lerp(prev.value, cur.value, curve_i.transform(local))` 求值。
- 退化与边界：同一时刻的相邻 `Stop`（零宽段）取该段起点值；`stops_` 经 `std::stable_sort` 排序时 `curve` 随所属 `Stop` 一起移动；区间外语义不变（取最近端点值，空停靠点返回 `T{}`）。
- `Animator::bind(controller, Keyframes, target)` 自动应用分段曲线。
- 向后兼容：既有构造不传 curve 即线性，`Keyframes` 既有用例行为逐位不变；`codespec/specification/05-event-navigation.md` §6.1 现文「插值严格线性」随本项一并改口径。
- 与 `TimelinePlayer::track` 的每段 `Tween` 自带 `Curve` 形成两条均可达的分段曲线路径，等价 Flutter `TweenSequence` 每段带 `curve`。

### 明确不做

- 不引入隐式动画（属性赋值即补间）：遵循 `SPEC.API.EXPLICIT-FIRST.001`，无隐式动画是设计约束而非缺口。
- 不做任意路径 / 形变动画（`PathMotion`）：与纯软件栅格能力一致，`Hero` 矩形 lerp 已覆盖共享元素切片，列为 non-goal。
- 不在本提案接线 `NavigatorHost` 的逐路由 `RouteTransition::curve`、pop 强制 `Fade`、`TransitionKind` 仅 `Fade`/`Slide` 这三类导航层限制：它们是导航层接线缺口，依赖本提案的 `set_progress` 与分段曲线能力（见「关联限制与后续」）。
- 不缩放事件、布局、输入与 `Scheduler` 定时任务的时间轴——漏斗只喂给动画推进点。
- 不缩放近边缘自动滚边（`DragAutoScroll::advance` 与 `ReorderableList::tick_auto_scroll`）：速度型手感无进度与终态，缩放会让滚动与手的位置脱钩（裁决见「应用内全局时间缩放」节的例外条）。
- 不收口 `ReorderableList` 的落位弹簧（`spring_active_` / `settle_t_` / `tick_settle`）：其落点是普通成员而非 `State<double>`，复用公共原语的前置是另一项状态模型裁决，见「关联限制与后续」。本提案只把它计入缩放漏斗（`tick_settle` 走 `scaled_frame_dt`），不改其写回目标。
- 不给 `AnimatedValue` / `TimelinePlayer` / `Dismissible` 增加 `repeat` / 时间缩放 / 弹簧的转发方法；调用方经既有 `controller()` 与自由函数访问。
- 不把弹簧泛化为 `SpringAnimation<T>`（向量 / 矩形 / 颜色弹簧不在本提案）。
- 不新增公共头，因此伞头 `include/aurora/aurora.h` 与 `tools/check/umbrella_manifest.txt` 不动。

### 拟议 API 用法

以下为**实施后的目标形态**，非当前代码可编译证明；每段自带声明以便实施批次直接转为可编译用例（正向片段须转成 `tests/unit/` 用例，不留成伪码）。

```cpp
// 1. 循环 / 往复播放：无限往复呼吸态（注意 animate() 已在构造时 forward(0)）
au::State<double> glow{0.0};
auto av = au::animate(glow, au::Tween<double>{0.0, 1.0, au::Curves::ease_in_out()}, 1.2);
av.controller().repeat(-1, /*alternate=*/true);   // 经既有可变引用设模式，无限循环 => has_active() 恒真
av.attach(app.animator());

// 2. 应用内全局时间缩放：调试慢放两倍（覆盖 Animator 与控件自驱动两条路径）
app.animator().set_time_scale(0.5);   // == Flutter timeDilation = 2；0.0 = 冻结且不产帧

// 3. 手势驱动进度：抽屉 / 滑删 / 可中断转场
au::AnimationController drawer_ctrl{/*duration_seconds=*/0.3, /*value=*/0.0};
au::Animator *anim = au::Animator::current();
if (anim != nullptr) {          // 无运行中 App 时必须降级，见 Animator::current() 注释
    anim->drive(drawer_ctrl);
    drawer_ctrl.set_progress(dx / drawer_width);   // 只定位：不改 status_、不改 PlaybackMode
}

// 4. 弹簧即插封装：标量收敛到目标，is_settled 自动定格停帧
au::State<double> offset{0.0};
au::SpringAnimation spring{offset};   // 内部持 State 句柄副本（与来源共享，非裸引用）
spring.on_settled([] { /* 收敛后收尾 */ });
spring.animate_to(120.0, au::SpringDescription{.stiffness = 180.0, .damping = 22.0}, /*velocity=*/-200.0);
spring.attach(app.animator());        // 走新增的带 dt 可注销绑定入口；或每帧 spring.tick(dt) 自驱

// 5. 关键帧分段曲线：每段各自曲线（第 i 段的曲线挂在起点那个 Stop 上）
au::AnimationController kf_ctrl{0.6};
au::Keyframes<double> kf{
    std::vector<au::Keyframes<double>::Stop>{
        {0.0, 0.0, au::Curves::ease_out()},   // 作用于 [0.0, 0.5]
        {0.5, 1.0, au::Curves::linear()},     // 作用于 [0.5, 1.0]
        {1.0, 0.0},                           // 末点：曲线不参与求值
    },
};
au::State<double> morph{0.0};
kf_ctrl.forward(0.0);
au::Animator *a2 = au::Animator::current();
if (a2 != nullptr) {
    a2->bind(kf_ctrl, kf, morph);
}
```

## 验收判据

每条判据均以可编译用例 + 行为断言覆盖，并附负向判据（不得破坏既有语义）。逐条取证见本节之后的「验收证据」；取证过程中对判据措辞的收紧与偏离，连同理由一并记在文末「实施记录与偏离（落码期回写）」。

### 循环 / 往复播放

1. `repeat(-1, false)`：单一 `AnimationController` 到达 `1.0` 后把进度重置为 `0.0` 并继续 `Forward`（同向重启），持续多帧推进不抛异常、不泄漏；`repeat(0, ...)` 与 `repeat(-1, ...)` 同义（无限），不得被实现成「播 0 次」。
2. `repeat(-1, true)`：到达 `1.0` 翻为 `Reverse`、到达 `0.0` 翻为 `Forward`，进度在两端往复，不卡死在单端；每次翻转帧 `dirty()` 为真。
3. `repeat(n, false)`：有限 `n` 次到达端点后于该端点置 `Completed`/`Dismissed` 并停止，之后 `dirty()` 不再置位、`has_active()` 转假；`n == 1` 与现状一次性播放逐位等价（无回归）。
4. `reset` / `stop` 清除 `Repeat` 模式回到 `Once`，其余行为与现状逐位一致；`repeat` 与 `forward`/`reverse` 正交（先 `forward` 再 `repeat` 不覆盖方向、先 `repeat` 再 `forward` 不清模式）。
5. 循环期间的 completed 语义：经 `AnimatedValue::attach` 的循环用例里，`on_completed` 只在有限计数耗尽置终态那一帧触发一次；无限循环全程不触发。
6. `reduce_motion` 优先：无障碍开关生效时，`repeat(-1, true)` 的控制器一次落到本次目标端点并置终态、不进入循环（判据 2 的同一用例在无障碍档下须落此结果，而不是仍往复）。
7. 空闲纪律：`repeat(-1, ...)` 在途期间 `Animator::has_active()` 恒真；`stop()` 后转假。文档与注释须明示「无限循环 = 常驻活跃帧」。

### 应用内全局时间缩放

8. `set_time_scale(2.0)` 下，相同 `dt` 序列推进到同一 `value` 的帧数约为 `scale = 1.0` 的一半；`set_time_scale(0.0)` 冻结；`set_time_scale(-1.0)` 与 NaN 夹取为 `0.0`；默认 `1.0`。
9. 缩放**只**影响推进步长：目标 `State` 终值、`duration`、曲线不变；缩放还原为 `1.0` 后行为与原先一致（无残留）。缩放不得叠加——同一条帧步长从取值到消费至多经过 `scaled_frame_dt` 一次（`ScrollGlide::tick` 与其消费点不得两层都乘）；判据：以 `scale = 2.0` 注入固定 dt 序列，各路径的到位帧数都恰为 `scale = 1.0` 的一半，出现 1/4 即双层缩放。
10. **覆盖矩阵（真全局的判据）**：同一缩放值下，逐一验证 `Animator` 登记控制器、`AnimatedValue::tick`、`TimelinePlayer::tick`、`TweenAnimation::tick`、`SpringAnimation::tick`（`DragToDismiss` 重构后经它）、`ScrollGlide::tick` 的每个消费点（实测四处：`Scroll` / `LazyList` / `GridView` / `PullToRefresh`）、`ReorderableList::tick_settle` 的落位弹簧与 `Skeleton` 的微光相位——任一仍吃裸 dt 即本项判据不成立。反向项：同一缩放值下 `DragAutoScroll::advance` / `ReorderableList::tick_auto_scroll` 的每帧滚边增量不变（验证「例外」是真例外，不是漏网）。
11. 冻结不空转（两条停手机制各自可证）：`scale == 0.0` 时 (a) `Animator` 侧登记控制器不置脏、绑定回调不写回；(b) 控件自驱侧的冻结帧不推进、不写回、不标脏、**不请求帧**——判据形式为固定帧序列下统计重绘请求数（`Widget::on_dirty` 计数），冻结段内该数须恒为 0 而不随帧数线性上升，解冻后须重新为正（同计数器自证不是「回调没接线」）。控件的每帧计时标志（`needs_gesture_tick_` / 在途 `is_animating()`）**在冻结段保留不摘**：摘了就没有续跑点，而全局入口拿不到逐控件指针，故解冻由 `set_time_scale` 置一枚重挂请求、`Application::wait_once` 消费出「下一帧不深睡」，那一帧各控件照常 `tick_gestures` 并自行续帧；恢复 `> 0` 后冻结前进度不丢。 Animator 侧的对应判据是 `has_active()` 在倍率为 0 时恒假（否则帧循环被钉在满帧）。
12. 确定性不破：`Animator::current()` 为空（无头 / golden 场景）时 `scaled_frame_dt` 恒等返回，既有 golden 基线逐字节不变。
13. `reduce_motion` 与缩放正交且优先：短路落端点的路径不受倍率影响。
14. 该入口不进入业务动画契约：文档标注调试 / 演示用途，并写清「不缩放事件、布局、输入与定时任务」。

### 交互式进度 setter

15. `set_progress(t)` 写入 `value_` 且 `status_` 与 `PlaybackMode` 保持不变（原本非播放态调用后仍非播放态，不翻转）；越界 `t` 夹入 `[0,1]`。
16. **写回链路**：手势在帧边界前 `set_progress`，同一帧 `Animator::tick` 的绑定阶段须把新进度经补间写回目标 `State`。此判据直接锚定外部脏位机制——若实现只置 `dirty_` 而不清 `tick()` 的入口清脏，本条转红。
17. **既有语义不得被机制改动带走**：`tests/unit/utest_animator.cpp` 的 `controller_reverse_reset_stop_semantics`（`stop()` 后 `tick()` 断言 `dirty()` 为假）与 `animator_bindings_write_only_on_dirty_frames`（空闲帧不写回）必须继续绿；`Animator` 帧末 `clear_dirty()` 后 `dirty()` 恒假。
18. 与 `reset` 区分：`reset(v)` 仍按值改写 `status_`（`≥1 → Completed`，否则 `Dismissed`）；`set_progress` 在任何 `v` 下都不改写 `status_`。
19. 手势驱动转场的前置可行性：抽屉 / 滑删用例以 `set_progress(dx / width)` 直接定位，进度随拖拽连续变化，松手后可由 `forward`/`reverse` 接管收尾（不依赖 `reset` 的状态副作用）。

### 弹簧即插封装与 DragToDismiss 收口

20. `SpringAnimation::animate_to(target, spring, velocity, tolerance)` 起播后，目标 `State` 每帧逼近 `target`，`is_settled` 为真后定格于 `target` 并停止推进（不再增长内部时间、不再写 `State`、`has_active()` 转假）；`on_settled` 恰触发一次。
21. **活跃性**：`attach` 后弹簧在途期间 `Animator::has_active()` 为真（否则 `Application::wait_once` 会深睡、弹簧半途冻结）；`remove(controller)` 后该弹簧的绑定被一并摘除，目标 `State` 析构后帧循环不再触碰它。
22. 收敛判定可转红：临时把 `is_settled` 短路为恒假，则应出现「永不停止 / 持续写 State / `has_active()` 恒真」的判据转红（验证停帧由 `is_settled` 真实驱动，而非偶然落点）。
23. 不替换 `SpringSimulation`：纯求解器（`value`/`velocity`/`is_settled`/`target`）仍为独立可测单元，签名与语义不动。
24. `reduce_motion` 下 `SpringAnimation` 单帧落 `target` 并结束，与换封装前一致。
25. **重复实现归零（分两处清点，不谎称全仓归零）**：
    - `include/aurora/event/gesture.h` 的 `DragToDismiss`：本提案收口，落地后该头内不再存在与 `SpringAnimation` 同构的私有「累积时间 + `is_settled` 定格 + 写 `State`」三件套（以符号级核对为判据，不以注释声明为准）。
    - `include/aurora/widget/reorderable_list.h` 的 `spring_active_` / `settle_t_` / `tick_settle`：本提案**不收口**，理由见「关联限制与后续」——它落位写的是普通成员 `drag_follow_` 而非 `State<double>`，复用公共原语需要先裁决「给该控件加 State 句柄」还是「给原语加值汇形参」，那是另一项状态模型决策，塞进本提案会把收口范围扩到 widget 层。判据：落地后全仓同构私有弹簧**只剩这一处**，且该处就地注释指向后续提案。
26. **DragToDismiss 重构回归**：§6.7 四条对外语义逐条可证（跟手 1:1 且 `tick` no-op、阈值二选一裁决与初速度方向规则、`reduce_motion` 单帧落端点、spring 阶段由 `tick(dt)` 自驱），`on_dismissed` 仍为一次性；`tests/unit/utest_gesture.cpp` 与 `tests/unit/utest_dismissible.cpp` 全部用例继续绿，且缩放 / 冻结判据（本文件判据 10/11）在 Dismissible 路径上同样成立。
27. 标量闭环：只做 `double`；组合多个标量实例驱动向量 / 矩形为合法用法，不做框架内扩展。

### 关键帧分段曲线

28. `Stop` 带 `curve` 时，`value(t)` 在 `[stop_i, stop_{i+1}]` 段按**第 *i* 个 `Stop`** 的曲线插值；与「段间恒线性」基线对比，曲线段下中间帧取值不同；末个 `Stop` 的曲线不参与求值（判据须断言「改末点曲线不影响任何取值」，把段归属口径钉死）。
29. 缺省 `curve`（既有 `Stop{time, value}` 构造）保持线性，向后兼容：`Keyframes` 既有用例行为逐位不变。
30. 零宽段（同时刻相邻 `Stop`）取该段起点值；`stable_sort` 后 `curve` 随所属 `Stop` 移动（乱序入参与排序后取值一致）；区间外仍取最近端点值。
31. `Animator::bind(controller, Keyframes, target)` 自动应用分段曲线，脏帧写回目标 `State` 的中间值符合各段曲线。

### 门禁与变异自证

实施验收须重建库、测试、受影响 demos 与 `aurora_api_json`，构建启用最大可用并行线程，再运行全量 ctest（含无头 golden 与动画 / 事件 / widget 域用例）。按 `codespec/ARCHITECTURE.md` §14.6「公共 API」行的完整矩阵，必跑静态门禁为：`check_api_schema_sync`（先建生成器）、`check_naming_conventions`、**`check_api_budget`**、**`check_doc_comments`**、`lint`（覆盖实际修改 TU）、`format-check`；另跑 `check_umbrella_header`、`check_arch_module_map`、`check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format`、`check_change_proposals`、`check_version_consistency`、`check_no_cjk_literals`、`check_no_hardcoded_paths`、`check_nolint_layout`，测试面改动对应 CTest 名 `registry_integrity`（即 `tools/check/check_test_registry.py`）与 `check_test_temp_hygiene`，文档目标另跑 `--target docs`。

新增枚举 `PlaybackMode` **不进** `tools/include/known_enums.h`，理由就地写在该表的动画域注释里：该表的用途是给工具链提供「可填写的取值域」（widget 属性键 / MCP 入参 / LSP 补全），而 `PlaybackMode` 与既存的 `AnimationStatus` 同族——两者只经 getter 读出（`AnimationController::mode()` / `status()`），既不出现在任何 `props[].type`，也不作为工具链入参（`repeat(int count, bool alternate)` 收计数与布尔，不收枚举）。入表会把「读出来的状态」提示成「填进去的值」，那类建议编译不过。`tests/integration/itest_known_enums.cpp` 按属性类型对账，公共枚举无须全量入表，故本批不改该文件。

变异只在临时验证副本实施，逐项还原，记录确实失败的测试；不拿「不存在某诊断字符串」或无关测试冒充能力证明。

| 临时变异 | 必须转红的判据 |
| :--- | :--- |
| 删除 `repeat` 的端点续播分支 | 判据 1/2/3 的循环 / 往复 / 有限计数用例 |
| 让 `Repeat` 分支排在 `reduce_motion` 短路之前 | 判据 6 的无障碍优先用例 |
| 把 `set_time_scale` 短路为恒 1 | 判据 8/9 的倍率 / 冻结 / 夹取用例 |
| 只让 `Animator::tick` 缩放、把覆盖矩阵里的任一自驱消费点改回裸 dt（四处 `ScrollGlide::tick` 消费点 / `ReorderableList::tick_settle` 的落位弹簧 / `SpringAnimation::tick`（`DragToDismiss` 经它）/ `Skeleton` 的微光相位 / `AnimatedValue` · `TimelinePlayer` · `TweenAnimation` 三条手动自驱路径） | 判据 10 的覆盖矩阵对应行（证明「真全局」不是嘴上口径） |
| 把自动滚边（`DragAutoScroll::advance` / `ReorderableList::tick_auto_scroll`）也纳入缩放 | 判据 10 的反向项（滚边每帧增量不随倍率变化） |
| 在 `ScrollGlide::tick` 与它的消费点两层各乘一次倍率 | 判据 9 的双层缩放项（`scale = 2.0` 下到位帧数变成 1/4 而非 1/2） |
| 冻结时仍逐帧自标脏 / 仍请求帧（在 `Scroll` 的零步长分支补一枚 `request_frame`，或把 `Skeleton` 的「相位变了才标脏」改成无条件标脏） | 判据 11(b) 的重绘请求计数用例（`scroll_glide_freeze_...` / `skeleton_freeze_...`） |
| 让 `Animator::has_active()` 的零倍率分支失效（冻结仍报「有活跃动画」） | 判据 11(a)：冻结下登记控制器仍被推进、置脏并经绑定写回 |
| 冻结只把 dt 归零、不置解冻重挂请求（摘除 `set_time_scale` 的 `rearm_pending_`） | 判据 11(b) 的推进次数计数用例（解冻后无人送首帧，进度停在冻结处不续跑） |
| 让 `set_progress` 只置 `dirty_`（不动外部位） | 判据 16 的当帧写回用例 |
| 删掉 `tick()` 入口清脏、让外部位与推进脏合一 | 判据 17 的既有 `controller_forward_ticks_to_completed` / `controller_reverse_reset_stop_semantics`（首稿此处还点了 `animator_bindings_write_only_on_dirty_frames`，实测该档下它不转红，见「验收证据」判据 17 行与实施记录） |
| 让 `set_progress` 改写 `status_` | 判据 15/18 的「status_ 不变」与「与 reset 区分」用例 |
| 把 `SpringAnimation` 收敛改成不经 `is_settled` | 判据 20/22 的容差 / 停帧时机用例（`spring_animation_settle_time_follows_tolerance`） |
| 让新绑定入口不带 `owner`（`remove()` 摘不掉） | 判据 21 的注销与悬垂用例 |
| `DragToDismiss` 保留自己的弹簧三件套不复用原语 | 判据 25 的符号级核对 + 判据 10 的 Dismissible 行 |
| 把分段曲线改成「末点曲线作用于向前段」 | 判据 28 的「改末点曲线不影响任何取值」断言 |
| 把 `Keyframes` 段间插值改回全局线性 | 判据 28/31 的分段曲线用例 |

## 验收证据

实施批次（2026-10-30，本机 MinGW GCC 16.1.0 / Ninja / ccache，构建一律启用最大并行）取得下列证据。判据编号沿用上一节；载体只写**实际断言该判据的用例名**，一条用例覆盖多条判据时在各行分别列出。

### 判据 → 载体

| 判据 | 取证载体（套件 = 文件 stem） | 结果 |
| :--- | :--- | :--- |
| 1 无限循环同向重启、`0` 与 `-1` 同义 | `utest_animator.repeat_infinite_restarts_forward_at_zero_and_zero_count_means_infinite` | 通过；变异 M01 转红 |
| 2 往复两端翻转 | `utest_animator.repeat_alternate_oscillates_between_both_endpoints` | 通过；变异 M01 转红 |
| 3 有限计数末次到端置终态、`n == 1` 与一次性等价 | `utest_animator.repeat_finite_count_stops_at_last_reached_endpoint` | 通过；变异 M01 转红 |
| 4 `reset` / `stop` 清模式、与方向正交 | `utest_animator.repeat_is_orthogonal_to_direction_and_cleared_by_reset_and_stop` | 通过；变异 M01 也命中该例（摘掉端点续播后正交断言失守） |
| 5 循环期 `on_completed` 只在计数耗尽那帧一次 | `utest_animator.repeat_completed_callback_fires_only_when_count_exhausts` | 通过；变异 M01 转红 |
| 6 `reduce_motion` 优先于循环 | `utest_animator.reduce_motion_short_circuits_repeat_instead_of_looping` | 通过；变异 M02 使该例转红（短路排到循环之后即失守） |
| 7 无限循环 = 常驻活跃帧 | `utest_animator.infinite_repeat_keeps_animator_active_until_stop` | 通过；变异 M01 转红（续播分支被摘后控制器到端即停，`has_active()` 提前转假） |
| 8 倍率减半帧数、`0` 冻结、负 / NaN 夹 `0.0`、缺省 `1.0` | `utest_animator.time_scale_halves_frames_and_clamps_invalid_values` | 通过；变异 M03 转红 |
| 9 只改步长不改终值 / 曲线 / `duration`，还原无残留；缩放不叠加 | `utest_animator.scaling_preserves_duration_curve_and_final_value_without_residue`；单层性由 `utest_scroll_viewport.scroll_glide_tick_is_immune_to_current_animator_time_scale` 钉住（`ScrollGlide::tick` 保持纯时序数学） | 通过；变异 M06（两层各乘一次）使到位帧数落到 1/4 而转红 |
| 10 覆盖矩阵：登记控制器 / `AnimatedValue` / `TimelinePlayer` / `TweenAnimation` / 四枚 glide 消费点 / `ReorderableList` 落位弹簧 / `SpringAnimation`（含 `DragToDismiss`）/ `Skeleton` 相位 | `utest_animator.every_progress_path_scales_by_exactly_the_multiplier`（内核四条路径同批断言）+ `utest_scroll.scroll_glide_frame_count_follows_time_scale` / `utest_lazy_list.lazy_list_glide_frame_count_follows_time_scale` / `utest_grid_view.grid_view_glide_frame_count_follows_time_scale` / `utest_pull_to_refresh.pull_to_refresh_setback_frame_count_follows_time_scale` + `utest_reorderable_list.settle_spring_frame_count_halves_with_time_scale` + `utest_spring.spring_animation_scales_with_the_animator_and_freezes_on_zero` + `utest_dismissible.dismissible_spring_frames_halve_when_time_scale_doubles` + `utest_skeleton.skeleton_shimmer_phase_frames_halve_with_time_scale`；反向项 `utest_scroll.scroll_drag_auto_scroll_delta_ignores_time_scale` 与 `utest_reorderable_list.auto_scroll_edge_increment_is_excluded_from_time_scale` | 通过；消费点单点改回裸 dt 可定位转红（M04a / M04e / M04g / M04j 各命中对应行），反向项由 M05a / M05b 转红 |
| 11(a) 冻结帧不置脏、不写回 | `utest_animator.frozen_animator_neither_dirties_nor_writes_back` | 通过；变异 M07c（`has_active()` 的零倍率分支失效）使该例转红——冻结仍被报成「有活跃动画」时，`Animator::tick` 照常推进登记控制器并置脏、经绑定写回目标 `State` |
| 11(b) 控件侧冻结不推进、不请求重绘；解冻续跑且不丢进度；`has_active()` 零倍率恒假 | 重绘请求计数用例 `utest_scroll.scroll_glide_freeze_holds_progress_and_emits_no_redraw` / `utest_lazy_list.lazy_list_glide_freeze_holds_progress_and_emits_no_redraw` / `utest_grid_view.grid_view_glide_freeze_holds_progress_and_emits_no_redraw` / `utest_pull_to_refresh.pull_to_refresh_setback_freeze_holds_progress_and_emits_no_redraw` / `utest_skeleton.skeleton_freeze_holds_phase_and_requests_no_frames` / `utest_reorderable_list.frozen_time_scale_pauses_settle_without_frame_requests_and_thaw_resumes` / `utest_dismissible.dismissible_freeze_holds_spring_and_thaw_resumes_to_same_endpoint`；`Animator` 侧 `utest_animator.thaw_arms_one_rearm_request_and_resumes_from_frozen_progress` | 通过；M07a（Scroll 的零步长分支补一枚 `request_frame`）⇒ `scroll_glide_freeze_holds_progress_and_emits_no_redraw` 转红；M07b（Skeleton 无条件标脏）⇒ `skeleton_freeze_holds_phase_and_requests_no_frames` 转红；M08（摘除 `rearm_pending_`）⇒ `thaw_arms_one_rearm_request_...` 转红（解冻无人送首帧）。三行互不重叠，归属确定 |
| 12 无当前 `Animator` 时漏斗恒等，golden 不破 | `utest_scroll_viewport.scaled_frame_dt_is_identity_without_current_animator`、`utest_animator.funnel_is_identity_without_current_animator`；全量 ctest 含既有像素 golden 套件在内 450 例全绿（见下节） | 通过 |
| 13 `reduce_motion` 与缩放正交且优先 | `utest_animator.reduce_motion_short_circuit_ignores_time_scale`、`utest_spring.spring_animation_reduce_motion_settles_in_one_frame` | 通过 |
| 14 不进入业务动画契约（文档标注 + 缩放边界） | `codespec/specification/05-event-navigation.md` §6.5 与 `codespec/GUIDELINE.md` §46.2 正文；代码面核对：本批只改动画步长取源，事件派发 / 布局 / `Scheduler` 的 dt 未被触及（改动文件清单可逐一对账） | 通过（文档与代码面核对项，不另立数值用例） |
| 15 `set_progress` 只定位、越界夹取、`status_` 与模式不变 | `utest_animator.set_progress_positions_without_touching_status_or_mode` | 通过；变异 M11（改写 `status_`）转红 |
| 16 当帧写回链路 | `utest_animator.set_progress_writes_back_through_binding_in_the_same_frame`（含 `has_active()` 不因定位转真、帧末双清） | 通过；变异 M09（只置 `dirty_`）转红，且同档连带命中判据 19 的 `gesture_drag_then_forward_or_reverse_takes_over` |
| 17 既有语义不被机制带走 | 既存的 `utest_animator.controller_reverse_reset_stop_semantics` 与 `utest_animator.animator_bindings_write_only_on_dirty_frames` 未改动继续绿 | 通过；变异 M10（摘掉 `tick()` 入口清脏）实测转红的是 `controller_forward_ticks_to_completed` 与 `controller_reverse_reset_stop_semantics` 两例——**`animator_bindings_write_only_on_dirty_frames` 在该档下不转红**（它按帧统计写回、推进脏被吞掉时其断言仍成立），故本行证据只认前两例 |
| 18 与 `reset` 区分 | 同判据 15 用例末段（`reset(1.0)` 对照改写 `status_` 并清模式） | 通过 |
| 19 手势驱动转场前置可行性 | `utest_animator.gesture_drag_then_forward_or_reverse_takes_over`、`utest_gesture.drag_recognizer_positions_bound_progress_then_playback_takes_over` | 通过；前者同时是 M09 与 M11 的转红载体（外部位失效或状态被改写时，「定位 → 接管」这条链先断） |
| 20 逐帧逼近、`is_settled` 后定格停帧、`on_settled` 恰一次 | `utest_spring.spring_animation_converges_then_settles_exactly_once`，容差另由 `utest_spring.spring_animation_settle_time_follows_tolerance` | 通过；变异 M12（把收敛判定改成不经 `is_settled` 的固定时长）实测**只**使容差用例 `spring_animation_settle_time_follows_tolerance` 转红——`spring_animation_converges_then_settles_exactly_once` 在该档下不转红（它不断言具体收敛时长，只数 `on_settled` 次数），故本行的数值证据以容差用例为准 |
| 21 `attach` 后在途即活跃、`remove` 连带摘除绑定 | `utest_spring.spring_animation_attach_keeps_active_and_is_removable` | 通过；变异 M13（绑定不带 `owner`）转红 |
| 22 停帧真由 `is_settled` 驱动 | 判据 20 的定格断言（`has_active()` 转假 + 不再写 `State`） | 通过；存在性证明落在判据 20 的容差用例上——停帧时机随容差变化而非受常量时长支配，说明判据取自 `is_settled`（同档下定格断言本身不转红，见上行，本行不据此另立证据） |
| 23 `SpringSimulation` 纯求解器不动 | `include/aurora/animation/spring.h` 本批 **161 增 / 0 删**；既存 6 例纯求解器用例（`spring_description_defaults_and_derived_quantities` 起）未改动继续绿 | 通过 |
| 24 `reduce_motion` 单帧落 `target` | `utest_spring.spring_animation_reduce_motion_settles_in_one_frame` | 通过 |
| 25 重复实现归零两处清点 | 符号级核对用例 `utest_gesture.drag_to_dismiss_holds_no_private_spring_duplicate`（读本仓 `include/aurora/event/gesture.h` 源码断言三件套符号不存在）；`reorderable_list.h` 的 `spring_active_` / `settle_t_` 仍在并按判据就地注释指向后续 | 通过；变异 M14（把三件套加回 `DragToDismiss`）使该例转红 |
| 26 `DragToDismiss` 重构回归（§6.7 四条对外语义） | `utest_gesture.dtd_follow_phase_tick_is_a_no_op` / `dtd_release_arbitrates_only_between_the_two_endpoints` / `dtd_release_drops_opposing_velocity_and_keeps_aligned_one` / `dtd_spring_phase_self_drives_and_dismissed_fires_exactly_once` / `dtd_reduce_motion_lands_in_one_frame_but_leaves_follow_untouched`；`utest_gesture`（21 例）与 `utest_dismissible`（11 例）全绿；缩放 / 冻结在 Dismissible 路径上的判据 10/11 见上两行 | 通过 |
| 27 只做标量、组合合法 | `utest_spring.two_scalar_springs_compose_into_a_vector_offset`；句柄拷贝共享载荷另由 `utest_spring.spring_animation_handle_copies_share_the_payload`、越界夹取窗口由 `utest_spring.spring_animation_writes_overshoot_unless_range_declared` | 通过 |
| 28 段曲线取**起点** `Stop`、末点曲线不参与求值 | `utest_timeline.keyframes_segment_curve_belongs_to_left_stop`（含「改末点曲线不影响任何取值」断言）、`utest_timeline.keyframes_segment_curve_applies_to_composite_value_type` | 通过；变异 M15（改成末点曲线）使该两例连同判据 31 的 `animator_keyframe_binding_applies_segment_curves` 一并转红 |
| 29 缺省线性、向后兼容 | 既存 `utest_timeline` 用例（16 例中 13 例未改动）继续绿 | 通过 |
| 30 零宽段取起点值、`stable_sort` 携带曲线、区间外取最近端点 | `utest_timeline.keyframes_sort_carries_curve_and_keeps_insertion_order_on_ties` | 通过 |
| 31 `Animator::bind` 自动应用分段曲线 | `utest_animator.animator_keyframe_binding_applies_segment_curves` | 通过；M15（末点曲线）与 M16（段间退回全局线性）两档均使该例转红，M16 同时命中判据 28 的两例 |

新增用例合计 **54 例**，分布在 12 个套件：`utest_animator` 18、`utest_spring` 8、`utest_gesture` 7、`utest_timeline` 3、`utest_scroll` 3（含滚边反向项 1 例）、`utest_reorderable_list` 3（含自动滚边反向项 1 例）、`utest_dismissible` / `utest_lazy_list` / `utest_grid_view` / `utest_pull_to_refresh` / `utest_scroll_viewport` / `utest_skeleton` 各 2。测试侧夹具 `tests/support/animation_fixture.h` 为本批新增文件（当前 / 作用域内 `Animator`、减弱动态效果档、缩放帧泵、重绘请求计数器四件），按 `codespec/CODING_STANDARDS.md` §3.2 的「一个单元头至多一个测试声明」约束不另立单元目标。

### 临时变异 → 实际转红

22 档变异逐档注入（一次性临时变异脚本（不入库），每档以「目标串在本文件中恰好出现一次」为锚点，注入后 `cmake --build build --target aurora_test_runner --parallel 16`，只跑受影响套件的 ctest 并收割 `[ FAILED ]` 行），每档跑完在 `finally` 里按字节还原。结果：**22 档全部 RED，无空转档**（`NOT_RED_OR_EMPTY=none`），合计 43 条失败用例实例、去重后 35 条用例。下表只列实际转红者，未转红的预期已就地写进判据行。

| 档 | 注入 | 实际转红的用例 | 归属判据 |
| :--- | :--- | :--- | :--- |
| M01 | 删除 `repeat` 的端点续播分支 | `utest_animator`：`infinite_repeat_keeps_animator_active_until_stop` / `repeat_alternate_oscillates_between_both_endpoints` / `repeat_completed_callback_fires_only_when_count_exhausts` / `repeat_finite_count_stops_at_last_reached_endpoint` / `repeat_infinite_restarts_forward_at_zero_and_zero_count_means_infinite` / `repeat_is_orthogonal_to_direction_and_cleared_by_reset_and_stop`（6） | 1/2/3/4/5/7 |
| M02 | 让 `Repeat` 分支排在 `reduce_motion` 短路之前 | `utest_animator.reduce_motion_short_circuits_repeat_instead_of_looping`（1） | 6 |
| M03 | 把 `set_time_scale` 短路为恒 1 | `utest_animator`：`every_progress_path_scales_by_exactly_the_multiplier` / `funnel_is_identity_without_current_animator` / `scaling_preserves_duration_curve_and_final_value_without_residue` / `time_scale_halves_frames_and_clamps_invalid_values` + `utest_scroll.scroll_glide_frame_count_follows_time_scale`（5） | 8/9/10/12 |
| M04a | 消费点改回裸 dt：`Scroll` | `utest_scroll`：`scroll_glide_frame_count_follows_time_scale` / `scroll_glide_freeze_holds_progress_and_emits_no_redraw`（2） | 10/11(b) |
| M04e | 消费点改回裸 dt：`ReorderableList::tick_settle` | `utest_reorderable_list`：`frozen_time_scale_pauses_settle_without_frame_requests_and_thaw_resumes` / `settle_spring_frame_count_halves_with_time_scale`（2） | 10/11(b) |
| M04g | 消费点改回裸 dt：`SpringAnimation::tick`（覆盖 `Dismissible`） | `utest_dismissible`：`dismissible_freeze_holds_spring_and_thaw_resumes_to_same_endpoint` / `dismissible_spring_frames_halve_when_time_scale_doubles` + `utest_spring.spring_animation_scales_with_the_animator_and_freezes_on_zero`（3） | 10/11(b) |
| M04j | 消费点改回裸 dt：`TimelinePlayer::tick` 手动自驱 | `utest_animator.every_progress_path_scales_by_exactly_the_multiplier`（1） | 10 |
| M05a | 把自动滚边也纳入缩放：`DragAutoScroll::advance` | `utest_scroll.scroll_drag_auto_scroll_delta_ignores_time_scale`（1） | 10 反向项 |
| M05b | 把自动滚边也纳入缩放：`ReorderableList::tick_auto_scroll` | `utest_reorderable_list.auto_scroll_edge_increment_is_excluded_from_time_scale`（1） | 10 反向项 |
| M06 | 在 `ScrollGlide::tick` 与消费点两层各乘一次倍率 | `utest_scroll.scroll_glide_frame_count_follows_time_scale` + `utest_scroll_viewport.scroll_glide_tick_is_immune_to_current_animator_time_scale`（2） | 9 |
| M07a | 冻结时仍逐帧自标脏：`Scroll` 收位滑动 | `utest_scroll.scroll_glide_freeze_holds_progress_and_emits_no_redraw`（1） | 11(b) |
| M07b | 冻结时仍逐帧自标脏：`Skeleton` 微光相位 | `utest_skeleton.skeleton_freeze_holds_phase_and_requests_no_frames`（1） | 11(b) |
| M07c | 冻结不摘 `Animator` 侧停帧判据（`has_active()` 的零倍率分支失效） | `utest_animator.frozen_animator_neither_dirties_nor_writes_back`（1） | 11(a) |
| M08 | 冻结只归零 dt、不送解冻重挂帧 | `utest_animator.thaw_arms_one_rearm_request_and_resumes_from_frozen_progress`（1） | 11(b) |
| M09 | 让 `set_progress` 只置 `dirty_`（不动外部位） | `utest_animator`：`gesture_drag_then_forward_or_reverse_takes_over` / `set_progress_writes_back_through_binding_in_the_same_frame`（2） | 16/19 |
| M10 | 删掉 `tick()` 入口清脏、让外部位与推进脏合一 | `utest_animator`：`controller_forward_ticks_to_completed` / `controller_reverse_reset_stop_semantics`（2） | 17 |
| M11 | 让 `set_progress` 改写 `status_` | `utest_animator`：`gesture_drag_then_forward_or_reverse_takes_over` / `set_progress_positions_without_touching_status_or_mode`（2） | 15/19 |
| M12 | 把 `SpringAnimation` 收敛改成不经 `is_settled` | `utest_spring.spring_animation_settle_time_follows_tolerance`（1） | 20/22 |
| M13 | 让新绑定入口不带 `owner`（`remove()` 摘不掉） | `utest_spring.spring_animation_attach_keeps_active_and_is_removable`（1） | 21 |
| M14 | `DragToDismiss` 保留自己的弹簧三件套不复用原语 | `utest_gesture.drag_to_dismiss_holds_no_private_spring_duplicate`（1） | 25 |
| M15 | 把分段曲线改成「末点曲线作用于向前段」 | `utest_animator.animator_keyframe_binding_applies_segment_curves` + `utest_timeline`：`keyframes_segment_curve_applies_to_composite_value_type` / `keyframes_segment_curve_belongs_to_left_stop`（3） | 28/31 |
| M16 | 把 `Keyframes` 段间插值改回全局线性 | 同 M15 的 3 例（3） | 28/31 |

判据 13 / 14 / 18 / 23 / 24 / 26 / 27 / 29 / 30 未单设变异档：它们的行为断言已由正向用例钉住（端点形态、纯求解器未改动、句柄共享、向后兼容与排序携带），且其失败面与上表已有档重合（如 13 落在 M02 / M03 的转红集合里、18 落在 M11 的对照断言里）；26 是既有对外语义的回归集合，本批的注入面不覆盖它。不为凑数给这些判据造无关变异。

### 未取证项

- `codespec/manual-test/08-animation.md` 的本批新增用例 TC-ANIMATION-008 … TC-ANIMATION-012（含既有的 TC-ANIMATION-001）**未按人工步骤在真实窗口上执行**，其三、四节（执行记录 / 缺陷反馈）留空；步骤里的观测量（缩放前后同一手势的到位时长、冻结期间 CPU 不升满帧、减弱动态效果档与缩放叠加的观感）由上述单测的等价断言覆盖，像素与主观流畅度仍需人工复核。
- 未做真实窗口的像素探针：本批未新增 golden，也不改渲染路径（判据 12 的「golden 不破」是以全量 ctest 的既有 golden 套件继续绿为证，不是以新截图为证）。
- 性能面未量化：冻结期间「不再满帧唤醒」由 `Animator::has_active()` 的零倍率分支与控件侧 `needs_gesture_tick_` 的摘除逻辑保证，用例按帧请求计数取证，未测绝对 CPU 占用曲线。

### 重建与门禁

按上一节的口径重建并复跑，构建一律 `--parallel 16`（本机 16 逻辑核，Ninja + MinGW GCC + ccache）。

- **重建**：库与测试目标 90 步、`demos` 177 步、`aurora_api_json` 2 步，三段 `EXIT=0`。`aurora_api.json` 重建后与 HEAD **零差异**（`git status` 对该文件无输出），与「schema 面不承载动画类、枚举仍来自策展表」的现状一致，不得据此宣称 schema 已反映本批能力。
- **全量测试**：`ctest --preset ninja-test` **450/450 通过**、`EXIT=0`、real 22.79 秒。格式化与 lint 整改前后各复跑一次，两次同为 450/450。该套件含本节列出的全部 `check_*` 静态门禁（`check_api_schema_sync` / `check_naming_conventions` / `check_api_budget` / `check_doc_comments` / `check_umbrella_header` / `check_arch_module_map` / `check_codespec_xref` / `check_code_doc_sync` / `check_manual_test_format` / `check_change_proposals` / `check_version_consistency` / `check_no_cjk_literals` / `check_no_hardcoded_paths` / `check_nolint_layout`）、`registry_integrity`、`check_test_temp_hygiene`，以及无头 golden 与动画 / 事件 / widget 域用例；判据 12 的「golden 不破」即以此为准。首稿担心的 `check_doc_comments` 面在本批实际为 0 findings（独立复跑 `EXIT=0`）。
- **format-check**：以 `tools/check/run_clang_format.py`（`format-check` 目标的同一执行器、构建目录里记录的同一 clang-format 二进制）对本批 29 枚源文件跑 `mode=check`：**checked 29 / conforming 29 / non-conforming 0 / diff lines 0**。两轮批内 `--fix` 的作用域由暂存文件清单拼出的路径正则限定，实测改写全部落在批内（批外文件名单为空），且逐 hunk 核对为尾注对齐与换行回收、无逻辑改动。全仓 `format-check` 目标未复跑——它是本机曾出现过静默不结束的那条形制，本批的合规面由同执行器的批内检核承担。
- **lint**：`select_lint_tus.py --changed-only` 从 768 枚 TU 里选出本批改动的 15 枚（改动的全部 `.cpp`；改动的公共头经其同源 `utest_*` TU 覆盖，无未覆盖 TU），`run_clang_tidy.py --fail-on warning --jobs 16` 首轮报 **38 条、全部落在本批新增行上**（历史背负 0 条），逐条改码而非抑制：31 条 `readability-identifier-naming`——本批新写的常量按本仓约定改名（文件 / 命名空间作用域常量取 `AURORA_` + 全大写、与 `utest_scroll.cpp` 既有的 `AURORA_ROW_H` 同形；函数内常量取 `lower_case`；类内 `static constexpr` 取全大写；`SpringAnimation::Payload` 的字段去掉尾下划线，与该头之外既有的 `AnimatedValue` / `TimelinePlayer` 载荷同形）；4 条 `readability-math-missing-parentheses`（补显式括号，不改算式）；`modernize-use-designated-initializers` 与 `modernize-use-auto` 各 1 条（demo 侧写法）。唯一未改码的是 1 条 `performance-unnecessary-copy-initialization`：那句 `SpringAnimation alias = spring;` 的**句柄拷贝正是被测对象**（验证两份句柄共享同一载荷），按建议直接复用 `spring` 会让用例失去所验语义，故按 `codespec/CODING_STANDARDS.md` §5.2 就地抑制——理由整段写在指令之前、名单单行闭合于代码行。复跑：**unique findings 0、blocking 0、`EXIT=0`**，`check_nolint_layout` 随全量 ctest 同绿。
- **docs**：`--target docs` 在 `WARN_AS_ERROR=YES` 下于 `include/aurora/net/http.h` 的 `Content-Range` 段注释早退（该行内联码里写着 `bytes */1234`，反引号块内的 `*/` 被 Doxygen 读成 C 注释收尾）。该文件本批未触碰（`git diff HEAD` 对其为空，最后一次改动来自上游提交），属既有红。为拿到完整名单另跑一遍全树扫描（同一 `Doxyfile`、仅把 `WARN_AS_ERROR` 置否、产物落在构建目录外）：Doxygen 1.18.0 全树 `EXIT=0`、共 10 条 warning，逐条落在 `net/http.h`(4) / `state/resource.h`(2) / `app/application.h`(2) / `core/thread_pool.h`(1) / `net/blocking.h`(1)——**本批的动画 / 事件 / widget 公共头一条不报**（名单里不含 `animation/animator.h`、`animation/spring.h`、`animation/timeline.h`、`event/gesture.h` 与本批改动的八枚 widget 头）。

## 提案时证据与未验证边界

2026-10-29 提案、2026-10-30 复审复核，下列**现状缺口证据**经源码与 `codespec/` 文档交叉核对（是现状，不是目标模型已验收）：

- `AnimationController`（`include/aurora/animation/animator.h`）仅 `forward`/`reverse`/`reset`/`stop`/`tick`，无 `repeat`/`loop`/`alternate`；`reset(double)` 按值改写 `status_`，`value()` 为只读 getter，无保留状态的进度 setter。实测 `codespec` 外的动画域（`include/aurora/animation/` + `src/aurora/animation/`）对 `repeat`/`loop_count`/`alternate`/`set_value` 零命中；`include/aurora/` + `src/aurora/` 对 `time_scale`/`dilation` 零命中。
- `AnimationController::tick`（`src/aurora/animation/animator.cpp`）首句即 `dirty_ = false`，随后非播放态早退——这是首稿 `set_progress` 机制不成立的直接依据。
- 帧步长有两条入口：`Animator::tick(dt)` 与控件自驱 `Widget::tick_gestures` 家族。`ScrollGlide`（`include/aurora/widget/scroll_viewport.h`）的消费点实测**四处**——`Scroll` / `LazyList` / `GridView` / `PullToRefresh`，各自在自己的帧计时里以墙钟差作裸 dt 推进；`AnimatedValue::tick` / `TimelinePlayer::tick` / `TweenAnimation::tick` 是无 `Animator` 的手动自驱路径。
- `include/aurora/widget/reorderable_list.h` **不消费** `ScrollGlide`，而是第三处私有弹簧：`std::optional<SpringSimulation> spring_active_` + `settle_t_` 累积 + `tick_settle(dt)` 里 `is_settled` 定格，dt 由 `on_tick(now)` 从单调时钟自算，`reduce_motion` 直接落位。它与 `DragToDismiss` 的区别决定了本提案不收口它：落位写的是普通成员 `drag_follow_`（内容坐标位移），**不是 `State<double>`**，故复用公共原语前必须先裁决「给该控件加 State 句柄」还是「给原语加值汇形参」。同文件的 `tick_auto_scroll(dt)` 与 `DragAutoScroll`（`scroll_viewport.h`，`Scroll` / `LazyList` / `GridView` 的 `drag_auto_scroll_`）是速度型滚边，无进度与终态，故列在缩放例外里。
- 控件侧的停手机制独立于 `Animator::has_active()`：`Widget::tick_gestures(now)` 家族用「在途」标志维持每帧计时——实测 `Scroll` 在 `!glide_.active && !drag_auto_scroll_.active` 时置 `needs_gesture_tick_ = false` 后 return，抵达终点那帧才复位 `glide_last_`；`LazyList` / `GridView` 同构，`PullToRefresh` 与 `Dismissible` 亦各自在途自驱。**首版只考虑了 `Animator` 侧的脏位、漏掉这条**：若冻结仅是「进帧但 dt=0」，在途的滑动/弹簧永不抵达终点，计时永不摘除，冻结会退化成满帧唤醒。
- `Animator::add_binding(std::function<void()>)` 登记的 `Binding::owner` 恒为 `nullptr`，`remove(const AnimationController&)` 只按 `owner` 摘除，故该回调不可注销；回调形参不含 dt。`Animator::has_active()` 只看登记控制器的状态，其消费点在 `src/aurora/app/application.cpp` 的 `wait_once` → `decide_wait`（无活跃动画时深睡）。
- `include/aurora/event/gesture.h` 的 `DragToDismiss` 已实现「`std::optional<SpringSimulation>` + `spring_t_` 累积 + `is_settled` 定格并写 `State<double> progress_`」，即首稿所称「完全缺位」的那层封装；`include/aurora/widget/dismissible.h` 经 `dtd_.tick(dt)` 自驱它。§6.7 已把该语义写成规格。
- `SpringSimulation`（`include/aurora/animation/spring.h`）为纯求解器（`value`/`velocity`/`is_settled`/`target`），`is_settled` 的容差默认 `0.01` 且与值同单位域。
- `Keyframes<T>::Stop`（`include/aurora/animation/timeline.h`）仅 `time` + `value`，`value(t)` 线性插值；`Keyframes` 的构造是 `explicit Keyframes(std::vector<Stop>)`（无 initializer-list 构造，故首稿片段 `Keyframes<double> kf{Stop, Stop, Stop}` 不可编译）；`TimelinePlayer::track` 的每段 `Tween<T>` 自带 `Curve`。§6.1 现文明写关键帧「插值严格线性」。
- 导航层 `Hero`/`TransitionLayer`/`NavigatorHost` 已实现并接入帧循环；`NavigatorHost` 的转场绑定注释即自陈「曲线固定 `Curves::ease_in_out()`，逐路由 `RouteTransition::curve` 当前未接线」，`pop` / `pop_to_root` 强制 `Fade`，`TransitionKind`（`include/aurora/navigation/route.h`）仅 `Fade` / `Slide`。`codespec/specification/05-event-navigation.md` §7 只写到 `Route` / `Navigator` / `Router` / `Hero`，导航层接线限制的正文落点是 §7.1，不是 §7.4。
- schema 面现状：`aurora_api.json` 的顶层键是 `widgets` / `enums` / `layout_rules` / `state_patterns` / `error_codes` 等，**不含任何动画类**（`AnimationController` / `Animator` / `TimelinePlayer` / `TweenAnimation` 均零命中），枚举来自 `tools/include/known_enums.h` 的策展表而非自动扫描；`check_api_budget` 的阈值是 75000 tokens，当前约 50445。测试面现状：`tests/unit/` 已有 `utest_animator.cpp`（目标单元 `animator.h`）、`utest_spring.cpp`（`spring.h`）、`utest_timeline.cpp`（`timeline.h`）、`utest_gesture.cpp`、`utest_dismissible.cpp`，`examples/demos/` 已有 `demo_animation.cpp` 与 `demo_dismissible.cpp`；`codespec/CODING_STANDARDS.md` §3.1 的「源—示例—测试 1:1」与 §3.2 `TEST-R3` 的「一个单元头至多被一个 `tests/unit/` 测试声明」据此约束回写形态（`TEST-R3` 不入硬门禁，靠评审）。

未验证边界：本提案未实施，未编译上述拟议 API，未运行 ctest / 性能 / 跨平台验收。「真全局缩放」的漏斗落点清单是按当前调用点穷举的，实施时若发现新的裸 dt 动画推进点须一并纳入（判据 10 的矩阵随之扩行）；`DragToDismiss` 重构是否会牵动 §6.7 之外的既有行为，须在实施批次以该文件的用例逐条取证，不在提案阶段断言。

## 关联限制与后续（不属本提案范围）

下列为与本次能力补齐强相关、但需另开提案的缺口：

- `NavigatorHost` 逐路由 `RouteTransition::curve` 当前未接线，转场曲线硬编码 `Curves::ease_in_out()`。
- `NavigatorHost` 的 `pop` / `pop_to_root` 强制 `Fade`，仅 `push` 走 `RouteTransition::kind`；`TransitionKind` 枚举仅 `Fade` / `Slide` 两种。
- 无声明式 `PageRouteBuilder`（路由即转场描述）封装；当前为 `NavigatorHost` + `Route` 的命令式接法。
- `AnimatedValue::attach` / `TimelinePlayer::attach` 经 `add_binding` 登记的回调 `owner` 为空、`remove()` 摘不掉（既存形态）。本提案新增的可注销 dt 绑定入口不得复制该缺陷，但把 `AnimatedValue` / `TimelinePlayer` 自身的注销收口留在后续，避免本提案顺手改动既有 pimpl 生命周期。
- `Dismissible` 的拖动主轴在实施前就**没有接到底层**：构造函数的 `axis` 形参只转给 `DragToDismiss`（决定手势锁轴与松手裁决），成员 `dtd_axis_` 从未被赋值，故绘制位移与行程标定（读它的 `travel_distance` 与平移矩形两处）恒按 `Horizontal` 走——请求 `DragAxis::Vertical` 时表现为「拖得动、裁决方向对，但位移画在水平轴上、行程按宽度折算」。`describe_static()` 把 `axis` 作为可填属性对外承诺，缺口因此在 schema 面可见。修它要连带裁决「`dtd_axis_` 与 `DragToDismiss` 内部轴谁是真值」，属缺陷修复批次。**该缺口已在后续缺陷修复批次闭合**：裁决取 `DragToDismiss` 内部轴为唯一真值，控件侧副本删除，三条读取路径（跟手取分量、行程标定、绘制平移）统一经新增的 `DragToDismiss::axis()` 只读访问器；判据为 `utest_dismissible` 的轴贯通用例（竖向按高标定行程、纯横向拖不给进度、位移只落 `y`），旧形态下该例三处断言同时转红。
- `ReorderableList` 的落位弹簧（`spring_active_` / `settle_t_` / `tick_settle`）收口到 `SpringAnimation`。阻塞点不是弹簧本身而是**值汇**：该控件落位写普通成员 `drag_follow_` 并靠重排绘制生效，而 `SpringAnimation` 的写回目标是 `State<double>`。两条路各自有代价——给控件加 State 句柄会把它的内部位移纳入响应式图（每次拖拽都经信号，可能牵动重建口径），给原语加「值汇形参」（回调式落点）则弱化「弹簧即插到 State」这一模型的第一读感。该裁决应连同其余同样非-State 落点的控件（若穷举后有第二例）一起定，不塞进本提案。

前三项依赖本提案的 `set_progress`（交互式 / 可中断转场）与分段曲线能力，建议在本提案归档后单独评估；第四、五项是本提案明确划出的收口边界，不为凑齐全仓归零而扩大自身范围；第六项是实施 `DragToDismiss` 回归（判据 26）时就地发现的既有缺陷，本批只改该文件的步长取源与冻结期标脏纪律、未触碰其行为，故判据 26 的对外语义回归结论不受影响。

## 回写落点

以下为实施同批回写清单；提案状态为「已提议」期间不把权威文档改写成代码已经具备这些能力。

- `codespec/specification/05-event-navigation.md`：§6.1 核心类型表补 `AnimationController::repeat` / `set_progress` 与 `PlaybackMode`、`SpringAnimation`、`Stop` 分段 `Curve`（并把该行现有的「插值严格线性」改口径）；§6.2 补 `Animator::set_time_scale` / `time_scale()`、新增可注销 dt 绑定入口与 `remove()` 的配对关系（该节现存的「控制器是控件成员却注册进全局 `Animator` 必须析构时 `remove`」告诫须覆盖弹簧）；§6.3 / §6.4 补「不设 `repeat` 转发、经 `controller()` 设模式」；§6.5 设计约束补缩放边界（只缩放动画时间轴，不缩放事件 / 布局 / 输入 / 定时任务）与 `reduce_motion` 优先；§6.6 注明分段曲线两条路径；§6.7 改写 `DragToDismiss` 的实现口径为「复用 `SpringAnimation`」，四条对外语义保持不变。导航层三条接线限制写进 §7.1 的现状说明（`RouteTransition::curve` 未接线 / `pop` 强制 `Fade` / `TransitionKind` 仅两态），**不在文档里出现提案编号或语义短名**（AGENTS.md §5 规则 14），只写限制本身。
- `codespec/ARCHITECTURE.md`：§4.5 模块地图的动画行补 SpringAnimation（仍落在 spring 头，公共头数不变）；设计不变量节补「循环模式不改写既有一次性语义」「`set_progress` 不改 `status_` 与模式」「动画时间轴的 dt 只有单一漏斗入口，缩放为 0 不得产生常驻帧」；§5.2 事件驱动帧循环的「完全空闲（无脏区 / 无动画 / 无定时任务）→ 无限等待事件」一行须随冻结 / 无限循环语义同步措辞（`repeat(-1)` 与在途弹簧都属「有动画」分支，冻结不得把它变成常驻帧）。
- `codespec/CONCEPTS.md`：概念清单第 13 行（Animation）补 `SpringAnimation`；跨框架对照表补 Flutter `AnimationController.repeat(reverse:)` / `timeDilation`（倒数关系）/ `SpringSimulation` 之上的驱动层 / `TweenSequence` 每段 `curve`，并更新既有手势驱动行以反映 `DragToDismiss` 收口到公共弹簧封装。
- `codespec/GUIDELINE.md`：补可编译配方（循环呼吸态、`set_time_scale` 调试慢放、手势 `set_progress` 驱动、弹簧 `animate_to`、关键帧分段曲线）；§29 Timeline 编排与 §38 滚动增强里「自驱动 tick，不占 `Animator`」的表述须按新漏斗口径改写（这些路径现在也受缩放约束），否则文档与代码漂移。
- `codespec/SPECIFICATIONS.md`：该文件的需求清单没有动画域条目，也没有独立的「动画特性表」；本提案不在这里发明需求 ID。仅在确需登记时按需求 ID 体系新增 `SPEC.FEAT.ANIMATION.*` 行并同时补其规格落点列（`check_codespec_xref` R5 会校验该行可达），否则不动该文件——「无隐式动画」「无路径形变」已在 `codespec/specification/05-event-navigation.md` §8.1 与 §6.5 有权威表述，不重复登记。
- `codespec/manual-test/08-animation.md`：补循环 / 往复、时间缩放、手势进度、弹簧收敛、关键帧分段曲线用例；用例号从现有最大值续排（`TC-ANIMATION-008` 起，禁跳号，`check_manual_test_format` 把关），并补执行记录。该文件 §1.1 现写「动画层（4 个公共头）」，本提案不新增公共头故计数不动；若实施中改为新增头，须同步该计数、`codespec/ARCHITECTURE.md` §4.5 与 `tools/check/umbrella_manifest.txt`。
- 测试与 demo 面（不新增文件，扩展既有同源文件以守住 1:1）：`tests/unit/` 下 utest_animator.cpp 加 `repeat` / `set_progress` / `time_scale` / 新绑定入口与注销用例；utest_spring.cpp 加 `SpringAnimation` 收敛停帧、reduce_motion、注销与悬垂用例；utest_timeline.cpp 加 `Stop` 分段曲线、段归属、零宽段与排序携带用例；utest_gesture.cpp 与 utest_dismissible.cpp 承接 `DragToDismiss` 重构回归；缩放覆盖矩阵的自驱动路径用例落在 `tests/unit/` 既有同源文件里：utest_scroll.cpp、utest_lazy_list.cpp、utest_grid_view.cpp、utest_pull_to_refresh.cpp（glide 四消费点）与 utest_reorderable_list.cpp（`tick_settle` 落位弹簧 + 自动滚边反向项）；utest_scroll_viewport.cpp 承接 `ScrollGlide` 自身的 dt 口径。demo 面扩展 examples/demos 下的 demo_animation.cpp（循环、慢放、手势定位、弹簧、分段曲线各一个可见场景），Dismissible 侧复用同目录的 demo_dismissible.cpp 不新建。若实施决定新增公共头，则必须按同一约定配一个新的 utest 与 demo 对，而不是往上述文件里塞第二目标单元。
- schema 与注释面：同步公共头注释（AGENTS.md §5 规则 12 的 `///` / `@brief` / `@param` / `@return` 形态，含新增枚举项注释）；伞头与 `tools/check/umbrella_manifest.txt` 不动；根目录的 aurora_api.json 经 `--target aurora_api_json` 重建，确认动画类未入表、内容不变（不得据此宣称 schema 已反映本提案能力）；PlaybackMode 是否进 tools/include 下的 known_enums.h 按上一节判据定案。
- 仓库根变更记录（CHANGELOG）：`[Unreleased]` 的 `Added` 节记动画五项能力与 `DragToDismiss` 收口（`DragToDismiss` 的对外语义不变，故归 `Added`/`Notes` 而非 `Breaking`；若实施中出现签名或头文件变动须改记 `Breaking` 并给迁移路径），本提案不自行决定升版。

实施开始后状态改为「实施中」，全部契约与门禁取得证据、回写完成后改为「已归档」。不得用文档检查通过代替实现验收。

## 实施记录与偏离（落码期回写）

1. **折算点按实测穷举为十一处，判据 10 的矩阵随之扩行**：`Animator::tick`（`src/aurora/animation/animator.cpp`，成员 `scale_frame_dt`）、`TimelinePlayer::tick`（同文件）、`AnimatedValue::tick` 与 `TweenAnimation::tick`（`include/aurora/animation/animator.h`）、`SpringAnimation::tick`（`include/aurora/animation/spring.h`）、六处控件自驱入口（`scroll.h` / `lazy_list.h` / `grid_view.h` / `pull_to_refresh.h` 的收位滑动、`reorderable_list.h` 的 `tick_settle`、`skeleton.h` 的微光相位）。首稿矩阵只列 glide 四消费点 + `ReorderableList` + `DragToDismiss`，**`Skeleton` 的相位与三条手动自驱路径是穷举后补进去的行**；`DragToDismiss` 收口到公共原语后不再单列，经 `SpringAnimation::tick` 计入。另有第十二处调用是 `dismissible.h` 的 `tick_gestures` 冻结判定，只取折算值作「本帧是否推进」的谓词、不作为步长，故不构成第二重缩放。
2. **折算下沉点定在消费点，求解器保持纯时序**：`ScrollGlide::tick` / `AnimationController::tick` / `SpringSimulation` 都不感知倍率。该取舍由 `utest_scroll_viewport.scroll_glide_tick_is_immune_to_current_animator_time_scale` 钉住，并由变异 M06（在 `ScrollGlide::tick` 与消费点两层各乘一次）实证可转红——否则 2.0 档静默变成 4.0 档。
3. **判据 11(b) 的实现形态与提案首稿不同，判据正文已按实现改写**：首稿写「冻结时摘掉控件侧每帧计时」，落码否决——全局缩放入口拿不到逐控件指针，而摘除在途标志就没有续跑点（解冻只能等下一次输入）。实现取「计时标志保持武装 + 冻结帧不推进、不写回、不标脏、不请求帧」，解冻由 `set_time_scale` 置一枚重挂请求、`Application::wait_once` 消费出「下一帧不深睡」，那一帧各控件照常 `tick_gestures` 并自行续帧。判据形式相应改为重绘请求计数（`Widget::on_dirty`），并新增 `Animator::has_active()` 在倍率为 0 时恒假这条对应判据。该形态不需要树遍历入口，故影响面一行里「视冻结重挂形态而定」的 `include/aurora/widget/widget.h` 本批未改动。
4. **本批唯一改动既有行为次序的一处（缺陷修正）**：`AnimationController::tick` 的零步长守卫首版排在 `reduce_motion` 短路之前，使「冻结帧 + 减弱动态效果」下控制器不落端点，全量 ctest 的既有 `utest_accessibility.reduce_motion_snaps_controller_to_endpoint` 实测转红。守卫改到短路之后（无障碍短路不使用 dt，「一步到位」本就是一帧到位，没有可被冻住的东西），次序与 `SpringAnimation::Payload::advance` 对齐——两个驱动件对同一无障碍档给出同一形态。判据 13 的新用例即锚定该对齐。
5. **漏斗只做乘法、不夹取**：入参既可能是调用方声明的步长（`Animator::tick(1.0)` 做「一步到位」是既有契约），也可能是控件自驱路径的墙钟差（既有 `Skeleton` 用例以三秒差推进相位），在此夹取会同时改掉两者语义、并让 `1.0` 档在掉帧时被静默改速。非法倍率改在 `set_time_scale` 入口折成 `0.0`（负值 / NaN / inf 同档）；`Animator::current()` 为空时自由函数恒等返回，故无头 golden 逐字节不变。
6. **`set_progress` 的脏位分工按「推进脏 / 外部脏」两位实现**：`tick()` 入口只清推进脏，`dirty()` 取两位之或，帧末 `clear_dirty()` 双清。首稿只说「外部脏位或进 `dirty()`」未写清入口清脏的范围，落码时该分工是判据 16（当帧写回）与判据 17（静止控制器不得使 `has_active()` 转真 / 空闲帧不写回）同时成立的前提，已回写进 `include/aurora/animation/animator.h` 的头注与 `codespec/specification/05-event-navigation.md` §6.1。
7. **可注销 dt 绑定入口复用既有容器，不新开一条注册表**：`Animator::add_frame_binding(fn, owner)` 向同一 `on_tick_` 向量登记 `Binding{.owner = &owner, .fn = {}, .dt_fn = std::move(fn)}`，`remove(owner)` 的摘除口径因此对两类绑定天然一致。既存 `add_binding(std::function<void()>)` 的 `owner` 恒 `nullptr` 这一缺陷本批未动（改它要连带裁决 `AnimatedValue` / `TimelinePlayer` 的注销收口，仍列「关联限制与后续」第四项）。
8. **`PlaybackMode` 不入 `tools/include/known_enums.h`**：该表只收「可作为属性值 / 工具入参填写」的取值域，`PlaybackMode` 与既存的 `AnimationStatus` 同为只读判别枚（`repeat(int count, bool alternate)` 收计数与布尔，不收枚举），入表会把读出来的状态提示成填进去的值。理由就地写在该表动画段的注释里；`tests/integration/itest_known_enums.cpp` 按属性类型对账，未改。
9. **回写落点微调（两处提案未列、不改即漂移）**：`codespec/specification/04-widget.md` §3.3 的「收位是短滑动」与 `codespec/specification/08-tooling.md` 的 E2E 收敛表行都写着「自驱动 `tick_gestures`，不占 `Animator`」，各补一句「步长在消费点经 `scaled_frame_dt` 折算一次」并指向 §6.2，否则文档与代码口径相反。`codespec/GUIDELINE.md` §38.1 的「自驱动 `tick_gestures`，不占 `Animator`」一句就地补上折算口径（§29 经核对没有同类表述——其 Timeline 配方走 `Animator::tick` 主路径，故未改），并新增 §46 五段配方。`codespec/SPECIFICATIONS.md` 未动——本批不发明需求 ID。`codespec/manual-test/08-animation.md` §1.1 的「动画层（4 个公共头）」计数不变（本批未新增公共头，`tools/check/umbrella_manifest.txt` 与伞头同不动）；新增的 `tests/support/animation_fixture.h` 是夹具而非被测单元，不触发「一源文件对一 `utest_*` 与一 `demo_*`」约定。`examples/demos/demo_animation.cpp` 承载五组新舞台，`demo_dismissible.cpp` 未改（`DragToDismiss` 对外语义不变，复用既有舞台即可）。
10. **变异自证有三档的实际转红集合与首稿预期不同，两处已同步改写、不留未兑现的预期**：M10（摘掉 `tick()` 入口清脏）不使 `animator_bindings_write_only_on_dirty_frames` 转红，转红的是 `controller_forward_ticks_to_completed` 与 `controller_reverse_reset_stop_semantics`；M12（收敛不经 `is_settled`）只使 `spring_animation_settle_time_follows_tolerance` 转红，`..._converges_then_settles_exactly_once` 因不断言收敛时长而不转红；M15（段曲线归末点）除判据 28 的两例之外还命中判据 31 的 `animator_keyframe_binding_applies_segment_curves`。判据行与「门禁与变异自证」的契约表按实测各改一遍，负向发现写明。另有九条判据（13/14/18/23/24/26/27/29/30）未单设变异档，理由记在「临时变异 → 实际转红」表后，不为凑数造无关变异。
11. **人工用例与像素面按实申报为未取证**：`TC-ANIMATION-008` 至 `TC-ANIMATION-012` 未在真实窗口按人工步骤执行（其三、四节留空），观测量由单测的等价断言覆盖；本批不新增 golden，也不改渲染路径——「判据 12 的 golden 不破」以全量 ctest 的既有 golden 套件继续绿为证，不以新截图为证。冻结期「不再满帧唤醒」按帧请求计数取证，未测绝对 CPU 曲线。
12. **实施中撞到两条文档口径缺口，回写落在 `codespec/ARCHITECTURE.md`（原「回写落点」未列）**：其一，`scroll_viewport.h` 不是 widget 单元头，其单测没有属性键 / schema 可对账，须明文排除出 §3.1 的「源—示例—测试 1:1」映射，否则下一批会为了凑齐 1:1 而给它造一个 demo；其二，`tools/include/known_enums.h` 的入表判据原先只散在该表注释里，现把「只读判别枚不入表」提为架构条目，与 `itest_known_enums.cpp` 的对账口径（只对账表内枚举 ↔ 属性类型，不查覆盖率）分开写明。
13. **lint 整改排在变异自证之后，是保行为的改名与补括号**：`readability-identifier-naming` 的 31 条要求把本批新写的常量按本仓约定更名（`SpringAnimation::Payload` 的字段一并去掉尾下划线、与该家族既有的两份载荷同形），另有 4 条补显式括号与 3 条 demo 侧写法改写，均不改算式、不改控制流、不改任何公共签名。上一节的 22 档变异与 43 条转红实例是在更名前的树上取得的，本条记录该先后关系；更名的行为不变性由同一套 450 例复跑绿（见「重建与门禁」）承担，未重跑变异。公共形态一节所载的构造函数形参（`const State<double> &`）与 `set_range` / `target()` / `clock()` 三枚读点，以及载荷字段名 `elapsed`，是按落码后的头文件回填的——首稿此处写作「非拥有引用」并用了 `t_sim_` 一名，与实现不符处已就地改正而非留作偏离。
