# 环境注入、主题与修饰符（environment / theming / i18n / modifier）

> 覆盖 `include/aurora/environment/`（3 个头）、`theming/`（15 个头）、`i18n/`（8 个头）、`modifier/`（6 个头）。
> 本文件是环境依赖注入、媒体查询、主题与国际化、以及 `Modifier` 修饰系统的**唯一权威**。
> 响应式属性见 [`02-state.md`](02-state.md) §2；控件的二层属性划分见 [`03-layout-render.md`](03-layout-render.md) §7.4。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 环境与构建上下文 | `environment/environment.h`、`environment/build_context.h` |
| 媒体查询 | `environment/media_query.h` |
| 主题 | `theming/theme.h`（聚合）、`theming/color_roles.h`、`theming/type_scale.h`、`theming/csd_roles.h`、`theming/csd_style_bridge.h`、`theming/theme_scope.h`、`theming/theme_query.h`、`theming/theme_extension.h`、`theming/color_scheme.h` |
| 国际化 | `i18n/message_id.h`、`i18n/locale.h`、`i18n/localized_string.h`、`i18n/message_catalog.h`、`i18n/plural.h`、`i18n/format.h`、`i18n/builtin_messages.h`（+ 生成件 `i18n/builtin_messages.gen.h`） |
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

构造 `Provider<T>` 用**位置式**而非指定初始化器（`Column` / `Row` 非聚合；子节点走初始化列表或构造首参，属性走链式 `*Props`，见 [`04-widget.md`](04-widget.md) §2.5）。

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
| `orientation` | 由 `size`（未接屏幕的后端）/ `screen_size`（Win32、macOS）派生：宽 ≥ 高判为 `Landscape`，正方形归入 `Landscape`。枚举 `ScreenOrientation{Portrait, Landscape}`，与 `divider.h` 的 `Orientation` 语义不同，**不复用** |
| `screen_size` | 物理屏幕的逻辑尺寸（dp）：Win32 取 `GetSystemMetrics(SM_CXSCREEN) ÷ scale`，macOS 取主显示器 `Display::bounds`（物理像素）÷ 本 Surface 的 dp 比；其余后端退化为 Surface 尺寸（拿窗口尺寸冒充屏幕尺寸，改窗口形状会连带改 `orientation` 判定） |
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
| `MediaQuery::from_surface(const Surface&)` | 跨平台成型：把 `s.content_inset()` 并入 `padding`；Win32 取真实屏幕逻辑尺寸与减弱动效；macOS 取真实主显示器尺寸与由其派生的 `orientation`（`platform` / `device` / `prefer_reduced_motion` 在 macOS 仍未接线，落结构体默认值——三者与 OS 无障碍偏好的接线口径跨后端一致，待变更提案）；其余后端取 Surface 尺寸与默认值 |
| `MediaQuery::of(const BuildContext&)` | 读最近祖先 Provider，无则返回进程级默认实例 |

**读取 API**（`MediaQueryProvider` 是 `using` 别名，无法加静态 `of`，故提供自由函数）：

- `media_query_of(const BuildContext&) -> const MediaQuery*`：向上查找最近注入的 `MediaQuery`；无 Provider 时返回 `nullptr`，调用方按需降级。
- `MediaQuery::of(const BuildContext&) -> const MediaQuery&`：便捷封装，无 Provider 时静默返回进程级默认实例。

### 3.2 自动注入

`Window::present_root` 每帧自动以 `MediaQuery::from_surface(*surface)` 在**根 `BuildContext`** 注入 `MediaQuery`（存入稳定的 `Window::root_env_`，地址恒定，避免子树 Provider 持悬空父指针）。

因此**无需手动包裹 `MediaQueryProvider`**，整棵树（含根 widget 自身）即可经 `media_query_of(ctx)` / `MediaQuery::of(ctx)` 读取设备上下文。`root_env_` 每帧原地更新注入值（对象本身不重建）以反映窗口 resize。`Application::run` 经同一 `present_root` 自动受益。手动 `MediaQueryProvider` 仍按「最近祖先优先」覆盖此默认值。

### 3.3 LayoutBuilder

`LayoutBuilder`（`widget/layout_builder.h`）是响应式构建原语。`BuilderFn = std::function<Node(const BuildContext&, const Constraints&)>` 以只读句柄 `ReadOnlyState<BuilderFn>` 安装（`set_builder(fn)` 便捷装箱 / `set_builder_source` 换源），布局阶段按 `constraints` 动态构建子节点。

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

**四后端映射**（策略解析纯函数 `csd::resolve_mode(policy, ssd_available)`，`src/aurora/window/detail/csd_decoration.h`；「有无 SSD」各后端构造期一次判定。macOS 不走该纯函数——AppKit 恒有服务端装饰，无协商环节）：

| 后端 | Auto / ServerSide | ClientSide | Borderless | Frameless |
|:---|:---|:---|:---|:---|
| Wayland | 有 `xdg-decoration` → SSD；无 → CSD（title+margin） | CSD（title+margin，即便合成器支持 SSD） | 仅 margin 带 | 无任何装饰 |
| X11 | 保留 WM 装饰（X11 的 SSD 即 WM 原生口沿，无须协商） | `_MOTIF_WM_HINTS` 撤 WM 口沿后自绘标题栏 + 内缘八向热区 | 仅内缘热区 | 无任何装饰 |
| Win32 | 保留 `DWM`/系统非客户区装饰 | 去 `WS_CAPTION`（保留 `WS_THICKFRAME` 系统缩放模态）+ `WM_NCCALCSIZE`（wParam=TRUE）吞非客户区使客户区 = 整窗，最大化把客户区拉回显示器工作区（经典 −8dp 边框问题修正）；`WM_NCHITTEST` 回报热区：内缘八向（复用 `csd::classify_resize_zone`，带宽 = `frame_margin_dp(frame, dwm_capable)`）→ `HTLEFT`/…、标题带空白 → `HTCAPTION`（系统白给拖拽/双击最大化/Aero Snap）、按钮 → `HTCLIENT`，修饰键（Alt/Meta）拖拽兜底 → `HTCAPTION`；标题栏自绘经 `paint_decoration` 于三表面 present 路径覆绘，按钮按压/释放结算与 X11/Wayland 共层；建窗 / `set_size` 的窗口矩形在吞框档**不做** `AdjustWindowRect` 非客户区外扩（窗口矩形 = 请求物理客户区，`DWMWA_EXTENDED_FRAME_BOUNDS` 实测 == 窗口矩形 ⇒ 整窗可见；外扩会让吞框后的客户区多出不可见边框带宽，布局视口与真实客户区错位——底部/右侧纯背景带） | 无标题栏，热区与修饰键拖拽同左（`csd_mod_move` 激活） | `WS_POPUP`，无任何装饰 |
| macOS | `Titled\|Closable\|Miniaturizable`（`resizable` 时再加 `Resizable`）＝原生标题栏 | **退化为 SSD**（macOS 无「撤掉服务端装饰再自绘」的原生对应口，也不接 CSD / margin 层；顶边内缩实测 32pt） | `Titled\|Closable\|Miniaturizable\|FullSizeContentView`（`resizable` 时再加 `Resizable`）配标题带透明 + 标题文本隐藏 + 三枚标准窗钮逐枚隐藏 ⇒ 看不到标题栏而**内容区 == 窗框**（顶边内缩实测 0），边缘缩放由原生窗框视图承担，**不经 `begin_window_resize`** | `NSWindowStyleMaskBorderless`（实测无缩放能力、边缘拖拽不改尺）；`WindowStyleOptions::frameless` 与本档同路 |

**macOS 一列的两条实测例外**（取证载体见 `tools/verify/macos_surface_live_probe.mm` 的装饰档位段）：其一，该档的缩放由 `NSWindow` 自己承担——建窗后向窗框右下角投递一次拖拽（AppKit 事件队列腿），300×200 确实被拖到 396×296，且新尺寸由 `Surface::size()` 同步读回。故 macOS **不设** `begin_window_resize` 覆写：Win32 的 `WM_NCLBUTTONDOWN + HT*` 与 Wayland 的 `xdg_toplevel.resize` 是「自绘边框的按下必须转交 OS 接管整段拖拽」的桥，而 AppKit 的窗框视图本就在做同一件事，补第二份实现只会让同一次拖拽有两个结局。其二，投递拖拽成立的前提是**窗口在序、事件队列已泵空、且起笔前先向窗框几何中心补投一次按下—抬起作 priming**；缺第三项时该腿实测**偶发**不发生改尺（同一份探针连跑三轮有两轮尺寸纹丝不动）。焦点态不是前提也不是判据：后台图形会话里 `isKeyWindow` 恒为 0 而改尺照常发生，同一条观测行也读到过 `key=1` 的两读皆绿，故 `key=` / `primed=` 只作观测。priming 的机制未取证，与实测最不矛盾的解释是 AppKit「点击获焦」的那一次性代价会吃掉非 key 窗口欠着的第一个 `mouseDown:`（与 OS 输入探针 TC-WINDOW-024 的 priming 同族语义），priming 把这笔代价提前吃掉、拖拽起笔才落到窗框视图。

**控件侧消费现状**：`WindowChrome::begin_resize` / `Surface::begin_window_resize` 目前**无控件调用方**（`TitleBar` 只消费 `begin_move`，控件层没有边缘命中测试），X11 连 `begin_window_move` 亦未覆写（落基类空实现）。CSD 缩放整链是跨后端缺口、不属 macOS；macOS 侧由上段的原生窗框路径覆盖同一需求，故该缺口在 macOS 上无产品可见后果。

**安全区**：CSD 标题栏 / 阴影边距占用区经 `Surface::content_inset()` 暴露，由 `present_root` 的下沉壳（`ContentInsetRoot`）自动消费（见 §3.1「客户端自绘装饰的安全区」）；根注入 `MediaQuery.padding` 恒为内容区口径（已消费 ⇒ 归零）。

Wayland CSD 的**阴影边距模型**（单表面，libadwaita 风格；纯逻辑收敛在 `src/aurora/window/detail/csd_geometry.h`）：wl_surface buffer = 应用内容 + 四周 `AURORA_SHADOW_MARGIN_DP`（10dp）边距，边距承载真 alpha 柔影与 ≥8dp 的八向缩放热区；`xdg_surface.set_window_geometry(m,m,内容宽,内容高)` 声明可视窗口边界，`wl_surface.set_opaque_region` 仅报内容矩形。此模型下 **`WaylandSurface::size()` 返回整幅表面逻辑尺寸（内容 + 2×margin）**，`content_inset()` 语义为「表面原点 → 应用内容原点」，按下表取值（`m` = margin、`tb` = 标题栏高）：

| 状态 | ClientSide / Auto 回退 CSD | Borderless | SSD / Frameless |
|:---|:---|:---|:---|
| Normal / 平铺外常态 | `{m, m+tb, m, m}` | `{m, m, m, m}` | `{0,0,0,0}` |
| Maximized / 任意向 tiled | `{0, tb, 0, 0}`（margin 塌缩） | `{0,0,0,0}` | `{0,0,0,0}` |
| FullScreen | `{0, tb, 0, 0}`（揭示条出现前 tb 亦不绘制） | `{0,0,0,0}` | `{0,0,0,0}` |

