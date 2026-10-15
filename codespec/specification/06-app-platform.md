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

`include/aurora/net/blocking.h` + `src/aurora/net/socket.cpp` 是全仓**唯一**的 Winsock2 / BSD socket 适配落点（CHG-006）：公共头零平台 socket 头，`#include <winsock2.h>` / `<sys/socket.h>` 只允许出现在该 `.cpp`（验收 grep 判据见 [`changes/winsock-bsd-socket-layer/proposal.md`](../changes/winsock-bsd-socket-layer/proposal.md)）。范围收敛为「Inspector 工具链 + 本机 AI 交互」所需：**地址族由 `Endpoint` 决定**（IPv4 / IPv6 同一条 `connect` / `bind` 通路，`*_loopback` 两枚工厂仍固定绑 `127.0.0.1`，都不经 DNS）、同步阻塞 + 显式超时、失败一律 `Result<T>` + 表驱动 `net-*` 错误码（`ERROR_CATALOG.md`，message 携带 OS 原生码）。

该同步面按提案 [`changes/xplat-network-stack/proposal.md`](../changes/xplat-network-stack/proposal.md) §4 归位为 `aurora::net::blocking` 子命名空间，**仅供非 UI 线程的工具与测试宿主**（`InspectorServer` 工作线程、`tools/` 服务器与探针、单测里的回环桩）——面向使用者的路径一律走 §8.8.11 起的异步传输族，阻塞语义不得再渗回 UI 侧。迁移期内 `include/aurora/net/socket.h` 只剩转发别名（见 §8.8.16）。

两个类型（外加一枚库内细节）：

- `detail::Session`（`src/aurora/net/detail/runtime.h`，**非公共面**）：作用域式运行时 RAII。Windows 腿引用计数配对 `WSAStartup`/`WSACleanup`（首个构造初始化、末个析构清理，嵌套/并存安全）；POSIX 腿零成本占位、`ok()` 恒真。不可拷贝/移动（纯作用域 guard）。公共面不再要求使用方自备它：`aurora::net::blocking` 在创建任何句柄前先钉住一枚**进程级、只申请不归还**的运行时引用（`src/aurora/net/socket.cpp` 的 `runtime_pinned()`，理由见 §8.8.16），引擎与传输层则各持一成员随自身生命周期收敛。
- `blocking::Listener`：`listen_loopback(port, backlog)`（只绑 `127.0.0.1`；port=0 由系统分配、`port()` 回报实际值）、`bind(endpoint, backlog)`（地址族取自 `Endpoint`，绑非回环地址即接受跨机连接，**访问控制交回调用方**）、`wait_readable(timeout) -> Result<bool>`（select 可中断等待，`false` = 纯超时非错误，两平台 nfds 差异内部消化）、`accept() -> Result<StreamSocket>`、`native_handle()`（借出原生句柄、不转移所有权，交 `wait_ready`/事件源注册口并入等待集）、`close()` 先 `shutdown(both)` 再关（唤醒并终止另一线程阻塞中的等待，`InspectorServer` 停机场径）。
- `blocking::StreamSocket`：`connect_loopback(port)`、`connect(endpoint)`（地址族取自 `Endpoint`；端口 0、本机 IPC 形态、无地址占位都在发起系统调用前落 `net-endpoint-invalid`——**同步阻塞面按既有裁决只承载 TCP 流**，本机 IPC 归异步传输族（§8.8.15），把同一地址形态在两个面上做成两种结果比统一拒掉更难排查）、`set_io_timeout(milliseconds)`（**毫秒统一口径**：Winsock 收 DWORD 毫秒、POSIX 收 `timeval`，形态换算封闭在实现侧——按平台字节形态误传会把 5 秒执行成 5 毫秒，本层的存在理由）、`read_some(span) -> Result<size_t>`（**0 = 对端有序关闭**，与错误态严格区分；超时落独立码 `net-recv-timeout`）、`write_all(span)`（内部 ≤1MiB 分块写满，`send` 返回值窄化一次解决）、`is_loopback_peer()`（服务端访问控制判据：`127.0.0.0/8` 全段 + `::1` + IPv4 内嵌形态 `::ffff:127.x`）、`peer_endpoint() -> Result<Endpoint>`（`getpeername` 还原为值类型，地址族与端口都带回来）、`native_handle()`（借出原生句柄、不转移所有权，同 `Listener`）。

链接面：`ws2_32` 随核心 `aurora` 目标在 Windows 无条件 `PUBLIC` 传导（见 `BUILD_OPTIONS.md`），各消费者不再散挂。Emscripten 腿经其 BSD socket shim 编译，但浏览器运行时无真实监听语义，Inspector 相关用例在 wasm 构建下按 `AURORA_TEST_SKIP` 退避。回归：`utest_net_socket`（行为契约 + 迁移期别名同体）+ `utest_inspector_client`（超时平台形态）。

#### 8.8.1 底座值类型：端点与取消令牌（CHG-018 已落地部分）

`include/aurora/net/address.h` 与 `include/aurora/net/cancellation.h` 是通用网络栈的第一批落点。两者都不引入平台 socket 头，也**不改变**上一段所述同步传输面的当前范围（`*_loopback` 两枚工厂仍固定绑回环；地址族无关的 `connect` / `bind` 通路见 §8.8.6）。

- `Endpoint` / `AddressFamily`：一条字节流「去往或来自何处」的纯值类型。**解析与规范化由仓内手写、不经 `inet_pton`**——同一输入在 Win32 / POSIX / Emscripten 上必须给出同一结果与同一条诊断（§12.2 一致行为判据；交给 libc 等于把「哪些畸形串被谁容忍」交给各平台实现）。判据要点：IPv4 禁前导零（避开八进制歧义）；IPv6 支持 `::` 压缩与内嵌 IPv4 尾段，输出按 RFC 5952 规范化（小写、去前导零、最左最长零游程压缩，故 `0:0:0:0:0:0:0:1` 与 `::1` 判等）；方括号形态只服务 IPv6 且**不得内嵌端口**（`"[::1]:8080"` 直接拒）；zone id 仅在 link-local（`fe80::/10`）上合法；回环与通配判定**按解析后的地址族与二进制值**而非字符串（`"localhost"` 是名字、不是地址）；回环判据在 IPv6 上有**两条**：`::1`，以及按 RFC 4291 §2.5.5 **跟随内嵌 IPv4 段**的 `::ffff:127/8`——双栈监听口上的同机连接在内核里就登记成这个形态（WSL 腿实测 `getpeername` 回报 `::ffff:127.0.0.1`），只认 `::1` 会把本机连接误判成远端，`is_loopback_peer()` 一类判据随之全线失真，因此该规则唯一落点在 `Endpoint::is_loopback()`，同步面与异步面的对端判定都只做「取对端 → 问它」；本机 IPC 路径在值类型层就受 `AURORA_NET_MAX_LOCAL_PATH` 与 NUL 约束，但该上限取的是**最宽一条腿**的 `sun_path` 容量（108 与 104 两档），故它只是第一道闸：装配 `sockaddr_un` 时按本平台实际宽度再判一次，超出即在系统调用之前落 `net-endpoint-invalid` 并在 `detail` 里写明本腿上限（见 §8.8.15）。端口 `0` 在值类型里合法并表达「由系统分配」，语境合法性由 `bind()` / `connect()` 分别判——值类型不预判用途。默认构造的无效占位在 `is_loopback()` / `is_unspecified()` 上一律 false（全零字节不参与语义判定）。`is_multicast()` 与 `is_loopback()` 同源，也按解析后的地址族与二进制值判：IPv4 落 `224.0.0.0/4`（`RFC 5771` §2）、IPv6 首字节 `0xFF`（`RFC 4291` §2.7）；广播地址（`255.255.255.255`）**不是**组播，本机 IPC 路径与无效占位恒 false（判据供 §8.8.14 的组地址校验使用，不在值类型里做任何套接字动作）。
- `Cancellation`：异步网络操作的取消令牌，共享状态句柄（拷贝即共享同一状态，`shares_state_with()` 可断言）。三条契约：**取消不抛异常**，一律以 `Result<T>` 的 `net-cancelled` 抵达调用方；**钩子至多一次、且在调用线程同步执行**——引擎据此把「释放句柄」排在「投递取消结果」之前，顺序反了会让回调里就地重连撞上刚被占满的句柄表；**取消先于登记时钩子立即触发**，不留「登记即失效」的窄竞态（那类缺陷只在真实网络延迟下现形）。它与既有 `Task::cancel()` 的「回调不再触发」**刻意不同**：协程帧只能靠 `resume` 前进到 `co_return` 正常析构，「不恢复」等于泄漏帧。
- 错误码：`net-endpoint-invalid`（`message` 携 `detail=` 指明被拒原因）、`net-cancelled`（`retryable = true`，供重试与策略判定直接消费）。
- 回归：`utest_net_address`（解析、规范化、校验与判等黄金判据）、`utest_net_cancellation`（含跨线程钩子落点与钩子内重入不死锁）。

#### 8.8.2 事件源注册口（`aurora::app::EventSourceRegistry`，CHG-018 底座）

网络栈 L0 宿主集成层的通用就绪通知底座（`include/aurora/app/event_source_registry.h` + `src/aurora/app/event_source_registry.cpp`）：把「一个可等待句柄 + 就绪回调」汇成一张待等待表，供第三方非阻塞会话（libssh2、DBus 一类）与库自带的 poll 线程引擎共同复用。**缺省驱动方是 §8.8.4 的常驻引擎线程**（本库各平台腿的统一形态）；自带等待集的宿主调 `set_host_driven(true)` 接管，引擎随即只等自己的唤醒通道。提案 [`changes/xplat-network-stack/proposal.md`](../changes/xplat-network-stack/proposal.md) §2 的逐平台并入点与 §18 定案 1 的竞态契约仍然有效，但**仓内宿主不再各自并入**（理由见 §8.8.5 与提案 §16 第 12 项）。

- **平台无关且位于 `net/` 之下**：本 `.cpp` 不出现任何平台 socket/poll 头（`winsock2.h`/`poll.h` 唯一落点仍是 `src/aurora/net/socket.cpp`，CHG-006 不变量不破）。真正的「阻塞直到某些句柄就绪」经三枚可注入接缝表达——`set_poller(Poller)`（等待实现，由 net 引擎层或测试安装）、`set_dispatcher(Dispatcher)`（主线程投递实现，缺省为就地同步执行，与 `post_to_main` 无 poster 时的口径一致）、`set_change_notify(ChangeNotify)`（表变动与驱动方转变的通知，归常驻等待方独占，判据见 §8.8.4 投递纪律 ②）。自带等待集的宿主不装 `Poller`、改走 `sources()` 取快照 + `dispatch(ready)` 投递。无 fd 语义的腿（wasm）在**构造期**把 `event_source_supported()` 置假（编译期事实只落一处，公共头维持零 `#ifdef`），此后 `register_source` 一律回 `net-platform-unavailable`，`wait_and_dispatch` 在未装 Poller 时同码，均**不忙等、不假降级**。
- **句柄生命周期与三条竞态契约（§18-1，规范性）**：`register_source` 返回 move-only RAII 句柄 `EventSourceRegistration`，析构即注销且**幂等**、移动后源转空态、其析构不再触发二次注销。注销语义 = **停止产生新回调**：`dispatch` 在锁内只快照待投递闭包（`shared_ptr` 按值捕获令条目独立于注册表存活），**释放锁后**才逐个投递——因此已进入 `Dispatcher` 队列的一次尾批仍会抵达一次（回调载荷须由使用方以弱令牌保护宿主），但此后新的 `dispatch` 不再为该令牌产生回调。需连尾批一起作废另用 `cancel_pending()`（置条目的 `cancelled` 原子位），该调用与析构**互不隐含**：前者只作废尾批、不影响后续投递；后者只移除快照条目、不作废已入队尾批。回调内可安全再注册/注销（投递在锁外）。
- **令牌关联**：同 `handle` 可多次登记、各自持独立令牌与回调（`next_token()` 单调、从 1 起、0 留空态）；宿主等待回来后按令牌回填 `ReadySource`，注销后重登记取新令牌、旧令牌的迟到就绪不会误触发新条目回调。
- **进程级单例刻意不析构**：本表的存续期必须覆盖**整个静态析构阶段**。常驻引擎先构造完自己、再在 `ensure_started()` 里首次触到本表，按「构造完成逆序析构」的口径本表反而先死，而 `~PollEngine` 在退出路径上收敛线程时要经 `set_change_notify` 回收接缝——给一枚已析构的 `mutex` 加锁即未定义行为（MinGW 腿实测为退出时段错误）。表里只有 `std::function` 与计数，没有任何需要归还给操作系统的资源，故泄漏没有代价；把正确性寄托在「谁先被碰到」上才有代价。
- 错误码：复用 `net-platform-unavailable`（本次新增，`retryable = false`、`fix_category = platform_capability`；`hint` 指向 `Engine::capabilities()` 查询与能力矩阵）。
- 回归：`utest_app_event_source_registry`（有效句柄与 `-1`/不支持腿的拒绝、就绪回调「抵达次数 == 就绪次数」、原生宿主无 Poller 走 `sources()`/`dispatch()` 的通路、`unregister` 与 `cancel_pending` 在排程 Dispatcher 下的尾批分岔、移动不双重注销、同句柄重登记取新令牌、`host_driven` 两次转变都通知常驻方）。引擎侧的互斥落地判据见 §8.8.4 的 `utest_net_engine`。

**逐平台等待集并入 = 宿主可选项，仓内宿主一律走引擎线程**（裁决与理由见 §8.8.5；提案 §16 第 12 项）：本注册口对外提供的就是这条可选项——自带循环的宿主（含嵌入本库的第三方事件循环）装 `set_host_driven(true)` 后自行 `sources()`/`dispatch()`，其余腿由常驻引擎驱动，回调一律经主线程投递器回到 UI 线程。

#### 8.8.3 事件驱动就绪基元（`aurora::net::wait_ready`，CHG-018 底座）

`include/aurora/net/poll.h` + 实现在 `src/aurora/net/socket.cpp`：给一组 `{句柄, 关注方向}` 阻塞到部分就绪或超时的**多句柄、四向（读/写/错误/挂断）事件驱动等待基元**。它是 §8.8.2 注册口 `Poller` 接缝的具体实现来源（引擎与测试都把它包成 Poller 装入），也是未来 poll 线程引擎与第三方会话共用的一条底座——刻意与 `blocking::Listener::wait_readable`（单句柄、`select`、同步回环）分工：本基元**只做等待、不碰协议语义**，不知道句柄背后是 socket、管道还是 DBus 描述符。

- **平台 socket/poll 头唯一落点不破**：POSIX `poll` 走 `<poll.h>`、Winsock `WSAPoll`/`WSAPOLLFD` 走 `<winsock2.h>`，两者都只在 `src/aurora/net/socket.cpp` 出现（CHG-006 不变量继续成立；`<poll.h>` 本就在受限清单之外，但 `WSAPoll` 依赖 `winsock2.h` 故必须落在此 TU）。`net/poll.h` 零平台头。
- **就绪语义定见**：返回向量与入参 `PollDescriptor` **等长同序**（第 `i` 项描述第 `i` 句柄），第 `i` 项 `handle < 0` 时该槽恒全 false 且**不进系统调用**——一个坏句柄不毁整批等待（POSIX 负 fd、Windows 传 `INVALID_SOCKET` 会 `WSAENOTSOCK` 让全批失败，故排除而非透传）。`readable`/`writable` 仅在对应方向被 `want_*` 请求时才置位（`POLLIN`/`POLLOUT` 按请求上报），`error`（含 `POLLNVAL`）与 `hangup`（`POLLHUP`）无条件上报（内核这两态不受 `events` 请求约束）。**纯超时不是错误**：无句柄就绪时返回 OK + 全 false 向量，调用方据此轮询自有停止标志；仅系统调用真失败落 `net-wait-failed`（message 携 OS 原生码）。
- **「等某个方向」就只能请求那个方向**：一次等待里任一被请求的方向就绪即返回（这正是 poll 的语义），故同时请求读与写的等待在「对端尚未把字节送到位」时会立刻带着「可写、不可读」回来，超时窗口形同虚设。Linux 回环上这一点必然现形（数据落地可以晚于 `send()` 返回，而可写位当场就成立），Windows 的同步投递把它掩盖掉——`utest_net_poll` 的「写入后可读」在 WSL 腿据此改为只请求可读。调用方要等某方向，要么单独请求它，要么按电平触发自己重轮询（`net::Engine` 走的正是后者）。
- **超时口径**：`timeout` 是**单次系统调用**的上限，POSIX 被 `EINTR` 打断时以同一预算重新武装（不吞错误、不提前返回），Windows `WSAPoll` 无 `EINTR` 语义故单次调用；`timeout <= 0` 表示非阻塞立即回报当前就绪态。跨平台一致行为判据下不承诺单调时钟意义下的精确总时长。
- **`native_handle()`（同步面新增的借用出口）**：`blocking::StreamSocket`/`blocking::Listener` 各加 `native_handle() -> std::intptr_t`（`-1` 恒无效，与 `is_valid()` 同口径），把原生句柄借给 `wait_ready`、事件源注册口或第三方库（libssh2 一类）接管。约束：**非所有权转移**，仅在本对象存活且未 `close()` 期间有效，调用方不得自行关闭（双重关闭会误伤系统复用后的新句柄）。Windows 腿为 `SOCKET`、POSIX 腿为文件描述符。
- **Windows 前置条件**：`WSAPoll` 需活跃 Winsock 运行时；该前置由实现侧承担（句柄创建点先钉住进程级运行时，见 §8.8.16），调用方不再自备会话。运行时确实起不来时以 `WSAENETDOWN` 落 `net-wait-failed`。测试用 `WSAPoll` 历史不稳的两条路径（listener 待 `accept` 的 `POLLIN`、对端 `FD_CLOSE` 的 `POLLHUP`）不作断言判据，只锁「连接即可写 / 写入后可读 / 排空即纯超时」这三条两平台同色的语义。
- **Headless 基线（§2「先立无 GUI 行为基线」）**：把 `wait_ready` 包成 Poller 装入 `EventSourceRegistry`、登记真实回环句柄，即可在无 GUI 单测里逐值验证「注册 → 就绪回调抵达次数 == 就绪次数 → 析构注销后 `sources()` 清空、`wait_and_dispatch` 空转且回调不再抵达」——这是 §18-1 生命周期契约在真实 fd 上的落地证据。
- 错误码：复用既有 `net-wait-failed`，本切片无新增码。
- 回归：`utest_net_poll`（空集合 OK、无效槽不毁批、可写→可读→空闲三态、注册口经真实 fd 的端到端事件源循环）。

#### 8.8.4 引擎与常驻 poll 线程（`aurora::net::Engine` / `Capabilities`，CHG-018 底座）

`include/aurora/net/capabilities.h` + `include/aurora/net/engine.h`（实现 `src/aurora/net/engine.cpp`）：把 §8.8.3 的基元包成**常驻循环**并接上主线程完成投递，即提案 §2 的默认形态——「谁在什么线程、以什么节奏调用基元」就此定死。

- **`Capabilities` 是运行期唯一真值源**：9 枚具名 bool（`tcp` `udp` `local_ipc` `dns` `tls` `listens` `event_source` `streaming` `custom_headers`），缺省全 false。纪律：**能力位不得先于实现点亮**——`tcp` / `listens` / `event_source` / `streaming` / `custom_headers` / `udp` / `dns` 已随各自切片为真（后两枚表达的是平台限制而非实现进度，native 无 `ReadableStream` / CORS 约束）。`local_ipc` 是九枚里唯一**运行期探测**得来的位：本机 SDK 头是否提供 `AF_UNIX` 地址结构是编译期事实，而该地址族在运行时能否开出套接字是另一件事，两者在 Windows 腿上可以分开成立，故本位由 `src/aurora/net/detail/async_io.h` 的探测出口给出（进程内一次，结果缓存）。`tls` 由被装配进来的那个后端 TU 的库内探测口 `detail::tls_backend_available()` 回答（没有后端时 `src/aurora/net/tls_unavailable.cpp` 答假），与「`handshake()` 真的能成功」同源；它同样不是硬编码的平台宏，因为同一平台上系统栈与 provider 两条腿的装配结果可以不同。`event_source` 的运行期落点就是 §8.8.2 的 `event_source_supported()`，两处真相不重复登记。
- **`Engine` 是引擎的公共面**：`instance()` / `name()` / `capabilities()` / `post()` / `post_after()` / `running()` / `shutdown()`。`name()` 回报实现名（自带引擎 `"poll"`、无 fd 语义腿 `"none"`），供 Inspector 与日志区分。查询能力**不**拉起线程；惰性启动的触发者是 `post` / `post_after`（§2「随首个需要它的对象惰性启动」）。
- **循环节奏**：「跑已到期任务 → 快照 `sources()` → `wait_ready` → 交回 `dispatch()`」，一条线程承载任务队列与到期定时器，不为定时器另起线程。等待窗口 = `min(最早到期定时, 空闲上限 200ms)` 且**下限 1ms**（不忙等；200ms 只是「唤醒通知丢失」的延迟上界，正常路径由唤醒通道即时打断）。
- **唤醒通道刻意不引入新平台代码**：它是一对**回环 TCP 流**（`blocking::Listener::listen_loopback` + `blocking::StreamSocket::connect_loopback` + `accept`），两平台的 poll 族都认得 socket 句柄，因此 `engine.cpp` 不出现 `poll.h` / `winsock2.h`——CHG-006「平台 socket 头唯一落点是 `src/aurora/net/socket.cpp`」不变量继续成立。单轮只读一块（8 字节）且不计数：读只在 `POLLIN` 报可读后发生，故必不阻塞；残余字节让下一轮立刻返回可读。引擎另持一成员 `detail::Session`，随自身生命周期收敛运行时引用（§8.8.16 的归属 ②）。
- **投递纪律（三条，规范性）**：① `Dispatcher` 只在宿主未装时补 `aurora::detail::post_to_main` 形态（与 `Task<T>::set_main_poster` 同一条投递器，§1 不变量），宿主已装则不改；② `ChangeNotify` 归常驻等待方（即本引擎），登记与**注销**都要通知——注销后宿主随即关闭句柄，滞留快照会让下一次系统调用撞上无效句柄；③ **一条事件源只由一个驱动方等待**：常驻引擎循环与宿主自带等待集互斥，同时启用会让一次就绪投递两次。开关就是注册口的 `set_host_driven(bool)`（缺省假 = 引擎驱动），引擎每轮等待都读它，置真即让位、置回假即接管，两种转变都经 ② 的通知立刻生效，不必等一个等待窗口或先 `shutdown()`。本库各平台腿缺省全部走引擎驱动（宿主并改为可选项，理由见 §8.8.5）。
- **瞬态等待失败不终止循环**：注销与就绪同帧发生时本轮可能正持着含已关闭句柄的快照，`wait_ready` 落 `net-wait-failed` 属预期竞态；循环丢弃本轮结果并在下一轮重取快照（`utest_net_engine` 的 `unregister_during_wait_keeps_the_loop_alive` 即此判据）。
- **停机语义**：`shutdown()` 幂等，停止接收新任务、把**已入队任务与已到期定时跑完**再 join（不丢已承诺的工作），未到期的定时**丢弃且不执行**——静默跑一个过期续体会伪造出错误的完成时序；已投进主线程队列的完成事件由宿主退出路径排空（§2）。收敛后再次 `post` 会重新惰性启动。
- **退出收敛是显式的，进程级单例刻意不析构**：`shared_engine()` 以 `*new PollEngine()`（wasm 腿为 `NullEngine`）存储且不登记销毁函数，与 §8.8.9 里 `app::EventSourceRegistry` 的处置成对——常驻线程的收敛要加锁、`join`、关唤醒套接字、拆 `detail::Session`，而静态析构阶段这些原语已不可依赖：winpthread 的互斥体属于它自己的静态区，可能先于本单例拆除，MinGW 腿实测（进程只做一次 `post` 即返回）在 `PollEngine::shutdown()` → `detail::Session::~Session()` 的加锁处段错误。保留析构但不 `join` 更糟（`std::thread` 在 joinable 状态下析构即 `std::terminate`），改经 `std::atexit` 登记只是换到同一个拆除阶段的另一个登记处。代价是一枚不回收的进程内分配与一条随进程终止的线程；套接字与 Winsock 引用计数由操作系统在退出时收回。需要确定性收敛的调用方（宿主关闭流程、真机探针、集成用例的 `EngineGuard`）自己调 `shutdown()`，该路径不依赖拆除次序。
- **无 fd 语义腿**：wasm 装配 `NullEngine`（`name()=="none"`、能力全 false、`post`/`post_after` 一律 `net-platform-unavailable`），绝不假降级也不排队等位。
- 错误码：本切片**无新增码**，直接产出的只有 `net-platform-unavailable`（无 fd 语义腿、停机竞态拒收新任务）与 `net-runtime-failed`（平台网络运行时不可用）；唤醒通道构建腿的失败码由 §8.8 的同步回环原语**原样透传**（`net-socket-failed` / `net-bind-failed` / `net-listen-failed` / `net-connect-failed` / `net-accept-failed`），引擎不自造第二套口径。
- 回归：`utest_net_engine`（能力矩阵如实——`local_ipc` / `tls` 两枚与各自探测口同源而非硬编、查询不起线程、`post` 在非调用线程执行、空任务不占队列、`post_after` 不早于登记意图、常驻循环驱动真实事件源、注销同帧不毁循环、`shutdown` 幂等且可重启、`host_driven` 置真时引擎不再等待注册口条目而宿主自投一次、交回后引擎重新送达）。

#### 8.8.5 逐平台驱动形态（底座收口裁决）

底座四件均已交付：值类型（§8.8.1）、事件源注册口（§8.8.2）、就绪基元（§8.8.3）、常驻引擎与能力矩阵（§8.8.4）。原列在 §17 底座里的「把注册口**并入各平台宿主等待集**」不再作为仓内宿主的改造项，改为宿主可选项——`set_host_driven(true)` 之后自行 `sources()`/`dispatch()`。三条理由：

