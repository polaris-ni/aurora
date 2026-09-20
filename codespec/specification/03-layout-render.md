# 布局与渲染（layout / render / image / media）

> 覆盖 `include/aurora/layout/`、`render/`、`image/`、`media/`（`render/` 顶层 16 个头，另有 `render/detail/` 下 3 个头——`gamma_lut.h`、`paint_timing.h`、`painter_simd.h`——与 `render/rhi/` 下 2 个头——`rhi_backend.h`、`software_rhi.h`——合计 21）。
> 布局协议以 `src/aurora/layout/flex_layouter.cpp` 的 `FlexLayouter::layout` 与 `include/aurora/widget/grid.h` 的 `Grid::on_layout` **实现为准**。
> 基础类型 `Point` / `Size` / `Rect` / `EdgeInsets` / `Length` / `Constraints` 定义见 [`01-core.md`](01-core.md) §2，本文只写其布局语义。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 布局求解 | `layout/`（`FlexLayouter`、`Grid` 等） |
| 软件栅格绘制 | `render/painter.h`、`render/font_engine.h`、`render/display_list.h` |
| 离屏与快照 | `render/offscreen.h`、`window/surface.h`（`HeadlessSurface`） |
| 增量渲染与缓存 | `render/dirty_region.h`、`render/snapshot_diff.h`、`render/image_cache.h`（职责见 §8.4） |
| 显示列表 | `render/display_list.h`（唯一绘制指令源，见 §8.6） |
| RHI 后端抽象 | `render/rhi/rhi_backend.h`、`render/rhi/software_rhi.h`（回放目标，见 §8.6） |
| 图像编解码 | `image/image_codec.h` |
| 媒体播放 | `media/`（`video_player.h`、`video_controls.h`、`video_source.h`、`image_sequence_source.h`） |

---

## 2 布局协议

### 2.1 范围与不变量

协议覆盖 **Flex 与 Grid** 两类布局的确定性几何求解；`Stack` 等绝对与叠放布局不在本协议的心算范围内。

**约束传递不变量**：父约束逐轴满足 `min ≤ max`；子节点返回的尺寸必须落在 `[min, max]` 内。

`padding` / `margin` 由 `Modifier` 层在测量前后施加，**不在本协议约束传递的求解范围内**；容器主轴尺寸 = Σ子尺寸 + gap，**无隐式边距合并**。

### 2.2 Length 四态的约束求解

| `LengthKind` | 语义 | 约束求解 |
|:---|:---|:---|
| `WrapContent` | 内容自适应 | 传给子项的 `max` = 父剩余空间，子项按内容返回尺寸 |
| `Expand` | 撑满父级 | `min = max = parentSize` |
| `Fixed` | 固定像素 | `min = max = Length::value` |
| `Fraction` | 父级比例 | `min = max = parent × Length::value`，`value ∈ [0,1]` |

四态是**四个无参枚举值**，尺寸数值存放在 `Length::value` 字段中（[`01-core.md`](01-core.md) §2.2）。

### 2.3 两阶段布局协议

所有容器布局遵循 **measure → place** 两阶段。

**阶段一 Measure**：对每个子节点施加约束 `cc`，子节点返回 `Size ∈ cc`。父约束到子约束的传递逐轴进行（下以主轴为例；交叉轴同理，但 `cc.min.cross` 取父 `min_cross` 而非强制归零）：

```text
cc.min.main  = 0                          // 主轴 min 归零（内容自适应起点）
cc.min.cross = parent.min.cross
cc.max.main  = parent.max.main - used     // 父剩余主轴空间
cc.max.cross = parent.max.cross
```

**阶段二 Place**：根据测量结果和对齐参数计算每个子节点的 `Rect{origin, size}` 并写入 `bounds`。容器自身尺寸 = `constrain(内容总尺寸)`，子节点位置 = 前导间距 + 累计偏移。

---

## 3 Flex 分配算法（FlexLayouter）

### 3.1 参数

```cpp
struct Flex {
    FlexDirection direction;         // Row | Column | RowReverse | ColumnReverse
    MainAxisAlignment main_axis;     // Start | Center | End | SpaceBetween | SpaceAround | SpaceEvenly
    CrossAxisAlignment cross_axis;   // Start | Center | End | Stretch
    float gap;                       // 相邻子项间距（像素），默认 0
    MainAxisSize main_axis_size;     // Min（默认）| Max
};
```

### 3.2 符号定义

| 符号 | 含义 |
|:---|:---|
| `P_main` | 父约束主轴最大值 |
| `P_min_main` | 父约束主轴最小值 |
| `P_cross` | 父约束交叉轴最大值 |
| `P_min_cross` | 父约束交叉轴最小值 |
| `n` | 子项数量 |
| `flex_i` | 第 `i` 子项的 flex 权重（`≥ 0`） |
| `gap` | 相邻子项间距 |
| `total_gap` | `(n > 1) ? (n-1) × gap : 0` |
| `Σ_flex` | 所有 `flex_i > 0` 子项的权重之和 |

### 3.3 阶段一 A：测量非 flex 子项

对每个 `flex_i == 0` 的子项：

```text
cc.main.min  = 0
cc.main.max  = max(0, P_main - used_main)
cc.cross.min = P_min_cross
cc.cross.max = P_cross
size_i = measure(cc)
used_main += size_i.main
max_cross = max(max_cross, size_i.cross)
```

### 3.4 阶段一 B：flex 子项分配

仅当 `P_main < ∞` 且 `Σ_flex > 0` 时执行：

```text
free    = max(0, P_main - used_main - total_gap)
alloc_i = free × flex_i / Σ_flex

cc.main.min  = 0
cc.main.max  = max(0, alloc_i)
cc.cross.min = P_min_cross
cc.cross.max = P_cross
size_i = measure(cc)
used_main += size_i.main
max_cross = max(max_cross, size_i.cross)
```

`alloc_i` 是 flex 子项的 **`max` 约束**，子项可返回 `≤ alloc_i` 的值；若子项内容大于 `alloc_i`，则被 clamp 到 `alloc_i`。

### 3.5 容器尺寸

```text
used_main += total_gap

if main_axis_size == Max && P_main < ∞:
    container_main = P_main
else:
    container_main = clamp(used_main, P_min_main, P_main)

free_space     = max(0, container_main - used_main)
container_cross = clamp(max_cross, P_min_cross, P_cross)
```

**`MainAxisSize` 语义**：默认 `Min` 取内容尺寸，对齐仅在父约束强制更大时产生可见自由空间；`Max` 撑满父级可用主轴空间；无限主轴约束（`P_main = ∞`）下 `Max` 退化为内容尺寸。

### 3.6 主轴对齐

| `MainAxisAlignment` | `leading` | `between` |
|:---|:---|:---|
| `Start` | `0` | `0` |
| `Center` | `free_space / 2` | `0` |
| `End` | `free_space` | `0` |
| `SpaceBetween` | `0` | `free_space / (n - 1)`（`n > 1`） |
| `SpaceAround` | `free_space / n / 2` | `free_space / n` |
| `SpaceEvenly` | `free_space / (n + 1)` | `free_space / (n + 1)` |

### 3.7 阶段二：定位

```text
pos = leading
for i in 0..n:
    if i > 0: pos += gap
    origin.main  = pos
    origin.cross = 按 cross_axis 计算（§3.8）
    pos += size_i.main + between
```

反向布局（`RowReverse` / `ColumnReverse`）对每个子项沿主轴镜像：

```text
origin.main = container_main - (origin.main + size.main)
```

反向一律使用 `FlexDirection` 的 `Reverse` 取值表达。

### 3.8 交叉轴对齐

| `CrossAxisAlignment` | `cross_pos` | `cross_size` |
|:---|:---|:---|
| `Start` | `0` | `size_i.cross` |
| `Center` | `(container_cross - size_i.cross) / 2` | `size_i.cross` |
| `End` | `container_cross - size_i.cross` | `size_i.cross` |
| `Stretch` | `0` | `container_cross`（强制拉伸填满） |
| `Baseline` | `max_above - b_i` | `size_i.cross`（不拉伸） |

**`Stretch` 语义**：子项交叉轴尺寸被强制设为 `container_cross`，无论其内容尺寸。若 `container_cross` 由 `P_min_cross` 撑大（如父 `min.width = 80`），子项也被拉伸到该值；子项同时受自身 `width` / `height` 等显式约束夹取。

**`Baseline` 语义**（仅水平主轴 / `Row` 有意义）：各子项按**首行文本基线**共线——图标与不同字号文字同行、按钮与说明文字混排时，视觉下沿不再参差。

- `b_i` 是子项「布局盒顶 → 首行基线」的距离：`Widget::baseline_distance(ctx)` 给出**内容盒**内距离（含控件自身内边距与垂直居中偏移），容器再补入 `Modifier::transform(size).translation.y`（`Modifier::padding` / Align 造成的内容盒位移）。
- **无基线子项**（钩子返回 `nullopt`，如图标）按 CSS 规则以**自身交叉轴底边**为合成基线参与，不报错、不崩溃；基线一律夹取到 `[0, size_i.cross]`。
- 容器交叉轴 = `max(b_i) + max(size_i.cross - b_i)`（记作 `max_above` / `max_below`），再夹入父约束；**全部子项无基线**时退化为 `End` 语义。
- 纵向主轴（`Column`）的交叉轴是水平的，基线无意义 ⇒ 按 `Start` 处理，并发出一次降级诊断（`Diagnostics::degraded`，每控件实例一次，不逐帧刷屏）。
- 该取值经 `props_io` 的 `cross_axis_alignment` 键往返；读到未知取值仍回退 `Start`（向后兼容）。
- 钩子返回值**不做整像素 snap**：容器按 `max_above - b_i` 定位后，绘制侧 `pen_y = floor(top + ascent + 0.5)` 中的 ascent 与之相消，各子项实绘基线像素一致。

---

## 4 Grid 布局算法

### 4.1 参数

```cpp
struct GridProps {
    int   columns;  // 列数（默认 1）
    float gap;      // 单元格间距（默认 4px）
};
```

### 4.2 算法

设 `cols = max(1, columns)`，`n = children.size()`，`rows = ceil(n / cols)`。

**列宽计算**：

```text
if P_main < ∞（宽度受限）:
    cell_w = max(0, (P_max.width - gap × (cols - 1)) / cols)   // 所有列等宽
else:
    列宽由内容决定（取该列最宽子项）
```

**测量**：对每个子项施加约束 `cc.max = (cell_w, +∞)` 或 `(+∞, +∞)`。

**行列尺寸**：

```text
col_w[c] = max(sizes[i].width)  for i % cols == c
row_h[r] = max(sizes[i].height) for i / cols == r
```

**容器尺寸**：

```text
total_w = Σ col_w[c] + gap × (cols - 1)
total_h = Σ row_h[r] + gap × (rows - 1)
return constrain(Size{total_w, total_h})
```

**定位**（行优先）：

```text
x = 0
for col in 0..cols:
    y = 0
    for row in 0..rows:
        idx = row × cols + col
        if idx < n:
            bounds[idx] = Rect{Point(x, y), sizes[idx]}
        y += row_h[row] + gap
    x += col_w[col] + gap
```

---

## 5 布局缓存与溢出策略

### 5.1 缓存一致性不变量

- **缓存键**：`Constraints` 逐字段相等（`min.w, min.h, max.w, max.h`）。
- **不变量**：若约束未变（`Constraints::operator==` 为真），布局结果不重算。
- **意义**：避免无效 re-layout，保证帧循环复杂度与脏节点数成正比。

> 布局父链必须闭合：凡在 `on_layout` 中把子节点登记为布局父（`set_layout_parent`）的容器，必须完整登记全部子节点，否则脏标记无法上溯，缓存永不失效。

### 5.2 溢出策略

| `OverflowStrategy` | 语义 |
|:---|:---|
| `Visible` | 子内容溢出容器边界仍可见（默认） |
| `Hidden` | 溢出部分裁剪（不绘制），不参与命中测试 |
| `Clip` | 同 `Hidden`，但保留命中测试事件穿透 |
| `Scroll` | 预留，当前等同 `Hidden` |

溢出策略**不影响布局尺寸计算**：容器尺寸始终由约束决定，溢出策略仅影响渲染裁剪与事件处理。

---

## 6 心算流程与示例

### 6.1 心算流程

给定父约束 + Flex/Grid 参数 + 子项列表：

1. 确定方向：Row → 主轴水平，Column → 主轴垂直。
2. 测量非 flex 子项：在剩余空间约束下获取内容尺寸。
3. 计算 flex 剩余：`free = P_main - used - total_gap`。
4. 分配 flex：`alloc_i = free × flex_i / Σ_flex`。
5. 容器主轴：`Min → clamp(used, P_min, P_max)`；`Max → P_main`。
6. 自由空间：`free_space = container_main - used`。
7. 主轴对齐：查 §3.6 得 `leading` / `between`。
8. 交叉轴：查 §3.8 得 `cross_pos` / `cross_size`（注意 `Stretch`）。
9. 定位：`pos = leading`，逐子项累加 `size + gap + between`。
10. 反向：若 Reverse，镜像主轴位置。

### 6.2 示例

