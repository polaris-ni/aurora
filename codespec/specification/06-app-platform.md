# 应用、窗口与平台（app / window / platform / preferences / storage / perf）

> 覆盖 `include/aurora/app/`（20 个头）、`window/`（21 个头）、`preferences/`、`storage/`、`perf/`、`debug/` 与后端 `Surface`（后端清单见 [`03-layout-render.md`](03-layout-render.md) §8.5）。
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
| 存储抽象 | `storage/`（7 个头） |
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
| `set_overlay(shared_ptr<Widget>)` | 注入独立于控件树的 HUD 叠加层（典型 `PerfOverlay`），由 `Window::present_root` 在 tree paint 之后、present 之前合成。叠加层渲染到独立离屏缓冲、按 `Window::AURORA_HUD_REFRESH_MS`（500ms）重绘自身，不触发整树重绘；**且叠加层可见时帧循环在空闲期也按时唤醒**（仅刷 HUD、不重排不重绘树），读数不会停在上一次活跃时的值上 |
| `dispatch_*` / `tick` / `render_to_png(path)` | 程序化派发与离屏渲染 |
| `window_state()` / `window_mode()` | 响应式 `State<WindowState>&` / `State<WindowMode>&`（在 `Effect` 内读取自动订阅刷新） |
| `set_on_window_state(cb)` / `set_on_window_mode(cb)` | 命令式回调 |
| `scheduler()` | 取应用级 `Scheduler` |
| `animator()` | 取应用级 `Animator &`，由 `run()` 每帧按 `dt` 驱动，供注册动画控制器 |
| `audio()` / `audio_shared()` | 应用级默认音频上下文（**惰性创建**，`media/audio.h` 图，[`03-layout-render.md`](03-layout-render.md) §9.4）：首次调用构造 `AudioContext` 并启动内置设备后端，未编译/启动失败自动静默模式；典型接线 `player.set_audio_context(app.audio_shared())` |
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
- **位置类事件的派发根与坐标口径**：鼠标/滚轮/触控/文件拖放/程序化指针以 `Window` 的整窗下沉壳（`Window::hit_root()`，`detail::ContentInsetRoot`）为命中与派发起点，事件坐标为**整幅表面逻辑坐标**（与 `content_inset()` 同域）；下沉平移与绘制共用壳的 `PaddingEdges` 修饰，命中/绘制零漂移（[`07-environment-modifier.md`](07-environment-modifier.md) §3.1）。无头宿主或首个 `present_root` 之前（壳未收养应用根，`hit_root_ready()` 为 false）回退场景根——后者与历史「先派发后渲染」语义等价。键盘/文本/IME 组合事件不经命中链，恒走场景根。
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
| 提升 z 序 | `WindowHost::raise()` / `Surface::raise()` | Win32 `BringWindowToTop`；GLFW 以 `glfwShowWindow` 近似（无独立 API）；Wasm 见 §2.4「已落地（z 序与页面标题）」 |
| 激活窗口 | `WindowHost::focus_window()` / `Surface::focus_window()` | Win32 `SetForegroundWindow` + `SetFocus`；GLFW `glfwFocusWindow`；Wasm 只取键盘路由、**不置顶**（申报偏差，见 §2.4） |
| 所在显示器 | `Surface::display_id()` / `WindowHost::display_id()` | Win32 `MonitorFromWindow`（HMONITOR 句柄值，与 `app::Display::id` 同源）；未知 -1 |
| 迁移到显示器 | `WindowHost::move_to_display(id)` | 转发既有 `app::move_window_to_display`（居中到目标工作区） |
| DPI 变化 | `Surface::set_scale_change_handler()` → `WindowHost::on_scale_changed()` | Win32 `WM_DPICHANGED`（取 wParam 高位的建议 DPI）→ 更新 `scale` 并强制整帧重排重绘 |

**已知后端限制（多窗口）**

- **Wasm**：**已落地（多窗口事件路由）**——键盘/resize 走「document/window 级单一分发器」：键盘按当前焦点 Surface 路由（鼠标按下/`focus_window()`/新建窗口均接管路由，焦点窗口销毁回落到存活实例），resize 广播全部实例各自刷新 CSS 尺寸（经 `Window::size()` 帧环检测触发重排）。可打印字符键在 KeyDown 的 `KeyEvent` 之外**另发** `TextInputEvent`（`key` 名折算，与 X11 路同口径；`keyCode` 数字码与库 `KeyCode` 枚举错位，不得直填）。
  - **已落地（z 序与页面标题）**：
    - `raise()` = 把本 canvas `appendChild` 到其父节点末子位。浏览器没有 z 序 API，**DOM 顺序即层叠顺序**（同 `z-index` 下末位压前位），故移动节点就是置顶。同一元素 `appendChild` 是**移动**而非重建——节点身份保留，已注册的鼠标回调、Canvas 2D 上下文与按 canvas id 索引的 ARIA 镜像容器全部随迁不失效，无需重注册。不改变激活状态（与基类契约一致）。
    - `document.title` 是**页面单值**而每窗口各有一份标题，故按桌面口径折算为「焦点窗口的标题即页面标题」：`set_title` 先写本窗口缓存（`title()` 为多窗口只读观测口），仅当自己是焦点窗口时才落 DOM；非焦点窗口改名只进缓存。焦点**易主即重播**缓存，接管点共四处（构造即接管、`focus_window()`、鼠标按下、焦点窗口析构回落），共用 `take_focus()` 收口以防漏其一。从未声明过标题的窗口**不动** DOM（页面标题可能归宿主），`set_title("")` 属显式声明则如实写空。
    - ⚠️ **一处如实申报的契约偏差**：基类 `focus_window()` 的桌面语义是「置顶 + 取键盘焦点」，Wasm **只做后者**。「置顶」在此只能靠改写宿主 DOM 顺序实现，隐式重排别人家的节点属越权（正常流下画布会换位跳动），故层序变更只在宿主显式调 `raise()` 时发生。
  - 真机验收：`tools/verify/wasm_multiwin_live_probe.cpp` + 页面壳 `wasm_multiwin_shell.html` + CDP 驱动 `tools/verify/wasm_multiwin_cdp_drive.mjs`（从仓库根 `node tools/verify/wasm_multiwin_cdp_drive.mjs`，起本地 http 服务承载 wasm（`file://` 拒绝流式编译）并派发真实点击/按键/`Browser.setWindowBounds`，十项判据全绿即退出码 0）：初始路由 / 双向切换 / 字符落字 / Enter 提交 / 反向不污染 / resize 广播 / 还原，加 z 序两判据（`raise` 前后 **DOM 末位**与**交叠区 `elementFromPoint` 命中者**双证同步——只看字符串可被 z-index 作弊，故两路都要变）与标题四判据（非焦点改名不上页 / `focus_window()` 易主重播 / `raise` 不改标题 / 关窗后标题 = 新焦点窗口缓存即无幽灵标题）。探针的三个观测通道分工即契约：`window.__mwState` 由 Wasm 每帧发布状态，`window.__mwCmd` 由驱动 JS 注入命令（`raise*`/`focus*`/`titleX:<t>`/`closeB`），`document.title` **只测不发**——它正是被测对象，探针自发布会让标题判据自证。
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
| `WindowGeometry{origin, size, mode, display_id}` | 坐标与尺寸为**屏幕物理像素**（与 `app::Display` 同源）——只有物理坐标跨 DPI 稳定；恢复时按当前 `scale_factor` 折回逻辑 dp。**该 `scale_factor` 与宿主内 dp ↔ 物理换算同源**（Win32 侧已收敛为 `Impl::scale` 单点真值，`scale_factor()` 读之；见 `03-layout-render.md` DPI 感知条）。**Linux 三后端（X11 / Wayland / GLFW）的缩放变化上报仍未实现**，故其 `scale_factor` 在改系统缩放或跨屏后不会自动更新，持久化恢复暂按旧值折算 |
| `save_window_geometry` / `load_window_geometry` | 读写；另有 `Preferences::Group` 重载用于**窗口组**：`prefs.group("windows")` + 各窗口键 |
| `is_window_geometry_usable(g, displays)` | 判据：尺寸为正，且与某显示器工作区**有交集**（部分越界算可用，多屏拼接/任务栏遮挡下不应拒绝恢复） |

- **时机**：窗口**打开时自动恢复**（`register_host` 与 `set_window_geometry_store` 两处入口）、**关闭时自动保存**（帧末回收阶段的收集步骤——包含因「保留最后一个宿主」而未真正销毁的单窗口场景，否则「关掉唯一窗口后下次启动恢复」永不生效；`WindowHost::geometry_saved()` 保证只写一次）。
- **兜底**：键缺失 / JSON 格式错误 / 几何不可用（显示器被拔除、分辨率变小导致完全落在屏外）→ 一律回退默认布局。**格式错误与不可用分两类 WARN**，便于区分「存储被破坏」与「显示器变了」。
- 本类不做隐式 I/O：落盘由调用方 `Preferences::flush()` 决定。

**统一帧循环**（`Application::run`，不再委托 `Window::run`）：

```
drain_posted → pump_all_once → on_frame → 逐宿主 tick → 共享 anim/sched tick → 逐宿主 render_frame → reap_closed → pump_deferred_work → wait_once
```

- **pump 一次还是 N 次**：由 `Surface::pumps_thread_queue()` 决定。Win32（`PeekMessage(nullptr,…)`）与 GLFW（`glfwPollEvents()`）是线程/进程级共享队列，一次 pump 即抽干全部窗口消息并按 HWND 路由，故每帧只 pump 一次；X11/Wayland/Wasm 为 per-surface 队列，逐个 pump。
- **等待聚合**：每帧只等待一次，取各宿主 `decide_wait` 与最近超时看守期限的**最小正值**（任一为 `0` 则不等待；全为「无限」才无限等待）。优先交由 `waits_thread_queue()==true` 的后端（Win32/GLFW）执行；无此类后端时（X11/Wayland）封顶 8ms 轮询，避免其余窗口饥饿。
- **回收时机**：`reap_closed()` 在**帧末**执行（不在事件派发栈内销毁宿主），并保留最后一个宿主，使 `scene()`/`window()` 等访问器在 `run()` 结束后仍可用。
- **帧尾兜底推进 `pump_deferred_work()`**（`reap_closed` 之后、`wait_once` 之前）：排空 deferred `ThreadPool` 队列 + 扫描 `with_timeout` 到期看守。**两步同挂帧循环而非上屏路径**是刻意的——空闲帧被脏区决策整段跳过就没有 `present()`，挂那里等于把续体与超时一起饿死（同 WASM ARIA 桥自驱拍的教训）。有后台线程的构建下二者恒为空操作。
- **跨线程回投**：`Task::set_main_poster` 唤醒**全部**窗口的等待通道，避免回投工作只唤醒其中一个窗口而延迟执行。

**帧统计归属**：单窗口刻意保留写入进程级单例 `FrameStats::instance()`（既有 `PerfOverlay`、基准工具与性能集成测试直读之，行为零变化）；登记第二个窗口起，各宿主切到自有实例（`WindowHost::own_frame_stats()`）。`PerfOverlay` 经 `Application::set_overlay` 自动绑定到主窗口统计，其他窗口的叠加层请显式 `bind_frame_stats(&host->frame_stats())`。

---

## 3 Window 与帧循环

### 3.1 帧循环

帧循环由 **`Application::run()` 统一驱动**（多窗口语义见 §2.4）：每帧 pump 事件 → 集中派发 → `tick` → 逐窗口 `present_root`（脏区决策）→ `present()`。`Window::run(on_frame, max_frames)` **保留**供单窗口低阶调用方（自拼帧循环、测试、demo 直驱）使用；多窗口请走 `Application::run`。所有构建、事件、重绘都在 UI 线程（单线程 UI，见 [`01-core.md`](01-core.md) §8.1）。

> ⚠ **`Window::run` 不含「逐宿主 tick」这一步**：`Widget::tick`（长按阈值、甩动惯性、`VideoPlayer` 播放时钟等一切每帧计时）只在 `Application::run` 的帧序里被驱动。低阶自拼循环若承载这类控件，须在 `on_frame` 回调内自行 `root.widget().tick(now)`，并把帧等待的 `anim_active` 取作「本帧实际发生了渲染」（`!Window::is_idle_frame()`），否则控件恒停在首帧、且首帧后即陷入无限深睡。`examples/demos/demo_common.h` 的 `run_demo` 已按此接线。

### 3.2 脏区追踪（默认开启）

`Window::present_root` 按「绘制脏 `DirtyRegionTracker` / 布局脏 `layout_dirty_` / 尺寸变化 / 根变化」四要素决策本帧：

| 情况 | 行为 |
|:---|:---|
| 四者全否且 HUD 叠加层未到期 | **整帧跳过**（idle 零开销，上帧画面仍有效，返回 `true` 不重绘） |
| 四者全否但 HUD 叠加层到期（软件路径） | **HUD-only 帧**：不重排不重绘树，只把新 HUD 合成到保留的主缓冲再全量上屏；仍记 idle（见下） |
| 仅绘制脏（文本选区高亮、主题切换、局部 `State` 文本变更） | **跳过整树 `layout`**，复用已缓存 `Node` 几何直接 `paint` |
| 布局脏或尺寸变化 | `layout + paint` |
| 根变化（`Navigator` 切换页面、`run_demo` 换树） | 强制整体重绘，避免停留旧页面 |

- `mark_needs_layout()` 置「布局脏 + 绘制脏」，`mark_needs_paint()` 仅置「绘制脏」。
- **HUD-only 帧是「整帧跳过」的唯一例外**：叠加层可见且到期时，帧循环唤醒一帧只刷 HUD，否则脏决策会在 HUD 合成段之前直接 `return`，屏幕上的读数永久停在最后一次活跃帧上（详见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md) §10.2）。该帧**仍记 idle**（未渲染树 ⇒ 不进 FPS / 帧时间统计，避免 HUD 用自己的刷新抬高它自己显示的帧率），并由 `WindowHost::decide_wait` 把 `Window::hud_refresh_due_ms()` 并入唤醒截止时间，使其固定在约 2Hz 而非退化成忙轮询。GPU 帧路径的软件缓冲只有底色（控件像素在 GPU 侧），裸 `present()` 会上屏一屏空白，故该路径标全脏、退回完整重渲染。
- `Widget::on_dirty` 是控件**自身**挂载的 `std::function<void(bool)>` 回调（`true` = 含布局脏），用于直接持有某控件、单独观察其标脏的场景；它**不参与树级脏传播**。`install_dirty_sink` 仅在控件树**根节点**安装**单一** `on_subtree_dirty`（`void(Widget &, bool)`）汇聚点，而非逐节点整树接线。
- `enable_dirty_tracking(bool)` 可关闭，回到每帧全量重绘的历史行为；`force_full_redraw()` 供动画 / 视频 / 定时器持续重绘或外部环境突变时强制下一帧全绘。
- **首帧 `first_frame_ = true` 强制全绘**；重新挂载 / 根变化时自动 `mount` 接线响应式订阅，使 `State` 与修饰变更能标脏重绘。

**仅绘制脏帧的处理**：跳过 `begin_frame` 保留上帧帧缓冲（部分后端的 `begin_frame` 会清零整帧，直接 begin 会使裁剪外黑屏），先 `Painter::clear_rect(merged_bounds)` 把脏矩形并界重置为新帧零基底，再 `push_clip(merged_bounds)` 裁剪重绘。裁剪内从零基底按原序重新合成、裁剪外沿用上帧像素，两侧均与整帧重绘**逐位一致**。`install_dirty_sink` 回调标记控件本次标脏应覆盖的绝对几何（`Widget::dirty_bounds()` = 最近一次 `paint` 的自身盒 ∪ 画在盒外的装饰），使裁剪命中精确区域。

- **为何不是 `paint_bounds()`**：基类统一焦点环刻意画在控件盒**外** 2–4 dp（见 [`05-event-navigation.md`](05-event-navigation.md) §4.4）。只按自身盒标脏会让该环带落在裁剪之外，两个方向同时出错：获焦帧环画不上屏（偶发靠同帧其它控件的脏区扩张才出现），失焦、焦点到达方式变更（键盘有环 → 指针点击隐环）或尺寸变化帧上一帧的环无人重绘而残留成「盒外一圈环色」。外扩判据与绘制分支共用 `Widget::focus_ring_shown()`，另加「上一帧画过环」（`painted_focus_ring_`）保证消除方向也进裁剪。
- **第二条汇聚路径**：`Scroll::on_descendant_dirty` 把后代的 `dirty_bounds()`（内容坐标系）并入离屏缓冲脏带，同一契约；漏了会把残留固化在离屏缓冲里随视口反复上屏。

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

**`FrameStats` 观测**：`record_wait(double)`、`wakeup_count()`、`wakeups_per_sec()`、`sleep_ratio()`。停帧状态：`record_idle(double dt)`（累加空闲时长）、`is_stale()` / `stale_duration_ms()`（详见 `ARCHITECTURE.md` §10.1 的停帧陈旧语义）。