- **分层方向**：注册口属 L0 `app`，提案 §1 只许可 L1 `net` 消费它（「L0 投递口」）。X11/Wayland 的 `poll` 数组与 AT-SPI 桥都在 window 后端，让后端回头消费 `app` 会形成 app↔window 双向依赖；要保住方向就得在公共 `Surface` 面上加一枚由 app 注入的「额外待等待 fd + 就绪回填」钩子——换来的是省掉一次微秒级跨线程投递，代价是公共 API 面扩大。
- **Win32 形态冲突**：`MsgWaitForMultipleObjectsEx` 只等内核对象，socket 要进句柄数组须经 `WSAEventSelect`，而该调用把套接字置入**事件态**，与 CHG-006 既有的同步收发面（同一句柄上的阻塞 `recv`/`send`）互斥；64 句柄上限还会把宿主等待集的规模变成注册口的容量约束。引擎腿用 `WSAPoll`，两处耦合都不存在。
- **一致性本就是默认形态**：提案 §2 写明自带引擎的默认形态是「引擎自持工作线程……不依赖各平台主循环的等待形态差异」，逐平台并入只是同一能力的第二种实现，不是新增能力。

由此定下的运行期口径：**Win32 / X11 / Wayland / macOS / GLFW / Headless 一律缺省由常驻引擎线程驱动**（回调经主线程投递器回 UI 线程，唤醒走各腿既有 `request_wake`：Win32 `PostMessage`、X11/Wayland 唤醒管道、GLFW `glfwPostEmptyEvent`）；**wasm 无 fd 语义**，能力位假且注册即报 `net-platform-unavailable`。§18-1 的「64 句柄上限以运行期错误显式暴露」因此只约束**宿主驱动腿**（宿主自有等待集的规模由宿主负责），本库缺省腿不受该上限约束。AT-SPI 的 DBus watch fd 通道维持其在 window 后端的自有集成（它早于注册口，且是 main-thread-only 的无障碍关键路径），注册口的「首例消费者」改为 libssh2 一类第三方会话——那才是它的外部用途，且已用真实回环 fd 在 Headless 基线里逐值验证。

传输族的异步流核心、名字 / 隧道连接编排、分帧器与数据报已落地（§8.8.11—§8.8.14），本机 IPC 流形态与 `NativeOptions` 逃生舱随 §8.8.15 落地；`Resource` 绑定、`FakeEngine`、TLS、HTTP、可观测性、SSE、WebSocket 依 §17 顺序在后续切片落地（`Url` 与 `Headers` 两枚基础件已落地，见 [`01-core.md`](01-core.md) §10、§11；地址族无关化见 §8.8.6，代理配置侧见 §8.8.7，握手字节层见 §8.8.8，主机名解析见 §8.8.9，系统代理读取见 §8.8.10）。

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

- **畸形即失败，绝不静默直连**：`HTTP_PROXY=http//host` 若被当作「没配代理」，症状是「在办公网能通、在客户网络不通」，且日志里无痕。故 `parse()` / `from_environment()` 返回 `Result`，且失败 message 必带**是哪个变量**或**是第几条规则**（`detail=HTTPS_PROXY: ...`、`detail=exception entry #2 '...': ...`——条目语境不写死成变量名，因为同一套文法也服务于系统设置里的例外清单），运维定位不需要先复现。
- **scheme 决定怎么连代理，不决定目标是否 TLS**：`HTTPS_PROXY=http://proxy:3128` 是合法且最常见的形态（明文连代理、代理隧道到 https 目标）。代理自身走 TLS（`https://`）与 SOCKS4 属已知但不实现，报独立的 `net-proxy-unsupported` 而非 `net-proxy-config-invalid`——调用方要能区分「用户写错了」与「本库不支持」；两者都**不退化成明文跳**，否则代理链路上的凭据会被送到错误信道。
- **决策只有一处**：`ProxyEnvironment::for_target(host, port, secure)` 是唯一入口，次序为「例外优先 → 安全目标 `HTTPS_PROXY`→`ALL_PROXY`→直连 / 明文目标 `HTTP_PROXY`→`ALL_PROXY`→直连」。安全目标**绝不**回退到 `HTTP_PROXY`：两个变量之间的隔离是运维的契约，把它猜平等于让企业网按直觉分流。缺省端口沿用 curl / Go 口径（CONNECT 80、SOCKS5 1080），裸 `host[:port]` 按 CONNECT 处理。
- **主机文法只有一套**：代理主机与例外项的规范化一律借 `Url::from_parts` / `Endpoint::from_ip` 的既有出口（ASCII 小写、punycode、IPv6 去括号、去尾点），本层不新造第二套主机文法，否则同一台代理会有两种拼写却判不等。
- **`NO_PROXY` 是封闭文法**：只接受 `*`、域名（`example.com` / `.example.com` / `*.example.com` 三写法等价，匹配本体与任意深层子域）、数字字面量（**全等**匹配，不做后缀——否则 `.0.0.1` 会把无关主机卷进例外）、CIDR（`10.0.0.0/8` / `fd00::/8`，主机位非零即拒），以及上述形态后跟 `:端口` 的限定；分隔符 `,` `;` 与空白等价、空项忽略。IPv6 项必须带括号（不带括号时 `:` 无法与端口限定区分）。CIDR 只与**数字字面量**目标比较——名字型目标在本层不做解析，口径与 Go / curl 一致。另接受两枚**系统代理专属令牌**：`<local>` 折算成「无限定域主机名」规则（名字里既无 `.` 也不是数字字面量——`<local>` 的意图正是「本机所在的无边局域网」，带点的主机不属它），`<-loopback>` 折算成 `127.0.0.0/8` 与 `::1/128` 两条前缀。二者都不是主机名，直接丢进域名文法会得到 `Rule::Kind::Domain` 的空壳或畸形报错，故在文法层单独建枚举形态而非事后特判。
- **凭据只在内存里**：userinfo 解析时立即百分号解码（密码里的 `%3A` 与用户名里的 `%40` 都保持原义，拆分只认第一个 `:`），`to_string()` 恒把密码替换为 `***` 且 IPv6 代理重新补括号（渲染结果必须可回解析）；诊断字段 `source()` 不参与相等比较，故「环境变量来的配置」与「代码里同值配置」判等。

环境变量读取器 `ProxyEnvironment::Reader` 是本件的注入接缝：每个变量先大写后小写（`HTTP_PROXY` 然后 `http_proxy`），空串视为未设置（CI 与容器里的显式清空写法），单测因此无需真实环境变量即可覆盖全部次序与例外形态。系统代理读取（Win32 WinHTTP 查询、macOS 系统配置，且只取地址与例外、不执行 PAC）见 §8.8.10；代理自身的 TLS 属非目标。

- 错误码：新增 `net-proxy-config-invalid` 与 `net-proxy-unsupported`（category `io`、`retryable=false`，数据源 `codespec/errors.toml`，`ERROR_CATALOG.md` 已随生成器刷新）。§10 预留的 `net-proxy-unreachable` / `net-proxy-auth-required` 待握手切片按真实失败分支启用，本件不预支。
- 回归：`utest_net_proxy` 十例——形态与缺省端口、七类畸形值与两码分岔、凭据解码与打印掩码、结构化工厂的入参校验、域名与端口限定匹配、字面量全等与 CIDR 前缀（含 `/0` 与 `fe80::/8` 越界样本）、封闭文法的十一类拒收与「第几条」定位、大小写优先级与变量名回报、`for_target` 的完整选择次序、`source()` 不参与判等。全用例纯逻辑，三平台一致，wasm 腿同样可跑，故无 `AURORA_TEST_SKIP`。

#### 8.8.8 代理握手字节层：CONNECT 与 SOCKS5 的编码、解析与落码

`src/aurora/net/detail/proxy_wire.h` + `src/aurora/net/detail/proxy_wire.cpp`（命名空间 `aurora::net::wire`，仅库实现可见，不属公共 API）把 §11 代理族的**握手字节**从传输里剥出来：零 socket、零平台头，产出待写的字节、消费已读的字节。「代理不可达绝不回退直连」这条禁令要能被逐值验证，前提就是它不依赖一个真实代理进程；反之把字节写进套接字、把结果翻成调用返回值属传输侧。六条定见：

- **解析是三态而非两态**（`Result<std::optional<T>>`）：有值 = 报文完整，成功且空 = 还没读够（调用方据此继续喂字节），失败 = 报文不合文法或超出头部上限。「没读到」不是错误——一次 `read()` 返回半个应答是常态，把它报成故障会让传输层写不出正确的重试循环。
- **状态字节到错误码的换算只有一处**（`connect_tunnel_error` / `socks5_error_for`）：CONNECT 的 `407`、SOCKS5 方法协商的 `0xFF`、用户名/密码子协商的非零状态 → `net-proxy-auth-required`；CONNECT 其余非 `2xx`、SOCKS5 的可达性类 `REP` → `net-proxy-unreachable`（同批把 §10 预留的这条码启用，**不回退直连**）；代理选用了本库不实现的形态（方法 `3` 的 TLS 协商、`REP` `X'07'` 命令不支持与 `X'08'` 地址类型不支持）→ `net-proxy-unsupported`；**这两值初稿记成 8/9**，那是把 `RFC 1928` §6 枚举表的下标当成了取值，落 §8.8.12 的逐阶段握手用例时按该节逐值核对纠正为 7/8。其余非零 `REP` 仍归可达性码：`X'01'` 服务器通用失败、`X'02'` 规则不允许、`X'03'` 网络不可达、`X'04'` 主机不可达、`X'05'` 连接被拒、`X'06'` TTL 超时，`X'09'`—`X'FF'` 该节明写「unassigned」，同样按可达性失败回报而不谎称本库不支持某个代理形态；报文不合文法或头部块越上限 → 本件新增的 `net-proxy-protocol`。「代理拒绝建隧道」与「代理不会说话」是两类故障：前者查代理配置与上游链路，后者多半是端口上坐着一个普通 HTTP 服务或强制门户（captive portal），混成一码会让排查方向错一半，故分码。同一个非零字节在不同阶段含义不同（`3` 在方法协商是「TLS 协商」，在子协商里是「失败」），所以 SOCKS5 侧的换算入口带阶段入参。
- **`ATYPE` 只由「解析权是否交给代理」决定**：`socks5h`（配置侧的 `resolves_via_proxy()`）走域名形态 `ATYPE=3`（长度 1..255）；未授权代理解析时本层不做 DNS，目标必须是数字字面量并按族走 `ATYPE=1` / `ATYPE=4`，给名字即 `net-endpoint-invalid`——把名字塞进一个未被授权解析的代理是不可见的行为差异，宁可当场报错。IPv6 作用域号在 SOCKS5 地址编码里没有字段可放，同样报错而不丢 zone（丢了就等于连到另一条链路上去）。
- **凭据只出现在写出的字节里**：CONNECT 的 `Proxy-Authorization: Basic` 就地按 `RFC 4648` §4 组装 base64，SOCKS5 子协商按 `RFC 1928` §3 的 `VER=1` 长度前缀形态；两型的诊断文本一律只带代理与目标的 `host:port`。用户名或密码超出单字节长度前缀（255）落 `net-proxy-config-invalid`——凭据没处安放是配置问题，不是网络故障。
- **头部块有上限**（`AURORA_PROXY_MAX_HEAD_BYTES = 16 KiB`）：不设上限时一个持续不发空行的对端会把读缓冲无界撑大，那是拒绝服务而不是兼容性。分行同时接受 CRLF 与裸 LF（本层只读不回写，为一条换行差异把可用代理判成畸形不值得）；原因短语可缺省。
- **字节读取一律经无符号八位组跳转**：`std::string_view` 的元素是带符号 `char`，`static_cast<std::uint8_t>` 会把 `0x90` 符号扩展成 `0xFF90`，而端口、长度前缀与方法号正是这类字节——这是 SOCKS5 绑定端口读回 `65424` 而非 `8080` 的根因，故本文件所有字节读取共用一枚 `octet()` 出口。应答解析只吃掉自己的长度（`header_bytes` / `consumed`），其后的字节属于隧道；误食首字节会表现成「隧道建立后第一条请求随机失败」，最难排查。

本层不证明「隧道真的通了」（那需要一个假代理进程与真实字节往返），该判据随传输切片兑现。

- 错误码：新增 `net-proxy-protocol`（category `io`、`retryable=false`），同批启用 §10 预留的 `net-proxy-unreachable`（`retryable=true`，代理侧瞬时故障值得重试）与 `net-proxy-auth-required`（`retryable=false`，凭据缺失盲目重试只会刷对端日志）；数据源 `codespec/errors.toml`，`ERROR_CATALOG.md` 与生成头随生成器刷新。
- 回归：`utest_net_proxy_wire` 七例——CONNECT 请求行的 authority-form 与 IPv6 补括号、Basic 凭据逐字节（含手算 base64 与「明文密码不出现在请求里」的反assertion）、应答头部块三态（未出现空行为缺失态、`header_bytes` 恰好停在隧道首字节、裸 LF 形态、缺原因短语）、九类畸形状态行与越上限（上限值进诊断文本）、SOCKS5 方法协商与子协商的逐字节形态（含版本字节与三例凭据长度越界）、`ATYPE` 由解析授权选路（域名 / IPv4 / IPv6 逐字节，名字未授权、256 字节域名、带作用域号三类拒收）、应答的可变地址长度（IPv4 尾随隧道字节仍只吃 10 字节并还原绑定端点、IPv6 22 字节、域名型不还原端点、未知 `ATYPE` 与错版本落 `net-proxy-protocol`、非零 `REP` 仍解析成功）、握手状态到三枚错误码的分岔（`REP` 按 `RFC 1928` §6 逐值断言，含上条纠正后归可达性码的 `X'09'`）。全用例零 socket、零平台头，三平台与 wasm 一致，故无 `AURORA_TEST_SKIP`。

#### 8.8.9 主机名解析：`aurora::net::Resolver`（双栈与竞速切片第二件）

`include/aurora/net/resolver.h` + `src/aurora/net/resolver.cpp` 把「一个主机名 + 一个端口」变成「一组可直接连接的 `Endpoint`」：`lookup()` 是同步形状（供引擎线程与测试逐值断言），`resolve(host, port, ResolveDone)` 是面向使用者的异步形状——阻塞式 `getaddrinfo` 发生在 §8.8.4 的常驻引擎线程上，完成经 `aurora::detail::post_to_main` 回到主线程。平台查表的唯一落点仍在 `src/aurora/net/socket.cpp`（`net::detail::platform_lookup`，CHG-006 的「平台 socket 头唯一落点」不变量继续成立；`<netdb.h>` 与 `getaddrinfo` 不新增受限头之外的落点）。六条定见：

- **数字字面量永不查 DNS，且短路点在注入接缝之前**：`Endpoint::from_ip()` 认得的形态（IPv4 点分、带括号 IPv6、含 zone id）直接产出单条答案，既不让一次网络往返去解析一个自足的字面量，也让测试能同时证明「字面量这条路没走 DNS」——接缝装在短路之后就证不出这一点。
- **异步形状用回调而不是 `Task<Result<…>>`**：解析器在 L1，`Task` / 协程在 L4（`state/async.h`），把 `Task` 写进 `net/` 公共头会让下层反向依赖上层。因此公共头只带 `ResolveDone`，`state/async.h` 的引用只出现在 `resolver.cpp`（实现侧消费 `post_to_main`，与 `Task<T>` 同一条主线程通道，不新增第三种排队形态）。协程包装随绑定层（`Resource`）在上层给出。
- **查表参数只认数字，且不做「本机才有」的过滤**：`AI_NUMERICSERV` 让服务名不参与解析（`/etc/services` 的名字表按机器而异，把它牵进来会让同一份代码给出不同端口）；**不启用 `AI_ADDRCONFIG`**——该标志按「本机当前是否有 IPv6 地址」过滤回报，于是同一份代码在纯 v4 与双栈机器上拿到不同次序与条数，与跨平台一致行为判据冲突；一族实际不可达由连接尝试本身暴露（竞速切片据此换族）。回报也不做排序：地址族偏好属连接策略（Happy Eyeballs 的 200 ms 交错随竞速切片落地），解析层排序会把那个策略拆成两处。缓存同样不在本层——一份带 TTL 的地址缓存是另一个子系统的职责，本层留 `lookup()` 这个无状态出口给它复用。
- **产出恒规范化且去重**：结果经 `endpoint_from_address()` → `Endpoint::from_bytes()`，与文本形态共用同一套 RFC 5952 规范化渲染（不经 `inet_ntop`）；「同族同地址同端口」只留首次出现的一条，且**保持首次出现次序**——列表规模恒为个位数，故用线性判重而非哈希，哈希会把这条唯一事实变成不确定项。
- **三枚码的分岔是排查方向的分岔**：空主机名落 `net-endpoint-invalid`（用法错误不得报成网络故障，否则 `net-dns-failed` 的 `hint` 会让调用方去重试一个永远不会成功的入参）；查表失败与「查成功但零条可用记录」都落 `net-dns-failed`（后者等同不可用，message 必带主机名）；异步排队失败（引擎起不来）把引擎的原因原样带进回调并冠以排队说明，**不静默悬挂**——一次永不触发的回调是本类最难定位的缺陷。
- **接缝只存在于库内**：`net::detail::lookup.h` 的 `set_lookup_hook()` 让全部次序与形态判据在无 DNS、无网络的环境下逐值可断言；它刻意不进公共 API，因为「一个会改变解析结果的公开开关」本身是安全隐患（`getaddrinfo` 结果被替换等于替换了目标主机）。真实平台腿仍有一例：`*.invalid` 是 RFC 6761 保留域，合规解析器必须回 NXDOMAIN，而完全离线的机器同样回失败，故该判据不依赖网络可达性，也不需要 `AURORA_TEST_SKIP`。

- 错误码：新增 `net-dns-failed`（category `io`、`retryable = true`、`fix_category = resource_error`；`hint` 指明数字字面量永不到达本码，故 DNS 本身是被测对象时直接用字面量）。
- 回归：`utest_net_resolver` 七例——字面量在钩子之前短路（v4 / `[::1]` / 带 zone 的 `fe80::1%2`，钩子调用次数须保持 0）、空主机名判成用法错误、去重且保首次次序（同址不同端口算独立条目）、查表失败与空回报同为 `net-dns-failed`（后者诊断含主机名）、真实平台腿对保留域失败、异步完成恰一次且查表腿与回调都不在调用线程上、空回调连查表都不发起。异步用例以 RAII 收敛引擎（`Engine::shutdown()` 幂等），不把在跑的线程留给同 runner 的后续用例——常驻循环与用例自有的事件源驱动方同时等同一注册口会造成二次投递。

#### 8.8.10 系统代理读取：`ProxyEnvironment::from_system()`（代理族第三件）

§11 代理族的「操作系统里配了什么」这一支。纯换算与平台采集分成两半：`src/aurora/net/detail/system_proxy.{h,cpp}` 把 `SystemProxySnapshot`（五个字段：手工开关、服务器表、例外清单、PAC URL、WPAD 开关）压成 `ProxyEnvironment`，零平台头、可逐值断言；`src/aurora/net/system_proxy.cpp` 是**本仓唯一的系统代理 API 落点**（Win32 走 `WinHttpGetIEProxyConfigForCurrentUser`、macOS 走 `SCDynamicStoreCopyProxies`，其余腿回空值），出口类型只有那个快照。七条定见：

- **PAC / WPAD 一律报 `net-proxy-unsupported`，绝不回空集合**：这两类配置的意图就是「流量须经代理」，把它读成「没配」会让办公网流量直连出去且无痕——正是 §11 立「代理不可达不静默回退直连」时要防的那类故障。判定先于手工开关，PAC 与手工项并存时同样不能假装手工项是全部意图。执行脚本本身属非目标（要 JS 引擎），本件只把「这里有个脚本」如实报出来，诊断文本带上脚本 URL。
- **「读不到」与「没配」在类型上就是两件事**：本腿没有系统代理概念（Linux / wasm）→ 回空快照 → `from_system()` 报 `net-platform-unavailable`；通道存在而被拒（注册表访问失败、动态存储无回报）→ 新增的 `net-proxy-config-unreadable`（`retryable = true`）。两者都**不**折算成空集合，否则 `from_environment_or_system()` 会把「策略封锁」表现成「这台机器直连就行」。
- **`fAutoDetect` 的三态不能压成布尔**：WinINet 口径里 `TRUE` 才是「开了自动探测」，`FALSE` 是明确关闭，`-1` 是用户从未表达过偏好；按布尔读会让默认镜像上的每台机器都被判成 WPAD 配置而全线报错。
- **服务器表按 scheme 令牌分槽，槽位与变量集合一一对应**：`http=` → `HTTP_PROXY` 槽、`https=` → `HTTPS_PROXY` 槽、裸项与 `all=` / `other=` / `*` → `ALL_PROXY` 槽、`socks5=` → `ALL_PROXY` 槽并按 `socks5h`（代理解析目标名——系统设置里的 SOCKS 从来是服务端解析）。`ftp=` / `gopher=` 整项略过：本库不发起这两类连接，它们既不决定路由也不该造成故障。`socks=`（Windows 与 IE 口径里的 SOCKS4）报 `net-proxy-unsupported`——配置里没有版本字段，猜成 SOCKS5 会让同一份配置在不同机器上连到不同协议的端口。同槽重复项报 `net-proxy-config-invalid` 而不是后者覆盖前者。
- **主机与例外文法只有一套**：每个项经 `ProxyConfig::parse()`、例外清单经 `NoProxy::parse()`，本件不新造第二套；内层错误码原样透出（`http=https://proxy:8443` 是「代理自身走 TLS」，仍属 `net-proxy-unsupported`，折算成配置畸形会把排查方向带偏），只加「第几项 + 原文」的语境。系统专属令牌 `<local>` / `<-loopback>` 在 `NoProxy` 里建成独立规则形态（见 §8.8.7），不在本件做特判。
- **接缝只存在于库内**：`detail::set_system_proxy_hook()` 让全部判据在不碰真实注册表 / 动态存储的环境下逐值断言。公共 API 里放一枚「改变代理决策来源」的开关，等于让任何调用方能把企业网流量指到直连上去，与「绝不静默绕过代理」的纪律同源冲突——这与 §8.8.9 的解析钩子是同一条理由。
- **合并次序是运维契约**：`from_environment_or_system(reader)` 先读环境变量，任一代理变量存在即**接管**（系统腿不被询问），不做逐槽位合并；容器 / CI / 代理调试现场全靠这条覆盖机器级配置，合并会让「设了却没生效」变成新的排查黑洞。系统腿回 `net-platform-unavailable` 时结果等价于纯环境变量读取（Linux 上这就是一次正常读取），回其它码时原样上抛。

- 错误码：新增 `net-proxy-config-unreadable`（category `io`、`retryable = true`、`fix_category = resource_error`）。链接依赖随之扩面：Windows 加 `winhttp`、Apple 加 `-framework SystemConfiguration -framework CoreFoundation`（均 `PUBLIC` 传导，见根 `CMakeLists.txt`；口径记于 `BUILD_OPTIONS.md`）。macOS 腿在本机不可运行验证，由 CI 的 `macos-clang` 作业做编译验证。
- 回归：`utest_net_system_proxy` 八例——PAC/WPAD/两者并存三类都判成 `net-proxy-unsupported` 且诊断带脚本 URL、没配代理是成功的空集合（带例外清单时例外仍解析）、四类 scheme 令牌落四个槽（`source()` 为 `system:<令牌>`）、裸项归 `all` 且 `ftp=`/`gopher=` 略过（缺省端口沿用配置侧口径）、六类歧义与畸形条目分岔到两码（含 SOCKS4、TLS-to-proxy、同槽重复、`=` 位置颠倒）、`<local>` 与 `<-loopback>` 的匹配面（无限定域名命中、带点与数字字面量不命中、`127.9.9.9` 与 `::1` 命中）、采集钩子三条回报（快照 / 畸形 / 空快照判成 `net-platform-unavailable`）、合并次序四态（接管时系统腿调用次数为 0、无变量时采纳快照、无通道等价环境变量、读失败原样上抛）。全用例零平台 API、零真实环境变量，三平台与 wasm 一致，故无 `AURORA_TEST_SKIP`。

#### 8.8.11 异步流传输：`StreamTransport` / `StreamListener`（CHG-018 传输族首件）

`include/aurora/net/transport.h` + `src/aurora/net/transport.cpp` 把 §8.8.3 的就绪基元、§8.8.4 的常驻引擎与 §8.8.2 的事件源注册口编排成「面向字节的异步流」：公共面只有 `ShutdownMode`、`StreamOptions`、四枚完成别名（`ConnectDone` / `AcceptDone` / `ReadDone` / `WriteDone`）、`StreamTransport`（`connect` / `read` / `write` / `shutdown` / `close` / `pause_read` / `resume_read` / `set_high_water_mark` / `pending_write_bytes` / `native_handle` / `peer_endpoint` / `is_loopback_peer` / `cancellation` / `is_open`）与 `StreamListener`（`bind` / `accept` / `local_port` / `native_handle` / `is_valid` / `close`），**全部异步出口都是回调形态**（L1 不得反向依赖 L4 的 `Task` / 协程，同 §8.8.9 的分层方向裁决）。非阻塞系统调用的唯一平台腿是 `src/aurora/net/detail/async_io.h` 的七个出口（建套接字并置非阻塞、发起连接、取连接错误、读、写、接受、关停，加绑定端口与对端还原），实现仍落在 `src/aurora/net/socket.cpp`——CHG-006 的「平台 socket 头唯一落点」不变量继续成立，`transport.cpp` 里一枚平台头都没有。连接与监听的状态（`detail::StreamState` / `ListenerState`）字段全在实现侧，公共头只有前向声明与一枚实现侧工厂 `detail::TransportFactory`（两个公共句柄的私有构造只对它敞开，因此**外部代码造不出伪句柄**）。九条定见：

