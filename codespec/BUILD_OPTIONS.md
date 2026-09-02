# 编译选项与宏定义统一参考

> 本文件是 Aurora 项目**所有**编译期可配置开关、缓存变量、传播宏与运行时环境变量的**唯一权威来源**。
> 凡涉及 CMake 构建选项、feature 宏、路径 / 插桩变量，一律以本文件为准；其余文档只做指针，不重复罗列，以避免文档漂移。

---

## 1 总览：三层命名分类法

所有构建选项按**语义**严格归入三组，组内前缀一致：

| 前缀 | 类别 | 语义 | 是否向库注入 feature 宏 |
|:---|:---|:---|:---|
| `AURORA_BUILD_*` | 构建产物开关 | 是否**构建**某个额外交付物（demos / tests / Inspector 服务器 / 图像编解码） | 否（例外：`AURORA_BUILD_IMAGE_*` 注入非 PUBLIC 的编译宏，见 §2.1） |
| `AURORA_BACKEND_*` | 内置后端开关 | 每个 `Surface` 后端一个开关；**开关名 = PUBLIC feature 宏名** | 是（`#ifdef` 剪裁 + PUBLIC 传播给消费者） |
| `AURORA_ENABLE_*` | 插桩 / 分析 / 能力开关 | 是否注入编译 / 链接期分析工具（覆盖率 / 内存检测 / 调试 / 性能插桩）或开启构建加速 / 内部能力（lld / ccache / SIMD / DEBUG） | 多数否；`PROFILING` / `TRACING` 注入 PUBLIC 宏，`SIMD` / `DEBUG` 注入内部宏，`LLD` / `CCACHE` 不注入宏 |

> `Win32/GDI` 后端仅在 `_WIN32` 下编译，无需额外开关，已由 `AURORA_BACKEND_WIN32` 的内置默认值覆盖。

### 1.1 CMake 脚本布局

顶层 `CMakeLists.txt` 只做「工程声明 + 核心库目标 + 模块编排」，具体逻辑按职责划分在 `cmake/` 下的模块（`include()` 不创建新作用域，各模块内 `option()` / 目标定义与写在顶层完全等价，开关名与默认值不变）：

| 模块 | 职责 |
|:---|:---|
| `cmake/AuroraThirdParty.cmake` | FreeType / HarfBuzz 源码构建 |
| `cmake/AuroraUtils.cmake` | 消费者目标统一配置辅助（`aurora_setup_consumer_target`，demo / 测试 / 工具复用链接 / PCH / C++20 / 告警） |
| `cmake/AuroraBackends.cmake` | 全部 `AURORA_BACKEND_*` 后端剪裁开关 + 架构级优化宏（`AURORA_LAYOUT_CACHE` 等） |
| `cmake/AuroraImageCodecs.cmake` | `AURORA_BUILD_IMAGE_JPEG` / `AURORA_BUILD_IMAGE_WEBP` / `AURORA_BUILD_IMAGE_PNG`（编译期能力开关） |
| `cmake/AuroraSimd.cmake` | `AURORA_ENABLE_SIMD`（光栅内核 SIMD 双实现，内部宏，不 PUBLIC 传播） |
| `cmake/AuroraCcache.cmake` | `AURORA_ENABLE_CCACHE`（ccache 编译缓存启动器） |
| `cmake/AuroraTools.cmake` | 工具 / 基准可执行（`aurora_add_tool()` 统一样板）+ `AURORA_BUILD_INSPECTOR_SERVER` |
| `cmake/AuroraDemos.cmake` | 示例 demo 定义块（须在 `AuroraTools` 与 `AuroraTests` 之后 include，因其依赖 `aurora_inspector_server` 目标） |
| `cmake/AuroraTests.cmake` | `AURORA_BUILD_TESTS` 注册式 runner（GLOB `tests/*.cpp` → 单一 `aurora_test_runner`，`AURORA_TEST()` 自注册） |
| `cmake/AuroraInstrumentation.cmake` | `AURORA_ENABLE_COVERAGE` / `AURORA_ENABLE_ASAN` / `AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING`（须在全部目标定义之后 include） |
| `cmake/AuroraInstall.cmake` | 安装 + `find_package(Aurora)` 导出（须在后端开关之后 include） |

---

## 2 `AURORA_BUILD_*`：构建产物开关

控制「是否编译某个额外交付物」。这些开关**不向库代码注入 feature 宏**，只决定目标是否被加入构建。

| 选项 | 默认值 | 含义 | 引入的目标 |
|:---|:---|:---|:---|
| `AURORA_BUILD_DEMOS` | `ON` | **定义**（非默认构建）`examples/demos/` 下每组件一个的可运行窗口 demo 目标；均 `EXCLUDE_FROM_ALL`，按需构建 | 各 `demo_<组件>` 可执行文件 + 聚合目标 `demos` |
| `AURORA_BUILD_TESTS` | `ON` | 编译 `tests/` 下全部用例并接入 CTest：`AURORA_TEST()` 注册、单一 runner 一次链接，逐条 `--run=<stem>` 隔离 | `aurora_test_runner` 可执行 + `enable_testing()` + `registry_integrity` 守护 |
| `AURORA_BUILD_INSPECTOR_SERVER` | `OFF` | 编译 Inspector 远程 HTTP 服务器（跨平台：Windows 链 `ws2_32` / POSIX 链 `pthread`） | `aurora_inspector_server` 静态库 |
| `AURORA_BUILD_IMAGE_JPEG` | `OFF` | 启用 JPEG 图像解码支持（关闭时需由消费者自行提供解码后的像素） | 仅改变编译期可用编解码能力，无独立目标 |
| `AURORA_BUILD_IMAGE_WEBP` | `OFF` | 启用 WebP 图像解码支持 | 同上 |
| `AURORA_BUILD_IMAGE_PNG` | `OFF` | 启用 PNG 图像**解码**支持（与 `HeadlessSurface` **输出** PNG 相互独立） | 同上 |

