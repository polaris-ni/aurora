# CHG-075 四容器滚动条接线与焦点环度量令牌消费

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-075 |
| 提出日期 | 2026-11-10 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.FEAT.WIDGET.SCROLL-INTERACTION.001 |
| 影响面 | 四个滚动容器（`Scroll` / `LazyList` / `GridView` / `LazyRow`）新增滚动条绘制与交互：前三者纵向、条在右侧，`LazyRow` 横向、条在底侧；新增库内共用几何与交互内核一份（新头 `include/aurora/widget/detail/` 下的 `scrollbar.h`，**计划/待建**）；两种占位模式由 `LayoutConventions::scrollbar_mode` 驱动（`Classic` 占位、`Overlay` 浮层，默认值不变仍为 `Classic`），粗细读 `ControlMetrics::scrollbar_thickness`——两枚令牌此前零消费点，本批首次接线；新增两枚色角色 `scrollbar_thumb` / `scrollbar_track`（`ColorRoles` 38 → 40，`sizeof` 变化属 ABI 破坏，消费方须随头文件净重编），`light()` / `dark()` 给基线取值、`with_high_contrast()` 按对比度登记表自动派生、五套风格档中 macOS / GNOME / KDE 三套覆写而 Windows / Material 两套继承基线；`Widget` 的焦点环绘制与脏区外扩同批改读 `ControlMetrics::focus_ring_width` / `focus_ring_inset`（默认 2 / 2 与现行常量逐位相同，默认档像素零变化；环圆角仍取常量，本批不新增令牌）；`Content` 布局面：`Classic` 模式为滚动条保留带宽，四个容器的内容可用长宽随之收缩（`Scroll` 的离屏缓冲宽、`LazyList` 的条目宽、`GridView` 的单元格宽、`LazyRow` 的子项可用高各有一处口径变化），`Overlay` 模式内容盒与现状逐位相同；`Guard` 面：`ControlMetrics::min_touch_target` 本批**仍不接线**（改绑会移动既有全部像素 golden，属未经请求的视觉变更），`LayoutConventions::focus_ring_style` 仍无消费点；受影响判据面：`tests/unit/utest_theme_tokens.cpp`（令牌消费现状的两段注释与断言）、`tests/unit/utest_theme_roles.cpp`（角色枚数与新增档）、`tests/unit/utest_theme_scale_tokens.cpp`（`scrollbar_thickness` 由「候选」改「已消费」的注释）、`tests/unit/utest_theme_presets.cpp`（逐档继承 / 覆写值与新增角色的逐对断言）、`tests/golden/e2e_scroll.png` 与任何含溢出滚动容器的像素基线（重拍）、`tests/golden/logical_snapshots.json` 的 `scroll_tall_content` 场景（子项盒宽随占位带收缩）；`aurora_api.json` 的 `theme_tokens` 块新增两枚角色，属性条目面零新增；令牌侧同步面还有 `include/aurora/theming/detail/contrast_audit.h` 的逐对归档行与显式声明枚数、`tools/gen/gen_contrast.cpp` 的二十列读数、`ColorRoles::operator==` 与登记表头注释里的枚数、`tools/check/umbrella_manifest.txt` 补 `aurora/widget/detail/*` 豁免一行；`tools/include/theme_literal_exemptions.h` 与 `theme_tokens.exemptions.files` 须零变化，`ScrollbarAxis` 属库内枚举、不进 `tools/include/known_enums.h` 与 `enums` 段；`CHANGELOG.md` 的 `Breaking` / `Added` / `Migration` 条目。 |

## 动机

1. **库能滚，但看不见「还能滚多少、现在在哪」。** 四个容器各自实现了偏移、夹取、收位滑动与无障碍滚动量，绘制路径却只做内容合成或子项直绘：`Scroll` 是「整块合成离屏缓冲 + 钉顶浮层」，`LazyList` / `GridView` / `LazyRow` 是「按视口坐标逐子项绘制」，四条路径都没有轨道、滑块或任何进度指示。用户只能靠滚轮盲试；审查者也拿不到「这个容器滚得动吗」的像素证据。

2. **两枚主题令牌因此长期悬空。** `ControlMetrics::scrollbar_thickness` 的字段注释写着「候选：库内尚未绘制滚动条，无既有实现可复现」，`LayoutConventions::scrollbar_mode` 的注释写着「滚动条占位模式」却没有任何消费者；规格里如实登记为「未接线」并给出理由。悬空令牌比没有令牌更糟：它让「换一套设计语言」的承诺在滚动面上落空，也让预设表里 macOS / GNOME 的 `Overlay`、KDE 的 `Classic` 三行取值成为无法验证的装饰。

