# CHG-042 运行期窗口显隐与置顶开关（附 `set_size` 单位注释改正）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-042 |
| 提出日期 | 2026-10-27 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `include/aurora/window/surface.h`（`Surface` 虚段表尾追加四槽 `set_visible` / `is_visible` / `set_always_on_top` / `is_always_on_top`，`HeadlessSurface` 补记录值 seam 与观测读点；同文件 `set_size` 的头注释单位改正）、`include/aurora/window/window_state.h`（`compute_window_state` 增第三形参 `on_screen`，默认 `true` 保持既有两形参调用逐位不变）、`src/aurora/window/win32_host.{h,cpp}`（四槽落地 + `update_window_state` 改按屏上可见性推导 + 构造期按可见性档播种状态）、`include/aurora/window/win32_surface.h` 与 `include/aurora/window/d3d11_surface.h`、`include/aurora/window/wgpu_win32_surface.h`（转发共享宿主）、`include/aurora/window/window.h`（四枚透传方法 + `WindowOptions::visibility` / `WindowStyleOptions::always_on_top` 注释补运行期入口）、`include/aurora/window/platform.h`（`PlatformCapabilities` 增两枚能力位）、`tests/unit/` 新增一枚测试 TU；`codespec/` 三处回写 |

## 动机

1. **两条窗口开关只存在于构造路径，之后应用再无句柄可触达。** 可见性档由 `WindowOptions::visibility` 在 `Win32Host` 构造尾部 `ShowWindow` 一次定档，置顶由 `WindowStyleOptions::always_on_top` 在同一个构造尾部 `SetWindowPos(HWND_TOPMOST)` 一次定档（实测两处调用都在构造函数体内，宿主类没有任何运行期开关方法）。下拉式（quake 型）终端的呼出／收起与「常驻最上」是同一会话内的反复切换，构造期定档表达不了。
2. **现有替代品都不对。** `Surface::raise()` / `focus_window()` 只动 z 序与焦点，对一个已 `SW_HIDE` 的窗口无事发生；`Window::minimize()` 是「最小化」不是「隐藏」——任务栏留痕、带还原动画、且还原腿不可靠，用它冒充隐藏是错的产品形态。也就是说框架今天没有「让这个窗口不进入用户视野，稍后再让它出现」这条能力。
3. **缺的是入口而不是机制。** Win32 的 `ShowWindow` 与 `SetWindowPos(HWND_TOPMOST / HWND_NOTOPMOST)` 是现成 OS API，`GWL_EXSTYLE` 可作读回；本批只把这两枚 OS 动作接到公共面上，不新增任何消息处理、不引入线程。
4. **置顶运行期可改这件事此前没有任何说明。** 读源看不到「构造后不可改」的字样，应用侧只能靠读实现发现——这类「接口描述与实际能力不一致」正是最难查的缺陷形态，与上一条同源：能力要在头注释里如实申报。
5. **不支持的后端不得做成静默 no-op。** GLFW / X11 / Wayland / WASM / macOS 本批不接线（X11 的 `XMapWindow` / `XUnmapWindow` 与 Wayland 的置顶缺位都是真实工作量，且本批的消费动机在 Windows 侧）。若公共面只是空实现，用户按了热键窗口不动，会误判成「热键坏了」。故按仓库既有口径用 `PlatformCapabilities` 能力位申报「机制存在性」，与 `recent_documents_os_projection` / `native_menu_bar` 同款：只在桥已落地的方向翻位（Win32 / D3D11 共用同一宿主故同翻），未落地方向不预翻。
6. **可见性与 `WindowState` 的关系必须自洽。** `WindowState::Hidden` 的定义是「最小化或不可见」，而今天一条 `SW_HIDE` 永远不会产生该上报；更进一步，Win32 在 `SW_HIDE` 时会同步派发 `WM_ACTIVATE(WA_INACTIVE)`，若只补一句上报 Hidden，紧随其后的失焦会把状态改写成 `Occluded`——「隐藏却报遮挡」比不报更糟。故可见性必须进入状态推导的输入，而不是事后打补丁。
7. **`Surface::set_size` 的头注释与实现相反（文档缺陷，随手修）。** 注释写「程序化设置窗口外框尺寸（物理像素）」，实现是 `to_physical(逻辑 dp)` 再做非客户区补偿，语义为「请求的客户区尺寸」（其注释自陈「逻辑 dp → 物理客户区 → 物理外框」，并明写不能把逻辑值当外框尺寸直传）。同文件 `set_position` 的「物理像素」注释与实现一致，可见这不是口径统一而是 `set_size` 一处的注释过期。照注释写就把 dp 当物理像素传，150% 缩放下窗口小三分之一。实现是对的（客户区口径才与「内容要占多大」的产品意图一致），故改注释对齐实现。

