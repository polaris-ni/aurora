# Aurora 演进路线图 + 详细 Phase 计划（草案）

> 状态：规划草案，未纳入 `codespec/`。采纳后相应条目须回填对应规格文档。
> 版本基线：`1.0.0-alpha.4`。**个人开源项目，不追求快速收敛**——按 `1.0.0-alpha.n` 逐步推进（见文末「落地计划」）。
> **当前处于 alpha 阶段：允许任意破坏性修改**（`SPECIFICATIONS.md` §12.1）。因此 RHI 抽取、多窗口 `Application` 生命周期改造、`FocusManager` scope API、`commands.h` 重塑、GPU 从非目标→可选加速等「会动既有 API 形态」的结构级改动，正应在 alpha 阶梯内一次做定，不必迁就旧签名。「只增不删」的 SemVer 纪律**从 beta 冻结起才生效**。
>
> **已确认决策（2026-09-11）**
> - **#1 网络**：核心**不内置通用 HTTP 客户端**（TLS 自研不现实）。异步图片走「可注入 fetcher 回调」，联网由 App 供给，保持零依赖。
> - **#2**：命令系统 **做**（与 AI-first 协同）；打印/分页/PDF **暂缓**；虚拟键盘 **随触摸一起预留**。
> - **#3 节奏**：按 `1.0.0-alpha.n` 逐步推进（alpha.3、alpha.4…… 见文末落地计划）。alpha 阶段**主动利用破坏性许可**做结构级改造，「只增不删」纪律从 beta 起生效。
> - **#4 GPU**：**策略 B——wgpu / WebGPU 作为跨平台 GPU 主力后端**（一套代码覆盖 Win/Mac/Linux/浏览器）；D3D11 现有 present 路径保留为 Windows 兜底，不作首个 GPU 渲染后端。**注**：wgpu-native/Dawn 作为 opt-in 三方（`third_party/` + `AURORA_BACKEND_GPU_WGPU` 默认 OFF）需纳入依赖评估。
> - **触摸**：暂不需要，仅保留扩展位（Track E 硬规则：手势建在 `pointer_id` 抽象流，后端原生触摸采集为可 feature 宏门控的后续增量）。
> - **alpha.3 已发布（2026-09-11）**：实际范围见文末落地计划——以构建地基（wasm）、缺陷收敛（ODR/焦点几何/告警）与 `simulate_*` 远程/AI 暴露为主；原计划的 C4/I1/I2/D0/D0b/F2 功能项顺延至后续 alpha。

---

# 第一部分：战略路线图

## 0. 机会全景与优先级

| Track | 主题 | 定级 | 一句话理由 |
|:---|:---|:---:|:---|
| **A** | 输入与文本国际化（IME / RTL / CLDR） | **Tier 1** | CJK 输入当前基本残废，作者面向中文场景，硬伤 |
| **B** | 无障碍平台桥接 | **Tier 1** | 语义树模型已建好，只差 OS 桥接，补齐性价比极高 |
| **C** | 可测试性（交互模拟接线 + 测试临时文件规范） | **Tier 1** | 补全 AI-first「可验证」卖点闭环 + 测试临时文件统一化 |
| **I** | 桌面完整度缺口（光标 / 焦点陷阱 / 多窗口 / 图表 / 音频） | **Tier 1–3** | 光标形状、焦点陷阱是最小工程量基本功；多窗口/图表/音频已确认需支持 |
| **D** | 渲染与图形（GPU=wgpu / 异步图片 / 色彩管理） | Tier 3 | GPU 是战略长线；异步图片是低风险先行子项 |
| **E** | 动画与手势交互 | Tier 2 | Timeline 编排、跟手动画、滚动增强是体验分水岭 |
| **F** | 布局系统补强 | Tier 2 | baseline 对齐、响应式原语、Overflow::Scroll |
| **G** | AI-first 护城河加深 + 命令系统 | Tier 2 | 差异化核心，MCP 实时编辑 / NL→UI / 命令即 AI 可调用动作 |
| **H** | 工程与生态 | 贯穿 | 包分发、文档站、体积/启动、门禁恢复 |

**推荐落地顺序**：见文末「落地计划（按 1.0.0-alpha.n 阶梯）」——每步利用 alpha 破坏性许可定形 API。

---

## Track A — 输入与文本国际化（Tier 1）

**缺口（已核实）**：`TextInputEvent` 仅带最终 `text`（无 preedit/候选/组合区间）；`TextAlign::Start/End` 注释称依赖书写方向但**无 `TextDirection`/bidi/布局镜像**；`i18n` 复数仅 `one/other`，无 CLDR 完整规则与数字/日期/货币格式。

## Track B — 无障碍平台桥接（Tier 1）

**缺口（已核实）**：`core/accessibility.h` 有完整进程内语义树模型，但**无任何平台桥接**、`bounds` 默认空、无 OS 无障碍事件、无高对比/reduced-motion 策略。

## Track C — 可测试性：交互模拟接线 + 测试临时文件规范（Tier 1）

**缺口（已核实，2026-09-12 更新）**：① `simulate_click/scroll/text_input` 三件套**已接线**（alpha.3，`b3bde1d`）——经 `EventDispatcher` 走真实命中测试 + 派发，并打通远程/AI 工具面（InspectorServer `POST /api/input/{click|scroll|text}` + `aurora_mcp` `simulate_interaction`，`c798cea`），直接服务 #11「AI 可验证正确性」。② 测试临时文件规范（C4）**已落地（2026-09-11~12）**——`isolation.cpp` 基目录改为 `fs::current_path()/test_temp`（不可写时回退系统 temp 并注原因）、每用例唯一子目录 + `end_case` 清理、`.gitignore` 覆盖、`check_test_temp_hygiene` 看护 + `CODING_STANDARDS.md` §3 成文。剩余缺口：C2（TestController）/ C3（ai_compat fixture）消费者侧仍未排期。

## Track D — 渲染与图形（Tier 3，GPU=wgpu）

**GPU/RHI**：在现有 `DisplayList`（`CmdKind`/`DrawCmd` 保留命令层）下插 RHI 抽象；软件 `Painter` 退为参考实现 + golden 逐位基准 + 无 GPU 回退；**首个 GPU 后端 = wgpu/WebGPU**（跨平台）。
**低风险先行子项**：异步图片（可注入 fetcher）、`OverflowStrategy::Scroll`、色彩管理（sRGB/Display P3）。

## Track E — 动画与手势交互（Tier 2）

