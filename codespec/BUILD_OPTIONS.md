# 编译选项与宏定义统一参考

> 本文件是 Aurora 项目**所有**编译期可配置开关、缓存变量、传播宏与运行时环境变量的**唯一权威来源**。
> 凡涉及 CMake 构建选项、feature 宏、路径 / 插桩变量，一律以本文件为准；其余文档只做指针，不重复罗列，以避免文档漂移。

---

## 1 总览：三层命名分类法

所有构建选项按**语义**严格归入三组，组内前缀一致：

| 前缀 | 类别 | 语义 | 是否向库注入 feature 宏 |
|:---|:---|:---|:---|
| `AURORA_BUILD_*` | 构建产物开关 | 是否**构建**某个额外交付物（demos / tests / Inspector 服务器） | 否（例外：`AURORA_BUILD_INSPECTOR_SERVER`（§2.3）的开关名同时作为编译宏注入——与后端组「开关名 = 宏名」同惯例） |
| `AURORA_BACKEND_*` | 内置后端开关 | 每个**内置 `Surface` 图形后端**一个开关；**开关名 = PUBLIC feature 宏名** | 是（`#ifdef` 剪裁 + PUBLIC 传播给消费者） |
| `AURORA_ENABLE_*` | 插桩 / 分析 / 能力开关 | 是否注入编译 / 链接期分析工具（覆盖率 / 内存检测 / 调试 / 性能插桩）或开启构建加速 / 内部能力（lld / ccache / SIMD / DEBUG / 测试注入点 / 内置音频设备后端） | 多数否；`PROFILING` / `TRACING` 与架构级优化三开关及 `DEBUG` / `TEST_HOOKS` / `AUDIO` / `AUDIO_WASAPI` / `AUDIO_ALSA` / `AUDIO_WEBAUDIO` 注入 PUBLIC 宏，`SIMD` / `IMAGE_*` 注入内部宏，`LLD` / `CCACHE` 不注入宏 |

> `Win32/GDI` 后端仅在 `_WIN32` 下编译，无需额外开关，已由 `AURORA_BACKEND_WIN32` 的内置默认值覆盖。

### 1.1 CMake 脚本布局

顶层 `CMakeLists.txt` 只做「工程声明 + 核心库目标 + 模块编排」，具体逻辑按职责划分在 `cmake/` 下的模块（`include()` 不创建新作用域，各模块内 `option()` / 目标定义与写在顶层完全等价，开关名与默认值不变）：

| 模块 | 职责 |
|:---|:---|
| `cmake/AuroraFeatures.cmake` | **feature 宏单一入口** `aurora_define_feature(<宏> [SCOPE] [TARGET] [RAW] [EXPORT])`：定义注入 + `AURORA_FEATURE_DEFINES` 导出登记二合一；全部 feature 宏调用点（后端 / 优化 / SIMD / PROFILING / TRACING / DEBUG / 编解码）经它声明。运行时查询入口 `aurora::debug::feature_flags()`（`include/aurora/debug/feature_flags.h`） |
| `cmake/AuroraThirdParty.cmake` | FreeType / HarfBuzz 源码构建 |
| `cmake/AuroraUtils.cmake` | 消费者目标统一配置辅助（`aurora_setup_consumer_target`，demo / 测试 / 工具复用链接 / PCH / C++20 / 告警 / MinGW `-Wa,-mbig-obj`） |
| `cmake/AuroraBackends.cmake` | 全部 `AURORA_BACKEND_*` Surface 图形后端剪裁开关 + 音频设备后端开关（`AURORA_ENABLE_AUDIO` / `AURORA_ENABLE_AUDIO_WASAPI` / `AURORA_ENABLE_AUDIO_ALSA` / `AURORA_ENABLE_AUDIO_WEBAUDIO`，ENABLE 组）+ 存储 SQLite 后端开关（`AURORA_ENABLE_STORAGE_SQLITE`，ENABLE 组）+ 架构级优化宏（`AURORA_ENABLE_LAYOUT_CACHE` 等） |
| `cmake/AuroraImageCodecs.cmake` | `AURORA_ENABLE_IMAGE_JPEG` / `AURORA_ENABLE_IMAGE_WEBP` / `AURORA_ENABLE_IMAGE_PNG`（编译期能力开关） |
| `cmake/AuroraSimd.cmake` | `AURORA_ENABLE_SIMD`（光栅内核 SIMD 双实现，内部宏，不 PUBLIC 传播） |
| `cmake/AuroraCcache.cmake` | `AURORA_ENABLE_CCACHE`（ccache 编译缓存启动器） |
| `cmake/AuroraTools.cmake` | 工具 / 基准可执行（`aurora_add_tool()` 统一样板）+ `AURORA_BUILD_INSPECTOR_SERVER` |
| `cmake/AuroraVerify.cmake` | `AURORA_BUILD_VERIFY_TOOLS`：真机验收探针（`tools/verify/` 下按「当前平台 + 已开启后端」条件定义，全部 `EXCLUDE_FROM_ALL`，**不进 CTest**） |
| `cmake/AuroraDemos.cmake` | 示例 demo 定义块（须在 `AuroraTools` 与 `AuroraTests` 之后 include，因其依赖 `aurora_inspector_server` 目标） |
| `cmake/AuroraTests.cmake` | `AURORA_BUILD_TESTS` 注册式 runner（GLOB `tests/*.cpp`、`tests/unit/*.cpp` 与 `tests/integration/*.cpp` → 单一 `aurora_test_runner`，`AURORA_TEST()` 自注册） |
| `cmake/AuroraInstrumentation.cmake` | `AURORA_ENABLE_COVERAGE` / `AURORA_ENABLE_ASAN` / `AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING` / `AURORA_ENABLE_DEBUG` / `AURORA_ENABLE_TEST_HOOKS`（须在全部目标定义之后 include） |
| `cmake/AuroraInstall.cmake` | 安装 + `find_package(Aurora)` 导出（须在后端开关之后 include） |
| `cmake/AuroraLint.cmake` | `AURORA_ENABLE_CLANG_TIDY`（`lint` / `lint-fix` 聚合目标，经 `tools/check/run_clang_tidy.py` 并行 lint 非 third_party 翻译单元；须在全部目标定义之后 include） |
| `cmake/AuroraFormat.cmake` | `AURORA_ENABLE_CLANG_FORMAT`（`format` / `format-check` 聚合目标，经 `tools/check/run_clang_format.py` 并行校验 / 重写非 third_party 源文件；与目标定义无关，放最后 include 亦可） |
| `cmake/AuroraCheckTestRegistry.cmake` | **遗留模块**：当前无 CMake `include()` 引用（`registry_integrity` 已改由 `AuroraTests.cmake` 直接注册 python 脚本 `check_test_registry.py`）；保留仅供手工 / 历史参考，不计入常规构建 |

---

## 2 `AURORA_BUILD_*`：构建产物开关

控制「是否编译某个额外交付物」。这些开关**不向库代码注入 feature 宏**，只决定目标是否被加入构建。

| 选项 | 默认值 | 含义 | 引入的目标 |
|:---|:---|:---|:---|
| `AURORA_BUILD_DEMOS` | `ON` | **定义**（非默认构建）`examples/demos/` 下每组件一个的可运行窗口 demo 目标；均 `EXCLUDE_FROM_ALL`，按需构建 | 各 `demo_<组件>` 可执行文件 + 聚合目标 `demos` |
| `AURORA_BUILD_TESTS` | `ON` | 编译 `tests/` 下全部用例并接入 CTest：`AURORA_TEST()` 注册、单一 runner 一次链接，逐条 `--run=<stem>` 隔离 | `aurora_test_runner` 可执行 + `enable_testing()` + `registry_integrity` 守护 |
| `AURORA_TEST_SHARDS` | `1` | 测试 runner 分片数（非开关、为正整数缓存变量）：`1` 与单 runner 完全等价；`N>1` 按 Suite（文件 stem）MD5 稳定散列把用例源拆为 N 个 runner（各含唯一 main），CTest 用例名带分片号（`<stem>_s<k>`，其中 `k` 从 `0` 起取 `0..N-1`），`registry_integrity` 对各 runner `--list` 取并集比对 | N 个 `aurora_test_runner_s<k>`（`k` 取 `0..N-1`）可执行；是否默认开启待收束期链接耗时数据 |
| `AURORA_BUILD_INSPECTOR_SERVER` | `OFF` | 编译 Inspector 远程 HTTP 服务器（跨平台：Windows 链 `ws2_32` / POSIX 链 `pthread`） | `aurora_inspector_server` 静态库 |
| `AURORA_BUILD_VERIFY_TOOLS` | `OFF` | **定义**（非默认构建）`tools/verify/` 下的真机验收探针：按「当前平台 + 已开启后端」条件定义，全部 `EXCLUDE_FROM_ALL`，**不进 CTest**（会创建真实窗口、读取屏幕光标，非确定且干扰用户桌面） | 各 `aurora_verify_<平台>_cursor` 可执行文件 + 聚合目标 `aurora_verify` |
### 2.1 demo 构建方式

demo 不进默认构建（`EXCLUDE_FROM_ALL`）：日常 `cmake --build build` 只建库 / 工具 / 测试；单个 demo 按名构建（`cmake --build build --target demo_lazy_list`），全部 demo 用聚合目标（`cmake --build build --target demos`）。关闭 `AURORA_BUILD_DEMOS` 则连目标都不定义。

### 2.2 预编译头（PCH）

- **库自身**：`include/aurora/aurora_pch.h` 收录标准库 + `nlohmann/json.hpp`（不含 aurora 自有头，保证库开发时命中率），`aurora` 库 PRIVATE 编译一份。**GCC（MinGW）下同样强制关闭**：实测 122MB 的库 gch 每库 TU 全量加载 + ccache 全文 hash，且 gch 字节参与缓存 key（头文件一变全部库 TU 失效）；关闭后全量重编 155.4s → 68s（库侧），冷构建省约 1 分钟。MSVC/Clang 不变。
- **消费者**：MSVC/Clang 下 `aurora_consumer_pch` 锚定目标把 `aurora.h` 伞头整体预编译一份，全部 demo / 测试 / 工具经 `target_precompile_headers(REUSE_FROM aurora_consumer_pch)` 复用（aurora 头变更本就触发消费者重编，不增加失效面）。**GCC（MinGW）下消费者 PCH 强制关闭**：实测 296MB 的 .gch 从未被消费者命中（生成/消费侧编译器设置失配，`-Winvalid-pch` 拒用），却仍要每 TU 全量探测加载（GCC）+ 全文 hash（ccache），每 TU ≈ 600MB 纯亏损 I/O，净收益为负；消费者改走伞头文本编译 + ccache 缓存。
- 覆盖率 / ASan 开启时 PCH 全部自动关闭（与 GCC 判定共用同一门控变量）。

### 2.3 `AURORA_BUILD_INSPECTOR_SERVER`

