# CHG-033 窗口几何恢复消费几何态：写读对称与一条幂等原语

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-033 |
| 提出日期 | 2026-10-22 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `src/aurora/app/application.cpp`（`apply_host_geometry` 按 `mode` 追几何态、位置与尺寸双让位、降级口径、`Minimized` 归 `Normal` 的日志）、`include/aurora/window/surface.h`（**新增一枚幂等虚函数 `set_window_mode(WindowMode)`**，默认空实现，**槽位追加在虚函数段表尾（`gpu_backend` 之后）——既有槽位序号不变**）、`include/aurora/window/window.h` **未改**（恢复腿经 `host.window()->surface()` 直达 Surface，无需向应用侧转发；`Window` 全头内联、无对应 TU）、`include/aurora/window/wayland_surface.h` 与 `src/aurora/window/wayland_surface.cpp`（set/unset_maximized 与 set/unset_fullscreen 组合）、`src/aurora/window/x11_surface.cpp`（`csd_toggle_maximize` 的 TOGGLE 语义重构为 ADD / REMOVE 两个 ClientMessage）、`include/aurora/window/wgpu_wayland_surface.h`（逐方法转发装饰器，`minimize` / `toggle_maximize` / `set_fullscreen` 均转发给 `host_`，新虚函数不跟着转发则该后端静默失效）、`include/aurora/window/win32_host.h` 与 `src/aurora/window/win32_host.cpp`（`PostMessage` SC_MAXIMIZE / SC_RESTORE / SC_MINIMIZE）、`include/aurora/window/glfw_surface.h` 与 `src/aurora/window/glfw_surface.cpp`（glfwRestoreWindow / glfwMaximizeWindow / glfwIconifyWindow）、`tests/unit/utest_window_geometry.cpp`（并入新增用例，不新建文件）、`codespec/specification/06-app-platform.md`、`codespec/SPECIFICATIONS.md`、`codespec/manual-test/10-window.md`；**新增 vtable 槽，需全量重编（ABI 面）**，**源码兼容**（默认空实现，自定义 Surface 派生类零改），`aurora_api.json` 预期零差异——该文件无 classes / methods / functions 段，Surface 虚函数不入册（其枚举段只来自 `tools/include/known_enums.h`） |

## 动机

1. **保存腿写、序列化写、反序列化还校验，唯独恢复腿不读。** `capture_host_geometry` 取 `Window::window_mode()` 入快照，`window_geometry_to_json` 把 `mode` 写成六键之一，`window_geometry_from_json` 对越界的 `mode` 判格式错误，而 `apply_host_geometry` 只下发 `set_size` + `set_position`，`g.mode` 全程无人使用。**这不是缺 API，是缺一条消费点。**
2. **「写了不读」比「不写」更有害**：最大化 / 全屏那一档重启回不到那一档，而落盘文件里那一格白纸黑字，读者会以为功能存在——误导面与不一致面同时成立。
3. **只在注释里加一句「`mode` 目前不恢复」不可接受**：字段照存、行为照旧，等于把误导从代码搬进注释。同理，删除 `mode` 字段虽能消除误导，但它是公共类型的破坏性变更，且逐窗几何记忆的本意本就含「最大化那一档也要回得来」。
4. **消费者无法在应用侧补这一腿**：恢复发生在 `register_host` 尾部（首帧之前），那一段没有公共入口可挂，晚一帧补调就是「先按普通尺寸上一帧再最大化」的可见跳动；且这属于应用侧改框架的装配 / 销毁路径。

## 变更内容

