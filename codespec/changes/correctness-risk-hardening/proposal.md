# CHG-080 正确性风险收敛——纪元数组有界化、透明度口径统一、schema 缓存失效与字形缓存回归钉

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-080 |
| 提出日期 | 2026-11-16 |
| 当前状态 | 实施中 |
| 关联需求 | 无 |
| 影响面 | 状态引擎 `src/aurora/state/reactor.cpp` 与 `include/aurora/state/detail/reactor.h`（存活登记 + 析构水位收缩 + DEBUG 观测方法）；渲染 `src/aurora/render/painter.cpp` 与 `include/aurora/render/detail/gamma_lut.h`（透明度整数化助手 + 五处替换）；序列化 `include/aurora/widget/serialization.h` 的 `register_factory`（调用失效入口）与 `src/aurora/widget/serialization.cpp`（缓存提升为文件级 + 失效定义）；字形缓存 `src/aurora/render/glyph_atlas.cpp` 判非缺陷不改码，仅以行为契约用例钉住；测试 `tests/unit/utest_reactor.cpp` / `utest_painter.cpp` / `utest_serialization.cpp` / `utest_glyph_atlas.cpp` 各增用例；文档 `codespec/specification/02-state.md` / `03-layout-render.md` / `04-widget.md` 回写（观测口登记并入 02-state，该文档无状态引擎章节故不再列 06-app-platform）。 |

## 动机

对 CHG-079 归档后的性能审计清单做了逐条源码核验，其中四个「正确性风险」条目的核验结论分化：

1. **状态引擎纪元数组无界（属实，但与已文档化取舍冲突）**。`reactor.cpp:262-272` 按**单调递增且不复用**的节点 id / 观察者序号对 `seen_source_epoch_` / `seen_observer_epoch_` 做 `resize(id+1)`；`reactor.h:302` 注释明示「稀疏下标换热路径零哈希，**不随存活量压缩**」——这是论证过的取舍，不是疏漏。但代价在长会话下成立：虚拟列表按行创建/销毁派生节点时，数组容量随**累计创建数**增长（每节点 2×4 字节），与存活量脱钩。epoch 回绕时的全量清零（`reactor.cpp:251-254`）只清值不缩容。本提案在不破坏「id 不复用」「构造序交付」两条已文档化契约的前提下，把容量上界从「累计创建峰值 id」收敛到「**当前存活集合的最高 id**」，并给出明确的边界：稠密数组 + 不复用 id 的固有代价是容量与最高存活 id 同阶，只有放弃零哈希热路径（改用映射重编号）才能做到 O(存活数)——该取舍维持不变。
2. **透明度整数化口径不一（属实）**。`set_pixel`（`painter.cpp:1421`）对 `a × global_alpha_` 做**截断**，梯度批量路径（`painter.cpp:2021/2045/2098/2238`）做 `std::lround` **四舍五入**——同一输入两口径最多差 1。:355/:417/:1989/:2205 的注释都声称「与 set_pixel 一致」，实际在最后一步整数化上不一致。`global_alpha_ == 1.0` 时两口径恒等（整数乘 1 无舍入差），故默认渲染逐位不变；分歧只在转场淡入淡出（`ga < 1`）期间，且现有用例对该路径全是不等式断言，无逐位 golden 依赖。
3. **schema 静态缓存无失效入口（属实）**。`serialization.cpp:513-514` 注释声称「工厂表在核心注册完成后固定」，但 `register_factory`（`serialization.h:61`）是公共 API、运行期随时可调：同名重注册后缓存返回旧 schema。缓存按类型名分键，新类型注册不污染既有键，**同名重注册**是唯一污染路径。单线程 UI 前提维持不变。
4. **GlyphAtlas 裸指针悬垂（核验为非缺陷）**。`unordered_map` 元素指针/引用在 rehash 下稳定；仅有的两条擦除路径——`clear()`（`glyph_atlas.cpp:40-45`，置空 `last_hit_`）与 `evict_oldest()`（`:56-58`，淘汰对象恰为 last-hit 时置空）——均已正确维护。O(n) 淘汰是冷路径上的既证取舍（`:48-49` 注释）。本项**不改码**；裁决依据是上述代码事实，**不靠行为用例间接证明**——用例只锁住「缓存条目内容正确」的行为契约，防止后续误改。

## 变更内容

### 范围裁决