margin 在 Maximized / FullScreen / tiled 态塌缩为 0（贴边无可拖缝隙、无阴影），回 Normal 恢复；状态切换走整幅重绘。指针在进入表面的三个入口（enter / motion / button）统一做「表面坐标 → 内容坐标（−margin）」映射，热区判定 `csd::classify_resize_zone` 在内容坐标域进行（表驱动纯函数，`utest_csd_geometry` 守门；映射值仅供后端内部 CSD 判定消费）。对应用上抛的事件（悬停/按压/滚轮）则按契约转发**整幅表面坐标原值**——下沉折算统一由派发根壳承担（见 §3.1），后端不做第二份平移。margin 厚度可经 `WindowFrameStyle::shadow_margin_dp` 显式配置（0 = 关闭；显式关闭 / 能力不足 / 非正值三路归零，见 `csd::frame_margin_dp`），默认值逐值等于既有 10dp 常量。X11 与 Win32 后端 buffer 即内容矩形（不假设 ARGB 视觉与合成器），无外延 margin 分量——外延阴影自动降级为 0，标题栏带为**内缘覆绘** + **内缘热区**（同一 `csd::classify_resize_zone` 判定式按「内容坐标缩进 margin 后的外延环」口径复用，不写第二份八向逻辑），`content_inset()` 只承载标题栏带高度；Win32 自绘档标题栏带宽 `frame_margin_dp(frame, dwm_capable)`（DWM 合成关闭时归零，探针 `aurora_verify_win32_csd` 锁降级语义）。Headless 维持零 inset 口径。

**程序化窗口控制**（`Surface` / `Window` 虚函数，默认空实现）：`close()`、`minimize()`、`toggle_maximize()`、`set_fullscreen(bool)`，另有目标态原语 `set_window_mode(WindowMode)`（见 [`06-app-platform.md`](06-app-platform.md)）。后端接线现状：Wayland 经 `xdg_toplevel` 协议生效；**Win32 / D3D11 四槽已接**（同一 `Win32Host` 宿主的 `SC_*` 目标态，`toggle_maximize` 以 OS 实值 `IsZoomed` 判档；`set_fullscreen(true)` 留唯一 WARN——全屏档为「可查询的不支持」，能力位 `runtime_window_mode` / `runtime_fullscreen` 见 [`06-app-platform.md`](06-app-platform.md) §5）；X11 / GLFW 的 `set_window_mode` 槽已接但三枚切换原语未覆写（能力位如实 false，应用先查询再决定画不画窗控钮），使无标题栏 / 无边框窗口也能由应用按钮驱动状态。

### 4.2 TitleBar

**`TitleBarStyle`**（`window/title_bar_style.h`）是 CSD 标题栏样式值类型，字段：`height`（默认 36）、`bg_active`、`bg_inactive`、`fg_active`、`fg_inactive`、`hover_tint`、`close_hover`、`button_layout`（`TitleBarButtonLayout{Adwaita, Windows, Mac}`，默认 `Adwaita`）、`show_minimize`、`show_maximize`、`show_close`、`show_title`、`center_title`，以及三类自定义项：`metrics`（`TitleBarMetrics`：按钮盒尺寸 / 间距 / 贴边外边距 / 图标槽 / 内容内边距 / 标题字号字重，各分量 **0 = 沿用视觉语言的栏高联动推导式**，非 0 才是显式覆写）、`corner_radius`（标题栏背景**下角**圆角，0 = 直角）、`pressed_tint` / `close_pressed`（按压态配色，绘制优先级「按压 > 悬停」）。

**预设**：`CsdPreset`（四套视觉语言 × 明暗两档 = `AdwaitaLight/Dark`、`WindowsLight/Dark`、`MacLight/Dark`、`BreezeLight/Dark` 八值）+ 工厂 `TitleBarStyle::from_preset(CsdPreset)`；`TitleBarStyle{}` 的默认成员值即 `AdwaitaDark`。旧三个静态预设函数（`adwaita_dark()` / `adwaita_light()` / `windows_dark()`）已删除，迁移路径见 `CHANGELOG.md` 的 Migration 节。各预设显隐开关全开、按压 alpha 不低于悬停；macOS 红绿灯圆形底色由绘制层按布局语言固定，预设不覆盖。

**度量口径四档分治**（`metrics` 的「0 = 沿用栏高联动推导式」仍成立，但四套语言的取值策略不同）：

| 视觉语言 | 栏高 | 度量取法 | 出处 |
|:---|:---|:---|:---|
| Adwaita | 36dp | **不钉** `metrics`（`TitleBarStyle{}` 默认成员值即 `AdwaitaDark` 的契约，钉值会让两者分叉）；按钮盒推导式 `clamp(h−6,26,34)` 在栏高 36 时给 **30dp**、间距 **3dp**、贴边外边距 **6dp** | libadwaita `.default-decoration`：`windowcontrols > button { min-width: 22px; padding: 4px }` → 盒 30 = 22 内容 + 4 外边距；间距出自 `windowcontrols { border-spacing: 3px }`；`headerbar` 另有 `padding: 0 6px` |
| Windows | 32dp | **显式钉值**：按钮盒 46dp、图标槽 16dp（栏高 32 时「`min(16, h−20)`」会退化成 12，故必须钉住）、标题 9pt / 400、悬停 / 按压遮罩 α≈0.0605 / ≈0.1 | Win11 DWM 真机实测（WinUI 标准遮罩口径） |
| Mac | 28dp | **显式钉值**：红绿灯直径 12dp、间距 8dp（相邻圆心距 20dp）、贴边外边距 14dp（首枚圆心距左缘 20dp）、图标槽 16dp、标题 13pt / 400 且居中 | macOS 标准窗口口径；旧推导式在栏高 36 下给直径 14 / 外边距 8，差 17% / 43% |
| Breeze | 36dp（未钉） | **只钉标题 10pt / 400，几何全不钉**：按钮语言沿用 `Adwaita` 档的右缘方形盒与栏高联动推导式 | 配色取 KDE/breeze 仓库 `colors/Breeze{Light,Dark}.colors` 的 `[Colors:Window]` 四支、标题字号取 plasma-integration 的 WM 字体 10pt；KWin 侧栏高是运行时推导（`max(文字高, gridUnit×2) + smallSpacing×4`），上游不给独立常量，钉死反而偏离真机 |

`TitleBarMetrics::title_font_weight` 默认 500 → **700**（libadwaita `.title` 为 bold），Windows / Mac / Breeze 三档（六预设）各自钉 400。

**字形与按钮盒宽解耦的不变量**（四套语言统一）：字形一律由绘制层按固定设计网格居中于按钮盒绘制，按钮盒再宽字形也不放大——Windows 10×10 epx（Segoe Fluent Icons）/ 线宽 1dp、Adwaita 16×16 symbolic / 1.5dp、macOS 6pt（12pt 圆内占一半）/ 1dp，常量见 `title_bar_painter.cpp` 的 `AURORA_{WIN,ADWAITA,MAC}_GLYPH_HALF_DP` / `..._STROKE_DP`；旧实现统一按「盒宽 / 3.0、`盒宽 / 3.6`」推导，字形随盒放大（Windows 档实测放大到 34dp，是挤压感根因）。Windows 关闭钮还原字形重绘为「8×8 主框 + 右上错位副框的顶边与右边」。Breeze 两档的按钮布局语言即 `Adwaita`（右缘方形盒、close→max→min），故字形走同一套 16×16 网格常量，绘制层无第四分支。

**关闭钮字形取色规则**（四套语言统一）：常态随前景色（亮色档为黑），仅当底色被悬停 / 按压的特征红替换时转白——红底上深色符号不可读、白底上白色符号不可见，是同一判据的两侧（修复 `AdwaitaLight`（`#EBEBEB`）/ `WindowsLight`（`#F3F3F3`）下关闭钮画不出来的功能性缺陷）。Adwaita 悬停 / 按压的圆形底与按压态反馈（优先级「按压 > 悬停」）同批补齐；Breeze 两档无红色关闭档（`close_hover` / `close_pressed` 全透明），关闭钮悬停与其余按钮同走强调色圆底。

**标题对齐**（水平与垂直各自成规，均由绘制层落地）：
- **水平**：`center_title=true` 时几何层给**整宽带**（不扣按钮组），绘制层按 `FontEngine::measure_width` 实测量宽把文字摆到整宽带的中点。默认成员值为 **true**（默认档即 Adwaita：libadwaita `AdwHeaderBar` 的标题居中），macOS 与 Breeze 四档同为 true（KWin Breeze 的默认标题对齐即居中），**Windows 两档显式置 false**（WinUI 标题左对齐）。此前该开关是「文档承诺、绘制层未实现」——置真只放开带宽，文字仍左对齐。
- **垂直**：四套语言一律居中，与 `center_title` 无关。绘制层按 `FontEngine::measure_height` 把字体行盒摆到栏高的中点再调 `draw_text`——`draw_text` 的 y 是**行盒顶**（基线 = 顶 + ascent），把标题带原样传下去会把文字顶到栏的上缘（实测墨迹中心比栏高中心偏上：Adwaita 6dp / Windows 8.5dp / Mac 1.5dp）。行盒含下伸部，纯大写标题的视觉中心因此比几何中心低 1~2dp，属字体度量固有偏差，不另加补偿常数。

挂载于 `WindowStyleOptions::title_bar`，运行期经 `Surface::set_title_bar_style` 热更。

**窗框样式**：`WindowFrameStyle`（`window/window_frame_style.h`，挂载于 `WindowStyleOptions::frame`，热更口 `Surface::set_window_frame_style`）：`shadow_enabled` / `shadow_margin_dp`（投影带厚度 = 缩放热区宽度）/ `shadow_color` / `shadow_blur_radius`（0 = 由 margin−1 推导，显式值夹到 margin 内）/ `corner_radius`（外轮廓圆角，0 = 直角）。默认值逐值等于既有 `AURORA_SHADOW_*` 常量，未配置零视觉变化；投影厚度 / 基色 / 模糊两条上屏路径（软件 `compose_shadow_margins_bgra` 与 GPU `draw_shadow`）同源消费折算后的值。X11 无外延 margin，厚度只作用于内缘热区宽度。

**共享装饰层**：策略解析、按钮命中、按压结算（**动作在释放触发**——按下即触发会使按压色永不可见，且失去「按下后拖开即取消」的纠错语义，对齐 GTK/Win32）、全屏揭示条状态机、连击记账与绘制装配收敛在库内私有头 `src/aurora/window/detail/csd_decoration.h`（纯值、无平台头，`utest_csd_decoration` 以策略 × SSD 有无 × 窗口几何态的快照表逐格锁语义）；各后端只做平台事件映射。

**几何单一来源**：纯函数 `title_bar_geometry(width, style, maximized, resizable)` 返回各按钮 / 图标 / 标题矩形（隐藏 = 空盒）；绘制与命中测试共用，规则以其实现注释为唯一权威。

