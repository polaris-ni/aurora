# 应用、窗口与平台（app / window / platform / preferences / storage / perf）

> 覆盖 `include/aurora/app/`（14 个头）、`window/`（17 个头）、`preferences/`、`storage/`、`perf/`、`debug/` 与后端 `Surface`（后端清单见 [`03-layout-render.md`](03-layout-render.md) §8.5）。
> 本文件是应用驱动、帧循环、窗口生命周期、定时任务、平台 Shell、持久化与性能观测的**唯一权威**。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 应用驱动与场景 | `app/application.h`、`app/scene.h`、`app/display.h` |
| 定时 | `app/scheduler.h`、`widget/timer.h` |
| 校验 | `app/validate.h`、`app/validate_ui.h` |
| 平台 Shell | `app/clipboard.h`、`app/file_dialog.h`、`app/system_tray.h`、`app/menu.h`、`app/shortcuts.h` |
| 性能可视化 | `app/perf_overlay.h`、`perf/` |
| 工具链集成 | `app/generate_ui.h`、`app/hot_reload.h` |
| 窗口与后端 | `window/` |
| 偏好持久化 | `preferences/preferences.h` |
| 存储抽象 | `storage/`（6 个头） |
| 调试门面 | `debug/` |

---

## 2 Application 与 Scene

### 2.1 构造形态

`Application` 的构造形态**不随后端数量增长**：

| 构造 | 说明 |
|:---|:---|
| `Application(Scene, unique_ptr<Window>, WindowOptions)` | 接受 `create_window(XxxOptions)` 产出的已组装 `Window`；`max_frames` 等运行期参数由 `WindowOptions` 透传；内部把该 `Window` 登记为**主窗口宿主**（`WindowHost`） |
| `Application(Scene, unique_ptr<Surface>, WindowOptions)` | 注入自定义 `Surface`，内部同样经 `create_window` 组装 `Window` 后登记宿主 |
| `Application(Scene, w, h)` | 无头便捷构造，**登记一个没有 OS 窗口的宿主**，仅用于 `render_to_png` 与程序化派发 |

三个构造共用私有 `register_host` 接线：创建 `WindowHost`、绑定后端事件与窗口级可见性/几何态上报、安装应用级快捷键前置拦截，并把首个登记的宿主指定为主窗口（多窗口见 §2.4）。

### 2.2 主要接口

| 成员 | 说明 |
|:---|:---|
| `run()` | 启动帧循环：pump → 派发 → `tick`（动画 / 定时任务推进）→ `present_root` |
| `set_on_frame(cb)` | 注入每帧自定义逻辑，在 `present_root` 之前调用 |
| `set_overlay(shared_ptr<Widget>)` | 注入独立于控件树的 HUD 叠加层（典型 `PerfOverlay`），由 `Window::present_root` 在 tree paint 之后、present 之前合成。叠加层渲染到独立离屏缓冲、以约 2Hz 重绘自身，不触发整树重绘 |
| `dispatch_*` / `tick` / `render_to_png(path)` | 程序化派发与离屏渲染 |
| `window_state()` / `window_mode()` | 响应式 `State<WindowState>&` / `State<WindowMode>&`（在 `Effect` 内读取自动订阅刷新） |
| `set_on_window_state(cb)` / `set_on_window_mode(cb)` | 命令式回调 |
| `scheduler()` | 取应用级 `Scheduler` |
| `animator()` | 取应用级 `Animator &`，由 `run()` 每帧按 `dt` 驱动，供注册动画控制器 |
| `set_strict_mode(StrictMode)` / `strict_mode()` | 严格模式设置器与**无参**取值器（[`01-core.md`](01-core.md) §4.3）。Application **没有**带参的 `strict_mode(StrictMode)` 形式；带参链式方法属 `App` 构建器（§4），其 `run()` 内部经 `set_strict_mode` 套用到 `Application` |

### 2.3 Scene

`Scene`（`app/scene.h`）是场景快照。

- `Scene::serialize()`：**无参**，返回 `std::string`，把当前 UI 树序列化为 JSON 结构。
- `Scene::serialize_widget(const Widget&, std::string&)`：内部递归辅助。
- `Scene::render_to_png(path, width, height)`：无头离屏渲染（[`03-layout-render.md`](03-layout-render.md) §8.4）。

序列化输出的形态与补丁协议见 [`08-tooling.md`](08-tooling.md)。

### 2.4 多窗口

一个 `Application` 可同时驱动多个窗口。每个窗口是一个 **`WindowHost`**（`app/window_host.h`）：一个完整可运行单元。

| 归属 | 内容 |
|:---|:---|
| `WindowHost`（逐窗口） | `Window`（**可为 nullptr**）、`Scene`、`FocusManager`、`EventDispatcher`/`TouchDispatcher` 捕获表、帧统计 `FrameStats`、窗口角色 `WindowRole` |
| `Application`（应用级共享） | `Animator`、`Scheduler`、`ShortcutRegistry`、`CommandRegistry`、跨线程回投队列、**统一帧循环** |

- **隔离语义**：焦点与指针/触控捕获**不跨窗口**，事件只在其所属宿主的作用域内派发；脏区、HUD 叠加层、窗口状态快照（`WindowState`/`WindowMode`）均为逐窗口状态。
- **共享语义**：`Animator`/`Scheduler` 每帧只推进一次（`dt` 唯一）；快捷键与命令注册表跨窗口一致（经宿主键盘前置拦截器注入）。
- **无头宿主**：`Window` 可为 nullptr，把「有/无窗口」统一成一条代码路径（无头构造即登记无窗宿主）。
- **兼容**：`window()` / `scene()` / `focus()` / `dispatch_*()` / `render_to_png` 作用于**主窗口**（首个登记的宿主），单窗口用法行为与历史完全一致。

**登记与查询**

| 成员 | 说明 |
|:---|:---|
| `open_window(unique_ptr<Window>, Scene, WindowOptions) -> WindowId` | 追加窗口宿主；`Window` 为 nullptr 时退化为无头宿主 |
| `open_window(unique_ptr<Surface>, Scene, WindowOptions) -> Result<WindowId>` | 注入自定义 `Surface`，内部经 `create_window` 组装 |
| `close_window(WindowId)` | 程序化请求关闭（下一次帧末回收） |
| `window_host(WindowId)` / `windows()` / `window_count()` | 宿主检索与枚举 |
| `WindowHost`：id() / scene() / focus() / frame_stats() / dispatch_*() / render_frame() / tick() / should_close() / teardown() | 逐窗口运行期接口 |

**角色与退出策略**

| 角色 `WindowRole` | 语义 |
|:---|:---|
| `Main` | 主窗口（默认）：`scene()`/`focus()`/`window()` 的作用对象；`MainWindowClosed` 策略下其关闭连带关闭全部窗口 |
| `Auxiliary` | 独立辅助窗口：关闭只影响自己 |
| `Transient` | 从属窗口：`WindowOptions::owner` 指向所依附的宿主，**owner 关闭时连带关闭**（帧末回收阶段执行） |

| 退出策略 `ExitPolicy` | 行为 |
|:---|:---|
| `LastWindowClosed`（默认） | 最后一个**有 OS 窗口**的宿主关闭即退出；单窗口用法 ≡ 历史行为 |
| `MainWindowClosed` | 主窗口关闭 → 连带关闭全部窗口并退出（传统单主窗应用） |
| `ExplicitOnly` | 仅 `Application::quit()` 可退出：窗口全关也继续运行（常驻 / 托盘型应用） |