**缺口**：`Timeline` 编排类型「规划中未实现」。
**触摸硬规则**：手势驱动动画一律建在 `pointer_id` 抽象流上，绝不直接依赖 `TouchEvent`/具体后端；触摸已 touch-ready-unfed（`dispatch_touch` 无调用者），后端原生采集为可 feature 宏门控的后续增量。

## Track F — 布局系统补强（Tier 2）

**缺口**：`CrossAxisAlignment` 无 baseline；缺响应式布局原语（BreakpointBuilder 已落地）。~~`OverflowStrategy::Scroll` 等同 Hidden~~（D0b 已落地）；~~`Skeleton`/`GridView` 未接序列化工厂~~（F2 已落地）。

## Track G — AI-first 护城河 + 命令系统（Tier 2）

MCP 实时改运行中 UI、NL→UI、UI→代码往返保真、语义化视觉回归；**命令系统**：以现有 `app/shortcuts.h` 的 `ShortcutRegistry` 为基座扩出统一命令模型（命令即 AI 可调用动作）。

## Track H — 工程与生态（贯穿）

包分发（vcpkg/Conan）、文档站/Recipe 检索、二进制体积/启动时间、`TEST-R5` 门禁从「仅报告」恢复硬门禁。beta 收敛**不设硬门禁**（个人 OSS）。

## Track I — 桌面完整度缺口（Tier 1–3）

**基本功（Tier 1）**：焦点作用域/陷阱、鼠标光标形状 API。
**已确认需支持**：多窗口、数据可视化/图表、内置音频播放。
**Tier 2**：通用网络（走 #1 fetcher 方案，不内置 HTTP）、滚动位置保存/恢复、列表拖拽重排。
**暂缓/预留**：打印/PDF（暂缓）、虚拟键盘（随触摸预留）。

---

# 第二部分：详细 Phase 计划

> 每个 Phase 给出：目标 / 任务 / 触及文件·接口 / 测试与门禁 / 完成判据。
> 通用纪律：新增公共 API 走 `auto f(...) -> Ret` 尾置返回、强类型几何、pimpl 隔离平台头、配套 `utest_*.cpp` + `demo_*.cpp`、更新 `aurora_api.json`（`gen_api_tools`）与对应 `codespec/` 文档。

## Track C — 交互模拟接线（最小工程量、补卖点闭环，建议最先做）

**Phase C1：接线三个 simulate_***
> **状态：已完成（alpha.3）**——`b3bde1d` 接线（不再返回 `GeneralNotSupported`，无头下可复现）+ `c798cea` 打通远程/AI 工具面（InspectorServer `POST /api/input/{click|scroll|text}`、`aurora_mcp` `simulate_interaction`，见问题 2 接线）。C2（TestController）/ C3（ai_compat fixture）消费者侧仍未排期。
- 目标：`inspector/inspector_api.h:70-76` 的 `simulate_click/scroll/text_input` 从 `GeneralNotSupported` 变为真实派发。
- 任务：
  1. `simulate_click(w)`：合成 `MouseEvent{Press}` + `{Release}`（`e.position` 取 `w.bounds()` 中心，`local_position` 由 `EventDispatcher` 本地化），经 `EventDispatcher::dispatch(root, e, fm)` 派发；需拿到 root + FocusManager → 经 `current_focus_manager()` 或新增测试上下文入口。
  2. `simulate_scroll(w, dx, dy)`：合成 `ScrollEvent{delta_x, delta_y}` 派发到命中链。
  3. `simulate_text_input(w, text)`：合成 `TextInputEvent{text}`，经 `EventDispatcher::dispatch(root, TextInputEvent&, fm)` 派发到焦点 widget（dispatcher.h:85 已有该重载）。
- 触及：`inspector/inspector_api.h/.cpp`、`event/dispatcher.h`、`event/event.h`。
- 测试：`utest_inspector_api` 新增用例——构造 Button/TextInput/Scroll，simulate 后断言状态变化（click 计数、text 内容、scroll offset）。
- 完成判据：三者不再返回 `GeneralNotSupported`；`ctest -R inspector` 全绿；无头下可复现。

**Phase C2：消费者侧 TestController**
- 目标：给 App/测试提供 widget 测试工具（对标 Flutter `WidgetTester`）。
- 任务：新增 `app/test_controller.h`：`pump(frames)` / `pump_and_settle()` / `set_viewport(size, dpi)` / `find_by_key|text|type(...)` / `tap|drag|enter_text(finder)` / `expect_visible|expect_prop(...)`。内部复用 C1 的 simulate + `HeadlessSurface`。
- 触及：新增 `app/test_controller.h`+`src/aurora/app/test_controller.cpp`；依赖 `window/surface.h`(HeadlessSurface)、C1。
- 测试：`utest_test_controller`；`demo_test_controller.cpp`。
- 完成判据：一段 TestController 脚本可对无头 UI 完成「渲染→交互→断言」闭环。

**Phase C3：接入 ai_compat_test**
- 目标：把「生成→交互→断言」纳入 AI 兼容测试 fixture。
- 任务：`tests/fixtures/ai_compat/` 增交互脚本 fixture；`ai_compat_test` 跑 TestController 脚本比对期望状态。
- 完成判据：`ai_compat_test` 覆盖至少 3 个交互回归 fixture。

**Phase C4：测试临时文件统一落 `test_temp/` + 收尾清理**
> **状态：已完成（2026-09-11~12，alpha.4）**——基目录 `cwd()/test_temp`（不可写回退系统 temp 并注原因）、逐用例唯一子目录 + `end_case` 清理、报告排除在清理外、`.gitignore` 覆盖、`check_test_temp_hygiene` 看护（扫描绕过 `temp_dir()` 的裸 `temp_directory_path()`/`/tmp`/写 cwd）与 `CODING_STANDARDS.md` §3 约定均已就位；全量 ctest 后 `test_temp/` 空/不存在（报告除外）。
- 目标：所有测试产生的临时文件统一存放在**当前程序运行路径下的 `test_temp/`**（即 `fs::current_path()/test_temp`）；测试完成后自动删除；**测试报告（`--report` 产物）等可保留**；确有特殊原因须放他处的，代码注释写清理由。
- 现状（已核实）：`tests/framework/isolation.cpp:138+` 基目录优先级是「系统临时目录 → `/tmp` → 工作目录隐藏目录」，即**默认落系统 temp、不在运行路径**；`temp_dir()`（isolation.h:35）接管 `TMPDIR/TMP/TEMP`，`end_case` 还原环境。**改造即翻转此默认。**
- 任务：
  1. 改 `isolation.cpp::ensure_base_dir`：基目录定为 **`fs::current_path()/test_temp`**，不可写时才回退系统 temp 并**注释标注原因**；保留每用例唯一子目录（`test_temp/<case>`）。
  2. 生命周期：逐用例子目录在 `end_case` 删除；会话结束清空 `test_temp/`；**报告写到会话级固定路径并排除在清理外**（`--report` 目标不被删）。
  3. `.gitignore` 加 `test_temp/`，避免误纳版本管理。
  4. 约定成文：`CODING_STANDARDS.md` §3 增「测试临时文件一律走 `temp_dir()`，不得自行 `temp_directory_path()`/裸 `/tmp`/写 cwd；放他处须注释说明原因」。
  5. 守护 `check_test_temp_hygiene`：扫描测试树内绕过 `temp_dir()` 的 `std::filesystem::temp_directory_path` / 裸 `"/tmp"` / 写 cwd 直方图，无豁免注释即红灯；注册进 `cmake/AuroraTests.cmake`。
