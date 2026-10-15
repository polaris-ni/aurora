# CHG-017 WebView 能力：原生 Web 视图的插件式嵌入（交互 / 快照双模）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-017 |
| 提出日期 | 2026-10-14 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.PLATFORM.ZERO-IFDEF.001 |
| 影响面 | 新增公共模块 `webview/`（命名空间 `aurora::webview`；公共头 `web_view.h`、`native_view_host.h`（二者均位于 `include/aurora/webview/`）+ `src/aurora/webview/` 下五个实现源文件）；新增公共控件 `WebView`（第 74 个 widget，`aurora_api.json` 与 `tools/check/umbrella_manifest.txt` 同步收录）；新增 `Surface` 之外的第二条平台扩展点 `NativeViewHost`（宿主侧原生子视图书架：每帧几何同步 + Linux 侧 fd 并入事件等待）；新增线程局部宿主访问口（形态仿 `current_focus_manager()`）；新增 `AURORA_ENABLE_WEBVIEW` 组编译开关（默认 OFF，`FeatureFlags` 运行时查询同步收口）；`codespec/errors.toml` 新增 `webview-*` 错误组；`validate_ui` 增一类组合诊断；`codespec/manual-test/` 新增探针配套条目。不改 `Painter` / `Surface` / 布局 / 事件既有语义 |

## 动机

本库 73 个控件覆盖了自绘 UI 的全部常规形态，但**没有承载 Web 内容的宿主**：AI-first 场景里的「嵌一个设备厂商的管理后台页面」「渲染一份远程 HTML 报告」「给应用内嵌一个富文本编辑器」，目前只能外跳浏览器，或退回到富文本子集自绘。这是公共 API 面的真实缺口，不是体验优化。

业界三条路线的事实口径（依据 Qt 官方模块划分、Tauri 所用 wry 库的平台说明、以及 Linux WebView 托管实践文档）：

| 路线 | 代表 | 体积 | 进程 / 渲染 | 与本库定位的冲突 |
|:---|:---|:---|:---|:---|
| 捆绑浏览器引擎 | Qt WebEngine（Chromium）、CEF | 几十 MB 至上百 MB 辅助文件（资源包、独立进程可执行文件） | 多进程 + GPU 进程 | 与「纯软件栅格、无 GPU 依赖、静态库交付」正面冲突；也摧毁 CI 构建时间与 ccache 命中率 |
| 系统原生桥 | Qt WebView、wry（Tauri） | 近乎零（不打包引擎） | 引擎进程由 OS 承担 | 需处理三平台能力不齐与运行时缺失 |
| 商业自绘 HTML 引擎 | Ultralight / Sciter | 中等 | 库内自绘，可直进帧流 | 商业授权与厂商单点依赖；与「third_party 不允许修改」纪律存在张力 |

**结论**：走「系统原生桥」，并把本仓既有的两个成熟手法直接套用到它上面——

1. **运行时动态加载 + 缺失即降级**：Win32 UIA 桥经 `LoadLibraryA` 加载 `UIAutomationCore.dll`、Linux AT-SPI2 桥经 `dlopen("libdbus-1.so.3")`，两套桥已经确立「核心零链接依赖、缺库即降级 no-op + 一次 `Diagnostics::warn`」的口径。WebView 沿用同一手法：编译期不链 WebView2 / WebKitGTK / WebKit 框架，运行期探测不到即无此能力，而不是构建失败或运行崩溃。
2. **降级而非中止**：对齐 `media/audio.h`「无设备后端或初始化失败 → 静默模式（图照常运转）」——WebView 缺引擎时降级为占位矩形，树照常布局与绘制。

第二条动机是**为什么必须双模**：

- 「网页能用」和「网页能被 Painter 合成」是互斥的。要能用（点击、滚动、选中、输入法、右键菜单、无障碍），就必须让 OS 的原生视图承载它；而原生视图一旦进入，它就**不在 Painter 帧流里**——圆角裁剪、Opacity、变换、以及「在它上面画东西」全部失效。
- 但另一类场景（报告预览、文档块、静态渲染）恰恰要求它**必须**在帧流里：要能被裁剪、能被叠加绘制、要能进 `HeadlessSurface` 的 golden 与逻辑快照。

只做其中一半都会留下一半需求落不了地，因此把取舍做成**控件上的一个显式模式位**，由使用方按场景声明，而不是由框架替他猜。

## 变更内容

### 1. 模块与文件

沿用 `media/audio.h` + `src/aurora/media/audio_{wasapi,alsa,webaudio}.cpp` 的「公共抽象 + 平台实现」形状：

