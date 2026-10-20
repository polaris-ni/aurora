# CHG-027 系统拖放后端接线：四平台真实后端接入与能力位翻位

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-027 |
| 提出日期 | 2026-10-20 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.APP.SYSTEM-DRAG-DROP.001 |
| 影响面 | `include/aurora/window/platform.h`（`system_drag_in` / `system_drag_out` 能力位按方向翻位与 `capabilities()` 聚合）、`src/aurora/window/detail/` 新增折算层 system_drag_translate 与 Win32 桥 win32_drag 各一对 h/cpp、X11 / Wayland 桥内联于 `src/aurora/window/x11_surface.cpp` / `wayland_surface.cpp` 的 `Impl`、`src/aurora/window/win32_host.cpp`（窄路径移除与桥持有）、`src/aurora/window/macos_surface.cpp`、`tools/verify/` 新增三枚真机探针（win32 / x11 / wayland 各一枚 drag_live_probe）、`tests/unit/` 新增 utest_system_drag_translate 与既有四份（`tests/unit/utest_drag_payload.cpp`、`tests/unit/utest_drag_drop.cpp`、`tests/unit/utest_dispatcher.cpp`、`tests/unit/utest_application.cpp`）、`codespec/ARCHITECTURE.md`、`codespec/specification/05-event-navigation.md`、`codespec/specification/06-app-platform.md`、`codespec/SPECIFICATIONS.md`、`codespec/manual-test/` 新增第 24 卷 systemdrag；无新增公共 API 符号 / 枚举 / 错误码（六枚 slug 已在 `codespec/errors.toml`），`aurora_api.json` 预期零差异；另记 CHANGELOG.md（不属 codespec 树，不列为回写落点） |

## 动机

1. **唯一直缺口＝跨进程会话没有实现方。** CHG-012（`codespec/changes/system-drop-event-family/proposal.md`）与 CHG-023（`codespec/changes/cross-process-data-exchange/proposal.md`）先后交付了事件模型、控件契约、数据契约与测试底座，但系统拖放仍是纯契约态：`system_drag_in` / `system_drag_out` 恒为 false（`include/aurora/window/platform.h`），无测试后端时 `system_drag::begin` 一律返回 `system-drag-not-supported`（`retryable = false` 的稳态）。业界对标全部三平台内置：Qt（`QDrag` / `QMimeData`）、GTK4（`GtkDragSource` / `GtkDropTarget` / `GdkDrop` 异步读取）、wxWidgets（`wxDropTarget` / `wxDropSource` / `wxDataObject`）——本库是「事件族 + 契约完备、四个后端零实现」。
2. **事件驱动执行模型已审定，后端职责即翻译。** CHG-023 已把会话执行模型裁决为 GTK4 口径：`codespec/specification/06-app-platform.md` §8.9 现文明写「不引入嵌套消息泵」（原「接线期设计项」就此收敛为「不引入」），`begin()` 发起即返回、终态经 `system_drag::pump_reads()` 在帧循环排空兑现。于是各后端不必自行决定会话形态，只需把 OS 会话事件翻译成四态回调与终态投递。唯一例外是 Win32 `DoDragDrop`：规格否掉的是**库自发嵌套泵**，而 `DoDragDrop` 一旦调用，OS 侧固有的嵌套模态循环照旧运行——该窗口期内库自身的帧步进与派发须有重入闸，裁决见变更内容第 4 条。
3. **后端接线是可观察、可验收的收束项，且仓内已有同构模板可循。** `codespec/ARCHITECTURE.md` §8.5 无障碍桥与 §8.6 输入法桥已给出「平台中立值类型 → 零平台头折算层 → 平台桥 → 真机探针」的三层结构与桥实例归属纪律；系统拖放是同一形态的第四个桥，照模板落即可把绝大部分语义收进无头单测。载荷（大文件 / 远程数据）在 Win32 `IDataObject::GetData`、Wayland `wl_data_offer.receive`、X11 selection 上都是潜在的跨进程 I/O——异步通道（`read_async` + `pump_reads`）正是为它设计，优先级排在「先接一个小载荷同步路径、再接大载荷异步路径」；能力位翻位有既有「恒 false 锚定」测试守护（`tests/unit/utest_drag_payload.cpp`），翻位后须同步替换该锚定。

## 变更内容