| # | 条件 | 输出 |
|:---|:---|:---|
| 1 | 父 `max=100×100`，`Row/Start/Start`，A/B/C 均 `20×10`，`flex=0` | 容器 `60×10`；A.x=0, B.x=20, C.x=40 |
| 2 | 父 `max=100×100`，`Row/Start/Start`，A/B 均 `flex=1`、内容 `0×10` | `free=100`，`alloc=50` 各；容器 `100×10`；A.x=0, B.x=50 |
| 3 | 父 `max=100×100`，A `flex=0` 内容 `20×10`，B/C `flex=1` | `free=80`，`alloc=40` 各；容器 `100×10`；A.x=0, B.x=20, C.x=60 |
| 4 | 父 `min=100×0`/`max=100×100`，`Row/Center/Start`，A/B `20×10` | `container=100`，`free_space=60`，`leading=30`；A.x=30, B.x=50 |
| 5 | 父 `min=100×0`/`max=100×100`，`Row/SpaceBetween`，A/B/C `20×10` | `between=20`；A.x=0, B.x=40, C.x=80 |
| 6 | 父 `min=80×0`/`max=80×200`，`Column/Start/Stretch`，A/B `20×30` | `container_cross=80`，Stretch 拉伸；容器 `80×60`；A=`Rect{(0,0),80×30}`，B=`Rect{(0,30),80×30}` |
| 7 | 父 `max=100×100`，`Row/Start/Start`，`gap=10`，A `flex=0` `20×10`，B/C `flex=1` | `total_gap=20`，`free=60`，`alloc=30` 各；容器 `100×10`；A.x=0, B.x=30, C.x=70 |
| 8 | 父 `max=100×200`，`Grid{columns=2, gap=4}`，4 个子项各 `30×20` | `cell_w=48`，`col_w=[30,30]`，`row_h=[20,20]`；容器 `64×44`；child[0]=`(0,0)`，child[1]=`(34,0)`，child[2]=`(0,24)`，child[3]=`(34,24)` |

---

## 7 布局引擎 API

### 7.1 入口

- `Constraints`：父对子的尺寸约束；子必须 `constrain()` 回落到约束内。
- `Length`：强类型尺寸。`Length::fixed(px)` / `wrap()` / `expand()` / `ratio(f)`，等价工厂 `au::px(v)` / `au::percent(f)` / `au::fill()` / `au::auto_length()`。
- `EdgeInsets`：边距与内边距。
- `Alignment`：`TopLeft` `TopCenter` `TopRight` `CenterLeft` `Center` `CenterRight` `BottomLeft` `BottomCenter` `BottomRight`（`widget/alignment.h:19`）。
- `Flex` / `FlexLayouter`：两遍求解（先宽后高），方向与对齐语义见 §3。
- `LayoutEngine` / `LayoutBox`（`layout/layout_engine.h`、`layout/layout_box.h`）：静态布局入口。`LayoutEngine::layout(Widget& root, const Constraints&, const BuildContext& = {})` 驱动 `Widget::layout` 两阶段布局，结果写入各 widget 的 `bounds` / `size`；`LayoutEngine::layout_to_box(Node& root, const Constraints&, ...)` 布局后一次性产出 `LayoutBox` 树（每盒含 `rect` + 收到的 `constraints` + `children`），供命中测试、调试快照与无头渲染复用；`LayoutEngine::build_box(const Node&)` 仅从已布局的 `Node` 树收集几何，不施加约束。

```cpp
au::Text a{ au::TextProps{ .content = "A" } };
au::Text b{ au::TextProps{ .content = "B" } };
b.width(au::px(120)); // 宽度意图走 Widget::width(Length)，返回 Widget&（引用，供原地链式），不可 std::move 其结果塞进 Node
auto row = au::Row(au::RowProps{
    // 指定初始化器须按成员声明序：RowProps 中 children 在前、gap 在后
    .children = { au::Node{ std::move(a) }, au::Node{ std::move(b) } },
    .gap = 8.0F,
});
row.modifier = au::Modifier{}.padding(8);
```

布局是 `layout(tree, viewport) -> boxes` 的纯函数：父宽 = Σ子宽 + 间距（无隐式边距合并）；百分比参照父 **content** 宽度；窗口 resize 仅重算布局、不改变语义。

### 7.2 Column / Row 的对齐 API

`Column` / `Row` 通过挂载的 `Flex` 承载 `main_axis`、`cross_axis` 与 `main_axis_size`，并提供链式 setter：

```cpp
au::Column{}
    .set_main_axis_alignment(au::MainAxisAlignment::Center)     // 主轴居中
    .set_cross_axis_alignment(au::CrossAxisAlignment::Stretch)  // 交叉轴拉伸填满
    .set_main_axis_size(au::MainAxisSize::Max)                  // 撑满父级主轴 → 对齐可见
    .set_gap(8.0F);
```

三者均为**固有属性**，随 `Column` / `Row` 序列化（键 `main_axis_alignment` / `cross_axis_alignment` / `main_axis_size` / `gap`），可经 `to_json` / `from_json` / `diff` / `apply_patch` 往返。

### 7.3 共享枚举

跨控件共享枚举统一定义于 `core/enums.h`，经 `props_io.h` 提供 `*_to_json` / `json_to_*` 互转供各控件 `serialize_props` / `deserialize_props` 使用，并登记于 `tools/gen/gen_api.cpp` 的 `known_enums()`，供 `aurora_api.json` 与代码生成工具消费。

| 枚举 | 取值 | 用途 |
|:---|:---|:---|
| `TextAlign` | `Left` `Right` `Center` `Start` `End` `Justify` | `Text` / `RichText` 文本对齐。`Justify` 仅对多行文本的非末行按词均分铺满整行宽度；单行等同 `Left` |
| `TextOverflow` | `Clip` `Ellipsis` `Fade` | 超出 `max_lines` 时的处理。`Fade` 在 `Painter` 不支持时降级为 `Clip` |
| `FontWeight` | `Thin(100)` `ExtraLight(200)` `Light(300)` `Normal(400)` `Medium(500)` `SemiBold(600)` `Bold(700)` `ExtraBold(800)` `Black(900)` | 字重，数值即 Flutter 同名词重 |
| `FontStyle` | `Normal` `Italic` | 字形风格。`Italic` 经 GDI `LOGFONT.lfItalic` 生效（Windows/GDI）；非 GDI 回退路径不倾斜 |
| `TextDecoration` | `None` `Underline` `Overline` `LineThrough`（可按位 `\|` 组合） | 文本装饰线 |
| `MainAxisSize` | 语义见 §3.1 / §3.5 | `Column` / `Row` 主轴尺寸策略 |
| `MainAxisAlignment` | 取值见 §3.1，公式见 §3.6 | `Column` / `Row` 主轴对齐 |
| `CrossAxisAlignment` | 取值见 §3.1，公式见 §3.8 | `Column` / `Row` 交叉轴对齐 |
| `StackFit` | `Loose` `Expand` `Passthrough` | `Stack` 子项尺寸拟合 |
| `BoxFit` | `Fill` `Contain` `Cover` `FitWidth` `FitHeight` `None` `ScaleDown` | `ImageView` 图片缩放拟合 |

### 7.4 二层属性划分

控件可配置性由两层构成：

- **固有属性**（`XxxProps` 字段）描述控件自身身份，随控件序列化、可被 Inspector 枚举。
- **`Modifier`** 承载跨切面、可叠加、可 `Reactive` 变化的通用装饰（padding / background / border / align / opacity / rotate / scale / transform / clickable）。

两者重叠的能力（如 `padding` / `corner_radius` / `background_color`）以**控件固有属性优先**；`Modifier` 同类项保留用于「给任意控件套一层」的跨切面场景。绘制时 `Modifier` 在外、固有属性在内。

---

## 8 渲染核心

### 8.1 Painter

`Painter`（`render/painter.h:31`）是**软件栅格**绘制内核，跨平台零依赖，不使用 GPU。

**帧缓冲与状态**

| 方法 | 说明 |
|:---|:---|
| `begin(width, height)` | 开始一帧 |
| `width()` / `height()` / `data()` | 缓冲尺寸与像素数据 |
| `get_pixel(x, y)` | 读像素 |
| `set_alpha(double)` / `global_alpha()` | 全局透明度 |

**几何与文本原语**

| 方法 | 说明 |
|:---|:---|
| `fill_rect(Rect, Color)` | 填充矩形 |
| `clear_rect(Rect)` | 清除矩形 |
| `draw_rect(Rect, Color)` | 描边矩形 |
| `draw_line(Point, Point, float, Color)` | 画线 |
| `fill_rounded_rect(Rect, float radius, Color)` | 填充圆角矩形 |
| `draw_rounded_border(Rect, float radius, float thickness, Color)` | 圆角描边 |
| `stroke_polyline(const vector<Point>&, float width, Color)` | 抗锯齿多段线（真距离场 SDF：圆角连接 + 圆帽，1px 羽化）。覆盖度按**几何一次算成**，故半透明下顶点不会二次合成串珠 |
| `fill_sector(Point center, float outer_r, float inner_r, float a0, float a1, Color)` | 抗锯齿扇形 / 环扇（y 轴向下，弧度制；`a1 - a0 >= 2π` 视为整圆 / 整环；`inner_r <= 0` 即实心扇形） |
| `stroke_arc(Point center, float radius, float thickness, float a0, float a1, Color)` | 弧线描边（`fill_sector` 的环带语义糖） |
| `draw_text(Rect, string, Font, Color[, TextLayoutOpts][, TextAAMode])` | 绘制文本，三个重载 |
| `draw_image(const Image&, const Rect&)` | 绘制图像（双线性采样） |

**混合与效果**

| 方法 | 说明 |
|:---|:---|
| `blend_pixel(x, y, Color)` | 单像素 alpha 混合 |
| `blend_rect(Rect, Color)` | 矩形 alpha 混合 |
| `blend_subpixel(x, y, Color, cr, cg, cb)` | 子像素混合 |
| `blend_subpixel_span(...)` | 子像素跨段混合 |
| `draw_linear_gradient(Rect, Point, Point, vector<Color>, ...)` | 线性渐变 |
| `draw_radial_gradient(Rect, Point, float, vector<Color>, ...)` | 径向渐变 |
| `draw_shadow(Rect, offset_x, offset_y, blur_radius, Color)` | 阴影 |
| `blur_region(Rect, float)` | 区域模糊 |
| `blend_region(Rect, BlendMode, Color, float)` | 区域混合模式 |
| `mask_region(Rect, ShaderMaskKind, float)` | 遮罩 |

**合成与像素搬运**

| 方法 | 说明 |
|:---|:---|
| `composite(const Painter&, const Matrix2D&)` | 把另一 `Painter` 按矩阵合成进来 |
| `composite(const Image&, const Matrix2D&, float src_scale)` | 把 `Image` 按矩阵合成进来 |
| `composite_pixels(...)` | 原始像素合成 |
| `to_image()` | 导出为 `Image` |
| `shift_pixels(float dy)` | 就地垂直平移帧缓冲（`dy` 为逻辑 dp，>0 下移、<0 上移）。录制态静默 no-op |

**裁剪栈**

| 方法 | 说明 |
|:---|:---|
| `push_clip(Rect)` / `push_clip_rounded(Rect, radius, anti_alias = true)` | 压入矩形 / 圆角裁剪 |
| `pop_clip()` | 弹出裁剪 |
| `has_clip()` / `clip_bounds()` | 查询裁剪状态与边界 |

`push_clip` 与 `pop_clip` **必须成对**。

**高 DPI**

`set_scale(float)` / `scale()`：高 DPI 下把 dp 坐标几何放大到物理像素帧缓冲。像素级写入（`blend_pixel` / `draw_text`）直接落在物理像素，不再乘 scale。

**显示列表**

`record(DisplayList&)` 开始录制，`stop()` 结束，`is_recording()` 查询状态，`mark_recording_dynamic()` 标记动态内容。`set_skip_dl_record(bool)` / `skip_dl_record()` 控制是否跳过录制。命令结构、变长数据池与**回放目标抽象（RHI 后端）**见 §8.6。

> **录制态不变量**：`Painter::composite(const Image&, ...)` 在录制态**必须**录制 Composite 命令而非就地写像素。否则缓存的显示列表回放进祖先录制时，祖先列表缺失该合成，回放时子树像素整体缺失。
>
> **自驱动动画与缓存**：在 `on_paint` 内自行 `mark_needs_paint()` 推进动画的控件，必须覆写 `can_cache_display_list()` 返回 `false`，否则开启显示列表缓存后动画会被冻结。

**不变量**：`Painter::fill_rect` 必须乘 `global_alpha_`；`set_alpha(double)` 须在 `on_paint` 结束前复位为 `1.0`。

### 8.2 FontEngine

`FontEngine`（`render/font_engine.h`）是文本引擎单例，以 **FreeType** 做栅格化、**HarfBuzz** 做 shaping（经仓库 `third_party/` 源码编入静态库，跨平台一致、确定性）。`draw_text_impl` / `measure_width` / `caret_x` 统一按文本 run 调 `hb_shape` 经 `hb_ft_font` 桥接 FreeType，度量、光标、命中三者逐位一致。

