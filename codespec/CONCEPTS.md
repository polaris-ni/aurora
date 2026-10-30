# Aurora 概念地图

> 面向 AI 编码助手的概念认知与翻译表。
> 本文件是核心概念审计（全部可枚举 UI 原语）与跨框架概念映射的**唯一权威**；架构与设计原则见 [`ARCHITECTURE.md`](ARCHITECTURE.md) §13，各原语的 API 契约见 `specification/` 八份子系统文档。

---

## 1 核心概念审计

目标：Aurora 的全部 UI 原语可枚举、可命名、可映射，统一审计，便于 AI 检索与迁移训练。以下为当前库的真实原语盘点（头文件见 `include/aurora/`）。

| # | 概念 | 成员 | 头文件 |
|---:|:---|:---|:---|
| 1 | **Widget（原子控件）** | `Text` `TextInput` `Button` `Image`（`ImageView`） `Checkbox` `Switch` `ProgressIndicator` `Slider` `Canvas` `Skeleton` `VideoPlayer` `VideoControls` `BottomNavBar` | `widget/text.h` `widget/text_input.h` `widget/button.h` `widget/image_widget.h` `widget/checkbox.h` `widget/switch.h` `widget/progress.h` `widget/slider.h` `widget/canvas.h` `widget/skeleton.h` `media/video_player.h` `media/video_controls.h` `widget/bottom_nav_bar.h` |
| 2 | **Container（多子布局）** | `Column` `Row` `Stack` `Grid` `GridView` `Scroll` `PullToRefresh` `StickyHeader` `Spacer` `Repeater` `LazyList` `LazyRow` | `widget/containers.h` `widget/stack.h` `widget/grid.h` `widget/grid_view.h` `widget/scroll.h` `widget/pull_to_refresh.h` `widget/sticky_header.h` `widget/spacer.h` `widget/repeater.h` `widget/lazy_list.h` `widget/lazy_row.h` |
| 3 | **Modifier（声明式装饰）** | `Padding` `PaddingEdges` `FlexWeight` `SizeModifier` `Background` `GradientBackground` `ShadowNode` `BlendNode` `ShaderMaskNode` `CacheLayerNode` `Border` `Clip` `ClipRounded` `OpacityNode` `BlurNode` `AlignNode` `OffsetNode` `TransformNode` `CursorNode` `Clickable` `Draggable` `LongPress` `TouchListener` `TooltipNode` `ContextMenuNode` | `modifier/modifier_base.h` `modifier/modifier_layout.h` `modifier/modifier_paint.h` `modifier/modifier_transform.h` `modifier/modifier_input.h` |
| 4 | **Control Flow（组合 / 条件）** | `Show` `Repeater` `Provider` `Lifecycle` `Timer` | `widget/show.h` `widget/repeater.h` `widget/provider.h` `widget/lifecycle.h` `environment/*.h` `widget/timer.h` |
| 5 | **State（响应式状态）** | `State<T>` `ReadOnlyState<T>` | `state/state.h` `state/read_only_state.h` |
| 6 | **Derived（派生与订阅）** | `computed(fn)` `effect(fn)` `Subscription` | `state/computed.h` `state/effect.h` `state/subscription.h` |
| 7 | **Async Resource（异步三态）** | `Resource<T>` `StreamResource<T>` `ResourceSnapshot<T>` `StreamSnapshot<T>` `ResourcePhase` | `state/resource.h` |
| 8 | **Theme（主题）** | `Theme` `ThemeProvider` `colors` | `theming/theme.h` `widget/provider.h` |
| 9 | **Environment（跨树注入）** | `Environment` `Provider<T>` `LocaleProvider` `MediaQueryProvider` `MediaQuery` | `environment/*.h` |
| 10 | **Navigation（路由）** | `Navigator` `Route` `RouteRegistry` `TransitionLayer` `RouteTransition` `Hero`（共享元素转场） `open_uri`（深层链接） | `navigation/navigator.h` `navigation/route.h` `navigation/hero.h` `navigation/transition_layer.h` |
| 11 | **Layout（布局引擎）** | `Constraints` `EdgeInsets` flex / grid 求解；含 `RelayoutBoundary` 重排边界（`Widget::is_relayout_boundary()`），其 paint 侧孪生为 `CacheLayer` 修饰 | `layout/*.h` `core/types.h` |
| 12 | **Event（事件）** | `MouseEvent` `KeyEvent` `ScrollEvent` `TextInputEvent` `TextCompositionEvent`（IME 预编辑：preedit / 组合光标 / 待转换选区 / commit，经 `EventDispatcher` 只路由焦点控件的 `on_text_composition`；Win32 已接 IMM32 桥、X11 已接 XIM 桥、Wayland 已接 text-input-v3 桥，余下后端仅契约，接线状态见 [`05-event-navigation.md`](specification/05-event-navigation.md) §2.4） `TouchEvent` `SystemDropEvent`（跨进程系统拖放四态：enter / over / leave / drop，MIME 载荷 + Copy / Move / Link 动作协商） `EventDispatcher` `TouchDispatcher` `FocusManager`（焦点作用域 `push_scope`/`pop_scope`：模态弹层 Tab 陷阱与焦点恢复） | `event/event.h` `event/dispatcher.h` `event/focus.h` |
| 13 | **Animation（动画）** | `Tween<T>` `Keyframes<T>` `Curve` `Animator` `SpringSimulation` `SpringAnimation` `AnimationController` `AnimatedValue<T>` | `animation/*.h` |
| 14 | **Platform Shell（平台 Shell）** | `FileDialog`（open_file / save_file / open_folder） `SystemTray`（show / hide / set_icon / show_balloon / on_activate） `Clipboard`（set_text / get_text / set_image / get_image，四者一律返回 `Result<...>`，失败机器可见、空内容记 `Ok`） `SystemDropEvent` + `Widget::on_system_drop` / `query_system_drag_accept`（跨进程拖放四态与 accept 查询） `Display` + `list_displays` / `primary_display` / `display_containing` / `move_window_to_display` `CursorShape` + `Surface::set_cursor`（悬停光标：`Modifier::cursor(...)` 声明 > `Widget::cursor_shape()` 虚钩子 > Clickable 缺省 PointingHand） | `app/file_dialog.h` `app/system_tray.h` `app/clipboard.h` `app/display.h` `event/event.h` `window/surface.h` |
| 15 | **Accessibility（无障碍）** | `AccessibilityNode`（id / role / name / value / hint / bounds / actions / state / range / level / is_control / is_content / stable_key / labelled_by / labelled_by_id） `AccessibilityRole` `AccessibilityAction` `AccessibilityState` `AccessibilityRange` `AccessibilityTextSelection` `infer_accessibility_role` `build_accessibility_tree`（含几何回填；语义由控件虚钩子 `Widget::accessibility_role` / `accessibility_label` / `accessibility_value` / `accessibility_hint` / `accessibility_state` / `accessibility_range` / `accessibility_level` / `accessibility_text` / `accessibility_selection` / `accessibility_scroll` 自填，可覆写） `Widget::explicit_accessibility_label` / `set_accessibility_label`（宿主侧显式读屏名，对标 `aria-label`） `Widget::stable_key` / `set_stable_key` + `labelled_by_key` / `set_labelled_by`（宿主命名稳定键与引用式标签关联，对标 `id` + `aria-labelledby`，Name 回退链最高优先级、跨重建不变） `Widget::runtime_id()`（稳定身份） `Widget::perform_accessibility_action`（读屏反向操作，默认路由真实事件路径） `Widget::announce` / `notify_accessibility_announcement`（Live Region 动态播报） `AccessibilitySettings`（`reduce_motion` / `high_contrast` / `font_scale` / `screen_reader_active`，经 Environment 注入或进程级默认） `AccessibilityEvent` / `AccessibilityEventKind` / `set_accessibility_event_handler`；平台桥层 `a11y::Provider` + `ProviderRegistry` + `TreeSnapshot` / `diff_snapshots` + `UtfOffsetMap`（Win32 UIA 实现在 `window/detail/win32_ua.*`） | `core/accessibility.h` `core/a11y_types.h` `core/a11y_provider.h` `core/a11y_text.h` `widget/widget.h` `widget/a11y_tree.h` `widget/a11y_diff.h` |
| 16 | **DevTools（开发工具）** | `HotReload` `generate_ui` `validate_ui` `ui_prompt`（prompt 投影 / 确定性机修 / 自修复环） `Diagnostics`（report / warn / degraded） `inspect`（dump_tree / query / get_state / collect_widget_boxes / diff_trees） `Logger` | `app/hot_reload.h` `app/generate_ui.h` `app/validate_ui.h` `app/ui_prompt.h` `core/diagnostics.h` `widget/inspect.h` `core/log.h` |
| 17 | **Render（渲染）** | `Painter` `Surface` `HeadlessSurface` `Win32Surface` `D3D11Surface` `WgpuWin32Surface` `GlfwSurface` `X11Surface` `WaylandSurface` `MacOSSurface` `WasmSurface` `create_window`（工厂） `auto_detect_surface` | `render/painter.h` `window/*.h` |
| 18 | **Result / Error（错误）** | `Result<T>` `Error` | `core/result.h` `core/error_codes.h` |
| 19 | **Lifecycle / Window（生命周期）** | `Lifecycle`（控件挂载 / 卸载副作用） `WindowState`（Visible / Occluded / Hidden） `WindowMode`（Normal / Maximized / Minimized / FullScreen） | `widget/lifecycle.h` `window/window_state.h` |
| 20 | **Command（命令）** | `Command`（id / title / icon / category / action / default_binding / scope / enabled 谓词 / when_label 标签）`CommandRegistry`（注册 / 解绑 / 启停 / 调用 / 模糊检索 / 序列化信封 / 快捷键与菜单投影）`command_fuzzy_score`（子序列打分）`CommandPalette`（模态面板：搜索框 + 结果列表 + Esc/↑/↓/Enter 键位 + 焦点陷阱） | `commands.h` `widget/command_palette.h` |
| 21 | **Audio（音频）** | `AudioContext`（Web Audio 语义节点图：源 / 增益 / 声像 / 分析 / 录音 / 设备）+ `AudioNode` 家族（`AudioDestinationNode` / `AudioStreamSourceNode` / `GainNode` / `PannerNode` / `AnalyserNode` / `AudioBufferSourceNode` / `AudioRecordingDestinationNode` / `AudioMicrophoneSourceNode`）；设备后端 `AudioDeviceBackend` / `AudioCaptureBackend`（WASAPI / ALSA / CoreAudio / Web Audio，经 `AURORA_ENABLE_AUDIO_*` 开关裁剪） | `media/audio.h` |