1. **总体架构：照既有桥模板落三层与所有权**（`codespec/ARCHITECTURE.md` §8.5、§8.6 同构）：
   - 值类型层（已有，本提案不改）：`SystemDropEvent` 四态与 `SystemDragOutcome` 四结局（`include/aurora/event/event.h`）。
   - 折算层：`src/aurora/window/detail/` 新增 system_drag_translate 一对 h/cpp，**零平台头**——坐标换算、formats 映射、动作求交、outcome 映射四件平台无关逻辑全在这里，由 `tests/unit/` 新增的 utest_system_drag_translate 无头全证（仿 utest_ime_composition 覆盖 ime_composition 的口径）。
   - 平台桥：Win32 OLE 面宽，拆独立桥类 win32_drag 一对 h/cpp（同目录下，仿 `win32_ime.h`）；X11 Xdnd 与 Wayland wl_data_device 入口点少，分别内联在 `x11_surface.cpp` / `wayland_surface.cpp` 的 `Impl` 内（仿 XIM 与 text-input-v3 的处置，不为一个调用链多劈一个文件）。
   - 所有权：桥实例由 `Win32Host::Impl` **唯一持有**，`Win32Surface` / `D3D11Surface` / `WgpuWin32Surface` 转发同一实例（仿 §8.5 无障碍桥的持有方式，避免多份 HWND → 窗口映射分裂）；Linux 侧桥随各自 surface 的 `Impl` 生命周期。
   - 接缝：真实后端注册面落在库内 detail 层（非公共 API）；inert 测试后端优先（既有 `install_test_backend` / `reset_test_backend` / `remove_test_backend` 三件套），二者互斥不冲突；无后端时 `system-drag-not-supported` 降级语义不变。
   - 真机接缝：照 IME 三枚探针的先例（`tools/verify/win32_ime_live_probe.cpp` / `x11_ime_live_probe.cpp` / `wayland_ime_live_probe.cpp`，`AURORA_BUILD_VERIFY_TOOLS` 口径、`EXCLUDE_FROM_ALL`、不进 CTest），在 `tools/verify/` 新增 win32 / x11 / wayland 三枚 drag_live_probe；目标与 §8.6 同形——**无头 CI 证明不了的只剩三条平台接缝**。
   - 非目标：不新增公共 API 符号与错误码；不改 CHG-023 冻结的契约；不接 GLFW；不做应用内拖拽（走既有 `AURORA_MIME_WIDGET` 通路）；不做拖放动画（仅拖拽影像）。
2. **折算层契约（平台无关五件事 + 两个现在必须定死的来源）**：
   - ① 坐标：OS 会话坐标 → **窗口逻辑坐标（dp）**，走 `codespec/specification/05-event-navigation.md` §2.3 坐标契约；Win32 侧复用窄路径既有的 `to_logical()`（屏幕 → 客户区 → 逻辑）与 `scale_factor()`，不重写。
   - ② formats 枚举：OS 可用格式 → MIME 清单，取 `include/aurora/app/mime_types.h` 单一真源，禁硬编码串。
   - ③ proposed：宿主在构造事件前经 `proposed_action_for(modifiers, default_action)` 翻译，按 `AURORA_MODIFIER_PRESSABLE_MASK` 摘锁定态位。**拖入侧 `default_action` 的来源必须定死**：拖出侧取自 `DragOffer::default_action`，而拖入侧源是外部进程、没有 `DragOffer`，故取 OS 报告的源侧建议动作（Win32 `DragEnter` 的 `pdwEffect`、X11 `XdndEnter` 的 action 位、Wayland `source_actions`、macOS `draggingSourceOperationMask`）；OS 未报告时按 `Copy` 兜底，与 `proposed_action_for` 的既有兜底一致。
   - ④ allowed：`query_system_drag_accept` 查询并与 proposed 求交后回写给 OS 光标反馈，逐平台落点为 Win32 `DragOver` 的 `pdwEffect`、X11 `XdndStatus`、Wayland `set_actions`、macOS `draggingEntered` / `draggingUpdated` 返参。
   - ⑤ 载荷：小载荷走同步（`DragPayload::from_bytes`），大载荷 / 远程载荷走 `read_async` → `pump_reads()` 帧内排空，回调只调一次、宿主不排空不到；**同步 / 异步的分界阈值定为共享常量单一真源**，不散在四个平台桥里各写一个数。
   - 载荷编解码依赖点名：uri-list ↔ 平台路径双向走仓内既有的 `src/aurora/app/uri_list.h`（内部头，`src/aurora/app/clipboard.cpp` 与 `src/aurora/event/drag_payload.cpp` 已在用）的 `uri_list::encode` / `decode`；Win32 文本经 `CF_UNICODETEXT` ↔ UTF-8；图像载荷 `CF_DIBV5` ↔ `image/png` 依赖 PNG 编解码能力（受 `AURORA_ENABLE_IMAGE_PNG` 门控，关闭时该格式须如实不可得、不得假装支持）。
   - **取消检测逐平台来源**（Esc / 系统取消 → `Cancelled`）：Win32 `IDropSource::QueryContinueDrag` 返回 `DRAGDROP_S_CANCEL`；macOS `draggingSession(_:endedAt:operation:)` 的 operation 为 none；Wayland `wl_data_source.cancelled`；X11 未收到 `XdndFinished`。四路映射到同一个 `Cancelled` 结局，不新增 slug。
