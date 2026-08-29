# Aurora 架构

> 本文件是架构、运行时、分层、模块映射与设计不变量的**唯一权威**。
> 功能规格与 API 契约见 [`SPECIFICATIONS.md`](SPECIFICATIONS.md) 及 `specification/` 八份子系统文档；概念映射见 [`CONCEPTS.md`](CONCEPTS.md)；编码规则见 [`CODING_STANDARDS.md`](CODING_STANDARDS.md)；复制即用配方见 [`GUIDELINE.md`](GUIDELINE.md)；编译选项见 [`BUILD_OPTIONS.md`](BUILD_OPTIONS.md)。

---

## 1 概览

Aurora 是一个 C++20 跨平台 GUI 库，以**声明式 + 响应式**为核心范式。其 API 经过专门设计以对 AI 编码助手友好（§13）：概念可枚举、声明式优于命令式、数据单向流动、错误信息携带修复建议。

区别于传统 GUI 框架，Aurora **不引入 GPU 依赖**，采用纯软件栅格渲染（`Painter`），并以「内核 / 适配分离」的分层架构实现跨平台：`Painter` 只操作抽象帧缓冲，平台差异收敛到 `Surface` 实现（Headless 内存、Win32/GDI、D3D11 GPU 上屏、GLFW/OpenGL、X11/Xlib、Wayland、Wasm/Canvas、macOS/AppKit）。

---

## 2 分层架构

```text
┌───────────────────────────────────────────┐
│  App / Examples (examples/)               │  应用层：组装组件树、注册自定义控件
├───────────────────────────────────────────┤
│  Widgets / Controls (include/aurora/widget/)│  组件层：声明式 UI 原语
├───────────────────────────────────────────┤
│  Reactive Core (include/aurora/state/)    │  响应式核心：State / Signal / Effect
├───────────────────────────────────────────┤
│  Layout Engine (include/aurora/layout/)   │  布局引擎：Constraints / flex / grid 求解
├───────────────────────────────────────────┤
│  Render Core (include/aurora/render/)     │  渲染核心：Painter（软件栅格）
├───────────────────────────────────────────┤
│  Platform Abstraction (include/aurora/window/) │  平台抽象：Surface 家族
├───────────────────────────────────────────┤
│  Foundation (include/aurora/core/)        │  基础层：types / Result / Error / Log
└───────────────────────────────────────────┘
```

- **依赖方向单向向下**：上层依赖下层，下层不感知上层（渲染核心不知道具体 widget）。
- **内核 / 适配分离**：平台差异收敛到 `Surface` 实现。
- **响应式与渲染解耦**：状态变更经 `State` / `Signal` 精确投递到受影响的 widget 子树，不经「整树 diff + 重建」；渲染核心只负责把 widget 树绘制到 `Surface`。

---

## 3 运行时

### 3.1 线程与事件模型

- **单线程 UI**：所有 widget 树操作、状态变更、事件处理都在主线程。`State::set` 仅限主线程调用（赋值 + notify 无锁无原子）；跨线程计算结果须经 `au::async` / `Task::set_main_poster` 回投主线程后再写 `State`。
- **同步事件**：`EventDispatcher` 在收到原生平台事件后同步派发，命中测试链自最深节点向根冒泡，写 `e.handled = true` 即止。
- **响应式细粒度信号**：`State<T>` / `Signal` 订阅精确到具体订阅者；状态变更只刷新依赖它的 widget，避免整树重绘。

### 3.2 几何权威

命中链的根矩形取自根 `Node` 的 `m_bounds`（由 `Window::present_root` 写入窗口矩形）；子树几何由容器在布局时经 `child.set_bounds(box)` 写入各 `Node`。`Widget` **不持有任何几何缓存**。

输入坐标本地化在 `EventDispatcher`：命中链 `hit_test_chain` 返回 `std::vector<HitNode>`，派发器在冒泡每个控件前写入 `MouseEvent::local_position = position - origin`，控件在 `on_pointer_event` 中直接消费本地坐标。

### 3.3 跨帧缓存引用的生命周期

命中链、悬停链、指针捕获（`HitNode`）与焦点记录（`FocusManager`）都会**跨事件、跨帧**缓存控件引用，而控件可能在两次使用之间被销毁（`LazyList` 滚动回收子项、`push_replacement` 重建页面、用户 `on_click` 里销毁自身子树）。统一模式：

- 构造时探测该控件是否由 `shared_ptr` 持有——是则存 `weak_ptr` 守卫并在取用时判活；
- 否则（栈 / 成员控件的 `weak_from_this()` 为空弱引用）回退为裸指针，其生命周期由持有者保证。

> 只用 `weak_ptr` 会让栈上控件恒 `lock` 失败而静默吞掉全部事件；只用裸指针则是 use-after-free。两者都不可取。

**同步回调期间必须持住强引用**：事件派发是同步的，用户回调可以在回调内销毁触发它的控件；而回调返回后框架仍要继续访问该控件（`Widget::on_pointer_event` 写 `m_pressed` 等）。因此凡要解引用跨帧缓存的控件引用，都须在**整个调用期间**持有强引用（`HitNode::lock(keepalive)`），而非仅做一次「取指针 + 判空」。

**观察者图生命周期安全**：信号源与 `Effect` 经 `Connection`（`weak_ptr` 锚点）连接，`notify()` 惰性摘除失效边，任一侧先析构均不解引用悬垂对象。