| 项 | 处置 | 破坏面 |
|:---|:---|:---|
| 纪元数组 | 有界化：存活登记 + 析构水位收缩 + 轮末兜底；契约（id 不复用、构造序交付）逐字保留 | 库内部容量策略；`mark` 热路径 O(1) 数组访问不变 |
| 透明度口径 | 统一为单一 `lround` 助手，替换五处整数化点 | `ga == 1` 默认渲染逐位不变；`ga < 1` 转场期 ±1 alpha 变化，无逐位 golden 依赖 |
| schema 缓存 | 缓存提升为文件级匿名命名空间对象；头文件声明外部链接失效入口，`register_factory` 调用 | 公共 API 行为修正（重注册后取到新 schema）；无线程模型变化 |
| GlyphAtlas | 判非缺陷不改码；行为契约用例钉住 | 无 |

### 纪元数组有界化

- `Engine` 增加存活登记：`SourceNode` / `Observer` 构造时把自身 id / seq 插入 Engine 持有的 `std::set<std::uint64_t>`（单线程 UI 前提下无锁），析构时摘除。**构造路径由此新增对 Engine 单例的依赖**：原先只用函数内静态原子计数器（`reactor.cpp:28-41`），现在要访问 Engine 的集合；`Engine::instance()` 是 `new Engine()` 的进程级常驻单例，静态销毁期仍可达，语义与既有提交路径同源（原稿「与 run_direct 同源」措辞不准，已更正为「由 `Engine::instance()` 惰性构造」）。
- 收缩触发点（两处，缺一不可）：
  - 析构水位：`SourceNode` / `Observer` 析构时摘除存活身份并检查收缩；检查是 O(1) 判等，无节流必要（设想的摊还 stride 会让存活尾巴错过最后一次检查，实施时已放弃），且仅当当前不在交付阶段（`in_delivery_` 为 false，防止容器在 `mark` 遍历中被修改）时执行。此路径覆盖「只创建/销毁、从不写提交」的长会话。
  - 轮末兜底：`deliver()` 末尾再检查一次（此时无在途 mark），覆盖析构发生在交付内而被守卫跳过的情形。
- 收缩动作与正确性论证：设 `M` = 当前存活集合的最大 id / seq（空集取 0）。当 `arr.size() > max(4096, 4 × live_count)` 时截断到 `M + 1` 并以「拷贝 + swap」强制缩容（`shrink_to_fit` 是非绑定请求，实现可不缩容，而判据要求 size == capacity 必须确定性成立）。安全性：id 严格递增不复用 ⇒ 未来构造的新节点 id 必 > M，`resize` 会扩容；≤ M 的已死节点不会再出现在 `mark` 的遍历源（活跃边与 `pending_` 只持有活对象）；因此不会漏掉有效去重戳。epoch 回绕全量清零处（`reactor.cpp:251-254`）复用同一收缩检查。
- **容量边界（必须随判据一起读）**：收缩保证 `capacity ≤ max(4096, M + 1)`，**不保证** `capacity ≤ 4 × live_count`——稀疏存活（例如仅剩一个高 id 的长期节点）时二者相差很大。这是「不复用 id + 稠密数组下标」两条既有契约的固有代价，不是泄漏；判据据此写，不另造不可达目标。
- DEBUG 观测方法：`AURORA_ENABLE_DEBUG` 门控下在 `Engine` 暴露 `seen_source_size()` / `seen_source_capacity()` / `seen_observer_size()` / `seen_observer_capacity()` 四个只读查询，**声明与定义同受门控**（非 DEBUG 构建下声明不存在，消费者不可调用，也不产生符号）；按「测试注入」登记制度记入规格。新增方法按 `CODING_STANDARDS.md` §13 写全 `@brief` / `@return`。
- **备选与不推荐理由**（供裁决）：① `unordered_map<id, epoch>` 替换数组——保契约、天然有界，但 `mark` 热路径每观察者一次哈希查找，与本文件「稀疏下标换热路径零哈希」的既证取舍相抵，需基准证明无回退；② id/seq 复用 free list——直接违反 `reactor.h:150`「进程内唯一且不复用」与 `:110`「构造顺序」两条契约，且 `RunRecord` 跨轮持有 seq，复用有因果序污染风险，否决；③ 存活节点重编号到紧凑槽位（需额外 id→槽位映射）——可做到 O(存活数) 容量，但把零哈希热路径换成一次映射查找，超出本批范围，否决。

### 透明度口径统一