**绘制单一来源**：`csd::paint_title_bar(Painter&, const TitleBarPaintState&)`（`src/aurora/window/detail/title_bar_painter.h`，库内私有）把 `title_bar_geometry` 的矩形集光栅为一次 `Painter` 调用（背景 + 图标 + 标题 + 三套视觉语言的按钮，hover/激活态入状态结构）。软件路径（`WaylandSurface::Impl::draw_decoration`）与 GPU 路径（`WaylandSurface::record_client_decoration` → 帧 `Sink::end_frame` 回放进 swapchain 帧，见 [`03-layout-render.md`](03-layout-render.md) §8.8）同为它的消费者，故两条上屏路径的装饰逐位一致（`utest_title_bar_painter` 以「直绘 vs 录制回放」全画布字节比对守门）；无装饰可绘（SSD 合成器 / `Frameless`）时录制侧返回 false、当帧不含装饰命令。

**窗口阴影 underlay**：`csd::paint_window_shadow`（同头文件）以内容矩形为形状、零偏移、模糊半径 `margin−AURORA_SHADOW_BLUR_INSET_PX` 录制 `Painter::draw_shadow`，仅作 GPU/DisplayList 路径的阴影源（`record_client_decoration_underlay` → `Sink::begin_frame` 先于 app 帧回放，Shadow 管线写真 alpha 到透明清除的 swapchain）。软件 wl_shm 路径不经 Painter（Painter 目标画布恒不透明，混合恒写 alpha=255，画不出半透明柔影）：`present()` 在 RGBA→BGRA 预乘 swizzle 之后调 `csd::compose_shadow_margins_bgra`（`src/aurora/window/detail/csd_shadow_compose.h`）直接覆写 margin 环的 BGRA 字——内容矩形跳过、角部按两向欧氏距离衰减、外缘写透明字 0 且幂等。视觉参数两条路径同源于 `csd_geometry.h`（`AURORA_SHADOW_BASE_COLOR{0,0,0,70}` / `AURORA_SHADOW_BLUR_INSET_PX` / `shadow_attenuation`），由 `utest_csd_shadow_compose` 对 BGRA 字逐值守门。

**`Surface` 相关虚函数**：`set_title_bar_icon(std::shared_ptr<Image>)`（图标槽）、`begin_window_move()` / `begin_window_resize(WindowResizeEdge)`（控件发起拖拽 / 缩放——**须在指针按下事件派发栈内同步调用**，受 Wayland `xdg` move / resize 的 serial 时效约束）。覆写分布：`begin_window_move` 落 Win32 两型与 Wayland 两型加 macOS（`performWindowDragWithEvent:`）；`begin_window_resize` 只落 Win32 两型与 Wayland 两型——macOS 的边缘缩放由原生窗框视图承担（见 §4.1 该档的实测例外），X11 两者皆未覆写、落基类空实现。

**`WindowChrome` 服务**（`window/window_chrome.h`）：经 Environment 注入的窗口动作门面（`close` / `minimize` / `toggle_maximize` / `set_fullscreen` / `begin_move` / `begin_resize` / `content_inset`），由 `present_root` 注入根环境供控件消费（headless 安全 no-op）。

**`TitleBar` 控件**（`widget/title_bar.h`）：声明式标题栏，支持 icon 位图、title / subtitle、`add_action` 文本 chips、内置窗口控制钮与 Snap 弹窗；空白区拖拽、双击最大化、失焦变暗，支持 `describe_static` 与序列化往返（自定义 Snap 动作除外）。

---

## 5 主题

### 5.1 主题值对象与角色集

`Theme`（`theming/theme.h`）是角色化主题值对象，结构为 `Theme{ style, roles, type_scale, csd, shapes, spacing, controls, motion, conventions, scheme, extensions }`——身份位（`style` / `scheme`）与四组色/字/度量令牌、一组动效时长档加一张 `ThemeExtension<T>` 注册表。度量令牌各组不分明暗，故 `light()` / `dark()` 在 `shapes` / `spacing` / `controls` / `motion` 上逐字段同值。`Theme` 及各角色组逐字段可判等（`operator==`，`ThemeExtensions` 一侧按类型键逐条判等，见下）。`Theme::light()` / `Theme::dark()` 是两个内置档位，`with_defaults()` 返回 light 基准列（`scheme` 保持 `Unspecified`），二者与 `from_style(Material, ·)` 逐值相同。经 `ThemeProvider` / `ThemeScope` 注入，组件用 `inherit_theme(ctx)` 读取。

**读取面按引用交付**：`inherit_theme(ctx)` 的返回类型是 `const Theme &`，指向注入存储本身而非副本。理由是量出来的：`Theme` 现 984 字节，按值返回的每次读取都是一次整值拷贝；拷贝是否带堆分配取决于字体族名档位——`TypeScale` 的 15 枚 `Font` 各含一枚 `std::string` family，族名落在小串优化容量内时整值拷贝**零分配**（每读只余整值搬运），一旦越过该容量即每枚 Font 各一次分配。基准 `tools/bench/bench_theme.cpp` 在 984 档七次复跑（分配计数逐位相同）：注入越界族名档每次读取 15 次分配、一帧 +552 KB、每读增量 418–436 ns，稳态帧成本是按引用档的 **2.20–2.37 倍**；换肤一帧的分配数 12296 对 296。**「基线族名在容量内」不等于「缺省配置零分配」**——风格轴落地后 `Window` 每帧把本机风格档注入根环境，Windows 档的族名 22 字节已在容量之外（libstdc++ 的 `std::string` 小串容量为 15 字节），故什么都不设的应用实测就是越界档：基准里那一行 scene 因此叫 `win_root_platform_family`，15.00 次分配/读、一帧 +69 KB，稳态帧成本为按引用档的 **1.63–1.68 倍**（旧口径「默认族名在容量内、缺省配置零分配，稳态帧 2.25–2.36 倍」是合流前 912 档读数，已被上面这组取代）。「未显式设色即取主题角色」的读取点实测 51 处，这笔开销按**读取次数**而非控件数增长。返回引用的**有效期是本次布局 / 绘制遍历**——`Provider` 每次进入本层都按当前值重建子环境（旧存储随之析构），故控件不得跨帧持有该引用，要留存须显式拷贝。无任何注入时返回库内**单一**的明色基线快照（`theming/theme_scope.h` 里一枚函数内 `static const Theme`，进程内地址恒定）＝ `Theme::light()`；`theming/theme_query.h` 的 `resolve_theme` 兜底与它合一。旧偏差是两条通路各兜各的（`light()` 与 `with_defaults()`，差在 `scheme` 位），合一取 `light()`：本主题的 `roles` / `csd` 已确定为明色列，再标 `Unspecified`（＝不表态、交给优先链下层）是自我矛盾的。

**色角色层 `ColorRoles`**（`theming/color_roles.h`，纯值 struct，snake_case 字段名一套、明暗两列取值，命名对标 Material 3 `ColorScheme` 全集）：

- 主色四组：`primary` / `on_primary` / `primary_container` / `on_primary_container`，`secondary` / `on_secondary` / `secondary_container` / `on_secondary_container`，`tertiary` / `on_tertiary` / `tertiary_container` / `on_tertiary_container`，`error` / `on_error` / `error_container` / `on_error_container`
- 表面族：`surface` / `surface_dim` / `surface_bright` / `surface_container_lowest` / `surface_container_low` / `surface_container` / `surface_container_high` / `surface_container_highest` / `on_surface` / `on_surface_variant` / `surface_variant`
- 描边族：`outline` / `outline_variant`
- 反转族：`inverse_surface` / `inverse_on_surface` / `inverse_primary`
- 其他：`scrim` / `shadow` / `surface_tint`
- Aurora 扩展角色（Material 3 无对应）：`focus_ring`（焦点环极性色，light 纯黑 / dark 纯白）、`hover` / `pressed`（交互态配色的收纳位）
- 取舍：Material 3 的 `background` / `on_background` 折叠进 `surface` / `on_surface`（M3 新版 `ColorScheme` 亦移除 background，本库跟随）

每个 `on_*` 角色与配对容器角色的对比度按**三档口径**判，而非笼统套 AA（角色基准值即预设取值，dark `primary` 为达标修正后的取值，理由见 `CHANGELOG.md` 破坏性条目）：

- **文本档 ≥ 4.5:1**（WCAG 2.2 SC 1.4.3）：承载可读文字的前景对——十枚 `on_*` 与其配对底色，加同一前景落在库内会绘制文字的每个中性底上（`surface_dim` / `surface_bright` / 五档 `surface_container_*` / 反转族两枚）。
- **非文本档 ≥ 3.0:1**（WCAG 2.2 SC 1.4.11）：靠形状而非文字承载信息的前景对——输入框描边 `outline`、焦点环 `focus_ring`、错误态图形 `error`、交互控件的活跃轨道与滑块 `primary`，各对 `surface`。
- **不适用**：存储值从来不是上屏值的对（`scrim` / `shadow` 由调用点显式给 alpha 合成后才落屏，`hover` / `pressed` 自带半透明叠加，`surface_tint` 垫在状态层之下），以及不承载识别信息的装饰对（`outline_variant` 分隔线）。不适用不是口头免判：每格**逐条登记理由**；**未登记色对默认按前两档判**——判据面里新出现的配对不会因为「没人表过态」而被跳过。

亮度与比值的真值只有一处：`core/color.h` 的 `relative_luminance` / `contrast_ratio`（alpha 不参与：半透明层的对比度只在合成之后有定义；阈值不在函数里——3:1 / 4.5:1 是「哪一类配对」的属性，判档由登记表承担）。取数由 `gen_contrast_tools`（`tools/gen/gen_contrast.cpp`）走 `Theme::from_style(风格档, 明暗档, 无障碍快照)` 逐档实测，列面是五个风格值 × 明暗 × 对比度两档 = **二十列**：normal 列就是真正上屏的那套值，high 列是同一次调用把 `high_contrast` 置位后的读数——即按「开关声称做到的事」取数，不另造一条派生通路。登记表 `theming/detail/contrast_audit.h` 三张表（档位下限 3 行、逐对归档 30 行 = 文本 20 + 非文本 4 + 不适用 6、预设豁免 14 行），派生实现、门禁与单测共读同一张。门禁 `check_theme_contrast` 判十件事并逐条编号（编号即违规行里的 `C0`–`C9`，变异自证按它定位）：`C0` 运行环境（工具缺失 / 非零退出 / 产物非 JSON / 输入文件缺席，一律红而不静默放行）；`C1` 表声明的枚数与实际行数及产物读数三方对号；`C2` 列面是否穷举 `enum class ThemeStyle` × 明暗 × 对比度两档；`C3` 色角色令牌表的 38 枚是否枚枚被工具读出、且枚枚出现在某格的任一侧、配对两端是否是真角色、档位名是否已知——**期望集取自 `color_roles.h` 的 `AURORA_TOKENS` 而不是产物读数**：按产物建期望时，工具少发一枚角色就等于把该角色从判定面里静默删掉，这条腿因此是唯一可能空转的一腿；`C4` 档位下限是否仍是 WCAG 那两个数与那两条 SC；`C5` 逐格实测比值是否达标；`C6` 每格豁免是否「仍然需要且确有出处」（双向：比值 ≥ 登记下限**且**仍 < 档位下限，后者转红即该格已修好、条目该删）；`C7` 理由是否逐条非空 ASCII；`C8` 本节与表的等集（两枚比值与两条 SC 编号**必须同句在场**、三条判据句与「不适用」角色清单，文档与表任一处漂开即红——只做全节子串搜索会让档位句改成别的数而别处仍写着 4.5，那一类漏判已按变异自证补掉）；`C9` **派生列不享有任何豁免**——高对比那一档逐格按裸档位下限判（开关声称做到的事按它的判据验，不按被豁免过的读数验），同时钉住「派生只是修对比、不是重配色」：某列的原生取值已全部达标时派生列必须逐字节不动，原生取值有未达标格时派生列必须确有改动，且至少有一列发生变化（否则这半条判据空转）。当前读数：480 格参与判定，其中 120 格按「不适用」登记（逐条带理由），十列高对比派生列里六列改动了上屏角色。`utest_theme_roles` 断言的是同一件事的另一侧：它调库内实现、读同一张登记表，不另写一份 WCAG 公式。**十腿变异自证**（每腿一处改动，要求它只把自己那条判红；跑完还原并重跑一次判据）：`C0` 指向不存在的构建目录、`C1` 产物少报一枚审计行、`C2` 工具的风格档清单丢 `Kde`、`C3` 角色循环不发 `tertiary_container`、`C4` 档位下限自改 4.0、`C5` 逐格读数虚报 1.0、`C6` 预设豁免绑到派生列、`C7` 理由置空、`C8` 文本档句改成 4.6、`C9` 派生列不接线——十腿全部按预期转红、还原后回零。首轮自证里 `C3` / `C8` 两腿曾判绿：前者因期望集来自产物（少发即少判），后者因比值的子串在同一节的说明句里也出现（档位句漂开无人报）；两腿的判据按上述口径加固后复跑，各自转红。