- `quit()` 在**任何**策略下都使 `run()` 退出；`set_main_window(WindowId)` 可改主窗口（默认首个登记的宿主）。
- `set_on_window_closed(cb)`：宿主被回收**之后**触发一次（参数为被关闭窗口 id），供释放外部资源。
- **回收规则**：`reap_closed()` 每帧末执行——先收集待回收 id 再统一销毁（杜绝迭代中销毁）；从属窗口的连带关闭只置位、由**下一次**回收执行；全部窗口都关闭时**保留主窗口宿主**，使 `scene()`/`window()` 等访问器在 `run()` 结束后仍可用。

**模态与父子窗口**

- `WindowOptions::owner` 声明所依附的宿主；`WindowOptions::modal = true` 时打开窗口会：① 对其 owner 调用 `Surface::set_owner()` 建立 OS 层从属（恒浮于 owner 之上、随其最小化、不产生独立任务栏条目）；② 对其 owner 调用 `Surface::set_enabled(false)` 屏蔽输入。窗口被回收时自动 `set_enabled(true)` 恢复（否则 owner 将永久停在禁用态）。
- **无需焦点陷阱**：`FocusManager` 逐窗口隔离，焦点本就不跨窗；模态只需阻断 owner 的 OS 输入。
- 后端映射：Win32 / D3D11 = `GWLP_HWNDPARENT` + `EnableWindow`；GLFW 无父子 API（`set_owner` 空实现，层级由 WM 决定）；Headless 记录调用供测试断言。

**z 序、显示器与 DPI**

| 能力 | 入口 | 后端映射 |
|:---|:---|:---|
| 提升 z 序 | `WindowHost::raise()` / `Surface::raise()` | Win32 `BringWindowToTop`；GLFW 以 `glfwShowWindow` 近似（无独立 API） |
| 激活窗口 | `WindowHost::focus_window()` / `Surface::focus_window()` | Win32 `SetForegroundWindow` + `SetFocus`；GLFW `glfwFocusWindow` |
| 所在显示器 | `Surface::display_id()` / `WindowHost::display_id()` | Win32 `MonitorFromWindow`（HMONITOR 句柄值，与 `app::Display::id` 同源）；未知 -1 |
| 迁移到显示器 | `WindowHost::move_to_display(id)` | 转发既有 `app::move_window_to_display`（居中到目标工作区） |
| DPI 变化 | `Surface::set_scale_change_handler()` → `WindowHost::on_scale_changed()` | Win32 `WM_DPICHANGED`（取 wParam 高位的建议 DPI）→ 更新 `scale` 并强制整帧重排重绘 |

**已知后端限制（多窗口）**

- **Wasm**：鼠标事件按 canvas 元素注册（多窗口可用），但**键盘**与 **resize** 注册在 `document` / `window` 级——多窗口下只有最后注册的 Surface 能收到，故 Wasm 上的多窗口键盘输入尚不可用。修复需拆成「document 级单一分发器 + 按焦点 Surface 路由」，须在 Emscripten 工具链下真机验证，属后续专项。
- **X11 / Wayland**：无全局窗口枚举能力，`raise` / `focus_window` 为尽力而为；等待通道是 per-surface 的，帧循环对这类后端施加 8ms 等待上限（见「统一帧循环」）。

**跨窗通信**

- **共享状态** → 直接用既有 `Store<S>`：多个窗口共享同一实例，各自 `au::connect(store, ...)` 订阅并在 `asSignal()` 上绑定控件，**无需新 API**。
- **一次性通知 / 命令** → `Application::bus()`（`WindowEventBus`）：`post<T>(payload, from)` 类型化发布，`on<T>(cb, filter_from)` 订阅（返回 RAII `Subscription`，析构自动取消；`filter_from` 用于点对点来源过滤）。
- 语义：主线程**同步扇出**（与 `Application` 的同步事件派发一致）；遍历前拷贝订阅列表，订阅者可在回调内安全新增 / 取消订阅；订阅句柄内部持有共享状态，**总线先析构、句柄后析构**也安全。

**窗口几何持久化**

| 接口 | 说明 |
|:---|:---|
| `WindowOptions::persist_id` | 几何持久化键（空 = 不持久化），多个窗口用不同键区分 |
| `Application::set_window_geometry_store(prefs)` | 指定 `Preferences` 存储；会对**已登记**窗口补做恢复（主窗口在构造期即已登记） |
| `WindowGeometry{origin, size, mode, display_id}` | 坐标与尺寸为**屏幕物理像素**（与 `app::Display` 同源）——只有物理坐标跨 DPI 稳定；恢复时按当前 `scale_factor` 折回逻辑 dp |
| `save_window_geometry` / `load_window_geometry` | 读写；另有 `Preferences::Group` 重载用于**窗口组**：`prefs.group("windows")` + 各窗口键 |
| `is_window_geometry_usable(g, displays)` | 判据：尺寸为正，且与某显示器工作区**有交集**（部分越界算可用，多屏拼接/任务栏遮挡下不应拒绝恢复） |

- **时机**：窗口**打开时自动恢复**（`register_host` 与 `set_window_geometry_store` 两处入口）、**关闭时自动保存**（帧末回收阶段的收集步骤——包含因「保留最后一个宿主」而未真正销毁的单窗口场景，否则「关掉唯一窗口后下次启动恢复」永不生效；`WindowHost::geometry_saved()` 保证只写一次）。
- **兜底**：键缺失 / JSON 格式错误 / 几何不可用（显示器被拔除、分辨率变小导致完全落在屏外）→ 一律回退默认布局。**格式错误与不可用分两类 WARN**，便于区分「存储被破坏」与「显示器变了」。
- 本类不做隐式 I/O：落盘由调用方 `Preferences::flush()` 决定。

**统一帧循环**（`Application::run`，不再委托 `Window::run`）：

```
drain_posted → pump_all_once → on_frame → 逐宿主 tick → 共享 anim/sched tick → 逐宿主 render_frame → reap_closed → wait_once
```

- **pump 一次还是 N 次**：由 `Surface::pumps_thread_queue()` 决定。Win32（`PeekMessage(nullptr,…)`）与 GLFW（`glfwPollEvents()`）是线程/进程级共享队列，一次 pump 即抽干全部窗口消息并按 HWND 路由，故每帧只 pump 一次；X11/Wayland/Wasm 为 per-surface 队列，逐个 pump。
- **等待聚合**：每帧只等待一次，取各宿主 `decide_wait` 的**最小正值**（任一为 `0` 则不等待；全为「无限」才无限等待）。优先交由 `waits_thread_queue()==true` 的后端（Win32/GLFW）执行；无此类后端时（X11/Wayland）封顶 8ms 轮询，避免其余窗口饥饿。
- **回收时机**：`reap_closed()` 在**帧末**执行（不在事件派发栈内销毁宿主），并保留最后一个宿主，使 `scene()`/`window()` 等访问器在 `run()` 结束后仍可用。
- **跨线程回投**：`Task::set_main_poster` 唤醒**全部**窗口的等待通道，避免回投工作只唤醒其中一个窗口而延迟执行。