- 落点定论（2026-09-11 确认）：`cwd()/test_temp`。因 `ctest` 各用例 `WORKING_DIRECTORY` 不一（golden 类设仓库根、其余默认 `build/`），`test_temp` 会随各用例 cwd 分布在 `build/` 与仓库根两处——均在 gitignore 覆盖内，属可接受；若日后要单点收敛再统一锚仓库根。
- 触及：`tests/framework/isolation.h/.cpp`、`tests/framework/test_main.cpp`（报告路径不受清理）、`.gitignore`、`CODING_STANDARDS.md` §3、新增 `tools/check/check_test_temp_hygiene.*`、`cmake/AuroraTests.cmake`。
- 测试：自证——用例结束后断言其 `test_temp/` 子目录已删、报告文件仍在。
- 完成判据：全量 `ctest` 后运行目录下 `test_temp/` 空/不存在（报告除外）；豁免用例带注释；`check_test_temp_hygiene` 绿。

## Track I — 光标形状 + 焦点陷阱（Tier 1 基本功）

**Phase I1：鼠标光标形状 API**
- 目标：补桌面 UX 基本功。
- 任务：
  1. `core/` 新增 `enum class CursorShape{ Arrow, IBeam, PointingHand, ResizeNS, ResizeEW, ResizeNWSE, ResizeNESW, Move, Crosshair, NotAllowed, Wait }`。
  2. `window/surface.h` 新增 `virtual auto set_cursor(CursorShape) -> void {}`（默认 no-op，仿 `set_title` 模式）；各后端实现（Win32 `SetCursor`/`LoadCursor`、GLFW `glfwSetCursor`、X11 `XDefineCursor`、Wayland 指针、macOS `NSCursor`）。
  3. `Modifier` 新增 `cursor(CursorShape)`；`Window::present_root` 命中测试后按悬停控件的 cursor 修饰下发（复用现有 hover 基础设施 `on_hover_change`）。
  4. `TextInput`/`RichTextEdit` 悬停默认 IBeam、`Clickable`/`Button` 默认 PointingHand。
- 触及：`core/enums.h`、`window/surface.h` + 各后端 `.cpp`、`modifier/modifier.h`、`window/window.h`。
- 测试：`utest_surface`（Headless 记录 set_cursor 调用序列断言）；后端专属用例在宏未开时 `AURORA_TEST_SKIP`。
- 完成判据：悬停不同控件触发对应 `set_cursor`；Headless 可断言；文档回填 `specification/03-layout-render.md` §8.3 Surface 表 + `BUILD_OPTIONS.md`（无新宏则免）。
- **状态：核心已落地（2026-09-12），任务 2 的平台后端覆写待排**。已做：任务 1（`CursorShape` 枚举 + known_enums/aurora_api.json 登记）；任务 3（`CursorNode` + `Modifier::cursor(...)`、`Widget::cursor_shape()` 虚钩子、派发器悬停链解析——修饰链 > 钩子 > Clickable→PointingHand，最深命中者生效、形状变化才下发，`Application` 接线 `Surface::set_cursor`）；任务 4（TextInput/RichTextEdit→IBeam、Button→PointingHand）。任务 2 中 `Surface::set_cursor` 默认空实现已入（仿 set_title 模式），但 GLFW/X11/Win32/Wayland/macOS 覆写须在对应平台编译验证后补。测试：`utest_dispatcher` 9 例（悬停光标解析全序）、`utest_surface`、`utest_text_input`/`utest_button` 钩子用例全过。

**Phase I2：焦点作用域 / 焦点陷阱**
- 目标：模态弹层能把 Tab 焦点关在层内；为 Track B 无障碍铺路。
- 任务：
  1. `event/focus.h` 的 `FocusManager` 增「作用域栈」：`push_scope(Widget* subtree)` / `pop_scope()`；`move_focus` 与 `collect_focusable` 限定在当前栈顶 scope 子树内。
  2. `Dialog`/`Popup`/`Drawer` 挂载时 `push_scope(自身)`、卸载时 `pop_scope()`；打开时自动把焦点移入、关闭时恢复到打开前的 `focused()`。
  3. 焦点陷阱循环：scope 内 `Forward` 到末尾回卷到首个 focusable（`move_focus` 已循环，限定 scope 即可）。
- 触及：`event/focus.h/.cpp`、`widget/dialog.h`、`widget/popup.h`、`widget/drawer.h`。
- 测试：`utest_focus` 新增——打开 Dialog 后 Tab 循环不逃出、关闭后焦点恢复。
- 完成判据：模态弹层焦点不外泄；`ctest -R focus` 全绿。
- **状态：已落地（2026-09-12）**。`FocusManager` 作用域栈（`push_scope`/`pop_scope`/`scope_depth`，压栈自动移焦入内 + 记录快照、弹栈按守卫判活恢复、可嵌套逆序恢复；`move_focus` 候选限定栈顶子树，scope 内回卷）；Dialog（show/close）、Popup（open_at/close）、Drawer（set_open，permanent 排除）接线焦点陷阱。测试：`utest_focus` 7→11 例（陷阱/子树限定/嵌套恢复/Dialog 验收）全过，全量 251/251 绿。
> **前置修复（alpha.3，`8686bfb`）**：`Widget::focus_bounds_` 此前无写入点，`FocusManager::move_focus` 方向键导航失效（Tab 序不受影响）；现由 `Widget::paint` 入口写入（与 `paint_bounds_` 同源同值），方向键导航恢复可用。本 Phase 的 scope 限制可在此之上叠加；看护见 `itest_keyboard_nav::directional_nav_uses_geometry_written_by_paint`。