### 1.1 序列化工厂注册状态

`LazyList` / `LazyRow` / `GridView` / `BottomNavBar` / `Skeleton` 均已接入 `register_core_widgets()`（JSON 工厂名 `"LazyList"` / `"LazyRow"` / `"GridView"` / `"BottomNavBar"` / `"Skeleton"`，C++ 便利构造器 `ui::lazy_list(...)` / `ui::lazy_row(...)` / `ui::bottom_nav_bar(...)`）。

其中 `Skeleton` / `BottomNavBar` 的属性由 `deserialize_props` 全量回填；`LazyList` / `LazyRow` / `GridView` 是**虚拟化条目容器**，JSON 只还原标量属性（`count` / `item_extent` / `columns` / `cell_extent` / `cache_extent` / `restore_key` / 吸附三件套 / `scroll_offset`），**条目工厂不在序列化范围内**：宿主须在 `from_json` 之后经 `set_item_builder(...)` 挂上条目构造器，控件才会产出可见条目（约定同 `Repeater`）。显式序列化的 `scroll_offset` 优先于 `restore_key` 的滚动位置恢复，避免两条恢复通道互相覆盖。

`ReorderableList` 是模板控件且数据源为运行时 `State`，自描述声明 `Rebuildable: no`（工厂注册仅收录元数据）。