### 3.4 Win32 上屏与系统重绘

**上屏性能契约**：`Win32Surface::present()` 经常驻 BGRA（GDI 原生序）DIB section + `BitBlt` 上屏（全量帧整幅 RGBA→BGRA swizzle，脏区帧仅 swizzle + blit 脏矩形），**禁止退回 RGBA 掩码（`BI_BITFIELDS`）直推**——非原生掩码会迫使 GDI 逐像素慢速转换。`WM_SIZE` 同步重渲染后 `ValidateRect`，免去紧跟 `WM_PAINT` 的重复全量上屏。

**系统重绘处理**：窗口类背景刷 `wc.hbrBackground` 用浅灰实心刷 `RGB(245,245,247)`（而非默认黑色擦除）；`wnd_proc` 处理 `WM_PAINT`，在系统要求重绘时立即 `present()` 当前已就绪帧缓冲。

**最大化白闪处理**：`Surface` 提供 `set_present_request` 回调通道（默认空实现），`Window` 构造时把该回调接为「对当前缓存根再渲染一帧」；`Win32Surface` 的 `WM_SIZE` / `WM_PAINT` 在几何变化当下同步调用该回调，使离屏缓冲在 DWM 合成前已为新尺寸真实内容。浅灰刷保留作兜底。`present_count()` 观测器供测试验证「WM_SIZE 触发了同步重渲染」，**与 `Surface::frame_count()` 无关**：后者是「已呈现帧数」，`Win32Surface::present()` 每真正上屏一帧自增一次（帧循环出帧计入，几何未变也计入），由 `itest_win32_present` 锁死两个计数器的独立性（逐帧 `present()` 使 `frame_count()` +1 而 `present_count()` 不动）。此口径曾出错：`frame_count()` 一度直接转发宿主的同步重渲染计数，导致持续出帧的窗口恒报 0。

**系统重绘 × GPU 帧路径**（`Window::evaluate_dirty_plan` 的 `system_redraw_` 分支）：该请求落在「无脏、无布局脏、尺寸未变」的 idle 判定时，软件后端只需全量 blit 兜底（`set_present_dirty({})` → `present()`，GPU 无关）；但 **GPU 栅格生效期间** `present()` 上屏的是 `Painter` 软件缓冲，而该缓冲在 GPU 模式下只铺底色、从不含控件像素——裸 `present()` 等于闪一屏空白（白闪缺陷的真因）。故此时改为 `dirty_.mark_all()` 落回正常渲染决策，走完整的「重录帧 DL → `replay` → `sink.end_frame`」；`is_full` 已保证无裁剪、全量上屏。已永久回退（`gpu_fallback_`）的后端维持裸 `present()` 兜底。观测签名：各 wgpu 宿主的 `software_present_count()` 在 GPU 生效期间恒 0（`utest_window` 两例分别锁「GPU 生效时重渲染而非裸 present」与「回退后维持裸 blit」）。

**关窗语义（多窗口）**：`WM_DESTROY` 只置位**本窗口**的 `should_close`，**不再** `PostQuitMessage(0)`——`WM_QUIT` 是**线程级**的，任一窗口销毁都投递它，会让同线程内所有窗口（乃至之后新建的窗口）在首次 poll 时被误判为「已请求关闭」，这是「关一个窗口整个应用退出」与「顺序创建多窗口立即退出」的根因。是否退出帧循环改由 `Application` 的 `ExitPolicy` 决定（§2.4）；外部/宿主代码主动 `PostQuitMessage` 时，`poll_platform_events` 仍按历史行为把 `WM_QUIT` 折算为关闭请求。

### 3.5 硬件加速上屏偏好

强类型枚举 `RendererPreference{ Auto, Software, GpuD3D11 }` 统一控制**上屏后端**（仅影响「像素如何上屏」，绘制仍由软件 `Painter` 完成）：

| 取值 | 行为 |
|:---|:---|
| `Auto` | 编译了 `AURORA_BACKEND_D3D11` 且设备可用则走 D3D11 GPU 上屏，否则静默回退 Win32/GDI（`AURORA_LOG_INFO`） |
| `Software` | 强制 GDI |
| `GpuD3D11` | 强制 D3D11；未编译或设备创建失败返回 `make_error(ErrorCode::RendererUnavailable, ...)`（slug `renderer-unavailable`），**不静默降级** |

`D3D11Surface` 支持 device-lost 恢复（present 报 `DXGI_ERROR_DEVICE_REMOVED` / `RESET` → 下次 `poll_platform_events` 重建 device / swapchain + 全量重渲染）、`set_vsync(bool)` 与 `paces_frames()`（`D3D11Options.vsync` 默认 `true`）。

### 3.6 窗口可见性策略

`WindowVisibility{ Normal, NoActivate, Hidden }` 是**生命周期选项**（非 `WindowStyleOptions`：样式描述
「窗口长什么样」，可见性描述「窗口是否进入用户视野」），经 `WindowOptions::visibility` 在建窗时下发，
默认 `Normal`（行为与既往完全一致）。

| 档位 | 语义 | 典型用途 |
|:---|:---|:---|
| `Normal` | 正常显示并激活 | 普通应用窗口（默认） |
| `NoActivate` | 显示但不激活：不抢焦点、不打断用户当前前台窗口 | 通知 / 悬浮提示 / 辅助面板 |
| `Hidden` | 不显示：窗口不进入用户视野，但渲染与像素读回照常工作 | 无人值守自动化（E2E）、预热与后台渲染 |

关键契约：**隐藏窗口仍可渲染、可读回像素、可接收框架合成输入**——`Hidden` 只切断「进入用户视野」
这一步，不切断绘制与上屏管线。枚举定义在 `include/aurora/window/surface.h`，枚举器显式赋值且无条件
出现（不随 `AURORA_BACKEND_*` 裁剪，稳定性契约同 `SurfaceKind`）；各宿主的具体映射（`SW_SHOWNA` /
`SW_HIDE` / `GLFW_VISIBLE` / X11 是否 `XMapWindow` / Wayland 是否 attach-commit）见
[`03-layout-render.md`](03-layout-render.md) §8.3。

参数在**建窗时**下发而非「先可见后隐藏」，避免一帧闪烁；`NoActivate` 在 X11 / Wayland 无对应请求
（聚焦由 WM / 合成器策略决定，Aurora 从不主动 `XSetInputFocus`），与 `Normal` 同路。

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

`PlatformCapabilities` 字段：`multitouch`、`high_frequency_pointer`、`desktop`、`mobile`、`system_drag_in`、`system_drag_out`。

后两位表「**机制是否存在**」：支持接收系统拖入 / 支持发起系统拖出。**当前恒为 false**——系统拖放后端尚未接线，能力位不预先声称支持（接线后按各后端实测翻位，并由单测锚定）。能力位与运行期失败**不互相替代**：机制存在但会话建立失败、源中断等仍走 `Result` 错误（`system-drag-*` 码，`retryable = false`，机制缺失是稳态）。

```cpp
const au::Platform p = au::platform();
if (p.is_mobile() && p.capabilities().multitouch) { /* 启用多指手势 */ }
```

**OS 明暗偏好查询**：`Platform::color_scheme`（`ColorScheme{Unspecified, Light, Dark}`）承载 OS 档位快照，**须由调用方经 `os_color_scheme()`（`window/platform.h`）显式填充**——读偏好有文件 / 注册表访问副作用，`platform()` 不代做。各平台实现与局限见 `os_color_scheme()` 函数头：Linux 为 GTK 配置文件启发式；Win32（`AppsUseLightTheme`）与 macOS（`NSAppearance`）为**契约注记，未在本仓库无头构建内编译验证**。档位用于明暗三级优先链的最低层（见 [`07-environment-modifier.md`](07-environment-modifier.md) §5.1）。

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

### 6.3 无障碍桥的激活与销毁

**惰性激活。** 桥不是窗口创建时构建的，而是**首个平台查询到达**时才构造（Win32 = 首个 `WM_GETOBJECT(lParam == UiaRootObjectId)`），并在此时把 `AccessibilitySettings::screen_reader_active` 回填为 `true`（进程级设置，其余字段保留）。未激活 = 语义树零构建、零事件，即无读屏在线时无障碍路径完全不进热路径。桥的构造点即挂接点：语义树根由宿主 `Win32Host::Impl` 承接（`set_accessibility_root`，每帧在 `paint` 之后由 `Window::present_root` 注入），桥构造后由宿主补喂已记录的根。

**事件路由（保持单槽契约）。** `accessibility.h` 的事件处理器仍是**进程级单槽**（宿主用），桥不占用它：桥经进程级 `a11y::ProviderRegistry` 注册，事件到达时广播给全部**已激活**桥，桥内按 `id → Widget*` 映射判定归属、无关即忽略（多窗口规模下广播成本可忽略；不为桥把单槽改多播）。故「宿主处理器 + 桥」二者同时在场、安装顺序无关。

**销毁顺序（两条硬不变量，均为实机崩溃的修复结论）。** 关闭窗口时 `Win32Host::Impl` 先 `disconnect_all()`（全量 `UiaDisconnectProvider`）再析构桥（`deactivate()` 与 `disconnect_all()` 等价且幂等，并在其中**从注册表注销**——否则进程级广播会在已析构的桥上调用 `is_active()`）。另有一条与窗口无关的路径：桥缓存的语义树根是裸指针，**根控件销毁**时经单源通道 `notify_accessibility_widget_destroying()`（出自 `Node::~Node()`）立即切断根与快照。两条路径的成因、机制与不变量见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md) §8.5。

**多窗口。** 桥与窗口一一对应（`Win32Host::Impl` 唯一持有），多窗口即多个桥同时注册；`screen_reader_active` 是**进程级**设置，任一桥激活即置 `true`、任一桥去激活即试置 `false`（多窗口下复位语义为 best-effort，见 §6.2 同类启发式约定）。

**线程模型（provider 回调一律回投主人线程）。** UIA 把 in-proc provider 的**每一次**回调投递在 `UIAutomationCore.dll` 的 COM/RPC 线程上（激活时初始化 STA 套间只保证公寓模型，并不保证那就是 UI 线程），而 widget 树、`snap_`、`id_by_widget_`、`providers_` 只属于**主人线程**（= 构造桥并安装 `main_poster` 帧循环的那条）。故 `Win32UiaBridge` 把访问面劈成两半：

- **回投 + 按值副本**（provider 侧唯可用形）：`eval_on_main` / `eval_widget_on_main` 派生出的 `snapshot_copy`（属性与几何读的都是副本）、`navigate_target`、`has_scrollable_ancestor`、`hit_test_id`、`focused_id`、`visible_box`、`provider_for`、`root_provider`，以及全部动作（`Invoke` / `Toggle` / `SetValue` / `Scroll` / `ScrollIntoView` / `SetFocus`）与文本面（`ITextRangeProvider` / `ITextProvider` 的每次 `accessibility_text` / `accessibility_char_bounds` / 选区读写）。跨线程只传值，悬垂读由构造排除。
- **主人线程裸口**（provider 侧禁用）：`sync_if_dirty` / `rebuild` / `find_node` / `snapshot()` / `id_of` / `mark_dirty` / `queue_*`。拉取式重建会整体换掉 `snap_`，任何指向它的指针出不了主人线程。
- **就地执行的两条合法路径**（都不入队、不等待，代价与回投改造之前一致）：调用线程本就是主人线程（`WM_GETOBJECT`、帧循环、`UiaDisconnectProvider` 的同步重入），或进程级 `main_poster` 未安装（无头 / 单测，与 `Task::post_to_main` 的无回投器回退同语义）。
- **限时等待 + 降级**：读路径预算 `AURORA_UI_READ_TIMEOUT_MS`（250ms）、动作路径 `AURORA_UI_ACTION_TIMEOUT_MS`（500ms），显著小于读屏客户端自身的秒级超时。超时即返回零值，provider 侧映射为 `UIA_E_ELEMENTNOTAVAILABLE`；「控件没有这项语义」仍回 `UIA_E_NOTSUPPORTED`，两者是不同判据，故回投结果用三态承载而非单一 `optional`。超时路径**刻意不打诊断**（`Diagnostics` 的收集器是主人线程专属），可观测性由客户端看到的错误码承担。
- **两道闸**（队列项真正执行前逐条过）：`abandoned` —— 调用方已超时返回，则晚到的项绝不补做用户动作（Invoke/SetValue 不能事后发生）；`alive_` —— 桥已析构，则闭包绝不解引用本桥。`wait_on_main` 入队后只碰局部的 promise/future，不再访问 `this`，故桥与等待方是两条独立时间线。`rtl_` 与 `listener_count_` 因 RPC 线程也会读/写而改为原子量。

回归面见 `tests/unit/utest_win32_ua_marshal`（换线程执行 / 两条就地路径 / 超时零值且丢弃晚到项 / 桥析构丢弃在途项 / provider 读经回投）。

**滚动量 → UIA `IScrollProvider` 映射（公式单一真源）。** `get_VerticalViewSize` 的 UIA 语义是「可见内容占全部内容的百分比」，唯一真源是平台中立纯函数 `core::compute_vertical_view_size(range)`（`range.viewport / range.content × 100`，夹到 `[0,100]`）；`content <= 0` 或 `viewport <= 0` 即「无内容/无视口」报 100；无任何滚动语义的控件由桥侧直接报 100（恒「全部可见」，不反推 span）。两量 `viewport`/`content` 由 `Widget::accessibility_scroll()` 的覆写方填：纵向语义下 `viewport` = 视口高、`content` = 内容总高。`LazyRow` 等横轴专用控件纵轴不可滚，报 `viewport = content` ⇒ 纵轴 100% 可见。`min`/`max`/`position` 仅用于滚动位置（`get_VerticalScrollPercent`），与 `VerticalViewSize` 的「占比」语义相互独立——`content - viewport != max - min` 时仍按 `viewport/content` 算得有几何意义的百分比。此前的反向缺陷（`max/(max+1)×100` 把「可滚跨度」误当「可见占比」，内容越大反而越接近 100%）已修复，验证见 `utest_accessibility` 的 `vertical_view_size_*` 三例 + `utest_scroll` / `utest_lazy_list` 各一例（三平台全绿，变异自证：仅把公式换回 `max/(max+1)` ⇒ 占比判据转红）。

`UIAutomationCore.dll` 运行时动态加载，缺库或必要导出缺失即整桥降级 no-op + 一次 `Diagnostics::warn`（无链接期依赖、无编译期裁剪开关）。

### 6.4 Linux 无障碍桥（AT-SPI2）

**分层与 Win32 UIA 同构**，翻译目标换成 AT-SPI2 D-Bus 协议：语义树快照（平台中立）→ 中立折算层（`src/aurora/window/detail/atspi_protocol.{h,cpp}`：`AtspiModel` 把 `a11y::TreeSnapshot` 投影为 id 寻址的 AT-SPI 对象表，角色/状态/接口/几何/文本偏移全部在此折算，**零 D-Bus 依赖**，无头单测 `utest_atspi_protocol` 全覆盖）→ 传输桥（`src/aurora/window/detail/atspi_bridge.{h,cpp}`：运行时 `dlopen("libdbus-1.so.3")` + 对象注册表 + `Socket.Embed` 握手，实现 `a11y::Provider`）。编译门控 = `AURORA_PLATFORM_LINUX ∧ (AURORA_BACKEND_X11 ∨ AURORA_BACKEND_WAYLAND)`；libdbus 缺失、无会话总线、`org.a11y.Bus` 不可达或 **`NO_AT_BRIDGE=1`**（GNOME 惯例显式免提）⇒ `create()` 恒返回 `nullptr`，整桥静默降级。总线地址可经 **`AT_SPI_BUS_ADDRESS`** 直给（跳过 `org.a11y.Bus.GetAddress` 查询）。

**激活模型与 Win32 不同：宿主侧一次性尝试，而非平台查询驱动。** X11/Wayland 后端没有 `WM_GETOBJECT` 这类「读屏在线才出现」的查询信号（总线订阅对 app 不可见），故桥在**首个 `set_accessibility_root` 注入**时构造（`atspi_attempted` 一次性置位，失败 = 永久降级不再重试）；构造即向注册表 `Embed` 请求挂接。无读屏桌面下代价为一次总线握手 + 惰性方法派发；建树同步点 `sync_point()` 由「方法派发」与「帧循环 dirty」双侧驱动（见下文事件通道），首帧之前仍不构建语义树。

**帧循环集成。** 桥暴露 `poll_watches()`（libdbus 传输 unix socket fd + 读/写_interest），X11 `wait_events` / Wayland 事件等待把这些 fd 一并 `poll`，fd 就绪即 `pump()` 读入并派发 AT-SPI 方法调用（应答经同一 fd 写出）。单线程 UI 内无额外线程；`pump` 只消化已排队消息（`dispatch` 返回 `DATA_REMAINS` 才继续）。

