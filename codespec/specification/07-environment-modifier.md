# 环境注入、主题与修饰符（environment / theming / i18n / modifier）

> 覆盖 `include/aurora/environment/`（3 个头）、`theming/`（4 个头）、`i18n/`（3 个头）、`modifier/`（6 个头）。
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
| `Environment` | 类型 → 值的不可变映射。`env.with<T>(value)` 返回注入了 `T=value` 的**新** `Environment`；`env.get<T>()` 读取 |
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
| `scale_factor` | 设备像素比（由 `Window::present_root` 从 `Surface::scale_factor()` 注入到 `BuildContext::scale_factor`，是本地坐标缩放的**唯一权威来源**） |
| `text_scale_factor` | 系统字体缩放（辅助功能） |
| `orientation` | 由 `screen_size` 派生：`Portrait`（高 ≥ 宽）/ `Landscape`（宽 > 高）。枚举 `ScreenOrientation{Portrait, Landscape}`，与 `divider.h` 的 `Orientation` 语义不同，**不复用** |
| `screen_size` | 物理屏幕的逻辑尺寸（dp） |
| `platform` | 编译期常量——Win32 下为 `Windows`，其余 `Unknown`（不做运行时 OS 探测）。枚举含 `Unknown/Windows/macOS/Linux/Web` |
| `device` | 编译期常量——Win32 下为 `Desktop`，其余 `Unknown`。枚举含 `Unknown/Desktop/Mobile/Tablet` |
| `padding` | 安全区（刘海 / 状态栏）内边距（dp），默认 0 |
| `prefer_reduced_motion` | 系统「减弱动效」偏好 |

`PlatformKind` / `DeviceKind` 反映**编译目标**平台（编译期常量），非运行期 OS 探测；运行期能力探测走 `au::platform().capabilities()`（见 [`06-app-platform.md`](06-app-platform.md) §5）。

**客户端自绘装饰的安全区**：Wayland 客户端自绘装饰（CSD）标题栏 / 边框占用的区域经 `Surface::content_inset()` 并入 `padding`，子树据其为内容留白以避开自绘装饰——对齐 Flutter `MediaQuery.padding` / `SafeArea` 范式。

**工厂**

| 工厂 | 说明 |
|:---|:---|
| `MediaQuery::of(float scale)` | 轻量构造，仅设缩放因子 |
| `MediaQuery::from_surface(const Surface&)` | 跨平台成型：非 Windows 取 Surface 尺寸与默认值，并把 `s.content_inset()` 并入 `padding`；Win32 取真实屏幕逻辑尺寸与减弱动效 |
| `MediaQuery::of(const BuildContext&)` | 读最近祖先 Provider，无则返回进程级默认实例 |

**读取 API**（`MediaQueryProvider` 是 `using` 别名，无法加静态 `of`，故提供自由函数）：

- `media_query_of(const BuildContext&) -> const MediaQuery*`：向上查找最近注入的 `MediaQuery`；无 Provider 时返回 `nullptr`，调用方按需降级。
- `MediaQuery::of(const BuildContext&) -> const MediaQuery&`：便捷封装，无 Provider 时诊断并返回默认实例。

### 3.2 自动注入

`Window::present_root` 每帧自动以 `MediaQuery::from_surface(*surface)` 在**根 `BuildContext`** 注入 `MediaQuery`（存入稳定的 `Window::m_root_env`，地址恒定，避免子树 Provider 持悬空父指针）。

因此**无需手动包裹 `MediaQueryProvider`**，整棵树（含根 widget 自身）即可经 `media_query_of(ctx)` / `MediaQuery::of(ctx)` 读取设备上下文。`m_root_env` 每帧重建以反映窗口 resize。`Application::run` 经同一 `present_root` 自动受益。手动 `MediaQueryProvider` 仍按「最近祖先优先」覆盖此默认值。

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
| `Auto` | 优先协商服务端装饰（SSD，原生标题栏）；compositor 不支持时回退客户端自绘（CSD）标题栏 + 边框兜底 |
| `ServerSide` | 强制 SSD；不可用时退化为 CSD 兜底（避免无装饰且不可操作） |
| `ClientSide` | 强制自绘 CSD 标题栏（即便 compositor 支持 SSD） |
| `Borderless` | 无标题栏，但保留可拖拽缩放边框；移动靠**修饰键拖拽**（按住 `Super` / `Alt` 拖拽任意处 → `xdg_toplevel_move`） |
| `Frameless` | 完全无装饰，由应用自绘 UI 并经程序化窗口控制驱动状态 |