`X11Surface` / `WaylandSurface` / `MacOSSurface` / `WasmSurface` 的 CMake 开关均已接入（`AURORA_BACKEND_X11` / `AURORA_BACKEND_WAYLAND` / `AURORA_BACKEND_MACOS` / `AURORA_BACKEND_WASM`）：默认值除 macOS 后端「在 Apple 上 ON、其余平台 OFF」外一律 OFF。后端能力差异与开关默认值见 [`BUILD_OPTIONS.md`](BUILD_OPTIONS.md)。

### 1.2 重叠能力优先级判定

当同一视觉能力同时存在于「固有属性」与「Modifier」时：

- 控件**自身身份语义**的能力（如 `Button` 的 `background_color`、`corner_radius`、`padding`）→ 用**固有属性**优先：随控件序列化、可被 Inspector 枚举、参与 diff。
- 给**任意控件临时套一层**通用装饰（如给 `Image` 加 `Padding`、给 `Text` 加 `Background`）→ 用 **Modifier**：正交、可叠加、可随句柄变化，且不污染控件身份。
- 若控件固有属性已提供该能力，则 **Modifier 同类项不再重复写**，仅作为「跨控件通用兜底」。

| 重叠能力 | 固有属性（控件内） | Modifier（正交链） | 判定边界 |
|:---|:---|:---|:---|
| `padding` | `XxxProps::padding`（随控件序列化） | `Padding` 修饰 | 控件自带留白用固有；跨控件统一留白用 Modifier |
| `corner_radius` | `XxxProps::corner_radius` | `Clip` / `Border` 修饰 | 控件圆角用固有；给任意矩形切圆角用 Modifier |
| `background_color` | `XxxProps::background_color` | `Background` 修饰 | 控件底色用固有；叠加高亮 / 状态色用 Modifier |

