# 布局与渲染（layout / render / image / media）

> 覆盖 `include/aurora/layout/`、`render/`、`image/`、`media/`（`render/` 顶层 15 个头，另有 `render/detail/` 下 3 个头——`gamma_lut.h`、`paint_timing.h`、`painter_simd.h`——合计 18）。
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
| 显示列表 | `render/display_list.h` |
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

**`Stretch` 语义**：子项交叉轴尺寸被强制设为 `container_cross`，无论其内容尺寸。若 `container_cross` 由 `P_min_cross` 撑大（如父 `min.width = 80`），子项也被拉伸到该值；子项同时受自身 `width` / `height` 等显式约束夹取。

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

`record(DisplayList&)` 开始录制，`stop()` 结束，`is_recording()` 查询状态，`mark_recording_dynamic()` 标记动态内容。`set_skip_dl_record(bool)` / `skip_dl_record()` 控制是否跳过录制。

> **录制态不变量**：`Painter::composite(const Image&, ...)` 在录制态**必须**录制 Composite 命令而非就地写像素。否则缓存的显示列表回放进祖先录制时，祖先列表缺失该合成，回放时子树像素整体缺失。
>
> **自驱动动画与缓存**：在 `on_paint` 内自行 `mark_needs_paint()` 推进动画的控件，必须覆写 `can_cache_display_list()` 返回 `false`，否则开启显示列表缓存后动画会被冻结。

**不变量**：`Painter::fill_rect` 必须乘 `m_global_alpha`；`set_alpha(double)` 须在 `on_paint` 结束前复位为 `1.0`。

### 8.2 FontEngine

`FontEngine`（`render/font_engine.h`）是文本引擎单例，以 **FreeType** 做栅格化、**HarfBuzz** 做 shaping（经仓库 `third_party/` 源码编入静态库，跨平台一致、确定性）。`draw_text_impl` / `measure_width` / `caret_x` 统一按文本 run 调 `hb_shape` 经 `hb_ft_font` 桥接 FreeType，度量、光标、命中三者逐位一致。

**字体注入**：`set_default_font(ttf_path)` / `register_font(family, ttf_path)` / `register_font_from_memory(family, ttf_bytes)`（`family` 为空表示默认 sans-serif）。

**度量与绘制**：`measure_width` / `measure_height` / `draw_text`。

**选中原语**：`caret_x(text, idx, font)` / `hit_test_char(text, x, font)` / `hit_test_char_inclusive`（以码点为索引，UTF-8 安全）。

**默认字体**是内置 Noto Sans（OFL），引擎首次使用时自动注册为 `""` / `"sans-serif"` / `"Noto Sans"` / `"default"`，含 Headless，保证跨机文本渲染逐位确定。缺字按候选 `FT_Face` 链回退（含系统 CJK 字体）避免豆腐块；无任何可用字体文件时回退内置 `BitmapFont` 保底。

**抗锯齿**：`enum class TextAAMode { Supersample, ClearType }` 加进程级 `set_text_aa_mode(mode)` / `text_aa_mode()`，默认 `Supersample`。`Supersample` 用 `FT_RENDER_MODE_NORMAL` 输出 A8 灰度覆盖度，与背景无关；`ClearType` 用 `FT_RENDER_MODE_LCD` 输出 3× 水平 RGB 子像素覆盖度，经 `Painter::blend_subpixel` 逐通道合成，**仅当文本不透明时启用**，半透明或字体不可用时自动回退 `Supersample`。

**排版选项（`TextLayoutOpts`）**：`measure_width` / `caret_x` / `hit_test_char` / `draw_text` 均提供接受 `TextLayoutOpts` 的重载，携带 `letter_spacing`（相邻字形间间距，整串共 `n-1` 次）、`word_spacing`（词间距，仅空格后追加）、`italic`（经 FreeType `FT_Set_Transform` 仿斜）。统一 opts 保证度量、光标、命中、绘制四者完全一致。

