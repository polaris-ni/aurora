# H.4 / H.4.1 环境/DI + 媒体查询；H.5 主题/i18n

> 本文件是「三、特性详细规范」按功能域/子系统划分出的子文档；返回主线索引见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。
> 后续核心子系统 API 章节（H.11–H.17 + Log + AI-First）见 [`../subsystems_api/`](../subsystems_api)：SUBSYSTEM_API_SERIALIZE / SUBSYSTEM_API_LAYOUT_ENGINE / SUBSYSTEM_API_WIDGETS / SUBSYSTEM_API_INSPECTOR / SUBSYSTEM_API_TOOLING / SUBSYSTEM_API_LOG_AI。
> 相关功能域规范（A–G）见 [`../features/`](../features)：FEATURE_API_DESIGN / FEATURE_ARCH_STATE / FEATURE_RUNTIME_SAFETY / FEATURE_LAYOUT_RENDER / FEATURE_CROSS_PLATFORM / FEATURE_AI_INSPECTION / FEATURE_AI_TOOLING / FEATURE_ENGINEERING。

#### #H.4 环境（Environment）与依赖注入

核心目标：把主题、Locale、自定义服务等"下行"数据通过树显式传递，组件用类型查询，避免隐式全局状态。

- **`Environment`**：类型 → 值的不可变映射。`env.with<T>(value)` 返回"注入了 T=value"的新 `Environment`；`env.get<T>()` 读取。
- **`BuildContext`**：每个组件渲染时拿到的上下文，`ctx.environment<T>()` 向上查找最近注入的 `T`；找不到返回默认构造的 `T`。
- **`Provider<T>`**：控件，在子树根注入 `T`：`au::Provider<T>{ x, au::Column(au::ColumnProps{ .children = { ... } }) }`
  （位置式，非指定初始化器；`Column`/`Row` 非聚合，须用 `*Props` 聚合或初始化列表）。
- **`ThemeProvider` / `LocaleProvider` / `MediaQueryProvider`**：`T=Theme` / `Locale` / `MediaQuery` 的专用 Provider（见
  §H.5）。

```cpp
// 父级注入
auto root = au::ThemeProvider{ au::Theme::dark(), build_app() };

// 子组件内查询（无需参数传递）
au::Text("Hi").color(ctx.environment<au::Theme>().text);
```

> 与 §8 一致：主题 / 字体 / 颜色通过显式 `Provider` / scope 传递，不依赖隐式上下文继承。

#### #H.4.1 响应式媒体查询（MediaQuery）与 LayoutBuilder

核心目标：把设备度量（屏幕方向 / 平台 / 设备形态 / 安全区 / 减弱动效 / 屏幕尺寸 / 缩放因子）作为响应式上下文沿树下行，子树按约束动态构建不同
UI（Flutter `MediaQuery` + `LayoutBuilder` 语义）。

- **`MediaQuery`**（`environment/media_query.h`）：响应式上下文值类型（纯增量，向后兼容）。字段：
    - `Size size`：当前窗口 / 子树可用逻辑尺寸（dp）。
    - `float scale_factor`：设备像素比（由 `Window::present_root` 从 `Surface::scale_factor()` 注入到
      `BuildContext::scale_factor`，本地坐标缩放的唯一权威来源）。
    - `float text_scale_factor`：系统字体缩放（辅助功能）。
    - `ScreenOrientation orientation`：由 `screen_size` 派生（`Portrait`=高≥宽，`Landscape`=宽>高）。枚举
      `ScreenOrientation{Portrait, Landscape}`（与 `divider.h` 的 `Orientation` 语义不同，不复用）。
    - `Size screen_size`：物理屏幕的逻辑尺寸（dp）；Win32 经 `GetSystemMetrics` 取得。
    - `PlatformKind platform`：编译期常量——Win32 下 `Windows`，其余 `Unknown`（不做运行时 OS 探测）。枚举含
      `Unknown/Windows/macOS/Linux/Web`。
    - `DeviceKind device`：编译期常量——Win32 下 `Desktop`，其余 `Unknown`。枚举含 `Unknown/Desktop/Mobile/Tablet`。
    - `EdgeInsets padding`：安全区（刘海 / 状态栏）内边距（dp）。 **Wayland 客户端自绘装饰（CSD）标题栏/边框占用的区域经
      `Surface::content_inset()` 并入本字段**（见 `DecorationPolicy`），子树据其为内容留白以避开自绘装饰——对齐 Flutter
      `MediaQuery.padding` / `SafeArea` 范式，使「无标题栏窗口内容也不被遮挡」。** 默认 0（无装饰）。
    - `bool prefer_reduced_motion`：系统「减弱动效」偏好；Win32 经 `SystemParametersInfo(SPI_GETCLIENTAREAANIMATION)` 读取。
    - 工厂：`MediaQuery::of(float scale)`（轻量构造，仅设缩放因子）；`MediaQuery::from_surface(const Surface&)`（跨平台成型：
      `Headless`/非 Windows 取 Surface 尺寸与默认值，并 **将 `s.content_inset()` 并入 `padding`**；Win32 经
      `win32_media_query` 取真实屏幕逻辑尺寸与减弱动效）；`MediaQuery::of(const BuildContext&)`（读最近祖先
      Provider，无则返回进程级默认实例）。