**字体注入**：`set_default_font(ttf_path)` / `register_font(family, ttf_path)` / `register_font_from_memory(family, ttf_bytes)`（`family` 为空表示默认 sans-serif）。

**度量与绘制**：`measure_width` / `measure_height` / `measure_ascent` / `draw_text`。

**选中原语**：`caret_x(text, idx, font)` / `hit_test_char(text, x, font)` / `hit_test_char_inclusive`（以码点为索引，UTF-8 安全）。

**默认字体**是内置 Noto Sans（OFL），引擎首次使用时自动注册为 `""` / `"sans-serif"` / `"Noto Sans"` / `"default"`，含 Headless，保证跨机文本渲染逐位确定。缺字按候选 `FT_Face` 链回退（含系统 CJK 字体）避免豆腐块；无任何可用字体文件时回退内置 `BitmapFont` 保底。

**抗锯齿**：`enum class TextAAMode { Supersample, ClearType }` 加进程级 `set_text_aa_mode(mode)` / `text_aa_mode()`，默认 `Supersample`。`Supersample` 用 `FT_RENDER_MODE_NORMAL` 输出 A8 灰度覆盖度，与背景无关；`ClearType` 用 `FT_RENDER_MODE_LCD` 输出 3× 水平 RGB 子像素覆盖度，经 `Painter::blend_subpixel` 逐通道合成，**仅当文本不透明时启用**，半透明或字体不可用时自动回退 `Supersample`。

**光栅状态世代**：`FontEngine::raster_generation()` 返回全局计数，`set_text_aa_mode` 与三个字体注入接口（换用不同字面同样改变字形光栅结果）在**值真正变化**时自增它。控件的 Display List 与离屏层缓存在录制/生成那一刻固化了 AA 模式与字面，而 `Widget::mark_needs_paint()` 只沿父链向上传播失效、不触及后代缓存——故控件必须把本世代纳入缓存命中条件（`Widget::paint` 已内置），否则切换后后代仍回放旧光栅，表现为「切换瞬间无变化、过一会儿才随无关失效零星生效」。字形图集键已含 AA 模式与 `px`，世代失效只触发重录、不产生脏条目。

**排版选项（`TextLayoutOpts`）**：`measure_width` / `caret_x` / `hit_test_char` / `draw_text` 均提供接受 `TextLayoutOpts` 的重载，携带 `letter_spacing`（相邻字形间间距，整串共 `n-1` 次）、`word_spacing`（词间距，仅空格后追加）、`italic`（经 FreeType `FT_Set_Transform` 仿斜）。统一 opts 保证度量、光标、命中、绘制四者完全一致。

**锚定契约**：`draw_text(r, ...)` 的 `r.origin.y` 是**行盒顶**而非基线。实现内部首行基线 = `origin.y + 主 face ascender`，回退 face 字形统一按主 face 基线对齐。全库调用方均按顶锚定传值，**不得自行加减 ascent**。该 ascent 由 `measure_ascent(f)` 公开（与绘制同源、不做绘制侧的整像素 snap），供 `CrossAxisAlignment::Baseline` 的控件级基线使用；无可用字体面时回退 `BitmapFont::measure_ascent`（同一 `pixel_size` 口径），恒有 `0 <= measure_ascent <= measure_height`。

**实显度量（`display_*`）**：FT hinting 把每个字形 advance 取整到整像素，同一字形在不同像素尺寸下的 advance 不成 scale 比例。因此 `display_width` / `display_caret_x` / `display_hit_test_char{,_inclusive}` 必须按「绘制同源的物理像素尺寸 `lround(px × scale)` 真算前缀推进后折回 dp」，**不得写成自然度量的转发别名**——否则缩放屏下行内累计误差跨字符边界，造成命中 off-by-one。`scale == 1` 时退化为对应自然版。

**命中复杂度契约**：`hit_test_char{,_inclusive}` 与 `display_` 变体均为单趟扫描 O(n)，**禁止**退化为「逐边界重算前缀」的 O(n²)。

**Shape 缓存**：`shape_line(line, faces, px, opts)` 是全库唯一 shaping 入口，其外包一层进程级 LRU 缓存，键为 `{line, px, opts, faces_key}`，双上限 **4096 条 / 8 MiB**，命中即复用已 shape 的 `ShapedLine::glyphs` 并跳过 `hb_shape`，结果与重算逐位一致。公共 API：`FontEngine::shape_cache_stats()`（返回 `ShapeCacheStats{hits, misses, entries, bytes}`）与 `FontEngine::shape_cache_clear()`；二者**不受 `AURORA_ENABLE_PROFILING` 门控，任何构建下均可读**。字体注入接口内部自动清空 shape 缓存，避免字体集合变更后复用旧字形索引。

> **Headless 行为**：字体加载**不按后端分支**——`render/font_discovery` 无条件注册内置 Noto Sans（见上文「默认字体」），`HeadlessSurface` 下走同一 FreeType 栅格化路径，`draw_text` 实际写入内存帧缓冲。这正是上文「含 Headless 逐位确定」与 §10.1 像素级 golden 回归成立的前提，涉及真实字形绘制的问题在无头下同样可复现。

### 8.3 Surface

`Surface` 是绘制目标抽象（窗口 / 离屏）。

| 方法 | 说明 |
|:---|:---|
| `begin_frame(int width, int height) -> Result<bool>` | 纯虚；按设备像素宽高分配 / 重置一帧画布 |
| `painter() -> Painter&` / `present() -> Result<bool>` | 取当前帧绘制器 / 呈现当前帧（刷新到窗口或落盘） |
| `set_event_handler(EventHandler)` | 后端只「采集原生事件并翻译为 `aurora::Event` 上抛」；事件派发集中到 `Application` 经 `EventDispatcher` + `FocusManager` |
| `scale_factor()` | 默认 `1.0`；`Win32Surface` / `D3D11Surface` 返回 `dpi/96`，启用 Per-Monitor DPI 感知，按物理像素创建窗口与帧缓冲，事件坐标除以 scale 还原为 dp |
| `set_present_dirty(const std::vector<Rect>&)` | `Window::present_root` 在清脏前把本帧脏矩形（逻辑→设备坐标）交给后端；支持增量上屏的后端仅更新变化区，空向量表示全量上传 |
| `set_title(const std::string&)` | 虚方法，默认空实现；`Win32Window` 经 `SetWindowTextA` + `utf8_to_acp` 生效，`Headless` / `Glfw` 忽略。`Window::set_title` 写 `title_` 后同步下发 |
| `set_cursor(CursorShape)` | 虚方法，默认空实现。宿主在悬停链解析出的形状**变化**时下发（`EventDispatcher` 按 `cursor_emitted_` / `current_cursor_` 去重，避免每个 Move 都打平台 API）。平台映射在各后端 `.cpp`：GLFW `glfwSetCursor` + `glfwCreateStandardCursor`（句柄缓存）、Win32 `SetCursor` + `LoadCursor(nullptr, IDC_*)`、X11 `XDefineCursor` + `XCreateFontCursor`（`XC_*` 字形，句柄缓存）、Wayland 客户端主题光标（`wl_cursor_theme_load(nullptr, 24×scale, wl_shm)` + `wl_cursor_theme_get_cursor(cursor_rfc_name)` → 专用 cursor `wl_surface` attach 主题自有 ARGB `wl_buffer` → `wl_pointer_set_cursor(pointer, enter_serial, ...)`，详见下方 Wayland 光标条目）、macOS `[[NSCursor …] set]`。`HeadlessSurface` 覆写为「按序记录」（`cursor_log()` / `last_cursor()` / `clear_cursor_log()`），使整链在无头环境可端到端断言；`D3D11Surface` 复用 Win32 宿主映射（内部头 `src/aurora/window/win32_cursor.h` 的 `detail::set_win32_cursor`，与 `Win32Surface` 共用一份），`WasmSurface` 保持空实现（浏览器自管 cursor） |
| `native_handle() -> void*`（`const`） | 虚方法，**基类默认返回 `nullptr`**；真实窗口后端须覆写为宿主原生句柄。Win32 家族两路 `Win32Surface`（GDI 上屏）/ `D3D11Surface`（GPU 上屏）共用同一个 `Win32Window` 宿主，**二者均**返回 `hwnd()`（与 `hwnd()` 访问器同义）。`aurora::debug::surface_state()` 的 `has_native_window` 就由它非空判定——**漏覆写会让真实窗口后端误报「无原生窗口」**（`D3D11Surface` 曾如此，2026-09-13 补齐）。该契约由 `tests/unit/utest_native_surfaces.cpp` 的 `windows_family_native_handle_contract` 以类型级 `static_assert` 守门（`decltype(&T::native_handle)` 判定覆写存在） |
| ⚠️ X11 翻译单元的宏碰撞（实现约束，非 API 变更） | `<X11/X.h>`（经 `Xlib.h` 引入）**无条件** `#define CursorShape 0`（"largest size that can be displayed"），与本表类型名 `aurora::CursorShape` 硬碰撞：不解除时该记号一律被预处理器展开为 `0`，`CursorShape shape` 变成 `0 shape`，报出极难定位的 `expected ')' before 'shape'`（2026-09-13 开 `AURORA_BACKEND_X11=ON` 真编译时才暴露）。故**任何引入 Xlib 的翻译单元**都必须在 Xlib 头之后、并在引入 `cursor_map.h` 之前 `#undef CursorShape`（同款处置见 `src/aurora/window/x11_surface.cpp` 顶部与 `x11_surface.h` 的 `@warning`）。同理 `#undef None` 用于避免污染 `ModifierKey::None` |

> **光标形状映射 SSOT**：形状 → 平台中立的规范名由 `window/cursor_map.h` 提供唯一一份表——
> `cursor_rfc_name(CursorShape)` 返回 freedesktop 光标主题名，同时即 W3C CSS `cursor` 关键字
> （`default` / `text` / `pointer` / `ns-resize` / `ew-resize` / `nwse-resize` / `nesw-resize` /
> `move` / `crosshair` / `not-allowed` / `wait`），未知取值回退 `default`。Wayland
> （`wl_cursor_theme_get_cursor`）与浏览器/Wasm（canvas CSS cursor）可直接消费该字符串；需原生常量的
> 后端（GLFW/Win32/X11/macOS）在各自 `.cpp` 内按 `CursorShape` 取值序 `switch`，长度契约由
> `kCursorShapeCount` 对齐（新增形状漏填即编译期红灯）。

> **Wayland 光标的实现口径**：`wl_pointer.set_cursor` 要求一个 **enter serial**（只来自
> `wl_pointer.enter`），且合成器在指针重新进入本表面时会回到默认光标——故后端把形状存进
> `pending_cursor_shape`，enter 前不下发（`set_cursor` 返回即生效，无须调用方重试），enter 与
> `wl_output.scale` 变化时**强制重提交**（主题按 `24 × scale` 设备像素重载）。主题查找链为
> `cursor_rfc_name(shape)` → `default` → `left_ptr`，全落空才 WARN 一次并留在合成器默认光标。
> 动画光标（如 `wait`）取 `images[0]` 首帧，客户端不驱动帧序列。`wp_cursor_shape_manager_v1`
> （新协议，免主题）实测本机 WSLg Weston 未发布该全局，故不走该路。观测面 `cursor_state()`
> 返回本次提交的「主题命中名 / 位图尺寸 / 热点 / buffer 身份 / 提交次数」，用于真机探针判定。
>
> **`set_cursor` 的真机验收（无头 CI 无法覆盖的部分）**：单元/集成测试只能断言到
> 「`HeadlessSurface` 记录序列」与「各后端覆写存在」这一层——「屏幕上显示的光标是否真的变了」
> 必须建真实窗口、在真实桌面会话里验收。为此 `tools/verify/` 提供五份**人工触发**的探针
> （`cmake/AuroraVerify.cmake` 定义、`AURORA_BUILD_VERIFY_TOOLS` 门控、**不进 CTest**）：
> X11 经 XFIXES `XFixesGetCursorImage` 读回、Win32 经 `GetCursorInfo` 读回、macOS 经
> `[NSCursor currentCursor]` 单例同一性读回、GLFW（无光标查询 API）走「自动能力核对 +
> `--interactive` 人工目视」。Wayland 与前三者有一处本质差别：**协议没有客户端可达的「屏幕当前
> 光标」读回**（合成器不广播、也不允许查询），故 `aurora_verify_wayland_cursor` 的判据只能落在
> 「本端向合成器提交了什么」（`cursor_state()` 逐项比对请求的规范名），「合成器接受并画在屏幕上」
> 由 `--interactive` 人工目视段负责。各探针的验收范围与退出码语义见其源文件头注释；
> 真机验收须在对应平台手工执行（探针不进 CTest）。

### 8.4 离屏渲染与快照

定义于 `render/offscreen.h`，与 `HeadlessSurface` 解耦。

```cpp
[[nodiscard]] auto render_to_png(Node &root, int width, int height, const char *path) -> Result<bool>;
[[nodiscard]] auto render_to_image(Node &root, int width, int height) -> Image;
[[nodiscard]] auto render_to_logical_snapshot(Node &root, int width, int height) -> Json;
```