**线格式契约（libatspi 2.60 客户端实测锁定，WSLg 真机验收）**：状态集 = 恰两枚 uint32 位掩码的 `"au"`（`states[1]<<32|states[0]` 拼合；计数 + 枚举列表会被整集丢弃）；`Component.GetExtents` 应答为结构体 `(iiii)`（`GetPosition`/`GetSize` 平铺）；Action 计数走 `Properties.Get("Action","NActions")`（`"i"`），逐动作 `GetName/GetLocalizedName/GetDescription/GetKeyBinding`（`"i=>s"`）与 `DoAction`（`"i=>b"`）；`Cache.GetItems` 行签名 `a((so)(so)(so)iiassusau)`，数组元素签名须带外层括号；`Component.GrabFocus` 回包 = **boolean**（回空签被客户端判失败），语义为**尽力而为的受理**：真值表示目标活控件存在、动作通道已装配且 `Focus` 动作已派发，**不承诺焦点已落地**（折算单点在 `AtspiModel::grab_focus`，走 `env_.perform` 动作通道并刻意丢弃其返回值；`id → 可写控件` 的解引用与去 const 也只发生在 `AtspiModel::widget_of` 一处，桥侧不得自行 `const_cast`——快照指针的常量性仅来自 `flatten_snapshot` 的只读遍历签名，控件对象本身由树持有且非 const）。关系集、叠层（`GetLayer` 恒 0）、键绑定为**如实申报的空位**，随后续增量补齐。

**推送式事件通道（与拉取式方法面相对）。** `pump()` 帧循环开头 `if (dirty) sync_point()`——事件不依赖有入站查询，批处理粒度 = 帧（dirty 仅由 `ProviderRegistry` 广播置位）。`sync_point()` 对前后两快照做 `a11y::diff_snapshots`，按上游 `atk-adaptor/event.c` 的形发放信号：interface = 事件类（`org.a11y.atspi.Event.Object` / `.Focus`）、member = major 名 D-Bus 化（`StateChanged`/`PropertyChange`/`ChildrenChanged`/`Announcement`/`Focus`）、path = 源对象自身路径、**无 destination**（总线广播，应用侧无监听簿记——`RegisterEvent` 由 registryd 记账，客户端本地过滤）。对象事件体固定 `s i i v a{sv}` =（minor, detail1, detail2, variant, properties），libatspi 对非 `siiv(so)`/`siiva{sv}` 签名整条丢弃，故 properties 恒发空字典。映射表：`diff.added` → `Cache.AddAccessible`（完整行结构）先于 `children-changed:add`（源不可解析即丢事件，顺序是硬要求）；`diff.removed` 逆序（先子后父）→ `children-changed:remove` + `Cache.RemoveAccessible`（`(so)` 引用在 `model.sync` **之前**定格）；`FieldChange::Name/Value/Hint` → `property-change:accessible-{name,value,description}`（variant int32 0，新值靠客户端重读）；`FieldChange::State` → 逐位 `state-changed:<规范名>`（44 表 `atspi_state_name`，与 GLib 枚举 nick 同源，`utest_atspi_protocol.state_name_table_pins_event_minors` 钉死）；`focused_id` → `Event.Focus`；FRAME 是合成节点不入根树 diff ⇒ `set_window_title` 直发 `property-change:accessible-name`。播报 `on_announcement` 不经 diff 直发 `Announcement`（minor ""、detail1 = **POLITE(1)**、variant "s" 文本）；客户端类型串中 announcement **无尾冒号**（上游 `handle_event` 对空 minor 不追加）。申报空位续：`window:*` 事件、`text-changed`/`text-caret-moved`、Bounds/Range/Actions 类变化（无规范事件词汇，客户端靠重读）。

**拆除**：`on_widget_destroying` 对根置单向拆除门闩（pump 期查询只读旧快照），析构 = deactivate + 关连接，与 §6.3 两条硬不变量对齐。真实总线接线（含事件通道）由 `tools/verify/atspi_live_probe.cpp`（目标 `aurora_verify_atspi`）探针把关——外部 libatspi 客户端逐检查项比对（静态查询段 + 事件推送段），见 [`08-tooling.md`](08-tooling.md) §7.5；宿主生命周期面在 `utest_atspi_bridge` 的 `AURORA_LIVE_ATSPI=1` 选择加入用例中覆盖。

### 6.5 Wasm 无障碍桥（ARIA 镜像）

**形态：语义树 → 页面隐藏 DOM 镜像 → 浏览器原生 ARIA 支持消费。** 与前两桥「自研平台协议」不同，本桥不实现任何协议——把 `a11y::TreeSnapshot` 折算成 WAI-ARIA 1.2 镜像 DOM（role / `aria-*` / 文本内容），读屏经浏览器无障碍引擎（Chromium AX 树等）天然消费。分层与 §6.4 完全同构：中立折算层 `src/aurora/window/detail/aria_protocol.{h,cpp}`（零 Emscripten/平台头：role 映射、属性确定序、全量/ops/播报 JSON 全部在此，无头单测 `utest_aria_protocol` 全覆盖）+ 粘合桥 `include/aurora/window/wasm_aria.h` / `src/aurora/window/wasm_aria.cpp`（生命周期、镜像 DOM 应用与反向动作回灌，EM_JS/EM_ASM 独占此层）。编译门控 = `AURORA_PLATFORM_WASM ∧ AURORA_BACKEND_WASM`；桥类 `WasmAriaBridge` 实现 `a11y::Provider`，每窗口一实例，由 `WasmSurface::set_accessibility_root` 首帧构造。

**镜像容器。** 每窗口一个隐藏 div `#aurora-a11y-<canvas_id>`（`position:absolute` + `clip-path:inset(50%)` 视觉隐藏但**保留在可访问性树中**——`display:none` 会整树出局），容器 `role=group`；镜像元素 DOM id = `aurora-a11y-<runtime_id>`（`aria-activedescendant` 的 IDREF 目标），`data-aurora-id` 属性供寻址与观测。焦点 = 容器 `aria-activedescendant` + 元素 `data-aurora-focused="1"`；播报 = 容器内懒建的 `[data-aurora-live]` 子元素（`aria-live=polite` + `aria-atomic`，同文本重播先清空再经 `setTimeout(0)` 回写）。**无几何面**（如实申报）：镜像元素不带画布坐标，读屏按 DOM 顺序导航，不支持「点按位置探测」。

**激活（惰性激活的既定例外）。** 浏览器没有 `WM_GETOBJECT`/总线订阅那样的「读屏在线」探测信号（navigator 无读屏 API），故**首个 `set_root` 注入即激活**并置 `screen_reader_active = true`（启发式申报）；拉取式仍成立——只有 dirty（结构/字段事件、换根、动作回灌）才重投影并发载荷，静止页面零 DOM churn。多窗口下各桥独立镜像，`runtime_id` 进程内唯一 ⇒ 反向动作跨桥按 id 寻址。

**载荷协议。** 首发全量 `{"els":[...],"focus":N,"rtl":b}`（els 先序表），续发增量 `{"ops":[remove|add|move|update...],"focus":N}`——段序固定 remove → add（新快照先序）→ move → update（去重、add 者不重发），`focus` 为**绝对值**（新快照获焦者 id，0 = 无）。元素属性确定序与 role 映射表由 `utest_aria_protocol` 逐位钉死。

**自驱 rAF 拍与反向动作通道。** JS 侧在镜像元素挂 click/focus 监听，把 `(id, 动作位)` 写入页面级队列 `window.__auroraAriaQueue`；桥激活时自排一拍 `emscripten_request_animation_frame` 蹦床（`raf_tick`）：每拍 `sync_if_dirty → pump_actions → sync_if_dirty`（动作 `perform` 成功即补脏，回写的焦点/状态位**同拍收敛**）。**为何不挂 `present()` 帧尾**：静止页面没有脏帧就没有 present，反向通道会被饿死（CDP 真机验收实测坐实）；rAF 自驱保证任何帧序下动作延迟 ≤1 拍。蹦床先以 `live_bridges()` 成员性校验再解引用 userData（`Application::raf_owner_` 同语义的悬垂守卫），去激活/标签页隐藏时断链（隐藏页动作停摆与帧循环同语义，如实申报）。走**轮询队列**而非 `-sEXPORTED_FUNCTIONS` 导出，是为了零链接配置要求（消费者任何 Emscripten 设置下都成立；代价 = 动作下一拍生效 ≤16ms，读屏交互无感）。

**真机验收**：`tools/verify/wasm_aria_live_probe.cpp`（目标 `aurora_verify_wasm_aria`，浏览器产物不进聚合 `aurora_verify`）——无头 Edge + CDP 取证 20 项：`Accessibility.getFullAXTree` 证角色/名进真实无障碍树；JS 点击镜像元素证 Invoke/Toggle 闭环；`aria-live` 区文本证播报；`MutationObserver` 证首帧后仅子树级增量（零容器整铺重放）；`aria-activedescendant` 证焦点回写。

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

`Clipboard`（`app/clipboard.h`）是同址的一个类（文本 / 图像 / 富格式三族都在它上面，不另开门面类），全部静态接口**一律返回 `Result<...>`**，失败必机器可见（不再以空串 / 静默 no-op 冒充成功）：

| 接口 | 签名 | 说明 |
|:---|:---|:---|
| 写文本 | `set_text(const std::string &) -> Result<bool>` | Windows 经 `OpenClipboard` + `SetClipboardData(CF_UNICODETEXT)`；UTF-8 入参，平台按需转码 |
| 读文本 | `get_text() -> Result<std::string>` | 可读但无文本 → `Ok("")` |
| 写图像 | `set_image(const Image &) -> Result<bool>` | 仅 Windows 经 `CF_DIB`；其它平台 `GeneralNotSupported` |
| 读图像 | `get_image() -> Result<Image>` | 格式不存在 → `Ok(空 Image)`（`width == 0`） |

**失败口径**：

| 情形 | 错误码 | retryable |
|:---|:---|:---|
| 剪贴板打不开 / 被别的线程或进程占用 / 外壳工具（xclip、xsel、pbcopy）缺失 / 跨进程载荷非法 | `ClipboardAccessFailed` | ✅（占用是暂时态，可退避重试） |
| 已打开但载荷提交失败（`GlobalAlloc` / `GlobalLock` / `SetClipboardData`）/ 入参文本不是合法 UTF-8 | `ClipboardWriteFailed` | ❌ |
| 本平台无剪贴板实现 | `GeneralNotSupported` | ❌ |
| 图像尺寸超限或像素缓冲与维度不一致（调用方参数错） | `GeneralInvalidArgument` | ❌ |
| 本平台无该 MIME 的读写机制（富格式族） | `ClipboardFormatUnsupported` | ❌（机制缺失是稳态） |

- **空内容不是失败**：「可访问但当前没有文本/图像」是正常态，返回 `Ok`，不得与「读不到」混为一谈。
- **空写入是契约内 no-op**：空文本 / 空图像不触碰剪贴板、**保留既有内容**，返回 `Ok`。
- **Windows 写文本先转码后开剪贴板**：`MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` 在 `EmptyClipboard` **之前**校验，非法 UTF-8 那一笔被拒时不会把用户原本复制的内容清掉；同理 `SetClipboardData` 失败意味着所有权未转移，须 `GlobalFree` 归还，不泄漏句柄。
- 声明与迁移：四函数由「`void` / `std::string` 静默返回」改为 `Result`，属预览期破坏性收敛（semver 记录随版本收口批量落 `CHANGELOG.md`）；消费端把原 `std::string` 返回值改 `.value()` 即可保持旧行为，但**建议检查 `error()`**，否则又回到静默吞失败。

**富格式族（files / uri-list / HTML / 自定义 MIME）**：`available_formats()`、`set_payload(mime, bytes)` / `get_payload(mime)`、`set_files(paths)` / `get_files()`、`set_html(html)` / `get_html()`。要点：

- **MIME 词汇表单一真源**是 `app/mime_types.h` 的常量表（`AURORA_MIME_*`，命名空间作用域 `constexpr std::string_view`）：`text/plain`、`text/uri-list`、`image/png`、`text/html`、`aurora/widget`（应用内拖拽既有词汇），另附 `is_known_mime()`。剪贴板侧与**系统拖放**侧同引此表（拖放事件结构本身用裸 `std::string`，因为 `event/` 不依赖 `app/`；两侧取值一致由常量表保证），各处禁止就地硬编码 MIME 串。
- **`available_formats()` 如实反映此刻能读到什么**：平台路径上是各平台既有机制覆盖的格式（文本三平台恒在列、`image/png` 仅 Windows 在列），富格式待平台机制落地后才入列；memory 测试后端下反映实际写入的格式。**不因「API 已声明」就谎报可用**。
- **`text/uri-list` 编解码是纯逻辑**（`src/aurora/app/uri_list.h`，无平台依赖）：编码逐条路径产出 `file://` URI（空 authority 的第三个斜杠按需补出，空格等保留字符 `%XX` 大写十六进制），行以 CRLF 结尾；解码容错 CRLF / LF / CR 三种分隔符、忽略空行、`file://` 行百分号解码为本地路径并还原盘符形态（`file:///C:/x` → `C:/x`；POSIX 绝对路径的前导 `/` 保留）。**非 `file` scheme 的行（`http://` 等）不映射为路径、静默跳过**——`get_files()` 只返回可映射为本地路径的条目，完整原始行经 `get_payload(AURORA_MIME_URI_LIST)` 取得；全部条目被跳过时是 `Ok(空清单)`。`set_files` **只接受本地路径**（URL 类内容走 `set_payload`），与解码侧口径对称。
- **空口径沿旧约**：格式不存在 = `Ok` 空值（空内容不是失败）；空串 / 空清单 / 空字节写入 = no-op，**保留既有内容**；`mime` 为空串是调用方参数错（`GeneralInvalidArgument`）。
- **平台实现不在本批**：Win32 既有 text / image 两族零改动；富格式的平台后端（`CF_HDROP` / "HTML Format" / `RegisterClipboardFormat`、X11 / Wayland selection 目标协商、macOS `NSPasteboard`）随后续批次落地，落地前非测试路径按 `ClipboardFormatUnsupported` 报错。memory 测试后端已覆盖富格式往返。

**控件层（`TextInput` / `Text` / `RichTextEdit` 的 Ctrl+C/X/V）**：这些快捷键路径无 `Result` 出口（`on_key_event` 返回 `void`），故失败经 `Diagnostics::warn(msg, where, code)` 上报——桥接 Logger 且进 `Diagnostics::report()` 收集，机器可读，与 `widget.cpp` / `timer.h` 既有口径一致。同时快捷键语义随失败收紧：

- **Ctrl+X 只在复制成功后才删选区**：剪贴板写入失败时保留选区与内容（剪切 = 复制 + 删除，复制没成就不该半程提交造成内容丢失）。
- **Ctrl+V 读失败视为空剪贴板**：不插入任何内容，同时上报 `Diagnostics::warn`。
- 只读态的降级路径（Ctrl+C/Ctrl+X 仅复制、Ctrl+V 忽略）不变。

**测试注入点（test-only）**：`install_test_backend` / `reset_test_backend` / `remove_test_backend` 三个静态函数构成仓库私有测试设施（`tests/`）用于并行隔离的最小注入面——安装后 `set_text` / `get_text` / `set_image` / `get_image` 全部改走进程内 memory 后端，不触碰系统剪贴板。

- 声明常驻（消费端调用始终可编译）；实现体按 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏裁切，任一关闭（含 Release 下 DEBUG AUTO 自动关闭）时返回 `false` / no-op，平台行为不变。
- `install_test_backend()` 清空并激活后端、返回是否生效；`reset_test_backend()` 仅清内容；`remove_test_backend()` 卸载并恢复平台实现、返回是否确有后端被卸载。
- 编译开关语义见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) §4（`AURORA_ENABLE_TEST_HOOKS`）。

**Linux/macOS 实现（及为何测试不得断言系统剪贴板）**：非 Win32 平台经 `xsel`/`xclip`（macOS `pbcopy`/`pbpaste`）外壳进程中转（`popen`），并在作用域内把 `SIGPIPE` 置为忽略，避免工具缺失或无显示时子进程立即退出导致写已关闭管道把本进程杀掉。该路径的 selection 所有权是**异步**的（xsel 二次写入依赖既有守护进程交接、xclip 常驻等待读者），内容读回还受各工具的换行约定影响——因此**仓库测试禁止用系统剪贴板做内容断言**（既非确定，也不是 aurora 自身的契约）：`itest_text_selection` 的 Ctrl+C 复制用例改为先断言环境无关的选区范围，再在 `install_test_backend()` 成功时经进程内后端断言复制内容；双测试宏未开（Release）时仅跳过内容断言，选区/焦点/按键消费断言仍生效。

### 8.3 系统托盘

`SystemTray`（`app/system_tray.h`）在 Win32 经 `Shell_NotifyIcon` + 隐藏消息窗口实现，支持图标、气泡与激活回调 `on_activate`；非 Win32 为 no-op（仅记录 `last_balloon_message`）。

