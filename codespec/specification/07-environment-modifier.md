# 环境注入、主题与修饰符（environment / theming / i18n / modifier）

> 覆盖 `include/aurora/environment/`（3 个头）、`theming/`（4 个头）、`i18n/`（5 个头）、`modifier/`（6 个头）。
> 本文件是环境依赖注入、媒体查询、主题与国际化、以及 `Modifier` 修饰系统的**唯一权威**。
> 响应式属性见 [`02-state.md`](02-state.md) §2；控件的二层属性划分见 [`03-layout-render.md`](03-layout-render.md) §7.4。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 环境与构建上下文 | `environment/environment.h`、`environment/build_context.h` |
| 媒体查询 | `environment/media_query.h` |
| 主题 | `theming/theme.h`、`theming/theme_scope.h`、`theming/theme_query.h`、`theming/style_props.h` |
| 国际化 | `i18n/localized_string.h`、`i18n/string_table.h`、`i18n/locale.h` |
| 修饰符 | `modifier/modifier.h`、`modifier/modifier_base.h`、`modifier/modifier_layout.h`、`modifier/modifier_paint.h`、`modifier/modifier_input.h`、`modifier/modifier_transform.h` |

---

## 2 环境依赖注入

### 2.1 核心概念

| 概念 | 说明 |
|:---|:---|
| `Environment` | 类型 → 值的映射。**对外继承链不可变**：`env.with<T>(value)` 返回注入了 `T=value` 的**新** `Environment`（子级读取不改写父级）；根 `Environment` 另提供 `set_local<T>()` / `set<T>()` 原地写入（每帧刷新注入值时不必换对象）。`env.get<T>()` 读取 |
| `BuildContext` | 每个组件渲染时拿到的上下文。`ctx.environment<T>()` 向上查找最近注入的 `T`，返回 `const T*`；未注入时返回 `nullptr`，调用方判空或回退默认 |
| `Provider<T>` | 控件，在子树根注入 `T` |

```cpp
// 父级注入
auto root = au::ThemeProvider{ au::Theme::dark(), build_app() };

// 子组件内查询（无需参数传递）
au::Text("Hi").color(ctx.environment<au::Theme>() ? ctx.environment<au::Theme>()->text
                                                  : au::Theme::light().text);
```

构造 `Provider<T>` 用**位置式**而非指定初始化器（`Column` / `Row` 非聚合，须用 `*Props` 聚合或初始化列表）。

**专用 Provider 别名**：`ThemeProvider`（`T = Theme`）、`LocaleProvider`（`T = Locale`）、`MediaQueryProvider`（`T = MediaQuery`）。

**与显式原则的关系**：主题、字体、颜色通过显式 `Provider` / scope 传递，**不依赖隐式上下文继承**（见 [`05-event-navigation.md`](05-event-navigation.md) §8.1）。

---

## 3 媒体查询与响应式构建

### 3.1 MediaQuery

`MediaQuery`（`environment/media_query.h`）是响应式上下文值类型，把设备度量沿树下行。

| 字段 | 说明 |
|:---|:---|
| `size` | 当前窗口 / 子树可用逻辑尺寸（dp） |
| `scale_factor` | 设备像素比（`dpi / 96`；由 `Window::present_root` 从 `Surface::scale_factor()` 注入到 `BuildContext::scale_factor`，是本地坐标缩放的**唯一权威来源**）。**Win32 已收敛为单点真值源**：宿主内 `Impl::scale` 一个成员经 `to_physical` / `to_logical` 两个换算函数供给全部坐标面（见 `03-layout-render.md` 的 DPI 感知条与 `08-tooling.md` §8.2）；**X11 / Wayland / GLFW 的缩放变化上报仍未实现**（`Surface::set_scale_change_handler` 未被它们 override），跨屏或改系统缩放时该值不会自动更新 |
| `text_scale_factor` | 系统字体缩放（辅助功能） |
| `orientation` | 由 `size`（非 Win32）/ `screen_size`（Win32）派生：宽 ≥ 高判为 `Landscape`，正方形归入 `Landscape`。枚举 `ScreenOrientation{Portrait, Landscape}`，与 `divider.h` 的 `Orientation` 语义不同，**不复用** |
| `screen_size` | 物理屏幕的逻辑尺寸（dp） |
| `platform` | 编译期常量——Win32 下为 `Windows`，其余 `Unknown`（不做运行时 OS 探测）。枚举含 `Unknown/Windows/MacOs/Linux/Web` |
| `device` | 编译期常量——Win32 下为 `Desktop`，其余 `Unknown`。枚举含 `Unknown/Desktop/Mobile/Tablet` |
| `padding` | 安全区（刘海 / 状态栏）内边距（dp），默认 0 |
| `prefer_reduced_motion` | 系统「减弱动效」偏好 |

`PlatformKind` / `DeviceKind` 反映**编译目标**平台（编译期常量），非运行期 OS 探测；运行期能力探测走 `au::platform().capabilities()`（见 [`06-app-platform.md`](06-app-platform.md) §5）。

**客户端自绘装饰的安全区（框架自动下沉）**：Wayland 客户端自绘装饰（CSD）标题栏 / 边框占用的区域经 `Surface::content_inset()` 暴露，由 `Window::present_root` **自动下沉**——应用根统一挂到框架私有壳 `detail::ContentInsetRoot` 下，内缩量经其 `PaddingEdges` 修饰在布局期收紧约束、绘制/命中期平移内容盒，应用树无感知。位置类事件（鼠标/滚轮/触控/文件拖放）的命中与派发根**即该壳**（`Window::hit_root()`），事件坐标为**整幅表面逻辑坐标**（与 `content_inset()` 同域），下沉平移由壳的同一条 `PaddingEdges` 修饰在命中期承担——与绘制零漂移（窗口宿主侧见 [`06-app-platform.md`](06-app-platform.md) §2.4）。根注入的 `MediaQuery` 相应为**内容区口径**：`padding` 归零（占用区已被框架消费）、`size` 扣除内缩；应用**不应**再手动消费 `padding`（双重内缩）。需要「自绘进安全区」等特殊场景时，经 `Window::content_inset()` 读取原始占用区，并以空 `MediaQueryProvider` 覆盖根注入口径。inset 为零的后端（各后端的 SSD / 系统非客户区档与 Frameless 档）壳的修饰链为空，行为与无壳逐位一致；inset 变化（如进出全屏归零）下一帧重排生效。测试见 `tests/integration/itest_content_inset_sink.cpp`。