- 在 `include/aurora/render/detail/gamma_lut.h` 新增 `aurora::detail` 命名空间的 `inline` 助手（按 §13 写全注释），语义为 `clamp(lround(a × ga), 0, 255)`，单精度 `ga` 在调用处转 `double`。该头属 `aurora/render/detail/*`（umbrella manifest 已登记为内部实现细节），不新增公共头、不进 `aurora_api.json`。
- `painter.cpp` 五处整数化点（`:1421` 的 `set_pixel` 与 `:2021 / :2045 / :2098 / :2238` 的梯度路径）改为调用同一助手；`:356 / :419 / :1476 / :1539` 为浮点中间量路径，无整数化点，不动。
- 注释同步：`:355 / :417 / :1989 / :2205` 的「与 set_pixel 一致」从口头声明变为代码事实（同一助手）。
- 方向论证：取 `lround` 而非截断，因梯度路径已是多数派且 `lround` 数学误差更小；`ga == 1` 时 `a × 1.0` 为精确整数，两口径恒等，默认渲染零变化。

### schema 缓存失效（跨 TU 可实现方案）

- `src/aurora/widget/serialization.cpp`：把 `component_schema` 内的函数级 `static std::map<std::string, Json> cache` 提升为**文件级匿名命名空间对象**，供 `component_schema` 与失效入口共用（保持单 TU 内部链接，不进公共 API）。
- `include/aurora/widget/serialization.h`：在 `aurora::serialization` 命名空间声明外部链接函数 `invalidate_component_schema(name)`（写全注释）；`serialization.cpp` 给出定义，内部对该文件级缓存执行 `erase(name)`。
- `WidgetRegistry::register_factory`（头文件内联成员，`:61`）在写入 `factories_` 后调用上述**已声明的外部链接函数**——原稿「调用匿名命名空间入口」在链接期不可行（内联成员在每个消费 TU 展开，无法解析另一个 TU 的内部链接符号），已更正。
- 注释改写：`serialization.cpp:510-514` 的「工厂表在核心注册完成后固定」改为「注册与缓存失效联动；缓存非线程安全，限 UI 线程」；`serialization.h` 的 `register_factory` 注释补失效语义。

### 字形缓存行为契约钉（不改码）

- `utest_glyph_atlas.cpp` 增用例只断言**确定性行为契约**：任意插入/淘汰/清空序列后，对已淘汰 key 的 `find` 返回空、对存活 key 的 `find` 返回其内容、对同一 key 重新 `insert(v2)` 后 `find` 必返回 v2。覆盖「淘汰对象恰为最近命中条目」与「`clear()` 后复用 key」两个序列。
- 这些用例**不作为「无悬垂」的证明**——裁决来自指针稳定性与两条擦除路径的代码事实；用例的作用是防止后续把行为改坏。O(n) 淘汰维持现状（冷路径，注释已有论证）。

## 兼容性与迁移

- 纪元数组收缩不改变任何可观察行为：去重语义、交付顺序、因果环切断判据全部依赖「戳 == 本轮」比较，收缩只影响容量，不影响值语义。
- 透明度口径统一在 `ga < 1` 的转场动画里可能产生 ±1 的 alpha 变化；现有用例均为不等式断言（如 `utest_painter_primitives.cpp` 的 `polyline_applies_global_alpha`），无逐位 golden 依赖，预期零迁移。像素 golden 均在 `ga == 1` 下采集，逐位不变。
- `register_factory` 重注册后取到新 schema 是行为修正而非破坏：旧行为（返回陈旧 schema）无正当消费场景。
- **公共面变化（如实记账）**：`Engine` 新增 DEBUG 门控的四个只读观测方法、`Engine` 类新增两个存活集合成员、`aurora::serialization` 新增一个外部链接函数、`aurora::detail` 新增一个 `inline` 助手。均位于 `include/aurora/**/detail` 或 widget 序列化内部命名空间，**不属 widget/enum 反射面**，`aurora_api.json` 无需刷新；非 DEBUG 构建下观测方法声明不存在，消费者不可调用。
- `CHANGELOG.md` 顶部当前版本段追加 Changed 条目（schema 缓存失效语义与观测口新增）。
- 源码树与安装态消费者均无感知迁移义务：无签名删除、无字段删除、无枚举变化。

## 不变量与边界

