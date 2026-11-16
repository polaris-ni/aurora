# CHG-080 正确性风险收敛——纪元数组有界化、透明度口径统一、schema 缓存失效与字形缓存回归钉

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-080 |
| 提出日期 | 2026-11-16 |
| 当前状态 | 已提议 |
| 关联需求 | 无 |
| 影响面 | 状态引擎 `src/aurora/state/reactor.cpp` 与 `include/aurora/state/detail/reactor.h`（存活登记 + 轮末收缩 + DEBUG 观测口）；渲染 `src/aurora/render/painter.cpp`（透明度整数化口径统一为单一助手，五处替换）；序列化 `include/aurora/widget/serialization.h` 的 `register_factory`（失效同名 schema 缓存项）与 `src/aurora/widget/serialization.cpp`（缓存注释改写）；字形缓存 `src/aurora/render/glyph_atlas.cpp` 判非缺陷不改码，仅以回归用例钉住；测试 `tests/unit/utest_reactor.cpp` / `utest_painter.cpp` / `utest_serialization.cpp` / `utest_glyph_atlas.cpp` 各增用例；文档 `codespec/specification/02-state.md` / `03-layout-render.md` / `04-widget.md` / `06-app-platform.md` 回写。 |

## 动机

对 CHG-079 归档后的性能审计清单做了逐条源码核验，其中四个「正确性风险」条目的核验结论分化：

1. **状态引擎纪元数组无界（属实，但与已文档化取舍冲突）**。`reactor.cpp:262-272` 按**单调递增且不复用**的节点 id / 观察者序号对 `seen_source_epoch_` / `seen_observer_epoch_` 做 `resize(id+1)`；`reactor.h:302` 注释明示「稀疏下标换热路径零哈希，**不随存活量压缩**」——这是论证过的取舍，不是疏漏。但取舍的代价在长会话下成立：虚拟列表按行创建/销毁派生节点时，数组容量随**累计创建数**增长（每节点 2×4 字节），与存活量脱钩。epoch 回绕时的全量清零（`reactor.cpp:251-254`）只清值不缩容。本提案在不破坏「id 不复用」「构造序交付」两条已文档化契约的前提下把容量收敛到存活量级。
2. **透明度整数化口径不一（属实）**。`set_pixel`（`painter.cpp:1421`）对 `a × global_alpha_` 做**截断**，梯度批量路径（`painter.cpp:2021/2045/2098/2238`）做 `std::lround` **四舍五入**——同一输入两口径最多差 1。:355/:417/:1989/:2205 的注释都声称「与 set_pixel 一致」，实际在最后一步整数化上不一致。`global_alpha_ == 1.0` 时两口径恒等（整数乘 1 无舍入差），故默认渲染逐位不变；分歧只在转场淡入淡出（`ga < 1`）期间，且现有用例对该路径全是不等式断言，无逐位 golden 依赖。
3. **schema 静态缓存无失效入口（属实）**。`serialization.cpp:513-514` 注释声称「工厂表在核心注册完成后固定」，但 `register_factory`（`serialization.h:61`）是公共 API、运行期随时可调：同名重注册后缓存返回旧 schema。缓存按类型名分键，新类型注册不污染既有键，**同名重注册**是唯一污染路径。单线程 UI 前提维持不变。
4. **GlyphAtlas 裸指针悬垂（核验为非缺陷）**。`unordered_map` 元素指针/引用在 rehash 下稳定；仅有的两条擦除路径——`clear()`（`glyph_atlas.cpp:40-45`，置空 `last_hit_`）与 `evict_oldest()`（`:56-58`，淘汰对象恰为 last-hit 时置空）——均已正确维护。O(n) 淘汰是冷路径上的既证取舍（`:48-49` 注释）。本项**不改码**，以回归用例把「不悬垂」钉成可执行判据，防止后续被当缺陷误修。

## 变更内容

### 范围裁决

