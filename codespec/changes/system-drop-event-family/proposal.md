# CHG-012 系统拖放事件族立骨（SystemDropEvent 四态取代 FileDropEvent）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-012 |
| 提出日期 | 2026-10-13 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.APP.SYSTEM-DRAG-DROP.001 |
| 影响面 | `event/event.h`（新增 `DragAction` / `SystemDropPhase` / `SystemDropEvent` / `DragPayload`，`FileDropEvent` 移除）、`widget/widget.h`（`on_file_drop` → `on_system_drop`、新增 `query_system_drag_accept`）、`event/dispatcher.h` 与其实现（派发入口换型）、`app/application.h` 与 `app/window_host.h` / `src/aurora/app/window_host.cpp`（`dispatch_file_drop` → `dispatch_system_drop`）、`window/platform.h`（`PlatformCapabilities` 增两位）、`src/aurora/window/win32_host.cpp` 的 `handle_dropfiles`（构造的事件类型换型，HDROP 解析与 `ScreenToClient` + `to_logical` 坐标换算逻辑不动）、`codespec/errors.toml` 与生成物、`tests/unit/utest_dispatcher.cpp` / `utest_event.cpp` / `utest_application.cpp` |

## 动机

1. **现状是「一半」**：跨出进程边界的系统拖放只有 Win32 一条窄路径——`win32_host.cpp` 的 `DragAcceptFiles` / `handle_dropfiles`（`DragQueryFileW` + `DragQueryPoint` + `ScreenToClient` + `to_logical`）构造 `FileDropEvent` 后交 `handler`，由 `WindowHost` 事件处理器经 `EventDispatcher::dispatch(Widget&, FileDropEvent&)` 派发到 `Widget::on_file_drop`；与之并列的另一条入口是程序化的 `WindowHost::dispatch_file_drop` / `Application::dispatch_file_drop`（`window_host.h` / `application.h`）。`FileDropEvent`（`event.h`）只有 `position` 与 `paths` 两个字段：只能传文件路径，无自定义 MIME 格式、无 enter / over / leave 拖入反馈（放置高亮在事件模型上不可能实现）、无 Copy / Move / Link 动作协商、无拖出发起；X11 / Wayland / macOS / WASM / GLFW 五个后端零实现。
2. **业界对标全部做进事件模型**：Qt（`QDragEnterEvent` / `QDropEvent` + `QMimeData`，`QDrag::exec` 返回 DropAction）、GTK4（`GtkDropTarget` + `GdkContentProvider` / `GtkDragSource`）、wxWidgets（`wxDropTarget` / `wxDataObject`）均三平台内置，MIME 为互操作货币、动作三态。Aurora 的应用内拖拽（`event/drag_drop.h`）已自有体系，缺口恰在「跨出进程边界」的那一半；补法只有与事件模型对齐，跨平台一致行为需求（SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001）才有着落。
3. **单一真源**：若保留 `FileDropEvent` 再叠加四态事件族，同一物理拖放将有两套事件语义（一套仅 drop 态、一套四态），控件要双实现、后端要双派发路径、文档要双份契约。alpha 阶段一次替换的成本最低；迁移集中在收束程（`on_file_drop` 旧调用方改写），中途不出现双真源并跑。
4. **立骨定位**：本提案只交付「事件模型 + 控件契约 + 能力位 + 错误码 + 测试底座」，平台后端（Win32 OLE、X11 Xdnd、Wayland `wl_data_device`、macOS dragging session）由接线程提案落地。交付后四态可在无头环境完整派发与单测，后端接线不改变本提案的任何契约。

## 变更内容