3. **两种模式不是可选项，而是预设表已经登记的两个值。** macOS 与 GNOME 预设写 `Overlay`（浮层、悬停显现），KDE 预设写 `Classic`（占位、常驻），GNOME 另把 `scrollbar_thickness` 标到 8 而基线是 4。只实现一种，都会让另一个预设的声明变成假话；只做绘制不做交互，则 `Overlay` 模式下条一出现就挡住内容、还点不动。

4. **四个容器的几何模型互不相同，必须一套内核四种接线。** `Scroll` 的内容活在独立坐标系（`Node` 原点归零、绘制期整体平移），且有滑窗离屏缓冲；`LazyList` / `GridView` / `LazyRow` 直接以视口坐标绘制子项；前三者纵向、`LazyRow` 横向；`LazyRow` 的命中链只把自身交给派发器（虚拟化子项不在链里），另外三者要下降。任何「各写一份几何」的做法都会在滑块长度、最小滑块、轨道内缩、拖拽映射四处漂成四套口径。

5. **焦点环两枚令牌的欠账与滚动条同源，必须同批结清。** `focus_ring_width` / `focus_ring_inset` 的字段注释都写着「复现现值」，取值恰好等于 `Widget` 的环厚与环距常量；单测里明写本模块未改读的原因——环宽除绘制侧还决定脏区外扩，而 `dirty_bounds()` 没有 `BuildContext`，只接绘制侧会让「画的环」与「标脏的盒」取到不同宽度、留残影像素，故留待两处一并改读。本批新增的滚动条绘制同样要读主题（粗细 + 模式），两件事都落在 `Widget` 的绘制与标脏面，分开做就要把同一段上下文入口改两遍。`min_touch_target` 不在此列：它绑的是命中盒下限，改绑会让既有全部像素基线一起移动，属未经请求的视觉变更。

6. **为什么新增专用色角色，而不是复用描边色。** 滚动条滑块是「形状承载信息」的状态指示器（非文本档，3:1），而 `outline` 的语义是「输入框边界」；两者在语义、可动端与高对比派生上是两条独立的判据行。复用会让一次调色同时动两处观感，也让对比度门禁的「哪一端可动」判断失真。轨道则是纯装饰（不标识状态、不承担边界），按既有口径登记为「不适用」并写明理由——与分隔线、状态叠层同一张表的同一类行。

7. **两枚新角色落在一条已成型的判定链上，不是只补两行取值。** `check_theme_contrast` 的 C3 以 `ColorRoles::AURORA_TOKENS` 为期望集，要求每枚角色至少出现在登记表某行的一侧；C1 判登记表声明枚数、解析行数与产物读数三方对号，而 `AURORA_CONTRAST_AUDITS` 是显式声明 30 行的 `std::array`；高对比派生只读这张表，覆盖面是五套风格档 × 明暗 × 对比度两档共二十列。因此「Material 明暗基线两列越过 3:1」不足以推出「派生列逐字节不动」——预设会覆写 `surface`，C9 又是按列判「原生列全达标 ⇔ 派生列不动」；须逐列实测，未达标的列要么改基线值、要么按 `AURORA_CONTRAST_PRESET_FLOORS` 登记豁免并同步其枚数，十腿变异自证也要在扩表后重跑。

## 变更内容

### 1 共用内核（新头 `scrollbar.h`，库内件）

新增 `include/aurora/widget/detail/` 下的 `scrollbar.h`（**计划/待建**；按仓库惯例 `detail/` 头不进伞头，本批在 `tools/check/umbrella_manifest.txt` 的 `[exempt]` 段按目录前缀补一行并写明「库内滚动条几何与交互，四个容器的公共实现」——R8 是逐目录分类，不是目录名自动豁免）。内容为纯值 + 纯函数，零分配、零字符串、不含虚函数：