**非拥有登记必须成对注销**：`Animator::drive` / `bind` 以裸指针登记控制器与目标 `State`。若被登记对象的生命周期短于 `Animator`（典型：widget 成员控制器注册进应用级 `Animator`），**必须**在其析构函数中调用 `Animator::remove(c)`，否则下一帧 `tick` 即写入已释放内存。凡新增「把自身成员注册进更长寿命管理器」的代码，都要同时写好注销路径。

### 3.4 确定性与降级

- **确定性渲染**：`Painter` 为纯函数式绘制（给定 widget 树 + 尺寸 → 确定像素），便于 `HeadlessSurface` 输出可比对快照（golden test）。
- **降级而非中止**：非法输入 / 部分代码缺失产出 `Diagnostics` 并降级到安全默认，而非抛异常崩溃。

---

## 4 模块映射

路径前缀均为 `include/aurora/`，实现位于 `src/aurora/`（静态库）。头文件引用的最终校验见 `tools/check_arch_module_map.py`（缺失 / 歧义 / 错位引用返回非零退出码，可作 CI 门禁）。

### 4.1 基础层与响应式核心

| 模块 | 路径 | 主要头文件 |
|:---|:---|:---|
| 基础层 | `core/` | `types.h` `result.h`（`Result<T>` / `Error`） `log.h` `diagnostics.h` `color.h` `dimension.h` `image.h` `font.h` `expected.h` `immutable.h` `strict_mode.h` `event_stream.h` |
| 响应式核心 | `state/` | `state.h` `computed.h` `effect.h` `binding.h` `store.h` `reactive.h` `signal_view.h` `async.h` `coroutine.h` `state_graph.h` `state_registry.h` |

### 4.2 布局与渲染

| 模块 | 路径 | 主要头文件 |
|:---|:---|:---|
| 布局引擎 | `layout/` | `flex.h` `flex_layouter.h` `layout_box.h` `layout_engine.h`（`Constraints` 在 `core/types.h`） |
| 渲染核心 | `render/` | `painter.h` `font_engine.h` `bitmap_font.h` `png.h` `offscreen.h` `blend.h` `dirty_region.h` `display_list.h` `font_discovery.h` `glyph_atlas.h` `image_cache.h` `snapshot_diff.h` `text_aa_mode.h` |

### 4.3 平台抽象

| 模块 | 路径 | 主要头文件 |
|:---|:---|:---|
| 平台抽象 | `window/` | `surface.h`（`Surface` / `HeadlessSurface`） `window.h` `native_surfaces.h` `win32_window.h` `win32_surface.h` `glfw_surface.h` `x11_surface.h` `wayland_surface.h` `wasm_surface.h` `macos_surface.h` `d3d11_surface.h` `frame_pacing.h` `title_bar_geometry.h` `title_bar_style.h` `window_chrome.h` `window_state.h` `platform.h`（`au::platform()` / `Platform` / `PlatformCapabilities`） |

`Surface` 为可扩展边界：自定义 Surface 经 `Application(Scene, unique_ptr<Surface>)` 注入，不随内置 Surface 增长；各后端由 feature 宏 `AURORA_BACKEND_*` 控制代码剪裁。

`win32_window.h` 与 `glfw_surface.h` 均 **pimpl 隔离**，公共头零 `<windows.h>` / GLFW / OpenGL 依赖。全部真实后端（`Win32Window` / `GlfwSurface` / `X11Surface` / `WaylandSurface`）的公共头均已收敛为 pimpl 句柄，即便后端开启，消费者编译单元也不会被拉入重型平台头，连带避免 `min` / `max`、`None` / `Bool` / `Status` 等宏污染。

### 4.4 组件层、修饰与控制流

| 模块 | 路径 | 职责 |
|:---|:---|:---|
| 组件层 | `widget/`（59 个头） | `widget.h` `descriptor.h` `props_io.h` `containers.h` `text.h` `button.h` `image_widget.h` `checkbox.h` `switch.h` `slider.h` `canvas.h` `progress.h` `divider.h` `rich_text.h` `scroll.h` `stack.h` `grid.h` `spacer.h` `show.h` `repeater.h` `provider.h` `timer.h` `lifecycle.h` `text_span.h` `codegen.h` `inspect.h` `inspector_panel.h` `layout_query.h` `serialization.h` `yaml.h` `recipes.h` 等 |
| 修饰节点 | `modifier/` | `modifier.h`（Padding / Background / Border / Clip / Opacity / SizeModifier / FlexWeight / Clickable / AlignNode / OffsetNode / Draggable / LongPress / TouchListener） |
| 控制流 | `widget/` | `show.h` `repeater.h` `provider.h` `timer.h` |
| 序列化 | `widget/serialization.h` `widget/codegen.h` `widget/yaml.h` | 树 ⇄ JSON、差异补丁、树 ⇄ 源码、树 → YAML |

### 4.5 导航、动画、环境、国际化

| 模块 | 路径 | 主要头文件 |
|:---|:---|:---|
| 导航 | `navigation/` | `navigator.h` `route.h`（`Route` / `RouteTransition`） `router.h` `transition_layer.h` `navigator_host.h` `hero.h` |
| 动画 | `animation/` | `timeline.h`（`Tween` / `Keyframes`） `animator.h` `easing.h`（`Curve`） `spring.h`（`SpringSimulation`） |
| 主题 | `theming/` | `theme.h` `theme_scope.h` `theme_query.h` `style_props.h` |
| 环境注入 | `environment/` | `environment.h` `media_query.h` `build_context.h` |
| 国际化 | `i18n/` | `locale.h` `localized_string.h` `string_table.h` |
| 事件 | `event/` | `event.h` `dispatcher.h` `gesture.h` `focus.h` `keycode.h` `drag_drop.h` |