**工厂**

| 工厂 | 说明 |
|:---|:---|
| `MediaQuery::of(float scale)` | 轻量构造，仅设缩放因子 |
| `MediaQuery::from_surface(const Surface&)` | 跨平台成型：非 Windows 取 Surface 尺寸与默认值，并把 `s.content_inset()` 并入 `padding`；Win32 取真实屏幕逻辑尺寸与减弱动效 |
| `MediaQuery::of(const BuildContext&)` | 读最近祖先 Provider，无则返回进程级默认实例 |

**读取 API**（`MediaQueryProvider` 是 `using` 别名，无法加静态 `of`，故提供自由函数）：

- `media_query_of(const BuildContext&) -> const MediaQuery*`：向上查找最近注入的 `MediaQuery`；无 Provider 时返回 `nullptr`，调用方按需降级。
- `MediaQuery::of(const BuildContext&) -> const MediaQuery&`：便捷封装，无 Provider 时静默返回进程级默认实例。

### 3.2 自动注入

`Window::present_root` 每帧自动以 `MediaQuery::from_surface(*surface)` 在**根 `BuildContext`** 注入 `MediaQuery`（存入稳定的 `Window::root_env_`，地址恒定，避免子树 Provider 持悬空父指针）。

因此**无需手动包裹 `MediaQueryProvider`**，整棵树（含根 widget 自身）即可经 `media_query_of(ctx)` / `MediaQuery::of(ctx)` 读取设备上下文。`root_env_` 每帧原地更新注入值（对象本身不重建）以反映窗口 resize。`Application::run` 经同一 `present_root` 自动受益。手动 `MediaQueryProvider` 仍按「最近祖先优先」覆盖此默认值。

### 3.3 LayoutBuilder

`LayoutBuilder`（`widget/layout_builder.h`）是响应式构建原语。`builder` 类型为 `Reactive<std::function<Node(const BuildContext&, const Constraints&)>>`，布局阶段按 `constraints` 动态构建子节点。

**仅在「约束显著变化」或「builder 闭包被替换」时重建并重新 mount 子节点**，约束不变则复用缓存子树，避免每帧抖动。闭包内部读取的 `media_query_of` 不自动订阅——响应式驱动来自约束变化（如窗口缩放）。

```cpp
auto root = au::MediaQueryProvider{
    au::MediaQuery::from_surface(surface),
    au::LayoutBuilder{
        [](const au::BuildContext& ctx, const au::Constraints& c) -> au::Node {
            const au::MediaQuery* mq = au::media_query_of(ctx);
            const bool wide = (mq != nullptr) && (c.max.width >= 600.0F);
            return wide ? au::Node{ build_two_column(ctx) }
                        : au::Node{ build_one_column(ctx) };
        }},
};
```

---

## 4 窗口装饰与自绘标题栏

### 4.1 DecorationPolicy

`DecorationPolicy`（`window/surface.h`）跨后端声明，各 `Surface` 按 compositor 能力映射，解决「compositor 不支持 `xdg-decoration` 时无标题栏、窗口不可操作」的问题。

枚举：`Auto` / `ServerSide` / `ClientSide` / `Borderless` / `Frameless`，置于 `WindowStyleOptions::decoration`（默认 `Auto`；`WindowStyleOptions::frameless` 等价于 `Frameless`）。

| 取值 | 行为 |
|:---|:---|
| `Auto` | 优先协商服务端装饰（SSD，原生标题栏）；compositor 不支持时回退客户端自绘（CSD）标题栏 + 窗口阴影边距（margin 承载缩放热区，见下） |
| `ServerSide` | 强制 SSD；不可用时退化为 CSD 兜底（避免无装饰且不可操作） |
| `ClientSide` | 强制自绘 CSD 标题栏（即便 compositor 支持 SSD），带窗口阴影边距 |
| `Borderless` | 无标题栏，保留窗口阴影边距（margin 带可拖拽缩放）；移动靠**修饰键拖拽**（按住 `Super` / `Alt` 拖拽任意处 → `xdg_toplevel_move`） |
| `Frameless` | 完全无装饰、无阴影边距，由应用自绘 UI 并经程序化窗口控制驱动状态 |

**三后端映射**（策略解析纯函数 `csd::resolve_mode(policy, ssd_available)`，`src/aurora/window/detail/csd_decoration.h`；「有无 SSD」各后端构造期一次判定）：

| 后端 | Auto / ServerSide | ClientSide | Borderless | Frameless |
|:---|:---|:---|:---|:---|
| Wayland | 有 `xdg-decoration` → SSD；无 → CSD（title+margin） | CSD（title+margin，即便合成器支持 SSD） | 仅 margin 带 | 无任何装饰 |
| X11 | 保留 WM 装饰（X11 的 SSD 即 WM 原生口沿，无须协商） | `_MOTIF_WM_HINTS` 撤 WM 口沿后自绘标题栏 + 内缘八向热区 | 仅内缘热区 | 无任何装饰 |
| Win32 | 保留 `DWM`/系统非客户区装饰 | 去 `WS_CAPTION`（保留 `WS_THICKFRAME` 系统缩放模态）+ `WM_NCCALCSIZE`（wParam=TRUE）吞非客户区使客户区 = 整窗，最大化把客户区拉回显示器工作区（经典 −8dp 边框问题修正）；`WM_NCHITTEST` 回报热区：内缘八向（复用 `csd::classify_resize_zone`，带宽 = `frame_margin_dp(frame, dwm_capable)`）→ `HTLEFT`/…、标题带空白 → `HTCAPTION`（系统白给拖拽/双击最大化/Aero Snap）、按钮 → `HTCLIENT`，修饰键（Alt/Meta）拖拽兜底 → `HTCAPTION`；标题栏自绘经 `paint_decoration` 于三表面 present 路径覆绘，按钮按压/释放结算与 X11/Wayland 共层；建窗 / `set_size` 的窗口矩形在吞框档**不做** `AdjustWindowRect` 非客户区外扩（窗口矩形 = 请求物理客户区，`DWMWA_EXTENDED_FRAME_BOUNDS` 实测 == 窗口矩形 ⇒ 整窗可见；外扩会让吞框后的客户区多出不可见边框带宽，布局视口与真实客户区错位——底部/右侧纯背景带） | 无标题栏，热区与修饰键拖拽同左（`csd_mod_move` 激活） | `WS_POPUP`，无任何装饰 |