- **窗口装饰策略（DecorationPolicy）**（`window/surface.h`）：跨后端声明、各 `Surface` 按 compositor 能力映射，解决「GNOME 不支持
  `xdg-decoration` 时无标题栏、窗口不可操作」问题，使 **无标题栏也能移动/缩放/关闭**。枚举
  `DecorationPolicy{ Auto, ServerSide, ClientSide, Borderless, Frameless }`，置于 `WindowStyleOptions::decoration`（默认
  `Auto`；`WindowStyleOptions::frameless` 等价于 `Frameless`）。
    - `Auto`：优先协商服务端装饰（SSD，KDE 原生标题栏）；compositor 不支持（GNOME）时回退客户端自绘（CSD）标题栏 + 边框兜底。
    - `ServerSide`：强制 SSD；不可用（GNOME）时退化为 CSD 兜底（避免无装饰且不可操作）。
    - `ClientSide`：强制自绘 CSD 标题栏（即便 compositor 支持 SSD）。
    - `Borderless`：无标题栏，但保留可拖拽缩放边框；移动靠 **修饰键拖拽**（按住 `Super`/`Alt` 拖拽任意处 →
      `xdg_toplevel_move`）。
    - `Frameless`：完全无装饰，由应用自绘 UI 并经 **程序化窗口控制**驱动状态。
    - **安全区**：CSD 标题栏/边框占用区经 `Surface::content_inset()` 暴露（Wayland 下
      `EdgeInsets{ left/right/bottom = border, top = title_bar_h }`），并入 `MediaQuery.padding`，子树据其避开装饰（对齐
      Flutter `SafeArea`）。
    - **程序化窗口控制**（新增 `Surface`/`Window` 虚函数，默认空实现）：`close()`（等价点 ×）、`minimize()`、
      `toggle_maximize()`、`set_fullscreen(bool)`；Wayland 经 `xdg_toplevel` 协议生效，使无标题栏/无边框窗口也能由应用按钮驱动状态。
- **读取 API**：因 `MediaQueryProvider` 是 `using` 别名无法加静态 `of`，故提供：
    - `media_query_of(const BuildContext&) -> const MediaQuery*`：向上查找最近注入的 `MediaQuery`；无 Provider 时返回
      `nullptr`（调用方按需降级）。
    - `MediaQuery::of(const BuildContext&) -> const MediaQuery&`：便捷封装，无 Provider 时诊断并返回默认实例。
- **`LayoutBuilder`**（`widget/layout_builder.h`）：响应式构建原语。`builder` 为
  `Reactive<std::function<Node(const BuildContext&, const Constraints&)>>`；布局阶段按 `constraints` 动态构建子节点。
  **仅在「约束显著变化」或「builder 闭包被替换」时重建并重新 mount 子节点**，约束不变则复用缓存子树，避免每帧抖动。闭包内部读取的
  `media_query_of` 不自动订阅——响应式驱动来自约束变化（如窗口缩放）。
- **`MediaQueryProvider`**：`using MediaQueryProvider = Provider<MediaQuery>`；子树根注入 `MediaQuery`，后代经
  `media_query_of(ctx)` 读取（「最近祖先 Provider 生效」，注入链仅对 Provider 的后代可见）。
- **自动注入（T8）**：`Window::present_root` 每帧自动以 `MediaQuery::from_surface(*surface)` 在 **根 `BuildContext`** 注入
  `MediaQuery`（存入稳定的 `Window::m_root_env`，地址恒定，避免子树 Provider 持悬空父指针）。因此 **无需手动包裹
  `MediaQueryProvider`**，整棵树（含根 widget 自身）即可经 `media_query_of(ctx)` / `MediaQuery::of(ctx)` 读取设备上下文；
  `m_root_env` 每帧重建以反映窗口 resize。`Application::run` 经同一 `present_root` 自动受益。手动 `MediaQueryProvider`
  仍按「最近祖先优先」覆盖此默认值。

```cpp
// 子树根注入设备上下文
auto root = au::MediaQueryProvider{
    au::MediaQuery::from_surface(surface),
    au::LayoutBuilder{
        [](const au::BuildContext& ctx, const au::Constraints& c) -> au::Node {
            const au::MediaQuery* mq = au::media_query_of(ctx);
            const bool wide = (mq != nullptr) && (c.max.width >= 600.0f);
            return wide ? au::Node{ build_two_column(ctx) }
                        : au::Node{ build_one_column(ctx) };
        }},
};
```

> 与 §8 一致：响应式上下文以 `Window::present_root` 自动注入的根 `MediaQuery` 为默认来源（T8），同时兼容显式
> `MediaQueryProvider` 覆盖（最近祖先优先）；不依赖进程级可变全局状态。

