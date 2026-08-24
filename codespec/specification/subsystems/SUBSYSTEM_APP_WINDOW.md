# H.10 App/Scene/Window + H.10b Scheduler + H.10c DEBUG 门面 + H.10d 生命周期

> 本文件是「三、特性详细规范」按功能域/子系统划分出的子文档；返回主线索引见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。
> 后续核心子系统 API 章节（H.11–H.17 + Log + AI-First）见 [`../subsystems_api/`](../subsystems_api)：SUBSYSTEM_API_SERIALIZE / SUBSYSTEM_API_LAYOUT_ENGINE / SUBSYSTEM_API_WIDGETS / SUBSYSTEM_API_INSPECTOR / SUBSYSTEM_API_TOOLING / SUBSYSTEM_API_LOG_AI。
> 相关功能域规范（A–G）见 [`../features/`](../features)：FEATURE_API_DESIGN / FEATURE_ARCH_STATE / FEATURE_RUNTIME_SAFETY / FEATURE_LAYOUT_RENDER / FEATURE_CROSS_PLATFORM / FEATURE_AI_INSPECTION / FEATURE_AI_TOOLING / FEATURE_ENGINEERING。

#### #H.10 应用（Application）/ 场景（Scene）/ 窗口（Window）与帧循环

核心目标：把组件树接到真实窗口、输入与 VSync 帧循环。

- **`Application`**：应用驱动。构造形态（不随 backend 数量增长）：`Application(Scene, unique_ptr<Window>, WindowOptions)`（接受由
  `create_window(XxxOptions)` 产出的已组装 `Window`，`max_frames` 等运行期参数由 `WindowOptions` 透传；内部
  `set_event_handler` 把上抛事件经 `EventDispatcher + FocusManager` 集中派发）、
  `Application(Scene, unique_ptr<Surface>, WindowOptions)`（注入自定义 `Surface`，内部同样经 `create_window` 组装 `Window`
  ）；保留无头构造 `Application(Scene, w, h)`。两个持有 `Window` 的构造共用私有 `attach_window` 接线（事件派发 +
  窗口级可见性/几何态上报），避免重复。`set_on_frame(cb)` 注入每帧自定义逻辑（在 `present_root` 之前调用）；`run()`
  启动帧循环（pump → 派发 → `present_root` → `tick`）。保留 `dispatch_*` / `tick` / `render_to_png`。新增
  `set_overlay(std::shared_ptr<Widget>)`：注入独立于 widget 树的 HUD 叠加层（典型 `PerfOverlay`），由 `Window::present_root`
  在 tree paint 之后、present 之前合成；叠加层渲染到独立离屏缓冲、以 ~2Hz 重绘自身，不再触发整树重绘（见 ARCHITECTURE
  §10.2.1）。
- **`Scene`**：场景快照。`Scene::serialize()`（无参，返回 `std::string`）把当前 UI 树序列化为 JSON 结构；
  `Scene::serialize_widget(const Widget&, std::string&)` 为内部递归辅助。供 Inspector / 测试消费（见 `app/scene.h`）。
- **`Window` / `Surface`**：窗口与绘制目标抽象；后端经 `create_window` 选择（`Headless` 默认、`Win32` 零依赖、`GlfwSurface`
  真实窗口）。
- **帧循环**：`Window` 提供 VSync 回调，每帧 `tick(dt)` → `present_root`（脏区域决策）→ `present()`。单线程 UI（见
  §18）：所有构建 / 事件 / 重绘在 UI 线程。
