# CHG-003 控件窗口逻辑 dp 绝对盒的事后查询（`Widget::window_bounds()`）

| 项目 | 内容 |
|:---|:---|
| 变更编号 | CHG-003 |
| 提出日期 | 2026-10-06 |
| 当前状态 | 实施中 |
| 关联需求 | 无 |
| 影响面 | `Widget` 公共几何面、`Scroll` 内容子树的坐标语义、无障碍语义树（UIA / AT-SPI2 桥）、`a11y_tree.h` 几何取值、`debug_paint` 观测面 |

## 动机

**`paint_bounds()` 的注释承诺与实际读数不符。** `Widget::paint_bounds()` 的声明注释写的是「最近一次 paint 接收的绝对（窗口逻辑 dp）盒」，`Widget::paint` 入口的形参注释同样写「本控件的绝对（窗口逻辑 dp）盒」。但 `Scroll` 把内容录进离屏缓冲时，传给子树的是 `Rect{.origin = {0, -buffer_origin_y_}}`（`scroll.h` 的「几何与命中契约」节与 `on_descendant_dirty` 各写了一遍），因此**滚动容器内后代的 `paint_bounds()` 是缓冲坐标读数**，与注释声明的窗口坐标不是同一件事。`focus_bounds_` 与它同源同值，同样带这个限制（其 `@note` 已自陈内容坐标系，但公共头没有把「窗口 dp」这句承诺改口）。

**消费侧结构上无法折算。** 派发链给子节点的 `HitNode.origin` 与 `paint_bounds()` 是两个不同读数，而把后者换算成前者需要 `buffer_origin_y_`——该量既无 getter，也不进 `serialize_props`（`Scroll::serialize_props` 只写 `offset`）。全仓 grep `to_global` / `global_rect` / `window_rect` / `absolute_bounds` / `local_to_global` 在 `include/aurora` 只命中 `command_palette.h` 的一个私有静态 `to_global(local, bounds)` ⇒ **公共面没有「控件 → 窗口绝对盒」这项能力**，应用侧要拿就得自己复制框架私有算式。

**第二受益面是无障碍桥，它直接读 `paint_bounds()`。** `a11y_tree.h` 的 `accessibility_box()` 与 `sibling_label_name()`、`win32_ua.cpp` 与 `atspi_protocol.cpp` 都取 `paint_bounds()` 作为语义几何盒，于是同一个缺陷原样传导到读屏用户看到的控件位置。这不是旁枝，是同一处坐标系缺陷的第二个出口。

**实测确认读数确实分叉。** 用独立探针 TU（不改动被测代码）实测一条滚后可见的内容行：`Scroll` 视口位于窗口 `y = 60`、`offset_y_ = 200`、内容为 20 行 × 40dp，取内容 `y = 200` 的行——

| 读数 | 实测值 | 实际坐标系 |
|:---|:---|:---|
| 真窗口 y（视口原点 + 内容 y − offset） | 60 | 窗口逻辑 dp |
| `paint_bounds().origin.y` | 200 | 内容坐标 |
| `HitNode.origin.y` | 260 | 视口窗口原点 + 内容盒原点（**未扣 `offset_y_`**） |

三者互不相等。

## 变更内容

### 公共面新增

`Widget` 新增只读入口：

```cpp
[[nodiscard]] auto window_bounds() const -> std::optional<Rect>;
```

语义为**窗口逻辑 dp 绝对盒**：控件自身（含 Modifier 的 padding / border 位移）在窗口客户区坐标系里的盒，与 `MouseEvent::position` 同空间——同一控件、同一帧、同一坐标空间。

**返回 `std::nullopt` 的情形（不留未定义）**：本控件 `show == false`、或尚未布局过（尺寸为零盒）、或不在任何已布局的树内（`layout_parent()` 为空且自身无根盒）。取 `nullopt` 而非零盒，是为了让消费侧能区分「查不到有效窗口盒」与「盒恰好在窗口原点」——`a11y_tree.h` 的「非空则优先绘制盒」判据正是靠这个区分避免把空盒当有效几何。

### 实现路径

查询时沿 `layout_parent_` 自叶向根累加，**不进每帧绘制路径**：