**回调 `lParam` 有两种编码，按注册版本解读**：图标以 `NIM_SETVERSION` 升到 `NOTIFYICON_VERSION_4` 后，回调消息的 `lParam` 是打包值——低 16 位才是事件（鼠标消息或 `NIN_*`），高 16 位是图标的 `uID`；未升级时 `lParam` 是整值事件。故 `on_activate` 与上下文菜单的触发判定必须先取版本再取位段（`src/aurora/app/detail/tray_events.h` 的 `classify_tray_callback`），按整值比较鼠标消息会让两条路径在真实 shell 下静默失效。一次真实左键对应 4 条回调（`NIN_POPUPOPEN`、`WM_LBUTTONDOWN`、`WM_LBUTTONUP`、`NIN_SELECT`），故版本 4 分支只认 `NIN_*` 三个事件、且须排除 `uID` 不匹配的回调，否则一次单击会重复触发。

### 8.4 菜单、快捷键、命令与显示

| 头文件 | 能力 |
|:---|:---|
| `app/menu.h` | 菜单（`MenuBar` 控件的数据层） |
| `app/shortcuts.h` | 快捷键注册 |
| `commands.h` | 命令模型与注册表（快捷键 / 菜单 / 命令面板的统一真源） |
| `widget/command_palette.h` | 命令面板（模态浮层，模糊检索并执行命令） |
| `app/os_hotkey.h` | **OS 级全局热键**（无焦点时也触发，见 §8.6） |
| `app/notification.h` | **跨平台系统通知**（Win32 气球 / XDG 桌面通知，见 §8.7） |
| `app/display.h` | 显示设备与 DPI 查询 |

**命令是唯一真源**：`CommandRegistry`（`commands.h`）持 `Command{ id, title, icon, category, action, default_binding, scope, enabled, when_label }`。`bind_shortcuts(ShortcutRegistry&)` 与 `to_menu_items()` 是它面向快捷键与菜单的两个**投影**，命令面板是第三个消费方；三者共用 `invoke(id)` 出口，故启用条件与空动作判定单点生效。启用条件为两段式：`enabled` 谓词承担运行期判定（空 = 恒启用），`when_label` 仅作展示 / 序列化标签（**不参与求值**，也不解析条件 DSL）。`to_json()` 产出 `{"commands":[…]}` 自描述信封供工具面枚举；`search()` 与工具面共用 `command_fuzzy_score()`，故 AI 检索与用户检索次序一致。

**菜单数据模型的单一真源（`MenuModel`）**：菜单数据此前与行为、渲染提示混装在同两个结构里——`MenuItem`（`app/menu.h`）装 `on_click` 行为 + 数据 + `shortcut_text` / `icon` 渲染提示，`Menu`（`widget/menu_bar.h`）装顶级菜单名与下拉项，二者被自绘 `MenuBar`、`Modifier` 层的 `ContextMenuNode` 与 `SystemTray` 三方共用。原生菜单桥（Win32 `HMENU` / macOS `NSMenu` / Linux appmenu）需要的不是「再抄一份菜单 API」，而是**同一个模型的多投影**（与 `CommandRegistry` 的「单一真源 + 快捷键 / 菜单 / 命令面板三投影」同款范式），因此提取纯数据模型：

- `MenuModel`（`app/menu.h`）只承载数据：`id`（稳定标识，可空）、`label`、`enabled`、`checkable`、`checked`、`shortcut_text`、`separator`、`children`。**不含** `on_click`（行为留在 `MenuItem`）与 `icon`（渲染提示留在 `MenuItem`）。顶级菜单与菜单项**共用一套模型**：顶级菜单标题落在 `label`、其下拉项落在 `children`。
- 投影函数（`app/menu.h` 声明）：`to_menu_model(const MenuItem &)`、`to_menu_model(const Menu &)`（`title` → `label`、`items` → `children` 递归）、`to_menu_models(const std::vector<Menu> &)`（菜单栏整体投影，顺序逐位保真）、`from_menu_model(const MenuModel &)`（回转，`on_click` / `icon` 为空——供原生桥按 `id` 反查命令后自行接线行为）。四者均为无副作用纯函数。
- **`id` 是原生桥的前置条件**：自绘路径靠指针身份路由点击即可，原生路径跨菜单重建只能靠稳定标识，故 `MenuItem` 与 `Menu` 各增一个 `id` 字段（默认空串，自绘路径忽略、零参与）。
- 自绘路径**零改动**：`MenuBar` / `ContextMenuNode` / `SystemTray` 的既有字段、构造与绘制命中逻辑一行不动，`MenuModel` 投影是新增加的消费路径。自绘与原生「二显一存」——同一份模型，任一时刻只有一条渲染路径生效。

**接线**：`Application::commands()` 返回注册表；`app.commands().bind_shortcuts(app.shortcuts())` 一行把默认快捷键接入。绑定为**显式**而非 `run()` 内自动执行——否则 `run()` 之后注册的命令会静默失效。`Application` 在 `dispatch*` 入口统一暴露「当前焦点管理器」（`current_focus_manager()`），快捷键动作与控件回调内同样可取到，模态弹层据此完成焦点陷阱。原「命令式逃生舱」`aurora::imperative::run_raw`（`imperative.h`）与命令系统无关。

**快捷键可重绑（宿主覆盖表的启动重放）**：框架侧为此提供两个入口，二者组合才闭合——`KeyCombo::from(text)` 把覆盖表里的组合键文本反解成 `KeyCombo`，`CommandRegistry::set_binding(id, combo)` 把反解结果回灌到该命令的 `default_binding`（`id` 未注册返回 `false`）。前者与 `to_string()` **逐字节往返**：可解析的形态就是 `to_string()` 输出的那批串（不额外支持别的写法，否则组合键的显示串规范就有两份），修饰次序**不敏感**（`"Shift+Ctrl+P"` 与 `"Ctrl+Shift+P"` 同解，产出仍走 `to_string()` 的规范序），畸形输入**一律**回 `std::nullopt` 而**不**回落成「修饰位 + 空主键」的假有效值——回落会把「这一条没配上」伪装成「配上了某个键」，宿主按覆盖表重放时该条目会静默消失且不留痕迹。后者**只改绑定**：除 `default_binding` 外不动任何字段（`title` / `icon` / `action` / `category` / `scope` / `enabled` 谓词 / `when_label`），也不改变注册次序——改绑定此前只有 `add(Command)` 一条路，而它是**整条覆盖**语义，宿主重述一遍就得把 `enabled` 谓词这类「取得到、复制不了语义」的字段抄全，漏抄即静默的能力丢失。已 `bind_shortcuts` 过时 `set_binding` 连带把该条在快捷键表里的绑定换成新组合键（先撤后建），使命令表与快捷键表两个投影不各持一份真值；尚未投影过则是纯数据变更，由下一次 `bind_shortcuts`（幂等重建）生效。宿主侧的处置口径是「逐条解析、该条失败只留痕并跳过、其余照旧」——一条畸形覆盖不得挡住启动。

**命令面板键位**：`CommandPalette` 打开时把自己的作用域压入 `FocusManager`（子树内**唯一**可聚焦控件是搜索框，故左右方向键仍落到搜索框做光标移动、上下方向键不引发焦点跳转）；Enter 经搜索框的提交回调执行选中项；Esc / ↑ / ↓ 经打开期临时注册的快捷键绑定接管（依赖注册表已 `bind_shortcuts`，未接线时这几键不可用，面板以 WARN 提示）。Space 只经文本输入落字，不触发执行。命令清单可经 `to_json()` 序列化并由 MCP 工具面枚举，见 [`08-tooling.md`](08-tooling.md) §7.1。

**`KeyCombo::matches` 的匹配语义：只比可按住的位**。`ModifierKey` 把「可按住的修饰位」（`Shift` / `Control` / `Alt` / `Meta`）与「键盘锁定态位」（`NumLock`）建模在**同一个字节**里，而四个后端都会在事件上盖章锁定位（Win32 读 `GetKeyState(VK_NUMLOCK)` 的 toggle 位、X11 折 `Mod2`、Wayland 查 `XKB_STATE_MODS_LOCKED`、GLFW 键盘路径查 `GLFW_KEY_NUM_LOCK`）。匹配前两侧都先与 `AURORA_MODIFIER_PRESSABLE_MASK` 取子集，锁定态位两侧一律屏蔽——否则 `NumLock` 开着时**任何**未登记该位的应用内快捷键恒不匹配，是整层静默失效（与焦点、作用域无关）。四个配套常量（`event.h`）：

| 常量 | 含义 | 纪律 |
|:---|:---|:---|
| `AURORA_MODIFIER_PRESSABLE_MASK` | 可按住的位并集（四个） | 判定「某修饰键是否按下」用单个位即可，无需本掩码；本掩码专用于**位集的整体比较** |
| `AURORA_MODIFIER_LOCK_MASK` | 键盘锁定态位的并集（当前只有 `NumLock`） | **新增锁定类位（`CapsLock` / `ScrollLock` 等）时必须并入本掩码**，否则该位会落进两掩码之外的缝隙、在按位比较中继续污染结果 |

两者应恒为 `ModifierKey` 全部已定义位的一个**无余划分**（并集全覆盖、交集为 0），该恒等式由 `utest_shortcuts` 的位集自证用例钉住——新增锁定位时那条用例会要求同步。

其余三条口径均为刻意决定，不得顺手改：

- **仍是「相等」而非「包含」**：`Ctrl+O` 不匹配 `Ctrl+Shift+O`。这是 Qt `QKeySequence` / WPF `KeyGesture` / Flutter `SingleActivator` 的共同语义；改成包含会让所有「更具体组合优先」的注册表失去区分度。
- **注册侧的锁定位不可表达**：`KeyCombo{Control | NumLock, K}` 里的锁定位被忽略（等价于 `Control+K`）。`matches` **刻意不加断言、不加日志、不返回 `Result`**——它在每条按键消息上被调用，热路径里的噪声比它想防的错更贵，该口径由 `matches` 的文档注释承担。`to_string()` 不列锁定位，与此恰好一致。
- **不引入 per-combo 的「要求 NumLock 开 / 关」字段**：当前无任何消费者需要，属扩大 API 预算。若将来需要，应另立字段并走一次独立的公共 API 变更。相应地**不新增匹配档位 / 枚举参数**，保持单一算式，避免同一字段出现两个真值源。

### 8.6 OS 级全局热键（无焦点时也触发）

`OsHotkeyRegistry`（`app/os_hotkey.h`）把键组合注册到**操作系统**，焦点在别的进程里同样生效。经 `Application::os_hotkeys()` 取用。

**与 `ShortcutRegistry` 的分工**（两者不重叠、也不互相接管）：

| | `ShortcutRegistry`（`app/shortcuts.h`） | `OsHotkeyRegistry` |
|:---|:---|:---|
| 作用域 | 应用内：需窗口有焦点、经键盘事件派发命中 | OS 级：系统范围内，应用无焦点也触发 |
| 键组合类型 | `KeyCombo`（复用同一类型） | `KeyCombo`（复用同一类型） |
| 失败语义 | 无冲突概念 | 被别的进程抢占 / 平台无后端 → `OsHotkeyRegisterFailed` |

`ShortcutScope::Global` 的语义**不变**：它仍是「应用内跨焦点作用域」的标记，不因本节新增而改写为 OS 级。

**触发时机 —— 排队到帧循环排空，不在消息泵内同步回调**：`WM_HOTKEY` 到达时只把命中的 ID 推进内部队列，由 `Application::step_frame()` 每帧调 `os_hotkeys().drain_pending()` 在主线程排空并调用动作。理由：动作可能重建页面 / 触发重排，在窗口过程帧内重入会把布局与绘制切到半途的状态。代价是最坏延迟一帧（约 16ms），对热键这类低频触发无感。

**平台矩阵**：

| 平台 | 实现 | 状态 |
|:---|:---|:---|
| Windows | `RegisterHotKey` + `WM_HOTKEY`，经共享的内部隐藏消息窗口 `src/aurora/app/detail/platform_shell_win32.{h,cpp}` 收消息 | 支持 |
| X11 | `xcb_grab_key` | 支持 |
| Wayland | 无全局热键协议 | 运行时降级：`enabled()==false`，`add()` 返回错误 |
| GLFW | 无对应能力 | 不支持 |
| macOS | — | 本轮不实现 |

**失败口径一律机器可见，不做静默 no-op**：主键无法映射到原生虚拟键、组合未指定主键、注册表内重复注册、OS 侧被抢占，统一为 `ErrorCode::OsHotkeyRegisterFailed`，`detail` 给出具体成因。`remove()` 对无效 / 非本表的句柄返回 `false`（不抛、不报错）。

**句柄**：`OsHotkeyHandle{std::uint32_t id}`，值语义，`id == 0` 恒为无效句柄，有效 ID 自 1 起。强类型而非裸整数，避免与 `ShortcutRegistry::add()` 返回的 `int` ID 混淆（传错类型编译期即报错）。

**测试注入**：`OsHotkeyRegistry::install_test_backend(bool)` 安装进程内 inert 后端，此后 `add`/`remove` 只走内存、不触碰 OS 热键接口。单测不得真的抢占系统热键（CI 机器上会干扰系统与别的用例），但又必须覆盖「平台不支持」的降级路径——那在受支持的平台上没有别的办法复现。该注入面受 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏裁切，Release 下自动失效。

### 8.7 跨平台系统通知

`NotificationCenter`（`app/notification.h`）发系统通知，`Application::notify()` / `Application::set_on_notification_activated()` 是其转发入口。

```cpp
app.notify(Notification{.title = "Build finished", .body = "42 targets", .tag = "build",
                        .urgency = NotificationUrgency::Normal, .timeout_ms = 5000});
app.set_on_notification_activated([](std::string tag) { focus_pane(tag); });
```

`Notification{title, body, tag, urgency, timeout_ms}`：`tag` 既是**去重 / 替换标识**（同一 tag 的后续通知在支持去重的服务器上替换前一条），也是**激活回调回传的键**——点击通知时回调收到的就是这个 tag。`timeout_ms == 0` 表示交给平台默认策略。

**平台矩阵**：

| 平台 | 实现 | 激活回调 |
|:---|:---|:---|
| Windows | `Shell_NotifyIconW` 按需添加隐藏托盘图标 + `NIF_INFO` 气球；超时 / 关闭 / 点击后 `NIM_DELETE` 撤掉图标 | 支持（`NIN_BALLOONUSERCLICK`） |
| Linux | 三层运行时降级链，见下 | 前两层支持（需宿主排空 `pump_events()`） |
| Headless | 仅记录 `last_notification()`，`notify()` 返回成功 | 不支持 |
| macOS / 其它 | 本轮不支持 | 不支持 |

**Win32 的托盘副作用**：应用本身没有托盘图标时，发通知会在任务栏**短暂出现一个图标**（`Shell_NotifyIcon` 气球必须依附图标）。气球消失后实现会 `NIM_DELETE` 撤掉，不长期占位。这是「零接线即可发通知」换来的可见副作用；调用方若已有 `SystemTray`，实现复用其图标、不额外增删。

**Linux 三层降级链**（全部 `dlopen` / `popen`，**无构建期依赖**，也不新增 `AURORA_ENABLE_*` 开关）：

| 优先级 | 后端 | 能力 |
|:---|:---|:---|
| 1 | `dlopen("libnotify.so.4")` | 高层 API；激活回调可接 |
| 2 | `dlopen("libdbus-1.so.3")` 手写 `org.freedesktop.Notifications.Notify` | 原生协议；经 `ActionInvoked` 信号接激活回调 |
| 3 | `popen("notify-send")` | 兜底，即发即忘，无激活回调 |
| 4 | 全部缺失 | `NotificationPostFailed` |

因为全部走 `dlopen`，实现**不** `#include <dbus/dbus.h>` / `<libnotify/notify.h>`，改为自行声明函数指针类型与 `dlsym` 符号名。

**失败口径**：一切失败统一为 `ErrorCode::NotificationPostFailed`（`detail` 指出缺失哪个库 / 哪个系统调用失败），调用方可据此退化为自己 UI 内的提示条；不抛异常、不静默 no-op。

**`last_notification()` 在所有平台上维护**（含投递失败的调用），让无桌面环境也能断言「请求了什么」——这是 Headless 与自动化测试的主观测面。`install_recording_backend()` 可强制只记录、不触达系统通知服务，供开发机上的字段往返测试。

**宿主排空义务**：Windows 下依附宿主消息泵（`PeekMessage` 天然覆盖）；Linux 下必须靠 `pump_events()` 驱动 GLib 主循环迭代 / libdbus 套接字读取，`Application::step_frame()` 已每帧调用一次。**宿主不排空 ⇒ 通知照常显示、回调不到**（不会虚假成功）。

### 8.5 输入法桥（Win32 IMM32 / X11 XIM / Wayland text-input-v3）

事件侧契约（`TextCompositionEvent` 字段口径、preedit 不进 `value()`、候选窗定位盒）见 [`05-event-navigation.md`](05-event-navigation.md) §2.4；本节只记平台壳的接线。