- `ScrollbarAxis`：`Vertical` / `Horizontal`。
- `ScrollbarMetrics`：`content_length` / `viewport_length` / `offset` / `thickness` / `inset` / `min_thumb`，单位一律逻辑 dp，不做 DPI 换算（折算留在调用方既有路径）。
- `ScrollbarGeometry`：轨道矩形、滑块矩形、`track_length`、`max_offset`、`overflowing`（内容是否真溢出）。
- `ScrollbarInteraction`：`hovered` / `dragging` / `drag_origin_offset` / `drag_origin_pointer` / `hold_until`（`std::chrono::steady_clock::time_point`）；三个动作 `on_pointer`（按下 / 移动 / 抬起与「命中滑块或轨道」的判定）、`advance(now)`（返回可见性是否翻转，供宿主决定是否标脏）、`on_scroll(now)`（滚轮或程序化滚动后按注入时间刷新 `hold_until`）。

几何口径（一处给出、四处共用）：

- `overflowing = content_length > viewport_length + 0.5`（半像素容差吸收浮点累加）。不溢出时不画条，也不保留空间。
- `track_length = viewport_length - 2 × inset`。
- `thumb_length = clamp(track_length × viewport_length / content_length, min(min_thumb, track_length), track_length)`，`min_thumb = 24`。下界须先与 `track_length` 取小：极小视口下 `track_length < 24` 时直接 `clamp(v, 24, track_length)` 会违反 `std::clamp` 的 `lo <= hi` 前提（未定义行为）；`track_length <= 0` 时整条不绘制。
- `thumb_pos = (track_length - thumb_length) × offset / max_offset`，`max_offset = content_length - viewport_length`。
- 拖拽映射：`Δoffset = Δpointer × max_offset / (track_length - thumb_length)`；`track_length - thumb_length` 小于等于浮点容差（滑块占满轨道的退化几何）时不响应拖拽。这与「内容不溢出就不画条」是两件事，判据不得合并。
- 内核不自读时钟：`on_pointer` / `on_scroll` / `advance` 一律接收调用方注入的 `now`，与 `scroll_viewport.h` 里 `DragAutoScroll` 的既有口径一致（宿主可冻结 / 快进，无头夹具可确定性步进）。
- 轨道翻页：按点击点落在滑块之前 / 之后决定方向，位移取一页 = `viewport_length`，夹到 `[0, max_offset]`；不做「点击即居中跳转」（那会让一次误点把内容甩出很远）。

常量集中在内核头：`AURORA_SCROLLBAR_INSET = 2.0F`、`AURORA_SCROLLBAR_MIN_THUMB = 24.0F`、`AURORA_SCROLLBAR_HOVER_GROWTH = 2.0F`、`AURORA_SCROLLBAR_OVERLAY_HOLD = 1.0s`。前两枚与 24 这一数的出处（GNOME / Windows / M3 三家的滑块最小可点尺寸共识）写进注释；它们是**几何常量而非主题令牌**——不随设计语言变化，故不进 `ControlMetrics`。

### 2 四容器接线

| 容器 | 轴向 | 内容长 | 视口长 | 偏移 | 占位口径（`Classic`） | 绘制位置 |
|:---|:---|:---|:---|:---|:---|:---|
| `Scroll` | 纵向 | `content_h_` | `viewport_h_` | `offset_y_` | 右侧预留 `thickness + 2 × inset`，内容列宽与离屏缓冲宽按收缩后的 `viewport_w_` | 缓冲合成与钉顶浮层之后 |
| `LazyList` | 纵向 | `count_ × item_extent_` | 视口高 | `offset_` | 同上；条目宽 = 收缩后的可用宽 | 子项直绘之后 |
| `GridView` | 纵向 | `row_count() × cell_extent_` | 视口高 | `offset_` | 同上；`cell_width_ = 收缩后的可用宽 / columns_` | 子项直绘之后 |
| `LazyRow` | 横向 | `full_content_` | 视口宽 | `offset_` | 底侧预留同一带宽；**不自增固有高**，带宽从既有固有高中扣除，子项可用高相应收缩 | 子项直绘之后 |

