# H.13 布局引擎 + H.13b 共享枚举

> 本文件是「三、特性详细规范」子文档，覆盖 **§H.13**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #H.13 布局引擎（Layout Engine）

核心目标：代数一致、可推理、确定性的尺寸与位置求解（与 §20 互补）。

- **`Constraints`**：父对子的尺寸约束 `{ min: Size, max: Size }`；子必须 `constrain()` 回落在约束内。
- **`Length`**：强类型尺寸。`Length::fixed(px)`（固定像素）、`Length::wrap()`（内容自适应）、`Length::expand()`（撑满）、
  `Length::ratio(f)`（父尺寸比例）。等价工厂：`au::px(v)` / `au::percent(f)` / `au::fill()` / `au::auto_length()`（来自
  `dimension.h`）。
- **`EdgeInsets`**：边距 / 内边距 `{left, top, right, bottom}`。
- **`Alignment`**：对齐
  `TopLeft / TopCenter / TopRight / CenterLeft / Center / CenterRight / BottomLeft / BottomCenter / BottomRight`。
- **`Flex` / `FlexLayouter`**：主轴 / 交叉轴方向与对齐。`FlexDirection{ Row, Column }`、
  `MainAxisAlignment{ Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly }`、
  `CrossAxisAlignment{ Start, Center, End, Stretch }`；`FlexLayouter::layout(...)` 两遍求解（先宽后高，见 §20）。

```cpp
auto row = au::Row(au::RowProps{
    .gap = 8,
    .children = {
        au::Text(au::TextProps{ .content = "A" }),
        std::move(au::Text(au::TextProps{ .content = "B" }).width(au::px(120)))   // 固定宽度
    }
});
row.modifier = au::Modifier{}.padding(8);
```

设计要点：布局是 `layout(tree, viewport) -> boxes` 的纯函数；父宽 = Σ子宽 + 间距（无隐式边距合并）；百分比参照父 **content**
宽度；溢出策略显式（Scroll 包裹 / clamp）；窗口 resize 仅重算布局、不改变语义（§20 规则 5–7）。

> **形式化协议**：约束传递规则、Flex/Grid 分配公式、Length 四态语义、心算示例等详见 [`LAYOUT_PROTOCOL.md`](../../LAYOUT_PROTOCOL.md)（以 `FlexLayouter::layout` 实现为准）。

#### #H.13b 共享枚举（Shared Enums）

控件丰富属性引入的跨控件共享枚举，统一定义于 `core/enums.h`，并经 `props_io.h` 提供 `*_to_json` / `json_to_*`
互转（供各控件 `serializeProps`/`deserializeProps` 使用）；枚举值亦登记于 `gen_api.cpp` 的 `known_enums()`， 供
`aurora_api.json` 与代码生成工具消费。

| 枚举                 | 取值                                                                                                                             | 用途                                                                                                      |
|----------------------|----------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------|
| `TextAlign`          | `Left` `Right` `Center` `Start` `End` `Justify`                                                                                  | `Text`/`RichText` 文本对齐（`Justify` 已实现：仅对多行文本的非末行按词均分铺满整行宽度；单行等同 `Left`） |
| `TextOverflow`       | `Clip` `Ellipsis` `Fade`                                                                                                         | 超出 `max_lines` 时的处理（`Fade` Painter 不支持，降级为 `Clip`）                                         |
| `FontWeight`         | `Thin(100)` `ExtraLight(200)` `Light(300)` `Normal(400)` `Medium(500)` `SemiBold(600)` `Bold(700)` `ExtraBold(800)` `Black(900)` | 字重（数值即 Flutter 同名词重）                                                                           |
| `FontStyle`          | `Normal` `Italic`                                                                                                                | 字形风格（`Italic` 已实现：经 GDI `LOGFONT.lfItalic`，Windows/GDI 下生效；非 GDI 回退路径不倾斜）         |
| `TextDecoration`     | `None` `Underline` `Overline` `LineThrough`（可按位 `\|` 组合）                                                                  | 文本装饰线                                                                                                |
| `MainAxisSize`       | `Min` `Max`                                                                                                                      | `Column`/`Row` 主轴尺寸策略                                                                               |
| `MainAxisAlignment`  | `Start` `Center` `End` `SpaceBetween` `SpaceAround` `SpaceEvenly`                                                                | `Column`/`Row` 主轴对齐                                                                                   |
| `CrossAxisAlignment` | `Start` `Center` `End` `Stretch`                                                                                                 | `Column`/`Row` 交叉轴对齐                                                                                 |
| `VerticalDirection`  | `Up` `Down`                                                                                                                      | 主轴方向（反向布局）                                                                                      |
| `StackFit`           | `Loose` `Expand` `Passthrough`                                                                                                   | `Stack` 子项尺寸拟合                                                                                      |
| `BoxFit`             | `Fill` `Contain` `Cover` `FitWidth` `FitHeight` `None` `ScaleDown`                                                               | `ImageView` 图片缩放拟合                                                                                  |

**`Column`/`Row` 对齐与主轴尺寸**：`Column`/`Row` 通过挂载的 `Flex` 承载 `main_axis`（`MainAxisAlignment`）、`cross_axis`（
`CrossAxisAlignment`）与 `main_axis_size`（`MainAxisSize`），并提供链式 setter：

```cpp
au::Column{}
    .set_main_axis_alignment(au::MainAxisAlignment::Center)   // 主轴居中
    .set_cross_axis_alignment(au::CrossAxisAlignment::Stretch) // 交叉轴拉伸填满
    .set_main_axis_size(au::MainAxisSize::Max)                 // 撑满父级主轴 → 对齐可见
    .set_gap(8.0f);

au::Row{}
    .set_main_axis_alignment(au::MainAxisAlignment::SpaceBetween)
    .set_cross_axis_alignment(au::CrossAxisAlignment::Center);
```

- 三者均为 **固有属性**，随 `Column`/`Row` 序列化（键：`main_axis_alignment`/`cross_axis_alignment`/`main_axis_size`/`gap`
  ），可经 `to_json`/`from_json`/`diff`/`apply_patch` 往返。
- `main_axis_size` 默认 `Min`（内容尺寸，对齐仅父约束强制更大时可见）；设 `Max` 可使容器撑满父级可用主轴空间，让 `main_axis`
  对齐在内容不足时产生可见自由空间。无限主轴约束下 `Max` 退化为内容尺寸。
- `cross_axis = Stretch` 时，子项在交叉轴方向被拉伸至容器交叉轴尺寸（受自身 `width`/`height` 等约束夹取）。

**二层属性划分（已采纳）**：控件可配置性由「固有属性（`XxxProps` 字段）」与「正交修饰（`Modifier` 链）」
两层构成——固有属性描述控件自身身份（随控件序列化、可被 Inspector 枚举），`Modifier` 承载跨切面、可叠加、 可 `Reactive`
变化的通用装饰（padding/background/border/align/opacity/rotate/scale/transform/clickable/...）。 两者重叠能力（如 `padding`/
`corner_radius`/`background_color`）以控件固有属性优先；`Modifier` 同类项保留用于 「给任意控件套一层」的跨切面场景，绘制时
`Modifier` 在外、固有属性在内（见 `CODING_STANDARDS.md` 一.10）。