3. **错误映射与能力位纪律**：
   - 映射：会话建立失败 / 载荷不可得 / 取消 / 编解码失败 / Move 回滚 → `system-drag-session-failed` / `system-drag-read-failed` / `system-drag-cancelled` / `system-drag-decode-failed` / `system-drag-move-rollback`，不新增错误码（六枚已在 `codespec/errors.toml`）。
   - **两个能力位按方向独立翻位**（不是成对翻位）：批次次序里 Win32 拖入与拖出分两批落地，故首批只翻 `system_drag_in`，拖出批次落地后才翻 `system_drag_out`；未翻位的方向仍返回 `system-drag-not-supported`，与「能力位 false ⇒ 不声称支持」的既有语义自洽。既有锚定用例 `capability_bits_for_system_drag_are_false_until_backends_land`（`tests/unit/utest_drag_payload.cpp`，八个 SurfaceKind 全遍历）随之改为**方向级分叉断言**（每一方向对每个 SurfaceKind 有明确期望值），而非一次性全翻。
   - Windows 侧两类 Surface 同翻：`Win32` 与 `D3D11` 共用 `Win32Host::Impl` 持有的同一桥实例（D3D11 只是上屏形态，不换宿主）。**`WgpuWin32` 不是 `SurfaceKind`**——wgpu 是渲染后端偏好（`WindowOptions::renderer`），其宿主仍是 `Win32` / `X11` / `Wayland` 之一，故不单列、亦无需单独翻位。
   - Glfw 恒 false（**结构所迫，非延后**）：`include/aurora/window/platform.h` 是零 `#ifdef` 公共头，而「Glfw on Windows 翻位」需按宿主 OS 分叉，在无 ifdef 的公共头里做不到；且 GLFW 自持的 drop 通路仅「drop + 路径清单」、不承载四态协商，`src/aurora/window/glfw_surface.cpp` 现状零拖放接线。Windows 侧经 `glfwGetWin32Window` 接 OLE 属另一形态，不在本提案范围。
   - Headless 恒 false；Wasm 按第 8 条结论定。
