# CHG-001 Wayland CSD 阴影边距（单表面 margin 架构）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-001 |
| 提出日期 | 2026-10-09 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | Wayland 窗口后端（`WaylandSurface` 软件 wl_shm 路径、`WgpuWaylandSurface` GPU 装饰合成）、CSD 装饰绘制层（`csd::paint_title_bar` / `TitleBarPaintState`）、`Surface::content_inset()` 口径、wl_shm 像素格式（XRGB→ARGB 预乘）、xdg-shell window geometry / opaque region / tiled 状态解析；不改 X11/Win32、不改 `DecorationPolicy` 对外语义 |

## 动机

GNOME 不支持 `xdg-decoration`，Aurora 按 `DecorationPolicy::Auto` 回退 CSD。现状把 6px 缩放热区内嵌在内容 buffer（`Impl::border = 6`），只能在「热区吞内容事件」与「内缩画可见色带（过渡实现 35851da）」之间二选一，两者均非主流观感。业界（GTK4/libadwaita、libdecor、Electron）把热区与阴影像素放在视觉窗口之外，经 `xdg_surface.set_window_geometry` 声明可视边界。任务书《Wayland CSD「阴影边距」架构改造》已裁示方案 A（单表面阴影边距，SHADOW_MARGIN ≈ 10px）并经用户立项。

实现期核对代码后确认一处对任务书 §4.1 字面方案的必要偏离（如实记录，供评审追溯）：

- 任务书写「`WaylandSurface::size()` 对外保持内容尺寸，`content_inset()` 仅留标题栏 top」。但框架事实是：`Painter` 无 translate/origin 变换，`DisplayList::replay` 无 offset 入参，`Window::run_layout` 根约束直接取 `size()`，GPU 路径 `present_gpu_frame` 以 `size()` 调 `sink.begin_frame` 且帧 DL 无偏移入口——应用内容无法在不改上述四处公共机制的前提下落到 buffer 的 (margin, margin)。
- 故采用 **Route B（借道既有 `ContentInsetRoot` 壳承载偏移）**：`size()` 返回整幅表面逻辑尺寸（内容 + 2×margin），`content_inset()` 语义为「表面原点 → 应用内容原点」，CSD Normal 态 = `{margin, margin+标题栏, margin, margin}`。经 `Window::prepare_context` 既有的 `mq.size = size() − inset` 折算后，MediaQuery 尺寸与改造前**逐值相同**（宽 = 内容宽，高 = 内容高 − 标题栏），应用/布局/事件坐标零感知；GPU swapchain 随 `begin_frame(size())` 自动整幅分配、帧 DL 坐标经同一壳平移，**GPU 路径的帧调度零改动**。任务书「下游零感知、内容满幅」的意图与 DoD 完全保留，仅承载机制从「size 保持内容口径」改为「inset 承载 margin 分量」。

## 变更内容