| 项 | 处置 | 破坏面 |
|:---|:---|:---|
| 纪元数组 | 有界化：存活 id 登记 + 轮末收缩；契约（id 不复用、构造序交付）逐字保留 | 仅库内部容量策略；`mark` 热路径 O(1) 数组访问不变 |
| 透明度口径 | 统一为单一 `lround` 助手，替换五处整数化点 | `ga == 1` 默认渲染逐位不变；`ga < 1` 转场期 ±1 alpha 变化，无逐位 golden 依赖 |
| schema 缓存 | `register_factory` 失效同名缓存项 | 公共 API 行为修正（重注册后取到新 schema）；无线程模型变化 |
| GlyphAtlas | 判非缺陷不改码；回归用例钉住 | 无 |

### 纪元数组有界化

- `Engine` 增加存活登记：`SourceNode` / `Observer` 构造时把自身 id / seq 插入 Engine 持有的 `std::set<std::uint64_t>`（单线程 UI 前提下无锁；构造经 `Engine::instance()` 拉起进程级单例，与既有 `run_direct` 等路径同源），析构时摘除。
- 收缩判据挂在轮边界（`deliver()` 末尾，此刻无在途 mark）：当 `arr.size() > max(4096, 4 × live_count)` 时 `resize(max_live_id + 1)` 并 `shrink_to_fit`。正确性论证：纪元戳单调递增，被收缩掉的槽位必属已死节点（id 严格递增不复用 ⇒ 活节点 id 均 ≤ max_live_id）；下轮若被更高 id 触达，`resize` 原样回填。epoch 回绕清零点（`reactor.cpp:251-254`）追加同一收缩判据，覆盖「存活极低但曾达 2^32 轮」的极端会话。
- DEBUG 观测口：`AURORA_ENABLE_DEBUG` 门控下暴露两数组 `size()` / `capacity()` 的只读查询（供验收用例断言容量收敛），按「测试注入」登记制度记入规格。
- **备选与不推荐理由**（供裁决）：① `unordered_map<id, epoch>` 替换数组——保契约、天然有界，但 `mark` 热路径每观察者一次哈希查找，与本文件「稀疏下标换热路径零哈希」的既证取舍相抵，需基准证明无回退；② id/seq 复用 free list——直接违反 `reactor.h:150`「进程内唯一且不复用」与 `:110`「构造顺序」两条契约，且 `RunRecord` 跨轮持有 seq，复用有因果序污染风险，否决。

### 透明度口径统一

- `painter.cpp` 匿名命名空间新增单一助手（整数化 = `clamp(lround(a × ga), 0, 255)`），替换 `:1421`（`set_pixel`）与 `:2021 / :2045 / :2098 / :2238`（梯度路径）五处；`:356 / :419 / :1476 / :1539` 为浮点中间量路径，无整数化点，不动。
- 注释同步：:355 / :417 / :1989 / :2205 的「与 set_pixel 一致」从口头声明变为代码事实（同一助手）。
- 方向论证：取 `lround` 而非截断，因梯度路径已是多数派且 `lround` 数学误差更小；`ga == 1` 时 `a × 1.0` 为精确整数，两口径恒等，默认渲染零变化。

### schema 缓存失效

- `register_factory` 在写入工厂表后失效缓存中同名键（`cache.erase(name)`；取缓存句柄经由既有的函数内 static 访问路径，新增一个匿名命名空间的失效入口供其调用）。
- 注释改写：`serialization.cpp:510-514` 的「工厂表在核心注册完成后固定」改为「注册与缓存失效联动；缓存非线程安全，限 UI 线程」，头文件 `serialization.h` 的 `register_factory` 注释补一句失效语义。

### 字形缓存回归钉（不改码）

- `utest_glyph_atlas.cpp` 增两条用例：①构造「插入至触发淘汰且淘汰对象恰为最近命中条目」的序列，随后以公共 `find`/`insert` 接口继续访问，断言行为正确（间接证伪悬垂解引用）；②`clear()` 后再插入/查找正常。O(n) 淘汰维持现状（冷路径，注释已有论证）。

## 兼容性与迁移