4. **Win32 OLE（首批 reference 实现）**：
   - **硬前提**：全仓现无 OLE 初始化（只有 `src/aurora/app/file_dialog_win32.cpp` 里 scoped 的 `CoInitializeEx`），而 `RegisterDragDrop` / `DoDragDrop` 要求线程已 `OleInitialize`（`CoInitializeEx(APARTMENTTHREADED)` 不等价）。首批须补进程级 OLE 初始化与配对收尾，并定其失败时的降级路径（该方向能力位保持 false、会话建立失败落 `system-drag-session-failed`，不谎报）。
   - 拖入：`RegisterDragDrop` + `IDropTarget`；`DragEnter` → Enter、`DragOver` → Over、`DragLeave` → Leave、`Drop` → Drop；formats 经 `IDataObject::EnumFormatEtc`（`CF_HDROP` → uri-list、`CF_UNICODETEXT` → 纯文本、`CF_DIBV5` → PNG）；allowed 经 `DragOver` 的 `pdwEffect` 返参（第 2 条 ④ 的 Win32 落点）；proposed 从 `GetKeyState` 取修饰键后走 `proposed_action_for`。
   - 拖出：`IDropSource` + `DoDragDrop`；`QueryContinueDrag` 承担取消检测（第 2 条）；返回值 `DROPEFFECT` 映射 `SystemDragOutcome` + `DragAction`；`DROPEFFECT_MOVE` 后经 `DragOffer::on_move_confirmed` 确认（失败 → 终态 `Failed` + `Move` + `system-drag-move-rollback`）；拖拽影像经 `IDragSourceHelper` 设置（空图像交 OS 默认），热点取 `DragOffer::icon_hotspot`。
   - reentry guard（Win32 独立设计项）：`DoDragDrop` 会话内跑 OS 侧嵌套模态循环，该窗口期内到达的库内请求不得重入帧循环——裁决：`system_drag` 侧置进程级重入闸（会话登记即置位、终态投递即清除），闸内帧步进请求记待办、`DoDragDrop` 返回后由 `pump_reads()` 同一次排空补拍，闸内事件派发直接返回（事件留平台队列、不嵌套派发）。与 CHG-023 验收判据「无重入」同源，裁决回写 `codespec/specification/06-app-platform.md` §8.9。
   - 旧窄路径处置：既有 `DragAcceptFiles` / `WM_DROPFILES`（`src/aurora/window/win32_host.cpp` 的 `handle_dropfiles` 分族）与 OLE 二选一——OLE 是严格超集（四态 + 格式清单 + 动作协商 + 拖出 vs 仅 drop + 文件清单），故 Win32 批次内移除窄路径，避免双真源；git 历史即归档，不留死代码。**不保留其为 OLE 失败时的 fallback**，理由三条：① 触发条件（`OleInitialize` 失败）极罕见，为它长期维护第二套坐标换算与格式解析不成比例；② 窄路径无动作协商，`on_move_confirmed` 与 Move 回滚在其上是死的，保留即须在规格里长期描述一条半残通路；③ 与变更内容第 2 条「PNG 编解码关闭时该格式如实不可得、不假装支持」是同一条纪律——拖入亦不备语义更弱的备胎。OLE 初始化失败时按既有降级语义如实落 `system-drag-not-supported` / `system-drag-session-failed`，不谎报。
5. **X11 Xdnd（Linux 实现）**：
   - 拖入前置：窗口须广播 `XdndAware` 属性并带协议版本（`XdndEnter` 携带源侧版本，按 ≤ 5 协商）——不广播则源侧不会向窗口发任何 Xdnd 消息，拖入无从开始。
   - 事件：`XdndEnter` / `XdndPosition` / `XdndLeave` / `XdndDrop` 映射四态；`XdndPosition` 的 root 相对坐标换算回窗口逻辑坐标；目标侧以 `XdndStatus` 回报「是否接受 + 动作 + 矩形」（第 2 条 ④ 的 X11 落点）。
   - 载荷：数据在 `XdndDrop` 后经 selection 请求获取——异步 round-trip，天然走 `read_async` + `pump_reads`。
   - 拖出：`XdndSetSelectionOwner` + 向目标发 `XdndEnter`；`SelectionRequest` 是源侧 `DragOffer::provide` 的触发点；落放结果以 `XdndFinished` 为唯一依据（缺失即未成功，对应第 2 条的取消检测）。
   - 动作：`XdndAction*` 位集 → `DragAction`，与 accept 查询求交。
   - 能力位：X11 SurfaceKind 下按方向翻位。
6. **Wayland wl_data_device（Linux 实现）**：
   - 拖入：`wl_data_device` 的 data_offer 事件映射四态；`wl_data_offer.receive` 为异步数据回调，写成 `read_async`。
   - 拖出：`wl_data_device.start_drag(source, origin, icon, serial)` 是发起入口（原草案漏了这一条，缺它则拖出无从开始）；`wl_data_source` 的 target / send / cancelled / `dnd_drop_performed` / `dnd_finished` 分别对应目标反馈、provide 触发、取消检测、落放确认与会话收口——落放确认与收口两枚是 outcome 映射的来源。
   - 动作：`wl_data_offer.source_actions` / `dnd_action` 与目标侧 `set_actions` 协商——这些在 **接口版本 3** 引入（原草案误记为「协议 23」）。
   - 验收形态：CI 的 `linux-x11-wayland-glfw` 作业**编译并 lint** Wayland 后端（含 `wayland-gen` 协议头生成），**不承载**真机拖放联调；真机归 `wayland_drag_live_probe.cpp` 与 manual-test 第 24 卷。
   - 能力位：Wayland SurfaceKind 下按方向翻位。