1. 每层祖先把「自身内容原点」加进累计量。内容原点取 `content_origin(parent_bounds.origin, tf)`（`Modifier::TransformInfo` 的 `translation`），与 `render_into` / `hit_test_chain` 共用同一份产物，即 [`04-widget.md`](../../specification/04-widget.md) 与 [`05-event-navigation.md`](../../specification/05-event-navigation.md) §3.2.3 已确立的「origin 与绘制仿射同源」不变量。**不得在查询侧另写一份「加不加 padding」的算式。**
2. 途经滚动祖先（`Scroll`）时逐段扣减滚动偏移：`Scroll` 的内容子树在内容坐标系，`window_bounds()` 须扣掉 `offset_y_`，使其与「视口原点 + 内容 y − `offset_y_`」这一真窗口位对齐。`buffer_origin_y_` 是缓冲录制锚点、只影响 `paint_bounds()` 那条读数，**不参与**窗口盒的折算（扣它会二次偏移）。
3. `LazyList` / `LazyRow` / `GridView` 的偏移已参与子布局（子 bounds 直接写视口坐标），沿父链累加即自动正确，无需特殊处理。
4. 遇 relayout boundary 的上溯口径与 `mark_needs_layout_impl` 一致（该处已有的 `!is_relayout_boundary()` 闸）。

### 注释改口

- `Widget::paint_bounds()` 与 `paint()` 入口形参的「绝对（窗口逻辑 dp）盒」随实修改口：改述为「最近一次 paint 实际传入的盒」，并**在公共头**写明两条限制：① 离屏缓冲（如 `Scroll` 内容）内的后代为**缓冲/内容坐标**，其原点含 `-buffer_origin_y_`；② 需要窗口绝对盒时改用 `window_bounds()`。
- `focus_bounds_` 的 `@note` 同步指向 `window_bounds()`。
- `a11y_tree.h` 的坐标注解改为「绘制盒；需要窗口绝对盒时用 `window_bounds()`」，并让 `accessibility_box()` 在绘制盒落入缓冲坐标时不再把它当屏幕坐标（见下条）。

### 无障碍桥受益

`win32_ua.cpp` 与 `atspi_protocol.cpp` 改取 `window_bounds()`（缺省回落既有绘制盒读数），使读屏拿到的控件位置在滚动容器内不再错位一个滚动量。这是本提案的第二受益面，与公共入口同批落地。

### 破坏性说明

纯新增，无破坏性变更：不删不改任何既有入口，既有调用点零改动。`Scroll` 的绘制路径**不改动**（本入口只在查询时算，不进每帧绘制路径），故滚动吞吐 / 像素门禁不受影响。

## 验收判据

1. `window_bounds_window_box_matches_independently_recomputed_origin_after_scrolling`：`Scroll` 内容后代在非零偏移后取其窗口盒，逐位等于测试**自行独立复算**的真窗口位（视口窗口原点 + 内容 y − `offset_y_`）。判据对 `offset_y_` 敏感，同时断言「朴素布局原点」与复算值相差恰为 `offset_y_`。
2. `window_bounds_matches_hit_chain_origin_without_scrolling`：未滚动、以及不在滚动容器内两种形态，窗口盒与独立复算值**容差 0** 逐位相等，守住缺省路径零变化。
3. `window_bounds_is_empty_for_hidden_and_unlaid_out_widget`：负守卫定死——`show == false` 的控件与从未布局的控件均返回 `std::nullopt`（不是零盒）。
4. `paint_bounds_of_scrolled_descendant_stays_in_buffer_coordinates`：`paint_bounds()` 的缓冲坐标口径被单测钉住（本次只改注释、不改行为），防止有人日后误把它当窗口坐标。

**变异自证**：

- 只把 `paint_bounds()` 的注释 / 口径改对而不补事后查询 ⇒ 判据 1 转红（`window_bounds()` 不存在，编译失败即等价于「无该能力」）。
- 只补事后查询但不同源（漏扣 `offset_y_`，或退回取 `paint_bounds()` 的缓冲坐标）⇒ 判据 1 与 2 转红。
- 把 `nullopt` 口径改成返回零盒 ⇒ 判据 3 转红。

## 回写落点

- `codespec/specification/04-widget.md`（滚动的两套坐标系一节，登记窗口盒查询入口与 `paint_bounds()` 改口后的限制）
- `codespec/specification/05-event-navigation.md`（§3.2.3 命中链坐标模型，登记 `window_bounds()` 与 `HitNode.origin` 的关系及差异）

## 版本记录

本提案新增公共入口，落码时另按仓库的变更纪律记入仓库根的版本记录文件（新增条目 + 迁移说明）。该文件不属于本提案的 codespec 回写落点，故不列入上节。