**排版角色层 `TypeScale`**（`theming/type_scale.h`）：Material 3 排版 15 档（`display` / `headline` / `title` / `body` / `label` 各 large/medium/small）的 `Font` 字段；图表族经 `compute_geometry` 读取的字阶语义由 `type_scale.body_medium` 承接。

**度量令牌组 `ShapeScale` / `SpacingScale` / `ControlMetrics` / `MotionScale`**（`theming/shape_scale.h` / `spacing_scale.h` / `control_metrics.h` / `motion_scale.h`）与惯例组 `LayoutConventions`（`theming/layout_conventions.h`）：形状 6 档、间距 7 档、控件基准度量 12 项、时长 6 档，加一组枚举惯例；单位一律逻辑 dp（时长为 ms），强类型字段 + 逐字段声明处默认值。度量与配色解耦，与 `TypeScale` 同理**不分明暗**，故各持一列。前三组与惯例组的逐字段取值口径见 [`specification/04-widget.md`](04-widget.md) §4.1 的落点清单（`shapes.small` = 6 dp 即 `Button` 现行圆角、`spacing.large` / `small` = 12 / 6 即 `Button` 现行水平 / 垂直内边距、`controls.indicator_size` = 20 即 `Checkbox` 方框边长、`controls.separator_thickness` = 1 即 `Divider` 现行线厚），初值逐位复现令牌层落地前的硬编码，切风格前观感零变化。`MotionScale` 是本批新增的时长档（`motion.medium` = 150 ms 即滚动视口滑行默认时长、`motion.none` = 0 是「减弱动态效果」开关置位时的短路档），与惯例组的 `motion_scale` 倍率是两轴——倍率只缩放既有曲线，二者相乘消费。

每组结构体在自身头文件就近登记一张「成员指针 → 短名」编译期对照表（`AURORA_TOKENS` 数组 + `token_name(member)` 反查），属性行据此以**成员指针**作活令牌回落点（`.fallback<&ShapeScale::small>()`）：回落点在编译期烧成一次成员指针解引用，**令牌改名即编译失败**，渲染路径不出现字符串查名。表内短名即 `aurora_api.json` 里 `default_expr` / `default_value` 的用名，前缀规则为 `theme.<Theme 字段名>.<档名>`——度量档真路径 `shapes.small` → 短名 `theme.shapes.small`、`controls.indicator_size` → `theme.controls.indicator_size`，色角色沿用既有拼法（真路径 `roles.primary` → 短名 `theme.primary`，即表在 `ColorRoles` 一侧、前缀不带 `roles.`）。登记表只收**可作属性行活令牌回落点**的组，判据是「其成员指针能否出现在 `.fallback<&X::y>()` 的模板实参位置」，不是「是否存在于 `Theme` 可达路径上」。据此**三组有意不登记**：`LayoutConventions`（字段是枚举而非标量，消费方是排布与绘制分支选择）、`TypeScale`（字体聚合，渲染路径经 `inherit_theme(ctx).type_scale.body_medium` 直读，从不作回落点）、`CsdRoles`（窗口框色，走 `TitleBarStyle` 自有通道）。登记的即五组，共 69 枚短名：`ColorRoles` 38 / `ShapeScale` 6 / `SpacingScale` 7 / `ControlMetrics` 12 / `MotionScale` 6。**表与实际字段的一致性由两枚门禁校验，各管一侧**：`check_theme_token_registry` 判「表 ↔ 字段」方向（T1 集合相等——删条目或改成员名而表未跟即红、T2 短名符合前缀规则、T3 发射的 `value_type` 与字段实型对号、T4 反向覆盖——登记了 `AURORA_TOKENS` 却没进 `theme_token_groups()` 同样红、T5 登记序等于声明序）；`check_no_theme_display_literals` 判「字段 ↔ 表」方向，即源码里不得再出现**手抄**的 `"theme.` 显示串（裸串 / 转义串 / raw-string 三种拼法各自清点，带定界符的 raw-string 归入 raw-string 档；表名自身、`theme_token_groups()` 的前缀串、带 `THEME-LITERAL:` 标记的行与名案文件四类豁免逐条可解释）。五组名同时机器可读地发射进 `aurora_api.json` 顶层的 `theme_tokens` 非条目块（逐组 `{owner, field, prefix, type, count, consumed, tokens}`）。`consumed` 是该组的**属性行消费位**：只要该组至少一枚令牌出现在某条属性条目的 `default_expr` 里就为 `true`——它回答「该组有没有属性行接线」，逐枚令牌的接线明细不在该位表达；绘制路径直读的角色（叠加层读 `hover` / `pressed`、阴影读 `shadow`、遮罩读 `scrim`）没有属性行默认值，不计入此位。消费状态如实记：`shapes` / `spacing` / `controls` 的接档已随四枚控件属性落码（见 [`specification/04-widget.md`](04-widget.md) §4.1），`motion` 当前无属性行消费端（库内唯一时长型属性 `Skeleton::duration` 是 shimmer **周期**、默认 1.5 s，不属该梯度），未接线前不得描述为已生效能力。**属性行未消费组**（与 `theme_tokens` 的 `consumed=false` 同源，由 `check_no_theme_color_literals` 判等集，文档与表漂开即红）：`motion`。

同一枚 `theme_tokens` 块还登记**颜色字面量豁免**。控件层（`include/aurora/widget` / `src/aurora/widget`）与 CSD 预设表（`include/aurora/window/title_bar_style.h`）里，`Color{...}` / `Color(...)` 数字构造式必须归零；仍允许存在的每一处只能来自 `tools/include/theme_literal_exemptions.h` 的逐条登记（仓库相对路径 + 精确出现次数 + ASCII 理由），并原样发射为 `theme_tokens.exemptions.files`。`check_no_theme_color_literals` 判三件事：扫描面出现未登记构造式、登记次数与实际出现次数不等（不允许按文件放行）、豁免表与 `exemptions.files` 不等。当前豁免四条：图表内置 8 色兜底与 `ChartPalette` 默认表（16 处）、`ColorPicker` 的 16 色默认板加一条描述符示例串（17 处）、组合配方在无 `BuildContext` 时的三处固定背景（3 处）、`CsdPreset` 各风格标题栏参照取值（40 处）。同一枚门禁还把控件层 `shaded(...)` 乘算魔数判零——交互态由绘制路径在半透明层上叠加承担，属性行只回 `roles.hover` / `roles.pressed`。取消一处豁免 = 同时改登记表与理由并重刷 schema。

**类型安全扩展 `ThemeExtension<T>`**（`theming/theme_extension.h`）：应用自定义令牌面以类型为键注册——概念基类 + 模板模型（值语义深拷贝，与 `Theme` 随控件回调网被值拷贝的现状一致）；`Theme::extension<T>()` 返回 `const T*`，未注册返回 `nullptr`（调用方显式判空，无字符串拼写静默回落）。典型用途是图表色板（`ChartPalette` 扩展）。扩展**不参与** JSON 自动序列化，由应用自行负责；`operator==` 按类型键逐条判等，扩展类型自身无 `operator==` 时该项恒判不等（保守方向：宁可多标一次脏，不放过真实变更）。扩展位**不参与属性回落**——它是 `void*` + 类型键的线性扫描面，既取不到成员指针（回落点须「改名即编译红」）、也不可枚举与序列化，故库内度量与配色令牌的落点只在 `Theme` 的一等字段上。

**窗口装饰角色组 `CsdRoles`**（`theming/csd_roles.h`）：`Theme` 的库自有字段（与 `roles` / `type_scale` / 度量令牌组同层，不经 `ThemeExtension<T>`——窗口装饰配色是框架资产且必须在渲染路径零分配），8 个字段与标题栏/窗框配色一一对应（`title_bar_bg_active` / `title_bar_bg_inactive` / `title_bar_fg_active` / `title_bar_fg_inactive` / `title_bar_hover_tint` / `title_bar_pressed_tint` / `title_bar_close_hover` / `shadow_color`）。取值来源见「CSD 派生通道」。

**风格轴 `ThemeStyle`**（`theming/theme_style.h`）：`{Material, Windows, MacOs, Gnome, Kde}` 五档，与明暗轴 `ColorScheme{Unspecified, Light, Dark}` **正交**——一套设计语言的亮/暗是同一档的两种呈现，不是两档风格（枚举名里不带 Light/Dark 字样是这条的命名面）。`Material` 同时是**无证据回落档**（无头构建、WASM、裸 X11 拿到的就是它，故 golden 基线与无头 CI 的观感与此前逐位相同）。第二枚证据 `DesktopEnvironment{Unknown, Gnome, Kde, Other}` 只在 Linux / BSD 上读得到，公共入口 `desktop_environment()` 见 [`06-app-platform.md`](06-app-platform.md) §5。

**两轴解算面**：`Theme::from_style(ThemeStyle, ColorScheme = Unspecified, const AccessibilitySettings & = {})` 是纯函数，按（风格, 档）出一整套标定令牌，取值出处逐字段带等级注记（官方 / 实测 / 派生 / 未取证）；`Theme::default_style()` 探测本机的风格档；`Theme::defaults()` ≡ `from_style(default_style())`。三条口径：

