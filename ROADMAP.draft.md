# Aurora 演进路线图 + 详细计划（草案）

> 状态：规划草案，未纳入 `codespec/`。采纳后相应条目须回填对应规格文档。
> 版本基线：`1.0.0-alpha.3`。**个人开源项目，不追求快速收敛**——按 `1.0.0-alpha.n` 逐步推进（见文末「落地计划」）。
> **当前处于 alpha 阶段：允许任意破坏性修改**（`SPECIFICATIONS.md` §12.1）。因此 RHI 抽取、多窗口 `Application` 生命周期改造、`FocusManager` scope API、`commands.h` 重塑、GPU 从非目标→可选加速等「会动既有 API 形态」的结构级改动，正应在 alpha 阶梯内一次做定，不必迁就旧签名。「只增不删」的 SemVer 纪律**从 beta 冻结起才生效**。
>
> **已确认决策**
> - **网络**：核心**不内置通用 HTTP 客户端**（TLS 自研不现实）。异步图片走「可注入 fetcher 回调」，联网由 App 供给，保持零依赖。
> - **命令系统**：做（与 AI-first 协同）；打印/分页/PDF **暂缓**；虚拟键盘 **随触摸一起预留**。
> - **节奏**：按 `1.0.0-alpha.n` 逐步推进，alpha 阶段**主动利用破坏性许可**做结构级改造，「只增不删」纪律从 beta 起生效。
> - **GPU**：策略 B——wgpu / WebGPU 作为跨平台 GPU 主力后端（一套代码覆盖 Win/Mac/Linux/浏览器）；D3D11 现有 present 路径保留为 Windows 兜底，不作首个 GPU 渲染后端。**注**：wgpu-native/Dawn 作为 opt-in 三方（`third_party/` + `AURORA_BACKEND_GPU_WGPU` 默认 OFF）需纳入依赖评估。
> - **触摸**：暂不需要，仅保留扩展位（手势建在 `pointer_id` 抽象流，后端原生触摸采集为可 feature 宏门控的后续增量）。
> - **alpha.3 已发布（2026-09-13，tag `v1.0.0-alpha.3`）**：已完成全部既有规划工作的合并收口——构建地基（wasm）、缺陷收敛、交互模拟远程/AI 暴露、无障碍语义树补全、IME 组合输入核心、RTL/bidi 全切片、可测试性收尾、光标形状平台接线（含 X11 真机双验证）、真机验收探针工具化、RHI 抽象抽取骨架。详见 `CHANGELOG.json` 的 `1.0.0-alpha.3` 条目与下方「落地计划」。后续规划项顺延至 alpha.4 起，见文末阶梯。

---

# 第一部分：战略路线图

## 0. 机会全景与优先级

| 主题 | 定级 | 一句话理由 |
|:---|:---:|:---|
| 输入与文本国际化（IME / RTL / 本地化格式） | **Tier 1** | CJK 输入当前基本残废，作者面向中文场景，硬伤 |
| 无障碍平台桥接 | **Tier 1** | 语义树模型已建好，只差 OS 桥接，补齐性价比极高 |
| 可测试性（交互模拟接线 + 测试临时文件规范） | **Tier 1** | 补全 AI-first「可验证」卖点闭环 + 测试临时文件统一化 |
| 桌面完整度缺口（光标 / 焦点陷阱 / 多窗口 / 图表 / 音频） | **Tier 1–3** | 光标形状、焦点陷阱是最小工程量基本功；多窗口/图表/音频已确认需支持 |
| 渲染与图形（GPU=wgpu / 异步图片 / 色彩管理） | Tier 3 | GPU 是战略长线；异步图片是低风险先行子项 |
| 动画与手势交互 | Tier 2 | Timeline 编排、跟手动画、滚动增强是体验分水岭 |
| 布局系统补强 | Tier 2 | baseline 对齐、响应式原语、Overflow::Scroll |
| AI-first 护城河加深 + 命令系统 | Tier 2 | 差异化核心，MCP 实时编辑 / NL→UI / 命令即 AI 可调用动作 |
| 工程与生态 | 贯穿 | 包分发、文档站、体积/启动、门禁恢复 |