### 4.6 应用驱动与持久化

| 模块 | 路径 | 职责 |
|:---|:---|:---|
| 应用驱动 | `app/` | `application.h` `scheduler.h` `scene.h` `clipboard.h` `file_dialog.h` `system_tray.h` `display.h` `perf_overlay.h` `menu.h` `shortcuts.h` `validate.h` `validate_ui.h` `generate_ui.h` `hot_reload.h` |
| 偏好配置 | `preferences/` | `preferences.h`（JSON 文件后端 + 响应式键值存储；不新增 UI 控件） |
| 数据存储 | `storage/` | `storage.h`（`Storage` 门面） `backend.h` `memory_backend.h` `fs_backend.h` `serializable.h` `storage_types.h` |

### 4.7 性能、媒体、调试、入口

| 模块 | 路径 | 职责 |
|:---|:---|:---|
| 性能检测 | `perf/` | `perf_log.h` `counters.h` `profiler.h` `stopwatch.h` `perf_session.h` `trace_writer.h` `scroll_bench.h` |
| 媒体 | `media/` | `video_player.h` `video_source.h` `video_controls.h` `image_sequence_source.h` |
| 调试门面 | `debug/` | `debug_backend.h` `debug_paint.h` `debug_runtime.h` `debug_trace.h`（`aurora::debug`） |
| 图像编解码 | `image/` | `image_codec.h` |
| 检查器 | `inspector/` | `inspector_server.h` `inspector_api.h` |
| 工厂语法糖 | `ui/` | 声明式工厂函数（见 [`specification/04-widget.md`](specification/04-widget.md) §5） |
| 入口 | `aurora.h` | 聚合 include + `namespace au` 别名提示 |

### 4.8 数据存储抽象层（Storage）

`Storage` 是比 `preferences` 更通用的持久化抽象（信封 / 类型化 / 异步 / 事务 / 可注入后端），与 UI 控件解耦。**新代码默认优先 `Storage`**，`preferences::Preferences` 保留为面向「响应式键值 + JSON 文件」的轻量特化。

**契约要点：**

- **门面**：`put` / `get` / `remove` / `list` / `contains` / `clear`（value 为 `Json` 或原生 `StorageBytes`）；类型化 `put<T>` / `get<T>` 经 `StorageSerializable` ADL 定制点序列化；信封级 `put_record` / `get_record`；异步 `async_put` / `async_get` / `async_get_value` / `async_remove` / `async_list`（返回 `aurora::Task<T>`，把 IO 卸载出 UI 线程）；`on_change(cb)` 返回 `aurora::Subscription`；`transaction(body)`（跨记录原子，失败全回滚）；进程级 `default_instance()` 单例。
- **后端抽象** `StorageBackend`：纯虚 `put_record` / `get_record` / `remove` / `list`（信封级），另有带默认实现的 `contains` / `clear` / `flush` / `close` 与默认 `transaction`（顺序 apply + 异常回滚）。`MemoryBackend` 以 `std::map` 全量快照实现回滚；`FilesystemBackend` 每记录一文件（原子写 `tmp` + `rename`）、目录级锁串行化事务。
- **信封** `StorageRecord{ id, type, version, encoding, mtime, payload, blob_ref }`：版本号支撑乐观并发与迁移。`StorageChange{ op(Put|Remove|Clear|Batch), id }` 供 `on_change` 投递。
- **错误模型**：统一经 `Result<T>`；后端 IO 失败返回 `Error` 而非抛异常；`get` 未命中返回「未找到」错误（区分于 `null` 值）；事务中途失败回滚并报告首个失败原因。
- **线程模型**：门面 API 主线程调用，`async_*` 经 `au::async` 卸载到 worker 线程；同步 `get` 直通后端（门面无内存缓存）。后端实现须线程安全。

API 契约以 `include/aurora/storage/*.h` 的落地声明为准（见 [`specification/06-app-platform.md`](specification/06-app-platform.md) §9.2）。

---

## 5 核心数据流

```text
[State / Signal 变更]
      │
      ▼
[Reactive Core 计算依赖图] ──精确──► [受影响的 Widget 子树]
      │                                        │
      ▼                                        ▼
[Layout Engine 求解 Constraints]        [局部重绘调度（主线程）]
      │                                        │
      ▼                                        ▼
[Render Core: Painter 绘制到 Surface] ◄────────┘
      │
      ▼
[Platform Surface: Headless / Win32(GDI) / D3D11(GPU) / Glfw(OpenGL) / X11 / Wayland / Wasm]
```

### 5.1 要点

- 状态变更**不**触发整树重建，只通知订阅它的 widget 子树（fine-grained）。
- **布局与渲染按脏分类按需执行**（脏区追踪，默认开启），决策矩阵见 [`specification/06-app-platform.md`](specification/06-app-platform.md) §3.2。仅绘制脏的帧跳过 `begin_frame` 保留上帧缓冲、对脏区并界先 `clear_rect` 零基底、再以 `Surface::clear_color()` 重铺脏区底色、然后 `push_clip` 裁剪重绘，使脏区重绘与整帧**逐位一致**。若不重铺底色，脏区内无不透明背景的控件（裸 `Text`、无背景 `LazyList` 子项）归零后只画字形会露出黑底。
- **脏区裁剪期间禁用 Display List 子树缓存**（`Painter::set_skip_dl_record`，部分脏路径设置、退出即清）：partial clip 下 `paint` 只遍历命中裁剪区的子节点，若此时录制 DL 会**丢失裁剪区外子节点的命令**，后续整帧回放该 DL 时这些子节点永久消失。故裁剪帧强制直绘，下帧整帧再重录完整 DL。
- **子节点视图接口 `child_nodes()` 返回 `const std::vector<Node>&`（引用，非副本）**：`Container` 直接返回 `m_children` 成员，单子容器与惰性容器以 `mutable` 成员缓存惰性重建。若按值返回 `std::vector<Node>`，临时副本析构会触发 `Node::~Node` 清空子控件的 `m_layout_parent`，遍历后子控件 `request_frame` 沿父链上溯断链、脏标记无法到达渲染根。调用方仅限**单帧内只读遍历**（`dump_tree` / `validate` / `hit_test` / inspector），树重建期间引用可能失效。
- **三端一致**：脏追踪是 `Surface` 无关的核心层改动，全部后端共用同一 `present_root`；仅各后端在上屏方式（`BitBlt` / `XPutImage` / `wl_shm` / 纹理上传 / `putImageData`）与系统重绘处理上有差异。

