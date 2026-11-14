# CHG-077 路由转场按配置驱动

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-077 |
| 提出日期 | 2026-11-14 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `NavigatorHost` 的转场驱动改为**每次切换重建**控制器与补间（`begin_transition` 一处），使 `RouteTransition::curve` / `duration_seconds` 真正生效；`pop()` / `pop_to_root()` 不再硬编码 `Fade` + 0.3 秒，改读本轮被弹出路由的转场配置（未声明 `animated` 时保留历史默认淡出）；`route.h` 新增声明式 `PageRouteBuilder`（薄封装，把「页面 + 转场」写成一次构造）；`TransitionKind` 仍为 `Fade` / `Slide` 两态，不扩枚举；零新增错误码、零新增主题令牌、`aurora_api.json` 的属性面零变化（`PageRouteBuilder` 不是控件）；影响判据面：`utest_navigator_host`、`utest_route`、`codespec/specification/05-event-navigation.md` §7.1 的「现状限制」段、`CHANGELOG.md` 的 `Added` / `Fixed` 条目。 |

## 动机

1. **`RouteTransition::curve` 是声明位，不是能力。** `begin_transition` 只在首次转场时 `bind` 一次，补间固定 `Curves::ease_in_out()`，此后所有路由共用首条的时长与曲线。应用写 `RouteTransition{.curve = Curves::ease_out()}` 不会得到任何效果差异——这类「字段存在但无消费者」的声明位比缺字段更糟：它让调用方以为已经配置成功。

2. **`pop()` 的方向与形态由代码写死，路由说了不算。** 推入时声明 `Slide` 的页面，返回时一律淡出；声明 0.6 秒的页面返回用 0.3 秒。用户看到的「推入滑、返回淡」不是设计选择，而是未接线。

3. **重建绑定是唯一不引入重复登记的接法。** `Animator::bind` 内部调 `drive(c)` 把控制器压进登记表，连续 `bind` 同一控制器会重复推进；而 `Animator::remove(c)` 会连同该控制器的全部绑定一并摘除。故「按本次配置重建」的形态必须是 `remove` → 重新构造控制器 → `bind`，不能只换补间对象。

4. **声明式入口只差一层薄封装。** 现形是 `NavigatorHost` + `Route` 的命令式接法：调用方每次都要先写 `RouteTransition` 再塞进 `Route`。规格已把「没有 `PageRouteBuilder`」登记为限制；补的是「页面 + 转场」一次写成的构造形态，不是新语义——产物仍是一个 `Route`。

## 变更内容

### 1 转场驱动按配置重建

`NavigatorHost::begin_transition` 改为接收整份 `RouteTransition`（原来只收时长）：

- 时长取 `max(duration_seconds, 1e-6)`；曲线取 `tr.curve`；类型取 `tr.kind`。
- 已绑定时先 `anim_.remove(ctrl_)`，再以本次时长构造新控制器并 `bind(ctrl_, Tween<double>{0.0, 1.0, tr.curve}, progress_)`。`remove` 按控制器地址摘除，`ctrl_` 是成员、地址恒不变，故旧绑定必被摘净，不会重复推进。
- 起播仍走 `ctrl_.forward(0.0)`（进度归零并进入 `Forward`），与本批前一致；`mark_needs_layout()` 保留。
- 析构摘除的口径不变：`bound_` 为真时 `anim_.remove(ctrl_)`。

### 2 `pop` / `pop_to_root` 读被弹出路由

- `pop()`：取 `nav_.current().transition()` 作为本轮配置。`animated` 为真则按其 `kind` / `curve` / `duration_seconds` 合成；为假则用历史默认（`Fade` / `Curves::ease_in_out()` / 0.3 秒）。
- `pop_to_root()`：同一条规则，配置取当前栈顶路由（被丢弃的那一页），因为它才是「正在离开」的页面。
- 保留默认淡出的理由：本批前 `pop` 恒淡出，若改成「未声明即无动画」，所有既有应用返回时都会从「淡出」突变为「瞬切」，属未经请求的行为变更。默认值只是**缺省**，声明了就以声明为准。
- 反向播放（Slide 从右侧退场而非继续左出）不在本批：`TransitionLayer` 只按 `kind` 取一次方向，加方向位要动它的绘制与既有像素基线，属独立小批。

### 3 声明式 `PageRouteBuilder`

`route.h` 新增：

- `PageRouteBuilder(Node root, std::string name = "")`；
- 链式 `set_animated(bool = true)` / `set_kind(TransitionKind)` / `set_curve(Curve)` / `set_duration(double seconds)`；
- `transition()` 读回当前配置，`build()` 交出 `Route`。

它只持有 `Node` + `std::string` + `RouteTransition` 三个值，不做延迟构建（本库路由的页面子树在构造期即已成形，没有 Flutter 那种「builder 等到入栈才调用」的需求；加延迟构建会引入「谁持有环境」的新问题，而当前没有消费方）。

### 4 不变面

- `TransitionLayer` 的绘制、`Hero` 配对、`Navigator` 栈语义、`open_uri` 的「无转场重建」、`push_replacement` 的转场处理全部不动。
- `Navigator::pop` 的拒绝语义（栈深 ≤1 返回 false）不变。
- `Route` 的字段与构造签名不变；`RouteTransition` 的字段与默认值不变。

## 验收判据

