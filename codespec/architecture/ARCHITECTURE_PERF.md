# ARCHITECTURE_PERF

> 本文件由 [`ARCHITECTURE.md`](../ARCHITECTURE.md) 划分而出（性能检测体系 / 设计不变量）。章节编号保持原样。
> 返回主线见 [`ARCHITECTURE.md`](../ARCHITECTURE.md)。

**本文包含章节：**

- [10. 性能检测体系（Performance Profiling）](#10-性能检测体系performance-profiling)
- [11. 设计不变量（Invariants）](#11-设计不变量invariants)

## 10. 性能检测体系（Performance Profiling）

Aurora 内置轻量级运行时性能检测体系，为开发者提供帧级指标采集、分阶段计时、可视化叠加与日志导出能力，帮助定位渲染瓶颈与帧率异常。整个体系零外部依赖，所有组件默认关闭、按需启用。

### 10.1 FrameStats 单例

- **头文件**：`include/aurora/app/perf_overlay.h`
- **进程级单例**，128 帧环形缓冲区 O(1) 采集，提供以下指标：
  - FPS（滑动窗口平均帧率）
  - 平均帧时间（`avg_frame_ms`）
  - P50/P95/P99 百分位帧时间（`percentile_ms`）
  - 帧时间标准差 / 抖动（`jitter_ms`）
  - 掉帧计数与掉帧率（`dropped_frame_count` / `dropped_frame_ratio`，超过帧预算即计为掉帧）
  - Hitch 计数（`hitch_count`，帧耗时 > 100ms 视为 hitch）
  - Idle 帧计数（`idle_frame_count`，脏区跳帧不污染渲染统计）
- **分阶段计时**：`record_phases(layout_ms, paint_ms, present_ms)` 独立记录三阶段耗时，64 帧环形缓冲，提供 `avg_layout_ms` / `avg_paint_ms` / `avg_present_ms` 查询。
- **帧预算**：`set_frame_budget_ms(ms)` 设置帧预算（默认 16.67ms ≈ 60 FPS），超出即计为掉帧。
- **`reset()`**：清空所有状态，用于基准测试隔离。

### 10.2 PerfOverlay 控件

- **头文件**：`include/aurora/app/perf_overlay.h`
- 右上角叠加面板，实时显示：
  - 多行统计文本（FPS / avg / P99 / jitter / 掉帧数 / hitch 数 / idle 帧数）
  - FPS 颜色告警（绿 ≥ 55、黄 ≥ 30、红 < 30）
  - 帧时间条形图（最近 128 帧，超预算帧标红）
- 通过 `PerfOverlay::enable()` / `PerfOverlay::disable()` 按需开关，不启用时零开销。

#### 10.2.1 分层 HUD 叠加层（CPU 性能专项）

`PerfOverlay` 既可作普通 `SingleChild` 包裹内容，也推荐作为**独立 HUD 叠加层**使用：经
`Application::set_overlay(...)` / `Window::set_overlay(...)` / `App::overlay(...)` 注入后，它**脱离
widget 树**，由 `Window::present_root` 在 tree paint 之后、present 之前合成到主缓冲：

- 叠加层渲染到独立离屏 `Painter` 缓冲（逻辑窗口尺寸 × 窗口 scale），仅以 **~2Hz** 重绘自身
  （面板背景为不透明，确保叠在保留自上一帧的主缓冲之上不产生重影）；
- 每帧把缓存的 HUD 缓冲 `composite`（alpha 混合）到主缓冲；app 树仅在**其自身脏**时重绘，
  叠加层刷新开销（~1–2ms）被隔离在离屏缓冲内，**不再触发整树重绘**；
- 叠加层内容发生 2Hz 重绘的帧强制全量上屏（HUD 像素可能落在 app 脏区之外，避免滞后 1 帧）；
- 与「把 `PerfOverlay` 作为根控件包裹内容」的旧用法**互斥**：启用叠加层后，`Scene` 根即为真实
  内容树，`PerfOverlay` 不应再出现在树内。该分层是消除 2Hz 自标脏引发 P99 尖峰（旧做法每 500ms
  整树重绘 ~24ms）的正解。

### 10.3 PerfLog 日志导出

- **头文件**：`include/aurora/perf/perf_log.h`
- 定期日志输出 + 快照导出：
  - `enable(interval_frames)` 启用定期日志（默认每 300 帧输出一次）
  - `on_frame_end()` 帧结束调用，内部按间隔触发日志
  - `snapshot_json()` / `snapshot_csv()` 返回当前指标的 JSON / CSV 快照字符串
- 日志经 `Log` 子系统输出，可重定向到文件或控制台。

### 10.4 Idle 帧区分

- **`Window::is_idle_frame()`**：当前帧是否为脏区跳帧（idle 跳过帧）。
- **`FrameStats::record_idle()`**：记录 idle 跳帧计数，与渲染帧分开统计。
- 脏区渲染体系（§5）中，无脏且尺寸未变时整帧跳过——这类 idle 帧不应污染 FPS / 帧时间 / 掉帧等渲染指标。`FrameStats` 将 idle 帧与渲染帧分开计数，`fps()` / `avg_frame_ms()` 等指标仅基于渲染帧计算。

### 10.5 自动化测试

| 测试文件 | 类型 | 覆盖范围 |
|----------|------|----------|
| `test_perf_frame_stats.cpp` | 单元测试 | FrameStats API 正确性（环形缓冲、百分位、掉帧/hitch/idle 计数、分阶段计时、reset） |
| `test_perf_frame_loop.cpp` | 端到端基准 | 帧循环集成（present_root 分阶段计时、idle 帧隔离、PerfOverlay 叠加渲染） |

### 10.6 性能优化实施记录

> 各优先级性能优化的**具体措施、实现要点与实测收益**（P0 文本渲染热点 / P1 帧循环与布局 / P2 渲染快速路径 / P3 SIMD 双实现）已迁出至 [`ARCHITECTURE_PERF_LOG.md`](./ARCHITECTURE_PERF_LOG.md)，供回归参照与瓶颈定位。

架构层须始终遵守的两条硬约束（明细见日志）：

- **快速路径逐位一致**：所有快速路径必须与慢路径 golden 零差异，修改后须跑 `test_offscreen` 全量回归。
- **SIMD 双实现确定性**：SIMD 路径必须与标量黄金路径逐位一致（`-ffp-contract=off`、同浮点运算序列、整型 `cvtt` 截断）；CI 由 `test_simd_parity`（37,805 比对用例）逐位比对，G-13 一票否决；`AURORA_ENABLE_SIMD` 默认 ON（详见 `BUILD_OPTIONS.md` §3.2）。

### 10.7 调试能力设计依据（真实后端 DEBUG）

> `aurora::debug` 门面（帧缓冲/真实窗口截图、Widget 树、性能快照、可视化调试叠层、控件拾取）的 API 契约与编译开关以 `SPECIFICATIONS.md` §H.10c 与 `BUILD_OPTIONS.md`（`AURORA_ENABLE_DEBUG`）为准；本节收纳其**设计取舍依据**（业界对标与落地风险），供后续演进参考。

**业界对标矩阵**（设计期取舍参考）：

| 维度 | Flutter DevTools | React DevTools | Qt Creator / GammaRay | Aurora `aurora::debug` |
|------|------------------|----------------|------------------------|------------------------|
| 布局可视化 | Layout Explorer（选中控件盒模型 + 约束） | 组件树高亮（无盒模型） | 部件树 + 几何检查 | `DebugPaintFlags`（layout_guides / relayout_boundaries / layer_borders / repaint_highlight / overdraw） |
| 性能 | Performance / Timeline（帧耗时百分位） | Profiler（commit 火焰图） | 信号槽探查器 | `frame_phase_timeline`（L/P/R 三相位 + ASCII flamegraph）+ `PerfLog` |
| 因果链 | 无（靠经验） | 无 | 无 | `why_trace`（mark-needs-layout/paint 触发因果链，含 `propagated` 根因/传播区分） |
| 远程访问 | DevTools Server（WebSocket） | React DevTools 独立进程 | GammaRay 进程注入 | `InspectorServer` localhost REST（`/api/debug/*`） |
| 平台限制 | Skia/Impeller 后端 | DOM/React  reconciler | 原生 Qt | 真实窗口截图 Wayland/Headless 不支持（设计取舍：不破解合成器隐私边界） |

**落地风险与偏差记录**：已迁出至 [`ARCHITECTURE_PERF_LOG.md`](./ARCHITECTURE_PERF_LOG.md#落地风险与偏差记录)（含 `why_trace` 埋点、`Surface` DEBUG 契约稳定性、真实窗口截图后端覆盖），供回归参考。

---

## 11. 设计不变量（Invariants）

> 任何改动都不得破坏以下不变量（违反会导致挂起、闪烁或行为不确定）。

1. **细粒度订阅去重**：`State::subscribe` 必须对同一 `Effect` 去重，否则约 25 帧后挂死。
2. **根挂载唯一**：`Window::presentRoot` 对同一根只 mount 一次。
3. **命中测试用局部坐标**：`Rect{Point{0,0}, bounds.size}.contains(local)`（非 `bounds.contains`）。
4. **事件冒泡协议**：`Widget::on_pointer_event` 写 `e.handled = true` 即停止冒泡；
   纯展示控件不拦截以放行父级点击。
5. **确定性渲染**：相同 widget 树 + 尺寸 → 相同像素输出（`HeadlessSurface` 快照可比对）。
6. **降级而非中止**：非法输入产出 `Diagnostics` 并降级到安全默认，不抛异常。
7. **单线程 UI**：widget 树 / 状态订阅 / 重绘调度只在主线程；跨线程写入经 `State::set` 串行化。
8. **头文件尾置返回类型**：所有函数声明/定义用 `auto f(...) -> Ret`。
9. **强类型几何**：尺寸/颜色/长度使用 `Length`/`Color`/`px()` 等强类型，禁止裸整数隐式转换。
10. **`RelayoutBoundary` 不变量**：`set_relayout_boundary(true)` 的控件成为显式重排边界——布局脏标记冒泡在边界处截断（`widget.h:mark_needs_layout` 的 `if (m_layout_parent && !is_relayout_boundary())`），且其 `on_dirty(true)` 走 `register_dirty_boundary`（局部重排）而非整树重排。**`Scroll` 不得无条件置此标志**：骨架→真实内容的切换逻辑位于 Scroll 之上的祖先（由下方 `mark_needs_layout` 驱动），若 Scroll 成为边界会截断该脏冒泡，使内容切换永不触发（离屏缓冲恒为骨架）。该误用在 WS-2 中曾导致 `test_navigator_layout_cache` 回归（离屏缓冲恒灰），移除标志后逐像素恢复 HEAD 行为。仅在确有「子树自包含、且切换由本控件自身驱动」的语义时才置位。

---

