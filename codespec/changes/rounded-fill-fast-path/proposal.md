# CHG-070 圆角填充行级快路径的全覆写区求解修正

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-070 |
| 提出日期 | 2026-11-05 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `src/aurora/render/painter.cpp` 的 `rounded_full_x_range`（全覆写区求解的阈值、解根与整数取整边界）；`tests/unit/utest_painter.cpp` 新增一条「行级快路径 ↔ 逐像素 coverage 逐位一致」用例；`codespec/ARCHITECTURE.md` §8.1 与 §10 的性能口径回写；`CHANGELOG.md`。**不改 `Painter` 的任何公开入口签名与语义、不改混合公式、不改 `Scroll` 离屏缓冲与帧调度**；`aurora_api.json` 公共 API 结构无差异 |

## 动机

1. **现象**：`bench_scroll` 的 `lazy` / `grid` 两个隔离场景（`LazyList` / `GridView` 直接作为根、不经外层 `Scroll` 缓冲）在 1100×760、300 帧匀速滚动下 p99 34.0 / 35.7 ms、`full redraw` 300/300，G-1 至 G-4 四项时间门槛全部不满足；而走离屏缓冲的 `synthetic` / `google_play` 只要 1.4–3.0 ms。两者有效像素吞吐相差约 36 倍。

2. **归因**：`PaintTiming` 分项实测把 27 ms 全部收敛到一处——`lazy` 单帧 `paint` 21.1 ms 中 `fill` 占 **20.86 ms**，`text` 仅 0.25 ms、`glue` 0.03 ms；`grid` 为 `fill` **15.65 ms** / `text` 0.21 ms。也就是说开销不是文字整形（`shape` / `glyph` 缓存命中率本就 ~100%）、不是树遍历，而是 **19 / 38 个 `Chip` 胶囊背景的矩形填充本身**：836 000 像素花掉 20.9 ms，合 25 ns/px，比同一条 `blend_linear_region` 快路径（~1 ns/px）慢一个数量级。

3. **根因**：`Painter::fill_rect_slow_path` 的「行级全覆写快路径」依赖 `rounded_full_x_range` 求每行的 coverage==1 区间。该函数把 **AA 阈值 `t`（−0.5）当成了有效半径**：解根用 `disc = t² − py²`，提前退出用 `py > −t`，直边段直接用 `half_range = hw`。按 SDF `d = sqrt(max(qx,0)² + max(qy,0)²) − rad` 反解，coverage==1（`d ≤ t`）的精确条件应是 `rad + t` 这个**外扩后的有效半径**：`disc = (rad + t)² − py²`、退出条件 `py > rad + t`、直边段 `half_range = hw + t`。用 `t` 做半径时，`py` 只要超过 0.5 就被判成「本行没有全覆写区」。

4. **为什么恰好打在 `Chip` 上**：`Chip::paint_background` 的默认半径是 `height / 2`，即**胶囊形**。胶囊的 `half_height − rad == 0`，于是 `py = |y − cy|`——除中心一行外**每一行**都落进上面那个 `py > 0.5` 分支，整条胶囊退回逐像素 SDF（含 `sqrt`）。非胶囊的普通圆角矩形（`rad ≪ height/2`）只有两端各 `rad` 行受影响，所以这个缺陷长期藏在普通卡片里看不出来，一旦换成胶囊（Chip / 全圆角按钮 / 导航胶囊 / 开关）就整片命中。

5. **顺带暴露的一处既有缺陷**：直边段 `half_range = hw` 比精确边界 `hw + t` 宽半个像素。整数锚点时边界列的真实覆盖率恰为 0.5，却被当成全覆写写满，使**每个整数锚点的圆角矩形左侧硬化一像素**（非整数锚点因取整恰好跳开该列而未暴露）。此项与第 3 点同一函数、同一判据，一并修正。

6. **本提案的边界**：只改全覆写区的求解与取整。**不改** `apply_offset` 每滚动帧标布局脏的行为——那正是这些隔离场景 `full redraw` 恒为 300/300、`scroll_buffer_bytes` 恒为 0 的原因（根控件没有外层 `Scroll`，自驱动滚动走「重排 + 全量重绘」），但在填充成本降到 1.7 ms 后它已不是瓶颈，属另一条独立的帧调度优化线，不在本提案内。也**不改**行内快写与逐像素路径在半透明源色下的混色差异（前者是 sRGB 空间 lerp、后者是线性光伽马混合，既有且被现有用例固化），本提案的一致性判据只在两者必然相同的范围内成立（见验收判据 1）。