**安全区**：CSD 标题栏 / 阴影边距占用区经 `Surface::content_inset()` 暴露，由 `present_root` 的下沉壳（`ContentInsetRoot`）自动消费（见 §3.1「客户端自绘装饰的安全区」）；根注入 `MediaQuery.padding` 恒为内容区口径（已消费 ⇒ 归零）。

Wayland CSD 的**阴影边距模型**（单表面，libadwaita 风格；纯逻辑收敛在 `src/aurora/window/detail/csd_geometry.h`）：wl_surface buffer = 应用内容 + 四周 `AURORA_SHADOW_MARGIN_DP`（10dp）边距，边距承载真 alpha 柔影与 ≥8dp 的八向缩放热区；`xdg_surface.set_window_geometry(m,m,内容宽,内容高)` 声明可视窗口边界，`wl_surface.set_opaque_region` 仅报内容矩形。此模型下 **`WaylandSurface::size()` 返回整幅表面逻辑尺寸（内容 + 2×margin）**，`content_inset()` 语义为「表面原点 → 应用内容原点」，按下表取值（`m` = margin、`tb` = 标题栏高）：

| 状态 | ClientSide / Auto 回退 CSD | Borderless | SSD / Frameless |
|:---|:---|:---|:---|
| Normal / 平铺外常态 | `{m, m+tb, m, m}` | `{m, m, m, m}` | `{0,0,0,0}` |
| Maximized / 任意向 tiled | `{0, tb, 0, 0}`（margin 塌缩） | `{0,0,0,0}` | `{0,0,0,0}` |
| FullScreen | `{0, tb, 0, 0}`（揭示条出现前 tb 亦不绘制） | `{0,0,0,0}` | `{0,0,0,0}` |

margin 在 Maximized / FullScreen / tiled 态塌缩为 0（贴边无可拖缝隙、无阴影），回 Normal 恢复；状态切换走整幅重绘。指针在进入表面的三个入口（enter / motion / button）统一做「表面坐标 → 内容坐标（−margin）」映射，热区判定 `csd::classify_resize_zone` 在内容坐标域进行（表驱动纯函数，`utest_csd_geometry` 守门；映射值仅供后端内部 CSD 判定消费）。对应用上抛的事件（悬停/按压/滚轮）则按契约转发**整幅表面坐标原值**——下沉折算统一由派发根壳承担（见 §3.1），后端不做第二份平移。margin 厚度可经 `WindowFrameStyle::shadow_margin_dp` 显式配置（0 = 关闭；显式关闭 / 能力不足 / 非正值三路归零，见 `csd::frame_margin_dp`），默认值逐值等于既有 10dp 常量。X11 与 Win32 后端 buffer 即内容矩形（不假设 ARGB 视觉与合成器），无外延 margin 分量——外延阴影自动降级为 0，标题栏带为**内缘覆绘** + **内缘热区**（同一 `csd::classify_resize_zone` 判定式按「内容坐标缩进 margin 后的外延环」口径复用，不写第二份八向逻辑），`content_inset()` 只承载标题栏带高度；Win32 自绘档标题栏带宽 `frame_margin_dp(frame, dwm_capable)`（DWM 合成关闭时归零，探针 `aurora_verify_win32_csd` 锁降级语义）。Headless 维持零 inset 口径。

**程序化窗口控制**（`Surface` / `Window` 虚函数，默认空实现）：`close()`、`minimize()`、`toggle_maximize()`、`set_fullscreen(bool)`。Wayland 经 `xdg_toplevel` 协议生效，使无标题栏 / 无边框窗口也能由应用按钮驱动状态。

### 4.2 TitleBar

**`TitleBarStyle`**（`window/title_bar_style.h`）是 CSD 标题栏样式值类型，字段：`height`（默认 36）、`bg_active`、`bg_inactive`、`fg_active`、`fg_inactive`、`hover_tint`、`close_hover`、`button_layout`（`TitleBarButtonLayout{Adwaita, Windows, Mac}`，默认 `Adwaita`）、`show_minimize`、`show_maximize`、`show_close`、`show_title`、`center_title`，以及三类自定义项：`metrics`（`TitleBarMetrics`：按钮盒尺寸 / 间距 / 贴边外边距 / 图标槽 / 内容内边距 / 标题字号字重，各分量 **0 = 沿用视觉语言的栏高联动推导式**，非 0 才是显式覆写）、`corner_radius`（标题栏背景**下角**圆角，0 = 直角）、`pressed_tint` / `close_pressed`（按压态配色，绘制优先级「按压 > 悬停」）。

**预设**：`CsdPreset`（三套视觉语言 × 明暗两档 = `AdwaitaLight/Dark`、`WindowsLight/Dark`、`MacLight/Dark` 六值）+ 工厂 `TitleBarStyle::from_preset(CsdPreset)`；`TitleBarStyle{}` 的默认成员值即 `AdwaitaDark`。旧三个静态预设函数（`adwaita_dark()` / `adwaita_light()` / `windows_dark()`）已删除，迁移路径见 `CHANGELOG.md` 的 Migration 节。各预设显隐开关全开、按压 alpha 不低于悬停；macOS 红绿灯圆形底色由绘制层按布局语言固定，预设不覆盖。