1. **采纳形态：甲＝恢复腿按 `mode` 追一次几何态**。否决乙（删 `mode` 字段）：`WindowGeometry` 是公共类型，删字段是破坏性变更，且需求本意含「最大化那一档也要回得来」。
2. **新增幂等原语 `Surface::set_window_mode(WindowMode)`（默认空实现）——这是对初版「不改公共签名」的正式修订。** 三条实测理由：
   - `Window::toggle_maximize()` 是**切换**语义（Wayland 实现见 `src/aurora/window/wayland_surface.cpp`：已是 Maximized 时再调即 unset），接管恢复用它不可靠；`Window::set_window_mode(WindowMode)` 只是**写本地快照**、不转发后端、不产生任何效果。
   - `Surface` 基类的 `minimize` / `toggle_maximize` / `set_fullscreen` **默认全空**，全仓只有 Wayland 覆写；Win32 / X11 / GLFW / macOS / Headless 上落基类 no-op。原样落地会得到「仅 Wayland 真恢复、其余后端静默不恢复，而无头用例因快照被写绿而全绿」的**假绿**实现——正是任务书自己要反对的形态。
   - 新原语由后端实现并**经既有 `notify_window_mode` 路径上报**，使三层消费者（`Window::window_mode()` 快照、`Application::window_mode()` State、`set_on_window_mode` 回调）同链同步。
3. **新槽的代价与兼容性（承接复审裁决）**：新槽**追加在 `set_fullscreen` 之后（vtable 表尾）**，不得插在既有槽之间——`include/aurora/window/surface.h` 自述「vtable 槽稳定、属 Surface 契约」，故显式申报：**新增 vtable 槽，需全量重编（ABI 面）**；**源码兼容**：默认空实现 ⇒ 自定义 Surface 派生类零改即可编译。影响面已补 `src/aurora/window/x11_surface.cpp`（正文点名的 `csd_toggle_maximize` 重构）与 `include/aurora/window/wgpu_wayland_surface.h`（逐方法转发装饰器，不补转发则该后端静默失效）。
4. **否决「不加虚函数、改由宿主组合既有三枚原语」**：`Surface` 现有 `minimize` / `toggle_maximize` / `set_fullscreen` **全是切换语义**，无法表达 `Maximized → FullScreen` 这类直设目标态（凑出来要先读当前态再补 toggle，而异步 WM 下读到的态不可靠 ⇒ 又回到非幂等）；且该方案要把编排塞进宿主层，正撞 CHG-034 的 `window_host` 改动，本批不做跨提案的宿主重构。
5. **各后端实现分工（已按此落码）**：Wayland 实现 set/unset_maximized + set/unset_fullscreen 组合（复用其既有实现两侧），并在实现内立即 `notify_window_mode`——本地态仍由 configure 单独驱动（`on_xdg_surface_configure` 是唯一写方），稍后到达的 configure 以相同值幂等收敛；Win32 用 `PostMessage` SC_RESTORE / SC_MAXIMIZE / SC_MINIMIZE，WM_SIZE 回来定档上报（与用户走系统菜单同路径）；X11 把 CSD 的 `_NET_WM_STATE_TOGGLE` 拆成 ADD / REMOVE 两个 ClientMessage（新增 `Impl::send_net_wm_state` 助手，CSD 钮改按本地 mode 快照算目标态，一次性消掉非幂等原语），Minimized 走 `XIconifyWindow`；GLFW 落地 glfwRestoreWindow / glfwMaximizeWindow / glfwIconifyWindow（回调回来定档上报，与 Wayland 同构）；**macOS 为骨架后端，保持默认空实现并在规格中如实申报**；Headless 覆写为「记录目标态 + 同步 `notify_window_mode`」，无 OS 语义、零副作用、可断言；`WgpuWaylandSurface` 补转发（另三个同源面历来不转发任何几何原语，属统一既有缺口，见不做清单）。
6. **`apply_host_geometry` 的最终形态**：
   - `Normal`：`set_size` + `set_position`（现行为逐位保留）；
   - `Maximized` / `FullScreen`：**只追几何态，位置与尺寸都不下发**（升级初版的「只让尺寸」——最大化 / 全屏后位置由 OS / compositor 摆放，下发位置既无意义也可能触发多余的 `SetWindowPos` / configure）；
   - `Minimized`：**恢复为 `Normal`** 并留一条 **INFO**（tag `"app"`，见第 8 条日志口径），走 `Normal` 分支（位置尺寸照常恢复，用户可见地「打开」）。