## 变更内容

1. `rounded_full_x_range` 改为按有效半径求解：新增 `rr = rad + t`，提前退出条件由 `py > -t` 改为 `py > rr`，圆弧段解根由 `t² − py²` 改为 `rr² − py²`，直边段 `half_range` 由 `hw` 改为 `hw + t`。四项改动同源，缺任一项仍会漏掉全覆写区或多算边界像素。
2. 右界取整改为 `floor(cx + half_range) + 1`：旧式 `ceil(cx + half_range) + 1` 在锚点非整数时多含一个采样点，该点覆盖率恒小于 1，被当作全覆写会把圆弧边缘硬化。左界仍取 `ceil(cx − half_range)`，与「采样点是整数像素坐标」的口径一致。
3. `tests/unit/utest_painter.cpp` 新增用例：以「`push_clip_rounded` + 逐像素 `blend_pixel`」作**独立基准**（走 `ClipRegion::coverage` 的 SDF 精确求解，与行级快路径正交），与 `fill_rounded_rect` 的输出逐像素比对；覆盖正胶囊、胶囊 + 非整数锚点、普通圆角、普通圆角 + 非整数锚点四种形态，源色取不透明、迭代域与 `fill_rect` 的 `[floor(左), ceil(右))` 对齐，使任何差异都只能来自「覆盖度不足 1 的像素被误判进全覆写区」。
4. 回写 `codespec/ARCHITECTURE.md` §8.1 的圆角抗锯齿条目（写明行级快路径的成立条件与精确性约束）与 §10 的性能基线口径；`CHANGELOG.md` 的 `### Fixed` 记一条。

## 验收判据

1. 新用例在 `fill_rounded_rect` 与逐像素基准之间 **0 像素差异**，四种形态全部覆盖；把 `half_range` 退回 `hw` 时该用例必须转红（实测 1 / 15 个像素差异），还原后全绿。
2. 性能判据（同机同构建、`PROFILING=ON`、1100×760、300 帧）：`lazy` 的 `fill` 由 20.86 ms 降至 1.69 ms、p99 由 34.0 降至 **2.47 ms**；`grid` 的 `fill` 由 15.65 ms 降至 1.93 ms、p99 由 35.7 降至 **2.95 ms**。G-1（p99 ≤ 16.67）、G-2（worst ≤ 33.3）、G-3（jitter ≤ 2.0）、G-4（长任务 ≤ 3）四项由**全不满足转为全部满足**（worst 2.71 / 3.02 ms、jitter 0.14 / 0.18 ms、长任务 0 / 0）。
3. 反向自证：把解根退回 `t² − py²` 时用例仍绿（旧式结果正确、只是慢），但 `lazy` 的 `fill` 回升到 27.2 ms、p99 回升到 33.8 ms——两条判据分别钉住「正确性」与「性能」，互不代偿。
4. `utest_painter` / `utest_painter_primitives` / `utest_offscreen` / `itest_dirty_clip_paint` / `itest_partial_repaint_bg` / `utest_scroll` / `itest_perf_display_list` / `utest_counters` 全绿；改动 TU 的增量 lint 与 format-check 零 finding；全量门禁不新增失败。
5. `aurora_api.json` 的公共 API 结构无差异（本提案只改一个私有静态成员的实现体与一条测试）。

## 回写落点

- `codespec/ARCHITECTURE.md`：§8.1「圆角抗锯齿裁剪」条目补写行级全覆写快路径的成立条件（有效半径 `rad + t`、直边段 `hw + t`、右界 `floor + 1`）与「快路径结果必须与逐像素 coverage 逐位一致」的约束；§10 性能检测体系补记本轮 `lazy` / `grid` 基线与其归因口径，说明隔离场景的 `full redraw` 属帧调度线而非光栅线。
- `codespec/BUILD_OPTIONS.md`：§3.3 架构级渲染 / 布局优化的清单补一条「圆角填充行级快路径」，与该节既有三项同列（恒开、无开关、无退化路径）。
