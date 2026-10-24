# CHG-036 系统拖放剩余批次接线：Win32 拖出与 X11 / Wayland / macOS 双向

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-036 |
| 提出日期 | 2026-10-23 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.SYSTEM-DRAG-DROP.001 |
| 影响面 | `include/aurora/window/platform.h`（`system_drag_out` 能力位与 `capabilities()` 聚合按批次翻位）、`src/aurora/window/detail/win32_drag.h` 与 `src/aurora/window/detail/win32_drag.cpp`（拖出侧 `IDropSource` / `DoDragDrop` / 取消检测 / 拖拽影像）、`src/aurora/window/detail/system_drag_translate.h` 与 `src/aurora/window/detail/system_drag_translate.cpp`（X11 `XdndAction*` / Wayland 动作协商 / macOS `NSDragOperation` ↔ `DragAction` 折算）、`src/aurora/window/x11_surface.cpp` / `src/aurora/window/wayland_surface.cpp` / `src/aurora/window/macos_surface.cpp`（各 `Impl` 持有桥实例并接线；X11 / Wayland / macOS 的桥按「完整双向状态机」判据独立成 `detail/x11_drag.{h,cpp}`、`detail/wayland_drag.{h,cpp}` 与 `detail/macos_drag.{h,cpp}`，不内联进 surface；后两者为 ObjC++ TU，由 `cmake/AuroraBackends.cmake` 标 `LANGUAGE OBJCXX`）、`src/aurora/window/win32_host.cpp`（拖出后端每窗构造期注册与影像缩放的进程级提示推送）、`src/aurora/event/detail/system_drag_backend.h`（拖出注册接缝，本批新增内部头）、`src/aurora/event/drag_payload.cpp`（拖出后端注册接缝的判优与进程级在途闸登记）、`tests/unit/utest_drag_payload.cpp`（能力位方向级分叉锚定表按批次更新）、`tools/verify/` 新增 x11 / wayland / macos 三枚 drag_live_probe 并扩建 win32 探针的拖出腿（新件，不在此列名）；`cmake/AuroraVerify.cmake` 增 wayland 探针目标（pkg-config 探测 `wayland-server`）与 macos 探针目标（`enable_language(OBJCXX)` + `-fobjc-arc`）（pkg-config 探测 `wayland-server`，缺包则 configure 期跳过）、`codespec/manual-test/24-systemdrag.md` 新增拖出与三平台双向用例；无新增公共 API 符号 / 枚举 / 错误码 / feature 宏，`aurora_api.json` 预期零差异；另记 CHANGELOG.md（不属 codespec 树，不列为回写落点） |

## 动机

