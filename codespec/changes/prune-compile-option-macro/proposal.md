# CHG-078 收敛编译开关：删恒值宏与手工旋钮

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-078 |
| 提出日期 | 2026-11-16 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.ZERO-IFDEF.001 |
| 影响面 | 构建选项三档共十一项。A 档删宏（含 option 与导出宏）四枚：`AURORA_BACKEND_HEADLESS`（恒注入，纯死守卫清理）、`AURORA_ENABLE_GLFW_GPU_GL`（并入 `AURORA_BACKEND_GLFW`）、`AURORA_ENABLE_SIMD`（恒开，`-ffp-contract=off` 改无条件注入）、`AURORA_ENABLE_TEST_HOOKS`（并入 `AURORA_ENABLE_DEBUG`）；B 档删 option、宏降级为「父开关 ^ 平台」推导五枚：`AURORA_ENABLE_AUDIO_WASAPI` / `AURORA_ENABLE_AUDIO_ALSA` / `AURORA_ENABLE_AUDIO_COREAUDIO` / `AURORA_ENABLE_AUDIO_WEBAUDIO`、`AURORA_ENABLE_WEBVIEW_WEBVIEW2`（宏名、TU 门控与 `feature_flags` 镜像全保留，仅手工 `-D` 旋钮消失）；C 档删内部宏两枚：`AURORA_ENABLE_NET_TLS_SCHANNEL` / `AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT` 并入 `AURORA_ENABLE_NET_TLS_SYSTEM ^ 平台宏`。公共头十二文件（`application.h` / `test_controller.h` / `surface.h` / `window.h` / `native_surfaces.h` / `glfw_surface.h` / `gpu_gl_rhi.h` / `painter_simd.h` / `painter_simd.inl` / `feature_flags.h` / `event.h` / `os_hotkey.h`）去门控与注释改写；实现十三文件（`window_factory.cpp` / `test_controller.cpp` / `notification.cpp` / `scroll_bench.cpp` / `feature_flags.cpp` / `glfw_surface.cpp` / `drag_payload.cpp` / `os_hotkey.cpp` / `system_drag_ingress.h` / TLS 四文件）；`FeatureFlags` 删 `backend_headless` / `simd` 两字段；测试与工具约四十文件去门控与文案改写；`codespec/BUILD_OPTIONS.md` / `codespec/ARCHITECTURE.md` / `codespec/GUIDELINE.md` / `codespec/CODING_STANDARDS.md` / specification 三篇 / `codespec/debug_api.toml` / `codespec/manual-test/10-window.md` / `codespec/manual-test/23-net.md` 文档回写；`.github/workflows/ci.yml` 矩阵同步；根目录 `CHANGELOG.md` 记破坏性变更。 |

## 动机

「能力有没有」的运行时代码已由门面承担（`feature_flags()`、`Engine::capabilities()`、`AudioContext` 静默模式 / `WebView::probe()` 运行期探测 / TLS 注册表运行期按链装配）；编译期宏只剩一类职责——决定这段代码要不要编进产物。据此立一条判据：**一枚编译开关「必要」，当且仅当它承载真实的构建期依赖（三方头 / 链接库 / vendor 源码），或不可由运行时表达的产品语义（凭 ABI、零开销、镜像库选择）。** 以下逐项审计，不满足判据的收敛。

1. **`AURORA_BACKEND_HEADLESS`：恒注入的伪开关。** `cmake/AuroraBackends.cmake` 的 Headless 段对宏无条件注入并随安装导出（注释自述「恒编入，无开关」——测试套件与像素读回回退路径以 `HeadlessSurface` 为基底，不存在受支持的关闭组合）。因此全仓的 `#ifdef AURORA_BACKEND_HEADLESS` 恒真，仅 `src/aurora/perf/scroll_bench.cpp` 的 `#ifndef` 支路恒假，是死代码。实测 38 个文件 131 处引用（范围 `include/`、`src/`、`tests/`、`examples/`、`tools/`）。`feature_flags().backend_headless` 恒真，对消费端不携带任何信息。删除即纯清理：编译面与链接面零字不差，无任何行为分支被激活或关闭。Headless 本体不动——`create_native_window` 在无显示环境（无 `DISPLAY` / `WAYLAND_DISPLAY` 的 CI / SSH / 容器）下的运行期降级路径与像素读回回退都以它为基准，这是能力不是开关。

2. **`AURORA_ENABLE_GLFW_GPU_GL`：与 `AURORA_BACKEND_GLFW` 无差异档的切片。** 该宏只控制 GLFW 后端里 GPU GL（`GpuGlRhi`）接线层是否编译，不引入独立后端、不影响任何公共 API 签名。运行期语义早已由 `GlfwOptions::gpu` 与「初始化失败回退软件路径」承担；「GLFW=ON 且本宏=OFF」的产物与纯软件 GLFW 构建在能力上完全相同，只是少编一段接线。合并进 `AURORA_BACKEND_GLFW` 后，GLFW=ON 即编译 GPU 通道，`gpu_backend()` 的运行期语义不变。代价如实记账：`src/aurora/window/glfw_surface.cpp` 的 GPU 接线包含 `<GL/gl.h>` / `<OpenGL/gl.h>`，合并后任何 GLFW=ON 构建都要求 GL 头在构建机可用——Windows SDK 与 macOS 的 OpenGL.framework 恒备，Linux 需 GL dev 包（与 GLFW dev 包通常同装）；CI 各 GLFW 腿本就带本宏=ON，无新增腿要求。