- `render_to_png`：`root` 为 `Node&`，内部自行 `mount`，调用方无需预挂载；`width` / `height` 为画布逻辑尺寸。
- `render_to_image`：与 `render_to_png` 同源同结果，但不落盘，直接返回 RGBA8 内存图 —— 供需要在进程内二次消费像素的路径使用（如快照比对、MCP `compare_snapshot`）。`render_to_png` 现为其薄壳（渲染 + 写出）。
- `render_to_logical_snapshot`：返回平台无关的**逻辑快照** JSON（结构树 + 盒模型），供 AI 在无头环境校验。

`Scene::render_to_png(path, width, height)`（`app/scene.h:29`）与 `Application::render_to_png(path)`（`app/application.h:224`）是无头便捷封装。

**其余渲染支撑头**（`render/`，公开）：`dirty_region.h` 提供 `DirtyRegionTracker`——收集脏矩形并把重叠项合并为并集，条数超上限（默认 `16`，可经 `set_max_rects` 调整）即退化为整帧脏（`mark_all` / `is_full`），以 `rects()` / `merged_bounds()` 出结果，衔接 §8.3 `set_present_dirty` 的增量上屏；`snapshot_diff.h` 提供 `compare_snapshots(baseline, current, tolerance = 0) -> SnapshotDiff`——逐像素比对两张 RGBA8 快照，产出差异像素数、最大通道差、差异占比与差异可视化图，`SnapshotDiff::passed(max_ratio)` 按阈值判定通过，供 golden 回归与 `aurora-cli snapshot --compare` 使用；`image_cache.h` 提供 `ImageCache`——进程级单例（`instance()`）的按路径 LRU 解码缓存（`get` / `put` / `remove` / `clear`，字节上限 `set_max_bytes`，解码失败不缓存），`count()` / `hit_count()` 供诊断与性能覆盖层读取。

#### 8.4.1 快照差异的语义化

`compare_snapshots` 只回答「有多少像素不对」，回答不了「**在哪儿**」和「**是谁画的**」。下面这组 API 是它的纯增量扩展（`compare_snapshots` 签名与语义零改动），把逐像素统计升级成可定位、可归因的报告，供 golden 回归、MCP 与 Inspector 消费。

| 符号 | 说明 |
|:---|:---|
| `DiffRegionOptions{tile_size=8, tile_dirty_ratio=0.0, min_region_pixels=1}` | 聚合选项：`tile_size` 为网格边长（`<=0` 退化为逐像素）；`tile_dirty_ratio` 为网格判脏阈值（默认 0 即「有一处差异即脏」，宁可多报不漏报）；`min_region_pixels` 滤碎块 |
| `DiffRegion{bounds, diff_pixels, coverage, max_color_delta}` | 差异区域。矩形以**图像像素**为单位，复用既有 `Rect`（本库无整数矩形类型，像素值以 float 承载） |
| `cluster_diff_regions(baseline, current, tolerance, opt) -> vector<DiffRegion>` | 网格归并：切网格 → 判脏 → 四连通（并查集，恒以较小根为根故结果确定）→ 每连通块取包络。按 `diff_pixels` 降序输出 |
| `WidgetBox{path, type, bounds}` | 控件的布局盒快照（纯值）。`path` 为**索引路径**，与 `find_node_by_path` / `PUT /api/widget/{path}` 同格式（根为空串） |
| `RegionAttribution{region, widget_path, widget_type, widget_bounds, widget_area_ratio, partial_overlap}` | 已归因的区域。`attributed()` 判据是**类型名非空**——根控件的合法路径本身就是空串，用路径判空会误判 |
| `attribute_diff_regions(regions, span<const WidgetBox>, pixels_per_unit=1.0)` | 归因：取交叠面积最大者，交叠相等时取 DFS 序更靠后者（= 更深的后代，真正画出像素的那个） |
| `SnapshotDiffReport{raw, regions, attributed, attributed_ratio}` | 报告。`to_json()` 给机器、`to_text(max_regions)` 给人或 LLM 直接读；`passed()` 沿用逐像素判据 |
| `build_snapshot_diff_report(baseline, current, boxes={}, tolerance, opt, pixels_per_unit)` | 一步产出报告的推荐入口（等价于依次调用上面三个） |

由 `collect_widget_boxes(const Node&) -> vector<WidgetBox>`（`widget/inspect.h`）把控件树拍平成 `WidgetBox` 表；其输出顺序即**先序**，是归因 tie-break 依赖的契约。

消费方：

- **golden 回归**：`tests/framework/golden.h` 的 `compare_or_update(name, current_path, root)` 在失败时把 `SnapshotDiffReport::to_text()` 附在断言消息里（归因只在失败时计算，通过路径零开销）。
- **MCP**：`compare_snapshot` 工具渲染 UI 树 JSON 并与 golden 基线比对，返回 `report.to_json()`（含 `summary` 文本摘要）。

#### 8.4.2 golden 测试的共享设施

`golden_dir()` / `env_value|flag|int` / `compare_or_update_golden()` 曾在 7 个测试文件里各抄一份，现收敛为 `tests/framework/golden.h`（`aurora::testing::golden`，仅测试框架内部、不进 `include/`）：

| 符号 | 说明 |
|:---|:---|
| `dir()` | golden 真值目录，`AURORA_GOLDEN_DIR` 覆盖优先 |
| `env_value` / `env_flag` / `env_int` | 环境变量读取（`env_int` 解析失败回退 fallback，不抛异常） |
| `write_temp_png(painter, tag)` | 把 Painter 写成用例临时 PNG 并返回路径 |
| `compare_or_update(name, current_path, root = nullptr)` | 与基线比对；`AURORA_UPDATE_GOLDEN` 非空时改为重生成基线 |
| `compare_or_update_painter(name, painter)` | 便捷重载，直接吃 Painter |
| `compare_gpu_tolerance(name, current_image, tolerance, max_diff_pixels, root = nullptr)` | GPU 容差层：离屏读回帧 vs 软件 SSOT 基线，容差/预算由调用方**逐场景显式申报**；不吃全局 env 旋钮、无 update 分支（GPU 输出永不回写基线）。wgpu 与 GL 两条实路径共用（`itest_wgpu_golden` / `itest_gl_golden`），场景与帧装配单一来源为 `tests/support/gpu_golden_scenes.h` |

判据沿用历史语义（差异像素数 <= `AURORA_GOLDEN_MAX_PIXELS`、单通道差 > `AURORA_GOLDEN_MAX_DIFF` 才算差异像素），故本次收敛**不改变任何 golden 的通过结论**。`AURORA_GOLDEN_MAX_DIFF/PIXELS` 是软件逐位红线的**显式放松开关**，仅 `compare_or_update` 族读取——GPU 容差层的场景级容差带与之刻意隔离，防一视同仁设值连带静默放松软件侧判据（见 §8.8 测试段）。

### 8.5 后端与工厂

`SurfaceKind{Headless, Win32, Glfw, D3D11, X11, Wayland, MacOS, Wasm}` 现仅为**类型标签**（只用于 `auto_detect_surface()` 返回类型与 `Platform::surface` 字段），不再用于构造选择。枚举器无条件全部出现且数值固定（见 `window.h`），不随 `AURORA_BACKEND_*` 宏开关增减，以保证序列化与 ABI 兼容：未编译的后端其标签仍存在，只是运行期不会被 `auto_detect_surface()` 产出、对应 `create_window` 重载不可用。

后端选择收口于类型安全工厂 `create_window(const XxxOptions&)`（`window/window.h`），每个后端有专属选项结构：`HeadlessOptions{png_path}` / `Win32Options{}` / `D3D11Options{vsync}` / `WgpuOptions{vsync}` / `GlfwOptions{gl_major, gl_minor, resizable, gpu}` / `X11Options{}` / `WaylandOptions{}` / `MacOSOptions{}` / `WasmOptions{canvas_id}`，外加通用 `WindowOptions{size, title, max_frames}`。编译器会拒绝把某后端专属字段误用到不相关后端。`GlfwOptions::gpu = true` 请求 GPU 栅格模式（§8.7，需 `AURORA_ENABLE_GLFW_GPU_GL` 编译进库），窗口创建或 GPU 初始化失败自动回退软件纹理路径；`WgpuOptions` 与 `create_window(WgpuOptions)` 工厂随 `AURORA_BACKEND_GPU_WGPU` ∧（`AURORA_BACKEND_WIN32` ∨ `AURORA_BACKEND_X11` ∨ `AURORA_BACKEND_WAYLAND`，§8.8）编译，Linux 两宿主宏并开时编译期取 X11（Wayland 宿主经 `WaylandOptions` + `GpuWgpu` 或 `create_native_window` 会话选择直达）。

| 后端 | 说明 | 开关 |
|:---|:---|:---|
| `HeadlessSurface` | 内存帧缓冲 / 离线 PNG，软件 `Painter` | `AURORA_BACKEND_HEADLESS`（默认 ON） |
| `Win32Surface` | Win32/GDI，零三方依赖，仅 `_WIN32` | `AURORA_BACKEND_WIN32`（Windows 默认 ON） |
| `D3D11Surface` | D3D11 GPU 增量上屏，Win32 专属 | `AURORA_BACKEND_D3D11`（默认 OFF） |
| `WgpuSurface` / `WgpuX11Surface` / `WgpuWaylandSurface` | wgpu GPU 栅格上屏（帧级 DisplayList 在 GPU 端光栅化），Win32 / X11 / Wayland 宿主 | `AURORA_BACKEND_GPU_WGPU`（默认 OFF，§8.8）∧ 宿主后端 |
| `GlfwSurface` | GLFW/OpenGL 3.3 兼容剖面 | `AURORA_BACKEND_GLFW` |
| `X11Surface` | X11/Xlib 原生窗口，仅 Linux 桌面 | `AURORA_BACKEND_X11`（默认 OFF） |
| `WaylandSurface` | 原生 Wayland（`wl_shm` + `xdg-shell` + `xkbcommon`） | `AURORA_BACKEND_WAYLAND`（默认 OFF） |
| `MacOSSurface` | AppKit / CoreGraphics，仅 Apple | `AURORA_BACKEND_MACOS`（默认 OFF） |
| `WasmSurface` | Emscripten / Canvas 2D，浏览器 rAF 驱动 | `AURORA_BACKEND_WASM`（默认 OFF） |

全部开关的默认值与 `AURORA_BUILD_*` / `AURORA_ENABLE_*` 完整列表见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)。

**Wayland 上屏与 configure 竞态**：`wl_shm` 双缓冲槽在 `pick_slot` 内用 `wl_display_roundtrip` 等 `wl_buffer.release`，这是**一帧之内唯一的事件派发点**。其间的 `xdg_toplevel.configure` 会把表面 `size` 立刻改成新几何，而它请求的同步重绘又被 `present_root` 的重入护栏吞掉（同一帧栈内），于是 painter 与缓冲槽仍按旧尺寸分配。照旧 attach 就是一副「旧尺寸 buffer + 新 configure 态」，Weston 直接判协议错误并杀连接（实测报文 `xdg_surface buffer (3840 x 2088) does not match the configured maximized state (3840 x 2160)`，最大化→全屏切换瞬间命中）。故 `present()` 在 attach 前比对 painter 尺寸与 `size × scale`：不合即丢帧不 commit（脏区原样留下），并挂起「下次事件泵补一帧」——此刻已离开渲染栈，重绘请求不再被吞。`WgpuWaylandSurface` 的软件回退帧走同一 `WaylandSurface::present()`，故同受保护。

**自动选择**：不显式指定时 `App::run()` 经 `auto_detect_surface()` 自动选用，优先级为原生 Wayland / X11 / MacOS / Wasm > Win32 > Glfw > Headless；X11 与 Wayland 同时编译时按运行期会话类型择优（`WAYLAND_DISPLAY` 存在选 Wayland，否则 X11）。`create_native_window()` 在 Linux 上按同序尝试并在真实显示不可用时回退 `Headless`。

**自定义后端入口**：扩展点收口于 `Surface` 子类与 `create_window` 工厂，而非 `Application` 构造重载。`Application` / `App` 只认两种形态——(a) `create_window(XxxOptions)` 产出的 `unique_ptr<Window>`；(b) 任意自定义后端经 `Application(Scene, unique_ptr<Surface>, WindowOptions)` 或 `App().surface(...)` 注入的 `unique_ptr<Surface>`。空 `Surface` / `Window` 仅告警降级。无头便捷构造 `Application(Scene, w, h)` 保持不变。

**DPI 感知**：`enable_dpi_awareness()`（`window/window.h`）在进程创建**任何窗口之前**启用高 DPI 感知。这是 **OS/进程级**设置，非 per-Window、非 per-Surface——Win32 经 `SetProcessDpiAwarenessContext`（Per-Monitor V2 → V1 → `SetProcessDPIAware`）一次性启用；macOS 与 Linux 无需 opt-in，为空实现。**关键不变量**：必须在 `init_console()`（`AllocConsole` 会创建控制台窗口）与 `create_window()` 之前调用，否则 Windows 上启用失败会退化为 DPI 未感知（scale = 1.0）。每窗口的 scale 查询仍是各 `Surface::scale_factor()` 的职责，与「启用」正交。

### 8.6 显示列表与 RHI 后端