1. **缺口属实，且已按代码逐项实证。** 总提案 CHG-027 的首批（折算层 + 拖入测试接缝 + Win32 OLE 拖入四态与同步载荷，翻 `system_drag_in`）已交付；其余四个批次在当前代码中零实现：① Win32 拖出——`src/aurora/window/detail/win32_drag.cpp` 仅有 `DropTargetImpl : IDropTarget`（拖入），全文无 `IDropSource` / `DoDragDrop` / `QueryContinueDrag` / `IDragSourceHelper`，`win32_drag.h` 文件头明文「拖出……属后续批次」；`system_drag::begin` 在无 inert 测试后端时恒返回 `system-drag-not-supported`（`src/aurora/event/drag_payload.cpp`）；能力位锚定用例（`tests/unit/utest_drag_payload.cpp`）中八个 `SurfaceKind` 的 `system_drag_out` 全为 false。② X11——`src/aurora/window/x11_surface.cpp` 全文零拖拽代码：无 `XdndAware` 广播、无四态消息处理、无 selection 读取，`system_drag_in` / `system_drag_out` 均 false。③ Wayland——`src/aurora/window/wayland_surface.cpp` 全文零 `wl_data_device` / `wl_data_source` / `start_drag` 接线。④ macOS——`src/aurora/window/macos_surface.cpp` 全文零拖拽代码：无 `prepareForDragOperation` 闸门、无 `draggingSession(_:endedAt:operation:)` 结局投递。
2. **契约与折算层已就绪，缺口集中在平台桥与翻位。** 事件驱动执行模型（`system_drag::begin` 发起即返回 + `pump_reads()` 帧内排空兑现）、`SystemDragOutcome` 四结局、`DragOffer`（含影像 / 热点 / 缺省动作 / Move 确认）、拖出 inert 测试三件套、拖入入口 `system_drag::ingress::dispatch` 与拖入 inert 后端均已由 CHG-012 / CHG-023 / CHG-027 首批交付。剩余工作是把四个平台的 OS 会话翻译到既有契约上并按方向翻能力位——不改公共 API、不加错误码、不加 feature 宏。
3. **业界对标三平台内置，本库是「Win32 单向拖入 + 三平台零实现」。** Qt（`QDrag` / `QMimeData`）、GTK4（`GtkDragSource` / `GtkDropTarget` / `GdkDrop` 异步读取）、wxWidgets（`wxDropSource` / `wxDropTarget` / `wxDataObject`）均把拖入拖出做进事件模型；SPEC.FEAT.APP.SYSTEM-DRAG-DROP.001 的「拖入反馈与拖出发起、跨后端一致」与现状之间正差这四个批次。
4. **为何单独立提案而非在 CHG-027 内续批。** CHG-027 首批已交付、范围就此凝固；剩余四批各有需要单独评审的设计决策（Win32 进程级重入闸的待办补拍时序、X11 selection 异步 round-trip 与 `XdndFinished` 唯一成功依据、Wayland 接口版本 3 的动作协商、macOS `prepareForDragOperation` 闸门落点）。按「提案是可评审单元」的纪律拆开：编号是句柄、时序由提出日期承载（`tools/check/check_change_proposals.py` 头部 Rules 同款口径）。CHG-027 已裁定的各条（重入闸形态、能力位按方向独立翻位、不加 feature 宏、Glfw 恒 false、窄路径不备 fallback）本提案全盘继承、不重新裁决。

## 变更内容

1. **拖出后端注册接缝（新增，detail 层、非公共 API）**：`system_drag::begin` 当前只认 inert 测试后端；新增进程级真实后端注册面（内部头，落在 `src/aurora/event/detail/`，声明发起 / 取消两枚入口与在途查询），由各平台桥在窗口宿主持有期注册、销毁期注销。`begin` 的判优序为「inert 测试后端（安装期间）→ 真实后端 → `system-drag-not-supported`」：inert 安装期间由 inert 接管（只记录要约、不触达真实后端，与 CHG-027 首批 inert 口径一致），未安装才落真实后端分支，两者皆无时如实报 not-supported。无新增公共符号，`aurora_api.json` 零差异。
2. **Win32 拖出（`IDropSource` + `DoDragDrop`）**：
   - `IDropSource` 实现 + `DoDragDrop` 发起；取消检测取 `IDropSource::QueryContinueDrag` 返回 `DRAGDROP_S_CANCEL`（Win32 取消检测的唯一来源：Esc / 程序化取消标志 / 右键 → CANCEL，左键未按 → DROP）；`DoDragDrop` 返回值经 `exit_of_drag_out` 折成退出码枚举、再由 `result_from_win32_drag_out` 折算终态——`DROPPED` + 效果位非零 → `Succeeded` + 动作（经既有 `action_from_win32_drop_effect` 取 Copy → Move → Link），`DROPPED` + `DROPEFFECT_NONE` → `Rejected`，`CANCELED` / `USEDEFAULTCURSORS` → `Cancelled`，异常 HRESULT → `Failed` + `None`（不冒充取消、不谎报成功）。
   - Move 回滚：`DROPEFFECT_MOVE` 落放后调 `DragOffer::on_move_confirmed`；返回 `Error` 即源删除失败，终态改写为 `Failed` + `Move` 并上报 `system-drag-move-rollback`（目标侧数据保持不动）。确认调用点严格早于 `on_end(Succeeded, Move)` 投递，该次序从外部可断言。
   - 拖拽影像：`IDragSourceHelper` 设置 `DragOffer::icon` 与 `icon_hotspot`（空图像交 OS 默认影像）；COM 单元由**拖拽线程自持一次 `OleInitialize`**（自有 STA）——`DoDragDrop` 要求调用线程有 COM 单元，与 UI 线程上拖入桥的 `OleInitialize` 互不相干，两个 STA 各管各的会话；线程侧初始化失败时拖出方向如实以 `Failed` + `None` 收尾，不谎报有通路。
   - 重入闸（继承 CHG-027 裁决）：`system_drag` 侧进程级闸——会话登记即置位、终态投递即清除；`DoDragDrop` 跑在库自管拖拽线程上，UI 线程帧循环在会话期间照常推进（模态循环不在 UI 线程上嵌套，故不需要「帧步进待办 / 返回后补拍」机制）；闸内在途时第二次 `begin` 被拒（事件层报 `system-drag-session-failed`，引擎侧在途标志为第二道闸，发起失败即回滚会话态、不悬挂后续 begin）；取消路径就地收口在途读取（不等帧循环）后**先**登记 `Cancelled`，拖拽线程迟到的终态登记被「首登记者赢」规则作废；自拖放取数经拖入侧在途要约直取（UI 线程直接调 `provide`，不经数据对象）——跨单元形态下数据对象指针被 COM 编组为代理，`GetData` 会让 UI 线程与拖拽线程互等到墙钟超时，直取从根上消解该互等。单线程 UI 下一次一会话的拒绝语义沿用 `begin` 既有的 in-flight 分支。
   - 能力位：`Win32` 与 `D3D11` 两类 `SurfaceKind` 翻 `system_drag_out`（共用 `Win32Host::Impl` 与同一桥实例，与拖入同口径）；`WgpuWin32` 不单列（渲染后端偏好，宿主仍是 Win32）；`Glfw` 恒 false。