---

## 2 状态作用域决策树

面向 AI 编码助手的快速决策规则：先问「消费方是否应获得直接写入能力」，再问值的来源。日常面只有 `State<T>` / `ReadOnlyState<T>` / `computed(fn)` / `effect(fn)` 四个词；`Resource` 与 `Subscription` 在引入异步或显式取消时出现（分档见 [`specification/02-state.md`](specification/02-state.md) §2）。

```text
问：消费方需要直接改这个值吗？
│
├─ 需要（本控件或同一来源的多个编辑者） ──► State<T>（可写句柄）
│
├─ 不需要，但值会变 ─────────────────────► ReadOnlyState<T>
│    ├─ 由其他状态算出 ──────────────────► computed(fn)，结果即 ReadOnlyState<T>
│    ├─ 来自异步作业 ────────────────────► Resource<T> → ReadOnlyState<ResourceSnapshot<T>>
│    └─ 父级下发 / 属性常量 ────────────► 参数或属性直传 ReadOnlyState<T>
│
└─ 值变时要执行副作用（命令 / 日志 / 外部源）► effect(fn)，返回 Subscription
```

跨不相关子树共享时，把 `State<T>` 按值交给 `Provider` 注入（`Provider<State<T>>` 显式授予写权限；`Provider<T>` 只注入只读值）；应用级长生命周期对象用 `shared_ptr<State<T>>` 自持所有权，再向子树下发句柄副本。

### 2.1 生命周期规则

| 类型 | 所有权 | 生命周期 | 清理时机 |
|:---|:---|:---|:---|
| `State<T>` / `ReadOnlyState<T>` | 句柄与来源共享所有权（复制句柄不新增来源） | 句柄按值存活；来源活到最后持有者释放 | 栈上随作用域；来源随最后一个句柄 / 订阅 / 派生闭包释放 |
| `Subscription` | 拥有订阅或运行记录，并保活其依赖来源 | 凭证存活期 | 析构或 `reset()` 即取消 |
| `computed` 返回的 `ReadOnlyState<T>` | 派生节点自持计算体与依赖来源 | 依赖源全部存活即可 | 派生节点随读句柄释放；上游析构自动解边 |
| `Resource<T>` / `StreamResource<T>` | 共享控制核；读句柄是保活控制核的别名投影 | 最后一个控制句柄、订阅或派生闭包释放 | 最后一个持有者释放时取消未完成作业 |

**关键约束**：`get()` 返回的是借用，不得跨写入、来源重绑或最后持有者释放保存；跨异步边界先复制数据或持有句柄本身。`shared_ptr<State<T>>` 是应用级所有权选择，不再是属性接线的必要条件。

### 2.2 反例：保存 `get()` 的借用跨子树存活

句柄按值传递时自带来源所有权，子页面独立销毁不会悬空；真正会悬空的是从 `get()` 取出的引用或对局部句柄的 `[&]` 捕获。跨子树场景应复制句柄（或先复制数据）：

```cpp
// 错误：只保存借用；父级 set() 后该引用失效，子页面读到悬垂内存
// const bool &stale = flag.get();

// 正确：按值持有句柄——复制共享来源，引用计数保活
auto flag = au::State<bool>{true};
push_route(au::Checkbox{ flag });   // 子页面的句柄与 flag 共享来源，任一存活即来源存活
```

---

## 3 与 React / Flutter / Qt 的概念映射

面向从 React / Flutter / Qt 迁移的开发者。下两表覆盖与三框架有清晰对应关系的 UI 原语与基础设施概念。

### 3.1 主要 UI 原语映射