| 属性 | 值 |
|:---|:---|
| 类型 | `option()` |
| 默认值 | `OFF` |
| 说明 | 编译 Inspector 远程 HTTP 服务器（`InspectorServer`），暴露 REST 端点供外部工具远程访问运行时控件树 |
| 传播宏 | `AURORA_BUILD_INSPECTOR_SERVER`（开关名 = 宏名，与 `AURORA_BACKEND_*` 同惯例；`aurora_inspector_server` 目标 PUBLIC，随链接注入 demo / 测试，经 `aurora_define_feature` 注入） |
| 平台限制 | 跨平台（Windows: `ws2_32` / POSIX: `pthread`） |
| 产物 | `aurora_inspector_server` 静态库（`src/aurora/inspector/inspector_server.cpp`） |
| 头文件 | `include/aurora/inspector/inspector_server.h` |

`inspector_server.cpp` 已从核心 `aurora` 库源文件列表中排除（`list(FILTER ... EXCLUDE)`），仅当开关为 `ON` 时编入独立静态库，避免未开启时引入 Winsock2 依赖。

```powershell
cmake -S . -B build -DAURORA_BUILD_INSPECTOR_SERVER=ON
```

### 2.4 `AURORA_BUILD_VERIFY_TOOLS`（真机验收探针）

| 属性 | 值 |
|:---|:---|
| 类型 | `option()` |
| 默认值 | `OFF` |
| 说明 | 定义 `tools/verify/` 下的**真机验收探针**可执行目标。这类探针证明的是「无头 CI 无法证明」的平台接线能力（典型：光标形状的各后端 `Surface::set_cursor` 是否真的改变了屏幕上显示的光标；输入法桥的 `WM_IME_*` 是否真的落到焦点控件） |
| 传播宏 | 无（纯交付物开关，不向库代码注入宏） |
| 模块 | `cmake/AuroraVerify.cmake` |
| 产物 | 按条件定义：`aurora_verify_x11_cursor`（`AURORA_BACKEND_X11`）/ `aurora_verify_wayland_cursor`（`AURORA_BACKEND_WAYLAND`）/ `aurora_verify_win32_cursor`（`WIN32` 且开 `AURORA_BACKEND_WIN32` 或 `D3D11`）/ `aurora_verify_macos_cursor`（`APPLE` 且开 `AURORA_BACKEND_MACOS`，需 ObjC++）/ `aurora_verify_glfw_cursor`（`AURORA_BACKEND_GLFW`）/ `aurora_verify_glfw_gpu_features`（`AURORA_BACKEND_GLFW` 且开 `AURORA_ENABLE_GLFW_GPU_GL`）/ `aurora_verify_win32_ua`（`WIN32` 且开 `AURORA_BACKEND_WIN32` 或 `D3D11`，另链 `oleacc`）/ `aurora_verify_win32_ime`（`WIN32` 且开 `AURORA_BACKEND_WIN32` 或 `D3D11`，`imm32` 已随库 PUBLIC 链接）/ `aurora_verify_x11_ime`（`LINUX` 且开 `AURORA_BACKEND_X11`，XIM 协商/焦点宣告/XTEST 落键）/ `aurora_verify_wayland_ime`（`LINUX` 且开 `AURORA_BACKEND_WAYLAND`，门 `AURORA_HAVE_WL_TEXT_INPUT`）/ `aurora_verify_win32_wgpu`（`WIN32` 且开 `AURORA_BACKEND_GPU_WGPU` + `AURORA_BACKEND_WIN32`，§3.8）/ `aurora_verify_x11_wgpu`（开 `AURORA_BACKEND_GPU_WGPU` + `AURORA_BACKEND_X11`，§3.8，XGetImage 截图物证，需 `AURORA_ENABLE_DEBUG` 开启）/ `aurora_verify_wayland_wgpu`（开 `AURORA_BACKEND_GPU_WGPU` + `AURORA_BACKEND_WAYLAND`，§3.8）/ `aurora_verify_wasapi_audio`（`WIN32` 且开 `AURORA_ENABLE_AUDIO_WASAPI`）/ `aurora_verify_alsa_audio`（`LINUX` 且开 `AURORA_ENABLE_AUDIO_ALSA`）；聚合目标 `aurora_verify`。**浏览器**产物另按 `EMSCRIPTEN` + `AURORA_BACKEND_WASM` 定义 `aurora_verify_wasm_raf` / `aurora_verify_wasm_multiwin` / `aurora_verify_wasm_aria` / `aurora_verify_wasm_audio`（末者另需 `AURORA_ENABLE_AUDIO_WEBAUDIO`）——它们是 `.html+.js+.wasm` 三件套、须走本地 http 由无头 Edge 打开，判据由随库的 `tools/verify/wasm_*_cdp_drive.mjs` 派发，**有意不进**聚合目标（该目标面向本机直接运行的探针） |

三点与其它"产物开关"不同的地方：

1. **不进 CTest**：探针会创建真实窗口、读取屏幕光标/指针状态，非确定且会干扰用户桌面，属**人工触发的验收工具**而非自动化用例。
2. **条件定义**：按「当前平台 + 已开启后端」逐一判定；无一匹配时连目标都不定义（与 demo `EXCLUDE_FROM_ALL` 同口径）。macOS 路会条件 `enable_language(OBJCXX)`，对其它平台零影响。
3. **默认 OFF 且 `EXCLUDE_FROM_ALL`**：`cmake --build build` 不会连带构建，既有构建/门禁/CI 完全不受影响。

```powershell
# 例：在 Linux 桌面验收 X11 光标接线
cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DAURORA_BACKEND_X11=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
cmake --build build-verify --target aurora_verify_x11_cursor
./build-verify/aurora_verify_x11_cursor
```

各探针的验收范围、逐项期望、退出码语义写在对应源文件头注释内（`tools/verify/*.cpp|.mm`）；真机验收须在**对应平台**手工执行（探针不进 CTest，见上方三点）。

---

## 3 `AURORA_BACKEND_*`：后端开关（= feature 宏）

每个内置 `Surface` 图形后端一个开关；GPU 栅格后端 `AURORA_BACKEND_GPU_WGPU`（wgpu，§3.8）亦归此组。而 `AURORA_ENABLE_GLFW_GPU_GL` 是既有 GLFW 后端之上的 GPU 栅格模式增强、非独立后端，内置音频设备后端（`AURORA_ENABLE_AUDIO` / `AURORA_ENABLE_AUDIO_WASAPI` / `AURORA_ENABLE_AUDIO_ALSA` / `AURORA_ENABLE_AUDIO_WEBAUDIO`）为能力开关，二者均归 `AURORA_ENABLE_*` 组（§4）。**开关名与 PUBLIC 编译宏名完全相同**，宏以 `target_compile_definitions(aurora PUBLIC …)` 传播给所有消费者；库代码用 `#ifdef AURORA_BACKEND_XXX` 做代码剪裁。关闭某后端后，对应实现类、工厂重载与重型平台头被预处理器剔除，链接产物不再含该后端。自定义注入路径（自定义 `Surface` / 自定义 `AudioDeviceBackend`）始终可用，故「只用自定义 backend」可不编译任何内置后端。

| 选项 | 默认值 | 含义 | 传播宏 | 额外链接 |
|:---|:---|:---|:---|:---|
| `AURORA_BACKEND_HEADLESS` | `ON` | 无头内存 / PNG 后端（`HeadlessSurface`，离线渲染 / 测试） | `AURORA_BACKEND_HEADLESS` | — |
| `AURORA_BACKEND_WIN32` | Windows `ON`，否则 `OFF` | Win32/GDI 后端（`Win32Surface` + `Win32Host` 共享宿主） | `AURORA_BACKEND_WIN32` | `user32` `gdi32` `shell32` `ole32` `uuid` `imm32`（仅 `_WIN32`；`imm32` 供输入法组合桥） |
| `AURORA_BACKEND_D3D11` | `OFF` | D3D11 GPU 增量上屏后端（`D3D11Surface`） | `AURORA_BACKEND_D3D11` | `d3d11` `dxgi` `d3dcompiler`（仅 `_WIN32`） |
| `AURORA_BACKEND_GPU_WGPU` | `OFF` | wgpu GPU 栅格后端（`WgpuRhi`；真窗口帧路径 `WgpuWin32Surface` / `WgpuX11Surface` / `WgpuWaylandSurface` 另与宿主 `AURORA_BACKEND_WIN32` ∨ `AURORA_BACKEND_X11` ∨ `AURORA_BACKEND_WAYLAND` 合取），源码经 cargo 构建，需 Rust 工具链 + libclang，配置期缺项 FATAL，见 §3.8 | `AURORA_BACKEND_GPU_WGPU` | `wgpu_native` 静态库 + `ws2_32` `userenv` `bcrypt` `advapi32` `oleaut32` `ntdll`(Windows)/`dl` `pthread` `m`(Linux) |
| `AURORA_BACKEND_GLFW` | `OFF` | GLFW + OpenGL（上下文 3.3 兼容剖面，绘制 1.1 立即模式） | `AURORA_BACKEND_GLFW` | `glfw` 目标（源码静态库）+ `opengl32`(Windows)/`OpenGL::GL`(其他平台) |
| `AURORA_BACKEND_X11` | `OFF` | X11 / Linux 桌面后端（`X11Surface`，pimpl 完整实现） | `AURORA_BACKEND_X11` | `${X11_LIBRARIES}`（`find_package(X11)`） |
| `AURORA_BACKEND_WAYLAND` | `OFF` | 原生 Wayland / Linux 桌面后端（`WaylandSurface`，pimpl 完整实现） | `AURORA_BACKEND_WAYLAND` | `${WAYLAND_CLIENT_LIBRARIES}` `${WAYLAND_CURSOR_LIBRARIES}` `${XKBCOMMON_LIBRARIES}`（`pkg-config`） |
| `AURORA_BACKEND_MACOS` | `OFF` | macOS 后端（`MacOSSurface`，顶层 `enable_language(OBJCXX)` 先于目标定义，非 Apple 开启 FATAL） | `AURORA_BACKEND_MACOS` | `Cocoa` `AppKit`（框架） |
| `AURORA_BACKEND_WASM` | `OFF` | WebAssembly 后端（`WasmSurface`，需 Emscripten 工具链：`cmake --preset wasm`（经 `$EMSDK` 注入 toolchain，等价 `emcmake cmake`）；非 Emscripten 开启 FATAL。构建期生成器改经 `_native_tools` 原生子项目产出，见 `AuroraTools.cmake`） | `AURORA_BACKEND_WASM` | Emscripten 工具链 |

### 3.1 GLFW 源码构建

GLFW 后端**无伴随缓存变量**：依赖仓库内置 `third_party/glfw` 源码构建（与 FreeType / HarfBuzz 同口径——源码进仓库、断网可构建、版本确定），不存在外部安装根定位。

```powershell
cmake -S . -B build -DAURORA_BACKEND_GLFW=ON
```