- **脏区域追踪（默认开启）**：`Window::present_root` 按「绘制脏 `DirtyRegionTracker` / 布局脏 `m_layout_dirty` / 尺寸变化 /
  根变化」四要素决策本帧——
    - 四者全否 → **整帧跳过**（idle 零开销，上帧画面仍有效，返回 `true` 不重绘）； **例外——系统重绘请求驱动的帧不得只跳过**：由
      `set_present_request` 回调（Win32 `WM_PAINT`/`WM_SIZE`）驱动的 `present_root`，即便脏追踪判定跳帧，也必须
      `set_present_dirty({})` 后 `present()` 全量 blit 重新上屏——帧缓冲内容仍有效但窗口表面已被 OS 置无效（典型：
      **最小化还原**后为类背景刷底色，若只跳过则白屏；遮挡揭开同理）；普通 idle 帧不受影响，仍零上屏（回归：`test_present_skip`
      #6）；
    - 仅绘制脏（如文本选区高亮、主题/颜色切换、局部 `State` 文本变更）→ **跳过整树 `layout`**，复用已缓存 `Node` 几何直接
      `paint`（拖选在大窗/最大化下收益随面积放大最显著）；本帧绘制时 `present_root` 进一步 **跳过 `begin_frame`
      保留上帧帧缓冲**（`Headless`/`D3D11` 的 `begin_frame` 会清零整帧，直接 begin 会使裁剪外黑屏），先
      `Painter::clear_rect(merged_bounds)` 把脏矩形并界重置为新帧零基底，再 `push_clip(merged_bounds)` 裁剪重绘（逻辑 dp，与
      `Painter` 同坐标系）——裁剪内从零基底按原序重新合成、裁剪外沿用上帧像素， **两侧均与整帧重绘逐位一致（golden
      零差异，无残留、无半透明双重混合）**；`wire_dirty` 回调标记控件最近一次 `paint` 的绝对几何（`Widget::paint_bounds()`
      ，而非一律 `mark_all`），使裁剪命中精确区域。（子树跳过作为后续性能增强，当前以逐像素裁剪保证正确性的前提下不做，避免文本/AA
      发光溢出绘制被跳掉产生差异。）
    - 布局脏或尺寸变化 → `layout + paint`； **Win32 上屏性能契约**：`Win32Surface::present()` 经常驻 BGRA（GDI 原生序）DIB
      section + `BitBlt` 上屏（全量帧整幅 RGBA→BGRA swizzle，脏区帧仅 swizzle+blit 脏矩形）， **禁止退回 RGBA 掩码（
      `BI_BITFIELDS`）直推**——非原生掩码会迫使 GDI 逐像素慢速转换（5760×3132px 实测全量 87ms vs 现 ~8ms，最大化/resize
      卡顿主因；bench `bench_win32_present` ④ 监控）；`WM_SIZE` 同步重渲染后 `ValidateRect`，免去紧跟 `WM_PAINT` 的重复全量上屏；
    - 根变化（如 `Navigator` 切换页面、`run_demo` 换树）→ 强制整体重绘，避免停留旧页面。
      `mark_needs_layout()` 置「布局脏 + 绘制脏」，`mark_needs_paint()` 仅置「绘制脏」；`Widget::on_dirty` 改为
      `std::function<void(bool)>`（`true`=含布局脏）经 `wire_dirty` 接线整棵树。`enable_dirty_tracking(bool)`
      可关闭回到每帧全量重绘历史行为；`force_full_redraw()` 供动画/视频/定时器持续重绘或外部环境突变时强制下一帧全绘。
      **首帧 `m_first_frame=true` 强制全绘**；重新挂载/根变化时自动 `mount` 接线响应式订阅，使 `State`
      /修饰变更能标脏重绘（修复旧版靠每帧全量重绘掩盖的「未挂载树不响应响应式」潜在回归）。
    - **脏矩形数量上限可调（`render/dirty_region.h`）**：`DirtyRegion` 累积的矩形数超过上限时会合并为单一并界（避免矩形数无界增长导致裁剪与遍历成本反超收益）。上限默认仍为
      `DirtyRegion::AURORA_MAX_RECTS == 16`，新增进程级可调接口 `DirtyRegion::max_rects() -> std::size_t` /
      `DirtyRegion::set_max_rects(std::size_t)`，`mark()` 改读 `max_rects()`。用途是按场景调参（如大量小控件独立标脏的列表页可上调、超大窗口可下调）；
      **改变该值只影响合并时机与性能，不影响像素结果**——裁剪内始终从零基底按原序重新合成，与整帧重绘逐位一致。
- **Win32 系统重绘处理（`win32_surface.h`）**：窗口类背景刷 `wc.hbrBackground` 用浅灰实心刷 `RGB(245,245,247)`（而非默认黑色擦除）；
  `wnd_proc` 处理 `WM_PAINT`，在系统要求重绘（最大化/缩放）时立即 `present()` 当前已就绪帧缓冲，填平「OS 放大 → 内容跟上」空档。
  `GLFW`/`Headless` 后端无此问题。
- **Win32 最大化白闪处理（`Surface`/`Window`/`win32_surface.h`）**：浅灰刷只擦除「系统要求重绘」的无效区，但最大化时 **DWM
  窗口动画**——动画首帧 OS 抓取窗口重定向位图时，新放大区域尚无内容（离屏缓冲要等下一帧 `present_root` 才按新尺寸重渲染），DWM
  把那块显示为白色，表现为「先白屏再出内容」。处理方式：`Surface` 提供 `set_present_request` 回调通道（默认空实现，`Headless`/
  `GLFW` 不触发），`Window` 构造时把该回调接为「对当前缓存根再渲染一帧」；`Win32Surface` 的 `WM_SIZE`/`WM_PAINT`
  在几何变化当下同步调用该回调，使离屏缓冲在 DWM 合成前已为新尺寸真实内容。浅灰刷保留作兜底。`present_count()`
  观测器供测试验证「WM_SIZE 触发了同步重渲染」。