**度量口径三档分治**（`metrics` 的「0 = 沿用栏高联动推导式」仍成立，但三套语言的取值策略不同）：

| 视觉语言 | 栏高 | 度量取法 | 出处 |
|:---|:---|:---|:---|
| Adwaita | 36dp | **不钉** `metrics`（`TitleBarStyle{}` 默认成员值即 `AdwaitaDark` 的契约，钉值会让两者分叉）；间距与贴边外边距由推导式旧值 4/8 改为 **6/6** | libadwaita `.toolbar`（`AdwHeaderBar` 自动带该类）「6px margins and spacing between widgets」，`headerbar` 另有 `padding: 0 6px`；按钮盒推导结果 `clamp(h−10,22,30)` = 26dp 已等于 libadwaita `windowcontrols` 尺寸，故不钉 |
| Windows | 32dp | **显式钉值**：按钮盒 46dp、图标槽 16dp（栏高 32 时「`min(16, h−20)`」会退化成 12，故必须钉住）、标题 9pt / 400、悬停 / 按压遮罩 α≈0.0605 / ≈0.1 | Win11 DWM 真机实测（WinUI 标准遮罩口径） |
| Mac | 28dp | **显式钉值**：红绿灯直径 12dp、间距 8dp（相邻圆心距 20dp）、贴边外边距 14dp（首枚圆心距左缘 20dp）、图标槽 16dp、标题 13pt / 400 且居中 | macOS 标准窗口口径；旧推导式在栏高 36 下给直径 14 / 外边距 8，差 17% / 43% |

`TitleBarMetrics::title_font_weight` 默认 500 → **700**（libadwaita `.title` 为 bold），Windows / Mac 两档各自钉 400。

**字形与按钮盒宽解耦的不变量**（三套语言统一）：字形一律由绘制层按固定设计网格居中于按钮盒绘制，按钮盒再宽字形也不放大——Windows 10×10 epx（Segoe Fluent Icons）/ 线宽 1dp、Adwaita 16×16 symbolic / 1.5dp、macOS 6pt（12pt 圆内占一半）/ 1dp，常量见 `title_bar_painter.cpp` 的 `AURORA_{WIN,ADWAITA,MAC}_GLYPH_HALF_DP` / `..._STROKE_DP`；旧实现统一按「盒宽 / 3.0、`盒宽 / 3.6`」推导，字形随盒放大（Windows 档实测放大到 34dp，是挤压感根因）。Windows 关闭钮还原字形重绘为「8×8 主框 + 右上错位副框的顶边与右边」。

**关闭钮字形取色规则**（三套语言统一）：常态随前景色（亮色档为黑），仅当底色被悬停 / 按压的特征红替换时转白——红底上深色符号不可读、白底上白色符号不可见，是同一判据的两侧（修复 `AdwaitaLight`（`#EBEBEB`）/ `WindowsLight`（`#F3F3F3`）下关闭钮画不出来的功能性缺陷）。Adwaita 悬停 / 按压的圆形底与按压态反馈（优先级「按压 > 悬停」）同批补齐。

**标题对齐**（水平与垂直各自成规，均由绘制层落地）：
- **水平**：`center_title=true` 时几何层给**整宽带**（不扣按钮组），绘制层按 `FontEngine::measure_width` 实测量宽把文字摆到整宽带的中点。默认成员值为 **true**（默认档即 Adwaita：libadwaita `AdwHeaderBar` 的标题居中），macOS 两档同为 true，**Windows 两档显式置 false**（WinUI 标题左对齐）。此前该开关是「文档承诺、绘制层未实现」——置真只放开带宽，文字仍左对齐。
- **垂直**：三套语言一律居中，与 `center_title` 无关。绘制层按 `FontEngine::measure_height` 把字体行盒摆到栏高的中点再调 `draw_text`——`draw_text` 的 y 是**行盒顶**（基线 = 顶 + ascent），把标题带原样传下去会把文字顶到栏的上缘（实测墨迹中心比栏高中心偏上：Adwaita 6dp / Windows 8.5dp / Mac 1.5dp）。行盒含下伸部，纯大写标题的视觉中心因此比几何中心低 1~2dp，属字体度量固有偏差，不另加补偿常数。

挂载于 `WindowStyleOptions::title_bar`，运行期经 `Surface::set_title_bar_style` 热更。

**窗框样式**：`WindowFrameStyle`（`window/window_frame_style.h`，挂载于 `WindowStyleOptions::frame`，热更口 `Surface::set_window_frame_style`）：`shadow_enabled` / `shadow_margin_dp`（投影带厚度 = 缩放热区宽度）/ `shadow_color` / `shadow_blur_radius`（0 = 由 margin−1 推导，显式值夹到 margin 内）/ `corner_radius`（外轮廓圆角，0 = 直角）。默认值逐值等于既有 `AURORA_SHADOW_*` 常量，未配置零视觉变化；投影厚度 / 基色 / 模糊两条上屏路径（软件 `compose_shadow_margins_bgra` 与 GPU `draw_shadow`）同源消费折算后的值。X11 无外延 margin，厚度只作用于内缘热区宽度。

**共享装饰层**：策略解析、按钮命中、按压结算（**动作在释放触发**——按下即触发会使按压色永不可见，且失去「按下后拖开即取消」的纠错语义，对齐 GTK/Win32）、全屏揭示条状态机、连击记账与绘制装配收敛在库内私有头 `src/aurora/window/detail/csd_decoration.h`（纯值、无平台头，`utest_csd_decoration` 以策略 × SSD 有无 × 窗口几何态的快照表逐格锁语义）；各后端只做平台事件映射。

**几何单一来源**：纯函数 `title_bar_geometry(width, style, maximized, resizable)` 返回各按钮 / 图标 / 标题矩形（隐藏 = 空盒）；绘制与命中测试共用，规则以其实现注释为唯一权威。