- **溢出判定与时序**：`LazyList` / `GridView` / `LazyRow` 的内容长在布局前即可算出，一次定夺；`Scroll` 的内容高要经子项测量才可得，故按「先按满宽排一次判溢出 → 不溢出就不画条也不收窄；溢出才按占位带宽再排一次并画条」的顺序处理，最坏两次测量且只在真溢出时发生。反过来先收窄会产生**假阳性条**：满宽放得下、收窄后换行变多反而溢出，条凭空出现并把内容无谓收窄；此时再按满宽重排也来不及，因为条已经按收窄布局定夺。四容器的条几何一律取本帧布局完成后的最终 `content_h_` / `full_content_`，与 `accessibility_scroll()` 申报的 `content` 同源，不得取测量中间值。
- **横向高度口径**：`LazyRow` 的 `Classic` 带宽从既有固有高（`item_extent + 上下 padding`）中扣除，不向父级多要高度。若改为「固有高增加」，则在父约束宽松时内容可用高与现状逐位相同、`Classic` 与 `Overlay` 的内容区不再相差一条带宽，且同一容器在不同父约束下出现两套几何；统一扣除带宽才与纵向三容器的内容列收缩对称，也让判据一在任意父约束下可达。
- **叠加模式**：`Overlay` 不预留带宽，内容布局与现状逐位相同；条浮在内容之上，绘制顺序在内容之后；两档的条都画在容器自身盒内，条带不参与内容坐标换算。
- **命中优先级**：指针落在滑块 / 轨道矩形内时，容器的命中链只交自身（浮层下也不把点击让给内容）。纵向三容器的滚动条矩形不参与内容坐标换算；`LazyRow` 的虚拟化子项本就不在链里，无需额外处理。
- **不变面**：滚轮、键盘、吸附、程序化 `scroll_to`、收位滑动、`restore_key` 恢复、无障碍滚动量与拖入自动滚边全部不动；滚动条只作为同一偏移的第二种输入与可观测输出。

### 3 交互

- **拖拽**：按下命中滑块即进入拖拽，记录按下点与本帧偏移；拖拽中按映射公式写偏移，经容器既有的偏移写入路径夹取、标脏并回写 `restore_key`。指针离开轨道或离开控件仍继续（派发器在按下时捕获命中链，移动 / 抬起沿用同一链，与滑块的拖拽同一机制）。抬起结束拖拽。
- **轨道翻页**：按下命中轨道、且不在滑块内时，按方向翻一页并立即生效；按住不重复翻页（避免「按住即连滚」的加速行为与既有滚轮逐事件语义分叉）。
- **悬停**：指针进入滑块时条加粗 `AURORA_SCROLLBAR_HOVER_GROWTH`，以轨道中轴对齐；拖拽期间维持加粗。悬停判定取自指针移动事件的本地坐标，不经全局光标查询。
- **`Overlay` 显隐**：指针悬停在本容器内即显；滚轮或程序化滚动后另保持 `AURORA_SCROLLBAR_OVERLAY_HOLD`，到期隐藏。显隐是**瞬时切换**，不做淡入淡出——动画会让同一帧的像素取决于进入时刻，基线无法逐像素判定；同理，显隐状态在一次帧内只由帧首注入的 `now` 定夺一次，绘制期不重新取时，否则同一帧两次重绘可能跨过到期点而给出不同像素。保持窗口由既有手势计时入口（`needs_gesture_tick_` + `tick_gestures`）驱动：`LazyRow` 当前没有该覆写与状态位，本批须补齐；四容器帧计时的去留按「滚动条保持窗口、收位滑动、拖入滚边三者都空闲」判，不得由滚动条单独摘除而掐掉在途滑动（该共用计时契约见 `codespec/specification/05-event-navigation.md` §5.2）。空闲节流行为不变。
- **事件消费**：命中条时按下 / 移动 / 抬起置 `is_handled`，不再冒泡；未命中条的事件原样落到基类处理，`LazyRow` 自身的横向拖拽与子项点击逻辑排在滚动条分支之后，语义不变。
- **无障碍**：滚动条**不新增**无障碍节点，也不新增 `IScrollProvider` 成员；可滚范围仍由既有 `accessibility_scroll` / `accessibility_scroll_to` 表达（滚动条是同一偏移的可视化，不是第二条控制面）。

### 4 主题消费：模式与粗细

- 模式：四个容器在布局期读 `inherit_theme(ctx).conventions.scrollbar_mode`；`Classic` 预留、`Overlay` 浮层。主题切换时整树重排重绘，占位随之切换，不需要额外的失效通道。
- 粗细：绘制期读 `inherit_theme(ctx).controls.scrollbar_thickness`。三套预设因此首次生效：GNOME 档的 8.0 变粗、macOS / GNOME 的 `Overlay` 变浮层、KDE 的 `Classic` 保持占位；其余两档（Windows / Material）取基线 4.0 与默认 `Classic`。
- 本批**不新增**公开开关：不提供「隐藏滚动条」属性，也不给 `ScrollbarMode` 加第三个枚举值。理由是本批没有提出该需求的消费方，而两种模式的语义已经覆盖「常驻」与「悬停显现」；若后续出现「整屏截图 / 壳层 UI 不要条」的场景，再加枚举值属单纯扩枚举，不必现在预支。