### 2.1 图像编解码开关

`AURORA_BUILD_IMAGE_JPEG` / `WEBP` / `PNG` 默认均 `OFF`，由 `cmake/AuroraImageCodecs.cmake` 定义并在 `CMakeLists.txt` 中 `include(AuroraImageCodecs)`，是真实生效的构建开关；关闭任一选项则该格式的解码路径不参与编译。

三者虽归入 `AURORA_BUILD_*` 组，但会注入**非 PUBLIC** 的 `AURORA_BUILD_IMAGE_*` 编译宏（codec 编译单元以 `#ifdef` 剪裁），属「编译期能力开关」而非「交付物开关」的例外——该宏不向消费者传播，关闭时仅损失解码能力、不改变像素输出。

### 2.2 demo 构建方式

demo 不进默认构建（`EXCLUDE_FROM_ALL`）：日常 `cmake --build build` 只建库 / 工具 / 测试；单个 demo 按名构建（`cmake --build build --target demo_lazy_list`），全部 demo 用聚合目标（`cmake --build build --target demos`）。关闭 `AURORA_BUILD_DEMOS` 则连目标都不定义。

### 2.3 预编译头（PCH）

- **库自身**：`include/aurora/aurora_pch.h` 收录标准库 + `nlohmann/json.hpp`（不含 aurora 自有头，保证库开发时命中率），`aurora` 库 PRIVATE 编译一份。
- **消费者**：`aurora_consumer_pch` 锚定目标把 `aurora.h` 伞头整体预编译一份，全部 demo / 测试 / 工具经 `target_precompile_headers(REUSE_FROM aurora_consumer_pch)` 复用。aurora 头变更本就触发消费者重编，不增加失效面。
- 覆盖率 / ASan 开启时 PCH 全部自动关闭。

### 2.4 `AURORA_BUILD_INSPECTOR_SERVER`

| 属性 | 值 |
|:---|:---|
| 类型 | `option()` |
| 默认值 | `OFF` |
| 说明 | 编译 Inspector 远程 HTTP 服务器（`InspectorServer`），暴露 REST 端点供外部工具远程访问运行时控件树 |
| 平台限制 | 跨平台（Windows: `ws2_32` / POSIX: `pthread`） |
| 产物 | `aurora_inspector_server` 静态库（`src/aurora/inspector/inspector_server.cpp`） |
| 头文件 | `include/aurora/inspector/inspector_server.h` |

`inspector_server.cpp` 已从核心 `aurora` 库源文件列表中排除（`list(FILTER ... EXCLUDE)`），仅当开关为 `ON` 时编入独立静态库，避免未开启时引入 Winsock2 依赖。

```powershell
cmake -S . -B build -DAURORA_BUILD_INSPECTOR_SERVER=ON
```

---

## 3 `AURORA_BACKEND_*`：后端开关（= feature 宏）

每个内置 `Surface` 后端一个开关。**开关名与 PUBLIC 编译宏名完全相同**，宏以 `target_compile_definitions(aurora PUBLIC …)` 传播给所有消费者；库代码用 `#ifdef AURORA_BACKEND_XXX` 做代码剪裁。关闭某后端后，对应 `Surface` 子类、工厂重载与重型平台头被预处理器剔除，链接产物不再含该后端。自定义 `Surface` 注入路径始终可用，故「只用自定义 backend」可不编译任何内置后端。

| 选项 | 默认值 | 含义 | 传播宏 | 额外链接 |
|:---|:---|:---|:---|:---|
| `AURORA_BACKEND_HEADLESS` | `ON` | 无头内存 / PNG 后端（`HeadlessSurface`，离线渲染 / 测试） | `AURORA_BACKEND_HEADLESS` | — |
| `AURORA_BACKEND_WIN32` | Windows `ON`，否则 `OFF` | Win32/GDI 后端（`Win32Surface` + `Win32Window` 共享宿主） | `AURORA_BACKEND_WIN32` | `user32` `gdi32`（仅 `_WIN32`） |
| `AURORA_BACKEND_D3D11` | `OFF` | D3D11 GPU 增量上屏后端（`D3D11Surface`） | `AURORA_BACKEND_D3D11` | `d3d11` `dxgi` `d3dcompiler`（仅 `_WIN32`） |
| `AURORA_BACKEND_GLFW` | `OFF` | GLFW + OpenGL（上下文 3.3 兼容剖面，绘制 1.1 立即模式） | `AURORA_BACKEND_GLFW` | `glfw` 目标（源码静态库）+ `opengl32` |
| `AURORA_BACKEND_X11` | `OFF` | X11 / Linux 桌面后端（`X11Surface`，pimpl 完整实现） | `AURORA_BACKEND_X11` | `${X11_LIBRARIES}`（`find_package(X11)`） |
| `AURORA_BACKEND_WAYLAND` | `OFF` | 原生 Wayland / Linux 桌面后端（`WaylandSurface`，pimpl 完整实现） | `AURORA_BACKEND_WAYLAND` | `${WAYLAND_CLIENT_LIBRARIES}` `${XKBCOMMON_LIBRARIES}`（`pkg-config`） |
| `AURORA_BACKEND_MACOS` | `OFF` | macOS 后端（`MacOSSurface`，`enable_language(OBJCXX)`，非 Apple 开启 FATAL） | `AURORA_BACKEND_MACOS` | `Cocoa` `AppKit`（框架） |
| `AURORA_BACKEND_WASM` | `OFF` | WebAssembly 后端（`WasmSurface`，须 `emcmake cmake`，非 Emscripten 开启 FATAL） | `AURORA_BACKEND_WASM` | Emscripten 工具链 |