3. **X11 Xdnd 双向（独立桥 `detail/x11_drag.{h,cpp}`，由 `X11Surface::Impl` 唯一持有）**：
   - 拖入前置：窗口创建后广播 `XdndAware` 属性并带协议版本（`XdndEnter` 携带源侧版本，按 ≤ 5 协商）；不广播则源侧不会向窗口发任何 Xdnd 消息，拖入无从开始。
   - 四态：`XdndEnter` / `XdndPosition` / `XdndLeave` / `XdndDrop` → Enter / Over / Leave / Drop；`XdndPosition` 的 root 相对坐标换算回窗口逻辑坐标；目标侧以 `XdndStatus` 回报「是否接受 + 动作 + 矩形」（accept 查询的 X11 落点）。
   - 载荷：`XdndDrop` 后经 selection 请求获取——异步 round-trip，天然走 `read_async` + `pump_reads`，同步 / 异步分界走折算层共享常量（单一真源，不散落各桥）。
   - 拖出：`XdndSetSelectionOwner` + 向目标发 `XdndEnter`；`SelectionRequest` 是 `DragOffer::provide` 的触发点；**`XdndFinished` 是落放成功的唯一依据**，缺失即未成功（取消检测走该路径，不新增 slug）。
   - 动作：`XdndAction*` 位集 ↔ `DragAction` 折算落 `system_drag_translate`（零平台头层，无头单测覆盖）；拖拽影像在 Xdnd 无标准协议，`DragOffer::icon` 按契约静默忽略（不改变会话语义）。
   - 能力位：`X11` 两个方向同翻。