## 变更内容

1. **`Surface` 四槽（虚段表尾追加，既有槽位序号不变）**：
   - `virtual auto set_visible(bool on) -> void {}`：运行期显隐开关。默认空实现＝该后端不接线（配合能力位申报，不静默）。
   - `[[nodiscard]] virtual auto is_visible() const -> bool { return true; }`：读回当前可见性。基类默认 `true` 与默认空 setter 配对——「没有隐藏过」是未接线后端的诚实答案，接线后端按 OS 实值或记录值回答。
   - `virtual auto set_always_on_top(bool on) -> void {}` 与 `[[nodiscard]] virtual auto is_always_on_top() const -> bool { return false; }`：同构的一对。
   - **读回取 OS 实值而非应用意图**：Win32 用 `IsWindowVisible` 与 `GWL_EXSTYLE & WS_EX_TOPMOST`。理由是最小化 / 被别的应用覆盖时「意图」与「实际」可以不同，框架不得把二者混为一谈；而这两枚 OS 查询恰好就是本批要改的东西，读它们等于读自己刚写下的值，不需要新增第二份真值源（也不需要新增宿主成员位）。
2. **Win32 / D3D11 落地**：`Win32Host::set_visible(on)` 走 `ShowWindow(on ? SW_SHOWNA : SW_HIDE)`——**显示档不抢焦点**（quake 呼出后要焦点是应用侧另调 `focus_window()`，与既有 `WindowVisibility::NoActivate` 档同一语义，避免「热键一按就打断用户当前输入」）；`set_always_on_top(on)` 走 `SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)`，保证切换不产生移动、尺寸变化或激活。两枚都以 OS 当前值先行判等（幂等：同值不再发 OS 调用，避免重复 `WM_WINDOWPOSCHANGED` 与多余的 `Present`）。顺带把构造期的置顶 `SetWindowPos` 补上 `SWP_NOACTIVATE`（与运行期同口径，建窗档不该因置顶而激活）。
3. **可见性进入 `WindowState` 推导**：`compute_window_state(minimized, active, on_screen = true)`——不在屏上恒为 `Hidden`，在屏上沿用旧规则（最小化 `Hidden`，否则激活 `Visible`／失焦 `Occluded`）。既有两形参调用点（X11 / GLFW）默认 `on_screen = true`，行为逐位不变。`Win32Host::Impl::update_window_state()` 改传 `IsWindowVisible(hwnd)`，于是 `set_visible(false)` 报 `Hidden`，`set_visible(true)` 按当时的 `minimized` / `active` 重算；`SW_HIDE` 带进来的那条 `WM_ACTIVATE(WA_INACTIVE)` 也落在同一函数里，被 `on_screen` 归一为 `Hidden`，不再出现「隐藏却报遮挡」。
4. **构造期状态播种**：`Win32Host` 构造尾部按可见性档播种 `Impl::state`（`Hidden` 档 ⇒ `Hidden`，`NoActivate` 档 ⇒ `Occluded`，`Normal` 档 ⇒ `Visible`），并让 `Impl::active` 与之一致。理由：宿主以 `Visible` 起步而窗口其实 `SW_HIDE`，则第一次真实变化会被「与上次相同」的去重吞掉——播种后新建 `Hidden` 档窗口与运行期 `set_visible(false)` 的读回是同一个值，这是本批判据要的自洽。**上报序列不变**：播种只写成员值，不触发任何 handler 调用（handler 此时尚未安装）。
5. **`Window` 四枚透传**：与既有 `minimize()` / `toggle_maximize()` / `raise()` 同形态的 `const` 透传（无 `surface_` 时为 no-op，读回落 `true` / `false`），使 `Application::window()` 与 `WindowHost::window()` 都能直接用。`WindowOptions::visibility` 与 `WindowStyleOptions::always_on_top` 的头注释同步补「建窗为该档初值，运行期可经 `Window::set_visible` / `set_always_on_top` 改」。
6. **能力位两枚**：`PlatformCapabilities::runtime_visibility` 与 `runtime_always_on_top`，按 `SurfaceKind` 在 `Platform::capabilities()` 里填：Win32 / D3D11（同一宿主同翻）为 `true`，其余恒 `false`。口径与既有 `system_drag_in` 等位一致——表「机制存在且本库已接线」，与运行期失败不互相替代。Wayland 的置顶缺位是协议层事实（无核心协议对应），如实缺位而非假装可用。
7. **无头 seam**：`HeadlessSurface` 记录两值（与既有 `set_position` / `set_size` / `set_window_mode` 记录值形态同族）并各自提供观测读点；`set_visible` 同时经 `notify_window_state` 上报（`false ⇒ Hidden`、`true ⇒ Visible`），使「显隐 → 应用级状态」这条链在无头 CI 里可断言，不依赖真机会话。
8. **`set_size` 注释改正**（不改实现）：单位写「逻辑 dp」，语义写「请求的客户区尺寸」，并点名「外框尺寸由框架按装饰 / 菜单补偿得出，调用方无需自己加」。