- `scheme = Unspecified` 时**填明色列但 `scheme` 字段保持 `Unspecified`**——不表态不等于亮，这一条与 §5.2 的三级优先链共用同一口径。
- `default_style()` 的证据源是 `platform().kind` 与 `auto_detect_surface()`，**不是** `MediaQuery::platform`（后者表渲染后端、非 Windows 恒 `Unknown`，取错源会让 Linux 桌面误落回落档）。判据表本身是纯函数 `detail::style_from(PlatformKind, DesktopEnvironment, has_window_backend)`，无头一票否决、Windows / macOS 单轴定档、Linux 有 KDE 证据取 `Kde` 否则 `Gnome`（Adwaita 基线与既有 GTK 配置启发式同源）、`PlatformKind::Unknown` 按外壳证据取档两者皆无才回落 `Material`、`Web` 恒 `Material`；14 格逐格断言由 `utest_theme_probe` 锚定。
- `defaults()` **只定风格轴、不定明暗档**：不在构造期直读注册表 / 配置文件，那会同时违反 §5.2「创建时读一次」与应用侧「构造期不隐式读 OS 偏好」。要一份确定的暗色写 `from_style(default_style(), os_color_scheme())`，由调用方显式承担那次读取。

**无障碍开关是第三路输入，不是第三根轴**：`high_contrast` / `font_scale` / `reduce_motion` 三枚位**不进 `Theme` 字段、不进 `theme_tokens` 表**，只在**唯一物化点** `Theme::with_accessibility(const AccessibilitySettings &)`（声明 `theming/theme.h`、实现 `theming/theme_presets.cpp`）作为输入折进快照；`from_style` 在收尾处转调它，第三枚形参默认全关 ⇒ 取值与开关接入前逐位一致。物化点是这几路输入的唯一交点，除它之外的读取通路（`inherit_theme` / `resolve_theme` / 属性行回落）一律不看这三枚位——第二条隐式通道因此不存在，开关轴与风格 / 明暗两轴保持正交。三个开关各自的折法：

- `high_contrast` 走 `ColorRoles::with_high_contrast()`（实现 `src/aurora/theming/color_roles_high_contrast.cpp`）的**整族调 tone 派生**，不另打第三列手时常量——判据出处只有 `theming/detail/contrast_audit.h` 那一张登记表。派生只改**相对亮度**、不重新配色：把线性 RGB 等比缩放到该行档位下限的目标亮度（仅当目标越出色域壳时才向白点收敛），观感因此是「同一套配色的强对比版」。可动端优先取底色、其次取前景（前景标签一动即换观感），底色为满足自己的档位必须移动时压在其上的前景在同一步内被修复，故修复不级联；纯底色（只作背景出现者，`surface` 族等）恒不动——高对比语义下页面地不得移位。本已全达标的列返回逐字段相等的副本，默认档 golden 因此不变。**覆盖面边界**：`CsdRoles` 不在登记表内，窗口装饰配色不随高对比变形（待登记后再接，届时同一张表加行）。
- `font_scale` 作用于 `TypeScale` 解析并**带动非文本度量**：字阶 15 档、`SpacingScale` 7 档、`ShapeScale` 6 档同倍增长，后两组遍历各自 `AURORA_TOKENS` 登记表而不手写第二份名单（`check_theme_token_registry` 的 T1 保证表 == 全部公开标量字段）。倍率**只在这一处乘** ⇒ 控件侧不再有第二次末端乘法，读 `type_scale.body_medium` 的图表控件、未表态字号的 `Text` 回落点、以及八枚可配字号控件（`Button` / `TextInput` / `Chip` / `Dropdown` / `RadioGroup` / `SpinBox` / `SegmentedControl` / `TabBar`）的未表态 `font_size` 回落点因此同值。教训来自 Apple `@ScaledMetric` 与 Compose `sp` 的同一条共识：度量不跟着长就是溢出硬失败，这是可达性不是观感。显式设值仍压过主题——本函数只改令牌，表态过的属性行读自己的字段、不读这里。
- `reduce_motion` 是 `MotionScale` 的**短路门**（整族取零档），不是倍率——与惯例组 `conventions.motion_scale` 那根缩放轴分列两轴，「二者相乘消费」的口径不变。动画侧另有两处等价短路（`AnimationController::tick` 与 `SpringAnimation` 的推进），它们一步落端点而不等令牌解析，理由见下面「读取入口」。

**读取入口统一**为 `resolved_accessibility_settings(ctx)`（`core/accessibility.h`：上下文注入优先，缺失回落进程级单例 `current_accessibility_settings()`）。动画驱动器由控件墙钟 tick 进入（`Widget::tick` → 控制器 / 弹簧 `tick(dt)`），链上没有 `BuildContext` 可取，故那两处传 `BuildContext{}` 走兜底档——那是同一入口的缺省腿，不是绕开入口的第二条通道；带上下文的一档已经存在（`Theme::with_accessibility` 把置位折成零档时长），时长一经真正从令牌族解析即随作用域生效，无需短路。`screen_reader_active` 不参与物化：它不改任何主题取值，只影响语义树播报，故既不入折法也不入 §5.2 的解算缓存键。「开关置位 → 生效值变」的成对断言分两侧落点：值那一侧在 `utest_accessibility`，帧通道那一侧在 `utest_window`（口径见 §5.2）。

**令牌层四组**（`theming/shape_scale.h` / `spacing_scale.h` / `control_metrics.h` / `layout_conventions.h`，全为逻辑 dp 的纯值 struct，`Theme` 逐字段直挂）：`shapes`（`small` / `medium` / `large` / `extra_large` 圆角）、`spacing`（留白与内边距档位）、`controls`（控件高、命中下限、焦点环粗细与偏移、滚动条粗细与内缩）、`conventions`（`DialogButtonOrder{AcceptFirst, AcceptLast, VerticalRightAligned}` / `DialogTitleAlignment{Leading, Centered}` / `MenuBarPlacement{InWindow, NativePreferred}` / `ScrollbarMode{Classic, Overlay}` / `FocusRingStyle{Outline, OuterGlow, None}` 五枚惯例枚举加 `motion_scale` 动效倍率）。四组的声明默认值即 Material 基线，故 `Theme{}` 与 `from_style(Material, Unspecified)` 逐位等于 `with_defaults()`。

**消费面**（默认值即「未显式设值时跟随主题档」，逐控件的落点清单见 [`04-widget.md`](04-widget.md) §4.1）：`Button` 圆角读 `shapes.small`、内边距读 `spacing.large`（水平）与 `spacing.small`（垂直）；`Divider` 线厚读 `controls.separator_thickness`；`Checkbox` 方框边长读 `controls.indicator_size`；`Dialog` 的两个工厂读 `conventions.dialog_button_order` / `dialog_title_alignment` 与 `spacing.medium` / `spacing.large`。`controls` 的其余字段（`control_height` / `control_height_small` / `menu_item_height` / `icon_default_size` / `content_padding` / `min_touch_target` / `scrollbar_thickness` / `focus_ring_*`）与 `conventions` 的 `menu_bar_placement` / `scrollbar_mode` / `focus_ring_style` / `motion_scale` **当前无控件消费点**：预设表照标定值写入它们，是为了让风格档可比较、也让后续接线有唯一落点，但「已登记」不等于「已生效」——`scrollbar_thickness` 无消费是因为本库不画滚动条，`focus_ring_*` 无消费是因为环宽同时决定脏区外扩而 `dirty_bounds()` 拿不到 `BuildContext`（只接绘制侧会让环与脏区取到不同宽度、留残影像素）。`utest_theme_tokens` 逐字段钉住 Material 基线，控件侧的三条判据（基线实测 / 改令牌即随动 / 显式设值压过主题）见 `utest_button`、`utest_divider`、`utest_checkbox` 与 `utest_dialog`。

### 5.2 默认根注入与 OS 跟随

`Window`（`window/window.h`）持有主题状态，暴露 `set_theme(const Theme&)`（应用显式设置，最高优先）、`theme()`、`set_theme_style(ThemeStyle)` / `theme_style()`（只钉风格轴，明暗仍跟随 OS）与 `set_os_color_scheme(ColorScheme)`（供宿主推入 OS 档位的设定器）。`prepare_context` 每帧向根 `BuildContext` 注入 `Theme`（`root_env_.set<Theme>(resolved_root_theme())`），与 `MediaQuery` / `WindowState` / `WindowMode` / `WindowChrome` 同址注入，机制完全对称——子树无 `ThemeScope` 时经 `inherit_theme(ctx)` 读到此值。

**换档落地的两半（`set_theme` / `set_theme_style` / `set_os_color_scheme` 三枚 setter 共用）**：换档要同时关掉两条独立的复用通道，缺一半就出现「逻辑对了、屏幕没变」。

- **向下失效**——脏标记只有向上通道，而子树的后代在「约束未变」时短路复用布局缓存与 Display List；故 setter 置一枚 `theme_pending_` 挂起标志，`prepare_context` 在注入那一帧对呈现根调用 `Widget::invalidate_subtree_caches()`（与 §6.5 locale 的挂起标志同一机制、同一函数）。
- **整帧标脏**——配色变化推不出任何有限脏区（每一枚像素都可能变），故 setter 同批调用 `force_full_redraw()`（`layout_dirty_ = true` + 脏面 `mark_all()`）。只挂失效不标脏，增量脏区不会重绘；只标脏不失效，重排仍读旧档缓存。
- **同值短路**——`set_theme_style` / `set_os_color_scheme` 遇相同档位直接返回，两半都不做：宿主 `attach_surface` 时会原样再推一次当前 OS 档，若短不了就每次开窗白清一次整子树缓存、白重绘一帧。`set_theme` 无短路（它携带整棵主题，逐值比较的成本高于收益）。

因此 `WindowHost::on_os_color_scheme` 不再自己补 `force_full_redraw`——整帧标脏与整子树失效都在 setter 内成对完成，宿主层重复一次会让同档重推绕过 setter 的短路白重绘一帧。判据在 `itest_theme_root_injection`：常驻树上换档帧断言「重排发生且读到新档」（关脏追踪观察失效那一半）、换档帧断言 `has_pending_dirty()`（开脏追踪观察像素那一半）、同值重推两半皆无。

**无障碍换档（第四类换档源：没有 setter，故改差量比对）**：§5.1 那三枚参与物化的开关位住在进程级单例（宿主 / OS 桥经 `set_accessibility_settings` 翻位），本窗没有 setter 可挂标志，因此置位点换成帧入口的 `sync_accessibility_fold()`——与本窗上次物化读到的快照比对 `high_contrast` / `resolved_font_scale()` / `reduce_motion`，换档即按与 `set_theme_style` 同构的口径把两半一起挂上（`theme_pending_` 向下失效 + `force_full_redraw` 整帧标脏）。四条口径：

- 比对**必须排在脏区计划之前**：计划一旦判定「无脏、无布局变化」就整帧跳过，在那之后才标脏的换档信号会随本帧一起丢掉，「开关置位 → 生效值变」退化成「等到下次交互」。
- 差异判定走归一化的 `resolved_font_scale()` 而非裸字段，宿主反复写入同一非法值（0 / NaN）归一化后恒等于 1.0，不会被误判为换档；`screen_reader_active` 不入键也不参与比对。
- 显式 `theme_` 在场时开关不参与物化（那套值是应用作者写死的；要高对比由应用自行 `theme.with_accessibility(...)` 后再 `set_theme`），翻位因此不构成重绘理由；比对基准仍照常跟进，免得撤销显式主题后拿一份旧快照白判一次换档。
- 判据为何不是「挂标志」而是每帧三次内存读：漏判的代价是「开关置位不生效」，那是可达性缺陷不是观感问题，三次纯内存读买它不亏。

