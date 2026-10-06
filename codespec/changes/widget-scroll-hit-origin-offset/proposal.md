# CHG-002 修派发链 origin 漏扣滚动偏移（滚动容器内控件事件本地化错位）

| 项目 | 内容 |
|:---|:---|
| 变更编号 | CHG-002 |
| 提出日期 | 2026-10-06 |
| 当前状态 | 实施中 |
| 关联需求 | 无 |
| 影响面 | `Scroll` 命中链下降、事件坐标本地化（`MouseEvent::local_position`）、`HitNode::origin` 语义、平台无障碍桥的控件位置 |

## 动机

`Scroll::on_hit_test_chain` 把局部命中点**加上** `offset_y_` 换算回内容坐标再下降，这半是对的；但它下传给内容子树的**全局原点**是「视口原点 + 内容盒原点」，**漏扣了 `offset_y_`**。派发器据此算 `local_position = position - origin`，于是滚动容器内每个控件收到的本地坐标都整体错位一个滚动量。

**实测事实**（独立探针，视口窗口原点 60、`offset_y_` = 200、内容 20 行 × 40dp，点击内容 y=200 的行的视觉中心，窗口 y=80）：

| 量 | 值 |
|:---|:---|
| 该行盒内的正确局部 y | 20 |
| 控件实际收到的 `MouseEvent::local_position.y` | **-180** |
| 差| 200 = `offset_y_` |

**为什么错位量恰好等于 `offset_y_`**：`origin` 少扣了它，而 `local` 侧多加了它，两处符号相反 ⇒ 偏差为 2× 是巧合之外的另一层——准确说，`origin` 缺 `offset_y_` 使 `position - origin` 整体偏大 `offset_y_`。控件若用 `local_position` 做本地几何判定（如 `Dropdown` 按 `local.y` 反算选项序号、`TextInput` 用 `local` 定光标落点、拖拽阈值判定）会全部偏一个滚动量。

**与 CHG-003 的关系**：CHG-003 查明 `HitNode.origin` 在 `Scroll` 后代上不是窗口坐标，并选择新增 `window_bounds()` 提供事后查询，**未改派发链**。本提案补上这半：让派发链自身的 origin 也成为真窗口坐标，使二者终于同源。

## 变更内容

### 只改 `Scroll` 一处下传原点

`Scroll::on_hit_test_chain` 给内容子树的 `global.origin` 由「视口原点 + 内容盒原点」改为「视口原点 + 内容盒原点 **− `offset_y_`**」。

**只改 origin、不改 local**：`local` 侧的 `+offset_y_` 换算已把点正确送到内容坐标系，两侧各自职责不变。同时改会使命中判定整体错位（`content_box.contains(content_local)` 与下游逐层 `bounds.contains` 全失效）。

### `ancestor_offset`（追加命中盒通路）**刻意不动**

`covers_descendant_extra_hit_box` 下传的 `ancestor_offset` 同样未扣 `offset_y_`，但 `widget.h` 声明它的语义是「本控件原点在**视口坐标系**中的 y」。在视口坐标系下，「离视口多远」本就不该含滚动量，故该现状**自洽**——`Dropdown::panel_box` 要判的正是「离视口多远决定翻上/翻下」，改成窗口坐标反而会改掉它的翻转阈值（嵌套滚动场景下行为变化）。

因此两条通路是**有意不同的两个坐标系**，不是漏改的同一处：
- `HitNode.origin` = 窗口坐标（供 `position - origin` 本地化）
- `ancestor_offset` = 视口坐标（供「离视口多远」的几何决策）

此区分写进 `05-event-navigation.md` §3.2.3，避免后续维护者误把它们「统一」成同一空间。

### 影响面与破坏性说明

- 公共签名与类型**零变更**，既有调用点零改动。
- 行为变更范围：滚动容器内控件收到的 `local_position.y`（错位量减少一个 `offset_y_`）。这是**修正**而非破坏——变更前的值是错的。
- 不进每帧绘制路径，只在派发期算。

## 验收判据

1. `scrolled_descendant_receives_correct_local_position`：`Scroll` 内容后代在非零偏移后收到的事件，`local_position` 等于**独立复算**的行内局部 y（点在该行视觉中心时为行高的一半），且断言该值不等于「减去 `offset_y_` 后」的错位值。
2. `hit_node_origin_equals_independently_recomputed_window_position`：`HitNode.origin` 逐位等于**独立复算**的真窗口位（视口原点 + 内容 y − `offset_y_`），并与 `widget_bounds()` 的窗口盒口径一致。
3. `hit_test_still_selects_the_visual_row_after_scrolling`（**既有**用例，不得回归）：滚后点击视觉上那一行仍命中它——证明只改 origin 未破坏命中判定。
4. `dropdown_flip_inside_scroll_keeps_viewport_semantics`：滚动容器内 `Dropdown` 的翻转判据仍按视口坐标（`ancestor_offset` 未被改动），面板贴视口下沿时仍翻上。

**变异自证**：
- 把下传原点改回不扣 `offset_y_` ⇒ 判据 1、2 转红，判据 3 仍绿（命中判定本身不依赖 origin 的绝对值）。
- 连带把 `local` 侧的 `+offset_y_` 也去掉 ⇒ 判据 3 转红（证明只改一侧会破坏命中）。
- 连带把 `ancestor_offset` 也改成窗口坐标 ⇒ 判据 4 转红。

## 回写落点

- `codespec/specification/05-event-navigation.md`（§3.2.3 登记 origin 修复，以及它与 `ancestor_offset` 刻意分属两个坐标系的说明）
- `codespec/specification/04-widget.md`（§6.3 移除「`HitNode.origin` 未扣 `offset_y_`」的限制描述，改为已修复并指向本节）