| Aurora | React | Flutter | Qt (QML/C++) |
|:---|:---|:---|:---|
| Widget / Node | 函数组件返回值 / JSX element | `Widget`（Stateless / Stateful） | `Item` / `QWidget` |
| `State<T>` | `useState` / `useSignal`（可写状态） | `ChangeNotifier` + `setState` | `Q_PROPERTY` + 信号槽 |
| `ReadOnlyState<T>` / `computed(fn)` | `useMemo` / 派生 selector（消费方无写入口） | `ValueListenable` / 派生 Provider（riverpod / provider） | 只读 `Q_PROPERTY` / 绑定表达式 |
| `Subscription` / `effect(fn)` | `useEffect` + cleanup 返回 | `addListener` + `removeListener` | 信号槽连接 / `QMetaObject::Connection` |
| `Provider<T>` / `Provider<State<T>>`（注入只读值或显式写权限） | `Context.Provider`（值 + setter） | `InheritedWidget` / `Provider` | 单例 / 上下文属性 |
| `Modifier` | 组合（无直接等价，靠 props） | `Container` / `DecoratedBox` | 属性 / `Item` 嵌套 |
| `BuildContext` / `Environment` | `Context` / `Provider` | `BuildContext` + `InheritedWidget` | 上下文属性注入 |
| `Painter` | `<canvas>` 2D | `CustomPainter` | `QPainter` |
| 滚动吸附 / 分页（`ScrollSnap` + `ScrollViewport::snap_target`，`Scroll` / `LazyList` / `GridView` 三宿主） | CSS Scroll Snap（`scroll-snap-type` / `scroll-snap-align` 的 `start` / `center` / `end` 三档同名对应） | `PageView`（整页）+ `ScrollController.animateTo` + `SnapAlignment`（`ListView` 无内建条目吸附） | `ListView::snapMode`（`NoSnap` / `SnapToItem`，**无居中/居末档**，也非周期概念） |
| `PullToRefresh` | 无内置（`react-pull-to-refresh` 等三方） | `RefreshIndicator` / `CupertinoSliverRefreshControl` | `SwipeView` 无此语义；Android 侧对照 `PullToRefreshLinearLayout` / Material `SwipeRefreshLayout` |
| `StickyHeader` | `position: sticky`（同语义：占位随内容滚、顶到边界钉驻并被下一头项顶出） | `SliverPersistentHeader(pinned: true)` | `ListView` + `Section` 代理（QML 无内建吸顶，需自写 `Stack` + `mapFromItem`）；Android 对照 `RecyclerView` sticky headers |
| 滚动驱动动画（`Widget::offset_signal() -> ReadOnlyState<float>`） | `scroll` 事件 + `scrollTop` / CSS Scroll-driven Animations（`animation-timeline: scroll()`） | `NotificationListener<ScrollNotification>` / `ScrollController.offset`（须自建 `AnimatedBuilder` 联动） | `Flickable::contentY`（可绑定，但非细粒度信号） |
| 嵌套滚动协调（`ScrollEvent::remaining_y` 余量上冒，见 `specification/05-event-navigation.md` §3.3） | 浏览器原生链式滚动（chain）+ `overscroll-behavior` | `ScrollBehavior` + `ScrollEndNotification` 手动转发 | `Flickable` 无内建余量移交，需自写 `contentY` 边界判定接线 |
| `Surface` / `Window` | DOM / `react-dom` | `Window` / `Surface` | `QWindow` / `QQuickWindow` |
| 视频播放 / 媒体 | `<video>` / `react-player` | `video_player` / `ExoPlayer` | `QMediaPlayer` + `QVideoWidget` |
| Layout（Flex） | Flexbox | `Flex` / `Row` / `Column`（`MainAxisAlignment` / `CrossAxisAlignment` / `MainAxisSize` 与 Flutter 同名概念一一对应） | `Row` / `Column` 布局 |
| `RelayoutBoundary` | 无直接等价（`React.memo` / `useMemo` 仅影响重渲染而非布局冒泡） | `RelayoutBoundary`（同源概念：截断 layout 脏冒泡、仅重排本子树；`isRepaintBoundary` 是其 paint 侧孪生） | 无直接等价（`QLayout` 无重排边界语义，整树重算） |
| 基线对齐（`CrossAxisAlignment::Baseline`） | `align-items: baseline`（无基线项的合成基线 = 其 margin box 底边） | `CrossAxisAlignment.baseline`（Flutter 另需 `textBaseline` 分档 alphabetic / ideographic，且任一子项无基线即 assert 崩溃；Aurora **只做 alphabetic 单档**、无基线子项按 CSS 规则以自身底边合成基线，不崩溃） | 无直接等价（QML 需手工计算基线偏移） |
| Animation | `react-spring` / Framer | `AnimationController` + `Curve`；`repeat(repetitions:, reverse:)` ⇔ `AnimationController::repeat(count, alternate)`（Flutter 省略 `repetitions` 即无限，Aurora 用 `count <= 0`，`0` 与 `-1` 同义）；`AnimationController.value = t`（直接定位、不改 `status`） ⇔ `set_progress(t)`；`timeDilation` ⇔ `Animator::set_time_scale`（**互为倒数**：`timeDilation = 2` 即 `time_scale = 0.5`） | `PropertyAnimation`（循环靠 `QAbstractAnimation::setLoopCount`，定位靠 `setCurrentTime`） |
| 弹簧驱动层 | `react-spring` 的 `useSpring`（配置即动画） | `SpringSimulation`（纯求解器）之上由 `AnimatedBuilder` / 监听器手写「逐帧取值 + 判静止停帧」；Aurora 把这一层收进 `SpringAnimation`（`animate_to` / `tick` / `attach` / `on_settled`），求解器本身不动 | Qt Quick 有把弹簧直接绑到属性的动画类型，但**无「纯求解器 + 独立驱动封装」的两层划分**可对照 |
| Timeline 编排（`TimelineSpec` / `TimelinePlayer`） | 动画编排库（Framer `variants` / `react-spring` 序列） | `Interval` + `TweenSequence` / 官方 staggered 模式（`TimelineSpec::staggered` 自动完成手算端点；`TweenSequence` 每段带 `curve` ⇔ `Keyframes<T>::Stop{..., curve}`，段归属同为**该段起点**）；WPF `Storyboard` 同类定位（Aurora 不做运行时时间线树） | `QSequentialAnimationGroup` / `QParallelAnimationGroup` |
| 手势驱动动画（`DragRecognizer` / `DragToDismiss` / `Dismissible`） | `react-swipeable` / `framer-motion` drag | `Draggable` + `AnimationController` / `Dismissible`（跟手 + spring 裁决语义与 Flutter `Dismissible` 同构；Aurora 的 `DragToDismiss` 不自持弹簧实现，松手段复用公共 `SpringAnimation`，等价于 Flutter 在 `SpringSimulation` 之上那层驱动的官方封装形态） | `SwipeDelegate`（Qt Quick） |
| 命令系统（`Command` / `CommandRegistry` / `CommandPalette`） | 无内置命令表（context + 自建菜单 / 快捷键 hook，同一动作常在多处重复定义） | `Intent` + `Actions` + `Shortcuts`（Aurora 把三者收敛到单一 `CommandRegistry`，快捷键与菜单降为其投影）；面板形态对照 Flutter `SearchAnchor` / VSCode Command Palette | `QAction` + `QShortcut` + `QMenu`（`QAction` 即同源「动作唯一真源」，但无内建命令面板） |
| 连击序号（`MouseEvent::click_count`，见 `specification/05-event-navigation.md` §2.2.1） | `MouseEvent.detail`（同为 1/2/3 连击计数，语义同源） | `TapDownDetails` 无内建计数（双击需 `GestureDetector.onDoubleTap` 单独注册，与单击回调互斥裁决） | `QMouseEvent` 无内建计数（双击靠 `QWidget::mouseDoubleClickEvent` 单独虚回调） |
| 定时任务（`Timer` / `Scheduler`） | `setTimeout` / `setInterval` | `Timer.periodic` / `Timer` | `QTimer` |
| `Navigator` | React Router | `Navigator` / `Navigator 2.0`（`Hero` 共享元素转场、`open_uri` 深层链接对应 Flutter `Hero` / 路由 URI 解析） | `StackView` / 路由 |
| `MediaQuery` / `LayoutBuilder` / `BreakpointBuilder` | 媒体查询 hook（无直接内置）/ 条件渲染 / 断点档位 | `MediaQuery` + `LayoutBuilder` + `BreakpointBuilder` | 屏幕度量 / `LayoutBuilder` 等价模式 / `Breakpoint{Compact,Medium,Expanded}`（阈值 600/840 可调） |
| 图表控件族 `BarChart` / `LineChart` / `Sparkline` / `PieChart` / `ScatterChart` | 无官方图表库（Recharts / `react-chartjs-2` / visx）；`CustomPainter` 思路对应 `Painter` | `bar_chart` / `charts_flutter`（同族：数据进 props、`toJson`/`fromJson` 可序列化、轴域与命中反查同源） | 无官方图表（QCustomPlot / QChart 第三方）；Aurora 五图均为 `LeafWidget` + 纯值 `XxxProps` + `defaults()`，AI 可经 `aurora_api.json` schema + `from_json` 生成带真实数据的图表 |