**安全区**：CSD 标题栏 / 边框占用区经 `Surface::content_inset()` 暴露，并入 `MediaQuery.padding`。

**程序化窗口控制**（`Surface` / `Window` 虚函数，默认空实现）：`close()`、`minimize()`、`toggle_maximize()`、`set_fullscreen(bool)`。Wayland 经 `xdg_toplevel` 协议生效，使无标题栏 / 无边框窗口也能由应用按钮驱动状态。

### 4.2 TitleBar

**`TitleBarStyle`**（`window/title_bar_style.h`）是 CSD 标题栏样式值类型，字段：`height`（默认 36）、`bg_active`、`bg_inactive`、`fg_active`、`fg_inactive`、`hover_tint`、`close_hover`、`button_layout`（`TitleBarButtonLayout{Adwaita, Windows, Mac}`，默认 `Adwaita`）、`show_minimize`、`show_maximize`、`show_close`、`show_title`、`center_title`。预设：`adwaita_dark()` / `adwaita_light()` / `windows_dark()`。

挂载于 `WindowStyleOptions::title_bar`，运行期经 `Surface::set_title_bar_style` 热更。

**几何单一来源**：纯函数 `title_bar_geometry(width, style, maximized, resizable)` 返回各按钮 / 图标 / 标题矩形（隐藏 = 空盒）；绘制与命中测试共用，规则以其实现注释为唯一权威。

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

所有文本属性类型为 `Reactive<LocalizedString>`，因此 `.content = "Hi"` 与 `.content = au::LocalizedString::tr("greeting")` 等价。

```cpp
au::Button(au::ButtonProps{ .label = au::LocalizedString::tr("save") });
au::Text(au::LocalizedString::tr("greeting"));
```

---

## 7 Modifier 修饰系统

### 7.1 概述

`Modifier` 用一组**正交、可组合**的修饰符表达内边距、背景、可点击、尺寸、边框、裁剪、对齐、偏移、拖拽等横切能力，替代继承爆炸。

`Modifier` 是**不可变值**：每个方法返回新副本，可链式组合并赋值给任意组件的 `.modifier` 属性。

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

### 7.2 工厂清单

全部返回 `Modifier`，可链式：

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
| `.align(Alignment)` | 在父约束内的对齐 |
| `.offset(float dx, float dy)` | 绘制期平移（Transform 切片） |
| `.rotate(float degrees)` | 绕内容中心旋转（Transform 切片，仿射矩阵） |
| `.scale(float sx, float sy)` / `.scale(float s)` | 绕内容中心缩放（Transform 切片） |
| `.transform(Matrix2D)` | 应用任意 2×3 仿射矩阵（Transform 切片，绕内容中心） |
| `.expand(float weight = 1)` | 在 Flex 主轴占权重（产生 `FlexWeight` 节点） |
| `.gradient_linear(from, to, angle_deg = 0)` / `.gradient_linear(colors, stops, angle_deg)` / `.gradient_radial(center, edge)` | 线性（双色/多色标）/ 径向渐变背景 |
| `.shadow(offset_x = 0, offset_y = 2, blur = 4, color = 黑色 25%)` | 投影阴影（绘制于内容之下） |
| `.blur(float radius)` | 内容模糊：子树绘制完成后对整个内容盒做高斯近似模糊 |
| `.backdrop_filter(float radius)` | 背景滤镜（毛玻璃）：绘制内容前先模糊内容盒背后已绘像素，配合半透明 `background` |
| `.blend_mode(...)` / `.shader_mask(...)` / `.cache_layer(...)` | 像素混合 / 渐变遮罩 / 离屏缓存（`BlendMode` / `ShaderMaskKind` 枚举见 `render/blend.h`） |
| `.draggable(...)` / `.long_press(...)` | 手势（单指，由 `Draggable` / `LongPress` 修饰节点驱动） |
| `.touch(on_touch)` | 原始多点触摸流回调（`TouchListener` 节点，不消费命中） |
| `.tooltip(std::string, float delay_ms = 500)` / `.context_menu(std::vector<MenuItem>)` | 悬停提示气泡 / 右键上下文菜单 |
| `.then(N node)` | 就地追加任意 `ModifierNode` |