### 5 新增色角色

`ColorRoles` 追加两枚（声明序追加在 `pressed` 之后、`AURORA_TOKENS` 同步）：

- `scrollbar_thumb`：滑块。`light()` 取 `Color{0x6E, 0x6E, 0x6E}`（对 `surface` 白底约 5.1:1），`dark()` 取 `Color{0x9A, 0x9A, 0x9A}`（对 `dark()` 的 `surface` `0x20, 0x21, 0x24` 约 5.7:1）；对比度登记为**非文本档**（形状承载「当前位置」这一信息）。高对比档由登记表派生：Material 明暗基线两列已越过 3:1，但 `C9` 是**按列**判「原生列全部达标 ⇔ 该列派生逐字节不动」，而五套风格档会覆写 `surface`，故其余十八列须由 `tools/gen/gen_contrast.cpp` 逐格实测；出现未达标格时，要么调基线值，要么按 `AURORA_CONTRAST_PRESET_FLOORS` 登记豁免并同步该表枚数，不能只凭基线两列断言「派生列不动」。
- `scrollbar_track`：轨道。`light()` 取 `Color{0x00, 0x00, 0x00, 20}`、`dark()` 取 `Color{0xFF, 0xFF, 0xFF, 20}`，按半透明叠层绘制；登记为**不适用**，理由是「纯装饰凹槽；以调用点固定的部分透明度合成，登记值本身不直接上屏」——与 `hover` / `pressed` / `scrim` 同一类行。

消费面只有滚动条绘制一处（绘制期读角色，不作为属性行默认值），故 `aurora_api.json` 的属性条目零新增，`theme_tokens` 的 `ColorRoles` 组枚数由 38 改 40、`consumed` 位不变（绘制直读不计入属性行消费位，与 `hover` / `pressed` / `shadow` / `scrim` 同口径）。

**登记表与取值链同步面**：`include/aurora/theming/detail/contrast_audit.h` 的 `AURORA_CONTRAST_AUDITS` 须为两枚新角色各补一行（thumb 作非文本档前景配 `surface`，track 作带理由的不适用行），显式声明枚数由 30 改 32，表头注释里的「38 color roles」「30 of 31 pairs」同步改；`ColorRoles::operator==` 的「三十八项全等」注释改「四十项」。`tools/gen/gen_contrast.cpp` 遍历 `AURORA_TOKENS` 取角色读数，本身不需要逐枚加代码，但产物里每列的角色集合与逐对行数随之变化，`tools/check/check_theme_contrast.py` 的 C1 / C3 / C8 / C9 要在扩表后重跑十腿变异自证。新增绘制代码位于 `check_no_theme_color_literals` 的扫描范围内，故不得出现 `Color{...}` 数字构造式或 `shaded(...)` 调用（L1 / L2 / L5），`tools/include/theme_literal_exemptions.h` 与 `theme_tokens.exemptions.files` 零变化（E1）；新角色的基线取值写在 `color_roles.h`，不在该门禁的扫描范围内。

`ColorRoles` 的 `sizeof` 随两枚 `Color` 增长，属 ABI 破坏：静态库消费方须随头文件**净重编**（增量对象混用会让同一结构体在两侧尺寸不同，仓库内已有该形态的崩溃先例）。`AURORA_TOKENS` 是静态常量表，不进 `sizeof`；`ScrollbarAxis` 是库内枚举，不进 `tools/include/known_enums.h` 与 `aurora_api.json` 的 `enums` 段。

### 6 焦点环两枚令牌接线