### 3.1 GLFW 源码构建

GLFW 后端**无伴随缓存变量**：依赖仓库内置 `third_party/glfw` 源码构建（与 FreeType / HarfBuzz 同口径——源码进仓库、断网可构建、版本确定），不存在外部安装根定位。

```powershell
cmake -S . -B build -DAURORA_BACKEND_GLFW=ON
```

构建细节：关 examples / tests / docs / install、`EXCLUDE_FROM_ALL`（仅 aurora 链接时连带构建）、静态链接无 DLL 依赖。仓库缺 `third_party/glfw` 源码时配置期直接 `FATAL_ERROR`（不回退外部二进制，避免发行版路径漂移）。

### 3.2 Linux 桌面后端（X11 / 原生 Wayland）

两个 Linux 桌面后端均为纯软件 `Painter` 上屏（无 GPU），可单开或**同时开启**；同时开启时运行期按会话类型自动择优：`WAYLAND_DISPLAY` 存在 → 优先原生 `WaylandSurface`，否则 `X11Surface`（Wayland 会话下经 XWayland）；真实显示不可用时 `create_native_window` 回退 `HeadlessSurface`。

**开发依赖**（仅编译期，运行库桌面环境通常已自带）：

| 后端 | Fedora / RHEL | Debian / Ubuntu |
|:---|:---|:---|
| X11 | `dnf install libX11-devel` | `apt install libx11-dev` |
| Wayland | `dnf install wayland-devel wayland-protocols-devel libxkbcommon-devel` | `apt install libwayland-dev wayland-protocols libxkbcommon-dev` |

构建示例（同时开启，最常见的 Linux 桌面配置）：

```bash
cmake -S . -B build -DAURORA_BACKEND_X11=ON -DAURORA_BACKEND_WAYLAND=ON
cmake --build build -j $(nproc)
```

要点：

- Wayland 后端在配置期用 `pkg-config` 检测 `wayland-client` / `xkbcommon`，并调用 `wayland-scanner` 把 `xdg-shell.xml` / `xdg-decoration-unstable-v1.xml` 生成为 C 胶水（落在 `build/wayland-gen/`，不入仓）。
- 服务端窗口装饰经 `zxdg_decoration_manager_v1` 协商：KDE 等支持方绘制标题栏；**GNOME 不实现该协议 → 窗口无服务端标题栏**（`AURORA_LOG_INFO` 提示），属合成器限制而非缺陷。
- 非 Linux（含 Apple）平台开启任一后端将触发 `FATAL_ERROR`。

### 3.3 架构级渲染 / 布局优化开关

三项互不依赖的架构级优化。三者均**开关名 = PUBLIC feature 宏名**，宏以 `target_compile_definitions(aurora PUBLIC …)` 传播；库代码用 `#ifdef AURORA_XXX` 做代码剪裁。关闭任一开关即回退到原始实现路径（等价无优化），可独立退化。

| 选项 | 默认值 | 含义 | 传播宏 | 退化行为 |
|:---|:---|:---|:---|:---|
| `AURORA_LAYOUT_CACHE` | `ON` | 布局约束缓存：约束不变且非 layout dirty 时跳过子树 layout 递归 | `AURORA_LAYOUT_CACHE` | 每次 `layout()` 都重新计算整棵子树 |
| `AURORA_OCCLUSION_CULLING` | `ON` | 遮挡剔除：跳过不与 Painter 裁剪区相交的子控件绘制 | `AURORA_OCCLUSION_CULLING` | 始终遍历并绘制全部子控件 |
| `AURORA_DISPLAY_LIST` | `ON` | Display List 录制 / 回放：子树未脏时直接 replay 命令，跳过 paint 遍历 | `AURORA_DISPLAY_LIST` | 每次 `paint()` 都重新遍历整棵子树 |

三者默认全开；排查回归时可单独关闭定位：

```powershell
cmake -S . -B build -DAURORA_LAYOUT_CACHE=OFF -DAURORA_DISPLAY_LIST=OFF
```

### 3.4 Display List 集成约束（正确性不变量）

- **绘制副作用 / 每帧变动内容的控件必须退出 DL 缓存**：`Widget::can_cache_display_list()` 默认 `true`；绘制阶段产生副作用（如 `Hero` 向 `HeroRegistry` 上报几何）或内容每帧变化（如 `TransitionLayer` / `NavigatorHost` 按 `progress` 合成淡变）的控件**必须**覆盖为 `false`，否则缓存回放会跳过必要的每帧 `on_paint`，导致注册丢失 / 转场冻结。命中不可缓存控件时，其祖先录制会被标记 `mark_recording_dynamic()`。
- **外部裁剪不参与控件 DL**：`present_root` 的脏区裁剪 `push_clip` 不录入控件 DL；故 `Widget::paint` 在 `Painter::has_clip()` 为真时直接重录但**不缓存**，避免无裁剪帧回放越界绘制（见 `tests/test_dirty_clip_paint.cpp`）。
- **布局变更同步失效 DL**：`Widget::mark_needs_layout()` 一并调用 `invalidate_display_list_up()`，保证重排后的几何 / 内容不被旧 `bounds` 录制的 DL 回放。
- **`m_layout_parent` 悬垂安全**：`Node` 析构时将其持有的子控件 `m_layout_parent` 置空，使树重建（父容器销毁而子控件经共享所有权存活）时 `mark_needs_layout()` / `invalidate_display_list_up()` 不会解引用已释放的父指针。