| 路径 | 内容 |
|:---|:---|
| `web_view.h` | 公共控件 `WebView` + `WebViewProps` + `WebViewMode` + 回调类型 + `WebViewCapabilities` |
| `native_view_host.h` | `NativeViewHost`：宿主侧原生子视图书架 + 引擎能力探针（公共头零平台污染） |
| `web_view.cpp` | 控件本体：布局占位、快照 blit、命令转发、降级占位绘制 |
| `native_view_host.cpp` | 登记表、每帧几何同步、纯函数坐标换算、可用性裁决 |
| `engine_null.cpp` | Null 实现：恒为确定性桩，供无头 / WASM / CI 走降级路径 |
| `engine_webview2.cpp` | Windows 实现（`AURORA_ENABLE_WEBVIEW_WEBVIEW2`） |
| `engine_webkitgtk.cpp` | Linux 实现（`AURORA_ENABLE_WEBVIEW_WEBKITGTK`） |

macOS（WKWebView）本次只预留枚举位与注册表入口，**不落实现**——本机与 CI 均无 Apple 目标平台可构建，落了就是拿不到证据的代码。

### 2. 引擎抽象与降级契约

```cpp
enum class WebViewEngine { Null, WebView2, WebKitGtk, WkWebView };
enum class WebViewAvailability { Available, MissingRuntime, UnsupportedPlatform, Disabled };

struct WebViewCapabilities {
    bool interactive = false;   // 原生子视图模式是否可用
    bool snapshot = false;      // 快照 blit 模式是否可用
    WebViewEngine engine = WebViewEngine::Null;
    WebViewAvailability availability = WebViewAvailability::Disabled;
};
```

四条降级契约（作为能力边界写明，违反即为缺陷）：

1. 宏关闭 → `WebView` 降级为占位：参与布局占位、绘制主题色边框与一行提示、**附一次 Diagnostics 警告**、不抛异常、不阻塞树渲染。
2. 宏开启但运行期探测失败（无 WebView2 Runtime、无 `libwebkit2gtk`、无可用显示连接）→ 同一占位语义，`availability` 为 `MissingRuntime`。
3. 平台或后端不支持（Wayland 交互模式、WASM）→ 占位，`availability` 为 `UnsupportedPlatform`。
4. 探测**只做一次**并缓存结果；不得每帧重复 `dlopen` / `LoadLibrary`。

### 3. 控件 API 面

`WebViewProps`（全部带合理默认值，声明处可见）：`url`（初始导航目标，空串表示暂不加载）、`html`（内联 HTML，`url` 非空时被忽略）、`base_uri`（内联 HTML 的基 URI）、`mode`（`Interactive` / `Snapshot`）、`zoom_factor`、`javascript_enabled`、`background`（快照模式下的占位底色）。

回调：`on_navigation`（含 url 与加载完成 / 失败三态）、`on_title_changed`、`on_web_message(std::string json)`、`on_process_failed`。

方法：`load_url` / `load_html` / `reload` / `stop` / `go_back` / `go_forward` / `evaluate_script(js, fn)` / `post_json(json)` / `request_snapshot(fn)`。

双向桥统一到**一条 JSON 消息通道**：

- Web→Native：文档创建时注入一段 shim（各平台各自的入口），把 `window.aurora.postMessage(obj)` 收敛到宿主侧一条管线 → `on_web_message(json)`。
- Native→Web：`evaluate_script` 与消息投稿。**两条都是异步、且回调落在 UI 线程**（三平台引擎契约如此），因此直接契合本库单线程 UI 模型，不需要跨线程回投；仅需一处守卫：回调到达时宿主与自己可能已解绑，收到即判活、不判活不解引用。

### 4. 双模语义

| 维度 | `Interactive`（原生子视图） | `Snapshot`（快照 blit） |
|:---|:---|:---|
| 交互 | 原生全量（点击 / 滚动 / 文本选中 / 输入法 / 右键菜单 / 无障碍） | 无（静态像素） |
| Painter 合成 | **不参与**：不受祖先 `Clip*` / `Opacity` / 非纯平移 `Transform` 影响 | 完全参与，等同 `ImageWidget` |
| z 序 | 恒在 Painter 内容之上（Tooltip / ContextMenu 压不住它） | 正常树序 |
| golden / 逻辑快照 | 不进入（对该矩形无判定能力） | 进入，可与 `HeadlessSurface` 逐像素比对 |
| CPU 开销 | 零拷贝，由 OS 合成 | 每次刷新一次全矩形像素拷贝 |
| 滚动容器内 | 几何同步滞后，可见 tearing（见第 9 节已知限制） | 无此问题 |