## 验收判据

1. **可见性**：无头后端 `set_visible(false)` 后 `is_visible()` 为假且 `WindowState` 上报 `Hidden`；再 `set_visible(true)` 回 `Visible`；隐藏期间的同值重复调用不再产生第二次上报（幂等）。真机腿由 `Window::window_state()` 与新建 `Hidden` 档窗口读回同一值这条口径覆盖（读回走 `IsWindowVisible`，与建窗档同源）。
2. **置顶**：`set_always_on_top(true)` 读回为真、`false` 读回为假；无头后端切换不影响 `position()` / `size()` 的任何记录值。
3. **状态推导**：`compute_window_state` 三形参口径逐值可单测——不在屏恒 `Hidden`（与 `minimized` / `active` 四组合都成立），在屏逐位等于旧两形参结果；既有两形参调用点不改即得旧行为。
4. **不支持后端不静默**：`PlatformCapabilities::runtime_visibility` / `runtime_always_on_top` 逐 `SurfaceKind` 矩阵（Win32 / D3D11 为真，X11 / Wayland / Glfw / MacOS / Wasm / Headless 为假），且假档下 `set_visible` / `set_always_on_top` 为可观测的 no-op（基类默认值不被改写）。
5. **既有行为不动**：`WindowVisibility` 三档建窗行为、`always_on_top` 建窗档、`position()` / `set_position()` 物理像素读数、`is_window_geometry_usable()` 只判不夹的判据、`WindowState` / `WindowMode` 上报序列逐位不变，既有用例全绿。
6. **`set_size` 注释与实现一致**：头注释单位为客户区逻辑 dp；文档 `codespec/specification/06-app-platform.md` 的几何面同口径，不再有「外框物理像素」的第二套说法。
7. **变异自证**：至少三条改坏各自转红——把 `compute_window_state` 的 `on_screen` 判定摘掉、把无头 `set_visible` 的状态上报摘掉、把 `is_always_on_top` 的读回写死为 `true`。
8. `lint`（改动相关 TU）与 `format-check` 绿；全量 `ctest --preset ninja-test` 无新增红灯。

## 回写落点

- `codespec/specification/06-app-platform.md` §2.4（窗口运行期控制与能力位归属）与 §5（`PlatformCapabilities` 字段表两枚新位 + `platform()` 现读口径段）。
- `codespec/specification/01-core.md` 无需改动；`codespec/specification/09-cli.md` 无关。
- `codespec/SPECIFICATIONS.md` 的 `SPEC.FEAT.APP.MULTI-WINDOW.001` 需求行补运行期显隐 / 置顶一档能力。
- CHANGELOG 的 [Unreleased] Added 与 Fixed（set_size 注释）各记一条。
- 公共 API 面为 Surface / Window 的成员方法，aurora_api.json 的 schema 面（widgets / enums / layout_rules / state_patterns / error_codes / debug）不承载成员方法，预期零差异，落码后重建核对

## 实施记录与偏离（落码期回写）

