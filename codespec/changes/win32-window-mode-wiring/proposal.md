# CHG-055 Win32 窗控四槽接线与全屏能力位

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-055 |
| 提出日期 | 2026-10-31 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `include/aurora/window/win32_surface.h`（新增四枚覆写：`minimize` / `toggle_maximize` / `set_window_mode` / `set_fullscreen`，均为既有虚槽，**不新增 vtable 项**）、`include/aurora/window/d3d11_surface.h`（同一 `Win32Host` 宿主的四枚内联转发）、`include/aurora/window/platform.h`（`PlatformCapabilities` 新增 `runtime_window_mode` / `runtime_fullscreen` 两格并在 `capabilities()` 按 `SurfaceKind` 置位）、`include/aurora/window/window.h`（仅头注释：`set_window_mode` 的快照写入口与动作入口分工）、`tests/unit/utest_window_runtime_control.cpp`（能力矩阵用例补两格与透传链桩，不新建用例文件）、`tools/verify/win32_window_mode_live_probe.cpp`（新增真机探针）、`cmake/AuroraVerify.cmake`（注册探针目标）、`codespec/specification/06-app-platform.md`、`codespec/specification/07-environment-modifier.md`、`codespec/SPECIFICATIONS.md`、`codespec/manual-test/10-window.md`；无 ABI 面变化（不新增槽、不改既有签名），`aurora_api.json` 预期零差异（`PlatformCapabilities` 不入册，实测以 `check_api_schema_sync` 为准） |

## 动机

1. **公共入口在 Win32 上是静默 no-op（读源实测）**：`include/aurora/window/surface.h` 的 `minimize()` / `toggle_maximize()` / `set_fullscreen(bool)` 三枚与 `set_window_mode(WindowMode)` 一枚均声明为 `virtual ... {}`；`include/aurora/window/win32_surface.h` 只覆写 `begin_window_move` / `begin_window_resize` ⇒ 四槽全部落到基类空实现。
2. **能力已经存在于同一后端**：`src/aurora/window/win32_host.cpp` 的 `Win32Host::set_window_mode` 已按目标态 Post `SC_RESTORE` / `SC_MAXIMIZE` / `SC_MINIMIZE`，结果由 `WM_SIZE` → `handle_size` → `classify_size_mode` 定档并上报（与用户走系统菜单同路径）；`FullScreen` 分支只留一条 WARN。**缺的不是实现，而是四行转发。**
3. **应用侧三个入口逐条落到空实现**：`include/aurora/window/window.h` 的 `Window::minimize()` / `toggle_maximize()` / `set_fullscreen()` 直转 `surface_->...`；`WindowChrome` 同构。`Borderless` / `Frameless` 档没有框架自绘标题栏，应用自绘标题栏只能走这三枚公共入口 ⇒ 按钮按下去既不动窗口也不报错。
4. **几何持久化的 `mode` 腿在 Win32 上「写了不读」**：`src/aurora/app/application.cpp` 的 `apply_host_geometry` 按落盘 `WindowGeometry::mode` 调 `sf.set_window_mode(mode)`（CHG-033 的写读对称承诺），但 Win32 该槽为空实现 ⇒ 最大化那一档重启回不来，且落盘文件里那一格白纸黑字。
5. **既有 CSD 自用路径不覆盖本场景**：`Win32Host::Impl::csd_trigger_button` 直发 `SC_*` 仅在该 `csd_title` 为真（`ClientSide` 档）时可达；`resolve_mode` 对 `Borderless` / `Frameless` 判 `title=false`，故应用自绘标题栏不经过那条腿。
6. **缺口范围比报告面更宽（实测纠正）**：三枚切换原语在 **Win32 / D3D11 / X11 / GLFW** 四家都落基类 no-op，只有 macOS、Wayland 与 `WgpuWaylandSurface` 四槽齐全；第四枚 `set_window_mode` 在 X11 / GLFW / Headless 有覆写，Win32 / D3D11 没有。所以「仅 Win32 缺四槽」不成立，但 Win32 家族（GDI 与 D3D11 共用 `Win32Host`）确实四槽全缺，本批按该家族收口并如实登记其余家。
7. **文档现为过高承诺**：`codespec/specification/06-app-platform.md` 的后端分工表把 Win32 的 `set_window_mode` 标成「真实现」——那实现是 `Win32Host` 的方法，`Surface` 槽上并无覆写；同文「已知缺口」只记了 `WgpuX11Surface` / `WgpuWin32Surface` / `D3D11Surface` 不转发三枚切换原语，未记 Win32 本体。
8. **为什么必须在本仓修**：`Win32Surface::native_handle()` 是公共 API 且返回 `HWND`，应用侧技术上可直发 `SC_*`；但那是永久的第二份 `SC_*` 语义，且违反 AGENTS.md 硬规则与「缺口先在框架侧补」的口径。框架侧只差转发。

