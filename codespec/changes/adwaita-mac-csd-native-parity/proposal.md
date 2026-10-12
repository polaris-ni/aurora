# CHG-009 macOS / Adwaita 预设标题栏对齐原生度量 + 字形设计网格三档统一

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-009 |
| 提出日期 | 2026-10-13 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | `CsdPreset::MacLight` / `MacDark` 两档预设取值、`Adwaita` 语言的几何推导式（间距 / 外边距）与默认标题字重、三套语言的字形绘制口径、`Adwaita` 关闭钮字形取色与按压态、`center_title` 的绘制层实现、`demo_csd_gallery` 的预设循环；不改公共类型与函数签名，不改 `DecorationPolicy` / `content_inset()` / 事件契约 |

## 动机

CHG-008 只把 Windows 两档对齐了真机，同一次自检暴露出另外三处同类问题：

1. **macOS 两档的几何沿用 Adwaita 的栏高与推导式，与真机差距 22%~43%**。macOS 标准窗口（无 toolbar）标题栏高 28pt、红绿灯直径 12pt、相邻圆心距 20pt（边缘间距 8pt）、首枚圆心距窗口左缘 20pt（左外边距 14pt）；自带实现在栏高 36dp 下给出直径 `clamp(36×0.4,11,14)=14` 与外边距 8，且栏高本身差 8dp。
2. **Adwaita 的间距 / 外边距记错**：现为间距 4、外边距 8，而 libadwaita 的 `.toolbar`（`AdwHeaderBar` 自动带该类）是「6px margins and spacing」、`headerbar` 另有 `padding: 0 6px`。
3. **Adwaita 关闭钮字形与 WindowsLight 是同一个功能性缺陷**：`paint_title_bar` 的 Adwaita 分支恒写 `Color{255,255,255,235}`，而 `AdwaitaLight` 常态下关闭钮没有底色（只有悬停才铺红），白字形落在 `#EBEBEB` 上**画不出来**。
4. **Adwaita 分支不消费 `pressed_button`**：只判 `hovered_button`，按下时没有任何反馈（`button_fill` 的「按压 > 悬停」优先级只被 Windows 分支使用）。
5. **`center_title` 是「文档承诺、绘制层未实现」的开关**：几何层在 `center_title=true` 时给出整宽带并注释「文字由绘制层水平居中」，但绘制层只是原样 `draw_text(g.title, ...)`，从不居中——该开关此前置真只会把带宽放开，视觉上仍是左对齐（Mac 档若直接置真会让标题压在红绿灯上）。

另有一项与上面同源：三套语言的字形尺寸此前统一按「盒宽 / 3」推导，CHG-008 只把 Windows 档改成固定设计网格，Adwaita / Mac 仍随盒放大（Mac 档占圆直径 2/3，真机约一半）。

## 变更内容

1. **macOS 两档预设钉值**（`include/aurora/window/title_bar_style.h`）：`height` 36 → 28dp、`metrics.button_size` = 12dp、`button_gap` = 8dp、`button_margin` = 14dp、`icon_size` = 16dp、`title_font_size_pt` = 13pt / `title_font_weight` = 400、`center_title` = true（macOS 标题居中；WinUI 左对齐，故只有 Mac 档置真）。钉值而非改推导式的理由：Mac 的推导式 `clamp(height×0.4,11,14)` 是「未钉值时」的回落，改它会把手工构造 Mac 样式的既有调用方一并拖动。
2. **Adwaita 语言改按原生口径推导，预设不钉值**（`src/aurora/window/title_bar_geometry.cpp`）：间距 4 → 6、右缘外边距 8 → 6（libadwaita `.toolbar` 口径）。不改预设的理由是契约——文档与单测都要求 `TitleBarStyle{}` 的默认成员值等于 `from_preset(CsdPreset::AdwaitaDark)`，在预设里钉 metrics 会让两者在度量上分叉；其原生按钮盒 `clamp(height−10,22,30)` = 26dp 本就等于 libadwaita `windowcontrols` 按钮尺寸，故无需钉。默认 `metrics.title_font_weight` 500 → 700（libadwaita `.title { font-weight: bold }`）同属这一条：默认档即 Adwaita 档。
3. **字形设计网格三档统一**（`src/aurora/window/detail/title_bar_painter.cpp`）：把原先按盒宽推导的 `glyph_min/max/close` 删去，改为按调用方给定「设计网格半宽 + 线宽」绘制的 `glyph_min_at/glyph_max_at/glyph_close_at`——Windows 10×10dp / 线宽 1dp、Adwaita 16×16dp（symbolic 图标）/ 1.5dp、macOS 6pt（12pt 圆内占一半）/ 1dp。Windows 的还原态仍单列（真机是「8×8 主框 + 副框只露顶边与右边」，与通用双框不同形）。
4. **Adwaita 关闭钮取色与按压态**（同文件）：关闭钮字形改为「底色是否被填充」决定——常态随前景色，底色换成悬停 / 按压特征红后转白（与 CHG-008 的 Windows 判据逐字同源）；非关闭钮改走 `button_fill`，使按压态生效。
5. **`center_title` 落地 + 水平口径收敛**（同文件）：绘制层按 `FontEngine::measure_width` 的实测量宽把标题摆到整宽带的中点，偏移后右缘不动（超宽标题的截断口径与左对齐一致）。默认成员值由 false 改 **true**（默认档即 Adwaita），Adwaita / Mac 两档居中、Windows 两档显式置 false——libadwaita `AdwHeaderBar` 与 macOS 窗口标题均居中，WinUI 标题左对齐。`TitleBarStyle{}` ≡ `from_preset(AdwaitaDark)` 的契约不受影响（两者同为 true）。
6. **标题垂直居中**（同文件）：`draw_text` 的 y 是行盒顶，绘制层按 `FontEngine::measure_height` 把行盒摆到栏高的中点后再画。此前文字贴栏的上缘（实测偏上 Adwaita 6dp / Windows 8.5dp / Mac 1.5dp），三套语言的标题在原生里都是栏内垂直居中的。
7. **demo 补齐**（`examples/demos/demo_csd_gallery.cpp`）：`theme_preset` / `theme_name` 由 4 档扩到全部 6 档（补 `MacDark` / `MacLight`），否则 `set_title_bar_style` 的红绿灯分支在该 demo 里永远走不到；同时把 `policy_label` / `policy_description` / `theme_name` 的返回类型改为 `std::string_view`（消除每个调用点的构造开销，CLion / clang-tidy 告警项）。

