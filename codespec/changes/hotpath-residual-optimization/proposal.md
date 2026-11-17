# CHG-079 主帧热路径固定分配与逐像素回退的收敛

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-079 |
| 提出日期 | 2026-11-16 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `src/aurora/render/painter.cpp`（`draw_image` 缩放路径、`fill_sector` 羽化带、圆角过渡列、`draw_shadow` 边缘环、`set_pixel` 裁剪判界、渐变停止点规范化）与 `include/aurora/render/painter.h`（裁剪交集缓存成员）；`src/aurora/render/glyph_atlas.cpp` / `include/aurora/render/glyph_atlas.h`（淘汰结构与指针有效期）；`src/aurora/event/dispatcher.cpp`（悬停与触控链）；`src/aurora/widget/widget.cpp`（修饰节点回调、兄弟查找）、`src/aurora/widget/serialization.cpp`（差分路径串）、`include/aurora/widget/widget.h`（尺寸意图上溯合并）；`src/aurora/state/reactor.cpp`（纪元戳数组、Job 链）；`src/aurora/layout/flex_layouter.cpp`；`src/aurora/render/font_engine.cpp`（ShapeCache 键）；`src/aurora/core/json/value.cpp`（对象键索引）；`include/aurora/animation/animator.h` / `include/aurora/animation/spring.h` / `include/aurora/core/main_queue.h` 与 `include/aurora/state/async.h`；`tests/unit` 下的对应用例。**公共 API 结构与 `aurora_api.json` 无差异**；`Painter` 仅增私有成员 |

## 动机

1. **每次调用的固定分配仍在主帧路径上。** `Painter::draw_image` 的缩放分支每次调用新建 `col_i0` / `col_i1` / `col_tx` 三张下标与权重表；`Dispatcher` 在悬停与触控活跃期整链拷贝 `HitNode`（每元素一次 `weak_ptr` 原子增减）；`Widget::layout` 的每个修饰节点包一层 `std::function`，通常一次堆分配。这三处成本与窗口内容无关，随指针事件频率、控件数量与图层深度线性放大。

2. **合成阶段的整帧拷贝量级最大**（`to_image()` 复制 `pixels_` 后入 DL 图像池，大窗口每帧 10MB 量级），但它会改动 DL 的图像所有权与序列化契约，须独立提案，本提案不改。

3. **批量快路径留下未收敛的边界带。** `fill_sector` 羽化带逐像素 `atan2` / `fmod`；圆角填充的圆弧过渡列逐像素求 SDF；`draw_shadow` 边缘环逐像素 `sqrt`；`set_pixel` 的矩形裁剪每像素遍历整个 `clip_stack_`。同一图形内「全覆写带」与「过渡列」的 alpha 取整口径还不一致（截断 vs `lround`），偏差上限 1 LSB 却形成可见分带。

4. **两处前置缺口会污染像素基线。** `blend_region` / `mask_region` 只按窗口与区域求界、不读裁剪栈，圆角控件下的效果会外溢；`GlyphAtlas` 返回图集内部的 `Entry*`，淘汰后可能悬垂，且头文件未写指针有效期。二者不先闭环，后续像素判据无法建立可信基线。

5. **反应式与外围仍有固定开销。** `Engine::deliver` 每个 Job 整条拷贝 `active_chain_`；`add_observer` 线性去重；`Animator` / `Spring` 每帧每控制器重复读可访问性设置；`main_queue` 发起路径两次加锁；`Json` 对象键线性查找；`ShapeCache` 命中路径先拷贝整行再整串哈希。

6. **本提案的边界。** 不改 DL 图像所有权、不改 `on_hit_test_chain` 签名、不改 `pending_` 的 `shared_ptr` 保活模型、不做 `swizzle` SIMD（整窗转换受内存带宽限制，先看编译产物）、不做 `draw_line` 轴对齐特判（调用点分布未证）、不改 `request_wake_all`（多窗口共享状态，定向唤醒前提未证）。这些项若推进，各自独立提案。

## 变更内容

1. 先修前置：`blend_region` / `mask_region` 求界并入 `clip_stack_` 交集；`GlyphAtlas` 头注释写明「指针仅在下次 `insert` / `clear` 前有效」，淘汰由全表扫描改为按插入序的最小堆；`reactor` 的纪元戳数组改存活紧凑下标，容量随存活量回落；`set_pixel` 与各批量路径的 alpha 取整统一到单一 helper（以既有 golden 为基线选定口径）；`mark_size_intent_changed` 合并为「自身置位 + 从 `layout_parent_` 无条件上溯一次 + 单次失效与唤醒」。