### 5. 宿主接线：`NativeViewHost`

这是本次唯一触及运行时框架的改动，且刻意做成**宿主集中管理**（仿无障碍桥 `set_accessibility_root` 的宿主侧口径，而不是让控件各自去找窗口）：

- `BuildContext` 目前只有 `env` / `scale_factor` / `size` / `host_id`，**没有任何窗口指针**，控件自身无从把 dp 矩形换算成宿主物理坐标。故新增线程局部访问口（实现与存取形态完全仿 `event/focus.h` 的 `current_focus_manager()` / `set_current_focus_manager()`），由宿主在**每帧帧首、树重建（mount）之前**安装并覆盖整帧、帧尾恢复——控件登记发生在 mount（构建期）而非绘制期，安装口必须早于 mount，否则挂载时拿不到宿主。
- `WindowHost` 侧新增一张「原生子视图登记表」（控件 → 引擎视图句柄 + 上次下发的矩形 + 上次可见态）。控件挂载时经该线程局部口登记自己、卸载时注销；**登记与注销必须成对**（与 `Animator::remove` 同纪律）。
- 每帧在布局落定之后、present 之前，**由宿主一次性遍历登记表**下发几何：把控件 dp 矩形换算为父窗口物理矩形，与上次不同才下发。换算做成**独立纯函数**（输入：dp 矩形 / `Surface::scale_factor()` / `Surface::content_inset()`；输出：物理矩形），便于单测钉住。
- **Linux 侧的事件泵接入**：WebKitGTK 需要 GTK 自己的循环推进。X11 后端的 `wait_events` 已是 `poll(2)`，故新增 `NativeViewHost::poll_watches()` 把 GDK/GLib 的 fd 与超时并入同一 `poll`——完全仿照既有 Linux AT-SPI2 桥「`Provider::poll_watches()` 并入 X11/Wayland 事件等待」的做法，不引入任何额外线程。`WaylandSurface` 同口径接入：快照模式（第 6 节待验证项）若要求 GTK display 连接活动，其事件等待同样须并入 `poll_watches()`（AT-SPI 桥已两条后端都接），不允许只在 X11 接、Wayland 裸奔。
- **焦点口径（写明为契约，不留隐含）**：`WebView` 不进 `FocusManager` 焦点链——交互模式的键盘焦点由原生视图向 OS 承担，Tab 序进出 Web 内容属引擎行为；宿主只做两件事：① 控件卸载或窗口失活时若焦点在 Web 内，由引擎侧回收，Aurora 侧焦点态保持「从未持有」；② `host-unbound` 之外的焦点态不做双向同步，首版不承诺 Aurora ↔ Web 的 Tab 遍历贯通（列入第 9 节已知限制）。
- **无障碍共存口径**：交互模式下 Web 内容的无障碍树由引擎自带的 UIA（Windows）/ AT-SPI（Linux）提供，子 HWND / 原生窗口对 OS 自成片段树，本库 a11y 桥**不穿透**——宿主语义树把 `WebView` 控件暴露为一个叶子节点（角色与名称由 props 决定），Web 内部结构不经本库语义树上报。快照模式无交互、无自带树，语义树按占位图口径上报。

### 6. 平台实现的真实边界（据实写明，不做平台一致性承诺）

| 平台 / 后端 | Interactive | Snapshot | 依据与约束 |
|:---|:---|:---|:---|
| Windows / `Win32Surface`（D3D11 与 wgpu 复用同一宿主） | 可用 | 可用 | WebView2 窗口托管为 HWND 子窗口，创建后不可重挂父窗口，故宿主变更或重建须销毁重建；几何每次下发 `set_bounds` 即可。DPI 由宿主接管：关闭引擎的自动显示器缩放探测，由我们按 `Surface::scale_factor()` 下发光栅化比例。快照走 `CapturePreview`（视口） |
| Linux / `X11Surface` | 可用 | 可用 | wry 的 X11 路径已证成可行：以外部 X11 窗口句柄经 `gdk_x11_window_foreign_new_for_display` 包成 GDK 窗口，再把 GTK 部件树放进去。**待验证决策点**：优先 GTK4 + WebKitGTK 6.0（现代线，Ubuntu 24.04+ 默认）；若 GTK4 的对应 foreign surface 入口不等价可用，则回落 GTK3 + WebKitGTK 4.1（wry 现行方案） |
| Linux / `WaylandSurface` | **不可用** | 待验证 | Wayland 协议层不能把另一位客户端的 GTK surface 作为带定位的子 surface 附加到宿主的 `wl_surface`；业界唯一解法是让 GTK 拥有整个顶层窗口，与本库 Wayland 后端的宿主权冲突，不可接受。故交互模式在该后端不可用。快照模式要求 GTK 有可用 display 连接，是否必须承载一个可见 GTK 窗口属**待验证**事项 |
| macOS / `MacOSSurface` | 预留 | 预留 | WKWebView 为 NSView 子视图，交互模式天然可行；抽象已预留，本次不落实现 |
| `HeadlessSurface` / `WasmSurface` / 未开宏构建 | 不可用 | 走 Null 实现 | Null 实现产出确定性桩图，使 golden 与逻辑快照仍能覆盖快照模式的绘制路径 |

