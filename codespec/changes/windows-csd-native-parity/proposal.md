# CHG-008 Windows 预设标题栏对齐原生度量

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-008 |
| 提出日期 | 2026-10-12 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | `CsdPreset::WindowsLight` / `WindowsDark` 两档预设取值、Windows 按钮布局的字形绘制口径、关闭钮字形取色；不改公共类型与函数签名，不改几何规则表以外的推导式，不改 `DecorationPolicy` / `content_inset()` / 事件契约 |

## 动机

`demo_csd_gallery` 的 ClientSide 预览窗切到 `WindowsLight` / `WindowsDark` 时，观感与 Win32 真机 DWM 标题栏差距过大，逐项实测（截图按 150% 缩放折算为逻辑 dp）如下：

- 按钮盒：原生 46×32dp，自带实现 52×36dp（`round(36×1.44)`）。
- 最小化字形：原生 10×10dp 设计尺寸，自带实现 36dp 宽（= 按钮盒宽 × 2/3）。
- 最大化字形：原生 10×10dp，自带实现约 31dp。
- 字形线宽：原生 1px，自带实现 1.2dp。
- 标题字号：原生 12px（≈9pt），自带实现 13pt（≈17.3px）。
- 关闭钮字形：原生随前景色（亮色下为深色），自带实现恒为白色 α235。

最后一项是功能性缺陷而非观感差异：`WindowsLight` 背景 `#F3F3F3` 与白色字形几乎同亮度，实测像素分析在该按钮区域检不到任何墨迹——**关闭按钮在亮色主题下画不出来**。

根因是字形尺寸与按钮盒尺寸被绑成比例（`盒宽 / 3.0`、`盒宽 / 3.6`），而 Win11 的实际行为是**固定 10×10 epx 设计网格**（Segoe Fluent Icons 的字形度量），按钮盒变宽时字形不变。两者独立后，「盒宽 52 / 字形 34.7」这种挤压感随之消失。

## 变更内容

1. 预设取值（`include/aurora/window/title_bar_style.h`）——仅 `WindowsLight` / `WindowsDark` 两档：
   - `height` 36 → 32dp（对齐 Win11 标题栏高）。
   - `metrics.button_size` 显式置 46dp（原靠 `round(height×1.44)` 推导，改栏高后该式恰好也得 46，显式置值使栏高被再次调整时按钮盒不随之漂移）。
   - `metrics.icon_size` 显式置 16dp（原推导 `min(16, height−20)` 在栏高 32 时退化为 12，须显式守住原生 16dp 图标槽）。
   - `metrics.title_font_size_pt` 13 → 9pt；`metrics.title_font_weight` 500 → 400（对齐 Win11 标题栏 Regular 口径）。
   - `hover_tint` / `pressed_tint` 改 WinUI 遮罩口径：非关闭钮悬停 α 26 → 15（≈0.0605），按压 α 52 → 25（≈0.1）；关闭钮悬停/按压红不变。
2. 字形设计网格（`src/aurora/window/detail/title_bar_painter.cpp`）：Windows 分支的三枚字形改用**固定 10×10dp 设计网格**居中于按钮盒（`half = 5dp`），与盒宽解耦；线宽 1.2 → 1dp。还原（restore）字形改按设计网格重绘为「8×8 主框 + 右上错位副框的顶边与右边」，取代原按 `盒宽/3.6 × 0.45` 推导的错位量。
   - Adwaita / macOS 两套语言维持既有「盒宽推导」口径不变，本次不改其观感。
3. 关闭钮字形取色（同文件）：常态跟随 `fg_active` / `fg_inactive`，仅当底色被悬停或按压的特征红替换时转白——红底上深色符号不可读，白底上白色符号不可见，二者是同一个判据的两侧。

## 验收判据

1. `utest_title_bar_painter` 新增断言：`WindowsLight` 样式下，关闭按钮盒内**坐标点处必须出现前景色墨迹**（常态无悬停）、按钮盒内两侧留白区必须仍是底色（证明字形未撑满盒宽）——以「盒内出现墨迹 + 字形外接框宽 ≤ 半个盒宽」两条独立基准钉住，不依赖字形实现细节。
2. `utest_title_bar_painter` 新增断言：Windows 布局下最小化字形的外接框宽度恒为 10dp（与盒宽无关）——以两组不同 `metrics.button_size` 的渲染结果比对，证明「字形与盒解耦」这一不变量。
3. `utest_title_bar_style` 更新并守住：两档 Windows 预设的 `height == 32`、`metrics.button_size == 46`、`metrics.icon_size == 16`、`title_font_size_pt == 9`、`button_layout == Windows`；且 Windows 预设的特征红（`close_hover`）与 Adwaita 一样跨明暗恒定。
4. `utest_title_bar_geometry` 更新并守住：Windows 预设下按钮盒为 46×32、三枚按钮自右上角无缝向左排、图标槽 16dp、标题区左界不侵入图标槽。
5. 真机走查 `demo_csd_gallery`：ClientSide 预览窗在 `WindowsLight` / `WindowsDark` 下按钮字形尺寸、间距、关闭按钮可见性与 Win32 系统标题栏（Auto 档）观感一致；三枚按钮悬停/按压反馈正确。
6. 全量 `ctest --preset ninja-test` 绿（含 `check_api_schema_sync`、`check_doc_comments`、`check_naming_conventions`、`lint`、`format-check` 等全部 `check_*`）。

## 回写落点

- `codespec/specification/07-environment-modifier.md`（§4「窗口装饰与自绘标题栏」：`TitleBarStyle` 字段说明段与「预设」段的 Windows 档取值口径，补记字形设计网格与按钮盒解耦的不变量，以及关闭钮字形取色规则）。
- 发布记录 CHANGELOG.md 的 `Changed` 节（Windows 预设标题栏对齐原生度量）——该文档不属
  codespec 树，故按 AGENTS.md 硬规则 5 不列为回写落点，实际已同步。