2. `draw_image` 缩放路径：`sx` 恒定，`ix` / `tx` 按行增量递推，去掉三张表；内层定点双线性按行 SIMD，保留标量尾部；`AURORA_ENABLE_SIMD` 关闭时全标量。

3. 事件链：`hover_chain_` 与触控捕获链改成员缓冲复用与引用读取，消除每次指针 Move 的整链拷贝；回调内链失效时序与 `weak_ptr` 判活语义不变。

4. 布局：`Widget::layout` 的修饰节点回调改 trampoline（函数指针 + 上下文），与 `FlexItem` 的测量回调同构，移除 `std::function` 分配。

5. 渲染细分：`fill_sector` 羽化带按行解析角度区间、`sweep >= π` 走半平面组合；圆角填充过渡列改 2~3 列小批量；`draw_shadow` 边缘环按行分解 `dy` 与左 / 外投影 / 右三段；`Painter` 在 push/pop 时维护裁剪交集缓存，`set_pixel` 单次判界；渐变入口把停止点规范化为升序且覆盖 [0,1]，让非常规输入复用既有分段路径。

6. 外围：`Job` 链改共享只读快照；`add_observer` 加存在性索引；`Animator` / `Spring` 在 `tick` 入口单次读取可访问性设置；`main_queue` 合并为一次加锁发布 `epoch` 与 `closing`；`Json` 对象加键索引且与插入序解耦；`window_bounds` 用父侧兄弟索引；`diff_objects` 先判子树相等再拼路径并复用路径缓冲；`ShapeCache` 键改持有调用方保活的 `string_view`。

7. 每个分组落地时同步在 `CHANGELOG.md` 记 `### Performance` 或 `### Fixed`，并按「回写落点」更新对应规格段落。

## 验收判据

1. 像素一致性：触及像素的改动在 `utest_painter` / `utest_painter_primitives` / `utest_offscreen` 与 golden 场景下逐位 0 差异；alpha 口径统一若改变基线，须在实施记录中列出差异像素数与分布且上限 1 LSB，禁止放宽容差或只重生成基线而不解释。

2. 反向自证：分别把「裁剪交集」「渐变停止点规范化」「裁剪交集缓存」退回旧实现，对应用例必须转红；还原后全绿。绿不算证据，转红才算。

3. SIMD 逐位：新增内核纳入 `utest_simd_parity`，标量 / SSE2 / AVX2 同一输入逐字节一致；`AURORA_ENABLE_SIMD=OFF` 构建下同名用例以 `AURORA_TEST_SKIP` 退让，`registry_integrity` 不漂移。

4. 分配可计数：`draw_image`、`Widget::layout` 修饰链、悬停 / 触控链三处在测试 TU 内以分配计数钩子断言「同输入重复调用时堆分配次数为 0 或较基线下降的确定数量」；该断言须在优化落地前先转红。

5. 性能：同机同构建、`PROFILING=ON`，用现有滚动与渲染基准记录改动前后 `paint` 分项与 p99 / worst；只接受实测改善，`perf_gates.json` 阈值仅在实测稳定通过后才收紧。

6. 不变量与门禁：交付顺序、反馈环截断、取消态各有单测；`main_queue` 单锁发布补跨线程可见性用例；`ctest --preset ninja-test` 全绿（含全部 `check_*`、`lint`、`format-check`）；`aurora_api.json` 无差异。

## 回写落点

- `codespec/ARCHITECTURE.md`：渲染章节补「批量与逐像素 alpha 口径单一来源」「裁剪交集缓存」「渐变色标规范化」三条约束；性能章节补「热路径禁止每次调用固定堆分配」与基准口径。
- `codespec/CODING_STANDARDS.md`：补「热路径分配纪律」与「SIMD 三实现逐位一致」的可检查表述。
- `codespec/specification/03-layout-render.md`：补 `draw_image` 缩放路径、羽化带行级边界、阴影与描边边缘分解、裁剪交集缓存。
- `codespec/specification/02-state.md`：补纪元戳数组的紧凑下标与 `deliver` 批次顺序不变量。
- `codespec/specification/05-event-navigation.md`：补悬停 / 触控链的缓冲复用约束与失效时序。
- `codespec/specification/04-widget.md`：补尺寸意图变更的上溯路径与兄弟索引。
- `codespec/specification/01-core.md`：补 `Json` 对象键索引与插入序解耦的约束、`main_queue` 单次加锁发布。