- **完成恒回到发起线程**：每连接一枚互斥量（引擎定时器与跨线程 `cancel()` 都是真实入口，无锁即无正确性）；锁内只收集待触发的回调、**一律在锁外触发**（回调里再调本层方法不得自锁）；求得的完成结果若不在发起线程就经 `aurora::detail::post_to_main` 投回，与 `Task<T>` 共用同一条主线程通道，不新增第三种排队形态。
- **「还没法做」不是错误**：内部腿返回三态（`IoAttempt{bytes,blocked,failure}` / `AcceptAttempt{peer,blocked,failure}`），`EWOULDBLOCK` 与 `EINTR` 一类的可重试态**不消耗错误码**——把 would-block 报成 `net-recv-failed` 会让上层在正常轮询路径上分支一个不存在的故障。
- **半关闭用 0 字节表达，不新增码**（§18 第 2 项）：读得 0 = 对端 FIN，此后每次读恒 0 且反向仍可继续写；`shutdown(ShutdownMode::Read)` 复用同一条 EOF 口——已排队的读恰一次得 0，本层此后不再注册可读关注。停写方向上「已排队的写」以 `net-cancelled` 收尾，其后 `write()` 落 `net-option-failed`。
- **一次在途连接必须有强所有权**：非阻塞 `connect()` 从登记事件源到结果落地之间，状态的唯一持有者若是事件源闭包里的 `shared_ptr`，就形成「状态 → 登记 → 闭包 → 状态」的环，移动赋值与析构都救不了它（实测表现是 fd 泄漏且回调永不抵达）。故本层设**在途连接表**：发起时入表、结果落地即出表，事件源闭包只持 `weak_ptr` 见证；注销与关句柄的次序恒为**先注销、后关句柄**（§18 第 1 项），否则同一句柄可能已被别的源复用。
- **关注方向按拉模型计算**：没有排队读请求就不注册可读，没有待发字节就不注册可写，没有待交付的 accept 回调就不注册监听口（连接留在内核 backlog 里，等宿主想收时再收）。任何关注变化都 `++generation` 并重新登记，旧登记的尾批据此作废——这既挡住「注销后仍有事件」的悬垂访问，也让「同一句柄反复改关注」不会攒出多份投递。
- **背压是显式契约而非节流暗示**：待发字节越过高水位后新的 `write()` 直接被拒（`net-buffer-limit`），回落到 `write_low_water_bytes` 以下才重新接受（成对阈值避免在临界点上抖动）；`set_high_water_mark()` 会按新高水位重算低水位。**单次写允许越界至多一枚请求**，否则正文大于高水位的写永远发不出去。读侧同样有上限（`max_pending_bytes` 约束排队读缓冲总量），`read_chunk_bytes` 决定一轮投递多少字节——内核里多于它就分轮投递，故调用方缓冲可以比它小。
- **驱动方可切换**：`connect()` / `bind()` 先经 `ensure_driver()` 叫醒驱动方——宿主已声明自带等待集（`host_driven()`）时**不投引擎**，否则同一句柄会被两边等待。本层不自己开线程。
- **空态是合法值**：默认构造或移动后的句柄调用成员不崩、不悬挂，以 `net-socket-failed` 形态送达；入参不合用（空缓冲、空回调、端口 0）同样走回调，**没有「既不成功也不失败」的第三种结局**。`connect_timeout` 由引擎定时器投递 `net-connect-timeout`，置 0 即不设时限。
- **取消即恰一次收尾**：`Cancellation` 钩子在锁外登记（「登记时已取消」会在调用线程就地执行钩子，锁内登记即自锁）；取消后未完成的读写各以 `net-cancelled` 抵达一次，句柄归还与注销同批完成。

- **完成回调可能在析构边界内派发，因此不得抛出**：`close()` 与 `~StreamListener` / `~StreamTransport` 会把排队中的 accept / 读 / 写各以一次失败收尾，这条派发路径落在析构函数之内（`noexcept` 边界）。回调里抛出（测试框架的致命断言即抛 `CaseAbort`）会直接 `std::terminate`，整条进程连带消失而不是一份失败报告——本层不吞异常（口径同「完成回调一律在锁外触发，抛出只可能来自调用方」），但**调用方在回调里不得依赖致命断言 / 抛出式前置检查**，`utest_net_transport` 与 `utest_net_remote_connect` 的完成回调一律用非致命断言记录失败，前提是否成立由等待方的断言负责转红。
- 错误码：`net-option-failed`（停写后再写、连接期参数不合用）与 `net-buffer-limit`（读写两侧缓冲越限）都在原规划清单内；**`net-connect-timeout` 是原规划未列、本切片补入的一枚**（`severity=warning`、`retryable=true`、`fix_category=timeout`）——连接超时与「连不上」的排查方向不同（前者多半是黑洞路由或防火墙丢包，后者是端口上没有服务），与 `net-proxy-protocol` 同类的分岔理由。
- 工具链面：`ShutdownMode` 与同批补齐的 `AddressFamily` / `ProxyKind` 登记进 `tools/include/known_enums.h`（`aurora_api.json` 的 `enums` 段随之刷新），`itest_known_enums` 加编译期锚点，改名即编译失败。
- 回归：`utest_net_transport` 十例——连接与接受各恰一次送达且三条观测口（`is_open` / `native_handle` / `peer_endpoint` / `is_loopback_peer`）如实回报、半关闭后本端读得 0 而反向仍可收发、`shutdown(Read)` 以 0 字节收尾已排队的读、写背压拒收后回落重受（含「至多越界一枚」的上界断言）、`pause_read` 把字节留在内核直至 `resume_read`、取消在途读恰一次送达、参数与空态契约、取消在途连接后事件源登记数归零（不泄漏句柄也不泄漏登记）、关闭监听口后待交付 accept 恰一次失败、以及**引擎驱动**形态下不碰 `dispatch()` 也能收满（`host_driven()` 为假时由常驻线程投递）。宿主驱动用例以 RAII 守卫进出 `set_host_driven(true)`，用 `wait_ready` + `dispatch` 自建轮询循环，故等待时刻与线程归属都可同线程断言；wasm 腿无 fd 等待语义，用例体内 `AURORA_TEST_SKIP`。本机验证腿：Win32（Winsock `WSAPoll`）与 WSL（POSIX `poll`）同口径。

#### 8.8.12 名字连接与代理隧道：连接编排（双栈与竞速 + 代理族的传输侧收口）

`src/aurora/net/detail/remote_connect.h` + `src/aurora/net/detail/remote_connect.cpp`（命名空间 `aurora::net::detail`，仅库实现可见）把 §8.8.9 的解析产出、§8.8.11 的流连接出口与 §8.8.8 的握手字节编排成公共侧的两枚新入口：`StreamTransport::connect(std::string_view host, std::uint16_t port, …)` 与 `StreamTransport::connect_via(const ProxyConfig &, std::string_view host, std::uint16_t port, …)`，并给 `StreamOptions` 补上竞速窗口 `race_stagger`。本层**不新增任何系统调用、一枚平台头都没有**（CHG-006 不变量继续成立）：它只把已有三件按次序与判据编排，因此在零 DNS（钩子见 §8.8.9）、零真实代理进程（进程内假代理）的条件下可逐值断言。七条定见：

- **地址次序是连接策略而不是解析事实**：`race_preference()` 把偏好族（IPv6）首条提到居首，其后两族交替，一族耗尽即由另一族尾部自然补齐（`RFC 8305` §5.4 形态）；解析层因此保持不排序（§8.8.9），两处真相各归其位。本函数只改次序、不增不减，故调用方能拿同一份列表证明「一条都没丢」；**本机 IPC 形态在此被剔除**（不属于 TCP 竞速，随本机 IPC 切片另立发起口），非 IP 族因此天然不参与交错。
- **竞速是串行交错而不是并发齐发**：首条立即发起，其后每条在「上条发起后 `race_stagger`（缺省 200 ms，提案 §11 定案）」与「上条已出结果」两者**先到**时发起——失败即让位、不等满窗口，这是快败场景不慢于纯串行的保证；交错定时器只在还剩候选时装，落不到引擎就退化为「不交错」（后续候选仍由失败快跟发起），因为时限本就由每条自带的 `connect_timeout` 负责，不必为一条优化路径报错。窗口置 0 即只发偏好族首条。最先建成者独占那次交付（`claimed` 先到先得），落败者随局部对象析构关闭。
- **一次名字连接只交付一次结局**：成功取最先建成的流；全败携**最早那条失败原因**（`first_error`，保留现场的第一手信息，而不是「没有候选」这类无法排查的兜底码）；空主机名与端口 0 落 `net-endpoint-invalid` 且**不查解析器**，字面量同样不查（§8.8.9 的短路在接缝之前），解析本身失败的原因原样转达（`net-dns-failed`），本层不另造一套分岔；空回调直接不发起——事件源登记数保持不动。
- **代理隧道只在握手成功时成立，任何分支都不回退直连**：`ProxyKind::Direct` 转交直连入口（显式关代理就是直连，不是「还没决定」）；其余形态先连代理——代理常常是双栈的，故代理自身也走「解析 + 竞速」——再按 §8.8.8 的字节层握手，每个失败分支都经该层的换算口落成 `net-proxy-*` 原因。`connect_timeout` 在此覆盖「直到隧道可用」的整段：一枚时限同时盯着代理连接与握手应答，代理连上了却不应答也会有结局，否则静默悬挂。
- **多段握手必须带读偏移**：SOCKS5 的方法协商、用户名/密码子协商、CONNECT 子协商是三段独立报文，`Tunnel::offset` 在每段解析成功后前移该段长度，否则下一段会把上一段当成自己的开头重读（实测表现是「CONNECT 应答里读出未知 `ATYPE` 0」——那是方法协商回报的版本字节）。HTTP CONNECT 与 SOCKS5 共用同一个偏移口径：解析口只看得见 `[offset, filled)` 这段窗口。
- **隧道 leftover 接回读队列前端**：握手为读满不定长头部块而多读到的字节属于隧道，经实现侧接缝 `detail::seed_read()` 存入 `StreamState::read_ahead`，`drive_read` 先经 `satisfy_reads_from_ahead()` 交付这些字节再取内核字节。该接缝刻意不进公共 API：调用方没有理由手工伪造「内核之前的字节」，那会破坏「读到的字节恒等于线上字节」这条不变量。同时修正排队读的计数（`release_read_slot` 的额度按实际交付字节归还），否则 leftover 与内核字节混投时 `max_pending_bytes` 会虚高。
- **完成恒回到发起线程，且归队之后不设锁**：解析回调、交错定时器、隧道时限都可能在引擎线程落地，故每次进入编排状态前先 `run_on_origin()` 归队（与 `transport.cpp` 的 `deliver` 同一条通道、同一处局限：宿主从工作线程发起时以投递器的落点为准）。归队后状态转换全在同一条线程上串行，`Race` / `Tunnel` 两张表因此都没有互斥量——这不是省锁，而是「所有入口都在同一线程」的直接推论，去掉归队即失效。在途定时器与事件源回调以 `weak_ptr` 见证进入、编排状态由 `shared_ptr` 独占，用例结束时不会有迟到的回调落到已销毁的栈对象上。

- 错误码：本切片**不新增**，启用的是既有分岔——`net-endpoint-invalid`（空主机名 / 端口 0）、`net-dns-failed`（解析失败或零条可用）、`net-connect-failed` / `net-connect-timeout`（竞速全败携带的原因、隧道时限）、`net-proxy-auth-required` / `net-proxy-unreachable` / `net-proxy-unsupported` / `net-proxy-protocol`（§8.8.8 的换算口）。诊断文本一律只带代理与目标的 `host:port`，凭据不出现在任何 message 里。
- 回归：`utest_net_remote_connect` 十一例——`race_preference` 的两族交替与「不增不减 / 剔除本机 IPC 形态」、快败立即让位（交错窗口置 0 时第二条只可能由第一条的失败发出来，交付的是建成那条而非列表首条，且一次名字连接只查一次表）、首个成功独占交付而后来者**根本没开始**（窗口置 200 ms 后另起宽限窗，第二个监听口的 accept 计数保持 0，这条比「落败者已关闭」更强）、全败恰一次且携连接类原因、入参不合用与字面量都不查解析器且空回调不发起任何登记、HTTP CONNECT 隧道把 leftover 逐字节交回上层、CONNECT 拒绝按状态分码（`407` → 认证码，`403`/`502` → 不可达码）、SOCKS5 三阶段逐段走完并保留 leftover（含逐字节的方法协商问候与 CONNECT 请求形态）、SOCKS5 各阶段状态字节逐段分码（方法 `0xFF` 与子协商非零 → 认证码，`REP` 非零按 §8.8.8 分岔）、`Direct` 配置等价于直连且不开隧道、代理端口上没有服务时以失败收尾且 `watch` 侧 accept 计数为 0（**证明没有回退直连**）。除 `race_preference` 一例外全部在 wasm 腿 `AURORA_TEST_SKIP`（浏览器运行时既没有可失败又可恢复的套接字，也没有可竞速的套接字）。本机验证腿：Win32 与 WSL 同口径；「代理不在监听」一例在 Win32 上约 2 s（回环端口上的拒绝要等内核重试到点），非缺陷。WSL 腿补出一条时序裁决：`connect()` 成功只是把连接放进内核 backlog，监听口侧的 accept 交付**晚一轮泵**，故凡断言「对端已收到这条连接」的用例都必须同时等到 accept 计数就位（只等连接结果会把断言变成竞态，Linux 上稳定转红而 Windows 上掩盖着）；`utest_net_remote_connect` 的竞速两例与 `direct_proxy_config` 一例据此把等待谓词改成「连接出结果 **且** 监听口已交付」。


#### 8.8.13 分帧器：`net::Framer` 的三型形态与拉模型背压（CHG-018 传输族）

`include/aurora/net/framer.h` + `src/aurora/net/framer.cpp`：把 §8.8.11 交付的「这次读到了若干字节」切成「一条完整消息」，**只做这一件事**——没有 socket、没有平台头、没有线程，因此全部判据在零网络条件下逐值可断言（`StreamTransport` 只是它的字节来源之一）。公共面只有 `LengthByteOrder`、`Frame`、`FrameSink`、`FrameOptions`、`FeedReport`、`FinishReport` 与 `Framer`（三型工厂 `lines` / `until` / `length_prefixed` 加 `feed` / `finish` / `reset` / `buffered_bytes`），状态（`detail::FramerState`：型别、口径、一件缓冲、搜索起点、终态错误）全在实现侧，公共头只有前向声明。判据如下（提案 §11 把它拆成七条，本节的①同时给出「三型边界」与「空帧是语义而非噪声」两条口径、④同时给出「上限终态」与「工厂期校验」两条，内容不增不减）：

- **三型的边界判据各归其位**：行型只认 LF 与 CRLF（**裸 CR 是普通字节**，SSE 那类需要兼容裸 CR 的来源由上层先归一化，本层不猜），分隔符型认任意非空字节串，长度前缀型按 1..8 字节的定长字段解码（大端 / 小端由 `byte_order` 决定，`length_counts_prefix` 决定声明值含不含字段自身）。终止符、分隔符、长度字段本身**都不进帧体**；行型的空行与分隔符型的连续命中都是**空帧而不是噪声**——SSE 的事件边界正是空行，把它们当空白丢掉等于把所有事件粘成一条。
- **拉模型：每次 `feed()` 都吃掉全部输入**，交付多少条由回调返回值决定（`true` 继续、`false` 暂停）。被拒的那一帧本身已经交付，故 `FeedReport::delivered` 计的是「回调实际收到的帧数」，暂停之后的字节留在内部缓冲由下一次 `feed()`（可以是空 span，语义「只续投不新增」）续投，**既不丢也不重复投递**。调用方的正确背压动作是「暂停期间别再读那条流」，而不是把字节再喂一遍。空回调视为「一条都不投」：字节照吃、缓冲不动。
- **一件缓冲 + 回调期有效的视图**：`Frame::bytes` 指向本对象缓冲，前缀只在回调返回（或抛出）之后才被抹掉，所以「视图有效期 = 本次回调之内」是实现的直接结果而非额外纪律；抹除用一枚 RAII 完成，回调抛出时同样前移，否则异常展开后同一帧会被二次投递。搜索起点 `scan_from` 跨 `feed()` 保留，避免重扫已确认无边界的前缀；分隔符型只回退「长度减一」那段（尾部可能是分隔符前缀，跨边界的命中必须还能命中）。
- **两侧上限都是硬约束，且都是终态**：单条消息越过 `max_frame_bytes` 落 `net-framer-overflow`，缓冲（未交付的完整帧 + 凑不满一帧的残片）越过 `max_buffered_bytes` 落 `net-buffer-limit`；两者置位后所有操作原样回报同一条错误，直到 `reset()`（换一条连接复用本对象）。**超限处不重同步**——一条超长消息已足以说明字节流不可信，在残缺帧里找回边界会把错乱的偏移当成新消息的开头。口径校验在工厂期完成：上限为 0、缓冲小于一帧、空分隔符、分隔符超单帧上限、长度字段宽度不在 1..8 一律当场拒绝（`net-framer-config-invalid`），不留「静默接受无界帧或切错位置」的形态。
- **输入结束的语义随型别分岔**：行 / 分隔符型把没有自带终止符的尾巴补成最后一帧，并以 `Frame::unterminated == true` 标明来源，上层据此决定收下还是丢弃（SSE 按规范丢弃、行协议通常收下）；长度前缀型里「长度承诺未兑现」的字节不构成一条消息，直接丢弃并在 `FinishReport::dropped_bytes` 报出字节数。
- 错误码：本切片**新增两枚**——`net-framer-overflow`（`severity=error`、`retryable=false`、`fix_category=resource_error`）与 `net-framer-config-invalid`（`severity=error`、`retryable=false`、`fix_category=invalid_value`），复用清单外的一枚 `net-buffer-limit`（§8.8.11 已有，语义扩展到「分帧缓冲越限」）。三枚的 `message` 都携 `detail=` 指明具体形态（超长的是无终止符残片还是声明值超限、越限的字节数与上限），排查时无需读码即可分支。
- 工具链面：`LengthByteOrder` 登记进 `tools/include/known_enums.h`，`itest_known_enums` 加编译期锚点；`aurora_api.json` 与 `codespec/ERROR_CATALOG.md` 由 `errors.toml` 一次生成刷新。伞头 `aurora/aurora.h` 与 `tools/check/umbrella_manifest.txt` 同步收录 `net/framer.h`。
- 回归：`utest_net_framer` 十三例——LF/CRLF 双终止与跨 `feed()` 拼装、空行是空帧、裸 CR 归帧体、分隔符跨 feed 命中与空帧、暂停-续投不重复（含空回调惰性形态）、单帧超限到 `reset()` 复原的终态链、慢消费者越缓冲上限、长度前缀大小端与「字段拆成两次投递」、声明值含自身宽度与不足宽度的非法、声明超上限即拒、`finish()` 的型别分歧（补尾 vs 丢弃并报数）、五类工厂口径拒绝与 8 字节宽度上界合法、移动后落点惰性。分帧器无 fd 语义，故 wasm 腿无需跳过；本机验证腿：Win32 与 WSL 同口径（纯逻辑，两侧应逐值一致）。

#### 8.8.14 数据报传输：`DatagramTransport` 的消息边界、connect 过滤与组播 / 广播（CHG-018 传输族）

`include/aurora/net/datagram.h` + `src/aurora/net/datagram.cpp` 把 §8.8.3 的就绪基元、§8.8.4 的常驻引擎与 §8.8.2 的事件源注册口编排成「面向消息的异步数据报」，与 §8.8.11 的流传输**并列而非包含**：流面向字节、没有消息分界，数据报一条一条、每条自带边界与来源地址。公共面只有 `Datagram`、两枚完成别名（`DatagramSendDone` / `DatagramDone`）、`DatagramOptions` 与 `DatagramTransport`（工厂 `bind` / `connect`，操作 `send_to` / `send` / `receive` / `join_multicast_group` / `leave_multicast_group` / `set_multicast_loopback` / `set_broadcast_allowed` / `close`，观测 `is_open` / `native_handle` / `local_port` / `peer` / `pending_send_bytes` / `multicast_loopback()` / `broadcast_allowed()` / `cancellation`），**全部异步出口都是回调形态**（分层方向同 §8.8.11）。状态 `detail::DatagramState`（句柄、收发两条队列、事件源登记、代数、组播组记录、互斥量）字段全在实现侧，公共头只有前向声明与一枚实现侧工厂 `detail::DatagramFactory`。平台系统调用仍只有 `src/aurora/net/socket.cpp` 一处落点（CHG-006 不变量），`datagram.cpp` 经 §8.8.11 的 `src/aurora/net/detail/async_io.h` 接缝借出新增出口：建数据报套接字并绑定、固定对端、`sendto` / `send` / `recvfrom` 的三态形态、组播成员与回环、`SO_BROADCAST`。判据（前三条是**消息语义**，第四条起是**形态裁决**）：

- **一次 `receive()` 交付恰一条数据报**：绝不跨数据报拼装，也不把一条拆成两轮投递——接收槽与数据报一一对应，`FIFO` 且每枚槽只被满足一次。
- **零字节是一条消息**：`Datagram::bytes` 为空且 `truncated == false` 表示对端发了空报。数据报没有「对端已收摊」这个状态（UDP 无连接语义），故 0 字节不是 EOF 而是内容——把它做成终止信号会让上层永远读不到合法的空调用（与 §8.8.13「空帧是语义而非噪声」同源）。实现侧的同一条口径反过来约束入口：空 span 是**一条要发的空数据报**，不是「无事可做」，所以本层另设 `send` / `sendto` 出口而不复用把空 span 判为无进展的 `try_write`（那会让零长数据报永远悬挂）。
- **截断在库内判定，代价是一次复制**：Windows 的 `recvfrom` 在缓冲不足时失败并**整条丢弃**该数据报，POSIX 静默截断，同一份代码两条腿两种结局（`SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001`）。故库内接收缓冲恒按内核上界 65535 字节配足、与合同口径无关，整条读进库内后再按「调用方缓冲长度与 `max_datagram_bytes` 取小者」裁剪并置 `Datagram::truncated`；被裁掉的尾部**不回头补投**——下一条槽交付的是下一条消息（用例以「预投两枚槽 + 发两条」的形状把这条钉死）。`max_datagram_bytes` 的语义因此是**发送侧拒收口径 + 单次交付上限**，而不是「递给内核的缓冲长度」；合法区间 `1..65535`，越界口径在 `bind()` 即拒。
- **收发失败不关闭套接字**：数据报没有连接状态可破坏，一条消息的失败（向广播地址发送却没置允许位、目标不可达引发的 `ECONNREFUSED`）不该让整枚套接字退役，故失败恰一次落到**该条请求**的回调上、队列继续推进。这与 §8.8.11「写失败即关流」是刻意的差别——那里失败意味着那条字节流已不可信。
- **connect 形态改的是两个入口而不只是过滤**：固定对端后内核只与该地址收发（接收侧过滤非对端数据报），发送侧必须走 `send(2)`：Linux 让带地址的 `sendto` 在已连接的数据报套接字上以 `EISCONN` 失败，Windows 却忽略该地址，同一调用不能有两种结局。于是 `send_to()` 在该形态上报 `net-option-conflict`（不是静默改道），未固定对端时调 `send()` 落 `net-endpoint-invalid`。
- **拉模型与内核丢弃**：没有待交付的接收槽就不注册可读关注，数据报留在内核接收队列；队列满时由内核丢弃。这是 UDP 的固有语义，本层不伪造排队、不为它编造错误码，也不设无界队列。关注方向、代数与尾批丢弃的机制与 §8.8.11 完全同源（重登记必 `++generation`），故取消 / 关闭后的登记数可同样以 `live_count()` 归零断言。
- **组播与 connect 互斥**（提案 §18-3）：内核在已 connect 的套接字上只收对端数据报，静默接受组播等于造出一个永远收不到包的配置，故该形态下 `join_multicast_group()` / `leave_multicast_group()` 一律报 `net-option-conflict`；需同时收单播与组播时用两枚 `DatagramTransport`。组地址须 `Endpoint::is_multicast()` 为真且族与套接字一致，否则在发起系统调用前落 `net-endpoint-invalid`；接收网卡由内核按路由选择（IPv4 取 `imr_interface = INADDR_ANY`，IPv6 的 `ipv6mr_interface` 填 zone id 的十进制值）；本层另记一份「已加入的组」，未加入过即离开时报 `net-option-failed` 而不是让内核去猜别的套接字加过什么。**不提供 `disconnect()`**：POSIX 无可靠取消连接语义、Windows 需换协议技巧，跨平台一致性不值得为此破例。
- **本机 IPC 形态一律拒绝**：`Endpoint::local()` 在 `bind()` / `connect()` / 发送目标三处都落 `net-endpoint-invalid`——本机 IPC 切片（§8.8.15）只落地**流式**形态，`AF_UNIX` `SOCK_DGRAM` 不进公共面；两平台都有 Unix 数据报，但它没有流式形态的那套半关闭与背压语义，做成「看起来一样、语义不同」的第三种流比不支持更难排查。
- **绑定与连接是同步 `Result` 工厂**：UDP 没有握手，`connect(2)` 在本地登记对端即完成，把它做成异步等于给一个不会失败的调用编造完成通道；端口 `0` 由系统分配并经 `local_port()` 回报。
- **背压是被发队列的总字节上限**：`max_pending_bytes`（缺省 1 MiB）约束 `sends` 里尚未整条进入内核的载荷副本，越限落 `net-buffer-limit`；单条越过 `max_datagram_bytes` 或 65535 内核上界同样在发起系统调用前即拒，`pending_send_bytes()` 是断言点。
- **空态与参数契约**：默认构造或移动后的句柄调用成员不崩、不悬挂，各以一次失败形态送达（`net-socket-failed`）；`receive()` 的空缓冲是**用法错误**（0 字节已被「空消息」占用，不能同时充当「没给缓冲」）落 `net-buffer-limit`；空回调不建副本、不登记事件源；`close()` 与 `cancel()` 让未完成的收发各以 `net-cancelled` 收尾一次，顺序恒为**先注销、后关句柄**（§18 第 1 项）。

- 错误码：本切片**新增一枚** `net-option-conflict`（`severity=error`、`retryable=false`、`fix_category=invalid_value`）——两条设置在同一枚套接字上不能同时成立（connect 与组播、`send_to` 与固定对端），`message` 携 `detail=` 指明是哪一对，`hint` 直接给出「另开一枚套接字」的修法；复用 `net-endpoint-invalid` / `net-buffer-limit` / `net-cancelled` / `net-option-failed` / `net-socket-failed` / `net-recv-failed` / `net-send-failed` / `net-bind-failed` / `net-platform-unavailable`。数据报侧**没有**新增「超时」类码：UDP 无连接，本层也没有 per-datagram 定时器。
- 能力位：`Capabilities::udp` 随本切片点亮；同批补账把 `dns` 也点亮（§8.8.9 的 `net::Resolver` 早已落地，「能力位不得先于实现点亮」是双向约束）；`local_ipc` 随 §8.8.15 置真，且它是九枚里唯一由**运行期探测**得来的位；`tls` 仍恒假。wasm 腿的 `NullEngine` 一切为假、名为 `"none"`（§2「不提供假降级」）。
- 工具链面：`aurora_api.json` 与 `codespec/ERROR_CATALOG.md` 由 `errors.toml` 一次生成刷新；伞头 `aurora/aurora.h` 与 `tools/check/umbrella_manifest.txt` 同步收录 `net/datagram.h`。
- 回归：`utest_net_datagram` 九例——回环数据报往返且 `source` 如实回报来源端口（双向各一次）、零长数据报是消息且其后一条照常送达、截断置位而下一槽拿的是下一条消息（含合同口径收紧的第二次裁剪）、connect 形态过滤第三方数据报且 `send_to` 报互斥 / `send` 走通、组播的互斥与校验分支及加入—离开—再离开的记录一致性（内核拒绝加入时整条用例判 `AURORA_TEST_SKIP`，因为那需要本机有可组播的路由，悄悄放行会让强断言永不执行）、广播允许位如实记录且**缺它时那条发送失败只作废那一条**（随后同一套接字的单播照常送达）、口径与高水位在发起系统调用前即拒（含 `bind()` 期的三类非法口径与本机 IPC 形态）、取消与关闭各恰一次收尾且登记数归零（含空态句柄与空回调）、引擎驱动形态不碰 `dispatch()` 也能收满。宿主驱动用例以 RAII 守卫进出 `set_host_driven(true)`，形态与纪律同 §8.8.11（完成回调里只用非致命断言）；wasm 腿无 fd 等待语义，用例体内 `AURORA_TEST_SKIP`。本机验证腿：Win32（Winsock）与 WSL（POSIX）同口径。