**推荐落地顺序**：见文末「落地计划（按 1.0.0-alpha.n 阶梯）」——每步利用 alpha 破坏性许可定形 API。

---

## 输入与文本国际化（Tier 1）

**已落地（alpha.3）**
- **IME 组合输入核心**：`TextCompositionEvent` + `EventDispatcher::dispatch` 重载 + `Widget::on_text_composition` 虚钩子；TextInput / RichTextEdit 绘制组合下划线 + 候选插入点 + 组合光标，preedit 参与宽度测量，失焦/commit 取消组合。
- **书写方向 RTL/bidi 全切片**：`TextDirection{LTR,RTL}` + 双来源注入；`FontEngine` 显式 direction（shape 缓存键含方向，RTL 走 hb 视觉序反转）、caret/hit 逻辑↔视觉镜像；Flex 布局镜像（`Flex.rtl` + FlexLayouter 镜像 pass，Environment 注入自动接线）；TextInput RTL caret/选区；RichTextEdit RTL（整段右对齐 + 跨 run 顺序 + caret/选区/命中镜像）；混排 UBA 多 run 视觉重排（双键切 run + L2 层叠反转）；完整逐字符 UBA（X1-X9 / W1-W7 / N0-N2 / I1-I2 隐式层级，N0 成对括号 BD16 配对与 W1 NSM）；阿拉伯/希伯来真实字体验收（仓库内 Amiri，连字与 cursive joining 度量断言）；bidi 格式控制符（LRE/RLE/PDF/LRO/RLO/LRI/RLI/FSI/PDI、LRM/RLM）输入侧闭环（渲染侧零宽零墨迹 + 序列化往返保留字节）；修复 `hit_test_char` / `hit_test_char_inclusive` 对 x<=0 无条件返回 0 的 RTL 语义 bug。
- **CLDR 复数 + 本地化格式**：`i18n/plural.h`（六类枚举 + 规则表驱动分类器，覆盖 ar/ru/fr/en·de/zh·ja）；`i18n/string_table.h` 复数扩到 CLDR 六类；`i18n/format.h`（数字/日期/货币按 Locale 格式化，守零依赖自研表）。

**缺口（仅平台接线，待对应平台与真实环境）**
- 平台 IME 接线（Win32 TSF/IMM32、macOS NSTextInputClient、X11/Wayland xkbcommon + IBus/Fcitx、Wasm）——无头 Linux 无法完成，须有真实输入法环境后补。

## 无障碍平台桥接（Tier 1）

**已落地（alpha.3）**
- **语义树补全**（无平台依赖）：`build_accessibility_tree` 回填 bounds、自填 name/value/hint；`Widget` 增 `accessibility_label()` / `accessibility_value()` / `accessibility_hint()` 钩子并落位主流控件；`show == false` 子树不入树。
- **无障碍事件 + 设置注入**：`AccessibilityEvent` / `AccessibilityEventKind` + 事件通道（焦点/取值/结构三类，未安装处理器零开销）；`AccessibilitySettings{ reduce_motion, high_contrast, font_scale, screen_reader_active }` 支持 `Environment` 注入与进程级默认双来源；`reduce_motion` 由 `AnimationController::tick` 消费、`font_scale` 由 `Text::effective_font` 消费。`high_contrast` / `screen_reader_active` 仅落「设置位」，换色策略待定。

**缺口（待平台桥）**
- 首个平台桥（Win32 UIAutomation）：`Surface` 扩展点 `accessibility_provider()`；Win32 实现 provider 把语义树映射到 UIA 元素、动作映射到 UIA pattern、事件映射到 UIA 事件。pimpl 隔离、`AURORA_BACKEND_WIN32` 门控。
- 后续：macOS NSAccessibility、Linux AT-SPI2、Wasm ARIA（各平台独立增量）。

## 可测试性：交互模拟接线 + 测试临时文件规范（Tier 1）