1. **两腿都取甲（直接开关），并附带把「读权」一并交出**：`set_visible` / `set_always_on_top` 各配一枚 `[[nodiscard]]` 读回（`is_visible()` / `is_always_on_top()`）。派发侧把甲 / 乙列为二选一，实际形态是「甲的开关 + 乙的读回」——读回的成本只是两枚内联虚函数，而没有读回就无法为「切换确实生效」写自动化判据（无头面只能断言记录值）。
2. **读回取 OS 实值而非意图位**：`IsWindowVisible` 与 `GWL_EXSTYLE & WS_EX_TOPMOST`。派发口径里「`is_visible()` 答意图还是答实际」需要写清——这里答**实际**，理由是最小化 / 被覆盖时两者可以不同；选 OS 实值还免掉一份额外宿主成员位（本批没有新增任何状态存储，除了 `HeadlessSurface` 的两枚记录值）。
3. **「不静默」用既有能力位机制，不新增 `supports_*` 虚函数**：`PlatformCapabilities` 增 `runtime_visibility` / `runtime_always_on_top`，逐 `SurfaceKind` 填。这是仓库既有口径（`system_drag_in` / `recent_documents_os_projection` / `native_menu_bar` 同族），派发侧给的「如 `Surface::supports_runtime_visibility()` 或让 setter 返回 bool」是同一件事的两种写法，取前者以保持 setter 的 `void` 签名与既有 setter 族一致；判据要求的「可查询」由能力位兑现，且缺位平台的默认实现是**可观测**的 no-op（`is_visible()` 恒为基类默认 `true`、`is_always_on_top()` 恒为 `false`），有对应用例锚定。
4. **可见性并入 `compute_window_state` 的第三形参，而不是补一条上报**：第三形参带默认值 `true`，X11 / GLFW 的既有两形参调用点一行未改即得旧行为；`Win32Host::Impl::update_window_state()` 传 `IsWindowVisible(hwnd)`。这样 `SW_HIDE` 顺带派发的 `WM_ACTIVATE(WA_INACTIVE)` 被同一函数归一为 `Hidden`，不存在「隐藏却报遮挡」的窗口。
5. **构造期播种只写成员值**：`Impl::active` / `Impl::state` 在建窗可见性档定档后按实值播种，不触发 handler（该时刻 handler 尚未安装），故 `WindowState` 上报序列与既往逐位相同，而「新建 `Hidden` 档」与「运行期 `set_visible(false)`」的读回同为 `Hidden`（用例 `constructed_hidden_window_and_runtime_hide_agree` 锚定）。
6. **顺带把构造期置顶补上 `SWP_NOACTIVATE`**：原调用是 `SWP_NOMOVE | SWP_NOSIZE`，与运行期口径不一致（置顶是 z 序属性，不该顺带激活）。这是本批唯一改动既有 Win32 调用参数的一处，属「同一开关的建窗档与运行期必须同语义」的必然结果，已写入规格。
7. **回写落点微调**：运行期开关的规格正文落在 `codespec/specification/06-app-platform.md` **§3.6（窗口可见性策略）**而非 §2.4——§3.6 才是 `WindowVisibility` 的既有归属，运行期开关与它是同一份状态的两侧；§2.4 的多窗口面不改（应用级 `window_state()` 聚合范围仍是主窗，本批未动）。§5 补两枚能力位段，§6.2 补 `compute_window_state` 的 `on_screen` 输入、构造期播种与无头 `set_visible` 上报三处。另顺手把 §5 的能力位字段清单补齐 `native_menu_bar`（上一批漏列，与本批同类字段同段申报）。
8. **验收判据兑现情况**：第 1—5 条由新增 `utest_window_runtime_control` 的 8 例兑现（未接线后端可观测 no-op、无头记录值 + `Hidden`/`Visible` 上报 + 同值幂等、`Window` 透传读回、置顶切换不动几何、`compute_window_state` 的 `on_screen` 逐值矩阵、能力位逐 `SurfaceKind` 矩阵、显隐经既有 sink 链聚合到应用级 `window_state()`、建窗 `Hidden` 档与运行期隐藏同值）；第 6 条见下条变异实测；第 7 条 `set_size` 注释改正后，全仓已无「外框物理像素」的第二套说法（`codespec/specification/08-tooling.md` 的 resize 几何段本就写的是客户区口径，与本批改正一致）；第 8 条回归与门禁全绿（全量 `ctest --preset ninja-test` **448/448**，含本批新增 8 例；`lint` 改动相关 TU 0 findings、`format-check` 1222 文件 0 非合规）。
9. **变异自证三条均已实跑**（三处改坏同批注入、一次重建复跑，命中用例互不重叠，故归属确定）：摘掉 `compute_window_state` 的 `on_screen` 判定 ⇒ `compute_window_state_on_screen_matrix` 转红；摘掉无头 `HeadlessSurface::set_visible` 的状态上报 ⇒ `headless_records_visibility_and_reports_state` / `visibility_aggregates_to_application_state` / `constructed_hidden_window_and_runtime_hide_agree` 转红；把基类 `is_always_on_top()` 写死为 `true` ⇒ `unwired_backends_are_observable_noops` 转红。合计 5 红 / 3 通过，三条改坏后全部还原本批源码并复跑回绿（8 通过），用例非空转。
10. **未新增真机探针**：派发侧的回货判据写明「Headless 即可，不要求真实桌面会话」，本批 8 例全部无头可断言；Win32 腿的两枚 OS 调用与既有构造路径同源（同一 `ShowWindow` / `SetWindowPos` 通路，只是入参档位不同），故真机面留给人工走查（下拉式面板的呼出 / 收起与置顶保持），不在本批交付自动化探针。
11. **不接线的后端保持不接线**：X11（`XMapWindow` / `XUnmapWindow`）、GLFW（`glfwShowWindow` / `glfwHideWindow`）、macOS 窗口壳、Wayland 置顶都属独立工作量，本批只在能力位与注释里如实申报缺位，未顺手实现任何一条——消费动机在 Windows 侧，先开的口子越窄越好。。