**Phase I3：多窗口（已确认需支持）**
- 目标：从单窗口模型扩到多窗口。
- 任务：
  1. `Application` 持有 `std::vector<unique_ptr<Window>>`；新增 `open_window(XxxOptions) -> Result<Window*>` / `close_window(Window*)` / `windows()`。
  2. 每窗口独立 `FocusManager` + `Scene` + 事件路由（原生事件按来源 HWND/NSWindow/wl_toplevel 分派到对应 Window）。
  3. 帧循环协调多窗口 `wait_events`（各 Surface 已有阻塞原语）；主循环聚合所有窗口的 next deadline。
  4. 各后端支持创建多个原生窗口（Win32 多 HWND、GLFW 多 window、Wayland 多 xdg_toplevel）。
- 触及：`app/application.h/.cpp`、`window/window.h`、各后端 `.cpp`。
- 测试：`itest_multi_window`（Headless 多实例，断言独立 Scene/焦点/事件隔离）。
- 完成判据：可开≥2 个 Headless 窗口各自渲染、焦点独立、事件不串。
- 风险：改动 `Application` 生命周期属核心，须守不变量 2（根挂载唯一）与 7（单线程 UI）；建议出独立设计草案再落地。

**Phase I4：图表控件族（已确认需支持）**
- 目标：补数据可视化。
- 任务：新增 `widget/charts.h`：`LineChart`/`BarChart`/`PieChart`/`ScatterChart`/`Sparkline`，走软件 `Painter` 矢量原语（`draw_line`/`fill_rounded_rect`/渐变已在）；props 含 `series`/`axis`/`legend`/`color_palette`；接入序列化 + `aurora_api.json` + `gen_api` known_enums。
- 触及：新增 `widget/charts.h`+`src/aurora/widget/charts.cpp`、`tools/gen/gen_api.cpp`、`tools/include/known_enums.h`。
- 测试：`utest_charts`（golden 逻辑快照 + 像素容差）；`demo_charts.cpp`。
- 完成判据：图表可构造/渲染/序列化往返；`check_api_schema_sync` 绿；同步收敛 `SPECIFICATIONS.md` §4.1「复杂数据网格」非目标措辞（明确图表 ≠ 被排除的复杂数据网格）。

**Phase I5：内置音频（已确认需支持，opt-in 门控）**
- 目标：补 `AudioPlayer`，feature 宏 `AURORA_ENABLE_AUDIO`（默认 OFF）。
- 任务：
  1. `media/audio_player.h`：`AudioPlayer` 控件 + `play/pause/seek/set_volume/set_muted`；数据源复用 `AudioSink` 抽象（video_source.h 已有）。
  2. 各平台音频后端：WASAPI（Win）/CoreAudio（mac）/AAudio（Android）/PulseAudio·PipeWire（Linux），pimpl 隔离，宏门控剪裁。
  3. 解码：可选 opt-in（miniaudio/dr_mp3 类单头），或只吃 PCM。
- 触及：新增 `media/audio_player.h`+`src`、`cmake/AuroraFeatures`（新宏）、`BUILD_OPTIONS.md`。
- 测试：无真实音频设备下用 null sink 断言状态机；宏未开 `AURORA_TEST_SKIP`。
- 完成判据：`AURORA_ENABLE_AUDIO=ON` 编译过、状态机可测；默认 OFF 时零成本、产物不含音频码。

**Phase I6：滚动位置保存/恢复 + 列表拖拽重排（Tier 2）**
- 任务：`Scroll`/`LazyList` 增 `scroll_offset()` 读写 + 与 `navigation/` 路由状态联动记忆；`LazyList` 增 `on_reorder` + 拖拽落点指示（复用 `draggable` + pointer_id 流）。
- 测试：`utest_scroll`（位置往返）、`utest_lazy_list`（reorder 回调与顺序）。

## Track B — 无障碍平台桥接

**Phase B1：语义树补全（无平台依赖）**
> **状态：已完成（2026-09-11，alpha.4 进行中）**——`build_accessibility_tree` 回填 bounds（已绘制取绘制盒、未绘制按 `Node` 局部原点累加；无 `Node` 几何的虚拟容器兜底继承父盒）、自填 name/value/hint；`Widget` 增 `accessibility_label()` / `accessibility_value()` / `accessibility_hint()` 三个可覆写钩子，并在 Button / Text / RichText / TextInput / RichTextEdit / Checkbox / Switch / Slider / ProgressIndicator 落位语义自填；`show == false` 子树不入树。`utest_accessibility` 用例 6 → 14 全绿；`CHANGELOG.json` 已记 alpha.4 条目。下一步 B2（事件 + 设置注入）待排。
- 任务：布局阶段回填 `AccessibilityNode::bounds`（`build_accessibility_tree` 已留位）；控件补 `name`/`value` 自填（Button 取 label、TextInput 取内容、Slider 取值）；`Widget` 增可选 `accessibility_label()`/`accessibility_hint()` 覆写。
- 触及：`core/accessibility.h`、`widget/widget.h` + 各控件、布局写回点（`window/window.h` present_root）。
- 测试：`utest_accessibility`——断言 bounds 非空、role/name/value 正确、node_count。
- 完成判据：无头下语义树 bounds/name/value 完整。

**Phase B2：无障碍事件 + 设置注入**
> **状态：已完成（2026-09-11，alpha.4 进行中）**——`AccessibilityEvent` / `AccessibilityEventKind` + 事件通道落位（焦点 / 取值 / 结构三类，未安装处理器时零开销）；`AccessibilitySettings{ reduce_motion, high_contrast, font_scale, screen_reader_active }` 支持 `Environment` 注入与进程级默认双来源；`reduce_motion` 由 `AnimationController::tick` 消费（直落端点）、`font_scale` 由 `Text::effective_font`（含 ctx 重载）消费。`utest_accessibility` 用例 14 → 20 全绿，全量 ctest 251/251。`high_contrast` 与 `screen_reader_active` 仅落地「设置位」，尚无控件消费——换色策略待定，见下。
- 任务：焦点/值/结构变化经现有信号上抛为 `AccessibilityEvent`；`Environment`（environment.h:21）注入 `AccessibilitySettings{ reduce_motion, high_contrast, font_scale, screen_reader_active }`，控件响应（reduced-motion 关动画、font_scale 缩放、高对比换色）。
- 触及：`environment/environment.h`、`core/accessibility.h`、`animation/animator.h`（reduce_motion）。
- 测试：`utest_environment`（注入生效）、`utest_accessibility`（事件触发）。