### 7. 编译开关（归入 `AURORA_ENABLE_*` 能力组，形态仿 `AURORA_ENABLE_AUDIO` 家族）

| 选项 | 默认 | 含义 |
|:---|:---|:---|
| `AURORA_ENABLE_WEBVIEW` | `OFF` | 编入 `webview/` 公共模块与 Null 实现 |
| `AURORA_ENABLE_WEBVIEW_WEBVIEW2` | `OFF` | Windows 原生实现（构建期需其 SDK 头文件，运行期动态加载） |
| `AURORA_ENABLE_WEBVIEW_WEBKITGTK` | `OFF` | Linux 原生实现（`pkg-config` 探测头文件，运行期 `dlopen`） |

三枚开关与 `AURORA_ENABLE_AUDIO*` 同口径：开关名即宏名；**登记调用点在 `cmake/AuroraBackends.cmake`**（audio / 存储开关同址），机制统一走 `AuroraFeatures.cmake` 的 `aurora_define_feature` 单一入口。EXPORT 与否随「公共头零宏引用」裁决：公共头不出现这三枚宏（能力查询走 `WebViewCapabilities` 运行时值），故**不 EXPORT**，与 SIMD 等纯内部宏口径一致；引擎实现源文件内的条件编译使用 `AURORA_ENABLE_WEBVIEW_*` 规范宏（非原生平台宏，`check_platform_macros` 不拦）。三枚宏同步进 `debug/feature_flags` 的 `FeatureFlags` 运行时查询收口（结构体字段 + JSON 导出），维持「应用代码零 `#ifdef`」契约——若裁决豁免，须在 `BUILD_OPTIONS.md` 写明理由。**字段命名沿用 `audio` 的「去前缀」口径**（新增 `webview` / `webview_webview2` / `webview_webkitgtk`）；须留意该结构现有命名并不自洽（`AURORA_ENABLE_AUDIO` 落为 `audio`，而 `AURORA_ENABLE_AUDIO_WASAPI` 落为 `enable_audio_wasapi` 保留前缀），本次按多数侧取值、不连带改名既有字段（改名属破坏性变更），仅在 `BUILD_OPTIONS.md` 记一笔该现状。

### 8. 错误与诊断

- `codespec/errors.toml` 新增 `webview-*` 组（至少覆盖：runtime-missing、engine-init-failed、navigation-failed、script-eval-failed、snapshot-unsupported、host-unbound、unsupported-mode），跑生成器刷新三处产物。
- `validate_ui` 新增一类**设计期诊断**（把上述合成限制从「运行时诡异行为」前移成「写的时候就告诉你」）：`Interactive` 模式控件的祖先链上存在 `Clip*` / `ClipRounded` / `Opacity`（小于 1）/ `Transform`（非纯平移）修饰时，产出一条带修复建议的诊断。只报诊断，不改绘制行为。

### 9. 首版明确不做（写进边界，避免后续被误判为遗漏）

DevTools 协议接入、cookie 与站点存储管理、下载管理、自定义 scheme 与资源拦截、权限与打印、多实例共享 profile、页面内音视频与宿主音量体系的桥接。另有四条**已知限制**须写入文档而非隐含：`Interactive` 模式在 `Scroll` 容器内的几何跟随滞后同步、拖拽或快速滚动时可见 tearing；`Interactive` 模式的 Web 内容之上无法叠加任何 Painter 绘制物；首版不承诺 Aurora ↔ Web 的 Tab 焦点遍历贯通（见第 5 节焦点口径）；本库语义树不穿透 Web 内容的无障碍树（见第 5 节无障碍共存口径）。

## 验收判据