- **事件驱动帧循环（`window/frame_pacing.h` / `Surface` / `Window` / `Application`）**：`Application::run` / `Window::run`
  采用「事件驱动 + 帧节流」（静态界面空闲 CPU 趋近 0，而非忙轮询占满一核）。
    - **决策纯函数**：
      `compute_wait_timeout(has_dirty, anim_active, next_deadline_ms, frame_budget_ms, elapsed_ms, backend_paced) -> double`
      （返回毫秒：`<0` 无限等待 / `0` 不等 / `>0` 等待时长）。
    - **`Surface` 扩展**：`wait_events(double timeout_ms)`（阻塞到事件/超时；默认限频 sleep，`Headless` 覆盖为 no-op，Win32=
      `MsgWaitForMultipleObjectsEx`、GLFW=`glfwWaitEventsTimeout`）、`request_wake()`（线程安全唤醒；Win32=
      `PostMessage(WM_NULL)`、GLFW=`glfwPostEmptyEvent`）、`paces_frames()`（后端自带帧节拍，如 D3D11 vsync）。
    - **`Surface` DEBUG 契约（`window/surface.h`）**：两个虚函数 `save_snapshot(path)`（导出当前帧软件帧缓冲为 PNG；默认实现按
      `data()` 判空后调 `write_png`，各真实后端在 `AURORA_ENABLE_DEBUG` 下覆写 `data()` 返回 Painter 缓冲即通用可用）与
      `capture_window(path)`（导出真实屏幕窗口为 PNG，默认返回 `GeneralNotSupported`，Win32/X11/GLFW 在
      `AURORA_ENABLE_DEBUG` 下覆写，Wayland/Headless 保持 unsupported）。`capture_window` 的实质实现：Win32 经共享内部
      `detail::capture_window_by_hwnd`（`src/aurora/window/win32_capture.h`，仅 `_WIN32`）——
      `PrintWindow(PW_RENDERFULLCONTENT)`（含非客户区）入 32bpp BGRA DIB，失败回退 `BitBlt` 从窗口 DC，再
      `swizzle_bgra_to_rgba` → `write_png`；GLFW-on-Windows 经 `glfwGetWin32Window` 取原生 HWND 复用该路径，非 Win32 的
      GLFW 回落 unsupported；X11 经 `XGetImage` 抓窗口并按 Visual 掩码（`red/green/blue_mask` + `XGetPixel`）提取 RGBA →
      `write_png`；Wayland/Headless 保持默认 unsupported。两函数 **始终声明**（vtable 槽稳定，属 Surface 契约）；Release
      下默认实现仅判空/返回错误，零开销；宏只裁切后端专属覆写体，不门控默认实现，避免宏不一致引发 ODR。`native_handle()` 同步改为
      `const`（只读查询，不修改 Surface 状态），Win32/X11/Wayland 覆写随之更新（GLFW/D3D11/Wasm 沿用基类 `nullptr`）。另新增虚函数
      `framebuffer_size()`（默认返回逻辑 `size()`）：`save_snapshot` 以它作 PNG 宽高；`Win32`/`D3D11` 因 painter 缓冲按 DPI
      物理分辨率分配须覆写返回物理像素，避免缩放比≠1 时 PNG 尺寸与像素数据错位（GLFW/X11/Wayland 缓冲为逻辑尺寸，默认实现已正确）。统一门面与收编见
      **#H.10c 真实后端 DEBUG 门面（`aurora::debug`）**。
    - **`Window` 新增**：`has_pending_dirty()`、`set_next_wait(double)`；`run` 在帧末 `wait_events`
      阻塞（一次性消费，未设则不等，低阶调用方行为不变）。
    - **`Animator::has_active()` / `Scheduler::next_deadline_ms()`**：供决策判断活跃动画与最近定时任务到期。
    - **`WindowOptions` 新增**：`int max_fps=60`（活跃帧帧率上限，`0`=不限）、`bool power_saving=true`（idle 阻塞等待；`false`
      =忙轮询旧行为，持续重绘场景 opt-out）、`RendererPreference renderer=Auto`（上屏后端偏好）。
    - **跨线程回投**：`Application::run` 安装 `Task::set_main_poster`，`au::async` 的 `then` 回调入队 + `request_wake()`
      唤醒主循环，下一帧开头主线程排水执行（单线程 UI 不变；无运行循环时在完成线程直接调用，行为不变）。
    - **`FrameStats` 新增**：`record_wait(double)`、`wakeup_count()`、`wakeups_per_sec()`、`sleep_ratio()`（`PerfOverlay` 第三行
      `stats_line3()` 展示）。
    - **验收**：静态界面稳态 CPU < 5%（改造前 ≈ 100% 单核；`tools/bench_idle_cpu.exe` 实测 0.0%）；活跃场景帧率钳制在
      `max_fps` 附近；输入到重绘延迟不高于现状（`wait_events` 被输入即时打断）；全量 ctest 通过、golden 零差异。
- **硬件加速上屏偏好（CPU 性能专项 B，`RendererPreference` / `create_window(Win32Options)`）**：强类型枚举
  `RendererPreference{ Auto, Software, GpuD3D11 }` 统一控制上屏后端（仅影响「像素如何上屏」，绘制仍由软件 `Painter` 完成）：
  `Auto`——编译了 `AURORA_BACKEND_D3D11` 且设备可用则走 D3D11 GPU 上屏，否则静默回退 Win32/GDI（`AURORA_LOG_INFO`）；
  `Software`——强制 GDI；`GpuD3D11`——强制 D3D11，未编译/设备创建失败返回 `make_error(ErrorCode::RendererUnavailable, ...)`
  （slug 为 `"renderer-unavailable"`，不静默降级，错误归属调用方）。`D3D11Surface` 补强：device-lost 恢复（present 报
  `DXGI_ERROR_DEVICE_REMOVED/RESET` → 下次 `poll_platform_events` 重建 device/swapchain + 全量重渲染）、`set_vsync(bool)`与
  `paces_frames()`（vsync `Present(1,0)` 自带节拍，帧调度跳过 CPU sleep；`D3D11Options.vsync` 默认 `true`）。

```cpp
auto win_res = au::create_window(au::Win32Options{ .title = "Demo" });
au::Application app{ build_ui(), win_res ? std::move(win_res.value()) : nullptr };
app.set_on_frame([] { /* 把共享状态写入 Reactive 标签 */ });
app.run();                           // 帧循环 + 事件派发

// 自定义 backend：注入任意 Surface 子类（不随内置 backend 增加 Application 构造） au::Application custom{ build_ui (),
std::make_unique<MySurface>() };

// 特化选项：先经工厂组装 Window，再交给 Application / App（类型安全在工厂处保证） auto win = au::create_window (au::
HeadlessOptions{ .png_path = "out.png" }); au::App ().window (win ? std::move (win.value ()) : nullptr).view (build_ui
()).frames (1).run ();
```