### 3.5 事件驱动帧循环运行时选项（非编译开关）

以下为 `WindowOptions` **运行时字段**（非 CMake 开关，随窗口选项传入）：

| 字段 | 类型 | 默认值 | 含义 |
|:---|:---|:---|:---|
| `max_fps` | `int` | `60` | 活跃帧（有脏区 / 动画）帧率上限；`0` = 不限帧率。同步为 `FrameStats` 帧预算 |
| `power_saving` | `bool` | `true` | idle 时阻塞等待事件（省电）；`false` = 忙轮询旧行为，供持续重绘场景 opt-out |
| `renderer` | `RendererPreference` | `Auto` | 上屏后端偏好，见下表 |

**`renderer` 与 `AURORA_BACKEND_D3D11` 编译开关的关系**（仅 Win32 工厂 `create_window(Win32Options)` 生效）：

| `renderer` \ 编译 | `AURORA_BACKEND_D3D11=ON` 且设备可用 | D3D11 未编译 / 设备创建失败 |
|:---|:---|:---|
| `Auto`（默认） | 选 D3D11 GPU 上屏 | 静默回退 Win32/GDI（`AURORA_LOG_INFO` 说明） |
| `Software` | 强制 Win32/GDI | Win32/GDI |
| `GpuD3D11` | 选 D3D11（含 WARP 兜底） | 返回 `renderer-unavailable` 错误（不静默降级，错误归属调用方） |

`AURORA_BACKEND_D3D11=ON` 时 `D3D11Options.vsync`（默认 `true`）控制 `Present(1,0)`（阻塞到 vblank，后端自带帧节拍，帧调度跳过 CPU sleep）或 `Present(0,0)`（交还 CPU 帧预算节流）。

### 3.6 `aurora_api.json` 的生成（三段 merge-only，互不截断）

`aurora_api.json`（API 描述数据，供 Inspector / codegen 消费）由三个独立生成器各写各自段、读现有文件保留其它段，可任意顺序运行：

| 生成器 | 源 | 写入段 |
|:---|:---|:---|
| `gen_error_codes`（`tools/gen/gen_error_codes.cpp`） | `codespec/errors.toml` | `error_codes` |
| `gen_api_tools`（`tools/gen/gen_api.cpp`） | 库注册表 | `widgets` / `enums` / `layout_rules` / `state_patterns`，并 merge 现有 `error_codes` + `debug` |
| `gen_debug_api`（`tools/gen/gen_debug_api.cpp`） | `codespec/debug_api.toml` | `debug`（仅声明 `aurora::debug` 公共自由函数） |

```powershell
cmake --build build --target aurora_api_json        # gen_api_tools 直写 aurora_api.json
cmake --build build --target gen_debug_api_json     # 仅刷新 debug 段
# error_codes 段由 gen_error_codes 在 errors.toml 变更时重跑
```

新增 / 删除 widget 或类型后，须重新生成 aurora_api.json 以使其与 `register_core_widgets()` 注册表保持一致。

---

## 4 `AURORA_ENABLE_*`：插桩 / 分析 / 能力开关

不影响库功能，只改工具链参数，用于开发期质量保障。本组除插桩 / 分析外，亦含构建加速（`LLD` / `CCACHE`）与内部能力（`SIMD` / `DEBUG`）等开关，均按 `AURORA_ENABLE_*` 命名组归类。

| 选项 | 默认值 | 含义 | 注入内容 |
|:---|:---|:---|:---|
| `AURORA_ENABLE_COVERAGE` | `OFF` | 行覆盖率（终端摘要，不生成 HTML；按编译器分流） | GCC：`--coverage -O0 -g`（gcov）；Clang：`-fprofile-instr-generate -fcoverage-mapping -O0 -g`（LLVM 原生 source-based）。均清除默认 `-O3/-Os/-DNDEBUG`、关闭 PCH，提供 `coverage` custom target |
| `AURORA_ENABLE_ASAN` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer | 对所有目标注入 `-fsanitize=address,undefined -fno-omit-frame-pointer -g -O0`；仅 GNU/Clang 生效 |
| `AURORA_ENABLE_PROFILING` | `AUTO` | 渲染性能插桩（作用域计时 + 渲染计数器） | 三态，见 §4.1 |
| `AURORA_ENABLE_TRACING` | `OFF` | Chrome Trace Event 时间线落盘 | 注入 `AURORA_ENABLE_TRACING`（PUBLIC 传播 + 安装导出），**并强制**打开 `AURORA_ENABLE_PROFILING` |
| `AURORA_ENABLE_DEBUG` | `AUTO` | 真实后端 DEBUG 能力（截图、控件树、性能快照、可视化调试叠层、控件拾取） | 三态；**仅库内部、不 PUBLIC 传播、不导出消费者**。调试 API 在头文件始终声明、`.cpp` 体按宏裁切，消费端调用始终可编译、关闭时返回 disabled |
| `AURORA_ENABLE_SIMD` | `ON` | 光栅内核 SIMD 双实现（SSE2 基线 + AVX2 运行时分发） | 注入 `AURORA_ENABLE_SIMD`（仅库内部，不 PUBLIC 传播）；详见 §4.2 |
| `AURORA_ENABLE_CCACHE` | `ON` | ccache 编译缓存（加速重复编译） | 设置 `CMAKE_C_COMPILER_LAUNCHER` 与 `CMAKE_CXX_COMPILER_LAUNCHER`；支持 winget 安装路径自动检测 |
| `AURORA_ENABLE_LLD` | `ON` | 链接器选择（lld 加速静态链接） | GNU/Clang 下 `find_program(ld.lld)` + `check_linker_flag` 探测通过则全局注入 `-fuse-ld=lld -B<lld 目录>`；失败静默回退 GNU ld；**不注入 feature 宏** |

