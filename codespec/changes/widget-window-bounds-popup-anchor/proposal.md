# CHG-004 `Popup` 内容控件的窗口盒补锚点平移（`anchor_` 对 `window_bounds()` 不可见）

| 项目 | 内容 |
|:---|:---|
| 变更编号 | CHG-004 |
| 提出日期 | 2026-10-07 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `Widget` 公共几何面（`window_bounds()`）、`Popup` 坐标系契约、无障碍语义树与平台桥的浮层内容读数 |

## 动机

**`window_bounds()` 对 `Popup` 内容恒返回「未加锚点」的盒。** `Widget::window_bounds()`（`src/aurora/widget/widget.cpp`）沿 `layout_parent_` 链自叶向根累加每层的 `Node::bounds().origin`、父 `Modifier` 的内容平移与滚动修正，递推式与 `Container::on_paint` 的下降式逐字同构。但 `Popup` 的内容并不走这条下降式：`Popup::on_layout` 把内容子盒的 `origin` 钉在 `{0,0}`（浮层不参与父布局），`anchor_` 只在 `Popup::on_paint` 与 `Popup::on_hit_test_chain` 两处各自施加。基类递推经过 `Popup` 这一层时看到的是「origin 为零的子盒」，看不到 `anchor_` ⇒ 挂在 `Popup` 里的控件读数少一个锚点平移。

**读数差值恰是锚点。** 实测（随搜索浮层的集成场景，2026-10-07）：输入框 `window_bounds().origin` 为 `{8,11}`，同一时刻真实派发盒为 `{20,23}`，差值 `{12,12}` 与该 `Popup` 的 `anchor_` 逐位相等。

**症状落在事件链上，不是「读数不好看」。** 消费侧按 `window_bounds()` 的读数取点，点的位置与控件真实位置错开一个锚点 ⇒ 点不到控件。表现为「`type()` 的字符被吞」：事件未派到 `TextInput`，而是派到了命中链上更浅的一环；基类 `Widget::on_text_input` 缺省实现即 `e.is_handled = true`，吞掉时不报错、返回 true，故失败完全静默。

**绘制与命中两侧本身是对的。** `Popup::on_paint` 用 `content_box{origin = anchor_}` 下传、`on_hit_test_chain` 用 `content_box.contains(global)` 判定，两侧口径一致 ⇒ 真实派发盒确实在 `anchor_` 上。缺的只是事后查询这一条腿。故本提案**不改** `Popup` 的绘制与命中行为，只补查询侧。

**与 CHG-003 的关系**：CHG-003 引入 `window_bounds()` 并以 `Widget::scroll_content_offset` 承载**加性**修正（`Scroll` 把内容写在内容坐标系，须扣 `offset_y_`）。`Popup` 需要的不是加性修正而是**替换性重映射**——`anchor_` 是全局坐标（`Popup` 的两条路径都从 `bounds.origin + local` 起算后与 `anchor_` 盒比较，全程不参与 `Popup` 自身在树中的位置），故经过该层的累计量必须被 `anchor_` **替换**而非叠加，且其上祖先链一律不再参与。这两语义不可由同一个钩子承载。

## 变更内容

### 公共面新增

`Widget` 新增一个 **protected virtual** 坐标重映射钩子（`widget/widget.h`）：

```cpp
[[nodiscard]] virtual auto child_content_origin() const -> std::optional<Point> { return std::nullopt; }
```

语义：**本控件作为宿主时，其子树的窗口盒基准被重映射到哪个绝对坐标**。返回 `std::nullopt`（缺省，绝大多数控件）表示子树沿父链正常递推——缺省路径与改动前逐位等价。返回非空值表示「本控件子树内任一后代 `W` 的窗口盒原点 = 本入口返回值 + `W` 相对本控件内容盒原点的偏移」，上溯到此**终止**。

与 `scroll_content_offset` 的分工写死在 `@brief` 里：后者是加性修正（滚动量），本入口是替换性重映射（坐标系基准），两者不可互相代偿。

### `window_bounds()` 递推改动

`Widget::window_bounds()`（`src/aurora/widget/widget.cpp`）的逐层上溯循环中，每层先问一次 `parent->child_content_origin()`：返回非空值即以它替换已累加量并终止上溯；返回 `std::nullopt` 则维持既有递推（加 `cb.origin` + 滚动修正 + Modifier 内容平移）。既有的 `show == false` / `has_measured_` 负守卫与「尺寸取自父侧 `Node`」的口径不动。

