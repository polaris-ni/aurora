# CHG-001 控件挂载补齐与卸载对称（运行期追加子树 + 换宿主重挂）

| 项目 | 内容 |
|:---|:---|
| 变更编号 | CHG-001 |
| 提出日期 | 2026-10-06 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | 控件生命周期与挂载语义、响应式订阅、`OverlayHost` / `TabBar` 公共入口、`BuildContext` 数据面 |

## 动机

两条腿都不成立，且症状与「代码没写」同形、无日志，归因成本极高。

**其一，运行期追加的子树不经 `mount(ctx)`。** `Container::on_mount` 只在容器自身被挂载时遍历一遍
`children_`；而 `OverlayHost::add_overlay` 与 `TabBar::add_tab` 的函数体只有「push + 标脏」，
不带任何 `mount` 调用。`Window::present_root` 的条件是 `!root_mounted_ || root_changed`——根不变即
永不重挂整树。于是「根长期存活 + 运行期加子树」这一常见形态（浮层、Toast、动态 tab、动态面板）里，
新子树从未进入挂载序列：覆写了 `on_mount` 的控件收不到挂载，主题不跟、闪烁与定时档不启动、外部信号
订阅不注册。对照本仓既有五个挂载点（`LayoutBuilder` / `BreakpointBuilder` / `LazyList` / `GridView` /
`Splitter`·`Drawer`·`Timer`），形态都是「父侧持有 `on_layout` 的那份 ctx，新增子树即时 `mount(ctx)`」——
`OverlayHost` / `TabBar` 是这条既有语义的两个漏点，不是另一种设计。

**其二，`mounted_` 只有置真路径，无退役路径。** `Widget::mount` 首句 `if (mounted_) return;` 紧接
`mounted_ = true;`，全仓无任何置 `false` 的点。订阅随 `effects_` 存活到控件析构，于是一只被摘出树的
控件仍持着主题 / 信号订阅，被标脏时脏标记沿已断的父链被丢弃。另一面：一只实例被摘下后挂进另一份
`BuildContext`（另一窗口、或跨换页重挂同一视口），因 `mounted_` 恒真而跳过 `on_mount`，新 ctx 永远拿不到，
而旧 ctx 派生的订阅仍活着——要么陈旧、要么重复。

## 变更内容

1. **`BuildContext` 新增 `host_id`（`std::uint64_t`，缺省 0）**：宿主身份，仅用于挂载同一性判据，
   **只比较不解引用**。由 `Window` 构造期取唯一值并在 `prepare_context` 写入，沿 `Provider` 子环境、
   转场层、容器逐层透传不变。选它而非 `ctx.env` 指针的理由是实测约束：`NavigatorHost::rebuild_display`
   每轮用**新的** `Provider` 重新包裹页面，`Provider::on_mount` 以 `child_env_` 挂载子树 ⇒ 直接比环境
   指针会把「转场复用同一页实例」误判成换宿主，`on_mount` 反复重触发，直接违反既有幂等保护。`0` 表示
   「未声明宿主」（无头渲染、单元测试），此时所有 `0` 互判为同一宿主，既有行为逐位不变。
2. **`Widget::unmount()`（无参，public）**：置 `mounted_ = false`、清空 `effects_`（真正退订）、复位手势
   tick 位，并以**挂载期记录的那份 ctx** 派发新增的 `virtual on_unmount(const BuildContext&)`（protected，
   与 `on_mount` 对称）。未挂载即调用是幂等空操作，不留半个退订状态。
3. **`Widget::mount` 幂等判据换语义**（D3/D4）：已挂载且 `host_id` 相同 ⇒ 跳过（转场复用同一实例的既有
   保护原样保留）；已挂载但 `host_id` 不同 ⇒ 先 `unmount()` 再挂载。**不比较 `scale_factor` / 主题等值**——
   值相等不等于同一宿主。