**绘制单一来源**：`csd::paint_title_bar(Painter&, const TitleBarPaintState&)`（`src/aurora/window/detail/title_bar_painter.h`，库内私有）把 `title_bar_geometry` 的矩形集光栅为一次 `Painter` 调用（背景 + 图标 + 标题 + 三套视觉语言的按钮，hover/激活态入状态结构）。软件路径（`WaylandSurface::Impl::draw_decoration`）与 GPU 路径（`WaylandSurface::record_client_decoration` → 帧 `Sink::end_frame` 回放进 swapchain 帧，见 [`03-layout-render.md`](03-layout-render.md) §8.8）同为它的消费者，故两条上屏路径的装饰逐位一致（`utest_title_bar_painter` 以「直绘 vs 录制回放」全画布字节比对守门）；无装饰可绘（SSD 合成器 / `Frameless`）时录制侧返回 false、当帧不含装饰命令。

**窗口阴影 underlay**：`csd::paint_window_shadow`（同头文件）以内容矩形为形状、零偏移、模糊半径 `margin−AURORA_SHADOW_BLUR_INSET_PX` 录制 `Painter::draw_shadow`，仅作 GPU/DisplayList 路径的阴影源（`record_client_decoration_underlay` → `Sink::begin_frame` 先于 app 帧回放，Shadow 管线写真 alpha 到透明清除的 swapchain）。软件 wl_shm 路径不经 Painter（Painter 目标画布恒不透明，混合恒写 alpha=255，画不出半透明柔影）：`present()` 在 RGBA→BGRA 预乘 swizzle 之后调 `csd::compose_shadow_margins_bgra`（`src/aurora/window/detail/csd_shadow_compose.h`）直接覆写 margin 环的 BGRA 字——内容矩形跳过、角部按两向欧氏距离衰减、外缘写透明字 0 且幂等。视觉参数两条路径同源于 `csd_geometry.h`（`AURORA_SHADOW_BASE_COLOR{0,0,0,70}` / `AURORA_SHADOW_BLUR_INSET_PX` / `shadow_attenuation`），由 `utest_csd_shadow_compose` 对 BGRA 字逐值守门。

**`Surface` 相关虚函数**：`set_title_bar_icon(std::shared_ptr<Image>)`（图标槽）、`begin_window_move()` / `begin_window_resize(WindowResizeEdge)`（控件发起拖拽 / 缩放——**须在指针按下事件派发栈内同步调用**，受 Wayland `xdg` move / resize 的 serial 时效约束）。

**`WindowChrome` 服务**（`window/window_chrome.h`）：经 Environment 注入的窗口动作门面（`close` / `minimize` / `toggle_maximize` / `set_fullscreen` / `begin_move` / `begin_resize` / `content_inset`），由 `present_root` 注入根环境供控件消费（headless 安全 no-op）。

**`TitleBar` 控件**（`widget/title_bar.h`）：声明式标题栏，支持 icon 位图、title / subtitle、`add_action` 文本 chips、内置窗口控制钮与 Snap 弹窗；空白区拖拽、双击最大化、失焦变暗，支持 `describe_static` 与序列化往返（自定义 Snap 动作除外）。

---

## 5 主题

### 5.1 Theme

`Theme`（`theming/theme.h`）是扁平设计令牌：`Theme{ .background, .primary, .on_primary, .text, .font }`；`Theme::light()` / `Theme::dark()` 提供默认主题。经 `ThemeProvider` 注入，组件用 `ctx.environment<Theme>()` 读取。

**命名令牌层**（叠加于扁平字段，不改注入机制）：

| 成员 | 说明 |
|:---|:---|
| `Theme::set_token(name, TokenValue)` | 登记命名令牌。`TokenValue` 可承载 `Color` / `Font` / `double`（dp）三态之一 |
| `Theme::token(name)` | 返回 `std::optional<TokenValue>` |
| `Theme::token_or<T>(name, fallback)` | 强转目标类型，缺失或类型不匹配时回退 |

令牌随 `ThemeProvider` 注入整体传递，后代经 `environment<Theme>()` 读到的 `Theme` 即含令牌表。

**明暗双档取值**（一个主题自带两套，角色名一套、明暗两列——Material 3 color roles / Fluent 2 alias tokens 同构）：`Theme::scheme`（`ColorScheme{Unspecified, Light, Dark}`，`light()` / `dark()` 工厂自动置位）+ 暗色覆写表 `dark_tokens`；解析经 `token_for(name)` / `token_in(name, scheme)`——Dark 档先查覆写表、未命中回落基础表 `tokens`，其余档位只查基础表。`light()` / `dark()` 播种约 16 个语义令牌（`surface` / `surface.raised` / `text` / `text.muted` / `accent` / `border` / `hover` / `pressed` / `danger`，命名取两家公共子集）与 `csd.*` 组件覆写位；`csd.*` 数值基准与 `CsdPreset::AdwaitaLight/Dark` 一致，保证「从令牌派生」与「直接取预设」两条路输出相同的默认观感。

