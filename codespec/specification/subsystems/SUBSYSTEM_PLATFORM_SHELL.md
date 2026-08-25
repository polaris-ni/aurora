# H.11 平台 Shell + H.11b 文字选中

> **编号说明**：本文件仅覆盖 **H.11 平台 Shell**；文字选中（**H.11b**）实际定义见 `SUBSYSTEM_APP_WINDOW.md` 的「#H.11b 文字选中」节。

> 本文件是「三、特性详细规范」子文档，覆盖 **§H.11**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #H.11 平台 Shell（文件对话框 / 系统托盘 / 剪贴板 / 文件拖放 / 多显示器）

核心目标：桌面系统集成的最小能力集——文件选择、通知区域图标、剪贴板（文本+图像）、操作系统文件拖放与多显示器枚举（对标 Qt
`QFileDialog` / `QSystemTrayIcon` / `QClipboard` / 拖放事件 / `QScreen`，以及 Flutter `Clipboard` / `Drop` / `Screen`
）。应用层经这些 API 即可获得原生桌面能力，不感知平台差异；跨平台回退（非 `_WIN32` / Headless）均保证不崩溃、且可被 `tests/`断言。

- **`file_dialog`（`app/file_dialog.h`，真实实现 `src/aurora/app/file_dialog_win32.cpp`）**：原生文件对话框（COM 驱动）。
    - `open_file(Options) -> Result<std::vector<std::string>>`：多选打开（`IFileOpenDialog` + `FOS_ALLOWMULTISELECT`）。
    - `save_file(Options) -> Result<std::string>`：保存（`IFileSaveDialog` + `FOS_OVERWRITEPROMPT`）。
    - `open_folder(Options) -> Result<std::string>`：文件夹选择器（`IFileOpenDialog` + `FOS_PICKFOLDERS`）。
    - `Options{ title, initial_dir, filters }`（`Filter{ name, extensions }`）；`initial_dir` 预选目录因本机 MinGW 工具链缺
      `SHCreateItemFromParsingName` 暂未接线（字段保留、向后兼容）。
    - headless 钩子 `headless_open_result` / `headless_save_result` / `headless_folder_result`：置非空即直接返回，供
      `tests/` 断言；`interactive` 开关（`true` 弹真实对话框；`false` 在自动化/headless 环境直接返回空，避免 CTest 卡 GUI）。非
      `_WIN32` 为内联回退（恒返回 headless 钩子值），真实 COM 实现仅在 `_WIN32` 编译。
- **`SystemTray`（`app/system_tray.h`，真实实现 `src/aurora/app/system_tray_win32.cpp`）**：通知区域图标。RAII——构造即尝试添加图标，析构移除。
    - `set_title(string)` / `set_icon(path)`（从文件加载图标，空路径用默认应用图标）/ `show_balloon(title, msg)`（气泡通知）/
      `show()` / `hide()` / `on_activate(cb)`（左键/气泡点击触发）。
    - `last_balloon_message()`：最近一次 `show_balloon` 正文（headless 下亦可用，供测试）。
    - 真实 `_WIN32` 实现经 `Shell_NotifyIcon` + 隐藏消息窗口（`HWND_MESSAGE`）接收激活/气泡回调，并在资源管理器重启（
      `TaskbarCreated`）后自动重新添加图标；非 `_WIN32` / Headless 下所有方法为 no-op（仅记录 `last_balloon_message`）。
- **`Clipboard` 图像能力（`app/clipboard.h`，真实实现 `src/aurora/app/clipboard.cpp`）**：在既有文本能力之外新增图像读写。
    - `set_image(const Image&) -> void` / `get_image() -> Image`：Win32 经 `SetClipboardData(CF_DIB)`（RGBA8 → BGRA
      DIB，自顶向下、32bpp），读取解析 `BITMAPINFOHEADER` 支持 32/24bpp（BGRA/BGR → RGBA，处理自底向上翻转）；空图像 `set_image`
      早退（不清除已有内容），无图像 `get_image` 返回空 `Image`（`width==0`）。
    - Headless / 非 `_WIN32`：`set_image` 为 no-op，`get_image` 返回空图像。测试 `tests/test_clipboard.cpp` 在「系统剪贴板可用」时做
      RGBA 往返断言，不可用时（沙箱/被占用）自动跳过往返、仅覆盖 no-op 安全路径。
