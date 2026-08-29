# Aurora 概念地图

> 面向 AI 编码助手的概念认知与翻译表。
> 本文件是核心概念审计（全部可枚举 UI 原语）与跨框架概念映射的**唯一权威**；架构与设计原则见 [`ARCHITECTURE.md`](ARCHITECTURE.md) §13，各原语的 API 契约见 `specification/` 八份子系统文档。

---

## 1 核心概念审计

目标：Aurora 的全部 UI 原语可枚举、可命名、可映射，统一审计，便于 AI 检索与迁移训练。以下为当前库的真实原语盘点（头文件见 `include/aurora/`）。

| # | 概念 | 成员 | 头文件 |
|---:|:---|:---|:---|
| 1 | **Widget（原子控件）** | `Text` `TextInput` `Button` `Image`（`ImageView`） `Checkbox` `Switch` `ProgressIndicator` `Slider` `Canvas` `Skeleton` `VideoPlayer` `VideoControls` `BottomNavBar` | `widget/text.h` `widget/text_input.h` `widget/button.h` `widget/image_widget.h` `widget/checkbox.h` `widget/switch.h` `widget/progress.h` `widget/slider.h` `widget/canvas.h` `widget/skeleton.h` `media/video_player.h` `media/video_controls.h` `widget/bottom_nav_bar.h` |
| 2 | **Container（多子布局）** | `Column` `Row` `Stack` `Grid` `GridView` `Scroll` `Spacer` `Repeater` `LazyList` `LazyRow` | `widget/containers.h` `widget/stack.h` `widget/grid.h` `widget/grid_view.h` `widget/scroll.h` `widget/spacer.h` `widget/repeater.h` `widget/lazy_list.h` `widget/lazy_row.h` |
| 3 | **Modifier（声明式装饰）** | `Padding` `Background` `Border` `Clip` `Opacity` `SizeModifier` `FlexWeight` `Clickable` `Align` `Offset` `Blur`（blur / backdrop_filter / shadow） `BlendMode` `ShaderMask` `CacheLayer` `Draggable` `LongPress` `Rotate` `Scale` `Transform` | `modifier/modifier.h` `render/blend.h` |
| 4 | **Control Flow（组合 / 条件）** | `Show` `Timer` | `widget/show.h` `widget/provider.h` `environment/*.h` `widget/timer.h` |
| 5 | **State（响应式状态）** | `State<T>` `Binding<T>` `Store<T>` `Reactive<T>` | `state/state.h` `state/binding.h` `state/store.h` `state/reactive.h` |
| 6 | **Derived（派生值）** | `Computed<T>` `Effect` | `state/computed.h` `state/effect.h` |
| 7 | **Signal（细粒度订阅）** | `SignalView` / `Subscription`（`SignalViewBase`） | `state/signal_view.h` `state/subscription.h` |
| 8 | **Theme（主题）** | `Theme` `ThemeProvider` `colors` | `theming/theme.h` `widget/provider.h` |
| 9 | **Environment（跨树注入）** | `Environment` `Provider<T>` `LocaleProvider` `MediaQueryProvider` `MediaQuery` | `environment/*.h` |
| 10 | **Navigation（路由）** | `Navigator` `Route` `RouteRegistry` `TransitionLayer` `RouteTransition` `Hero`（共享元素转场） `open_uri`（深层链接） | `navigation/navigator.h` `navigation/route.h` `navigation/hero.h` `navigation/transition_layer.h` |
| 11 | **Layout（布局引擎）** | `Constraints` `EdgeInsets` flex / grid 求解；含 `RelayoutBoundary` 重排边界（`Widget::is_relayout_boundary()`），其 paint 侧孪生为 `CacheLayer` 修饰 | `layout/*.h` `core/types.h` |
| 12 | **Event（事件）** | `MouseEvent` `KeyEvent` `ScrollEvent` `TextInputEvent` `TouchEvent` `FileDropEvent` `EventDispatcher` `TouchDispatcher` `FocusManager` | `event/event.h` `event/dispatcher.h` `event/focus.h` |
| 13 | **Animation（动画）** | `Tween<T>` `Keyframes<T>` `Curve` `Animator` `SpringSimulation` `AnimationController` `AnimatedValue<T>` | `animation/*.h` |
| 14 | **Platform Shell（平台 Shell）** | `FileDialog`（open_file / save_file / open_folder） `SystemTray`（show / hide / set_icon / show_balloon / on_activate） `Clipboard`（set_text / get_text / set_image / get_image） `FileDropEvent` + `Widget::on_file_drop` `Display` + `list_displays` / `primary_display` / `display_containing` / `move_window_to_display` | `app/file_dialog.h` `app/system_tray.h` `app/clipboard.h` `app/display.h` `event/event.h` |
| 15 | **Accessibility（无障碍）** | `AccessibilityNode` `AccessibilityRole` `AccessibilityAction` `infer_accessibility_role` `build_accessibility_tree` | `core/accessibility.h` |
| 16 | **DevTools（开发工具）** | `HotReload` `generate_ui` `validate_ui` `Diagnostics`（report / warn / degraded） `inspect`（dump_tree / query / get_state） `Logger` | `app/hot_reload.h` `app/generate_ui.h` `app/validate_ui.h` `core/diagnostics.h` `widget/inspect.h` `core/log.h` |
| 17 | **Render（渲染）** | `Painter` `Surface` `HeadlessSurface` `Win32Surface` `D3D11Surface` `GlfwSurface` `X11Surface` `WaylandSurface` `MacOSSurface` `WasmSurface` `create_window`（工厂） `auto_detect_surface` | `render/painter.h` `window/*.h` |
| 18 | **Result / Error（错误）** | `Result<T>` `Error` | `core/result.h` `core/error_codes.h` |
| 19 | **Lifecycle / Window（生命周期）** | `Lifecycle`（控件挂载 / 卸载副作用） `WindowState`（Visible / Occluded / Hidden） `WindowMode`（Normal / Maximized / Minimized / FullScreen） | `widget/lifecycle.h` `window/window_state.h` |