- 「id 进程内唯一且不复用」「观察者按构造序交付」两条契约逐字保留；本批不触碰 id 分配策略。
- `mark` 热路径保持纯数组 O(1)；存活登记与收缩检查在构造/析构（低成本树操作 + O(1) 判等），受 `in_delivery_` 守卫。
- 容量上界按「最高存活 id」定义，不承诺 O(存活数)；稀疏存活的容量残留是既有契约代价，已在动机与判据中明示。
- 默认渲染（`global_alpha_ == 1.0`）逐位不变；`utest_simd_parity` 逐位一致门禁不受影响。
- schema 缓存单线程前提不变，不引入锁；失效只挂在注册这一冷路径上。
- GlyphAtlas 结构与淘汰策略零改动。
- 不做与本次无关的重构、改名或格式化；`codespec/changes/` 下历史提案不改。

## 验收判据

1. 长会话模拟（**含纯创建/销毁、零写提交的段落**）：批量创建并销毁大量 `State`/派生节点（量级 ≥ 10 万累计），经 DEBUG 观测口断言：① 轮末兜底与析构检查后 `size == capacity ≤ max(4096, M + 1)`（M 为存活集合最高 id / seq）；② 存活集合清空后两数组 `size == 0`；③ 容量不随累计创建数单调增长（对比「10 万累计 / 存活 1」与「20 万累计 / 存活 1」两档，容量均收敛到各自 M+1 而非累计峰值）。
2. 透明度助手网格用例：直接对 `aurora::detail` 的助手扫 `(a, ga)` 网格（含 a=0/255、ga=0/0.5/1、非整数乘积边界）断言输出等于 `clamp(lround(a×ga), 0, 255)`；五处调用点均由该助手实现（代码核对）；既有 painter 全部用例与 `ga == 1` 像素 golden 逐位不变。**不设「两条绘制路径输出同值」这类不可直接比较的判据。**
3. schema 失效用例：注册 → 取 schema → 同名重注册 → 再取 schema，断言取到新 schema；既有序列化往返用例全绿。
4. 字形缓存行为契约用例：覆盖「淘汰对象恰为最近命中条目」与「`clear()` 后复用 key」两序列，断言已淘汰 key `find` 为空、存活 key 内容正确、重新 `insert(v2)` 后 `find` 返回 v2；`utest_glyph_atlas` 既有用例全绿。**本条不是悬垂修复的证据。**
5. `ctest --preset ninja-test` 全绿（含 `check_declared_gates` / `check_doc_comments` / `check_nolint_layout` / `check_api_schema_sync`）；改动 TU 增量 clang-tidy 零 findings；`format-check` 幂等。
6. DEBUG 观测口按「测试注入」登记制度记入规格；DEBUG 构建下可编译并调用，非 DEBUG 构建下声明不可见（由常规构建 leg 的消费者 TU 编译通过间接证明，不另设符号检查）。

## 实施步骤

- [ ] 状态引擎：存活登记 + 析构水位收缩 + 轮末兜底 + DEBUG 观测方法（登记并入状态域文档）；`utest_reactor.cpp` 增长会话收敛用例（含零提交段落）。
- [ ] 渲染口径：`gamma_lut.h` 助手 + 五处替换 + 注释同步；`utest_painter.cpp` 增助手网格用例。
- [ ] schema 失效：缓存提升为文件级匿名对象 + 头文件外部链接声明 + `register_factory` 调用 + 注释改写；`utest_serialization.cpp` 增重注册用例。
- [ ] 字形缓存行为契约钉：`utest_glyph_atlas.cpp` 增用例。
- [ ] 文档回写与 `CHANGELOG.md` 追加 Changed 条目；增量 tidy + format + 全量门禁；状态流转「实施中 → 已归档」。本批不含性能路径变更，不设 bench 回归判据（radial/linear 比值基线自身波动 13%~76%，信噪比不足；性能验证留给后续优化批次）。

## 回写落点

- `codespec/specification/02-state.md`：纪元数组容量策略（有界化语义、以最高存活 id 为界的收缩判据、契约保留声明）与 DEBUG 观测口登记（状态域文档为 `02-state.md`，原拟登记到 `06-app-platform.md` 的条目并入此处——该文档无状态引擎章节）。
- `codespec/specification/03-layout-render.md`：全局透明度整数化口径统一与助手归属；GlyphAtlas「不悬垂」代码事实与行为契约用例的定位（不作为证明）。
- `codespec/specification/04-widget.md`：schema 缓存失效联动与单线程前提。

根目录 CHANGELOG.md（不属 codespec 树，故不列为回写落点，随批必做）：Changed 条目记 schema 缓存失效语义与观测口新增。