3. **`AURORA_ENABLE_SIMD`：关闭档不改变语义输出。** 默认 ON；运行期能力分发（CPUID 探测 AVX2 / 回退 SSE2 / 标量尾巴）早已存在，`utest_simd_parity` 以逐位一致门禁保证快路径与标量路径逐位等价。其历史存在理由——MinGW 覆盖率构建下 AVX2 内联函数的编译组合问题——已被两点消化：`-ffp-contract=off` 禁 FMA 融合、函数级 attribute target（`AURORA_AVX2_TARGET`）不依赖全局 `-mavx2`。删宏后 `-ffp-contract=off` 改为无条件注入：它是 SIMD 与标量路径逐位一致的前提，不允许随任何开关消失。

4. **`AURORA_ENABLE_TEST_HOOKS`：无独立行为的第二开关。** 默认 ON；注入点生效条件是 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏交切，真正的行为开关是 `AURORA_ENABLE_DEBUG`（Release 下 AUTO 自动关闭 → 注入点自动失效）。TEST_HOOKS 单独 OFF 的唯一效果是把「已关闭的注入点」再关一次，语义上不存在第三态。并入 DEBUG：声明常驻、`DEBUG=OFF` 时 no-op 的契约逐位不变；「消费者与库同值」的 PUBLIC 口径由 DEBUG 继承。

5. **音频四枚子开关：平台互斥使四枚 option 退化为一个布尔。** `AURORA_ENABLE_AUDIO_WASAPI` / `AURORA_ENABLE_AUDIO_ALSA` / `AURORA_ENABLE_AUDIO_COREAUDIO` / `AURORA_ENABLE_AUDIO_WEBAUDIO` 各有「非本平台开启即 FATAL」守卫，任何一台构建机至多一枚可为 ON；默认值已按平台写死（本平台 ON、其余 OFF）。四枚 option 的实际自由度只剩「把本平台设备后端关掉」一档，而该 OFF 档没有独立产品语义——`AudioContext` 静默降级 `enabled=false` 是运行期缺失，不是裁剪后的能力。改为 `AURORA_ENABLE_AUDIO=ON` 时按平台推导注入：`WIN32 → AURORA_ENABLE_AUDIO_WASAPI`、`LINUX → AURORA_ENABLE_AUDIO_ALSA`、`APPLE → AURORA_ENABLE_AUDIO_COREAUDIO`、`EMSCRIPTEN → AURORA_ENABLE_AUDIO_WEBAUDIO`；宏名、TU 门控与 `feature_flags` 的 `enable_audio_*` 四字段全部保留，默认组合逐位不变。如实记账的能力收缩：`AUDIO=ON` 时无法再把本平台设备后端 TU 裁出产物（此前 WASAPI=OFF 可裁）；该诉求由「不编 AUDIO + 自定义 `AudioDeviceBackend` 构造注入」覆盖，注入路径恒在。

6. **`AURORA_ENABLE_WEBVIEW_WEBVIEW2`：OFF 档是编译面逃生门，不是产品语义。** Windows 上 WebView2 是运行期 `LoadLibrary` 的零链接依赖，TU 级 PRIVATE 条件编译；WEBVIEW=ON 且 WIN32 时推导注入宏。OFF 组合（WEBVIEW=ON 但不编 WebView2 TU）此前只服务「不想要该引擎 TU」的极小体积诉求，消失后由 `WebView::probe()` 运行期探测与占位降级兜底。

7. **`AURORA_ENABLE_NET_TLS_SCHANNEL` / `AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT`：`AURORA_ENABLE_NET_TLS_SYSTEM ^ 平台` 的纯函数。** `cmake/AuroraNetTls.cmake` 在 SYSTEM=ON 时按平台从两枚里挑一枚注入，PRIVATE、不进 `feature_flags`、消费者编译面本不含。改为 SYSTEM=ON 时注入 `AURORA_ENABLE_NET_TLS_SYSTEM`（SCOPE PRIVATE），四处 TU 门控改写为 `AURORA_ENABLE_NET_TLS_SYSTEM` 与 `AURORA_PLATFORM_WINDOWS` / `AURORA_PLATFORM_MACOS` 的组合——平台分发回归编译目标宏本身。三个手工旋钮保留：`AURORA_ENABLE_NET_TLS_SYSTEM`（不支持平台置 ON 即 configure 硬失败）、`AURORA_NET_TLS_PROVIDERS`（provider 安装根目录，引擎实由运行期探测推导）、`AURORA_ENABLE_NET_TLS_MBEDTLS`（vendored 源码构建）——这组是链接期库选择（系统库 / vendored mbedTLS），运行期探测替代不了「库编没编」；Linux 缺宏时 `capabilities().tls == false`、无明文回退，是合法语义而非开关冗余。

### 同步明确不删，记录裁决防止复漂移

