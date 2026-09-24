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
│  Render Core (include/aurora/render/)     │  渲染核心：DisplayList → RHI 后端（Painter 软件栅格）
├───────────────────────────────────────────┤
│  Platform Abstraction (include/aurora/window/) │  平台抽象：Surface 家族
├───────────────────────────────────────────┤
│  Foundation (include/aurora/core/)        │  基础层：types / Result / Error / Log
└───────────────────────────────────────────┘
```

- **依赖方向以向下为主，`core/` 为硬边界**：`core/`（基础层）的**公共头不依赖任何其他 aurora 模块**（只依赖自身与标准库）；其余层间以「上层依赖下层」为常态，确有必要时下层可反向引用上层（例：`render/offscreen.h` 的 `render_to_logical_snapshot` 需读取 widget 类型名与属性以产出逻辑快照；`window/` 的 `Surface` 家族需渲染类型）。该边界由门禁 `tools/check/check_core_layer_boundary.py` 守护（扫描 `include/aurora/core/**` 的 `#include`，任何指向上层的包含即失败）。`src/aurora/core/` 的实现**不在门禁范围**——其 2 处既知上层引用属叶子级、非传递性的元数据查阅：`diagnostics.cpp` 读 `widget/props_io.h` 以格式化属性值、`image.cpp` 委派 `image/image_codec.h` 解码。
- **内核 / 适配分离**：平台差异收敛到 `Surface` 实现。
- **命令流与执行分离**：`DisplayList` 是绘制命令的**唯一来源**，`rhi::RhiBackend` 是它的**回放目标抽象**（command sink）；软件 `Painter`（经 `rhi::SoftwareRhi`）与 GPU 后端是它的**平级消费者**。新增后端只实现 `RhiBackend`，不改动录制侧与 `DisplayList`。
- **响应式与渲染解耦**：状态变更经 `State` / `Signal` 精确投递到受影响的 widget 子树，不经「整树 diff + 重建」；渲染核心只负责把 widget 树绘制到 `Surface`。

---

## 3 运行时

### 3.1 线程与事件模型

- **单线程 UI**：所有 widget 树操作、状态变更、事件处理都在主线程。`State::set` 仅限主线程调用（赋值 + notify 无锁无原子）；跨线程计算结果须经 `au::async` / `Task::set_main_poster` 回投主线程后再写 `State`。
- **多窗口（仍在同一线程内）**：一个 `Application` 持有一组 `WindowHost`（各自 `Window` + `Scene` + `FocusManager` + 指针/触控捕获表 + 帧统计），由**单一帧循环**统一驱动（`Application::run`，不再委托 `Window::run`）。**焦点与捕获不跨窗口**；`Animator` / `Scheduler` / 快捷键 / 命令注册表为应用级共享（`dt` 每帧只推进一次）。详见 `specification/06-app-platform.md` §2.4。
- **同步事件**：`EventDispatcher` 在收到原生平台事件后同步派发，命中测试链自最深节点向根冒泡，写 `e.handled = true` 即止。
- **响应式细粒度信号**：`State<T>` / `Signal` 订阅精确到具体订阅者；状态变更只刷新依赖它的 widget，避免整树重绘。

### 3.2 几何权威

命中链的根矩形取自根 `Node` 的 `bounds_`（由 `Window::present_root` 写入窗口矩形）；子树几何由容器在布局时经 `child.set_bounds(box)` 写入各 `Node`。`Widget` **不持有任何几何缓存**。

输入坐标本地化在 `EventDispatcher`：命中链 `hit_test_chain` 返回 `std::vector<HitNode>`，派发器在冒泡每个控件前写入 `MouseEvent::local_position = position - origin`，控件在 `on_pointer_event` 中直接消费本地坐标。

### 3.3 跨帧缓存引用的生命周期

命中链、悬停链、指针捕获（`HitNode`）与焦点记录（`FocusManager`）都会**跨事件、跨帧**缓存控件引用，而控件可能在两次使用之间被销毁（`LazyList` 滚动回收子项、`push_replacement` 重建页面、用户 `on_click` 里销毁自身子树）。统一模式：

- 构造时探测该控件是否由 `shared_ptr` 持有——是则存 `weak_ptr` 守卫并在取用时判活；
- 否则（栈 / 成员控件的 `weak_from_this()` 为空弱引用）回退为裸指针，其生命周期由持有者保证。

> 只用 `weak_ptr` 会让栈上控件恒 `lock` 失败而静默吞掉全部事件；只用裸指针则是 use-after-free。两者都不可取。

**同步回调期间必须持住强引用**：事件派发是同步的，用户回调可以在回调内销毁触发它的控件；而回调返回后框架仍要继续访问该控件（`Widget::on_pointer_event` 写 `pressed_` 等）。因此凡要解引用跨帧缓存的控件引用，都须在**整个调用期间**持有强引用（`HitNode::lock(keepalive)`），而非仅做一次「取指针 + 判空」。

**观察者图生命周期安全**：信号源与 `Effect` 经 `Connection`（`weak_ptr` 锚点）连接，`notify()` 惰性摘除失效边，任一侧先析构均不解引用悬垂对象。

**非拥有登记必须成对注销**：`Animator::drive` / `bind` 以裸指针登记控制器与目标 `State`。若被登记对象的生命周期短于 `Animator`（典型：widget 成员控制器注册进应用级 `Animator`），**必须**在其析构函数中调用 `Animator::remove(c)`，否则下一帧 `tick` 即写入已释放内存。凡新增「把自身成员注册进更长寿命管理器」的代码，都要同时写好注销路径。

### 3.4 确定性与降级

- **确定性渲染**：`Painter` 为纯函数式绘制（给定 widget 树 + 尺寸 → 确定像素），便于 `HeadlessSurface` 输出可比对快照（golden test）。
- **降级而非中止**：非法输入 / 部分代码缺失产出 `Diagnostics` 并降级到安全默认，而非抛异常崩溃。

---

## 4 模块映射

路径前缀均为 `include/aurora/`，实现位于 `src/aurora/`（静态库）。头文件引用的最终校验见 `tools/check/check_arch_module_map.py`（缺失 / 歧义 / 错位引用返回非零退出码，可作 CI 门禁）。

### 4.1 基础层与响应式核心

| 模块 | 路径 | 主要头文件 |
|:---|:---|:---|
| 基础层 | `core/` | `types.h` `result.h`（`Result<T>` / `Error`） `log.h` `diagnostics.h` `color.h` `dimension.h` `image.h` `font.h` `expected.h` `strict_mode.h` `event_stream.h` `accessibility.h` `a11y_types.h` `a11y_provider.h` `a11y_text.h` |
| 响应式核心 | `state/` | `state.h` `computed.h` `effect.h` `binding.h` `immutable.h`（`Immutable<T>` / `Mutable<T>` 作用域权限包装） `store.h` `reactive.h` `signal_view.h` `async.h` `coroutine.h` `state_graph.h` `state_registry.h` |

### 4.2 布局与渲染

| 模块 | 路径 | 主要头文件 |
|:---|:---|:---|
| 布局引擎 | `layout/` | `flex.h` `flex_layouter.h` `layout_box.h` `layout_engine.h`（`Constraints` 在 `core/types.h`） |
| 渲染核心 | `render/` | `painter.h` `font_engine.h` `bitmap_font.h` `png.h` `offscreen.h` `blend.h` `dirty_region.h` `display_list.h` `font_discovery.h` `glyph_atlas.h` `image_cache.h` `snapshot_diff.h` `text_aa_mode.h` `render/rhi/rhi_backend.h` `render/rhi/software_rhi.h` |

> **RHI 抽象（command sink）**：`DisplayList` 回放的目标是 `rhi::RhiBackend`（`render/rhi/rhi_backend.h`：`name()` + 单一 `submit(const DrawCmd&, const CmdData&)`），`rhi::SoftwareRhi`（`render/rhi/software_rhi.h`）把每条命令逐条转发回 `Painter` 对应原语——即抽取前的 `DisplayList::replay(Painter&)` 语义，**像素输出逐位不变**。`DisplayList::replay` 的两个重载（`RhiBackend&` 为唯一实现、`Painter&` 为兼容薄壳）见 `specification/03-layout-render.md` §8.6。接口收成单一 `submit` 而非 18 个平铺虚函数：命令几何/标量已全在 `DrawCmd` 内，把「如何解释命令、如何合并批次」留给后端（GPU 后端正靠此做管线切换与批处理）。

### 4.3 平台抽象

| 模块 | 路径 | 主要头文件 |
|:---|:---|:---|
| 平台抽象 | `window/` | `surface.h`（`Surface` / `HeadlessSurface`） `window.h` `native_surfaces.h` `win32_host.h` `win32_surface.h` `glfw_surface.h` `x11_surface.h` `wayland_surface.h` `wasm_surface.h` `macos_surface.h` `d3d11_surface.h` `frame_pacing.h` `title_bar_geometry.h` `title_bar_style.h` `window_chrome.h` `window_state.h` `platform.h`（`au::platform()` / `Platform` / `PlatformCapabilities`） |

`Surface` 为可扩展边界：自定义 Surface 经 `Application(Scene, unique_ptr<Surface>)` 注入，不随内置 Surface 增长；各后端由 feature 宏 `AURORA_BACKEND_*` 控制代码剪裁（该宏组覆盖内置 `Surface` 图形后端；音频设备后端开关归 `AURORA_ENABLE_AUDIO` / `AURORA_ENABLE_AUDIO_WASAPI` / `AURORA_ENABLE_AUDIO_ALSA` / `AURORA_ENABLE_AUDIO_WEBAUDIO`，见 `BUILD_OPTIONS.md` §1/§3/§4）。

`win32_host.h` 与 `glfw_surface.h` 均 **pimpl 隔离**，公共头零 `<windows.h>` / GLFW / OpenGL 依赖。全部真实后端（`Win32Host` / `GlfwSurface` / `X11Surface` / `WaylandSurface`）的公共头均已收敛为 pimpl 句柄，即便后端开启，消费者编译单元也不会被拉入重型平台头，连带避免 `min` / `max`、`None` / `Bool` / `Status` 等宏污染。

### 4.4 组件层、修饰与控制流

| 模块 | 路径 | 职责 |
|:---|:---|:---|
| 组件层 | `widget/`（75 个头） | `widget.h` `descriptor.h` `props_io.h` `a11y_tree.h` `a11y_diff.h` `containers.h` `text.h` `button.h` `image_widget.h` `checkbox.h` `switch.h` `slider.h` `canvas.h` `progress.h` `divider.h` `rich_text.h` `scroll.h` `stack.h` `grid.h` `spacer.h` `show.h` `repeater.h` `provider.h` `timer.h` `lifecycle.h` `text_span.h` `codegen.h` `inspect.h` `inspector_panel.h` `layout_query.h` `serialization.h` `yaml.h` `recipes.h` 等 |
| 修饰节点 | `modifier/` | `modifier_base.h`（`ModifierNode` 基类）`modifier_layout.h`（Padding / PaddingEdges / FlexWeight / SizeModifier）`modifier_paint.h`（Background / GradientBackground / ShadowNode / BlendNode / ShaderMaskNode / CacheLayerNode / Border / Clip / ClipRounded / OpacityNode / BlurNode）`modifier_transform.h`（AlignNode / OffsetNode / TransformNode）`modifier_input.h`（CursorNode / Clickable / Draggable / LongPress / TouchListener / TooltipNode / ContextMenuNode） |
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
| 应用驱动 | `app/` | `application.h` `scheduler.h` `scene.h` `clipboard.h` `file_dialog.h` `system_tray.h` `display.h` `perf_overlay.h` `menu.h` `shortcuts.h` `validate.h` `validate_ui.h` `generate_ui.h` `hot_reload.h` `window_host.h`（窗口宿主）`window_bus.h`（跨窗事件总线）`window_geometry.h`（几何持久化） |
| 偏好配置 | `preferences/` | `preferences.h`（JSON 文件后端 + 响应式键值存储；不新增 UI 控件） |
| 数据存储 | `storage/` | `storage.h`（`Storage` 门面） `storage_backend.h` `memory_backend.h` `fs_backend.h` `serializable.h` `storage_types.h` |

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
| 命令行解析 | `cli/` | `args.h`（`ValueKind` `Arity` `Value` `Arguments` `Invocation` `parse`） `command.h`（`CommandSpec` `OptionSchema` `PositionalSchema` `validate` `usage_line` `help_text` `schema_json`）；契约见 [`specification/09-cli.md`](specification/09-cli.md) |

### 4.8 数据存储抽象层（Storage）

`Storage` 是比 `preferences` 更通用的持久化抽象（信封 / 类型化 / 异步 / 事务 / 可注入后端），与 UI 控件解耦。**新代码默认优先 `Storage`**，`preferences::Preferences` 保留为面向「响应式键值 + JSON 文件」的轻量特化。

**契约要点：**

- **门面**：`put` / `get` / `remove` / `list` / `contains` / `clear`（value 为 `Json` 或原生 `StorageBytes`）；类型化 `put<T>` / `get<T>` 经 `StorageSerializable` ADL 定制点序列化；信封级 `put_record` / `get_record`；异步 `async_put` / `async_get` / `async_get_value` / `async_remove` / `async_list`（返回 `aurora::Task<T>`，把 IO 卸载出 UI 线程）；`on_change(cb)` 返回 `aurora::Subscription`；`transaction(body)`（默认实现为顺序执行 body、失败不回滚；`MemoryBackend` 覆写为快照回滚，`SqliteBackend`(opt-in) 走真事务 `BEGIN IMMEDIATE`/`COMMIT`/`ROLLBACK` 真实回滚）；进程级 `default_instance()` 单例。
- **后端抽象** `StorageBackend`：纯虚 `put_record` / `get_record` / `remove` / `list`（信封级），另有带默认实现的 `contains` / `clear` / `flush` / `close` 与默认 `transaction`（顺序 apply + **无原子回滚**）。`MemoryBackend` 以 `std::map` 全量快照实现回滚；`FilesystemBackend` 每记录一文件（原子写 `tmp` + `rename`）、目录级锁串行化事务；`SqliteBackend`（opt-in：`AURORA_ENABLE_STORAGE_SQLITE`，sqlite3 amalgamation 源码构建）单文件库或 `:memory:`，真事务 `BEGIN IMMEDIATE`/`COMMIT`/`ROLLBACK`（嵌套计深度加入同一事务）、二进制载荷 BLOB 内联（无 sidecar）、`contains`/`clear` 单语句化，C++ 侧递归互斥 + serialized sqlite 双保险支撑 `async_*` 的 worker 线程触库。
- **信封** `StorageRecord{ id, type, version, encoding, mtime, payload, blob_ref }`：版本号支撑乐观并发与迁移。`StorageChange{ op(Put|Remove|Clear|Batch), id }` 供 `on_change` 投递。
- **错误模型**：统一经 `Result<T>`；后端 IO 失败返回 `Error` 而非抛异常；`get` 未命中返回「未找到」错误（区分于 `null` 值）；事务语义取决于后端：默认实现不回滚（失败仅报告首个原因），仅 `MemoryBackend` 在快照失败时整体回滚并报告首个失败原因。
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
- **全局光栅状态变更按世代失效缓存**：控件 DL（`dl_valid_`）与离屏层（**`Modifier::cache_layer`**，`paint_cache_valid_`）把光栅结果固化在录制/生成时点，而 `mark_needs_paint()` 只沿父链向上失效、不触及后代缓存。故两类缓存的命中条件均含 `render::FontEngine::raster_generation()`——AA 模式 / 默认字体变更会自增该世代，使全树缓存在下一帧按需重录，无需整树遍历。缺少此判据时，切换 AA 模式后后代控件仍回放旧光栅（表现为「切换瞬间无变化、过一会儿才零星生效」）。
- **子节点视图接口 `child_nodes()` 返回 `const std::vector<Node>&`（引用，非副本）**：`Container` 直接返回 `children_` 成员，单子容器与惰性容器以 `mutable` 成员缓存惰性重建。若按值返回 `std::vector<Node>`，临时副本析构会触发 `Node::~Node` 清空子控件的 `layout_parent_`，遍历后子控件 `request_frame` 沿父链上溯断链、脏标记无法到达渲染根。调用方仅限**单帧内只读遍历**（`dump_tree` / `validate` / `hit_test` / inspector），树重建期间引用可能失效。
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
- **指针点击语义**：`Press` 置 `pressed`；`Release` 且 `pressed` 触发一次；点击与长按互斥（`click_pending_` 在 Press 置位，Release 时若未触发长按且未拖拽才触发 click）。
- **焦点**：`FocusManager`（root + focused）按 `tab_index()` 排序移动（`move_focus(FocusDirection)` 循环取前 / 后）；`Widget::request_focus()` 读取派发期线程局部「当前焦点管理器」（`current_focus_manager()`），控件自身不持有 `FocusManager*`。
- **多点触控并发（按指针分发）**：`TouchDispatcher`（实例级，由 `Application` 持有）对 `TouchEvent` 按 pointer id 做命中缓存与独立路由——某 pointer id 首按做命中测试并缓存链，活跃期复用缓存链，抬起即清缓存。因此 `draggable` / `long_press` / `pinch` / `rotate` 各自绑定具体指针，支持单指持发 + 多指并发。每次 `TouchEvent` 同时 (1) 向缓存链广播完整 `TouchEvent`（原始流，供 `TouchListener` 修饰回调）、(2) 合成带 `pointer_id` 的 `MouseEvent` 驱动既有点击 / 拖拽手势。
- **输入法组合只到焦点控件**：`TextCompositionEvent` 与 `TextInputEvent` 同径——不经命中链、不冒泡，仅交给 `FocusManager` 当前焦点控件的 `on_text_composition`（容器误吞即丢字）。组合串由平台桥折算后同步投出，preedit 不进 `value()`（详见 §8.6）。
- **悬停态基础设施**：`EventDispatcher` 在无捕获 Move 时把新命中链与上次悬停链 diff，对离开 / 进入控件回调 `Widget::on_hover_change(bool)`（默认仅记录 `hover_` 不标脏；需要视觉反馈的控件覆写并追加 `mark_needs_paint`）；`Widget::hovered()` 供 `on_paint` 读取。Win32 宿主经 `TrackMouseEvent(TME_LEAVE)` 在光标离窗时合成远离 Move 清除悬停，否则高亮残留。

---

## 8 渲染与布局

### 8.1 渲染核心

- **`Painter`**：纯软件栅格（RGBA8 帧缓冲），不依赖 GPU；接口为纯函数式（给定节点 + 约束 → 确定像素），支持确定性快照比对。
- **圆角抗锯齿裁剪**：`Painter` 用 SDF coverage + 1px 羽化。
- **矢量描边原语**：`draw_line(a, b, width, color)`（点到线段距离 SDF，圆帽 + 1px 羽化）、`fill_rounded_rect(r, radius, color)`、`draw_rounded_border(r, radius, thickness, color)`（圆角矩形 SDF 带状覆盖、向内描边）；三者均接入 Display List 录制回放，回放与直绘逐位一致。
- **GPU 栅格后端（`GpuGlRhi`）**：`DisplayList` 的第二类消费者（与软件 `Painter` 平级，见 §2 命令流与执行分离）。语义与软件路径**同源**——渐变经 1D LUT 纹理复现软件采样；图像预乘 alpha（PMA）上传、`ONE/ONE_MINUS_SRC_ALPHA` 混合规避半透明缩放暗晕；文本复用软件 `GlyphAtlas` 光栅化（经字形发射桥 `emit_text_glyphs`，shaping / 度量 / 缓存单一代码路径）；Shadow/Blur/Blend/Mask 按软件逐像素公式以 SDF / ping-pong pass 等价实现。GL 函数表自写最小 loader（无 GLAD/gl3w），GL 上下文创建与呈现由所在 Surface（GLFW）承担，本后端只做「DisplayList → 批渲染」。**常驻资源双通道**：GPU 层缓存把 `cache_layer` 子树重定向到 FBO 常驻层纹理（epoch + 尺寸键控失效，干净帧仅一条 `DrawLayer` 合成；软件回退经 `SoftwareRhi` 进程级全局层存储语义对齐）；流式纹理常驻槽按 `stream_key` 寻址、`stream_version` 门控增量子上传（视频 / 大图逐帧更新免全量重传）。能力位 `RhiCapabilities` 与原生 GPU 表面导入 `import_native_surface` 为后端无关契约位（GL 恒返回 0，供平台互操作后端兑现）。
- **wgpu GPU 栅格后端（`WgpuRhi` + `WgpuWin32Surface`）**：跨平台 GPU 主力路径——同一套 WGSL 管线经 wgpu-native（Rust 静态库，源码 vendored 于 `third_party/wgpu-native/`，cargo 构建）覆盖 Vulkan / D3D12 / Metal / GLES。命令消费面与 `GpuGlRhi` 实现同一 `RhiBackend` + `RhiFrameSink` 契约、语义同源（SDF 裁剪 / LUT 渐变 / PMA 图像 / 字形图集复用 / 层缓存 + 流式槽）；另有离屏模式（`read_pixels` 全帧读回，测试与探针通道）。compute 实路径：静态大图整条 mip 链由 `cs_mip` compute shader 逐级生成、`DrawImage` 缩小走三线性 mip 采样（`compute` 能力位 Vulkan/D3D12/Metal 端为 true，GLES 端与管线构建失败时如实回落单级 + lod0）。宿主接线：`WgpuWin32Surface` 复用 `Win32Host` 宿主、`WgpuX11Surface` / `WgpuWaylandSurface` 组合内嵌 `X11Surface` / `WaylandSurface` 宿主，做 GPU 直渲上屏（`end_frame` 内 present），失败永久回退软件上传路径（GDI / XPutImage / wl_shm）；`RendererPreference::GpuWgpu` 强制路由不降级（Linux `create_native_window` 运行期按 `WAYLAND_DISPLAY` 会话择宿主）。规格见 `specification/03-layout-render.md` §8.8。

### 8.2 高 DPI

所有 widget / 布局坐标均为**逻辑 dp**（96 DPI 基准 = 1 dp ≈ 1 px@96）。`Surface::scale_factor()` 返回 `dpi/96`；`Painter::set_scale(scale)` 把 dp 几何 ×scale 放大到物理像素，1:1 贴窗避免发虚。事件坐标在 `Surface` 内由物理像素 `/scale` 还原为 dp。

**调用顺序不变量**：`SetProcessDpiAwarenessContext` 在「进程已有任何窗口」时失败。因此 `aurora::enable_dpi_awareness()` 必须在 `init_console()`（`AllocConsole` 会创建控制台窗口）与 `create_window()` **之前**调用。否则进程退化为 DPI 未感知，窗口以系统 DPI 虚拟化缩放（scale = 1.0），高分屏下界面发虚。

字体测量（`measure_width` / `caret_x` / `hit_test_char`）在逻辑 dp 空间进行，而光栅化（`FontEngine::draw_text`）按真实屏幕 DPI 生成物理分辨率字形——二者解耦，高 DPI 下文字清晰。

### 8.3 Scroll 离屏内容缓冲（滑窗）

`Scroll` 把内容录进与滚动偏移无关的**内容坐标滑窗缓冲** `content_`（`unique_ptr<Painter>`），尺寸 = 视口高 × (1 + 2 × `overscan`)，仅覆盖可见区上下各 `overscan` 视口高的带而非整页；`buffer_origin_y_` 标记该带在内容坐标系中的锚点。`ScrollProps::overscan`（默认 `1.0F`，共 3 屏厚）控制缓冲带厚度。

- 滚动帧满足「内容仍有效且为纯滚动且未触发重锚点」时，直接 `p.composite(*content_, translate(0, buffer_origin_y_ - offset_y_))` 一次 blit，不重新栅格。
- **程序化跳转**（`set_offset` / `restore_key` 恢复）**不**置 `content_valid_ = false`：偏移不参与内容录制（内容以稳定内容坐标录进缓冲），跳越缓冲窗口时由 `on_paint` 的 `!in_buffer → reanchor` 分支整块/条带重录兜底，故无需在此强制整块重录。
- 视口逼近缓冲带两端时把 `buffer_origin_y_` 重锚并**重录当前缓冲带**。
- 非滚动帧（子动画 / 内容变化）按脏区**重录当前缓冲带**。

重锚点几何与 composite 几何均用**逻辑 dp**，避免单位 bug 导致缓冲错位。该设计使缓冲字节数 = 视口像素 × 4 × (1 + 2 × `overscan`)，**与内容总量无关**。

### 8.4 后端家族

| 后端 | 上屏方式 | 开关 |
|:---|:---|:---|
| `HeadlessSurface` | 内存帧缓冲，可同步导出 PNG | `AURORA_BACKEND_HEADLESS`（默认 ON） |
| `Win32Surface` | 常驻 BGRA DIB section + `BitBlt`（RGBA→BGRA CPU swizzle），支持 `set_present_dirty` 增量上屏 | `AURORA_BACKEND_WIN32`（Windows 默认 ON） |
| `D3D11Surface` | 复用 `Win32Host` 宿主，把 `Painter` RGBA8 帧缓冲作为动态纹理，脏矩形经 `UpdateSubresource` 增量上传，全屏三角形 + 像素着色器线性采样呈现（`Present(1,0)`） | `AURORA_BACKEND_D3D11`（默认 OFF） |
| `WgpuWin32Surface` | 复用 `Win32Host` 宿主，帧级 DisplayList 交 `WgpuRhi` 在 GPU 端光栅化并 `wgpuSurfacePresent` 直渲上屏；GPU 失败永久回退 GDI `SetDIBitsToDevice` 上传路径 | `AURORA_BACKEND_GPU_WGPU` ∧ `AURORA_BACKEND_WIN32`（默认 OFF） |
| `GlfwSurface` | OpenGL 3.3 兼容剖面（绘制采用 1.1 立即模式），pimpl 隔离；开 `AURORA_ENABLE_GLFW_GPU_GL` 后可请求 GPU 渲染模式（`GlfwOptions::gpu`），上下文与 swapBuffers 仍由本后端承担 | `AURORA_BACKEND_GLFW` |
| `GpuGlRhi`（`Surface::gpu_backend()`） | DisplayList 的 OpenGL 3.3 core 批渲染：整帧回放 → Solid/Border/Grad/Image/Text/Shadow 管线合批（纹理 / 状态变化断批）；MSAA 渲染缓冲 + resolve 呈现；区域效果（Blur/Blend/Mask）经 resolve 纹理 ping-pong 回写；GPU 层缓存（FBO 常驻层纹理，epoch 键控失效）与流式纹理常驻槽（版本门控增量子上传）；初始化失败运行期回退软件路径 | `AURORA_ENABLE_GLFW_GPU_GL`（依赖 GLFW） |
| `WgpuRhi`（`Surface::gpu_backend()`） | DisplayList 的 wgpu（WGSL）批渲染：与 `GpuGlRhi` 同一命令契约与批切分口径；宿主模式（Win32 HWND / X11 Display+XID / Wayland display+wl_surface → swapchain）与离屏模式（内部纹理 + `read_pixels` 读回）双形态；后端 Auto 择链（Windows：D3D12 → Vulkan → GLES） | `AURORA_BACKEND_GPU_WGPU`（默认 OFF） |
| `X11Surface` | 按 Visual 掩码 CPU swizzle 后 `XPutImage`，支持增量上屏；`wait_events` 经 `poll(2)`；`scale_factor` 解析 `Xft.dpi` | `AURORA_BACKEND_X11`（默认 OFF） |
| `WgpuX11Surface` | 组合内嵌 `X11Surface` 宿主（窗口/事件/光标全走它，本类零 Xlib），帧级 DisplayList 交 `WgpuRhi` 在 GPU 端光栅化并 swapchain present 直渲上屏；GPU 失败永久回退内嵌宿主的 `XPutImage` 上传路径 | `AURORA_BACKEND_GPU_WGPU` ∧ `AURORA_BACKEND_X11`（默认 OFF） |
| `WaylandSurface` | CPU swizzle 到 `WL_SHM_FORMAT_XRGB8888` 经 `wl_shm` 共享内存双缓冲槽；`wait_events` 经 `poll(2)`；`scale_factor` 取 `wl_output.scale`；光标走 `libwayland-cursor` 客户端主题（cursor 专用 `wl_surface` + `wl_pointer.set_cursor`，见 `specification/03` §8.3） | `AURORA_BACKEND_WAYLAND`（默认 OFF） |
| `WgpuWaylandSurface` | 组合内嵌 `WaylandSurface` 宿主（窗口壳/事件/xkb/CSD 全走它，本类零 wayland-client），帧级 DisplayList 交 `WgpuRhi` 在 GPU 端光栅化并经 Wayland surface swapchain present 直渲同一 `wl_surface`；GPU 失败永久回退内嵌宿主的 `wl_shm` 上传路径；GPU 模式下自绘 CSD 不上屏（申报口径，见 `specification/03` §8.8） | `AURORA_BACKEND_GPU_WGPU` ∧ `AURORA_BACKEND_WAYLAND`（默认 OFF） |
| `WasmSurface` | `<canvas>` 像素写回（`EM_ASM` `putImageData`） | `AURORA_BACKEND_WASM`（默认 OFF） |
| `MacOSSurface` | AppKit / CoreGraphics | `AURORA_BACKEND_MACOS`（默认 OFF） |

`Win32Surface`、`D3D11Surface` 与 `WgpuWin32Surface` 共用 `Win32Host` 宿主（创建 / 消息泵 / 事件翻译 / DPI / 同步重渲染 / 运行期标题），仅 present 后端不同。宿主采用 pimpl 隔离，故 `hwnd()` / `background_brush()` 以 `void*` 返回（调用方 `static_cast`）。窗口过程按**消息族**分派到各 `handle_*` 函数，取代单体 switch。X11/Wayland 侧的 `WgpuX11Surface` / `WgpuWaylandSurface` 不走共享宿主路线而是**组合**内嵌 `X11Surface` / `WaylandSurface`（窗口创建 / 事件泵 / 光标全走它，本类只做 GPU 帧路径与软件回退分流，句柄以 void* 同源取得），避免把 Xlib / wayland-client 逻辑复制第二份。

**多窗口契约（`Surface` 上的可选覆写）**：多窗口帧循环需要后端自述两类能力——`pumps_thread_queue()`（事件泵是否抽干线程/进程级共享队列：Win32/GLFW/D3D11/WGPU = true，X11/Wayland/Wasm = false）与 `waits_thread_queue()`（等待通道是否覆盖全部窗口）。此外提供父子/模态（`set_owner` / `set_enabled`）、z 序与显示（`raise` / `focus_window` / `display_id` / `position` / `set_position` / `set_size`）与 DPI 变化上报（`set_scale_change_handler`）。**全部有默认空实现**，自定义后端不覆写即退化为单窗口语义，源码兼容。详见 `specification/06-app-platform.md` §2.4。

**自定义后端**：任意 `Surface` 子类经 `Application(Scene, unique_ptr<Surface>)` / `App().surface(...)` 注入，无需为每种后端在 `Application` 上加构造重载；`Surface` 之外的扩展点收口在 `create_window` 工厂。

**编译 / 链接期代码剪裁**：关闭某 `AURORA_BACKEND_*` 后，对应 `Surface` 实现类、工厂重载与重型平台头被预处理器剔除，链接产物不再含该后端；关闭 `AURORA_ENABLE_AUDIO_WASAPI` / `AURORA_ENABLE_AUDIO_ALSA` / `AURORA_ENABLE_AUDIO_WEBAUDIO` 同样剔除对应音频设备后端实现。自定义注入路径（自定义 `Surface` / 自定义 `AudioDeviceBackend`）不受影响，故「只用自定义 backend」可不编译任何内置后端。

### 8.5 无障碍桥接（platform accessibility bridge）

**分层与所有权。** 无障碍能力分三层，公共头零平台污染：语义树（纯值类型与节点 / 事件 / 设置留在 `core/a11y_types.h` 与 `core/accessibility.h`——`core/` 侧只以**指针**持 `Widget`；需要遍历控件树的构建器与快照落在 `widget/a11y_tree.h` 与 `widget/a11y_diff.h`，故 `core/` 不反向依赖任何模块，见 §2）→ 桥抽象（`core/a11y_provider.h` 的 `a11y::Provider` 接口 + 进程级 `ProviderRegistry`）→ 平台实现（Win32 = `src/aurora/window/detail/win32_ua.{h,cpp}`，随 `AURORA_BACKEND_WIN32` 编入；D3D11 复用同一桥，门控为「平台宏 ∧ 后端宏析取」，与 `win32_cursor.h` 同款；Linux = AT-SPI2 桥，拆「中立折算 `detail/atspi_protocol.{h,cpp}` + libdbus 传输 `detail/atspi_bridge.{h,cpp}`」两文件，门控为「Linux 平台 ∧（X11 ∨ Wayland）后端析取」，`dlopen("libdbus-1.so.3")` 运行时加载，折算层零 D-Bus 依赖故无头单测可全证其语义）。桥实例由 `Win32Host::Impl` **唯一持有**（`Win32Surface` / `D3D11Surface` / `WgpuWin32Surface` 都转发同一实例），避免两份 `id → Widget*` 映射分裂。

**Surface 扩展点（两处，均有默认空实现）。**

| 扩展点 | 作用 |
|:---|:---|
| `Surface::accessibility_provider() const -> a11y::Provider*` | 返回本窗口的桥（无桥后端返回 `nullptr`）。**只读**：不构造桥。 |
| `Surface::set_accessibility_root(Widget*) -> void` | 每帧由 `Window::present_root` 在布局与绘制完成后调用，注入语义树根。 |

为什么不只靠 `accessibility_provider()`：桥是**惰性**构造的（首个平台查询到达才构造，见下）——首个查询到达时它才存在，此刻若还没有任何注入记录就无根可投影。故根由**恒存在**的宿主（`Win32Host::Impl::a11y_root`）承接，桥构造后由宿主补喂；无桥后端为 no-op。注入点在 `paint` 之后：语义树几何取自布局与绘制产物，先于此即无效。

**重建模型（拉取式）。** 事件到达只置 dirty（`Provider::mark_dirty()`），平台查询（UIA `Navigate` / 属性拉取）到达时才 `sync_if_dirty()` → 全量重投影 + 快照 diff + 派发平台事件。**不进帧循环**：无读屏在线时零构建、零事件。规模假设是「桌面应用百级节点」，全量 O(n) 足够，避免高频变更下的重建风暴。

**根的生命周期（两处硬不变量，均为实机崩溃的修复结论）。**
1. 桥只持**根**的裸指针（子节点每次查询重投影，不缓存）。根控件销毁时经 `Node::~Node()` 的单源通道 `notify_accessibility_widget_destroying()` 广播到已激活桥，桥立即切断根 / 快照 / 平台对象并停发事件——否则宿主「先拆 UI 树、后拆窗口」的常规顺序下，窗口存活期间的平台查询会拿悬垂根重建语义树。
2. 窗口销毁时 `disconnect_all()` 会先立**单向拆除门闩**再调 `UiaDisconnectProvider`：该 API 会**同步重入** provider 取属性（UIA 需为被丢弃的侦听者补发属性变更事件），门闩保证重入路径只读旧快照、绝不重建。

`deactivate()` 与 `disconnect_all()` 等价且幂等，并在其中**从 `ProviderRegistry` 注销** —— 缺这一步，进程级事件广播会在已析构的桥上调用 `is_active()`（use-after-free）。
**线程与降级。** 全 main-thread（in-proc provider 由 UIA core 在 UI 线程回调，桥激活时把套间初始化为 STA）；`UIAutomationCore.dll` 运行时 `LoadLibraryA` 动态加载，缺库或函数缺失即整桥降级 no-op + 一次 `Diagnostics::warn`，无链接期依赖。

**Linux 桥差异（AT-SPI2）。** 无 `WM_GETOBJECT` 式「读屏在线才出现」的查询信号，故构造时机改为**首次语义树根注入时一次性尝试**（失败 = 永久降级，不再重试）；建树同步点由「入站查询」与「帧循环 dirty」双侧驱动——`pump()` 每轮开头 `sync_point()`（若脏），事件推送不依赖客户端恰好在做查询。D-Bus 传输 fd 经 `Provider` 侧 `poll_watches()` 并入 X11/Wayland 事件等待的 `poll`，fd 就绪由 `pump()` 读入并派发（单线程、无额外线程）。事件通道 = 快照 diff 的另一种消费：`sync_point()` 产 `TreeDiff` 后按上游 `atk-adaptor` 线格式发 D-Bus 信号广播（added/removed → Cache Add/RemoveAccessible + children-changed；Name/Value/Hint/State → property-/state-changed；焦点 → Event.Focus；播报 → Announcement 直发），与 Win32 UIA 桥 `queue_*` 系列同一「diff → 平台事件」消费范式。降级面 = 无会话总线 / `org.a11y.Bus` 不可达 / libdbus 缺失 / `NO_AT_BRIDGE=1`；线格式契约与申报空位见 [`specification/06-app-platform.md`](specification/06-app-platform.md) §6.4。

**Wasm 桥差异（ARIA 镜像）。** 第三个平台实现（`include/aurora/window/wasm_aria.h` + `src/aurora/window/wasm_aria.cpp`，门控 `AURORA_PLATFORM_WASM ∧ AURORA_BACKEND_WASM`，折算层同样是零平台头的 `detail/aria_protocol.{h,cpp}` + 无头单测 `utest_aria_protocol`），消费端不是 IPC 协议而是**浏览器原生 ARIA 支持**：快照折算成页面隐藏 DOM 镜像（clip 隐藏保进树、`role`/`aria-*`/`aria-activedescendant`/`aria-live`），读屏经无障碍引擎直接消费，故无 `sync_point` 应答面、也**无几何面**（申报）。激活为 D14 惰性激活的既定例外（浏览器无读屏探测信号，首根注入即激活）；同步与反向动作排水由桥**自持 rAF 蹦床**每拍执行（`live_bridges()` 成员性做悬垂守卫），不挂 `present()` 帧尾——静止页面没有脏帧就没有 present，读屏动作会被饿死。反向动作走 JS 写队列 + 帧尾轮询（刻意避开 `-sEXPORTED_FUNCTIONS`，保消费者零链接配置）。契约、载荷协议与 CDP 真机验收见 [`specification/06-app-platform.md`](specification/06-app-platform.md) §6.5。

### 8.6 输入法桥接（platform IME bridge）

**分层与所有权。** 与无障碍桥同构，但方向相反（平台 → 树，而非树 → 平台）：组合事件值类型（`event/event.h` 的 `TextCompositionEvent`，平台中立）→ 折算层（`src/aurora/window/detail/ime_composition.{h,cpp}`，**零平台头**：UTF-16/UTF-8 字节下标 → 码点、组合属性 → 待转换选区）→ 平台桥（Win32 IMM32 = `src/aurora/window/detail/win32_ime.{h,cpp}` 独立桥类；X11 XIM 与 Wayland text-input-v3 = 分别内联在 `x11_surface.cpp` / `wayland_surface.cpp` 的 `Impl` 内——各自只有五六个协议入口点，拆文件只会把一条调用链劈成两半）。折算层刻意不含平台头，故「无头 CI 证明不了的只有与 IMM32 / XIM / v3 的三条平台接缝」——接缝由真机验收探针 `tools/verify/win32_ime_live_probe.cpp` / `x11_ime_live_probe.cpp` / `wayland_ime_live_probe.cpp` 覆盖，其余全部落在 `utest_ime_composition` 的可执行覆盖里。

桥与窗口一一对应：Win32 由 `Win32Host::Impl` **唯一持有并随窗口构造**（`Win32Surface` / `D3D11Surface` / `WgpuWin32Surface` 转发同一实例），X11/Wayland 随各自 `Impl` 构造。与 a11y 桥的**惰性激活相反**：桥本身没有查询代价，且无输入法环境下一条平台事件也不会投递，早构造零成本，故无需门闩与注册表（Wayland 另有构建期软门 `AURORA_HAVE_WL_TEXT_INPUT`，缺协议 XML 即整桥裁切）。

**Surface 扩展点（一处，默认空实现）**：`Surface::set_composition_caret_provider(std::function<Rect()>)`。方向与 a11y 的 `set_accessibility_root`（宿主 push）相反——插入点是**拉取**的：候选窗定位只在组合期才需要，而「当前焦点是哪个控件」只有上层 `FocusManager` 知道，故由宿主提供查询回调（`WindowHost` 返回焦点控件的 `Widget::composition_caret_bounds()`，窗口逻辑 dp；桥内 `× scale` 后映射到平台坐标——Win32 `ClientToScreen` / X11 `XNSpotLocation` / Wayland `set_cursor_rectangle`），三平台共用同一判据形状：provider 报零盒 = 不接管（Win32 不移候选窗、Wayland 保持 disable）。

**两条不变量（均为实机行为结论，非推测）**：

1. **上屏只有一条通道**。Win32 侧 `GCS_RESULTSTR` 是唯一的 commit 来源，故 `WM_IME_CHAR` 必须吞掉、组合期间的 `WM_CHAR` 必须丢弃——放行任一条即与 `DefWindowProc` 的逐字 `WM_CHAR` 转换叠加，同一汉字上屏两次（DBCS ACP 下还会被拆成前导/尾随字节产出乱码）。X11/Wayland 同理：组合期上屏只认平台 commit 通道（cb 风格下 `Xutf8LookupString` 的提交串 / v3 `commit_string`）并折算进 `committed`，普通键文本路径仅在无组合时落字。
2. **preedit 永不进数据模型**。组合串只在绘制与测量期参与（`composed_text()`），`value()` / 序列化 / golden 因此与「用户是否正打到一半」无关，保持确定性。

**为何是 IMM32 而非 TSF**：TSF 要求实现 `ITextStoreACPServices` 全套文本存储代理（约 1.5k 行 COM，且与「控件自持状态、无 Windows 文本对象」的模型正交）；Win10/11 的 CTF 加载器对非 TSF-store 窗口提供 IMM32 兼容**读**通道，组合串 / 上屏串 / 光标 / 属性全部可读、候选窗定位亦生效。代价是该通道不接受外部**写**（`ImmSetCompositionStringW` 被 TSF 型输入法拒绝），因此组合文本无法自动化注入，preedit 渲染须人工目视收口。桥的对外形状（`Hooks` + `handle()`）与 TSF 无冲突，后续替换不动上层。

契约与接线状态表见 `specification/05-event-navigation.md` §2.4，各平台侧细节（IMM32 取舍 / XIM 风格协商与取字溢出 / v3 enable 判据与去重）见 `specification/06-app-platform.md` §8.5。

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

`include/aurora/app/perf_overlay.h`，128 帧环形缓冲区 O(1) 采集。

**归属（多窗口）**：默认实例为进程级单例 `FrameStats::instance()`；单窗口用法（含既有基准与性能集成测试）始终写入它，行为不变。登记**第二个**窗口起，各 `WindowHost` 改绑自有实例（`Window::set_frame_stats`），避免多窗数据互相污染；`PerfOverlay` 经 `Application::set_overlay` 自动绑定主窗口统计，其他窗口请显式 `bind_frame_stats(&host->frame_stats())`。

**指标**：FPS（滑动窗口平均）、平均帧时间、P50 / P95 / P99 百分位帧时间、帧时间标准差（jitter）、掉帧计数与掉帧率、hitch 计数（帧耗时超过帧预算 2 倍）、idle 帧计数。

**停帧陈旧语义**：`fps()` 是滑动窗口内已记录帧的均值、**不做时间衰减**——渲染一停环形缓冲不再有新样本，返回值便冻结在最后一次活跃 burst 上（HUD 显示「421.0 FPS」而实际已停帧数秒，是误导而非信息）。`WindowHost` 在 idle 帧上以本帧墙钟间隔调用 `record_idle(dt)` 累计空闲时长（任何一次 `record(dt)` 都将其清零），累计达 `AURORA_FPS_STALE_MS`（500ms）后 `is_stale()` 为真，`stale_duration_ms()` 给出空闲时长。**取值保持末值、不归零**：归零会丢掉「上次活跃帧率」这一排障信息。判据基于空闲**时长**而非帧数——空闲段里帧循环仍可能被事件 / 定时器 / HUD 刷新唤醒，用帧数衡量会把「一帧都没有」误判成「刚过了几帧」。

**分阶段计时**：`record_phases(layout_ms, paint_ms, present_ms)` 独立记录三阶段耗时，64 帧环形缓冲，提供 `avg_layout_ms` / `avg_paint_ms` / `avg_present_ms`。

**帧预算**：`set_frame_budget_ms(ms)`（默认 16.67ms ≈ 60 FPS），超出即计为掉帧。`reset()` 清空状态用于基准隔离。

### 10.2 PerfOverlay

右上角叠加面板，实时显示多行统计文本（FPS / avg / P99 / jitter / 掉帧数 / hitch 数 / idle 帧数）、FPS 颜色告警（绿 ≥ 55、黄 ≥ 30、红 < 30）与帧时间条形图（最多绘制最近 64 根柱，超预算帧标红；底层环形缓冲为 128 帧）。读数陈旧时（`FrameStats::is_stale()`）第一行由绿/黄/红**转为灰色**并在末尾追加 `stale <空闲秒数>`，避免把冻结值当成本刻帧率；统计行一律显式传入本面板绑定的统计实例（无参重载读进程级单例，多窗口下不一致）。经 `PerfOverlay::set_visible(false)` 关闭显示。

**分层 HUD 叠加层（推荐用法）**：`PerfOverlay` 既可作普通 `SingleChild` 包裹内容，也推荐作为**独立 HUD 叠加层**使用——经 `Application::set_overlay(...)` / `Window::set_overlay(...)` / `App::overlay(...)` 注入后，它**脱离 widget 树**，由 `Window::present_root` 在 tree paint 之后、present 之前合成到主缓冲：

- 叠加层渲染到独立离屏 `Painter` 缓冲，仅按 `Window::AURORA_HUD_REFRESH_MS`（500ms）重绘自身（面板背景不透明，确保叠在保留自上一帧的主缓冲之上不产生重影）；
- 每帧把缓存的 HUD 缓冲 `composite` 到主缓冲；app 树仅在**其自身脏**时重绘，叠加层刷新开销被隔离在离屏缓冲内，不再触发整树重绘；
- 叠加层内容发生重绘的帧强制全量上屏（HUD 像素可能落在 app 脏区之外，避免滞后 1 帧）；
- **空闲期不会深睡**：整树无脏但叠加层到期时，脏决策仍放行一帧——软件路径下只把新 HUD 合成到保留的主缓冲再全量上屏（不重排、不重绘树），且该帧仍记 idle（避免 HUD 用自己的刷新抬高它自己显示的帧率）；GPU 路径因软件缓冲只有底色而标全脏、退回完整重渲染。同时 `WindowHost::decide_wait` 把 `hud_refresh_due_ms()` 并入「非渲染唤醒」截止时间，否则空闲分支会睡到下一个定时器（无定时器即无限等待）。缺少这两处，脏决策会在 HUD 合成段之前直接 `return`，读数永久停在最后活跃帧上；
- 空闲期唤醒频率由此固定为约 2Hz（叠加层可见时）；卸下叠加层后本窗口恢复正常的事件驱动深睡；
- 与「把 `PerfOverlay` 作为根控件包裹内容」的旧用法**互斥**：启用叠加层后，`Scene` 根即为真实内容树，`PerfOverlay` 不应再出现在树内。

### 10.3 PerfLog

`include/aurora/perf/perf_log.h`：定期日志输出 + 快照导出。`enable(interval_frames)`（默认每 300 帧）、`on_frame_end()`、`snapshot_json()` / `snapshot_csv()`。日志经 `Log` 子系统输出，可重定向。

### 10.4 Idle 帧区分

`Window::is_idle_frame()` 判定当前帧是否为脏区跳帧；`FrameStats::record_idle(dt)` 单独计数并累计空闲时长。无脏且尺寸未变时整帧跳过——这类 idle 帧不应污染 FPS / 帧时间 / 掉帧等渲染指标。

脏决策有三种落点，**后两种都记 idle（未渲染树）**：

| 落点 | 触发 | 是否上屏 | `is_idle_frame()` |
|:---|:---|:---|:---|
| 整帧跳过 | 无脏、尺寸未变、叠加层未到期、无系统重绘请求 | 否 | true |
| HUD-only 帧 | 无脏、尺寸未变，但叠加层到期且软件缓冲可复用 | 是（仅合成新 HUD） | true |
| 完整渲染 | 有脏 / 布局脏 / 尺寸变化 / 根变化 / 系统重绘 / GPU 路径下叠加层到期 | 是 | false |

空闲期两种场景（无叠加层的事件驱动深睡、叠加层可见的每 500 ms 一帧）的实测基线与复现步骤见 [`specification/08-tooling.md`](specification/08-tooling.md) §7.4.1（口径、基线与复现）与人工用例 [`manual-test/17-perf.md`](manual-test/17-perf.md)（TC-PERF-006 覆盖无叠加层、TC-PERF-008 覆盖叠加层可见）。

### 10.5 硬约束

- **快速路径逐位一致**：所有快速路径必须与慢路径 golden 零差异，修改后须跑 `utest_offscreen` 全量回归。
- **SIMD 双实现确定性**：SIMD 路径必须与标量黄金路径逐位一致（`-ffp-contract=off`、同浮点运算序列、整型 `cvtt` 截断）；CI 由 `utest_simd_parity` 逐位比对，一票否决。

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

错误处理遵循「**机器可解析的错误**」约束。错误码目录（数据）由 `tools/gen/gen_error_codes.cpp` 自动生成并维护于 [`ERROR_CATALOG.md`](ERROR_CATALOG.md)，**请勿手改**。编码层面的写法规则见 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §1。

### 12.1 设计目标

每个错误同时服务于两类受众：

- **进程外**（JSON / 日志 / IDE 工具 / AI 编码助手）：只认冻结 `slug`（如 `"nav-depth-exceeded"`，改名标识符也不变）与 `code_enum`。
- **进程内**：用 `code_enum` 做类型安全分支，用 `severity` / `category` / `auto_fixable` / `retryable` 等元数据做策略判断。

失败路径统一：可恢复失败经 `Result<T>` 沿调用链返回，不靠异常横跨业务边界。

### 12.2 类型模型

`Error`（`core/result.h`）字段分两类受众：`code` / `code_enum` 供机器解析；`severity` / `category` / `auto_fixable` / `retryable` / `fix_category` / `fix_params` 供进程内策略判断；另有 `message` / `suggestion` / `docs` / `where` / `hint` 供人与 AI 阅读。所有表驱动元数据由 [`errors.toml`](errors.toml) 经生成器产出，经 `make_error` 自动填充，无需手填。

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
        │  tools/gen/gen_error_codes.cpp
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
- **不可恢复错误**（断言边界）：使用 `assert` / 前置条件检查，见 `include/aurora/core/aurora_assert.h` 与 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §1。

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
11. **事件模型**：统一事件类型（`MouseEvent` / `KeyEvent` / `ScrollEvent` / `TextInputEvent` / `TextCompositionEvent` / `TouchEvent` / `FileDropEvent`）+ 冒泡协议；事件坐标在 `e.position` 而非 `e.x` / `e.y`。
12. **状态作用域**：状态作用域显式——局部 `State` 由组件持有，全局 `Store` 由应用持有并通过 `Environment` 注入；`Binding<T>` 为非拥有引用，上游生命周期须更长。

---

## 14 测试与 CI 架构

### 14.1 目标

- **可回归**：每次变更可经 `ctest` 全量复跑，失败即阻断。
- **渲染零差异（golden）**：光栅输出像素级稳定，快速路径 / SIMD 路径必须与标量黄金路径逐位一致。
- **跨平台一致**：同一测试矩阵覆盖 Linux / Windows / macOS。

### 14.2 分层

**单元测试（`tests/unit/utest_*.cpp`）**：每个公共源文件对应一个 `utest_*.cpp`（与 `examples/demos/demo_*.cpp` 同构：1 源文件 ↔ 1 测试 ↔ 1 demo）；跨控件 / 端到端集成用例放 `tests/integration/itest_*.cpp`。经 `cmake/AuroraTests.cmake` 收集（`file(GLOB CONFIGURE_DEPENDS)`），**全部用例链入单一可执行 `aurora_test_runner`**：用例用 `AURORA_TEST_CASE(<Case>)` 宏静态自注册（全名 = `<文件 stem>.<Case>`），`main()` 由 `tests/framework/test_main.cpp` 唯一提供。CTest 逐条以 `aurora_test_runner --run=<stem>` 注册（进程隔离），并由 `registry_integrity` 按用例级清单守护漏注册。

**Golden 测试（渲染像素级）**：以 `utest_offscreen` 为主，把 widget 树渲染到 `HeadlessSurface` 内存缓冲，与 golden 基准图逐像素比对。依赖相对路径，须从**仓库根**运行（`ctest` 已为其把 CWD 设为仓库根），可用 `AURORA_GOLDEN_DIR` 覆盖解析基准。逐位红线唯一属软件路径；GPU 侧为**容差 golden 双层**——`itest_wgpu_golden`（wgpu）与 `itest_gl_golden`（GL，需 GLFW + GPU_GL 配置）把同一场景 DisplayList 经离屏后端重放读回，与同一张软件基线按**场景级申报**的容差带比对（场景与帧装配单一来源 `tests/support/gpu_golden_scenes.h`；`golden::compare_gpu_tolerance`，不读全局放松旋钮、不回写基线），见 `specification/03-layout-render.md` §8.4.2 / §8.8。

**性能基准**：见 §10 与 [`specification/06-app-platform.md`](specification/06-app-platform.md) §10。

### 14.3 组织约定

- **命名**：测试文件以 `utest`（单元，`tests/unit/`）/ `itest`（集成，`tests/integration/`）为**前缀**（非 `_test` 后缀），与源文件同名主体；每个测试 TU 包裹在 `namespace aurora::test_cases::utest_<名>`（集成用例为 `itest_<名>`）内。
- **运行**：`ctest -R <名>` 逐条拉起 `aurora_test_runner --run=<stem>`；从仓库根运行以保证相对路径解析；本地全量复跑推荐 `ctest --preset ninja-test`（`CMakePresets.json` testPresets，等价固定 `ctest -j 16`）。并行模型为「CTest 进程隔离 + 框架用例边界资源虚拟化」（tmpdir / cwd / 单例 / 剪贴板注入，见 `tests/framework/isolation.h`），不使用 `RUN_SERIAL` 串行白名单。
- **耗时观测**：`tools/check/build_baseline.py`（手动跑、非门禁）解析构建目录的 `.ninja_log` 与 ctest `LastTest.log`，输出编译边耗时分布 / top-N 慢边与测试串行耗时合计 / top-N 慢测（并行关键路径），`--json` 落基线供跨次对照。
- **新增约束**：新增公共 API / widget / 核心逻辑须配套单测并接入 CTest。

### 14.4 CI 执行层

CI 配置位于 `.github/workflows/`：

| 工作流 | 作用 |
|:---|:---|
| `ci.yml` | 宏矩阵全量覆盖，每推送 / PR 触发，`concurrency` 取消旧运行以提速。共 10 组 job：**core**（linux/gcc + linux/clang + windows/msvc + windows/mingw + windows/llvm（clang-cl）+ macos/clang，Release 与 Debug 各编一次覆盖 `AUTO` 三态的两个分支，Debug 覆盖 linux/gcc、windows/msvc 与 windows/llvm；linux-gcc-release 与 windows-llvm 额外构建 `demos` 聚合目标）；**backends**（X11/Wayland/GLFW、D3D11/GLFW、macOS/GLFW 各编译一次）；**toggles**（优化三开关全关 / SIMD 关 / 图像编解码+Inspector 开 / PROFILING+TRACING+DEBUG 强制开 / DEBUG 强制关，均 ubuntu/gcc）；**asan**（ASan+UBSan 全量 ctest）；**coverage**（`coverage` 聚合目标：ctest + gcov 摘要，CSV/HTML 入 artifact）；**wasm**（emcmake + `AURORA_BACKEND_WASM=ON`，构建库与测试 runner 并全量 ctest，能力缺失用例走 SKIP 路径）；**install-consumer**（`cmake --install` + `find_package(Aurora)` 最小消费端冒烟，GUIDELINE §1 配方；默认配置与 Release 下强制 `AURORA_ENABLE_DEBUG=ON` 的「非默认宏一致性」各一组）；**lint**（clang-tidy 门禁，矩阵 = DEBUG OFF/ON 两份编译数据库 × TU 分片 4 片 = 8 个作业，去重后 0 finding 才过）；**lint-wasm**（同一门禁的浏览器口径，`--emscripten` 重写后的 wasm 编译库分 4 片，只 configure 不构建）；**fmt**（`format-check` 全量排版校验）。三道 lint 与排版均为**必过**位，各自清单逐片入 artifact；静态检查为什么按 TU 分片、分片改了判据没有，见 [`BUILD_OPTIONS.md`](BUILD_OPTIONS.md) §4.5 |
| `release.yml` | 发布流程（构建产物 / 版本标签） |

矩阵按「每个 feature 宏分支至少被一个 job 编译一次」设计；选项语义见 [`BUILD_OPTIONS.md`](BUILD_OPTIONS.md)。CI 只负责「拉起构建 + 跑 CTest」，不承载测试设计。

**编译缓存与执行口径**：

- 全部 Test 步骤并行执行（`ctest -j "$(nproc)"`；coverage 聚合目标内的 ctest 除外——gcda 并发写在非加锁平台有损坏风险）。
- 编译缓存接入 `hendrikmuhs/ccache-action`（key 按 job / 矩阵名分桶）：core（Linux / macOS / windows-mingw）、backends（Linux / macOS）、toggles、asan、coverage、install-consumer；缓存口径（SLOPPINESS / BASEDIR / NOHASHDIR / 压缩）由 `cmake/AuroraCcache.cmake` 的编译器启动器统一注入，与本地构建共享同一语义，避免「本地命中、CI 全 miss」。
- windows-msvc（默认 Visual Studio 多配置生成器）不接入编译缓存：CMake 的编译器启动器（`<LANG>_COMPILER_LAUNCHER`，sccache / ccache 的挂接点）仅在 Makefile / Ninja 生成器实现，VS 生成器下被静默忽略；要接入须先将该矩阵切换到 Ninja + cl，暂无必要（该配置无缓存路径、PCH 净收益显著，冷构建本身不构成瓶颈）。
- windows 侧 Test 步骤显式 `shell: bash` 并设 `PYTHONUTF8=1`（默认 pwsh 无 `nproc`；cp1252 控制台无法编码 CJK 诊断输出）。