**桥的位置与生命周期**：`src/aurora/window/detail/win32_ime.h`（`detail::Win32ImeBridge`），由 `Win32Host::Impl` **随窗口构造**、与窗口一一对应（不像 §6.3 的无障碍桥那样惰性激活——IMM32 无查询代价，无输入法时 `WM_IME_*` 一条也不会投递）。`Win32Surface`（GDI）与 `D3D11Surface` 共用同一 `Win32Host` 宿主与同一份桥，故接一次覆盖两路后端。

**两条通道**：桥经 `Hooks{emit, caret_bounds, scale_factor}` 三个回调与宿主对话——`emit` 复用窗口既有的事件单槽（`Impl::handler`，与鼠标/键盘同径，最终落到 `WindowHost::dispatch` → `EventDispatcher`），`caret_bounds` 由 `Surface::set_composition_caret_provider()` 注入（宿主侧查询当前焦点控件的 `composition_caret_bounds()`），`scale_factor` 供 dp→像素换算。`WndProc` 侧把 IME 单独分族：`handle_ime(msg, wp, lp)`（`win32_host.cpp`）认领 `WM_IME_STARTCOMPOSITION` / `WM_IME_COMPOSITION` / `WM_IME_ENDCOMPOSITION` / `WM_IME_CHAR` 与 `WM_KILLFOCUS`（失焦取消侧路径），桥返回 `std::nullopt` 表示不处理、回落 `DefWindowProc`。

**三条实机才暴露的纪律**（无头 CI 无法验证，由 `tools/verify/win32_ime_live_probe.cpp` 自动段守住）：

1. `WM_IME_CHAR` **必须吞掉**（返回 0 不放行）。否则 `DefWindowProc` 会把它转成逐字 `WM_CHAR`，与 `GCS_RESULTSTR` 通道叠加 ⇒ 同一汉字上屏两次；DBCS ACP 下还会把宽字符拆成前导/尾随字节产出乱码。
2. 组合期间的 `WM_CHAR` **整条丢弃**（`handle_char` 查 `ime->is_composing()`）。输入法回发的逐字符 `WM_CHAR` 与 preedit 是同一份内容，上屏只认 `GCS_RESULTSTR`。
3. `WM_IME_COMPOSITION` 的 `lParam` 是**标志位集合**（`GCS_COMPSTR` / `GCS_CURSORPOS` / `GCS_COMPATTR` / `GCS_RESULTSTR`），只在命中相应位时刷新对应缓存；组合串每次都要重读（`ImmGetCompositionStringW` 无「未变」通知），光标位缺失时按「串尾」解释（`< 0` 一律夹紧）。

**IMM32 而非 TSF**：TSF 要求实现 `ITextStoreACPServices` 全套文本存储代理（约 1.5k 行 COM，且与 aurora「控件自持状态、无 Windows 文本对象」的模型正交），而 Win10/11 的 CTF 加载器对**非 TSF-store 窗口**会提供 IMM32 兼容读通道——`GCS_COMPSTR` / `GCS_RESULTSTR` / `GCS_CURSORPOS` / `GCS_COMPATTR` 全部可读，候选窗定位 `ImmSetCandidateWindow` 亦生效。故选 IMM32 首桥，覆盖中文/日文/韩文输入法的主流程；代价是**外部写组合串被拒**（见下）。将来若接 TSF，桥的对外形状（`Hooks` + `handle()`）无需变化。

**⚠️ 自动化局限**：`ImmSetCompositionStringW(SCS_SETSTR)` 在 TSF 型输入法（微软拼音、五笔等）下**被拒**（返回 FALSE、`GetLastError()` 为 0）——TSF 不允许外部往 IME32 兼容上下文写组合串。因此探针**不能**靠注入组合串来验收 preedit 渲染与上屏：该段按 SKIP 处理，改由 `--interactive` 人工段用真实输入法逐项目视（preedit 下划线更新、候选窗落在插入点旁、选字只上一次屏、Esc 取消无残留、切走焦点无半截拼音）。折算层本身（UTF-16 单元 → 码点、`GCS_COMPATTR` → 待转换选区、代理对夹紧）由无头单测 `tests/unit/utest_ime_composition.cpp` 全覆盖。

**依赖**：`imm32` 随 `aurora` PUBLIC 链接（`cmake/AuroraBackends.cmake` 的 Win32 分支：`user32 gdi32 shell32 ole32 uuid imm32`），属系统库，无三方依赖、无编译期裁剪开关。

**⚠️ 只有 `Application` 下才有效**：`set_event_handler` / `set_composition_caret_provider` 由 `WindowHost::attach_surface()` 接线，而焦点（组合事件的路由前提）也归 `WindowHost::focus_` 管。以 `create_native_window` 得到**裸 `Window`** 并直接 `present_root` 时，窗口没有任何事件处理器——鼠标、键盘、Tab 焦点与 IME 一律不通。真机探针与自建消息泵都必须经 `Application`（本文 §2.1）驱动。

#### 8.5.1 X11 桥（XIM，`src/aurora/window/x11_surface.cpp` 内联）

Xlib 桥没有独立的 detail 类（与 Win32 的 `Win32ImeBridge` 不同）：XIM 只有五个入口点（`XOpenIM`/`XCreateIC`/`XSetICFocus`/`Xutf8LookupString`/`XSetICValues`），直接挂在 `X11Surface::Impl` 上并与既有事件派发栈同构，拆文件只会把一条调用链劈成两半。

**协商与逐级降级**（`ime_setup()`，构造期与首次 `FocusIn` 各调一次、幂等——`XMODIFIERS`/IM 服务器晚就绪是 Linux 桌面常态）：`XOpenIM` 失败（无 IM）→ 整桥静默缺席，keysym 取字路径不受影响；成功则经 `XNQueryInputStyle` 查询 IM 广告的风格（`XGetIMValues` 契约：**成功返回 NULL**，输出参数有效），只有精确命中 `XIMPreeditCallbacks | XIMStatusNothing` 才走全事件路（preedit draw/caret 回调回推），否则回退 `XIMPreeditNothing`（commit 仍可经 `Xutf8LookupString` 到达，仅无组合期内容）。

**回调注册**：draw/caret 经 `XVaCreateNestedList` + `XSetICValues(XNPreeditAttributes)` 挂到 IC（Xlib R6 固定形态）；两者均按 `XICProc`（int 返回）形态注册——Xlib 把一切 IC 回调统一擦成该联合式签名、派发时按注册名还原（Qt/GTK 同款契约），`client_data` 直指 `Impl`（回调全在事件派发栈内同步触发，生命周期无忧）。

**焦点宣告**：`FocusIn` → `XSetICFocus`、`FocusOut` → `XUnsetICFocus`——不宣告则 IM 永不把组合事件路由进本 IC。XIC 建于映射之后时 WM 的 `FocusIn` 已错过，故构造期若窗口已 `active` 立即补一次 `XSetICFocus`（XIM 惯例兜底）。

**取字与通道收敛**：`KeyPress` 在 IC 在场时走 `Xutf8LookupString(ic)`；**溢出纪律**——`st == XBufferOverflow` 时 Xlib 不写缓冲而是返回所需字节数（一句中文即可超过 63 字节栈缓冲），此时改堆缓冲重查一次并一律 `clamp`，杜绝把未初始化栈内存当文本上屏。可打印文本：cb 风格且组合串非空 → `TextCompositionEvent.committed`（与 Win32/Wayland 同一条落字路径，preedit 随之清空）；其余（PreeditNothing 风格或组合未开始）→ `TextInputEvent`——**普通字符不得被 IM 接线吞掉**（防「接了 IM 反而吃掉键盘」回归，由探针 XTEST 段守住）。

**候选窗锚点**：`ime_update_spot()` 把 provider 盒（窗口逻辑 dp）按 `× scale` 取**底边中点 y**（候选窗落在组合串下方）写入 `XNSpotLocation`（同样经嵌套列表），盒未变则去重；只在组合态变化时刷新（`ime_emit_preedit` 出口处，`!utf8.empty() || was_composing`）——空串 = 取消，无须再摆候选窗。另有一条全局前置：`XOpenIM` 前须 `setlocale(LC_CTYPE, "")` + `XSetLocaleModifiers("")`（构造期一次性），否则 `Xutf8LookupString` 退化为 latin1，CJK 全灭。

**观测面**（供探针/单测）：`ImeState{im_open, ic_created, preedit_callbacks, focused, preedit, draw_callbacks, spot_updates}`。**降级面**：无 XIM 服务器机器上整桥缺席属合法形态（`XFilterEvent`/派发栈照常），探针该段 SKIP 不判负。**本机实测（WSLg/Xwayland）**：`XMODIFIERS` 未设时 `XOpenIM` 仍成功（Xlib 内建本地 XIM）且风格协商落到 PreeditNothing；`XSetInputFocus` 焦点往返与 XTEST 假键 `'a'` → `TextInputEvent("a")` 全绿——组合期 preedit 需真实 XIM 进程驱动，交 `--interactive`（`tools/verify/x11_ime_live_probe.cpp`）。

#### 8.5.2 Wayland 桥（text-input-unstable-v3，`src/aurora/window/wayland_surface.cpp` 内联）

**构建期门**：协议胶水由 `wayland-scanner` 从 `wayland-protocols` 的 `text-input-unstable-v3.xml` 生成，缺失即宏 `AURORA_HAVE_WL_TEXT_INPUT=0`、桥体整段裁切（软探测，不报配置期红灯，见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) §3.2）。

**对象生命周期**：`zwp_text_input_manager_v3` 在 `on_global` 绑定；`zwp_text_input_v3` 对象在本 `seat` **首次 offer keyboard 能力**时创建（v3 按 seat 建模，一个表面一个输入对象）。合成器不发布 manager（**WSLg Weston 实测即如此**）→ 整桥缺席、零协议请求，连接健康，属合法降级形态。

**enable 判据**（`ti_refresh_enable()`，由 `present()` 与 `poll_platform_events()` 双路 `refresh_ime_input_state()` 驱动）：`want = 键盘焦点在本表面（enter/leave 维护）∧ provider 报非零插入点盒（焦点在文本控件）`；`want == 当前态` 即去重不发，故静态界面每帧零协议流量。enable 时随一次性 `set_content_type(NONE, NORMAL)`（密码框等专用 hint 留待控件自描述出现时映射）；provider 未接线视为不接管（保持 disable，与 Win32 未接 provider 时仅剩系统默认行为对齐）。

**插入点盒**：`ti_update_cursor_rect()` 逻辑 dp → 表面本地物理 px（x/y 向下取整、宽高向上取整，与 `set_cursor_rectangle` 的 buffer_scale 口径一致），与上一盒逐字段去重、仅 enabled 时发送——每帧 present 都走此路径，去重是硬要求。`enter` 事件清空全部去重缓存（v3 语义：enter 后服务端状态视图重建，enable/content_type/盒须随下次刷新重发）。

**事件折算**：`preedit_string` → `ime::make_preedit_state_utf8`（字节下标 → 码点契约，与 Win32 同一纯函数层）；`commit_string` → `TextCompositionEvent.committed`（空 commit 只清 preedit、不发事件）；`leave` 带残留组合串 → 全默认字段空事件 = 控件 `cancel_composition`（与 Win32 失焦取消同收敛路径）。`delete_surrounding_text`：本端从不 `set_surrounding_text`（空上下文），非零回删请求折算成 `ArrowLeft`×before + `Backspace`×after 的 `KeyEvent` 对交控件处理（与物理退格同一派发路径，不旁路选择/剪贴板逻辑），单侧上限 64 防恶意大值刷帧。

---

### 8.8 Socket 操作层（`aurora::net`）

`include/aurora/net/socket.h` + `src/aurora/net/socket.cpp` 是全仓**唯一**的 Winsock2 / BSD socket 适配落点（CHG-006）：公共头零平台 socket 头，`#include <winsock2.h>` / `<sys/socket.h>` 只允许出现在该 `.cpp`（验收 grep 判据见 [`changes/winsock-bsd-socket-layer/proposal.md`](../changes/winsock-bsd-socket-layer/proposal.md)）。范围收敛为「Inspector 工具链 + 本机 AI 交互」所需：**地址族由 `Endpoint` 决定**（IPv4 / IPv6 同一条 `connect` / `bind` 通路，`*_loopback` 两枚工厂仍固定绑 `127.0.0.1`，都不经 DNS）、同步阻塞 + 显式超时、失败一律 `Result<T>` + 表驱动 `net-*` 错误码（`ERROR_CATALOG.md`，message 携带 OS 原生码）。

三个类型：

- `Session`：进程级运行时 RAII。Windows 腿引用计数配对 `WSAStartup`/`WSACleanup`（首个构造初始化、末个析构清理，嵌套/并存安全）；POSIX 腿零成本占位、`ok()` 恒真。不可拷贝/移动（纯作用域 guard）。
- `Listener`：`listen_loopback(port, backlog)`（只绑 `127.0.0.1`；port=0 由系统分配、`port()` 回报实际值）、`bind(endpoint, backlog)`（地址族取自 `Endpoint`，绑非回环地址即接受跨机连接，**访问控制交回调用方**）、`wait_readable(timeout) -> Result<bool>`（select 可中断等待，`false` = 纯超时非错误，两平台 nfds 差异内部消化）、`accept() -> Result<StreamSocket>`、`native_handle()`（借出原生句柄、不转移所有权，交 `wait_ready`/事件源注册口并入等待集）、`close()` 先 `shutdown(both)` 再关（唤醒并终止另一线程阻塞中的等待，`InspectorServer` 停机场径）。
- `StreamSocket`：`connect_loopback(port)`、`connect(endpoint)`（地址族取自 `Endpoint`；端口 0、本机 IPC 形态、无地址占位都在发起系统调用前落 `net-endpoint-invalid`）、`set_io_timeout(milliseconds)`（**毫秒统一口径**：Winsock 收 DWORD 毫秒、POSIX 收 `timeval`，形态换算封闭在实现侧——按平台字节形态误传会把 5 秒执行成 5 毫秒，本层的存在理由）、`read_some(span) -> Result<size_t>`（**0 = 对端有序关闭**，与错误态严格区分；超时落独立码 `net-recv-timeout`）、`write_all(span)`（内部 ≤1MiB 分块写满，`send` 返回值窄化一次解决）、`is_loopback_peer()`（服务端访问控制判据：`127.0.0.0/8` 全段 + `::1` + IPv4 内嵌形态 `::ffff:127.x`）、`peer_endpoint() -> Result<Endpoint>`（`getpeername` 还原为值类型，地址族与端口都带回来）、`native_handle()`（借出原生句柄、不转移所有权，同 `Listener`）。

链接面：`ws2_32` 随核心 `aurora` 目标在 Windows 无条件 `PUBLIC` 传导（见 `BUILD_OPTIONS.md`），各消费者不再散挂。Emscripten 腿经其 BSD socket shim 编译，但浏览器运行时无真实监听语义，Inspector 相关用例在 wasm 构建下按 `AURORA_TEST_SKIP` 退避。回归：`utest_net_socket`（行为契约）+ `utest_inspector_client`（超时平台形态）。

#### 8.8.1 底座值类型：端点与取消令牌（CHG-010 已落地部分）

`include/aurora/net/address.h` 与 `include/aurora/net/cancellation.h` 是通用网络栈的第一批落点。两者都不引入平台 socket 头，也**不改变**上一段所述同步传输面的当前范围（`socket.h` 仍只消费回环端点；非回环连通性随后续切片开放）。

- `Endpoint` / `AddressFamily`：一条字节流「去往或来自何处」的纯值类型。**解析与规范化由仓内手写、不经 `inet_pton`**——同一输入在 Win32 / POSIX / Emscripten 上必须给出同一结果与同一条诊断（§12.2 一致行为判据；交给 libc 等于把「哪些畸形串被谁容忍」交给各平台实现）。判据要点：IPv4 禁前导零（避开八进制歧义）；IPv6 支持 `::` 压缩与内嵌 IPv4 尾段，输出按 RFC 5952 规范化（小写、去前导零、最左最长零游程压缩，故 `0:0:0:0:0:0:0:1` 与 `::1` 判等）；方括号形态只服务 IPv6 且**不得内嵌端口**（`"[::1]:8080"` 直接拒）；zone id 仅在 link-local（`fe80::/10`）上合法；回环与通配判定**按解析后的地址族与二进制值**而非字符串（`"localhost"` 是名字、不是地址）；本机 IPC 路径在值类型层就受 `AURORA_NET_MAX_LOCAL_PATH` 与 NUL 约束，不等 bind 才发现路径过长。端口 `0` 在值类型里合法并表达「由系统分配」，语境合法性由 `bind()` / `connect()` 分别判——值类型不预判用途。默认构造的无效占位在 `is_loopback()` / `is_unspecified()` 上一律 false（全零字节不参与语义判定）。
- `Cancellation`：异步网络操作的取消令牌，共享状态句柄（拷贝即共享同一状态，`shares_state_with()` 可断言）。三条契约：**取消不抛异常**，一律以 `Result<T>` 的 `net-cancelled` 抵达调用方；**钩子至多一次、且在调用线程同步执行**——引擎据此把「释放句柄」排在「投递取消结果」之前，顺序反了会让回调里就地重连撞上刚被占满的句柄表；**取消先于登记时钩子立即触发**，不留「登记即失效」的窄竞态（那类缺陷只在真实网络延迟下现形）。它与既有 `Task::cancel()` 的「回调不再触发」**刻意不同**：协程帧只能靠 `resume` 前进到 `co_return` 正常析构，「不恢复」等于泄漏帧。
- 错误码：`net-endpoint-invalid`（`message` 携 `detail=` 指明被拒原因）、`net-cancelled`（`retryable = true`，供重试与策略判定直接消费）。
- 回归：`utest_net_address`（解析、规范化、校验与判等黄金判据）、`utest_net_cancellation`（含跨线程钩子落点与钩子内重入不死锁）。

