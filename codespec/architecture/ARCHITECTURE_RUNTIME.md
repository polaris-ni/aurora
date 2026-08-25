# ARCHITECTURE_RUNTIME

> 本文件由 [`ARCHITECTURE.md`](../ARCHITECTURE.md) 划分而出（运行时 / 模块映射 / 核心数据流）。章节编号保持原样。
> 返回主线见 [`ARCHITECTURE.md`](../ARCHITECTURE.md)。

**本文包含章节：**

- [3. 运行时（Runtime）](#3-运行时runtime)
- [4. 模块映射（Module Map）](#4-模块映射module-map)
- [5. 核心数据流（Core Data Flow）](#5-核心数据流core-data-flow)

## 3. 运行时（Runtime）

- **单线程 UI（Single-threaded UI）**：所有 widget 树操作、状态变更、事件处理都在主线程。
  状态写操作（`State::set`）可来自任意线程，但订阅回调与重绘调度在主线程串行化。
- **同步事件（Synchronous events）**：`EventDispatcher` 在收到原生平台事件后同步派发，
  命中测试链（hit-test chain）自最深节点向根冒泡，写 `e.handled = true` 即止。
- **几何权威在 `Node`**：命中链的根矩形取自根 `Node` 的 `m_bounds`（由 `Window::present_root` 写入窗口矩形）；
  子树几何由容器在布局时经 `child.set_bounds(box)` 写入各 `Node`。`Widget` **不再持有任何几何缓存**
  （`m_bounds`/`bounds()`/`set_bounds()` 已彻底移除）。输入坐标本地化上移至 `EventDispatcher`：命中链
  `hit_test_chain` 返回 `std::vector<HitNode>{Widget* widget, Point origin}`，派发器在冒泡每个控件前写入
  `MouseEvent::local_position = position - origin`，控件在 `on_pointer_event` 中直接消费本地坐标，无需查询自身绝对位置。
- **响应式细粒度信号（Fine-grained reactivity）**：`State<T>` / `Signal` 订阅精确到
  具体订阅者；状态变更只刷新依赖它的 widget，避免整树重绘。
- **观察者图生命周期安全**：信号源与 `Effect` 经 `Connection`（`weak_ptr` 锚点）连接，`notify()` 惰性摘除失效边，**任一侧先析构均不解引用悬垂对象**（消除此前双向裸指针的悬垂隐患，见 `state/state.h` / `state/effect.h`）。
- **跨帧缓存的控件引用一律「弱引用守卫 + 裸指针回退」**：命中链 / 悬停链 / 指针捕获（`HitNode`）与焦点记录
  （`FocusManager`）都会**跨事件、跨帧**缓存控件引用，而控件可能在两次使用之间被销毁（`LazyList` 滚动回收子项、
  `push_replacement` 重建页面、用户 `on_click` 里销毁自身子树）。统一模式：构造时探测该控件是否由 `shared_ptr`
  持有——是则存 `weak_ptr` 守卫并在取用时判活，否则（栈/成员控件的 `weak_from_this()` 为空弱引用）回退为裸指针，
  其生命周期由持有者保证。**只用 `weak_ptr` 会让栈上控件恒 `lock` 失败而静默吞掉全部事件，只用裸指针则是
  use-after-free，两者都不可取。**
- **同步回调期间必须持住强引用**：事件派发是同步的，用户回调可以在回调内销毁触发它的控件；而回调返回后
  框架仍要继续访问该控件（`Widget::on_pointer_event` 写 `m_pressed` 等）。因此凡要解引用跨帧缓存的控件引用，
  都须在**整个调用期间**持有强引用（`HitNode::lock(keepalive)`），而非仅做一次「取指针 + 判空」。
- **非拥有登记必须成对注销**：`Animator::drive`/`bind` 以裸指针登记控制器与目标 `State`。若被登记对象的生命周期
  短于 `Animator`（典型：widget 成员控制器注册进应用级 `Animator`），**必须**在其析构函数中 `Animator::remove(c)`，
  否则下一帧 `tick` 即写入已释放内存。凡新增「把自身成员注册进更长寿命管理器」的代码，都要同时写好注销路径。
- **确定性渲染**：`Painter` 为纯函数式绘制（给定 widget 树 + 尺寸 → 确定像素），
  便于 `HeadlessSurface` 输出可比对快照（golden test）。
- **降级而非中止（Graceful degradation）**：非法输入/部分代码缺失产出 `Diagnostics`
  并降级到安全默认，而非抛异常崩溃。

---

## 4. 模块映射（Module Map）
> 路径前缀均为 `include/aurora/`。实现位于 `src/aurora/*`（静态库）。
> 模块按架构分层分组；每组下列「模块 / 路径 / 职责」表，超长说明以条目置于表下。
> 头文件引用的最终校验见 `tools/check_arch_module_map.py`（运行：`python3 tools/check_arch_module_map.py`，CI 可门禁：缺失/歧义/错位引用返回非零退出码）。

### 4.1 基础层与响应式核心

| 模块 | 路径 | 职责 |
|------|------|------|
| 基础层 | `core/` | `types.h` `result.h`(`Result<T>`/`Error`) `log.h` `diagnostics.h` `color.h` `dimension.h` `image.h` `font.h` `expected.h` `immutable.h` `strict_mode.h` `event_stream.h` |
| 响应式核心 | `state/` | `state.h` `computed.h` `effect.h` `binding.h` `store.h` `reactive.h` `signal_view.h`(`Signal`/`SignalView`) `async.h` `state_graph.h` `state_registry.h` |

### 4.2 布局与渲染

| 模块 | 路径 | 职责 |
|------|------|------|
| 布局引擎 | `layout/` | `flex.h` `flex_layouter.h` `layout_box.h` `layout_engine.h`（`Constraints` 在 `core/types.h`） |
| 渲染核心 | `render/` | `painter.h` `font_engine.h` `bitmap_font.h` `png.h` `offscreen.h`(`render_to_png`) `blend.h` `dirty_region.h` `display_list.h` `font_discovery.h` `glyph_atlas.h` `image_cache.h` `snapshot_diff.h` `text_aa_mode.h` |

### 4.3 平台抽象

| 模块 | 路径 | 职责 |
|------|------|------|
| 平台抽象 | `window/` | `surface.h`(`Surface`/`HeadlessSurface`) `window.h` `native_surfaces.h` `win32_window.h`(`Win32Window`) `win32_surface.h`(`Surface::native_handle()` 返回 `void*`，Win32 下实为 `HWND`) `glfw_surface.h` `d3d11_surface.h` `platform.h`(`au::platform()`/`Platform`/`PlatformCapabilities`) |

- `Surface` 为可扩展边界：自定义 Surface 经 `Application(Scene, unique_ptr<Surface>)` 注入，不随内置 Surface 增长；各后端由 feature 宏 `AURORA_BACKEND_*` 控制代码剪裁（见 `window.h` 顶部契约）。
- `win32_window.h`（共享窗口宿主、消息泵、`WM_DROPFILES` 文件拖放）与 `glfw_surface.h`：均 **pimpl 隔离**，公共头零 `<windows.h>` / GLFW / OpenGL 依赖。

### 4.4 组件层、修饰与控制流

| 模块 | 路径 | 职责 |
|------|------|------|
| 组件层 | `widget/` | `widget.h` `text.h` `text_input.h` `button.h` `image_widget.h`(`ImageView`) `checkbox.h` `switch.h` `slider.h` `canvas.h` `progress.h` `divider.h` `rich_text.h` `scroll.h` `stack.h` `grid.h` `spacer.h` `show.h` `repeater.h` `provider.h` `timer.h`(`Timer`) `containers.h`(`Column`/`Row`) `text_span.h` `codegen.h`(`to_code`) `inspect.h` `inspector_panel.h`(`InspectorPanel`+`export_code`) `layout_query.h` `props_io.h` `serialization.h`(`to_json`/`from_json`/`diff`/`apply_patch`/`to_yaml`) `yaml.h`(`to_yaml` YAML 发射器) `recipes.h` |
| 修饰节点 | `modifier/` | `modifier.h`(`Padding`/`Background`/`Border`/`Clip`/`Opacity`/`SizeModifier`/`FlexWeight`/`Clickable`/`AlignNode`/`OffsetNode`/`Draggable`/`LongPress`/`TouchListener`) |
| 控制流 | `widget/` | `show.h` `repeater.h` `provider.h` `timer.h`（控制流语义，复用组件层定义） |
| 序列化 | `widget/serialization.h` `widget/codegen.h` `widget/yaml.h` | 树 ⇄ JSON（`to_json`/`from_json`）、差异补丁（`diff`/`apply_patch`）、树 ⇄ 源码（`to_code`）、树 → YAML（`to_yaml`） |

### 4.5 Inspector 远程接口

| 模块 | 路径 | 职责 |
|------|------|------|
| Inspector 远程接口 | `inspector/inspector_server.h` | `InspectorServer`：localhost-only HTTP 服务器，暴露 REST 端点远程访问运行时 widget 树。 |

- 端点：`/api/tree`、`/api/widget`、`/api/components`、`/api/yaml`、`/api/to_code`；`/api/debug/*` 收编 `aurora::debug` 门面（`state`/`perf`/`timeline`/`diagnostics`/`why`/`tree`/`snapshot`/`pick`，及 `POST /api/debug/flags` 运行时开关可视化叠层）。
- 所有 Surface / 树 / 全局状态读取经主线程 marshal（复用 `aurora::detail::main_poster`），与 `Surface` 的 main-thread-only 约束一致。
- CMake `AURORA_BUILD_INSPECTOR_SERVER`；跨平台：Windows=Winsock2 / Linux·macOS=BSD sockets。

### 4.6 导航 / 动画 / 主题

| 模块 | 路径 | 职责 |
|------|------|------|
| 导航 | `navigation/` | `navigator.h`(`Navigator`/`RouteRegistry`/`open_uri`) `route.h`(`Route`) `router.h`(`Router`) `transition_layer.h`(`TransitionLayer`) `navigator_host.h`(`NavigatorHost`) `hero.h`(`Hero` 共享元素转场)（`RouteTransition` 在 `route.h`） |
| 动画 | `animation/` | `timeline.h`(`Tween`/`Keyframes`) `animator.h` `easing.h`(`Curve`) `spring.h`(`SpringSimulation`) |
| 主题 | `theming/` | `theme.h`(`Theme`) `theme_scope.h` `theme_query.h` |

### 4.7 事件 / 环境 / 国际化

| 模块 | 路径 | 职责 |
|------|------|------|
| 事件 | `event/` | `event.h` `dispatcher.h`(`EventDispatcher`/`TouchDispatcher` 按 pointer id 指针捕获与并发路由) `gesture.h`(`PinchRecognizer`/`RotationRecognizer` 锁定 pointer id 对) `focus.h`(`FocusManager`) `keycode.h` |
| 环境注入 | `environment/` | `environment.h` `media_query.h` `build_context.h` |
| 国际化 | `i18n/` | `locale.h` `localized_string.h` `string_table.h` |

### 4.8 应用驱动

| 模块 | 路径 | 职责 |
|------|------|------|
| 应用驱动 | `app/` | `application.h`(`Application`：`dispatch_file_drop` 便捷派发) `scheduler.h`(`Scheduler`/`TimerHandle` 定时任务、帧循环驱动) `scene.h`(`Scene`) `clipboard.h`(`Clipboard`：文本 + 图像 `set_image`/`get_image`(CF_DIB)，非 Win32/Headless no-op) `file_dialog.h`(`file_dialog_win32.cpp`：`IFileOpenDialog`/`IFileSaveDialog` COM 实现) `system_tray.h`(`system_tray_win32.cpp`：`Shell_NotifyIcon` 实现) `display.h`(`display_win32.cpp`：`EnumDisplayMonitors` 多显示器枚举 + `move_window_to_display`/`display_containing` 窗口迁移，依赖 `Surface::native_handle()`) `perf_overlay.h`(`FrameStats`/`PerfOverlay` 性能检测与叠加面板) `menu.h`(`MenuBar`/`Menu`) `shortcuts.h`(`Shortcut`/`Shortcuts`) |

### 4.9 持久化配置

| 模块 | 路径 | 职责 |
|------|------|
| 持久化配置 | `preferences/` | `preferences.h`(`aurora::preferences::Preferences`：JSON 文件后端、响应式键值存储、主动 `flush`；属状态/存储层扩展，**不新增 UI 控件**） |
| 数据存储抽象层 | `storage/` | `storage.h`(`Storage` 门面) `backend.h` `memory_backend.h` `fs_backend.h` `serializable.h` `storage_types.h`；对标 `preferences` 但与 UI 控件解耦，可注入任意后端（SQLite 等）。详细设计见 [§4.11](#411-数据存储抽象层设计storage)。 |

### 4.10 性能 / 调试 / 媒体 / 图像编解码 / 入口

| 模块 | 路径 | 职责 |
|------|------|
| 性能检测 | `perf/` | `perf_log.h`(`PerfLog` 定期日志输出与 JSON/CSV 快照导出) `counters.h`(`Counters`) `profiler.h`(`Profiler`) `stopwatch.h`(`Stopwatch`) `perf_session.h`(`PerfSession`) `trace_writer.h`(`TraceWriter`) `scroll_bench.h`(`ScrollBench`) |
| 媒体 | `media/` | `video_player.h`(`VideoPlayer`) `video_source.h`(`VideoSource`) `video_controls.h`(`VideoControls`) `image_sequence_source.h`(`ImageSequenceSource`) |
| 调试 | `debug/` | `debug_backend.h` `debug_paint.h` `debug_runtime.h` `debug_trace.h`（`aurora::debug` 门面，供 Inspector `/api/debug/*` 调用，详见 `SPECIFICATIONS.md` §H.10c） |
| 图像编解码 | `image/` | `image_codec.h`(`ImageCodec`) |
| 入口 | `aurora.h` | 聚合 include + `namespace au` 别名提示 |

### 4.11 数据存储抽象层设计（Storage）

> 存储子系统统一抽象（门面 + 可注入后端），与 UI 控件解耦、可承载任意后端（内存 / 文件系统 / 未来 SQLite 等）。**已实现**（门面 + Memory/FS 后端已落地于 `src/aurora/storage/*`）；本小节为架构权威描述，API 契约以 `include/aurora/storage/*.h` 落地的声明为准。

**模块布局**（`include/aurora/storage/`）：`storage.h`（`Storage` 门面 + `default_instance`）、`backend.h`（`StorageBackend` 抽象 + 默认 `transaction`）、`memory_backend.h`（`MemoryBackend`：内存快照回滚）、`fs_backend.h`（`FilesystemBackend`：默认零依赖文件系统后端）、`serializable.h`（`StorageSerializable` / `StorageBinarySerializable` / `StorageStorable` 概念与 ADL 定制点）、`storage_types.h`（`StorageRecord` 信封 / `Json` / `StorageBytes` / `StorageChange`）。实现位于 `src/aurora/storage/*`。

**核心抽象契约**：
- `Storage` 门面：`put<T>(key, value)` / `get<T>(key)` / `remove(key)`（类型化、经 `StorageSerializable` ADL 定制点序列化）；`async_put<T>` / `async_get<T>`（返回 `Future`，把 IO 卸载出 UI 线程）；`on_change(key?, cb)` 返回 `aurora::Subscription`（键级或全局变更通知）；`transaction([](Tx&){...})`（跨记录原子，失败全回滚）；进程级 `default_instance()` 单例。
- `StorageBackend` 抽象：`put_raw` / `get_raw` / `remove_raw` / `list_keys` + 默认 `transaction`（顺序 apply + 异常回滚）；`MemoryBackend` 以 `std::map` 全量快照实现回滚；`FilesystemBackend` 每记录一文件（原子写 `tmp`+rename）、目录级锁串行化事务。
- 信封 `StorageRecord{ key, value(Json|StorageBytes), version, timestamp, type_tag }`：版本号支撑乐观并发与迁移；`StorageChange{ key, old_value, new_value, reason }` 供 `on_change` 投递。

**错误模型**：统一经 `Result<T>` / `Error`；后端 IO 失败（文件损坏 / 权限 / 磁盘满）返回 `Error` 而非抛异常；`get` 未命中返回 `Error{NotFound}`（区分于 `null` 值）；事务中途失败回滚并报告首个失败原因。`Error` 携带 `category`/`code`/`message`，机器可解析（见 SPECIFICATIONS 错误子系统）。

**线程模型**：门面 API 主线程调用、经内部队列 marshal 到后台 IO 线程（不阻塞 UI）；`async_*` 天然异步；同步 `get` 在主线程命中内存缓存时零等待、未命中则经 future 等待（调用方自行决定）。后端实现须线程安全（FS 后端目录锁、内存后端原子换页）。

**CMake**：`AURORA_BUILD_STORAGE`（默认 ON，静态库内部模块，不 PUBLIC 传播）；实现期若引入 SQLite 等三方依赖，须以 `AURORA_STORAGE_SQLITE` 子开关隔离、默认 OFF，保持默认零三方依赖。

**与 `preferences` 的关系**：`Storage` 是更通用的持久化抽象（信封 / 类型化 / 异步 / 事务 / 可注入后端），`preferences::Preferences` 是面向「响应式键值 + JSON 文件」的轻量特化；新代码默认优先 `Storage`，`preferences` 保留为兼容层。

---

## 5. 核心数据流（Core Data Flow）

```
[State/Signal 变更]
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
[Platform Surface: Headless / Win32(GDI) / D3D11(GPU) / Glfw(OpenGL)]
```

要点：
- 状态变更 **不** 触发整树重建，只通知订阅它的 widget 子树（fine-grained）。
- **`Layout` 与 `Render` 按脏分类按需执行**（脏区域追踪，默认开启）：无脏且尺寸/根未变 → 整帧跳过（idle 零开销）；仅绘制脏（如选区高亮、主题切换）→ **跳过整树 `layout`** 复用已缓存几何直接 `paint`，且 `present_root` 跳过 `begin_frame` 保留上帧缓冲、对脏区并界先 `clear_rect` 零基底、再以 `Surface::clear_color()`（后端 `begin_frame` 铺的底色，窗口后端为浅灰 `(245,245,247)`、`Headless`/`D3D11` 为透明零基底）重铺脏区底色、然后 `push_clip` 裁剪重绘，使脏区重绘与整帧**逐位一致**（golden 零差异）——若不重铺底色，脏区内无不透明背景的控件（裸 `Text`、无背景 `LazyList` 子项）归零后只画字形会露出黑底；布局脏/尺寸变化 → `layout + paint`。`HeadlessSurface` 可同步产出 PNG。
  - **脏区裁剪期间禁用 Display List 子树缓存**（`Painter::set_skip_dl_record`，`present_root` 部分脏路径设置、退出即清）：partial clip 下 `paint` 只遍历命中裁剪区的子节点，若此时录制 DL，会**丢失裁剪区外子节点的命令**，后续整帧 replay 该 DL 时这些子节点永久消失（与整帧重绘逐位不一致）。故裁剪帧强制 `render_into` 直绘（Painter 裁剪栈保证越界像素被裁），下帧整帧再重录完整 DL。
  - **子节点视图接口 `child_nodes()` 返回 `const std::vector<Node>&`（引用，非副本）**：`Container` 直接返回 `m_children` 成员，`SingleChild`/`Drawer`/`Splitter`/`TabBar`/`LayoutBuilder` 以 `mutable` 成员缓存惰性重建。若按值返回 `std::vector<Node>`，临时副本析构会触发 `Node::~Node` 清空子控件的 `m_layout_parent`（树所有权语义），遍历后子控件 `request_frame` 沿父链上溯断链、脏标记无法到达渲染根（历史 bug：grid_rows 滚动失效）。调用方仅限**单帧内只读遍历**（`dump_tree`/`validate`/`hit_test`/inspector），树重建期间引用可能失效。
- **三端一致**：脏追踪是 `Surface` 无关的核心层改动，`Win32`/`D3D11`/`Glfw`/`Headless` 共用同一 `present_root`；仅 `Win32`/`D3D11` 经共享 `Win32Window` 宿主补全 `WM_PAINT` + 浅灰背景刷消除最大化黑屏，并在 `WM_SIZE`/`WM_PAINT` 经 `Surface::set_present_request` 回调**同步重渲染**当前根到新尺寸，消除 DWM 最大化动画首帧的放大区域白闪（`Glfw` 双缓冲浅灰 `glClear`、`Headless` 无窗口天然无黑屏）。`D3D11` 另在 `present` 时按 `set_present_dirty` 收到的脏矩形增量上传纹理。

### 5.1 事件驱动帧循环（CPU 性能专项）

`Application::run` / `Window::run` 为**事件驱动 + 帧节流**模型（非历史的忙轮询）：每帧末尾经纯函数
`compute_wait_timeout`（`window/frame_pacing.h`）决策下次唤醒，循环在 `Surface::wait_events(timeout_ms)` 阻塞：
- **完全空闲**（无脏区/无动画/无定时任务）→ 无限等待事件，静态界面 CPU 趋近 0；
- **有脏区/动画**（活跃帧）→ 按 `WindowOptions::max_fps` 帧预算节流；
- **仅定时任务**→ 睡到最近到期时刻（`Scheduler::next_deadline_ms`）；
- **后端自带节拍**（D3D11 vsync `Present(1,0)`，`Surface::paces_frames()` 为真）→ 跳过 CPU sleep 避免双重限速。

各后端实现真阻塞：`Win32` 经 `MsgWaitForMultipleObjectsEx(QS_ALLINPUT|MWMO_INPUTAVAILABLE)`、`Glfw` 经
`glfwWaitEventsTimeout`；`Headless` 为 no-op（测试以 `max_frames` 驱动，保证确定性与速度）。跨线程
回投（`au::async` 的 `then` 回调）经 `Task::set_main_poster` 入队 + `Surface::request_wake()`（`Win32` = `PostMessage(WM_NULL)`、
`Glfw` = `glfwPostEmptyEvent`）唤醒睡眠中的主循环，下一帧开头在主线程排水执行（单线程 UI 不变）。
`power_saving=false` 或 `max_fps=0` 退回旧忙轮询（持续重绘场景的 opt-out）。`FrameStats` 新增 `wakeups/s`
与 `sleep_ratio` 观测（`PerfOverlay` 第三行）。

---