**约束**：

- `AURORA_ENABLE_COVERAGE` 与 `AURORA_ENABLE_ASAN` **互斥**：同时 `ON` 触发 `FATAL_ERROR`（都改写代码生成）。
- **Clang 下禁止注入 `--coverage`**：clang 的 gcov 兼容运行时（`llvm_gcda_*`）在 Windows 进程退出刷写 `.gcda` 时稳定崩溃（关闭窗口 / 测试退出即 `0xC0000005`）。现已按 `CMAKE_CXX_COMPILER_ID` 自动分流，同一开关对两套工具链透明。
- 覆盖率需覆盖测试目标（大量 widget 是 header-only，仅在测试编译单元中被编译，否则覆盖率严重偏低）。
- 插桩构建（`-O0` 全量插桩）退出前写 profile 较慢：关闭窗口后进程可能需数秒至十余秒才退出，属正常现象。

覆盖率摘要用法（GCC 与 Clang 工具链相同命令）：

```powershell
cmake -S . -B build -DAURORA_ENABLE_COVERAGE=ON
cmake --build build --target coverage -- -j $env:NUMBER_OF_PROCESSORS
```

- GCC：ctest 后经 `tools/coverage/coverage_report.ps1` 聚合 gcov 行覆盖。
- Clang：ctest 在 `LLVM_PROFILE_FILE=<build>/profraw/aurora-%p.profraw`（按 pid 分文件，并行不互覆）环境下运行，再经 `tools/coverage/coverage_report_llvm.ps1`（`llvm-profdata merge` + `llvm-cov report`）输出终端摘要。

### 4.1 `AURORA_ENABLE_PROFILING`（三态）

本开关**注入 feature 宏**（`AURORA_ENABLE_PROFILING`，PUBLIC 传播），控制 `aurora::perf` 子系统（`Stopwatch` / `Profiler` / `ScopedTimer` / `RenderCounters`）的编译期存在性。

| 取值 | Debug / RelWithDebInfo | Release / MinSizeRel | 说明 |
|:---|:---|:---|:---|
| `AUTO`（默认） | 注入 | 不注入 | 经生成器表达式 `$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:…>` 按配置分流 |
| `ON` | 注入 | 注入 | 全配置强制开启；同时进入 `AURORA_EXPORTED_DEFINES`（安装期导出给消费者） |
| `OFF` | 不注入 | 不注入 | 全配置强制关闭 |

分级语义（宏关闭时全部退化为**零开销**空语句，不产生任何指令）：

| 层级 | 接口 | 宏关闭时行为 |
|:---|:---|:---|
| 帧级 | `PerfLog` / `PerfOverlay` / `FrameStats`（**不受本开关控制**） | 始终可用 |
| 作用域级 | `AURORA_PROFILE_SCOPE(name)` / `AURORA_PROFILE_FUNCTION()` | 展开为 `((void)0)`，`ScopedTimer` 对象不构造 |
| 计数器级 | `AURORA_PROFILE_COUNT(field, n)` / `AURORA_PROFILE_SET(f, v)` | 展开为 `((void)0)`，`RenderCounters::current()` 不调用 |
| 编译期查询 | `constexpr bool aurora::profiling_enabled()` | 返回 `false`（可用于 `if constexpr` 剪裁） |

常用组合：

```powershell
# 日常开发（Debug 自动带插桩）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# 性能压测：Release 优化级别 + 插桩（计数器 / 作用域计时可读）
cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=Release -DAURORA_ENABLE_PROFILING=ON

# 纯净基线：Release 无插桩（时间类硬门槛以此配置为准）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAURORA_ENABLE_PROFILING=OFF

# 时间线落盘（Chrome about:tracing / Perfetto 可读）
cmake -S . -B build-trace -DCMAKE_BUILD_TYPE=Release -DAURORA_ENABLE_TRACING=ON
```

> **门槛配置约定**：**时间类**硬门槛（帧时间、P99、长任务）在 `Release + PROFILING=OFF` 下测量，避免插桩污染读数；**计数类**硬门槛（`RenderCounters` 各字段、脏区面积比、full-redraw 帧数）在 `Release + PROFILING=ON` 下测量——计数器在宏关闭时恒为 0，无法作为门槛。
> 两者互不冲突：计数是确定性的（与机器无关），可作为 CI 回归锚点；时间是环境相关的，只做趋势对比。测量配方见 [`GUIDELINE.md`](GUIDELINE.md) §14。