#### 8.8.2 事件源注册口（`aurora::app::EventSourceRegistry`，CHG-010 底座）

网络栈 L0 宿主集成层的通用就绪通知底座（`include/aurora/app/event_source_registry.h` + `src/aurora/app/event_source_registry.cpp`）：把「一个可等待句柄 + 就绪回调」汇成一张待等待表，供第三方非阻塞会话（libssh2、DBus 一类）与库自带的 poll 线程引擎共同复用。**缺省驱动方是 §8.8.4 的常驻引擎线程**（本库各平台腿的统一形态）；自带等待集的宿主调 `set_host_driven(true)` 接管，引擎随即只等自己的唤醒通道。提案 [`changes/xplat-network-stack/proposal.md`](../changes/xplat-network-stack/proposal.md) §2 的逐平台并入点与 §18 定案 1 的竞态契约仍然有效，但**仓内宿主不再各自并入**（理由见 §8.8.5 与提案 §16 第 12 项）。

- **平台无关且位于 `net/` 之下**：本 `.cpp` 不出现任何平台 socket/poll 头（`winsock2.h`/`poll.h` 唯一落点仍是 `src/aurora/net/socket.cpp`，CHG-006 不变量不破）。真正的「阻塞直到某些句柄就绪」经三枚可注入接缝表达——`set_poller(Poller)`（等待实现，由 net 引擎层或测试安装）、`set_dispatcher(Dispatcher)`（主线程投递实现，缺省为就地同步执行，与 `post_to_main` 无 poster 时的口径一致）、`set_change_notify(ChangeNotify)`（表变动与驱动方转变的通知，归常驻等待方独占，判据见 §8.8.4 投递纪律 ②）。自带等待集的宿主不装 `Poller`、改走 `sources()` 取快照 + `dispatch(ready)` 投递。无 fd 语义的腿（wasm）在**构造期**把 `event_source_supported()` 置假（编译期事实只落一处，公共头维持零 `#ifdef`），此后 `register_source` 一律回 `net-platform-unavailable`，`wait_and_dispatch` 在未装 Poller 时同码，均**不忙等、不假降级**。
- **句柄生命周期与三条竞态契约（§18-1，规范性）**：`register_source` 返回 move-only RAII 句柄 `EventSourceRegistration`，析构即注销且**幂等**、移动后源转空态、其析构不再触发二次注销。注销语义 = **停止产生新回调**：`dispatch` 在锁内只快照待投递闭包（`shared_ptr` 按值捕获令条目独立于注册表存活），**释放锁后**才逐个投递——因此已进入 `Dispatcher` 队列的一次尾批仍会抵达一次（回调载荷须由使用方以弱令牌保护宿主），但此后新的 `dispatch` 不再为该令牌产生回调。需连尾批一起作废另用 `cancel_pending()`（置条目的 `cancelled` 原子位），该调用与析构**互不隐含**：前者只作废尾批、不影响后续投递；后者只移除快照条目、不作废已入队尾批。回调内可安全再注册/注销（投递在锁外）。
- **令牌关联**：同 `handle` 可多次登记、各自持独立令牌与回调（`next_token()` 单调、从 1 起、0 留空态）；宿主等待回来后按令牌回填 `ReadySource`，注销后重登记取新令牌、旧令牌的迟到就绪不会误触发新条目回调。
- 错误码：复用 `net-platform-unavailable`（本次新增，`retryable = false`、`fix_category = platform_capability`；`hint` 指向 `Engine::capabilities()` 查询与能力矩阵）。
- 回归：`utest_app_event_source_registry`（有效句柄与 `-1`/不支持腿的拒绝、就绪回调「抵达次数 == 就绪次数」、原生宿主无 Poller 走 `sources()`/`dispatch()` 的通路、`unregister` 与 `cancel_pending` 在排程 Dispatcher 下的尾批分岔、移动不双重注销、同句柄重登记取新令牌、`host_driven` 两次转变都通知常驻方）。引擎侧的互斥落地判据见 §8.8.4 的 `utest_net_engine`。

**逐平台等待集并入 = 宿主可选项，仓内宿主一律走引擎线程**（裁决与理由见 §8.8.5；提案 §16 第 12 项）：本注册口对外提供的就是这条可选项——自带循环的宿主（含嵌入本库的第三方事件循环）装 `set_host_driven(true)` 后自行 `sources()`/`dispatch()`，其余腿由常驻引擎驱动，回调一律经主线程投递器回到 UI 线程。

#### 8.8.3 事件驱动就绪基元（`aurora::net::wait_ready`，CHG-010 底座）

`include/aurora/net/poll.h` + 实现在 `src/aurora/net/socket.cpp`：给一组 `{句柄, 关注方向}` 阻塞到部分就绪或超时的**多句柄、四向（读/写/错误/挂断）事件驱动等待基元**。它是 §8.8.2 注册口 `Poller` 接缝的具体实现来源（引擎与测试都把它包成 Poller 装入），也是未来 poll 线程引擎与第三方会话共用的一条底座——刻意与 `Listener::wait_readable`（单句柄、`select`、同步回环）分工：本基元**只做等待、不碰协议语义**，不知道句柄背后是 socket、管道还是 DBus 描述符。

- **平台 socket/poll 头唯一落点不破**：POSIX `poll` 走 `<poll.h>`、Winsock `WSAPoll`/`WSAPOLLFD` 走 `<winsock2.h>`，两者都只在 `src/aurora/net/socket.cpp` 出现（CHG-006 不变量继续成立；`<poll.h>` 本就在受限清单之外，但 `WSAPoll` 依赖 `winsock2.h` 故必须落在此 TU）。`net/poll.h` 零平台头。
- **就绪语义定见**：返回向量与入参 `PollDescriptor` **等长同序**（第 `i` 项描述第 `i` 句柄），第 `i` 项 `handle < 0` 时该槽恒全 false 且**不进系统调用**——一个坏句柄不毁整批等待（POSIX 负 fd、Windows 传 `INVALID_SOCKET` 会 `WSAENOTSOCK` 让全批失败，故排除而非透传）。`readable`/`writable` 仅在对应方向被 `want_*` 请求时才置位（`POLLIN`/`POLLOUT` 按请求上报），`error`（含 `POLLNVAL`）与 `hangup`（`POLLHUP`）无条件上报（内核这两态不受 `events` 请求约束）。**纯超时不是错误**：无句柄就绪时返回 OK + 全 false 向量，调用方据此轮询自有停止标志；仅系统调用真失败落 `net-wait-failed`（message 携 OS 原生码）。
- **超时口径**：`timeout` 是**单次系统调用**的上限，POSIX 被 `EINTR` 打断时以同一预算重新武装（不吞错误、不提前返回），Windows `WSAPoll` 无 `EINTR` 语义故单次调用；`timeout <= 0` 表示非阻塞立即回报当前就绪态。跨平台一致行为判据下不承诺单调时钟意义下的精确总时长。
- **`native_handle()`（同步面新增的借用出口）**：`StreamSocket`/`Listener` 各加 `native_handle() -> std::intptr_t`（`-1` 恒无效，与 `is_valid()` 同口径），把原生句柄借给 `wait_ready`、事件源注册口或第三方库（libssh2 一类）接管。约束：**非所有权转移**，仅在本对象存活且未 `close()` 期间有效，调用方不得自行关闭（双重关闭会误伤系统复用后的新句柄）。Windows 腿为 `SOCKET`、POSIX 腿为文件描述符。
- **Windows 前置条件**：`WSAPoll` 需活跃 Winsock 运行时，故 `wait_ready` 要求调用方持 `Session`（与所有 net 用法一致）；否则以 `WSAENETDOWN` 落 `net-wait-failed`。测试用 `WSAPoll` 历史不稳的两条路径（listener 待 `accept` 的 `POLLIN`、对端 `FD_CLOSE` 的 `POLLHUP`）不作断言判据，只锁「连接即可写 / 写入后可读 / 排空即纯超时」这三条两平台同色的语义。
- **Headless 基线（§2「先立无 GUI 行为基线」）**：把 `wait_ready` 包成 Poller 装入 `EventSourceRegistry`、登记真实回环句柄，即可在无 GUI 单测里逐值验证「注册 → 就绪回调抵达次数 == 就绪次数 → 析构注销后 `sources()` 清空、`wait_and_dispatch` 空转且回调不再抵达」——这是 §18-1 生命周期契约在真实 fd 上的落地证据。
- 错误码：复用既有 `net-wait-failed`，本切片无新增码。
- 回归：`utest_net_poll`（空集合 OK、无效槽不毁批、可写→可读→空闲三态、注册口经真实 fd 的端到端事件源循环）。

#### 8.8.4 引擎与常驻 poll 线程（`aurora::net::Engine` / `Capabilities`，CHG-010 底座）

`include/aurora/net/capabilities.h` + `include/aurora/net/engine.h`（实现 `src/aurora/net/engine.cpp`）：把 §8.8.3 的基元包成**常驻循环**并接上主线程完成投递，即提案 §2 的默认形态——「谁在什么线程、以什么节奏调用基元」就此定死。

- **`Capabilities` 是运行期唯一真值源**：9 枚具名 bool（`tcp` `udp` `local_ipc` `dns` `tls` `listens` `event_source` `streaming` `custom_headers`），缺省全 false。纪律：**能力位不得先于实现点亮**——本底座落地为 `tcp` / `listens` / `event_source` / `streaming` / `custom_headers` 为真（后两枚表达的是平台限制而非实现进度，native 无 `ReadableStream` / CORS 约束），`udp` / `local_ipc` / `dns` / `tls` 随各自切片置真。`event_source` 的运行期落点就是 §8.8.2 的 `event_source_supported()`，两处真相不重复登记。
- **`Engine` 是引擎的公共面**：`instance()` / `name()` / `capabilities()` / `post()` / `post_after()` / `running()` / `shutdown()`。`name()` 回报实现名（自带引擎 `"poll"`、无 fd 语义腿 `"none"`），供 Inspector 与日志区分。查询能力**不**拉起线程；惰性启动的触发者是 `post` / `post_after`（§2「随首个需要它的对象惰性启动」）。
- **循环节奏**：「跑已到期任务 → 快照 `sources()` → `wait_ready` → 交回 `dispatch()`」，一条线程承载任务队列与到期定时器，不为定时器另起线程。等待窗口 = `min(最早到期定时, 空闲上限 200ms)` 且**下限 1ms**（不忙等；200ms 只是「唤醒通知丢失」的延迟上界，正常路径由唤醒通道即时打断）。
- **唤醒通道刻意不引入新平台代码**：它是一对**回环 TCP 流**（`Listener::listen_loopback` + `connect_loopback` + `accept`），两平台的 poll 族都认得 socket 句柄，因此 `engine.cpp` 不出现 `poll.h` / `winsock2.h`——CHG-006「平台 socket 头唯一落点是 `src/aurora/net/socket.cpp`」不变量继续成立。单轮只读一块（8 字节）且不计数：读只在 `POLLIN` 报可读后发生，故必不阻塞；残余字节让下一轮立刻返回可读。
- **投递纪律（三条，规范性）**：① `Dispatcher` 只在宿主未装时补 `aurora::detail::post_to_main` 形态（与 `Task<T>::set_main_poster` 同一条投递器，§1 不变量），宿主已装则不改；② `ChangeNotify` 归常驻等待方（即本引擎），登记与**注销**都要通知——注销后宿主随即关闭句柄，滞留快照会让下一次系统调用撞上无效句柄；③ **一条事件源只由一个驱动方等待**：常驻引擎循环与宿主自带等待集互斥，同时启用会让一次就绪投递两次。开关就是注册口的 `set_host_driven(bool)`（缺省假 = 引擎驱动），引擎每轮等待都读它，置真即让位、置回假即接管，两种转变都经 ② 的通知立刻生效，不必等一个等待窗口或先 `shutdown()`。本库各平台腿缺省全部走引擎驱动（宿主并改为可选项，理由见 §8.8.5）。
- **瞬态等待失败不终止循环**：注销与就绪同帧发生时本轮可能正持着含已关闭句柄的快照，`wait_ready` 落 `net-wait-failed` 属预期竞态；循环丢弃本轮结果并在下一轮重取快照（`utest_net_engine` 的 `unregister_during_wait_keeps_the_loop_alive` 即此判据）。
- **停机语义**：`shutdown()` 幂等，停止接收新任务、把**已入队任务与已到期定时跑完**再 join（不丢已承诺的工作），未到期的定时**丢弃且不执行**——静默跑一个过期续体会伪造出错误的完成时序；已投进主线程队列的完成事件由宿主退出路径排空（§2）。收敛后再次 `post` 会重新惰性启动。
- **无 fd 语义腿**：wasm 装配 `NullEngine`（`name()=="none"`、能力全 false、`post`/`post_after` 一律 `net-platform-unavailable`），绝不假降级也不排队等位。
- 错误码：本切片**无新增码**，直接产出的只有 `net-platform-unavailable`（无 fd 语义腿、停机竞态拒收新任务）与 `net-runtime-failed`（平台网络运行时不可用）；唤醒通道构建腿的失败码由 §8.8 的同步回环原语**原样透传**（`net-socket-failed` / `net-bind-failed` / `net-listen-failed` / `net-connect-failed` / `net-accept-failed`），引擎不自造第二套口径。
- 回归：`utest_net_engine`（能力矩阵如实、查询不起线程、`post` 在非调用线程执行、空任务不占队列、`post_after` 不早于登记意图、常驻循环驱动真实事件源、注销同帧不毁循环、`shutdown` 幂等且可重启、`host_driven` 置真时引擎不再等待注册口条目而宿主自投一次、交回后引擎重新送达）。

#### 8.8.5 逐平台驱动形态（底座收口裁决）

底座四件均已交付：值类型（§8.8.1）、事件源注册口（§8.8.2）、就绪基元（§8.8.3）、常驻引擎与能力矩阵（§8.8.4）。原列在 §17 底座里的「把注册口**并入各平台宿主等待集**」不再作为仓内宿主的改造项，改为宿主可选项——`set_host_driven(true)` 之后自行 `sources()`/`dispatch()`。三条理由：

- **分层方向**：注册口属 L0 `app`，提案 §1 只许可 L1 `net` 消费它（「L0 投递口」）。X11/Wayland 的 `poll` 数组与 AT-SPI 桥都在 window 后端，让后端回头消费 `app` 会形成 app↔window 双向依赖；要保住方向就得在公共 `Surface` 面上加一枚由 app 注入的「额外待等待 fd + 就绪回填」钩子——换来的是省掉一次微秒级跨线程投递，代价是公共 API 面扩大。
- **Win32 形态冲突**：`MsgWaitForMultipleObjectsEx` 只等内核对象，socket 要进句柄数组须经 `WSAEventSelect`，而该调用把套接字置入**事件态**，与 CHG-006 既有的同步收发面（同一句柄上的阻塞 `recv`/`send`）互斥；64 句柄上限还会把宿主等待集的规模变成注册口的容量约束。引擎腿用 `WSAPoll`，两处耦合都不存在。
- **一致性本就是默认形态**：提案 §2 写明自带引擎的默认形态是「引擎自持工作线程……不依赖各平台主循环的等待形态差异」，逐平台并入只是同一能力的第二种实现，不是新增能力。

由此定下的运行期口径：**Win32 / X11 / Wayland / macOS / GLFW / Headless 一律缺省由常驻引擎线程驱动**（回调经主线程投递器回 UI 线程，唤醒走各腿既有 `request_wake`：Win32 `PostMessage`、X11/Wayland 唤醒管道、GLFW `glfwPostEmptyEvent`）；**wasm 无 fd 语义**，能力位假且注册即报 `net-platform-unavailable`。§18-1 的「64 句柄上限以运行期错误显式暴露」因此只约束**宿主驱动腿**（宿主自有等待集的规模由宿主负责），本库缺省腿不受该上限约束。AT-SPI 的 DBus watch fd 通道维持其在 window 后端的自有集成（它早于注册口，且是 main-thread-only 的无障碍关键路径），注册口的「首例消费者」改为 libssh2 一类第三方会话——那才是它的外部用途，且已用真实回环 fd 在 Headless 基线里逐值验证。