## 验收判据

1. `utest_title_bar_painter` 新增：AdwaitaLight 下关闭钮盒内必须出现前景色墨迹（常态无悬停），且盒内左侧留白仍是底色——与 CHG-008 判据 1 同构，直接钉住「关闭钮可见」这个修复。
2. `utest_title_bar_painter` 新增：Adwaita 最小化字形外接框宽恒 16dp、macOS 恒 6pt，各以两组不同 `metrics.button_size` 的渲染比对，证明「字形与盒解耦」不变量（macOS 判据把「底色」传成圆点色、只扫圆心单行，避开圆外底色）。
3. `utest_title_bar_painter` 新增：`center_title=true` 时标题墨迹中心 ≈ 窗口水平中心，`center_title=false` 的对照组明显偏左——钉住「居中」真的发生，而不只是把带宽放开。Mac 档与 Adwaita 档各一条（后者即默认档，守的是 `center_title` 默认值的口径）。
4. `utest_title_bar_painter` 新增：三套语言的标题墨迹**垂直**中心与栏高中心的偏差 < 2.5dp。敏感侧在 Adwaita / Windows（修复前偏上 6 / 8.5dp，远超阈值）；Mac 档修复前只偏 1.5dp，单看不足以区分，故判据以三档整体为准——撤销居中偏移时该用例必转红。
5. `utest_title_bar_style` 新增 / 更新：Mac 两档的 `height == 28`、`button_size == 12`、`button_gap == 8`、`button_margin == 14`、`icon_size == 16`、`title_font_size_pt == 13`、`title_font_weight == 400`、`center_title == true`，两档度量同源；Adwaita 两档的 `title_font_weight == 700`；`from_preset(AdwaitaDark)` 与 `TitleBarStyle{}` 在**度量与 `center_title` 上亦逐值相同**（守住「不钉 metrics」这条设计约束）。
6. `utest_title_bar_geometry` 新增 / 更新：Adwaita 三枚按钮按间距 6 / 外边距 6 排布（close 768 / max 736 / min 704 @800dp 宽）；Mac 预设下红绿灯 14,8,12,12 / 34,8,12,12 / 54,8,12,12、图标槽 78,6,16,16、标题区为整宽带，且与「未钉值回落」用例（直径 14 / 外边距 8）并存——两组数字不同正是钉值生效的证据。
7. 全量 `ctest --preset ninja-test` 绿（含 `check_api_schema_sync`、`check_doc_comments`、`check_naming_conventions`、`check_change_proposals`、`lint`、`format-check` 等全部 `check_*`）；`demo_csd_gallery` 的 ClientSide 预览窗可循环到六档且不崩。

## 回写落点

- `codespec/specification/07-environment-modifier.md`（§4「窗口装饰与自绘标题栏」：三套语言的几何口径与字形设计网格、Mac 档钉值、标题水平 / 垂直两个方向的对齐口径与绘制层落地、关闭钮取色规则）。
- 发布记录 CHANGELOG.md（`Changed` 节：macOS / Adwaita 预设对齐原生度量与默认标题字重变更、`center_title` 默认口径；`Fixed` 节：Adwaita 亮色档关闭钮不可见、`center_title` 未生效、标题未垂直居中）——该文档不属 codespec 树，故按 AGENTS.md 硬规则 5 不列为回写落点，实际已同步。
