# BUILD_OPTIONS_BACKEND

> 本文件由 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) 划分而出（AURORA_BACKEND_* 后端开关（= feature 宏））。
> 返回主线见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)。

**本文包含章节：**

- [2. `AURORA_BACKEND_*` —— 后端开关 = feature 宏](#2-aurora_backend_--后端开关--feature-宏)

## 2. `AURORA_BACKEND_*` —— 后端开关 = feature 宏

每个内置 `Surface` 后端一个开关。 **开关名与 PUBLIC 编译宏名完全相同**，宏以 `target_compile_definitions(aurora PUBLIC …)`
传播给所有消费者；库代码用 `#ifdef AURORA_BACKEND_XXX` 做代码剪裁。关闭某后端后，对应 `Surface` 子类、工厂重载与重型平台头（
`<windows.h>`/GLFW/OpenGL）被预处理器剔除，链接产物不再含该后端。自定义 `Surface` 注入路径始终可用，故「只用自定义
backend」可不编译任何内置后端。

| 选项                      | 默认值                   | 含义                                                                                                                                                                                                                                                                                                                                           | 传播宏                    | 额外链接                                                               |
|---------------------------|--------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------|------------------------------------------------------------------------|
| `AURORA_BACKEND_HEADLESS` | `ON`                     | 无头内存/PNG 后端（`HeadlessSurface`，离线渲染/测试）                                                                                                                                                                                                                                                                                          | `AURORA_BACKEND_HEADLESS` | —                                                                      |
| `AURORA_BACKEND_WIN32`    | Windows `ON`，否则 `OFF` | Win32/GDI 后端（`Win32Surface` + `Win32Window` 共享宿主）                                                                                                                                                                                                                                                                                      | `AURORA_BACKEND_WIN32`    | `user32` `gdi32`（仅 `_WIN32`）                                        |
| `AURORA_BACKEND_D3D11`    | `OFF`                    | D3D11 GPU 增量上屏后端（`D3D11Surface`）                                                                                                                                                                                                                                                                                                       | `AURORA_BACKEND_D3D11`    | `d3d11` `dxgi` `d3dcompiler`（仅 `_WIN32`）                            |
| `AURORA_BACKEND_GLFW`     | `OFF`                    | GLFW + OpenGL（上下文 3.3 兼容剖面，绘制 1.1 立即模式）后端（`GlfwSurface`）；GLFW 经仓库内置 `third_party/glfw`（3.5.1）源码构建，无伴随缓存变量                                                                                                                                                                                              | `AURORA_BACKEND_GLFW`     | `glfw` 目标（源码静态库）+ `opengl32`                                  |
| `AURORA_BACKEND_X11`      | `OFF`                    | X11/Linux 桌面后端（`X11Surface`，pimpl + `src/aurora/window/x11_surface.cpp` 完整实现：软件 `Painter` 帧缓冲经 Visual 掩码 swizzle + `XPutImage` 上屏，Xlib 事件翻译，XIM 文本输入，EWMH 窗口状态；`find_package(X11)` 链接 libX11，非 Linux 开启 FATAL）                                                                                     | `AURORA_BACKEND_X11`      | `${X11_LIBRARIES}`（`find_package(X11)`）                              |
| `AURORA_BACKEND_WAYLAND`  | `OFF`                    | 原生 Wayland/Linux 桌面后端（`WaylandSurface`，pimpl + `src/aurora/window/wayland_surface.cpp` 完整实现：`wl_shm` 双缓冲 + `xdg-shell` 窗口壳 + `xkbcommon` 键盘，软件 `Painter` swizzle 到 `XRGB8888` 上屏；`pkg-config` 检测 `wayland-client`/`xkbcommon`，`wayland-scanner` 生成 `xdg-shell`/`xdg-decoration` C 胶水，非 Linux 开启 FATAL） | `AURORA_BACKEND_WAYLAND`  | `${WAYLAND_CLIENT_LIBRARIES}` `${XKBCOMMON_LIBRARIES}`（`pkg-config`） |
| `AURORA_BACKEND_MACOS`    | `OFF`                    | macOS 后端（`MacOSSurface`，Objective-C++/Cocoa 实现待补全；`enable_language(OBJCXX)`，非 Apple 开启 FATAL）                                                                                                                                                                                                                                   | `AURORA_BACKEND_MACOS`    | `Cocoa` `AppKit`（框架）                                               |
| `AURORA_BACKEND_WASM`     | `OFF`                    | WebAssembly 后端（`WasmSurface`，`<canvas>` 像素写回已实现（`wasm_surface.h` EM_ASM `putImageData` 胶水），Emscripten 构建/链接验证待补全；须 `emcmake cmake`，非 Emscripten 开启 FATAL）                                                                                                                                                                                                                                     | `AURORA_BACKEND_WASM`     | Emscripten 工具链                                                      |

### 2.1 伴随缓存变量

GLFW 后端 **无伴随缓存变量**：依赖仓库内置 `third_party/glfw` 源码构建（与 FreeType/HarfBuzz 同口径——源码进仓库、
断网可构建、版本确定），不存在外部安装根定位。

开启 GLFW 后端示例：

```powershell
cmake -S . -B build -DAURORA_BACKEND_GLFW=ON
```

源码构建细节：关 examples/tests/docs/install、`EXCLUDE_FROM_ALL`（仅 aurora 链接时连带构建）、静态链接无 DLL 依赖。
仓库缺 `third_party/glfw` 源码时配置期直接 `FATAL_ERROR`（不回退外部二进制，避免发行版路径漂移）。

### 2.2 Linux 桌面后端（X11 / 原生 Wayland）

两个 Linux 桌面后端均为纯软件 `Painter` 上屏（无 GPU），可单开或 **同时开启**；同时开启时运行期按会话 类型自动择优：
`WAYLAND_DISPLAY` 存在 → 优先原生 `WaylandSurface`，否则 `X11Surface`（Wayland 会话下经 XWayland）；真实显示不可用时
`create_native_window` 回退 `HeadlessSurface`（详见 `SUBSYSTEM_APP_WINDOW.md`
的 `auto_detect_surface` / `create_native_window` 契约）。

开发依赖（ **仅编译期**，运行库桌面环境通常已自带）：

| 后端    | Fedora/RHEL                                                            | Debian/Ubuntu                                                   |
|---------|------------------------------------------------------------------------|-----------------------------------------------------------------|
| X11     | `dnf install libX11-devel`                                             | `apt install libx11-dev`                                        |
| Wayland | `dnf install wayland-devel wayland-protocols-devel libxkbcommon-devel` | `apt install libwayland-dev wayland-protocols libxkbcommon-dev` |

构建示例（同时开启，最常见的 Linux 桌面配置）：

```bash
cmake -S . -B build -DAURORA_BACKEND_X11=ON -DAURORA_BACKEND_WAYLAND=ON
cmake --build build -j $(nproc)
```

要点：

- Wayland 后端在配置期用 `pkg-config` 检测 `wayland-client`/`xkbcommon`，并调用 `wayland-scanner` 把
  `xdg-shell.xml` / `xdg-decoration-unstable-v1.xml` 生成为 C 胶水（落在 `build/wayland-gen/`，不入仓、不纳入版本管理）。
- 服务端窗口装饰经 `zxdg_decoration_manager_v1` 协商：KDE 等支持方绘制标题栏； **GNOME 不实现该协议 →
  窗口无服务端标题栏**（`AURORA_LOG_INFO` 提示），属合成器限制而非缺陷。
- 非 Linux（含 Apple）平台开启任一后端将触发 `FATAL_ERROR`。

> **`aurora_api.json` 的生成（三段 merge-only，互不截断）**：`aurora_api.json`（API 描述数据，供 `Inspector` / `codegen` 消费）由三个独立生成器各写各自段、读现有文件保留其它段，可任意顺序运行：
> - `gen_error_codes`（`tools/gen_error_codes.cpp`，源 `codespec/errors.toml`）→ `error_codes` 段；
> - `gen_api_tools`（`tools/gen_api.cpp`，接入 `AuroraTools.cmake`）→ `widgets` / `enums` / `layout_rules` / `state_patterns` 段，并 merge 现有 `error_codes` + `debug`；
> - `gen_debug_api`（`tools/gen_debug_api.cpp`，源 `codespec/debug_api.toml`）→ `debug` 段（仅声明 `aurora::debug` 公共自由函数）。
>
> 重建（推荐自定义目标，内部直接写文件）：
> ```powershell
> cmake --build build --target aurora_api_json        # gen_api_tools 直写 aurora_api.json
> cmake --build build --target gen_debug_api_json     # 仅刷新 debug 段
> # error_codes 段由 gen_error_codes 在 errors.toml 变更时重跑
> ```
> 新增/删除 widget 或类型后，须重新生成 `aurora_api_json` 以使 `aurora_api.json` 与 `register_core_widgets()` 注册表保持一致（详见 `ARCHITECTURE_WIDGET.md` 的组件发现 / 注册约定）。

### 2.3 架构级渲染/布局优化开关

引入三项互不依赖的架构级渲染/布局优化。三者均 **开关名 = PUBLIC feature 宏名**，宏以
`target_compile_definitions(aurora PUBLIC …)` 传播给所有消费者；库代码用 `#ifdef AURORA_XXX` 做代码剪裁。
关闭任一开关即回退到原始实现路径（等价无优化），可独立退化。

| 选项                       | 默认值 | 含义                                                                | 传播宏                     | 退化行为                           |
|----------------------------|--------|---------------------------------------------------------------------|----------------------------|------------------------------------|
| `AURORA_LAYOUT_CACHE`      | `ON`   | 布局约束缓存：约束不变且非 layout dirty 时跳过子树 layout 递归      | `AURORA_LAYOUT_CACHE`      | 每次 `layout()` 都重新计算整棵子树 |
| `AURORA_OCCLUSION_CULLING` | `ON`   | 遮挡剔除：跳过不与 Painter 裁剪区相交的子控件绘制                   | `AURORA_OCCLUSION_CULLING` | 始终遍历并绘制全部子控件           |
| `AURORA_DISPLAY_LIST`      | `ON`   | Display List 录制/回放：子树未脏时直接 replay 命令，跳过 paint 遍历 | `AURORA_DISPLAY_LIST`      | 每次 `paint()` 都重新遍历整棵子树  |

### 2.3.1 Display List 集成约束（正确性不变量）

- **绘制副作用 / 每帧变动内容控件必须退出 DL 缓存**：`Widget::can_cache_display_list()` 默认 `true`； 绘制阶段产生副作用（如
  `Hero` 向 `HeroRegistry` 上报几何）或内容每帧变化（如 `TransitionLayer`/`NavigatorHost`
  按 `progress` 合成淡变）的控件 **必须** 覆盖为 `false`，否则缓存回放会跳过必要的每帧 `on_paint`，导致注册丢失 /
  转场冻结。命中不可缓存控件时，其祖先录制会被标记 `mark_recording_dynamic()`，保证易变输出不被上游缓存。
- **外部裁剪不参与控件 DL**：`present_root` 的脏区裁剪 `push_clip` 不录入控件 DL；故 `Widget::paint` 在
  `Painter::has_clip()` 为真时直接重录但 **不缓存**，避免无裁剪帧回放越界绘制（见 `tests/test_dirty_clip_paint.cpp`）。
- **布局变更同步失效 DL**：`Widget::mark_needs_layout()` 一并调用 `invalidate_display_list_up()`，保证重排后的几何 / 内容
  不被旧 `bounds` 录制的 DL 回放。
- **`m_layout_parent` 悬垂安全**：`Node` 析构时将其持有的子控件 `m_layout_parent` 置空，使树重建（父容器销毁而子控件经
  共享所有权存活）时 `mark_needs_layout()` / `invalidate_display_list_up()` 不会解引用已释放的父指针。稳态帧不销毁
  `Node`，故存活树的部分失效传播不受影响。

三者默认全开；若需排查回归，可单独关闭定位：

```powershell
cmake -S . -B build -DAURORA_LAYOUT_CACHE=OFF -DAURORA_DISPLAY_LIST=OFF
```

### 2.4 CPU 性能专项：事件驱动帧循环运行时选项（非编译开关）

CPU 性能专项把 `Application::run` 从「忙轮询」改造为「事件驱动 + 帧节流」：静态界面下帧循环在
`Surface::wait_events` 阻塞等待事件/超时（CPU 趋近 0），活跃帧按帧预算节流。以下为 `WindowOptions`
运行时字段（ **非** CMake 开关，随窗口选项传入）：

| 字段           | 类型                 | 默认值 | 含义                                                                                                                    |
|----------------|----------------------|--------|-------------------------------------------------------------------------------------------------------------------------|
| `max_fps`      | `int`                | `60`   | 活跃帧（有脏区/动画）帧率上限；`0` = 不限帧率（等价旧忙轮询节奏）。同步为 `FrameStats` 帧预算。                         |
| `power_saving` | `bool`               | `true` | idle 时阻塞等待事件（省电）；`false` = 忙轮询旧行为，供持续重绘场景 opt-out（与 `enable_dirty_tracking(false)` 配套）。 |
| `renderer`     | `RendererPreference` | `Auto` | 上屏后端偏好，见下表。                                                                                                  |

**`renderer`（`RendererPreference`）与 `AURORA_BACKEND_D3D11` 编译开关的关系**（仅 Win32 工厂 `create_window(Win32Options)`
生效）：

| `renderer` \ 编译 | `AURORA_BACKEND_D3D11=ON` 且设备可用 | D3D11 未编译 / 设备创建失败                                    |
|-------------------|--------------------------------------|----------------------------------------------------------------|
| `Auto`（默认）    | 选 D3D11 GPU 上屏                    | 静默回退 Win32/GDI（`AURORA_LOG_INFO` 说明）                   |
| `Software`        | 强制 Win32/GDI                       | Win32/GDI                                                      |
| `GpuD3D11`        | 选 D3D11（含 WARP 兜底）             | 返回 `renderer-unavailable` 错误（不静默降级，错误归属调用方） |

> D3D11 后端本阶段维持默认 OFF；`AURORA_BACKEND_D3D11=ON` 时 `D3D11Options.vsync`（默认 `true`）控制
> `Present(1,0)`（阻塞到 vblank，后端自带帧节拍，帧调度跳过 CPU sleep）或 `Present(0,0)`（交还 CPU 帧预算节流）。

---