**已落地（alpha.3）**
- `simulate_click/scroll/text_input` 三件套接线——经 `EventDispatcher` 走真实命中测试 + 派发，并打通远程/AI 工具面（InspectorServer `POST /api/input/{click|scroll|text}` + `aurora_mcp` `simulate_interaction`）。
- 消费者侧 TestController（`app/test_controller.h`）：内部以 `HeadlessSurface` + `Window` 跑真实脏区间帧循环，对外提供帧驱动（`pump` / `pump_and_settle` / `set_viewport`）、查找（`find_by_key|text|type`）、交互（`tap` / `drag` / `enter_text`）、断言（`expect_visible` / `expect_prop`）。
- 交互脚本 fixture（`tests/fixtures/ai_compat/interact_*.json` + runner），把「生成→交互→断言」纳入 AI 兼容测试。
- 测试临时文件规范：基目录 `cwd()/test_temp`（不可写回退系统 temp 并注原因）、逐用例唯一子目录 + `end_case` 清理、`.gitignore` 覆盖、`check_test_temp_hygiene` 看护 + `CODING_STANDARDS.md` §3 成文。
- Track 全部完成。

## 桌面完整度缺口（Tier 1–3）

**基本功（Tier 1，已落地 alpha.3）**
- **鼠标光标形状 API**：`CursorShape` 枚举（Arrow/IBeam/PointingHand/Resize*/Move/Crosshair/NotAllowed/Wait）；`Surface::set_cursor` 虚方法（默认 no-op）；`Modifier::cursor` + `Widget::cursor_shape()` 虚钩子 + 派发器悬停链解析（修饰链 > 钩子 > Clickable→PointingHand）；`cursor_rfc_name` 平台中立映射 SSOT（`kCursorShapeCount` 长度契约）。各真实后端覆写：HeadlessSurface 记录序列 seam；GlfwSurface/Win32Surface/X11Surface/MacOSSurface 平台光标 API（句柄缓存 + 析构释放）；D3D11Surface 复用 Win32 宿主映射（内部头 `win32_cursor.h` 的 `detail::set_win32_cursor`）；WasmSurface 保持空实现。
  - **真机验证台账（2026-09-13）**：✅ X11 编译 + 运行时双验证（11/11 形状逐项吻合，附 `tools/verify/x11_cursor_live_probe.cpp`）；✅ Wayland 编译验证（真机语义待合成器接线）；⛔ GLFW 本机无法编译（缺 `libgl-dev`/`libxrandr-dev` 等）；⏳ Win32/D3D11/macOS 未编译验证（本机 Linux 容器）。附带修复：`D3D11Surface` 缺 `native_handle()` 致 `surface_state().has_native_window` 恒 false，已补齐。
  - **真机验收探针**（`tools/verify/`，`AURORA_BUILD_VERIFY_TOOLS` 开关，不进 CTest）：X11 经 XFIXES 读回、Win32 经 `GetCursorInfo` 读回、macOS 经 `[NSCursor currentCursor]` 读回、GLFW 走自动能力核对 + 人工目视。各探针验收范围与退出码语义见其源文件头注释。
- **焦点作用域 / 焦点陷阱**：`FocusManager` 作用域栈（`push_scope`/`pop_scope`/`scope_depth`）；Dialog/Popup/Drawer 接线焦点陷阱；方向键导航（依赖 `Widget::paint` 写入 `focus_bounds_`）。

**已确认需支持（待规划）**
- ✅ 数据可视化/图表控件族（LineChart/BarChart/PieChart/ScatterChart/Sparkline）——已随 alpha.5 切片 1–9 全量落地。
- 内置音频播放（feature 宏 `AURORA_ENABLE_AUDIO`，opt-in）。
- 滚动位置保存/恢复 + 列表拖拽重排。

## 渲染与图形（Tier 3，GPU=wgpu）