7. **`Minimized` 政策（写死，不留给调用方猜）**：落盘的 `Minimized` 恢复为 `Normal`。除「启动即看不见、应用看起来没启动」外，补一条初版没提的理由：`WindowMode::Minimized` 与 `WindowState::Hidden` 耦合，而恢复腿没有设置 `WindowState` 的通道（`Window::set_window_state` 同样只是快照）——恢复成 `Minimized` 会直接制造「快照说最小化、可见性说可见」的分裂；`Normal` / `Maximized` / `FullScreen` 三档都不触发该耦合。
8. **降级拆成两件事（修正初版的误述）**：
   - **几何不可用**（屏外 / 尺寸非正）：走既有宽松判定 → `load` 回 `nullopt` → `apply` 不执行 → `mode` 天然保持默认 `Normal`。**当前结构里「默认尺寸 + 全屏」这种组合不存在**（无消费点），它是甲案落地后新引入的风险，故以测试钉住「降级后 surface 尺寸 == 创建默认值」。
   - **`display_id` 指向的显示器已不在**：这是**新路径**（`is_window_geometry_usable` 的判据只有「尺寸为正 + 与任一 work_area 相交」，**完全不看 `display_id`**，见 `src/aurora/app/window_geometry.cpp` 第 79-96 行）。建议**不拒绝恢复**（与「部分越界不拒」同精神：物理坐标相交就摆得回），但 `mode` 归 `Normal` + 一条 WARN。**按 `display_id` 拒绝恢复不在本批**（须先修 `is_window_geometry_usable` 并裁决与宽松口径的冲突，见「不做清单」）。
   - **日志口径（本轮定死）**：两条日志 tag 一律为 `"app"`，文案分开——① 降级归 `Normal`：`WARN`，文案形如 `stored window display is gone; restoring as Normal (key=<k>)`；② `Minimized` → `Normal`：`INFO`，文案形如 `stored window mode is Minimized; restoring as Normal (key=<k>)`。② 用 INFO 而非 WARN 的理由：用户最小化是合法操作，不是异常，记 WARN 会让每次启动都刷一条噪声。
9. **无头面**：`HeadlessSurface::set_window_mode` 记录 + 同步 `notify_window_mode`；`mode != Normal` 时安全 no-op、不报错、不让宿主被判为不可用（否则既有无头几何用例会因几何「不可用」转红）。
10. **恢复时刻的可信性（实测结论）**：`register_host` 尾部（首帧之前）时 `scale_factor` 在各后端已可信——Win32 建窗期定稿、X11 构造期读 Xft.dpi、Wayland 两次 roundtrip 后 wl_output scale 已到、GLFW 即时查询、Headless 恒 1.0。与「Linux 三后端缩放变化上报未实现」是**两个独立偏差**（那条是改缩放 / 跨屏后不更新），不得混成一个读数。`display_id` 在非 Win32 后端恒 -1，故「显示器不在」降级在 Linux 真机 / 无头面**结构性不可触发**，只能走单测（直接构造 `WindowGeometry` 喂 load 路径）。
11. **序列化面零改动**：六键稳定不变，越界 `mode` 仍判格式错误；`mode` 自引入起结构体从未变更（git 考古证实），不存在五键旧文件，缺键文件按格式错误处理是刻意口径。
12. **与 CHG-034 的交互（初版漏，落码后实测结论）**：保存点在 `reap_closed` 收集步、只对 `should_close()==true` 的宿主执行并打 `geometry_saved_` 一次性闩 ⇒ 关闭被否决的宿主不进收集步、不写快照、不打闩，用户确认后下次真关时以**最新几何**落盘——「否决不写、下次真关写最新」由 CHG-034 落地后的现有结构**天然满足**，`mark_geometry_saved` 闩语义无需放宽（初版建议的「一行判断」经实测无落点，已撤回）。
13. **消费侧零接线**：本提案只补框架内部的消费点与一枚原语，调用方无需改代码。

## 验收判据

