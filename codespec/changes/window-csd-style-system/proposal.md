# CHG-004 跨后端 CSD 装饰层：自定义面加深 + 预设体系 + 明暗跟随

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-004 |
| 提出日期 | 2026-10-11 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | CSD 装饰「策略/状态层」从 `WaylandSurface` 抽出为跨后端共享组件；公共样式类型 `TitleBarStyle` 扩「度量/圆角/自定义配色」三类字段并新增与之一一对应的窗框样式类型；新增 6 套预设（三视觉语言 × 亮暗）与深浅色信号源（`window/platform.h`）；`X11Surface` 与 `Win32Host`（Win32/D3D11/WgpuWin32 三 Surface 共用）首次获得自绘装饰路径与 `WindowStyleOptions::decoration` 的真实映射；`Theme` 与 CSD 建立令牌派生通道；`aurora_api.json` schema、`codespec/specification/{03,06,07}`、`codespec/manual-test/10-window.md`、`CHANGELOG.md` 同批回写。既有 3 个预设工厂、`DecorationPolicy` 五档枚举语义、既有安全区口径与软件/GPU 双上屏路径均保持向后兼容 |

## 动机

CHG-001 把 Wayland CSD 的几何模型收敛到「单表面 + 阴影边距」，但 CSD 整体仍是**后端私有实现 + 半编程 + 与主题隔绝**的四处错位：

1. **只有 Wayland 真的自绘装饰。** `WindowStyleOptions::decoration` 在 X11 是死字段（该后端只认 `frameless`，走 `_MOTIF_WM_HINTS` 去装饰），在 Win32 同样是死字段（`Win32Host` 只认 `frameless`/`resizable`/`always_on_top`/`transparent`/`min_size`/`max_size`），GLFW/macOS/Wasm/Headless 无映射。同一份 `WindowOptions` 在三个桌面后端上跑出三种外观，`Auto` 的「不可用时回退 CSD」承诺只在 Wayland 成立。
2. **可自定义面窄。** `TitleBarStyle` 只有 9 个字段（高度、四组激活/失焦色、hover_tint、close_hover、按钮语言）+ 5 个显隐开关，按钮尺寸/间距、图标槽宽、标题字号与对齐、悬停按压态则全部由 `title_bar_geometry()` 里的字面量决定，应用不可触达。
3. **窗框几何完全锁死。** 阴影基色 `AURORA_SHADOW_BASE_COLOR{0,0,0,70}`、模糊半径 `margin − AURORA_SHADOW_BLUR_INSET_PX`、边距 `AURORA_SHADOW_MARGIN_DP = 10` 与整张塌缩矩阵全是 `constexpr`；没有圆角概念。任何想做「更重的投影」或「直角」的应用都只能给上游提需求。
4. **不成体系的预设。** 有 `Mac` 按钮语言却没有 macOS 配色，`windows_dark()` 没有对应的亮色版，实际可用组合是 3 个而非「三语言 × 亮暗 = 6」。`title_bar_style.h` 的预设之间是抄字段式重复，不是一张可持续维护的表。
5. **与 `Theme` 零联动。** `theming/` 里没有任何标题栏令牌，应用换 `Theme` 标题栏纹丝不动；库内也不存在任何 OS 深浅色探测能力，「跟随系统主题」这条桌面应用的基本能力目前无任何承载点。

若不先把上述 1 的共享层抽出来就铺三个后端，同一套命中测试与安全区计算会被复制三份，`title_bar_geometry()` 与热区分家正是 CHG-001 已经用真源收敛修掉的那类缺陷——这次会在更大尺度上复发。

## 变更内容

1. **抽出跨后端共享的 CSD 策略/状态层**（新增抽象 `aurora::csd::{resolve_mode, hit_button, consume_double_click, make_paint_state, DecorationInteraction, DecorationInput}`，落脚 `src/aurora/window/detail/csd_decoration.h`，仍属库内私有；命名与 `DecorationPolicy` 同源，刻意避开既有公共类 `WindowChrome`）。从 `WaylandSurface` 抽出五类职责，三后端只做平台映射：
   - 策略解析表：`DecorationPolicy` ×「后端能否提供 SSD」→ `{csd_title, csd_border, mod_move}` 三元组，现行 Wayland 语义**逐格锁进快照测试**后再接入其余后端，避免「重构」顺手改语义；
   - 指针语义：按钮序号命中（0=min / 1=max / 2=close）、标题带判定、拖拽移动 / 双击最大化的连击窗口（复用 `AURORA_DEFAULT_CLICK_WINDOW_MS` / `AURORA_DEFAULT_CLICK_RADIUS_DP` 库级常量，不新造阈值）、阴影边距八向缩放分类（复用 `csd::classify_resize_zone`）；
   - 状态装配：`TitleBarPaintState` 的构造（尺寸/原点/模式/焦点/hover/标题/图标/样式/shadow_margin）；
   - 安全区：`content_inset_for` 的调用口径与「状态切换 → 整幅重绘」触发；
   - hover 状态机（进/出/按键反馈）。
   既有 `csd::paint_title_bar` / `paint_window_shadow` / `csd_geometry.h` **职责不变**，仍是几何与绘制的唯一真源，本层只承接「策略与状态」，不接管任何矩形推导。