1. `include/aurora/event/event.h` 新增三件套：`enum class DragAction : std::uint8_t { None, Copy, Move, Link }`（`None` = 不接受 / 无动作，默认值）；`enum class SystemDropPhase : std::uint8_t { Enter, Over, Leave, Drop }`；`struct SystemDropEvent : Event`，字段为 `phase`、`position`（窗口逻辑坐标，沿用既有整幅表面逻辑坐标契约，见 CHG-007）、`formats`（可用 MIME 清单）、`proposed`（源建议动作）、`allowed`（目标允许的动作，与 `proposed` 的交集由宿主在派发前算定、供控件读取）、以及 `std::shared_ptr<const DragPayload>` 载荷句柄。`formats` 用裸 `std::string`（`event/` 不依赖 `app/`）；MIME 词汇表的单一真源由 CHG-013 的 `mime_types.h` 提供，两提案可各自独立合并，本提案不依赖 CHG-013 落地。
2. `DragPayload` 为 pimpl 类（头文件前置声明 + 读取方法，实现藏于 `src/aurora/event/`）：按 MIME 读取原始字节；`SystemDropEvent` 提供便利读取 `files()` / `text()` / `image()` / `bytes(mime)`，返回 `Result`——**格式不存在 = `Ok` 空值（空内容不是失败）**，与 `Clipboard` 既有空口径一致。enter / over 态载荷句柄可为空（数据延迟到 drop 读取），控件不得在非 drop 态假定有数据。
3. `include/aurora/widget/widget.h`：`on_file_drop` 更名 `on_system_drop(SystemDropEvent &e)`（消费时置 `e.is_handled` 阻止继续，默认不处理）；新增 `virtual auto query_system_drag_accept(const std::vector<std::string> &formats) -> DragAction`，基类默认返回 `DragAction::None`（不接受）。文档注明：宿主在 enter / over 态调用该查询以决定 OS 光标反馈，是 accept 语义的唯一真值源。
4. `include/aurora/event/dispatcher.h` 与实现：`dispatch(Widget &root, SystemDropEvent &e)` 取代 `dispatch(Widget &root, FileDropEvent &e)`；四态一律不冒泡、仅交给命中目标（既有 `FileDropEvent` 派发规则原样延续）。
5. `include/aurora/app/application.h` 与 `src/aurora/app/window_host.cpp`：`dispatch_file_drop` 更名 `dispatch_system_drop`，主入口为 `dispatch_system_drop(SystemDropEvent &e)`（四态统一程序化入口，phase 由调用方给定）；旧签名 `dispatch_file_drop(paths, x, y)` 保留并标 deprecated，内部构造 `phase = Drop` 且仅含 files 载荷的事件转发，保证迁移期内既有调用点保编译。
6. `include/aurora/window/platform.h`：`PlatformCapabilities` 同址增 `system_drag_in` / `system_drag_out` 两位，不改既有四字段语义。立骨期 `Platform::capabilities()` 对两者**恒置 false**（后端未接线，不假支持）；接线程按各后端实测翻位，并由接线程提案替换本条的过渡锚点。能力位表「机制是否存在」，运行期不可用（会话建立失败等）仍走 `Result` 错误，二者不互相替代。
7. `codespec/errors.toml` 新增两个 slug（只增不改），并重生成 `include/aurora/core/error_codes.gen.h` 与 `codespec/ERROR_CATALOG.md`：
   - `system-drag-not-supported`：平台 / 后端无系统拖放机制或能力位为 false 时发起会话或查询（category = platform、severity = error、retryable = **false**——机制缺失是稳态，重试无意义）；
   - `system-drag-session-failed`：会话建立或进行期失败（协商破裂、源数据提供中断等；category = platform、severity = error、retryable = **false**——按错误码定稿原则归稳态，瞬态重试由调用方按 hint 自行决策）。