断言在 `utest_window` 的无障碍注入用例：字号缩放三档（取 2 → 3 → 1，因为换档检测比的是相邻两帧的输入，从 1.0 起步的第一步与基线同值、本就不该出一帧）断言「出一帧且字阶 / 间距 / 形状同倍增长、`Text` 回落点与图表读同一枚字阶」，减动效断言「`MotionScale` 整族取零档且不串档」，高对比断言「该位参与解算键、置位即出一帧」（生效值本身的成对断言在 `utest_accessibility`：内置色列本就达线，「折叠后必须改变」按风格档与本机探测结果而定，跨机器不成立），读屏在线反过来断言**不出帧**。撤开关后必须复原到逐字段全等的快照——折算必须是输入的纯函数。

**已知不对称（现状，非设计）**：`set_locale` 只有向下失效那一半、没有整帧标脏。文本换档同样改的是每一枚字形，理论上需要同样的整帧重绘；本条只记录口径，未随本批改动。

**两轴解算**：`resolved_root_theme()` 在显式 `theme_` 缺席时返回 `Theme::from_style(风格档, 明暗档)`——风格轴取 `set_theme_style` 钉住的档位，未钉时取 `Theme::default_style()` 的探测档；明暗轴取宿主推入的 `os_scheme_`，其中 `Unspecified` ≡ 明色，故注入主题的 `scheme` 恒为 `Light` / `Dark` 具体档、不为 `Unspecified`（这一条与下面三级链的口径同源）。`set_theme` 与 `set_theme_style` 同时给出时前者生效（它携带完整两轴取值，短路排在风格解算之前）。零回归边界：`from_style(Material, Light)` ≡ `Theme::light()`、`from_style(Material, Dark)` ≡ `Theme::dark()`，故风格档落回 `Material` 的那几类环境（无窗口后端的构建——含测试宿主与无头 CI；`PlatformKind::Web`；以及未映射进平台家族且读不到外壳证据的目标）解算结果与两轴模型引入前逐值相同。注意 Linux 一行的 `Unknown` / `Other` **不**回落 `Material` 而是取 `Gnome`（理由见 `theming/theme_style.h` 判据表注释：本库既有的 `os_color_scheme()` Linux 支读 GTK `settings.ini`，已经默认这台机器在跑 GTK 系桌面，Adwaita 是与该启发式同源的基线，而非另造一档「无主风格」）。

**解算结果的缓存**：`Theme` 是含 15 枚字体族名串的 936 字节聚合（`sizeof` 实测，见 `CHANGELOG.md` 的 [Unreleased] Notes），而本函数每帧被 `prepare_context` 调用，故按**解算后的输入档位对**缓存、同组合内不再调 `Theme::from_style`——帧路径因此不含主题构造。键自两轴模型落地起是 `(风格档, 明暗档)`，无障碍接线后扩为 `(风格档, 明暗档, 高对比位, 归一化字号倍率, 减动效位)`（三枚无障碍输入即上面差量比对的那三枚，两处口径同源；`screen_reader_active` 不入键——它不改任何主题取值，入键会让每次桥激活白重建一份 936 字节快照）。缓存不靠 setter 失效：任一 setter 换档、或宿主翻开关，都会改变键、下一帧自然重建，显式 `theme_` 走短路分支根本不读写缓存。断言在 `itest_theme_root_injection`（删去键中的明暗比对后，OS 档翻转、seam 生产链路与双轴钉档三格同时转红）。

**行为变更（主题解算口径的破坏性变化；控件属性类型侧的破坏另见 `CHANGELOG.md` 的 [Unreleased]）**：应用什么都不设时，默认观感从「Material 基线」变为「本机风格档」——Windows 构建得 Windows 档、macOS 得 MacOs 档、Linux 按外壳证据得 Gnome 或 Kde 档。两条迁移路径：`set_theme(Theme::light())` 取回与本窗探测无关的逐值相同旧观感；`set_theme_style(ThemeStyle::Material)` 保留明暗跟随但锁死风格档。

**三级优先链**（应用未显式设置时）：显式 `set_theme` > OS 档（经 `set_os_color_scheme` 推入）> 明色。OS 未表态（`Unspecified`，headless/CI 常见）回落明色，故「`Unspecified` ≡ 明色列」是全系统单一口径：根注入链底、CSD 桥基座（见 5.3）与**两条读取通路的无注入兜底**（`inherit_theme(ctx)` 与 `resolve_theme`，同取 `Theme::light()`，见 5.1）同解。注入的主题 `scheme` 携带解析后的最终档（`Light` / `Dark`，不为 `Unspecified`）。

**OS 偏好的推入路径**：`platform.h` 反向依赖 `window.h`，故 `Window` 不直读 `os_color_scheme()`；由 `WindowHost`（app 层）在 `attach_surface` 创建窗口时推入 `set_os_color_scheme(os_color_scheme())`，与 `set_window_state` / `set_window_mode` 的同位推送同构。读取口径：注册表 / 配置文件访问有副作用，**不在每帧直读**——创建时读一次。运行期变更：Win32 后端在 `WndProc` 捕获 `WM_SETTINGCHANGE`（明暗外观切换以 `ImmersiveColorSet` 标识；宿主构造期播种一次档位基线，只上报相对基线的变化，避免把已推入档位当新变化重报），经 `Surface::set_os_color_scheme_handler`（与 `set_scale_change_handler` 同构的基类 seam）上抛 `WindowHost`，转发为 `Window::set_os_color_scheme`——「跟随系统深浅色」下一帧生效，无轮询；整帧重排重绘与整子树缓存失效都在该 setter 内成对完成（见上面「换档落地的两半」），宿主层不再补置脏。macOS 走同一条 seam：内容视图的 `viewDidChangeEffectiveAppearance` 在用户切换系统明暗（或应用外观跟随变化）时被 AppKit 送达，`MacOSSurface::Impl::sync_color_scheme` 读 `macos_os_color_scheme()` 并在与基线不同时经 `notify_os_color_scheme_change` 上抛，同样落到 `Window::set_os_color_scheme`；`Unspecified` 既不参与去重也不上报（不知道不猜）。`WindowHost::teardown` 同步解绑该句柄（消息泵在宿主回收后仍可收到 `WM_SETTINGCHANGE`，捕获 `this` 未解绑即 use-after-free）。Linux 无对等系统信号，策略文档化为「创建时读 + 应用可随时 `set_theme` 显式接管」，不假装实时。

**框架那一半之外另有一条应用级转发**：`WindowHost::on_os_color_scheme` 在完成「推 `Window`」之后，再向 `WindowHost::set_os_scheme_sink`（`Application` 在 `register_host` 对**每个**宿主安装）报一次原值，由 `Application::os_color_scheme_state()`（`ReadOnlyState<ColorScheme>`）与命令式回调 `set_on_os_color_scheme()` 消费——三级优先链、setter 内成对的整帧标脏与整子树失效、Win32 的逐宿主基线去重与 `Surface::set_os_color_scheme_handler` 的独占单槽语义**全部不变**，本层只加「应用级会看到」这一层；应用侧旁听走 sink，不去抢那一枚槽（抢了会顶掉框架的根主题跟随）。四条口径：跨宿主去重（N 扇窗各收一次同值切换在应用级只算一次，比较在转发处做而非指望 `State::set`——它对相等值照样发信号）、回调恒在主线程（消息泵线程同步转发，不引入后端或探测线程）、初值 `Unspecified` 不隐式 seed（启动期解算走显式的 `refresh_os_color_scheme()`，宿主登记时框架自己那一读只推 `Window` 快照）、**原值透传不回落**（「系统不表态回哪一档」是应用策略，框架回落会让应用永久失去区分「系统说亮」与「系统没说」的能力）。聚合范围与 `window_state()` / `window_mode()` 的「只投影主窗」相反，完整契约见 [`06-app-platform.md`](06-app-platform.md) §6.2。

**两轴在「读一次」上的差异是有意的**：明暗档运行期跟随 OS（上面那条上报链），风格档则不跟随——`Theme::default_style()` 的读数落进程级静态缓存，用户在会话中途换桌面外壳（GNOME ↔ Plasma）不改当前进程的风格，需重启应用。理由两条：外壳切换在 XDG 侧是会话级事件、库侧既不该订阅也不该轮询；且风格档决定的是整套令牌，运行期翻档会让一棵已排好的树整体换族名与度量，成本远高于换明暗色。口径都在本条，代码侧的注记（`Theme::default_style()` 函数头与 `src/aurora/theming/detail/theme_probe.cpp` 文件头）指向这里。