### 1.1 序列化工厂注册状态

`LazyList` / `LazyRow` / `BottomNavBar` 已接入 `register_core_widgets()`（JSON 工厂名 `"LazyList"` / `"LazyRow"` / `"BottomNavBar"`，C++ 便利构造器 `ui::lazy_list(...)` / `ui::lazy_row(...)` / `ui::bottom_nav_bar(...)`）。

`Skeleton` 与 `GridView` 已有头文件与类型，但**尚未接入序列化工厂**，因此暂不可经 JSON 反序列化还原（二者仍可正常构造与渲染）。

`X11Surface` / `WaylandSurface` / `MacOSSurface` / `WasmSurface` 的 CMake 开关均已接入（`AURORA_BACKEND_X11` / `AURORA_BACKEND_WAYLAND` / `AURORA_BACKEND_MACOS` / `AURORA_BACKEND_WASM`，默认均 OFF）。后端能力差异与开关默认值见 [`BUILD_OPTIONS.md`](BUILD_OPTIONS.md)。

### 1.2 重叠能力优先级判定

当同一视觉能力同时存在于「固有属性」与「Modifier」时：

- 控件**自身身份语义**的能力（如 `Button` 的 `background_color`、`corner_radius`、`padding`）→ 用**固有属性**优先：随控件序列化、可被 Inspector 枚举、参与 diff。
- 给**任意控件临时套一层**通用装饰（如给 `Image` 加 `Padding`、给 `Text` 加 `Background`）→ 用 **Modifier**：正交、可叠加、可 `Reactive` 变化，且不污染控件身份。
- 若控件固有属性已提供该能力，则 **Modifier 同类项不再重复写**，仅作为「跨控件通用兜底」。

| 重叠能力 | 固有属性（控件内） | Modifier（正交链） | 判定边界 |
|:---|:---|:---|:---|
| `padding` | `XxxProps::padding`（随控件序列化） | `Padding` 修饰 | 控件自带留白用固有；跨控件统一留白用 Modifier |
| `corner_radius` | `XxxProps::corner_radius` | `Clip` / `Border` 修饰 | 控件圆角用固有；给任意矩形切圆角用 Modifier |
| `background_color` | `XxxProps::background_color` | `Background` 修饰 | 控件底色用固有；叠加高亮 / 状态色用 Modifier |

---

## 2 状态作用域决策树

面向 AI 编码助手的快速决策规则：在有限上下文内正确选择 `State` / `Store` / `Binding` / `Computed`。