### 4.2 `AURORA_ENABLE_SIMD`

| 项 | 值 |
|:---|:---|
| 默认值 | `ON` |
| feature 宏 | 注入 `AURORA_ENABLE_SIMD`，**仅库内部使用，不 PUBLIC 传播、不导出给消费者** |
| 编译期行为 | `ON`：编译 `painter_simd.inl` 中的 SSE2（x86-64 基线恒可用）+ AVX2（运行时 CPUID 分发）快路径；`OFF`：仅编译标量黄金路径（`gradient_*_scanline_scalar`），无 SIMD 代码生成 |
| 运行时分发 | `detect_simd_level()` 懒初始化 `g_simd_level`；SSE2 恒可用，AVX2 仅在 CPU 支持时启用，未支持则回退 SSE2 / 标量尾补 |
| 确定性约束 | SIMD 路径必须与标量黄金路径**逐位一致**（`-ffp-contract=off` 禁 FMA + 同序浮点运算 + `cvtt` 整型截断）；CI 由 `test_simd_parity` 逐位比对，一票否决 |

该开关为**纯内部优化开关**：消费者代码与 ABI 均不感知 SIMD 是否存在，关闭后仅损失性能、不改变任何像素输出。开启 SIMD 不引入新的公共 API；相关函数位于 `aurora::detail`（`include/aurora/render/detail/painter_simd.h`），不计入 `aurora_api.json`。

### 4.3 `AURORA_ENABLE_CCACHE`

| 项 | 值 |
|:---|:---|
| 默认值 | `ON` |
| feature 宏 | 不注入，仅设置编译器启动器 |
| 缓存策略 | 启用压缩（level 6）、硬链接、默认缓存大小 2GB |

- **安装方式**：支持 PATH 中的 ccache，也支持 winget 安装路径自动检测（`%LOCALAPPDATA%/Microsoft/WinGet/Packages/Ccache.Ccache_*/ccache-*/ccache.exe`）。
- **配置变量**：`AURORA_CCACHE_DIR`（缓存目录，默认系统默认）、`AURORA_CCACHE_MAXSIZE`（最大缓存，默认 `2G`）。

```powershell
cmake -S . -B build -DAURORA_ENABLE_CCACHE=OFF                                  # 禁用
cmake -S . -B build -DAURORA_CCACHE_DIR=D:/ccache -DAURORA_CCACHE_MAXSIZE=5G    # 自定义
```

### 4.4 `AURORA_ENABLE_LLD`

| 项 | 值 |
|:---|:---|
| 默认值 | `ON` |
| feature 宏 | 不注入，仅影响链接器选择 |
| 编译期行为 | `find_program(ld.lld)` + `check_linker_flag` 探测通过则全局切换至 lld；找不到或检查失败静默回退 GNU ld |
| 自定义 | 非 PATH 安装的 LLVM 可用 `-DAURORA_LLD_DIR=<LLVM bin>` 提示位置 |

```powershell
cmake -S . -B build -DAURORA_ENABLE_LLD=OFF                                     # 回退 GNU ld
cmake -S . -B build -DAURORA_LLD_DIR="D:/Development/Environment/LLVM/bin"      # 显式指定
```

---

## 5 强制缓存变量（三方库源码构建内部）

FreeType 与 HarfBuzz 均从仓库内置源码（`third_party/freetype`、`third_party/harfbuzz`）经 `add_subdirectory` 编入 `aurora` 静态库（断网可构建、版本确定）。`CMakeLists.txt` 先 `add_subdirectory(third_party/freetype)` 后 `add_subdirectory(third_party/harfbuzz)`——harfbuzz 在 `if (TARGET freetype)` 时自动开启 `HB_HAVE_FREETYPE`（提供 `hb-ft.h` 并链接 freetype）。aurora 直接 `target_link_libraries(aurora PUBLIC freetype harfbuzz)`，文本 shaping 由 HarfBuzz（`hb_shape` + `hb_ft_font`）完成，故 FreeType 自身保持 `FT_DISABLE_HARFBUZZ=ON`（standalone，避免别名耦合）。

以下变量由 Aurora 以 `CACHE BOOL "" FORCE` 强制设置，**普通消费者无需手动配置**：

| 变量 | 值 | 说明 |
|:---|:---|:---|
| `FT_DISABLE_BZIP2` | `ON` | 关闭 FreeType 的 bzip2 依赖 |
| `FT_DISABLE_PNG` | `ON` | 关闭 FreeType 的 PNG 依赖 |
| `FT_DISABLE_HARFBUZZ` | `ON` | FreeType 不自带 HarfBuzz |
| `BUILD_SHARED_LIBS` | `OFF` | 静态链接，消费者无需额外 DLL |
| `HB_BUILD_SUBSET` | `OFF` | 关闭 HarfBuzz subset 库 |
| `HB_BUILD_RASTER` | `OFF` | 关闭 HarfBuzz raster 库 |
| `HB_BUILD_VECTOR` | `OFF` | 关闭 HarfBuzz vector 库 |
| `HB_BUILD_GPU` | `OFF` | 关闭 HarfBuzz GPU 后端 |
| `HB_BUILD_UTILS` | `OFF` | 关闭 HarfBuzz 命令行工具 |
| `GLFW_BUILD_EXAMPLES` | `OFF` | GLFW（仅 `AURORA_BACKEND_GLFW=ON`）：关示例 |
| `GLFW_BUILD_TESTS` | `OFF` | GLFW：关测试 |
| `GLFW_BUILD_DOCS` | `OFF` | GLFW：关文档 |
| `GLFW_INSTALL` | `OFF` | GLFW：关安装规则 |