- `AURORA_ENABLE_AUDIO`：默认 OFF 是真实产品决策（设备后端 TU 体量不小：WASAPI COM 事件驱动、CoreAudio `AudioQueue` 双端），且静默模式给了 OFF 诚实落点。保留。
- `AURORA_ENABLE_WEBVIEW_WEBKITGTK`：引擎编译需要 GTK3 / WebKitGTK dev 头——WebView 唯一真实的构建机依赖，CI Linux 显式 OFF 正是为绕开没装 dev 包的机器。保留（远期可改 pkg-config 软探测，但那会使构建结果随机器漂移，与 X11 / Wayland 的显式 REQUIRE 风格相悖，不在本批）。
- `AURORA_ENABLE_NET_TLS_SYSTEM` / `AURORA_NET_TLS_PROVIDERS` / `AURORA_ENABLE_NET_TLS_MBEDTLS`：见动机第 7 条。保留。
- `AURORA_ENABLE_COMPRESSION`：OFF 侧承载「极小体积构建 / 仅自定义解压器」的真实配置面，toggles 有 `compression-off` 腿隐证诚实降级路径。保留。
- `AURORA_ENABLE_DEBUG` / `AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING`：前者在宏下新增 `Widget` 成员、改 ABI，PUBLIC 编译期定义是 ODR 硬要求；后两者契约是「宏关闭时零指令」，改运行时判断即每作用域引入一次读内存，违反零开销设计。保留。
- 其余编译开关（`AURORA_BACKEND_*` 七枚、`AURORA_ENABLE_IMAGE_*` / `AURORA_ENABLE_STORAGE_SQLITE`）：三方头与链接库的编译期决定，无运行时对应物；能力查询已由运行期门面承担。保留。

## 变更内容

行号在实施时以全仓检索定位；本节按文件与符号粒度冻结锚面。

### 范围裁决

| 项 | 处置 | 破坏面 |
|:---|:---|:---|
| `AURORA_BACKEND_HEADLESS` | A：删宏（恒注入） | 导出宏删除；`FeatureFlags` 删 `backend_headless` 字段 |
| `AURORA_ENABLE_GLFW_GPU_GL` | A：删宏（并入 `AURORA_BACKEND_GLFW`） | 导出宏删除；`GlfwOptions::gpu` 成唯一真源 |
| `AURORA_ENABLE_SIMD` | A：删宏（恒开） | PUBLIC 编译定义不再下发；`FeatureFlags` 删 `simd` 字段 |
| `AURORA_ENABLE_TEST_HOOKS` | A：删宏（并入 `AURORA_ENABLE_DEBUG`） | PUBLIC 编译定义不再下发；默认组合行为逐位不变 |
| `AURORA_ENABLE_AUDIO_WASAPI/ALSA/COREAUDIO/WEBAUDIO` | B：删 option、宏改 `AUDIO ^ 平台` 推导 | 仅手工 `-D` 旋钮消失；宏、TU 门控、`feature_flags` 四字段全保留 |
| `AURORA_ENABLE_WEBVIEW_WEBVIEW2` | B：删 option、宏改 `WEBVIEW ^ WIN32` 推导 | 仅手工 `-D` 旋钮消失（PRIVATE） |
| `AURORA_ENABLE_NET_TLS_SCHANNEL/SECURE_TRANSPORT` | C：删内部宏，并入 `SYSTEM ^ 平台` | PRIVATE，消费者编译面本不含；`AURORA_ENABLE_NET_TLS_SYSTEM` 改注入同名 PRIVATE 宏 |

### 构建与导出

- `cmake/AuroraBackends.cmake`：
  - Headless 段：删 `aurora_define_feature(AURORA_BACKEND_HEADLESS EXPORT)` 调用，注释改写为「恒编入，无 feature 宏」（该段现有注释已自述恒编入，本批去掉最后一行注入）。
  - GLFW GPU 段：删 `AURORA_ENABLE_GLFW_GPU_GL` 的 `option` 块整段（含 `aurora_error` 依赖守卫与日志行）。
  - 音频段：删四枚 `option()` 与「非本平台开启 FATAL」守卫，改为 `AURORA_ENABLE_AUDIO=ON` 时按平台推导注入——`WIN32 → AURORA_ENABLE_AUDIO_WASAPI`（EXPORT）、`LINUX → AURORA_ENABLE_AUDIO_ALSA`（EXPORT + `CMAKE_DL_LIBS`）、`APPLE → AURORA_ENABLE_AUDIO_COREAUDIO`（EXPORT + `AudioToolbox`/`CoreAudio` framework）、`EMSCRIPTEN → AURORA_ENABLE_AUDIO_WEBAUDIO`（EXPORT）；日志行收敛为一条。文件头注释的音频子开关列举改写为「由父开关按平台推导」。
  - WebView 段：删 `AURORA_ENABLE_WEBVIEW_WEBVIEW2` 的 `option` 与 FATAL 守卫；`WEBVIEW=ON 且 WIN32` 时注入该宏（SCOPE PRIVATE）；`AURORA_ENABLE_WEBVIEW_WEBKITGTK` 段原样保留。