4. **Wayland `wl_data_device` 双向（独立桥 `detail/wayland_drag.{h,cpp}`，由 `WaylandSurface::Impl` 唯一持有）**：
   - 拖入：`data_offer` + `offer`（线上名清单经折算层换 MIME）→ `enter` / `motion` / `leave` / `drop` 四态；`wl_data_offer.receive` 给的是管道写端，载荷由本端非阻塞逐帧读入（`tick` 排空）后收口 `finish`——Wayland 没有 X11 selection 那样的 `INCR` 分块分叉，大小载荷同一条路径。
   - 拖出：`wl_data_device.start_drag(source, origin, icon, serial)` 是发起入口，serial 取本次按下序（宿主指针按下路径经 `note_press_serial` 刷新，多窗口下只可能是本窗口的按下）；`wl_data_source` 的 `target` / `send` / `cancelled` / `dnd_drop_performed` / `dnd_finished` 分别对应目标反馈、`provide` 触发、取消检测、落放确认与会话收口——后两枚是 outcome 映射的来源。
   - 动作：`source_actions` / `action` 与目标侧 `accept` + `set_actions` 协商——这些在**接口版本 3** 引入；版本协商不到 3 时会话如实降级（不假装支持动作协商），不引入第二套语义。v3 下 `cancelled` 一词一义 = 用户取消，但落放已发生却没等来 `dnd_finished` 时按 `Rejected` 折算（目标没有 `finish` 即拒绝）；v1/v2 下该事件一词二义（取消 / 会话结束），按「是否应答过 send」分叉。
   - **协议事件序事实**：`source_actions` / `action` 在 `enter` **之后**到达，桥的 `on_enter` 先复位会话态再收状态，早到的会被清掉——探针按真实序驱动。
   - 修饰态：协议**不随指针事件送修饰态**，桥经 hooks 从 seat 的 xkb 缓存（`on_modifiers` 维护）取，与键盘 / 鼠标事件同一份缓存、同一折算入口。
   - 拖拽影像：start_drag 的 icon surface（`DragOffer::icon` 非空时建 buffer surface，空交 OS 默认）。
   - 能力位：`Wayland` 两个方向同翻；CI 的 `linux-x11-wayland-glfw` 作业**编译并 lint** Wayland 后端（含 `wayland-gen` 协议头生成），**不承载**真机拖放联调；真机取证由探针的进程内 mock 合成器腿（服务端自建 `wl_display` + 三段 global，客户端经 socketpair + registry 绑定拿到与桥同侧的真实代理）加可选冒烟腿（有 `WAYLAND_DISPLAY` 时建真实 `WaylandSurface` 泵帧）与人工段承担。
5. **macOS AppKit 双向（独立桥 `detail/macos_drag.{h,cpp}`，由 `MacOSSurface::Impl` 唯一持有）**：
   - 拖入：内容视图 `registerForDraggedTypes:` 登记后实现 `NSDraggingDestination` 五方法——`draggingEntered` / `draggingUpdated` / `draggingExited` 驱动三态与光标；**`prepareForDragOperation` 的返回值是 accept 闸门**（`query_system_drag_accept` 的落点：AppKit 只在其返回 YES 后调 `performDragOperation`，把闸门放在后者会让「拒绝」路径无处实现——目标侧从未收到数据却也无法告诉 OS 不接受）。取数在 `performDragOperation` 内**同步**完成（AppKit 读取 API 自身阻塞到数据就绪），故本平台没有另两桥那种 tick 级读取泵。
   - 拖出：`NSDraggingSource` + `beginDraggingSessionWithItems:event:source:`（event 取宿主指针按下路径经 `note_press_event` 暂存的 `NSEvent`，无窗口或无按下事件时**不发起**）；数据经 `NSPasteboardItem` 的 data provider 惰性送达；结局经 `draggingSession:endedAtPoint:operation:` 投递（operation 为 none → `Cancelled` + `None`，非零 → `Succeeded` + 折算动作），是 `Succeeded` / `Cancelled` 的来源。AppKit 没有公开的程序化中止 API，`system_drag::cancel()` 的取消意图由事件层就地登记（首登记者赢）。
   - formats / provider 经 `NSPasteboard` 映射：`public.utf8-plain-text` / `public.file-url` / `public.png` 登记，旧式 `NSFilenamesPboardType`（载荷是路径数组，本库不解析）与 `public.tiff` 刻意不登记——不声称能读。`NSDragOperation` ↔ `DragAction` 折算落 `system_drag_translate`（零平台头层，无头单测覆盖），位序 Copy=1 / Link=2 / Move=16 与另两平台都不同。修饰态：`NSDraggingInfo` 不携带修饰位集，桥经 hooks 从宿主键盘缓存取（骨架期如实报 `None`）；源侧 `ignoreModifierKeysForDraggingSession:` 返回 YES 让渡回库内唯一的 `proposed_action_for` 翻译点。拖拽影像经 dragging item 影像（`DragOffer::icon` 非空时用，空交 OS 默认）。
   - 能力位：`MacOS` 两个方向同翻（窗口壳仍为骨架 ⇒ 运行期 `install()` 因无内容视图必失败并记日志降级；能力位表机制归属，与 `multitouch` 等位的声明口径一致）。CI 的 `macos-appkit-glfw` 作业**编译**该后端与探针（`--target aurora_verify`），**不承载**真机拖放联调（无人值守 macOS 无窗口系统）；真机取证由探针自动段（协议对象层替身：真实 `NSPasteboard` + 库自带目标视图 + 一枚只回答桥真正读取的三个 `NSDraggingInfo` 选择子的对象驱动桥）与人工段承担，人工段待窗口壳实写后可执行。