1. 新增 `utest_web_view.cpp`（Null 实现，无 GUI）：覆盖 props 默认值与查询语义、`to_json` / `from_json` 往返逐值一致、**四种 `WebViewAvailability` 降级路径**（各自产占位、各自上报一次 Diagnostics、均不抛异常）、未绑定宿主时不崩且访问口返回空、登记与注销配对（重复注销幂等）。
2. 新增 `utest_native_view_host.cpp`：「dp 矩形 + scale + content_inset → 物理矩形」纯函数换算穷举覆盖（含 scale 1.0 / 1.25 / 1.5 / 2.0、CSD inset 非零、深嵌套偏移、零尺寸盒），以及**几何未变则不重复下发**的去重判据。
3. 快照模式的绘制路径进 `HeadlessSurface`：Null 实现产出固定桩图，经 `utest_offscreen` 做 golden 逐像素比对与逻辑快照逐字段比对，证明双模中「帧流那一半」确实做到了帧流内。
4. **变异自证**：在前述两处判据上至少注入两处变异——① 把 `content_inset` 从换算里扣掉；② 去掉几何去重的「是否变化」判定——各自必须令对应用例转红、还原后转绿。只报全绿不算验收通过。
5. 降级矩阵端到端：关闭 `AURORA_ENABLE_WEBVIEW` 时全仓仍构建通过且既有用例零回归（证明核心链接面未被污染）；开启宏而目标机无引擎运行时时，demo 启动只产警告、不崩。文档回写完成时在 `CHANGELOG.md` Added 段记新增公共模块与控件（MINOR 语义）。
6. 真机验收探针（仿 `AURORA_BUILD_VERIFY_TOOLS` 口径，全部 `EXCLUDE_FROM_ALL`、**不进 CTest**）：`aurora_verify_webview_webview2`（加载内联 HTML → 标题回报 → `post_json` 与 `on_web_message` 一个完整往返 → 改窗口宽度后几何随动）、`aurora_verify_webview_webkitgtk`（X11 环境同套判据）。本机只能跑前者，后者归 Linux 真机手验。两枚探针在 `codespec/manual-test/` 各配套一条手工测试条目（编号与格式受 `check_manual_test_format` 管辖，仿 `codespec/manual-test/09-event.md` 的 `aurora_verify_win32_syskey` 条目形态）。
7. `cmake --build build --target aurora_api_json` 后，`check_api_schema_sync`、`check_api_budget`（以门禁实测值为准，须在 75k 预算内仍有余量）、`check_doc_comments`、`check_umbrella_header`、`check_arch_module_map`、`check_core_layer_boundary`（`webview/` 对 `window/` 宿主接线的依赖方向受审）、`check_naming_conventions`、`check_platform_macros`（新模块不得书写原生平台宏）、`lint`、`format-check` 全绿；`ctest --preset ninja-test` 全量绿；CI `toggles` 矩阵纳入三枚新开关的组合；`FeatureFlags` 结构体与 JSON 导出同步纳入三枚新宏（`debug/feature_flags.cpp` 收口，验收时以 `feature_flags_json()` 输出核对）。
8. lint 覆盖面须与既有的默认 OFF 宏纪律一致：这类宏不显式打开时其代码在分析面上是「不存在」而非「干净」，故须把 `AURORA_ENABLE_WEBVIEW_WEBVIEW2` 纳入既有 lint 扩面矩阵，否则本次等于没真正过 lint。

## 回写落点

- `codespec/ARCHITECTURE.md`（§2 分层与新增扩展点的关系、§4 模块映射新增 `webview/` 行、§11 设计不变量补「原生子视图不可被 Painter 合成」一条）
- `codespec/BUILD_OPTIONS.md`（§1 三层命名分类法、§4 能力开关：新增 `AURORA_ENABLE_WEBVIEW` 组三枚）
- `codespec/specification/04-widget.md`（新控件须同样遵守的既有契约、控件清单与 props / 序列化口径）
- `codespec/specification/06-app-platform.md`（§12.1 零 `#ifdef` 契约下新增的「宿主管理原生子视图」扩展点、Linux 事件泵 fd 并入口径、`feature_flags()` 新增三枚 webview 宏的运行时查询口径）
- `codespec/specification/03-layout-render.md`（快照模式进入 Painter 帧流的确定性口径，以及与既有 golden 假设不冲突的说明）
- `codespec/manual-test/`（两枚真机探针配套的手工测试条目，归入对应域文件）
- `codespec/errors.toml`（新增 `webview-*` 错误组）
- `codespec/ERROR_CATALOG.md`（生成物：`webview-*` 错误码，随 `gen_error_codes` 刷新）
- `codespec/changes/webview-platform-embedding/proposal.md` 自身状态随批次流转（已提议 → 实施中 → 已归档）