- **`au::App()`流式构建器（已实现，§4.5）**：薄封装，内部构造 `Application` 并进入帧循环，不破坏旧用法。`title(string)` / `size(w,h)` / `surface(unique_ptr<Surface>)`（自定义后端，注入后 `run()` 经 `create_window` 组装 `Window`）/ `window(unique_ptr<Window>)`（接受 `create_window(XxxOptions)` 产出）/ `view(Node)` / `on_frame(cb)` / `frames(int)`（限制帧数，`-1` 跑到 `should_close`）/ `overlay(shared_ptr<Widget>)`（链式注入 HUD 叠加层，等价于 `Application::set_overlay`）后调 `run()`。`run()` 后端优先级：自定义 `Surface` > 预组装 `Window` > 自动检测（默认 `auto_detect_surface()`）。
- **`au::platform()`**：显式运行时平台查询，跳过后端探测。返回 `Platform{ PlatformKind kind, DeviceKind device, SurfaceKind backend }`，含 `is_mobile()` / `is_desktop()` / `capabilities()`（`PlatformCapabilities{ multitouch, high_frequency_pointer, desktop, mobile }`）。`multitouch` 在真实显示后端（Win32/Glfw）为 `true`、`Headless` 为 `false`；`high_frequency_pointer` 在 Glfw/Win32 为 `true`。

```cpp
// 一行式启动（等价于上方 Application 构造）
au::App().title("Demo").size(640, 480).view(build_ui()).run();

// 平台/能力显式查询（无 #ifdef，运行期）
const au::Platform p = au::platform();
if (p.is_mobile() && p.capabilities().multitouch) { /* 启用多指手势 */ }
```

#### #H.10c 真实后端 DEBUG 门面（`aurora::debug`）

核心目标：为真实后端 DEBUG 能力（设计取舍与落地偏差见 `ARCHITECTURE.md` §10.7）提供统一、门控（`AURORA_ENABLE_DEBUG`
）的薄封装入口，收编零散调试能力，不搬迁任何生产子系统引擎（Inspector / Diagnostics / perf 留原地）。所有项在
Release（未开开关）下不产出调试代码：`capture` 返回 disabled 错误、`surface_state` 返回 unavailable JSON；输出目录 API
为纯文件系统辅助，始终可用。

- **命名空间**：`aurora::debug`（扩展自 `core/debug.h` 既有 `check_render_purity()`）；头
  `include/aurora/debug/debug_backend.h`，经 `aurora.h` 单一入口暴露。
- **`CaptureSource{ Framebuffer, OnScreenWindow }`**：截图源。`Framebuffer` = 软件帧缓冲（全后端通用、确定性）；
  `OnScreenWindow` = 真实屏幕窗口（含 OS 装饰，Wayland/Headless 不支持）。
- **`capture(Surface&, path, src=Framebuffer) -> Result<bool>`**：调 `resolve_output_path` 解析路径 → 自动建父目录 → 转发
  `Surface::save_snapshot`（Framebuffer）或 `Surface::capture_window`（OnScreenWindow）。相对文件名落入 `output_directory()`
  ，绝对 / 带目录的相对路径原样使用。
- **输出目录 API**（无调试内部依赖，始终可用）：`set_output_directory(dir)`（空串复位为 `current_path()/aurora_debug`）、
  `output_directory()`（未显式设置时懒计算）、`resolve_output_path(path)`（纯文件名→目录前缀；其余原样返回）。
- **`surface_state(const Surface&) -> Json`**：`width`/`height`/`scale_factor`/`frame_count`/`clear_color`(RGBA 数组)/
  `should_close`/`has_native_window`；DEBUG 下可用，Release 下返回 `{"available":false,"reason":...}`。
- **门控与 ODR 安全**：API 头 **始终声明**、`.cpp` 体按 `AURORA_ENABLE_DEBUG` 裁切；`Surface::save_snapshot`/
  `capture_window` 默认实现按运行时 `data()` 判空（宏无关），后端专属截图体门控；宏为 PRIVATE（不导出消费者 ABI，但 CMake
  在同配置注入测试目标使测试可编译/调用成功路径）。`Surface::data()` 各真实后端在 `AURORA_ENABLE_DEBUG` 下覆写返回 Painter
  缓冲（Win32/X11/Wayland 本专项补；D3D11/GLFW/Wasm 此前已覆写）。