### `Popup` 覆写

`Popup` 覆写该钩子，返回 `anchor_`；关闭态（`!open_`）与无子节点时返回 `std::nullopt`，此时内容未被布局、`content_size_` 为零盒，维持既有递推即可。`Popup::on_paint` / `on_hit_test_chain` / `on_layout` **一行不改**。

### 注释改口

- `Widget::window_bounds()` 的 `@brief` / `@note` 补上「途经坐标系重映射宿主（如 `Popup` 按 `anchor_`）时以宿主的重映射基准替换累计量」，并把它与 `scroll_content_offset` 的分工写明。
- `Widget::paint_bounds()` 的 `@warning` 里「其它覆盖绘制 / 离屏合成的控件（`Popup` 按 `anchor_`…）」一句保留——那是绘制盒的既有限制，本次不动绘制读数。
- `codespec/specification/04-widget.md` §6.3 补「坐标系重映射宿主」一节；`codespec/specification/05-event-navigation.md` §3.2.3 补三读数在 `Popup` 下的关系。

### 破坏性说明

**纯新增 + 修正一处错误读数，无破坏性变更。** 不删不改任何既有入口签名，既有调用点零改动。唯一行为变化是 `Popup` 内容控件的 `window_bounds()` 从「少一个锚点」变为「与派发链同源」——这是缺陷修正，不是语义变更，方向与 CHG-002 对 `HitNode.origin` 的修正同构。`Popup` 的绘制与命中行为逐位不变。绘制路径不引入任何新查询（钩子只在 `window_bounds()` 里被问，不进每帧绘制），故滚动吞吐 / 像素门禁不受影响。

## 验收判据

1. `popup_content_window_bounds_matches_probe_measured_reachable_box`：`Popup` 内容控件的 `window_bounds()` 与**用命中链实测的可达框**（对同一控件用 `hit_test` / `hit_test_chain` 在其四周密集取点，量出「命中该控件」的点集外接框）**逐位相符**。基准取自真实命中链而非实现自身的输出——G36 的教训是 `HitNode.origin` 在滚动容器内本身不是窗口坐标，故此处不用它当基准，改为密集探针实测。判据同时断言「朴素布局原点 + 锚点」与读数相差恰为 `anchor_`，并断言读数与「未加锚点」值不相等。
2. `plain_subtree_window_bounds_matches_probe_measured_reachable_box`：未挂 `Popup` 的普通子树（`Column` 套探针控件），窗口盒与同一套探针实测的可达框同样**逐位相符**。守住缺省路径零变化：任何多减 / 少减一份平移都会在这里现形。
3. `popup_paint_and_hit_semantics_unchanged`：钉住 `Popup` 现有语义作为两形态用例的基线——`anchor_` 处命中内容、`anchor_` 盒外不命中、`on_paint` 下传的内容盒原点仍为 `anchor_`、`content_bounds()` 仍为 `{anchor_, content_size_}`、关闭态不命中。
4. `dispatch_by_window_bounds_lands_on_popup_content`：端到端复现症状的正面形态——按 `window_bounds()` 的读数在内容控件上取点派发 `MouseEvent`，该控件须收到事件且 `local_position` 等于其在盒内的偏移；同时断言「按未加锚点的旧读数取点」落不到该控件（钉住缺口真实存在，非判据自证）。

**变异自证**：

- 抽掉 `Popup::child_content_origin` 覆写（框架回到今天行为）⇒ 判据 1 与 4 转红，判据 2、3 保持绿（后者证明修复只动查询腿）。
- 钩子改成**加性**（在累计量上叠加而非替换，且不终止上溯）⇒ 判据 1 转红（读数变成 `Popup` 树上位置 + `anchor_` 之和，与派发盒分叉）。
- `Popup::child_content_origin` 在关闭态也返回 `anchor_` ⇒ 判据 3 转红（关闭态内容零盒，基准不该被替换）。

## 回写落点

- `codespec/specification/04-widget.md`（§6.3 三条几何读数，登记坐标系重映射宿主与两类修正钩子的分工）
- `codespec/specification/05-event-navigation.md`（§3.2.3 命中链坐标模型，登记 `Popup` 下三读数的关系）