- 绘制侧：`Widget::paint_content` 在画环分支里改为 `out = max(0, focus_ring_inset) + max(0, focus_ring_width)`，环厚取 `focus_ring_width`（夹到 `≥ 0`），环圆角继续取既有常量 `AURORA_FOCUS_RING_RADIUS`（本批不新增圆角令牌）。
- 标脏侧：把本次实际使用的 `out` 缓存到一枚绘制期成员，初值 `2 + 2 = 4`（与改动前常量逐位相同），`dirty_bounds()` 改读该缓存——它仍无 `BuildContext`，但不再需要：标脏要回答的是「上一帧画到多远、这一帧要画到多远」。**切换帧的外扩须取两帧并集**：令 `out_used = max(上一帧缓存的 out, 本帧绘制所用 out)`，环宽由小变大时只按上一帧的 4 标脏会把新增环带裁在脏区之外（留下半截环），由大变小时同理要把旧环带擦掉；单读一份缓存会留下「脏区按旧宽度、绘制按新宽度」的中间态。`Widget::dirty_bounds()` 有两个读取方——`include/aurora/window/window.h` 的脏区收集与 `include/aurora/widget/scroll.h` 的离屏缓冲——两者同源，不得只改一处。
- 默认档零观感变化：两枚令牌默认值 2 / 2 与常量逐位相同，`out` 默认 4，故默认主题下的环带像素、标脏外扩与改动前逐像素逐值相同。
- 其余不变：`focus_ring_shown()` 的判据、`painted_focus_ring_` 的残影防护、`wants_focus_ring()` 的自绘控件退出机制全都保留；`utest_theme_tokens` 里「本模块未改读这两枚令牌」的注释与理由同步改述为已接线，并补「改令牌即随动、绘制与标脏同步」的成对断言。

### 7 影响面与判据口径

- **像素基线**：默认档为 `Classic`，任何「内容溢出」的滚动容器都会新出现一条右侧 / 底侧滚动条；`tests/golden/e2e_scroll.png` 必须重拍。不溢出的容器（含绝大多数控件用例）像素零变化。`Overlay` 档若不注入悬停 / 滚轮事件，在无头环境恒不绘制。重拍不是「接受变化」：先用改动前基线跑一次全量像素扫描，列出实际转红的场景清单，逐场景归因到「滚动条带新增」或「内容盒按带宽收缩」两类，差异不得落到第三类区域；`tests/golden/logical_snapshots.json` 同理逐项核对，禁止整份重写。
- **逻辑基线**：`tests/golden/logical_snapshots.json` 的 `scroll_tall_content` 场景记录的是盒结构，`Classic` 占位会让 `Scroll` 内容列的宽度按带宽收缩，逐值差异必须与「原宽 − （粗细 + 2 × 内缩）」对上再刷新，不得整份重写。
- **测试形态**：拖拽、轨道翻页、悬停与显隐全部经 `EventDispatcher::dispatch_mouse` 合成指针事件注入 `HeadlessSurface`，沿用 `utest_splitter` 的命中链拖拽夹具，不引入真 GUI 交互测试；时间推进一律用注入的 `now`，不依赖墙钟或帧计数（帧循环非限速，按帧数判定会漂移）。新夹具同样受 `check_no_cjk_literals` 与 `check_test_registry` 约束。
- **令牌门禁**：`check_theme_token_registry` 判登记表与字段集合、短名与声明序（T1 / T5 尤其不能让新角色漏登记或声明序与表序错开）；`check_theme_contrast` 扩表后判 C1 声明枚数与解析行数 / 产物读数三方对号、C3 每枚角色出现在某行一侧（期望集取自 `AURORA_TOKENS`，非产物）、C8 本节与表的等集（不适用角色清单漏补 `scrollbar_track` 即红）、C9 派生列按裸档位下限判且按列不重配色，十腿变异自证须在扩表后重跑；`check_no_theme_color_literals` 的 L1 / L2 / L5 / E1 判滚动条绘制不出现数字色构造式、不调 `shaded(...)`、豁免表与 `theme_tokens.exemptions.files` 零变化。
- **接口门禁**：`check_api_schema_sync`（重建 `aurora_api_json` 后 diff 只落在 `theme_tokens` 的 `ColorRoles` 组）、`check_api_budget`（当前约 54661 / 75000 tokens，余量 20339，新增两行不撞线）、`check_doc_comments`、`check_umbrella_header`（R8 要求每个公共头被分类，`widget` 下的 `detail/` 子目录本批新建，须在 `tools/check/umbrella_manifest.txt` 的 `[exempt]` 段按目录前缀补一行，不是「`detail/` 目录自动豁免」）、`check_naming_conventions`、`format-check` 与 `lint`。
- **性能面**：滚动条几何是常数级纯计算，每帧只在 `on_paint` 与命中判定各算一次；拖拽帧走既有标脏路径，不新增每帧计时（`Overlay` 的保持窗口只在到期前与收位滑动、滚边共用一次手势计时，三者都空闲才摘除）。不满足任何性能门槛，故不新增基准；零开销按**确定性计数**验收（每帧分配次数、内核时钟读取次数、条不该绘制时的几何计算次数恒 0，注入变异须转红），墙钟耗时只作辅助上界。