- **可视化调试叠层 `DebugPaintFlags` 与控件拾取（头 `include/aurora/debug/debug_paint.h`，经 `aurora.h` 暴露）**：全部门控
  `AURORA_ENABLE_DEBUG`，Release 下头始终声明、体为零开销 no-op。
    - `DebugPaintFlags{ layout_guides, relayout_boundaries, layer_borders, repaint_highlight, overdraw }`（bool，默认全
      false）：运行时叠层开关。
    - `set_flags(const DebugPaintFlags&)` / `flags()` / `any_flag_enabled()`：全局开关读写；Release 下 `set_flags` 为
      no-op、`flags` 恒全 false、`any_flag_enabled` 恒 false。
    - `current_debug_frame()` / `bump_debug_frame()`：调试帧计数（`repaint_highlight` 时序判定用）。
    - `paint_debug_overlays(Painter&, const Widget& root, const Rect& root_bounds, const BuildContext& ctx)`：在
      `Window::present_root` 全树绘制后调用，按开启的 flag 统一绘制 5 个叠层（layout_guides 青色盒 / relayout_boundaries
      品红盒 / layer_borders 橙盒 / repaint_highlight 循环色填充 / overdraw 控件粒度暖色热力）；仅 `AURORA_ENABLE_DEBUG` 且
      `any_flag_enabled()` 时进入，Release 不编译分支。
    -
  `DebugOverlayStats{ layout_guides_drawn, relayout_boundaries_drawn, layer_borders_drawn, repaint_highlight_drawn, overdraw_regions_drawn }` +
  `overlay_stats()` / `reset_overlay_stats()`：叠层绘制计数（测试断言用）。
    - `widget_picker(Widget& root, const Rect& root_bounds, const BuildContext& ctx, Point screen) -> DebugPickResult`
      ：复用 `Widget::hit_test_chain` 解析命中链，返回 `DebugPickResult{ chain（root→最深，元素 `DebugPickNode{ type_name,
      bounds }`）, hit }`；`Surface` 不持有树根故根由调用方显式提供；Release 返回 `{ {}, false }`。
    - **运行时信息导出（头 `include/aurora/debug/debug_runtime.h`，经 `aurora.h` 暴露）**：5 个薄封装 / 聚合门面，全部双门控
      `AURORA_ENABLE_DEBUG`；Release 下统一返回 `{"available":false,"reason":"AURORA_ENABLE_DEBUG not enabled"}`
      （零开销，不产出调试代码）。
        - `widget_tree(const Node& root) -> Json`：薄封装 `Inspector::tree_json_full(root)`（引擎留 `inspector/`），含`type`
          等控件树结构字段。
        - `perf_snapshot() -> Json`：聚合 `FrameStats` 读数（`fps` / `avg_frame_ms` / `worst_frame_ms` / `jitter_ms` /
          `p50_ms` / `p99_ms` / `dropped_frames` / `dropped_ratio` / `hitches` / `idle_frames` / `total_frames` /
          `frame_budget_ms`）+ `PerfLog::snapshot_json()`（包为 `perf_log` 子对象，parse 失败容错为 `{}`）。
        - `frame_phase_timeline(std::size_t limit=64) -> Json`：复用 `FrameStats` 已维护的 `layout` / `paint` / `present`
          三相位环形缓冲——输出 `avg_layout_ms` / `avg_paint_ms` / `avg_present_ms` / `fps` / `avg_frame_ms` /
          `worst_frame_ms` / `dropped_frames` / `hitches` + 最近 `limit` 帧 `recent_frame_ms`（秒→毫秒，最新在前）+ ASCII
          `flamegraph`（48 宽，`L=layout / P=paint / R=present`）。 **偏差**：原计划含 `composite` 四段，引擎无独立 composite
          阶段，落地为 L/P/R 三段。
        - `why_trace(std::size_t limit=64) -> Json`：输出 mark-needs-layout / paint 触发因果链（`AURORA_ENABLE_DEBUG` 下经
          `detail::record_dirty` 写入全局 ring buffer，cap 256，超界 `pop_front`）。返回 `count` / `total_recorded` /
          `entries[]`；每条
          `entry{ kind("layout"|"paint"), type(控件类型名), frame(调试帧), propagated(false=业务根因 / true=引擎沿父链自动冒泡) }`。
          **偏差**：原计划「复用现有埋点通道」实际不存在，落地为最小 DEBUG 热路径埋点——`mark_needs_*` 公开签名不变（委托私有
          `*_impl(bool propagated)`），仅 DEBUG 下 `record_dirty` 写缓冲；Release 热路径零开销（`record_dirty` 全吞参）。传播在
          relayout boundary 处截断（boundary 自身局部重排，不向上冒泡）。
        - `diagnostics() -> Json`：只读封装 `Diagnostics::get_last_diagnostics()`（生产子系统留原地）——`count` +
          `diagnostics[]`（每条 `severity` / `category` / `message` / `where` / `code` / 可选`fix{ code, description }`）。
        - 单测 `tests/test_debug_runtime.cpp`：Headless 双配置断言（Debug 字段断言 / Release `available==false`）；覆盖 5 个
          API 与 why_trace 的 `propagated` 根因/传播区分（用非 relayout boundary 控件 + 显式 `set_layout_parent` 建父链观测
          `propagated=true`）。

#### #H.10b 定时任务（Scheduler / Timer）

核心目标：提供通用延时 / 周期定时能力（库内原仅有 `async` 后台计算 + 超时、`Animator` 帧动画进度、`Application::run()` 每帧
`on_frame` 回调），由帧循环按 `steady_clock` 复用每帧 `dt` 驱动、主线程触发，单线程 UI 内无额外计时线程。

- **`Scheduler`（`app/scheduler.h`，应用驱动层）**：由 `Application::run()` 每帧 `tick(dt)` 推进（与 `Animator` 并列），并通过线程局部
  `Scheduler::current()` 暴露给组件级 `Timer`（`run()` 起止自动 `set_current` / 重置）。
    - `set_timeout(d, cb) -> TimerHandle`：一次性延时任务，到期触发一次并自动移除。
    - `set_interval(period, cb) -> TimerHandle`：周期任务，每 `period` 触发，直至 `TimerHandle::cancel()`。
    - `tick(double dt_seconds)`：每帧推进并触发到期项（先收集到期项、再统一触发，避免回调内重注册导致迭代器失效）。
    - `clear()`：取消并清空全部任务。
    - 实测规模下内部用 `std::vector` + 线性扫描，每帧 O (n)，无堆抖动；回调均在主线程（跨线程仅经 `State::set` 回写，与
      `async` 一致）。