`DisplayList`（`render/display_list.h`）是**绘制指令的唯一来源**：`Painter` 在录制态把每条上屏原语追加为一条 `DrawCmd`——几何 / 颜色 / 标量内联，文本字符串 / 渐变色标 / 字体 / 图像 / 变换矩阵等**变长数据入池**、命令只持下标（避免每条命令内嵌大对象）。命中缓存时父级整树一次 `replay` 即可压平重放。

**回放目标抽象**：`replay` 的目标是 `rhi::RhiBackend`，而非具体的 `Painter`。

```cpp
namespace aurora::rhi {
struct CmdData {  // 池下标解析出的只读指针；不引用的字段为 nullptr
    const std::string *text = nullptr;
    const Font *font = nullptr;
    const std::vector<Color> *colors = nullptr;
    const std::vector<float> *stops = nullptr;
    const Image *image = nullptr;
    const Matrix2D *matrix = nullptr;
    const std::vector<Point> *points = nullptr;  // Polyline 点集
};
class RhiBackend {
  public:
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;
    virtual auto submit(const DrawCmd &cmd, const CmdData &data) -> void = 0;

    // 能力位（默认全 false）：GPU 后端按实现如实申报。
    [[nodiscard]] virtual auto capabilities() const -> RhiCapabilities { return {}; }

    // 流式纹理常驻槽（逐帧更新通道，见 §8.7）：键寻址 + 版本门控增量上传。
    [[nodiscard]] virtual auto acquire_stream_image(std::uint64_t key, int width, int height)
        -> StreamImageId { return 0; }  // 0 = 后端不支持
    virtual auto update_stream_image(StreamImageId id, const std::uint8_t *pixels,
                                     std::size_t stride_bytes, int x, int y, int w, int h) -> void {}
    virtual auto release_stream_image(StreamImageId id) -> void {}

    // 原生 GPU 表面导入契约位（平台互操作，GL 恒返回 0）。
    [[nodiscard]] virtual auto import_native_surface(const NativeSurfaceFrame &frame)
        -> StreamImageId { return 0; }
};
}  // namespace aurora::rhi
```

**能力位与流式接口（后端无关契约）**：`RhiCapabilities{gpu, native_surface_import, compute}` 由 `capabilities()` 申报（`StreamImageId` 为非零句柄、0 = 不可用哨兵）。流式三接口服务**逐帧更新型内容**（视频 / 大图）：`acquire_stream_image(key, w, h)` 按 `stream_key` 取常驻纹理槽、同键复用；`update_stream_image` 按 `stride_bytes`（0 = 紧凑行）与脏矩形增量上传，版本门控在调用方；`release_stream_image` 释放（重复释放无害）。`import_native_surface` 收 `NativeSurfaceFrame`（`core/native_surface.h`，携带平台句柄与可空 `release` 回调——**先判空再调用**）。三个接口默认实现均为「不支持」（返回 0 / no-op），软件后端零负担。

`DisplayList::replay` 因此有两个重载：

| 重载 | 语义 |
|:---|:---|
| `replay(rhi::RhiBackend&)` | **唯一实现**：遍历 `cmds_`，把池下标解析为 `CmdData`（负下标 → `nullptr`）后逐条 `submit` |
| `replay(Painter&)` | 兼容薄壳：构造临时 `rhi::SoftwareRhi{p}` 后转发到上式（调用点无需改动） |

**首个后端 `SoftwareRhi`**（`render/rhi/software_rhi.h`）把 23 类 `CmdKind`（含图表原语 `Polyline` / `Sector` 与 GPU 层命令三件套，见 §8.1）逐条转发回 `Painter` 的对应原语，参数逐字段与抽取前的 `replay` 一致，故 **DC 像素输出逐位不变**（重构红线，由 `utest_rhi` 的 `SoftwareRhi` 与直接绘制逐字节比对锁定）。未绑定 `Painter` 时 `submit` 为 no-op（便于测试构造空后端）；GPU 后端 `GpuGlRhi`（§8.7）实现同一接口，成为**平级第二消费者**——新增后端不改动录制侧与 `DisplayList`。

**设计取舍**：接口收成**单一 `submit`**，而非把 18 个绘制原语各设一个虚函数。命令的几何 / 标量已全在 `DrawCmd` 里，单入口既让回放循环保持一行，也把「如何解释命令、如何合并成批次」留给后端——GPU 后端正靠这一点做管线切换与批处理，而 18 个平铺虚函数会强迫它在原语之间重新推断管线状态。`Painter` 侧无需任何改动。

**GPU 层命令三件套（`BeginLayer` / `EndLayer` / `DrawLayer`）**：带 `cache_layer` 修饰的控件在 GPU / 录制路径不再逐帧走离屏 `Composite` 重栅，而是把子树绘制**重定向到常驻层纹理**——录制侧 `Painter::begin_layer(key, size)` / `end_layer()` / `draw_layer(key, matrix, src_scale)` 分别落为三命令（Direct 直绘模式 no-op，仍走 paint_cache 位图路径）。命令字段：`BeginLayer` 携 `aux_key`（层键：进程内唯一、0 保留、`next_gpu_layer_key()` 惰性取号且永不回收）与 `bounds`（层逻辑尺寸）；`DrawLayer` 携 `aux_key` + `matrix_idx`（放置矩阵）+ `composite_scale`（层录制缩放）。整体失效由**全局层代际（epoch）**承载：消费端层存储整体丢弃或 `DrawLayer` 未命中时 bump 一次，全部控件下帧整体重录 `BeginLayer`——单帧缺口自愈，不逐帧抖动。

**`SoftwareRhi` 层仿真**（软件回退下语义对齐）：`BeginLayer` 起把后续命令缓冲进捕获栈（嵌套层成栈），`EndLayer` 触发离屏定稿——子树命令离屏重放为层位图，以层键存入**进程级全局层存储**（GPU 录制的 DL 回退软件回放时逐帧构造临时 `SoftwareRhi`，回退帧与离屏渲染共用一份存储，干净帧 `DrawLayer` 才能跨帧命中）；容量上限 64 条，超限清空并 bump epoch。`DrawLayer` 未命中时跳过本帧并 bump epoch（告警键级去重），下帧由控件重录补齐。

> **池下标是录制方契约**：`DrawCmd` 的 `str_idx` / `font_idx` / `col_idx` / `flt_idx` / `image_idx` / `matrix_idx` / `pt_idx`（`Polyline` 点集）由 `Painter::record*` 生成，回放侧**只解析、不构造**；`DisplayList` 的 `string_at` / `colors_at` / `floats_at` / `font_at` / `image_at` / `matrix_at` / `points_at` 只读访问器即为此提供。
>
> **标量槽位**：`DrawCmd` 的 `f0..f3` 按命令语义取用（`Sector` 占满四槽：外半径 / 内半径 / 起角 / 止角），新增原语优先复用既有槽位，确需扩展时才加槽（+4B/命令）。
>
> **`CmdKind::Composite` 的例外**：离屏合成在录制态**必须**录制为命令（见 §8.1 录制态不变量），但其像素来源是离屏缓冲快照（`Image`），故 `CmdData` 的 `image` / `matrix` 两字段专供它使用。

### 8.7 GPU GL 栅格后端（GpuGlRhi）

定义于 `render/rhi/gpu_gl_rhi.h`，实现 `src/aurora/render/gpu/gpu_gl_rhi.cpp`；同时实现 `RhiBackend`（§8.6 命令消费）与 `RhiFrameSink`（帧调度，`render/rhi/rhi_frame_sink.h`）。开关 `AURORA_ENABLE_GLFW_GPU_GL`（默认 OFF，依赖 `AURORA_BACKEND_GLFW`，见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) §3）。

**职责切分**：GL 上下文创建与 swapBuffers 呈现归所在 Surface（GLFW）；本类只做「DisplayList → GL 批渲染」。GL 函数表 `GLFn` 由 `load_gl(proc)` 经加载回调逐名装载（如 `glfwGetProcAddress`），**自写最小 loader，无 GLAD/gl3w 三方依赖**；公共头不含任何 GL 原生头（类型以 `GLenum_` 等同宽别名承载）。本类与 `load_gl` / `GLFn` **恒编译进库**（不裁切于 feature 宏）：宏只控制 `GlfwSurface` 是否接线 GPU 模式，未开启时本类同样可显式装配（供测试桩与自定义 Surface）。

**帧调度契约（`RhiFrameSink`）**：`Window::present_root` 在 GPU 路径按序 `begin_frame(设备宽, 设备高, scale)` → `replay(sink.backend())` → `end_frame`，随后 `Surface::present()` 完成 swap。GPU 路径恒全量重绘（`begin_frame` 重置零基底并复位裁剪/alpha 态）；`end_frame` 完成 flush + blit 到默认帧缓冲——resolve 纹理仍新鲜（本帧效果 pass 已 resolve 且其后无新绘制）时直接从它上屏，否则 MSAA 直 blit（省一次无人消费的中间 resolve；`read_pixels` 按需补做）。`begin_frame` 返回 false = 后端不可用，调用方本帧回退软件路径，此后视该后端永久失效（`valid()` 转 false）。

**批渲染模型**：`submit` 只做「命令 → 顶点/状态」翻译（不触 GL），GL 调用集中在批 flush 与 `end_frame`。批切分**保序不重排**：管线（Solid/Border/Grad/Image/Text/Shadow）、裁剪态、混合态、纹理任一变化即断批；全局 alpha 烘焙进顶点色不断批。索引缓冲 quad 复用、顶点 `pos2f + uv2f + color4ub`（20 字节）。`FrameStats{draw_calls, vertices, skipped_cmds}` 供诊断与测试断言。

**语义同源承诺**（与软件路径逐公式对齐，非视觉近似）：

| 命令族 | GPU 实现 |
|:---|:---|
| 几何 / 状态 / 裁剪 | 矩形与圆角裁剪统一走 shader 内 SDF alpha（不用 scissor、不 discard），栈顶 = 各层矩形交集（与 `Painter::push_clip` 交叠语义一致） |
| LinearGradient / RadialGradient | 256×1 LUT 纹理采样，LUT 内容复现软件 `sample_gradient` 的取值语义；渐变几何参数属批 key，同 LUT 可合批 |
| DrawImage | 上传时预乘 alpha（PMA），混合 `ONE / ONE_MINUS_SRC_ALPHA`，双线性采样（在 PMA 空间插值，与 §9.1 同理）；纹理按 `Image::content_hash()` 惰性摘要 + 维度缓存（`DisplayList::add_image` 预热源对象，逐帧拷贝零重算；直接改写 `pixels` 后须 `invalidate_content_hash()`） |
| Composite | 仿射矩阵直烘四角顶点（uv = 源逻辑角点归一化），NEAREST 逐像素 floor 取样同软件 `composite_pixels`；空像素 / 维度非法 / 缓冲不足与软件同形跳过 |
| DrawText | 光栅化**复用软件 `GlyphAtlas`**（经字形发射桥 `emit_text_glyphs`——shaping / 行切分 / 基线 snap / 间距推进与 `FontEngine::draw_text` 单一代码路径），GPU 侧**多页**架式打包 A8（R8）图集：常规页 1024² 满页开新页，页数达 8 后 LRU 页淘汰（清槽位、复用纹理，淘汰前 flush）；超大字形开 ≤ 2048² 专用页，放不下放弃。`tex_sub_image_2d` 槽位增量上传，槽位携带页纹理与预归一化 uv（跨页文本自然断批）；LCD 子像素模式不进 GPU（一律灰度）。页边长可经 `set_glyph_page_size` 注入（测试覆盖翻页/淘汰路径） |
| Shadow | 单 quad 覆盖扩展区（外扩 `blur × 2`），shader 内到阴影矩形欧氏距离线性衰减 `max(0, 1 − dist/blur)`，与软件 `draw_shadow` 衰减因子同构；`blur ≤ 0` 硬阴影退化为 Solid 实心 quad |
| BlurRegion | 两遍分离 box blur（水平 → 垂直）经 resolve → temp FBO ping-pong，tap 钳制在区域内；半径 `max(1, trunc(radius × scale))`、区域 floor/ceil + 画布钳制与软件同形；`radius ≤ 0` 直接跳过 |
| BlendRegion / MaskRegion | 单 pass 采样 resolve 纹理直写回 MSAA（直写替换，混合禁用）；Blend 八模式枚举序与 shader `u_mode` 分支一一对应，Mask 三种渐变因子按区域内像素索引计算；`strength` 截断 `[0,1]`、`≤ 0` 跳过与软件同形 |

**效果 pass 机制**：已绘内容位于 MSAA 渲染缓冲（不可采样），效果命令前先把 MSAA resolve 成纹理（`msaa_dirty` 门控：同帧连续效果只在内容变化后重新 blit）。纹理缓存（渐变 LUT / 图像纹理）溢出淘汰前先 flush 当前批——待提交批可能仍引用将被删除的纹理（与字形图集满页 flush 同因）。