```text
问：该值的使用范围？
│
├─ 仅本控件内部使用 ─────────────────► State<T>（组件内状态）
│
├─ 父子 / 兄弟共享 ──────────────────► 提升到最近公共祖先的 State<T> + 经 Binding<T> 下发给子组件
│
├─ 跨不相关子树（全局 / 跨页面） ────► Store<S> + Environment 注入
│
└─ 纯派生值（可由其他状态计算） ─────► Computed<T>（不存储，纯函数计算）
```

### 2.1 生命周期规则

| 类型 | 所有权 | 生命周期 | 清理时机 |
|:---|:---|:---|:---|
| `State<T>` | 通常 `shared_ptr` 或栈上 | 随创建者管理 | 栈上随作用域；`shared_ptr` 随引用计数归零 |
| `Store<S>` | 通常 `shared_ptr` 全局单例 | 进程级（或应用级） | 进程退出 / `shared_ptr` 释放 |
| `Binding<T>` | **非拥有**（裸指针指向上游 `State`） | 上游 `State` 须更长存活 | 无自身清理；上游析构后不可再访问 |
| `Computed<T>` | 自管理（内部持有 `Effect`） | 依赖源全部存活即可 | 依赖源析构后 Effect 自动惰性摘除 |

**关键约束**：`Binding` 不拥有上游——传递给子组件时，父组件须保证 `State` 的存活期 ≥ 子组件。

### 2.2 反例：Binding 跨子树独立存活

当子组件可能独立于父组件销毁（如经 `Navigator` push 的页面、或异步加载的面板），裸 `Binding<T>` 借用的上游 `State` 会悬空。此场景应改用 `shared_ptr<State<T>>` 共享所有权：

```cpp
// 错误：子页面关闭后 flag 随父析构，Binding 悬空
// au::Binding<bool>{ *parent_flag }   // parent_flag 为栈上 / 短生命周期 State

// 正确：以 shared_ptr 共享所有权，引用计数保活
auto flag = std::make_shared<au::State<bool>>(true);
// 经 Environment 注入或参数下发 shared_ptr；子组件用 Reactive 包装
push_route(au::Checkbox{ au::Reactive<bool>{ flag } });   // flag 存活期 = 最长引用者
```

---

## 3 与 React / Flutter / Qt 的概念映射

面向从 React / Flutter / Qt 迁移的开发者。下两表覆盖与三框架有清晰对应关系的 UI 原语与基础设施概念。

### 3.1 主要 UI 原语映射

| Aurora | React | Flutter | Qt (QML/C++) |
|:---|:---|:---|:---|
| Widget / Node | 函数组件返回值 / JSX element | `Widget`（Stateless / Stateful） | `Item` / `QWidget` |
| `State<T>` / Signal | `useState` / `useSignal` | `ChangeNotifier` + `setState` | `Q_PROPERTY` + 信号槽 |
| `Store<T>` | `useContext` / Redux store | `InheritedWidget` / `Provider` | 单例 / 上下文对象 |
| `Modifier` | 组合（无直接等价，靠 props） | `Container` / `DecoratedBox` | 属性 / `Item` 嵌套 |
| `BuildContext` / `Environment` | `Context` / `Provider` | `BuildContext` + `InheritedWidget` | 上下文属性注入 |
| `Painter` | `<canvas>` 2D | `CustomPainter` | `QPainter` |
| `Surface` / `Window` | DOM / `react-dom` | `Window` / `Surface` | `QWindow` / `QQuickWindow` |
| 视频播放 / 媒体 | `<video>` / `react-player` | `video_player` / `ExoPlayer` | `QMediaPlayer` + `QVideoWidget` |
| Layout（Flex） | Flexbox | `Flex` / `Row` / `Column`（`MainAxisAlignment` / `CrossAxisAlignment` / `MainAxisSize` 与 Flutter 同名概念一一对应） | `Row` / `Column` 布局 |
| `RelayoutBoundary` | 无直接等价（`React.memo` / `useMemo` 仅影响重渲染而非布局冒泡） | `RelayoutBoundary`（同源概念：截断 layout 脏冒泡、仅重排本子树；`isRepaintBoundary` 是其 paint 侧孪生） | 无直接等价（`QLayout` 无重排边界语义，整树重算） |
| Animation | `react-spring` / Framer | `AnimationController` + `Curve` | `PropertyAnimation` |
| 定时任务（`Timer` / `Scheduler`） | `setTimeout` / `setInterval` | `Timer.periodic` / `Timer` | `QTimer` |
| `Navigator` | React Router | `Navigator` / `Navigator 2.0`（`Hero` 共享元素转场、`open_uri` 深层链接对应 Flutter `Hero` / 路由 URI 解析） | `StackView` / 路由 |
| `MediaQuery` / `LayoutBuilder` | 媒体查询 hook（无直接内置）/ 条件渲染 | `MediaQuery` + `LayoutBuilder` | 屏幕度量 / `LayoutBuilder` 等价模式 |