- **`TimerHandle`**：`set_timeout`/`set_interval` 的返回值；轻量、可拷贝、可值传递；`cancel()`（幂等）置取消标志——不访问
  `Scheduler` 实例，句柄可安全跨作用域持有（含 `Scheduler` 已析构后取消）；`active()` 查询是否仍活跃。内部经
  `shared_ptr<TimerEntry>` 引用，触发前校验 `cancelled` 避免悬空。
- **组件级 `Timer`（`widget/timer.h`，控制流 Widget）**：声明式、响应式为主 + 可选回调。
    - 构造 `Timer(period, TickBuilder, on_tick = {})`：`TickBuilder = std::function<Node(const SignalView<int>&)>`
      ，在构造时调用一次构建子树，内部经响应式绑定刷新（非每次 tick 重建）。
    - 挂载（`on_mount`）向 `Scheduler::current()` 注册周期任务；每次 tick 自增内部 `State<int>`（经 `ticks()` 暴露为
      `SignalView<int>`，计数从 1 起）驱动子 UI 自动重绘，并可选调用 `on_tick(int)`。
    - 卸载 / 析构自动 `cancel()` 句柄，停止后续 tick。
    - 降级：无运行中 App（`Scheduler::current() == nullptr`，如纯 `render_to_png`）时记 `Diagnostics::warn` 并以`ticks()==0`
      渲染子 UI，不崩溃。
    - 跨框架对照：`setTimeout` / `setInterval`（JS）、`Timer.periodic`（Flutter）、`Disposable` / `Handler`（Android）。

```cpp
// 应用级命令式：每 1s 轮询，5s 后停止
au::Application app{ /* ... */ };
auto &sched = app.scheduler();
au::TimerHandle h = sched.set_interval(1s, [&] { poll(); });
sched.set_timeout(5s, [&] { h.cancel(); });

// 组件级声明式（时钟：子 UI 经 ticks() 响应式刷新）
au::Timer(1s, [](const au::SignalView<int> &tick) {
    return au::Text(au::computed([&] { return "tick: " + std::to_string(tick.get()); }));
});
```

#### #H.10d 生命周期（Lifecycle 控件 / WindowState / WindowMode）

Aurora 不移植安卓 `Activity` 的 `onCreate/onStart/onResume/onPause/onStop/onDestroy` 命令式生命周期（其面向 OS
窗口可见性，与声明式树模型冲突、且 per-widget 的 resume/pause 无实际意义）。改为两级正交机制：

1. **控件子树级 `au::Lifecycle`**（控制流 Widget，对齐 React `useEffect([])` + Flutter `initState`+`dispose`，对应 Android
   `View.onAttachedToWindow`/`onDetachedFromWindow`）：
    - 构造 `Lifecycle(Node child, MountCb on_mount, UnmountCb on_unmount = {})`；`on_mount(const BuildContext&)`
      在子树挂载完成后恰好触发一次（可注册外部源、启动定时器、读环境注入），`on_unmount()` 在控件被销毁（RAII 析构，如
      `Repeater` 缩容 / `Navigator` pop / 持有 `Node` 释放）时触发清理。
    - 卸载依赖 RAII 析构而非新增虚函数：派生类析构无法正确派发到 `~Widget()`，故用标准库 `std::function` 回调 +
      析构触发，零额外容器改造、无回归面。
    - `Show` 隐藏子树时保留 `Node` 存活（不析构、不卸载），故 `on_unmount` 不被触发——与 Flutter `Visibility` 的「隐藏保留状态」语义一致。
    - 跨框架对照：`useEffect`（React）、`initState`/`dispose`（Flutter）、`View.onAttachedToWindow`/`onDetachedFromWindow`
      （Android）、`QObject::installEventFilter`/`QTimer`（Qt）。

