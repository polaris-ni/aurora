# CHG-010 macOS / Adwaita 两档还原原生观感细节（红绿灯描边与失焦灰、全屏箭头、header bar 分隔线与新版配色）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-010 |
| 提出日期 | 2026-10-13 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | `CsdPreset::MacLight`/`MacDark`/`AdwaitaLight`/`AdwaitaDark` 四档预设的取色与度量、`TitleBarStyle` 新增 `separator` 成员、`title_bar_geometry()` 的 Adwaita 盒宽与间距推导式、`csd::paint_title_bar()` 的 Mac / Adwaita 两个分支；不改 Windows 两档、不改公共类型名与函数签名、不改 `DecorationPolicy` / 事件契约 |

## 动机

CHG-008 / CHG-009 把三套语言的**度量**对齐了真机，但度量的正确不等于观感的正确。逐项比对原生后仍差六处，且都属于「一眼能看出来」的级别：

1. **macOS 红绿灯在窗口失焦时不变灰**。真实 macOS 的非活动窗口三枚圆点统一变浅灰（`#DDDDDD` 底 + `#D1D0D2` 描边）且不显符号；本仓此前无论焦点如何都恒画三色圆点。
2. **macOS 红绿灯没有描边**。真机每枚圆点都有一圈比填色深一档的 1px 描边（close `#E24B41` / min `#E1A73E` / max `#2DAC2F`），这圈描边是圆点立体感的来源；本仓画的是纯色圆盘，看上去是「三个扁平色块」。
3. **macOS 符号只在悬停那一枚上浮现**。真机是**指针进入红绿灯组即三枚符号同时浮现**（`NSWindow` 的 group hover 语义）；本仓只给 `hovered_button` 那一枚画符号，指到红钮时只有 ✕ 出现，与真机明显不同。
4. **macOS 符号取色是统一灰**。真机符号取各钮自身的深色调（close `#460804` / min `#90591D` / max `#2A6218`），不是同一支灰色。
5. **macOS 绿钮字形是方框**。真机绿钮是「进入全屏」，符号为**双向对角箭头**（退出全屏时为内向箭头）；本仓沿用通用空心方框，与绿钮语义也不符。
6. **Adwaita 亮色档背景是灰且没有底部分隔线**。libadwaita 现行 `headerbar` 亮色背景为 `#FFFFFF`（暗色 `#2E2E32`），且**底缘恒有一条 shade 分隔线**（亮色 = 12% 黑、暗色 = 36% 黑）；本仓沿用旧 Adwaita 的灰底 `#EBEBEB` 且完全不画分隔线，观感停留在 GNOME 3 时代。

另有三处量级差异同源：Adwaita 的悬停圆底按盒内切（26dp）而非真机的「16dp symbolic 图标 + 3dp 内边距」= 22dp；按钮盒 26dp / 间距 6dp 而真机 `.default-decoration` 下是 30dp / `border-spacing: 3px`；symbolic 字形撑满 16dp 画布，而真机 16×16 画布内符号只占约 12×12。

## 变更内容

