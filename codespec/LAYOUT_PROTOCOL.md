# 布局协议形式化规范（Layout Protocol Specification）

> **以 [`FlexLayouter::layout`](../src/aurora/layout/flex_layouter.cpp)（`src/aurora/layout/flex_layouter.cpp`）与 [`Grid::on_layout`](../include/aurora/widget/grid.h)（`include/aurora/widget/grid.h`）实现为准。**
>
> 本文档目标：使 LLM 在 500 token 内能"心算"任意 Flex/Grid 布局的最终几何（尺寸 + 位置）。

---

## §1 基础类型

### 1.1 Constraints（约束）

```cpp
struct Constraints {
    Size min;                    // 最小尺寸（默认 0）
    Size max = Size::infinity(); // 最大尺寸（默认 +∞）
};
```

- `Size::infinity()` 表示 `float(+∞)`，即"不限制"。
- `constrain(const Size &s)` → `clamp(s, min, max)`：逐轴将尺寸夹入 `[min, max]`。
- **约束传递不变量**：父约束 `min ≤ max`（逐轴），子节点返回的尺寸必须 `∈ [min, max]`。

### 1.2 Length 四态

| `LengthKind`  | 语义       | 约束求解                                          |
|---------------|------------|---------------------------------------------------|
| `WrapContent` | 内容自适应 | 传给子项的 `max` = 父剩余空间，子项按内容返回尺寸 |
| `Expand`      | 撑满父级   | `min = max = parentSize`（强制填满）              |
| `Fixed(px)`   | 固定像素   | `min = max = px`                                  |
| `Fraction(f)` | 父级比例   | `min = max = parent × f`（`f ∈ [0,1]`）           |

### 1.3 其他基础类型

- `Point{x, y}`：逻辑像素坐标。
- `Size{width, height}`：逻辑像素尺寸，支持 `infinity()`。
- `Rect{origin: Point, size: Size}`：轴对齐矩形；`right() = x + w`，`bottom() = y + h`。
- `EdgeInsets{left, top, right, bottom}`：四边内边距；`horizontal() = left + right`，`vertical() = top + bottom`。（padding / margin 由 Modifier 层在测量前后施加，不在本协议约束传递求解范围内；容器主轴尺寸 = Σ子尺寸 + gap，无隐式边距合并。）

---

## §2 两阶段布局协议

所有容器布局遵循 **measure → place** 两阶段：

> **范围**：本协议覆盖 Flex 与 Grid 两类布局的确定性几何求解；Stack / Positioned 等绝对 / 叠放布局不在本协议心算范围内。

### 阶段一：Measure（测量）

对每个子节点施加约束 `cc`，子节点返回 `Size ∈ cc`。

- 父约束 → 子约束的 clamp 逻辑（ **逐轴**，以下以主轴为例；交叉轴同理，但 `cc.min.cross` 取父 `min_cross` 而非强制 0）：
  ```
  cc.min.main = 0                   // 主轴 min 归零（内容自适应起点）
  cc.min.cross = parent.min.cross
  cc.max.main = parent.max.main - used   // 父剩余主轴空间
  cc.max.cross = parent.max.cross
  ```
- 子节点返回的尺寸必须满足 `cc.min ≤ size ≤ cc.max`（逐轴）。

### 阶段二：Place（定位）

根据测量结果和对齐参数，计算每个子节点的 `Rect{origin, size}` 并写入 `bounds`。

- 容器自身尺寸 = `constrain(内容总尺寸)`。
- 子节点位置 = 前导间距 + 累计偏移。

---

## §3 Flex 分配算法（FlexLayouter）

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

| 符号          | 含义                                        |
|---------------|---------------------------------------------|
| `P_main`      | 父约束主轴最大值（`parent.max` 沿主轴分量） |
| `P_min_main`  | 父约束主轴最小值                            |
| `P_cross`     | 父约束交叉轴最大值                          |
| `P_min_cross` | 父约束交叉轴最小值                          |
| `n`           | 子项数量                                    |
| `flex_i`      | 第 i 子项的 flex 权重（`≥ 0`）              |
| `gap`         | 相邻子项间距                                |
| `total_gap`   | `(n > 1) ? (n-1) × gap : 0`                 |
| `Σ_flex`      | 所有 `flex_i > 0` 子项的权重之和            |