1. **序列化往返（现在就该绿的回归保护，不占验收名额）**：并入既有 `tests/unit/utest_window_geometry.cpp` 的往返用例（已断 `Maximized`），参数化补 `FullScreen` / `Minimized` 两档，**不新建用例文件**。
2. **消费点存在 + 分窗分层断言（防假绿的关键）**：两扇无头宿主，先置 `Maximized` 落盘，重建后经 `set_window_geometry_store` 恢复。**主窗断言双层**：`app.window_mode().get()` State **与** `host->window()->window_mode()` 宿主快照同时回到 `Maximized`；**第二窗只断言 `host->window()->window_mode()` 快照**——`register_host` 装的 mode sink 只在 `raw == main_host_` 时转发（`src/aurora/app/application.cpp` 第 87-91 行），第二窗的几何态永不进应用级 State，对它同时断言两层必红。同案跑 `FullScreen` 与 `Normal` 三档。底座复用既有几何恢复用例（内存 `Preferences` + `persist_id`）。
3. **不被尺寸拉扯（后置断言，删「记录序」）**：恢复为 `Maximized` / `FullScreen` 后，断言 `surface->size()` **与** `surface->position()` **均等于创建默认值**（即不等于快照 size / origin）。**不查调用记录序**：`HeadlessSurface`（`include/aurora/window/surface.h`）只有 `cursor_log_` 与若干专项计数器，没有通用调用序日志，要查须给公共头加新成员，代价不划算；与判据 2、5 共用的观测面统一到 `size()` / `position()` 后置断言一处。
4. **`Minimized` 政策钉死**：落盘 `Minimized` 重启后为 `Normal`，且一条 **INFO**（tag `"app"`）可归因。
5. **降级不留脏态（单测，不写无头 itest）**：直接构造**屏外 / 非正尺寸**的 `WindowGeometry`（origin 远在任何 work_area 之外，或 `width` / `height` ≤ 0）走 `load_window_geometry`（内存 Preferences）证 `nullopt` + WARN（tag `"app"`）。**不构造 `display_id=9999`**：`is_window_geometry_usable` 完全不看 `display_id`，那样构造不会返回 `nullopt`，必红；按 `display_id` 拒绝恢复本批不做。
6. **越界值仍被拒**：既有 malformed 用例已覆盖 `mode=99`；补一条「经 store 的端到端越界」。
7. **无头不转红**：`tests/unit/utest_window_geometry.cpp` 与 `tests/unit/utest_multi_window.cpp` 全家全绿。
8. **门禁**：`check_api_schema_sync`（先 `--target aurora_api_json`；预期零差异）、`check_naming_conventions`、`check_doc_comments`（**新增公共虚函数，`@brief` / `@return` / `@note` 须写全**）、`check_api_budget`、`check_platform_macros`、`check_no_cjk_literals`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`registry_integrity`、`check_version_consistency`；本轮补 `check_arch_module_map`（后端 .cpp 改动）、`check_test_registry`（并入既有用例、不新建）、`check_no_hardcoded_paths`、`check_dpi_single_source`（提案对 X11 / Wayland 的 scale 可信性下了断言）；几何存储用例走 `isolation::temp_dir()`（`check_test_temp_hygiene`）；`lint`（仅改动 TU）与 `format-check` 全绿；按 ARCHITECTURE.md §14.5 补跑 wasm 与 Linux X11/Wayland 两配置；**新增 vtable 槽 ⇒ 全量重编后**全量 `ctest` 绿。
9. **真机读数（必须一次，两条分开记）**：POSIX 三后端 + Win32 + macOS 各一次——① `mode` 是否回到那一档（若本批只实装 Wayland，读数须明写「X11 / Win32 的 mode 恢复未实装」而非「读不回」）；② 最大化 / 全屏在 WM 与 compositor 下的实际尺寸与动画。
10. **变异自证**：把 `set_window_mode` 改回裸写 `Window::set_window_mode`（绕 `notify_window_mode`）⇒ 判据 2 的 State 断言必须转红；把 `Maximized` 分支改成仍下发尺寸 ⇒ 判据 3 转红；把 `Minimized` 恢复成 `Minimized` ⇒ 判据 4 转红；**删掉 `parse_and_validate` 里的 `is_window_geometry_usable` 判定（直接返回 parsed）⇒ 判据 5 转红**（覆盖判据 5）。