**Phase B3：首个平台桥（Win32 UIAutomation）**
- 任务：`Surface` 扩展点新增 `accessibility_provider()`；Win32 实现 UIAutomation provider（IRawElementProviderSimple 等），把 `AccessibilityNode` 树映射到 UIA 元素、动作映射到 UIA pattern、事件映射到 UIA 事件。pimpl 隔离，`AURORA_BACKEND_WIN32` 门控。
- 触及：新增 `src/aurora/window/win32_accessibility.cpp`、`window/win32_window.h`。
- 测试：UIA 客户端冒烟（可用 `inspect.exe`/UIA COM 客户端脚本）；CI 无 GUI 会话下 `AURORA_TEST_SKIP` + Headless 语义树断言兜底。
- 完成判据：Windows Narrator/inspect.exe 能读到 Aurora 窗口语义树。
- 后续：B4 macOS NSAccessibility、B5 Linux AT-SPI2、B6 Wasm ARIA（各平台独立增量）。

## Track A — 输入与文本国际化

**Phase A1：IME 组合输入**
- 任务：
  1. 新增 `event/event.h::TextCompositionEvent{ preedit, cursor_index, sel_start, sel_end, committed }`（与 `TextInputEvent` 并存：committed 走 TextInputEvent、组合态走 CompositionEvent）。
  2. `RichTextEdit`（已有 selection/cursor/undo）绘制组合态下划线 + 候选插入点；`TextInput` 同步。
  3. 各后端接平台 IME：Win32 **TSF/IMM32**（`ImmSetCompositionWindow`/`WM_IME_*`）、macOS `NSTextInputClient`、X11/Wayland **xkbcommon + IBus/Fcitx**（`zwp_text_input_v3`）、Wasm 浏览器 IME。pimpl 隔离、后端门控。
- 触及：`event/event.h`、`event/dispatcher.h`、`widget/rich_text_edit.h`、`widget/text_input.h`、各后端 `.cpp`。
- 测试：`utest_text_input`（合成 CompositionEvent 断言 preedit 显示 + commit 落字）；真实 IME 在 CI `AURORA_TEST_SKIP`。
- 完成判据：本机 Windows 中文输入法在 TextInput 中可组合输入、候选窗定位正确、commit 落字。
- **状态：核心已落地（2026-09-11），任务 3（平台 IME 接线）待排**。已做：任务 1 完成（`TextCompositionEvent` + `EventDispatcher::dispatch` 重载 + `Widget::on_text_composition` 虚钩子，默认消费防重复上屏）；任务 2 完成（TextInput / RichTextEdit 绘制组合下划线 + 待转换片段高亮 + 组合光标，preedit 参与宽度测量，失焦/commit 取消组合，commit 与普通输入共用落字路径——限长/选区替换/on_changed/无障碍 ValueChanged 一致生效）。测试：`utest_text_input` 12 例、`utest_rich_text_edit` 9 例、`utest_dispatcher` 8 例全过（含组合事件路由、preedit 布局、失焦取消、commit 落字）。**未做**：任务 3 平台 IME 接线（Win32/macOS/X11/Wayland/Wasm）无法在无头 Linux 实现与验收，须有真实输入法环境后补；完成判据随之顺延。

**Phase A2：书写方向 RTL/bidi**
- 任务：
  1. `core/enums.h` 新增 `enum class TextDirection{ Ltr, Rtl, Auto }`；`Environment` 注入 `text_direction`。
  2. 布局镜像：`TextAlign::Start/End`、Flex `Row` 主轴、`Alignment::*Left/Right`、padding/margin 的 start/end 按方向解析。
  3. 文本 bidi：接 HarfBuzz `hb_buffer` 的 bidi/RTL（`hb_ot`，third_party 已含 HarfBuzz）+ 逻辑→视觉重排；`FontEngine::shape_line` 加方向参数（shape 缓存键含方向）。
- 触及：`core/enums.h`、`environment/environment.h`、`layout/*`、`render/font_engine.h/.cpp`、`widget/alignment.h`。
- 测试：`utest_font_engine`（RTL shaping 逐位）、`utest_flex`（镜像布局）、golden 逻辑快照含 RTL 场景。
- 完成判据：设 `TextDirection::Rtl` 后布局镜像 + 阿拉伯/希伯来文本正确 bidi。
- **状态：核心切片已落地（2026-09-12）**。`TextDirection{LTR, RTL}`（枚举值定名 LTR/RTL，不再用计划中的 Auto——Auto 语义由 `explicit_text_direction` 返回 nullopt 表达）+ `core/directionality.h` 双来源注入；`FontEngine` 显式 direction（shape 缓存键含方向，RTL 走 hb 视觉序反转）、caret/hit 逻辑↔视觉镜像映射、Text `direction` 属性 + `TextAlign::Start/End` 方向解析。测试：utest_font_engine 16→20（RTL 镜像逐位）、utest_directionality 新增 3 例、utest_text +2（含 End 对齐方向翻转像素验证）；全量 254/254 绿。**第二切片：Flex 布局镜像已落地（2026-09-12）**——`Flex.rtl` + FlexLayouter 镜像 pass + Row/Column 经 Directionality 自动接线（Environment 注入 > 进程级）。`Alignment`/`EdgeInsets`/`padding` 保持物理语义不镜像（对标 Flutter）。**第三切片：TextInput RTL caret/选区已落地（2026-09-12）**——direction 属性 + 生效方向缓存 + caret/命中/方向键/preedit 四路径镜像。**第四切片与收官已落地（2026-09-12，本 Phase 完成）**：RichTextEdit RTL（整段右对齐 + 跨 run 顺序 + caret/选区/命中镜像）；混排 UBA 多 run 视觉重排（「字体面 + UBA 层级」双键切 run + L2 层叠反转）；完整逐字符 UBA（`uba_levels` 走 X1-X9 / W1-W7 / N0-N2 / I1-I2，`uba_visual_order` 走 L2，含 N0 成对括号 BD16 配对与 W1 NSM）；阿拉伯/希伯来真实字体验收（仓库内 Amiri，连字与 cursive joining 度量断言），顺带修复 `hit_test_char` / `hit_test_char_inclusive` 对 x<=0 无条件返回 0 的 RTL 语义 bug。行为修正（标准 UBA 语义）：RTL 段内同层连续 run 不再整体互换、显式 LTR 段落中的阿文按内容脚本层级 1 正确整形、RTL 段内数字内序保持（如 `م 34` 显示 `34 م`）。**仍顺延**：TextInput 对 bidi 控制符（LRE/RLE/PDF/LRI/RLI/PDI）的输入侧支持（UBA 解析侧已支持，控件侧待接）。