**明暗三级优先链**（`theming/csd_style_bridge.h` 的 `resolve_color_scheme(explicit, theme, os)` 纯函数）：显式样式覆盖 > `Theme::scheme` > OS 偏好；各层以 `Unspecified` 表示「不表态」逐层下探，全链未指定时由调用方回落 `AdwaitaDark`。OS 层经 `os_color_scheme()`（`window/platform.h`）显式查询——Linux 读 GTK 配置文件启发式（不引 DBus 依赖），Win32 读注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` 的 `AppsUseLightTheme`（REG_DWORD，0 = Dark；键在值缺 = Light；键打不开 = Unspecified，实现独立 TU `win32_platform.cpp` 避免公共头引入 `<windows.h>`），macOS 按 `NSAppearance` 契约标注未在本仓库无头构建内编译验证；`Platform::color_scheme` 字段须由调用方填充（`platform()` 不代做——读偏好有文件 / 注册表访问副作用）。

**Theme → CSD 派生通道**：`title_bar_style_from_theme(theme)` / `window_frame_style_from_theme(theme, base)`（`csd_style_bridge.h`）——预设做基座，`csd.title_bar.*` 组件令牌逐项覆盖，几何 / 显隐 / 度量字段不参与派生（属定制面）；应用在主题变化时重调 `set_title_bar_style(派生结果)` 即完成运行期明暗切换（装饰重绘下一帧生效），不引入轮询。测试见 `utest_theme_csd_bridge`。

**内置主题登记的令牌**（`Theme::light()` / `Theme::dark()` 各自 `set_token`，`Theme::with_defaults()` 与裸 `Theme{}` 不登记任何令牌）：

| 令牌名 | 浅色 | 深色 | 消费方 | 未登记时的回退 |
|:---|:---|:---|:---|:---|
| `focus.ring` | 纯黑 | 纯白 | `Widget::paint_content` 的统一焦点环（见 [`05-event-navigation.md`](05-event-navigation.md) §4.4） | `primary` |

回退语义是刻意的：环色改由令牌驱动后，未登记该令牌的自定义主题仍画 `primary`，行为与改动前逐位一致，不构成未经请求的视觉变更。

### 5.2 StyleProps

`StyleProps`（`theming/style_props.h`）是轻量样式叠加结构，字段（`background` / `foreground` / `font` / `corner_radius` / `padding`）均为 `TokenOr<T>` 两态——可填「具体值」或「令牌名」。

`StyleProps::resolve(theme)` 把全部两态字段解析为 `ResolvedStyle` 具体值：令牌名经 `Theme` 查询且类型匹配则返回，否则回退 fallback。组件可在绘制前用最近祖先 `Theme` 解析 `StyleProps`，实现「令牌驱动的样式」而无需逐一手写扁平字段读取。

`theming/theme_scope.h` 与 `theming/theme_query.h` 分别提供换肤 scope 与主题查询辅助。

---

## 6 国际化

| 类型 | 说明 |
|:---|:---|
| `LocalizedString` | 可本地化的字符串。`LocalizedString::tr(key)` 按 key 查表，或 `LocalizedString{ "字面量" }` 原样显示；`resolve(const StringTable*, const Locale&)` 解析为最终文本 |
| `StringTable` | 字符串表（key → 各语言文本）。`default_string_table()` 返回内置表；`StringTable::add(locale, key, tmpl)` 注册 |
| `Locale` | 语言 / 地区标识（如 `"zh-CN"`），随 `LocaleProvider` 注入，文本解析时按当前 Locale 查表 |
| `PluralCategory` | CLDR 复数类别（`Zero`/`One`/`Two`/`Few`/`Many`/`Other`），与 CLDR 定义一致；每个语言至少含 `Other` |
| `plural_category` | `plural_category(double n, const Locale&)` —— 按 CLDR 基数规则表把计数值映射到类别，`loc.language` 选规则；未知语言回退英语规则。覆盖 ar(全六类)/ru(one·few·many·other)/fr(one·many·other)/en·de(one·other)/zh·ja(other) |
| `Currency` | 货币代码（`USD`/`EUR`/`JPY`/`CNY`/`GBP`），符号与默认小数位取自轻量自研表 |
| `format_number` / `format_currency` / `format_date` | 按 `Locale` 格式化：数字（千位分组 + 小数点）、货币（符号位 + 小数位）、日期（本地模式，如 de `dd.MM.yyyy`、zh/ja `yyyy年M月d日`） |

所有文本属性类型为 `Reactive<LocalizedString>`，因此 `.content = "Hi"` 与 `.content = au::LocalizedString::tr("greeting")` 等价。

```cpp
au::Button(au::ButtonProps{ .label = au::LocalizedString::tr("save") });
au::Text(au::LocalizedString::tr("greeting"));
```

---

### 6.1 CLDR 复数与本地化格式

`StringTable` 复数语法从 `one/other` 扩到 CLDR 六类（`zero`/`one`/`two`/`few`/`many`/`other`），分支由 `plural_category(n, loc)` 依 `loc.language` 的规则表选择；旧式仅 `one=`/`other=` 的模板在缺类别时回退 `other=`，与既有行为兼容。

```cpp
t.add(Locale{"ar"}, "files",
      "{0, plural, zero={0} 个文件 one={0} 个文件 two={0} 个文件 few={0} 个文件 many={0} 个文件 other={0} 个文件}");