**帧统计归属**：单窗口刻意保留写入进程级单例 `FrameStats::instance()`（既有 `PerfOverlay`、基准工具与性能集成测试直读之，行为零变化）；登记第二个窗口起，各宿主切到自有实例（`WindowHost::own_frame_stats()`）。`PerfOverlay` 经 `Application::set_overlay` 自动绑定到主窗口统计，其他窗口的叠加层请显式 `bind_frame_stats(&host->frame_stats())`。

---

## 3 Window 与帧循环

### 3.1 帧循环

帧循环由 **`Application::run()` 统一驱动**（多窗口语义见 §2.4）：每帧 pump 事件 → 集中派发 → `tick` → 逐窗口 `present_root`（脏区决策）→ `present()`。`Window::run(on_frame, max_frames)` **保留**供单窗口低阶调用方（自拼帧循环、测试、demo 直驱）使用；多窗口请走 `Application::run`。所有构建、事件、重绘都在 UI 线程（单线程 UI，见 [`01-core.md`](01-core.md) §8.1）。

### 3.2 脏区追踪（默认开启）

`Window::present_root` 按「绘制脏 `DirtyRegionTracker` / 布局脏 `layout_dirty_` / 尺寸变化 / 根变化」四要素决策本帧：

| 情况 | 行为 |
|:---|:---|
| 四者全否 | **整帧跳过**（idle 零开销，上帧画面仍有效，返回 `true` 不重绘） |
| 仅绘制脏（文本选区高亮、主题切换、局部 `State` 文本变更） | **跳过整树 `layout`**，复用已缓存 `Node` 几何直接 `paint` |
| 布局脏或尺寸变化 | `layout + paint` |
| 根变化（`Navigator` 切换页面、`run_demo` 换树） | 强制整体重绘，避免停留旧页面 |

- `mark_needs_layout()` 置「布局脏 + 绘制脏」，`mark_needs_paint()` 仅置「绘制脏」。
- `Widget::on_dirty` 是 `std::function<void(bool)>`（`true` = 含布局脏），经 `install_dirty_sink`（`on_subtree_dirty`）接线整棵树。
- `enable_dirty_tracking(bool)` 可关闭，回到每帧全量重绘的历史行为；`force_full_redraw()` 供动画 / 视频 / 定时器持续重绘或外部环境突变时强制下一帧全绘。
- **首帧 `first_frame_ = true` 强制全绘**；重新挂载 / 根变化时自动 `mount` 接线响应式订阅，使 `State` 与修饰变更能标脏重绘。

**仅绘制脏帧的处理**：跳过 `begin_frame` 保留上帧帧缓冲（部分后端的 `begin_frame` 会清零整帧，直接 begin 会使裁剪外黑屏），先 `Painter::clear_rect(merged_bounds)` 把脏矩形并界重置为新帧零基底，再 `push_clip(merged_bounds)` 裁剪重绘。裁剪内从零基底按原序重新合成、裁剪外沿用上帧像素，两侧均与整帧重绘**逐位一致**。`install_dirty_sink` 回调标记控件最近一次 `paint` 的绝对几何（`Widget::paint_bounds()`），使裁剪命中精确区域。

**系统重绘请求驱动的帧不得只跳过**：由 `set_present_request` 回调（Win32 `WM_PAINT` / `WM_SIZE`）驱动的 `present_root`，即便脏追踪判定跳帧，也必须 `set_present_dirty({})` 后 `present()` 全量 blit 重新上屏——帧缓冲内容仍有效但窗口表面已被 OS 置无效（典型：**最小化还原**后为类背景刷底色，若只跳过则白屏；遮挡揭开同理）。普通 idle 帧不受影响，仍零上屏。

**脏矩形数量上限**：`DirtyRegionTracker` 累积的矩形数超过上限时整帧标脏（`mark_all()`），避免矩形数无界增长导致裁剪与遍历成本反超收益。默认 `DirtyRegionTracker::AURORA_MAX_RECTS == 16`，可调接口 `max_rects()` / `set_max_rects(std::size_t)`。**改变该值只影响合并时机与性能，不影响像素结果。**

### 3.3 事件驱动帧节流

`Application::run` / `Window::run` 采用「事件驱动 + 帧节流」：静态界面空闲时不忙轮询，CPU 占用趋近 0。

**决策纯函数**：

```cpp
compute_wait_timeout(has_dirty, anim_active, next_deadline_ms, frame_budget_ms, elapsed_ms, backend_paced) -> double
```

返回毫秒：`<0` 无限等待 / `0` 不等 / `>0` 等待时长。

**`Surface` 扩展**：

| 方法 | 说明 |
|:---|:---|
| `wait_events(double timeout_ms)` | 阻塞到事件或超时；默认限频 sleep，`Headless` 覆盖为 no-op，Win32 = `MsgWaitForMultipleObjectsEx`，GLFW = `glfwWaitEventsTimeout` |
| `request_wake()` | 线程安全唤醒；Win32 = `PostMessage(WM_NULL)`，GLFW = `glfwPostEmptyEvent` |
| `paces_frames()` | 后端自带帧节拍（如 D3D11 vsync） |

**`WindowOptions` 相关字段**：`int max_fps = 60`（活跃帧帧率上限，`0` = 不限）、`bool power_saving = true`（idle 阻塞等待；`false` = 忙轮询旧行为）、`RendererPreference renderer = Auto`。

**`Window` 新增**：`has_pending_dirty()`、`set_next_wait(double)`；`run` 在帧末 `wait_events` 阻塞（一次性消费，未设则不等）。

**`Animator::has_active()`** 与 **`Scheduler::next_deadline_ms()`** 供决策判断活跃动画与最近定时任务到期。

**跨线程回投**：`Application::run` 安装 `Task::set_main_poster`；`au::async` 的 `then` 回调入队 + `request_wake()` 唤醒主循环，下一帧开头主线程排水执行。单线程 UI 不变；无运行循环时在完成线程直接调用。

**`FrameStats` 观测**：`record_wait(double)`、`wakeup_count()`、`wakeups_per_sec()`、`sleep_ratio()`。

### 3.4 Win32 上屏与系统重绘

**上屏性能契约**：`Win32Surface::present()` 经常驻 BGRA（GDI 原生序）DIB section + `BitBlt` 上屏（全量帧整幅 RGBA→BGRA swizzle，脏区帧仅 swizzle + blit 脏矩形），**禁止退回 RGBA 掩码（`BI_BITFIELDS`）直推**——非原生掩码会迫使 GDI 逐像素慢速转换。`WM_SIZE` 同步重渲染后 `ValidateRect`，免去紧跟 `WM_PAINT` 的重复全量上屏。

**系统重绘处理**：窗口类背景刷 `wc.hbrBackground` 用浅灰实心刷 `RGB(245,245,247)`（而非默认黑色擦除）；`wnd_proc` 处理 `WM_PAINT`，在系统要求重绘时立即 `present()` 当前已就绪帧缓冲。

**最大化白闪处理**：`Surface` 提供 `set_present_request` 回调通道（默认空实现），`Window` 构造时把该回调接为「对当前缓存根再渲染一帧」；`Win32Surface` 的 `WM_SIZE` / `WM_PAINT` 在几何变化当下同步调用该回调，使离屏缓冲在 DWM 合成前已为新尺寸真实内容。浅灰刷保留作兜底。`present_count()` 观测器供测试验证「WM_SIZE 触发了同步重渲染」。