- `cmake/AuroraSimd.cmake`：删 `option(AURORA_ENABLE_SIMD ...)` 与 `aurora_define_feature(AURORA_ENABLE_SIMD)` 调用及 OFF 日志；`-ffp-contract=off`（MSVC `/fp:precise`）改为无条件注入——它是 SIMD 与标量路径逐位一致的前提，不允许随任何开关消失。
- `cmake/AuroraInstrumentation.cmake`：删 `AURORA_ENABLE_TEST_HOOKS` 的 `option` 块与日志行；「注入点生效条件」注释随双宏改单宏。
- `cmake/AuroraNetTls.cmake`：系统栈不再注入 `AURORA_ENABLE_NET_TLS_SCHANNEL` / `AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT`，改为 SYSTEM=ON 时注入 `AURORA_ENABLE_NET_TLS_SYSTEM`（SCOPE PRIVATE）；「不支持平台置 ON 即 FATAL」守卫保留；provider 探测与链接目标不动。系统栈的链接面与日志同步改写——现有系统栈链接按平台判定（`WIN32 → schannel` / `APPLE → SecureTransport`），平台判定宏不依赖被删宏；`configure` 日志 `tls backend: system stack (...)` 的括注随宏名改写。
- `cmake/AuroraVerify.cmake`：`aurora_verify_glfw_gpu_features` 的门控条件从 `AURORA_BACKEND_GLFW 且 AURORA_ENABLE_GLFW_GPU_GL` 收窄为 `AURORA_BACKEND_GLFW`；音频 / WebView 探针门控条件不变（B 档宏仍按推导注入，同名宏继续存在）；探针注释与 `configure` 日志文本随宏名同步。
- `cmake/AuroraInstall.cmake`：无需改动——`AURORA_FEATURE_DEFINES` 随 A 档两处 EXPORT 调用点消失自动收敛（`AURORA_BACKEND_HEADLESS` 与 `AURORA_ENABLE_GLFW_GPU_GL` 从安装导出清单移除）；B 档五枚宏仍按推导注入并保持原导出属性，默认构建的导出面不变。

### 公共头

- `include/aurora/app/application.h`：Headless 分支守卫拆除（HeadlessSurface 工厂分支无宏条件）。
- `include/aurora/app/test_controller.h`：整头 `#ifdef AURORA_BACKEND_HEADLESS` 守卫拆除；头注释「默认 ON / 关闭时声明」改写为「无头后端恒编入，本头恒声明」。
- `include/aurora/window/surface.h`：HeadlessSurface 段守卫拆除；「由 `AURORA_BACKEND_HEADLESS` 宏恒定义」的指涉注释改写为「无头后端随库编入，无开关」。
- `include/aurora/window/window.h`：后端宏清单注释的 HEADLESS 行改写；`GlfwOptions::gpu` 字段注释去第二开关指涉。
- `include/aurora/window/native_surfaces.h`：聚合头注释「由宏控制」改「恒编入」。
- `include/aurora/window/glfw_surface.h`：`gpu_backend()` 注释改写——「GLFW 构建即含 GPU 通道；运行期初始化失败回退软件路径时为 `nullptr`」。
- `include/aurora/render/rhi/gpu_gl_rhi.h`：注释去宏名指涉（与 GLFW 后端同在）。
- `include/aurora/render/detail/painter_simd.h`：两处 `#ifdef AURORA_ENABLE_SIMD` 拆除（声明块无条件编译）；`AURORA_SIMD_X86_64` 架构门保留。
- `include/aurora/render/detail/painter_simd.inl`：实现块外层 `#ifdef AURORA_ENABLE_SIMD` 拆除；`AURORA_AVX2_TARGET` 等函数级 attribute 不动。
- `include/aurora/debug/feature_flags.h`：删 `backend_headless` 与 `simd` 两字段；字段注释同步（恒开且无开关的能力不再入镜像，对齐布局缓存三枚的既有先例）；音频 / WebView 字段不动。
- `include/aurora/event/event.h` 与 `include/aurora/app/os_hotkey.h`：双宏 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 的注释表述改为单宏 `AURORA_ENABLE_DEBUG`。

### 实现

- `src/aurora/window/window_factory.cpp`：五处 HEADLESS 守卫拆除，含 `#elif` 链中的恒真支路（无头回退分支成为无条件回退）。
- `src/aurora/app/test_controller.cpp`：文件级 `#ifdef AURORA_BACKEND_HEADLESS` 守卫拆除。
- `src/aurora/app/notification.cpp`：平台链中的 `#ifdef AURORA_BACKEND_HEADLESS` 支路改为无条件回退支路。
- `src/aurora/perf/scroll_bench.cpp`：`#ifndef AURORA_BACKEND_HEADLESS` 死分支与 SKIP 日志删除。
- `src/aurora/debug/feature_flags.cpp`：删两字段镜像、对应 `#ifdef` 与两个 JSON 键；音频 / WebView 字段镜像不动（推导注入保持同名宏）。
- `src/aurora/window/glfw_surface.cpp`：十一处 `#ifdef AURORA_ENABLE_GLFW_GPU_GL` 拆除（GPU 接线无条件编译；DEBUG 读回分支不动）。
- `src/aurora/event/drag_payload.cpp` 与 `src/aurora/app/os_hotkey.cpp`：`#if defined(AURORA_ENABLE_DEBUG) && defined(AURORA_ENABLE_TEST_HOOKS)` 改单宏 `AURORA_ENABLE_DEBUG`；文件内测试后端宏（`AURORA_SYSTEM_DRAG_TEST_BACKEND` / `AURORA_OS_HOTKEY_TEST_BACKEND`）的 `#define` 条件随之简化，用点不动。
- `src/aurora/event/detail/system_drag_ingress.h`：双宏表述改单宏。
- TLS 四文件：`src/aurora/net/tls_schannel.cpp` 与 `src/aurora/net/tls_backend.cpp` 中 `AURORA_ENABLE_NET_TLS_SCHANNEL` 门控改写为 `AURORA_ENABLE_NET_TLS_SYSTEM && AURORA_PLATFORM_WINDOWS`；`src/aurora/net/tls_securetransport.cpp` 与 `src/aurora/net/detail/tls_backend.h` 中 `AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT` 门控改写为 `AURORA_ENABLE_NET_TLS_SYSTEM && AURORA_PLATFORM_MACOS`。
- 音频 / WebView 实现无需改动：`src/aurora/media/audio.cpp` / `src/aurora/webview/engine_webview2.cpp` 的文件级门控在 B 档下仍按同名宏生效。