### 5.2 事件驱动帧循环

`Application::run` / `Window::run` 为**事件驱动 + 帧节流**模型：每帧末尾经纯函数 `compute_wait_timeout`（`window/frame_pacing.h`）决策下次唤醒，循环在 `Surface::wait_events(timeout_ms)` 阻塞。

| 情形 | 行为 |
|:---|:---|
| 完全空闲（无脏区 / 无动画 / 无定时任务） | 无限等待事件，静态界面 CPU 趋近 0 |
| 有脏区 / 动画（活跃帧） | 按 `WindowOptions::max_fps` 帧预算节流 |
| 仅定时任务 | 睡到最近到期时刻（`Scheduler::next_deadline_ms`） |
| 后端自带节拍（D3D11 vsync，`Surface::paces_frames()` 为真） | 跳过 CPU sleep，避免双重限速 |

各后端实现真阻塞：`Win32` 经 `MsgWaitForMultipleObjectsEx`、`Glfw` 经 `glfwWaitEventsTimeout`；`Headless` 为 no-op（测试以 `max_frames` 驱动，保证确定性与速度）。跨线程回投（`au::async` 的 `then` 回调）经 `Task::set_main_poster` 入队 + `Surface::request_wake()` 唤醒睡眠中的主循环，下一帧开头在主线程排水执行。

`power_saving = false` 或 `max_fps = 0` 退回忙轮询（持续重绘场景的 opt-out）。`FrameStats` 以 `wakeups_per_sec()` 与 `sleep_ratio()` 观测。

---

## 6 组件树与组合模型

- **不可变声明式树**：`Node` 持有 `Widget` 的共享引用；组件以声明式构造，状态变更驱动局部刷新。
- **组合优于继承**：布局与装饰通过 `Modifier`（`Padding` / `Background` 等）包裹节点，而非继承子类。
- **扁平组合**：深层嵌套应尽量避免；库提供 `Column` / `Row` / `Stack` / `Grid` / `Scroll` 等容器与 `Modifier` 来扁平表达。
- **控制流原语**：`Show`（条件）、`Repeater`（数据驱动列表）、`Provider`（环境注入）、`Lifecycle`（挂载副作用）、`Timer`（周期刷新）以组件形式存在，而非语言级关键字。

---

## 7 事件与命中测试

- **命中测试链**：`Widget::hit_test_chain` 返回「根 → 最深」的节点路径。
- **冒泡**：`EventDispatcher::dispatch(MouseEvent&)` 自最深向根调用 `on_pointer_event`；某节点写 `e.handled = true` 即停止向上传递。
- **纯展示控件**（如 `Text`）不置位 `wants_click()`，以便事件冒泡给父级 `Clickable`；可点击控件（如 `Button`）覆写 `wants_click()` 为「有 `on_click`」。
- **指针点击语义**：`Press` 置 `pressed`；`Release` 且 `pressed` 触发一次；点击与长按互斥（`m_click_pending` 在 Press 置位，Release 时若未触发长按且未拖拽才触发 click）。
- **焦点**：`FocusManager`（root + focused）按 `tab_index()` 排序移动（`move_focus(FocusDirection)` 循环取前 / 后）；`Widget::request_focus()` 读取派发期线程局部「当前焦点管理器」（`current_focus_manager()`），控件自身不持有 `FocusManager*`。
- **多点触控并发（按指针分发）**：`TouchDispatcher`（实例级，由 `Application` 持有）对 `TouchEvent` 按 pointer id 做命中缓存与独立路由——某 pointer id 首按做命中测试并缓存链，活跃期复用缓存链，抬起即清缓存。因此 `draggable` / `long_press` / `pinch` / `rotate` 各自绑定具体指针，支持单指持发 + 多指并发。每次 `TouchEvent` 同时 (1) 向缓存链广播完整 `TouchEvent`（原始流，供 `TouchListener` 修饰回调）、(2) 合成带 `pointer_id` 的 `MouseEvent` 驱动既有点击 / 拖拽手势。
- **悬停态基础设施**：`EventDispatcher` 在无捕获 Move 时把新命中链与上次悬停链 diff，对离开 / 进入控件回调 `Widget::on_hover_change(bool)`（默认仅记录 `m_hover` 不标脏；需要视觉反馈的控件覆写并追加 `mark_needs_paint`）；`Widget::hovered()` 供 `on_paint` 读取。Win32 宿主经 `TrackMouseEvent(TME_LEAVE)` 在光标离窗时合成远离 Move 清除悬停，否则高亮残留。

---

## 8 渲染与布局

### 8.1 渲染核心