**关窗语义（多窗口）**：`WM_DESTROY` 只置位**本窗口**的 `should_close`，**不再** `PostQuitMessage(0)`——`WM_QUIT` 是**线程级**的，任一窗口销毁都投递它，会让同线程内所有窗口（乃至之后新建的窗口）在首次 poll 时被误判为「已请求关闭」，这是「关一个窗口整个应用退出」与「顺序创建多窗口立即退出」的根因。是否退出帧循环改由 `Application` 的 `ExitPolicy` 决定（§2.4）；外部/宿主代码主动 `PostQuitMessage` 时，`poll_platform_events` 仍按历史行为把 `WM_QUIT` 折算为关闭请求。

### 3.5 硬件加速上屏偏好

强类型枚举 `RendererPreference{ Auto, Software, GpuD3D11 }` 统一控制**上屏后端**（仅影响「像素如何上屏」，绘制仍由软件 `Painter` 完成）：

| 取值 | 行为 |
|:---|:---|
| `Auto` | 编译了 `AURORA_BACKEND_D3D11` 且设备可用则走 D3D11 GPU 上屏，否则静默回退 Win32/GDI（`AURORA_LOG_INFO`） |
| `Software` | 强制 GDI |
| `GpuD3D11` | 强制 D3D11；未编译或设备创建失败返回 `make_error(ErrorCode::RendererUnavailable, ...)`（slug `renderer-unavailable`），**不静默降级** |

`D3D11Surface` 支持 device-lost 恢复（present 报 `DXGI_ERROR_DEVICE_REMOVED` / `RESET` → 下次 `poll_platform_events` 重建 device / swapchain + 全量重渲染）、`set_vsync(bool)` 与 `paces_frames()`（`D3D11Options.vsync` 默认 `true`）。

---

## 4 App 流式构建器

`au::App()` 是薄封装，内部构造 `Application` 并进入帧循环，不破坏旧用法。

| 方法 | 说明 |
|:---|:---|
| `title(string)` / `size(w, h)` | 窗口标题与尺寸 |
| `surface(unique_ptr<Surface>)` | 注入自定义后端；`run()` 经 `create_window` 组装 `Window` |
| `window(unique_ptr<Window>)` | 接受 `create_window(XxxOptions)` 产出 |
| `view(Node)` | 根 UI |
| `on_frame(cb)` | 每帧回调 |
| `frames(int)` | 限制帧数，`-1` 跑到 `should_close` |
| `overlay(shared_ptr<Widget>)` | 链式注入 HUD 叠加层（等价于 `Application::set_overlay`） |
| `strict_mode(StrictMode)` | 严格模式 |
| `run()` | 进入帧循环 |

`run()` 的后端优先级：自定义 `Surface` > 预组装 `Window` > 自动检测（`auto_detect_surface()`）。

```cpp
au::App().title("Demo").size(640, 480).view(std::move(root)).run();

// 特化选项：先经工厂组装 Window，再交给 App
auto win = au::create_window(au::HeadlessOptions{ .png_path = "out.png" });
au::App().window(win ? std::move(win.value()) : nullptr)
         .view(build_ui()).frames(1).run();
```

> 指定初始化器不能指名基类成员。要设置通用 `WindowOptions` 字段，先构造基类选项再拷贝：
> ```cpp
> au::WindowOptions base{ .title = "Demo" };
> au::Win32Options opts;
> static_cast<au::WindowOptions &>(opts) = base;
> ```

---

## 5 运行时平台查询

`au::platform()` 是显式运行时平台查询，跳过后端探测。返回 `Platform{ PlatformKind kind, DeviceKind device, SurfaceKind surface }`，含 `is_mobile()` / `is_desktop()` / `capabilities()`。

`PlatformCapabilities` 字段：`multitouch`、`high_frequency_pointer`、`desktop`、`mobile`。

```cpp
const au::Platform p = au::platform();
if (p.is_mobile() && p.capabilities().multitouch) { /* 启用多指手势 */ }
```

---

## 6 生命周期

Aurora **不移植**安卓 `Activity` 的 `onCreate/onStart/onResume/onPause/onStop/onDestroy` 命令式生命周期——它面向 OS 窗口可见性，与声明式树模型冲突，且 per-widget 的 resume/pause 无实际意义。改为两级正交机制。

### 6.1 控件子树级 Lifecycle

`au::Lifecycle`（`widget/lifecycle.h`）是控制流控件，对应 Android `View.onAttachedToWindow` / `onDetachedToWindow` 而非 `Activity`。

构造 `Lifecycle(Node child, MountCb on_mount, UnmountCb on_unmount = {})`：

- `on_mount(const BuildContext&)` 在子树挂载完成后**恰好触发一次**，可注册外部源、启动定时器、读环境注入。
- `on_unmount()` 在控件被销毁（RAII 析构，如 `Repeater` 缩容 / `Navigator` pop / 持有 `Node` 释放）时触发清理。
- 卸载依赖 **RAII 析构**而非新增虚函数：派生类析构无法正确派发到 `~Widget()`，故用 `std::function` 回调 + 析构触发。
- `Show` 隐藏子树时保留 `Node` 存活（不析构、不卸载），故 `on_unmount` 不被触发——对齐 Flutter `Visibility`。

```cpp
au::Lifecycle(
    au::Text("子树"),
    [](const au::BuildContext &ctx) { subscribe(); },   // on_mount：恰好一次
    []() { unsubscribe(); }                              // on_unmount：销毁时清理
);
```

### 6.2 窗口级 WindowState 与 WindowMode

两个正交枚举，仅窗口层报告。

`WindowState`（`window/window_state.h`）：

| 取值 | 语义 |
|:---|:---|
| `Visible` | 前台激活 |
| `Occluded` | 可见但失焦（近似为「被其它窗口遮挡」） |
| `Hidden` | 最小化 / 不可见 |

由后端 `(minimized, active)` 经纯函数 `compute_window_state` 映射。精确像素级遮挡检测昂贵且普遍不被框架支持，故以「失焦 = 被遮挡」近似。

`WindowMode`（`window/window_state.h`）：`Normal` / `Maximized` / `Minimized` / `FullScreen`，与 `WindowState` **正交**——最大化只改变几何态，`WindowState` 仍为 `Visible`；最小化同时满足二者。纯函数 `compute_window_mode(minimized, maximized, fullscreen)` 提供映射。

**后端上报**：`Win32Surface` 处理 `WM_SIZE`（MINIMIZED / MAXIMIZED / RESTORED → 几何态；最小化同时置 `WindowState::Hidden`）与 `WM_ACTIVATE`（非 `WA_INACTIVE` → `Visible`，否则 `Occluded`）；`GlfwSurface` 注册 `glfwSetWindowIconifyCallback` / `glfwSetWindowFocusCallback` / `glfwSetWindowMaximizeCallback`；`HeadlessSurface` 提供 `simulate_window_state` / `simulate_window_mode` 测试 seam。后端各自仅「**状态变化时**」经 `Surface::WindowStateHandler` / `WindowModeHandler` 上报。