构建细节：关 examples / tests / docs / install、`EXCLUDE_FROM_ALL`（仅 aurora 链接时连带构建）、静态链接无 DLL 依赖。仓库缺 `third_party/glfw` 源码时配置期直接 `FATAL_ERROR`（不回退外部二进制，避免发行版路径漂移）。

`AURORA_ENABLE_GLFW_GPU_GL=ON` 时 GLFW 窗口可请求 GPU 渲染模式（`GlfwOptions::gpu = true`）：`GlfwSurface` 负责创建 3.3 core 上下文与 swapBuffers 呈现，`GpuGlRhi` 只实现「DisplayList → GL 批渲染」（自写最小函数表 loader，无 GLAD/gl3w 三方依赖）。GPU 初始化失败（驱动过老 / 无 3.3）运行期自动回退软件纹理上传路径，不抛异常；`Surface::gpu_backend()` 非空时其 `name()` 恒为 `"gpu-gl"`。

### 3.2 Linux 桌面后端（X11 / 原生 Wayland）

两个 Linux 桌面后端均为纯软件 `Painter` 上屏（无 GPU），可单开或**同时开启**；同时开启时运行期按会话类型自动择优：`WAYLAND_DISPLAY` 存在 → 优先原生 `WaylandSurface`，否则 `X11Surface`（Wayland 会话下经 XWayland）；真实显示不可用时 `create_native_window` 回退 `HeadlessSurface`。

**开发依赖**（仅编译期，运行库桌面环境通常已自带）：

| 后端 | Fedora / RHEL | Debian / Ubuntu |
|:---|:---|:---|
| X11 | `dnf install libX11-devel` | `apt install libx11-dev` |
| Wayland | `dnf install wayland-devel wayland-protocols-devel libxkbcommon-devel` | `apt install libwayland-dev wayland-protocols libxkbcommon-dev` |
| GLFW（X11 扩展） | `dnf install libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel` | `apt install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev` |
| GLFW（OpenGL 链接） | `dnf install mesa-libGL-devel` | `apt install libgl1-mesa-dev` |

构建示例（同时开启，最常见的 Linux 桌面配置）：

```bash
cmake -S . -B build -DAURORA_BACKEND_X11=ON -DAURORA_BACKEND_WAYLAND=ON
cmake --build build -j $(nproc)
```

要点：

- Wayland 后端在配置期用 `pkg-config` 检测 `wayland-client` / `wayland-cursor` / `xkbcommon`（三者皆 `REQUIRED`：客户端主题光标 `wl_cursor_theme_*` 是 `set_cursor` 的硬依赖，缺失即配置期红灯而非运行期降级），并调用 `wayland-scanner` 把 `xdg-shell.xml` / `xdg-decoration-unstable-v1.xml` 生成为 C 胶水（落在 `build/wayland-gen/`，不入仓）。**依赖表无需追加包**：`libwayland-cursor` 属 wayland 核心项目，其头与 `.pc` 随核心 devel 包发布（Debian/Ubuntu 实测 `wayland-cursor.h` / `wayland-cursor.pc` 均在 `libwayland-dev` 内），上表已覆盖。
- 服务端窗口装饰经 `zxdg_decoration_manager_v1` 协商：KDE 等支持方绘制标题栏；**GNOME / WSLg Weston 不实现该协议 → 后端自绘 CSD 标题栏兜底**（`AURORA_LOG_INFO` 提示），属合成器限制而非缺陷。
- 输入法桥协议 `zwp_text_input_manager_v3` 为**软探测**（`AURORA_HAVE_WL_TEXT_INPUT`，`PUBLIC` 编译宏 = `1`/`0`）：配置期在 `wayland-protocols` 的 XML 清单中找 `text-input-unstable-v3.xml`，命中则 `wayland-scanner` 生成胶水并置 `1`；缺失（老协议集发行版）则跳过生成并置 `0`——桥体整段被宏裁切，后端其余能力照常编译，**不报配置期红灯**（与 xdg-shell 硬依赖区分：输入法可运行期降级，协议头缺失不可）。
- 非 Linux（含 Apple）平台开启任一后端将触发 `FATAL_ERROR`。

### 3.3 架构级渲染 / 布局优化开关

三项互不依赖的架构级优化。三者归入 `AURORA_ENABLE_*` 命名组（组内特例：PUBLIC 注入**并随安装导出**，因消费者须与库同宏取值编译 `aurora.h`——ODR / 剪裁一致性）；开关名 = 宏名，库代码用 `#ifdef` 剪裁。关闭任一开关即回退到原始实现路径（等价无优化），可独立退化。

| 选项 | 默认值 | 含义 | 传播宏 | 退化行为 |
|:---|:---|:---|:---|:---|
| `AURORA_ENABLE_LAYOUT_CACHE` | `ON` | 布局约束缓存：约束不变且非 layout dirty 时跳过子树 layout 递归 | `AURORA_ENABLE_LAYOUT_CACHE` | 每次 `layout()` 都重新计算整棵子树 |
| `AURORA_ENABLE_OCCLUSION_CULLING` | `ON` | 遮挡剔除：跳过不与 Painter 裁剪区相交的子控件绘制 | `AURORA_ENABLE_OCCLUSION_CULLING` | 始终遍历并绘制全部子控件 |
| `AURORA_ENABLE_DISPLAY_LIST` | `ON` | Display List 录制 / 回放：子树未脏时直接 replay 命令，跳过 paint 遍历 | `AURORA_ENABLE_DISPLAY_LIST` | 每次 `paint()` 都重新遍历整棵子树 |

三者默认全开；排查回归时可单独关闭定位：

```powershell
cmake -S . -B build -DAURORA_ENABLE_LAYOUT_CACHE=OFF -DAURORA_ENABLE_DISPLAY_LIST=OFF
```

### 3.4 Display List 集成约束（正确性不变量）

- **绘制副作用 / 每帧变动内容的控件必须退出 DL 缓存**：`Widget::can_cache_display_list()` 默认 `true`；绘制阶段产生副作用（如 `Hero` 向 `HeroRegistry` 上报几何）或内容每帧变化（如 `TransitionLayer` / `NavigatorHost` 按 `progress` 合成淡变）的控件**必须**覆盖为 `false`，否则缓存回放会跳过必要的每帧 `on_paint`，导致注册丢失 / 转场冻结。命中不可缓存控件时，其祖先录制会被标记 `mark_recording_dynamic()`。
- **外部裁剪不参与控件 DL**：`present_root` 的脏区裁剪 `push_clip` 不录入控件 DL；故 `Widget::paint` 在 `Painter::has_clip()` 为真时直接重录但**不缓存**，避免无裁剪帧回放越界绘制（见 `tests/integration/itest_dirty_clip_paint.cpp`）。
- **全局光栅状态按世代失效**：`Widget::paint` 的 DL / 离屏层缓存命中条件含 `FontEngine::raster_generation()`；`set_text_aa_mode` 与字体注入接口在值变化时自增它，从而让全树缓存下一帧重录。新增「全局影响字形光栅」的设置时，须在 setter 内自增该世代，否则缓存会回放旧光栅。
- **布局变更同步失效 DL**：`Widget::mark_needs_layout()` 一并调用 `invalidate_display_list_up()`，保证重排后的几何 / 内容不被旧 `bounds` 录制的 DL 回放。
- **`layout_parent_` 悬垂安全**：`Node` 析构时将其持有的子控件 `layout_parent_` 置空，使树重建（父容器销毁而子控件经共享所有权存活）时 `mark_needs_layout()` / `invalidate_display_list_up()` 不会解引用已释放的父指针。

### 3.5 事件驱动帧循环运行时选项（非编译开关）

以下为 `WindowOptions` **运行时字段**（非 CMake 开关，随窗口选项传入）：

| 字段 | 类型 | 默认值 | 含义 |
|:---|:---|:---|:---|
| `max_fps` | `int` | `60` | 活跃帧（有脏区 / 动画）帧率上限；`0` = 不限帧率。同步为 `FrameStats` 帧预算 |
| `power_saving` | `bool` | `true` | idle 时阻塞等待事件（省电）；`false` = 忙轮询旧行为，供持续重绘场景 opt-out |
| `renderer` | `RendererPreference` | `Auto` | 上屏后端偏好，见下表 |

**`renderer` 与 `AURORA_BACKEND_D3D11` / `AURORA_BACKEND_GPU_WGPU` 编译开关的关系**（Win32 工厂 `create_window(Win32Options)` 生效；`GpuWgpu` 行同款语义亦适用 X11 工厂 `create_window(X11Options)`，Linux 下 `create_native_window` 对该偏好直接走 X11 工厂；列 = 对应后端「编译进库且设备/adapter 可用」与否）：

| `renderer` \ 编译 | 对应后端 `ON` 且设备可用 | 后端未编译 / 设备（adapter）创建失败 |
|:---|:---|:---|
| `Auto`（默认） | 选 D3D11 GPU 上屏 | 静默回退 Win32/GDI（`AURORA_LOG_INFO` 说明） |
| `Software` | 强制 Win32/GDI | Win32/GDI |
| `GpuD3D11` | 选 D3D11（含 WARP 兜底） | 返回 `renderer-unavailable` 错误（不静默降级，错误归属调用方） |
| `GpuWgpu` | 与 `AURORA_BACKEND_GPU_WGPU` 合取：编译且 adapter 可用时选 `WgpuWin32Surface`（Win32 宿主）/ `WgpuX11Surface`（X11 宿主）/ `WgpuWaylandSurface`（Wayland 宿主；`create_native_window` 运行期按 `WAYLAND_DISPLAY` 会话择路，§3.8）（GPU 栅格） | 未编译 / 无 adapter 时返回 `renderer-unavailable`（不降级；`Auto` 优先序不含 wgpu） |

`AURORA_BACKEND_D3D11=ON` 时 `D3D11Options.vsync`（默认 `true`）控制 `Present(1,0)`（阻塞到 vblank，后端自带帧节拍，帧调度跳过 CPU sleep）或 `Present(0,0)`（交还 CPU 帧预算节流）。

### 3.6 `aurora_api.json` 的生成（三段 merge-only，互不截断）

`aurora_api.json`（API 描述数据，供 Inspector / codegen 消费）由三个独立生成器各写各自段、读现有文件保留其它段，可任意顺序运行：

| 生成器 | 源 | 写入段 |
|:---|:---|:---|
| `gen_error_codes`（`tools/gen/gen_error_codes.cpp`） | `codespec/errors.toml` | `error_codes` |
| `gen_api_tools`（`tools/gen/gen_api.cpp`） | 库注册表 | `widgets` / `enums` / `layout_rules` / `state_patterns`，并 merge 现有 `error_codes` + `debug` |
| `gen_debug_api`（`tools/gen/gen_debug_api.cpp`） | `codespec/debug_api.toml` | `debug`（仅声明 `aurora::debug` 公共自由函数） |

```powershell
cmake --build build --target aurora_api_json        # 先 gen_api_tools 直写 aurora_api.json，再 gen_debug_api 合并 debug 段（含 debug 段二次 merge）
cmake --build build --target gen_debug_api_json     # 仅刷新 debug 段
# error_codes 段由 gen_error_codes 在 errors.toml 变更时重跑
```