```

覆盖语言：ar(全六类) / ru(one·few·many·other) / fr(one·many·other) / en·de(one·other) / zh·ja(other)，未知语言回退 en。

数字 / 货币 / 日期按 `Locale` 格式化（守零依赖自研表）：

```cpp
au::format_number(1234567.0, Locale{"de"}, 0);                 // "1.234.567"
au::format_currency(1234.56, au::Currency::EUR, Locale{"de"}); // "1.234,56 €"
au::format_date(2025, 10, 25, Locale{"de"});                   // "25.10.2025"
```

---

### 6.2 控件内置文案的词条 key

除宿主传入的文本属性外，少数控件**自带**上屏文案（占位提示、空态提示之类）。这些不是字面量：
一律按 key 查 `default_string_table()`，宿主登记译文即可本地化。**库不预置任何语言的词条**
（`StringTable` 无删除接口，库侧写入会给宿主留下不可逆污染），未登记时回退控件内声明的英文字面量。

| key | 控件 | 文案 | 兜底 |
|:---|:---|:---|:---|
| `command_palette.placeholder` | `CommandPalette` | 搜索框占位符 | `Type a command...` |
| `command_palette.no_results` | `CommandPalette` | 无匹配结果时的空态提示 | `No matching commands` |

三档优先级：**文本覆盖（`set_placeholder` / `set_empty_message`）> 按 key 查表 > 兜底字面量**。
文本覆盖生效后该串**不再查表**，宿主须自己备多语言；换 key 则**仍走 i18n**，故同一进程内多个面板
可各挂不同词条且都能翻译。设 key 会清掉此前的文本覆盖。

解析发生在布局与绘制期，locale 取 `ctx` 注入的 `Locale`（未注入回落缺省档），与 `Text` / `Button`
的 `resolved_text()` / `resolved_label()` 同口径，故运行期切 locale 立即生效。

```cpp
auto &t = au::default_string_table();
t.add(Locale{"zh"}, "command_palette.placeholder", "输入命令…");
t.add(Locale{"zh"}, "command_palette.no_results", "没有匹配的命令");
```

> 上表 key 是**外部契约**：宿主登记译文要靠这条串。单测以字面量钉死这两条 key（不用常量自比自），
> 否则库侧 key 拼错时「注册端」与「查表端」一起错、两边抵消，用例会恒绿。

---

## 7 Modifier 修饰系统

### 7.1 概述

`Modifier` 用一组**正交、可组合**的修饰符表达内边距、背景、可点击、尺寸、边框、裁剪、对齐、偏移、拖拽等横切能力，替代继承爆炸。

`Modifier` 是**不可变值**（`.then()` 除外）：除 `.then()` 外每个方法返回新副本，可链式组合并赋值给任意组件的 `.modifier` 属性。

```cpp
auto save_btn = au::Button(au::ButtonProps{ .label = "保存" });
save_btn.modifier = au::Modifier{}
    .padding(12)                      // 内边距（float 或 EdgeInsets）
    .background(au::colors::AURORA_BLUE)   // 背景填充
    .clickable([&]{ save(); })             // 点击回调
    .border(1, au::colors::AURORA_GRAY)    // 边框（线宽, 颜色）
    .align(au::Alignment::Center)
    .fill_max_width();