**上层聚合**：`Application` 暴露响应式 `State<WindowState>& window_state()` / `State<WindowMode>& window_mode()` 与命令式回调 `set_on_window_state` / `set_on_window_mode`；每帧经 `Window::present_root` 将当前快照注入根 `Environment`，子树可 `ctx.environment<WindowState>()` / `ctx.environment<WindowMode>()` 读取。

```cpp
app.set_on_window_state([](au::WindowState s) {
    if (s != au::WindowState::Visible) pause_animation();
    else resume_animation();
});
```

---

## 7 定时任务

### 7.1 Scheduler

`Scheduler`（`app/scheduler.h`）由 `Application::run()` 每帧 `tick(dt)` 推进（与 `Animator` 并列），并通过线程局部 `Scheduler::current()` 暴露给组件级 `Timer`（`run()` 起止自动 `set_current` / 重置）。单线程 UI 内无额外计时线程。

| 方法 | 说明 |
|:---|:---|
| `set_timeout(d, cb)` | 一次性延时任务，到期触发一次并自动移除，返回 `TimerHandle` |
| `set_interval(period, cb)` | 周期任务，每 `period` 触发，直至 `TimerHandle::cancel()`，返回 `TimerHandle` |
| `tick(double dt_seconds)` | 每帧推进并触发到期项（先收集到期项、再统一触发，避免回调内重注册导致迭代器失效） |
| `clear()` | 取消并清空全部任务 |
| `next_deadline_ms()` | 供帧节流决策读取最近到期时间 |

回调均在主线程执行。

### 7.2 TimerHandle

`set_timeout` / `set_interval` 的返回值：轻量、可拷贝、可值传递。

- `cancel()`：**幂等**，置取消标志——不访问 `Scheduler` 实例，句柄可安全跨作用域持有（含 `Scheduler` 已析构后取消）。
- `active()`：查询是否仍活跃。

内部经 `shared_ptr<TimerEntry>` 引用，触发前校验 `cancelled` 避免悬空。

### 7.3 组件级 Timer

`Timer`（`widget/timer.h`）是声明式、响应式为主的控件。

构造 `Timer(period, TickBuilder, on_tick = {})`，其中 `TickBuilder = std::function<Node(const SignalView<int>&)>`，在构造时调用**一次**构建子树，内部经响应式绑定刷新（非每次 tick 重建）。

- 挂载（`on_mount`）向 `Scheduler::current()` 注册周期任务。
- 每次 tick 自增内部 `State<int>`（经 `ticks()` 暴露为 `SignalView<int>`，计数从 1 起）驱动子 UI 自动重绘，并可选调用 `on_tick(int)`。
- 卸载 / 析构自动 `cancel()` 句柄。
- 降级：无运行中 App（`Scheduler::current() == nullptr`，如纯 `render_to_png`）时记 `Diagnostics::warn` 并以 `ticks() == 0` 渲染子 UI，不崩溃。

```cpp
// 应用级命令式：每 1s 轮询，5s 后停止
auto &sched = app.scheduler();
au::TimerHandle h = sched.set_interval(1s, [&] { poll(); });
sched.set_timeout(5s, [&] { h.cancel(); });

// 组件级声明式：子 UI 经 ticks() 响应式刷新
au::Timer(1s, [](const au::SignalView<int> &tick) {
    return au::Text(au::computed([&] {
        return au::LocalizedString{ "tick: " + std::to_string(tick.get()) };
    }));
});
```

---

## 8 平台 Shell

### 8.1 文件对话框

`app/file_dialog.h` 提供 `file_dialog::open_file` / `save_file` / `open_folder`。

- Win32 经 COM 实现（`IFileOpenDialog` 多选 / `IFileSaveDialog` / `FOS_PICKFOLDERS` 文件夹选择器）；非 Win32 走 headless 钩子。
- `Options{title, initial_dir, filters}` + `interactive` 开关，避免自动化环境卡在 GUI。

### 8.2 剪贴板

`Clipboard`（`app/clipboard.h`）在 Win32 经 `SetClipboardData` 实现，默认 no-op。文本复制经 `Clipboard::set_text`。

**测试注入点（test-only）**：`install_test_backend` / `reset_test_backend` / `remove_test_backend` 三个静态函数构成仓库私有测试设施（`tests/`）用于并行隔离的最小注入面——安装后 `set_text` / `get_text` / `set_image` / `get_image` 全部改走进程内 memory 后端，不触碰系统剪贴板。

- 声明常驻（消费端调用始终可编译）；实现体按 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏裁切，任一关闭（含 Release 下 DEBUG AUTO 自动关闭）时返回 `false` / no-op，平台行为不变。
- `install_test_backend()` 清空并激活后端、返回是否生效；`reset_test_backend()` 仅清内容；`remove_test_backend()` 卸载并恢复平台实现、返回是否确有后端被卸载。
- 编译开关语义见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) §4（`AURORA_ENABLE_TEST_HOOKS`）。

**Linux/macOS 实现（及为何测试不得断言系统剪贴板）**：非 Win32 平台经 `xsel`/`xclip`（macOS `pbcopy`/`pbpaste`）外壳进程中转（`popen`），并在作用域内把 `SIGPIPE` 置为忽略，避免工具缺失或无显示时子进程立即退出导致写已关闭管道把本进程杀掉。该路径的 selection 所有权是**异步**的（xsel 二次写入依赖既有守护进程交接、xclip 常驻等待读者），内容读回还受各工具的换行约定影响——因此**仓库测试禁止用系统剪贴板做内容断言**（既非确定，也不是 aurora 自身的契约）：`itest_text_selection` 的 Ctrl+C 复制用例改为先断言环境无关的选区范围，再在 `install_test_backend()` 成功时经进程内后端断言复制内容；双测试宏未开（Release）时仅跳过内容断言，选区/焦点/按键消费断言仍生效。

### 8.3 系统托盘

`SystemTray`（`app/system_tray.h`）在 Win32 经 `Shell_NotifyIcon` + 隐藏消息窗口实现，支持图标、气泡与激活回调 `on_activate`；非 Win32 为 no-op（仅记录 `last_balloon_message`）。

### 8.4 菜单、快捷键、命令与显示

| 头文件 | 能力 |
|:---|:---|
| `app/menu.h` | 菜单（`MenuBar` 控件的数据层） |
| `app/shortcuts.h` | 快捷键注册 |
| `commands.h` | 命令模型与注册表（快捷键 / 菜单 / 命令面板的统一真源） |
| `widget/command_palette.h` | 命令面板（模态浮层，模糊检索并执行命令） |
| `app/display.h` | 显示设备与 DPI 查询 |

**命令是唯一真源**：`CommandRegistry`（`commands.h`）持 `Command{ id, title, icon, category, action, default_binding, scope, enabled, when_label }`。`bind_shortcuts(ShortcutRegistry&)` 与 `to_menu_items()` 是它面向快捷键与菜单的两个**投影**，命令面板是第三个消费方；三者共用 `invoke(id)` 出口，故启用条件与空动作判定单点生效。启用条件为两段式：`enabled` 谓词承担运行期判定（空 = 恒启用），`when_label` 仅作展示 / 序列化标签（**不参与求值**，也不解析条件 DSL）。`to_json()` 产出 `{"commands":[…]}` 自描述信封供工具面枚举；`search()` 与工具面共用 `command_fuzzy_score()`，故 AI 检索与用户检索次序一致。