**Phase A3：CLDR 复数 + 本地化格式**
- 任务：`i18n/string_table.h` 复数从 `one/other` 扩到 CLDR 六类（zero/one/two/few/many/other，规则表驱动）；新增 `i18n/format.h`：数字/日期/货币按 `Locale` 格式化（轻量自研表 or opt-in ICU 子集，守零依赖）。
- 触及：`i18n/string_table.h`、`i18n/locale.h`、新增 `i18n/format.h`。
- 测试：`utest_string_table`（各语言复数分支）、`utest_format`（数字/日期分组与符号）。
- 完成判据：阿拉伯语（六类复数）、德语（日期）、日元/美元（货币）格式化正确。

## Track D — GPU（wgpu）+ 图形子项

**Phase D0（低风险先行）：异步图片（#1 fetcher 方案）**
- 目标：`ImageView` 支持异步源，核心不内置 HTTP。
- 任务：
  1. 定义 `using ImageFetcher = std::function<aurora::Task<std::vector<std::uint8_t>>(std::string_view url)>`；进程级/Environment 注入默认 fetcher（App 提供，未注入则 URL 源降级 Placeholder）。
  2. `ImageView` 增 `from_url(url)`（区别于现有 `source` 文件路径）：占位盒 → `au::async` 调 fetcher → 字节交 `ImageSource`/decode → `ImageCache` 缓存 → 主线程回归 setState → 失败降级 `Placeholder`。
- 触及：`widget/image_widget.h`、`render/image_cache.h`、`state/async.h`、`environment/environment.h`（fetcher 注入）。
- 测试：`utest_image_widget`（注入 mock fetcher 返回内置字节，断言占位→加载→失败三态）。
- 完成判据：注入 mock fetcher 后 URL 图片异步加载三态正确；未注入 fetcher 优雅降级。
- **状态：已落地（2026-09-12）**。`ImageFetcher`（URL → Task<bytes>，核心不内置 HTTP）+ 三级解析（显式实参 > 进程级 `set_default_image_fetcher` > `Environment` 注入，无则停留占位降级）；`ImageView::from_url` 三态：占位 → Loading → fetcher（worker）→ `decode_memory` 解码 → `ImageCache`（URL 为键）→ `Task` 主线程投递器回填（`Application::run` 已接线）；缓存命中构造即 Loaded。测试：`utest_image_widget` 8→12 例（成功+缓存命中零 fetch / 取失败 / 解码失败 / 无 fetcher 降级）全过，全量 252/252 绿。

**Phase D0b：OverflowStrategy::Scroll + 色彩管理**
- 任务：`OverflowStrategy::Scroll` 从「等同 Hidden」补真实滚动裁剪；`core/color.h` 加色彩空间标注（sRGB/Display P3），输出按目标 profile 转换（Painter 合成在线性/统一空间）。
- 完成判据：Overflow::Scroll 内容可滚；广色域下颜色转换 golden 容差内。
- **状态：已落地（2026-09-12）**。① Overflow::Scroll：新增 `ScrollViewport` 内核（全库统一 clamp/符号约定，Scroll::on_scroll 与 Widget 基类共用）；Widget 基类轻量滚动（内容平移 + 视口裁剪，不建离屏缓冲），`can_cache_display_list` 对滚动控件返回 false；滚轮派发改沿命中链找最近 `wants_scroll()` 者（最深滚动者优先、可点击子控件不拦截）。② 色彩管理（分层策略，2026-09-12 拍板）：`core/color_space.h`（ColorSpace{SRGB,DisplayP3} + D65 矩阵转换，8bit 往返 ≤ ±3 LSB）；**sRGB golden 保持逐位精确**（GOLDEN_COLORSPACE 常量 + golden 注记字段守卫，不全局放宽容差）；P3 转换路径以独立单测验收。测试：itest_overflow_strategy 5→9、utest_color_space 新增 8 例；全量 ctest 253/253 绿。

**Phase D1：RHI 抽象层抽取（纯重构，零 GPU 依赖）**
- 目标：把 `DisplayList::replay` 的目标从硬编码 `Painter` 改为 RHI 后端接口。
- 任务：
  1. 新增 `include/aurora/render/rhi/`：`RhiBackend`（command sink 提交）、`RhiTexture`/`RhiGlyphAtlas`、`RhiPipeline`、`RhiSwapchain`（present）抽象。
  2. **首个后端 = `SoftwareRhi`**：直接包裹现有 `Painter`，`DisplayList::replay` 走它，行为零变化。
  3. `Surface` 契约扩展：保留 `painter()`；新增 `command_sink()`/`rhi()`（默认返回 SoftwareRhi）。
  4. `DisplayList` 正式成为唯一绘制指令源，`Painter` 与 GPU 后端为其平级消费者。
- 触及：新增 `render/rhi/*`、`render/display_list.h/.cpp`（replay 目标抽象）、`window/surface.h`。
- 测试：**全部现有 ctest + golden 逐位不变**（这是重构红线）。
- 完成判据：golden 零漂移；DisplayList 有两个 replay 消费者（Software + 未来 GPU）的接口位。

**Phase D2：wgpu/WebGPU 后端垂直切片**
- 目标：打穿端到端 GPU 渲染，验证 RHI 抽象。
- 任务：
  1. 引入 **wgpu-native（或 Dawn）** 作为 opt-in 三方（`third_party/` + `cmake/AuroraThirdParty`），feature 宏 `AURORA_BACKEND_GPU_WGPU`（默认 OFF）。
  2. 实现 `WgpuRhi : RhiBackend`：device/queue/swapchain、纹理上传、pipeline、命令编码。
  3. 命令覆盖优先级：`FillRect`/`DrawImage`/`RoundedBorder`/`Linear·RadialGradient`/`PushClip(Rounded)`（SDF + 纹理合成）先打穿；文本走 `glyph_atlas` 纹理化 + instanced quad（shaping 仍 CPU）。
  4. `BlurRegion`/`Shadow`/复杂 `BlendMode`/`MaskRegion`：先回退 SoftwareRhi，D3 再补。
  5. **shader 离线预编译**（Impeller 教训）：WGSL 编译产物随库分发，避免首帧 jank。
  6. `WgpuSurface`：接入 wgpu swapchain present，复用现有 `frame_pacing`/`paces_frames`（vsync）。