## 不做清单

- **不做 `Minimized` 真恢复**：恢复为 `Normal` 是写死的政策（变更内容第 7 条），不是待办。
- **不做按 `display_id` 拒绝恢复**：须先改 `is_window_geometry_usable` 并裁决与「部分越界不拒」宽松口径的冲突，移出本批。
- **不修 X11 与 Wayland 的 `set_size` / `set_position` 未实现**：既有独立缺口，位置尺寸恢复本身在 Linux 上就是 no-op；不在本批交付，真机读数须与 `mode` 读数分开记（原判据 9 ③ 移入此处，避免被误读为同批交付）。
- **不做 Win32 与 GLFW 的 FullScreen 落地**：两后端均无 monitor 切换通道（glfwSetWindowMonitor 会改视频模式、Win32 无对应 SC_*），`set_window_mode(FullScreen)` 调用时留一条 WARN（tag `"window"`）后不收场；真全屏属独立特性，不在本批。
- **不给 `WgpuX11Surface` / `WgpuWin32Surface` / `D3D11Surface` 补几何原语转发**：三个同源面历来不转发 minimize / toggle_maximize / set_fullscreen（仅 WgpuWaylandSurface 转发），本批只补已转发家族的 WgpuWaylandSurface；统一缺口单独排期，不在本批半修。
- **不处理 X11 的「Normal ← iconified」路径**：`XIconifyWindow` 不维护 `d.mode`（EWMH 推导只产 Hidden/Fullscreen/Maximized/Normal），恢复腿又把 Minimized 归 Normal，故该组合不可达；不做额外映射。
- **不做 macOS 落地**：骨架后端保持默认空实现，规格如实申报；真实 AppKit 侧随该后端实写一并落地。

## 回写落点

- `codespec/specification/06-app-platform.md`（窗口几何持久化节：`mode` 参与恢复的消费点、`set_window_mode` 幂等原语与各后端分工、位置与尺寸双让位口径、`Minimized` 恢复为 `Normal` 的政策及其理由、降级两拆口径、两条日志的 tag 与文案、无头 no-op 处置、恢复时刻 scale 可信性与两条偏差分离、`geometry_saved_` 闩的放宽；**并补记一条既有口径：应用级 `window_mode` State 只聚合主窗**——`register_host` 的 mode sink 仅在 `raw == main_host_` 时转发，非主窗的几何态只落在自己的宿主快照上）
- `codespec/SPECIFICATIONS.md`（§5 特性表 `SPEC.FEAT.APP.MULTI-WINDOW.001` 行：逐窗几何记忆「尺寸 / 位置 / 几何态」三项的现状复核；该行由 CHG-032/033/034 共享，批次末一次合并改写）
- `codespec/manual-test/10-window.md`（新增 `TC-WINDOW-020`：该文件 §1.4 规定序号不复用、取下一个可用序号，现最高 019 已由 CHG-034 占用，017/018 为兄弟提案早期预留留下的断档按约不复用；真机走查项：三档重启表现 + 六腿分列，须与既有「缩放与跨屏不更新」「X11/Wayland 位置尺寸未实现」两条读数分开记）
- 收尾：本提案状态随批次流转（已提议 → 实施中 → 已归档）。
- 版本与发布记录：条目类型 `Fixed`，落 CHANGELOG.md 的 `[Unreleased]` 节；**本批不单独升 alpha 号**，四份收口时一次性三处同步并跑 `check_version_consistency`。该文档不属 codespec 树，按 AGENTS.md 硬规则 5 不列为回写落点。

## 落码次序

仅改 `src/aurora/app/application.cpp` 内部一枚函数 + `include/aurora/window/surface.h` 一枚虚函数与各后端覆写，与 CHG-032 / CHG-034 的文件重叠仅在 `application.cpp` 的不同函数段。新 vtable 槽按 ABI 面处理：**先改 `surface.h` 并全量重编**，再落后端覆写与用例。建议次序：CHG-031 → **CHG-033** → CHG-032 → CHG-034。