## 验收判据

1. 四个容器在内容溢出时按 `scrollbar_mode` 各自绘制滚动条；内容不溢出时不绘制、也不保留空间（含 `Scroll` 的「满宽放得下、收窄后换行才溢出」临界夹具，不得出现假阳性条）。同一场景在 `Classic` 与 `Overlay` 两档逐像素对比：前者内容区较后者窄（或矮）恰好 `thickness + 2 × inset`，条的位置与粗细在两档一致；`Overlay` 侧须注入合成指针进入事件令条显现，无注入时恒不绘制，两档对比的前提不成立。四容器的拖拽与几何口径逐一同值断言，不得只验一个容器。
2. 滑块长度满足 `clamp(track_length × viewport / content, min(24, track_length), track_length)`：构造「内容为视口 2 倍」的夹具，滑块长为轨道一半；构造「理论滑块长小于 24」的夹具（如视口 100dp / 内容 800dp，理论值约 12dp），滑块长夹到 24；另构造 `track_length < 24` 的极小视口夹具，滑块长夹到 `track_length` 且不得触发 `std::clamp` 的 `lo > hi` 未定义行为；`track_length <= 0` 时不画条。轨道两端内缩 2dp。
3. 拖拽滑块：偏移随指针单调变化，指针移出控件边界后仍继续（命中链捕获），抬起即停；`Δoffset / Δpointer` 等于 `max_offset / (track_length - thumb_length)`，用「内容为视口 2 倍」夹具逐值核对。`track_length - thumb_length` 落在浮点容差内的退化几何（极小视口）不响应拖拽；「内容不溢出故不画条」是判据一的另一件事，不得拿它充当分母零的夹具。
4. 轨道点击：点击滑块之前 / 之后分别向上 / 向下一页位移 `viewport_length` 并夹取；按住不动不产生重复翻页。
5. `Overlay` 档：指针进入容器即显、离开即隐；滚轮滚动后保持 `AURORA_SCROLLBAR_OVERLAY_HOLD` 再隐。保持窗口用注入的 `now` 步进验证（帧首前、到期前、到期后三次推进），不真等墙钟；帧循环非限速，按帧数或真实秒表判定都会漂移。显隐为瞬时切换，同一帧内两次重绘逐像素一致（绘制期不得二次取时）。悬停滑块时粗度加 2dp 且以轨道中轴对齐，拖拽期间保持。涉及指针捕获的用例 Press 必须配对 Release，否则单例派发器的捕获表会串到下一例。
6. 焦点环：把 `focus_ring_width` / `focus_ring_inset` 分别设为非默认值，断言「绘制环带的外扩距离」与 `dirty_bounds()` 的外扩距离相等（像素 + 标脏成对断言）；默认 2 / 2 档下与改动前的基线与标脏盒逐像素、逐值相同。改主题后新帧的环带按新值绘制，且按 `max(旧 out, 新 out)` 的并集标脏：由小变大和由大变小两个方向都须验到，前者防新增环带被脏区裁掉，后者防旧环带留残影；`window.h` 的脏区收集与 `scroll.h` 的离屏缓冲两处读取结果同源。
7. 色角色：`scrollbar_thumb` 对 `surface` 在**五套风格档 × 明暗 × 对比度两档共二十列**逐格实测 ≥ 3:1（Material 基线两列不算全覆盖），未达标格按登记表派生或登记预设豁免；`scrollbar_track` 登记为不适用并在规格 §5.1 逐枚列出；`with_high_contrast()` 按 C9 逐列判「原生列全达标 ⇔ 派生列逐字节不动」，轨道作为不适用行不参与派生；`utest_theme_roles` / `utest_theme_presets` 的角色枚数与逐对断言同步，`check_theme_contrast` 扩表后十腿变异自证重跑全绿、还原后回零。
8. `aurora_api.json` 重建后差异仅限 `theme_tokens` 的 `ColorRoles` 组（枚数 38 → 40、新增两条令牌行、五组短名总计 69 → 71），属性条目、控件数、枚举数、`consumed` 位与 `theme_tokens.exemptions.files` 零差异；登记表 `AURORA_CONTRAST_AUDITS` 的声明枚数与解析行数一致（30 → 32）；`check_api_schema_sync`、`check_api_budget`、`check_theme_token_registry`、`check_theme_contrast`、`check_no_theme_color_literals` 全绿。
9. 像素基线重拍清单仅含「内容溢出的滚动容器」场景（至少 `e2e_scroll`）；重拍前先出「改动前基线 → 改动后」的全场景差异归因表，逐格限定在滚动条带与收缩后的内容区，不得整份重写；重拍后全量 ctest 绿，且 `logical_snapshots.json` 的 `scroll_tall_content` 差异逐项等于占位带宽引起的收缩值。
10. 净重编后 `itest_widget_lifetime` 与全量 ctest 绿；`tools/verify/installed_consumer` 独立消费工程经 `cmake --install` → `find_package(Aurora)` → 编译链接 → 运行退出码 0，证明静态库与新版头文件同尺寸；`format-check`、`lint`、`check_doc_comments`、`check_umbrella_header` 绿。
11. **判据非空转须真注入变异**：下列五处一处一改、每处只让自己要钉的那条判据转红，还原后回绿——① 把 `thumb_length` 的下界写回 24（极小视口夹具转红，或在 asan 口径下由 `std::clamp` 前提违例直接暴露）；② 把 `Scroll` 的溢出判定顺序改回「先收窄」（临界夹具出现假阳性条，判据一转红）；③ 去掉 `Classic` 的内容盒收缩（判据一的带宽差与 `scroll_tall_content` 差异同时转红）；④ 让 `dirty_bounds()` 只读上一帧缓存而不取并集（焦点环由小变大用例转红）；⑤ 令 `Overlay` 绘制期二次取时（同帧两次重绘一致性转红）。绿灯本身不算证据，每一处都要记录「改了什么、哪条用例转红、还原后是否回绿」。