GLFW 同口径自 `third_party/glfw` 源码构建，但仅在 `AURORA_BACKEND_GLFW=ON` 时经 `add_subdirectory(... EXCLUDE_FROM_ALL)` 引入并静态链接；默认 OFF 时链接产物不含 GLFW。

---

## 6 全局编译定义（非选项，固定注入）

| 宏 | 注入方式 | 作用域 | 说明 |
|:---|:---|:---|:---|
| `NOMINMAX` | `add_compile_definitions(NOMINMAX)` | 全局 | 抑制 `<windows.h>` 的 `min` / `max` 宏，保证 `std::min` / `std::max` 在 Windows 可用 |
| `_CRT_SECURE_NO_WARNINGS` | `target_compile_definitions(aurora PUBLIC …)` | 仅 MSVC | 抑制 MSVC 对 `std::fopen` 等 POSIX 函数的弃用警告 |
| `AURORA_BACKEND_*` | `target_compile_definitions(aurora PUBLIC …)` | 由 §3 开关控制 | 后端 feature 宏 |

---

## 7 运行时 / 测试环境变量

以下变量不进入编译，仅在运行测试 / demo 时被 `std::getenv` 读取：

| 变量 | 取值 | 作用 |
|:---|:---|:---|
| `AURORA_GOLDEN_DIR` | 目录路径 | golden 真值目录；缺省为 `tests/golden` |
| `AURORA_UPDATE_GOLDEN` | 非空（如 `1`） | 把当前渲染覆盖为新的 golden（首次生成 / 主动更新真值） |
| `AURORA_GOLDEN_MAX_DIFF` | 整数 | 像素最大允许色差阈值 |
| `AURORA_GOLDEN_MAX_PIXELS` | 整数 | 允许不一致像素数上限 |
| `AURORA_BENCH_SCALE` | 整数倍率（如 `2`） | 仅 `examples/demos/bench_gp_fps.cpp` 使用：模拟真实后端的 DPI 缩放，`2` → 每维 ×2（像素量 ×4） |

> CTest 默认 CWD = `build/`，故依赖相对路径的 golden 测试须从仓库根直接运行可执行文件（仓库 `cmake/AuroraTests.cmake` 已为依赖相对路径的测试显式设置 `WORKING_DIRECTORY` 为仓库根，故 `ctest` 下直接可跑）。

---

## 8 标准 CMake 变量

| 变量 | 默认值 | 说明 |
|:---|:---|:---|
| `CMAKE_BUILD_TYPE` | `Release`（若未设） | 常规构建默认 Release；覆盖率 / ASan 开关会自行清除其中的 `-O3` / `-Os` / `-DNDEBUG` |
| `CMAKE_CXX_STANDARD` | `20` | 强制 C++20（`CMAKE_CXX_STANDARD_REQUIRED ON`，`CMAKE_CXX_EXTENSIONS OFF`） |
| 生成器 | — | 推荐 `Ninja`（空转 / 增量调度远快于 Make）；Make 仍支持。GLFW / D3D11 后端链接依赖对应工具链的 `lib-*` 目录 |

---

## 9 安装与 find_package（消费端集成）

Aurora 以静态库交付，并提供 `find_package(Aurora)` 消费端集成。安装产物布局（前缀 `<PREFIX>`）：

```text
<PREFIX>/include/aurora/...        # 公共 API 头（aurora.h 入口）
<PREFIX>/include/nlohmann/...      # 随附的 nlohmann/json 头（aurora.h 传递包含）
<PREFIX>/include/freetype2/...     # FreeType 头
<PREFIX>/include/harfbuzz/...      # HarfBuzz 头
<PREFIX>/lib/libaurora.a           # 主静态库
<PREFIX>/lib/libfreetype.a         # 随附 FreeType 静态库
<PREFIX>/lib/libharfbuzz.a         # 随附 HarfBuzz 静态库
<PREFIX>/lib/cmake/Aurora/AuroraConfig.cmake         # 包配置（定义导入目标）
<PREFIX>/lib/cmake/Aurora/AuroraConfigVersion.cmake
```

### 9.1 安装

```powershell
cmake --install build --prefix <PREFIX>     # 或 CMAKE_INSTALL_PREFIX
```

安装规则由 `cmake/AuroraInstall.cmake` 提供（采用**手写 `AuroraConfig.cmake`**，不依赖三方自带的 export 集，以避免整图导出冲突与 `ZLIB::ZLIB` 等跨工程引用失效）。