### 测试

去门控保留断言；SKIP 文案随宏删除改写为普通语义（不再罗列已删除宏名）。

- 集成（HEADLESS）：`tests/integration/itest_ai_compat.cpp` / `itest_dirty_clip_paint.cpp` / `itest_keyboard_nav.cpp` / `itest_nav_win.cpp` / `itest_occlusion_culling.cpp` / `itest_relayout_boundary.cpp` / `itest_reorder.cpp` / `itest_selection_paint_only.cpp` / `itest_window_options.cpp`。
- 集成（GLFW GPU）：`tests/integration/itest_gl_golden.cpp` / `itest_gpu_gl_smoke.cpp` 门控条件从 `defined(AURORA_BACKEND_GLFW) && defined(AURORA_ENABLE_GLFW_GPU_GL)` 收窄为 `#ifdef AURORA_BACKEND_GLFW`。
- 单元（HEADLESS）：`tests/unit/utest_surface.cpp` / `utest_test_controller.cpp` / `utest_native_surfaces.cpp` / `utest_file_dialog.cpp` / `utest_e2e_harness.cpp` / `utest_notification.cpp` / `utest_window_factory.cpp` / `utest_application.cpp`。
- 单元（其余）：`tests/unit/utest_simd_parity.cpp` 守卫收窄为 `defined(AURORA_SIMD_X86)`（非 x86 仍按架构在用例体内 SKIP；`AURORA_ENABLE_SIMD` 关闭档分支删除）；`tests/unit/utest_feature_flags.cpp` 删 `backend_headless` / `simd` 两行表项、把「全部 23 个归一化镜像字段」计数注释改 21、删 `value_initialized_flags_are_all_false` 内两条对应断言（数量断言随表自动收敛）；`tests/unit/utest_system_drag_ingress.cpp` / `utest_os_hotkey.cpp` 的 SKIP 文案双宏改单宏。
- 音频 / WebView 用例无需改动：`tests/unit/utest_audio.cpp` / `utest_audio_alsa.cpp` / `utest_audio_coreaudio.cpp` / `utest_audio_webaudio.cpp` 等门控宏在 B 档下仍按推导注入。

### 工具与示例

- `tools/bench/bench_render.cpp` / `tools/bench/bench_theme.cpp` / `tools/include/e2e/harness.h`：HEADLESS 守卫删除与注释改写（`tools/include/bench_result.h` 的 `#elif` 支路改无条件回退）。
- `tools/verify/glfw_gpu_features_live_probe.cpp`（SKIP 文案与门控随 `AURORA_ENABLE_GLFW_GPU_GL` 删除收窄）、`tools/verify/win32_os_hotkey_live_probe.cpp`（注入前提文案随双宏改单宏）、`tools/verify/x11_cursor_live_probe.cpp`（编译示例去 HEADLESS 段）。
- 音频 / WebView 探针无需改动：`tools/verify/wasapi_audio_live_probe.cpp` / `coreaudio_audio_live_probe.cpp` / `alsa_audio_live_probe.cpp` / `wasm_audio_live_probe.cpp` / `webkitgtk_webview_live_probe.cpp` 引用的宏名在 B 档下不变。
- `examples/demos/demo_test_controller.cpp`：HEADLESS 守卫删除与提示文案改写。

### CI（`.github/workflows/ci.yml`）

- 头注释（GLFW GPU / SIMD / TEST_HOOKS 字样）与各腿说明随宏删除同步改写。
- 删除各腿的 `-DAURORA_ENABLE_GLFW_GPU_GL=ON`（GLFW GPU 随 GLFW 后端打开）。
- `lint` 主遍 configure：删 `-DAURORA_ENABLE_SIMD=ON` 与 `-DAURORA_ENABLE_TEST_HOOKS=ON`（恒开），保留 `-DAURORA_ENABLE_DEBUG=ON`。
- `lint-polarity`：删 `simd-testhooks-off` 矩阵项整项（含其独立编译库 job 与 `-DAURORA_ENABLE_SIMD=OFF`）；`debug-off` 项 flags 只留 `-DAURORA_ENABLE_DEBUG=OFF`，token 选集去 `AURORA_ENABLE_TEST_HOOKS`。
- B / C 档无需新增腿：`toggles` 的 `optional-subsystems-on` 腿（Linux 下 ALSA 推导）、`backends` Windows 腿（WASAPI）、macOS 能力腿（CoreAudio）、wasm 腿（WebAudio）与 `tls-openssl-shared` 反极性腿已天然覆盖推导与双引擎并存；CI 各腿现有 `-D` 面不含已删旋钮。
- 已核实无需改：`ci.yml` 的 Windows lint token 选集（含 `AURORA_ENABLE_AUDIO_WASAPI`，该宏 B 档保留）与各腿 `-D` 面均未传已删旋钮。