- **`Painter`**：纯软件栅格（RGBA8 帧缓冲），不依赖 GPU；接口为纯函数式（给定节点 + 约束 → 确定像素），支持确定性快照比对。
- **圆角抗锯齿裁剪**：`Painter` 用 SDF coverage + 1px 羽化。
- **矢量描边原语**：`draw_line(a, b, width, color)`（点到线段距离 SDF，圆帽 + 1px 羽化）、`fill_rounded_rect(r, radius, color)`、`draw_rounded_border(r, radius, thickness, color)`（圆角矩形 SDF 带状覆盖、向内描边）；三者均接入 Display List 录制回放，回放与直绘逐位一致。

### 8.2 高 DPI

所有 widget / 布局坐标均为**逻辑 dp**（96 DPI 基准 = 1 dp ≈ 1 px@96）。`Surface::scale_factor()` 返回 `dpi/96`；`Painter::set_scale(scale)` 把 dp 几何 ×scale 放大到物理像素，1:1 贴窗避免发虚。事件坐标在 `Surface` 内由物理像素 `/scale` 还原为 dp。

**调用顺序不变量**：`SetProcessDpiAwarenessContext` 在「进程已有任何窗口」时失败。因此 `aurora::enable_dpi_awareness()` 必须在 `init_console()`（`AllocConsole` 会创建控制台窗口）与 `create_window()` **之前**调用。否则进程退化为 DPI 未感知，窗口以系统 DPI 虚拟化缩放（scale = 1.0），高分屏下界面发虚。

字体测量（`measure_width` / `caret_x` / `hit_test_char`）在逻辑 dp 空间进行，而光栅化（`FontEngine::draw_text`）按真实屏幕 DPI 生成物理分辨率字形——二者解耦，高 DPI 下文字清晰。

### 8.3 Scroll 离屏内容缓冲（滑窗）

`Scroll` 把内容录进与滚动偏移无关的**内容坐标滑窗缓冲** `m_content`（`unique_ptr<Painter>`），尺寸 = 视口高 × (1 + 2 × `overscan`)，仅覆盖可见区上下各 `overscan` 视口高的带而非整页；`m_buffer_origin_y` 标记该带在内容坐标系中的锚点。`ScrollProps::overscan`（默认 `1.0f`，共 3 屏厚）控制缓冲带厚度。

- 滚动帧满足「内容仍有效且为纯滚动且未触发重锚点」时，直接 `p.composite(*m_content, translate(0, m_buffer_origin_y - m_offset_y))` 一次 blit，不重新栅格。
- 视口逼近缓冲带两端时把 `m_buffer_origin_y` 重锚并**重录当前缓冲带**。
- 非滚动帧（子动画 / 内容变化）按脏区**重录当前缓冲带**。

重锚点几何与 composite 几何均用**逻辑 dp**，避免单位 bug 导致缓冲错位。该设计使缓冲字节数 = 视口像素 × 4 × (1 + 2 × `overscan`)，**与内容总量无关**。

### 8.4 后端家族

| 后端 | 上屏方式 | 开关 |
|:---|:---|:---|
| `HeadlessSurface` | 内存帧缓冲，可同步导出 PNG | `AURORA_BACKEND_HEADLESS`（默认 ON） |
| `Win32Surface` | 常驻 BGRA DIB section + `BitBlt`（RGBA→BGRA CPU swizzle），支持 `set_present_dirty` 增量上屏 | `AURORA_BACKEND_WIN32`（Windows 默认 ON） |
| `D3D11Surface` | 复用 `Win32Window` 宿主，把 `Painter` RGBA8 帧缓冲作为动态纹理，脏矩形经 `UpdateSubresource` 增量上传，全屏三角形 + 像素着色器线性采样呈现（`Present(1,0)`） | `AURORA_BACKEND_D3D11`（默认 OFF） |
| `GlfwSurface` | OpenGL 3.3 兼容剖面（绘制采用 1.1 立即模式），pimpl 隔离 | `AURORA_BACKEND_GLFW` |
| `X11Surface` | 按 Visual 掩码 CPU swizzle 后 `XPutImage`，支持增量上屏；`wait_events` 经 `poll(2)`；`scale_factor` 解析 `Xft.dpi` | `AURORA_BACKEND_X11`（默认 OFF） |
| `WaylandSurface` | CPU swizzle 到 `WL_SHM_FORMAT_XRGB8888` 经 `wl_shm` 共享内存双缓冲槽；`wait_events` 经 `poll(2)`；`scale_factor` 取 `wl_output.scale` | `AURORA_BACKEND_WAYLAND`（默认 OFF） |
| `WasmSurface` | `<canvas>` 像素写回（`EM_ASM` `putImageData`） | `AURORA_BACKEND_WASM`（默认 OFF） |
| `MacOSSurface` | AppKit / CoreGraphics | `AURORA_BACKEND_MACOS`（默认 OFF） |

`Win32Surface` 与 `D3D11Surface` 共用 `Win32Window` 宿主（创建 / 消息泵 / 事件翻译 / DPI / 同步重渲染 / 运行期标题），仅 present 后端不同。宿主采用 pimpl 隔离，故 `hwnd()` / `background_brush()` 以 `void*` 返回（调用方 `static_cast`）。窗口过程按**消息族**分派到各 `handle_*` 函数，取代单体 switch。

**自定义后端**：任意 `Surface` 子类经 `Application(Scene, unique_ptr<Surface>)` / `App().surface(...)` 注入，无需为每种后端在 `Application` 上加构造重载；`Surface` 之外的扩展点收口在 `create_window` 工厂。