新增 / 删除 widget 或类型后，须重新生成 aurora_api.json 以使其与 `register_core_widgets()` 注册表保持一致。

### 3.7 无障碍桥（Win32 UIA / Linux AT-SPI2）：**无独立开关**

无障碍桥**不引入任何 CMake 选项或 feature 宏**（编译期定义按模块头直接 `#include core/platform.h`，无新宏）。门控规则只有两条：

1. **编译门控**：Win32 UIA 桥实现在 `src/aurora/window/detail/win32_ua.{h,cpp}`，随 `AURORA_BACKEND_WIN32`（Windows 默认 ON）编入；`AURORA_BACKEND_D3D11` 复用同一桥，故门控为「平台宏 ∧ 后端宏析取」，与 `src/aurora/window/win32_cursor.h` 同款。Linux AT-SPI2 桥（`src/aurora/window/detail/atspi_{protocol,bridge}.{h,cpp}`）门控为「Linux 平台 ∧（`AURORA_BACKEND_X11` ∨ `AURORA_BACKEND_WAYLAND`）」，两后端共用同一桥。语义树 / 钩子升级（`core/accessibility.h`、`core/a11y_*.h`、`Widget` 虚钩子）**无任何门控**——公共头纯增量，所有构建路径可见。
2. **运行期门控**：`UIAutomationCore.dll` **动态加载**（`LoadLibraryA`），无链接期依赖；缺库或必要导出缺失时整桥降级为 no-op 并 `Diagnostics::warn` 一次。桥本身**惰性构造**——首个平台查询（`WM_GETOBJECT`）到达才构建，无读屏在线时零开销。Linux 侧同样 **`dlopen("libdbus-1.so.3")`** 动态加载（无链接期依赖）：libdbus 缺失、无会话总线、`org.a11y.Bus` 不可达或 `NO_AT_BRIDGE=1` 时整桥降级为不存在；构造时机为首帧语义树根注入（Linux 无查询驱动信号，见 `specification/06-app-platform.md` §6.4），总线地址可用 `AT_SPI_BUS_ADDRESS` 显式直给。

### 3.8 wgpu GPU 栅格后端（cargo 源码构建 + Rust 工具链探测）

`AURORA_BACKEND_GPU_WGPU=ON`（默认 `OFF`）启用 `WgpuRhi`（同一套 WGSL 管线覆盖 Vulkan / D3D12 / Metal / GLES）；真窗口帧路径 `WgpuWin32Surface`（Win32 宿主）/ `WgpuX11Surface`（X11 宿主）/ `WgpuWaylandSurface`（Wayland 宿主）与 `create_window(WgpuOptions)` 另与 `AURORA_BACKEND_WIN32` ∨ `AURORA_BACKEND_X11` ∨ `AURORA_BACKEND_WAYLAND` 合取门控（有哪个宿主宏就产出哪个宿主；Linux 两宏并开时 `WgpuOptions` 编译期取 X11，Wayland 宿主经 `WaylandOptions` + `GpuWgpu` 或 `create_native_window` 会话选择直达）。依赖为仓库内置 `third_party/wgpu-native/`（gfx-rs v29 源码，**保持上游原样不修改**），经 cargo 构建为静态库（staticlib）链入——对齐「静态交付、消费者无额外 DLL」。

```powershell
cmake -S . -B build -DAURORA_BACKEND_GPU_WGPU=ON -DAURORA_BACKEND_WIN32=ON
cmake --build build   # 首次连带 cargo build --release wgpu-native（在线拉取 crates.io 依赖）
```

Linux/X11 宿主同口径（WSLg 等带真实显示的环境可跑真机探针）：

```bash
cmake -S . -B build -G Ninja -DAURORA_BACKEND_GPU_WGPU=ON -DAURORA_BACKEND_X11=ON
cmake --build build
```

- **Rust 依赖不入库**：仅 `wgpu-native` 自身源码 + `Cargo.lock`（版本确定）入库，crates.io 依赖首次构建由 cargo 在线拉取进本机缓存，缓存命中后支持断网增量构建；cargo 的 `target/` 构建树不入库（`.gitignore`），静态库产物拷贝至 `build/wgpu-native/` 供 IMPORTED 目标引用。国内网络可在用户 cargo 配置（家目录下 `.cargo` 目录内的 config.toml）配 rsproxy 镜像加速：
  ```toml
  [source.crates-io]
  replace-with = 'rsproxy-sparse'
  [source.rsproxy-sparse]
  registry = "sparse+https://rsproxy.cn/index/"
  ```
- **配置期探测**（任一缺项 `FATAL_ERROR`，不静默回退）：`cargo` / `rustc` 在 `PATH`（缺失提示按平台给出安装命令：Windows winget / Linux rustup）；host 三元组与 C++ 编译器 ABI 一致（MinGW 要求 `*-windows-gnu`、MSVC 要求 `*-windows-msvc`、Linux 要求 `*-linux-(gnu|musl)`，否则静态库 ABI 不兼容并给出修复命令）；`libclang` 共享库（wgpu-native 的 `build.rs` 经 bindgen 从 `webgpu.h` 生成 FFI 所必需。定位按「显式传入优先、自动探测兜底」四级：`-DAURORA_LIBCLANG_DIR=<目录>` → 环境变量 `LIBCLANG_PATH` → `PATH` 上的 `clang` 旁目录 → 平台通用默认位（Windows：LLVM 安装器写入的注册表键与 `%ProgramFiles%`；Linux：按 `llvm-*/lib/libclang.so*` 与发行版 `libclang-*.so*` 布局择最高版本）。**CMake 内禁止写死任何本机安装路径**（盘符 / 用户目录一律不得入库——换机即失效且污染他人构建）；显式传入项若不存在即 `FATAL_ERROR`，不静默回退）；`third_party/wgpu-native` 源文件完整。
- **构建接线**：`add_custom_command` 执行 `cargo build --release --target <host 三元组>`（`DEPENDS` `src/*.rs` / `build.rs` / `Cargo.toml` / `Cargo.lock` 做增量），聚合为 `wgpu_native_build` 目标；以 `RUSTUP_TOOLCHAIN=<当前活动工具链完整 id>` 覆盖上游 `rust-toolchain.toml` 的钉版通道（避免 rustup 按宿主启发误装其它 toolchain）。
- **链接面**：`wgpu_native` 静态库 + Rust `windows` crate / libc 族的系统库（Windows：`ws2_32` `userenv` `bcrypt` `advapi32` `oleaut32` `ntdll`；Linux：`dl` `pthread` `m`，Vulkan/Xlib 运行期 dlopen 加载，静态库侧无需 `libx11-dev`——X11 头/库仅 C++ 侧经 `AURORA_BACKEND_X11` 已链接）。`webgpu.h` / `wgpu.h` 仅给库内实现 TU（pimpl 隔离，公共头不外泄三方头）。
- **运行期**：无可用 adapter / 驱动失败 → `WgpuRhi::valid()` false；`create_window(WgpuOptions)` 与 `renderer = GpuWgpu` 强制路由报 `renderer-unavailable`（不降级），`Auto` 偏好优先序不含 wgpu 路径，行为不变。

---

## 4 `AURORA_ENABLE_*`：插桩 / 分析 / 能力开关

不影响库功能，只改工具链参数，用于开发期质量保障。本组除插桩 / 分析外，亦含构建加速（`LLD` / `CCACHE`）与内部能力（`SIMD` / `DEBUG`）等开关，均按 `AURORA_ENABLE_*` 命名组归类。