**视频播放器定制**：`VideoPlayer` 可子类化（`create_default_controls()` 虚化并在挂载期生效、`current_frame()` / `paint_frame()` 读 / 绘帧、`on_pointer_event` / `wants_click` 为 public）；`VideoControls` 可子类化换肤 / 重排（`play_button()` / `time_text()` / `mute_button()` 访问器 + `build_children()` 虚函数）。详见 [`specification/03-layout-render.md`](specification/03-layout-render.md) §9.2。

### 3.2 基础设施与平台映射

| Aurora | React | Flutter | Qt (QML/C++) |
|:---|:---|:---|:---|
| `Diagnostics` | Error Boundary + 控制台 | 调试断言 / `debugFillProperties` | 警告输出 |
| Lifecycle（控件挂载 / 卸载副作用） | `useEffect(…, [])` + cleanup 返回 | `StatefulWidget.initState` + `dispose` | `QObject` 父子 RAII / `QWidget` 事件（无内置钩子，靠析构） |
| 窗口生命周期 `WindowState` / `WindowMode` | `document.visibilityState` / `window` focus-blur（仅应用级） | `WidgetsBindingObserver.didChangeAppLifecycleState` + 窗口尺寸状态 | `QWindow::windowStateChanged` / `QApplication::applicationStateChanged` |
| 多窗口（一个进程多个顶层窗口） | 多 `window` / 多 `document`（各自独立树与焦点） | 多 `FlutterView` / 多 Engine 实例（各自 `WidgetsApp`） | 单个 `QApplication` + 多个 `QWindow` |
| 窗口宿主 `WindowHost` | 一个 `document` 的根 scope | 一个 `FlutterView` + 其 `Binding` | `QWindow` + 其 `QWidget` 树 |
| 跨窗通信 `WindowEventBus` | `window.postMessage` / `BroadcastChannel` | 无原生对应（靠上层注入共享 `InheritedWidget` / 单例） | 信号槽 / `QCoreApplication::postEvent` |
| `TitleBar`（自绘标题栏 / CSD） | 无原生对应 ≈ 社区 window chrome 方案 | `AppBar` + `window_manager` | `QQuickWindow` headerBar 或 `KWindowSystem` |
| IME 组合输入 `TextCompositionEvent` + `Widget::on_text_composition` | `onCompositionStart` / `onCompositionUpdate` / `onCompositionEnd`（DOM 组合事件，`data` + 光标） | `TextInputClient.updateEditingState`（`TextEditingValue`：`text` + `selection`，组合态由平台 embedder 折算） | `QInputMethodEvent`（`preeditString` / `commitString` / `selectionStart` / `selectionLength` + `SURROUNDING_TEXT` 回答） |