**编译 / 链接期代码剪裁**：关闭某 `AURORA_BACKEND_*` 后，对应 `Surface` 子类、工厂重载与重型平台头被预处理器剔除，链接产物不再含该后端；自定义 `Surface` 注入路径不受影响，故「只用自定义 backend」可不编译任何内置后端。

---

## 9 序列化与元信息

- **树 ⇄ JSON**：`serialization::to_json` / `from_json` / `diff` / `diff_into` / `apply_patch`，结合 `WidgetRegistry`（工厂注册）。`from_json` 流程：`make` → `deserialize_props` → `adopt_children`。
- **树 → YAML**：`serialization::to_yaml(const Widget&)` / `to_yaml(const Json&)`，内部经 `yaml.h` 的递归下降发射器把 JSON 转为 YAML（仅输出方向，无 `from_yaml`）。
- **树 ⇄ 源码**：`serialization::to_code` 反向生成等效构造代码。
- **API 描述**：`gen_api_tools` 输出 `aurora_api.json`（全部 widget 类型、属性键、核心枚举），供 LSP / 文档生成 / 设计工具消费。
- **不可重建控件**：`Repeater` / `Canvas` 的工厂返回「not-restorable」错误；含它们的树 `from_json` 应作预期提示而非硬失败。

---

## 10 性能检测体系

Aurora 内置轻量级运行时性能检测体系，提供帧级指标采集、分阶段计时、可视化叠加与日志导出。整个体系零外部依赖，所有组件默认关闭、按需启用。

### 10.1 FrameStats

`include/aurora/app/perf_overlay.h`，进程级单例，128 帧环形缓冲区 O(1) 采集。

**指标**：FPS（滑动窗口平均）、平均帧时间、P50 / P95 / P99 百分位帧时间、帧时间标准差（jitter）、掉帧计数与掉帧率、hitch 计数（帧耗时超过帧预算 2 倍）、idle 帧计数。

**分阶段计时**：`record_phases(layout_ms, paint_ms, present_ms)` 独立记录三阶段耗时，64 帧环形缓冲，提供 `avg_layout_ms` / `avg_paint_ms` / `avg_present_ms`。

**帧预算**：`set_frame_budget_ms(ms)`（默认 16.67ms ≈ 60 FPS），超出即计为掉帧。`reset()` 清空状态用于基准隔离。

### 10.2 PerfOverlay

右上角叠加面板，实时显示多行统计文本（FPS / avg / P99 / jitter / 掉帧数 / hitch 数 / idle 帧数）、FPS 颜色告警（绿 ≥ 55、黄 ≥ 30、红 < 30）与帧时间条形图（最近 128 帧，超预算帧标红）。经 `PerfOverlay::set_visible(false)` 关闭显示。

**分层 HUD 叠加层（推荐用法）**：`PerfOverlay` 既可作普通 `SingleChild` 包裹内容，也推荐作为**独立 HUD 叠加层**使用——经 `Application::set_overlay(...)` / `Window::set_overlay(...)` / `App::overlay(...)` 注入后，它**脱离 widget 树**，由 `Window::present_root` 在 tree paint 之后、present 之前合成到主缓冲：

- 叠加层渲染到独立离屏 `Painter` 缓冲，仅以约 **2Hz** 重绘自身（面板背景不透明，确保叠在保留自上一帧的主缓冲之上不产生重影）；
- 每帧把缓存的 HUD 缓冲 `composite` 到主缓冲；app 树仅在**其自身脏**时重绘，叠加层刷新开销被隔离在离屏缓冲内，不再触发整树重绘；
- 叠加层内容发生 2Hz 重绘的帧强制全量上屏（HUD 像素可能落在 app 脏区之外，避免滞后 1 帧）；
- 与「把 `PerfOverlay` 作为根控件包裹内容」的旧用法**互斥**：启用叠加层后，`Scene` 根即为真实内容树，`PerfOverlay` 不应再出现在树内。

### 10.3 PerfLog

`include/aurora/perf/perf_log.h`：定期日志输出 + 快照导出。`enable(interval_frames)`（默认每 300 帧）、`on_frame_end()`、`snapshot_json()` / `snapshot_csv()`。日志经 `Log` 子系统输出，可重定向。

### 10.4 Idle 帧区分

`Window::is_idle_frame()` 判定当前帧是否为脏区跳帧；`FrameStats::record_idle()` 单独计数。无脏且尺寸未变时整帧跳过——这类 idle 帧不应污染 FPS / 帧时间 / 掉帧等渲染指标。

### 10.5 硬约束

- **快速路径逐位一致**：所有快速路径必须与慢路径 golden 零差异，修改后须跑 `test_offscreen` 全量回归。
- **SIMD 双实现确定性**：SIMD 路径必须与标量黄金路径逐位一致（`-ffp-contract=off`、同浮点运算序列、整型 `cvtt` 截断）；CI 由 `test_simd_parity` 逐位比对，一票否决。

### 10.6 调试能力的设计依据

`aurora::debug` 门面（帧缓冲 / 真实窗口截图、控件树、性能快照、可视化调试叠层、控件拾取）的 API 契约见 [`specification/06-app-platform.md`](specification/06-app-platform.md) §11。其设计取舍对标：