**GPU 层缓存（`cache_layer` 对齐，见 §8.6 层命令三件套）**：`BeginLayer` 把绘制重定向到 FBO 常驻层纹理（直色 RGBA，与画布同构；层键缺席或尺寸变化才重建，重建前先 flush 待提交批），随后清空层内裁剪栈（层局部坐标，全局 alpha 保持）；`EndLayer` 定稿并恢复 MSAA 目标与状态；`DrawLayer` 走 Image 管线合成——NEAREST 逐像素取样与软件 `composite_pixels` 同语义、放置矩阵直烘四角顶点、非 PMA 直色混合。层内效果命令（Blur/Blend/Mask）经**惰性分配的 aux 采样拷贝**（aux FBO + 纹理）读取层内容，不污染主画布；尺寸变化时 aux 一并重建。`DrawLayer` 未命中常驻层纹理（后端重建等冷存储）时告警（键级一次性）+ bump 全局 epoch + 本帧跳过，下帧控件重录自愈。干净帧子树零重栅：仅一条 `DrawLayer` 合成。

**流式纹理常驻槽**：`RhiBackend` 流式接口（§8.6）的 GL 落地——`stream_key` 寻址**固定纹理槽**，同键跨帧复用（不重建 storage，尺寸变化才重定义），`stream_version` 变化触发**增量 sub-upload**（`UNPACK_ROW_LENGTH` 承载跨距行、脏矩形区域上传、PMA 预乘下沉到上传时），逐帧视频 / 大图更新免全量重传与逐帧新建纹理。防御性 no-op：句柄 0 / 空指针 / 零尺寸 / 越界脏矩形；`release_stream_image` flush 后删除纹理（重复释放无害）。`DrawImage` 命令路径同样感知 `Image::stream_key`（非 0 时绕过 `content_hash` 纹理缓存直走流式槽，与显式流式 API 共用同一槽表）。

**能力位与原生表面导入（后端无关契约）**：`capabilities()` 返回 `RhiCapabilities{gpu, native_surface_import, compute}`——GL 3.3 core 下 `gpu = true`、`compute = false`（无 compute 内部加速路径）、`native_surface_import = false`。`import_native_surface(frame)` 为平台原生 GPU 表面零拷贝导入的**契约位**（供 D3D11 / Metal 互操作后端未来兑现）：GL 侧恒不支持，warn-once 后返回 0（0 = 不可用哨兵），不抛异常、不破坏帧状态。

**初始化失败链**（函数表缺项 / GL 版本不足 / 着色器链接失败 / GL 错误）→ `valid()` 为 false，调用方整体回退软件路径，**不做逐命令混合**。`Surface::gpu_backend()` 非空时其 `name()` 恒为 `"gpu-gl"`；首帧初始化失败时 Window 内部永久回退软件路径（`gpu_backend()` 仍可能非空——契约只断言「非空即 `gpu-gl`」）。`read_pixels()` 提供当前帧内容读回（诊断 / 快照用）：懒 resolve——调用时按需补 MSAA→resolve blit；调用窗口为 `end_frame` 之后、下一次 `begin_frame` 之前；GLFW 侧 `data()` 在 DEBUG 下经此懒读回（无消费者时零 GPU→CPU 读回成本，Release 恒空）。

**测试**：`utest_gpu_gl_rhi` 以 fake GL 驱动桩（全量填充 `GLFn` + 调用记录）覆盖帧生命周期 / 各管线批切分 / 渐变 LUT 内容 / PMA 上传 / 字形图集子上传 / 效果 ping-pong 序 / 初始化失败链 / 能力位与流式拒绝 / 流式槽版本门控子上传 / 流式公共 API 契约（复用 / 跨距 / 防御 no-op）/ 层缓存生命周期与 miss 自愈；`utest_gpu_layers` 覆盖层键唯一性与 epoch 单调、`NativeSurfaceFrame` 契约、`Painter` 层命令录制与 `SoftwareRhi` 层仿真（roundtrip 逐位一致 / miss 跳过 + bump / 跨实例共享存储）；`itest_gpu_layer_cache` 覆盖控件级首帧层录制 / 干净帧仅 `DrawLayer` / 失效重录且像素与直绘一致 / epoch 推进整体失效 / miss 自愈闭环 / 尺寸变化重录；`itest_gpu_gl_smoke` 在真实 GLFW 窗口验证 GPU 模式呈现与后端身份契约（无显示环境自动 SKIP）。

### 8.8 GPU wgpu 栅格后端（WgpuRhi）与 WgpuSurface

跨平台 GPU 主力路径：同一套 WGSL 管线经 **wgpu-native**（Rust，gfx-rs v29，`third_party/wgpu-native/` 源码 vendored、保持上游原样）覆盖 Vulkan / D3D12 / Metal / GLES，由 cargo 构建为静态库链入（开关 `AURORA_BACKEND_GPU_WGPU`，默认 OFF，工具链要求见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) §3.8）。类声明以 `#ifdef AURORA_BACKEND_GPU_WGPU` 整体门控（区别于 `GpuGlRhi` 的恒编译口径）——特性关闭时头不导出声明、库内无实现 TU。公共头 pimpl 隔离，`webgpu.h` / `wgpu.h` 不外泄。

**WgpuRhi**（`render/rhi/wgpu_rhi.h` + `src/aurora/render/gpu/wgpu_rhi.cpp`）：`RhiBackend` + `RhiFrameSink` 双实现，命令语义、批切分与裁剪/混合口径以 §8.6/§8.7 为同源参照——shader 内 SDF 裁剪、256×1 LUT 渐变、PMA 图像上传、字形复用软件 `GlyphAtlas`（`emit_text_glyphs` 单一代码路径）、GPU 层缓存与流式纹理常驻槽齐备。装配选项 `WgpuRhiOptions{backend, native_window, native_display, offscreen_width/height, vsync}`：`native_window = nullptr` 为**离屏模式**（渲染目标内部纹理，`read_pixels` 可全帧读回，测试/探针通道）；宿主模式接 Win32 HWND、X11（`WGPUSurfaceSourceXlibWindow`，`native_window` 为 XID `Window` 全宽装入 void*，须同时给 `native_display` = `Display*`）与 Wayland（`WGPUSurfaceSourceWaylandSurface`，`native_display` = `wl_display*`、`native_window` = `wl_surface*` 双 void* 直传）；Linux 两协议句柄无法自述归属，由 `WgpuRhiOptions::linux_host{X11|Wayland}` 判别（默认 X11）。`backend` 默认 `Auto`（Windows 偏好 D3D12 → Vulkan → GLES），运行期实测以 `backend up (<api>, surface=host|offscreen)` 日志申报。初始化失败（无 adapter / device 申请失败）→ `valid()` false，调用方整体回退软件路径，不做逐命令混合。与 GL 的读回窗口差异：wgpu `read_pixels` 仅离屏模式提供且必须在 `end_frame`（GPU 提交 + 读回映射登记）之后、下一次 `begin_frame` 之前调用。读回是**单帧深度**的通道：帧尾登记一次拷贝、`read_pixels` 取走即解除映射，故「连帧提交、只消费末帧」（吞吐基准、批尾排空）是合法用法——上一帧未被消费的待 map 由下一次 `begin_frame` 退役（等映射就绪后 `Unmap`，再登记本帧拷贝）。`set_readback_enabled(false)` 整体关闭帧尾拷贝与映射登记（关闭期间 `read_pixels` 恒返回 false），用于**连帧提交而不逐帧读回**的测量场合；重新打开后自**下一帧**起恢复。真窗口（swapchain）模式本就不登记读回，该开关无作用。能力位 `gpu = true`、`compute` 随所选后端（GLES 为 false）——true 时已有实路径：静态大图（`stream_key = 0`、边长 ≥ 4）首用时由 `cs_mip` compute shader 逐级生成整条 mip 链（2×2 box 均值、边缘钳位），`DrawImage` 缩小绘制走三线性 mip 采样，降采样不再走样（画质对软件/GL 的 lod0 双线性是**已接受偏差**：后两者缩小即混叠）；`compute = false`（GLES 兜底）或 compute 管线构建失败时能力位如实回落 false，纹理退回单级 + lod0 采样，行为与本切片之前一致。流式槽与层纹理恒单级（逐帧重传内容与 mip 语义冲突）。`native_surface_import` 恒 false——wgpu-native v29 C API 无外部共享纹理导入入口，`import_native_surface` 与 GL 同款 warn-once 返回 0（兑现路径随上游 C API 扩展）。批顶点为无索引三角列（单 fill quad = 6 顶点，与 GL 索引 quad 的 4 顶点计数不同形）。

**WgpuSurface**（`window/wgpu_surface.h`，门控 `AURORA_BACKEND_GPU_WGPU ∧ AURORA_BACKEND_WIN32`）：复用 `Win32Window` 宿主（消息泵 / 事件翻译 / DPI / 光标 / IME / UIA 桥与 `Win32Surface` / `D3D11Surface` 同款转发），`Surface::gpu_backend()` 返回嵌套 `Sink` 适配器（`name()` 恒 `"gpu-wgpu"`，逻辑 dp × scale → 设备像素后转发 `WgpuRhi::begin_frame`）。帧序：`present_gpu_frame` 走 begin→replay→end（`end_frame` 内完成 submit + `wgpuSurfacePresent`，`present()` 对 GPU 帧为 no-op）；GPU 初始化或首帧失败后**永久回退** GDI `SetDIBitsToDevice` 上传路径（`gpu_active()` 为诊断读口）。`WgpuOptions{vsync}` + 专属工厂 `create_window(WgpuOptions)`（构造期 adapter/HWND 不可得即返回错误）；`Win32Options.renderer = GpuWgpu` 为**强制路由**——不可用报 `renderer-unavailable`，不静默降级，`Auto` 偏好的优先序不含它。

**WgpuX11Surface**（`window/wgpu_x11_surface.h`，门控 `AURORA_BACKEND_GPU_WGPU ∧ AURORA_BACKEND_X11`，Linux）：与 `WgpuSurface` 同族、同帧调度契约（同一 `Sink` 适配器口径、`name()` 恒 `"gpu-wgpu"`、失败分层与 `gpu_active()` 读口一致），差别在宿主实现方式——**组合**内嵌 `X11Surface`（窗口创建 / 事件泵 / 光标 / 标题 / 几何全走它，本类不含任何 Xlib 调用，句柄经 `X11Surface::native_display()` / `native_handle()` 以 void* 同源取得后 `reinterpret_cast`），而非像 Win32 版那样共享宿主类；软件回退 = `present()` 委托内嵌 `X11Surface::present()`（XPutImage）。`create_window(WgpuOptions)` 在 Linux（X11 宏开启时）即产此类；`X11Options.renderer = GpuWgpu` 同款强制路由。与 Win32 版差异如实申报：X11 侧无 per-monitor DPI 回调、无 UIA / IMM32 等价物（`X11Surface` 本就未覆写这些扩展点）。

**WgpuWaylandSurface**（`window/wgpu_wayland_surface.h`，门控 `AURORA_BACKEND_GPU_WGPU ∧ AURORA_BACKEND_WAYLAND`，Linux）：与 `WgpuX11Surface` 同构的**组合**宿主——内嵌 `WaylandSurface`（窗口壳 / 事件泵 / xkb 输入 / CSD / 几何全走它，本类零 wayland-client 调用，句柄经 `WaylandSurface::native_display()` / `native_handle()` 同源取得并以 `linux_host = Wayland` 装配），GPU 端经 `WGPUSurfaceSourceWaylandSurface` swapchain present 直渲同一 `wl_surface`；软件回退 = `present()` 委托内嵌 `WaylandSurface::present()`（wl_shm）。`wl_surface` 在 wgpu 接线前已被 `xdg_toplevel.configure` 定型（内嵌宿主构造阻塞等齐首个 configure），无「未配置即提交」窗口。路由：`WaylandOptions.renderer = GpuWgpu` 强制路由（同款不静默降级）；`create_native_window` 对 `GpuWgpu` 偏好**运行期按会话择宿主**——有 `WAYLAND_DISPLAY` 走 Wayland 工厂，否则 X11（与软件路径的会话选择同序）。与 X11 版差异如实申报：① 无 `capture_window`（Wayland 协议无抓屏原语，内嵌宿主同样未覆写，基类默认报 disabled 即最终行为）；② GPU 帧**含自绘 CSD 装饰**：`Sink::end_frame` 先经内嵌宿主 `WaylandSurface::record_client_decoration(dl)` 把装饰命令录入当帧 DL 再回放（app 内容之上、同一 swapchain 帧），不再有「swapchain 不 commit 软件缓冲 → 自绘标题栏消失」的降级观感；装饰光栅实现单一来源为 `csd::paint_title_bar`（`window/detail/title_bar_painter.h`），软件路径（`Impl::draw_decoration`）与 GPU 录制路径同为它的消费者，两路径逐位一致由 `utest_title_bar_painter` 的「直绘 vs 录制回放」全画布字节比对锁死；录制用宿主内独立 `Painter`（app 帧正在录制时嵌套会污染帧 DL），无装饰可绘（SSD 合成器 / 无标题栏且无可见边框）时返回 false 且不触传入的 DL。回退与合成观测口：三宿主统一提供 `software_present_count()`（GPU 生效期间经软件路径上屏的帧数，非零即「app 帧绕过 GPU 通道」= 白闪签名）与 Wayland 版额外的 `decoration_replay_count()`（装饰回放进 GPU 帧的帧数；SSD 合成器恒 0，CSD 兜底合成器逐帧递增）。