传输族、`Resource` 绑定、`FakeEngine`、TLS、HTTP、可观测性、SSE、WebSocket 依 §17 顺序在后续切片落地（`Url` 与 `Headers` 两枚基础件已落地，见 [`01-core.md`](01-core.md) §10、§11；双栈首件见 §8.8.6，代理配置侧见 §8.8.7）。

#### 8.8.6 地址族无关化（双栈与竞速切片首件）

`AF_INET` 的硬编码构造点（`socket()` 与两处地址组装）整体改为**地址族取自 `Endpoint`**，这是双栈与竞速的前置条件——`Resolver` 同查 A / AAAA 之后，连接侧必须能把 IPv6 端点变成 `sockaddr_in6`，否则解析结果无处可去。三条定见：

- **组装与还原只有一条通路**：`make_address(Endpoint) → sockaddr_storage`（按族清零后填族字段、端口、地址字节与作用域号）与 `endpoint_from_address(sockaddr_storage) → Result<Endpoint>` 互为逆映射。为此 `Endpoint` 增加 `from_bytes(family, bytes, port, zone)` 工厂——二进制地址的规范化渲染与 `from_ip()` 共用同一套实现，因此 `getaddrinfo` 的字节序结果不会另产生一种写法（`from_bytes(...) == from_ip(host(), port())` 对无 zone 地址成立）。平台 DNS 与 `getpeername` 的出参从此不经任何 libc 文本转换，跨平台一致行为判据不因 `inet_ntop` 的差异漂移。
- **IPv6 作用域号只认十进制**：`sin6_scope_id` 是数字，`Endpoint` 的 zone 却是文本，中间那步「接口名→索引」在 POSIX 是 `if_nametoindex`、在 Winsock 属 iphlpapi 的 `if_nameindex`，两条腿的可达性不同。与其让同一个 `[fe80::1%eth0]` 在一台机器上能连、另一台上报错，一律按**十进制作用域号**判：非数字 zone 在发起系统调用前落 `net-endpoint-invalid`（数字 zone 则照常装配，链路可达性交网络层回报）。
- **回环判定升级为三段**：`is_loopback_peer()` 不再只看 `127.0.0.0/8`，另认 `::1` 与 IPv4 内嵌形态 `::ffff:127.0.0.1`——后者是双栈机上「v4 客户端接入 `::` 监听口」时 `getpeername` 的常见回报，漏判会把本机连接误认成外部连接。`peer_endpoint()` 同时把对端地址族与端口作为值类型交回，服务端访问控制从此可按地址族分别决策。

访问控制口径不变：`listen_loopback` / `connect_loopback` 固定绑 `127.0.0.1`，Inspector 工具链的信任模型逐值不动；新增的 `bind(endpoint, backlog)` 与 `connect(endpoint)` 是通用入口，**绑到非回环地址即接受跨机连接，访问控制由调用方负责**（本层不做「只允许本机」的隐式拦截，否则第三方库接管句柄时会得到无法解释的拒绝）。Windows 的 IPv6 套接字缺省是 v6-only（`IPV6_V6ONLY` 由注册表决定），因此「`::` 监听口收 127.0.0.1 接入并回报内嵌形态」这条判据在 POSIX 腿兑现、Windows 腿按环境缺失 `AURORA_TEST_SKIP`；显式清 `IPV6_V6ONLY` 属原生选项逃生舱（§11），不在本件里预支。

- 错误码：本件无新增码，`net-endpoint-invalid`（端点不合用：端口 0、本机 IPC 形态、无地址占位、非数字 zone）与既有的 `net-socket-failed` / `net-bind-failed` / `net-listen-failed` / `net-connect-failed` 覆盖全部失败分支。
- 回归：`utest_net_socket` 新增四例（不合用端点在发起系统调用前即拒、数字与接口名两型 zone 的分岔、`[::1]` 往返与两侧回环判定及 `peer_endpoint()` 端口回报、双栈监听口上的内嵌形态识别）。

#### 8.8.7 代理配置侧：`ProxyConfig` / `NoProxy` / `ProxyEnvironment`

`include/aurora/net/proxy.h` + `src/aurora/net/proxy.cpp` 交付 §11 代理族的**配置与决策**部分：五个类型全是值语义，零 socket、零 DNS、零平台头，唯一职责是把「配置文本」变成「可判定的规则集合」。`CONNECT` 与 SOCKS5 的握手字节属传输侧，不在本件。六条定见：

- **畸形即失败，绝不静默直连**：`HTTP_PROXY=http//host` 若被当作「没配代理」，症状是「在办公网能通、在客户网络不通」，且日志里无痕。故 `parse()` / `from_environment()` 返回 `Result`，且失败 message 必带**是哪个变量**或**是第几条规则**（`detail=HTTPS_PROXY: ...`、`detail=NO_PROXY entry #2 '...': ...`），运维定位不需要先复现。
- **scheme 决定怎么连代理，不决定目标是否 TLS**：`HTTPS_PROXY=http://proxy:3128` 是合法且最常见的形态（明文连代理、代理隧道到 https 目标）。代理自身走 TLS（`https://`）与 SOCKS4 属已知但不实现，报独立的 `net-proxy-unsupported` 而非 `net-proxy-config-invalid`——调用方要能区分「用户写错了」与「本库不支持」；两者都**不退化成明文跳**，否则代理链路上的凭据会被送到错误信道。
- **决策只有一处**：`ProxyEnvironment::for_target(host, port, secure)` 是唯一入口，次序为「例外优先 → 安全目标 `HTTPS_PROXY`→`ALL_PROXY`→直连 / 明文目标 `HTTP_PROXY`→`ALL_PROXY`→直连」。安全目标**绝不**回退到 `HTTP_PROXY`：两个变量之间的隔离是运维的契约，把它猜平等于让企业网按直觉分流。缺省端口沿用 curl / Go 口径（CONNECT 80、SOCKS5 1080），裸 `host[:port]` 按 CONNECT 处理。
- **主机文法只有一套**：代理主机与例外项的规范化一律借 `Url::from_parts` / `Endpoint::from_ip` 的既有出口（ASCII 小写、punycode、IPv6 去括号、去尾点），本层不新造第二套主机文法，否则同一台代理会有两种拼写却判不等。
- **`NO_PROXY` 是封闭文法**：只接受 `*`、域名（`example.com` / `.example.com` / `*.example.com` 三写法等价，匹配本体与任意深层子域）、数字字面量（**全等**匹配，不做后缀——否则 `.0.0.1` 会把无关主机卷进例外）、CIDR（`10.0.0.0/8` / `fd00::/8`，主机位非零即拒），以及上述形态后跟 `:端口` 的限定；分隔符 `,` `;` 与空白等价、空项忽略。IPv6 项必须带括号（不带括号时 `:` 无法与端口限定区分）。CIDR 只与**数字字面量**目标比较——名字型目标在本层不做解析，口径与 Go / curl 一致。
- **凭据只在内存里**：userinfo 解析时立即百分号解码（密码里的 `%3A` 与用户名里的 `%40` 都保持原义，拆分只认第一个 `:`），`to_string()` 恒把密码替换为 `***` 且 IPv6 代理重新补括号（渲染结果必须可回解析）；诊断字段 `source()` 不参与相等比较，故「环境变量来的配置」与「代码里同值配置」判等。

环境变量读取器 `ProxyEnvironment::Reader` 是本件的注入接缝：每个变量先大写后小写（`HTTP_PROXY` 然后 `http_proxy`），空串视为未设置（CI 与容器里的显式清空写法），单测因此无需真实环境变量即可覆盖全部次序与例外形态。系统代理读取（Win32 WinHTTP 查询、macOS 系统配置，且只取地址与例外、不执行 PAC）涉及新平台头与链接依赖，留作独立子步；代理自身的 TLS 属非目标。

- 错误码：新增 `net-proxy-config-invalid` 与 `net-proxy-unsupported`（category `io`、`retryable=false`，数据源 `codespec/errors.toml`，`ERROR_CATALOG.md` 已随生成器刷新）。§10 预留的 `net-proxy-unreachable` / `net-proxy-auth-required` 待握手切片按真实失败分支启用，本件不预支。
- 回归：`utest_net_proxy` 十例——形态与缺省端口、七类畸形值与两码分岔、凭据解码与打印掩码、结构化工厂的入参校验、域名与端口限定匹配、字面量全等与 CIDR 前缀（含 `/0` 与 `fe80::/8` 越界样本）、封闭文法的十一类拒收与「第几条」定位、大小写优先级与变量名回报、`for_target` 的完整选择次序、`source()` 不参与判等。全用例纯逻辑，三平台一致，wasm 腿同样可跑，故无 `AURORA_TEST_SKIP`。

---

### 8.9 系统拖放（跨进程）

事件族与控件契约见 [`05-event-navigation.md`](05-event-navigation.md) §5.2；本节只记**平台 Shell 侧**的入口、注入点与错误口径。

- **程序化派发入口**：`Application::dispatch_system_drop(SystemDropEvent &)` / `WindowHost::dispatch_system_drop(SystemDropEvent &)`，四态共用，阶段由调用方给定。位置类事件同款：派发根为整窗下沉壳，坐标为整幅表面逻辑坐标。无头环境与单测经它完整走通四态派发，**不依赖任何真实窗口**。
- **平台后端接入点**：后端把 OS 会话翻译成事件后交 `WindowHost` 的事件处理器（Win32 `WM_DROPFILES` 路径已按此换型，HDROP 解析与坐标换算原样保留）。其余后端的 OLE / Xdnd / `wl_data_device` / dragging session 属后续接线范围。
- **拖出**：`system_drag::begin(DragOffer)` 是会话入口。当前无后端接线时返回 `system-drag-not-supported`；控件侧入口为 `Widget::begin_system_drag()`（取 `on_provide_drag()` 的要约），结束回 `on_system_drag_end(DragAction)`。**不引入嵌套消息泵**（Win32 `DoDragDrop` 的嵌套泵与重入 guard 属接线期设计项）。
- **错误口径**（`ERROR_CATALOG.md`）：`system-drag-not-supported`（无机制 / 能力位为 false；`retryable = false`，机制缺失是稳态，重试无意义）与 `system-drag-session-failed`（会话建立或进行期失败：协商破裂、源数据中断；同样按稳态定稿，瞬态重试由调用方按 hint 自行决策）。要约不合法（空 `formats` / 空 `provide`）是调用方参数问题，走 `general-invalid-argument`，与「平台没机制」分开报。
- **测试注入**：`system_drag::install_test_backend()` / `reset_test_backend()` / `remove_test_backend()` 三件套（同 `Clipboard::install_test_backend` 范式，`AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏裁切，Release 下零副作用）。安装后 `begin` 不触碰平台、直接回成功动作 `Copy`，使拖出分支在无头环境可测。载荷侧由 `DragPayload::from_bytes` 直接构造内存载荷，drop 态的往返不依赖平台。
- **热路径与无障碍**：`Over` 态按指针采样频率到达，accept 查询须常量级、禁在拖放路径触发重布局；拖放不是唯一路径，粘贴 / 文件对话框等非拖拽入口在 UI 上须可达，放置反馈不得只靠光标形态。

回归：`utest_drag_payload`（载荷往返、空口径、inert 后端、能力位锚定）+ `utest_dispatcher`（四态派发与 accept 查询）+ `utest_event` / `utest_application`（默认构造与程序化入口）。

---

### 8.10 最近文档（`app/recent_documents.h`）

`RecentDocuments` 是应用级「新 → 旧」有序集合，供「文件 → 最近打开」类菜单消费；形态与 `ScrollStorage`（§9.3）同构——进程级单例 + `Preferences` attach / detach + 内存优先懒回读，**但不引入 `Scope`**：最近文档是应用级全局语义（多窗口共享同一列表），不是窗口作用域状态。

- `instance()`（进程级单例，懒构造）、`AURORA_MAX_ENTRIES = 16`（容量上限，声明处可见；**无运行期 setter**，避免「设置与容量不一致」的第二真源）、`AURORA_GROUP_NAME = "recent_documents"`（持久化分组名）。
- 会话内读写：`add(path)`（同路径去重并置顶、超容量淘汰最旧；空路径 no-op）、`list()`（新 → 旧序值拷贝）、`clear()`（清内存并打墓碑）、`size()`、`clear_all()`（测试与重置用）。
- **路径口径：只做字符串级存取**——不校验文件是否存在、不做路径规范化（`..` / 相对路径 / 大小写差异一律按原样字符串比较）。理由：本类在主线程且不触碰文件系统，读存在性会引入不可测的 IO 副作用与竞态；「最近文档里留着已删文件要不要显示」是应用侧决策。
- **持久化信封**：分组下单键 JSON 数组 `{"documents":[...]}`。与 `ScrollStorage` 的「每键一条目」是**有意分歧**：最近文档是有序集合、顺序即语义，用索引当键会把顺序编码进键名，墓碑与重排逻辑都不必要地复杂。`sync()` 只写 `Preferences` 内存并通知订阅者，**落盘由 `App` 的 `flush()` 决定**（与 `ScrollStorage` 同口径）；读取懒回读，不做全量预热。
- **宽容降级**：读缺失或信封损坏（值非数组、元素非字符串）得到空列表，不抛错、不清除用户数据。
- **无内置变更通知**：本类只管存取，菜单刷新由调用方在 `add` / `clear` 后自行驱动（同 `ScrollStorage` 口径，避免引入第二套通知机制）。
- **OS 级投影不在本批**：Windows JumpList / macOS `NSDocumentController` 消费 `list()`，Linux 无 OS 级机制（能力位申报同属后续批次）；落地前应用内列表已全平台一致。

回归：`utest_recent_documents`（去重置顶 / 容量淘汰 / 顺序 / 持久化往返 / 空口径 / 损坏信封降级）。

---

## 9 偏好与存储

### 9.1 Preferences

`Preferences`（`preferences/preferences.h`）是键值持久化门面，类型化读写并带响应式绑定。

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

`Storage`（`storage/storage.h`，命名空间 `aurora::storage`）是记录级存储抽象，支持 JSON 与二进制载荷、异步卸载与变更通知。

**构造与默认实例**

| 方法 | 说明 |
|:---|:---|
| `create(FilesystemOptions = {}) -> Result<Storage>` | 打开缺省文件系统后端（零额外依赖，始终可构造）；打开失败返回 `storage-backend-unavailable` 错误（`:28`） |
| `create(SqliteOptions) -> Result<Storage>` | SQLite 后端（**需 `AURORA_ENABLE_STORAGE_SQLITE=ON`**，默认 OFF 时该重载不声明）：单文件库或 `:memory:`（`in_memory`），文件库默认 WAL（`wal`）；路径空 → `default_config_dir()/"aurora_storage.db"`；打开失败返回 `storage-backend-unavailable` |
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

**后端抽象**：`storage/storage_backend.h` 定义后端接口，`fs_backend.h`（文件系统）与 `memory_backend.h`（内存）是两个恒编译实现，`sqlite_backend.h`（`SqliteBackend`，opt-in：`AURORA_ENABLE_STORAGE_SQLITE`）是第三后端——真事务（`BEGIN IMMEDIATE`/`COMMIT`/`ROLLBACK`，嵌套加入同一事务）、二进制载荷 BLOB 内联（`blob_ref` 恒空，无 sidecar）、`contains` 走 SELECT EXISTS、`clear` 单语句 DELETE、`flush` 做 WAL checkpoint；`serializable.h` 定义可序列化概念，`storage_types.h` 定义 `StorageRecord` / `StorageValue` / `StorageBytes` / `FilesystemOptions` / `SqliteOptions`。

存储相关错误码：`storage-backend-unavailable`、`storage-record-not-found`、`storage-record-corrupt`、`storage-type-mismatch`、`storage-encoding-mismatch`、`storage-io-error`（见 [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)）。

### 9.3 ScrollStorage

`ScrollStorage`（`app/scroll_storage.h`）是滚动位置注册表：`restore_key → offset`，让滚动容器在重建 / 重启后回到原位置（对标 Flutter `PageStorage` + Android `rememberSaveable`）。

| 成员 | 说明 |
|:---|:---|
| `instance()` | 进程级单例（main-thread only；单线程 UI 故不加锁） |
| `write(key, offset)` / `read(key)` | 会话内读写。`write` 只动内存 map（滚动热路径零 JSON / 零磁盘）；`read` 内存优先，缺失且已 `attach` 时**懒回读** `Preferences` 并缓存 |
| `clear(key)` / `clear_all()` / `size()` | 清除单个键（对持久化侧打墓碑，随下次 `sync` 删除）/ 全量重置（测试用） |
| `attach(prefs)` / `detach()` / `attached()` | 绑定/解绑持久化后端（非拥有）。`attach` 不做全量预热枚举——读取走懒回读，避免大配置文件在注入时被整体搬运 |
| `sync()` / `pending_writes()` | 把待落盘改动（值 + 墓碑）**批量**写穿 `Preferences` 内存；文件落盘仍由 `Preferences::flush()` 决定。未 `attach` 时为空操作且不丢内存值。同键反复写只占 1 条待落盘（按**键数**而非滚动次数付费） |
| `Scope`（RAII） / `current_scope()` | 多窗口作用域：内部键为 `scope + 0x1f + key`；同值重复构造走零分配快路径 |
| `claim(key, owner)` / `release` / `owner_count` | 键认领：同一键被**不同持有者**认领时提示一次（`Diagnostics::warn`），运行语义为「后写覆盖、最后活跃者为准」 |

**数据落点**：`Preferences::group("scroll_positions")` 下，键为作用域化后的完整键。

**多窗口隔离**：`BuildContext` 不携带窗口标识，故由 `WindowHost::render_frame` 在渲染入口构造 `ScrollStorage::Scope`（作用域 = 本窗口 id 的字符串形式）；无头 / 无 `WindowHost` 的路径（`TestController`、golden、单窗口用法）作用域为空，退化为全局单桶，行为与不隔离时一致。

**恢复时机**：控件在**首次可滚动布局**恢复（`Scroll` 判 `content_h_ > viewport_h_`，虚拟化控件判 `max_scroll_offset() > 0`）——不可滚动时保持等待，避免先夹到 0 后真正可滚动时再也恢复不了；恢复只生效一次，用户主动滚动后不再被回拉。控件侧契约见 [`04-widget.md`](04-widget.md) §3.3。

**示例与验收**：`examples/demos/demo_scroll_restore.cpp`、`tests/integration/itest_scroll_restore.cpp`。

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

`FrameStats`（`app/perf_overlay.h`）的读数为**方法**：`fps()` / `avg_frame_ms()` / `worst_frame_ms()` / `jitter_ms()` / `percentile_ms(p)`（`p ∈ [0,1]`，任意百分位帧时间）/ `dropped_frame_count()` / `dropped_frame_ratio()` / `hitch_count()` / `idle_frame_count()` / `total_frames()` / `frame_budget_ms()` / `is_stale()` / `stale_duration_ms()`（后两者为停帧陈旧语义，见 `ARCHITECTURE.md` §10.1），以及 `layout` / `paint` / `present` 三相位环形缓冲（`avg_layout_ms()` / `avg_paint_ms()` / `avg_present_ms()`）。

`aurora::debug::perf_snapshot()`（§11.2）把上述方法映射为 **JSON 键**输出——键名与方法名不同：`p50_ms` = `percentile_ms(0.5)`、`p99_ms` = `percentile_ms(0.99)`、`dropped_frames` = `dropped_frame_count()`、`dropped_ratio` = `dropped_frame_ratio()`、`hitches` = `hitch_count()`、`idle_frames` = `idle_frame_count()`，另含 `fps` / `avg_frame_ms` / `worst_frame_ms` / `jitter_ms` / `total_frames` / `frame_budget_ms` 同名键与 `perf_log` 子对象。引用读数时勿把 JSON 键当成 C++ 成员。

`PerfOverlay` 以 `stats_line3()` 展示 `wakeups_per_sec()` / `sleep_ratio()` 等帧节流读数。

---

## 11 调试门面（`aurora::debug`）

`aurora::debug`（头 `include/aurora/debug/debug_backend.h`，经 `aurora.h` 单一入口暴露）为真实后端调试能力提供统一薄封装入口，不搬迁任何生产子系统引擎（Inspector / Diagnostics / perf 留原地）。其中**调试能力函数**门控 `AURORA_ENABLE_DEBUG`：Release 下不产出调试代码——`capture` 返回 disabled 错误、`surface_state` 返回 unavailable JSON；**输出目录 API**（`set_output_directory` / `output_directory` / `resolve_output_path`）与 **feature 宏运行时查询**（`feature_flags` / `feature_flags_json`，声明于独立头 `include/aurora/debug/feature_flags.h`，见下表）无调试内部依赖，不受该门控、始终可用（见下表与「门控与 ODR 安全」段）。

| 能力 | 说明 |
|:---|:---|
| `CaptureSource{ Framebuffer, OnScreenWindow }` | 截图源。`Framebuffer` = 软件帧缓冲（全后端通用、确定性）；`OnScreenWindow` = 真实屏幕窗口（含 OS 装饰，Wayland / Headless 不支持） |
| `capture(Surface&, path, src = Framebuffer) -> Result<bool>` | 自动建父目录后转发 `Surface::save_snapshot` 或 `Surface::capture_window` |
| `set_output_directory(dir)` / `output_directory()` / `resolve_output_path(path)` | 输出目录 API（无调试内部依赖，始终可用）；默认 `current_path()/aurora_debug` |
| `feature_flags() -> FeatureFlags` / `feature_flags_json() -> Json` | 编译期 feature 宏开关运行时查询（始终可用，编译期常量快照）。强类型结构体字段 + JSON 导出（键 = 完整宏名）。C++ 侧宏镜像单点收口于 `src/aurora/debug/feature_flags.cpp`，应用代码零 `#ifdef`（需求 SPEC.PLATFORM.ZERO-IFDEF.001） |
| `surface_state(const Surface&) -> Json` | `width` / `height` / `scale_factor` / `frame_count` / `clear_color` / `should_close` / `has_native_window`。`frame_count` = 该 Surface 的已上屏帧数，各后端在自身 `present()` 内自增：Headless / Win32 / Glfw / D3D11 / X11 / Wayland / WASM 与三个 wgpu 型有值，仅 macOS 未覆写故恒报 0（基类默认），跨后端比对读数前须先确认该后端是否计数 |