| 维度 | Flutter DevTools | React DevTools | Qt Creator / GammaRay | Aurora `aurora::debug` |
|:---|:---|:---|:---|:---|
| 布局可视化 | Layout Explorer | 组件树高亮（无盒模型） | 部件树 + 几何检查 | `DebugPaintFlags`（layout_guides / relayout_boundaries / layer_borders / repaint_highlight / overdraw） |
| 性能 | Performance / Timeline | Profiler（commit 火焰图） | 信号槽探查器 | `frame_phase_timeline`（L / P / R 三相位 + ASCII flamegraph）+ `PerfLog` |
| 因果链 | 无（靠经验） | 无 | 无 | `why_trace`（mark-needs 触发因果链，含 `propagated` 根因 / 传播区分） |
| 远程访问 | DevTools Server（WebSocket） | React DevTools 独立进程 | GammaRay 进程注入 | `InspectorServer` localhost REST（`/api/debug/*`） |
| 平台限制 | Skia / Impeller 后端 | DOM / reconciler | 原生 Qt | 真实窗口截图在 Wayland / Headless 不支持（不破解合成器隐私边界） |

---

## 11 设计不变量

任何改动都不得破坏以下不变量（违反会导致挂起、闪烁或行为不确定）。

1. **细粒度订阅去重**：`State::subscribe` 必须对同一 `Effect` 去重，否则约 25 帧后挂死。
2. **根挂载唯一**：`Window::present_root` 对同一根只 mount 一次。
3. **命中测试用局部坐标**：`Rect{Point{0,0}, bounds.size}.contains(local)`（非 `bounds.contains`）。
4. **事件冒泡协议**：`Widget::on_pointer_event` 写 `e.handled = true` 即停止冒泡；纯展示控件不拦截以放行父级点击。
5. **确定性渲染**：相同 widget 树 + 尺寸 → 相同像素输出（`HeadlessSurface` 快照可比对）。
6. **降级而非中止**：非法输入产出 `Diagnostics` 并降级到安全默认，不抛异常。
7. **单线程 UI**：widget 树 / 状态订阅 / 重绘调度只在主线程；`State::set` 仅限主线程。
8. **头文件尾置返回类型**：所有函数声明 / 定义用 `auto f(...) -> Ret`。
9. **强类型几何**：尺寸 / 颜色 / 长度使用 `Length` / `Color` / `px()` 等强类型，禁止裸整数隐式转换。
10. **`RelayoutBoundary` 语义**：`set_relayout_boundary(true)` 的控件成为显式重排边界——布局脏标记冒泡在边界处截断，且 `on_dirty(true)` 走局部重排而非整树重排。**`Scroll` 不得无条件置此标志**：骨架 → 真实内容的切换逻辑位于 `Scroll` 之上的祖先（由下方 `mark_needs_layout` 驱动），若 `Scroll` 成为边界会截断该脏冒泡，使内容切换永不触发（离屏缓冲恒为骨架）。仅在确有「子树自包含、且切换由本控件自身驱动」的语义时才置位。
11. **几何权威在 `Node`**：`Widget` 不持有任何几何缓存。

---

## 12 错误处理架构

错误处理遵循「**机器可解析的错误**」约束。错误码目录（数据）由 `tools/gen_error_codes.cpp` 自动生成并维护于 [`ERROR_CATALOG.md`](ERROR_CATALOG.md)，**请勿手改**。编码层面的写法规则见 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §1。

### 12.1 设计目标

每个错误同时服务于两类受众：

- **进程外**（JSON / 日志 / IDE 工具 / AI 编码助手）：只认冻结 `slug`（如 `"nav-depth-exceeded"`，改名标识符也不变）与 `code_enum`。
- **进程内**：用 `code_enum` 做类型安全分支，用 `severity` / `category` / `auto_fixable` / `retryable` 等元数据做策略判断。

失败路径统一：可恢复失败经 `Result<T>` 沿调用链返回，不靠异常横跨业务边界。

### 12.2 类型模型

`Error`（`core/result.h:25`）字段分两类受众：`code` / `code_enum` 供机器解析；`severity` / `category` / `auto_fixable` / `retryable` / `fix_category` / `fix_params` 供进程内策略判断；另有 `message` / `suggestion` / `docs` / `where` / `hint` 供人与 AI 阅读。所有表驱动元数据由 [`errors.toml`](errors.toml) 经生成器产出，经 `make_error` 自动填充，无需手填。

`Result<T>` 成功持 `T`、失败持结构化 `Error`；`Result<void>` 特化用于只关心「是否出错」的接口（如 `flush` / `reload`），以 `bool` 标记成功态。

`unwrap()` 仅在不可恢复场景下把错误转为 `std::runtime_error`，业务边界不应依赖它做流程控制。

### 12.3 分类维度

| 维度 | 取值 | 用途 |
|:---|:---|:---|
| `ErrorCategory` | `General` / `Layout` / `Widget` / `Render` / `Io` / `Validation` / `Navigation` / `Platform` / `Runtime` / `Generation` / `Diagnostic`（共 11 域） | 错误域归类，过滤与聚合 |
| `ErrorSeverity` | `Info` / `Warning` / `Error` / `Fatal` | 严重度分级，决定上报与中断策略 |
| `auto_fixable` | `bool` | 是否可被工具 / IDE 自动修复 |
| `retryable` | `bool` | 是否可重试（如异步超时） |
| `fix_category` / `fix_params` | 字符串 | 修复策略分类与参数 |

### 12.4 生成管线

```text
codespec/errors.toml          (源：slug / severity / category / 元数据 / message 模板)
        │  tools/gen_error_codes.cpp
        ▼
  include/aurora/core/error_codes.gen.h   (ErrorCode 枚举 + 表)
  codespec/ERROR_CATALOG.md               (人类可读目录)
  aurora_api.json                          (error_codes 段，供 AI / 工具消费)
```

- **`slug` 冻结对外契约**：跨语言 / JSON / 日志只认 `slug`；`enum` 为 C++ 标识符可自由改名。
- **只增不删**：新增错误码追加，不重用 / 删除已发布 `slug`。
- 三处生成物读现有文件、只写各自段，可任意顺序运行。