**已落地（alpha.3）**
- **RHI 抽象抽取（纯重构，golden 逐位不变）**：`render/rhi/rhi_backend.h`（`rhi::CmdData` + `rhi::RhiBackend` 单一 `submit`）、`render/rhi/software_rhi.h/.cpp`（`SoftwareRhi` 把 18 类 `CmdKind` 逐条转发回 `Painter`，像素逐位等价）；`DisplayList::replay` 改为两个重载（`replay(rhi::RhiBackend&)` 为唯一实现、`replay(Painter&)` 为兼容薄壳，调用点零改动）。`DisplayList` 成为唯一绘制指令源，`Painter` 与 GPU 后端为其平级消费者。全量 ctest 260/260，golden 零漂移。
- **异步图片**（低风险先行）：`ImageFetcher`（URL → Task<bytes>，核心不内置 HTTP）+ 三级解析；`ImageView::from_url` 三态（占位 → Loading → 解码 → `ImageCache` 回填）。
- **Overflow::Scroll + 色彩管理**：`ScrollViewport` 内核（全库统一 clamp/符号约定）；`core/color_space.h`（SRGB/DisplayP3 转换，sRGB golden 保持逐位精确，P3 独立单测）。

**GPU/RHI 后续（待规划）**
- 引入 wgpu-native（或 Dawn）作为 opt-in 三方（`AURORA_BACKEND_GPU_WGPU` 默认 OFF）；实现 `WgpuRhi : RhiBackend`（device/queue/swapchain、纹理上传、pipeline、命令编码）；命令覆盖优先级与 shader 离线预编译；`WgpuSurface` 接入 swapchain present。
- 确定性策略重定义：golden 逐位基准 = 软件参考路径（唯一 SSOT），GPU 走容差 golden + 结构快照双层；GPU 补齐 blur/shadow/mask。
- 跨平台 GPU 矩阵（Vulkan/Metal/D3D12/WebGPU）+ `Platform::capabilities()` 可用性查询 + 自动回退 SoftwareRhi。
- GPU 独有能力（视频/大图零拷贝上屏、动画/变换 GPU 层缓存、compute 矢量光栅）。

## 动画与手势交互（Tier 2）

**已落地（alpha.4）**
- Timeline 编排原语（`animation/timeline.h`：`TimelineSpec`/`TimelineResolved` 纯值区间树，顺序/并行/交错组合子；`TimelinePlayer` 单主控制器驱动多轨道，`forward`/`reverse` 中断续播）。
- 手势驱动动画（pointer-agnostic）：`DragRecognizer`/`DragToDismiss`（`event/gesture.h`）、`Dismissible` 控件（`widget/dismissible.h`）；跟手 1:1 + 松手 spring 裁决，建在 `pointer_id` 抽象流，不直接依赖 `TouchEvent`。

**缺口**
- 滚动增强：scroll snapping/paging、pull-to-refresh、sticky headers、嵌套滚动协调、滚动驱动动画；联动 reduce-motion。

## 布局系统补强（Tier 2）

**已落地（alpha.3）**
- `OverflowStrategy::Scroll`（原等同 Hidden，已补真实滚动裁剪）；`BreakpointBuilder` 响应式原语；`Skeleton` / `GridView` 接入序列化工厂。

**缺口**
- `CrossAxisAlignment::Baseline`：Row 内文本控件按基线对齐。

## AI-first 护城河 + 命令系统（Tier 2）

**已落地（alpha.3）**
- MCP 实时 UI 编辑闭环基础：`hot_reload.h` + `apply_patch` 与 `aurora_mcp` 打通。
- 交互模拟经 `aurora_mcp` `simulate_interaction` 暴露。

**已落地（alpha.4）**
- 命令系统：`Command` + `CommandRegistry`（`commands.h`）为唯一真源，`bind_shortcuts` / `to_menu_items` 分别投影到快捷键与菜单，`CommandPalette`（`widget/command_palette.h`）为可搜索模态面板；`to_json()` 序列化信封经 `aurora_mcp` 的 `list_commands` / `invoke_command` 暴露给 AI（`COMMAND_DESIGN.draft.md` 已归档）。附带修复：Enter/Space 此前在焦点路由前被无条件消费，致 `TextInput::on_submit` 与 `RichTextEdit` 换行不可达，现由控件级 `Widget::wants_activation_keys()` opt-in 经 `on_key_event` 观察。

**缺口（待规划）**
- NL→UI + 视觉回归语义化：提升 `generate_ui`/`validate_ui` 一次通过率；截图 diff 输出语义化描述；扩充 `ai_compat` fixture。

## 工程与生态（贯穿）