6. **批次次序与依赖**：注册接缝 → Win32 拖出（reference 实现，含重入闸与 Move 回滚）→ X11 双向 → Wayland 双向 → macOS 双向 → 末批全量门禁与真机记录归档。每批次独立可合入、独立翻位；后续批次复用接缝与 inert 测试件，不重造。**进度**：五批（接缝 + Win32 拖出 + X11 双向 + Wayland 双向 + macOS 双向）全部交付并推送，五个后端的能力位已按方向翻位；本地全量门禁（ctest 443/443、format 1200/1200、xref / change_proposals / version / manual_test_format / doc_comments 全绿）与 CI 全绿（末轮 run 40116831568）。macOS 批在本仓库内**只有编译证据**（CI `macos-appkit-glfw` 作业的 `--target aurora_verify` 编出 `aurora_verify_macos_drag`），运行期判据须在真机 macOS 上照 manual-test 第 24 卷 `TC-SYSTEMDRAG-024` 至 `028` 执行后登记；窗口壳实写前 `TC-SYSTEMDRAG-027` / `-028` 按 BLOCKED 处理。故本提案归档，遗留的真机执行记录按卷第 3 节随执行补登。
7. **横切纪律（继承 CHG-027，不重新裁决）**：
   - 不新增公共 API 符号 / 枚举 / 错误码（机制缺失、会话失败、读取失败、解码失败、取消、Move 回滚六枚既有 slug 全数复用）；不新增 feature 宏（后端寄生平台 TU，编译期门控由平台自身完成，`check_platform_macros` 照过）；`codespec/BUILD_OPTIONS.md` 与 `debug_api.toml` 不动。
   - 载荷编解码依赖点名：uri-list ↔ 平台路径走仓内既有的 `src/aurora/app/uri_list.h`（内部头）；Win32 文本 `CF_UNICODETEXT` ↔ UTF-8；图像 `CF_DIBV5` ↔ PNG 受 `AURORA_ENABLE_IMAGE_PNG` 门控，关闭时该格式如实不可得、不假装支持。
   - 测试接缝全复用：拖出终态经 `emit_session_end` 驱动（inert），拖入四态经 `system_drag::ingress::dispatch` + 拖入 inert 后端；真实后端接线不替换接缝、只补真机探针。
   - 非目标：WASM DataTransfer（CHG-027 可选批次，保持可选、不进本提案）；GLFW 拖放（结构所迫恒 false，沿用 CHG-027 结论）；应用内拖拽（`AURORA_MIME_WIDGET` 既有通路）。
   - 真机探针：新增 x11 / wayland 两枚 drag_live_probe（`tools/verify/` 口径、`EXCLUDE_FROM_ALL`、不进 CTest），win32 探针扩建拖出腿；无头 CI 证明不了的只剩平台接缝与真机拖放联调两件事。

## 验收判据