#### 8.8.15 本机 IPC 流与原生选项：`Endpoint::local` 上的同型传输（CHG-018 传输族）

`src/aurora/net/socket.cpp` 把 §8.8.11 的异步流打通到本机 IPC 地址族上，公共面只在 `include/aurora/net/transport.h` 补一枚 `StreamTransport::peer_credentials()` 与两枚值类型（`NativeOptions` / `PeerCredentials`）。四条定见：

- **本机 IPC 在每条腿上都落 `AF_UNIX`，不做 Windows 命名管道**：Windows 的 Winsock 自 Win10 起提供 `AF_UNIX` 流式套接字，而本层的全部就绪判据都建立在 poll 族对句柄的回答上——Winsock 的 `POLLIN` 位掩码（`0x300`）覆盖 `revents` 的 `0x100`，对端 `shutdown(SD_SEND)` 后本端报 `POLLHUP` 且 `recv` 回 0，即 §8.8.3 就绪基元与 §8.8.11 状态机**逐条原样可用**，因此两腿共用同一套流语义而不需要第二份实现。命名管道不是 socket 句柄：进不了事件源注册口，`native_handle()` 交出的东西在两条腿上含义不同，把它做成「本机 IPC 的第二种形态」等于让调用方按平台分支处理同一件事。落点仍只有 `src/aurora/net/socket.cpp` 一处（CHG-006 不变量继续成立）；Windows SDK 头不提供 `sockaddr_un`，故实现侧自带该结构声明，路径容量按本腿实际宽度在装配地址时二次校验（值类型那道闸取的是最宽一条腿，见 §8.8.1）。
- **同步阻塞面不接本机 IPC**：`blocking::StreamSocket::connect` / `blocking::Listener::bind` 对 `Endpoint::local()` 恒落 `net-endpoint-invalid`，路径形态只由异步传输族承载。同一份地址在两个面上给出两种结果，比统一拒掉更难排查；裁决随 §16 记录。
- **地址族专属的观测口各给确定答案，不伪造**：`peer_endpoint()` 在路径连接上落 `net-option-conflict`（提示改问 `peer_credentials()`），`local_port()` 恒 0，`is_loopback_peer()` 恒 false（回环判据只回答「IP 是否落在回环段」，本机 IPC 的连接本就只可能在同机发生）；`peer_credentials()` 反过来在 IP 连接上落 `net-option-conflict`——静默回一份全零身份会把「以为做了身份校验」的授权判断变成空转。身份取值只在 Linux / Android 腿用 `SO_PEERCRED` 登记（`src/aurora/net/socket.cpp` 内自带同布局的结构声明，不依赖 `_GNU_SOURCE` 才可见的 libc 类型名），其余腿以 `anonymous == true` 如实回报而不是伪造数值；macOS/BSD 的 `LOCAL_PEERCRED` 字段形态在本仓任何验证腿上都没有实测条件，因此不凭记忆实现（AGENTS 硬规则 4）。
- **`NativeOptions` 是逃生舱的一半，`native_handle()` 是另一半**：九项高频选项（`no_delay` `receive_buffer_bytes` `send_buffer_bytes` `reuse_address` `linger` `keep_alive_enabled` `keep_alive_idle` `keep_alive_interval` `keep_alive_probes` `source`）全部 `std::optional`，**未置值的字段一条 `setsockopt` 都不发起**，内核缺省原样保留；生效时机恒在 `connect()` / `bind()` 发起系统调用之前，因为 `SO_REUSEADDR` 与源地址在 bind 之后再设已无效，而本层不准备为「改了没反应」的第二入口编造语义。其余选项经 `native_handle()` 交出描述符自行接管。接受侧对每个新句柄**重放**监听口的选项集合——内核的选项继承表两腿不同（Linux 上 `TCP_NODELAY` 不随 `accept` 传下来，Winsock 传），同一份 `StreamOptions` 必须在两条腿上得到同一件事实。冲突一律落 `net-option-conflict` 而**不是静默忽略**：TCP 专属项（`no_delay`、keepalive 各档）出现在本机 IPC 流上、源地址出现在已绑定的句柄上、监听口句柄带源地址、`receive/send` 缓冲非正、`linger` 为负、显式关掉保活却仍设探活参数、源地址族与目标族不一致。平台不提供对应旋钮时同样不静默：Windows 的保活重试上限是系统级而非每连接（`net-option-failed` 写明），无每连接保活调档的腿整档报 `net-option-failed`。
- 错误码：本切片**无新增码**，产出的是既有码的新落点——`net-endpoint-invalid`（本机 IPC 路径超本腿容量、路径形态进同步面）、`net-option-conflict`（选项与地址族/句柄状态不匹配）、`net-option-failed`（平台无该旋钮、`SO_PEERCRED` 查询失败）、`net-bind-failed`（路径名字已被占用、源地址绑定失败）、`net-platform-unavailable`（本腿运行期开不出 `AF_UNIX`）。
- 能力位：`Capabilities::local_ipc` 随本切片置真，取值来自运行期探测而不是编译期声明（见 §8.8.4）。
- 工具链面：本切片**无新枚举**，故 `tools/include/known_enums.h` 与 `aurora_api.json` 的 `enums` 段零条目变化（`NativeOptions` / `PeerCredentials` 是值类型，schema 生成面只枚举控件、属性键与具名枚举，值类型不进该面），`check_api_budget` 与 `check_api_schema_sync` 同批复核为绿。公共头没有新增文件，伞头与 `tools/check/umbrella_manifest.txt` 因此无需收录动作。
- 回归：`utest_net_transport` 新增七例——路径端点上双向收发且三条观测口各给确定答案、对端身份只在登记过的腿给数值（IP 连接上问身份必冲突）、合法选项集合在系统调用前生效且接受侧重放后连接照常收发、TCP 专属项落在本机 IPC 上必冲突而缓冲窗口类两族通用、尺寸与方向矛盾的集合逐条在发起调用前被拒、路径长度的值类型那一档闸（空串 / 超最宽腿容量 / 恰等于容量的三态）、值类型放行但本腿装不下时在系统调用前落 `net-endpoint-invalid` 且「名字已被占用」的 bind 两腿同报 `net-bind-failed`。探测不过的腿整体 `AURORA_TEST_SKIP` 而非转红；wasm 无本机 IPC 语义，用例体内退避。本机验证腿：Win32（Winsock `AF_UNIX`）与 WSL（POSIX `AF_UNIX`）同口径，身份数值仅 Linux 腿强断言。

#### 8.8.16 同步阻塞面归位 `aurora::net::blocking` 与迁移期别名（CHG-018 传输族收口）

提案 §4 对旧三类型的处置在本切片落地，公共头换名而行为语义**逐值不动**：

- **落点**：类型声明搬进新公共头 `include/aurora/net/blocking.h`（命名空间 `aurora::net::blocking`），实现仍在 `src/aurora/net/socket.cpp`（该文件里 `blocking::StreamSocket` / `blocking::Listener` 的定义包在同一对命名空间括号内）——CHG-006「平台 socket 头唯一落点」不变量不破，本切片不新增 `.cpp`，故 `CMakeLists.txt` 的源文件清单零变化。新头已入伞头 `include/aurora/aurora.h` 与 `tools/check/umbrella_manifest.txt`（否则 `check_umbrella_header` 的「每个公共头都被分类」判据红）。
- **运行时归属（三段，规范性）**：① 本文件创建任何句柄前先过 `runtime_pinned()`，它取一枚**进程级、只申请不归还**的 Winsock 引用——「只申请不还」是刻意取舍而非疏漏，与 §8.8.2 注册口单例「刻意不析构」同一条裁决：`WSACleanup` 落在静态析构阶段后，任何退出路径上的 socket 调用都会撞 `WSANOTINITIALISED`，而本库不承诺退出期行为；取不到运行时以 `net-runtime-failed` 显式失败，不退化成 `net-socket-failed`（那会把「平台运行时没起来」误报成「这一次调用失败」，掩盖成因）。② 引擎、异步流传输与数据报各自持一成员 `detail::Session`，随自身生命周期申请与归还——这是 §4「引擎自身管理运行时生命周期」的落点。③ 使用方不再感知：`aurora::net::blocking` 的两类型没有任何需要先构造的运行时前置件，`Session` 降为库内细节（`src/aurora/net/detail/runtime.h`，非公共头）。名字解析（`getaddrinfo`）同样自带会话，其前置条件由实现侧承担。
- **迁移期别名**：`include/aurora/net/socket.h` 不再定义任何类型，只剩 `aurora::net::{StreamSocket,Listener}` 两枚 `[[deprecated]]` 转发别名与逐符号替代表，指向 `blocking` 的同名类型；判据是 `utest_net_socket` 的 `deprecated_aliases_resolve_to_blocking_types`（编译期 `static_assert` 同体 + 运行期经旧名建口），别名在下一版连同该用例一并移除。别名期内**禁止改动 `blocking` 的行为语义**（提案 §20 顺延条款），否则「旧名还在、新面已改语义」的中间态无人可辨。
- **消费者同批迁移**：`InspectorServer` 删掉运行时会话成员、监听与客户端句柄改走 `net::blocking`，REST 协议语义逐值不变（4 MiB body 上限、`Connection: close`、非回环对端 403、异常兜底 500）；`tools/servers/inspector_client.h` 的 `http_request` 签名与响应语义不变，其契约错误串（`connect() failed on 127.0.0.1:<port>`、`empty response`、`refusing non-loopback host: …`、`io timeout setup failed`）全部保留，只有 `socket runtime initialization failed` 因无来源而删除（全仓断言已核）；五处测试（`utest_net_socket` `utest_net_poll` `utest_net_engine` `utest_inspector_client` `itest_inspector_robustness`）删除自备会话并把回环桩改指 `blocking`。`utest_net_socket` 沿用现文件名，仅把「目标单元」指向 `blocking.h`——历史 §8.8 回归条目仍以该文件名为可达引用。
- 错误码：本切片**无新增码**，`net-runtime-failed` 获得新落点（此前只在会话构造失败时产生，现在句柄创建点即可能产出）。
- 回归：Windows（MinGW）与 WSL（POSIX）两条验证腿上，上述六个套件全绿；Windows 腿的 IPv6 双栈用例因本机 v6-only 而 `AURORA_TEST_SKIP`（环境缺失，非行为差异）。1MiB+7 往返用例的读线程结构与「对端关闭 = `read_some` 返回 0」口径均未改动，是「别名只做转发、语义零漂移」的主要证据。

#### 8.8.17 测试替身与等待辅助：脚本化时钟、脚本化对端与共用泵（CHG-018 测试替身切片）

提案 §14 承诺的「离线可验证」在本切片落地：故障现场不再靠真实网络与真实时钟碰运气，等待口径不再逐文件复制。设施全在 `tests/framework/`（仓库私有，不进 `include/`、不进 `aurora_api.json`），公共 API 与库行为除下述一处缺陷修正外零改动。

- **落点四份头 + 一枚库内接缝**：`async_wait.h`（条件等待与超时自报）、`net_wait.h`（宿主驱动泵）、`fake_engine.h`（脚本化时钟的 `net::Engine` 替身与作用域守卫）、`scripted_peer.h`（按脚本应答的回环监听口，用于代理握手与隧道类故障）。替身时钟接进库的路径是 `src/aurora/net/detail/engine_override.h` 的进程级 `Engine*` 覆盖接缝（内部头，`Engine::instance()` 先查它再回落单例）——**公共面不提供「换掉引擎」的开关**，那等于把完成投递线程的所有权交给调用方。
- **三类职责互不重叠**：`FakeEngine` 只管**时间**（到期任务由用例显式推进），`ScriptedPeer` 只管**对端字节**，`net_wait.h` 只管**就绪泵**。缺任何一条都会红成别的故障：`Engine::post` 的立即任务（名字解析、竞速交错）在脚本化时钟下没人跑，用例就一直睡到预算耗尽，报「超时」而不是「忘了推时钟」——故 `FakeEngineScope` 安装替身时同步把 `run_pending` 挂进泵尾巴（`pump_tail_slot()`），拆除时先摘尾巴再交回引擎入口，次序与安装相反。
- **等待口径收拢**：`wait_ready` 窗口 20 ms、预算 3 s、`HostDriven` / `EngineGuard` / `pump_once` 此前在 `utest_net_transport` / `utest_net_remote_connect` / `utest_net_datagram` / `utest_net_engine` 四份文件里各写一遍（预算还 2 s / 3 s / 1 s 不一），现一律取自这两份框架头；文件内只留把共用形态接回既有调用点的薄转发。**转发返回静默 `bool`**：竞速用例把 `pump_until` 的结果当分支条件用，替身目录里「这段时间里不该发生」的判据用负向断言，转发若自带断言会把正常分支与负向判据反着报成红灯。口径正文见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md) §3.7。
- **故障目录覆盖度**：`utest_net_test_double` 14 例覆盖七类全部——超时、取消、半关闭、乱序（在流上表现为**分帧边界错位**而不是丢包）、超大帧、代理不可达、TLS 握手失败，另加慢连接（握手停在半路）；每类都断言「结局码 + 恰一次交付 + 事件源已释放」。第七类随 TLS 切片补进目录（`a_peer_fatal_alert_settles_the_handshake_exactly_once`：脚本化明文对端在收到 `ClientHello` 头之后回一条 `alert(21) / handshake_failure(40)`，判据为该次握手恰一次结局、通道不处于打开态、事件源已释放且**不悬挂**；结局码按 §8.8.18 的「告警与记录层失败同码」口径为 `net-tls-handshake-failed`）。此前记下的「TLS 尚未落地，先以连上了但握手永不完成的超时形态兑现」一并作废——那一枚超时形态（`a_silent_proxy_times_out_exactly_once_without_wall_clock`）保留，因为它测的是时限裁决而不是告警处置。裁决与理由见提案 §16 第 17、22 项。
- **零出站是构造性成立而非环境假设**：用例只连回环；「端口上没有服务」经「绑定即关闭」取得而不写死端口号；每条连接都断言 `is_loopback_peer()`，故禁网 CI 上不存在可达的外呼路径，判据 12 的「CI 无出站」前提由此可证伪而不是靠约定。
- **库内缺陷修正（一处）**：`connect_via_proxy` 的时限回调改为**先落超时结局、再关在途的那条腿**。原先次序相反，`close()` 让未完成的握手读以「已取消」再回一次并先占住结局，调用方看到的失败原因就是 `net-cancelled` 而不是 `net-connect-timeout`——同一件事实的两种说法必须留下成因那一种。判据为替身目录的「静默代理」用例（红时正是报成取消）。
- 错误码：本切片**无新增码**，也未启用新落点。
- 回归：net 与基础件相关共 **15 个套件 147 例**两腿同口径复跑。Windows（MinGW）腿 146 通过 + 1 跳过（`utest_net_socket` 的 IPv6 双栈用例因本机 v6-only，属环境缺失非行为差异），其中 `utest_net_test_double` 13 / `utest_net_transport` 17 / `utest_net_remote_connect` 11 / `utest_net_datagram` 9 / `utest_net_engine` 9 / `utest_framework_async_wait` 8；WSL（POSIX）腿同 15 套 **147 例全通过**（`utest_net_socket` 13 例含 IPv6 两例，逐值与 Windows 腿一致）。`utest_net_remote_connect` 在两腿同为 11 例，WSL 腿实测合计 2593 ms（最慢一条是竞速窗口本身，非等待预算撑大）。

#### 8.8.18 TLS 通道：`TlsTransport` 会话状态机与系统栈后端（CHG-018 TLS 切片）

公共头 `include/aurora/net/tls.h`（`TlsContext` / `TlsVersion` / `PeerCertificate` / `TlsTransport` / `TlsHandshakeDone`）+ 平台无关实现 `src/aurora/net/tls.cpp` + 库内接缝 `src/aurora/net/detail/tls_backend.h` + 后端 TU（四枚文件恒在源列表里、每枚整体自罩在自己的引擎宏下，故**每次构建生效的后端恰好一枚**：Windows `tls_schannel.cpp`、macOS `tls_securetransport.cpp`、provider 形态 `tls_openssl.cpp` / `tls_mbedtls.cpp`；未选任何后端时由 `tls_unavailable.cpp` 给出那两枚符号）+ 装配唯一决策点 `cmake/AuroraNetTls.cmake`。**只有客户端**：没有 `TlsListener`，本库的监听腿是本机 IPC 与 Inspector 回路，不构成公网服务端场景。握手成功之后与 §8.8.11 的流**逐字同构**（读队列按登记次序满足、0 字节 = 对端关闭写向、写高水位背压、暂停读、取消、半关闭），故 L3 协议引擎不需要知道底下是否加密。公共头**零平台宏**（`SPEC.PLATFORM.ZERO-IFDEF.001`），也没有第二个「TLS 是否可用」的查询入口。

- **分工边界是规范性的一条：后端持有密文缓冲**。握手的分帧、记录边界与 leftover 的归位只有后端自己清楚（Schannel 按 `SECBUFFER_STREAM` 就地消费并回写剩余长度，OpenSSL/mbedTLS 按 memory BIO 推拉），把它抬到公共侧就得再表达一次「一条记录」的概念。上层只做三件事：`push_incoming()` 投字节、催后端推进、把 `outgoing` 交付底层流。接缝不承诺 thread-safe——会话状态机以每连接一枚互斥量串行化全部调用，后端因此可以用裸成员。
- **`next_records(plaintext, outgoing)` 的第二枚出参是例外通路**：数据阶段也可能产出手握字节（服务端发起的重协商、TLS 1.3 的后握手 NewSessionTicket）。实测形态是 `DecryptMessage` 回 `SEC_I_RENEGOTIATE`（票据以 255 字节记录抵达）——**跳过这一轮次不可行**：一次往返只消费一条外层记录，不补一次无输入的 `InitializeSecurityContext` 的话，同一上下文里的下一条记录一律以 `SEC_E_DECODE_BITS` 失败。把「数据阶段只产明文」当简化写进接缝，等于把系统的合法状态机切成两半。
- **能力位与「调用真的能成功」严格同源**：`Engine::capabilities().tls` 由 `detail::tls_backend_available()` 回答，而这枚函数由被编译进来的那个后端 TU 定义（有后端者 `true`、`tls_unavailable.cpp` `false`）。装配模块因此只保证「恰好编译一个后端 TU」，不在这里放运行期真值；能力位先于实现点亮会诱导调用方去分支一条今天不存在的路径。
- **口径不合在零后端处即判**（`invalid_inputs_are_decided_before_any_backend` 用例覆盖）：空 `server_name` 与 IP 字面量落 `net-endpoint-invalid`（IP 目标按证书校验语义没有可核对的名字，本层不跳过校验）、`max_version` 低于 `min_version` 落 `net-tls-version-mismatch`（允许集为空是配置矛盾，不该由协商现场去发现）、空回调不发起任何登记。危险开关 `allow_insecure_certificate` 置真时在发起处留一条 `warning` 诊断（正文点名「local testing only」），它跳过的是链与主机名判定，**不影响 SPKI pin**——「不设防」与「防错对象」是两条必须能分别表达的决策。
- **信任判定由本层产出，系统不给「半个结论」**（Windows 腿的落码形态）：凭据恒带 `SCH_CRED_MANUAL_CRED_VALIDATION`，信任结论一律由 `verify_chain()` 经 CryptoAPI 链引擎产出。理由是追加信任根这件事在 Schannel 的自动校验通路里做不到：`trusted_certificates` 与系统 ROOT 库**合并**成内存存储后交 `CERT_CHAIN_ENGINE_CONFIG.hExclusiveRoot`（未给追加根时传空引擎 = 完全走系统缺省策略，两条通路互不遮蔽）；做成替换会让所有对接公网的用例突然不认合法证书。链的中间证书不需要本层搬运——Schannel 把对端送来的整条链挂在叶子所属的内存存储上，实测 `entries` 随对端发送的张数增长，故把它作为 `hAdditionalStore` 交出即可；`SECPKG_ATTR_REMOTE_CERT_CHAIN` **本机实测不可用**（返回结构 `cbSize=1`，按链上下文解引用即访问违例）。只认叶子会建成部分链（`CERT_TRUST_IS_PARTIAL_CHAIN`）而被读成「证书不可信」，现场指向完全错误的方向。**吊销检查不在本腿**：`CertGetCertificateChain` 的 `dwFlags` 为 0，即不拉 CRL 也不走 OCSP——离线可验证的判据集合里没有它，接进去等于给每条握手加一次外呼，记为已知边界而非疏漏。
- **主机名核对按 SAN 优先、CN 只在 SAN 缺席时兜底**：SAN 里存在 dNSName 却没有一条命中，说明证书已明确表达「可核对的名字不在这张证书里」，再拿 CN 兜底等于把现代证书规则退回 2000 年代。通配符只允许最左标签（`name_matches`），且大小写不敏感。
- **SPKI pin 的形态是跨后端逐值判据**：`PeerCertificate::public_key_sha256` = base64(SHA-256(DER(`SubjectPublicKeyInfo`)))，不是整证摘要也不是十六进制指纹——按整证 pin 会在证书续签（公钥不变）时无预警失配，而那是运维最常见的「换证不换钥匙」。判定在 `net/tls.cpp` 完成（与后端无关），不命中落 `net-tls-pin-mismatch` 并把命中的摘要带进 `message`。摘要计算侧有一条硬约束：`BCrypt*Hash` 的哈希长度**现问不假设**、失败一律回空串——把失败读成「一枚全零摘要」会让「pin 不命中」被静默吞掉（本函数第一版正是这样）。
- **客户端 ALPN 宣告的通路只有一条，且不在凭据层**：`TLS_PARAMETERS` 的 `rgstrAlpnIds` 按文档是给「该组参数所适用的 ALPN 标识」划定范围，本机实测无论宽窄字符编码都协商不出结果（`SECPKG_ATTR_APPLICATION_PROTOCOL` 恒回 status=None）。可用形态是**首枚** `InitializeSecurityContextW` 随附的 `SECBUFFER_APPLICATION_PROTOCOLS` 输入缓冲，载荷布局为 `u32 长度 | u32 ext=ALPN | u16 表字节数 | (u8 单条长度 + ASCII 标识)*`（`ISC_REQ_ALPN` 请求位实测非必要，故不带）；系统只在生成 ClientHello 那一次读它，之后的调用给了也不生效。空集合必须**不产缓冲**：带着 12 字节的空表调用会回 `SEC_E_ILLEGAL_MESSAGE` 且连 ClientHello 都发不出去（本层由 5 通过/13 失败的整片回归实测出这条）。协商结果经 `SECPKG_ATTR_APPLICATION_PROTOCOL` 回读，未宣告与未选中都回 `net-option-conflict` 而**不是空串**——把「没有 ALPN」报成空串会让调用方把「未协商」误读成「协商出一个空协议」，后者的正确处置是关掉连接。
- **mTLS 客户端身份只认带私钥登记的那一张证书**：`TlsContext::client_identity` 是 PKCS#12 的**字节内容**（口令 `client_identity_passphrase`），导入个人存储后必须逐张查 `CERT_KEY_PROV_INFO_PROP_ID`；把没有私钥的证书交给 Schannel，只会在需要客户端认证时才失败，而那次失败看起来像「对端拒了我」，现场不留任何配置信息。没有可用私钥即在 `make_tls_backend` 期落 `general-invalid-argument`。
- **结局纪律：每个请求恰一次，且所有分支共用同一条收尾**——登记动作在锁内、交付在锁外，分支里就地 `return` 会跳过 `fire(tasks)`，被丢弃的正是「读必须有结局」那批回调（现场表现为挂起的读永远不落地，`peer_process_disappearance_settles_a_pending_read` 抓到的就是这条）。同批修正两处：`resume_read()` **必须先解底层流的暂停**再推进会话（顺序反了就是死锁形态：本层催出的读投递被仍在生效的暂停挡回，而挡回来的请求只有恢复后才可能被满足）；`peer_certificate()` 在通道已关闭后按公共头口径回 `net-socket-failed`，而不是继续交出固化的摘要。关闭幂等、`close()` 后一切读写都有确定结局、不悬挂。
- **provider 探测规则（装配面，Linux 缺省即无后端）**：`AURORA_NET_TLS_PROVIDER=<static|shared>:<prefix>`，`prefix` **只接受安装根**（不做向上猜测或递归搜索，写错路径时诊断打印实际检查过的 `include/` 与 `lib/` 组合）；后端种类由头文件探测推导（`<prefix>/include/openssl/ssl.h` → OpenSSL，`<prefix>/include/mbedtls/ssl.h` → mbedTLS，多命中 OpenSSL 优先并显式记录，两枚皆无 configure 期硬失败而不是静默降级成「无 TLS」），引擎宏由此推导而**不接受手工设置**；库定位除 `lib/` 外还查 `lib/<三元组>`（Debian multiarch 的实际位置，属「安装根」口径之内，不带它就会在 WSL/Ubuntu 上「头探到了、库说没有」）。探测通过后生成导入目标 `aurora::net::tls`（`aurora` 本体 / 测试 / 工具共用同一份 include 与库路径），`shared` 的运行期寻址交宿主部署，不写 RPATH。宏为 `PRIVATE` 作用域（公共头零 `#ifdef`，消费者编译不需要它）；系统栈腿的链接面是 `secur32 crypt32 bcrypt`（PUBLIC 传导）。仓库**不自带任何 TLS 源码**。
- 错误码：本切片启用 §10 已规划的 `net-tls-handshake-failed` / `net-tls-cert-rejected` / `net-tls-version-mismatch` / `net-tls-engine-unavailable` / `net-tls-pin-mismatch` 五枚，**无新增码**；失败 `message` 一律携带后端原生状态码的十六进制形态（`status=0x80090326` 一类），因为「谁拒了我」只有原生码能区分。
- **对端 fatal alert 与记录层失败同码**：三腿都没有可依赖的 alert 描述符出口（Schannel 只回 `SEC_E_*`、OpenSSL 回 `SSL_get_error` + reason 文本、mbedTLS 回握手返回码与 `mbedtls_ssl_get_verify_result`），故不补「收到致命告警」的第六枚码——那会让调用方以为能按告警类型分支，而今天只能读文本。告警一律落 `net-tls-handshake-failed` 并把平台成因写进 `detail`；离线判据（「对端在 `ClientHello` 之后回 fatal alert」）在 §8.8.17 的故障目录里以 `net-tls-handshake-failed` + 不悬挂 + 事件源已释放三条收口。裁决理由与顺延见提案 §16 第 22 项。
- **「服务端要求客户端证书」的拒绝时机随协议版本分岔，公共侧只承诺「匿名会话拿不到回显」**：TLS 1.2 及以下是握手内事件（服务端在 `CertificateRequest` 后直接告警终止），TLS 1.3 是**握手后**事件——客户端的 `SSL_connect` / `SSLHandshake` 已经成功，致命告警在第一次记录交换时才抵达（`RFC 8446` §4.3.2 / §4.4.2 的时序）。故「握手返回成功」不蕴含「服务端已接受客户端证书」，判据也不能写成「匿名握手必须失败」。实测：两条 provider 腿先成功后落败、Windows（Schannel）在握手内即拒，用例按腿无关形态收口（匿名半只断言「不得取得回显且必须有结局」）。见提案 §16 第 19 项。
- **provider 腿的字节推拉各自贴合自家形态**：OpenSSL 用两枚 memory BIO（收发各一，所有权随 `SSL_set_bio` 移交，此后由 `SSL_free` 释放），mbedTLS 用 `mbedtls_ssl_set_bio()` 挂本层的 `bio_send` / `bio_recv` 回调、取不到字节时回 `MBEDTLS_ERR_SSL_WANT_READ`。两者与 Schannel 的 `SECBUFFER_STREAM` 就地消费、SecureTransport 的 `SSLSetIOFuncs` + `errSSLWouldBlock` 一起，构成同一接缝（后端持密文缓冲）的四种实现——接缝只承诺「上层交付底层流」，不承诺后端内部形态一致。mbedTLS 侧另两条口径如实记账：熵源走 PSA（`psa_generate_random()`，无返回码故失败不可见）、**不装** `mbedtls_threading_set_alt()`（那是全进程的一次性决定），串行化完全依赖本接缝的每连接互斥量。
- **macOS 系统栈腿的四处形态与一处已知不重合**：信任判定经 `kSSLSessionOptionBreakOnServerAuth` 交回本层（该开关一设即关闭系统栈的自动校验，文档写明），追加根经 `SecTrustSetAnchorCertificates` + `SecTrustSetAnchorCertificatesOnly(false)` **合并**进系统根、判定用 `SecTrustEvaluateWithError`，校验完再调一次 `SSLHandshake` 续跑；主机名判定交 `SecPolicyCreateSSL(true, hostname)`（与 OpenSSL 腿交 `SSL_set1_host` 同口径），因此本腿**不做** CN 回落，而 Schannel 腿的手工校验通路必须自己实现、对「只有 CN 没有 SAN」的证书回落到 CN——这一处跨腿差异属平台现行策略，记为已知不重合；`PeerCertificate::public_key_sha256` 在本腿由证书 DER 内切 SPKI 段再取 SHA-256（`SecKeyCopyExternalRepresentation` 对 RSA 回 PKCS#1、对 EC 回 X9.63，都不是 SPKI，取它算 pin 会与其他腿永久失配）；危险开关 `allow_insecure_certificate` 跳过的是**结论**不是**动作**（仍评估一次，因为 `SecTrustGetCertificate*` 要求先评估，否则「设了开关反而读不到对端证书」）。该腿本机只有编译级证据（无 Apple SDK；clang-format + 对手写声明头的语法检查通过），运行裁决方是 CI 的 `macos-securetransport-system` 腿。见提案 §16 第 21 项。
- 测试设施：`tests/framework/tls_peer.h` 用宿主现成的 `openssl` 命令行拉起真实 TLS 服务端（`s_server`，只绑 `127.0.0.1`，`-rev` 回显形态）并按用例现签自签资产（SAN=`DNS:localhost`），私钥存活期不长于用例、随 `isolation::end_case()` 删除；命令行缺失时走 `AURORA_TEST_SKIP` 报成环境缺项而不是逻辑失败。参数一律 argv 直送不经 shell（MSYS 会把 `/CN=localhost` 当路径改写）。与 `scripted_peer.h` 的分工：那一份接管**明文**对端字节，这一份接管 TLS 层对端（加密、证书、记录），「取消落在未完成的握手」这类用例仍用 `scripts::silent()`。
- 回归：`utest_net_tls` **20 例**（握手前判定 / 空回调不发起 / 无后端诚实落点 / 能力位与探测同源 / 握手与对端摘要 / 与 L1 读写同构 / 载荷跨记录往返 / 自签默认被拒 / 追加根使其可信 / 过期证书即使追加根仍被拒 / 主机名不匹配即拒 / 危险开关留痕 / pin 不命中且危险开关不遮蔽 / ALPN 回报 / ALPN 无交集 / 取消恰一次 / 关闭幂等不悬挂 / 对端进程消失仍有结局 / 版本界冲突有结局 / mTLS 客户端身份）。三条腿各自的口径：**Windows（MinGW，Schannel 系统栈）19 通过 + 1 跳过**、**WSL（OpenSSL provider，`shared:` 系统安装根）19 通过 + 1 跳过**、**WSL（mbedTLS 3.6.5 provider，`static:/usr`）18 通过 + 2 跳过**——跳过的第一枚是 `absent_backend_reports_engine_unavailable`（有后端的构建里按环境缺失跳过，三腿同因），第二枚只在 mbedTLS 腿出现，是 `client_identity_satisfies_a_verified_peer_request` 的 PKCS#12 缺口（该腿在建后端前即落 `general-invalid-argument`，用例据此报成缺项而不是逻辑失败，见提案 §16 第 20 项）。另有一枚**无 provider** 的 Linux 构建（configure 日志 `TLS backend: none`）同套件 **4 通过 + 16 跳过**，且 `absent_backend_reports_engine_unavailable` 在该腿转为实跑通过——这是判据 7「未给 provider 时 configure 成功但能力位为假、TLS 构造报 `net-tls-engine-unavailable`」的可执行形态。同批 `utest_net_test_double` **14 例三腿全通过**（含随本切片补进目录的第七类故障），离线套件在三腿**失败数为 0**。判据 5 的「系统栈引擎与 provider 引擎各跑通一次」就此闭合（provider 两枚都跑过；离线组里的过期证书判据由 `tests/framework/tls_peer.h` 的 `Lifetime::Expired` 现签一张有效期整体落在过去的自签证书来造，宿主 `openssl` 无回填日期的旋钮时该例报成环境缺项而不是伪造一次拒绝；macOS 系统栈腿只有编译级证据，见提案 §16 第 21 项）。真机探针 `aurora_verify_tls_live` 在三腿同口径 PASS，且**同一张真实证书的两类摘要逐字相同**（`CN=example.com` 的叶子与签发者）：SPKI 摘要三腿都是 `toyMGpiThuoVaTygu6QeCcnum2PKJknnGOsHalH7BzU=`（pin 计算跨腿可移植的唯一证据），可分辨名文本三腿都是 `C=US, O=SSL Corporation, CN=Cloudflare TLS Issuing ECC CA 3`。后者是本轮由实测纠正的形态：OpenSSL 腿原先用 `XN_FLAG_RFC2253`，而它按 RFC 2253 **逆序**展开且分隔不带空格，与 Schannel（CryptoAPI）/ mbedTLS（`mbedtls_x509_dn_gets`）两腿的 DER 正序形态不一致——`TC-NET-006` 的跨腿逐值判据据此不成立，故该腿改为自己按 DER 条目次序渲染（短名 + `, ` 连接），macOS 腿的实现本来就按 DER 走一遍故同形（其一致性属代码形态层面，运行判据由 CI 的 `macos-securetransport-system` 腿补）。