**接线**：`Application::commands()` 返回注册表；`app.commands().bind_shortcuts(app.shortcuts())` 一行把默认快捷键接入。绑定为**显式**而非 `run()` 内自动执行——否则 `run()` 之后注册的命令会静默失效。`Application` 在 `dispatch*` 入口统一暴露「当前焦点管理器」（`current_focus_manager()`），快捷键动作与控件回调内同样可取到，模态弹层据此完成焦点陷阱。原「命令式逃生舱」`aurora::imperative::run_raw`（`imperative.h`）与命令系统无关。

**命令面板键位**：`CommandPalette` 打开时把自己的作用域压入 `FocusManager`（子树内**唯一**可聚焦控件是搜索框，故左右方向键仍落到搜索框做光标移动、上下方向键不引发焦点跳转）；Enter 经搜索框的提交回调执行选中项；Esc / ↑ / ↓ 经打开期临时注册的快捷键绑定接管（依赖注册表已 `bind_shortcuts`，未接线时这几键不可用，面板以 WARN 提示）。Space 只经文本输入落字，不触发执行。命令清单可经 `to_json()` 序列化并由 MCP 工具面枚举，见 [`08-tooling.md`](08-tooling.md) §7.1。

---

## 9 偏好与存储

### 9.1 Preferences

`Preferences`（`preferences/preferences.h:65`）是键值持久化门面，类型化读写并带响应式绑定。

| 成员 | 说明 |
|:---|:---|
| `Preferences(file)` / `Preferences()` | 绑定配置文件路径即获得持久化；无路径时为内存模式 |
| `at(file, opts)` / `with_location(name, dir = default_config_dir(), opts)` | 文件模式便捷静态构造：指定完整路径 / 在 `dir` 下按 `name`（自动补 `.json`）落位（`:85` / `:91`） |
| `instance(name = "app")` / `instance(name, dir)` / `instance_at(name, file)` | 按名注册表的进程级单例（懒构造、线程安全；同名重复调用返回同一实例，`:102`–`:108`） |
| `default_config_dir()` | 解析平台默认配置目录：XDG_CONFIG_HOME → LOCALAPPDATA(Windows) → HOME/.config → 当前工作目录（`:112`） |
| `is_persistent()` / `file_path()` | 持久化状态与路径查询（`:115` / `:118`） |
| `last_load_error()` | 上次加载错误（`:121`） |
| `group(name) -> Group` | 命名空间分组（`:181`） |
| `flush()` / `reload()` | 写盘 / 重载，返回 `Result<void>`（`:210` / `:213`） |
| `keys()` / `contains(key)` / `remove(key)` / `clear()` | 键集合操作（`:218`–`:227`） |
| `get<T>(key, fallback)` / `watch<T>(...)` / `binding<T>(...)` | 类型化读取；`watch` 返回订阅，`binding` 返回 `Binding<T>` |

未绑定文件时为**内存模式**，写盘返回 `prefs-not-persistent` 错误（slug 见 [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)）。

### 9.2 Storage

`Storage`（`storage/storage.h:25`，命名空间 `aurora::storage`）是记录级存储抽象，支持 JSON 与二进制载荷、异步卸载与变更通知。

**构造与默认实例**

| 方法 | 说明 |
|:---|:---|
| `create(FilesystemOptions = {}) -> Result<Storage>` | 打开缺省文件系统后端（零额外依赖，始终可构造）；打开失败返回 `storage-backend-unavailable` 错误（`:28`） |
| `create(unique_ptr<StorageBackend>) -> Storage` | 注入任意后端（自定义 / SQLite / 测试 Memory），对标 `Application(Scene, unique_ptr<Surface>)`（`:31`） |
| `set_default(Storage)` / `default_instance()` | 可选的进程级默认实例（对标 `Preferences::instance`，`:156`–`:157`） |

**同步 API**

| 方法 | 说明 |
|:---|:---|
| `put(id, const Json&)` / `get(id)` / `remove(id)` / `list()` / `contains(id)` / `clear()` | JSON 记录 CRUD（`:34`–`:39`） |
| `put(id, const StorageBytes&)` / `get_bytes(id)` | 二进制载荷（`:43`–`:44`） |
| `get_value(id)` | 返回原始 variant `StorageValue`（Json 或 bytes，`:45`） |
| `put_record(id, const StorageRecord&)` / `get_record(id)` | 信封级记录读写（`:48`–`:49`） |
| `put<T>(id, const T&)` / `get<T>(id)` | 类型化便捷层（`StorageStorable<T>` 约束，`:64`–`:125`）：自动信封化类型名 / 版本 / 编码（JSON 或二进制，按 `StorageBinarySerializable<T>` 分派）；`get<T>` 返回 `Result<T>`，类型不匹配报 `storage-type-mismatch`、编码不支持报 `storage-encoding-mismatch`；存储版本低于 `storage_version(const T*)` 时自动经 `migrate_storage(old_version, const T*, payload)` 升版迁移 |

三个定制点（`serializable.h`）均以 `const T*` 指针实参触发 ADL，用户在 T 所在命名空间定义非模板同名函数即可覆盖（零参模板调用依赖「显式模板实参参与 ADL」，GCC/MSVC 未实现，故不用）：

- `storage_version(const T*) -> std::uint32_t`：默认恒 1。
- `storage_type_name(const T*) -> const std::string&`：默认 typeid 短名（static 缓存零分配）。
- `migrate_storage(std::uint32_t old_version, const T*, Json|StorageBytes) -> Result<Json|StorageBytes>`：默认恒等返回。

**异步 API**（返回 `aurora::Task<...>`，见 [`02-state.md`](02-state.md) §5）

`async_put` / `async_get` / `async_get_value` / `async_remove` / `async_list`（`:53`–`:58`）。

**事务与变更通知**

- `transaction(std::function<Result<void>(Storage&)>) -> Result<void>`（`:61`）：跨记录事务。
- `on_change(StorageChangeCallback) -> aurora::Subscription`（`:153`）：变更订阅，返回 RAII 句柄。

**后端抽象**：`storage/storage_backend.h` 定义后端接口，`fs_backend.h`（文件系统）与 `memory_backend.h`（内存）是两个实现；`serializable.h` 定义可序列化概念，`storage_types.h` 定义 `StorageRecord` / `StorageValue` / `StorageBytes`。

存储相关错误码：`storage-backend-unavailable`、`storage-record-not-found`、`storage-record-corrupt`、`storage-type-mismatch`、`storage-encoding-mismatch`、`storage-io-error`（见 [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)）。

---

## 10 性能观测

| 头文件 | 能力 |
|:---|:---|
| `perf/counters.h` | 渲染计数器（`RenderCounters`，PROFILING 门控） |
| `perf/perf_log.h` | 性能日志与 `snapshot_json()` |
| `perf/perf_session.h` | 性能采集会话 |
| `perf/profiler.h` | 插桩 profiler |
| `perf/scroll_bench.h` | 滚动基准 |
| `perf/stopwatch.h` | 计时器 |
| `perf/trace_writer.h` | 轨迹写出 |
| `app/perf_overlay.h` | 屏幕性能叠加层（`PerfOverlay`，经 `Application::set_overlay` 注入） |