1. **能力位方向级分叉表按批次更新**：Win32 / D3D11 / X11 / Wayland / MacOS 两方向 true（五批已翻）；Glfw / Headless / Wasm 恒 false；「未接线先声称」的变异（为未落地方向翻位）使对应用例转红。
2. **Win32 拖出真机闭环**：应用拖文件到桌面 / 文件管理器，`on_system_drag_end(Succeeded, Copy)`；无修饰键按 `DragOffer::default_action`；Esc 取消 → `(Cancelled, None)` 且 `on_system_drag_cancel` 严格早于 `on_system_drag_end`；目标拒绝 → `(Rejected, None)`。
3. **重入闸判据**：在途会话期内第二次 `begin` 被拒（`system-drag-session-failed`，`utest_drag_payload` 锚定）且首会话终态不受污染；取消路径就地收口在途读取（不等帧循环）后**先**登记 `Cancelled`，拖拽线程迟到的终态登记不改写已排定终态（「首登记者赢」）。
4. **Move 回滚**：注入 `on_move_confirmed` 失败 → 终态 `Failed` + `Move` + `system-drag-move-rollback` 可观测（经 `test_last_session_error` 往返），目标侧数据不动；确认调用点早于 `on_end(Succeeded, Move)`。
5. **三平台双向真机闭环（探针 + manual-test）**：拖入四态序列与载荷往返保真，`read_async` 读取超过共享阈值的载荷时帧序号推进 ≥ 1 且回调发生在排空阶段；拖出发起 → 落放成功终态；取消路径按各自平台来源（X11 未收到 `XdndFinished`、Wayland `wl_data_source.cancelled`、macOS operation 为 none）映射同一 `Cancelled`。
6. **错误映射不新增 slug**：会话建立失败 / 读取失败 / 取消 / 解码失败 / Move 回滚仍落既有六枚；Wayland 接口版本协商不到 3 时如实降级、不报假动作。
7. **无头回归全绿**：既有 `utest_drag_payload` / `utest_system_drag_translate` / `utest_system_drag_ingress` / `utest_dispatcher` / `utest_application` 全绿；新增平台折算用例（`XdndAction*` / Wayland 动作协商 / macOS `NSDragOperation` ↔ `DragAction` 与粘贴板类型 ↔ MIME）进 `utest_system_drag_translate`；inert 三件套安装期间真实后端不接管、卸载恢复「无后端 → not-supported」。
8. **门禁与全量测试**：`check_platform_macros` / `check_no_cjk_literals` / `check_no_hardcoded_paths` / `check_core_layer_boundary` / `check_codespec_xref` / `check_code_doc_sync` / `check_change_proposals` / `check_manual_test_format` 全绿；`check_api_schema_sync` 预期零差异（若批次中新增公共符号则先 `--target aurora_api_json` 再跑）；`lint`（仅改动 TU，先 `--print-tus` 确认集合）与 `format-check` 全绿；全量 `ctest -j16` 绿。
9. **探针与 manual-test**：四枚探针在目标平台可构建运行（Win32 / X11 / Wayland / macOS）；`codespec/manual-test/24-systemdrag.md` 新增用例编号从 `TC-SYSTEMDRAG-006` 起连续无缺口（现至 `TC-SYSTEMDRAG-028`），执行记录表行序与用例声明序一致；macOS 批的运行期判据（含人工段）须在真机 macOS 上执行后方可登记，开发期仅 CI 编译证据。

## 回写落点

- `codespec/specification/05-event-navigation.md`（§5.2 拖拽节：四平台拖出侧事件翻译、终态四结局的逐平台来源、取消检测映射表）
- `codespec/specification/06-app-platform.md`（§8.9 系统拖放节：拖出后端注册接缝与 `begin` 判优序、Win32 重入闸与 Move 回滚、三平台双向接线现状与能力位终态；§5 能力位两位的方向级说明同步）
- `codespec/ARCHITECTURE.md`（§8.5、§8.6 同体例：系统拖放桥四平台接线与桥实例归属，拖出后端注册面登记为内部 detail 层）
- `codespec/SPECIFICATIONS.md`（§5 特性表 `SPEC.FEAT.APP.SYSTEM-DRAG-DROP.001` 行：接线后「跨后端一致」口径的承载与后端能力细化说明）
- `codespec/manual-test/24-systemdrag.md`（新增拖出与 X11 / Wayland / macOS 双向用例，`TC-SYSTEMDRAG-006` 起连续编号）
- `codespec/changes/system-drag-backend-wiring/proposal.md`（批次移交注记：其余批次由本提案承接，CHG-027 保留首批交付范围与全部裁决记录）
- 收尾：本提案状态随批次流转（已提议 → 实施中 → 已归档）。
- 发布记录落 CHANGELOG.md（`Added`：系统拖放拖出与三平台双向接线）——该文档不属 codespec 树，按 AGENTS.md 硬规则 5 不列为回写落点。