---

#### 8.8.19 HTTP 发起侧值类型：`Method` / `Request` / `Response`（CHG-018 HTTP 切片·值层）

新增公共头 `include/aurora/net/http.h`（命名空间 `aurora::net::http`）与实现 `src/aurora/net/http.cpp`（实现离头）。本小节只落 §6 草图的**值层**——一次 HTTP 交换的输入（方法、目标、字段、消息体、发起侧选项）与输出（状态行、字段、已物化消息体）——它是发起侧 `Client` 组装的对象，也是把字节层 `net/detail/http_wire.h` 接给调用方的最后一层纯逻辑。零 socket、零 DNS、零平台头，故三平台逐值一致、wasm 腿同样可跑。

四条落码口径：

- **字符串只能经显式解析进入**：`Request` 的六枚同名工厂（`get` / `head` / `post` / `put` / `patch` / `delete_`）在构造时即 `Url::parse` 目标文本；工厂返回 `Request` 而非 `Result`，解析失败被暂存进对象、到 `render_head()` 才以 `net-url-invalid` 原样交出。这一条把 §12「手拼 URL 是 AI 生成代码的头号错误源」的判据落到最早可确定的位置。`delete_` 的尾下划线是 `delete` 为关键字下的既定拼写（clang-tidy 明示不可自动修复，逐名豁免）。
- **方法令牌与线上文本一一对应且大小写敏感**：`method_text()` 给出 `RFC 9110 §9` 的大写规范拼写，`method_from_text()` 是其逆、只认这六枚（往返一致），小写与自定义方法（`PROPFIND` 一类）一律 `http-request-invalid`——自定义方法本批次不提供，静默接受会让 `Content-Length` 的取舍失据。换算表是文件内唯一真值源（`AURORA_HTTP_METHOD_TABLE`）。
- **`Content-Length` 只在有体且未自声明时补齐**：`render_head()` 复制字段表，体非空且既无 `Content-Length` 又无 `Transfer-Encoding` 时按体长度补一枚，其余（含 `Content-Length: 42` 显式声明）不改写；`Host` 的补齐仍由字节层按目标 URL 执行（含非缺省端口）。`HEAD` 请求丢弃消息体（`body()` 对 `Head` 无效），应答无体的判定经 `is_head()` 交给 `wire::framing_of`。
- **链式字段的两种语义**：`header()` 追加保序、同名可多条（对齐 `Headers` 不折叠口径），`set_header()` 清同名后追加到末尾；`timeout()` / `max_redirects()` 是发起侧选项，前者缺省 `nullopt`、后者缺省 `10`。`Response` 交出已物化（解压完毕）的消息体：`status` / `reason` / 大小写不敏感 `header()` / `body()`（未解码原始字节）/ `text()`（按 `Content-Type` 的 `charset` 现场解码为 UTF-8，返回 `Result<std::string>`）。透明解压（`Content-Encoding`）属物化之前的阶段、由发起侧管线执行，值层不碰；charset 解码属 `text()` 本身——首批 UTF-8（剥前导 BOM）与 Latin-1，缺 `Content-Type` 或缺 `charset` 按 UTF-8 缺省（RFC 9231），其余 charset 报 `http-charset-unsupported`，非法 UTF-8 序列原样直传、绝不静默替换。解码落在 `text()` 而非物化阶段，是「解码失败可被调用方显式看到」与「原始字节恒可取」两条同时成立的最小形态。

工具链面：伞头 `aurora/aurora.h` 与 `tools/check/umbrella_manifest.txt` 收录 `net/http.h`；`Method` 登记 `tools/include/known_enums.h` 并在 `itest_known_enums` 加编译期锚点、刷新 `aurora_api.json` 的 `enums` 段；**初件无新增错误码**（复用切片首件已落的 `http-request-invalid` 与既有 `net-url-invalid`），§12 charset 解码补齐时**新增 `http-charset-unsupported`**（`text()` 启用；其定义随之下沉 `http.cpp` 并 include 库内 `core/utf8.h`，公共面无新头 / 新枚举），`http-content-encoding-unsupported` 已随 §8.8.20 的透明解压接入启用。配套 `utest_net_http`（6 例：方法换算往返、目标解析失败延后到 `render_head`、`Host` / `Content-Length` 补齐、`HEAD` 弃体与链式字段 / 选项、应答取值口径、`text()` 按 charset 解码的六型分岔）。

**待建（本切片其余部分，随后续实现）**：§6 发起侧 `Client` 的 `open_sse`、`use(hook)` 与 `Session` 的 Cookie Jar 与连接池（`send` 核心闭环已落地，见 §8.8.20），以及 §12 的认证便捷件——`Bearer` / `Basic` 与 `Authenticator`（含 401 / 407 的凭据重放）——仍未落地。§12 的其余语义补齐件已各自收口：进度节流与分段超时随 `Client::send` 落地（见 §8.8.20）；流式接收随 `Client::open_stream` 落地（见 §8.8.21）；失败分型随 `HttpErrorClass` 落地（见 §8.8.22）；重试策略随 `RetryPolicy` 落地（见 §8.8.23）；条件请求与 `RFC 9111` 最小集缓存校验随 `Client::cache()` 落地（见 §8.8.24）；发起侧的代理接入随 `Client::proxy()` 落地（见 §8.8.25）；§12 / §13 的声明式安全策略随 `NetPolicy` 落地（见 §8.8.26，同批把「`Url::is_secure()` 供 `NetPolicy` 拒明文」那条预留口径兑现）；链路标识随 `Client::request_id()` 落地（见 §8.8.27）；上传便捷面（`BodySource` 流式请求体、`MultipartWriter`、字节范围、`Expect: 100-continue`）随 §8.8.28 落地。charset 解码已随 `Response::text()` 落地并启用 `http-charset-unsupported`；内容编码已升级为 core 级通用能力 `aurora::compression` 落地，见 [`01-core.md`](01-core.md) §12，并由 HTTP 物化管线接入、`http-content-encoding-unsupported` 随之启用，见 §8.8.20。契约引用提案 [`changes/xplat-network-stack/proposal.md`](../changes/xplat-network-stack/proposal.md) §6 / §10 / §12 / §17。

#### 8.8.20 HTTP 发起侧客户端：`Client::send`（CHG-018 §6 / §12 发起侧·核心闭环）

新增公共头 `include/aurora/net/http_client.h`（命名空间 `aurora::net::http`）与实现 `src/aurora/net/http_client.cpp`，回调→协程桥接落库内 `src/aurora/net/detail/http_await.h`（不占公共 API、不进伞头）。`Client` 把一份 `Request` 送出去、拿回物化的 `Response`：`http://` 直连 `StreamTransport`、`https://` 先在流上 `TlsTransport::handshake()` 再套读写（L3 对底下是否加密无感），写出 `render_head()` + 体、逐段喂 §8.8.19 的接收侧 `wire::ResponseAssembler` 按分帧物化，物化完毕再按 `Content-Encoding` 透明解压，按 `Location` 跟随重定向，三档超时（`Timeouts{connect, read_idle, total}`）任一到点落 `http-timeout`。落码口径：

- **协程出口 `CoroTask<Result<Response>>`、参数按值**：L1 传输刻意只给回调出口（把 `Task` 写进 L1 公共头会让下层反向依赖 L4，见 `net/transport.h`），协程形态归上层给出——本发起侧即那「上层」。§6 草图写 `send(const Request &) -> Task<Result<Response>>`，落地改两处并记此：返回 `CoroTask`（仓内既有协程任务型；线程池的 `Task<T>` 只能由 `async(fn)` 在池上 resolve，无法被事件驱动回调填充），参数改**按值**（协程引用参数不安全——帧不延长引用生命期，`cppcoreguidelines-avoid-reference-coroutine-parameters`）。成败一律以 `Result` 表达，绝不抛异常穿越协程帧（§18-4）。
- **桥接在库内、续体恒回主线程**：`net/detail/http_await.h` 的 `CallbackAwaiter` 把 connect / handshake / read / write 的完成回调经 `post_to_main` 恢复续体，与 `state::co_async` 同一条回投通道，故调用方在 `co_await` 之后可安全触碰响应式状态。awaiter 不外露。
- **一次 `send` = 一条连接的生命周期**：每（重定向）跳新建连接、应答物化后关闭；连接池、keep-alive 复用、`Session` 的 Cookie Jar 属后续切片，本件不为「复用」预先牺牲正确性。重定向按 `Location` 相对当前目标解析：`303` 把非 `HEAD` 改判 `GET`、`301` / `302` 对 `POST` 转 `GET`、`307` / `308` 严格保留方法与体，次数超 `Request::max_redirects()` 落 `http-redirect-limit`；`UntilClose` 形态经对端 FIN 判全，其余形态提前收摊落 `http-protocol-error`。
- **三档超时各自独立**：`connect` 覆盖建连（含 TLS 握手）、`read_idle` 是相邻两次读交付之间的滑动空闲上限、`total` 覆盖整请求全程；步骤 2 的单一 `Request::timeout()` 现等价 `total` 档（向后兼容保留）。各以一枚 `StageTimer`（引擎 `post_after` + 共享 `Cancellation`）实现，阶段结束即停表——`post_after` 不可撤销，故把共享 `live` 标志置假令尚未到期的闭包 no-op、不误伤后续；任一档先触发即记档号并取消令牌，收尾统一改道 `http-timeout` 并在 `detail` 标明是哪一档。新增公共类型 `Timeouts`（三枚 `optional<std::chrono::milliseconds>`）落 `net/http.h`，经 `Request::timeouts()` 存取。

- **内容编码在物化之后、值层之前**：应答体按分帧物化完毕，才依 `Content-Encoding` 逐级还原，因此 `Response::body()` 恒交未解码的（已解压）原始字节、`text()` 只管 charset。宣告与支持集同源——`AURORA_HTTP_ACCEPT_ENCODING` 就是解码器认得的那两枚（`gzip, deflate`），core 注册表的 gzip / zlib / deflate 三槽均已注册且调用方未自设 `Accept-Encoding` 时由 `send` 注入，已设则不覆盖；三槽未全部注册（内置解码器仅在 `AURORA_ENABLE_COMPRESSION=ON` 的构建编入，应用可自注册补位）时既不宣告、应答又带编码即落 `http-content-encoding-unsupported`，绝不把压缩字节当明文交出。编码链**书写次序是应用次序、解压走反序**（`RFC 9110 §8.4`：`gzip, deflate` 表示载荷先经 gzip 再经 deflate 加工，还原从右往左逐层剥），链上出现支持集之外的名字即整链拒绝并报出该名字；`deflate` 名义是 zlib（RFC 1950）但存在历史服务器发裸流（RFC 1951）的歧义，先试 `Zlib`、仅在其报「字节损坏」时回落 `Deflate`，两试皆坏才把 zlib 那条判定原样上报——这一消歧属 HTTP 层编排，不进 core 通用 API。`Content-Encoding` 落在无体应答（`framing == None`，含 `HEAD` / 1xx / 204 / 304）上不参与校验，那字段描述的是不存在的载荷。core 侧三枚 `compression-*` 在 HTTP 出口**全部有映射**：`compression-unavailable` → `http-content-encoding-unsupported`、`compression-output-too-large` → `http-body-too-large`（`detail` 写明越的是 `max_decompressed_bytes` 而非 `max_body_bytes`，两道闸各管一段：前者管解压产物、后者管线上传来字节）、`compression-data-corrupt` 及其余一律 → 新增 `http-body-corrupt`。解压产物另有独立上限 `Client::max_decompressed_bytes()`，未设则跟随 `max_body_bytes`——这是解压炸弹的唯一显式闸。文法解析、链序、逐层解码与错误映射全落库内 `src/aurora/net/detail/http_encoding.{h,cpp}`（不占公共 API、不进伞头），故零 socket 即可逐值断言。

- **两腿逐值回归实测到一处悬空视图（已修）**：同一份 `scan_transfer_encoding`（`net/detail/http_wire.cpp`）在 Win32（MinGW）腿把 `Transfer-Encoding: chunked` 判成 chunked、在 WSL（GCC 15 / POSIX）腿判成「本构建不解的传输编码」，四条走 chunked 的用例随之整片转红。根因不在逻辑而在**生命周期**：那里迭代的是 `std::vector<std::string>`，`raw.substr()` 产出的是临时 `std::string`，而 `trim_ows` 返回 `string_view`——视图指向已析构的临时量。短值走 SSO、字节就躺在临时对象的栈槽里，是否仍可读到原文取决于优化级别与栈复用次序，一腿侥幸、一腿翻车，正是「推断代替实测」会漏掉的那类跨腿差异。现切段一律从 `std::string_view{text}` 上做，`text` 指向的字节活在入参容器里、有效期覆盖整个循环。判据不新造用例：原有的 chunked 分帧用例就是这条缺陷的回归口径。

- **进度节流：合流闸在库内，发射机会由宿主提供**：挂了 `Request::on_progress()` 才观测，未挂时整枚闸零作为（不写入、不推进闸号、不发一次），无需求的调用方零行为改动。字节到达只更新「最新值」槽（上传 / 下载各一枚，同一机会内互不覆盖），发射发生在每一次可发射机会上且**每方向至多一笔**——缺省以「一次 `flush` = 一回合」为闸，装了 `Client::progress_frame_source()`（返回帧号的 `std::function`）时改以帧号为闸，严格「每帧至多一次」；闸分双相而不硬绑帧循环，是因为 L3 不得反向依赖 widget/app 的帧计数器，而频次判据又必须能在无 GUI 的测试里写成可证伪的断言。两相共有同一个例外：**每方向末值恒发一次**且不看闸号，`finished` 只宣告「这一趟收摊」，成败仍由 `Result<Response>` 表达。上传侧要节拍就得把请求体分块写出（64 KiB 一块），故同批定死两条：L1 的写契约是「全部字节进入内核或明确失败才算完成」，计数按块长推进而不回读交付数；分块只改库内完成次数，**线上字节序列与一次写完逐字节相同**，判据落在对端收到的请求全文上。下载侧计的是**分帧解出的线上字节**而非透明解压后的字节（物化路径的还原发生在收完之后，中途给不出还原量），总量只有 `Content-Length` 形态给得出，`chunked` / `UntilClose` 一律 `total_known = false` 而不是拿已到达数冒充。闸的槽位与闸号判定落库内 `src/aurora/net/detail/http_progress.{h,cpp}`，零 socket、零时钟、零线程，故发射频次可逐名对齐回合数。裁决见提案 §16 第 25 项。

工具链面：伞头与 `tools/check/umbrella_manifest.txt` 收录 `net/http_client.h`，`aurora_api.json` 随生成器刷新；`ProgressPhase` 登记 `tools/include/known_enums.h` 并在 `itest_known_enums` 加编译期锚点（`enums` 段 +1），进度闸本体落库内不进伞头；本切片**新增两枚错误码**（`http-content-encoding-unsupported`、`http-body-corrupt`，均 `retryable = false`），其余复用 `http-timeout` / `http-redirect-limit` / `http-body-too-large` / `http-protocol-error` 与既有 `net-*` / `net-url-invalid`。配套 `utest_net_http_client`（22 例：Content-Length 与 Chunked 两形态物化、跨对端 `302` 跟随、超限落 `http-redirect-limit`、`FakeEngine` 脚本化时钟分验 `total` / `read_idle` / `connect` 三档 `http-timeout`、gzip 体透明解压、`deflate` 裸流回落、多段链反序还原、`max_decompressed_bytes` 越限、缺省注入与不覆盖调用方 `Accept-Encoding`、`br` 落不支持、声明编码却解不开落 `http-body-corrupt`、`HEAD` 无体应答跳过编码校验、200 KB PUT 的上传按 64 KiB 节拍多笔推进且末值恰一次、注入恒定帧号后每方向收敛到「首发 + 末值」两笔、未挂回调与挂了回调两条对端收到的请求全文逐字节相同、无请求体不发上传、chunked 下载把总量如实报成未知；另 `build_advertises_and_decodes_only_when_compression_is_compiled_in` 与 `send_refuses_a_body_that_fails_its_declared_coding` 两例外壳两腿同名、体内按宏分岔断言（后者在解码器在场的腿是「流损坏」、缺席的腿是「环节解不了」——同一份「绝不回吐压缩字节」的拒绝姿态，两种成因），`AURORA_ENABLE_COMPRESSION=OFF` 腿把只在解码器在场时才成立的六例按 TEST-R6 纪律登记成同名 `AURORA_TEST_SKIP`，故两腿用例数逐名可比），全用例走 `ScriptedPeer` 只绑 `127.0.0.1`、零出站，宿主驱动 pump 推进协程；编码层另配套 `utest_net_http_encoding`（15 例，纯内存零网络：宣告集与支持集同源、链文法的大小写 / OWS / `identity` / 空成员 / `;q=`、应用次序保序、不支持编码点名、无环节直传、gzip 单层、`deflate` 两试、反序多段、空载荷非损坏、上限口径点名新旋钮、core 三码到 `http-*` 的逐条映射、能力关闭时拒发）；进度闸本体另配套 `utest_net_http_progress`（9 例，纯内存零 socket 零时钟零线程：只写入不发射、同一回合每方向至多一笔且必为最新值、干净槽不被下一次机会重复发射、帧号源在场时以帧号为闸且同帧多次机会只发首发、回合闸每 `flush` 推进一号、`finish` 无视闸号发末值、机会从未到来也要结末值、两方向槽位互不覆盖、空回调时闸零作为连闸号都不推进）。两腿四组构建同口径回归（本机 Win32 / MinGW 与 WSL / GCC 15 各一腿，`AURORA_ENABLE_COMPRESSION` 取 ON / OFF 各一组）：`=ON` 时 `utest_net_http_encoding` 15 例 + `utest_net_http_client` 22 例 + `utest_net_http_wire` 12 例 + `utest_net_http_progress` 9 例 + `utest_net_http` 7 例 + `itest_known_enums` 4 例逐腿全绿；`=OFF` 的单独构建树里同两套件各 9 通过 + 6 跳过、16 通过 + 6 跳过（跳的正是依赖解码器的那六例，两腿逐名相同），`utest_net_http_wire` / `utest_net_http_progress` / `utest_net_http` / `itest_known_enums` 四套件不随开关转跳、照常 12 + 9 + 7 + 4 全通过，另 `utest_core_compression` 0 通过 + 12 跳过；四组名册逐名可比（encoding + client + progress 三套件共 46 名），跨腿无逐值差异。**待建（随后续切片）**：`open_sse`、`Session` 与连接池，以及 §12 其余语义件（见 §8.8.19 待建）。契约引用提案 [`changes/xplat-network-stack/proposal.md`](../changes/xplat-network-stack/proposal.md) §6 / §12 / §17。