1. 曲线生效：以 `RouteTransition{.animated = true, .curve = Curve{CurveKind::EaseIn}, .duration_seconds = 0.4}` 推入，tick 0.2 秒后进度为 `0.5³ = 0.125`（`Curve{EaseIn}` 的定义值），而不是线性 0.5；同一宿主紧接着再推入一条 `Curves::linear()` 的 0.4 秒路由，tick 0.2 秒后进度为 0.5。两条读数在同一次 `Animator` 上取得，证明重建后的绑定没有被首条曲线粘住。
2. 时长逐条生效：先推 0.1 秒页（tick 0.1 后进度 1、`Animator` 转闲置），再推 0.6 秒页，tick 0.3 后进度约 0.5 且 `has_active()` 为真——旧实现会把第二次也压成 0.1 秒并已闲置。
3. `pop` 读配置：推入 `Slide` 页后 `pop()`，展示层出现 `TransitionLayer`，且在进度 0.5 处左半区是旧页颜色、右半区是新页颜色（滑动合成）；推入未声明 `animated` 的页后 `pop()`，进程到 0.5 时中心像素是两页混色（淡出缺省仍生效）。
4. `pop_to_root` 同规则：三层栈（根 + `Slide` 页 + 顶层未声明页）上 `pop_to_root()` 时按栈顶路由的配置合成（未声明 ⇒ 淡出），且 `depth() == 1`。
5. `PageRouteBuilder`：`PageRouteBuilder(node, "detail").set_animated().set_kind(Slide).set_curve(Curve{CurveKind::EaseOut}).set_duration(0.5).build()` 得到的 `Route`，其 `name()` / `transition()` 四个字段逐项等于链式设置的值；未调用 `set_animated` 时 `animated` 为假（默认不动画），与 `RouteTransition{}` 的缺省一致。
6. 绑定不重复登记：连续做 5 次带转场的切换后 `anim` 的活跃控制器数仍为 1（用「tick 一次后进度只前进一次」间接判定：每次切换后 tick 到 1 都能恰好停下，不多推进）；宿主析构后 `anim` 无残留（既有 `utest_navigator_host` 的摘除用例继续通过）。
7. 判据非空转须真注入变异：① 把 `begin_transition` 退回「只在首次 bind」⇒ 判据 1 的第二条读数与判据 2 转红；② 把 `pop` 的配置读回写死 `Fade` ⇒ 判据 3 的滑动分支转红；③ 令 `pop` 在未声明时也不转场 ⇒ 判据 3 的淡出分支转红。每处记录「改了什么、哪条转红、还原后是否回绿」。

    - 判据 7 变异自证记录：① 已执行——把 `begin_transition` 退回「`!bound_` 时才 bind、曲线写死 `Curves::ease_in_out()`」，重建后 `utest_navigator_host` 恰好两例转红（`host_transition_curve_is_taken_from_each_route`、`host_transition_duration_is_taken_from_each_route`），其余 14 例不受影响；还原后 16/16 全绿。② 已执行——把 `departing_transition` 的读回改成**无条件**返回 `Fade`（曲线与时长仍取声明值），`host_pop_uses_departing_route_kind` 转红（两条半区像素断言都失败），`host_pop_without_declaration_keeps_default_fade` 与其余 14 例不受影响。③ 已执行——给 `pop()` 的起播条件加一个合取项 `nav_.current().transition().animated`，`host_pop_without_declaration_keeps_default_fade` 与 `host_pop_transitions_and_completes` 两例转红（展示层从 `TransitionLayer` 退化为 `Provider`、`Animator` 无活跃控制器、中心像素为纯旧页），`host_pop_uses_departing_route_kind` 不受影响。②③ 还原后 16/16 复绿。

## 回写落点

- `codespec/specification/05-event-navigation.md`：§7.1 删去「转场侧尚未接线」三条限制中已兑现的两条（曲线硬编码、`pop` 强制 Fade），改为按配置驱动的口径并写明缺省值；`TransitionKind` 仍两态的限制保留；补 `PageRouteBuilder` 的构造与链式写法；§7.4 补一句「反向播放未实现」。
- `codespec/specification/04-widget.md`：若该文件登记 `NavigatorHost` 的控件条目，同步其转场说明（无条目则不动）。
- `codespec/SPECIFICATIONS.md`：特性表无导航行，本批不新增行；只要 `codespec/specification/05-event-navigation.md` 的既有锚点不变，该表无需改动。
- 根目录 CHANGELOG.md：`Added` 记 `PageRouteBuilder`；`Fixed` 记「逐路由曲线与时长不生效」「`pop` / `pop_to_root` 强制淡出」两条。

## 实施记录

- 落码：`include/aurora/navigation/navigator_host.h`（按配置重建绑定、`pop` / `pop_to_root` 读离场路由）、`include/aurora/navigation/route.h`（`PageRouteBuilder`）。
- 用例：`utest_navigator_host` 新增四例（曲线逐条、时长逐条、`pop` 按离场路由形态、未声明时保留缺省淡出）、`utest_route` 新增两例（链式配置生效、缺省与 `RouteTransition{}` 一致）。
- 文档：`codespec/specification/05-event-navigation.md` §7.1（转场配置逐条生效、`pop` 口径、`PageRouteBuilder`）与 §7.4（方向不翻转）、根目录 CHANGELOG 的 `Added` / `Fixed`。
- 收尾：回写落点逐项核实——§7.1 已改为按配置驱动并写明缺省值与 `PageRouteBuilder` 写法，§7.4 已记「出栈不翻转方向」；`codespec/specification/04-widget.md` 无 `NavigatorHost` 控件条目故不动；`codespec/SPECIFICATIONS.md` 特性表无导航行故不动；CHANGELOG 的 `Added`（`PageRouteBuilder`）与 `Fixed`（逐路由曲线时长、`pop` 强制淡出）两条已记。代码侧缺口为 0（反向播放按 §7.1 归独立小批），状态随批次流转（已提议 → 实施中 → 已归档）。