1. **macOS 红绿灯取色全面对齐真机取色表**（`src/aurora/window/detail/title_bar_painter.cpp`）：三色填色沿用 Apple 语义色 `#FF5F57` / `#FEBC2E` / `#28C840`（不变），新增各自深一档的 1px 描边；符号改按各钮深色调绘制；按下态改为「填色 + 描边 + 符号」三件同时加深（真机按下即整枚变深，本仓此前是叠一层白色罩）。
2. **macOS 失焦态**：`active == false` 时三枚改画 `#DDDDDD` 填色 + `#D1D0D2` 描边，且不绘制任何符号。
3. **macOS 符号改为整组同时浮现**：判据由「该枚被悬停」改为「该组任一枚被悬停 / 被按下」。
4. **macOS 绿钮字形改为全屏双向箭头**：常态（未最大化）画向外的对角双箭头，最大化态画向内的对角双箭头；最小化横线按真机 8px、关闭 ✕ 臂长按真机 8px。
5. **Adwaita 预设改取 libadwaita 现行色**（`include/aurora/window/title_bar_style.h`）：亮色 `bg_active` `#EBEBEB` → `#FFFFFF`、`bg_inactive` → `#FAFAFB`、`fg_active` → `#333334`（`RGB(0 0 6 / 80%)` 叠在白上的实色）；暗色 `bg_active` `#303030` → `#2E2E32`、`bg_inactive` → `#222226`。
6. **新增 `TitleBarStyle::separator`**（同文件）：标题栏底缘分隔线颜色，`alpha == 0` 即不画。Adwaita 两档给 shade 实色（亮色 `#E0E0E0` = 12% 黑叠白、暗色 `#1D1D20` = 36% 黑叠 `#2E2E32`），Windows / Mac 两档置全透明（Win11 与 macOS 标题栏均无此线）。默认成员值取 AdwaitaDark 的取值，以守住「`TitleBarStyle{}` ≡ `from_preset(AdwaitaDark)`」契约。
7. **Adwaita 几何改按 `.default-decoration` 口径**（`src/aurora/window/title_bar_geometry.cpp`）：按钮盒推导式 `clamp(h−10,22,30)` → `clamp(h−6,26,34)`（栏高 36 时给 30，= 22 内容 + 4 外边距），间距 6 → 3（libadwaita `windowcontrols { border-spacing: 3px }`）。
8. **Adwaita 悬停圆底改按语言常量 22dp**（绘图层）：圆是「16dp symbolic 图标 + 3dp 内边距」的产物，与按钮盒宽解耦（盒被覆写为任意宽时圆仍是 22dp、居中）。
9. **Adwaita 字形网格 16dp → 12dp**（绘图层）：16×16 symbolic 画布四周各留 2dp，符号实际占 12×12；线宽维持 1.5dp。
10. 受影响的既有单测基线同步（`utest_title_bar_style` / `utest_title_bar_geometry` / `utest_csd_decoration` / `utest_title_bar_painter`），并按「字形与盒解耦」不变量新增 Mac / Adwaita 的断言。

## 验收判据

1. `utest_title_bar_painter` 新增：Mac 档 `active == false` 时三枚圆点圆心像素为失焦灰 `#DDDDDD`，且任一枚悬停下也不出现符号墨迹——钉住「失焦变灰且不显符号」。
2. `utest_title_bar_painter` 新增：Mac 档圆点边缘 1px 环的取色不等于圆心填色（描边生效），且等于该钮的描边色。
3. `utest_title_bar_painter` 更新：Mac 档悬停**任一枚**时，另外两枚圆内也出现符号墨迹（整组浮现）；对照组（无悬停）三枚圆内均无符号墨迹。
4. `utest_title_bar_painter` 新增：Mac 档按下态圆心为「按下填色」（比常态填色深），且不再是常态填色叠白罩的结果。
5. `utest_title_bar_painter` 新增：Mac 绿钮（maximize）悬停时的墨迹形状为对角双箭头——判据取「圆内墨迹像素数显著多于同尺寸方框」或「墨迹外接框在对角方向有像素」，二者取可直接观测者。
6. `utest_title_bar_painter` 新增：Adwaita 档底缘分隔线存在（栏高 −1 行为 `separator` 色），且 Windows / Mac 两档同一位置仍是画布底色（无分隔线）。
7. `utest_title_bar_style` 更新：Adwaita 两档的 `bg_active` / `bg_inactive` / `separator` 取 libadwaita 现行值；Windows / Mac 四档 `separator` 为全透明；`from_preset(AdwaitaDark)` 与 `TitleBarStyle{}` 逐值相同（含新增的 `separator`）。
8. `utest_title_bar_geometry` 更新：Adwaita 按钮盒 30dp、间距 3dp 的排布数值；`utest_csd_decoration` 的既有基线同步。
9. `utest_title_bar_painter` 更新：Adwaita 字形外接框宽恒定 ≈12dp（换两组盒宽比对，证明解耦不变量）；悬停圆底直径恒 22dp（换两组盒宽比对）。
10. 全量 `ctest --preset ninja-test` 绿（含 `check_api_schema_sync`、`check_doc_comments`、`check_naming_conventions`、`check_change_proposals`、`lint`、`format-check` 等全部 `check_*`）。

## 回写落点

- `codespec/specification/07-environment-modifier.md`（§4「窗口装饰与自绘标题栏」：macOS 红绿灯的描边 / 失焦灰 / 整组浮现 / 全屏箭头口径，Adwaita 的现行配色、底缘 shade 分隔线、`.default-decoration` 盒宽与间距、symbolic 字形与按钮盒的三层解耦）。
- 发布记录 CHANGELOG.md（`Changed` 节：Mac / Adwaita 四档预设取色与度量改为现行原生口径、`separator` 新增；`Fixed` 节：红绿灯失焦不变灰、符号只显单枚、绿钮字形不符）——该文档不属 codespec 树，故按 AGENTS.md 硬规则 5 不列为回写落点，实际已同步。