## 回写落点

- `codespec/specification/04-widget.md`：§4.1 的度量令牌消费面（`scrollbar_thickness` / 焦点环两枚由「无消费点」改为已接线），新增滚动条的几何、模式与交互段（挂在滚动交互一节下），并修正「唯一被认可的盒外绘制是基类统一焦点环」一句中与滚动条有关的边界表述（滚动条画在自身盒内）。
- `codespec/specification/07-environment-modifier.md`：§5.1 的角色枚数（38 → 40）、五组短名总计（69 → 71）、C3 说明里的「38 枚」、逐对归档行数（30 → 32：文本 20 / 非文本 4 → 5 / 不适用 6 → 7）、参与判定格数（480 → 500）与不适用格数（120 → 140）、「十列派生里六列改动」按实测重算，非文本档清单补 `scrollbar_thumb`、不适用角色清单补 `scrollbar_track`（后者是 C8 等集判据的硬约束），消费面段删去 `scrollbar_thickness` 与 `focus_ring_*` 的「无消费」表述并写明新的读取点；`conventions.scrollbar_mode` 由「无消费」改为布局期消费，`min_touch_target` / `focus_ring_style` 仍标注无消费点。
- `codespec/specification/05-event-navigation.md`：§3.1 的 `dispatch_mouse` 指针捕获段点名滚动条拖拽这一消费者（沿用同一捕获机制，不新增第二套拖拽通道；该文件没有名为「指针捕获」的独立小节），§3.2 的自入链契约补「条带内只交自身、不下降」这一形态，§5.2 的共用帧计时去留口径补入 `Overlay` 保持窗口这一第三条驱动。
- `codespec/specification/06-app-platform.md`：§3 帧循环与空闲节流段补明「滚动条保持窗口、收位滑动、拖入滚边三者都空闲才摘帧计时」，与 `Widget::tick_gestures` 的既有口径对齐。
- `codespec/SPECIFICATIONS.md`：§5 特性表中 `SPEC.FEAT.WIDGET.SCROLL-INTERACTION.001` 的「能力」列补滚动条可视化与两种占位模式、拖拽 / 轨道翻页 / 悬停显现，「落点」列补新增的滚动交互段落锚点；特性行总数不变。
- 根目录 CHANGELOG.md：`Breaking` 记两枚新色角色带来的 `ColorRoles` / `Theme` 尺寸变化与净重编要求、`Classic` 档内容盒收缩；`Added` 记滚动条与两种模式、焦点环两枚令牌接线；`Migration` 给出「不想要占位条」的现可行替代（用 `Overlay`）。