## 实施记录

落地项（输出逐位不变；由既有用例与下列新增用例守护）：

- `Painter` 裁剪交集缓存与 `set_pixel` / `blend_subpixel` 单次判界；`blend_region` / `mask_region` 求界并入裁剪栈；线性渐变尾标规范化后复用分段路径。反证用例：`utest_painter` 的 `blend_region_respects_rect_clip` / `mask_region_respects_rect_clip`——把求交退回「只按窗口与传入区域求界」，两条用例的区外断言即转红。
- `draw_image` 缩放路径去掉三张临时表，源列下标与权重改为复用成员缓冲；alpha 归一见 `AlphaNormLut`。
- 事件链：触控捕获改共享只读快照、鼠标捕获链移动入表项、悬停链复用成员缓冲（持有形态与失效时序见 `specification/05-event-navigation.md` §3.4）。
- 布局：修饰链测量去掉逐层 `std::function` 堆分配；尺寸意图改一次上溯；`window_bounds` 兄弟定位加「下标 + 地址比对」可校验缓存。
- 状态与外围：提交去重改稀疏纪元戳；`main_queue` 单锁发布纪元与打烊标记；序列化差量复用路径缓冲并按子树相等剪枝。
- `GlyphAtlas` 的 `Entry*` 有效期写入头注释（`find` / `insert` 均声明「仅在下次 `insert` / `clear` 前有效」）。

反证实测（验收判据第 2 条）：临时停用 `blend_region` / `mask_region` 的裁剪求交后重跑 `utest_painter`，该套件 31 例中恰有 2 例转红且正是上述两条新用例；还原后全绿。`set_pixel` 裁剪交集缓存与渐变尾标规范化的逆操作不改变像素（纯等价的路径替换），无对应用例可转红，故不以「测试转红」充当证据。

未采纳，理由就地写进代码注释（结论以注释为准）：

- `GlyphAtlas` 淘汰改最小堆：溢出插入是冷路径，堆化会把 O(log n) 比较搬到每次命中上，而命中远多于溢出插入。
- `reactor` 纪元戳数组改存活紧凑下标：需在每条边访问里加「身份 → 槽位」哈希，现值是一次数组下标；容量压缩还要额外同步。
- `Json` 对象键索引：`Object` 是公共可写别名，旁挂索引与「经别名改键」脱钩，改别名本身是公共 API 变更。
- `ShapeCache` 键改持有调用方 `string_view`：入参生命周期不覆盖条目（跨帧存活至多 4096 条），悬垂键不可复现。
- 悬停链改共享快照：需改 `EventDispatcher::update_hover` 公共签名；现形态在稳态下已无每次调用的堆分配。
- 命中链收集改尾插 + 最外层 reverse：现状每层 `insert(begin())` 的残余成本是深度 k 累计 O(k²) 次元素移动，加上 `size == capacity` 时的几何扩容（每次命中约 ⌈log₂k⌉ 次分配）；换成尾插 + 最外层统一 reverse 可把移动降为 O(k)，彻底零分配则须把 `on_hit_test_chain` 改成出参收集（公共签名 + 全部覆写）。链深 5–15 时两种形态的单次差值在亚微秒级、低于测量噪声，故保留现状，准确量级写进 `Widget::hit_test_chain` 的注释。

移出本案，需独立提案（与本案判据不相容）：

- `fill_sector` 羽化带按行解析、圆角过渡列小批量、`draw_shadow` 边缘环按行分解：几何重写无法由现有用例自证逐位一致；现有批量核心区已覆盖主要成本。
- 分配计数钩子：需在测试二进制内替换全局 `operator new`，会同时改写其余用例的分配观测，属测试基建变更，另行安排。
- 实测基准（`paint` 分项 / p99 / worst）：本机无稳定读数，按「只接受实测改善」不写基线，`perf_gates.json` 未收紧。

已在他处落地，不再列为本案待办：

- alpha 取整统一：`aurora::detail::quantize_global_alpha` 已是 8 位 alpha 乘全局透明度的唯一口径（`set_pixel` 与线性 / 径向渐变的分段批量路径），`global_alpha == 1` 时逐位不变，由 `utest_painter` 的 `quantize_global_alpha_grid` 守护；golden 场景不使用 `global_alpha < 1`，基线无需重录。