2. **自定义面加深**：`TitleBarStyle` 新增三类字段，全部带默认值、追加在既有字段之后（`TitleBarStyle{...}` 聚合初始化与三个既有预设的取值保持逐值不变，Breaking 面为零）：
   - 度量 `TitleBarMetrics`：按钮盒尺寸 `button_size`、按钮间距 `button_gap`、按钮组贴边外边距 `button_margin`、图标槽尺寸 `icon_size`、内容安全内边距 `content_padding`、标题字号 `title_font_size_pt` 与字重 `title_font_weight`。判据统一为「**非 0 即覆写**」——0 表示沿用 `title_bar_geometry()` 按栏高联动的推导式，故三套既有预设的观感不会被一次性改坏（默认值逐值复现现状，由单测手写期望锁证）。已知边界：Adwaita/Mac 的间距与外边距推导值非零，故 0 无法表达「零间距」，需无缝排布时改用 `TitleBarButtonLayout::Windows`（其推导值本就是 0）；该边界就地写入几何层注释；
   - 圆角 `corner_radius`：标题栏背景**下两角**圆角半径（0 = 直角）；顶角归窗框/阴影外轮廓，故实现为「整圆角矩形 + 补回顶部两角」，标题栏顶边与窗口上沿齐平；
   - 交互态：除既有 `hover_tint` / `close_hover` 外补**按压态** `pressed_tint` / `close_pressed`，缺省即退化为现有观感；绘制优先级为「按压 > 悬停」（否则按下时按压色被悬停色遮住）；
   - 既有 5 个显隐开关与 `TitleBarButtonLayout` 语义不变。
   - **配套行为变更：标题栏按钮动作由「按下即触发」改为「释放时触发」**（新增 `csd::begin_press` / `csd::consume_press` 纯函数承载）。这不是风格选择而是按压态的前提：按下即触发时，按下与关闭/最大化同帧发生，按压态没有任何可渲染的时间窗，`pressed_tint` 永远画不出来。改为释放触发同时带来业界一致的「按下后拖开即取消」纠错语义（GTK/Win32 均如此），结算判据为「按下与释落在同一枚按钮上」。既有自动化用例不依赖该时序（已核查），影响面仅为人机交互手感与手工测试用例。
3. **窗框几何可编程 + 铺开到 X11 / Win32**：新增窗框样式类型，承载 `shadow{enabled, margin_dp, color, blur_radius}` 与 `corner_radius`，三后端共用同一份配置（Wayland 现状取值即其默认值，迁移零视觉变化）。
   - 默认行为：三后端默认 `margin_dp = 10`（与现状一致），运行期能力探测失败时**自动降 0 并打 `AURORA_LOG_INFO`**——X11 查选主是否存在 composite manager（`_NET_WM_CM_Sn`），Win32 查 DWM composition 是否启用；降级路径要求：不再分配 ARGB visual / 不写半透明环、热区与 `content_inset` 同步回落到 0 口径。
   - X11 端：去装饰沿用 `_MOTIF_WM_HINTS`；窗口 move/resize 走 `_NET_WM_MOVERESIZE`，信息栏/最大化/关闭走既有 `_NET_WM_STATE` 与 `WM_DELETE_WINDOW` 通道；带 alpha 的 margin 需 ARGB visual，取不到即走上面的降级。
   - Win32 端：去 `WS_CAPTION`（保留可缩放边框）后在客户区自绘并经 `WM_NCHITTEST` 回报三档热区（标题带→`HTCAPTION`、按钮→保留给自绘、边缘→八向 `HT*`）； Snap / 系统菜单在自绘模式下不代偿，属已知边界，明确写入 `BUILD_OPTIONS.md` 的平台差异注记。
   - macOS / GLFW / Wasm 本批不铺（无真机验证环境），接口形状保持一致以便后续接入。