### 7.3 四类切片与绘制顺序

修饰节点分为四类切片，按固定顺序作用于布局 / 绘制 / 输入：

| 切片 | 成员 | 头文件 |
|:---|:---|:---|
| `Layout` | `Padding` / `PaddingEdges` / `FlexWeight`（`.expand()` 产生的节点，无独立 `Expand` 节点）/ `SizeModifier` | `modifier/modifier_layout.h` |
| `Paint` | `Background` / `GradientBackground` / `ShadowNode` / `BlendNode` / `ShaderMaskNode` / `CacheLayerNode` / `Border` / `Clip` / `ClipRounded` / `OpacityNode` / `BlurNode`（`.blur()` 与 `.backdrop_filter()` 共用，按标志位区分） | `modifier/modifier_paint.h` |
| `Input` | `Clickable` / `Draggable` / `LongPress` / `TouchListener` / `TooltipNode` / `ContextMenuNode` | `modifier/modifier_input.h` |
| `Transform` | `AlignNode` / `OffsetNode` / `TransformNode`（`.rotate()` / `.scale()` / `.transform()` 共用，按构造参数区分） | `modifier/modifier_transform.h` |

`Transform` 切片累积为单个 `Matrix2D`（绕内容中心构造），绘制时整树离屏合成、命中测试用逆矩阵映射本地坐标；`Opacity` 复用 `Painter::set_alpha`。

### 7.4 三段式修饰绘制

`Widget::paint` 按以下顺序处理修饰：

1. **先绘阴影 / 背景毛玻璃（backdrop）**——须在圆角裁剪之前，以免外扩羽化被切。
2. **再压栈裁剪**（Clip / ClipRounded，与既有裁剪栈取交集）。
3. **随后绘背景 / 渐变背景**——因圆角裁剪已生效，背景随 `clip_rounded` 呈圆角（对齐 Flutter `ClipRRect` 语义：圆角裁剪作用于控件绘制的一切内容，含自身背景）。

**作用范围**：Paint 切片（背景、边框、阴影、裁剪、后效）统一作用于控件完整视觉盒子 `visual_box`；子节点 `on_paint` 与内容后效则限定在 `content_box`（已扣除 Padding / Align 等布局内边距）。内容后弹栈并绘制边框。

> 两种历史错误形态：把背景先于裁剪当作直角矩形填色；把 Paint 修饰限制在 `content_box` 导致 padding 区域露白。回归用例 `tests/test_clip_rounded_background.cpp` 覆盖这两种情况。

### 7.5 与固有属性的关系

控件可配置性由两层构成：固有属性（`XxxProps` 字段）描述身份并随控件序列化；`Modifier` 承载跨切面装饰。重叠能力以**固有属性优先**，`Modifier` 同类项保留用于「给任意控件套一层」的场景。绘制时 `Modifier` 在外、固有属性在内。

---

## 8 需求规格

### 8.1 #12 机器可读 API Schema

**控件自描述侧的契约**：各控件提供**静态** `describe_static()`；虚 `describe()` 在基类 `Widget` 已有默认实现（返回 `{ .name = type_name() }`，`widget.h:401`），无需富描述的控件可省略 override，仅在需要补充 properties / events / `children_policy` 等元数据时覆写（与 [`04-widget.md`](04-widget.md) §2.1 的表述一致）。`component_schema()` / `list_all_schemas()` 消费 `describe()` 输出；`aurora_api.json` 自动包含增强字段。

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