## 变更内容

1. **`Win32Surface` 四枚覆写（接线，不做语义扩张）**：
   - `minimize()` → `win_->set_window_mode(WindowMode::Minimized)`；
   - `toggle_maximize()` → 以 OS 实值 `IsZoomed(hwnd)` 判当前档，为真下发 `Normal`、否则下发 `Maximized`（不读宿主缓存 `mode`：缓存由 `WM_SIZE` 回报，请求与回报之间隔着消息窗口期）；
   - `set_window_mode(m)` → 原样转发 `win_->set_window_mode(m)`（目标态语义，不做翻转；几何恢复腿与 `Maximized → FullScreen` 这类直设都靠它）；
   - `set_fullscreen(bool on)` → `on` 转 `win_->set_window_mode(WindowMode::FullScreen)`（由宿主留唯一一条 WARN），`off` 为 no-op（从未进入全屏，无可退出）。不静默、也不为将来的全屏实现另立语义。
2. **结果上报链不改**：仍走 `WM_SIZE` → `handle_size` → `classify_size_mode` → `notify_window_mode`，使 `Window::window_mode()` 快照、`Application::window_mode()` 响应式 State 与 `set_on_window_mode` 回调同链更新；Win32 侧不在覆写里同步 `notify`（异步后端只发请求）。同值重复请求（已最大化再设 `Maximized`）由 `SC_*` + `WM_SIZE` 幂等收敛，代码不做翻转。
3. **`D3D11Surface` 同宿主四枚内联转发**：与 `Win32Surface` 逐字同一口径，消除「同一 `Win32Host` 两个上屏面一真一假」。`WgpuWin32Surface` 不在此列（见不做清单）。
4. **全屏档选「可查询的不支持」，不在本批补实现**：补实现需要新增 `fullscreen` 布尔、进入前存还原几何、改 `classify_size_mode` 的定档来源、并与 CSD 揭示条状态机共管——那是独立特性而非接线。按框架既有口径「不接线不等于静默」，在 `PlatformCapabilities` 加两格：
   - `runtime_window_mode`：`minimize()` / `toggle_maximize()` / `set_window_mode()` 三枚已接到 OS；
   - `runtime_fullscreen`：`set_fullscreen()` 与 `set_window_mode(FullScreen)` 已接到 OS。
   - 置位矩阵：Win32 / D3D11 为 `true` / `false`；X11 / GLFW 两格 `false`（切换腿未接、GLFW 无 monitor 通道，见不做清单）；Wayland / MacOS 两格 `true`（各自四槽已齐；wgpu 是渲染偏好而非独立 `SurfaceKind`，随宿主同翻）；Wasm / Headless 两格 `false`（Headless 的 `set_window_mode` 是测试 seam，不代表 OS 能力）。
   - 口径写死：能力位答的是「**公共入口是否全部接通**」，不是「OS 有没有这个机制」，故 X11 虽有 `_NET_WM_STATE_FULLSCREEN` 也如实为 `false`，待其公共切换腿补齐后再翻位。
5. **`Window::set_window_mode` 保持快照写入口，不改成转发**：它是 `Application` mode sink 的写入点（`on_window_mode` 回调链路），改成转发会构成「上报 → 写快照 → 下发 → 再上报」回环。应用侧主动设目标档走 `Window::surface().set_window_mode(m)`（公共可变引用已在）；头注释补明这一分工，防后来者按直觉「修」成回环。
6. **与 CSD 自用路径的关系**：`csd_trigger_button` 的直发保留（框架自绘标题栏自用），本批只给应用自绘标题栏开公共入口；两条路径共用同一宿主 `set_window_mode` 语义，不抽第三份 `SC_*`。
7. **文档同批校正**：`06-app-platform.md` 后端表按「`Surface` 槽」与「宿主方法」两列分清；「已知缺口」更新为「D3D11 已补转发、`WgpuWin32Surface` / `WgpuX11Surface` 仍缺、X11 / GLFW 三枚切换腿仍缺」；`07-environment-modifier.md` 的程序化窗控段补 Win32 家族接线与两枚能力位。

## 验收判据