2. **窗口/应用级 `WindowState` + `WindowMode`**（两个正交枚举，仅窗口层报告——所有主流桌面框架均只在窗口/应用层报告可见性/几何态，从不
   per-widget）：
    - `WindowState`（`window_state.h`）：`Visible`（前台激活）/ `Occluded`（可见但失焦，近似为「被其它窗口遮挡」）/ `Hidden`
      （最小化/不可见）。由后端 `(minimized, active)` 经纯函数 `compute_window_state`
      映射；精确像素级遮挡检测昂贵且普遍不被框架支持，故以「失焦 = 被遮挡」近似。
    - `WindowMode`（`window_state.h`）：`Normal` / `Maximized` / `Minimized` / `FullScreen`，与 `WindowState` 正交——最大化只改变几何态，
      `WindowState` 仍为 `Visible`；最小化同时满足二者。`compute_window_mode(minimized, maximized, fullscreen)` 提供纯函数映射。
    - 后端上报：`Win32Surface` 处理 `WM_SIZE`（MINIMIZED/MAXIMIZED/RESTORED→几何态；最小化同时置 `WindowState::Hidden`）+
      `WM_ACTIVATE`（非 `WA_INACTIVE`→`Visible` 否则 `Occluded`）；`GlfwSurface` 注册 `glfwSetWindowIconifyCallback` /
      `glfwSetWindowFocusCallback` / `glfwSetWindowMaximizeCallback`；`HeadlessSurface` 提供 `simulate_window_state` /
      `simulate_window_mode` 测试 seam。后端各自仅「状态变化时」经 `Surface::WindowStateHandler` / `WindowModeHandler`
      上报，逻辑零重复、可单测。
    - 上层聚合：`Application` 暴露 `State<WindowState>& window_state()` / `State<WindowMode>& window_mode()`（响应式：在
      `Effect` 内读取自动订阅刷新）与命令式回调 `set_on_window_state` / `set_on_window_mode`；每帧经`Window::present_root`
      将当前快照注入根 `Environment`，子树可 `ctx.environment<WindowState>()` /`ctx.environment<WindowMode>()` 读取。
    - 跨框架对照（窗口级）：`Activity.onResume/onPause/onStop` ≈ `WindowState`；Qt `WindowMaximized` / WPF
      `WindowState.Maximized` / Win32 `SIZE_MAXIMIZED` ≈ `WindowMode::Maximized`；Flutter
      `WidgetsBindingObserver.didChangeAppLifecycleState` / Web `document.visibilityState` / `window` blur-focus ≈
      `WindowState`（仅应用级）。

```cpp
// 控件级：挂载计数 / 卸载清理
au::Lifecycle(
    au::Text{ "子树" },
    [](const au::BuildContext &ctx) { subscribe(); },   // on_mount：恰好一次
    []() { unsubscribe(); }                              // on_unmount：销毁时清理
);

// 窗口级：订阅响应式 State（Effect 内读取自动刷新）
au::Application app{ /* ... */ };
app.set_on_window_state([](au::WindowState s) {
    if (s != au::WindowState::Visible) pause_animation(); // Hidden/Occluded 暂停
    else resume_animation();
});
// 子树内读取当前快照：
//   const auto *ws = ctx.environment<au::WindowState>();
```

#### #H.12 文字选中（Text / TextInput）

- **角色区分**：`Text` 是 **只读文本显示控件**，`TextInput` 是 **可编辑文本控件**。二者选区状态机相同，但「编辑光标（caret）」与「编辑键」仅属于
  `TextInput`：
    - `Text`（只读） **不绘制 caret**，点击/获焦均不显示插入点；仅支持选区高亮、拖选与 `Ctrl+A`/`Ctrl+C` 复制。`Ctrl+C` 复制
      **当前选区**； **无选区时为 no-op（不复制整段文本）**。
    - `TextInput`（可编辑）绘制 caret（经 `Painter::fill_rect` 的竖条），并支持完整键盘编辑（方向键移动、Backspace/Delete、Shift
      扩选等）。
- **选区状态（含头含尾模型，修复端点字符漏选）**：`Text`/`TextInput` 维护 **含头含尾**的 `[sel_start, sel_end]` 选区——
  `sel_start`/`sel_end` 均为「被选中字符的码点下标」，按下与松开所在字符 **始终计入**选区（右半点击某字符亦计入该字符），彻底消除旧半开区间
  `[sel_start, sel_end)` 在端点处的 off-by-one 漏选（`hit_test_char` 右半落入下一字符导致首/尾字符未高亮）。无选区时
  `sel_end == k_no_sel`（`has_selection()` 为 false，`selection()` 回退返回 `[caret, caret]`）。`selection()` 对外返回
  `[first, last+1]`（半开区间，便于直接切片 `cp_slice`）。落点：锚点 `caret` 用 `hit_test_char`（单击精确定位）， **选区端点
  `sel_start`/`sel_end` 用 `hit_test_char_inclusive`**（点击字符右半归入该字符，保证端点计入）；`caret` 对只读 `Text`
  仅作拖选锚点，不渲染。
- **指针**：`Press` 经 `hit_test_char` 落锚点并 `request_focus`；`Move` 且按住则拖选；`Release` 结束。
- **键盘（TextInput 编辑键）**：`Shift+Left/Right` 扩选、`Ctrl+Shift+Left/Right` 按词扩选、`Ctrl+A` 全选、`Ctrl+C` 经
  `Clipboard::set_text` 复制；方向键移动光标、`Backspace`/`Delete` 删除。（`Text` 只读仅响应 `Ctrl+A`/`Ctrl+C`，不响应编辑键。）
- **绘制**：选区高亮（`Color{80,120,220,110}`，半透明蓝）；caret 仅由 `TextInput` 经 `Painter::fill_rect` 绘制；`Clipboard`（
  `app/clipboard.h`）在 Win32 经 `SetClipboardData` 实现，默认 no-op。
- **Justify 行的选区与命中与绘制同源**：`TextAlign::Justify` 的非末行按逐词均分拉伸铺满整行，选区高亮与命中测试必须按
  **拉伸后的词位**（`Text::justify_layout` 与 `on_paint` 同源）计算，而非自然宽度——否则多行选中时高亮短于实绘行，行尾未被高亮。约定：拉伸行选区覆盖到行尾时高亮延伸到
  **行右缘**；词间拉伸间隙整体归属其空格字符（点击间隙命中空格、选中空格时高亮整个间隙）；末行/单行/不足两词的行不拉伸，退化为
  `FontEngine::caret_x`/`hit_test_char*` 自然路径。