- 触及：新增 `render/rhi/wgpu_*`、`window/wgpu_surface.h/.cpp`、`cmake/AuroraThirdParty`/`AuroraBackends`、`BUILD_OPTIONS.md`。
- 测试：GPU vs 软件 golden **容差比对**（`snapshot_diff.h` 的 `tolerance`）；Level 1/2 逻辑快照仍逐位；CI 无 GPU runner 下 `AURORA_TEST_SKIP` + SoftwareRhi 兜底。
- 完成判据：同一 widget 树 wgpu 后端与软件后端容差内一致；宏 OFF 时零成本、产物不含 wgpu。

**Phase D3：确定性策略重定义 + 效果补齐**
- 任务：**正式界定** golden 逐位基准 = 软件参考路径（唯一 SSOT），GPU 走容差 golden + 结构快照双层；回填 `ARCHITECTURE.md` §10.5 与不变量 5 表述。GPU 补齐 blur/shadow/mask（compute shader 或多 pass，借鉴 Vello）、文本 subpixel 的 GPU 等价或显式降级。`perf_gates.json` 增 GPU 帧时间/上屏延迟基准。
- 完成判据：文档回填 + GPU 全命令覆盖 + 性能门禁纳入。

**Phase D4：跨平台 GPU 矩阵 + 能力降级**
- 任务：wgpu 一套代码覆盖 Vulkan（Linux/Win/Android）/Metal（mac/iOS）/D3D12（Win）/WebGPU（Wasm，替代当前 `putImageData` CPU 回写）；`Platform::capabilities()` 增 GPU 可用性查询，不可用/驱动黑名单 → 自动回退 SoftwareRhi（守「降级而非中止」）；CI 矩阵按「每 GPU 分支至少一个 job 编译」扩展。
- 完成判据：≥2 个非 Windows 平台 wgpu 后端 CI 编译 + 容差 golden 通过；无 GPU 环境自动回退不崩。

**Phase D5：GPU 独有能力（可选）**
- 视频/大图零拷贝上屏（media 层解码帧直接 GPU 采样）、动画/变换 GPU 层缓存（`cache_layer` → render target）、compute 矢量光栅探索。

## Track E — 动画与手势

**Phase E1：Timeline 编排原语**
- 任务：新增 `animation/timeline.h::Timeline`（补齐规划中类型）：顺序/并行/交错/延迟分组编排 `Tween`/`Keyframes`；与 `Animator` 集成。
- 触及：`animation/timeline.h/.cpp`、`animation/animator.h`；回填 `specification/05-event-navigation.md` §（明写未实现处）。
- 测试：`utest_timeline`（编排时序确定性）。

**Phase E2：手势驱动动画（pointer-agnostic）**
- 任务：`drag-to-dismiss`、跟手 spring（`spring.h::SpringSimulation` 接手势速度初值）、`Dismissible` 类修饰。**硬规则：建在 `pointer_id` 抽象流（合成 MouseEvent / Draggable），不直接依赖 TouchEvent**。
- 测试：`utest_gesture`（鼠标拖拽驱动动画，触摸路径同构复用）。

**Phase E3：滚动增强**
- 任务：scroll snapping/paging、pull-to-refresh、sticky headers、嵌套滚动协调、滚动驱动动画；reduced-motion（联动 B2）。
- 触及：`widget/scroll.h`、`widget/lazy_list.h`、`animation/*`。
- 测试：`utest_scroll`（snapping 停靠位、sticky 偏移）。

## Track F — 布局补强

**Phase F1：baseline 对齐**
- 任务：`CrossAxisAlignment` 增 `Baseline`；`Row` 内文本控件按基线对齐（`Widget` 增 `baseline()` 查询，FontEngine 提供 ascent）。
- 触及：`core/enums.h`、`layout/flex_layouter.cpp`、`widget/widget.h`、`render/font_engine.h`。
- 测试：`utest_flex`（baseline 几何 golden）。

**Phase F2：响应式原语 + 序列化补全**
- 任务：基于现有 `layout_builder.h`/`media_query.h` 增 breakpoint-aware 容器；`Skeleton`/`GridView` 接入序列化工厂（`CONCEPTS.md` 明写未接）。
- 测试：`utest_serialization`（Skeleton/GridView 往返）、`utest_media_query`。
- **状态：已落地（2026-09-12）**。序列化：Skeleton 全属性往返重建（reg_default）、GridView 注册为已知类型（运行时 ItemBuilder 同 LazyList 先例，重建空数据占位；默认构造顺带消除 0 列除零隐患），两者入 aurora_api.json；BreakpointBuilder：`Breakpoint{Compact,Medium,Expanded}` + 按注入宽度构建子树（无 MediaQuery 退化父约束宽），阈值 600/840 可调，档位变化才重建、关闭布局缓存（防档位冻结）。测试：`utest_serialization` 3 例、`utest_media_query` 6→9 例（断点边界/注入驱动/约束回退）全过，全量 252/252 绿。

## Track G — AI-first 护城河 + 命令系统

**Phase G1：命令系统**
- 任务：以 `app/shortcuts.h::ShortcutRegistry` 为基座扩统一命令模型：`Command{ id, title, icon, action, default_binding, category, when_clause }` + `CommandRegistry`；命令面板控件 `CommandPalette`；命令可序列化 → 供 MCP/AI 枚举调用（命令即 AI 可调用动作）。
- 触及：新增 `app/command.h`（收敛 `commands.h` 空壳）、`widget/command_palette.h`、`tools/gen/gen_api.cpp`（命令入 schema）。
- 测试：`utest_command`（注册/绑定/when 条件/执行）。
- 完成判据：命令可注册、快捷键绑定、palette 检索、MCP 可枚举。

**Phase G2：MCP 实时 UI 编辑闭环**
- 任务：串 `hot_reload.h` + `apply_patch`：AI 经 `aurora_mcp` 改运行中 UI 树 → 即时 diff patch → 热重载看效果。
- 测试：`itest_mcp_live_edit`。

**Phase G3：NL→UI + 视觉回归语义化**
- 任务：提升 `generate_ui.h`/`validate_ui.h` 一次通过率；截图 diff 输出语义化描述（「按钮右移 8px」而非「N 像素不同」）喂 AI；扩充 `ai_compat` fixture。

## Track H — 工程与生态