### 9.2 消费端用法

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Aurora REQUIRED)               # 指向 <PREFIX>/lib/cmake/Aurora
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE Aurora::aurora)
```

`AuroraConfig.cmake` 定义导入目标（全部 `STATIC IMPORTED`，路径相对安装前缀解析，不依赖绝对路径硬编码）：

| 导入目标 | 含义 |
|:---|:---|
| `Aurora::aurora` | 主静态库：携带 `cxx_std_20`、公开 include、`AURORA_BACKEND_*` 等 feature 宏，以及下列传递依赖 |
| `freetype` | 随附 FreeType 静态库 |
| `harfbuzz` | 随附 HarfBuzz 静态库 |

`Aurora::aurora` 自动传递链接：`freetype` + `harfbuzz` + zlib（FreeType 解压字体表需要，尽力定位；找不到则跳过）+ `winpthread` / `pthread`（HarfBuzz 内部互斥，MinGW 下为 `winpthread`）+ Win32 系统库（`user32 gdi32 shell32 ole32 uuid`，仅 `WIN32`）。消费者**无需**手动 `find_package(FreeType)` / `find_package(HarfBuzz)`。

### 9.3 feature 宏导出约定

安装期将编译期生效的 `AURORA_BACKEND_*` / `AURORA_LAYOUT_CACHE` / `AURORA_OCCLUSION_CULLING` / `AURORA_DISPLAY_LIST` 与全局 `NOMINMAX` 收集进 `AURORA_EXPORTED_DEFINES`，由 `Aurora::aurora` 的 `INTERFACE` 编译定义导出，使消费者以与库**完全一致**的宏集编译 `aurora.h`（避免 ODR / 剪裁不一致）。新增 feature 宏时须同步本段与 `CMakeLists.txt` 的收集列表。

`AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING` 亦为 PUBLIC feature 宏，但导出策略特殊：

- `AURORA_ENABLE_PROFILING=ON`（显式强制）→ 宏对全部配置生效，写入 `AURORA_EXPORTED_DEFINES`；
- `AURORA_ENABLE_PROFILING=AUTO` → 宏由生成器表达式按**构建配置**决定，安装期无法用单一值表达，故**不导出**；消费者若需插桩接口，自行 `-DAURORA_ENABLE_PROFILING` 或安装 `=ON` 的构建；
- `AURORA_ENABLE_TRACING=ON` → 写入 `AURORA_EXPORTED_DEFINES`（其隐含的 PROFILING 亦被强制为 `ON`，一并导出）。

### 9.4 最小验证示例

`examples/consumer_find_package/`（独立工程，**不归属主构建**）是一个最小消费端：Headless 渲染一段文本到 PNG，可用来验证 `find_package` + 静态链接（含 FreeType / HarfBuzz）在目标工具链上工作。

```powershell
cd examples/consumer_find_package
cmake -S . -B build -DAurora_DIR="<PREFIX>/lib/cmake/Aurora"
cmake --build build
./build/consumer.exe          # 输出 consumer_out.png
```

> 消费端生成器须与安装库的生成器 / 工具链一致。

---

## 10 快速参考（速查表）

```text
# 产物开关
-D AURORA_BUILD_DEMOS=ON|OFF                  # demos（默认 ON）
-D AURORA_BUILD_TESTS=ON|OFF                  # CTest（默认 ON）
-D AURORA_BUILD_INSPECTOR_SERVER=ON|OFF       # Inspector HTTP 服务器（默认 OFF）
-D AURORA_BUILD_IMAGE_{JPEG,WEBP,PNG}=ON|OFF  # 图像解码（默认均 OFF）

# 后端开关（= feature 宏，PUBLIC 传播）
-D AURORA_BACKEND_HEADLESS=ON|OFF   # 无头 PNG（默认 ON）
-D AURORA_BACKEND_WIN32=ON|OFF      # Win32/GDI（Win 默认 ON，否则 OFF）
-D AURORA_BACKEND_D3D11=ON|OFF      # D3D11 GPU 上屏（默认 OFF）
-D AURORA_BACKEND_GLFW=ON|OFF       # GLFW/OpenGL（默认 OFF；源码构建）
-D AURORA_BACKEND_X11=ON|OFF        # X11/Xlib（Linux 桌面，默认 OFF）
-D AURORA_BACKEND_WAYLAND=ON|OFF    # 原生 Wayland（Linux 桌面，默认 OFF）
-D AURORA_BACKEND_MACOS=ON|OFF      # macOS（默认 OFF）
-D AURORA_BACKEND_WASM=ON|OFF       # WebAssembly（默认 OFF）

# 架构级优化（= feature 宏，PUBLIC 传播，默认均 ON）
-D AURORA_LAYOUT_CACHE=ON|OFF
-D AURORA_OCCLUSION_CULLING=ON|OFF
-D AURORA_DISPLAY_LIST=ON|OFF

# 插桩（COVERAGE 与 ASAN 互斥）
-D AURORA_ENABLE_COVERAGE=ON|OFF         # gcov / llvm-cov（默认 OFF）
-D AURORA_ENABLE_ASAN=ON|OFF             # ASan/UBSan（默认 OFF）
-D AURORA_ENABLE_PROFILING=AUTO|ON|OFF   # 渲染插桩（默认 AUTO）
-D AURORA_ENABLE_TRACING=ON|OFF          # Chrome Trace（默认 OFF，隐含 PROFILING=ON）
-D AURORA_ENABLE_DEBUG=AUTO|ON|OFF       # 真实后端 DEBUG 能力（默认 AUTO，内部宏）
-D AURORA_ENABLE_SIMD=ON|OFF             # 光栅 SIMD 双实现（默认 ON，内部宏）
-D AURORA_ENABLE_CCACHE=ON|OFF           # ccache 编译缓存（默认 ON）
-D AURORA_ENABLE_LLD=ON|OFF              # lld 链接器（默认 ON）

# 安装 / 消费端
cmake --install build --prefix <PREFIX>
cmake -S app -B app/build -DAurora_DIR="<PREFIX>/lib/cmake/Aurora"

# 运行时（测试）
AURORA_GOLDEN_DIR=<dir> AURORA_UPDATE_GOLDEN=1 ./build/aurora_test_runner --run=test_offscreen
```