**门控与 ODR 安全**：API 头**始终声明**，调试能力函数的 `.cpp` 体按 `AURORA_ENABLE_DEBUG` 裁切（例外：输出目录三函数的定义不裁切、无条件编译，与「始终可用」一致）；`Surface::save_snapshot` / `capture_window` 默认实现按运行时 `data()` 判空（宏无关），后端专属截图体门控。两函数在 `Surface` 上**始终声明**（vtable 槽稳定，属 `Surface` 契约）。Win32 家族两路（`Win32Surface` GDI 上屏 / `D3D11Surface` GPU 上屏，共用 `Win32Host` 宿主，经共享 `detail::capture_window_by_hwnd` 走 PrintWindow 路径）、X11、GLFW 在 `AURORA_ENABLE_DEBUG` + 对应后端下覆写 `capture_window`（GLFW：Windows 经原生 HWND 走 PrintWindow 含非客户区；X11/Wayland/Mac 走 GL 帧缓冲读回——软件路径先重放上一帧再 `glReadPixels`，GPU 路径经 `GpuGlRhi::read_pixels`，得客户区 framebuffer 尺寸画面，须在某次 present 之后调用）；Headless/Wayland 保持 unsupported（Wayland 客户端无法截图，属安全限制）。

`Surface` 另新增虚函数 `framebuffer_size()`（默认返回逻辑 `size()`）：`save_snapshot` 以它作 PNG 宽高；按 DPI 物理分辨率分配缓冲的后端须覆写返回物理像素，避免缩放比 ≠ 1 时 PNG 尺寸与像素数据错位。`native_handle()` 为 `const`（只读查询），**基类默认返回 `nullptr`**；真实窗口后端必须覆写为宿主原生句柄——Win32 家族两路（`Win32Surface` GDI 上屏 / `D3D11Surface` GPU 上屏，共用同一 `Win32Host` 宿主）**均**覆写为 `hwnd()`。上表 `surface_state()` 的 `has_native_window` 即由它非空判定，漏覆写会让真实窗口后端**恒报 false**，故由 `utest_native_surfaces` 的 `windows_family_native_handle_contract` 类型级守门。

### 11.1 可视化调试叠层

`include/aurora/debug/debug_paint.h`（经 `aurora.h` 暴露），全部门控 `AURORA_ENABLE_DEBUG`，Release 下体为零开销 no-op。

| 成员 | 说明 |
|:---|:---|
| `DebugPaintFlags{ layout_guides, relayout_boundaries, layer_borders, repaint_highlight, overdraw }` | 运行时叠层开关（bool，默认全 false） |
| `set_flags(...)` / `flags()` / `any_flag_enabled()` | 全局开关读写 |
| `current_debug_frame()` / `bump_debug_frame()` | 调试帧计数（`repaint_highlight` 时序判定用） |
| `paint_debug_overlays(Painter&, const Widget& root, const Rect& root_bounds, const BuildContext&)` | 在 `Window::present_root` 全树绘制后调用，按开启的 flag 统一绘制 5 个叠层。**部分裁剪帧（`plan.clip_logical` 非空）时调用点位于 `push_clip` / `pop_clip` 裁剪域之内**，叠层与脏区同域——半透明引导框若在 `pop_clip` 后整窗叠加，会在脏区外对上帧已叠加过的像素二次混色，破坏「脏区外零改写」不变量 |
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

### 12.1 SPEC.PLATFORM.ZERO-IFDEF.001 零 \#ifdef 跨平台 + 插件式平台扩展

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

**版本常量**（`core/version.h`）：`AURORA_VERSION_MAJOR` / `MINOR` / `PATCH`（数字分量，CMake `project(VERSION)` 注入）、`AURORA_VERSION_SUFFIX_STR` + `AURORA_HAS_VERSION_SUFFIX`（semver 预发布后缀，来自 CMake 缓存变量 `AURORA_VERSION_SUFFIX`）、合成宏 `AURORA_VERSION_STRING`（完整 semver 串）。**库发布版本的单一事实来源是根 `CHANGELOG.md` 的 `currentVersion`**。

**平台扩展点（第二类：宿主管理的原生子视图）：** 除 `Surface`（图形后端）外，本层新增**第二类平台扩展点** `NativeViewHost`，用于嵌入**系统原生视图**——首例为 `WebView` 控件：把系统 WebView 引擎的窗口作为**原生子视图**挂进 aurora 窗口，其像素**不经 `Painter` 合成**。契约：

- 应用代码零 `#ifdef`：控件与宿主均无平台宏；平台差异全在 `src/aurora/webview/` 下的引擎实现源文件中，且经运行期 `dlopen` / `LoadLibrary` 绑定（缺引擎只降级、不改链接面）。能力经 `WebViewCapabilities` 与 `feature_flags().webview` / `.webview_webview2` / `.webview_webkitgtk` **运行期查询**（相关 `AURORA_ENABLE_WEBVIEW*` 宏 `SCOPE PRIVATE`，公共头零宏引用）。
- 几何：宿主在**非 idle 帧**按「控件窗口逻辑盒 × scale + CSD 内缩」换算后下发原生矩形（换算唯一落点 `to_native_rect`，先加内缩再乘 scale）。原生子视图**不参与** `Painter` 帧流，也不受 aurora 裁剪 / 重叠影响（由窗口系统叠加）。
- 可见性：交互模式经宿主授予原生可见（映射上屏并接收输入）；快照模式**恒不映射**，仅作像素来源。
- Linux 事件泵：引擎（GLib 主上下文）的 fd 经 `Surface::set_extra_event_source` 并入 `X11Surface` / `WaylandSurface` 的 `wait_events` poll 集合；未装原生视图时为空操作。宿主访问口是**线程局部**的（形态仿 `current_focus_manager()`），由 `WindowHost` 在渲染入口 `set_current`、帧尾置空。

**WebView 可用性矩阵（运行期归因）：** `UnsupportedPlatform`（如 Linux **Wayland**：不能把另一客户端的 GTK surface 作定位子 surface 附到宿主 `wl_surface`）→ 诊断 slug `webview-unsupported-mode`；`Disabled`（宏关闭）与 `MissingRuntime`（无 WebView2 运行时 / 无 WebKitGTK）→ `webview-runtime-missing`。两者修复动作相反，**不得混报**。

**Web / WASM 平台适配：**

| 方面 | 做法 |
|:---|:---|
| 事件循环 | **已落地（rAF 驱动）**：浏览器主线程不可阻塞，`Application::run()` 在 WASM 构建下不进入同步 while——经 `emscripten_request_animation_frame_loop` 把统一帧循环挂到 rAF/vsync（蹦床 `Application::raf_tick` 每拍执行帧序步骤 1–7，不调 `wait_once`，帧节拍由浏览器承担），`run()` 注册完即返回。生命周期三件套：① `App::run()` 的 `launch` 助手在 WASM 下把 `Application` 交函数级 `static unique_ptr` **堆持至页面生命周期**（main 返回后栈对象必悬空）；② `inline static raf_owner_` 守卫——蹦床先比对 owner 再触碰 `user_data`，实例析构（`~Application` 摘除 owner）即令旧回调下一拍自停；③ 宿主无 `requestAnimationFrame`（如裸 Node）时探测回退同步阻塞循环并 ERROR 申报。`WasmSurface::wait_events` 保持空体（rAF 模式帧循环不调它）。真机验收：`tools/verify/wasm_raf_live_probe.cpp`（渲染/点击/async 续体三项，页面壳 `wasm_raf_shell.html` 提供 `<canvas id="aurora-canvas">`）。**注意**：rAF 模式下 main 立即返回，帧回调引用到的应用侧状态必须活在堆上（`shared_ptr`/`static`），引用捕获 main 局部变量即悬空栈写。 |
| 线程模型 | **已落地（deferred 排空 + 可选真并行）**：Emscripten 无 `-pthread` 构建（未定义 `__EMSCRIPTEN_PTHREADS__`，即 `AURORA_CAP_THREADS == 0`）下 `ThreadPool` 自动进入延迟排空模式（`core/thread_pool.h`，见 [`01-core.md`](01-core.md) §6.1）——不创建任何 `std::thread`，`au::async` / 协程任务只入队，由 `Application::step_frame()` **帧尾**（步骤 7，见 §2.4）调用 `pump()` 在主线程排空，随帧回写不丢任务；排空点不在 `present()`，故空闲跳帧帧也照排。真并行口径经构建开关 `AURORA_ENABLE_WASM_PTHREADS`（默认 OFF）：开 `-pthread` 后 `__EMSCRIPTEN_PTHREADS__` 与 `AURORA_CAP_THREADS` 同翻，回到普通 worker 池语义——代价是产物要求 SharedArrayBuffer，**宿主页面必须跨源隔离**（COOP `same-origin` + COEP `require-corp`），否则 wasm 实例化即失败（裸 Node 无此约束），故默认关闭。Web Worker + `postMessage` 的无共享内存并行执行仍为需求愿景、无接线。deferred 下 `future.get()` 与 `pump()` 同线程互等会自锁 |
| 渲染 | 渲染目标为 `<canvas>` 元素，内部自动选择。canvas 定位入参统一按 DOM id 理解（`"x"` 与 `"#x"` 等价）：上屏 `getElementById` 用裸 id，事件注册/尺寸查询用 CSS 选择器形态——Emscripten HTML5 事件目标经 `querySelector` 解析，裸 id 不带 `#` 会**静默注册失败** |
| 音频设备后端 | **已落地（Web Audio 推式环）**：浏览器对位后端经 `AURORA_ENABLE_AUDIO_WEBAUDIO` 编入。与桌面的根本差异是**无设备线程**——JS 调不进 wasm，故 C++ 在主线程每 20ms 把图渲染成帧推入线性内存定长环、JS 按头尾地址消费（零链接标志、零导出符号）；协商采样率取 `ctx.sampleRate`，声道契约恒 stereo 由浏览器上混。**自动播放闸门**：`start()` 返回 true 只表示设备在收样，**不等于出声**——未手势时上下文 `suspended`、图时钟如实冻结，有用户交互后由排空拍限速 `resume()` 开闸（真机四判据由 `tools/verify/wasm_audio_cdp_drive.mjs` 派发）。采集（`getUserMedia` 异步权限流）首切片未接线 ⇒ `create_microphone_source()` 显式报错不静默降级。契约与延迟/欠载口径全文见 [`03-layout-render.md`](03-layout-render.md) §9.4 |
| 限制 | 不支持高频指针直通路；`capabilities()` 声明此限制 |

应用代码使用相同的 `au::async` / `co_await` API，无需 `#ifdef`。

### 12.2 SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 跨平台一致行为 + 黄金文件验证

**核心目标：** AI 无需考虑平台差异。

- 布局引擎**自研**（不依赖平台布局），保证像素级一致。
- 字体渲染统一（自带文本整形引擎，见 [`03-layout-render.md`](03-layout-render.md) §8.2）。
- 事件模型统一（触摸 / 鼠标 / 键盘统一抽象）。
- 配套**跨平台布局测试套件**与**黄金文件验证**，确保同布局在各平台结果一致。
- AI 生成的代码无需为不同平台微调。

**验收标准：** 同一棵树在各后端下产出相同的逻辑快照（Level 1 / Level 2）；黄金文件比对零差异。逻辑快照黄金文件由 `utest_offscreen` 承担（11 个固定尺寸布局场景对 `tests/golden/logical_snapshots.json` 逐字段比对，见 [`03-layout-render.md`](03-layout-render.md) §10.1）；像素级 golden 由 `itest_image_view` / `utest_offscreen` 对 `tests/golden/*.png` 比对。