4. **运行期追加子树的补挂做成共享件**：基类新增「有待补挂子树」位与 `virtual flush_pending_mounts(ctx)`
   （缺省空实现），由 `Widget::layout` 入口在缓存判定之前消费（一次 bool 判定，未登记时零开销）；
   `Container` / `TabBar` 覆写为「遍历自己持有的子项逐个 `mount(ctx)`」。`OverlayHost::add_overlay` 与
   `TabBar::add_tab` 只登记，不要求调用方自备 `BuildContext` 或自行 `mount`——生命周期责任不外推给应用侧。
5. **`on_unmount` 与既有 `on_mount` 逐处对称**：`Container` / `SingleChild` / `TabBar` / `Provider` /
   `Splitter` / `Drawer` / `TransitionLayer` / `NavigatorHost` 递归各自持有的子树；`Timer` 取消挂载期
   注册的 interval 句柄；`LayoutBuilder` / `BreakpointBuilder` 释放挂载期建的 builder effect；
   `Dropdown` 清缓存的 `env` 指针。`Lifecycle` 的回调契约**不动**（那份 `on_unmount` 是析构回调，
   与控件级钩子各自独立）。
6. **容器不得自动代调 `unmount`**：`remove_overlay` / `remove_tab` 不代调。摘除时刻容器无从判断这只子件
   是否「活在容器之外仍被持有」（`SingleChild::child_nodes_mut()` 返回 `child_view_mut_` 即 `child_` 的拷贝
   副本；实测无外部持有时 `Container` 报 `use_count() == 1`、`SingleChild` 报 2），任何按引用计数自动退订
   的写法都会误伤仍存活的子树。卸载只由持有者显式发起，或由换宿主重挂路径内部发起。

破坏性说明：`mount` 的幂等判据由「只看是否已挂载」改为「已挂载且同宿主」，语义放宽方向为**多一次重挂**，
不改变任何既有「同宿主重复 mount 只订阅一次」的效果。新增项均为增量 API。

不在本次范围：`LazyList` / `GridView` 的 `live_` 回收销毁策略、`OverlayHost` 的可见性与焦点新语义、
`Lifecycle` 的回调契约、`ImageWidget` / `Sparkline` / `InspectorPanel` / `VideoPlayer` 挂载期启动的异步
加载与播放资源。

## 验收判据

1. `OverlayHost` 根已 `present_root` 之后运行期 `add_overlay` 一棵含「覆写 `on_mount` 置计数」控件的子树，
   一次布局之后计数恰为 1，且该控件在后续布局里读到宿主注入的新主题值。
2. `TabBar` 运行期 `add_tab` 的内容子树同上为 1。
3. 同一 widget 先挂进宿主 A 并布局，再摘下挂进宿主 B 并布局：`on_mount` 被调用 2 次，**净订阅数不增长**
   （B 侧生效、A 侧已退），且 A、B 两宿主的 `scale_factor` / 尺寸 / 主题刻意相同、只有 `host_id` 不同。
4. 同一 widget 在同一宿主的同一 ctx 上被重复追加 / 重复 `mount`：`on_mount` 仍只 1 次。
5. 未覆写 `on_mount` 的既有控件：非 e2e 全量测试与像素 / 派发用例逐位一致，无差分。
6. `Widget::unmount()` 在未挂载状态下调用幂等：不崩、不留半个退订状态。
7. 负向判据：`remove_overlay` 之后被摘下的子树仍处于挂载态（`on_mount` 计数不因摘除而回退）——
   容器若代调 `unmount` 即视为越界实现。

变异自证（发出方按此复核）：去掉补挂 ⇒ 判据 1 / 2 转红；保留补挂但去掉重挂前的 `unmount` ⇒ 判据 3 的
「净订阅数不增长」转红；把同一性判据改成值相等 ⇒ 判据 3 在两宿主值相同的构造下转红；让容器在
`remove_overlay` / `remove_tab` 里自动代调 `unmount` ⇒ 判据 7 转红。

## 回写落点

- `codespec/specification/04-widget.md`（§2.3 生命周期与虚拟回调：`on_unmount` 钩子、`mount` 幂等新语义、
  运行期追加子树的补挂时机与「容器不代调卸载」的负向契约）
