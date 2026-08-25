# ARCHITECTURE_PERF_LOG

> 本文件是 [`ARCHITECTURE_PERF.md`](./ARCHITECTURE_PERF.md) 的**实施记录 companion**：
> 收录性能优化的具体措施、实现要点、实测收益与调试能力的落地偏差。
> 架构层只保留约束与结论（见 `ARCHITECTURE_PERF.md` §10.6 / §10.7）；此处为可随实现演进的明细，便于回归参照与瓶颈定位。

---

## 性能优化实施记录

以下记录各优先级性能优化的具体措施与实现要点，供后续回归参照与瓶颈定位。

### P0 — 文本渲染热点消除

| 优化项 | 措施 | 效果 |
|--------|------|------|
| GlyphAtlas LRU 驱逐 | `std::list<key>` + `std::unordered_map<key, list::iterator>` 替代 O(n) `list::remove` | 查找/驱逐从 O(n) 降至 O(1) |
| make_key 紧凑化 | `uint64_t` 紧凑 key 替代 `std::string`，消除每字形堆分配 | 消除每字形 `malloc/free`，GC 压力显著降低 |
| resolve_faces 结果缓存 | `static unordered_map` 缓存字体解析结果 | 重复字形查询跳过 FreeType 字体解析 |

### P1 — 帧循环与布局开销削减

| 优化项 | 措施 | 效果 |
|--------|------|------|
| wire_dirty 条件化 | 仅在树结构变化时执行；`for_each_child` 直接遍历替代 `children()` 返回 vector 副本 | 每帧跳过无结构变化的 wire 遍历，消除堆分配 |
| Button 度量缓存 | `on_layout` 时缓存 `tw`/`th`，`on_paint` 直接使用 | 避免 paint 阶段重复调用 `measure_width` |
| resolved_text 缓存 | `on_layout` 时缓存 `LocalizedString` 解析结果 | 避免 paint 阶段重复解析本地化字符串 |
| tick_gestures 按需遍历 | 仅遍历注册了手势计时器的 widget（而非全树） | 无手势 widget 零开销 |
| FlexItem std::function 消除 | 函数指针 + `void*` 上下文替代 `std::function` | 消除 `std::function` 堆分配与间接调用开销 |

### P2 — 渲染管线快速路径

| 优化项 | 措施 | 效果 |
|--------|------|------|
| blend_pixel 矩形裁剪快速路径 | 纯矩形裁剪跳过 SDF coverage 遍历 | 直角矩形填充跳过逐像素 coverage 计算 |
| Modifier `dynamic_cast` → `kind()` switch | 用枚举 `kind()` 判别替代 RTTI `dynamic_cast` | 消除 RTTI 开销，分支预测友好 |
| fill_rect 圆角行级优化 | 按行计算圆角 x 范围，行内快速填充 | 圆角矩形逐行填充，行内连续内存写入 |

> **不变量约束**：所有快速路径必须与慢路径逐位一致（golden 零差异），修改后须跑 `test_offscreen` 全量回归。

### P3 — 光栅内核 SIMD 双实现（WS-4）

| 优化项 | 措施 | 效果 |
|--------|------|------|
| 渐变扫描线 SIMD 向量化 | 标量黄金 `gradient_*_scanline_scalar`（逐位一致基准）+ SSE2（4 像素/组）/ AVX2（8 像素/组）快路径；`gradient_linear_fill` / `gradient_radial_fill` 按 `g_simd_level` 运行时分发（AVX2→SSE2→标量尾补） | `linear_gradient_full` 20.60 → **3.01 ms**、`radial_gradient_full` 22.13 → **3.39 ms**（@1920×1080 scale 1.0，G-12 达标） |
| 不透明快路径接入 | `draw_linear_gradient` / `draw_radial_gradient` 仅对「双色标 + 两停靠点 + 两端 alpha==255」走 SIMD；逐行预计算 `py` 折叠 `dy` | 跳过 sRGB↔线性 LUT 往返（不透明像素数学等价于直写 sRGB，与旧实现位级一致） |

> **双实现确定性约束**：SIMD 路径必须与标量黄金路径**逐位一致**——`-ffp-contract=off` 禁 FMA、向量化沿用同一浮点运算序列、整型截断用 `cvtt`（`_mm_cvttps_epi32` / `_mm256_cvttps_epi32`）。CI 由 `test_simd_parity`（37,805 比对用例，随机化 + 固定种子，覆盖 5 色对 / 非对齐宽 / 多 stop0·range / 随机几何）逐位比对，G-13 一票否决；`test_offscreen` 同步零差异。
> 该优化为 `aurora::detail` 内部实现，不引入公共 API、不改变像素契约；`AURORA_ENABLE_SIMD` 默认 ON，OFF 时仅标量路径，详见 `BUILD_OPTIONS.md` §3.2。

---

## 落地风险与偏差记录

> 调试能力（`aurora::debug` 门面）收官时沉淀的偏差，供回归参考。
> API 契约与编译开关以 `SPECIFICATIONS.md` §H.10c 与 `BUILD_OPTIONS.md`（`AURORA_ENABLE_DEBUG`）为准。

- **`why_trace` 埋点**：原计划「复用现有埋点通道」实际不存在，落地为最小 DEBUG 热路径埋点——`mark_needs_*` 公开签名不变（委托私有 `*_impl(bool propagated)`），仅 DEBUG 下 `record_dirty` 写全局 ring buffer（cap 256，超界 `pop_front`）；Release 热路径零开销（`record_dirty` 全吞参）。传播在 relayout boundary 处截断（boundary 自身局部重排，不向上冒泡）。
- **真实窗口截图后端覆盖**：Win32（`PrintWindow`/`BitBlt` + BGRA→RGBA swizzle）、GLFW-on-Windows（复用 Win32 HWND 路径）、X11（`XGetImage` + Visual 掩码）已实现；Wayland / Headless 保持 `unsupported`（符合合成器隐私边界，非缺陷）。
- **`Surface` DEBUG 契约稳定性**：`save_snapshot` / `capture_window` 虚函数**始终声明**（vtable 槽稳定），宏只裁切后端专属覆写体、不门控默认实现，避免宏不一致引发 ODR。
