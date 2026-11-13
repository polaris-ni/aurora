# CHG-039 X11 缩放上报与窗口几何下发补齐

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-039 |
| 提出日期 | 2026-10-26 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `include/aurora/window/x11_surface.h`（新增 `position()` / `set_position()` / `set_size()` 三个既有 `Surface` 虚接口的覆写声明，`scale_factor()` 文档补运行期上报语义）、`src/aurora/window/x11_surface.cpp`（`RESOURCE_MANAGER` 的 `PropertyNotify` 处理、`XGetWindowProperty` 读资源串、`XResizeWindow` / `XMoveWindow` 下发、`XTranslateCoordinates` 读回、缩放变化时逻辑尺寸重算）、`tools/verify/x11_geometry_live_probe.cpp`（新增真机探针，按 `EXCLUDE_FROM_ALL` 口径不进 CTest）、`cmake/AuroraVerify.cmake`（注册探针目标）、`codespec/specification/06-app-platform.md`（§233–292 的滞后描述按代码改正）。X11 公共头多出三枚覆写 ⇒ 该头被多 TU include，需全量重编；`aurora_api.json` 预期零差异（`Surface` 虚函数不入册） |

## 动机

1. **规格状态滞后于代码，且滞后的方向会误导后续读者**：`06-app-platform.md` 曾把「Linux 三后端（X11 / Wayland / GLFW）缩放变化上报未实现」与「X11 与 Wayland 的 `set_size` / `set_position` 未实现」并列。实测 GLFW 的 `set_size` / `set_position` / 内容缩放回调、Wayland 的 `entered` outputs 缩放上报都早已落地；真实缺口只在 X11——`scale_factor()` 仅在构造期读一次 `Xft.dpi`，`set_size` / `set_position` 仍是基类空实现。若不按代码改正，后续排期会把已实现的能力重复认领，并把 X11 独有的缺口摊到三后端上。
2. **几何持久化恢复的 `Normal` 档在 X11 上是静默 no-op**：`apply_host_geometry` 对 `Normal` 下发 `set_size(g.size / scale)` 与 `set_position(g.origin)`，X11 侧两者都落基类空实现，用户看到的「记住窗口位置与大小」在 X11 会话里并不存在，且没有任何错误信号。
3. **只补 `set_position` 而不同批补 `position()` 会让保存腿写坏数据**：`capture_host_geometry` 用 `surface.position()` 取快照。若读回仍走基类 `(0,0)`，恢复腿就会把窗口搬到屏幕左上角——位置下发「看起来生效」，实际每次重启都把用户窗口挪走。故读回与下发必须同批对称。
4. **X11 没有标准「DPI 变化」事件**：唯一可靠信号是根窗口 `RESOURCE_MANAGER`（`Xft.dpi` 所在）的 `PropertyNotify`。该属性变化时重读一次即可，不能每帧解析资源串。
5. **`XResourceManagerString` 不能在运行期重读**：实测（libX11 1.8）该函数把首次读到的值缓存在 `Display` 上，即使已处理 `PropertyNotify`、属性已被改写，仍返回旧串。运行期上报必须用每次直达服务器的 `XGetWindowProperty`，否则「改 dpi → 上报」在真机上永远读旧值。

## 变更内容