**运行时契约注意**（wgpu-native v29）：`wgpuBufferGetMapState` / `wgpuInstanceWaitAny` 为未实现桩（panic=abort），映射状态自跟踪；`bufferMapAsync` 必须在 `queueSubmit` 之后登记（提交前登记映射触发 Validation panic）；`wgpuSurfaceGetCurrentTexture` 取回 surfaces 纹理须持有到 submit + present 之后再释放（提前释放 → Validation panic）；Win32 surface 描述符必须携带真实 HINSTANCE（NULL 使 `wgpuSurfaceGetCapabilities` 返回 Error）；Xlib surface 描述符要求 `display` 与 `window`（XID）同源且 XID 以 `uint64_t` 全宽传递（上游 conv.rs 将 screen 固定为 0，单屏环境无碍）；`WGPUTextureViewDescriptor` 的 `arrayLayerCount` 零初始化即 invalid（C FFI 侧 panic 不可 unwind，非可恢复错误），显式单层须写 `1`；compute pass 只能录在**无开场 render pass** 的 encoder 上（mip 生成前须 `flush_batch` + `close_pass`，录完 `ensure_target_pass` 重开）；离屏读回的映射生命周期：帧尾拷贝的 `bufferMapAsync` **提交之后**登记，且同一 `readback` 缓冲在上一份映射解除前不得再次录拷贝（否则 `wgpuQueueSubmit` Validation 错误经 C FFI 冒出来即 Rust panic，非可 unwind 错误 → 进程 abort），故 `begin_frame` 先退役待 map、`end_frame` 仅在 `!map_armed` 时登记。等待映射就绪只能用 `wgpuInstanceProcessEvents` + 睡眠轮询（v29 的 `wgpuInstanceWaitAny` 是未实现桩），睡眠粒度受平台时钟量子影响（Windows 实测一次 `sleep_for(1ms)` ≈ 一整个量子）——**每帧一次 `read_pixels` 的测量口径会把所有 wgpu 场景压到同一个地板值**，吞吐对比必须走「连帧提交 + 批尾一次排空」（见 `tools/bench/bench_gpu.cpp` 场景三/四/五）。

**测试**：`utest_title_bar_painter` 锁定 CSD 装饰光栅单一实现（`src/aurora/window/detail/title_bar_painter.cpp`）——「Painter 直绘」与「`record` → `replay` 回放」两路径全画布字节逐位相同（三套视觉语言各一例，状态携带图标 + 标题 + hover 使 DrawImage/DrawText/RoundedRect/DrawLine 全部参与）、门控态（无装饰/全屏未revealed）不出像素、Adwaita hover 圆底与关闭红、Windows 整高矩形 hover、Mac 交通灯无需 hover、非激活调色与隐藏槽位位移；`utest_wgpu_rhi` 离屏真实设备通路（无 adapter 自动 SKIP）——占位构造契约 / 帧生命周期与精确像素（裁剪内外 / ClearRect 基底 / 逐帧统计复位 / 尺寸变化）/ 流式槽键稳定与 `import_native_surface` 不兑现契约 / compute mip 链（64×64 逐纹素棋盘 4× 降采样整块为均值色、1:1 绘制仍端点色，GLES 无 compute 端 SKIP）/ 读回通道开关与连帧 submit（连帧不逐帧消费 `read_pixels` 不踩「缓冲仍映射」验证错误、末帧色可读，关闭期 `read_pixels` 拒绝、重开后下一帧恢复）/ 装饰 DL 叠加内容帧回放的读回像素与软件直绘同点位吻合；`itest_wgpu_present` 真实宿主窗口（Win32 / X11 / Wayland，宿主别名随编译择一）三帧不回退 + `GpuWgpu` 强制路由与 `Auto` 不受影响 + `software_present_count()` 恒 0（含开窗初期 map/configure 系统重绘突发），另有 `wayland_host_gpu_routing_and_frames` 用例不经别名直达 `WgpuWaylandSurface`（X11/Wayland 并开构建也验到 Wayland 宿主；`AURORA_BACKEND_WAYLAND` 未开启时 SKIP 桩）并锁 `decoration_replay_count()` 的 SSD/CSD 两态口径，`wayland_csd_decoration_replays_into_gpu_frame` 再按规格 §4.1 强制 `DecorationPolicy::ClientSide`（合成器提供 SSD 也自绘），把该实路径从「两态口径」变成确定断言——`content_inset().top > 0` 佐证 CSD 真生效、逐帧 `decoration_replay_count()` 增量 ≥ 出帧数、`software_present_count()` 仍恒 0，故 Weston 这类 SSD 合成器下也能拿到「装饰合成进 GPU 帧」的机器判据（无桌面会话或无 adapter SKIP）。**GPU 容差 golden 层**由 `itest_wgpu_golden`（wgpu 侧）与 `itest_gl_golden`（GL 侧，需 `AURORA_BACKEND_GLFW` + `AURORA_ENABLE_GLFW_GPU_GL` 配置，否则落 skip 桩）共同承担（`golden::compare_gpu_tolerance`，判据词汇见 §8.4.2）：几何 AA（`painter_polyline` / `painter_sector`）、文本字形（`golden_basic_column`）、复合控件图表（`chart_bar`）四场景经 `tests/support/gpu_golden_scenes.h` 单一来源提供，两侧共用同一批场景与同一张**软件 SSOT 基线 PNG**——同一 DisplayList 经离屏后端重放读回（`WgpuRhi` 直驱离屏；GL 侧经隐形 GLFW 窗口取 3.3 core 上下文，`GpuGlRhi` 的 MSAA/resolve 帧缓冲自持、不触默认帧缓冲，故窗口不参与渲染；GL 读回自底向上行序由共享帧装配翻转归一），与基线按逐场景申报的容差带（tol 48 + 场景预算）比对。实测漂移四值两侧同值到个位（Windows 与 WSLg 亦同值，预算取实测 ≈2.3×）：两条 GPU 实路径吃同一份 Painter 三角化顶点与规范化的 4× 多重采样位，对软件参考点的残差是同一个确定性量；差异像素 >0 本身即证明未静默回退软件路径。容差带仍按后端各自申报、不共享常量（某侧换 AA 方案时只有该侧重校准），GPU 输出永不回写基线，软件侧零漂移仍由各同名 utest 逐位红线守门。逐位 golden 红线仍唯一属软件路径：GPU 后端不做逐位 golden（跨驱动 AA/采样不可复现，与 GL 路径同口径），真机验收由探针 `aurora_verify_win32_wgpu` / `aurora_verify_x11_wgpu` / `aurora_verify_wayland_wgpu`（`tools/verify/`，不进 CTest）覆盖：宿主装配 / 能力契约 / 流式逐版本像素 / 层缓存跨帧持久性 / compute mip 采样 / 多帧上屏 **18 项**（`check()` 实际执行数，WSLg/Windows 实测 `[PASS]` 行数为准；X11 版另加 XGetImage 截图落盘物证共 **19 项**；Wayland 版无截图项、另加软件上屏帧数恒 0 与 CSD 装饰回放两态口径共 **20 项**，并以 `--interactive` 人工目视核对装饰像素与还原过程无白闪）+ `--interactive` 人工目视段。

### 8.9 无障碍桥扩展点（Surface）

`Surface` 上另有**两个**无障碍扩展点，均有默认空实现，故自定义后端不覆写即退化为「无无障碍桥」（源码兼容）：

| 方法 | 说明 |
|:---|:---|
| `accessibility_provider() const -> a11y::Provider*` | 虚方法，**基类默认返回 `nullptr`**；返回本窗口的无障碍桥。**只读、不构造**——桥是惰性构造的（首个平台查询到达才存在），故此处返回 `nullptr` 不等于「本后端不支持无障碍」。Win32 家族三路（`Win32Surface` / `D3D11Surface` / `WgpuSurface`）都转发**同一个** `Win32Window` 宿主持有的桥实例，使 id → Widget* 映射不分裂 |
| `set_accessibility_root(Widget* root) -> void` | 每帧由 `Window::present_root` 在**布局与绘制完成之后**调用，注入语义树根。为何不直接调 `accessibility_provider()->set_root()`：桥惰性构造，首个查询到达时它才存在，此刻若还没有注入记录就无根可投影——根必须由**恒存在**的宿主承接，桥构造后由宿主补喂 |

`surface.h` 仅前向声明 `aurora::a11y::Provider` 与 `aurora::Widget`，完整定义在 `core/a11y_provider.h` / `widget/widget.h`，公共头零平台污染。语义树几何取自布局与绘制产物（`paint_bounds` 语义），故注入点必须在 `paint` 之后。桥抽象、重建模型与根的生命周期不变量见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md) §8.5。

> **真机验收（无头 CI 无法覆盖的部分）**：桥与 Windows 的接缝（`WM_GETOBJECT` 能否应答根对象、树能否 `Navigate`、属性是否有值、pattern 能否 QueryInterface 到）只能在本机真实窗口上证明。`tools/verify/win32_ua_live_probe.cpp` 以 **COM UIA 客户端**（`CUIAutomation8`，与 NVDA / Narrator 同路径）`ElementFromHandle` 取根，再用**控件视图**遍历器先序下钻，逐节点读属性与 pattern 并与期望表比对；`FrameworkId == "Aurora"` 用于把桥投影的元素与 UIA 默认 HWND provider 合成的非客户区（标题栏 / 系统菜单 / 最小化-最大化-关闭）区分开。验收范围与退出码语义见源文件头注释。

---

## 9 图像与媒体

### 9.1 图像

`image/image_codec.h` 提供图像编解码，能力由编译期开关 `AURORA_ENABLE_IMAGE_JPEG` / `AURORA_ENABLE_IMAGE_WEBP` / `AURORA_ENABLE_IMAGE_PNG` 控制（见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)）。

`Painter::draw_image(const Image&, const Rect&)` 采用双线性采样，在 **premultiplied-alpha 空间插值**，避免半透明边缘暗边与光晕。

`Image::content_hash()`（`core/image.h`）提供像素内容的 FNV-1a 64 位惰性摘要：首次调用计算并缓存（const 访问经 mutable 落回本对象），拷贝携带缓存；GPU 纹理缓存等内容寻址消费方经此寻址，免除每帧全量哈希。**契约**：直接改写 `pixels` 后必须调用 `invalidate_content_hash()`，否则摘要过期、内容寻址消费方可能命中旧内容。

`Image` 另携**流式章**：`stream_key` / `stream_version`（均 0 = 非流式）。`stream_key != 0` 时 GPU 后端按键寻址固定纹理槽（不走 `content_hash` 缓存、不参与通用缓存淘汰），`stream_version` 变化即触发增量 sub-upload——视频 / 大图逐帧更新免全量重传（见 §8.7 流式纹理常驻槽）；软件路径不感知该章。

### 9.2 VideoPlayer

`VideoPlayer`（`media/video_player.h`）是视频播放控件，可子类化定制。

**播放控制**：`play()` / `pause()` / `toggle_play()` / `is_playing()` / `seek(microseconds)` / `seek_fraction(double)` / `position()` / `position_fraction()` / `duration()` / `set_volume(double)` / `set_muted(bool)` / `volume()` / `muted()`。

**数据源**：`set_source(shared_ptr<VideoSource>)` / `source()`；`set_fit(BoxFit)` / `fit()`。

**帧数据与 GPU 流式通道**：帧以 `VideoFrame`（`media/video_source.h`）承载——`image`（解码像素，原生表面路径可空）+ `native_surface`（平台原生 GPU 表面变体，None = CPU 路径）+ `pts`。播放器为每个实例惰性分配流式键：新帧到达时给 `Image` 盖 `stream_key` / `stream_version` 章，绘制随 `DrawImage` 命令进 GPU 流式常驻纹理槽（§8.7）——同键跨帧复用纹理、版本变化仅增量子上传，逐帧播放免全量重传；软件路径照常栅格，无额外成本。

**控件与回调**：`set_show_controls(bool)` / `show_controls()` / `set_controls(unique_ptr<Widget>)` / `set_on_tap(fn)` / `set_on_double_tap(fn)`。

**子类定制点**（均为 `virtual` 或 public，可在派生类覆盖）：

| 成员 | 说明 |
|:---|:---|
| `create_default_controls()` | `virtual`，返回默认控制条；在挂载期生效 |
| `current_frame()` | 读取当前帧 `const Image&` |
| `paint_frame(Painter&, const Rect&)` | 绘制当前帧 |
| `on_frame(const Image&)` | `virtual`，新帧到达 |
| `on_playback_tick(time_point)` | `virtual`，播放推进 |
| `on_tap()` / `on_double_tap()` | `virtual`，手势回调 |
| `on_pointer_event(MouseEvent&)` / `wants_click()` | 输入处理（public override） |

**信号**：`playing_signal()` / `progress_signal()` / `volume_signal()` / `muted_signal()` 返回对应 `Reactive<...>*`。

### 9.3 VideoControls

`VideoControls`（`media/video_controls.h`）是默认控制条，可子类化换肤或重排。