**视频播放器定制**：`VideoPlayer` 可子类化（`create_default_controls()` 虚化并在挂载期生效、`current_frame()` / `paint_frame()` 读 / 绘帧、`on_pointer_event` / `wants_click` 为 public）；`VideoControls` 可子类化换肤 / 重排（`play_button()` / `time_text()` / `mute_button()` 访问器 + `build_children()` 虚函数）。详见 [`specification/03-layout-render.md`](specification/03-layout-render.md) §9.2。

### 3.2 基础设施与平台映射

| Aurora | React | Flutter | Qt (QML/C++) |
|:---|:---|:---|:---|
| `Diagnostics` | Error Boundary + 控制台 | 调试断言 / `debugFillProperties` | 警告输出 |
| Lifecycle（控件挂载 / 卸载副作用） | `useEffect(…, [])` + cleanup 返回 | `StatefulWidget.initState` + `dispose` | `QObject` 父子 RAII / `QWidget` 事件（无内置钩子，靠析构） |
| 窗口生命周期 `WindowState` / `WindowMode` | `document.visibilityState` / `window` focus-blur（仅应用级） | `WidgetsBindingObserver.didChangeAppLifecycleState` + 窗口尺寸状态 | `QWindow::windowStateChanged` / `QApplication::applicationStateChanged` |
| `TitleBar`（自绘标题栏 / CSD） | 无原生对应 ≈ 社区 window chrome 方案 | `AppBar` + `window_manager` | `QQuickWindow` headerBar 或 `KWindowSystem` |

> 与 §3.1 并列于同一概念清单、仅因跨框架直接等价较少而单独成表的基础设施 / 平台层概念：Event（#12）、Platform Shell（#14）、Accessibility（#15）、DevTools（#16）、Result / Error（#18）。

### 3.3 生命周期：两级正交

Aurora **不引入**安卓 `Activity` 的 `onCreate/onStart/onResume/onPause/onStop/onDestroy` 命令式生命周期——它面向 OS 窗口可见性、与声明式树模型冲突，且 per-widget 的 resume / pause 无实际意义。改为两级正交机制：

- **控件子树级 `au::Lifecycle`**：包裹子树，`on_mount` 挂载后恰好一次（可注册外部源、读 `ctx.environment<T>()`），`on_unmount` 在控件 `Node` 析构时清理（RAII，覆盖 `Repeater` 缩容 / `Navigator` pop）。对应 Android `View.onAttachedToWindow` / `onDetachedToWindow`，而非 `Activity`。`Show` 隐藏保留子树存活（不析构、不卸载），对齐 Flutter `Visibility`。
- **窗口 / 应用级 `WindowState`（Visible / Occluded / Hidden）+ `WindowMode`（Normal / Maximized / Minimized / FullScreen）**：仅窗口层报告，由后端经 `Surface` 句柄上报，`Application` 暴露响应式 `State` 与命令式回调，并注入根 `Environment`。

最大化是独立几何态，**不并入** `WindowState`；精确「被其他窗口像素级遮挡」检测昂贵且普遍不被框架支持，故以「失焦 = 被遮挡」近似为 `Occluded`。

把「控件在不在树里」与「窗口是否可见 / 失焦」彻底分离，避免在任何控件上挂接 resume / pause 这种窗口语义的钩子。

契约细节见 [`specification/06-app-platform.md`](specification/06-app-platform.md) §6。

### 3.4 声明式工厂语法糖层（`aurora::ui`）

Aurora 的「真值来源」仍是声明式 `Node` 树 + `XxxProps` 聚合属性（`Column{...}`、`Text{...}` 值构造）。`aurora::ui`（别名 `au::ui`）是叠其上的**语法糖**：一组工厂函数把「构造 + 加父 + 返回强类型指针」三步合一，进一步压缩 AI 生成代码的 token 与出错面。