1. 尺寸模型：wl_surface buffer 扩为「内容 + 四周 margin」（`AURORA_SHADOW_MARGIN_DP = 10` 逻辑 px，纯逻辑常量收敛在 `csd_geometry.h` 供单测）；每帧 commit 前发 `xdg_surface_set_window_geometry(margin, margin, 内容宽, 内容高)`（表面坐标，双缓冲随 commit 生效）；`xdg_toplevel.configure` 回报尺寸语义 = geometry（内容）尺寸，维持 `Impl::size` 为内容尺寸，`WaylandSurface::size()` 改为整幅口径。
2. `content_inset()` 删除旧 border=6 内缩分量，改为 margin 模型：CSD Normal `{m, m+tb, m, m}`、Borderless `{m,m,m,m}`、Maximized/tiled `{0,tb,0,0}`、FullScreen/SSD/Frameless 全零。
3. 指针坐标统一在 ptr_enter/ptr_motion/ptr_button 三入口做「表面坐标 → 内容坐标（−margin）」映射；缩放热区迁到 margin 带（内容坐标域判定式形状不变、坐标域外移，厚度 10 ≥ 8），顶边带在标题栏之外、有标题栏时同样可缩放；`xdg_toplevel_show_window_menu` 回传表面坐标（映射前值）；标题栏按钮/拖拽命中仍在内容坐标域。热区判定抽纯函数进 `csd_geometry.h`（表驱动可单测），`!resizable` 时不发 resize。
4. 阴影绘制：`TitleBarPaintState.border` 字段转型为 `shadow_margin`，删除 35851da 的四条底色带绘制；新增 `csd::paint_window_shadow`（`Painter::draw_shadow` 内容矩形外柔和衰减，无可见边框带）。装饰分两层：阴影 underlay（**GPU/DL 路径**新增 `record_client_decoration_underlay` 由 Sink 在 `rhi.begin_frame` 后、app 帧回放前录制回放，Shadow 管线写真 alpha）与标题栏 overlay（既有 `record_client_decoration`，坐标加 margin 原点偏移）。**软件 wl_shm 路径实施期改道**（实测发现 Painter 的「目标画布恒不透明」不变量使 `set_pixel`/混合恒写 alpha=255，零基底上画不出真 alpha）：阴影不经 Painter，改为在 `present()` 的 swizzle 之后由 `csd::compose_shadow_margins_bgra` 直接向映射缓冲的 margin 环写 BGRA 预乘字（内容矩形跳过、角部欧氏距离、幂等可重复合成）；两条上屏路径的基色/模糊内缩/衰减公式同源 `csd_geometry.h`（`AURORA_SHADOW_BASE_COLOR`/`AURORA_SHADOW_BLUR_INSET_PX`/`shadow_attenuation`）。
5. 像素格式：wl_shm buffer 由 `XRGB8888` 改 `ARGB8888`，swizzle 增加 RGBA 直色 → BGRA 预乘变体（Wayland 按预乘 alpha 解读；内容区 alpha=255 预乘无损）；margin 环在 swizzle 后由合成器覆写（外缘衰减为 0 写透明字 0，兼清槽内残留）；新增 `wl_surface_set_opaque_region` 仅报内容矩形（几何变化时重建）。
6. 状态塌缩：解析 xdg-shell v2 的 `TILED_LEFT/RIGHT/TOP/BOTTOM`（wm_base 绑定已封顶 v2，可收）；Normal → Maximized/FullScreen/平铺时 margin 塌缩为 0 并触发同步重渲染（buffer 重分配走既有 resize 全量重绘通道），回 Normal 恢复。
7. 关联子系统核对结论：IME 光标矩形无需改（caret provider 返回的 `focus_bounds_` 是绘制期写入的窗口绝对坐标，已含 `ContentInsetRoot` 平移）；脏矩形已是 buffer 设备坐标；`present_stale` 比对改整幅尺寸；Headless/其他后端零影响（margin 恒 0）。
8. 已知边界（真机走查项，不阻塞本仓 DoD）：wgpu swapchain `alphaMode=Auto` 下阴影半透环的预乘色彩以 GNOME mutter 实测为准微调；Borealis 验证（任务书 T6）在另一仓。

## 验收判据

1. CSD Normal 态（Auto 无 SSD / ClientSide / Borderless）`content_inset()` 与 margin 塌缩矩阵（Normal/Maximized/FullScreen/tiled × 标题栏有无）有纯函数单测覆盖，逐值断言。
2. 指针热区表驱动单测：margin 带内各边/角八点判定为对应 resize edge，带外内容区不误判，`!resizable` 不命中 resize；坐标映射（表面→内容）逐向断言。
3. 阴影像素单测：软件路径对 `compose_shadow_margins_bgra` 的 BGRA 字逐值断言（四向外缘字=0、向内 alpha 单调递增、角部两向组合、内容矩形字不触、与 swizzle 预乘算术一致、幂等、margin=0 无操作、scale=2 物理几何）；GPU/DL 源 `paint_window_shadow` 验证直绘与 DisplayList 录制回放两路输出逐字节一致（沿用既有 bit-identical 比对模式）且内容区被不透明底色恢复。
4. 预乘 swizzle 单测：alpha=0 / 128 / 255 三档的 BGRA 输出字节正确，内容区（alpha=255）RGB 无损。
5. 全量 `ctest --preset ninja-test` 绿（含全部 `check_*` 门禁），`ninja` 预设构建无警告。
6. 规格与 CHANGELOG 回写完成，CHANGELOG Migration 注明 `content_inset()` 口径变化（含 margin 分量、旧 border 分量删除）与 `WaylandSurface::size()` 改为整幅表面尺寸。
7. GNOME 真机走查由人工完成：内容满幅、贴边可缩放、无可见框带、最大化/全屏/平铺无阴影边距（DoD 第 7 条，不在仓内自动化范围）。

## 回写落点

- `codespec/specification/07-environment-modifier.md`（§4.1 DecorationPolicy 安全区口径：content_inset 语义、margin 模型、塌缩矩阵）
- `codespec/specification/06-app-platform.md`（§8 Wayland 后端：window geometry、阴影边距、tiled、IME/opaque region 核对结论）
- `codespec/specification/03-layout-render.md`（§8 WgpuWaylandSurface：阴影 underlay/标题 overlay 两层装饰合成）