### 文档

- `codespec/BUILD_OPTIONS.md`：§1.1 模块表（AuroraBackends 行去音频子开关列举与 GLFW GPU 字样、AuroraSimd 行表述）；§1.2 TLS 引擎宏表——`SCHANNEL` / `SECURE_TRANSPORT` 行改写为「由 `AURORA_ENABLE_NET_TLS_SYSTEM ^ 平台宏` 推导，不再注入独立引擎宏；SYSTEM=ON 注入同名 PRIVATE 宏」；§3 后端表去 `AURORA_ENABLE_GLFW_GPU_GL`、§3.3 恒开表补 SIMD；§4 的音频四行改写为「`AURORA_ENABLE_AUDIO=ON` 时按平台推导」、§4.2 整节改写为「恒开（无开关）」；§4.5 lint-polarity 段（`simd-testhooks-off` 腿删除、`tidy-findings` 清单名收敛）与「TEST_HOOKS 默认 ON」表述；§6 固定注入表 HEADLESS 行改写；§10 速查表删已删旋钮行。
- `codespec/ARCHITECTURE.md`：后端表面表（Headless 行、`GlfwSurface` 与 `GpuGlRhi` 行）、音频子开关表述、CI 矩阵描述（backends 腿与 toggles 腿）与门禁矩阵行。
- `codespec/GUIDELINE.md`：GPU 模式配方去 `-DAURORA_ENABLE_GLFW_GPU_GL=ON`；`feature_flags` JSON 示例去两键；调试注入双宏表述改单宏。
- `codespec/CODING_STANDARDS.md`：SIMD 开关描述改恒开。
- `codespec/specification/03-layout-render.md`：GPU GL 门控表述、Headless 行、`GpuGlRhi` 表述；音频四后端（WASAPI / ALSA / CoreAudio / Web Audio）开关表述改为「`AURORA_ENABLE_AUDIO=ON` 时按平台推导」。
- `codespec/specification/06-app-platform.md`：测试注入双宏表述删减；调试门面（`FeatureFlags` 字段描述）；音频 / WebView / TLS 装配的子开关表述改写（全仓检索定位）。
- `codespec/specification/08-tooling.md`：SIMD 对比语义 / `TestController` 门控表述。
- `codespec/debug_api.toml`：`feature_flags` JSON 示例去两键，随后刷新根目录 `aurora_api.json`。
- `codespec/manual-test/10-window.md`：宏清单行改写；`codespec/manual-test/23-net.md`：C 档系统栈日志文本（宏名 → 引擎名）与 `TC-NET-001` 判据同步。
- 根目录 `CHANGELOG.md`：顶部当前版本段追加 Breaking / Changed 条目与迁移路径。

## 兼容性与迁移

- A 档四枚：`-DAURORA_BACKEND_HEADLESS=OFF` 等传入后成为未使用的缓存变量，不报错、不改变任何行为（对齐「架构级优化恒开」的既有口径：`-DAURORA_ENABLE_SIMD=OFF` 不再关闭任何能力）。
- 以 A 档导出宏做条件编译的消费者：`#ifdef AURORA_BACKEND_HEADLESS` / `AURORA_ENABLE_GLFW_GPU_GL`——两能力恒真，删除引用即可。
- 以 `feature_flags.backend_headless` / `simd` 字段或对应 JSON 键为依据的工具：删除读取分支——SIMD 的运行期能力已由 CPUID 分发承担，无需查询。
- 以 `AURORA_BACKEND_HEADLESS` / `AURORA_ENABLE_GLFW_GPU_GL` 做构建配置判定的消费者：随宏删除消失——编译面不变，运行期初始化失败自动回退软件路径，`Surface::gpu_backend()` 语义不变。
- `-DAURORA_ENABLE_TEST_HOOKS=OFF` 组合消失：钩子生效条件由 DEBUG 单宏继承；默认组合（Debug 开 / Release 关）行为逐位不变。
- B 档五枚手工旋钮消失：`-DAURORA_ENABLE_AUDIO_WASAPI=ON` 不再被读取（即默认行为）；`OFF` 不再关闭本平台设备后端。「音频 ON 但无本平台设备后端」的场景由自定义 `AudioDeviceBackend` 构造注入覆盖（注入路径恒在；与被裁掉的 OFF 档不等价——注入只能顶替行为，不能把 TU 从产物裁出，属如实记账的能力收缩）；「WEBVIEW=ON 但无 WebView2 引擎」同理由 `WebView::probe()` 运行期探测覆盖与降级。
- C 档宏变化仅库内部：`AURORA_ENABLE_NET_TLS_SCHANNEL` / `AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT` 不再注入（PRIVATE，消费者编译面本不含），改为 `AURORA_ENABLE_NET_TLS_SYSTEM ^ 平台宏` 门控；三个 TLS 手工旋钮语义不变（含不支持平台 FATAL）。
- 导出面变化：源码树（`add_subdirectory`）消费者此前经 PUBLIC 编译定义收到的 `AURORA_ENABLE_TEST_HOOKS` / `AURORA_ENABLE_SIMD` 不再下发；安装态消费者本就不收到这两枚（未 EXPORT），导出清单变化仅限 `AURORA_BACKEND_HEADLESS` / `AURORA_ENABLE_GLFW_GPU_GL` 两枚。
- 导出宏与公共结构体字段删除属破坏性变更，按 semver 记入根目录 `CHANGELOG.md` 并给迁移路径，版本处理遵循仓库现行版本打标。