8. 测试注入：新增 `SystemDrop` inert 后端（`install_test_backend` / `reset_test_backend` / `remove_test_backend` 三件套，`AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏裁切，同 `Clipboard::install_test_backend` 范式），使四态派发与 payload 往返在无头环境可测、且 Release 下零副作用。
9. 拖出类型先行：新增 `DragOffer`（`formats` + 按 MIME 提供数据的回调）与 `Widget::on_provide_drag` / `begin_system_drag` 入口声明；结束回调定名 `on_system_drag_end(DragAction)`，与应用内拖拽的 `on_drag_end` **分名**（同一控件上两套拖拽语义不共名）。动作口径统一：会话成功回 `Copy` / `Move` / `Link`，被取消、被拒绝或建立失败回 `None`（调用方不得把 `None` 当作成功）。后端实现（Win32 `IDropSource` / `DoDragDrop`、X11 Xdnd 主动侧、Wayland `wl_data_device.start_drag`、macOS dragging session）留接线程；立骨期发起拖出返回 `system-drag-not-supported`。**嵌套消息泵的重入 guard 属接线程设计项**（Win32 `DoDragDrop` 会在会话内跑嵌套泵、可能重入帧循环），本提案不引入任何嵌套泵。
10. 旧面**直接移除**（不做 deprecated 双轨）：`FileDropEvent`、`Widget::on_file_drop`、`EventDispatcher::dispatch(Widget&, FileDropEvent&)`、`Application::dispatch_file_drop(paths, x, y)` 与 `WindowHost::dispatch_file_drop(paths, x, y)` 一并删除。alpha 阶段允许破坏性改动，且「旧面 + 四态」并跑正是本提案动机 3 要消灭的双真源形态：留着它就要同时维护两套派发路径与两份契约，收益为零。迁移路径写进 `CHANGELOG.md` 的 Breaking 条目（`on_file_drop` → `on_system_drop`、`paths` → 按 MIME 读 `files()`）。
11. 既有测试同步迁移：`utest_dispatcher`（`TestBox` 的 `on_file_drop` override 与文件拖放派发三用例——命中派发 / 消费返回值 / 空白处不命中——改写为四态等价用例）；`utest_event`（`FileDropEvent drop{}` 默认构造断言改 `SystemDropEvent`）；`utest_application`（`dispatch_file_drop` 调用改新入口）。迁移期新增用例而非改写删除：四态各自一条，drop 态与旧用例逐条对应。
12. 热路径与无障碍契约随本提案落测试：accept 查询与格式匹配须常量级（预编译格式表，禁在拖放路径触发重布局）；over 态按指针频率到达，用例以连续 over 派发断言不触发布局；非拖拽替代路径（剪贴板粘贴 / 文件对话框）在文档层注明（配方属收束程）。

## 验收判据

1. 无头四态派发全绿：enter / over / leave / drop 各态——命中即派发到命中目标、不冒泡（stop-on-handled 同既有 drop 口径）、落点坐标契约与 CHG-007 一致、空白处不派发；`utest_dispatcher` 四态用例逐态断言（含旧三用例的逐条对应）。
2. accept 查询语义：基类默认 `DragAction::None`；控件覆写按 `formats` 返回 `Copy` 后 drop 照常派发且 `is_handled` 返回值正确；查询在 enter / over 各态被调用（记录调用次数与传入 formats 快照）。**变异自证**：将基类默认返回值从 `None` 改为 `Copy`，accept 相关用例必须转红（证明判据可观测，绿不算证据）。
3. payload 往返：drop 态经 inert 后端读取 `files()` / `text()` / `bytes(mime)` 往返保真；读不存在格式 = `Ok` 空值（非 Error）；非 drop 态访问载荷不崩溃、按空口径返回。
4. 旧面彻底移除：全仓检索 `FileDropEvent` / `on_file_drop` / `dispatch_file_drop` 在源码与测试侧零残留（仅 `CHANGELOG.md` 的历史条目与本文档保留旧名作为迁移说明）；`CHANGELOG.md` 记 breaking 与迁移路径（`on_file_drop` → `on_system_drop`、`paths` → 按 MIME 读取）。
5. 能力位锚定：单测守护立骨期末 `system_drag_in` / `system_drag_out` 在全部 `SurfaceKind` 下为 false（防「未接线先声称」）；该锚点为过渡判据，由接线程提案替换（提案正文注明，避免被误读为永久契约）。
6. API 面治理：`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移；`check_api_budget` 增量评估通过（新 API 全挂既有结构——事件族扩充 + 能力位增位，无新门面类型）。
7. 门禁与全量测试：`check_naming_conventions` / `check_doc_comments` / `check_platform_macros` / `check_no_cjk_literals` / `check_no_hardcoded_paths` / `check_core_layer_boundary` / `check_nolint_layout` 全绿；`lint`（仅改动相关 TU，先 `--print-tus` 确认集合）与 `format-check` 绿；全量 `ctest -j16` 绿。

## 回写落点

- `codespec/specification/05-event-navigation.md`（§2 事件表、§3.1 派发表、§5.2 拖拽节新增系统拖放子节：事件四态、派发规则、accept 查询契约、坐标口径）
- `codespec/specification/06-app-platform.md`（§5 运行时平台查询：两位能力位；§8 平台 Shell 新增系统拖放小节：程序化派发入口、inert 测试后端、错误码口径）
- `codespec/errors.toml`（两个新 slug）与 `codespec/ERROR_CATALOG.md`（生成物同步）
- `codespec/SPECIFICATIONS.md`（需求行已于本提案同批落定）