#### 8.8.21 HTTP 发起侧流式接收：`Client::open_stream` 与 `DataSink`（CHG-018 §6 / §12 发起侧·流式形态）

新增公共头 `include/aurora/net/http_stream.h`（`SinkFlow` / `DataSink` / `StreamOutcome`，命名空间 `aurora::net::http`）与 `Client::open_stream(Request, DataSink &) -> CoroTask<Result<StreamOutcome>>`；同一批把 §8.8.20 那条「流式路径仍待建」的口径落地：库内 `src/aurora/net/detail/http_encoding.{h,cpp}` 新增 `StreamingDecoder`，把 core 的 `aurora::Decompressor` 按 `Content-Encoding` 链**反序逐环串联**，末环经 `StreamingEmit` 直交宿主。与物化路径共用的协议语义不再重述（宣告同源、链反序、`Location` 跟随、三档超时、进度合流同判据），此节只记四条只有逐段交付才成立的口径与两条实现裁决：

- **完成事实是 `StreamOutcome` 而非 `Response`**：`response` 携最终一跳的头部块且 `body()` 恒空，另交两枚**不同口径**的计数——`wire_bytes` 是分帧解出后线上真到达的字节（跨全部重定向跳、透明解压前），`delivered_bytes` 是实际交给 `on_data` 的明文（只算终跳、解压后）。两枚并存而不是合成一枚，是因为进度回调只能报线上口径（还原发生在逐段之后，中途给不出「总共多少明文」），而调用方判断「收了多少」要的是交付口径；`stream_abandons_the_body_of_a_redirect_hop` 把两枚分别钉成 `4` 与 `1`，正是为了证明它们不是同一个数换了名字。
- **接收侧是抽象基类而不是三枚 `std::function`**：`on_head` / `on_data` / `on_end` 共享的是同一个宿主对象的状态（打开的文件句柄、已累计的偏移、要不要中途收摊），拆成三个回调就得让调用方自己把那份状态缝三次；生命周期同样由对象表达——协程帧按引用持有 Sink（`cppcoreguidelines-avoid-reference-coroutine-parameters` 就地豁免，理由写明「由调用方持有、帧只借用」），故契约要求 Sink 存活到 `is_done()` 之后。调用次序恒为 `on_head`（一次）→ `on_data`（零或多次、恒非空段）→ `on_end`（恰一次），**失败结局不调 `on_end`**：那时字节流断在半途，收尾该由调用方按 `Result` 分流，而不是库替它假装「收完了」。段的切分由分帧边界、读缓冲与解码器内部窗口共同决定，不是一段的应用语义单元，故头文件明确要求宿主自行缓冲行 / 记录边界——本库不猜载荷的分隔形态。
- **线上字节不设限，解压产物照 `max_decompressed_bytes()` 限**：`max_body_bytes()` 是物化路径的内存闸，流式路径不物化、没有内存可闸；更要紧的是它**不再被当作解压上限的缺省**——`send` 那边「未显式收紧即跟随 `max_body_bytes`」的最小口径搬到流式路径，会让「流式下载一个大压缩包」必须去调大一枚与内存无关的旋钮，等于把不设限 promise 又收回去。故 `open_stream` 只取 `max_decompressed_bytes()`，未设即逐层不限；越限仍落 `http-body-too-large` 且 `detail` 点名这一枚旋钮（`stream_ignores_the_materialized_body_ceiling` 与 `stream_enforces_the_decompressed_ceiling` 一侧证「物化闸无效」、一侧证「解压闸有效」）。
- **`deflate` 的两型按包装头判定，不按「先试再回落」**：物化路径手里有全部字节，解不开可以换一种格式重播；流式路径的字节一旦交给宿主就收不回来，重播等于让宿主重写已写下那段。故 `Deflate` 环节只看**本环节最前两枚输入字节**是否构成合法 zlib 头（`RFC 1950 §2.2` 的三条硬约束：CM 低四位为 8、CINFO ≤ 7、`(CMF<<8|FLG) % 31 == 0`），据此把格式锁死到整条流；不足两枚时按住该段不交，判定完成再把前缀补喂进解码器。代价是一条「头恰好通过 zlib 校验、体却按裸流编」的病态载荷会被判损坏而不是被回落救回——两型的首两字节由编码器写出，现实中不会同时满足另一型的校验，而被省下的正是「把整条流留在内存里再试一次」这笔开销。
- **重定向跳的消息体不读**：头部一判定为重定向即弃该跳载荷、发起下一跳（一条连接一个跳，收摊由连接对象的作用域完成）。`send` 逐跳物化，本入口不为注定被丢弃的体继续读字节；宿主因而**只见最终一跳**的头部与体（`on_head` 恰一次，中间跳不进 `on_head`）。上限判据同 `send`：超 `max_redirects()` 落 `http-redirect-limit` 且失败结局不宣告收摊。
- **宿主回 `Stop` 是正常结局**：`open_stream` 交**成功**的 `Result<StreamOutcome>` 并把事实写进 `stopped_by_sink`，剩余字节不再读取也不再交付，绝不落成取消类错误码——「够了 / 用户取消了 / 校验和不符」是调用方的判定，不是本库做不到。停读之后有两条连带纪律：链内**不得再调 `finish()`**（那是在宣告一条被自己掐断的流「应当完整」，只会凭空造出一枚损坏错误），以及链的 abort 态里 `Stop` 优先于错误（同一轮 `push` 里先交出的段先被宿主拒掉，故 Stop 在字节次序上总是更早的那个事实）。`on_end` 在停读路径上仍恰一次，它是「把缓冲刷下去、把句柄关掉」的时机。
- **本腿无流式能力时直接失败，不悄悄退化成物化**：`Engine::capabilities().streaming` 为假（wasm 无 `ReadableStream`）即落 `net-platform-unavailable`，且一个回调都不发。退化成交换一份 `Response` 会把调用方的内存预算和它的预期一起毁掉——它与 §8.8.4 那条「不提供假降级」是同一条纪律。

实现裁决：两条交付路（`send` / `open_stream`）共用同一套编排骨架（建连、类型擦除读写口、`StageTimer` 三档、`render_head` + 写出、`ResponseAssembler` 逐段装配、超时改道、进度收尾），代码结构相似而**刻意不做抽象**——协程体里的 `co_await` 无法从普通函数里发出，把「建连 + 写出」抽成辅助协程要么按值搬动那些类型擦除闭包、要么让每一跳多背一帧，两种都比这几十行的重复更容易出错；四条真差异全部落在各自看得见的地方，读一条即能对照另一条。逐段交付用一块每轮清空的 scratch 缓冲（`drain_body` 只追加不清空，故清空归调用方），峰值内存等于一次读的长度而非载荷总长。

工具链面：伞头与 `tools/check/umbrella_manifest.txt` 收录 `net/http_stream.h`；`SinkFlow` 登记 `tools/include/known_enums.h` 并在 `itest_known_enums` 加编译期锚点（`enums` 段 +1，`aurora_api.json` 的 `enums` 随生成器新增 `SinkFlow{Continue, Stop}`）；`DataSink` / `StreamOutcome` 与 `Client` 同为类与结构体，不进 `enums` 段；本切片**不新增错误码**，复用 `http-body-too-large` / `http-body-corrupt` / `http-content-encoding-unsupported` / `http-timeout` / `http-redirect-limit` / `http-protocol-error` / `general-invalid-argument` 与 `net-platform-unavailable`。配套 `utest_net_http_stream`（15 例，走 `ScriptedPeer` 只绑 `127.0.0.1`、零出站：40 KB 体按 16 KiB 读块逐段交付且段数大于 1、chunked 交付、`HEAD` 无体应答零段并跳过编码校验、体第二段收摊后段数停在 2 而 `on_end` 仍一次、头部即收摊时零段交付、跨对端 `302` 弃中间跳的体并把两枚计数钉成 4 与 1、超上限落 `http-redirect-limit` 且不宣告收摊、`max_body_bytes` 对入口零约束、`br` 在建链处拒且一段不交、`read_idle` 到点改道 `http-timeout`、下载进度以线上字节计且末值恰一次、`streaming` 位为假落 `net-platform-unavailable`；另 gzip 双计数、`deflate` 两型各锁一次、解压越限三条只在 `AURORA_ENABLE_COMPRESSION=ON` 的腿成立，`=OFF` 腿按 TEST-R6 登记成同名 `AURORA_TEST_SKIP`）。流式链本体另在 `utest_net_http_encoding` 补 10 例（两腿同名的三条：空链直通、缺产出出口落 `general-invalid-argument`、宿主 `Stop` 后链停住且不再打扰宿主；只在解码器在场的腿成立的七条：一枚字节一枚字节喂完 gzip、`deflate` 按头锁定两型、判定前按住首字节且锁定后补交、多段链反序、上限点名 `max_decompressed_bytes`、损坏与截断分别显形于 `push` / `finish`、停读发生在链中段时后续段不交），缺席腿同样登记同名 `AURORA_TEST_SKIP`；该套件另有**一枚只在能力关闭时成立**的 `stream_building_a_chain_without_the_capability_fails_at_create`（建链即拒）；ON 腿不执行它的断言，但按同一枚 TEST-R6 纪律把它登记成同名 `AURORA_TEST_SKIP`——`registry_integrity` 的判据是「测试源文件里每一枚字面用例名都必须出现在 `--list` 输出中」，缺席即红，登记后两腿各 26 名、逐名相同（本节初稿记成「OFF 腿 26 名、ON 腿 25 名、其余逐名可比」，那枚不对称在只按 `-R "^check_"` 收口时不被发现，因 `registry_integrity` 不在该前缀内；口径按本条纠回）。CHG-020 再增一枚只在能力关闭腿成立的 `registered_codecs_restore_the_compression_capability`（自注册 gzip / zlib / deflate 三槽后宣告判据转真、建链不再即拒，桩用完即注销回落空注册表），同样两腿同名登记，两腿现各 27 名、逐名相同。判据非空转已变异自证：把出口构造里的 `wire_bytes` / `stopped_by_sink` 换成 `delivered_bytes` / `false` 后，`utest_net_http_stream` 四条同时转红（两条停读、跨跳计数、gzip 双计数），还原后即恢复全绿。两腿四组构建同口径回归见 §8.8.20 末段口径（`AURORA_ENABLE_COMPRESSION` ON / OFF × 本机 Win32 / MinGW 与 WSL / POSIX）。裁决与形态来源见提案 §16 第 26 项。

#### 8.8.22 HTTP 失败分型：`HttpErrorClass` 与两枚归因函数（CHG-018 §12 错误分型切片）

新增公共面全部落在 §8.8.19 的值层头 `include/aurora/net/http.h`（映射表在 `src/aurora/net/http.cpp`）：`enum class HttpErrorClass : std::uint8_t`（九档，`Invalid = 0`）、`error_class_of(ErrorCode) -> HttpErrorClass`、`error_class_of_status(std::uint16_t) -> HttpErrorClass`、`error_class_text(HttpErrorClass) -> std::string_view`。它回答的是「这次失败该往哪儿排查」，与「能不能重试」分属两枚事实。

- **档位与 `retryable` 正交，不新造重试口径**：重试与否仍逐码读 `Error::retryable`（与 `codespec/errors.toml` 同源，`aurora::retryable(ErrorCode)` 亦已存在）。同一档里两种取值并存是刻意的——`Unreachable` 档内 `net-connect-failed` 可重试、`net-send-failed` 不可重试；把动作折进档位就得为每枚码另立一档，档数会被重试口径牵着走，而这条的存在理由恰是让界面能问「这是网络的事还是我的代码的事」。之所以不直接用生成表的 `ErrorCategory`：那是全库粗分类，本族 43 枚全在 `io` 下，分不出「名字解析不了」与「对端把字节写坏了」。
- **九档而非八档**：§6 草图的八档装不下本族里 18 枚「动作在代码或配置里而不在网络上」的码（`net-url-invalid` / `http-request-invalid` / `net-endpoint-invalid` / `net-platform-unavailable` / `net-tls-engine-unavailable` / `net-option-failed` / `net-option-conflict` / `net-timeout-option-failed` / `net-proxy-config-invalid` / `net-proxy-unsupported` / `net-framer-config-invalid` / `net-buffer-limit` / `net-framer-overflow` / `net-policy-violated` / `http-body-too-large` / `http-charset-unsupported` / `http-content-encoding-unsupported` / `http-body-not-replayable`），故补第 9 枚 `Invalid` 作兜底并取零值，使默认构造与「未归类」同一口径。归类的判据是**失败发生的跳**：`Dns` → `Unreachable` → `Proxy` → `Tls` → `Timeout` → `Protocol` → `Cancelled` 先具体后兜底，同一枚码恒只进一档。
- **两处边界归类值得点名**：`net-proxy-config-unreadable` 归 `Proxy` 而同族的 `net-proxy-config-invalid` 归 `Invalid`——「读不到系统代理」的动作在环境，「代理配置写错了」的动作在调用点，压成一档会让排查方向反掉；`http-redirect-limit` 归 `Protocol`（对端把重定向写成了环）而 `http-body-too-large` 归 `Invalid`（越的是本库上限旋钮）。
- **`Status` 档由 `error_class_of_status` 产出，不在码表里**：发起侧对非 2xx 交出的仍是**成功** `Result`（一份 404 的应答同样是完整的事实，折算成错误就丢掉状态码本身），故状态码类失败没有对应的 `ErrorCode`。该函数把边界钉在 `>= 400` 恒回 `Status`，`< 400` 回 `Invalid` 且此处语义退化为「无可归因的状态码类失败」，不代表这次交换出错；有了这枚函数，`Status` 才有唯一产出点而不是枚举里的死项。
- **分型是观测口而非校验口**：`error_class_of` 是全函数，非 `net-*` / `http-*` 族的码照样回 `Invalid`，不报错、不返回 `optional`——调用方不必先判「这枚码该不该问」。逐码归档以 `error_class_of` 的分支与 `utest_net_http` 的期望表为两处同批真值源，新增网络码若忘了归档，完整性断言直接拦下而不是让它静默落兜底档。
- **`error_class_text` 给跨进程的稳定令牌**：九档对应 `"invalid"` / `"dns"` / `"unreachable"` / `"proxy"` / `"tls"` / `"timeout"` / `"protocol"` / `"status"` / `"cancelled"`，恒小写、两两不同；取名 `error_class_text()` 而非 `to_string()` 与本头既有的 `method_text()` 同族，避免与 `std::to_string` 的重载集混。

工具链面：`HttpErrorClass` 登记 `tools/include/known_enums.h` 并在 `itest_known_enums` 加编译期锚点，`aurora_api.json` 的 `enums` 段随生成器新增该枚；本切片**不新增错误码**、不改任何既有签名（纯增量）。配套 `utest_net_http` 由 7 例增至 12 例（逐码归档 + 完整性断言、非网络族码落兜底、档位与 `retryable` 的正交、`error_class_of_status` 的 399/400 边界、九枚令牌的唯一小写），全用例零 socket、零 DNS，两腿同名可跑。判据非空转已变异自证：把 `http-redirect-limit` 改归 `Invalid` 后完整性用例即转红，还原即恢复全绿。裁决与形态来源见提案 §16 第 27 项。

#### 8.8.23 HTTP 重试策略：`RetryPolicy` 与 `Retry-After` 尊重（CHG-018 §12 重试策略切片）

公共面新增三处、全部为增量：值类型 `RetryPolicy{attempts, base, max_backoff, jitter, budget}` 落 §8.8.19 的值层头 `include/aurora/net/http.h`（缺省 `1 / 200ms / 30s / 0.2 / 不设`，即**缺省不重试**）、`Client::retry_policy([RetryPolicy])` 作客户端缺省、`Request::retry_policy([RetryPolicy])` 作逐请求覆盖（槽位是 `std::optional`：`nullopt` = 跟随 Client，设 `RetryPolicy{}` = 本条关掉，两种意思必须可区分）。判定与算式全部落库内 `src/aurora/net/detail/http_retry.{h,cpp}`（`aurora::net::detail`，零 socket、零时钟、日期基准由参数注入），发起侧只在三处各问一声：起手校验形态、拿到结局问该不该重放、决定重放前问该等多久。两条款式与四处定案记此（§16 第 28 项）：

- **重放资格是两份既有事实的交集**：`Error::retryable`（`codespec/errors.toml` 表驱动）∧ §8.8.22 的归因档 ∈ {`Dns`, `Unreachable`, `Timeout`}。只读前者会把 `net-cancelled` 重放出去（它确实标着可重试），而重放用户主动取消的请求等于违抗那次取消；只读档位会把 `Unreachable` 档里 `net-send-failed` / `net-recv-failed` 这类「连接还活着、动作坏了」的码也重放。`Protocol` 与 `Invalid` 重放一万次是同一个错，`Tls` / `Proxy` 再撞一次同一道墙，`Status` 档根本没有码可映射（走状态码那条独立分岔）。方法侧另加一道闸：白名单按 `RFC 9110` 的幂等定义取死集合 `GET` / `HEAD` / `PUT` / `DELETE`，不做成可调集合——重放非幂等请求的代价（把一次提交变成两次）在「响应没收到」这个失败现场无从判定，而那恰是失败本身。
- **两枚时间旋钮分家**：`Timeouts::total` 逐尝试重新计时（看门狗在重试环内构造），`RetryPolicy::budget` 是跨全部尝试与全部退避等待的一堵墙。裁决理由写进头注释：合成一枚会让 `attempts × base` 的乘积隐式决定总时长。墙到点掐掉在途尝试时，那次尝试以 `net-cancelled` 形态抵达——既不是对端说的也不是本库造的码，故收尾改报 `http-timeout` 并在 detail 点名 `retry budget exhausted`，使「超时」在两枚旋钮之间仍可分辨。环关着（`attempts <= 1` 或非幂等方法）时 `budget` 一并摘掉：没有第二次尝试的「跨尝试墙」会退化成一枚单段超时，那是凭空多出的落点。
- **五枚字段而非草图的三枚**：§6 草图写 `RetryPolicy{attempts, base, jitter}`，落地补 `max_backoff` 与 `budget`。`max_backoff` **同时**充当指数退避的封顶与 `Retry-After` 的封顶——两条路回答的是同一个问题（「这次最多愿意等多久」），分成两枚会让「对端要我等 10 分钟」与「我方上限 3 秒」互相矛盾却无人裁决；越上限即按上限等，随后照常发起下一次尝试。`budget` 见上条。形态越界（`base` 为负、`max_backoff < base`、`jitter` 不在 `[0,1]`、NaN）在起手即落 `http-request-invalid` 并点名是哪一枚，一条连接都不发起——「以为开了抖动其实没开」要等到线上重试风暴才现形，那时现场已经没有配置信息；`attempts` 的 `0` 与 `1` 同义，`budget` 非正值与不设等价（与 `Timeouts` 各档同口径）。
- **`Retry-After` 优先于算式、不加抖动、仍受封顶，且原应答一律保留**：延迟秒数与 IMF-fixdate 两形态都解（`RFC 9110 §10.2.3`），日期走仓内纯日历算术（不碰 `<ctime>` 的平台分支与本地化月份表，故三腿同值），过期日期折成零等待而不是负数；已弃用的 RFC 850 / asctime 两型不解，按「没给」走指数退避——比猜时区数学更诚实。非 2xx 是**成功**的 `Result`，故耗尽尝试后交出的仍是最后那份应答（503 连同它自己的头和体），绝不折成新错误码；传输类失败同理交出最后那枚底层错误。本切片**不新增错误码**、**不给 `Response` / `StreamOutcome` 加尝试计数或「已重试」字段**（跨尝试墙借用 `Cancellation` 的既有取消通道而不新增公共字段），只在退避前 `AURORA_LOG_INFO` 一行。抖动取库内置的非密码学序列（进程内播种、逐次推进）：它只负责打散同一时刻整批发起的节奏，密码学熵在这里没有消费者，却会让单测只能退化成区间断言。
- **流式路径的资格比物化窄：`on_head` 交出即耗尽**：`open_stream` 同样跑重试环，但壳层 `HeadGuardSink` 一旦向宿主转发过头部（或任何体字节），本次交换就不再可重放——已交给宿主的段收不回来，重播等于让宿主把已写下那段再写一遍；宿主自己回 `Stop` 的收摊同样不重放（再打一次是替它改变决定）。环关着时不套壳，`on_head` / `on_data` / `on_end` 的调用对象与装环前同一个，少一层「壳改变了宿主身份」的疑义。每一次尝试都是从原始请求重新发起（重新建连、重跑整条重定向链），不是「接着上次断的地方再读」。

工具链面：`RetryPolicy` 与 `Timeouts` 同为结构体，不进 `tools/include/known_enums.h` 也不进 `aurora_api.json`（该文件本切片零变化，`check_api_schema_sync` 复核为绿）；新增实现 TU `http_retry.cpp` 由 `CONFIGURE_DEPENDS` 源列表自动纳入，伞头无需改（`net/http.h` 已在列）。配套四套同源判据：新增 `utest_net_http_retry`（11 例，纯算式——越界点名、白名单、交集准入、瞬时状态族、逐次翻倍与封顶、`jitter=0` 恒等算得值 / `jitter=1` 落在 `[raw, 2*raw]`、`Retry-After` 两形态与封顶）；`utest_net_http_client` 由 22 例增至 33 例（退避真的等满才回头、`Retry-After` 的秒数压过算式并被 `max_backoff` 夹住、耗尽交出的是最后那份 503、非幂等方法与表上 `retryable=false` 的失败一律不重放、协议档不重放、`total` 逐尝试重计时而 `budget` 到点收摊、越界策略零连接）；`utest_net_http_stream` 由 15 例增至 18 例；`utest_net_http` 由 12 例增至 13 例（`RetryPolicy` 槽位的两级语义与整份替换）。**三条测试设施侧的事实同批记此**，它们是判据形态的约束来源：① `ScriptedPeer::serve()` 串行服务连接，而占住时限的那条连接必须由脚本自己放手才有第二次连接可连，放手又会在客户仍持有的 socket 上产生 hangup——真实睡眠与虚拟时限赛跑的输赢随机器负载而变，故流式重放不钉 `accepts()` 数值，改钉**虚拟时钟跑满几扇窗口**（三扇 ≥150 ms 对比一扇 ≤90 ms），两侧用例互为一组正反对偶；② 被拒的回环 connect 在 `FakeEngine` + `HostDriven` 下 40 轮泵内从不落地（既有设施限制，非产品缺陷），故端到端腿只覆盖 `Timeout` 档，`Dns` / `Unreachable` 两档的准入由 `utest_net_http_retry` 的表侧断言承担；③ 对端「写完就 `close()`」交给读侧的结局**不由剧本决定**——接收缓冲里压着没被读走的请求字节时，Winsock 可把这次关闭改成异常断开，同一份用例在高负载机器上时而落 `net-recv-failed`、时而落 `http-protocol-error`（实测翻脸），后者同样是不可重试码，用例看着像过却换了判据来源。要钉住 `Unreachable` 档就必须把关闭形态钉死，故 `ScriptedPeer` 新增 `write_then_reset()`（`SO_LINGER` 时限 0 后再关，两腿同形地发 RST），`send_refuses_to_replay_a_failure_the_table_marks_non_retryable` 按它改写，并把断言从「具体哪一枚码」收稳到「档位 + 标志 + 不重放」三件恒同时。判据非空转已五处变异自证：把瞬时状态族去掉 503、把准入改成只读 `retryable`、把 `Retry-After` 的封顶去掉，三处各自只让对应用例转红（`status_admits_only_the_transient_family` / `retry_admission_is_the_intersection_of_flag_and_class` / `retry_after_beats_the_backoff_and_skips_the_jitter`，第三处另让发起侧的 `send_clamps_a_retry_after_to_the_backoff_ceiling` 同因转红，即封顶在算式侧与协程侧各有一道口径）；另在发起侧摘掉 `!guard.handed_off()` 一条后 `stream_does_not_replay_after_the_head_is_handed` 转红，把流式用例的 `attempts` 改成 1 后 `stream_replays_a_timeout_raised_before_any_head_is_handed` 转红；第五处把准入改成**只读档位**（摘掉 `retryable` 那半），端到端的 `send_refuses_to_replay_a_failure_the_table_marks_non_retryable` 以 `accepts()` 3 比 1 转红——这条与第二处一正一反，恰好把「交集」的两半边各自钉在一条会转红的用例上，只写表侧断言时端到端腿是不可证的。两腿回归与门禁口径同 §8.8.20 末段（本机 Win32 / MinGW + WSL / POSIX 同口径逐名比对）。本切片改动的 13 枚 TU 以 `clang-tidy` 逐点收口至零 findings，其中两处值得记名：`jitter` 的越界判定写作 `!(jitter >= 0 && jitter <= 1)` 并带**点名理由的豁免**——DeMorgan 改写后对 NaN 判假，会把「NaN 也是越界」这条定案漏掉；`RetryExpectation` 表与两处可选值读取按仓内既有口径改为具名字段初始化与 `require_value()`，避免用豁免掩盖形态问题。裁决与形态来源见提案 §16 第 28 项。

#### 8.8.24 HTTP 条件请求与内存缓存：`CacheOptions` / `no_cache` / `from_cache`（CHG-018 §12 缓存校验切片）

公共面新增四处、全部为增量：值类型 `CacheOptions{enabled, max_entries}` 落 §8.8.19 的值层头 `include/aurora/net/http.h`（缺省 `false / 64`，即**缺省关**）、`Client::cache([CacheOptions])` 作客户端开关与只读口、`Request::no_cache([bool])` 作逐请求绕过、`Response::from_cache()` 作载荷出处（其构造尾随一枚带缺省值的 `bool` 形参，既有构造点零改动）。判定与表全部落库内 `src/aurora/net/detail/http_cache.{h,cpp}`（命名空间 `aurora::net::detail`，**零新增公共头、不进伞头、不占公共 API 预算**，零 socket、零 DNS、零平台分支，新鲜度基准时刻由参数注入），因此「能不能存、存多久、什么时候必须问对端、304 到手怎么并」这套 `RFC 9111` 算式在零网络下可逐值断言且三腿同值；发起侧只在三处各问一声：起手查表（`open_from_cache`）、拿到结局问进不进表（`settle_cache`）、304 到手要一份合并后的应答。九条定案记此（§16 第 29 项）：

