# ARCHITECTURE_WIDGET

> 本文件由 [`ARCHITECTURE.md`](../ARCHITECTURE.md) 划分而出（组件树与组合模型 / 事件与命中测试 / 渲染与布局 / 序列化与元信息）。章节编号保持原样。
> 返回主线见 [`ARCHITECTURE.md`](../ARCHITECTURE.md)。

**本文包含章节：**

- [6. 组件树与组合模型（Widget Tree & Composition）](#6-组件树与组合模型widget-tree--composition)
- [7. 事件与命中测试（Event & Hit-testing）](#7-事件与命中测试event--hit-testing)
- [8. 渲染与布局（Render & Layout）](#8-渲染与布局render--layout)
- [9. 序列化与元信息（Serialization & Meta）](#9-序列化与元信息serialization--meta)

## 6. 组件树与组合模型（Widget Tree & Composition）

- **不可变声明式树**：`Node` 持有 `Widget` 的共享引用；组件以声明式构造，状态变更驱动局部刷新。
- **组合优于继承**：布局/装饰通过 `Modifier`（如 `Padding`/`Background`）包裹节点，而非继承子类。
- **扁平组合（Flat composition）**：深层嵌套应尽量避免；库提供 `Column/Row/Stack/Grid/Scroll`
  等容器与 `Padding/Background/...` 修饰来扁平表达。
- **控制流原语**：`Show`（条件）、`Repeater`（数据驱动列表）、`Provider`（环境注入）以组件形式存在，
  而非语言级关键字。

---

## 7. 事件与命中测试（Event & Hit-testing）

- **命中测试链（hit-test chain）**：`Widget::hit_test_chain` 返回 根 → 最深 的节点路径。
- **冒泡（bubbling）**：`EventDispatcher::dispatch(MouseEvent&)` 自最深向根调用 `on_pointer_event`；
  某节点写 `e.handled = true` 即停止向上传递。
- **纯展示控件（如 `Text`）不置位** `wants_click()`，以便事件冒泡给父级 `Clickable`；
  可点击控件（如 `Button`）覆写 `wants_click()` 为 `on_click != nullptr`。
- **指针点击语义**：`Press` 置 `pressed`；`Release` 且 `pressed` 触发一次；点击与长按互斥
  （`m_click_pending` 在 Press 置位，Release 时若 `!long_press_fired()` 且未拖拽才触发 click）。
- **焦点（Focus）**：`FocusManager`（root + focused）按 `tabIndex` 移动；`Widget::requestFocus`
  定义放在 `focus.h`，读取派发期线程局部「当前焦点管理器」（`current_focus_manager()`），控件自身不持有 `FocusManager*`（挂载阶段不再递归注入）。
- **多点触控并发（按指针分发）**：`TouchDispatcher`（实例级，由 `Application` 持有 `m_touch`）对 `TouchEvent` 按 `pointer id` 做命中缓存与独立路由——某 `pointer id` 首按做命中测试并缓存链，活跃期复用缓存链（最深向根冒泡），抬起即清缓存；因此 `draggable`/`long_press`/`pinch`/`rotate` 各自绑定具体指针，支持单指持发 + 多指并发。每次 `TouchEvent` 同时 (1) 向缓存链广播完整 `TouchEvent`（原始流，供 `TouchListener` 修饰回调）(2) 合成带 `pointer_id` 的 `MouseEvent` 驱动既有点击/拖拽手势。全局静态捕获表已废弃（避免跨子树悬空 `Widget*`）。

---

## 8. 渲染与布局（Render & Layout）

- **渲染核心 `Painter`**：纯软件栅格（RGBA8 帧缓冲），不依赖 GPU；接口为纯函数式
  （给定节点 + 约束 → 确定像素），支持确定性快照比对。
- **布局引擎**：基于 `Constraints` 的 flex/grid 求解；`Flex` 同时承载 `main_axis`/`cross_axis`（对齐）与 `main_axis_size`（`Min`/`Max`）。
  - 默认 `MainAxisSize::Min`：容器主轴取"内容所需尺寸"，主轴对齐**仅当父约束强制容器更大时才可见自由空间**（与历史行为一致）。
  - `MainAxisSize::Max`：容器主轴**撑满父级可用主轴空间**，从而让 `main_axis` 对齐（`Center`/`End`/`Space*`）在内容不足时产生可见自由空间。无限主轴约束下 `Max` 退化为内容尺寸，不影响既有无界布局。
- **`Scroll` 离屏内容缓冲（滑窗）**：内容录进与滚动偏移无关的**内容坐标滑窗缓冲** `m_content`（`unique_ptr<Painter>`），尺寸 = 视口高 × (1 + 2×`overscan`)，仅覆盖可见区上下各 `overscan` 视口高的带而非整页；`m_buffer_origin_y` 标记该带在内容坐标系中的锚点。`ScrollProps::overscan`（默认 `1.0f`，共 3 屏厚）控制缓冲带厚度。滚动帧满足「内容仍有效 `m_content_valid` 且为纯滚动 `m_scrolling` 且未触发重锚点 `reanchor`」时，直接 `p.composite(*m_content, translate(0, m_buffer_origin_y - m_offset_y))` 一次 blit，不重新栅格；当视口逼近缓冲带两端（`reanchor`：`vp_top < m_buffer_origin_y + overscan_h` 或 `vp_bottom > m_buffer_origin_y + buffer_h - overscan_h`）时把 `m_buffer_origin_y` 重锚到 `clamp(m_offset_y - overscan_h, 0, max_origin)` 并重录当前缓冲带；非滚动帧（子动画 / banner 自滚动）按脏区**重录当前缓冲带**。重锚点几何与 composite 几何均用**逻辑 dp**（缓冲尺寸 = `m_viewport_h*(1+2*overscan)`，非 `m_content->height()` 的物理像素），避免单位 bug 导致缓冲错位。该设计使缓冲字节数 = 视口像素 × 4 × (1 + 2×`overscan`) 且**与内容总量无关**（内容 ×10 不增长），满足 WS-2 门槛 G-8；复杂度门槛 G-6/G-7 亦随之达标（滚动帧 `layout_nodes` ≡ 0，`dl_records` 正比滚动距离而非内容总量）。
- **圆角抗锯齿裁剪**：`Painter` 用 SDF coverage + 1px 羽化。
- **矢量描边原语（控件样式专项）**：`draw_line(a, b, width, color)`（点到线段距离 SDF，圆帽 + 1px 羽化，供勾号✓/斜线）、`fill_rounded_rect(r, radius, color)`（圆角裁剪 + 填充的便捷组合）、`draw_rounded_border(r, radius, thickness, color)`（圆角矩形 SDF 带状覆盖、向内描边；radius = 半径即圆环，RadioButton 外圈）；均接入 DisplayList 录制回放（`CmdKind::DrawLine`/`RoundedBorder`），回放与直绘逐位一致（`test_painter_primitives`）。
- **悬停态基础设施（控件样式专项）**：`EventDispatcher` 在无捕获 Move 时把新命中链与上次悬停链 diff，对离开/进入控件回调 `Widget::on_hover_change(bool)`（默认仅记录 `m_hover` 不标脏；需要视觉反馈的控件覆写并追加 `mark_needs_paint`）；`Widget::hovered()` 供 `on_paint` 读取。Win32 宿主经 `TrackMouseEvent(TME_LEAVE)` 在光标离窗时合成远离 Move 清除悬停（否则高亮残留）。
- **平台 Surface（可扩展后端边界）**：
  - `HeadlessSurface`：内存帧缓冲，可同步导出 PNG（测试/离线渲染）；由 feature 宏 `AURORA_BACKEND_HEADLESS`（默认 ON）控制编译。
  - `Win32Surface`：仅 `_WIN32` 且 `AURORA_BACKEND_WIN32` 定义（Windows 默认 ON）；`Painter` 帧缓冲经**常驻 BGRA DIB section + `BitBlt`** 贴窗（RGBA→BGRA CPU swizzle 后 blit；旧 `SetDIBitsToDevice`+RGBA 掩码会迫使 GDI 逐像素慢转，5760×3132px 实测 87ms → 现 ~8ms），支持 `set_present_dirty` 增量上屏（仅 swizzle+blit 脏矩形，拖选帧），零三方依赖。窗口宿主（创建/消息泵/事件翻译/DPI/同步重渲染/运行期标题 `SetWindowTextA`）抽取到共享 `Win32Window`（`window/win32_window.h`），`Win32Surface` 与 `D3D11Surface` 共用，仅 present 后端不同，避免复制消息泵的行为分歧；`WM_SIZE` 同步重渲染后 `ValidateRect` 免去紧跟 `WM_PAINT` 的重复全量上屏（最大化卡顿放大器）；`Surface::set_title` 虚方法经宿主 `Win32Window::set_title`（`SetWindowTextA`+`utf8_to_acp`）落地，`Window::set_title` 同步下发。宿主 `Win32Window` 采用 **pimpl 隔离**（与 X11/Wayland 一致）：`HWND`/`HDC`/DIB section/窗口过程等全部 Win32/GDI 细节收入 `src/aurora/window/win32_window.cpp` 的 `Impl`，公共头仅暴露 `std::unique_ptr<Impl>`，故 `hwnd()`/`background_brush()` 以 `void*` 返回（调用方 `static_cast<HWND>`/`static_cast<HBRUSH>`）。窗口过程 `Impl::wnd_proc`（静态成员）按**消息族**分发到 `handle_create`/`handle_mouse`/`handle_wheel`/`handle_key`/`handle_char`/`handle_size`/`handle_paint`/`handle_activate`/`handle_dpi_changed`/`handle_getminmaxinfo`/`handle_close`/`handle_destroy`/`handle_dropfiles`，取代原单体 52-case `switch`。
  - `D3D11Surface`：仅 `_WIN32` 且 `AURORA_BACKEND_D3D11` 定义（CMake `AURORA_BACKEND_D3D11=ON`，默认 OFF）；复用同一 `Win32Window` 宿主，把 `Painter` RGBA8 帧缓冲作为动态纹理，仅 `DirtyRegionTracker` 脏矩形（逻辑×scale→设备坐标）经 `UpdateSubresource` 增量上传，全屏三角形 + 像素着色器线性采样呈现（`Present(1,0)`），GPU 缩放避免拉伸模糊；首帧/整帧脏全量上传。`auto_detect_surface` 默认仍走 `Win32`（GDI），`D3D11` 须经 `create_window(D3D11Options)` 显式选择。无适配器时优雅跳过（测试 `test_d3d11_present`）。
  - `GlfwSurface`：OpenGL 3.3 兼容剖面（绘制采用 1.1 立即模式），`AURORA_BACKEND_GLFW`（由 CMake `AURORA_BACKEND_GLFW` 开关控制，默认 OFF）控制编译；**pimpl 隔离**（与 X11/Wayland/Win32 一致）：`GLFWwindow*`、GL 纹理与全部 GLFW 回调收入 `src/aurora/window/glfw_surface.cpp` 的 `Impl`，公共头零 `<GLFW/glfw3.h>`/`<GL/gl.h>` 依赖；GLFW 回调注册为 `Impl` 静态成员，窗口用户指针存 `Impl*` 供回调取回，`from_glfw_key`/`glfw_mods_to_aurora`/`utf8_from_codepoint` 为 `Impl` 静态纯函数。
  - `X11Surface`：仅 Linux 桌面且 `AURORA_BACKEND_X11` 定义（默认 OFF，`find_package(X11)` 链接 libX11）；pimpl 隔离（公共头零 Xlib 依赖，避免 `None`/`Bool`/`Status` 宏污染），`Painter` RGBA 帧缓冲按 Visual 掩码 CPU swizzle 到 X 原生像素序（常见 BGRX，与 Win32 RGBA→BGRA 等价）经 `XPutImage` 上屏，支持 `set_present_dirty` 增量上屏；事件翻译（`ButtonPress`/`MotionNotify`→`MouseEvent`、`Button4/5/6/7`→`ScrollEvent`、`KeyPress`→`KeyEvent`、`Xutf8LookupString`(XIM)→`TextInputEvent`、`ClientMessage(WM_DELETE_WINDOW)`→`should_close`、`FocusIn/Out`+`Map/Unmap`→`WindowState`、`PropertyNotify(_NET_WM_STATE)`→`WindowMode`）；样式经 EWMH/`_MOTIF_WM_HINTS`/`XSizeHints` 映射；`wait_events` 经 `poll(2)` 阻塞在 X 连接 fd + 自唤醒管道；`scale_factor` 解析 `Xft.dpi`。Wayland 会话下经 XWayland 无缝显示。
  - `WaylandSurface`：仅 Linux 桌面且 `AURORA_BACKEND_WAYLAND` 定义（默认 OFF，`pkg-config` 检测 `wayland-client`/`xkbcommon`，`wayland-scanner` 生成 `xdg-shell`/`xdg-decoration` C 胶水入 `build/wayland-gen/`）；pimpl 隔离，`Painter` 帧缓冲 CPU swizzle 到 `WL_SHM_FORMAT_XRGB8888` 经 `wl_shm` 共享内存双缓冲槽（`attach`+`damage_buffer`+`commit`，槽 busy 时 `roundtrip` 等 `release`）上屏；窗口壳 `wl_surface`+`xdg_surface`+`xdg_toplevel`（`configure` 驱动尺寸/几何态，`close`→`should_close`）；`wl_pointer`→`MouseEvent`/`ScrollEvent`、`wl_keyboard` 经 `xkbcommon` keymap→`KeyEvent`+`TextInputEvent`；`wait_events` 经 `poll(2)`（`prepare_read`/`read_events` 单线程范式）+ 自唤醒管道；`scale_factor` 取 `wl_output.scale`。服务端装饰经 `zxdg_decoration_manager_v1` 协商（KDE 有；**GNOME 不实现 → 无服务端标题栏**，属合成器限制）。
    - **装饰策略（`DecorationPolicy`，跨后端声明于 `window/surface.h`，置于 `WindowStyleOptions::decoration`）**：解决 GNOME 无 `xdg-decoration` 时「无标题栏、窗口不可操作」问题，使无标题栏也能移动/缩放/关闭。构造期依 `deco_mgr` 是否可用 + 策略解析为三标志位——`csd_title`（自绘标题栏：移动 + 关闭/最大化/最小化按钮 + 双击最大化）、`csd_border`（可缩放边框）、`mod_move`（无标题栏时 `Super`/`Alt`+拖拽移动）——组合覆盖全部五策略：`Auto`/`ServerSide` 在 compositor 不支持时回退 `csd_title+csd_border`；`ClientSide` 强制 `csd_title`（无边框）；`Borderless` 取 `csd_border+mod_move`；`Frameless` 全 false（应用自绘 + 程序化控制）。`Surface::content_inset()` 暴露 CSD 占用区（`EdgeInsets{ left/right/bottom = border, top = title_bar_h }`），由 `MediaQuery::from_surface` 并入 `padding` 安全区（对齐 Flutter `SafeArea`），子树据其避开装饰；程序化控制 `close/minimize/toggle_maximize/set_fullscreen`（`Surface`/`Window` 虚函数，Wayland 经 `xdg_toplevel` 生效）使无装饰窗口亦可由应用按钮驱动状态。
    - **标题栏重构（0.5.1→）**：CSD 绘制与命中统一消费 `title_bar_geometry()` 纯函数（三布局 Adwaita/Windows/Mac、样式 `TitleBarStyle` 经 `WindowStyleOptions.title_bar` 携带并可运行期热更）；`ptr_motion` 悬停跟踪经 `Impl::request_repaint()` 触发增量重绘；右键标题栏走 `xdg_toplevel_show_window_menu` 协议弹系统菜单；全屏默认隐藏 + 顶边 6px 揭示状态机；`ptr_button` 缓存 `last_press_serial` 供 `begin_window_move`/`resize` 在事件派发栈内同步调用（Win32 对应 `HTCAPTION` 伪装 NC 拖拽）。控件侧经 `present_root` 注入根 env 的 `WindowChrome` 服务驱动同一组 `Surface` 虚函数，实现「内置栏」与「`TitleBar` 控件」共享平台桥接。
  - **自定义 backend**：任意 `Surface` 子类经 `Application(Scene, unique_ptr<Surface>)` / `App().surface(...)` 注入，无需为每种后端在 `Application` 上加构造重载；`Surface` 之外的扩展点收口在 `create_window` 工厂（随 backend 增长）。
  - **编译/链接期代码剪裁**：关闭某 `AURORA_BACKEND_*` 后，对应 `Surface` 子类、工厂重载与重型平台头（`<windows.h>`/GLFW/OpenGL）被预处理器剔除，链接产物不再含该后端；自定义 `Surface` 注入路径不受任何内置后端是否编译的影响，故「只用自定义 backend」可不编译任何内置后端。**pimpl 补强**：全部真实后端（`Win32Window`/`GlfwSurface`/`X11Surface`/`WaylandSurface`）的公共头已收敛为 pimpl 句柄，即便后端**开启**，消费者 TU 也不会被拉入 `<windows.h>`/GLFW/OpenGL/Xlib/wayland-client 等重型头（连带避免 `min`/`max`、`None`/`Bool`/`Status` 等宏污染），编译期成本与命名空间洁净度不随后端数量增长。CMake 开关：`AURORA_BACKEND_HEADLESS` / `AURORA_BACKEND_WIN32` / `AURORA_BACKEND_GLFW`（feature 宏同名）。全部构建开关（`AURORA_BUILD_*` / `AURORA_BACKEND_*` / `AURORA_ENABLE_*`）、全局编译宏与运行期环境变量见 `codespec/BUILD_OPTIONS.md`。
- **高 DPI（device pixel ratio）**：所有 widget / 布局坐标均为**逻辑 dp**（96 DPI 基准 = 1 dp ≈ 1 px@96）。
  `Surface::scale_factor()` 返回 `dpi/96`；`Win32Surface` 启用 **Per-Monitor DPI 感知**（V2 → V1 → `SetProcessDPIAware` 运行时回退），
  按物理像素创建窗口与软件帧缓冲，`Painter::set_scale(scale)` 把 dp 几何 ×scale 放大到物理像素，1:1 贴窗避免发虚。
  - **调用顺序不变量**：`SetProcessDpiAwarenessContext` 在「进程已有任何窗口」时失败。因此 `aurora::enable_dpi_awareness()`（`window/window.h`）必须在 `init_console()`（`AllocConsole` 创建控制台窗口）与 `create_window()` **之前**调用；`run_demo`（`../examples/demos/demo_common.h` 统一启动器）已遵守。否则进程退化为 DPI 未感知，窗口以系统 DPI 虚拟化缩放（scale=1.0），高分屏下界面发虚。
  字体测量（`measure_width/height`、`caret_x`、`hit_test_char`）在逻辑 dp 空间进行（96 DPI），
  而光栅化（`FontEngine::draw_text`，**FreeType** 内核 + **HarfBuzz** 文本 shaping）按真实屏幕 DPI 生成物理分辨率字形，逐物理像素写入帧缓冲 —— 二者解耦，高 DPI 下文字清晰。默认字体为内置 **Noto Sans**（经 `third_party/` 源码构建编入的 FreeType 在引擎首次使用时注册，文本 shaping 由 HarfBuzz `hb_shape` 完成），跨机确定；缺字按系统字体回退链解析。
  事件坐标（鼠标 / 滚轮）在 Surface 内由物理像素 `/scale` 还原为 dp，与布局坐标一致。

---

## 9. 序列化与元信息（Serialization & Meta）

- **树 ⇄ JSON**：`serialization::to_json / from_json / diff / diff_into / apply_patch`，
  结合 `WidgetRegistry`（工厂注册）。`from_json` 流程：`make` → `deserialize_props` → `adopt_children`。
- **树 → YAML**：`serialization::to_yaml(const Widget&)` / `to_yaml(const Json&)`，将 widget 树序列化为 YAML 格式字符串（仅输出方向，无 `from_yaml`）。内部经 `yaml.h` 的递归下降 YAML 发射器（`detail::yaml_emit`）将 JSON 转为 YAML，与 `to_code` 平行作为输出格式适配层。
- **树 ⇄ 源码**：`serialization::to_code` 反向生成等效构造代码。
- **API 描述**：`gen_api_tools` 输出 `aurora_api.json`（全部 widget 类型、属性键、核心枚举），
  供 LSP / 文档生成 / 设计工具消费（见 `CODING_STANDARDS.md`「AI 友好性」4.1）。
- **不可重建控件**：`Repeater`/`Canvas` 的工厂故意返回「not-restorable」错误；
  含它们的树 `from_json` 应作预期提示而非硬失败。

---