- 访问器：`play_button()` / `time_text()` / `mute_button()`。
- 虚函数：`build_children()` 重建子控件布局。
- 静态工具：`format_time(long long ms)`。

### 9.4 音频图（media/audio.h）

`media/audio.h` 提供声明式音频图：`AudioContext` 为图与设备生命周期所有者，`AudioNode` 为图节点，信号自源节点流向 `AudioDestinationNode` 汇聚。内部固定 **stereo（2ch）float32** 混音；单声道源在上游混音时复制到双声道。语义模型对齐 Web Audio（节点图 + AudioParam 自动化），但零三方依赖。

**恒编译与静默模式**：图 API 始终编译（对齐 RHI 先例：契约常在、能力经 `feature_flags().audio` 运行期查询）；`AURORA_ENABLE_AUDIO`（见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)）只决定是否编入内置设备后端。未启用或设备启动失败 → **静默模式**：图照常运转、样本消费后丢弃（`device_state()==Silent`、`silent()==true`），对齐 GPU 通道回退语义——消费代码无需分支。

**节点清单**（`AudioContext` 工厂创建，节点归属唯一上下文）：

| 节点 | 工厂 | 说明 |
|:---|:---|:---|
| `AudioDestinationNode` | `create_destination()` / `destination()` | 图的终端；混音输出经主音量缩放后送设备 |
| `AudioStreamSourceNode` | `create_stream_source(ring_capacity_frames=16384)` | 实时推流源：`push(span<const int16_t>, rate, channels)` int16 PCM 入环；线性插值 SRC（拉，相位累加器，`step = src_rate/dev_rate`；推入率中途变更报错） |
| `GainNode` | `create_gain()` | 纯增益；`gain()` 返回 `AudioParam&` |
| `AudioBufferSourceNode` | `create_buffer_source()` | 内存缓冲播放：`set_buffer(shared_ptr<const AudioBuffer>)` / `start(when=0)` / `stop()` / `set_loop(bool)` / `finished()`；一次性播完自动 `finished()` |
| `PannerNode` | `create_panner()` | equal-power 立体声声像（对齐 Web Audio `panningModel='equalpower'`）：由声源相对 `ctx.listener()`（`AudioListener`，默认原点/朝 -Z/上向 +Y）的方位角计算 `pan = sin(az)`，`L=cos((pan+1)·π/4)`、`R=sin((pan+1)·π/4)`（能量守恒 L²+R²=1）；距离衰减 inverse 模型 `gain = ref/(ref + rolloff·(max(d,ref)-ref))`（`set_ref_distance`/`set_rolloff`）；位置/衰减块级更新；HRTF/锥形属后续增量 |
| `AnalyserNode` | `create_analyser()` | 直通分析节点：Blackman 窗 + radix-2 复 FFT（自实现零三方）；`set_fft_size`（2 的幂 32..32768，非法 → `AudioParamInvalid`）/`frequency_bin_count()`（= fft_size/2）/`set_smoothing_time_constant`（dB 域平滑 τ∈[0,1]，默认 0.8）/`set_min/max_decibels`（默认 [-100,-30]）；快照访问器 `get_float_frequency_data`（钳位 dB）/`get_byte_frequency_data`（dB 归一 0..255）/`get_byte_time_data`（-1..1 → 0..255，静音=128）；mono 下混 (L+R)/2，渲染线程逐块分析，UI 线程 mutex 读快照；幅度归一 mag/(N/4)（全幅正弦峰值桶 ≈ -1.5 dB） |
| `AudioMicrophoneSourceNode` | `create_microphone_source()` | 麦克风源（继承推流源，复用推流环；采集线程为唯一生产者）：**录制是显式能力**——采集设备不可用/权限拒绝返回 `AudioDeviceUnavailable` 显式错误，**不静默降级**；析构先停采集再释放环 |
| `AudioRecordingDestinationNode` | `create_recording_destination()` | 直通录制汇：`start()`（幂等；closed → `AudioContextClosed`）起录/`stop()` 停录（幂等，保留数据）；样本常驻内存（设备采样率 stereo float32），`recording()`/`recorded_frames()` 快照；`to_wav_bytes()` 导出 16-bit PCM stereo RIFF（44 字节头）；`save_wav(path)` 落盘失败 → `AudioRecordingFailed` 显式报错，不静默 |

**推流重采样质量档位**：`SrcQuality { Linear, Sinc }`，`set_src_quality()/src_quality()`，默认 Linear。Sinc 为 32-tap Blackman 窗 sinc 核 + 相位量化表（256 子相位 × 32 tap，行归一 DC 增益 1，构造期生成）：需 **16 帧前瞻**（前瞻不足停相位，窗尾帧须续推后才输出），读端保留 15 帧窗历史；流起点无历史按复制首帧处理（行归一保证 DC 保真）。

**拓扑管理**：`connect(src, dst)` / `disconnect(src, dst)` / `connection_count()`。UI 侧 DFS 禁环校验（成环 → `AudioGraphCycle`；边不存在 → `AudioEdgeNotFound`；destination 为终端不可作源）。节点由 `shared_ptr` 持有，上下文析构即拆图。

**AudioParam 与 AutomationTimeline**：`AudioParam` 双轨取值——即时 `set_value()/value()` + 事件链 `set_value_at_time` / `linear_ramp_to_value_at_time` / `exponential_ramp_to_value_at_time` / `set_target_at_time` / `cancel_scheduled_values`。事件时刻非负且不回退（违规 → `AudioParamInvalid`）；ramp 锚点 = 上一事件时刻 + 落点值（纯数学插值，不依赖渲染时钟）；指数 ramp 起终点须非零同号。事件链以 COW 快照存储（`atomic<shared_ptr>`），渲染线程无锁读。

**线程模型**：UI 线程提交图变更（connect/disconnect/参数/推流），渲染线程（设备回调或手动驱动）执行 `render_block`；图变更经 **SPSC 命令环**（256 槽）在块首排空生效。推流环单调 uint64 帧游标：**溢出丢最旧**（生产端 CAS 单调推进读端 + 键级一次告警 `AURORA_LOG_WARN`）、**欠载输出静音停在环头**（不跳相位，渲染端检测落后即重同步）。无设备（静默模式）时命令直接生效（direct 模式）。

**设备层**：`AudioDeviceBackend` 接口（`format()` / `start(RenderFn)` / `stop()`），真实后端自起设备线程，测试用 `FakeAudioDevice`（`tests/support/fake_audio.h`）手动驱动保证确定性。`render_block(out, frames)` 公开可手动泵图（无头/测试）。`suspend()` 冻结时钟输出静音；`close()` 终态不可逆。

**WASAPI 后端**（Windows，`AURORA_ENABLE_AUDIO_WASAPI`，pimpl 隔离于 `src/aurora/media/audio_wasapi.*`，windows.h 不外泄）：shared mode event-driven——引擎事件驱动设备线程逐块 `GetCurrentPadding → render → GetBuffer/ReleaseBuffer`；格式协商优先以图契约格式（48000/2 float32）+ `AUTOCONVERTPCM` 初始化（引擎侧转换吸收设备差异，重路由后契约不变），旧系统回退 float32 stereo 混合格式直用；协商失败 → `start()` 返回 false → 静默降级。**重路由**：`IMMNotificationClient` 监听默认设备变更/设备状态变化，设备线程重建端点客户端（失败退避 200ms 重试），期间时钟冻结（对齐 suspend 语义）。真机探针 `aurora_verify_wasapi_audio`（`tools/verify/`，自动段 + `--interactive` 出声段，不进 CTest）。

**应用接线**（`media/audio_sink_bridge.h` + `VideoPlayer`）：`AudioSinkGraphBridge` 实现既有 `AudioSink` 契约——`play_samples` 推入自持 `AudioStreamSourceNode`，音量/静音经图内 `GainNode` 单点施加；析构断边，未连图/上下文关闭时推入即丢弃（兜底路径不报错）。`VideoPlayer::set_audio_context(ctx)`（典型取 `app.audio_shared()`，见 `Application::audio()`，specification/06 §2.2）自动接管 `set_audio_callback` 的 PCM 通道：桥以 shared_ptr 进源回调（解码器线程推流无悬垂），换源自动重绑，`set_audio_context(nullptr)` 断边并清空回调；接线后 `set_volume`/`set_muted` 路由到图内 GainNode（不转发源，避免双重衰减），未接线保持既有转发语义。

**错误码**（slug 见 `ERROR_CATALOG.md`）：`AudioGraphCycle` / `AudioEdgeNotFound` / `AudioContextClosed` / `AudioParamInvalid` / `AudioBufferInvalid` / `AudioDeviceUnavailable`（设备不可用为 warning 级——静默降级而非失败）。

---

## 10 需求规格

### 10.1 #11 确定性渲染 + 逻辑快照测试

**核心目标：** AI 可验证正确性。

**需求陈述：** 相同输入 → 相同输出（跨平台）。提供 `au::render_to_png(root, width, height, path)` 离屏渲染为 PNG（§8.4）。

**分层验证策略：**

```text
Level 1  结构快照（JSON 树）—— AI 可完全验证
         {"type":"Column","children":[{"type":"Text","props":{"content":"Hi"}}]}

Level 2  布局盒模型快照 —— AI 可验证布局逻辑
         {"type":"Text","box":{"x":20,"y":10,"w":100,"h":24}}

Level 3  像素快照（PNG）—— 人类视觉回归测试用
```

**快照测试：**

```cpp
au::Node btn_node{ au::Button(au::ButtonProps{ .label = "Test" }) };
btn_node->width(au::px(100)).height(au::px(40));
const au::Json snap = au::render_to_logical_snapshot(btn_node, 100, 40);
AURORA_TEST_CHECK(std::string{ snap["type"].get<std::string>() } == "Button");
AURORA_TEST_CHECK(std::abs(snap["box"]["w"].get<float>() - 100.0F) < 0.001f);
```

**关键约束：** 快照格式是**平台无关的逻辑描述**（JSON 树 + 盒模型），不是像素位图。AI 的调试闭环只需要 Level 1 + Level 2，完全无头运行。

**验收标准：** 同一棵树在 Headless 与真实后端下产出相同的 Level 1 / Level 2 快照；`render_to_logical_snapshot` 返回的盒模型与布局协议（§2–§4）心算结果一致。

**系统化 golden 套件：** `utest_offscreen` 以 `render_to_logical_snapshot` 为基础建立跨布局的 Level 1+2 黄金文件比对：11 个固定尺寸场景（Column/Row/Stack/Grid/Scroll/嵌套容器、gap、padding、横/纵向 fill 分配）逐场景与 `tests/golden/logical_snapshots.json` 基准深度比对，盒模型逐字段漂移即红灯。场景全部使用 `px()` / `fill()` 等显式尺寸意图、不依赖字体度量，保证跨平台逐值一致。基准有意更新时设 `AURORA_UPDATE_GOLDEN=1` 重跑用例重写基准（见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) golden 环境变量）。

### 10.2 #20 布局系统的代数一致性

**核心目标：** AI 可推理尺寸和位置。

**规则：**

```cpp
// 规则 1：盒模型完全显式，无隐式行为
auto hi = au::Text(au::TextProps{ .content = "Hi" });
hi.modifier = au::Modifier{}
    .padding(8)                   // 内边距，永远加在内容尺寸之外
    .border(1, au::colors::AURORA_GRAY)  // 边框（命名空间级色常量为 AURORA_ + 全大写，见 core/color.h）
    .width(200);                  // 内容宽度
// 最终占用 = padding + border + width（无例外）
// Modifier::width 取 float；Length 强类型宽度走 Widget::width(Length)

// 规则 2：百分比的参照物永远明确
child.width(au::percent(0.5));
// 参照物 = 父容器的 content width（不含 padding）

// 规则 3：布局方程可求解、可验证
// 父容器宽度 = Σ(子宽度) + Σ(间距) + padding_left + padding_right
// 方程无解（子总宽 > 父宽）时有明确的溢出策略：Scroll 包裹，或依赖约束 clamp
au::Scroll(au::ScrollProps{ .child = au::Row(au::RowProps{ .children = { /* ... */ } }) });

// 规则 4：布局结果可查询
auto snap = au::render_to_logical_snapshot(root, 800, 600);
```

**关键约束：** 布局模型基于线性等式，具有明确可计算的盒模型；AI 能通过简单规则推导；**无隐藏的边距合并**；无隐式最小尺寸。

**动态与响应式布局规则：**

- **规则 5（resize）**：布局是纯函数 `layout(tree, viewport_size) → boxes`。窗口大小变化重新求解，结果确定。无动画插值：布局跳变是即时的，动画仅作用于视觉属性（opacity、transform）。
- **规则 6（动态内容）**：文本组件的 `height` 默认为 auto。布局分两遍——Pass 1 自上而下确定宽度，Pass 2 自下而上确定高度（文本换行后确定实际高度）。两遍布局保证确定性。
- **规则 7（动画期布局不变）**：动画仅影响渲染层的 transform / opacity，不改变布局盒模型；布局快照在动画前后完全一致。

**验收标准：** 给定父约束与子项列表，按 §6.1 心算流程得到的几何与 `render_to_logical_snapshot` 输出的盒模型逐字段一致；动画期间布局快照不变。