| 选项 | 默认值 | 含义 | 注入内容 |
|:---|:---|:---|:---|
| `AURORA_ENABLE_COVERAGE` | `OFF` | 行覆盖率（终端摘要，不生成 HTML；按编译器分流） | GCC：`--coverage -O0 -g`（gcov，MinGW 追加 `-Wa,-mbig-obj`，并对 `painter.cpp` 单独提升 `-O1`，见下方约束）；Clang：`-fprofile-instr-generate -fcoverage-mapping -O0 -g`（LLVM 原生 source-based）。均清除默认 `-O3/-Os/-DNDEBUG`、关闭 PCH，提供 `coverage` custom target |
| `AURORA_ENABLE_ASAN` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer | 对所有目标注入 `-fsanitize=address,undefined -fno-omit-frame-pointer -g -O0`；仅 GNU/Clang 生效。Clang + Windows 另有 release CRT 切换与运行时 DLL 暂存的前置决策（顶层 CMakeLists），见下方约束 |
| `AURORA_ENABLE_PROFILING` | `AUTO` | 渲染性能插桩（作用域计时 + 渲染计数器） | 三态，见 §4.1 |
| `AURORA_ENABLE_TRACING` | `OFF` | Chrome Trace Event 时间线落盘 | 注入 `AURORA_ENABLE_TRACING`（PUBLIC 传播 + 安装导出），**并强制**打开 `AURORA_ENABLE_PROFILING` |
| `AURORA_ENABLE_DEBUG` | `AUTO` | 真实后端 DEBUG 能力（截图、控件树、性能快照、可视化调试叠层、控件拾取） | 三态；注入 `AURORA_ENABLE_DEBUG`（**PUBLIC 传播**，经 `aurora_define_feature` 注册）。PUBLIC 的原因：Widget 类在宏下新增数据成员会改变类 ABI 布局，消费者（demo / tests / 宿主应用）必须与库同值，否则构造与成员偏移错位（ODR / 访问冲突）。**随安装导出**（`AuroraConfig.cmake`）且导出面严格对齐**安装产物的实际取值**：强制 ON、或 `AUTO` + 单配置 `Debug`/`RelWithDebInfo` 时导出；`AUTO` + Release（含空构建类型）与强制 OFF 时不导出（无条件导出会让发布态产物反向失配）。调试 API 与测试注入 API 均在头文件始终声明、`.cpp` 体按宏裁切，消费端调用始终可编译、关闭时返回 disabled |
| `AURORA_ENABLE_TEST_HOOKS` | `ON` | 库侧测试注入点（进程内 memory 剪贴板后端等，供仓库私有测试设施并行隔离） | 注入 `AURORA_ENABLE_TEST_HOOKS`（**PUBLIC 传播**，经 `aurora_define_feature` 注册）。注入 API 声明常驻，实现体按 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` **双宏**裁切，任一关闭（含 Release 下 DEBUG AUTO 自动关闭）返回 `false` / no-op，平台行为不变；钩子本身无副作用，真正的行为开关是 `AURORA_ENABLE_DEBUG` |
| `AURORA_ENABLE_SIMD` | `ON` | 光栅内核 SIMD 双实现（SSE2 基线 + AVX2 运行时分发） | 注入 `AURORA_ENABLE_SIMD`（仅库内部，不 PUBLIC 传播）；详见 §4.2 |
| `AURORA_ENABLE_CCACHE` | `ON` | ccache 编译缓存（加速重复编译） | 设置 `CMAKE_C_COMPILER_LAUNCHER` 与 `CMAKE_CXX_COMPILER_LAUNCHER`（`cmake -E env` 前缀注入 ccache 配置环境变量，构建期生效）；支持 winget 安装路径自动检测；详见 §4.3 |
| `AURORA_ENABLE_LLD` | `ON` | 链接器选择（lld 加速静态链接） | GNU/Clang 下 `find_program(ld.lld)` + `check_linker_flag` 探测通过则全局注入 `-fuse-ld=lld -B<lld 目录>`；失败静默回退 GNU ld；**不注入 feature 宏** |
| `AURORA_ENABLE_WASM_PTHREADS` | `OFF` | WASM 真并行（`-pthread`）：`__EMSCRIPTEN_PTHREADS__` 与 `AURORA_CAP_THREADS` 同翻 1，`ThreadPool` 从「任务只入队、帧尾 `pump()` 排空」回到普通 worker 池。**代价在宿主页面**：产物要求 SharedArrayBuffer，须跨源隔离（`COOP: same-origin` + `COEP: require-corp`）方可实例化（裸 Node 无此约束），非 Emscripten 开启 FATAL。故默认关闭——无隔离头的站点宁用单线程 deferred 排空也不换回打不开的产物 | 全局追加 `-pthread` 到 `CMAKE_C_FLAGS` / `CMAKE_CXX_FLAGS` / `CMAKE_EXE_LINKER_FLAGS`（**编译 + 链接同参，且须先于 `add_subdirectory(third_party/*)` 的标志快照**——`-pthread` 是整个链接闭包的约束，漏掉 freetype/harfbuzz 的 `.o` 时链接报 `wasm-ld: --shared-memory is disallowed by harfbuzz.cc.o`，实测；故本体位于根 `CMakeLists.txt` 而非 `AuroraBackends.cmake`）。**不注入 feature 宏**，不出现在 `debug::feature_flags` 镜像，运行期查询用 `ThreadPool::default_pool().is_deferred()` |
| `AURORA_ENABLE_CLANG_TIDY` | `ON` | Clang-Tidy 门禁（`lint` / `lint-fix` 聚合目标） | 需 `clang-tidy` 与 python 在 PATH；未开启时自动打开 `CMAKE_EXPORT_COMPILE_COMMANDS`。经 `tools/check/run_clang_tidy.py` 并行 lint **非 third_party** 翻译单元并按 `(file, line, check)` 去重；详见 §4.5 |
| `AURORA_ENABLE_CLANG_FORMAT` | `ON` | clang-format 门禁（`format` / `format-check` 聚合目标） | 需 `clang-format` 与 python 在 PATH。经 `tools/check/run_clang_format.py` 并行处理 **非 third_party** 源文件（配置源为仓库根 `.clang-format`）；`--fix` 即 `format`，默认只读校验即 `format-check`；详见 §4.7 |
| `AURORA_ENABLE_IMAGE_JPEG` | `OFF` | JPEG 图像解码能力（libjpeg-turbo 源码构建） | 注入 `AURORA_ENABLE_IMAGE_JPEG`（仅库内部，不 PUBLIC 传播）；详见 §4.6 |
| `AURORA_ENABLE_IMAGE_WEBP` | `OFF` | WebP 图像解码能力（libwebp 源码构建） | 注入 `AURORA_ENABLE_IMAGE_WEBP`（同上） |
| `AURORA_ENABLE_IMAGE_PNG` | `OFF` | PNG/GIF 图像解码能力（wuffs 源码构建） | 注入 `AURORA_ENABLE_IMAGE_PNG`（同上） |
| `AURORA_ENABLE_AUDIO` | `OFF` | 内置音频设备后端（`media/audio.h` 的 `AudioContext` 图 API **恒编译**——对齐 RHI 先例：契约常在、能力经 `feature_flags().audio` 运行期查询；本开关只决定是否编入内置设备后端。未启用或设备初始化失败 → `AudioContext` 静默模式：图照常运转、样本消费后丢弃，`device_state()==Silent`，对齐 GPU 通道回退语义）。后端明细见下三行 | 注入 `AURORA_ENABLE_AUDIO`（PUBLIC 传播，经 `aurora_define_feature` 注册） |
| `AURORA_ENABLE_AUDIO_WASAPI` | `AURORA_ENABLE_AUDIO=ON` 时 Windows `ON`，否则 `OFF` | WASAPI 音频设备后端（shared mode event-driven；`AudioContext` 的 Windows 内置默认设备。**前置依赖 `AURORA_ENABLE_AUDIO=ON`**，未开启音频时本开关不生效；非 Windows 开启 FATAL） | 注入 `AURORA_ENABLE_AUDIO_WASAPI`（PUBLIC 传播，经 `aurora_define_feature` 注册；额外链接 `ole32`——COM：MMDevice + IAudioClient，仅 Windows） |
| `AURORA_ENABLE_AUDIO_ALSA` | `AURORA_ENABLE_AUDIO=ON` 时 Linux `ON`，否则 `OFF` | ALSA 音频设备后端（Linux 对位后端：`"default"` 端点 + 简单参数 API，设备线程 avail/writei 驱动，XRUN/挂起自愈、断连退避重开）。**运行时绑定 `dlopen("libasound.so.2")`——零构建期依赖**（不需 libasound dev 包、不链 libasound；库缺失仅运行期 `start` 返回 false → 静默降级）。**前置依赖 `AURORA_ENABLE_AUDIO=ON`**；非 Linux 开启 FATAL | 注入 `AURORA_ENABLE_AUDIO_ALSA`（PUBLIC 传播，经 `aurora_define_feature` 注册；另链 `${CMAKE_DL_LIBS}`——glibc < 2.34 需 `-ldl`，之后为空） |
| `AURORA_ENABLE_AUDIO_WEBAUDIO` | `AURORA_ENABLE_AUDIO=ON` 时 Emscripten `ON`，否则 `OFF` | Web Audio 音频设备后端（浏览器对位后端：`AudioContext` + `ScriptProcessorNode` 消费链 + 主线程定间隔**推式环**缓冲，`src/aurora/media/audio_webaudio.cpp`）。与桌面后端的根本差异是**没有设备线程**——JS 侧回调调不进 wasm，故 C++ 每 20ms 把图渲染成帧推入线性内存环、JS 按头尾两个 int32 地址消费；跨边界只传地址，**零链接标志、零导出符号**（`EXPORT_KEEPALIVE` 默认 0 时 JS 调不到 wasm 函数，`embind` 又是 port）。另有自动播放闸门：`start()` 返回 true 只表示「设备在收样」，**不等于出声**，需用户手势后由排空拍限速 `resume()` 开闸；端到端延迟约 60–150ms，欠载补零并计数。采集（`getUserMedia`）首切片**未接线** → `WebAudioCaptureBackend` 恒 disabled 桩。**前置依赖 `AURORA_ENABLE_AUDIO=ON`**；非 Emscripten 开启 FATAL | 注入 `AURORA_ENABLE_AUDIO_WEBAUDIO`（PUBLIC 传播，经 `aurora_define_feature` 注册；`feature_flags().enable_audio_webaudio` 运行期可查） |
| `AURORA_ENABLE_STORAGE_SQLITE` | `OFF` | 存储层 `SqliteBackend` 后端（记录仓储第三后端：真事务 BEGIN IMMEDIATE/COMMIT/ROLLBACK、二进制载荷 BLOB 内联无 sidecar、`contains`/`clear` 单语句化）。存储门面与 Memory/Filesystem 两后端**恒编译**；本开关只决定是否编入 SQLite 后端与 `Storage::create(SqliteOptions)` 重载。sqlite3 以 amalgamation 源码入库 `third_party/sqlite/`（3.53.4，Public Domain），关闭时链接产物完全不含该组件 | 注入 `AURORA_ENABLE_STORAGE_SQLITE`（PUBLIC 传播，经 `aurora_define_feature` 注册；独立静态目标 `aurora_sqlite3` 编入 `sqlite3.c`，定义 `SQLITE_THREADSAFE=1` / `SQLITE_OMIT_LOAD_EXTENSION`，非 Win 另链 `dl`/`pthread`） |
| `AURORA_ENABLE_GLFW_GPU_GL` | `OFF` | GLFW 的 GPU OpenGL 3.3 core 栅格能力（`GpuGlRhi`：DisplayList 批渲染 + MSAA）。**非独立 `Surface` 后端**——仅为 `GlfwSurface` 的 GPU 栅格模式增强：无 `SurfaceKind`、硬依赖 `AURORA_BACKEND_GLFW`、GL 上下文与呈现由 GLFW 后端承担；未开 `AURORA_BACKEND_GLFW` 配置期 FATAL，GPU 初始化失败运行期自动回退软件纹理上传路径，`Surface::gpu_backend()` 非空时 `name()` 恒为 `"gpu-gl"`。细节见 §3.1 | 注入 `AURORA_ENABLE_GLFW_GPU_GL`（仅库内部，不 PUBLIC 传播；`GpuGlRhi` 类恒编译进库，宏只控制 `GlfwSurface` 是否接线 GPU 模式） |

**约束**：

- `AURORA_ENABLE_COVERAGE` 与 `AURORA_ENABLE_ASAN` **互斥**：同时 `ON` 触发 `FATAL_ERROR`（都改写代码生成）。
- **Clang + Windows（非 MinGW）+ ASan 的 CRT 前置决策**：LLVM 的动态 ASan 运行时（`clang_rt.asan_dynamic-*.dll`）按 release CRT 构建，与 CMake Debug 默认的 `/MDd` 调试 CRT 不兼容——进程退出阶段 `ucrtbased`/`MSVCP140D` 的内部释放被 ASan 判定 bad-free 稳定 abort（构建期生成器因此无法运行）。开启 ASan 时，顶层 CMakeLists 的前置决策块统一切 `MultiThreadedDLL`（含三方 freetype/harfbuzz，避免 `_ITERATOR_DEBUG_LEVEL` 混链 `LNK2038`），并把运行时 DLL 暂存进构建目录（Windows 加载器对 exe 同目录的搜索优先于 PATH，生成器/测试无需手工配置）。Debug 配置的 MSVC 调试 STL 检查由 ASan 顶替。该决策须先于三方 `add_subdirectory`，故位于顶层 CMakeLists 而非 `AuroraInstrumentation.cmake`。
- **Clang 下禁止注入 `--coverage`**：clang 的 gcov 兼容运行时（`llvm_gcda_*`）在 Windows 进程退出刷写 `.gcda` 时稳定崩溃（关闭窗口 / 测试退出即 `0xC0000005`）。现已按 `CMAKE_CXX_COMPILER_ID` 自动分流，同一开关对两套工具链透明。
- **MinGW GCC 两项特例**：(1) `-O0 --coverage` 组合会击穿 COFF 目标文件默认段数上限（大 TU 汇编报 "file too big"），故追加 `-Wa,-mbig-obj`；(2) `-O0 --coverage` 下对带 `target("sse4.1")/target("avx2")` 属性的函数（`painter_simd.inl` SIMD 栅格内核）生成崩溃代码（box blur AVX2 路径运行时 `0xC0000005`，`-O3` 与标量路径均正常），故对 `painter.cpp`（该内核唯一 TU）以源文件级 `-O1` 覆盖，gcov 行映射完整、SIMD 实现行不豁免出统计。
- 覆盖率需覆盖测试目标（大量 widget 是 header-only，仅在测试编译单元中被编译，否则覆盖率严重偏低）。
- 插桩构建（`-O0` 全量插桩）退出前写 profile 较慢：关闭窗口后进程可能需数秒至十余秒才退出，属正常现象。
- **`AURORA_ENABLE_WASM_PTHREADS` 的注入位置在根 `CMakeLists.txt`**（与 `-fexceptions`、ASan 的 CRT 前置决策同处），而非 `AuroraBackends.cmake`：`add_subdirectory(third_party/*)` 建立子目录时对 `CMAKE_C_FLAGS` / `CMAKE_CXX_FLAGS` 取**快照**，而 `AuroraBackends.cmake` 晚于它 include——在那里追加只会让库自身带上 `-pthread`，freetype/harfbuzz 的 `.o` 缺 atomics/bulk-memory 特性，链接期报 `wasm-ld: --shared-memory is disallowed by harfbuzz.cc.o`（实测）。同因，标志也不能只挂在 `aurora` 目标的 `PUBLIC` 选项上。

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
| 确定性约束 | SIMD 路径必须与标量黄金路径**逐位一致**（`-ffp-contract=off` 禁 FMA + 同序浮点运算 + `cvtt` 整型截断）；CI 由 `utest_simd_parity` 逐位比对，一票否决 |

该开关为**纯内部优化开关**：消费者代码与 ABI 均不感知 SIMD 是否存在，关闭后仅损失性能、不改变任何像素输出。开启 SIMD 不引入新的公共 API；相关函数位于 `aurora::detail`（`include/aurora/render/detail/painter_simd.h`），不计入 `aurora_api.json`。

### 4.3 `AURORA_ENABLE_CCACHE`

| 项 | 值 |
|:---|:---|
| 默认值 | `ON` |
| feature 宏 | 不注入，仅设置编译器启动器 |
| 缓存策略 | 压缩（level 6）、默认缓存大小 5G、`SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime`、`BASEDIR=<源码根>` + `NOHASHDIR`（多构建目录共享缓存） |

- **安装方式**：Aurora 作为三方库**不主动安装 ccache**，仅在 `PATH` 中查找；未找到则提示用户自行安装（见 configure 日志），缓存关闭不影响构建正确性。不支持扫描 winget 等安装目录的自动检测。
- **配置注入机制**：CMake 的 `set(ENV{...})` 只在 configure 期生效、不随构建期子进程传递，因此全部 ccache 配置经编译器启动器注入——`CMAKE_{C,CXX}_COMPILER_LAUNCHER = cmake -E env <CCACHE_*>… ccache [<用户选项>]`，在每个编译边构建期展开，精确作用于本项目、不污染全局环境，对 Ninja / Make / Visual Studio 生成器与 GCC/Clang/MSVC 一律适用。
- **SLOPPINESS 各项**：`pch_defines` + `time_macros` 为 PCH 场景必需（缺省时命令行带 `-include cmake_pch.hxx` 的消费者 TU 直接被判 Uncacheable）；`include_file_mtime` / `include_file_ctime` 让头文件时间戳变化而内容不变时仍命中（preprocessor 模式按内容摘要，安全）。
- **配置变量**：`AURORA_CCACHE_DIR`（缓存目录，默认系统默认）、`AURORA_CCACHE_MAXSIZE`（最大缓存，默认 `5G`）。二者同样经启动器注入构建期生效，仅在未设置 `AURORA_CCACHE_OPTIONS` 时作为 Aurora 默认值使用。注意：既有构建目录中已缓存的旧默认值（`2G`）不会自动更新，需显式 `-D` 覆盖。
- **用户自定义选项 `AURORA_CCACHE_OPTIONS`**：字符串，原样透传为 ccache 命令行选项。一旦设置，**直接使用用户输入**，不再注入 Aurora 默认的 `CCACHE_*` 环境配置（用户自行承担完整配置责任，含 PCH 缓存所需的 `--sloppiness=...`）。未设置时使用 Aurora 默认配置。
- **MSVC 豁免**：当检测到的编译器为 MSVC（`cl`）时，即便 `AURORA_ENABLE_CCACHE=ON`，ccache 也会整体被禁用（见 `cmake/AuroraCcache.cmake`）：ccache 对 MSVC 的 `/Yu + /FI + /Fp` PCH 旗标组合支持不完整，改写强制包含会丢失 PCH 边界匹配导致 C1010；且 VS 多配置生成器本就不实现 `<LANG>_COMPILER_LAUNCHER`（Ninja + cl 同样命中）。MSVC 下靠 PCH 提速（MSVC 上 PCH 为正收益），不接入 ccache。

```powershell
cmake -S . -B build -DAURORA_ENABLE_CCACHE=OFF                                  # 禁用
cmake -S . -B build -DAURORA_CCACHE_DIR=<缓存目录> -DAURORA_CCACHE_MAXSIZE=10G   # 自定义（默认分支）
cmake -S . -B build -DAURORA_CCACHE_OPTIONS="--max-size=5G --sloppiness=pch_defines,time_macros,include_file_mtime,include_file_ctime"  # 完全接管配置
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
cmake -S . -B build -DAURORA_LLD_DIR="<LLVM 安装根>/bin"                        # 显式指定
```

### 4.5 `AURORA_ENABLE_CLANG_TIDY`

| 项 | 值 |
|:---|:---|
| 默认值 | `ON`（找不到 `clang-tidy` 或 python 时自动降级：仅告警，不定义目标） |
| 提供目标 | `lint`（存在 warning 及以上即退出码 1）、`lint-fix`（就地应用 fix-it，**不因告警失败**） |
| 配置来源 | 仓库根 `.clang-tidy`（`Checks` / `CheckOptions` / `HeaderFilterRegex`） |
| 扫描范围 | `compile_commands.json` 中全部**非 `third_party/`** 翻译单元 |
| 去重 | 按 `(file, line, check)` 去重——头文件诊断会在每个包含它的 TU 中重复上报，原始条数不可直接用作门禁计数 |
| 依赖 | `clang-tidy`（PATH）+ python（PATH）+ `CMAKE_EXPORT_COMPILE_COMMANDS`（未开启时本模块自动打开） |

为何不用 `CMAKE_CXX_CLANG_TIDY` 随构建执行：该变量必须在目标定义**之前**设置才生效，与本项目「模块在最后 include」的编排冲突；且会让每次编译额外跑一遍 clang-tidy，日常开发构建被拖慢一个数量级。

```powershell
cmake --build build --target lint        # 全量 lint，有告警则失败
cmake --build build --target lint-fix    # 就地应用 fix-it，随后必须人工审阅 diff
python tools/check/run_clang_tidy.py --build-dir build --json-out findings.json  # 结构化清单
python tools/check/run_clang_tidy.py --build-dir build --include 'src/'          # 只 lint 库代码
```

**NOLINT 纪律**：凡用 `NOLINT` / `NOLINTNEXTLINE` 抑制告警，须遵守 `CODING_STANDARDS.md` §5.2——写明具体检查名（禁止裸 `NOLINT` 的新增使用），并紧邻注释说明「为何不能按建议修复」。

**门禁覆盖范围 = 该 build 目录的编译库**：`lint` 扫的是 `--build-dir` 下 `compile_commands.json` 里的 TU，因此只覆盖**该次 configure 实际定义的翻译单元**。浏览器专属件（`EMSCRIPTEN` 门后的 `wasm_*` 实现与 `aurora_verify_wasm_*` 探针）、以及按平台/后端条件定义的真机探针，都不在 Windows/Linux native db 内，`lint` 绿灯不等于它们干净。

**浏览器口径用 `--emscripten`**：`build-wasm` 之类的 db 直接喂给 clang-tidy 不可用——`em++` 是驱动包装器，其真实 argv 里的 wasm 三元组、`__EMSCRIPTEN__`、`include/compat` 之类的垫片目录都由驱动内部注入，且 PCH 由 emsdk 自带 clang 生成（本机 clang-tidy 版本稍差即判 `invalid or out-of-date precompiled header`）。故 `run_clang_tidy.py --build-dir <wasm_db 目录> --emscripten` 先把编译库重写成 native clang-tidy 可消费的形态（换三元组、指 sysroot、弃 PCH、补驱动宏）落到 `<build-dir>/tidy_emscripten/`，再走同一套并行/去重/汇总流程，两种口径的计数因此可比：

```powershell
python tools/check/run_clang_tidy.py --build-dir build-wasm --emscripten   # 浏览器 TU + #ifdef AURORA_BACKEND_WASM 分支
```

两条口径各自的把关面：native 遍覆盖 host 后端与 `#else` 分支，wasm 遍覆盖 `AURORA_PLATFORM_*` 的另一侧（POSIX 派发分支、Web Audio 后端、`wasm_*` 探针）与 libc++ 差异面。CI 两侧都跑：`lint` 作业做 native 双 Pass（DEBUG ON/OFF 各一份编译库），`wasm` 作业在 ctest 之后对同一份 wasm 编译库跑 `--emscripten` 口径；二者互不替代——任一绿灯都不构成另一条通过的证据。

⚠️ CI 里 **clang-tidy 的安装步骤必须排在该作业的 configure 之前**：`AuroraLint.cmake` 在 configure 阶段 `find_program(clang-tidy)`，找不到即 `return()`——`lint` / `lint-fix` 目标根本不会被定义，把安装排在 configure 之后只会让末尾那步报 `no rule to make target 'lint'`。本地同理：换目录重新 configure 时先确认 `clang-tidy` 已在 `PATH`。

**「0 告警」不等于「跑到了」**：TU 编译失败时 clang-tidy 不产出任何带 `[check]` 的诊断，静默下来就是一轮绿灯。故本脚本对**前端 `error:`（含 `fatal error:`、`unable to handle compilation`）与超时**单独记账并以退出码 1 失败，`--emscripten` 重写还会校验条数守恒与 include 目录存在性，任一不满足直接以退出码 2 拒跑。反例实测：用 `shlex.split(posix=True)` 拆 Windows 编译库里的 `command`，会把反斜杠一律当转义符吃掉——盘符与目录粘连、分隔符丢失，路径全废，整轮 TU 编译不过，而门禁显示 0 告警。

**`HeaderFilterRegex` 的路径分隔符缺口（已知，待决策）**：现值为 `(include/aurora|src/aurora|/tests/|/tools/|/examples/)`，只认正斜杠；而编译库里的头文件路径常是「正斜杠根 + 反斜杠段」的混合形态，某一段的分隔符与正则不符，该段之后的头文件告警就被过滤掉。实测后果：同一份 `tests/framework` 下的头文件，收口前 native 遍报 0 条、wasm 遍报十余条（两条口径拼出的路径形态不同，穿过过滤的能力也就不同），因此**当前 native 门禁的有效覆盖面主要是主文件诊断**；把正则改为分隔符无关会一次性放出 `include/aurora` 下的存量头文件告警，属独立专项。

### 4.6 `AURORA_ENABLE_IMAGE_*`（图像编解码能力）

| 项 | 值 |
|:---|:---|
| 默认值 | 均 `OFF` |
| feature 宏 | 注入同名 `AURORA_ENABLE_IMAGE_*`，**仅库内部使用，不 PUBLIC 传播、不导出给消费者** |
| 编译期行为 | `ON` 时对应 third_party 源码（libjpeg-turbo / libwebp / wuffs）编为 OBJECT 库链入 aurora，codec 编译单元以 `#ifdef` 剪裁参与编译；`OFF` 时该格式解码路径不参与编译，消费者需自行提供解码后像素 |
| 运行时影响 | 关闭仅损失解码能力、不改变像素输出；能力查询走 `aurora::debug::feature_flags()`（§11.2 调试门面） |

---

### 4.7 `AURORA_ENABLE_CLANG_FORMAT`

| 项 | 值 |
|:---|:---|
| 默认值 | `ON`（找不到 `clang-format` 或 python 时自动降级：仅告警，不定义目标） |
| 提供目标 | `format-check`（只读校验，任一非 third_party 源文件与 `.clang-format` 不一致即退出码 1）、`format`（就地重写） |
| 配置来源 | 仓库根 `.clang-format`（`BasedOnStyle: Google` + 本仓覆盖项，含 `PointerAlignment: Right`、`DerivePointerAlignment: false`） |
| 扫描范围 | `git ls-files` 中全部**非 `third_party/`、非 `build*/`** 的 `.cpp/.cc/.h/.hpp/.cxx`（无 git 时退化为文件系统遍历） |
| 依赖 | `clang-format`（PATH）+ python（PATH）；**不需要** `compile_commands.json` |

为何要有这道门禁：门禁的作用是让排版漂移在**引入的那一刻**暴露，而不是攒到需要一次性大改。

为何要有独立 runner（而非直接 `clang-format --dry-run --Werror`）：

1. 需要把范围限定在 first-party 源码——`third_party/` 自带各自的 `.clang-format`，不能被重写；
2. 需要并行（千文件级）；
3. 需要稳定的「按文件 / 按总量」摘要供 CI 日志阅读，而不是几百段原始 diff；
4. 需要固定**已知正确**的调用形态：clang-format 解析 `file` 风格时从**实参所在目录**向上查找，用相对路径的 `--assume-filename` 或在不同的 cwd 下运行，都会静默退回内建默认风格、得出方向相反的结论。runner 一律传绝对路径实参并把 cwd 钉在仓库根。

```powershell
cmake --build build --target format-check   # 只读校验，任一处不一致即失败
cmake --build build --target format         # 就地重写，随后必须人工审阅 diff
python tools/check/run_clang_format.py --include 'src/'                 # 只校验库代码
python tools/check/run_clang_format.py --fix --include 'src/aurora/window/'   # 只重写某子树
```

**排版与 NOLINT 的耦合**：`ReflowComments: Always` 会重排注释，可能把 `NOLINTNEXTLINE` 的理由注释折到它与目标行之间，使抑制失效（该形态曾一次性造成 36 条告警）。因此**理由注释一律写在 `NOLINTNEXTLINE` 之前**，且写完改动后须再跑一次 `format-check` 确认幂等。

同一方向还有反过来的一刀：抑制只认**物理行**，而 `ColumnLimit: 120` 会把长语句折行——`NOLINTNEXTLINE` 下方那条语句一旦变成多行，告警所在行就不再是它指向的行，抑制同样静默失效（实测 `cppcoreguidelines-pro-bounds-pointer-arithmetic` 与 `pro-type-reinterpret-cast` 各一处）。故**可能被折行的语句用 `NOLINTBEGIN/NOLINTEND` 覆盖整段**，`NOLINTNEXTLINE` 只留给确定单行的语句；判据以「重跑目标 tidy 归零」为准，别只看写了 NOLINT 就以为已豁免。

**排版与 `EM_JS` / `EM_ASM` 的耦合**：Emscripten 的 `EM_JS` / `EM_ASM` / `MAIN_THREAD_EM_ASM_*` 宏体是 **JavaScript**，而 clang-format 一律按 C++ 解析，会做出三类**破坏语义**的重排：`===` 拆成 `== =`、箭头 `=>` 拆成 `= >`、以及把 `EM_JS` 第三个宏实参（形参列表）的外层括号吃掉（`(const char *x)` → `const char *x`，宏参数数目随即错位）。这些改动**排版门禁查不出、构建期才炸**（WASM 侧报 `expected ';' after top level declarator` 之类），故所有 JS 宏块必须整块排除在排版之外：

```cpp
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(int, wa_available, (), { return typeof AudioContext === 'undefined' ? 0 : 1; });
// clang-format on
```

⚠️ 指令必须**裸写**：clang-format 22 把带尾注的 `// clang-format off  (理由)` 当成普通注释、保护**不生效**（实测静默损坏），理由注释另起一行放在指令上方即可（该处在保护体外，可被正常重排）。落点见 `src/aurora/media/audio_webaudio.cpp`、`src/aurora/window/wasm_aria.cpp`、`include/aurora/window/wasm_surface.h`、`include/aurora/app/application.h` 与 `tools/verify/wasm_*_live_probe.cpp`；新增任何触达 DOM 的 JS 宏时同样处理，并在 `format` 之后核对 JS 块与改前逐字一致。

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

除宏定义外，`aurora_setup_consumer_target`（`cmake/AuroraUtils.cmake`）在 **MinGW** 下对全部消费者目标（demo / 测试 / 工具）追加 `-Wa,-mbig-obj`：MinGW 汇编器的 COFF 目标文件默认段数上限 65535 会被 Debug（`-g`）下的超大消费者 TU 击穿（实测 `demo_google_play.cpp` 达 33614 段，报 "too many sections" / "file too big"），该标志把上限放宽到 2^32 段，产物仍为标准 COFF，对链接器透明；MSVC / clang-cl 的汇编器无此上限，不注入。`AURORA_ENABLE_COVERAGE` 的 GCC 分支出于同一原因亦注入（见 §4 约束）。

---

## 7 运行时 / 测试环境变量

以下变量不进入编译，仅在运行测试 / demo 时被 `std::getenv` 读取：

| 变量 | 取值 | 作用 |
|:---|:---|:---|
| `AURORA_GOLDEN_DIR` | 目录路径 | golden 真值目录；缺省为 `tests/golden` |
| `AURORA_UPDATE_GOLDEN` | 非空（如 `1`） | 把当前渲染覆盖为新的 golden（首次生成 / 主动更新真值）；像素 golden 与 `utest_offscreen` 的逻辑快照基准（`tests/golden/logical_snapshots.json`）共用此变量 |
| `AURORA_GOLDEN_MAX_DIFF` | 整数 | 像素最大允许色差阈值（软件逐位红线的显式放松开关；GPU 容差层 `golden::compare_gpu_tolerance` 不读此旋钮，容差带逐场景申报，见 `specification/03-layout-render.md` §8.4.2） |
| `AURORA_GOLDEN_MAX_PIXELS` | 整数 | 允许不一致像素数上限（同上，仅 `compare_or_update` 族读取） |
| `AURORA_REPO_ROOT` | 目录路径 | 测试框架仓库根定位的显式锚点（`tests/framework/isolation.cpp`）。缺省先按可执行文件位置、再按 cwd 逐级上溯找 `codespec/`+`CMakeLists.txt`；runner 构建 / 安装于仓库外（如 WSL home 目录构建 `/mnt/c` 源码仓）时上溯必然落空，用本变量指向仓库根即可，值须形如仓库根，否则忽略回落自动查找 |
| `AURORA_LIVE_X11` / `AURORA_LIVE_WAYLAND` | 非空（如 `1`） | 后端**真机**单测用例的显式选择加入开关（`utest_x11_surface` / `utest_wayland_surface`）：未置时该用例走 `AURORA_TEST_SKIP` 桩，置了才连接真实 X server / 合成器并创建真实窗口断言端到端接线。默认关闭的原因与探针同源——需要桌面会话、非确定且会动用户屏幕，不进无头 CTest |
| `AURORA_LIVE_ATSPI` | 非空（如 `1`） | Linux AT-SPI2 桥**真机**单测用例（`utest_atspi_bridge.live_embed_handshake_and_teardown`）的选择加入开关：置了才连真实会话总线走完整 dlopen + `Socket.Embed` 握手；未置走 `AURORA_TEST_SKIP` 桩。外部客户端视角（libatspi 逐检查项比对）由探针 `aurora_verify_atspi` 把关，见 `specification/08-tooling.md` §7.5 |
| `NO_AT_BRIDGE` | 非 `0` 即生效 | GNOME 惯例的显式免提开关：置位后 Linux AT-SPI2 桥 `create()` 恒返回 `nullptr`，不碰 libdbus / 总线，无障碍路径整体退出 |
| `AT_SPI_BUS_ADDRESS` | D-Bus 地址串 | 无障碍总线地址显式直给（跳过 `org.a11y.Bus.GetAddress` 查询），用于非常规桌面 / 测试注入；置了但地址无效仍按降级处理 |
| `AURORA_INSPECTOR_PORT` | 1–65535 | `aurora_mcp` 的 `live_*` 工具连接运行中应用的默认端口；缺省 `6280`（与 `InspectorServer::start()` 默认值一致）。单个工具调用可用 `session` 入参（`"6280"` 或 `"127.0.0.1:6280"`）覆盖。主机恒为回环，见 `specification/08-tooling.md` §5.4 |

> CTest 默认 CWD = `build/`，故依赖相对路径的 golden / fixture 测试以仓库根为基准：测试框架启动时统一把 cwd 切换到仓库根（`tests/framework/test_main.cpp` 的 `isolation::setup()`），故 `ctest` 下直接可跑，无需为各用例单独设置 `WORKING_DIRECTORY`（原 14 条 `WORKING_DIRECTORY` 白名单已移除）。

---

## 7.1 `aurora_test_runner` CLI

测试框架的命令行接口（实现于 `tests/framework/test_main.cpp`）。CTest 用例默认以 `--run=<stem>` 单文件粒度调用。

| 参数 | 作用 |
|:---|:---|
| `--run=<suite>` | 只执行指定套件的用例（套件名恒等于测试文件 stem） |
| `--filter=<substr>` | 按用例全名 `Suite.Case` 的子串过滤 |
| `--list` | 列出已注册用例后退出；每行 `Suite.Case` |
| `--format=<fmt>` | 仅对 `--list` 生效：`cases`（默认）或 `suites`（每行一个套件名，供 `registry_integrity` 比对） |
| `--verbose` | 额外输出用例诊断笔记 |
| `--report=<path>` | 结果报告落盘：扩展名 `.xml` → JUnit XML，其余 → JSON；超时同样写入（已完成的部分结果） |
| `--shuffle[=<seed>]` | 打乱用例顺序（暴露顺序依赖）；带种子可复现 |
| `--repeat=<n>` | 把选中集合跑 n 轮（暴露状态泄漏；报告里用例名带 `#轮次`） |
| `--timeout=<ms>` | 本轮总时限。看门狗到点先写报告、再以退出码 `3` 结束（协作式：进程内无法强杀死循环线程，进程级强杀由 CTest 的 `TIMEOUT` 属性承担） |
| `--selftest` | 执行框架内建自检（synthetic 用例，不消费注册表） |
| `-h` / `--help` | 显示帮助 |

框架内部参数（由死亡测试自行拼装，人工不必使用）：`--death-child=<站点键>` 让本进程以死亡测试子进程身份重跑同一用例；
`--death-capture=<file>` 指定子进程把 stderr 接管到哪个采集文件（子进程自行 `freopen`，命令行里因此不需要任何 shell 重定向）。

退出码协议：

| 码 | 含义 |
|:---|:---|
| `0` | 全部通过（Skipped 不计失败） |
| `1` | 至少一个用例失败 |
| `2` | CLI 参数错误、筛选结果为空（含「文件漏写注册宏」）、或报告无法写入 |
| `3` | 超时（看门狗触发；已完成的用例结果仍写入 `--report`） |

---

## 8 标准 CMake 变量

| 变量 | 默认值 | 说明 |
|:---|:---|:---|
| `CMAKE_BUILD_TYPE` | `Release`（若未设） | 常规构建默认 Release；覆盖率 / ASan 开关会自行清除其中的 `-O3` / `-Os` / `-DNDEBUG` |
| `CMAKE_CXX_STANDARD` | `20` | 强制 C++20（`CMAKE_CXX_STANDARD_REQUIRED ON`，`CMAKE_CXX_EXTENSIONS OFF`） |
| 生成器 | — | 推荐 `Ninja`（空转 / 增量调度远快于 Make）；Make 仍支持。GLFW / D3D11 后端链接依赖对应工具链的 `lib-*` 目录 |
| 编译器 | — | GCC / Clang / MSVC（Visual Studio 2022 x64，经 vcvars / VsDevShell 提供 `cl`）均为受支持工具链。MSVC 注记：源码为 UTF-8，构建系统自动加 `/utf-8`；项目统一告警 `-Wall -Wextra -Wpedantic` 仅注入 GCC/Clang 目标（MSVC 保持默认 `/W3`）；`-ffp-contract=off` 的 MSVC 等价为 `/fp:precise` |

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

`Aurora::aurora` 自动传递链接：`freetype` + `harfbuzz` + zlib（FreeType 解压字体表需要，尽力定位；找不到则跳过）+ `winpthread` / `pthread`（HarfBuzz 内部互斥，MinGW 下为 `winpthread`）+ Win32 系统库（`user32 gdi32 shell32 ole32 uuid imm32`，仅 `WIN32`）。消费者**无需**手动 `find_package(FreeType)` / `find_package(HarfBuzz)`。

### 9.3 feature 宏导出约定

安装期将编译期生效的 `AURORA_BACKEND_*` / `AURORA_ENABLE_LAYOUT_CACHE` / `AURORA_ENABLE_OCCLUSION_CULLING` / `AURORA_ENABLE_DISPLAY_LIST` 与全局 `NOMINMAX` 收集进 `AURORA_EXPORTED_DEFINES`，由 `Aurora::aurora` 的 `INTERFACE` 编译定义导出，使消费者以与库**完全一致**的宏集编译 `aurora.h`（避免 ODR / 剪裁不一致）。新增 feature 宏时须同步本段与 `CMakeLists.txt` 的收集列表。

`AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING` 亦为 PUBLIC feature 宏，但导出策略特殊：

- `AURORA_ENABLE_PROFILING=ON`（显式强制）→ 宏对全部配置生效，写入 `AURORA_EXPORTED_DEFINES`；
- `AURORA_ENABLE_PROFILING=AUTO` → 宏由生成器表达式按**构建配置**决定，安装期无法用单一值表达，故**不导出**；消费者若需插桩接口，自行 `-DAURORA_ENABLE_PROFILING` 或安装 `=ON` 的构建；
- `AURORA_ENABLE_TRACING=ON` → 写入 `AURORA_EXPORTED_DEFINES`（其隐含的 PROFILING 亦被强制为 `ON`，一并导出）。

### 9.4 最小验证示例

最小消费端验证由 CI 的 `install-consumer` job 承担：`cmake --install` 后，在工作流内联生成一个仅含 `CMakeLists.txt`（`find_package(Aurora REQUIRED)` + 链接 `Aurora::aurora`）与 `main.cpp`（[`GUIDELINE.md`](GUIDELINE.md) §1 最小配方：`Scene::render_to_png` 出 PNG）的临时工程，端到端验证 `find_package` + 静态链接（含 FreeType / HarfBuzz 传递依赖）在目标工具链上工作。配方：

```powershell
cd consumer
cmake -S . -B build -DCMAKE_PREFIX_PATH="<PREFIX>"
cmake --build build
./build/consumer.exe          # 输出 hello.png
```

> 消费端生成器须与安装库的生成器 / 工具链一致。

---

## 10 快速参考（速查表）

```text
# 产物开关
-D AURORA_BUILD_DEMOS=ON|OFF                  # demos（默认 ON）
-D AURORA_BUILD_TESTS=ON|OFF                  # CTest（默认 ON）
-D AURORA_TEST_SHARDS=<N>                     # 测试 runner 分片数（默认 1 = 单 runner）
-D AURORA_BUILD_INSPECTOR_SERVER=ON|OFF       # Inspector HTTP 服务器（默认 OFF）
-D AURORA_BUILD_VERIFY_TOOLS=ON|OFF           # 真机验收探针 tools/verify/（默认 OFF，EXCLUDE_FROM_ALL）

# 后端开关（= feature 宏，PUBLIC 传播）
-D AURORA_BACKEND_HEADLESS=ON|OFF   # 无头 PNG（默认 ON）
-D AURORA_BACKEND_WIN32=ON|OFF      # Win32/GDI（Win 默认 ON，否则 OFF）
-D AURORA_BACKEND_D3D11=ON|OFF      # D3D11 GPU 上屏（默认 OFF）
-D AURORA_BACKEND_GPU_WGPU=ON|OFF   # wgpu GPU 栅格（默认 OFF；需 Rust 工具链 + libclang，见 §3.8）
-D AURORA_BACKEND_GLFW=ON|OFF       # GLFW/OpenGL（默认 OFF；源码构建）
-D AURORA_BACKEND_X11=ON|OFF        # X11/Xlib（Linux 桌面，默认 OFF）
-D AURORA_BACKEND_WAYLAND=ON|OFF    # 原生 Wayland（Linux 桌面，默认 OFF）
-D AURORA_BACKEND_MACOS=ON|OFF      # macOS（默认 OFF）
-D AURORA_BACKEND_WASM=ON|OFF       # WebAssembly（默认 OFF）
-D AURORA_ENABLE_WASM_PTHREADS=ON|OFF   # WASM -pthread 真并行线程池（默认 OFF；仅 Emscripten；纯构建标志不注 feature 宏；宿主页面须 COOP/COEP，见 §4）
-D AURORA_ENABLE_AUDIO=ON|OFF       # 内置音频设备后端（默认 OFF；图 API 恒编译，关闭=静默模式）
-D AURORA_ENABLE_AUDIO_WASAPI=ON|OFF   # WASAPI 音频（依赖 ENABLE_AUDIO=ON；Win 默认 ON，否则 OFF）
-D AURORA_ENABLE_AUDIO_ALSA=ON|OFF   # ALSA 音频（依赖 ENABLE_AUDIO=ON；Linux 默认 ON，否则 OFF；dlopen 零构建依赖）
-D AURORA_ENABLE_AUDIO_WEBAUDIO=ON|OFF   # Web Audio 音频（依赖 ENABLE_AUDIO=ON；Emscripten 默认 ON，否则 OFF；推式环零导出）
-D AURORA_ENABLE_STORAGE_SQLITE=ON|OFF   # SQLite 存储后端（默认 OFF；存储门面与 Memory/Filesystem 恒编译）

# 架构级优化（= feature 宏，PUBLIC 传播，默认均 ON）
-D AURORA_ENABLE_LAYOUT_CACHE=ON|OFF
-D AURORA_ENABLE_OCCLUSION_CULLING=ON|OFF
-D AURORA_ENABLE_DISPLAY_LIST=ON|OFF

# 插桩（COVERAGE 与 ASAN 互斥）
-D AURORA_ENABLE_COVERAGE=ON|OFF         # gcov / llvm-cov（默认 OFF）
-D AURORA_ENABLE_ASAN=ON|OFF             # ASan/UBSan（默认 OFF）
-D AURORA_ENABLE_PROFILING=AUTO|ON|OFF   # 渲染插桩（默认 AUTO）
-D AURORA_ENABLE_TRACING=ON|OFF          # Chrome Trace（默认 OFF，隐含 PROFILING=ON）
-D AURORA_ENABLE_DEBUG=AUTO|ON|OFF       # 真实后端 DEBUG 能力（默认 AUTO，PUBLIC 宏）
-D AURORA_ENABLE_TEST_HOOKS=ON|OFF       # 库侧测试注入点（默认 ON，PUBLIC 宏；双宏裁切，见 §4）
-D AURORA_ENABLE_SIMD=ON|OFF             # 光栅 SIMD 双实现（默认 ON，内部宏）
-D AURORA_ENABLE_IMAGE_{JPEG,WEBP,PNG}=ON|OFF  # 图像解码能力（默认均 OFF，内部宏）
-D AURORA_ENABLE_GLFW_GPU_GL=ON|OFF    # GPU OpenGL 3.3 core 栅格（默认 OFF；依赖 AURORA_BACKEND_GLFW=ON，非独立后端）
-D AURORA_ENABLE_CCACHE=ON|OFF           # ccache 编译缓存（默认 ON）
-D AURORA_ENABLE_LLD=ON|OFF              # lld 链接器（默认 ON）
-D AURORA_ENABLE_CLANG_TIDY=ON|OFF       # lint / lint-fix 目标（默认 ON）

# 静态检查
cmake --build build --target lint        # 全量 lint（非 third_party，去重后计数）

# 安装 / 消费端
cmake --install build --prefix <PREFIX>
cmake -S app -B app/build -DAurora_DIR="<PREFIX>/lib/cmake/Aurora"

# 运行时（测试）
AURORA_GOLDEN_DIR=<dir> AURORA_UPDATE_GOLDEN=1 ./build/aurora_test_runner --run=utest_offscreen
```