1. **缩放上报接线**：构造期对根窗口追加 `PropertyChangeMask`；事件循环的 `PropertyNotify` 分支识别 `XA_RESOURCE_MANAGER`，用 `XGetWindowProperty` 重读资源串并解析 `Xft.dpi`（`scale = dpi / 96`，钳制 `[0.5, 4.0]`）。仅在值变化时：先更新本后端 `scale`，按 `XGetGeometry` 的物理尺寸重算逻辑尺寸，再调用基类 protected 的 `notify_scale_change` 与 `present_request_`。不覆写 `set_scale_change_handler`——基类已保存 handler，覆写只会引入第二份状态。
2. **`set_size(Size)` 真实下发**：入参是逻辑 dp（与 GLFW 同口径），经本后端唯一 `scale` 真值点换算为物理像素后 `XResizeWindow`，并同步逻辑尺寸缓存（几何恢复在首帧之前下发，等 `ConfigureNotify` 才更新会差一帧错位）。
3. **`set_position(Point)` 真实下发与 `position()` 读回**：位置是屏幕物理像素（与 GLFW / Win32 同口径，快照直接下发、不做 scale 换算），下发用 `XMoveWindow`；读回用 `XTranslateCoordinates` 折算到根窗口，消解重父化 WM 下 `x/y` 相对父窗口的歧义。不实现 `set_position` 的读回对称会使 `capture_host_geometry` 恒存 `(0,0)`（见动机第 3 条）。
4. **真机探针**：新增 `tools/verify/x11_geometry_live_probe.cpp`，覆盖四条：运行期改 `Xft.dpi` 后 `scale_factor()` 跟随且 `force_full_redraw` 真的出帧、逻辑尺寸按新 scale 重算；`set_size(dp)` 落到窗口物理尺寸；`set_position` 使窗口位移落在请求量附近；自注册 handler 收到新值且同值通知被去重。观测用独立 Xlib 调用（`XGetGeometry` / `XTranslateCoordinates`），不依赖 `xdotool`；探针对会话级 `RESOURCE_MANAGER` 用 RAII 还原。
5. **探针必须按真实时间等 WM 落地**：`XResizeWindow` / `XMoveWindow` 只是发给 WM 的请求，由 WM / 合成器决定何时生效（WSLg 的 Weston WM 实测 0.1–1 s）。探针改为轮询落地判据；位置判据给 48 px 容差以吸收重父化 WM 的装饰/重力常量偏移，位移≈0 仍必然判红。
6. **规格据实改正**：`06-app-platform.md` 的 §233 表格、§261 的 `Normal` 档、§288 的「两个独立偏差」与 §292 的已知缺口，均改为「三后端缩放上报已全部接线」「X11 几何 setter 已实现（含 `position()` 读回）」「Wayland 仍缺 `set_size`、`set_position` 协议层由 compositor 决定」。
7. **Wayland 的两项评估结论（本批不做）**：`set_size` 的真实现要按 xdg-shell 的「client 提议、compositor 决定」语义处理（`set_min_size` / `set_max_size` 只是约束，最终尺寸仍由 configure 决定），且需评估协议版本与副作用；`set_position` 在 Wayland 协议层没有客户端可用的定位请求，显式 no-op 并注释是唯一诚实形态。两项均不阻塞本批，继续保持规格中的「待补 / 不可设」申报。
8. **不做**：不改 `GlfwSurface`（其实现已正确）；不为 `WgpuX11Surface` 补几何转发（既有统一缺口，见「不做清单」）；不新增单测文件（行为只能在真实 X server + WM 上证明，由探针覆盖；`AURORA_LIVE_X11=1` 的单测面不引入依赖 WM 时延的断言）。

## 验收判据

1. `cmake --build build-x11 --target aurora_verify_x11_geometry` 通过；`AURORA_BUILD_VERIFY_TOOLS=ON` 下目标可见且 `EXCLUDE_FROM_ALL`（不进 CTest）。
2. 真机（WSLg / Linux X11 会话）运行探针退出码 0，且 (a) 基线、(b) 缩放上报、(c) 几何 setter、(d) 回调通道与去重逐条 PASS；根窗口 `RESOURCE_MANAGER` 在探针退出后与运行前逐字一致。
3. 增量 `clang-tidy`（`src/aurora/window/x11_surface.cpp`）0 findings；`clang-format` 对改动文件与探针通过。
4. `check_doc_comments`（`x11_surface.h` 新声明）、`check_codespec_xref`（`06-app-platform.md` 与本文）、`check_nolint_layout`、`check_dpi_single_source`、`check_api_budget`、`check_change_proposals` 全绿。
5. 两条偏差分开记：缩放上报由 (b)(d) 证明；几何恢复下发由 (c) 证明。不得用 (b) 的绿替代 (c) 的读数。

## 回写落点

- `codespec/specification/06-app-platform.md`（§233 窗口几何持久化表格的缩放上报现状、§261 `Normal` 档的 Linux 覆盖、§288 恢复时刻可信性与两条偏差、§290–292 的已知缺口与 Wayland 待补项）
- `codespec/SPECIFICATIONS.md`（§5 特性表 `SPEC.FEAT.APP.MULTI-WINDOW.001` 行：逐窗几何记忆的现状复核；该行由本批与 `CHG-033` 共享，只改现状描述、不新增需求）
- 版本与发布记录：条目类型 `Fixed`，落 CHANGELOG 的 `[Unreleased]` 节；该文档不属 codespec 树，按 AGENTS.md 硬规则 5 不列为回写落点，本批不单独升 alpha 号。