- 纪元数组收缩不改变任何可观察行为：去重语义、交付顺序、因果环切断判据全部依赖「戳 == 本轮」比较，收缩只影响容量，不影响值语义。
- 透明度口径统一在 `ga < 1` 的转场动画里可能产生 ±1 的 alpha 变化；现有用例均为不等式断言（如 `utest_painter_primitives.cpp` 的 `polyline_applies_global_alpha`），无逐位 golden 依赖，预期零迁移。像素 golden 均在 `ga == 1` 下采集，逐位不变。
- `register_factory` 重注册后取到新 schema 是行为修正而非破坏：旧行为（返回陈旧 schema）无正当消费场景。
- `CHANGELOG.md` 顶部当前版本段追加 Changed 条目（schema 缓存失效语义）；无公共签名变化，`aurora_api.json` 无需刷新。
- 源码树与安装态消费者均无感知：四项全部位于库内部或行为修正。

## 不变量与边界

- 「id 进程内唯一且不复用」「观察者按构造序交付」两条契约逐字保留；本批不触碰 id 分配策略。
- `mark` 热路径保持纯数组 O(1)；存活登记只发生在节点构造/析构（冷路径）。
- 默认渲染（`global_alpha_ == 1.0`）逐位不变；`utest_simd_parity` 逐位一致门禁不受影响。
- schema 缓存单线程前提不变，不引入锁；失效只挂在注册这一冷路径上。
- GlyphAtlas 结构与淘汰策略零改动。
- 不做与本次无关的重构、改名或格式化；`codespec/changes/` 下历史提案不改。

## 验收判据

1. 长会话模拟：批量创建并销毁大量 `State`/派生节点（量级 ≥ 10 万累计），经 DEBUG 观测口断言两纪元数组容量收敛到存活量级（判据：`capacity ≤ max(4096, 4 × live)` 于轮末成立），且不再随累计创建数单调增长。
2. 透明度 parity 用例：扫 `(a, ga)` 网格断言 `set_pixel` 路径与梯度批量路径同值；既有 painter 全部用例与像素 golden 逐位不变。
3. schema 失效用例：注册 → 取 schema → 同名重注册 → 再取 schema，断言取到新 schema；既有序列化往返用例全绿。
4. 字形缓存两条回归用例绿；`utest_glyph_atlas` 既有用例全绿。
5. `ctest --preset ninja-test` 全绿（含 `check_declared_gates` / `check_doc_comments` / `check_nolint_layout`）；改动 TU 增量 clang-tidy 零 findings；`format-check` 幂等。
6. `bench_render` 渐变场景无回退：radial/linear 同配置比值不高于基线（本批不含性能优化，仅防回归）。
7. DEBUG 观测口按「测试注入」登记制度记入规格；未开 DEBUG 构建下无新增符号。

## 实施步骤

- [ ] 状态引擎：存活登记 + 轮末收缩 + DEBUG 观测口（含测试注入表登记）；`utest_reactor.cpp` 增长会话收敛用例。
- [ ] 渲染口径：助手提炼与五处替换；`utest_painter.cpp` 增 parity 网格用例。
- [ ] schema 失效：`register_factory` 联动失效 + 注释改写；`utest_serialization.cpp` 增重注册用例。
- [ ] 字形缓存回归钉：`utest_glyph_atlas.cpp` 增两条用例。
- [ ] 文档回写与 `CHANGELOG.md` 追加 Changed 条目；增量 tidy + format + 全量门禁 + bench 防回归核验；状态流转「实施中 → 已归档」。

## 回写落点

- `codespec/specification/02-state.md`：纪元数组容量策略（有界化语义、收缩判据、契约保留声明）。
- `codespec/specification/03-layout-render.md`：全局透明度整数化口径统一；GlyphAtlas「不悬垂」不变量与淘汰策略取舍。
- `codespec/specification/04-widget.md`：schema 缓存失效联动与单线程前提。
- `codespec/specification/06-app-platform.md`：测试注入表新增 DEBUG 门控的纪元数组容量观测口。

根目录 CHANGELOG.md（不属 codespec 树，故不列为回写落点，随批必做）：Changed 条目记 schema 缓存失效语义。