**锚定契约**：`draw_text(r, ...)` 的 `r.origin.y` 是**行盒顶**而非基线。实现内部首行基线 = `origin.y + 主 face ascender`，回退 face 字形统一按主 face 基线对齐。全库调用方均按顶锚定传值，**不得自行加减 ascent**。

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
| `set_title(const std::string&)` | 虚方法，默认空实现；`Win32Window` 经 `SetWindowTextA` + `utf8_to_acp` 生效，`Headless` / `Glfw` 忽略。`Window::set_title` 写 `m_title` 后同步下发 |

### 8.4 离屏渲染与快照

定义于 `render/offscreen.h`，与 `HeadlessSurface` 解耦。

```cpp
[[nodiscard]] auto render_to_png(Node &root, int width, int height, const char *path) -> Result<bool>;
[[nodiscard]] auto render_to_logical_snapshot(Node &root, int width, int height) -> Json;
```

- `render_to_png`：`root` 为 `Node&`，内部自行 `mount`，调用方无需预挂载；`width` / `height` 为画布逻辑尺寸。
- `render_to_logical_snapshot`：返回平台无关的**逻辑快照** JSON（结构树 + 盒模型），供 AI 在无头环境校验。

`Scene::render_to_png(path, width, height)`（`app/scene.h:29`）与 `Application::render_to_png(path)`（`app/application.h:224`）是无头便捷封装。

**其余渲染支撑头**（`render/`，公开）：`dirty_region.h` 提供 `DirtyRegionTracker`——收集脏矩形并把重叠项合并为并集，条数超上限（默认 `16`，可经 `set_max_rects` 调整）即退化为整帧脏（`mark_all` / `is_full`），以 `rects()` / `merged_bounds()` 出结果，衔接 §8.3 `set_present_dirty` 的增量上屏；`snapshot_diff.h` 提供 `compare_snapshots(baseline, current, tolerance = 0) -> SnapshotDiff`——逐像素比对两张 RGBA8 快照，产出差异像素数、最大通道差、差异占比与差异可视化图，`SnapshotDiff::passed(max_ratio)` 按阈值判定通过，供 golden 回归与 `aurora-cli snapshot --compare` 使用；`image_cache.h` 提供 `ImageCache`——进程级单例（`instance()`）的按路径 LRU 解码缓存（`get` / `put` / `remove` / `clear`，字节上限 `set_max_bytes`，解码失败不缓存），`count()` / `hit_count()` 供诊断与性能覆盖层读取。

### 8.5 后端与工厂

`SurfaceKind{Headless, Win32, Glfw, X11, Wayland, MacOS, Wasm, D3D11}` 现仅为**类型标签**（只用于 `auto_detect_surface()` 返回类型与 `Platform::surface` 字段），不再用于构造选择。

后端选择收口于类型安全工厂 `create_window(const XxxOptions&)`（`window/window.h`），每个后端有专属选项结构：`HeadlessOptions{png_path}` / `Win32Options{}` / `D3D11Options{vsync}` / `GlfwOptions{gl_major, gl_minor, resizable}` / `X11Options{}` / `WaylandOptions{}` / `MacOSOptions{}` / `WasmOptions{canvas_id}`，外加通用 `WindowOptions{size, title, max_frames}`。编译器会拒绝把某后端专属字段误用到不相关后端。

| 后端 | 说明 | 开关 |
|:---|:---|:---|
| `HeadlessSurface` | 内存帧缓冲 / 离线 PNG，软件 `Painter` | `AURORA_BACKEND_HEADLESS`（默认 ON） |
| `Win32Surface` | Win32/GDI，零三方依赖，仅 `_WIN32` | `AURORA_BACKEND_WIN32`（Windows 默认 ON） |
| `D3D11Surface` | D3D11 GPU 增量上屏，Win32 专属 | `AURORA_BACKEND_D3D11`（默认 OFF） |
| `GlfwSurface` | GLFW/OpenGL 3.3 兼容剖面 | `AURORA_BACKEND_GLFW` |
| `X11Surface` | X11/Xlib 原生窗口，仅 Linux 桌面 | `AURORA_BACKEND_X11`（默认 OFF） |
| `WaylandSurface` | 原生 Wayland（`wl_shm` + `xdg-shell` + `xkbcommon`） | `AURORA_BACKEND_WAYLAND`（默认 OFF） |
| `MacOSSurface` | AppKit / CoreGraphics，仅 Apple | `AURORA_BACKEND_MACOS`（默认 OFF） |
| `WasmSurface` | Emscripten / Canvas 2D，浏览器 rAF 驱动 | `AURORA_BACKEND_WASM`（默认 OFF） |