`FrameStats`（`app/perf_overlay.h`）的读数为**方法**：`fps()` / `avg_frame_ms()` / `worst_frame_ms()` / `jitter_ms()` / `percentile_ms(p)`（`p ∈ [0,1]`，任意百分位帧时间）/ `dropped_frame_count()` / `dropped_frame_ratio()` / `hitch_count()` / `idle_frame_count()` / `total_frames()` / `frame_budget_ms()`，以及 `layout` / `paint` / `present` 三相位环形缓冲（`avg_layout_ms()` / `avg_paint_ms()` / `avg_present_ms()`）。

`aurora::debug::perf_snapshot()`（§11.2）把上述方法映射为 **JSON 键**输出——键名与方法名不同：`p50_ms` = `percentile_ms(0.5)`、`p99_ms` = `percentile_ms(0.99)`、`dropped_frames` = `dropped_frame_count()`、`dropped_ratio` = `dropped_frame_ratio()`、`hitches` = `hitch_count()`、`idle_frames` = `idle_frame_count()`，另含 `fps` / `avg_frame_ms` / `worst_frame_ms` / `jitter_ms` / `total_frames` / `frame_budget_ms` 同名键与 `perf_log` 子对象。引用读数时勿把 JSON 键当成 C++ 成员。

`PerfOverlay` 以 `stats_line3()` 展示 `wakeups_per_sec()` / `sleep_ratio()` 等帧节流读数。

---

## 11 调试门面（`aurora::debug`）

`aurora::debug`（头 `include/aurora/debug/debug_backend.h`，经 `aurora.h` 单一入口暴露）为真实后端调试能力提供统一薄封装入口，不搬迁任何生产子系统引擎（Inspector / Diagnostics / perf 留原地）。其中**调试能力函数**门控 `AURORA_ENABLE_DEBUG`：Release 下不产出调试代码——`capture` 返回 disabled 错误、`surface_state` 返回 unavailable JSON；**输出目录 API**（`set_output_directory` / `output_directory` / `resolve_output_path`）与 **feature 宏运行时查询**（`feature_flags` / `feature_flags_json`）无调试内部依赖，不受该门控、始终可用（见下表与「门控与 ODR 安全」段）。

| 能力 | 说明 |
|:---|:---|
| `CaptureSource{ Framebuffer, OnScreenWindow }` | 截图源。`Framebuffer` = 软件帧缓冲（全后端通用、确定性）；`OnScreenWindow` = 真实屏幕窗口（含 OS 装饰，Wayland / Headless 不支持） |
| `capture(Surface&, path, src = Framebuffer) -> Result<bool>` | 自动建父目录后转发 `Surface::save_snapshot` 或 `Surface::capture_window` |
| `set_output_directory(dir)` / `output_directory()` / `resolve_output_path(path)` | 输出目录 API（无调试内部依赖，始终可用）；默认 `current_path()/aurora_debug` |
| `feature_flags() -> FeatureFlags` / `feature_flags_json() -> Json` | 编译期 feature 宏开关运行时查询（始终可用，编译期常量快照）。强类型结构体字段 + JSON 导出（键 = 完整宏名）。C++ 侧宏镜像单点收口于 `src/aurora/debug/feature_flags.cpp`，应用代码零 `#ifdef`（需求 #14） |
| `surface_state(const Surface&) -> Json` | `width` / `height` / `scale_factor` / `frame_count` / `clear_color` / `should_close` / `has_native_window` |

**门控与 ODR 安全**：API 头**始终声明**，调试能力函数的 `.cpp` 体按 `AURORA_ENABLE_DEBUG` 裁切（例外：输出目录三函数的定义不裁切、无条件编译，与「始终可用」一致）；`Surface::save_snapshot` / `capture_window` 默认实现按运行时 `data()` 判空（宏无关），后端专属截图体门控。两函数在 `Surface` 上**始终声明**（vtable 槽稳定，属 `Surface` 契约）。Win32 家族两路（`Win32Surface` GDI 上屏 / `D3D11Surface` GPU 上屏，共用 `Win32Window` 宿主，经共享 `detail::capture_window_by_hwnd` 走 PrintWindow 路径）、X11、GLFW 在 `AURORA_ENABLE_DEBUG` + 对应后端下覆写 `capture_window`；Headless/Wayland 保持 unsupported（Wayland 客户端无法截图，属安全限制）。

`Surface` 另新增虚函数 `framebuffer_size()`（默认返回逻辑 `size()`）：`save_snapshot` 以它作 PNG 宽高；按 DPI 物理分辨率分配缓冲的后端须覆写返回物理像素，避免缩放比 ≠ 1 时 PNG 尺寸与像素数据错位。`native_handle()` 为 `const`（只读查询），**基类默认返回 `nullptr`**；真实窗口后端必须覆写为宿主原生句柄——Win32 家族两路（`Win32Surface` GDI 上屏 / `D3D11Surface` GPU 上屏，共用同一 `Win32Window` 宿主）**均**覆写为 `hwnd()`。上表 `surface_state()` 的 `has_native_window` 即由它非空判定，漏覆写会让真实窗口后端**恒报 false**（`D3D11Surface` 曾如此，2026-09-13 补齐），故由 `utest_native_surfaces` 的 `windows_family_native_handle_contract` 类型级守门。

### 11.1 可视化调试叠层

`include/aurora/debug/debug_paint.h`（经 `aurora.h` 暴露），全部门控 `AURORA_ENABLE_DEBUG`，Release 下体为零开销 no-op。

| 成员 | 说明 |
|:---|:---|
| `DebugPaintFlags{ layout_guides, relayout_boundaries, layer_borders, repaint_highlight, overdraw }` | 运行时叠层开关（bool，默认全 false） |
| `set_flags(...)` / `flags()` / `any_flag_enabled()` | 全局开关读写 |
| `current_debug_frame()` / `bump_debug_frame()` | 调试帧计数（`repaint_highlight` 时序判定用） |
| `paint_debug_overlays(Painter&, const Widget& root, const Rect& root_bounds, const BuildContext&)` | 在 `Window::present_root` 全树绘制后调用，按开启的 flag 统一绘制 5 个叠层 |
| `DebugOverlayStats` / `overlay_stats()` / `reset_overlay_stats()` | 叠层绘制计数（测试断言用） |
| `widget_picker(root, root_bounds, ctx, screen) -> DebugPickResult` | 复用 `Widget::hit_test_chain` 解析命中链；`DebugPickResult{ chain（root→最深，元素 `DebugPickNode{type_name, bounds}`）, hit }` |

叠层配色：`layout_guides` 青色盒、`relayout_boundaries` 品红盒、`layer_borders` 橙盒、`repaint_highlight` 循环色填充、`overdraw` 控件粒度暖色热力。

### 11.2 运行时信息导出

`include/aurora/debug/debug_runtime.h`，5 个薄封装 / 聚合门面，全部双门控 `AURORA_ENABLE_DEBUG`；Release 下统一返回 `{"available":false,"reason":"AURORA_ENABLE_DEBUG not enabled"}`。