**缺口**
- `TEST-R5` 门禁从「仅报告」恢复硬门禁（测试体系重写收束后）；`GUIDELINE.md` 配方做成可搜索文档站。
- 包分发（vcpkg/Conan）；度量并优化静态库冷启动与产物体积（`AURORA_ENABLE_*` 剪裁审计）。
- beta 收敛**不设硬门禁**（个人 OSS）。

---

# 第二部分：落地计划（按 1.0.0-alpha.n 阶梯）

> 前提：**alpha 阶段允许破坏性修改**——每阶梯把会动 API 形态的改造一次定形。每阶梯结束 = 全部门禁绿 + `CHANGELOG.json` 记 `alpha.n` + 打 tag。原则：**单阶梯要么纯重构（golden 逐位不变），要么一次定形一组相关破坏性 API，不混做**。

**`1.0.0-alpha.3`（已发布，2026-09-13）— 合并收口：构建地基 + 缺陷收敛 + 国际化 + 无障碍 + 可测试性 + 桌面完整度 + RHI 地基**

> 实际落地覆盖原计划的 alpha.3 至 alpha.6 全部已完成工作，统一归为 alpha.3 阶段。详细变更见 `CHANGELOG.json` 的 `1.0.0-alpha.3` 条目。要点：
> - 构建：WebAssembly（Emscripten）完整构建/测试链路；平台能力宏族 `AURORA_CAP_*`；GCC/Clang 误报告警收敛；安装导出补导 `AURORA_ENABLE_DEBUG`。
> - 缺陷：ODR 违规（漏导宏致消费端崩溃）、焦点几何（`Widget::focus_bounds_` 写入点）、X11 翻译单元 `CursorShape` 宏碰撞、D3D11Surface 缺 `native_handle()` 致 `has_native_window` 恒 false 等。
> - 输入与文本国际化：IME 组合输入核心、RTL/bidi 全切片（含真实字体与格式控制符闭环）、CLDR 六类复数与本地化格式。
> - 无障碍：语义树补全 + 事件通道 + 设置注入。
> - 可测试性：TestController 消费者侧 + 交互脚本 fixture + 测试临时文件统一规范。
> - 桌面完整度：光标形状 API 与跨后端平台接线（含 X11 真机双验证）+ 真机验收探针工具化；焦点作用域/陷阱。
> - 渲染：RHI 抽象抽取骨架（纯重构，golden 逐位不变）；异步图片；Overflow::Scroll + 色彩管理；Skeleton/GridView 序列化。
> - **破坏性许可点（实际触发）**：`environment → build_context` 依赖方向翻转（使用方须显式 `#include "aurora/core/build_context.h"`）；移除 clang-tidy 门禁与 `aurora_lint`；`Widget::focus_bounds()` 取值语义变更；`TextDirection`/`TextCompositionEvent` 新类型；布局镜像改 `TextAlign::Start/End`/`Flex`/`Alignment` 语义；`DisplayList::replay` 目标重塑（新增 `replay(RhiBackend&)` 重载，保留 `replay(Painter&)` 兼容壳，调用点零改动）。

**`1.0.0-alpha.4`（已落地）— GPU 栅格切片 + 动画/手势 + 命令系统**

- ✅ GPU 栅格落地（GL 3.3 core 切片，零三方依赖）：`GpuGlRhi` 全命令族实路径（几何/裁剪/渐变 LUT/PMA 图像/字形图集/Shadow/Blur/Blend/Mask/Composite），与软件路径逐公式同源；`RhiFrameSink` 帧调度契约 + `Surface::gpu_backend()` + `GlfwOptions::gpu`；初始化失败运行期自动回退软件纹理路径。契约见 `specification/03-layout-render.md` §8.7。
- 后续阶梯（原 wgpu 规划不变）：确定性策略重定义（软件 golden 为 SSOT，GPU 容差 + 结构快照）；wgpu-native / Dawn 作为 opt-in 三方（`AURORA_BACKEND_GPU_WGPU` 默认 OFF）覆盖 Vulkan/Metal/D3D12/WebGPU 矩阵。
- ✅ Timeline 编排原语 + 手势驱动动画（pointer-agnostic）：`TimelineSpec`/`TimelinePlayer` + `DragToDismiss`/`Dismissible` 已落地（`TIMELINE_DESIGN.draft.md` 已归档）。
- ✅ 命令系统：`CommandRegistry`（快捷键/菜单/面板统一真源）+ `CommandPalette` 模态面板 + `to_json()` 序列化经 `aurora_mcp` `list_commands`/`invoke_command` 供 MCP/AI 枚举调用（`COMMAND_DESIGN.draft.md` 已归档）。
- **破坏性许可点**：GPU 从「非目标」→「可选加速」（已回填 `SPECIFICATIONS.md`/`ARCHITECTURE.md`）；`commands.h` 重塑——原「命令式逃生舱」`run_raw` 移出为 `aurora::imperative::run_raw`（`imperative.h`），`commands.h` 让位给命令模型。**注**：纹理/字形图集/管线/交换链的进一步抽象与 wgpu 后端随后续 GPU 阶梯定形。