- **开关挂 `Client`、缺省关、拷贝共用同一张表**：`Session` 尚未落地，而本件的归属只需一枚「活着的、能持有共享状态的对象」，`Client` 恰是那一个；`Client` 的拷贝语义是「同一份配置的另一只手」，所以表以 `std::shared_ptr` 持有而不是各拿一份空表——各拿一份等于把已付的往返代价在每次按值传递时扔掉。缺省关是**行为可见性**的判定而不是「功能未完成」的标记：缓存改变的唯一事实是「这次没触网、上一字节可能来自更早」，而响应式界面里数据陈旧是用户可见缺陷，库不得在没有显式请求时把一次交换变成零次交换。关掉时整段判据零作为（连一次内存查找都不做），形态与缓存从未存在时逐字节相同。
- **`from_cache()` 只回答「载荷从哪儿来」，不回答「那条老不老」**：两种形态都算真——新鲜副本被直接交付（本次交换根本没连出去），以及条件请求拿到 304 后把已存副本合并交出（线上只走了头部）。走到调用方手里的一律是可交付的，新鲜度判定已在发起侧完成，所以这枚布尔不需要第二枚「陈旧度」字段配合。它与 §8.8.23 刻意**不给**出口值类型加「已重试」计数并不矛盾：重试次数没有消费者（重放对调用方的影响已经被交出最后那份事实消化），而「这份数据来自本地表」是界面要显示、要决定是否再校验的事实，没有出口就是把它藏起来。缓存未开启时恒假。
- **从不交付陈旧副本，因此 `must-revalidate` 在本库不需要分支**：过期条目必须先向对端验证，验证失败就把那次失败原样交出，绝不拿旧数据兜场——这正是 `must-revalidate` 的禁令，所以对本库它就是缺省行为。响应侧的 `no-cache` 指令折算为「抵达即过期」（存进表，但每次交付前都要问对端）。同时**不做启发式新鲜度**（`RFC 9111 §4.2.2` 那套拿 `Last-Modified` 与 `Date` 的比例猜寿命的算法）：同一个应答在不同抵达时刻会得到不同寿命，判据写不成定值，而本库的测试设施也无法用一台机器的时钟为另一台机器的结论背书。于是两条推论定死：无显式有效期又无任何校验子的应答**不进表**；有校验子而无有效期的照进表但即刻过期——省的是体字节，不省那一次往返。
- **表按 (方法, 目标 URL, `Vary` 点名的请求头取值) 建键**：`Vary` 存的是应答侧点名的头名**与当时请求里那些头的取值**，后续请求只有同名同值才命中；`Vary: *` 意为「这份应答随任何请求特征而变」，既不进表也永不命中（`§4.4`）。键取 `Url::parse` 成功后的规范化渲染，解析失败时缓存**不猜键**直接跳过——那是 `render_head()` 要报的 `net-url-invalid`，把用法错误记成一次缓存未命中会把它彻底藏掉。`Vary` 与 `Cache-Control` 都允许同名分行给，判定前一律先并成一串逗号分隔文本再解析；只取第一行会把「`no-store` 写在第二行」的请求读成普通请求，读侧与写侧就此分岔。
- **进表五道闸**：请求侧 `no-store` 或 `Request::no_cache()`、应答侧 `no-store`、`Vary: *`、方法不在 `GET` / `HEAD`、状态码不在可缓存死集合（200 / 203 / 300 / 301 / 404 / 405 / 410 / 414 / 501）、无有效期又无校验子。方法只收这两枚而不用 §8.8.23 的重试白名单——`PUT` / `DELETE` 按方法律是幂等的，但它们的应答描述的是「这次改动的结果」，把那份结果留在表里等于缓存一次写操作的后效。状态码里 `5xx` 除 501 外都不进：服务器此刻不行了，不代表这个资源长这样；`304` 永不进表，它是更新已有条目的输入而不是新条目。`s-maxage` 一律忽略——那是给共享缓存的，本库的表是用户代理私有的（`§5.2.2.9`）。其余不认得的指令（`public` / `private` / `immutable`）忽略而不报错：`Cache-Control` 是可扩展字段，未来的指令由未来的版本决定含义。
- **新鲜度算式的三处收口**：有效期取 `max-age` > `Expires − Date` > 0（`§4.2.1` 的次序，两枚都在时听 `Cache-Control`）；抵达时刻的初始年龄按 `§4.2.3` 收拢成 `max(抵达 − Date, Age + 请求滞留)`，本库不测请求滞留故取 0 并就地写明（那条要的是发起前的排队时长，它属于连接池切片）；「在本表里住了多久」在**判定时现算**而不是存入时算死，条目因此可以长期躺在表里。两处数值姿态是防御性的：`parse_seconds` 对越界数字串饱和而不报错（对端多打一位数不该让一条本来能缓存的应答解不开），`is_fresh` 的年龄加法夹到 `INT64_MAX`（一张 `Date` 倒退到 1970 年前的应答因此不会凭空变成新鲜条目，而 `Expires` 早于 `Date` 折算成 0）。
- **条件头由本库挂载、但不覆盖调用方的表达**：命中陈旧条目且它带着校验子时，起手往请求上挂 `If-None-Match`（有 `ETag`）与 `If-Modified-Since`（有 `Last-Modified`），调用方已自设那两枚时一律不动——与 §8.8.20 的 `Accept-Encoding` 同一条纪律：协商是调用方的表达。请求侧的 `Cache-Control: no-cache` 与 `Request::no_cache()` **刻意差一档**：前者按 `§5.2.1.5` 的字面语义只禁止「不经核对就交付」，条目带校验子时照样挂头去验证；后者取 `no-cache` ∧ `no-store` 那一面（既不读也不写），因为调用方显式按这个名字用它时说的是「这次要此刻的真相」，而弱形态留下的那次写入照样会让下一个请求看见本次的副本，那不算把缓存「越」过去。
- **304 只并字段、且四枚描述载荷的一律跳过**：同名字段以 304 交来的为准，未出现的原样留着（`§4.3.4`）——载荷本体来自已存那份，让一次没有消息体的应答去覆盖「这条消息有多长」是把长度交给不存在的体，故 `Content-Length` / `Content-Range` / `Trailer` / `Transfer-Encoding` 四枚排除在外；多值字段整体重挂而不是逐值合并。一处可见的连带后果记此：`Headers::set` 的仓内口径是「清同名后追加到末尾」（§8.8.19），所以被 304 更新过的那一枚在合并结果里落到尾部，字段的**取值**以新为准、**书写次序**不保留原位，`Headers` 的相等判定本就与次序无关故不影响任何判据。合并不了（条目已在本跳往返期间被逐出）时把对端交回的 304 原样交出而不是编造一份体——编造比交出真相更糟；调用方自己声明的条件请求所拿到的 304 同样原样流过，本件不知道它对应表里哪一条，无权替它合并。
- **`open_stream` 对表只读不写**：流式路径手里从来没有完整体（`StreamOutcome::response` 的 `body()` 恒空），写表等于用一份无体头部覆盖一条有体条目。于是三条形态：**命中新鲜副本**时把已存体按读块大小分段喂给同一个 `DataSink`，交付次序与真从对端取回时一致（`on_head` → `on_data`* → `on_end`），宿主除了 `from_cache()` 与 `wire_bytes` 看不出差别；**重验证拿到 304** 时（仅当条件头由本库注入）合并后交付已存体，304 本体绝不示人；**条目已被逐出**时把裸 304 交给宿主而不是伪造体字节。`wire_bytes` 在缓存命中与 304 那一跳上如实报 0——它是「线上真到达的字节」，缓存命中根本没有线上交换，把已存体的长度冒充成它既与 §8.8.21 的定义冲突，也会让「这次省了多少流量」变成猜不出来的一枚数。省下的字节由 `delivered_bytes` 表达，两枚计数各说一件事的口径在此原样适用。
- **容量按条数、逐出按最近使用，且缩小上限立刻生效**：`CacheOptions::max_entries` 管条数，字段的闸另有 `max_body_bytes()` / `max_decompressed_bytes()` 两枚旋钮，两件事不共用一枚口径（一条 8 MiB 的应答与八条 1 MiB 的应答对内存的意味不同，而「记住多少个回答」才是这份表要表达的意图）。`set_options()` 改小上限时当场逐出到新的上限，而不是等下一次写入才收缩——「调小上限」是一次要求交还资源的动作，把它推迟到不确定的未来等于没做。表本身按 `Client` 共享（见第一条），故它自带一枚互斥量只护表体，判定与折算全在锁外由自由函数完成；查找是线性扫描，上限只有几十条，不为这个量级上索引。

工具链面：伞头、`tools/check/umbrella_manifest.txt` 与本切片无关（`net/http.h` 与 `net/http_client.h` 早已在列，缓存表在 `src/.../detail/` 不进公共面）；`CacheOptions` 与 `RetryPolicy` / `Timeouts` 同为结构体，不进 `tools/include/known_enums.h` 也不进 `aurora_api.json` 的 `enums` 段，本切片 `aurora_api.json` **零变化**（生成器不收成员函数与构造体默认参数，`check_api_schema_sync` 复核为绿）；**无新增错误码**——缓存是「少打一次网络」而不是「打出一种新失败」，命中路径上的任何错误都还是发起侧原有那几枚。配套三套同源判据：新增 `utest_net_http_cache`（16 例，纯内存、基准时刻参数注入——`Cache-Control` 文法只读认得的那几枚、幂等方法闸、可缓存状态码死集合、`Vary: *` 与「没有 `Vary`」不同义、同名多行先并再解、`max-age` 压过 `Expires − Date` 再压过 0、初始年龄取 `Date` 差与 `Age` 的大者、`no-cache` 应答存而不送、居住时长aging 且倒退时钟不造新鲜条目、新鲜副本命中并自报出处、`Vary` 取值决定是否命中、五道写闸逐条拒存、LRU 逐出、缩小上限立刻逐出、304 保留体并采纳新头、合并跳过四枚描述载荷的字段）；`utest_net_http_client` 由 33 例增至 40 例（新鲜命中一条连接都不发起、陈旧条目先问再送、`no-store` 由对端否决整张表、逐请求绕过与随后的普通请求照常命中、开关未开时整片零作为、写方法的应答永不进表、`Vary` 让第二次请求成为另一个请求）；`utest_net_http_stream` 由 18 例增至 21 例（命中时把已存体分段交给同一个宿主且 `wire_bytes` 恒 0、流式交换不写表、陈旧条目重验证后交付已存体且第三次流式不再触网）。**缓存判定的基准时刻走 `std::chrono::system_clock`（`Date` / `Expires` 本来就是墙上时钟），而 `FakeEngine` 的脚本化时钟推不动它**，故上述 10 条端到端判据一律走真实时钟并钉在可直接观测的事实上——`ScriptedPeer::accepts()`（触网次数）、`Response::from_cache()`、体字节，而不是「新鲜度还剩几秒」；需要精确时刻的判据由 `utest_net_http_cache` 的自由函数注入时刻完成。**判据非空转已八处变异自证**（每次改一处、跑三套、还原后基线回到 16 / 40 / 21 全绿且零告警）：① `is_fresh` 的 `<` 改 `<=` → 5 条缓存用例 + 1 条发起侧 + 1 条流式转红（边界那一秒即过期与仍可交付的分界）；② 摘掉读侧的表查询 → `a_request_may_bypass_the_table` 以 `accepts()` 1 比 2、体 `"one"` 比 `"two"` 转红；③ 单独摘掉写侧的请求 `no_cache()` 闸**不转红**——它与写入前端的 `key.empty()` 判据冗余（绕过时既不查也不写，第二条闸独立成立），须与 ② 同摘才现形（`"two"` 比 `"one"`），故两条都保留、显式判据优先于隐式后果；④ 让流式路径也写表 → 2 条流式用例转红，其一以 `""` 比 `"stored"` 现形，正是「逐段交付手里没有完整体，写表等于用无体头部覆盖有体条目」；⑤ 让 304 直接交给宿主 → 6 处断言转红；⑥ 合并时不再排除四枚描述载荷的字段 → 2 条缓存用例转红；⑦ 把 `Vary: *` 当没给 → 4 条缓存用例 + 1 条发起侧转红；⑧ 忽略 `CacheOptions::enabled` → 2 条发起侧转红（开关的存在理由就是「关掉时整片零作为」）。两腿回归与门禁口径同 §8.8.20 末段（本机 Win32 / MinGW + WSL / POSIX 同口径逐名比对）。裁决与形态来源见提案 §16 第 29 项。

#### 8.8.25 HTTP 发起侧的代理接入：`Client::proxy` 与两种交付形态（CHG-018 §11 代理族的发起侧收口）

§8.8.7 定出「目标 → 代理」的唯一决策入口，§8.8.8 定出握手字节，§8.8.12 把它们编排成 `StreamTransport::connect_via` 一条建连路——三件都在，缺的是最后一段：HTTP 发起侧一直只调直连入口，调用方装配好的代理集合到了 `Client` 手里不生效。本节补上这一段，公共面新增两处、全部为增量：`Client::proxy(ProxyEnvironment)` 装集合与同名只读口（缺省 `ProxyEnvironment{}` = `disabled()`，即一切目标直连），以及 §8.8.19 值层 `Request::render_head()` 尾随一枚带缺省值的 `bool absolute_form` 形参（既有调用点逐名不变）。库内同批把协程桥接层的 `detail::await_connect` 更名为 `detail::await_connect_via(ProxyConfig, host, port, options, cancellation)` 并收成发起侧**唯一**的建连出口；路由折算落 `src/aurora/net/http_client.cpp` 实现内的 `HopRoute` 一枚小结构与 `plan_route()` 一个函数（文件内私有、不占公共 API 预算）。七条口径：

- **一条建连路、两种交付**：每一跳先由 `ProxyEnvironment::for_target(host, port, secure)` 定出 `ProxyConfig`，建连一律经 `await_connect_via` 发出（`ProxyKind::Direct` 按 `net/transport.h` 的契约本就等价于直连入口，故无代理的缺省路径与接入前逐字节同型）；库里不留第二条建连入口，「这次到底有没有经代理」因此只有一个答案可问。经代理之后分两种交付形态：**隧道**（`https://` 经 HTTP 代理、以及一切 SOCKS5）里对端仍是目标、请求行照写 origin-form；**转发**（`http://` 经 HTTP 代理）里对端换成代理本身、`connect_with` 降为 `Direct`（代理已站在门口，不必再「经代理」找它），请求行改写成绝对形态。
- **明文不为代理发 `CONNECT`**：`CONNECT` 到 80 端口常被拒止，那会把「这种用法代理不支持」伪装成连通性故障、把排查方向引向网络；转发本就是转发代理的本职。两条路因此按 scheme 分岔而不是按代理心情试探——试探等于用一次失败换一次成功，而失败的代价由调用方付。
- **绝对形态只有一处可谈**：请求行按 `RFC 9112 §3.2.2` 写成 `scheme://authority/path?query`，恒不含片段与 userinfo；缺省端口不写（80）、非缺省端口保留（`:8080`），写出来的是代理据以转交的权威文本。`Host` 仍按目标 URL 补齐，代理不改写这条。origin-form 与 absolute-form 的分岔就是字节层那枚布尔参数，不新增枚举、不进 `tools/include/known_enums.h`。
- **缺省绝不读环境变量**：集合只能由调用方显式装配（`ProxyEnvironment::from_environment()` / `from_system()` / `from_environment_or_system()` 都是调用方的动作）。同一段代码在不同机器上因环境而异，正是本提案要消灭的那类无痕故障，也让「连不上」的现场无法在别处复现。
- **隧道不成即失败，绝不回退直连**（§8.8.12 的传输侧禁令自此在 HTTP 腿同样成立）：代理不可达、拒绝建隧道、不会说话一律以对应 `net-proxy-*` / 连接类码收尾，`connect_timeout` 覆盖直到隧道可用的整段；绕过代理往往正是策略禁止的那条路（见 §8.8.26 的 `require_proxy`）。凭据只出现在写出的字节里，诊断文本一律只带 `host:port`（§8.8.8 的纪律在此原样适用）。
- **`https://` 在隧道之上再握手**：SNI、证书校验与 `TlsContext` 口径的目标始终是原主机，代理只见 CONNECT 与密文流——这是两种交付形态安全含义一致的原因：明文那条不加密，正因为要加密的目标绝不让代理看见载荷。
- **逐跳重算**：重定向把目标换成另一主机乃至另一方案时，本跳的决策按新目标重新 `for_target`，沿用的是同一份环境集合而不是上一跳的结论（`NO_PROXY` 例外与 scheme 槽位都因此逐跳现算）。

工具链面：伞头与 `tools/check/umbrella_manifest.txt` 在本节目无变化（`net/http_client.h` 早已在列）；本节目**无新增错误码**——代理的失败形态在 §8.8.8 / §8.8.12 就分完码了，HTTP 腿只是走到那些码之前多了一道决策。配套判据在 `utest_net_http_proxy`（新增 11 例，本节目占前 6 例：转发形态的绝对请求行与 `Host`、缺省端口省略与非缺省端口保留、直连一跳仍写 origin-form、`https://` 一跳发出 `CONNECT` 而不发请求行、代理不可达时源站 accept 计数恒 0、`NO_PROXY` 例外命中时回到 origin-form 且代理侧 accept 恒 0），全用例走 `ScriptedPeer` 只绑 `127.0.0.1`、零出站，且**全程零 DNS**：隧道那例用 `https://127.0.0.1:<port>` 造——TLS 输入校验拒绝 IP 字面量（`validate_handshake_inputs` → `net-endpoint-invalid`），于是「`CONNECT` 已按线上形态写出」可证而交换在握手前就干净收尾，不必把 `localhost` 的解析结果当成判据的一部分。判据非空转已三处变异自证：只把 `absolute_form` 置假（对端仍是代理）→ 绝对形态那两例转红；只把隧道分支的 `connect_with` 降为 `Direct` → `a_secure_hop_asks_for_a_tunnel_and_sends_no_request_line` 转红；把明文分支整条换回直连源站 → 上述两例加 `an_unreachable_proxy_never_falls_back_to_direct` 共三例转红。第三处同时给出一条分布事实：**既有 `utest_net_http_client` 40 例在该变异下全绿**——它们不配代理，故代理这条腿在本次之前没有任何判据消费者，这也是本节目必须自带一套用例的理由（而非「发起侧已有覆盖」）。两腿回归与门禁口径同 §8.8.20 末段（本机 Win32 / MinGW + WSL / POSIX 同口径逐名比对）；`AURORA_ENABLE_COMPRESSION=OFF` 腿另跑一遍，本切片四套套件（policy 11、proxy 11、client 40、stream 21）在两腿**逐名相同**，client / stream 依赖解码器的那几例按 TEST-R6 纪律以同名 `AURORA_TEST_SKIP` 登记（OFF 腿 34 通过 + 6 跳过、18 通过 + 3 跳过），policy / proxy 两套不随开关转跳。**收尾门禁的口径同批收严**：本节目起改跑**全量 ctest**（`ctest --preset ninja-test`）而不是只按 `-R "^check_"` 收口——`registry_integrity` 不在 `check_` 前缀内，而它恰恰是「两腿名册必须逐名可比」的执行者；本次全量跑出的唯一红项是 §8.8.21 那枚留在能力缺席腿的建链即拒用例（已按该节纠回口径处置），复现于本切片之前的源树，与本节目无关。裁决与形态来源见提案 §16 第 30 项。

#### 8.8.26 声明式安全策略：`NetPolicy` 的三维判定、进程级默认与发起侧闸门（CHG-018 §13 的策略件）

新增公共头 `include/aurora/net/policy.h` + 实现 `src/aurora/net/policy.cpp`（命名空间 `aurora::net`，与 `net/proxy.h` / `net/tls.h` 同层——HTTP 只是它的消费者之一，策略不认识方法与路径），伞头与 `tools/check/umbrella_manifest.txt` 同批收录。类型只回答一个问题：**这次交换允不允许发生**。它是纯判定的值类型，不碰套接字、不记状态，形态对齐 iOS `NSAppTransportSecurity` 与 Android `network_security_config`（声明一次、由框架逐请求执行），而不是「调用方自己记得别发 http」那种约定。九条定案：

- **三维与保守缺省**：`allow_cleartext_hosts` 缺省空 = 一切明文都拒；`min_tls_version` 缺省 `Tls12`；`require_proxy` 缺省假。缺省拒明文不是「功能没做完」的保守开关，而是本库对「凭据与用户数据会不会顺手中转」的默认回答，与 iOS ATS 同一条取舍。
- **只判不改**：策略从不改写 `TlsContext`。它检查那份上下文**允许的版本集合**是否整个落在下限之上——`min_version` 低于下限、或 `max_version` 落在下限之下，都算冲突并直接拒，detail 同时点名两枚配置。静默把下限抬到策略值是假安全：调用方以为自己的 `TlsContext` 说了算，实际有一处看不见的改写。
- **主机清单只有两种写法**：不带前导点 = 注册名全等（比较前 ASCII 小写、剥一枚根区尾点）；带**一枚**前导点 = 该域本体及其任意深层子域。`*` 是唯一通配项（这一维不设限，是显式写下的放弃而不是漏配）；空条目永不匹配（漏配一行不该变成放行一切）。不做 CIDR、不带端口限定、不认 `<local>` 那类平台令牌；数字字面量按文本全等比较，IPv6 取 `Url` 规范化后的无括号形态。
- **与 `NoProxy` 的域名规则刻意分岔**：那里 `.internal` 与 `internal` 同义，因为那是「少打几个字」的运维便利；而一份安全白名单里 `example.com` 静默放行 `evil.example.com` 是这一类配置最常见的失误形态，后缀在这里必须是显式选择的动作。两族规则不同不是历史包袱，是两种用途的必然差别，写进头注释以免后来者「顺手统一」。
- **没有隐含的回环例外**：`127.0.0.1` / `::1` / `localhost` 与任何其它主机一样受明文维度约束。本机服务同样是明文信道，把例外藏在库里等于让「策略放行了一切」与「实际放行了一切加本机」两种配置在调用方眼里长得一样——而测试环境正是这类「没人审查的隐含例外」最容易长出来的地方。
- **`require_proxy` 取字面含义**：为真时任何判定为直连的跳都被拒，`NO_PROXY` 例外命中的目标因此同样被拒（那是该条声明的字面意思，不是它的例外）。这条与 §8.8.25 的「不回退直连」是一对：库里没有一条路径能在策略要求经代理时绕过代理。
- **三级取定与快照时刻**：`Client::policy()` 设过用它，否则取 `default_policy()`，两者都没设即 `NetPolicy{}`。生效时刻是「交换发起那一刻」——`send` / `open_stream` 起手读定一份，之后整条链（含逐跳重定向与重试的每一次尝试）共用那一份，中途改旋钮或改进程级默认都不影响在途交换。进程级默认是本仓第一枚公共可变全局：安全策略天然属「宿主启动时声明一次、之后各处新建的客户端都该服从」的配置，逐客户端设置会让「忘记设」成为默认放行路径；它用互斥量护住一份**按值**往返的 `std::optional<NetPolicy>`，锁外拿不到引用，因此调用方手里的策略不会被后续 `set` 半途改写。
- **闸门的位置：起手先于缓存，其后逐跳，且一律在发出之前**：判定在任何 `co_await` 之前完成，被拒的那一跳一条连接都不发起、一个字节都不写出，`net-policy-violated` 的 message 里 `before any byte went out` 就是这件事的字面承诺。起手那次问在查缓存**之前**——策略先于「少打一次网络」这条优化，命中缓存照样是一次交付，不能因为它就没被问过。`plan_route()` 把代理决策与策略判定放在同一处（`require_proxy` 那一维读的就是本跳的实际决策，先有答案再问，库里因此没有第二个「有没有经代理」的答案）；URL 解析不出或端口不可得时起手闸放行，那是 `render_head()` / 发起处原有判据要报的错，策略不抢它们的活、更不替畸形目标编一条。三条维度按「明文 → 版本下限 → 是否必须经代理」的次序问，第一个不合的交出错误；判定不看方法、不看路径、不看端口（策略管的是信道，不是请求内容）。
- **证书固定不在本类型里**：SPKI pin 随 TLS 切片落在 `TlsContext::pinned_public_keys`（§8.8.18），与信任根、校验开关同属一次握手的口径；提案 §13 曾写「经 `NetPolicy{ pin_spki }` 开启」，落地按本条纠回（原文就地改写、编号不动，见 §16 第 31 项）。策略层管「能不能谈、谈的话版本下限多少」，不管谈成之后认哪张证书；把它挪进 `NetPolicy` 会让同一份握手口径存在两处真值。

工具链面：**新增错误码 `net-policy-violated`**（`category = io`、`severity = error`、`retryable = false`、`auto_fixable = false`、`fix_category = invalid_value`，插在 `net-proxy-config-unreadable` 之后，全表 124 → **125** 枚，经 `codespec/errors.toml` → `error_codes.gen.h` + `codespec/ERROR_CATALOG.md` + `aurora_api.json` 生成器刷新）；`aurora_api.json` 本切片的变化就是 error_codes 段那一条——`NetPolicy` 是结构体、`net::` 的类型本就不进 `enums` 段，`check_api_schema_sync` 复核为绿。§8.8.22 的失败分型同批改口径：本族 42 枚全在 `io` 下、`net-*` / `http-*` 两族 42 枚逐码有档，`net-policy-violated` 归 `Invalid` 兜底档（**策略闸掐在发起之前：拒的是这份声明与这个目标的组合，不是链路**，动作在代码与配置里而不在网络上），`utest_net_http` 的期望表加一行、完整性闸从 41 收到 42。配套两套同源判据：新增 `utest_net_policy`（11 例，纯判定、零 socket 零时钟零线程——缺省构造就是那组保守口径、裸条目只匹配该注册名（大写写法与带尾点写法各命中一次、子域不命中）、前导点条目打开该域与任意深层子域（`xample.com` / `notexample.com` 这类半截后缀不命中）、`*` 打开这一维而空条目什么都不打开、无隐含回环例外、版本下限拒「能谈到下限以下」的上下文（含 `max_version` 落在下限之下那型）且只判不改（判定后重读上下文两枚仍为原值）、明文目标不参与版本维度、`require_proxy` 拒一切直连跳、三维按定案次序发问、不看方法 / 路径 / 端口、进程级默认往返与清除），`utest_net_http_proxy` 的后 5 例管发起侧接入口径（起手闸在一条连接都不发起的前提下同时拒住 `send` 与 `open_stream`、闸先于缓存、逐跳重问、三级取定、在途快照）。**一处连带后果记此**：缺省拒明文让仓内每一条约回环 `http://` 的既有用例都变红（`utest_net_http_client` 39 例、`utest_net_http_stream` 20 例起手即落 `net-policy-violated`），处置不是给回环开后门而是**逐文件显式点名**——两套件各写一枚 `allow_test_cleartext(Client &)` 助手（清单里就是 `127.0.0.1`），触网用例逐条调用（client 42 处、stream 20 处，含两条有名客户端各一处）。删掉助手的调用会让该套件全部触网用例转红，这同时是「闸门确实在建连之前起作用」的自证；策略不给测试环境留隐含例外，正是上一条定案的第一次兑现。判据非空转已五处变异自证：把裸条目也按后缀匹配 → `a_bare_list_entry_matches_only_that_exact_name` 转红；版本维度只查 `max_version`（等于放过「能把版本谈到下限以下」的上下文）→ 下限那例与三维次序那例两例转红；在 `allows_cleartext_host` 开头补一条回环放行 → `no_implicit_loopback_exception` 转红；把 `require_proxy` 提到判定次序最前 → `the_three_dimensions_are_asked_in_order` 转红；摘掉起手闸两处调用（`send` 与 `open_stream` 各一）→ **只有** `the_gate_runs_before_the_cache` 转红，`the_policy_gate_refuses_before_any_connection` 照常绿——逐跳判定独立兜住「拒绝」这条事实，故起手闸的**唯一**独立职责是「先于缓存」那条次序。起手闸与逐跳闸因此不是重复设闸而是各管一件事；这与 §8.8.24 那处「只有合并摘除才现形」的冗余闸是相反的形态——那条不可由单点变异证明，这条不可由拒绝类用例证明。两腿回归与门禁口径同 §8.8.20 末段。裁决与形态来源见提案 §16 第 31 项。