7. **macOS dragging session（后续批次）**：
   - 拖入：`NSDraggingDestination` 的 `draggingEntered` / `draggingUpdated` / `draggingExited` 驱动四态与光标；**`prepareForDragOperation` 返回 Bool 是「是否接受」的闸门**（`performDragOperation` 仅在它返回 YES 后调用）——accept 查询落在这里，落在 `performDragOperation` 会让「拒绝」路径无处实现。
   - 拖出：`NSDraggingSource` + `beginDraggingSession`；结局经 `draggingSession(_:endedAt:operation:)` 投递，是 `Succeeded` / `Cancelled` 的来源（见第 2 条取消检测）。
   - formats / provider 经 `NSPasteboard` 映射。
   - 能力位：MacOS SurfaceKind 下按方向翻位。
8. **WASM DataTransfer（可选批次）**：HTML5 DnD：dragenter / dragover / dragleave / drop + `dataTransfer.items` / `files` / `setData`；按 wasm fetch 引擎（CHG-018，`codespec/changes/xplat-network-stack/proposal.md`）的同形态先例落地，浏览器侧零成本。能力位：Wasm SurfaceKind 下按 capability probe 翻位，或如实报不可用（无 probe 手段时保持 false 并在规格记明，不走假降级）。
9. **横切纪律**：
   - **不新增 feature 宏（显式裁决）**：后端寄生平台 TU，编译期门控由平台自身完成；能力位按 SurfaceKind 判定，「可产出 ⇒ 后端已编译」，宏参与只在无关平台制造恒假旋钮。故 `codespec/BUILD_OPTIONS.md` §1 无新增登记、`include/aurora/debug/feature_flags.h` 无新增字段，运行期查询经既有 `PlatformCapabilities`；`check_platform_macros` 照过。
   - **拖入测试接缝（新，双宏门控已裁定采纳）**：CHG-023 的 inert 三件套只覆盖拖出（`begin` / `emit_session_end`），拖入没有注入口，验收判据里「合成 OS 侧四态序列」无从落地。须补对称的拖入接缝：形态对齐既有三件套（安装 / 复位 / 移除，安装期间真实后端不接管）外加一枚「合成四态序列」的注入函数，`AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏裁切、Release 下零副作用，供折算层与派发链路无头全证。登记义务：新增调试面须同步 `codespec/specification/06-app-platform.md` §8.9 的测试注入清单（既有三件套与四件 `test_*` 接缝同段），措辞照 `codespec/GUIDELINE.md` 的双宏裁切范式书写。
   - 公共 API 面保持零 `#ifdef`；能力差异经 `capabilities()` 运行期查询（SPEC.PLATFORM.ZERO-IFDEF.001）。
   - manual-test 第 24 卷：用例编号按门禁规则取 `TC-SYSTEMDRAG-001` 起（`#### TC-<MODULE>-<NNN>`，MODULE 为文件名去数字前缀后大写），从 001 连续无缺口，体例对标 `codespec/manual-test/23-net.md`。

批次次序（依赖关系：折算层与拖入测试接缝先行，其余批次复用同一接缝）：先落「折算层 + 后端接缝 + 拖入测试接缝 + Win32 OLE 拖入四态与同步载荷路径（翻 `system_drag_in`）」，再落「Win32 拖出 + reentry guard + 拖拽影像 + Move 回滚（翻 `system_drag_out`）」，随后「X11 Xdnd 双向」「Wayland 双向」，再后「macOS 双向」，可选「WASM DataTransfer」；末批全量门禁与真机记录归档。

## 验收判据