### 标题栏样式与自绘标题栏（TitleBar）

**TitleBarStyle**（`window/title_bar_style.h`）：CSD 标题栏样式值类型——height (默认36)
/bg_active/bg_inactive/fg_active/fg_inactive/hover_tint/close_hover/button_layout
(`TitleBarButtonLayout{Adwaita,Windows,Mac}`，默认 Adwaita)
/show_minimize/show_maximize/show_close/show_title/center_title；预设 `adwaita_dark()/adwaita_light()/windows_dark()`。挂载于
`WindowStyleOptions::title_bar`，运行期经 `Surface::set_title_bar_style` 热更。 **几何单一来源**：
`title_bar_geometry(width, style, maximized, resizable)` 纯函数返回各按钮/图标/标题矩形（隐藏=空盒）；绘制与命中测试共用，规则唯一权威见其实现注释。
**Surface 新虚函数**：`set_title_bar_icon(std::shared_ptr<Image>)` 图标槽；
`begin_window_move()/begin_window_resize(WindowResizeEdge)` 控件发起拖拽/缩放——须在指针按下事件派发栈内同步调用（Wayland
xdg move/resize 的 serial 时效约束）。Win32 以 HTCAPTION/HT* 伪装 NC 拖拽实现。 **交互增强**（Wayland
CSD）：右键标题栏弹合成器系统菜单（xdg_toplevel_show_window_menu）；全屏隐藏+顶边悬停揭示；失焦变暗；悬停高亮。
**content_inset ()**：top 由 style.height 驱动（默认 36，原硬编码 32）；FullScreen 恒 0（揭示条为覆盖层不回流布局）。
**WindowChrome 服务**（`window/window_chrome.h`）：经 Environment
注入的窗口动作门面（close/minimize/toggle_maximize/set_fullscreen/begin_move/begin_resize/content_inset），present_root
注入根环境，供控件消费（headless 安全 no-op）。 **TitleBar 控件**（`widget/title_bar.h`）：声明式标题栏——icon
位图/title/subtitle/add_action 文本 chips/window_controls 内置钮/Snap 弹窗（降级版：内置最大化还原/最小化/全屏/关闭 +
add_snap_action 自定义项；⚠️ xdg-shell 客户端无法自我定位，真半屏平铺原生 Wayland
不可实现）；空白区拖拽/双击最大化/失焦变暗/describe_static/serialize 往返（自定义 Snap 动作除外）。v1 决策：槽位非任意
Node；视觉仅 Adwaita 形态。

#### #H.5 主题（Theme）与国际化（i18n）

- **`Theme`**：扁平设计令牌。`Theme{ .background, .primary, .on_primary, .text, .font }`；`Theme::light()` /`Theme::dark()`
  提供默认主题。经 `ThemeProvider` 注入，组件用 `ctx.environment<Theme>()` 读取。
    - **命名令牌层（T2，叠加于扁平字段，不改注入机制）**：`Theme::set_token(name, TokenValue)` 登记命名令牌，`TokenValue` 可承载
      `Color` / `Font` / `double`(dp) 三态之一；`token(name)` 返回 `std::optional<TokenValue>`，
      `token_or<T>(name, fallback)` 强转目标类型并在缺失/类型不匹配时回退。令牌随 `ThemeProvider` 注入整体传递，后代经
      `environment<Theme>()` 读到的 `Theme` 即含令牌表。
    - **`StyleProps`（T2）**：轻量样式叠加结构，字段（`background` / `foreground` / `font` / `corner_radius` / `padding`）均为
      `TokenOr<T>` 两态（可填「具体值」或「令牌名」）。`StyleProps::resolve(theme)` 把全部两态字段解析为 `ResolvedStyle`
      具体值：令牌名经 `Theme` 查询且类型匹配则返回，否则回退 fallback。组件可在绘制前用最近祖先 `Theme` 解析 `StyleProps`
      ，实现「令牌驱动的样式」而无需逐一手写扁平字段读取。
- **`LocalizedString`**：可本地化的字符串。`LocalizedString::tr(key)`（按 key 查表）或 `LocalizedString{ "字面量" }`（原样显示）；
  `resolve(Locale*, StringTable*)` 解析为最终文本。所有文本属性类型为 `Reactive<LocalizedString>`，因此 `.content = "Hi"`与
  `.content = au::LocalizedString::tr("greeting")` 等价。
- **`StringTable`**：字符串表（key → 各语言文本）。`default_string_table()` 返回内置表；`StringTable::add(key, locale, text)`
  注册。
- **`Locale`**：语言 / 地区标识（如 `"zh-CN"`），随 `LocaleProvider` 注入，文本解析时按当前 Locale 查表。

```cpp
au::Button(au::ButtonProps{ .label = au::LocalizedString::tr("save") });   // 按当前 Locale 解析
au::Text(au::LocalizedString::tr("greeting"));
```