**Phase H1：门禁恢复 + 文档站**
- 任务：`TEST-R5` 从「仅报告」恢复硬门禁（测试体系重写收束后）；`GUIDELINE.md` 28 组配方做成可搜索文档站。
**Phase H2：包分发 + 体积/启动**
- 任务：vcpkg/Conan port；度量并优化静态库冷启动与产物体积（`AURORA_ENABLE_*` 剪裁审计）。

---

## 落地计划（按 1.0.0-alpha.n 阶梯）

> 前提：**alpha 阶段允许破坏性修改**——每阶梯把会动 API 形态的改造一次定形。每阶梯结束 = 全部门禁绿 + `CHANGELOG.json` 记 `alpha.n` + 打 tag。原则：**单阶梯要么纯重构（golden 逐位不变，如 D1），要么一次定形一组相关破坏性 API，不混做**。
> 任务相关标记Phase X、Task X等等不允许出现在代码、文档、提交信息等地方，
**`1.0.0-alpha.3`（已发布，2026-09-11）— 构建地基 + 缺陷收敛 + 交互模拟远程暴露**
> 实际落地与初版计划有偏差：本阶梯被构建基础设施（wasm）与一批缺陷修复（ODR/焦点几何/告警）占据，原计划的 C4/I1/I2/D0/D0b/F2 功能项**均未落地、顺延至后续 alpha**（见各 Phase 与下方「顺延项」）。

- 实际落地：WebAssembly(Emscripten 6.0.9) 完整构建/测试链路接入（NODERAWFS cwd 运行时改写 POSIX 形态、`-fexceptions` 编译链接同参、`ALLOW_MEMORY_GROWTH`、宿主生成器改原生子项目）· 新增 `AURORA_CAP_*` 能力宏族（测试设施裸平台宏判据全量收敛到 `AURORA_PLATFORM_*`/`AURORA_COMPILER_*`/`AURORA_CAP_*`）· 安装导出补导 `AURORA_ENABLE_DEBUG`（修消费端 ODR 违规崩溃）· `Widget::focus_bounds_` 写入点修复（`move_focus` 方向键导航恢复）· `simulate_*` 远程/AI 工具面打通（InspectorServer `POST /api/input/*` + `aurora_mcp` `simulate_interaction`）· GCC/Clang 误报告警收敛（`-Wdangling-reference`/`-Warray-bounds`/`$` 占位符等）· `core/environment.h` 依赖图单向化
- **破坏性许可点（实际触发）**：`environment → build_context` 依赖方向翻转（使用方须显式 `#include "aurora/core/build_context.h"`）· 移除 clang-tidy 门禁与 `aurora_lint` 工具/目标 · `Widget::focus_bounds()` 取值语义变更（恒零盒 → 最近绘制绝对盒）
- 门禁：`check_version_consistency` 通过（currentVersion == 库版本）；`utest_inspector_server` / `itest_mcp` 新增模拟用例全绿；wasm 下 ctest 全量开启（能力缺失用例走 `AURORA_TEST_REQUIRE_*` 的 SKIP 路径）

**顺延项（原 alpha.3 计划）**：C4 测试临时文件规范 · I1 光标形状 · I2 焦点作用域/陷阱 · D0 异步图片(fetcher) · D0b `OverflowStrategy::Scroll` · F2 Skeleton/GridView 序列化补全 —— **六项已全部并入 alpha.4 落地完毕（2026-09-12）**。

**`1.0.0-alpha.4` — 无障碍 + 输入与文本国际化（已收口，2026-09-12）**（B1、B2、A1 核心、A2 全切片、alpha.3 顺延六项（C4/I1/I2/F2/D0/D0b）已落地 2026-09-11~12；**顺延至 alpha.5**：B3 Win32 UIAutomation 桥、A1 平台 IME 接线、I1 光标形状平台接线——均需对应平台编译与真实环境验收，无头 Linux 无法完成）


- B1–B3 无障碍语义树补全 + Win32 UIAutomation 桥 · A1 IME 组合输入 · A2 RTL/bidi
- **破坏性许可点**：`TextDirection`/`TextCompositionEvent` 新类型；布局镜像改 `TextAlign::Start/End`/`Flex`/`Alignment` 语义

**`1.0.0-alpha.5` — 渲染地基重构 + 动画/命令**
- D1 RHI 抽象抽取（纯重构，golden 逐位不变）· E1 Timeline 编排 · E2 手势驱动动画（pointer-agnostic）· G1 命令系统
- **破坏性许可点**：`Surface` 增 `command_sink()`/`rhi()`、`DisplayList::replay` 目标重塑、`commands.h` 空壳 → `CommandRegistry`

**`1.0.0-alpha.6` — GPU 落地（wgpu）**
- D2 wgpu 后端垂直切片（opt-in 三方 + `AURORA_BACKEND_GPU_WGPU`）· D3 确定性策略重定义 + 效果补齐 · I4 图表控件族
- **破坏性许可点**：GPU 从「非目标」→「可选加速」（回填 `SPECIFICATIONS.md` §2/§4.1、`ARCHITECTURE.md` §10.5/不变量 5）

**`1.0.0-alpha.7` — 跨平台矩阵 + 平台补全**
- D4 跨平台 GPU + 能力查询降级 · I3 多窗口（`Application` 生命周期改造，重破坏性，先出独立设计草案）· I5 内置音频（`AURORA_ENABLE_AUDIO`）· A3 CLDR 复数/本地化格式 · E3 滚动增强 · F1 baseline · I6 滚动记忆/重排

**`1.0.0-alpha.8+` → 进 beta.1 前收口**
- B4–B6 其他平台无障碍桥（macOS/Linux/Wasm）· G2–G3 MCP 实时编辑 / NL→UI / 视觉回归语义化 · D5 GPU 独有能力 · H1–H2 门禁恢复/文档站/包分发/体积启动
- 冻结前置：API 冻结 SSOT 稳定 → 进 beta，此后「只增不删」SemVer 纪律生效

> 阶梯粒度可调：单个 `alpha.n` 过重可拆 `alpha.n+1`；顺序按依赖锁定（RHI D1 必须先于 GPU D2；fetcher D0 必须先于任何联网图片用例）。

> 采纳后回填映射：Track A→`specification/05`+`03`；B→`core/accessibility`+`05`；C→`08-tooling`+`CODING_STANDARDS §3`（测试临时文件规范）；D→`ARCHITECTURE §8`+`SPECIFICATIONS §2/§4.1`（GPU 从非目标改可选加速）+`03`；E→`05`；F→`03`；G→`08`+`06`；I→`03`+`04`+`06`+`BUILD_OPTIONS`。