| JSON 值容器 `au::json::Value` | `JSON.parse` / `JSON.stringify`（值语义；读缺失键返回 `undefined`，不改动原对象） | `dart:convert` 的 `jsonDecode` / `jsonEncode`（产出 `Map` / `List`，无保真数字概念） | `QJsonDocument` / `QJsonObject` / `QJsonArray`（隐式共享，值语义） |

> 与 §3.1 并列于同一概念清单、仅因跨框架直接等价较少而单独成表的基础设施 / 平台层概念（括号内为本文件 §1 概念清单序号，非需求 ID）：Event（12）、Platform Shell（14）、Accessibility（15）、DevTools（16）、Result / Error（18）、JSON 值容器（30）。

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
- **响应式以 `State<T>` / `ReadOnlyState<T>` 为中心**：替代 React 的 `useState` 与 Flutter 的 `setState` / `ChangeNotifier`，粒度更细（细粒度信号避免整树重绘）。写权限靠类型表达：只读消费方拿 `ReadOnlyState<T>`，而不是 `const State&`。
- **订阅清理用 `aurora::Subscription`（RAII）**：`source.subscribe(fn)` 固定调用时来源并立即交付首值，`effect(fn)` 自动追踪读取并调用即首跑；两者都返回 `Subscription`，作用域结束自动取消，等价于 React `useEffect(…, [])` 的 cleanup 返回。旧 `connect` / `bind` 自由函数已删除（历史：`bind` 曾因 `std::enable_shared_from_this` 把 `std` 带进 ADL 关联集而被 `std::bind` 劫持，故一度改名 `connect`；改为成员入口后歧义消失）。
- **环境注入走 `Environment`**：主题 / locale / 媒体查询通过 `ctx.environment<T>()` 读取，等价于 React Context 与 Flutter `InheritedWidget`。设备度量（`MediaQuery`）由 `Window::present_root` **自动注入根 `BuildContext`**（每帧按 `from_surface` 重建），故无需手动包裹 `MediaQueryProvider` 即可经 `media_query_of(ctx)` / `MediaQuery::of(ctx)` 读取；如需覆盖特定子树，仍可在该子树根显式包 `MediaQueryProvider`（最近祖先优先）。结合 `LayoutBuilder(builder)` 按布局约束动态构建子树，等价于 Flutter 的 `MediaQuery` + `LayoutBuilder` 响应式写法。
- **平台无关渲染**：`Painter` 是纯软件栅格，无 GPU 依赖；`HeadlessSurface` 可直接出 PNG，便于测试与无头渲染。
- **降级而非中止**：非法输入 / 部分代码缺失产出 `Diagnostics` 并降级到安全默认，而非抛异常崩溃（与 React Error Boundary 哲学一致）。