## 不变量与边界

- `-ffp-contract=off` 无条件保留；`utest_simd_parity` 五组三元组逐位一致判据与标量黄金路径不变。
- 默认构建等于默认组合逐位不变：`AURORA_ENABLE_DEBUG` 的三态行为不变，注入 API 声明常驻的契约不变。
- 测试钩子开启后默认组合逐位不变：`feature_flags` 镜像除被删两字段外逐字段相同。
- B 档推导只改「旋钮存在性」，不改变任何默认构建的宏面：`AURORA_ENABLE_AUDIO=ON` 的平台后端组合与今日缺省完全一致，`feature_flags` 镜像层与 JSON 除被删两键外逐位不变。
- 无头后端恒编入，`HeadlessSurface` 作为测试基底与读回回退路径的既有约定不变；GPU 初始化失败运行期回退软件路径的契约不变；TLS「无引擎即 `capabilities().tls == false`、无明文回退」的安全语义不变。
- 不触碰 `AURORA_ENABLE_DEBUG` / `AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING` 的三态与 ABI；不触碰平台 / 架构 / 编译器目标宏；不做与本次无关的重构、改名或格式化。
- `codespec/changes/` 下历史提案不改。

## 验收判据

1. 全仓检索：已删的六枚宏名（A 档四枚、C 档两枚）在源码、公共头、CMake `option()` 调用、CI、现存规格文档中零残留；B 档五枚宏名仍存在于推导注入点、TU 门控、`feature_flags` 镜像与文档中——文档表述须为「由父开关 ^ 平台推导」而非「删除」；`codespec/changes/` 下历史提案与本提案的迁移性表述可含。
2. `ctest --preset ninja-test` 全绿，含 `check_platform_macros`、`check_declared_gates`、`check_change_proposals`、`check_codespec_xref`、`check_code_doc_sync`、`check_doc_comments`、`check_nolint_layout` 与 `format-check`。
3. `utest_feature_flags` 断言 21 个字段与 JSON 键一一对应；`utest_simd_parity` 在默认构建下真实对比全绿（不再有 OFF 档 SKIP），非 x86 目标仍按架构 SKIP 且 `registry_integrity` 不漂移。
4. `AURORA_BACKEND_GLFW=ON` 构建下 `itest_gl_golden` / `itest_gpu_gl_smoke` 不再依赖第二开关即编进 GL 栅格路径；`AURORA_ENABLE_DEBUG=OFF` 构建下全部注入 API 可编译且返回 no-op（原双宏组合语义由 DEBUG 单宏继承）。
5. `cmake --build --target aurora_api_json` 刷新后 `check_api_schema_sync` 绿，`codespec/debug_api.toml` 的 `feature_flags` 示例与 `FeatureFlags` 实体一致。
6. CI 矩阵同步后 toggles 与 lint 全量腿绿；`optional-subsystems-on` / backends Windows / macOS 能力 / wasm 各腿验证 B 档推导——`AURORA_ENABLE_AUDIO=ON` 时平台设备后端自动编入（Windows→WASAPI、Linux→ALSA、macOS→CoreAudio、wasm→WebAudio），且 `feature_flags` 的 `enable_audio_*` 四字段与平台一致。
7. Headless 段为纯守卫拆除：改动前后静态库符号集一致（GCC / MinGW 以 `nm -g` 定义列表比对），与已删宏名全仓零残留两条并立（符号面与预处理面分开取证）。
8. B 档推导集：API 导出清单无新增，消失的符号面仅来自被删的接线层，作为清理量证据。
9. C 档回归：`tls-openssl-shared` 反极性腿（provider + 系统栈并存）与系统栈配置腿（Windows / macOS）绿，证明 `SYSTEM ^ 平台` 门控下 Schannel / SecureTransport TU 与注册表装配无回归。
10. 推送前本地实跑：改动 TU 的增量 clang-tidy（`tools/check/select_lint_tus.py` 选单 + `tools/check/run_clang_tidy.py` 执行两步，与 CI `lint-incremental` 同口径）零 findings，`format-check` 幂等。

## 实施步骤