4. **预设体系 6 套**：`Adwaita` / `Windows11` / `macOS` 三视觉语言 × 亮暗，各自返回一个纯值类型样式；既有 `adwaita_dark()` / `adwaita_light()` / `windows_dark()` 保留为对应工厂的兼容别名（old values 逐值不变）。新增 `TitleBarStyle::from_preset(CsdPreset)` 工厂与 `CsdPreset` 枚举；预设表以「一份数据表 + 亮暗两份配色覆盖」承载，杜绝字段级复制。
5. **明暗三级优先**：显式样式覆盖 > `Theme` 令牌 > OS 偏好。新增 `ColorScheme{Unknown, Light, Dark}` 与 `Platform::color_scheme()`（`window/platform.h`，与 `capabilities()` 同列），OS 侧：Win32 读注册表 `AppsUseLightTheme` 并挂 `WM_SETTINGCHANGE`；Linux 优先 freedesktop appearance portal，不可用时回落 GTK 近似（`gsettings`/环境变量），探不到即 `Unknown` → 回落 `Theme`；macOS 读 `effectiveAppearance`（该分支标注未在本仓构建验证）。探到切换信号时标记装饰脏并重绘，不引入轮询定时器。
6. **AI 友好面**：6 个预设进 `aurora_api.json` 的枚举表与序列化往返（`check_api_schema_sync` + `aurora_api_json` 目标同批刷新），样式类型支持 JSON 往返以便 AI/工具链直接下发外观改动。
7. **测试**：新增 `utest_csd_decoration`（策略表逐格断言、连击窗口常量一致性、八向分类边界、三级优先解析顺序与回落链）；既有 `utest_title_bar_painter` 的「直绘 vs 录制回放」字节比对扩到 6 套预设；`utest_csd_geometry` / `utest_csd_shadow_compose` 按新的可配参数参数化（默认值==现状，作为回归基线）；``codespec/manual-test/10-window.md`` 增补真机用例（三后端各自的标题栏按钮/拖拽/双击/明暗切换）。
8. **版本台账**：实施落地时同批登记变更台账（新增公共类型与枚举记入 Added，Breaking 面为空，并随 alpha 升版段一并定稿）。

## 验收判据

1. `ctest --preset ninja-test` 全绿，含新增 `utest_csd_decoration`；`check_api_schema_sync` / `check_naming_conventions` / `check_api_budget` / `check_doc_comments` / `check_codespec_xref` / `check_code_doc_sync` 无新增告警。
2. **策略语义零漂移**：抽层前后，`DecorationPolicy` × {有 SSD, 无 SSD} × {Normal, Maximized, FullScreen, tiled} 共 20 组组合的 `{csd_title, csd_border, mod_move, content_inset}` 结果逐值相同（单测表驱动锁证）。
3. **默认外观零变化**：6 套预设中的 `Adwaita` 亮暗、`Windows11` 暗三套与既有 `adwaita_dark/light`、`windows_dark` 输出样式**逐字段相等**；Wayland 在本机构建上跑 demo 的 golden/字节比对与变更前一致。
4. **自定义生效**：把度量字段（按钮盒尺寸、间距、外边距、图标槽、内容内边距、标题字号/字重）、圆角、按压态各改动一次，`title_bar_geometry()` 产出的所有矩形随之变化，且**绘制热区与命中热区仍逐点重合**（同一份几何被两层共用的回归断言）。
5. **按压态与释放触发**：按下关闭钮后拖离再松手**不关窗**（取消成立）；按下并在同钮释放则动作成立；任一路径下 `pressed_button` 与 `grab` 均复位（无「卡住的高亮」、无事件残留被吞）；按压序号经 `make_paint_state` 抵达绘制层。
6. **窗框可编程 + 降级**：默认 `margin_dp=10` 下三后端（本机构建可验 Wayland/X11）视觉一致；人为把 CSD 窗框 margin 设 0 时，热区与 `content_inset` 同步归零且无黑边残留；X11 在无 composite 的会话下自动降 0 并打 INFO 日志、无 alpha 伪影。
7. **明暗跟随**：`Unknown` 源下回落 `Theme`；把 OS 偏好在 Dark/Light 间切换一次，标题栏配色在下一帧发生变化且重绘仅限装饰层。
8. **平台差异如实登记**：Win32 自绘实现标注「未在本仓库无头构建内编译验证」，其在 Windows 上的验收落到 ``codespec/manual-test/10-window.md`` 的对应用例；README/BUILD_OPTIONS 的平台差异注记与之一致。

## 回写落点

- `codespec/specification/07-environment-modifier.md`：§4.1 `DecorationPolicy` 补三后端映射表与深浅三级优先；§4.2 更新样式字段总表、6 套预设清单、窗框样式与安全区口径；Margin 塌缩矩阵按「可配 margin」重述。
- `codespec/specification/06-app-platform.md`：§5 能力模型增 `server_decorations` 能力标志与 `Platform::color_scheme()` 查询。
- `codespec/specification/03-layout-render.md`：§8.8 GPU 装饰合成与软件 margin 合成的「双路同源」约束扩展到新的窗框样式字段（任何新增视觉参数必须两路共用）。
- `codespec/manual-test/10-window.md`：增补三后端 CSD 真机用例（按钮/拖拽/双击/窗框/明暗切换）。
- `codespec/BUILD_OPTIONS.md`：补平台差异注记（Win32 Snap 与系统菜单在自绘模式下不代偿、X11 需 composite 才有外部阴影）。
