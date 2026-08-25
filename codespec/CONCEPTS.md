# Aurora 概念地图（Concept Map）

> 面向 AI 编码助手的概念认知与翻译表。分为两部分：
> 1. **核心概念审计**（规格 〇.2）：Aurora 全部 UI 原语可枚举、可命名、可映射，便于 AI 检索与组合。
> 2. **跨框架概念映射**（见 `architecture/ARCHITECTURE_AI.md` 12.3）：Aurora 概念与 React / Flutter / Qt 的对应关系，便于从训练知识迁移。
>
> 本文件是 `architecture/ARCHITECTURE_AI.md`（AI-first 设计原则）中 12.2 / 12.3 条目的具体展开。

---

本文档已划分为以下子文档（位于 `./<主题>/` 下）：

- [CONCEPTS_CORE.md](./concepts/CONCEPTS_CORE.md) — 核心概念审计（全部可枚举 UI 原语 #1–#19）

> 参考 [一、核心概念审计](./concepts/CONCEPTS_CORE.md#一核心概念审计)。

## 一、核心概念审计

Aurora 的全部 UI 原语可枚举、可命名、可映射，便于 AI 检索与迁移训练。完整审计表（含全部 #1–#19 类成员与头文件、注册状态、状态作用域决策树与示例）见子文档 [`CONCEPTS_CORE.md`](./concepts/CONCEPTS_CORE.md)。

## 二、与 React / Flutter / Qt 的概念映射

> 面向从 React / Flutter / Qt 迁移的开发者，给出 Aurora 概念与三者的对应关系。下表覆盖与三框架有清晰对应关系的 **主要 UI 原语**；Event / Platform Shell / Accessibility / DevTools / Result-Error / Lifecycle-Window 等偏基础设施 / 平台层概念单独列于 2.2（避免主表过长，非约束性排除）。

### 2.1 主要 UI 原语映射

| Aurora                        | React                                                           | Flutter                                                                                                        | Qt (QML/C++)                                     |
|-------------------------------|-----------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|--------------------------------------------------|
| Widget / Node                 | 函数组件返回值 / JSX element                                    | `Widget`（Stateless/Stateful）                                                                                 | `Item` / `QWidget`                               |
| State\<T\> / Signal           | `useState` / `useSignal`                                        | `ChangeNotifier` + `setState`                                                                                  | `Q_PROPERTY` + 信号槽                            |
| Store\<T\>                    | `useContext` / Redux store                                      | `InheritedWidget` / `Provider`                                                                                 | 单例 / 上下文对象                                |
| Modifier                      | 组合（无直接等价，靠 props）                                    | `Container` / `DecoratedBox`                                                                                   | 属性 / `Item` 嵌套                               |
| BuildContext / Environment    | `Context` / `Provider`                                          | `BuildContext` + `InheritedWidget`                                                                             | 上下文属性注入                                   |
| Painter                       | `<canvas>` 2D                                                   | `CustomPainter`                                                                                                | `QPainter`                                       |
| Surface / Window              | DOM / `react-dom`                                               | `Window` / `Surface`                                                                                           | `QWindow` / `QQuickWindow`                       |
| 视频播放 / 媒体（v0.20.0）    | `<video>` / `react-player`                                      | `video_player`(`VideoPlayer`) / `ExoPlayer`                                                                    | `QMediaPlayer` + `QVideoWidget`                  |
| Layout（Flex）                | Flexbox                                                         | `Flex` / `Row` / `Column`（`MainAxisAlignment`/`CrossAxisAlignment`/`MainAxisSize` 一一对应 Flutter 同名概念） | `Row` / `Column` 布局                            |
| RelayoutBoundary（重排边界）  | 无直接等价（`React.memo` / `useMemo` 仅影响重渲染而非布局冒泡） | `RelayoutBoundary`（同源概念：截断 layout 脏冒泡、仅重排本子树；`isRepaintBoundary` 是其 paint 侧的孪生）      | 无直接等价（`QLayout` 无重排边界语义，整树重算） |
| Animation                     | `react-spring` / Framer                                         | `AnimationController` + `Curve`                                                                                | `PropertyAnimation`                              |
| 定时任务（Timer / Scheduler） | `setTimeout` / `setInterval`                                    | `Timer.periodic` / `Timer`                                                                                     | `QTimer`                                         |
| Navigator                     | React Router                                                    | `Navigator` / `Navigator 2.0`（`Hero` 共享元素转场、`open_uri` 深层链接对应 Flutter `Hero` / 路由 URI 解析）   | `StackView` / 路由                               |
| MediaQuery / LayoutBuilder    | 媒体查询 hook（无直接内置）/ 条件渲染                           | `MediaQuery` + `LayoutBuilder`                                                                                 | 屏幕度量 / `LayoutBuilder` 等价模式              |

> **视频播放器定制**：`VideoPlayer` 可子类化（`create_default_controls()` 虚化并在挂载期生效、`current_frame()`/`paint_frame()` 读 / 绘帧、`on_pointer_event`/`wants_click` 为 public）；`VideoControls` 可子类化换肤 / 重排（`play_button()`/`time_text()`/`mute_button()` 访问器 + `build_children()` 虚函数）。详见 `SPECIFICATIONS.md` §视频播放器与 `GUIDELINE.md` §15。

### 2.2 基础设施 / 平台映射

| Aurora                                    | React                                                        | Flutter                                                            | Qt (QML/C++)                                                            |
|-------------------------------------------|--------------------------------------------------------------|--------------------------------------------------------------------|-------------------------------------------------------------------------|
| Diagnostics                               | Error Boundary + 控制台                                      | 调试断言 / `debugFillProperties`                                   | 警告输出                                                                |
| Lifecycle（控件挂载/卸载副作用）          | `useEffect(…,[])` + cleanup 返回                             | `StatefulWidget.initState` + `dispose`                             | `QObject` 父子 RAII / `QWidget` 事件（无内置钩子，靠析构）              |
| 窗口生命周期 `WindowState` / `WindowMode` | `document.visibilityState` / `window` focus-blur（仅应用级） | `WidgetsBindingObserver.didChangeAppLifecycleState` + 窗口尺寸状态 | `QWindow::windowStateChanged` / `QApplication::applicationStateChanged` |
| TitleBar（自绘标题栏 / CSD）              | 无原生对应 ≈ 社区 window chrome 方案                         | `AppBar` + `window_manager`                                        | `QQuickWindow` headerBar 或 `KWindowSystem`                             |

> **与 2.1 并列于同一概念清单、仅因跨框架直接等价较少而单独成表的基础设施 / 平台层概念**（完整枚举见 `CONCEPTS_CORE.md`）：Event（#12：MouseEvent / KeyEvent / Focus）、Platform Shell（#14：FileDialog / SystemTray / Clipboard / Display）、Accessibility（#15）、DevTools（#16：HotReload / inspect / generate_ui / validate_ui）、Result-Error（#18：Result / Error）。

### 生命周期：两级正交，不移植 Activity

Aurora **不引入**安卓 `Activity` 的 `onCreate/onStart/onResume/onPause/onStop/onDestroy` 命令式生命周期——它面向 OS
窗口可见性、与声明式树模型冲突，且 per-widget 的 resume/pause 无实际意义。改为两级正交机制：

- **控件子树级 `au::Lifecycle`**：包裹子树，`on_mount` 挂载后恰好一次（可注册外部源、读 `ctx.environment<T>()`），`on_unmount`
  在控件 `Node` 析构时清理（RAII，覆盖 `Repeater` 缩容 / `Navigator` pop）。对应 Android `View.onAttachedToWindow`/
  `onDetachedFromWindow`，而非 `Activity`。`Show` 隐藏保留子树存活（不析构、不卸载），对齐 Flutter `Visibility`。
- **窗口/应用级 `WindowState`（Visible/Occluded/Hidden）+ `WindowMode`（Normal/Maximized/Minimized/FullScreen）**
  ：仅窗口层报告（Qt/WPF/Win32/Flutter/Web 概莫能外），由后端经 `Surface` 句柄上报，`Application` 暴露响应式 `State` +
  命令式回调，并注入根 `Environment`（`ctx.environment<WindowState>()` 读取）。最大化是独立几何态， **不并入** `WindowState`
  ；精确「被其他窗口像素级遮挡」检测昂贵且普遍不被框架支持，故以「失焦 = 被遮挡」近似为 `Occluded`。

把「控件在不在树里」与「窗口是否可见/失焦」彻底分离，避免在任何控件上挂接 resume/pause 这种窗口语义的钩子。

### 声明式工厂语法糖层（aurora::ui）

Aurora 的「真值来源」仍是声明式 `Node` 树 + `XxxProps` 聚合属性（`Column{...}`、`Text{...}` 值构造）。`aurora::ui`（别名
`au::ui`）是叠其上的 **语法糖**：一组工厂函数把「构造 + 加父 + 返回强类型指针」三步合一，进一步压缩 AI 生成代码的 token
与出错面，等价于手写声明式构造但更短。它 **不引入新控件类型**，底层仍是 `Text` / `Button` / `Column`…，与现有概念映射表完全一致。

### 迁移要点

- **声明式 + 不可变树**：Aurora 组件树与 React/Flutter 一致，状态变更触发局部重渲染； 不要用命令式方式事后改树，应通过
  `State<T>` 驱动。
- **响应式以 `State<T>`/`Signal` 为中心**：替代 React 的 `useState` 与 Flutter 的
  `setState`/`ChangeNotifier`，粒度更细（细粒度信号避免整树重绘）。
- **订阅清理用 `aurora::Subscription`（RAII）**：把信号/`Store` 订阅包成对象，作用域结束自动取消，等价于 React
  `useEffect(…, [])` 的 cleanup 返回；`bind(src, fn)` 直接返回该句柄，避免手动保存/调用取消句柄导致监听器泄漏（#7 Signal
  概念的具体化）。
- **环境注入走 `Environment`**：主题/ locale / 媒体查询通过 `ctx.environment<T>()` 读取， 等价于 React Context 与 Flutter
  `InheritedWidget`。设备度量（`MediaQuery`）由 `Window::present_root`
  **自动注入根 `BuildContext`**（T8，每帧按 `from_surface` 重建），故无需手动包裹 `MediaQueryProvider` 即可经
  `media_query_of(ctx)` / `MediaQuery::of(ctx)` 读取（`Application::run` 自动受益）；如需覆盖特定子树，仍可在该子树根显式包
  `MediaQueryProvider`（最近祖先优先）。结合 `LayoutBuilder(builder)` 按布局约束动态构建子树，等价于 Flutter 的
  `MediaQuery` + `LayoutBuilder` 响应式写法。
- **平台无关渲染**：`Painter` 是纯软件栅格，无 GPU 依赖；`HeadlessSurface` 可直接出 PNG， 便于测试与无头渲染（见
  `tests/test_offscreen.cpp`，原 golden_test 已并入）。
- **降级而非中止**：非法输入/部分代码缺失产出 `Diagnostics` 并降级到安全默认， 而非抛异常崩溃（与 React Error Boundary
  哲学一致）。