```

**链序：先压入者靠外**。`Widget::layout` 以 `nodes()` 的逆序包裹测量，故 `Modifier{}.a().b()` 里 a 是 b 的**外层**。 「固定尺寸 + 在父容器内居中」须写成 `.align(Center).size(120,40)`；反过来写 `.size(120,40).align(Center)` 时 Align 只定位色块**内部**的内容，色块本身仍贴父容器起始边。

### 7.2 工厂清单

除 `.then()` 外全部返回 `Modifier`，可链式（`.then()` 为就地可变追加并返回 `Modifier&`）：

| 工厂 | 作用 |
|:---|:---|
| `.padding(float)` / `.padding(EdgeInsets)` | 内边距 |
| `.background(Color, float radius = 0)` | 背景填充（可选圆角） |
| `.clickable(std::function<void()>)` | 点击回调（命中即消费事件） |
| `.opacity(float a)` | 透明度（`a ∈ [0,1]`，复用 `Painter::set_alpha`） |
| `.size(w, h)` / `.width(float)` / `.height(float)` | 固定尺寸。`Length` 强类型宽度走 `Widget::width(Length)` |
| `.fill_max_width()` / `.fill_max_height()` / `.fill_max_size()` | 撑满父约束 |
| `.border(float, Color)` | 边框 |
| `.clip()` / `.clip_rounded(float)` | 矩形 / 圆角裁剪 |
| `.align(Alignment)` | 在父级让出的额外空间内**逐轴**定位子项（对标 Flutter `Align`/`RenderPositionedBox`）：某轴上限是父级既定槽位时该轴展开到上限并按 `Alignment` 摆放子盒；某轴只是「按需剩余空间」（Flex 主轴给非加权子项，`Constraints::loose_width/loose_height` 为真）或无界时该轴退化为内容尺寸——否则居中会吞掉同轴兄弟的空间。命中盒随子盒一同收缩（见 §7.4） |
| `.offset(float dx, float dy)` | 绘制期平移（Transform 切片） |
| `.rotate(float degrees)` | 绕内容中心旋转（Transform 切片，仿射矩阵） |
| `.scale(float sx, float sy)` / `.scale(float s)` | 绕内容中心缩放（Transform 切片） |
| `.transform(Matrix2D)` | 应用任意 2×3 仿射矩阵（Transform 切片，关于原点；绕内容中心请用 rotate/scale 或自行构造 from_*_about） |
| `.expand(float weight = 1)` | 在 Flex 主轴占权重（产生 `FlexWeight` 节点） |
| `.gradient_linear(from, to, angle_deg = 0)` / `.gradient_linear(colors, stops, angle_deg)` / `.gradient_radial(center, edge)` | 线性（双色/多色标）/ 径向渐变背景 |
| `.shadow(offset_x = 0, offset_y = 2, blur = 4, color = 黑色 25%)` | 投影阴影（绘制于内容之下） |
| `.blur(float radius)` | 内容模糊：子树绘制完成后对整个内容盒做高斯近似模糊 |
| `.backdrop_filter(float radius)` | 背景滤镜（毛玻璃）：绘制内容前先模糊内容盒背后已绘像素，配合半透明 `background` |
| `.blend_mode(...)` / `.shader_mask(...)` / `.cache_layer()` | 像素混合 / 渐变遮罩 / 离屏缓存（`BlendMode` / `ShaderMaskKind` 枚举见 `render/blend.h`；GPU 路径经常驻层纹理缓存，epoch 键控失效，见 `render/detail/gpu_layer.h`） |
| `.draggable(...)` / `.long_press(...)` | 手势（单指，由 `Draggable` / `LongPress` 修饰节点驱动） |
| `.touch(on_touch)` | 原始多点触摸流回调（`TouchListener` 节点；当前实现中 Input 类节点一经命中即返回自身，拦截向子节点下探） |
| `.tooltip(std::string, float delay_ms = 500)` / `.context_menu(std::vector<MenuItem>)` | 悬停提示气泡 / 右键上下文菜单 |
| `.cursor(CursorShape)` | 悬停光标形状（Input 切片，`CursorNode`） |
| `.then(N node)` | 就地追加任意 `ModifierNode`（**可变**，直接 `push_back` 到 `*this` 并返回 `Modifier&`） |

### 7.3 四类切片与绘制顺序

修饰节点分为四类切片，按固定顺序作用于布局 / 绘制 / 输入：

| 切片 | 成员 | 头文件 |
|:---|:---|:---|
| `Layout` | `Padding` / `PaddingEdges` / `FlexWeight`（`.expand()` 产生的节点，无独立 `Expand` 节点）/ `SizeModifier` | `modifier/modifier_layout.h` |
| `Paint` | `Background` / `GradientBackground` / `ShadowNode` / `BlendNode` / `ShaderMaskNode` / `CacheLayerNode` / `Border` / `Clip` / `ClipRounded` / `OpacityNode` / `BlurNode`（`.blur()` 与 `.backdrop_filter()` 共用，按标志位区分） | `modifier/modifier_paint.h` |
| `Input` | `Clickable` / `Draggable` / `LongPress` / `TouchListener` / `TooltipNode` / `ContextMenuNode` / `CursorNode` | `modifier/modifier_input.h` |
| `Transform` | `AlignNode` / `OffsetNode` / `TransformNode`（`.rotate()` / `.scale()` / `.transform()` 共用，按构造参数区分） | `modifier/modifier_transform.h` |

`Transform` 切片累积为单个 `Matrix2D`（绕内容中心构造），绘制时累积的 `Matrix2D` 非恒等才走离屏合成（恒等走零离屏快速路径直接 paint_content）、命中测试用逆矩阵映射本地坐标；`Opacity` 复用 `Painter::set_alpha`。

### 7.4 三段式修饰绘制

`Widget::paint` 按以下顺序处理修饰：

1. **先绘阴影 / 背景毛玻璃（backdrop）**——须在圆角裁剪之前，以免外扩羽化被切。
2. **再压栈裁剪**（Clip / ClipRounded，与既有裁剪栈取交集）。
3. **随后绘背景 / 渐变背景**——因圆角裁剪已生效，背景随 `clip_rounded` 呈圆角（对齐 Flutter `ClipRRect` 语义：圆角裁剪作用于控件绘制的一切内容，含自身背景）。

**作用范围**：Paint 切片（背景、边框、阴影、裁剪、后效）的作用盒由 `Modifier::paint_boxes(visual_box)` **逐节点**给出——链上没有改变盒形的 Transform 节点时，各 Paint 节点统一作用于完整视觉盒 `visual_box`（历史行为，逐位一致）；含外侧 `AlignNode` / `OffsetNode` 时，其**内侧**的 Paint 节点依次收缩到对齐后的子盒、平移到偏移后的盒。`Padding` / `PaddingEdges` **不**参与收缩：背景、边框、裁剪连内边距一起覆盖。子节点 `on_paint` 与内容后效则限定在 `content_box`（已扣除 Padding / Align 等布局内边距）。内容后弹栈并绘制边框。

**命中盒与绘制盒同源**：`Widget::hit_test` / `hit_test_chain` 以 `Modifier::TransformInfo::hit_size` 判定自身盒，该尺寸**只**被 `AlignNode` 收缩（Padding 不收缩，与上面的内边距豁免同源）；链上无 Align 时 `hit_size` 等于布局盒，行为与历史逐位一致。于是外侧 Align 展开出的那段空间既不承载绘制，也不接收点击。

> 三种历史错误形态：把背景先于裁剪当作直角矩形填色；把 Paint 修饰限制在 `content_box` 导致 padding 区域露白；外侧 Align 展开后 Paint/命中仍按整盒走，导致居中的小色块由 492dp 宽的展开行「顺带」涂满并吞掉展开区的点击。圆角裁剪与背景修饰项的语义由 `tests/unit/utest_modifier_paint.cpp` 覆盖（含 `clip_rounded` 与背景半径 / 颜色）；分段绘制盒与同源命中盒由 `tests/unit/utest_modifier.cpp`（`paint_boxes_*`）、`tests/unit/utest_modifier_transform.cpp`、`tests/unit/utest_flex_layouter.cpp`（主轴供给打标）与 `tests/integration/itest_widget_components.cpp`（`align_in_column_centers_*` / `align_hit_box_shrinks_*`）覆盖。

### 7.5 与固有属性的关系

控件可配置性由两层构成：固有属性（`XxxProps` 字段）描述身份并随控件序列化；`Modifier` 承载跨切面装饰。重叠能力以**固有属性优先**，`Modifier` 同类项保留用于「给任意控件套一层」的场景。绘制时 `Modifier` 在外、固有属性在内。

---

## 8 需求规格

### 8.1 SPEC.FEAT.TOOLING.API-SCHEMA.001 机器可读 API Schema

**控件自描述侧的契约**：各控件提供**静态** `describe_static()`；虚 `describe()` 在基类 `Widget` 已有默认实现（返回 `{ .name = type_name() }`，`widget.h`），无需富描述的控件可省略 override，仅在需要补充 properties / events / `children_policy` 等元数据时覆写（与 [`04-widget.md`](04-widget.md) §2.1 的表述一致）。`component_schema()` / `list_all_schemas()` 消费 `describe()` 输出；`aurora_api.json` 自动包含增强字段。

```cpp
auto info = au::Button::describe_static();
// WidgetDescriptor{
//   .name = "Button", .ns = "aurora",
//   .properties = {
//     {"label", "LocalizedString", "\"\"", true, "按钮文字"},
//     {"color", "Color", "Color::blue()", false, "背景色"}, ...
//   },
//   .events = {"on_click"},
//   .children_policy = "none",
//   .examples = {"au::Button(au::ButtonProps{ .label = \"OK\" })"}
// }

au::Button btn;
au::Widget &w = btn;
auto desc = w.describe();      // 运行时多态调用
auto all = au::serialization::list_all_schemas();
```

这使得 AI Agent 可以在**运行时**动态发现 Aurora API，而不完全依赖训练数据。

> Schema 文件结构、生成器与工具链消费方式见 [`08-tooling.md`](08-tooling.md)；属性元数据 `PropDescriptor` 定义见 [`04-widget.md`](04-widget.md) §2.1。