它**不引入新控件类型**，底层仍是 `Text` / `Button` / `Column` 等，与概念映射表完全一致。

---

## 4 迁移要点

- **声明式 + 不可变树**：Aurora 组件树与 React / Flutter 一致，状态变更触发局部重渲染；不要用命令式方式事后改树，应通过 `State<T>` 驱动。
- **响应式以 `State<T>` / `Signal` 为中心**：替代 React 的 `useState` 与 Flutter 的 `setState` / `ChangeNotifier`，粒度更细（细粒度信号避免整树重绘）。
- **订阅清理用 `aurora::Subscription`（RAII）**：把信号 / `Store` 订阅包成对象，作用域结束自动取消，等价于 React `useEffect(…, [])` 的 cleanup 返回；`bind(src, fn)` 直接返回该句柄，避免手动保存 / 调用取消句柄导致监听器泄漏。
- **环境注入走 `Environment`**：主题 / locale / 媒体查询通过 `ctx.environment<T>()` 读取，等价于 React Context 与 Flutter `InheritedWidget`。设备度量（`MediaQuery`）由 `Window::present_root` **自动注入根 `BuildContext`**（每帧按 `from_surface` 重建），故无需手动包裹 `MediaQueryProvider` 即可经 `media_query_of(ctx)` / `MediaQuery::of(ctx)` 读取；如需覆盖特定子树，仍可在该子树根显式包 `MediaQueryProvider`（最近祖先优先）。结合 `LayoutBuilder(builder)` 按布局约束动态构建子树，等价于 Flutter 的 `MediaQuery` + `LayoutBuilder` 响应式写法。
- **平台无关渲染**：`Painter` 是纯软件栅格，无 GPU 依赖；`HeadlessSurface` 可直接出 PNG，便于测试与无头渲染。
- **降级而非中止**：非法输入 / 部分代码缺失产出 `Diagnostics` 并降级到安全默认，而非抛异常崩溃（与 React Error Boundary 哲学一致）。

---

## 5 状态选择示例

### 5.1 组件内 State：Checkbox 的 checked 状态

```cpp
auto checked = std::make_shared<au::State<bool>>(false);
au::Checkbox cb{ au::Reactive<bool>{ checked } };
// checked 随父组件析构自动释放
```

```json
{ "type": "Checkbox", "props": { "checked": false } }
```

### 5.2 状态提升 + Binding：父组件持有 State，两个子组件共享

```cpp
auto shared_flag = std::make_shared<au::State<bool>>(true);
au::Checkbox cb_a{ au::Binding<bool>{ *shared_flag } };
au::Checkbox cb_b{ au::Binding<bool>{ *shared_flag } };
shared_flag->set(false);   // cb_a 与 cb_b 同时刷新
```

### 5.3 Store 集中管理：购物车状态

```cpp
struct Cart { std::vector<std::string> items; };
auto cart = au::make_store<Cart>(
    Cart{},
    [](const Cart &s, const au::Action &a) -> Cart {
        Cart next = s;
        if (a.type == "add") {
            if (auto *name = a.payload_as<std::string>()) next.items.push_back(*name);
        } else if (a.type == "clear") {
            next.items.clear();
        }
        return next;
    }
);
cart->dispatch(au::Action{ "add", std::string("Aurora Book") });
// 任意子树经 cart->as_signal() 订阅变化
```

### 5.4 Environment 注入：主题色

```cpp
auto theme = au::Theme::light();
theme.primary = au::colors::AURORA_BLUE;
au::Node root = au::ThemeProvider{ theme, au::Button(au::ButtonProps{ .label = "主题按钮" }) };
// 子组件内部：const Theme* t = ctx.environment<Theme>();   // 最近祖先优先
```

### 5.5 Computed 派生：过滤后的列表

```cpp
auto source  = std::make_shared<au::State<std::vector<std::string>>>(
    std::vector<std::string>{"Apple", "Banana", "Avocado", "Cherry"});
auto keyword = std::make_shared<au::State<std::string>>(std::string("A"));
auto filtered = au::computed([source, keyword]() -> std::vector<std::string> {
    std::vector<std::string> result;
    for (const auto &s : source->get()) {
        if (s.find(keyword->get()) != std::string::npos) result.push_back(s);
    }
    return result;
});
// filtered.get() == {"Apple", "Avocado"}
// keyword->set("Ch");  → filtered 自动重算 → {"Cherry"}
```