| 函数 | 说明 |
|:---|:---|
| `widget_tree(const Node&) -> Json` | 薄封装控件树导出（引擎留 `inspector/`） |
| `perf_snapshot() -> Json` | 聚合 `FrameStats` 读数 + `PerfLog::snapshot_json()`（包为 `perf_log` 子对象） |
| `frame_phase_timeline(limit = 64) -> Json` | 复用 `FrameStats` 的 `layout` / `paint` / `present` 三相位环形缓冲；输出均值 + 最近 `limit` 帧 `recent_frame_ms`（最新在前）+ ASCII `flamegraph`（48 宽，`L` / `P` / `R`） |
| `why_trace(limit = 64) -> Json` | 输出 `mark_needs_layout` / `mark_needs_paint` 触发因果链。每条 `entry{ kind("layout"\|"paint"), type, frame, propagated }`；`propagated=false` 表示业务根因，`true` 表示引擎沿父链自动冒泡（在 relayout boundary 处截断） |
| `diagnostics() -> Json` | 只读封装 `Diagnostics::get_last_diagnostics()` |

`why_trace` 在 DEBUG 下经 `detail::record_dirty` 写入全局 ring buffer（cap 256，超界 `pop_front`）。`mark_needs_*` 的公开签名不变（委托私有 `*_impl(bool propagated)`），Release 热路径零开销。

> `aurora::debug` 命名空间下的全部公共自由函数由 `tools/gen/gen_debug_api.cpp` 从声明源 [`debug_api.toml`](../debug_api.toml) 自动生成到 `aurora_api.json` 的 `"debug"` 段。新增或改名调试函数时，只改 `debug_api.toml` 再重跑生成器，无需在本文手列。

---

## 12 需求规格

### 12.1 #14 零 \#ifdef 跨平台 + 插件式平台扩展

**核心目标：** AI 无需处理平台分支。

```cpp
// 应用代码 100% 跨平台，零 #ifdef
au::App().title("My App").size(640, 480).view(std::move(root)).run();

// 平台差异封装在内部，提供显式运行时查询
if (au::platform().is_mobile()) { /* 移动端适配 */ }
```

**关键约束：**

- 应用层代码**绝不出现** `#ifdef _WIN32`。
- 平台差异通过**运行时查询**而非编译期宏。
- 所有平台差异封装在平台抽象层，对外暴露统一接口。
- 平台特定能力通过可选 trait / 插件显式启用。

**库内部无法回避的编译期剪裁**：平台 / 架构 / 位宽 / 编译器 / 能力分支一律使用 `core/platform.h` 的规范化目标宏（`AURORA_PLATFORM_*` / `AURORA_ARCH_*` / `AURORA_BIT_*` / `AURORA_COMPILER_*` / `AURORA_CAP_*`），**禁止直接书写 `_WIN32` / `__linux__` 等原生平台宏**。例外：该头自身、`third_party/`、CMake 脚本、`_WIN32_WINNT` 等 SDK 版本旋钮。

**自动化守护**：`tools/check/check_platform_macros.py`（CTest 用例 `check_platform_macros`）扫描 `include/` 与 `src/` 全部预处理条件（含续行），命中原生平台/架构/位宽宏即红灯；`core/platform.h` 自身按例外豁免，`_WIN32_WINNT` / `_WIN32_IE` 等 SDK 旋钮不在禁用集合。规范化宏（`AURORA_PLATFORM_*` / `AURORA_ARCH_*` / `AURORA_BIT_*` / `AURORA_BACKEND_*` / `AURORA_COMPILER_*` / `AURORA_CAP_*`）的分支密度仅打印报告、不设门槛，供平台抽象层演进时追踪趋势。编译器特性宏已收敛：`core/platform.h` 现提供 `AURORA_COMPILER_*`（GCC / CLANG / MSVC 三个 base 加 APPLE_CLANG / CLANG_CL / MINGW / EMSCRIPTEN 精化），库内原生 `__GNUC__` / `__clang__` / `_MSC_VER` / `__MINGW*` / `__apple_build_version__` 一律映射为后者，分支统一经 `AURORA_COMPILER_*`；原生编译器宏仅允许出现在 `core/platform.h` 自身（检查豁免）。**编译期能力宏已独立成族**：`core/platform.h` 提供**恒定义**的 `AURORA_CAP_*`（取值 0/1，回答「本 TU 可用什么能力」而非「目标是什么平台」），现仅 `AURORA_CAP_THREADS`——Emscripten 未开 `-pthread` 时为 0、其余目标为 1；原先散落的 `__EMSCRIPTEN_PTHREADS__` 直用一并归入该宏。

**版本常量**（`core/version.h`）：`AURORA_VERSION_MAJOR` / `MINOR` / `PATCH`（数字分量，CMake `project(VERSION)` 注入）、`AURORA_VERSION_SUFFIX_STR` + `AURORA_HAS_VERSION_SUFFIX`（semver 预发布后缀，来自 CMake 缓存变量 `AURORA_VERSION_SUFFIX`）、合成宏 `AURORA_VERSION_STRING`（完整 semver 串）。**库发布版本的单一事实来源是根 `CHANGELOG.json` 的 `currentVersion`**。

**Web / WASM 平台适配：**

| 方面 | 做法 |
|:---|:---|
| 事件循环 | 映射到浏览器 `requestAnimationFrame` 循环，而非自旋等待 |
| 线程模型 | **规划中，未实现**：`au::async` 映射为 Web Worker + `postMessage`、在支持 `SharedArrayBuffer` + `Atomics`（需跨源隔离）时启用真正多线程，均为需求愿景，仓内无任何接线（无 Worker / pthread / SharedArrayBuffer 代码）。当前 `WasmSurface` 仅负责 `<canvas>` 呈现与 rAF 驱动；`ThreadPool` **未做 WASM 适配**——它无条件使用 `std::thread` 与 `std::thread::hardware_concurrency()`（`core/thread_pool.h`，无 `EMSCRIPTEN` 分支），在 Emscripten 下属依赖 pthread / 共享内存的**未验证路径**（非静默降级为单线程），待补平台分支或改为协程驱动 |
| 渲染 | 渲染目标为 `<canvas>` 元素，内部自动选择 |
| 限制 | 不支持高频指针直通路，自动降级为 `requestAnimationFrame` 轮询，并在 `au::platform().capabilities()` 中声明此限制 |

应用代码使用相同的 `au::async` / `co_await` API，无需 `#ifdef`。

### 12.2 #15 跨平台一致行为 + 黄金文件验证

**核心目标：** AI 无需考虑平台差异。

- 布局引擎**自研**（不依赖平台布局），保证像素级一致。
- 字体渲染统一（自带文本整形引擎，见 [`03-layout-render.md`](03-layout-render.md) §8.2）。
- 事件模型统一（触摸 / 鼠标 / 键盘统一抽象）。
- 配套**跨平台布局测试套件**与**黄金文件验证**，确保同布局在各平台结果一致。
- AI 生成的代码无需为不同平台微调。

**验收标准：** 同一棵树在各后端下产出相同的逻辑快照（Level 1 / Level 2）；黄金文件比对零差异。逻辑快照黄金文件由 `utest_offscreen` 承担（11 个固定尺寸布局场景对 `tests/golden/logical_snapshots.json` 逐字段比对，见 [`03-layout-render.md`](03-layout-render.md) §10.1）；像素级 golden 由 `itest_image_view` / `utest_offscreen` 对 `tests/golden/*.png` 比对。