### 3.3 阶段一 (A)：测量非 flex 子项

对每个 `flex_i == 0`（或主轴无限时全部）的子项：

```
cc.main.min = 0
cc.main.max = max(0, P_main - used_main)   // 父剩余主轴空间
cc.cross.min = P_min_cross
cc.cross.max = P_cross
size_i = measure(cc)
used_main += size_i.main
max_cross = max(max_cross, size_i.cross)
```

### 3.4 阶段一 (B)：flex 子项分配（仅当 `P_main < ∞` 且 `Σ_flex > 0`）

```
free = max(0, P_main - used_main - total_gap)
alloc_i = free × flex_i / Σ_flex

cc.main.min = 0
cc.main.max = max(0, alloc_i)
cc.cross.min = P_min_cross
cc.cross.max = P_cross
size_i = measure(cc)
used_main += size_i.main
max_cross = max(max_cross, size_i.cross)
```

> **关键**：flex 子项的 `alloc_i` 是其 `max` 约束，子项测量函数可返回 `≤ alloc_i` 的值。
> 若子项内容 > `alloc_i`，则被 clamp 到 `alloc_i`。

### 3.5 容器尺寸

```
used_main += total_gap   // gap 计入总占用

// 主轴容器尺寸
if main_axis_size == Max && P_main < ∞:
    container_main = P_main
else:
    container_main = clamp(used_main, P_min_main, P_main)

free_space = max(0, container_main - used_main)

// 交叉轴容器尺寸
container_cross = clamp(max_cross, P_min_cross, P_cross)
```

### 3.6 主轴对齐（计算 leading 与 between）

| `MainAxisAlignment` | `leading`              | `between`                         |
|---------------------|------------------------|-----------------------------------|
| `Start`             | `0`                    | `0`                               |
| `Center`            | `free_space / 2`       | `0`                               |
| `End`               | `free_space`           | `0`                               |
| `SpaceBetween`      | `0`                    | `free_space / (n - 1)`（`n > 1`） |
| `SpaceAround`       | `free_space / n / 2`   | `free_space / n`                  |
| `SpaceEvenly`       | `free_space / (n + 1)` | `free_space / (n + 1)`            |

### 3.7 阶段二：定位

```
pos = leading
for i in 0..n:
    if i > 0: pos += gap
    cross_pos = 按 cross_axis 计算（见 §3.8）
    origin.main = pos
    origin.cross = cross_pos
    pos += size_i.main + between
```

**反向布局**（`RowReverse` / `ColumnReverse`）：最终对每个子项沿主轴镜像：

```
origin.main = container_main - (origin.main + size.main)
```

> 反向经 `FlexDirection::RowReverse` / `FlexDirection::ColumnReverse`（即 `FlexDirection` 的四值）表达；注意 `VerticalDirection` 枚举当前 **未被布局引擎消费**，反向请用 `FlexDirection` 的 Reverse 取值，而非 `VerticalDirection`。

### 3.8 交叉轴对齐

| `CrossAxisAlignment` | `cross_pos`                            | `cross_size`                      |
|----------------------|----------------------------------------|-----------------------------------|
| `Start`              | `0`                                    | `size_i.cross`                    |
| `Center`             | `(container_cross - size_i.cross) / 2` | `size_i.cross`                    |
| `End`                | `container_cross - size_i.cross`       | `size_i.cross`                    |
| `Stretch`            | `0`                                    | `container_cross`（强制拉伸填满） |

> **Stretch 语义**：子项交叉轴尺寸被强制设为 `container_cross`，无论其内容尺寸。
> 若 `container_cross` 由 `P_min_cross` 撑大（如父 `min.width = 80`），子项也被拉伸到该值。

---

## §4 Grid 布局算法

### 4.1 参数

```cpp
struct GridProps {
    int columns;  // 列数（默认 1）
    float gap;    // 单元格间距（默认 4px）
};
```

### 4.2 算法

设 `cols = max(1, columns)`，`n = children.size()`，`rows = ceil(n / cols)`。