- **实显 caret 校正（缩放屏选区与实绘像素对齐）**：文本度量（`FontEngine::measure_width`/`caret_x`）恒按逻辑 96 DPI（dp
  空间），而字形光栅按 **帧缓冲像素比推导的物理 DPI**（`draw_text` 的光栅 DPI = `lround(96 × Painter::scale())`，非全局主屏
  `dpi_y()`：Headless/测试 `scale=1` 按 96 光栅与度量自洽，窗口跨屏跟随所在显示器；间距模式的 letter/word spacing 为
  dp，物理推进须乘 `scale`）。GDI hinting 在两个 DPI 下逐字形取整 **不成比例**，整串绘制的行（非拉伸行）实绘字形位置偏离 96dp
  测量：行尾累计成「全选后末行尾部欠高亮」，行内则造成「从第一个字符开始选择、实际选中第二个」的 off-by-one 命中。约定：非
  Justify 行的选区高亮与命中一律按 **实显 caret**（`FontEngine::display_caret_x`：按 `Text` 缓存的最近一次绘制
  `Painter::scale()` 推导物理 DPI 测前缀 extent 后折算回 dp；GDI 整串绘制无 kerning，前缀 extent 即字形实绘起笔位置，
  **逐字符精确**）计算，命中经 `display_hit_test_char`/`display_hit_test_char_inclusive`（语义同自然版，字符边界改取实显
  caret）；禁用整行实显/自然宽度比的线性近似（仅行尾精确，行内相邻窄字符处跨边界）。Justify 拉伸行逐词定位、误差不跨词累计，仍走自然路径。退化：
  `scale=1`（含 Headless 测试）、间距模式（逐字形绘制不累计）或非 Windows 下实显版与自然版完全等价。
- **containment（高亮不外溢邻行）**：只读 `Text` 的选区高亮必须 **裁剪到本控件 `bounds`**（`push_clip`/`pop_clip`
  ，栈式、不破坏父容器裁剪，也不裁剪文本本身——带 `Modifier::scale/rotate` 的 `Text` 变换溢出绘制不受影响）；且高亮矩形高度
  **严格贴合实际文本行几何**（`行数 × 行高`
  ），而非整个布局盒高度。否则当盒子比文本高（文本顶对齐时下方留白、或父容器纵向拉伸放大盒子）时，半透明高亮会覆盖文本下方的空白带、紧贴相邻控件，造成「邻行被选中」的视觉假象。选区状态本身按控件实例隔离（
  `m_sel_start`/`m_sel_end` 私有），跨 widget 选中在事件层不可能发生。
- **指针捕获（拖选连续、释放不丢失）**：`EventDispatcher`（鼠标）与 `TouchDispatcher`（触控） **均已实现指针捕获**——`Press`
  命中后即缓存命中链，后续 `Move`/`Release` 即使实时 `hit_test` 失败（光标移出窗口 / 落入重叠的兄弟控件）也持续派发给同一目标，直到
  `Release` 解除捕获。这修复了「拖选时光标移出窗口、释放事件丢失导致选区卡在 `m_selecting`」以及「相邻控件重叠导致拖选被抢、首字选不中」两类问题。注意：鼠标
  `Release` 不在此切换焦点（焦点仅在 `Press` 转移），避免拖选结束落在邻行/窗口外时焦点被抢、选区被清空。
    - **便捷入口同样保留捕获**：静态 `EventDispatcher::dispatch` 现委托进程内「持久」`EventDispatcher`
      单例（指针捕获表存于该单例、非全局静态，避免悬空 `Widget*`），因此 **经该入口派发（如 `run_demo` 直接调用
      `EventDispatcher::dispatch`、或自定义 `Row` 布局用静态入口）同样享受跨事件捕获**——RTL 拖选越过文本左/右边界、或光标移出窗口时，
      `Move` 仍持续派发给按下时命中的 `Text`，`hit_test_char(文本, lx<=0)` 正确落 0，首字不被遗漏。指针捕获表刻意保持「实例成员」（而非全局静态），因控件树按
      `shared_ptr`/栈管理，全局静态会缓存悬空 `Widget*`。
    - **鼠标派发必须携带 `FocusManager`**：`request_focus()` 读派发期「当前焦点管理器」（`current_focus_manager()`），若鼠标派发未传
      `fm`（`dispatch(root, me)` 缺省 `nullptr`），点击 `Text`/`TextInput` 的获焦请求会静默 no-op，焦点始终为空，后续键盘事件（
      `Ctrl+C`/`Ctrl+A` 等）永远到不了控件。宿主（如 `run_demo`）应以 `dispatch(root, me, &fm)` 派发鼠标事件，与键盘/文本输入派发共用同一
      `FocusManager`。
- **`Text` 宽度语义（`soft_wrap`，默认 `true`）**：约束有界且 `soft_wrap=true` 时，`Text`
  **仅当文本固有（单行）宽度超出约束、确需换行**才填满整行宽度；短文本按内容（固有）宽度上报。这样相邻 `Text`（如 `Row`
  内联标签）彼此按内容并排、命中盒不重叠、各自可选中；长段落仍正常换行填满。需要整行撑满的对齐场景（居中/右对齐短文本）可将
  `Text` 包入指定宽度的 `Container`/`SizedBox` 或显式 `.width()`。`m_display_text` 在 `on_layout` 即缓存，未绘制时拖选也可用。