`os_color_scheme()`（`window/platform.h`）的探测细节：Linux 读 GTK 配置文件启发式（不引 DBus 依赖），Win32 读注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` 的 `AppsUseLightTheme`（REG_DWORD，0 = Dark；键在值缺 = Light；键打不开 = Unspecified，实现位于独立 TU 避免公共头引入 `<windows.h>`），macOS 问 `NSApp.effectiveAppearance` 的 `bestMatchFromAppearancesWithNames:`（`NSAppearanceNameDarkAqua` 命中即 Dark、`Aqua` 即 Light；`NSApp == nil`（CLI / 无头）或 bestMatch 为空回 `Unspecified`；`NSAppearance` 的最小明暗对也能正确归类，实现位于独立 TU 避免公共头引入 `<AppKit/AppKit.h>`，并已在 macOS 构建内编译验证）。`Platform::color_scheme` 字段须由调用方经 `os_color_scheme()` 显式填充（`platform()` 不代做——读偏好有文件 / 注册表访问副作用）。

**测试约定（防 CI 档位漂移）**：经 `Window` 呈现主题消费控件、且做绝对色像素断言的测试，必须显式钉住主题（`set_theme`，或至少 `set_theme_style(ThemeStyle::Material)` 锁风格档 + `set_os_color_scheme` 锁明暗档）——否则其观感随 runner 的 OS 家族与 OS 档位双向漂移（两轴模型后，风格轴也会漂）。做「与探测结果自洽」类断言的用例反过来**不设**风格档，判据取「注入档位 == `Theme::default_style()`」而非某个具体档名，避免同一份用例在 Windows 与无头 runner 上一红一绿。「跟随 OS」的正向验证经 seam 打桩完成（`set_os_color_scheme` 推入反转档位断言注入翻转），不依赖真机档位，见 `itest_theme_root_injection`。

### 5.3 CSD 派生通道（typed）

`title_bar_style_from_theme(theme)` / `window_frame_style_from_theme(theme, base)`（`theming/csd_style_bridge.h`）签名保留，实现为「预设基座 + CSD 角色组派生」：基座按 **`theme.style` × `theme.scheme` 两轴**取八枚 `CsdPreset` 之一，映射由纯函数 `csd_preset_for(ThemeStyle, ColorScheme)` 给出（`Windows` → `WindowsLight` / `WindowsDark`、`MacOs` → `MacLight` / `MacDark`、`Kde` → `BreezeLight` / `BreezeDark`、`Material` 与 `Gnome` → `AdwaitaLight` / `AdwaitaDark`；几何 / 显隐 / 度量字段不参与派生，属定制面——维持 §4.2 钉住的原生机度与字形口径不动），配色字段从 `csd` 角色组逐项派生（`bg_active` ← `title_bar_bg_active`、`fg_active` ← `title_bar_fg_active`、`close_hover` ← `title_bar_close_hover`、窗框投影色 ← `shadow_color` 等）。`Unspecified` 按 5.2 单一口径取明色列。

**逐值不变量**：`title_bar_style_from_theme(Theme::light())` 与 `from_preset(CsdPreset::AdwaitaLight)`、`Theme::dark()` 与 `from_preset(CsdPreset::AdwaitaDark)` 的配色字段逐值相同（`Theme::light()` / `dark()` 用预设 pinned 值播种 `csd` 组），由 `utest_theme_csd_bridge` 钉死。**`from_bare` 口径**：`with_defaults()` 即 light 基准列，故 `title_bar_style_from_theme(Theme::with_defaults())` ≡ `from_preset(CsdPreset::AdwaitaLight)`——裸主题不再解析出暗色标题栏，这是随角色化重构落地的有意行为变化（迁移路径见 `CHANGELOG.md`）。

**为什么不从通用角色派生**：现预设 CSD 值与通用角色值存在差集——light `title_bar_fg_active` = 0x333334 而 `text` 角色为纯黑；dark `title_bar_bg_active` = 0x2E2E32 而 `surface` = 0x303030、`title_bar_fg_inactive` = 0x919193 而 `text.muted` 口径的 `on_surface_variant` = 0x9A9996。从通用角色派生要么砸掉逐值不变量，要么把通用角色改成 CSD 专用值（污染所有读 `surface` 的控件）。CSD 配色本就是组件级覆写面，类型化后继续单列；应用逐字段定制直接改派生结果的 `TitleBarStyle` 字段（或直接 `set_title_bar_style`）。

### 5.4 作用域、查询与废除面

**保留面**：`ThemeScope`（`theming/theme_scope.h`，`Provider<Theme>` 子类，支持经共享 `State<Theme>` 运行时换肤，值变化即子树重渲染）、`inherit_theme(ctx)`、`resolve_theme(root, target)` 查询（`theming/theme_query.h`）、`resolve_color_scheme(explicit, theme, os)` 纯函数（`theming/csd_style_bridge.h`，三级链各层以 `Unspecified` 表示不表态逐层下探）。

`inherit_theme(ctx)` 返回的是**最近祖先注入主题的引用**而非拷贝：控件每帧解算默认度量与配色的路径因此零分配（调用方要长期持有须自行拷贝）。无注入时它指向一份进程内静态的 Material 基线档，`resolve_theme` 的兜底与它**合一**为 `Theme::light()`（旧偏差：本函数曾兜 `Theme::with_defaults()`，差在 `scheme` 位；「兜底主题的 `roles` 已确定为明色列却标 `Unspecified`」自我矛盾，故拉到 `light()` 一侧）——这两处兜底**恒 ≡ Material 基线**，加上 `Window` 根注入未显式设主题时走的两轴解算（§5.2），构成「三处兜底同源」：平台风格档只能经 `Window` 根注入或树内 `ThemeScope` 进入，任何兜底读探测值都会让「无主题」与「探测到某平台」两种状态给出同一观感。三处各有断言，见 `itest_theme_root_injection`（根注入）、`utest_theme_query`（查询），以及控件侧以裸 `BuildContext` 调 `resolve_*` 的「基线实测」用例（`utest_button` / `utest_divider` / `utest_checkbox`，证的是构建上下文这条兜底）。

**废除面**（随角色化重构退场，迁移路径见 `CHANGELOG.md`）：`Theme` 扁平字段 `background` / `primary` / `on_primary` / `text` / `font`；`TokenValue`；`tokens` / `dark_tokens` 命名令牌表及全部 `token*` / `set_token` 成员（含 `token_for` / `token_in` 解析入口与 `csd.*` / `focus.ring` / `chart.palette.*` 字符串令牌层）；`StyleProps` / `TokenOr<T>` / `ResolvedStyle`（theming 目录下 style_props.h 头无生产消费方，文件本体与测试 `utest_style_props` 已随 收束批删除）。控件取色统一为两态：显式属性值 > 主题角色（未显式设置时角色必有值，无第三态回落），映射表见 [`04-widget.md`](04-widget.md)。

**主题 JSON 重建面（计划面，未落地）**：`Theme` **没有** `to_json` / `from_json` 实现体——主题的现建重建通道是两轴纯函数 `Theme::from_style(style, scheme)`（§5.1）与预设工厂 `light()` / `dark()`，不走字符串。计划中的 JSON 面是「角色集（roles + type_scale + csd + scheme + 三组度量 + 惯例组）进出，键名即角色/令牌名——stringly 只允许存在于这个序列化边界，不进渲染路径」，其键名口径（含本批新增的 `shapes` / `spacing` / `controls` / `conventions` 四组）随该实现落地才生效，本节此刻不对其作现状承诺。`ThemeExtension<T>` 即便实现也不参与自动序列化（§5.1）。

树内的注入壳另有一层：`Provider<Theme>` 在 `src/aurora/widget/serialization.cpp` 以 `reg_no_props` 注册为 `"ThemeProvider"`，即**结构可重建、注入值不携带**（重建后取默认 `Theme{}` ≡ Material 基线明色列）。要持久化主题取值，等上一条 JSON 面落地，或由应用在自己侧存值、重建后再注入。

---

## 6 国际化

译文与代码分离：**构建期** `gen_i18n` 把 ARB 形态的 JSON 词表编译成「薄生成头 + `AUM1` 二进制词表」，
**运行期** `MessageCatalog` 载入二进制并按 `Locale` 解析模板。C++ 源码里既没有裸 Id 数字，也没有译文字面量
（`examples/demos/demo_i18n.cpp` 与全部库内控件同此口径）。

| 类型 | 说明 |
|:---|:---|
| `MessageId` | 全库唯一的强类型词条 Id（`struct { std::uint32_t value; }`，**无隐式整型转换**）。Id 常量由生成头给出；`AURORA_INVALID_MESSAGE_ID`（`0xFFFFFFFF`）是无效哨兵，Id 区间不得触雷 |
| `Locale` | language / script / region 三槽，`tag()` 拼成 BCP 47 形态（`zh-Hans-CN`）。三槽共同参与词条回退与数字 / 日期格式化 |
| `LocalizedString` | 值对象：`text` / `id` / `args` / `localize`。`tr(MessageId, {MessageArg...})` 造本地化值，字面量经隐式构造即非本地化值；`resolve(const MessageCatalog*, const Locale&)` 得最终文本，查表失败回退 `text` |
| `MessageArg` | 具名参数（名字 + `variant<std::string, std::int64_t, double, CivilDate>`）。占位按名匹配，译文自由调序不会错位（v2 起不再有位置占位与嵌套参数） |
| `MessageCatalog` | 消息目录：`register_vocabulary` / `load_binary` / `add` / `set_default_locale` / `resolve` / `canonical_name` / `find_by_name` / `missing_translations`。`default_catalog()` 给出进程级实例，内置词表在其中预登记 |
| `PluralCategory` / `plural_category` / `PluralOperands` | CLDR 表驱动的复数与序数判定（见 §6.1） |
| `CivilDate` / `DateStyle` / `format_number` / `format_currency` / `format_date` | 本地化格式化（见 §6.1） |

所有文本属性类型为 `LocalizedString`，`TextProps::set_content` 同时收 `const char*`、`std::string`、`LocalizedString` 与 `ReadOnlyState<LocalizedString>` 四种实参，彼此等价。

```cpp
au::Text(au::TextProps().set_content(demo::msg::demo::greeting(3, "Ada")));   // 生成头给出的具名形参构造函数
au::Text(au::TextProps().set_content(au::LocalizedString{"a plain literal"})); // 不经词表
```

**模板语法 = ICU MF1 子集**，运行期解析器与构建期校验器**同源**（`src/aurora/i18n/detail/message_engine.h`
被直接编入 `gen_i18n`，构建期能过的模板运行期必能过）：

| 形态 | 语义 |
|:---|:---|
| `{name}` | 具名占位，按参数类型直出文本 |
| `{name, number}` / `{name, number, integer}` | 千位分组 + 小数点按 locale；`integer` 取整 |
| `{name, currency, USD}` | 货币符号与默认小数位按 ISO 码 + locale |
| `{name, date, short\|medium\|long}` | 按 locale 的日期格式（参数须是 `CivilDate`） |
| `{name, plural, =0{…} one{…} other{…}}` | CLDR 基数类别；`=N` 精确匹配**优先于**类别 |
| `{name, plural, offset:1, …}` | `offset` 从生效值里扣减，`#` 与类别判定都用扣减后的值 |
| `{name, selectordinal, one{#st} two{#nd} few{#rd} other{#th}}` | CLDR 序数类别（en `1st/2nd/3rd/4th`） |
| `{who, select, admin{…} other{…}}` | 字面量选择，`other` 兜底 |
| `#` | 只在数字上下文（`plural` / `selectordinal` 分支体内）成立，替换为生效后的选择器值（含分组，如 en `1,234`）；其余位置按字面量 |
| `''` `'{` `'}` `'#` | 引号转义：`'` 打头吃掉下一个结构字符 |

嵌套深度上限 8；解析失败**回退模板原文**（运行期不设错误码——结构错误已在构建期被 `gen_i18n` 拦下）。

---

### 6.1 CLDR 复数与本地化格式

复数与序数判定**表驱动**：`tools/gen/gen_plural_rules.py` 把 CLDR 48（Unicode 16.0.0）的
`cldr-core/supplemental/{plurals,ordinals}.json` 编译成 `src/aurora/i18n/plural_rules.gen.cpp`
——224 语言的 cardinal / ordinal 规则与可达类别掩码，源数据不入库（见 `THIRD_PARTY_LICENSES.md` §10）。
未知语言码回退英语规则。

| 入口 | 说明 |
|:---|:---|
| `PluralOperands` | CLDR TR35 操作数（`n` / `i` / `v` / `w` / `f` / `t`）。三个构造口 `plural_operands_from_int` / `plural_operands_from_double` / `plural_operands_from_decimal`，最后者最精确（消息引擎缺省走它，`1.0` 与 `1` 的 `v` 不同即由此区分） |
| `plural_category(ops, loc)` | cardinal 类别（`Zero`/`One`/`Two`/`Few`/`Many`/`Other`），每语言至少含 `Other` |
| `plural_category_ordinal(ops, loc)` | ordinal 类别（en `1st`→`one`、`2nd`→`two`、`3rd`→`few`、`4th`→`other`） |
| `plural_category(double n, loc)` | 便捷重载（旧口径不变） |

```cpp
au::plural_category(au::plural_operands_from_decimal("1.0"), au::Locale{"en"});  // Other（en 的 one 要求 v=0）
au::plural_category_ordinal(au::plural_operands_from_int(2), au::Locale{"en"});  // Two
```