全部开关的默认值与 `AURORA_BUILD_*` / `AURORA_ENABLE_*` 完整列表见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)。

**自动选择**：不显式指定时 `App::run()` 经 `auto_detect_surface()` 自动选用，优先级为原生 Wayland / X11 / MacOS / Wasm > Win32 > Glfw > Headless；X11 与 Wayland 同时编译时按运行期会话类型择优（`WAYLAND_DISPLAY` 存在选 Wayland，否则 X11）。`create_native_window()` 在 Linux 上按同序尝试并在真实显示不可用时回退 `Headless`。

**自定义后端入口**：扩展点收口于 `Surface` 子类与 `create_window` 工厂，而非 `Application` 构造重载。`Application` / `App` 只认两种形态——(a) `create_window(XxxOptions)` 产出的 `unique_ptr<Window>`；(b) 任意自定义后端经 `Application(Scene, unique_ptr<Surface>, WindowOptions)` 或 `App().surface(...)` 注入的 `unique_ptr<Surface>`。空 `Surface` / `Window` 仅告警降级。无头便捷构造 `Application(Scene, w, h)` 保持不变。

**DPI 感知**：`enable_dpi_awareness()`（`window/window.h`）在进程创建**任何窗口之前**启用高 DPI 感知。这是 **OS/进程级**设置，非 per-Window、非 per-Surface——Win32 经 `SetProcessDpiAwarenessContext`（Per-Monitor V2 → V1 → `SetProcessDPIAware`）一次性启用；macOS 与 Linux 无需 opt-in，为空实现。**关键不变量**：必须在 `init_console()`（`AllocConsole` 会创建控制台窗口）与 `create_window()` 之前调用，否则 Windows 上启用失败会退化为 DPI 未感知（scale = 1.0）。每窗口的 scale 查询仍是各 `Surface::scale_factor()` 的职责，与「启用」正交。

---

## 9 图像与媒体

### 9.1 图像

`image/image_codec.h` 提供图像编解码，能力由编译期开关 `AURORA_BUILD_IMAGE_JPEG` / `AURORA_BUILD_IMAGE_WEBP` / `AURORA_BUILD_IMAGE_PNG` 控制（见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)）。

`Painter::draw_image(const Image&, const Rect&)` 采用双线性采样，在 **premultiplied-alpha 空间插值**，避免半透明边缘暗边与光晕。

### 9.2 VideoPlayer

`VideoPlayer`（`media/video_player.h`）是视频播放控件，可子类化定制。

**播放控制**：`play()` / `pause()` / `toggle_play()` / `is_playing()` / `seek(microseconds)` / `seek_fraction(double)` / `position()` / `position_fraction()` / `duration()` / `set_volume(double)` / `set_muted(bool)` / `volume()` / `muted()`。

**数据源**：`set_source(shared_ptr<VideoSource>)` / `source()`；`set_fit(BoxFit)` / `fit()`。

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
TCHECK(std::string{ snap["type"].get<std::string>() } == "Button");
TCHECK(std::abs(snap["box"]["w"].get<float>() - 100.0F) < 0.001f);
```

**关键约束：** 快照格式是**平台无关的逻辑描述**（JSON 树 + 盒模型），不是像素位图。AI 的调试闭环只需要 Level 1 + Level 2，完全无头运行。

**验收标准：** 同一棵树在 Headless 与真实后端下产出相同的 Level 1 / Level 2 快照；`render_to_logical_snapshot` 返回的盒模型与布局协议（§2–§4）心算结果一致。

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