1. **能力矩阵（无头单测）**：`tests/unit/utest_window_runtime_control.cpp` 的既有能力矩阵用例补两格，逐 `SurfaceKind` 断言变更内容第 4 条的矩阵；`PlatformCapabilities{}` 默认两格 `false`。
2. **透传链回归保护（无头单测）**：以记录型 `Surface` 桩断言 `Window::minimize()` / `toggle_maximize()` / `set_fullscreen()` 与 `WindowChrome` 同名三枚确实落到 `Surface` 覆写（证明 `Window` 层未另存意图）。
3. **Win32 真机探针（新增 `aurora_verify_win32_window_mode_live_probe`，Windows + `AURORA_BACKEND_WIN32` / `D3D11`，自动段）**：
   - `minimize()` 后 `IsIconic(hwnd)` 为真、`window_mode()` 快照为 `Minimized`、`set_on_window_mode` 回调恰触发一次；
   - `toggle_maximize()` 在 `Normal` 上 → `IsZoomed` 为真、快照为 `Maximized`；再调 → 窗口矩形回到调用前逐值（`GetWindowRect`，允许 OS 边框 ±1px）且快照为 `Normal`；
   - `set_window_mode(Maximized)` 连调两次 → 两次后仍 `IsZoomed` 为真、上报值不变（目标态不翻转）；`set_window_mode(Normal)` 还原；
   - `set_fullscreen(true)` → 窗口矩形逐值不变、`runtime_fullscreen` 为 `false`、恰一条 WARN；
   - `decoration = Borderless` 与 `Frameless` 两档上，以上各条逐条成立（缺口暴露档）；
   - 退出码口径与既有探针一致：0 全通过 / 1 判据失败 / 2 环境不可用。
4. **几何恢复消费点不重复造**：`apply_host_geometry` 的 `mode` 语义已由 CHG-033 的无头用例锁住；本批只新增「`Surface::set_window_mode` 在 Win32 上真的生效」这一条（判据 3 第 3 项），不复制消费点用例。
5. **如实申报测试边界**：无头 CI 无法证明 Win32 接线（无真实 HWND 与消息泵），判据 1–2 只覆盖声明与透传链，判据 3 是唯一接线证据；探针不进 CTest，由人工/真机触发。
6. **不回退**：macOS / Wayland / `WgpuWaylandSurface` / Headless 的既有覆写行为与用例逐条不变；X11 / GLFW 源码未改。
7. **门禁**：`check_api_schema_sync`（先 `aurora_api_json`，预期零差异）、`check_naming_conventions`、`check_doc_comments`（新增覆写与两个结构字段须写全 `@brief` / `@return` / `@note`）、`check_api_budget`、`check_platform_macros`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`check_no_hardcoded_paths`、`check_arch_module_map`、`check_test_registry`（并入既有用例、不新建）、`lint`（仅改动 TU）、`format-check`；全量 `ctest --preset ninja-test` 绿。
8. **变异自证**：删掉 `Win32Surface::minimize` 覆写 ⇒ 判据 3 第 1 项转红；删掉 `set_window_mode` 转发 ⇒ 判据 3 第 3 项转红；把 `set_fullscreen` 改回基类静默 no-op ⇒ 判据 3 第 4 项的 WARN 断言转红；把 `runtime_window_mode` 对 X11 误翻 `true` ⇒ 判据 1 转红。

## 回写落点

- `codespec/specification/06-app-platform.md`：后端分工表按「`Surface` 槽 / 宿主方法」两列校正（Win32 行以 `Win32Surface` 覆写为准、D3D11 补转发）；「已知缺口」更新；§5 的 `PlatformCapabilities` 字段清单补两格并写置位矩阵与「公共入口是否全部接通」的口径。
- `codespec/specification/07-environment-modifier.md`：程序化窗口控制段补 Win32 / D3D11 四槽接线与全屏能力位。
- `codespec/SPECIFICATIONS.md`：§5 特性表 `SPEC.FEAT.APP.MULTI-WINDOW.001` 行补「Win32 / D3D11 窗控四槽已接、全屏档为可查询的不支持」。
- `codespec/manual-test/10-window.md`：新增 `TC-WINDOW-022`（取下一个可用序号，021 已占用、按 §1.4 不复用）——自动段跑新探针，人工目视段核对任务栏最小化/还原动画与 `Borderless` 自绘标题栏。
- 收尾：本提案状态按 `已提议 → 实施中 → 已归档` 流转；CHANGELOG.md 的 `[Unreleased]` 记一条 `Fixed`（该文件不属 codespec 树，按硬规则 5 不列为回写落点）。

## 不做清单

- **不补 Win32 全屏实现**：monitor 枚举、还原几何、`classify_size_mode` 定档与 CSD 揭示条共管属独立特性；本批只做能力位 + 一条 WARN。
- **不补 X11 / GLFW 的三枚切换腿**：两家 `set_window_mode` 已真实现、CSD 钮走内部目标态路径，公共切换原语另立缺口；本批让两格能力位如实为 `false`，应用先查询再决定画不画钮。
- **不补 `WgpuWin32Surface` / `WgpuX11Surface` 的几何原语转发**：独立后端族，既有缺口单列，不在本批半修。
- **不把 `Window::set_window_mode` 改成转发**：见变更内容第 5 条的回环理由。
- **不在应用侧代偿 Post `SC_*`**：第二份语义与「缺口先在框架侧补」相冲突。
- **不处理 Alt+F4 与拖拽移动**：前者属输入映射侧、后者走 OS 模态移动循环，两者均另立缺口。