**列宽计算**：

```
if P_main < ∞ (宽度受限):
    cell_w = max(0, (P_max.width - gap × (cols - 1)) / cols)
    // 所有列等宽 = cell_w
else:
    // 列宽由内容决定（取该列最宽子项）
```

**测量**：对每个子项施加约束 `cc.max = (cell_w, +∞)` 或 `(+∞, +∞)`。

**行列尺寸**：

```
col_w[c] = max(sizes[i].width)  for i % cols == c
row_h[r] = max(sizes[i].height) for i / cols == r
```

**容器尺寸**：

```
total_w = Σ col_w[c] + gap × (cols - 1)
total_h = Σ row_h[r] + gap × (rows - 1)
return constrain(Size{total_w, total_h})
```

**定位**（行优先）：

```
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

## §5 布局缓存一致性不变量

- **缓存键**：`Constraints` 逐字段相等（`min.w, min.h, max.w, max.h`）。
- **不变量**：若约束未变（`Constraints::operator==` 为真），则布局结果不重算。
- **意义**：避免无效 re-layout，保证帧循环 O (脏节点) 复杂度。

---

## §6 溢出策略语义

| `OverflowStrategy` | 语义                                    |
|--------------------|-----------------------------------------|
| `Visible`          | 子内容溢出容器边界仍可见（默认）        |
| `Hidden`           | 溢出部分裁剪（不绘制），不参与 hit-test |
| `Clip`             | 同 `Hidden`，但保留 hit-test 事件穿透   |
| `Scroll`           | 预留，当前等同 `Hidden`                 |

> 溢出策略不影响布局尺寸计算——容器尺寸始终由约束决定，溢出策略仅影响渲染裁剪与事件处理。

---

## §7 心算示例

### 示例 1：Row / Start / 三固定子项

**条件**：

- 父约束：`max = Size(100, 100)`
- `Flex{Row, Start, Start}`
- 子项 A/B/C：内容尺寸均为 `20×10`，`flex = 0`

**计算**：

- `used_main = 20 + 20 + 20 = 60`，`total_gap = 0`
- `container_main = clamp(60, 0, 100) = 60`
- `free_space = 0`，`leading = 0`，`between = 0`
- 位置：A.x=0, B.x=20, C.x=40

**输出**：容器尺寸 `60×10`；A=`Rect{(0,0), 20×10}`，B=`Rect{(20,0), 20×10}`，C=`Rect{(40,0), 20×10}`

---

### 示例 2：Row / 两 flex=1 子项（零内容）

**条件**：

- 父约束：`max = Size(100, 100)`
- `Flex{Row, Start, Start}`
- 子项 A/B：`flex = 1`，内容尺寸 `0×10`

**计算**：

- 阶段一 (A)：无非 flex 子项，`used_main = 0`
- 阶段一 (B)：`free = max(0, 100 - 0 - 0) = 100`，`Σ_flex = 2`
    - `alloc_A = 100 × 1/2 = 50`，`alloc_B = 50`
    - 子项测量返回 `50×10`
- `used_main = 50 + 50 = 100`
- `container_main = 100`

**输出**：容器尺寸 `100×10`；A=`Rect{(0,0), 50×10}`，B=`Rect{(50,0), 50×10}`

---

### 示例 3：Row / 混合 flex 与非 flex

**条件**：

- 父约束：`max = Size(100, 100)`
- `Flex{Row, Start, Start}`
- A：`flex=0`，内容 `20×10`
- B：`flex=1`，内容 `0×10`
- C：`flex=1`，内容 `0×10`

**计算**：

- 阶段一 (A)：A 测量 → `20×10`，`used_main = 20`
- 阶段一 (B)：`free = max(0, 100 - 20 - 0) = 80`，`Σ_flex = 2`
    - `alloc_B = alloc_C = 80 × 1/2 = 40`
- `used_main = 20 + 40 + 40 = 100`

**输出**：容器尺寸 `100×10`；A.x=0, B.x=20, C.x=60

---

### 示例 4：Row / Center / 父约束 min=max=100

**条件**：

- 父约束：`min = Size(100, 0)`，`max = Size(100, 100)`
- `Flex{Row, Center, Start}`
- A/B：内容 `20×10`，`flex=0`

**计算**：

- `used_main = 40`
- `container_main = clamp(40, 100, 100) = 100`
- `free_space = 100 - 40 = 60`
- `leading = 60/2 = 30`

**输出**：容器尺寸 `100×10`；A.x=30, B.x=50

---

### 示例 5：Row / SpaceBetween / 三子项

**条件**：

- 父约束：`min = Size(100, 0)`，`max = Size(100, 100)`
- `Flex{Row, SpaceBetween, Start}`
- A/B/C：内容 `20×10`，`flex=0`

**计算**：

- `used_main = 60`，`container_main = 100`
- `free_space = 40`
- `between = 40 / (3-1) = 20`，`leading = 0`

**输出**：A.x=0, B.x=40, C.x=80

---

### 示例 6：Column / Stretch / 父约束强制交叉轴

**条件**：

- 父约束：`min = Size(80, 0)`，`max = Size(80, 200)`
- `Flex{Column, Start, Stretch}`
- A/B：内容 `20×30`，`flex=0`

**计算**：

- 主轴 = 垂直：`used_main = 30 + 30 = 60`
- `container_main = 60`（主轴 Min）
- `max_cross = 20`，`container_cross = clamp(20, 80, 80) = 80`
- Stretch：子项交叉轴尺寸强制 = `container_cross = 80`

**输出**：容器尺寸 `80×60`；A=`Rect{(0,0), 80×30}`，B=`Rect{(0,30), 80×30}`

---

### 示例 7：Row / gap + flex

**条件**：

- 父约束：`max = Size(100, 100)`
- `Flex{Row, Start, Start, gap=10}`
- A：`flex=0`，内容 `20×10`
- B：`flex=1`，内容 `0×10`
- C：`flex=1`，内容 `0×10`

**计算**：

- `total_gap = 2 × 10 = 20`
- 阶段一 (A)：A → `20×10`，`used_main = 20`
- 阶段一 (B)：`free = max(0, 100 - 20 - 20) = 60`，`Σ_flex = 2`
    - `alloc_B = alloc_C = 60/2 = 30`
- `used_main = 20 + 30 + 30 + 20(gap) = 100`

**输出**：容器尺寸 `100×10`；A.x=0, B.x=30, C.x=70

---

### 示例 8：Grid / 2 列 / 宽度受限

**条件**：

- 父约束：`max = Size(100, 200)`
- `Grid{columns=2, gap=4}`
- 4 个子项，内容尺寸均为 `30×20`

**计算**：

- `cols=2`，`rows=2`，`cell_w = (100 - 4×1) / 2 = 48`
- 各子项在 `cc.max = (48, +∞)` 下测量 → 返回 `30×20`
- `col_w = [30, 30]`，`row_h = [20, 20]`
- `total_w = 30 + 30 + 4 = 64`
- `total_h = 20 + 20 + 4 = 44`

**输出**：容器尺寸 `64×44`

- child[0]=`Rect{(0,0), 30×20}`，child[1]=`Rect{(34,0), 30×20}`
- child[2]=`Rect{(0,24), 30×20}`，child[3]=`Rect{(34,24), 30×20}`

---

## §8 快速心算流程（Checklist）

给定父约束 + Flex/Grid 参数 + 子项列表，按以下步骤心算：

1. **确定方向**：Row → 主轴=水平，Column → 主轴=垂直。
2. **测量非 flex 子项**：在剩余空间约束下获取内容尺寸。
3. **计算 flex 剩余**：`free = P_main - used - total_gap`。
4. **分配 flex**：`alloc_i = free × flex_i / Σ_flex`。
5. **容器主轴**：`Min → clamp(used, P_min, P_max)`；`Max → P_main`。
6. **自由空间**：`free_space = container_main - used`。
7. **主轴对齐**：查表得 `leading` / `between`。
8. **交叉轴**：查表得 `cross_pos` / `cross_size`（注意 Stretch）。
9. **定位**：`pos = leading`，逐子项累加 `size + gap + between`。
10. **反向**：若 Reverse，镜像主轴位置。