### 12.5 传播策略

- **可恢复失败**：沿调用链返回 `Result<T>`，调用方用 `if (result)` / `result.ok()` 检查；所有返回 `Result` 的接口标注 `[[nodiscard]]`，避免吞错。
- **不可恢复错误**（断言边界）：使用 `assert` / 前置条件检查，见 `include/aurora/core/assert.h` 与 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §1。

---

## 13 AI-first 设计原则

本库从立项起就面向「AI 编码助手友好」设计。以下为架构与设计理念层的原则；**编码规则层**的条目（命名 / 错误 / 工具链等）见 [`CODING_STANDARDS.md`](CODING_STANDARDS.md)，跨框架概念映射与核心概念审计见 [`CONCEPTS.md`](CONCEPTS.md)。

1. **Token 经济性**：API 表面保持紧凑，核心概念可枚举、可命名、可映射，使 AI 在有限上下文窗口内即可装载全部概念。
2. **概念可枚举性**：全部 UI 原语可枚举、可命名、可映射。
3. **概念映射透明性**：Aurora 概念与 React / Flutter / Qt 一一对应。
4. **声明式优于命令式**：组件以不可变声明式树表达，状态变更驱动局部刷新；禁止命令式事后改树。
5. **最小正交 API**：布局用 `Column` / `Row` / `Stack` / `Grid` / `Scroll`，装饰用 `Modifier`，状态用 `State` / `Signal` / `Store`，互不重叠；避免「多种方式做同一件事」带来的选择困惑。
6. **线程模型**：单线程 UI；`State::set` 仅限主线程（跨线程结果须经 `au::async` / `Task::set_main_poster` 回投后再写）。在文档与示例中明确标注，避免 AI 误用多线程改树。
7. **显式数据流**：状态经 `State` / `Signal` / `Store` 显式流动，依赖图可静态推导，便于 AI 推断「改 X 会影响哪些 widget」。
8. **单向 / 纯函数**：状态变更单向；`Computed` 为纯函数派生值，无副作用，可安全重算。
9. **扁平组合**：以 `Modifier` 包裹 + 容器组合替代深层继承嵌套。
10. **布局代数**：布局以可组合的 `Constraints` + 对齐参数表达，避免魔法数字。
11. **事件模型**：统一事件类型（`MouseEvent` / `KeyEvent` / `ScrollEvent` / `TextInputEvent` / `TouchEvent` / `FileDropEvent`）+ 冒泡协议；事件坐标在 `e.position` 而非 `e.x` / `e.y`。
12. **状态作用域**：状态作用域显式——局部 `State` 由组件持有，全局 `Store` 由应用持有并通过 `Environment` 注入；`Binding<T>` 为非拥有引用，上游生命周期须更长。

---

## 14 测试与 CI 架构

### 14.1 目标

- **可回归**：每次变更可经 `ctest` 全量复跑，失败即阻断。
- **渲染零差异（golden）**：光栅输出像素级稳定，快速路径 / SIMD 路径必须与标量黄金路径逐位一致。
- **跨平台一致**：同一测试矩阵覆盖 Linux / Windows / macOS。

### 14.2 分层

**单元测试（`tests/test_*.cpp`）**：每个公共源文件对应一个 `test_*.cpp`（与 `examples/demos/demo_*.cpp` 同构：1 源文件 ↔ 1 测试 ↔ 1 demo）。经 `cmake/AuroraTests.cmake` 收集（`file(GLOB CONFIGURE_DEPENDS)`），**全部用例链入单一可执行 `aurora_test_runner`**：用例用 `AURORA_TEST()` 宏静态自注册（用例名 = 文件名 stem），`main()` 由 `tests/au_test_main.cpp` 唯一提供。CTest 逐条以 `aurora_test_runner --run=<stem>` 注册（进程隔离），并由 `registry_integrity` 守护漏注册。

**Golden 测试（渲染像素级）**：以 `test_offscreen` 为主，把 widget 树渲染到 `HeadlessSurface` 内存缓冲，与 golden 基准图逐像素比对。依赖相对路径，须从**仓库根**运行（`ctest` 已为其把 CWD 设为仓库根），可用 `AURORA_GOLDEN_DIR` 覆盖解析基准。

**性能基准**：见 §10 与 [`specification/06-app-platform.md`](specification/06-app-platform.md) §10。

### 14.3 组织约定

- **命名**：测试文件以 `test` 为**前缀**（`test_*.cpp`，非 `_test` 后缀），与源文件同名主体。
- **运行**：`ctest -R <名>` 逐条拉起 `aurora_test_runner --run=<stem>`；从仓库根运行以保证相对路径解析；本地复跑以最高并行度执行（`ctest -j` 配满核心）。共享资源竞争用例（剪贴板、计时）以 `RUN_SERIAL` 单独隔离错峰，而非把整套退回串行。
- **新增约束**：新增公共 API / widget / 核心逻辑须配套单测并接入 CTest。

### 14.4 CI 执行层

CI 配置位于 `.github/workflows/`：

| 工作流 | 作用 |
|:---|:---|
| `ci.yml` | 三平台矩阵（ubuntu/gcc、windows/msvc、macos/clang），每推送 / PR 跑 `configure → build（库 + 工具 + 测试）→ ctest --output-on-failure`；`concurrency` 取消旧运行以提速 |
| `release.yml` | 发布流程（构建产物 / 版本标签） |

CI 只负责「拉起构建 + 跑 CTest」，不承载测试设计。覆盖率门禁由本地约束，不在 CI 架构内。