#### 8.8.27 链路标识：`X-Request-Id` 的三级取定、一次交换一枚与共用的非密码学随机源

不新增公共头：类型面只多出 `Request::request_id(std::string)` / `Request::request_id()`（值层头 `net/http.h`）与 `Client::request_id(bool)` / `Client::request_id()`（发起侧头 `net/http_client.h`）两对链式旋钮；头名常量、UUIDv4 组装与随机源全落库内 `src/aurora/net/detail/http_request_id.{h,cpp}` 与 `src/aurora/net/detail/noncrypto_random.{h,cpp}`（命名空间 `aurora::net::detail`，不进伞头、不占 API 预算）。这一件回答的是一个纯排查问题：**这次界面异常对应哪一次网络交换**——日志里的一行、时间线上的一条、对端服务器 access log 里的一个字段，三者能凭同一枚字符串对上。七条定案：

- **三级取定，调用方手写的头永远优先**：请求里已存在 `X-Request-Id`（同名不论原始大小写拼写，`Headers` 本就 ASCII 大小写不敏感）→ 原样发出，库不改写、不追加第二枚；否则看 `Request::request_id()`；两者都没表态才看 `Client::request_id()`（缺省**开**）。缺省开是本件与 §8.8.26 唯一方向相反的取舍，理由也相反：策略缺省拒是「不安全不该是默认」，标识缺省开是「不可追溯不该是默认」，而后可关的方向由调用方显式声明。
- **`nullopt` 与空串是两件事**：未设 = 跟随客户端缺省；显式设成空串 = 「这一条就是不带标识」的逐请求关闭，不是漏配。把空串折回 `nullopt` 会让「这条别带」这种意图只能靠 `Client` 级开关表达，而关掉整只客户端是比需要的粗得多的一刀。
- **一次交换一枚，落在原始请求那份副本上**：`send` / `open_stream` 起手把标识写到 `origin` 副本（在策略闸与查缓存之前），此后重定向的每一跳与重试的每一次尝试都从这份副本复制请求，因此整条交换在网络上只出现一枚标识。这与 §8.8.23 的「每一次尝试都从原始请求重新发起」和 §8.8.25 的「逐跳重算」是同一条纪律的另一面：**逐跳重算的是决策，不是身份**——身份若逐跳重生成，一次用户可见的失败就在三方日志里是三件事，本件要消灭的正是它。
- **补齐位置在查缓存之前且不打乱表**：§8.8.24 的键是 (方法, URL, `Vary` 点名的请求头取值)，`X-Request-Id` 不在其中，故自动补齐既不制造永不命中的条目、也不让同一 URL 的缓存副本随标识而变；`Vary` 真点名了它的部署是自愿放弃缓存，而不是库的副作用。缓存这条判据有专例钉住（补齐后第二次同名请求仍命中、对端 accept 次数恒 1）。
- **形态是 RFC 9562 的 UUIDv4 小写文本**：36 字符、四枚连字符在固定偏移、版本位 `4`、变体位取 `8` / `9` / `a` / `b` 之一。选它不是因为版本管理需要 UUID，而是因为它是**跨工具通用的关联标识事实标准**：网关、代理、日志采集与各家 APM 默认识别并索引这个形态，自定义格式在那些系统里只是一段不可检索的字符串。字段名 `X-Request-Id` 是事实标准的另一半（Envoy / nginx / 各大云平台同名），它不是 `IETF` 注册的规范头（`RFC 9513` 的 `Traceparent` 才是分布式追踪那一层），本件只做请求级关联，不做跨进程追踪上下文传播。
- **随机源是库内一枚非密码学序列，明确不是安全边界**：`detail::noncrypto_random.h` 定义 `noncrypto_unit()`（`[0, 1)`）与 `noncrypto_u64()`，状态是函数内静态原子量、首次使用时以单调时钟播种，推进体是 Knuth MMIX 参数的线性同余、比较交换循环保证多线程各取各值。刻意**不取** `std::random_device`：它在 MinGW 腿是确定性伪随机、在 wasm 腿可能直接抛，两个结果都让单测无法陈述「两次取值不同」这类真值；而用途（打散重试节奏、给交换编个可对上的号）不需要不可预测性。头注释把这条界限写死，以免后来者把它当熵源复用——需要不可预测值的是 TLS 与凭据，那条路走平台安全栈，不走这里。
- **一行 DEBUG 日志，只带可公开的部分**：每次交换起手经 `AURORA_LOG_DEBUG("net", ...)` 写出标识 + 方法 + 目标主机各一段（AGENTS.md 规则 8 的通道，Release 构建按日志级别裁掉）。 userinfo / 路径 / 查询串 / 任何头值都不进这行——路径与查询串是凭据与用户数据最常藏身的地方，而排查需要的关联键已经在标识里了。

工具链面：**无新增错误码、无新增公共头**，伞头与 `tools/check/umbrella_manifest.txt` 无变化；`aurora_api.json` 复核为零差异（两对旋钮挂在既有类上，`net::` 值类型本就不进 `enums` 段），`check_api_budget` 只多四枚方法。配套两套新判据 + 两套既加例：`utest_net_noncrypto_random`（3 例：`[0, 1)` 覆盖上下端且 200 次里出现大于 `0.95` 的取值——这条同时是下面那处修正的回归口径、`noncrypto_u64()` 一千次取值互不相同、63 位与 0 位都出现过 1 即两半拼接未退化成一枚 31 位有效段）、`utest_net_http_request_id`（4 例：头名拼写、UUIDv4 形态含版本位与变体位、500 枚互不相同、形态校验拒绝大写 / 长度不足 / 版本位 1 / 变体位 `c` / 自由文本值）；`utest_net_http_client` 由 40 例增至 **48** 例（缺省补齐、关掉不补、逐请求声明原样带上、空串声明不发、绝不覆盖调用方手写的、重试环的两次尝试同一枚、重定向链的两跳同一枚、补齐后缓存表仍命中）、`utest_net_http_stream` 由 21 例增至 **23** 例（流式两例：每跳同一枚 + 逐请求声明与客户端开关）。**一处既有用例的口径澄清**：§8.8.20 那三条「分块写出的线上字节与一次写完逐字节相同」的字节比对用例（位于 `utest_net_http_client`）此前不含自动头以外的固定偏移，补齐标识会让线上字节多出 52 字节；处置是在这三例显式 `client.request_id(false)` 并注明「本判据比的是写出次序而非头部内容」，而不是把断言从「逐字节相同」放宽成「长度差恒定」——后者恰好放过了本件最想排除的那类缺陷（同一份头被写两遍）。判据非空转已四处变异自证：去掉「已含同名头则原样发出」的分支 → **只有** `send_never_overwrites_an_identifier_the_caller_wrote` 转红；把客户端开关当恒真（缺省开的补齐无条件执行）→ `send_sends_no_identifier_when_the_default_is_off` 与流式那侧同名口径一例转红，**连带那三条字节比对用例同时转红**——这正是「开关确实在改变线上字节」的第二重证据；把补齐从 `origin` 副本挪进逐尝试循环 → `every_attempt_of_a_retry_sequence_carries_one_identifier` 转红，流式两例同因转红（逐尝试那一处拿不到「这次交换的声明」，只能无条件补），关掉缺省那例与三条字节比对用例也一并转红——一处落点之所以要紧，正在于它同时是开关与声明的唯一读取处；改在 `follow()` 里逐跳重生成（复制字段时跳过标识、下一跳另取一枚）→ **只有** `every_hop_of_a_redirect_chain_carries_one_identifier` 转红。后两处是分开的变异，各自只命中自己要钉的那条事实（重试环与重定向链复制请求的入口不同），合起来才说明「一次交换一枚」不是靠某一处侥幸成立。**一处连带修正记此**：随机源收拢时实测出 `RetryPolicy` 的抖动幅度此前被腰斩——旧的 `jitter_unit()` 取高 31 位却除以 `2^32`，值域因此是 `[0, 0.5)` 而头注释承诺的幅度是 `[0, 1]`；`noncrypto_unit()` 改为除以 `2^31` 兑现契约，而 `utest_net_http_retry` 那条宽松断言（抖动后落在 `[原值, 2×原值]`）在两种实现下都成立，故它不是这处缺陷的回归口径、`utest_net_noncrypto_random` 的第一例才是。两腿回归与门禁口径同 §8.8.20 末段。

#### 8.8.28 上传 / 下载便捷面：`BodySource` 流式请求体、`MultipartWriter`、字节范围与 `Expect: 100-continue`（CHG-018 §12 的上传便捷面）

公共面新增：抽象类 `BodySource` 与值类型 `ByteRange` / `ContentRange`、`Request` 的五枚新旋钮（`body(BodySource &)` / `range()` / `if_range()` / `expect_continue()` 及各自只读口）、`Response::content_range()` 与 `is_partial()`（全部落既有值层头 `include/aurora/net/http.h`），加一枚新头 `include/aurora/net/http_multipart.h`（`MultipartWriter`，伞头与 `tools/check/umbrella_manifest.txt` 同批收录）。库内新增 `src/aurora/net/detail/http_body.{h,cpp}`（形态计划 + 写出泵）、`src/aurora/net/http_multipart.cpp`，并把 `net/detail/http_await.h` 扩出一枚「带窗口的读」、把 `net/detail/http_wire.{h,cpp}` 补上请求侧分块编码与 1xx 前置。落码口径：

- **体的线上形态只有一个决策点**：`detail::plan_request_body()` 判一次，`Request::render_head()`（声明侧）与 `detail::RequestBodyPump`（字节侧）各问一次拿同一份计划。之所以收成一处：头部里那句 `Content-Length` 与实际流出去的字节必须说同一件事，两处各自判一遍时任何一次改动（新增体形态、允许自报长度、源改判总长）都会让「声明五个字节却发出分块」成为可能，而那种错位对端无从报错、只能等超时或错读下一段。四条裁决：**调用方写过的字段永远优先**（`Content-Length` / `Transfer-Encoding` 只要出现就照它定，与 `Accept-Encoding`、`X-Request-Id`、条件头是同一条纪律的第五个落点）；**两枚并存即拒**（`RFC 9110 §8.6` 让转发方删长度替别人收拾报文，发起侧自己声明两枚是调用点写错，当场 `http-request-invalid` 比替它猜哪枚算数更有价值）；**零长内存体不补 `Content-Length: 0`**（「没有体」与「体长为零」在线上等价，前者不必让一条 `POST` 多出一种对端可能另有解读的写法）；**分块只由「总长未知的源」触发**（本库绝不把一条有确切长度的请求主动改写成 `chunked`——那会改变中间层的转发行为，而它不是调用方要求的事）。
- **`BodySource` 三方法，`restart()` 是重放资格的唯一判据**：`next_chunk()` 交出成功且为空即「体已发完」（不用 `optional`——「本段为空但后面还有段」在线上根本没有对应形态，留着只多一条测不到的分支）；`total_size()` 交出确切长度或 `nullopt`（后者走分块）；`restart()` 能否回到原点决定本次交换进不进重试环、重定向带不带体重发。不可回退的体被自动挡在重放之外：重放半截体等于交付一份数据损坏的请求，且这个代价在失败现场无从判定，故不为此再开一枚旋钮。接口同步、被逐段问、一次调用不得等待外部事件（它跑在续体恢复所在的那条线程，缺省即主线程），拷贝与移动一律删除——`Request` 只持引用，搬动一枚已挂上的源会让那次搬运无人知晓。
- **重放闸有三处，各自问同一句 `restart()`**：起手资格判定（`send` 与 `open_stream` 各一处：挂了不可回退的源就不进重试环）、每一次尝试真正开始之前（`attempts > 1` 时复核一遍，防「源在起手说能回退、之后改了主意」）、`follow()` 里 `307` / `308` 带体重发之前。前两处各管一件事：起手那一处是「要不要套重试环」的准入，尝试之前那一处只有在源答真后又答假时才看得见。第三处失败时新增错误码 **`http-body-not-replayable`** 交出（`category = io`、`retryable = false`、`fix_category = resource_error`），而前两处不报错只是不重放——不可回退的体退出重试是资格，不是故障。
- **写出泵一段一问**：每次 `next_segment()` 至多问源要一段，内存体按 `detail::AURORA_HTTP_WRITE_CHUNK`（64 KiB，与传输层写块粒度同值）切片，源交出超上限的段则泵替它切开——分块形态切开只多几个长度行，定长形态切开不改变字节序列；不切会撞上 `StreamOptions::max_pending_bytes` 当场失败，「体不必整份进内存」这条承诺只在分段成立时兑现。分块终止块 `0\r\n\r\n` 由泵在源说结束之后补，绝不留给对端猜一条流什么时候算完。`payload_bytes()` 只数正文不含框架：上传进度回答的是「我的数据送到哪儿了」，不是「线上跑了多少字节」。
- **`Expect: 100-continue` 的等待窗口是库内置常量，不占 `Timeouts` 任何一档**（缺省关，窗口 300 ms）。开着且确有待发体时，头部写完后先等一小段：回 `1xx` 就立刻发体，到点没人应就**盲发**——后者是 `RFC 9110 §10.1.1` 明确允许的形态，且是现实里的常见路径（多数中间层压根不回 `100`）。这段等待刻意不借用本次交换的取消令牌与 `StageTimer`：令牌是连接级的，`cancel()` 会顺带掐掉这条连接，而「对端没回 100」是一条可以继续往下走的事实，不是一次取消。故 `detail::await_read_within()` 让读与窗口两路各跑各的、由一枚原子闸裁决，先到者承接结局、失者只把字节交给暂存队列绝不触碰可能已销毁的 awaiter；被放弃那次读的字节按传输层 FIFO 必然早于后续读的回调落地，故发起侧每轮先喂暂存队列再判结局，线上次序不乱。关掉时逐字节与「本功能从未存在」同型；无体的请求设了也不写 `Expect` 字段（问了也没有待发的体，只会让对端多等一个判定）。
- **装配器读的是「一条最终应答，加上它之前的信息应答」**：`100` / `102` / `103` 恒无体，收进待取队列并回到头部阶段继续读，`head()` 与 `complete()` 说的始终是最终那一条；`101 Switching Protocols` 是唯一被拒的 1xx——升级之后这条连接归谁不属 HTTP 客户端的判定，当场落 `http-protocol-error` 留给 WebSocket 切片。流水线不在本库面内（提案 §15），故一条最终应答装配完整后再收到字节即判畸形，与 `ChunkedDecoder`「终止块之后不收字节」是同一条纪律。
- **三枚协议旋钮不进 `headers()` 字段表**：`Range` / `If-Range` / `Expect` 由专用旋钮表达，只在 `render_head()` 现地补进线上字段。留在字段表之外的理由与 §8.8.19 那条同源：发起侧要据此判定线上形态、重放资格与是否先等一个 `1xx`，把这些判定的输入放进字段表就多出「读 `headers()` 以为这次没分段」的第二份真值源。`ByteRange` 三型（`between` / `from` / `suffix`，后缀型里 `last` 装的是长度而非偏移，故换算交给工厂与 `to_header_value()`）只表达一个区间——多区间的应答是 `multipart/byteranges`，本库不装配那种形态，故也不给「静默丢掉第二个区间」的口子；越界一律不裁剪（`Range: bytes=900-999` 打一条 500 字节的资源拿到 416 是协议的正确说法），形态非法（两端皆空、后缀长度为零、`last < first`）在 `render_head()` 落 `http-request-invalid`，因为那是一句要上线的话，不是一次查询。`If-Range` 只在同时设有 `range()` 时才上线。应答侧 `content_range()` 每次现解、不给 `Response` 加字段（它是纯值类型，绝大多数应答永远问不到这枚）；只认 `bytes` 单位的三种形态，字段缺失、文法不合、数字越界一律回 `nullopt` 而不报错——一条坏 `Content-Range` 附着的仍是一份要交出的应答，本层不替对端编造事实。`is_partial()` 只看状态码 `206` 而不看字段是否可解：带着坏字段的 206 仍然是「对端说了部分」这一事实，把解析失败折成「完整应答」会把它抹掉；续传落盘的调用方两枚都该问一遍。
- **带范围的请求与内存缓存之间有两道闸**：起手 `open_from_cache()` 认出 `range()` 就把键留空（`settle_cache()` 见键空即整段不收），收尾再由 `detail::is_cacheable_status()` 把 `206` 挡在可缓存死集合之外。同一 URL 的整份副本绝不能被拿来充当一个分段应答（那会把资源的前半截当成 `bytes=500-999` 的答案交出去），反向也一样。代价如实记此：范围请求一律不享缓存；且若调用方以裸 `Range` 头上行（本件认不出），前一半闸失效、后一半仍在——用 `range()` 而不是裸头是这层的约定。
- **`MultipartWriter` 是一枚 `BodySource` 实现，不是另一套体机制**：线上字节只从 `next_chunk()` 一处产出，故头部的 `Content-Type` 与实际流出去的体不可能各说一套。四条形态：文件部件只登记路径、读出发生在 `next_chunk()` 里一次至多一节写块上限（这是 `BodySource` 存在的头号理由，也是不给「先整份读进内存再交出」那条路的原因）；失败一律推迟到取字节时（追加方法返回自身引用以支撑链式写法，故不报错；部件名含 CR / LF、非法边界、边界撞进部件头或内存正文、文件打不开都在 `next_chunk()` 如实交出，`validate()` 是同一份判据的发起前检查口而非另建一套校验）；边界由库生成（取自 §8.8.27 的 `detail::noncrypto_u64()`，与重试抖动、`X-Request-Id` 同源）且可覆写，覆写值按 `RFC 2046 §5.1` 的字符集与 70 字符上限当场校验；`total_size()` 交出的是含每段边界行与部件头的完整线上长度，恒等于 `next_chunk()` 逐段交出之字节数之和，任一文件长度取不到就回 `nullopt` 改走分块——宁可少一句声明，也不报一个与实际写出对不上的长度。`restart()` 恒真：状态是一列可重放的部件描述加一个游标，故挂 multipart 的请求始终留在带体重放的白名单里（文件在两次尝试之间被换掉表现为下一次读取失败，而非不可重放）。文件部件不逐字节查边界相撞（那要先读完整份，与惰性读的承诺正相反），内存部件查。

工具链面：**新增错误码 `http-body-not-replayable`**（插在 `http-body-too-large` 之后，全表 132 → **133** 枚，经 `codespec/errors.toml` → `error_codes.gen.h` + `codespec/ERROR_CATALOG.md` + `aurora_api.json` 生成器刷新）；§8.8.22 的失败分型同批改口径，`net-*` / `http-*` 两族 42 → **43** 枚，新码归 `Invalid` 兜底档（**它拒的是这份体与这次重放的组合，动作在调用点的体形态里而不在网络上**），`utest_net_http` 的逐码期望表加一行、完整性闸从 42 收到 43。`BodySource` 是抽象类、`ByteRange` / `ContentRange` / `RequestBodyPlan` 是结构体或库内类型，均不进 `tools/include/known_enums.h`、不进 `enums` 段；`RequestBodyFraming` 是库内枚举同样不登记。伞头与 `tools/check/umbrella_manifest.txt` 各多一行（`net/http_multipart.h`）。配套判据：新增 `utest_net_http_body`（10 例：形态计划的四条裁决 + 泵的分段、终止块、超大段切分、`payload_bytes()` 口径）、`utest_net_http_multipart`（11 例：部件头文法、惰性文件读、边界生成与覆写、`validate()` 拒的六种形态、`apply_to()` 只动两枚、`total_size()` 与线上长度逐值相符）、`utest_net_http_await`（13 例：带窗口的读三型——字节先到 / 窗口先到 / 失败，失者字节不丢、对端提前关写向归 `WaitElapsed`）；既加例——`utest_net_http_wire` 由 12 例增至 **16** 例（请求侧分块编码的三种形态与终止块、1xx 前置与 `101` 拒），`utest_net_http` 由 13 例增至 **21** 例（三枚旋钮的线上补写、`ByteRange` 三型与非法形态、`ContentRange` 解析的六型分岔、逐码分型表），`utest_net_http_client` 由 48 例增至 **60** 例（三种体形态上线、expect 窗口三条腿、重放闸三处、`Range` 与缓存的两道闸、流式体未知总长走分块帧），`utest_net_http_stream` 由 23 例增至 **28** 例（同口径的流式腿 + 整份答案落在窗口里时宿主照收）。判据非空转已七处变异自证：① 摘掉起手闸的 `range()` 判据 → `a_range_request_neither_reads_nor_writes_the_table` 转红；② 把 `206` 放进 `is_cacheable_status()` 的可缓存集合 → `a_partial_answer_never_enters_the_table`（裸 `Range` 头写法，专为此半闸而设）转红——这两条合起来记明「206 不进表」是两道闸共同挡住的，摘任一半另一半仍成立，故单点变异各自只证明自己那一半；③ 摘掉 `open_stream` 交付循环的 `|| carried`（探测段已把装配器置 `complete` 的那条路）→ `stream_delivers_an_answer_that_arrived_inside_the_expect_window` 转红，宿主连 `on_head` 都收不到；④ 摘掉「尝试 > 1 之前复核 `restart()`」→ `a_source_that_changes_its_mind_about_rewinding_stops_the_retry_loop` 转红（该用例的源起手答真、第二次答假，故只有这一处看得见，起手闸无法替代）；⑤ 把「总长未知的源」改判定长 → 横跨计划、泵、渲染与两条发起路共 7 例转红（`utest_net_http_body` 3 例 + `utest_net_http` / `utest_net_http_multipart` / `utest_net_http_client` / `utest_net_http_stream` 各 1 例）；⑥ 摘掉 `MultipartWriter` 的「内存部件正文含边界即拒」→ `bad_forms_are_refused_before_a_single_byte_leaves` 与 `a_rewritten_boundary_revalidates_the_form` 转红；⑦ 把 `Expect` 的等待窗口由 300 ms 改 0 ms（等价于「不等闸门、写完头部就盲发」）→ 两腿各自**只**让 `expect_continue_holds_the_body_back_when_the_peer_answers_with_a_final_status` 与 `stream_holds_the_body_back_when_the_expectation_gets_a_final_answer` 转红（体在脚本答后继续观察的那段窗口里到达并进入留档），其余 expect 用例照绿。两处 `stream_delivers_an_answer_that_arrived_inside_the_expect_window` 一类正向断言不受影响——它们断言的是「体该上线」，盲发下同样成立，故这一处证明的是负向断言真的在看着闸门。**一处测试设施侧的结论记此**：带体的对端剧本必须把整条请求读完——只读头部就回身写应答，会在仍在写体的客户端 socket 上撞出 Winsock `os_error=10054` 的间歇红灯；处置是给 `ScriptedPeer` 加 `answer_then_watch()`（一次写完整条应答后继续读到超时或 EOF），于是「体已写出」与「体仍扣着」两种事实都能在捕获到的请求全文上直接读出。**第二处设施侧结论由两腿回归实测得出（已修）**：`answer_then_watch` 原先把留档写成「脚本收摊时一次性落档」，而用例在客户端收尾之后立刻取档。Windows 腿把这件事掩盖掉（客户端收尾晚于脚本那 150 ms 读超时），Linux 回环上客户端 2 ms 就收尾、脚本还停在读超时里，取到的是**空档案**——「`Expect` 字段该上线」因此假红，更要紧的是「体不该上线」那条**恒真**：空档案里当然找不到 `abcde`，于是它测的是脚本的时序而不是实现。处置是给两套件各加一枚 `await_recorded()`（有界等到档案就位，未就位即由 `AURORA_TEST_REQUIRE` 致命暴露），三条走 `answer_then_watch` 的用例先等再取；上面第 ⑦ 处变异同时是这条「空档案恒真」缺陷的反证——修好之后负向断言读的是一份确已收摊的档案。两腿四组构建同口径回归（本机 Win32 / MinGW 与 WSL / GCC 15 各一对，`AURORA_ENABLE_COMPRESSION` ON / OFF 各一组，失败数四组均为 0）：`=ON` 时 `utest_net_http_await` 13 + `utest_net_http_body` 10 + `utest_net_http_multipart` 11 + `utest_net_http_wire` 16 + `utest_net_http` 21 + `utest_net_http_retry` 11 + `utest_net_http_client` 60 + `utest_net_http_stream` 28 两腿逐值同过，`utest_net_http_encoding` 25 通过 + 1 跳过（那枚只在能力关闭时成立的既登记用例）；`=OFF` 时本切片新增的五套件（await / body / multipart / wire / http 共 71 例）**一例不跳、逐值同过**——它们不消费解码器，故开关只影响既有的 client / stream / encoding 三套件：client 54 通过 + 6 跳过、stream 25 通过 + 3 跳过、encoding 13 通过 + 13 跳过，跳过名册两腿逐名相同，四组构建各自复跑 `registry_integrity` 为绿。门禁口径同 §8.8.20 末段。裁决与形态来源见提案 §16 第 33 项。

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