1. **契约不回归与注入优先**：既有 `utest_drag_payload` / `utest_drag_drop` / `utest_dispatcher` / `utest_application` 四态与 inert 用例全绿；引入真实后端后 `reset_test_backend` 复位、`remove_test_backend` 恢复「无后端 → `system-drag-not-supported`」语义不变；安装测试后端（含新增拖入接缝）期间真实后端不接管。
2. **折算层无头全覆盖（新）**：`utest_system_drag_translate` 覆盖坐标换算（含 DPI 折算）、formats 枚举映射、动作求交、outcome 映射、拖入侧 `default_action` 来源（OS 报告值与缺席兜底两条）、取消检测四路映射、同步 / 异步阈值常量。**变异自证**：摘掉锁定态位掩码 → 修饰键用例转红；把兜底动作改成 `None` → 缺席兜底用例转红；把阈值改成各桥自持的散数 → 阈值一致性用例转红。
3. **拖入闭环（真机级，探针 + manual-test 卷）**：从 OS 文件管理器拖文件入应用，命中目标依次收到 Enter / Over / Drop，`files()` 往返保真；按格式矩阵验 `query_system_drag_accept` 与 OS 光标反馈（Win32 `pdwEffect` / X11 `XdndStatus` / Wayland `set_actions` / macOS `NSDragOperation`）逐项对应。
4. **拖出闭环（真机级）**：应用拖文件到桌面 / 文件管理器，`on_system_drag_end(Succeeded, Copy)`；按 `DragOffer::default_action` 验无修饰键动作；Esc 取消 → `(Cancelled, None)` 且 `on_system_drag_cancel` 严格早于 `on_system_drag_end`。Win32 批次另含重入闸判据：闸内执行一次会改动控件树的写操作，断言不触发嵌套派发、树状态与操作意图一致。
5. **Move 回滚**：注入 `on_move_confirmed` 失败 → 终态 `Failed` + `Move` + `system-drag-move-rollback` 可观测（经 `test_last_session_error` 往返），目标侧数据不动。
6. **大载荷不卡帧**：以变更内容第 2 条的共享阈值常量为界，`read_async` 读取超过阈值的载荷时断言帧序号推进 ≥ 1 且回调发生在排空阶段（`test_in_drain_phase()` 为真）；宿主不排空（`pump_reads` 不调用）⇒ 回调不到、不谎报；会话收口就地兑现不悬挂；同一 const 句柄并发读取各自独立登记、各自兑现一次。
7. **能力位方向级分叉**：每一方向对每个 SurfaceKind 都有明确期望（Win32 / D3D11 同翻、Glfw 与 Headless 恒 false、未落地方向仍 false）；「未接线先声称」的变异（为未接线方向翻位）使对应用例转红。
8. **错误映射**：五类失败分别落到既有五枚 slug，OLE 初始化失败亦落 `system-drag-session-failed`，无新增 slug。
9. **门禁与全量测试**：`check_platform_macros` / `check_no_cjk_literals` / `check_no_hardcoded_paths` / `check_core_layer_boundary` / `check_codespec_xref` / `check_code_doc_sync` / `check_change_proposals` / `check_manual_test_format` 全绿；`check_api_schema_sync`（本提案预期零差异——无新增公共符号；若批次中新增公共符号则先 `--target aurora_api_json` 再跑）；`lint`（仅改动 TU，先 `--print-tus` 确认集合）与 `format-check` 全绿；全量 `ctest -j16` 绿。
10. **窄路径移除无残留**：Win32 批次后 `src/` 与 `include/` 下 `WM_DROPFILES` / `DragAcceptFiles` / `handle_dropfiles` 全文零命中（以 `grep -c` 计数核验，不得以截断输出下结论）；`codespec/specification/` 与 `codespec/manual-test/` 中若仍出现该通路名，须为「已移除」的历史陈述而非现行行为描述。

## 回写落点

- `codespec/specification/05-event-navigation.md`（§5.2 拖拽节：四平台后端事件翻译、坐标换算与 §2.3 契约的衔接、光标反馈回写路径、SurfaceKind 与能力位的方向级对应表）
- `codespec/specification/06-app-platform.md`（§8.9 系统拖放节：真实后端接线现状、能力位终态、Win32 reentry guard 与 OLE 初始化裁决、窄路径移除结论；§8.9 测试注入清单追加拖入接缝与双宏裁切说明；§3.1 帧循环排空点如涉改动同步）
- `codespec/ARCHITECTURE.md`（§8.5、§8.6：无障碍桥与输入法桥的三层模板与桥实例归属；系统拖放桥按其体例登记为同级小节，写明折算层零平台头与桥实例由 `Win32Host::Impl` 唯一持有）
- `codespec/SPECIFICATIONS.md`（§5 特性表 `SPEC.FEAT.APP.SYSTEM-DRAG-DROP.001` 行：接线后复核「跨后端一致」口径的承载，必要时附后端能力细化说明）
- `codespec/manual-test/`（新增第 24 卷 systemdrag：`TC-SYSTEMDRAG-001` 起连续编号，格式受 `check_manual_test_format` 管辖，体例对标 `codespec/manual-test/23-net.md`）
- 收尾：本提案状态随批次流转（已提议 → 实施中 → 已归档）。
- 发布记录落 CHANGELOG.md（`Added`：系统拖放后端接线；`Fixed`：能力位翻位）——该文档不属 codespec 树，按 AGENTS.md 硬规则 5 不列为回写落点。