- [ ] CMake（A 档）：`cmake/AuroraBackends.cmake`（HEADLESS / GLFW GPU 两段）、`cmake/AuroraSimd.cmake`（option 删除 + `-ffp-contract=off` 无条件化）、`cmake/AuroraInstrumentation.cmake`（TEST_HOOKS）、`cmake/AuroraVerify.cmake`（GLFW GPU 探针条件）。
- [ ] CMake（B / C 档）：`cmake/AuroraBackends.cmake` 音频四段 option 删除与推导注入、WebView2 option 删除与推导注入；`cmake/AuroraVerify.cmake` 日志文本；`cmake/AuroraNetTls.cmake` 系统栈改注入 `AURORA_ENABLE_NET_TLS_SYSTEM`。
- [ ] 公共头去门控与注释：`application` / `test_controller` / `surface` / `window` / `native_surfaces` / `glfw_surface` / `gpu_gl_rhi` / `painter_simd.h` 与 `.inl` / `feature_flags` / `event` / `os_hotkey`。
- [ ] 实现文件去门控与注释：`window_factory` / `test_controller` / `notification` / `scroll_bench` / `feature_flags.cpp` / `glfw_surface.cpp` / `drag_payload` / `os_hotkey` / `system_drag_ingress.h`；TLS 四文件（`src/aurora/net/tls_schannel.cpp` / `src/aurora/net/tls_securetransport.cpp` / `src/aurora/net/tls_backend.cpp` / `src/aurora/net/detail/tls_backend.h`）。
- [ ] 测试与工具：上列集成 / 单元 / 工具 / 示例文件去门控与文案改写；`utest_feature_flags` 删两行表项、计数注释改 21、删两条对应断言。
- [ ] `ci.yml`：头注释 / 能力矩阵 / toggles / lint 主遍 / lint-polarity 同步。
- [ ] 文档回写：`BUILD_OPTIONS` / `ARCHITECTURE` / `GUIDELINE` / `CODING_STANDARDS` / `03-layout-render` / `06-app-platform` / `08-tooling` / `debug_api.toml` / `manual-test/10-window` / `manual-test/23-net` / 根目录 `CHANGELOG`。
- [ ] 刷新 `aurora_api.json`，configure + 构建 runner + `ctest --preset ninja-test`；改动 TU 的增量 clang-tidy 与 `format-check`；`libaurora.a` 符号集对账。
- [ ] 全仓检索六枚已删宏名确认零残留（B 档宏名按预期保留点除外）；按 CI 结果修复后状态流转「已提议 → 实施中 → 已归档」。

## 回写落点

- `codespec/BUILD_OPTIONS.md`：§1.1 / §1.2 / §3 / §3.3 / §4 / §4.2 / §4.5 / §6 / §10 按上文「文档」节逐处改写。
- `codespec/ARCHITECTURE.md`：后端表面表 / CI 矩阵描述 / 门禁矩阵行。
- `codespec/GUIDELINE.md`：GPU 配方 / `feature_flags` 示例 / 调试注入表述。
- `codespec/CODING_STANDARDS.md`：SIMD 能力描述。
- `codespec/specification/03-layout-render.md`：GLFW GPU 门控 / Headless 行 / `GpuGlRhi` 表述 / 音频四后端开关表述。
- `codespec/specification/06-app-platform.md`：测试注入双宏表述 / 调试门面 / 音频与 WebView 与 TLS 装配表述。
- `codespec/specification/08-tooling.md`：SIMD 对比语义 / `TestController` 门控表述。
- `codespec/debug_api.toml`：`feature_flags` JSON 示例去两键。
- `codespec/manual-test/10-window.md`：宏清单行。
- `codespec/manual-test/23-net.md`：C 档系统栈日志文本 / `TC-NET-001` 判据。
- `codespec/changes/prune-compile-option-macro/proposal.md`：状态流转与验收记录。
- 根目录 CHANGELOG.md 与根目录 aurora_api.json（不属 codespec 树，故不列为回写落点，随批必做）：Breaking / Changed 条目与迁移路径；以 `--target aurora_api_json` 重建。

## 验收记录

- 状态流转：已提议 → 实施中 → 已归档（2026-11-16）。
- 全仓检索：六枚已删宏在 `include/` / `src/` / `tests/` / `tools/` / `examples/` / `cmake/` / `.github/` 零命中；B 档五枚宏仍存在于推导注入点与 `feature_flags` 镜像；`codespec/` 仅保留历史手工测试记录与迁移说明中的字面名（非门控用途）。
- 构建与测试：LLVM 构建目录 configure + 库 + runner 全绿；`ctest -j16` **483/483 全过**（含 `utest_feature_flags` 21 字段、`utest_simd_parity`、`registry_integrity`、`utest_net_tls`）；31 项 `check_*` 门禁全绿；`format-check` 1368/1368 合规；`aurora_api_json` 刷新后 `check_api_schema_sync` 绿。
- 增量 tidy：`select_lint_tus.py --git-diff HEAD~3 --changed-only` 选出 44 TU，`run_clang_tidy.py --jobs 4` 全过（unique findings 0 / blocking 0）；两个平台专属探针 TU 不在本机编译库（预期）。
- 编译面证据：`compile_commands.json` 对六枚已删宏零命中；干净构建目录 configure 实测——六枚已删旋钮与 B 档五枚推导宏在全新 `CMakeCache.txt` 中均不再声明（option 已删）；既有构建目录残留的缓存变量成为未使用项，不改变任何行为（迁移说明已注明）。
- 未执行：Windows / macOS / wasm 远端 CI 腿（由推送后的 CI 矩阵覆盖）；`nm -g` 静态库符号集逐符号 diff（导出宏与结构体字段不产生符号名变化，编译面零命中已是更直接的证据）。