**`1.0.0-alpha.5`（规划）— 桌面完整度扩展 + 布局补强**

- ✅ 多窗口（`Application` 生命周期改造已落地：`WindowHost` + 单一帧循环 + `WindowRole`/`ExitPolicy` + 模态父子 + 跨窗 `WindowEventBus` + 每窗口 DPI/显示器/z 序 + 几何持久化；见 `CHANGELOG.json` 1.0.0-alpha.5 与 `specification/06-app-platform.md` §2.4）。遗留：Wasm 多窗口键盘/resize 事件路由（需 Emscripten 真机验证）。
- ✅ 图表控件族（LineChart/BarChart/PieChart/ScatterChart/Sparkline 五图全链路落地：软件 `Painter` 矢量原语 + 序列化 + 交互统一增强 + grow-in 动画；设计见 `CHARTS_DESIGN.draft.md` 已采纳）。
- 内置音频（feature 宏 `AURORA_ENABLE_AUDIO`，opt-in，各平台后端 pimpl 门控）。
- 滚动位置保存/恢复 + 列表拖拽重排。
- 布局补强：`CrossAxisAlignment::Baseline` 基线对齐。

**`1.0.0-alpha.6`（规划）— 无障碍平台桥接 + AI-first 闭环 + GPU 独有能力**

- 无障碍平台桥：Win32 UIAutomation（首个桥）+ macOS NSAccessibility + Linux AT-SPI（v2）+ Wasm ARIA。
- AI-first 闭环：MCP 实时 UI 编辑（`hot_reload` + `apply_patch` 即时 diff patch）、NL→UI、视觉回归语义化。
- GPU 独有能力：视频/大图零拷贝上屏、动画/变换 GPU 层缓存、compute 矢量光栅。

**`1.0.0-alpha.7`（规划）— 工程与生态 + 动画增强（进 beta 前收口）**

- 门禁恢复（`TEST-R5` 硬门禁）+ 文档站；包分发（vcpkg/Conan）+ 体积/启动优化。
- 滚动增强（snapping/paging/pull-to-refresh/sticky/嵌套滚动协调/滚动驱动动画，联动 reduce-motion）。
- 平台 IME 接线（任务 3，需真实输入法环境）、光标形状 Windows/macOS/GLFW 真机验收（补齐 alpha.3 余下未验证平台）。
- 冻结前置：API 冻结 SSOT 稳定 → 进 beta，此后「只增不删」SemVer 纪律生效。

> 阶梯粒度可调：单个 `alpha.n` 过重可拆 `alpha.n+1`；顺序按依赖锁定（RHI 必须先于 GPU 后端；fetcher 必须先于任何联网图片用例）。
> 所有工作项均不体现内部规划标记——以功能域与 alpha 阶梯描述，避免任务编号泄漏到代码/文档/提交信息。采纳后回填映射：输入与文本国际化→`specification/05`+`03`；无障碍→`core/accessibility`+`05`；可测试性→`08-tooling`+`CODING_STANDARDS §3`；渲染与图形→`ARCHITECTURE §8`+`SPECIFICATIONS §2/§4.1`+`03`；动画→`05`；布局→`03`；AI-first/命令→`08`+`06`；桌面完整度→`03`+`04`+`06`+`BUILD_OPTIONS`。