数字 / 货币 / 日期按 `Locale` 格式化（零依赖自研表；`Currency` 取 `USD`/`EUR`/`JPY`/`CNY`/`GBP`，
符号与默认小数位来自 `CurrencyInfo`，分组与小数点来自 `NumberSymbols`）：

```cpp
au::format_number(1234567.0, au::Locale{"de"}, 0);                    // "1.234.567"
au::format_currency(1234.56, au::Currency::EUR, au::Locale{"de"});    // "1.234,56 €"
au::format_date(2025, 10, 25, au::Locale{"de"});                      // medium（缺省）："25.10.2025"
au::format_date(2025, 10, 25, au::Locale{"en"}, au::DateStyle::Long); // "October 25, 2025"
```

`DateStyle` 三档（`Short` / `Medium` / `Long`）是 CLDR 语义的轻量自研子集：`Medium` 与旧版
`format_date` 逐位一致（en `2025-10-25`、zh/ja `2025年10月25日`），`Short` / `Long` 的月名表只覆盖
en / de / fr，其余语言按 en 兜底，zh / ja 三档同 medium。`CivilDate` 是把年 / 月 / 日收进一枚强类型的
载体，供 `{name, date}` 占位与生成 API 的日期形参使用（`format_date` 本身仍取三个整型参数）；
实现不校验日期范围，与旧口径一致。

---

### 6.2 控件内置文案：规范名 ↔ Id 契约

除宿主传入的文本属性外，少数控件**自带**上屏文案（占位提示、空态提示、拖拽播报之类）。它们不是字面量：
一律按内置 `MessageId` 查 `default_catalog()`，宿主按**规范名**登记译文即可本地化。

**库不预置任何语言的译文**：`default_catalog()` 只预登记内置词表的规范名（Id ↔ 名字），模板槽留空；
英文文案留在控件调用点，作为 `LocalizedString::text` 的查表失败兜底。理由不变——目录没有删除接口，
库侧写入译文会给宿主留下不可逆污染。

数据源是 `codespec/i18n_builtin.json`（唯一权威），经 `gen_i18n --emit-builtin` 生成
`include/aurora/i18n/builtin_messages.gen.h`（`aurora::msg` 的 Id 常量 + `aurora::detail` 的名字表与英文模板表），
代码侧统一经薄封装头 `i18n/builtin_messages.h` 引入（生成件不得被直接引用）。内置词表占 Id 保留区
`[0, 1024)`，应用词表基址的硬下限即 1024（见 §6.3）。

| Id | 规范名 | 控件 | 用途 | 英文兜底模板 |
|:---|:---|:---|:---|:---|
| 0 | `aurora.reorder.cancelled` | `ReorderableList` | 取消拖拽的读屏播报 | `Reorder cancelled. Item returned to position {position} of {total}.` |
| 1 | `aurora.reorder.dropped` | `ReorderableList` | 换位成功的读屏播报 | `Item moved from position {from} to position {to} of {total}.` |
| 2 | `aurora.reorder.dropped_in_place` | `ReorderableList` | 原地落定的读屏播报 | `Item dropped at position {position} of {total}.` |
| 3 | `aurora.reorder.grabbed` | `ReorderableList` | 抓取成功的读屏播报 | `Position {position} of {total}. Item grabbed. …` |
| 4 | `aurora.reorder.position` | `ReorderableList` | 键盘移动时的读屏播报 | `Position {position} of {total}.` |
| 5 | `command_palette.no_results` | `CommandPalette` | 无匹配结果的空态提示 | `No matching commands` |
| 6 | `command_palette.placeholder` | `CommandPalette` | 搜索框占位符 | `Type a command...` |

`CommandPalette` 的三档优先级不变：**文本覆盖（`set_placeholder` / `set_empty_message`）> 按 Id 查表 >
兜底字面量**。文本覆盖生效后该串不再查表，宿主须自己备多语言；`set_placeholder_key` /
`set_empty_message_key` 收的是 `MessageId`，换词条仍走 i18n，且会清掉此前的文本覆盖。

解析发生在布局与绘制期，locale 取 `ctx` 注入的 `Locale`（未注入回落缺省档），与 `Text` / `Button`
的 `resolved_text()` / `resolved_label()` 同口径；换档的生效时机见 §6.5。

```cpp
auto &c = au::default_catalog();
(void)c.add(au::Locale{.language = "zh"}, "command_palette.placeholder", "输入命令…");
(void)c.add(au::Locale{.language = "zh"}, "command_palette.no_results", "没有匹配的命令");
```

> 上表是**外部契约**：宿主登记译文要靠规范名，读屏与序列化要靠 Id。单测因此**以字面量钉死「规范名 ↔ Id」
> 映射**（`utest_message_catalog` / 控件侧用例），而不是拿生成常量自比自——否则库侧名字拼错时「登记端」与
> 「查表端」一起错、两边抵消，用例会恒绿。`gen_i18n` 的 golden 门禁再把「`aurora::msg::Id` 与内嵌名字/模板表
> 出自同一份 `codespec/i18n_builtin.json`」锁死（见 `specification/08-tooling.md`）。

---

### 6.3 构建期词表管线与 AUM1 布局

`gen_i18n` 吃 ARB 形态的 JSON（基准语言 + 任意多份译文），产两件：

- **薄生成头**（`messages_en.hpp`）：`<ns>::id::AURORA_*` 的 `constexpr MessageId` 常量，加每 key 一枚
  具名形参的 `inline` 构造函数（形参**按占位名字母序**排，类型取自 `@key.placeholders` 元数据：
  `string` / `int64` / `double` / `date`→`aurora::CivilDate`）。用错词条或漏参数在编译期即拦下。
- **`AUM1` v1 二进制词表**（`messages_en.bin`）：全语言合一，运行期整读交 `load_binary`。

CMake 侧经 `cmake/AuroraI18n.cmake` 的 `aurora_i18n_generate()` 接入（CLI 契约、校验清单与门禁见
`specification/08-tooling.md`）。译文迭代只改 JSON、只重出 bin，不触碰 C++ 编译；bin 不属任何源表，
故由该助手挂一个自定义目标兜住重生成，并随可执行文件落同目录。

`AUM1` 布局（全小端；`0xFFFFFFFF` 是「该 locale 无此词条」的哨兵）：

| 段 | 内容 |
|:---|:---|
| 头 20 字节 | `AUM1` magic(4) + version(4) + `id_base`(4) + `id_count`(4) + `locale_count`(4) |
| 名字表 | `id_count` 项，各为 u16 长度 + UTF-8 规范名（空串 = 保留区空洞占位，不进名字索引） |
| locale 槽表 | 每 locale：u8 tag 长度 + tag 字节 + `id_count` 项 `{u32 offset, u32 length}` |
| 模板池 | 全部 UTF-8 模板串按 offset/length 寻址（同串复用） |

区间 `[id_base, id_base + id_count)` 与既有登记重叠、名字映射到不同 Id、或触雷无效哨兵 → 结构化错误
（`errors.toml` 的 i18n 组）；magic / version / 字节序 / 截断同样结构化，且**任何失败都不落库**（载入原子性）。

---

### 6.4 目录模型与回退链

`MessageCatalog` 的存储是「Id → 模板」按 locale 分档的槽表；**各 locale 的查表视图在载入期沿回退链物化**，
故 `resolve` 是 O(1) 数组取槽，locale 切换只换视图不重排数据。回退阶梯按「先丢 region 后丢 script」展开，
缺子标签的档自动折叠去重，最多六档：

1. 完整 tag（`zh-Hans-CN`）
2. language-script（`zh-Hans`）
3. language-region（`zh-CN`）
4. language（`zh`）
5. `set_default_locale` 的完整 tag
6. 缺省档的 language

六档穷尽 → 回退 `LocalizedString::text`（调用点字面量）。`missing_translations(loc)` 给出「loc 无自有模板、
而缺省档或其他档有」的 Id 升序清单，是载入期聚合 WARN 的明细查询口。

props 序列化（`$msg` 形态，见 `specification/08-tooling.md` 的 UI-to-code 面）写侧落
`{"$msg": {"id": "<规范名>", "args": {...}}}`，读侧按规范名 `find_by_name` 反查 Id；旧的字面 string 形态
读侧继续兼容。

---

### 6.5 locale 的帧级注入

locale 沿 `Environment` 向下传播，两个注入点：

| 入口 | 作用域 | 口径 |
|:---|:---|:---|
| `Window::set_locale(loc)` | 整窗根注入（与 `set_theme` 同构） | setter 只存值，`prepare_context` 自**下一帧**起每帧注入；从未设定则不注入，控件回落缺省档 |
| `LocaleProvider{locale 或 State<Locale>, child}` | 子树覆写 | 「最近祖先优先」压过窗口级默认；接 `ReadOnlyState<Locale>`（`State<Locale>` 单向降级），换值即触发子树刷新 |

**帧内一致**：设定当帧的离屏快照仍是旧档，新档自下一帧整帧生效——一帧之内不会出现半中半英的混排。

**换档必须向下失效缓存**：脏标记只有向上通道（`mark_needs_layout` / `invalidate_display_list_up` 沿布局父链
溯祖先），而文本在**布局期**按环境 locale 解析并缓存解析结果。若只标脏注入点自身，子树后代仍按「约束未变 ⇒
复用布局缓存 / 回放 Display List」短路，内容就会冻结在旧档。故：

- `Widget::invalidate_subtree_caches()` —— 清本控件**及全部后代**的布局缓存与 DL 缓存；
- `Provider<T>::on_signal_dirty()` 覆写默认标脏口径，换值那一帧对子树调用它（`Theme` / `MediaQuery` 同源受益）；
- `Window::set_locale` 走挂起标志，在注入那一帧对呈现根调用它。`Window` 的三枚主题 setter（`set_theme` / `set_theme_style` / `set_os_color_scheme`）用同一机制的同一函数，另加一枚 `force_full_redraw` 负责像素那一半——口径与差异见 §5.2「换档落地的两半」。

代价是 O(子树) 且只在换帧发生一次，与首帧同量级；稳态帧不经过这条路径，缓存收益不受影响。
环境值依赖自身布局的控件另有第二条口径：覆写 `can_cache_layout()` 返回 false（见 `04-widget.md`）。

---

## 7 Modifier 修饰系统

### 7.1 概述

`Modifier` 用一组**正交、可组合**的修饰符表达内边距、背景、可点击、尺寸、边框、裁剪、对齐、偏移、拖拽等横切能力，替代继承爆炸。

`Modifier` 是**不可变值**（`.then()` 除外）：除 `.then()` 外每个方法返回新副本，可链式组合并赋值给任意组件的 `.modifier` 属性。

```cpp
auto save_btn = au::Button(au::ButtonProps().set_label("保存"));
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
//   .examples = {"au::Button(au::ButtonProps().set_label(\"OK\"))"}
// }

au::Button btn;
au::Widget &w = btn;
auto desc = w.describe();      // 运行时多态调用
auto all = au::serialization::list_all_schemas();
```

这使得 AI Agent 可以在**运行时**动态发现 Aurora API，而不完全依赖训练数据。

> Schema 文件结构、生成器与工具链消费方式见 [`08-tooling.md`](08-tooling.md)；属性元数据 `PropDescriptor` 定义见 [`04-widget.md`](04-widget.md) §2.1。