---

## 5 状态选择示例

### 5.1 组件内 State：Checkbox 的 checked 状态

```cpp
au::State<bool> checked{false};
au::Checkbox cb{checked};   // 句柄复制共享来源；用户点击写回同一来源
// checked 随父组件作用域释放，来源活到最后一个句柄消亡
```

```json
{ "type": "Checkbox", "props": { "checked": false } }
```

### 5.2 状态提升：父组件持有 State，两个子组件共享

```cpp
au::State<bool> shared_flag{true};
au::Checkbox cb_a{shared_flag};   // 两个句柄复制共享同一来源
au::Checkbox cb_b{shared_flag};
shared_flag.set(false);           // cb_a 与 cb_b 同时刷新
```

跨子树独立存活时，把句柄按值交给子页面即可；只在需要应用级生命周期管理时用 `std::shared_ptr<au::State<T>>` 自持所有权。

### 5.3 应用模型与领域命令：购物车状态

`Store` 已下线；校验、审计与撤销责任由应用领域命令承担（撤销显式用 `UndoStack`）。注入后代的模型只暴露只读句柄与命令，不授予任意写入口：

```cpp
struct Cart { std::vector<std::string> items; };

struct CartModel {
    au::State<Cart> state{};
    auto add(std::string name) -> void {        // 领域命令：校验在此，不在控件里
        Cart next = state.get();
        next.items.push_back(std::move(name));
        state.set(std::move(next));
    }
    auto clear() -> void { state.set(Cart{}); }
};
// 经 Provider<CartModel> 注入子树：后代拿 cm.state 降级出的只读句柄展示，
// 经 cm.add(...) / cm.clear() 修改；需要撤销时把命令同时包进 UndoStack。
```

### 5.4 Environment 注入：主题色

```cpp
auto theme = au::State<au::Theme>{au::Theme::light()};
theme.update([](const au::Theme &t) {
    au::Theme next = t;
    next.primary = au::colors::AURORA_BLUE;
    return next;
});
au::Node root = au::ThemeProvider{theme, au::Button(au::ButtonProps{ .label = "主题按钮" })};
// 子组件内部：const Theme* t = ctx.environment<Theme>();   // 最近祖先优先
// theme.set(...) 换肤时子树自动重渲染
```

### 5.5 computed 派生：过滤后的列表

```cpp
au::State<std::vector<std::string>> source{
    std::vector<std::string>{"Apple", "Banana", "Avocado", "Cherry"}};
au::State<std::string> keyword{"A"};
auto filtered = au::computed([source, keyword]() -> std::vector<std::string> {
    std::vector<std::string> result;
    for (const auto &s : source.get()) {
        if (s.find(keyword.get()) != std::string::npos) result.push_back(s);
    }
    return result;
});
// filtered.get() == {"Apple", "Avocado"}；filtered 是 ReadOnlyState，可直接接展示属性
keyword.set(std::string{"Ch"});   // → filtered 自动重算 → {"Cherry"}
```

### 5.6 异步三态：Resource 直通展示

```cpp
auto request = au::Resource<std::string>::from(producer);   // producer 返回 Task<std::string>
au::ReadOnlyState<au::ResourceSnapshot<std::string>> snapshot = request;
auto label = au::computed([snapshot]() -> au::LocalizedString {
    const auto &current = snapshot.get();
    if (current.phase == au::ResourcePhase::Failed) return au::LocalizedString{"Failed"};
    if (current.value) return au::LocalizedString{*current.value};
    return au::LocalizedString{"Loading"};
});
au::Text status{au::TextProps{.content = label}};
request.reload();   // 前序请求自动取消；迟到结果不覆盖新状态
```