- **文件拖放（`event/event.h` + `window/win32_window.cpp` + `event/dispatcher.h`）**：操作系统文件拖放事件端到端。
    - `FileDropEvent : Event { Point position; std::vector<std::string> paths; }`：窗口级事件，落点为窗口逻辑坐标。
    - `Widget::on_file_drop(FileDropEvent&)`：控件级虚入口（默认 no-op），消费时置 `e.handled`。
    - `EventDispatcher::dispatch(root, FileDropEvent&)`：在落点 `hit_test` 命中目标并调用其 `on_file_drop`（与滚轮事件同族）。
    - `Application::dispatch_file_drop(paths, x, y)`：便捷入口，构造 `FileDropEvent` 经主派发路径触发 `on_file_drop`。
    - Win32 真实来源：`Win32Window` 构造时 `DragAcceptFiles(hwnd, TRUE)`，并在 `wnd_proc` 处理 `WM_DROPFILES`（
      `DragQueryFileW`/`DragQueryPoint`/`ScreenToClient`/`DragFinish` 解析为 UTF-8 路径列表，落点按 `m_scale` 换算为逻辑坐标）后经
      `m_handler` 派发；交互为 `@manual`（需真实窗口与拖放，headless 经合成事件/注入断言，见 `tests/test_file_drop.cpp`）。注：
      `IDropTarget` OLE 拖放曾评估，在本机无头沙箱中因 COM 注册/反注册（`RegisterDragDrop`/`RevokeDragDrop`）不稳定导致
      teardown 确定性崩溃，已暂缓，保留稳定的 `WM_DROPFILES` 路径。
- **多显示器枚举与管理（`app/display.h`，真实实现 `src/aurora/app/display_win32.cpp`）**：对标 Flutter `Screen` / Qt
  `QScreen` + `QWidget::setScreen`。
    - `Display{ id, name, bounds, work_area, scale_factor, is_primary }`（`bounds`/`work_area` 为 **物理像素** `Rect`，与
      Windows 虚拟屏幕坐标系一致；`scale_factor = dpi/96` 单独给出，换算逻辑坐标用 `bounds / scale_factor`）。
    - `app::list_displays() -> std::vector<Display>` / `app::primary_display() -> Display`：Win32 经
      `EnumDisplayMonitors` + `GetMonitorInfoW`（含 `MONITORINFOF_PRIMARY` 主屏判定、排除任务栏的 `rcWork` 工作区、
      `GetDeviceCaps(LOGPIXELSX)` 的 per-monitor DPI）；非 `_WIN32` / Headless 回退为单默认屏（1920×1080, scale 1,
      primary）。
    - `app::display_containing(Point) -> Display`：返回包含该点（物理像素）的显示器，无命中回退 `primary_display()`。
    - `app::move_window_to_display(Window&, int display_id) -> void`：把窗口居中迁移到目标显示器工作区（
      `MonitorFromPoint` + `GetMonitorInfo` 取工作区，`GetWindowRect` 量当前尺寸，`SetWindowPos(SWP_NOSIZE|SWP_NOZORDER)`
      居中）；未知 id 回退主屏；`Window` 无真实句柄（Headless/无头）时 no-op 不崩溃。依赖 `Surface::native_handle()` 取`HWND`。
    - 测试 `tests/test_display.cpp`：断言至少一块显示器、恰一主屏、工作区不超出整屏；并断言 `display_containing`
      屏内落点命中、屏外回退主屏，以及 `move_window_to_display`（含未知 id）在无头下 no-op 不崩溃。
- **`Surface::native_handle()`（`window/surface.h`，真实实现 `win32_surface.h`）**：返回后端原生窗口句柄（Win32 下为 `HWND`，经
  `Win32Window::hwnd()`）；`Surface` 基类默认返回 `nullptr`，供 `move_window_to_display` 等需句柄的 API 取用，跨平台安全（无头/无句柄返回
  `nullptr`）。声明为 `const`（只读查询，不修改 Surface 状态），Win32/X11/Wayland 覆写同步为 `const`（GLFW/D3D11/Wasm 沿用基类
  `nullptr`）。
- **`Win32Window` 句柄访问器的类型契约（0.4.0 起，原 1.1.2）**：`Win32Window` 已 pimpl 化，公共头不再包含 `<windows.h>`，因此
  `Win32Window::hwnd() const -> void*` 与静态 `Win32Window::background_brush() -> void*` **以 `void*` 返回**（原为`HWND`/
  `HBRUSH`）。 调用方在自身已包含 `<windows.h>` 的 TU 内 `static_cast<HWND>(win->hwnd())` /
  `static_cast<HBRUSH>(Win32Window::background_brush())` 还原；
  `Win32Surface::hwnd()`/`background_brush()` 同步为 `void*` 透传。语义不变（未创建窗口时为 `nullptr`），仅静态类型放宽以斩断公共头对
  Win32 SDK 的依赖。
