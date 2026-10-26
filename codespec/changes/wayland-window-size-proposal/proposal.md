# CHG-040 Wayland 窗口尺寸提议与位置不可设申报

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-040 |
| 提出日期 | 2026-10-26 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `include/aurora/window/wayland_surface.h`（新增既有 `Surface` 虚接口 `set_size` / `set_position` 的覆写声明与文档）、`src/aurora/window/wayland_surface.cpp`（`set_size` 经 xdg_toplevel min/max 提议 + configure 后还原样式约束、`set_position` 显式 no-op、构造期约束值收敛到单一记录点）、`tools/verify/wayland_geometry_live_probe.cpp`（新增真机探针，按 `EXCLUDE_FROM_ALL` 口径不进 CTest）、`cmake/AuroraVerify.cmake`（注册探针目标）、`codespec/specification/06-app-platform.md` 与 `codespec/manual-test/10-window.md`（几何 setter 现状按代码同步）。`aurora_api.json` 预期零差异（覆写的均是既有 `Surface` 虚函数，不入册） |

## 动机

1. **Wayland 的 `Normal` 档尺寸恢复此前是静默 no-op**：`apply_host_geometry` 对 `Normal` 下发 `set_size(g.size / scale)`，而 `WaylandSurface` 未覆写该方法，落到基类空实现。用户的「记住窗口大小」在 Wayland 会话里并不存在，且没有任何错误信号。
2. **xdg-shell 没有「程序化设置尺寸」请求**：client 只能给 `xdg_toplevel` 的 min/max 约束，最终尺寸由合成器经 `xdg_toplevel.configure` 决定。把 min 与 max 同时收窄到目标值是表达「提议此尺寸」的标准做法，但它会临时把窗口锁成固定尺寸，必须在 configure 到达后还原——否则用户再也拖不动边框。
3. **整幅表面与 window geometry 是两套口径**：本后端的 `size()` 返回整幅表面（内容 + 2×CSD 阴影边距），而下发给 xdg_toplevel 的 min/max 作用在 window geometry（内容）上。若 `set_size` 把入参原样当内容尺寸，几何持久化每次重启都会多长出 2×边距；必须在唯一的换算点减去边距，才能与 `capture_host_geometry` 的保存腿对称。
4. **位置在协议层不可设，必须显式申报而不是保持沉默**：xdg-shell 不提供客户端可用的定位请求，窗口位置由合成器（含平铺/吸附）决定。显式 no-op 覆写加注释，使「本后端位置恢复如实降级」在代码与文档里都可见，避免后续读者误以为是漏实现。
5. **规格与手工用例仍把 Wayland 尺寸写成未实现**：上一批只把 X11 的缺口补上，Wayland 条目仍停在「待补」。本次落码后须同批改正，避免再次出现「规格滞后于代码」。

## 变更内容

1. **`set_size(Size)` 真实提议**：入参是整窗外框逻辑尺寸（与 `size()` / 几何快照同口径）。实现先减去当前态 CSD 阴影边距得到内容尺寸，再夹回 `WindowStyleOptions` 声明的 min/max（内容口径），随后把 xdg_toplevel 的 min/max 同时收窄到目标值并 commit。已是目标尺寸时直接返回，避免多余 configure 往返。
2. **configure 落地含 0×0 分支**：`on_xdg_surface_configure` 的尺寸来源取「合成器给出的正尺寸优先，否则回落在途提议」——xdg-shell 允许合成器回 `configure(0×0)` 把尺寸决定权交回 client，WSLg/Weston 对 min/max 约束变化的实测回答正是 0×0。此时按提议的内容尺寸更新本地值，下一次 `present()` 以该尺寸 attach 新 buffer 并 `set_window_geometry`，合成器据此把窗口设为该尺寸；若合成器回了正尺寸则以它为准。两条路径都走同一处更新，不另开旁路。
3. **configure 后还原样式约束**：在应用完尺寸与几何态后，若存在在途尺寸提议，立即按构造期记录的样式 min/max 重新下发并 commit，同时清空提议。`xdg_toplevel.configure` 就是合成器对提议的回答（接受或拒绝）；无论结果如何都还原，避免固定尺寸锁死。
4. **构造期约束值收敛为单一记录点**：新增 `style_min_w/h`、`style_max_w/h` 字段，构造时把 `WindowStyleOptions`（含 `resizable == false` 的固定尺寸档）解析结果写入，并统一经 `apply_style_size_limits()` 下发；还原路径复用同一函数，不出现第二套口径。
5. **`set_position(Point)` 显式 no-op**：覆写基类并写明协议层原因（合成器决定位置、client 不可查询/设置，`position()` 保持基类 `(0,0)`）。几何持久化的位置腿在本后端如实降级，尺寸腿照常工作。
6. **真机探针**：新增 `tools/verify/wayland_geometry_live_probe.cpp`，覆盖基线尺寸、`set_size` 经 configure 落地（含整窗/内容边距换算的正确性）、同值提议幂等、`set_position` no-op 且不扰动尺寸。判据以「`size()` 只在 configure 中更新」为端到端证据，不依赖合成器私有工具。
7. **规格与手工用例同步**：`06-app-platform.md` §261 的 `Normal` 档、§290 的 Linux 几何 setter 现状、§292 的已知缺口改为「Wayland 尺寸经约束提议、最终由合成器决定；位置协议层不可设」；`manual-test/10-window.md` 的 X11 / Wayland 腿预期结果与两条偏差说明按当前实现改写。
8. **不做**：不给 Wayland 增加任何「定位」伪造通道（如要求合成器私有协议）；不改 GPU 宿主对几何原语的转发口径；不新增单测文件（行为只能在真实合成器上证明，由探针覆盖）。

## 验收判据

1. `cmake --build build-wayland --target aurora_verify_wayland_geometry` 通过；`AURORA_BUILD_VERIFY_TOOLS=ON` 下目标可见且 `EXCLUDE_FROM_ALL`（不进 CTest）。
2. 真机（Wayland 会话，WSLg / Weston 或桌面合成器）运行探针退出码 0，且 (a) 基线、(b) 尺寸提议经 configure 落地、(c) 同值幂等、(d) 位置 no-op 逐条 PASS。
3. 增量 `clang-tidy`（`src/aurora/window/wayland_surface.cpp`）0 findings；`clang-format` 对改动文件与探针通过；`check_doc_comments`（`wayland_surface.h` 新声明）通过。
4. `check_change_proposals`、`check_codespec_xref`、`check_code_doc_sync`、`check_api_schema_sync`、`check_api_budget`、`check_dpi_single_source` 全绿。
5. 两条偏差分开记：尺寸提议由 (b) 证明；位置不可设由 (d) 证明。不得用尺寸的绿替代位置腿的如实降级申报。

## 回写落点

- `codespec/specification/06-app-platform.md`（§261 `Normal` 档的 Linux 覆盖、§290 Linux 几何 setter 现状、§292 已知缺口）
- `codespec/manual-test/10-window.md`（TC-WINDOW-020 的 X11 / Wayland 腿预期结果与两条既有偏差的分工说明）
- 版本与发布记录：条目类型 `Fixed`，落 CHANGELOG 的 `[Unreleased]` 节；该文档不属 codespec 树，按 AGENTS.md 硬规则 5 不列为回写落点，本批不单独升 alpha 号。
