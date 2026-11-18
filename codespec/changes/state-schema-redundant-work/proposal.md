# CHG-086 状态订阅登记与序列化读取的重复开销收敛

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-086 |
| 提出日期 | 2026-11-18 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.PERF.RUNTIME.FRAME-BUDGET.001 |
| 影响面 | `src/aurora/state/reactor.cpp`、`include/aurora/state/detail/reactor.h`、`include/aurora/state/state.h`、`include/aurora/state/read_only_state.h`（订阅登记）；`src/aurora/widget/serialization.cpp`、`include/aurora/widget/serialization.h`（数组差分与 schema 缓存）；`include/aurora/app/ui_prompt.h`、`include/aurora/app/generate_ui.h`、`include/aurora/widget/codegen.h`、`src/aurora/app/validate_ui.cpp`（只读消费端）；`tools/bench` 下新增基准源与 `cmake/AuroraTools.cmake`（新增基准登记）；`tests/unit/utest_reactor.cpp`、`tests/unit/utest_state.cpp`、`tests/unit/utest_serialization.cpp`；`CHANGELOG.md`；`aurora_api.json`（新增公共查询接口须重生成） |

## 动机

1. **订阅登记带一次线性扫描。** `SourceNode::add_observer`（`src/aurora/state/reactor.cpp:130-138`）在 `push_back` 前逐条比对 `observers_` 里的地址；`observers_` 是 `std::vector<Observer *>`（`include/aurora/state/detail/reactor.h:192`），同一来源被 K 个观察者订阅时，K 次登记累计 O(K²) 次比较。`Widget::wire_signals`（`src/aurora/widget/widget.cpp:1060`）在每次 `mount()` 走一遍 `StateSink::watch`，页面重建或重挂时全量重订阅，共享状态源上的观察者数因此线性累积。

2. **这次扫描不是纯冗余，但也不是最优解。** 固定来源订阅（`include/aurora/state/state.h:119`、`include/aurora/state/read_only_state.h:114/204`）为避免「首跑排队期间来源提交丢掉通知」先手动登记一次；随后首跑 `execute()` 里 `read()` 登记依赖（`reactor.h:210-216`），`replace_deps()`（`reactor.cpp:199-203`）又对同一条边登记一次。两条路径指向同一观察者对象，线性扫描正是用来吸收这一重复的。正确做法是让登记侧自身唯一（`add_observer` 的文档已经写明「去重由调用方保证」，但调用方并未做到），而不是在每次登记时重新发现重复。

3. **数组差分为每个元素新建路径串。** `diff_arrays`（`src/aurora/widget/serialization.cpp:400-412`）用 `path + "/" + std::to_string(i)` 组装下标路径，每个元素两次分配；同一文件的 `diff_objects`（`377-397`）已经复用一块 `child` 缓冲（`assign(path).append(1, '/').append(key)`），两分支口径不一致。数组元素多数相等时，路径串拼好后才在 `diff_into` 开头被判等提前返回，拼接纯属浪费。

4. **schema 缓存命中仍付一次深拷贝。** `component_schema`（`serialization.cpp:523-531`）缓存命中直接 `return it->second`；`Json` 是 `json::Value`（`include/aurora/widget/props_io.h:18`），内部为 `std::variant<… std::string, std::vector<Value>, std::vector<std::pair<std::string, Value>>>`（`include/aurora/core/json.h:322`），没有共享指针也不做写时复制，所以每次命中都是一次整棵树的深拷贝。`search_components`（`689`）与 `list_all_schemas`（`699`）按命中数/类型数再各取一份，量随类型数线性放大；`describe_component` 的只读调用点（`ui_prompt.h:59/295`、`generate_ui.h:120`、`codegen.h:381`）每次构建 prompt 或生成代码都要付这份拷贝。

5. **一项明确移出。** `RunRecord::as_terminal()`（`reactor.cpp:166`）每终端一次 `shared_from_this()` 的原子计数不纳入本案：`Engine::deliver()`（`reactor.cpp:369-396`）在交付前判 `record->cancelled()`，回调级联取消会让裸指针悬空，`pending_` 持有的 `shared_ptr` 是生命周期安全网而非纯开销。要改必须先给交付批次设计整体 pin 语义，收益与风险不匹配，单列遗留。

6. **边界。** 不改 `Json` 的内存表示（不引入写时复制或共享节点）；不改 `describe_component` / `search_components` / `list_all_schemas` 的按值返回契约；不改通知轮次、因果链剪枝与 `epoch` 去重口径；不为基准放宽任何既有时限。

## 变更内容

### 订阅登记

1. `RunRecord` 增加一个「首跑前已登记来源」标记（单指针，`nullptr` 表示无），并提供记录入口。固定来源订阅在手动 `add_observer` 之后记录该来源；`replace_deps()` 遍历新读集合时，若该来源正是预登记项则跳过 `add_observer`，随后清空标记。非固定来源的普通 effect 不设标记，行为与现状一致。

2. `SourceNode::add_observer` 删除线性扫描，改为 Debug 断言（`AURORA_ASSERT`，`NDEBUG` 下整体裁切、条件不求值）校验不重复登记。Release 下登记退化为一次 `push_back`；契约从「函数内部兜底去重」改成「调用方保证唯一 + Debug 兜底发现违约」，与头文件既有注释一致。

3. `RunRecord::cancel()` 若预登记标记仍非空，一并摘除该观察者并清空标记；`replace_deps()` 在标记指向的来源未出现在本轮读集合时同样摘除，避免「订阅已取消但观察者残留」。这两处是删除内部去重后必须补齐的所有权闭环。

### 数组差分

4. `diff_arrays` 改为与 `diff_objects` 同形：函数内复用一块 `std::string child` 缓冲，`assign(path).append(1, '/').append(std::to_string(i))`；两侧元素都存在时先判 `*a.at(i) != *b.at(i)` 再拼路径，相等元素不构造任何字符串。路径文本、补丁顺序与 `op` 取值不变。

### schema 只读视图

5. schema 缓存改存 `std::shared_ptr<const Json>`，构造成员仍只构造一次；新增两个公共只读入口：单类型视图与全量视图列表。视图返回的是缓存持有者共享的同一份 JSON，调用方无需复制即可读取任意时长——`invalidate_component_schema` 与同名重注册只影响之后的查询，已取得的视图继续指向当时的快照。原有的三个按值 API 保留，实现改为对视图解引用（单类型）或逐项解引用（列表），输出与改动前逐字段相同。

6. 只读消费端切换到视图入口：`ui_prompt.h`、`generate_ui.h`、`codegen.h`、`validate_ui.cpp` 中仅读取 schema 的局部变量改为持有视图；需要向外交出 `Json` 值的边界（`Inspector::components`、MCP/CLI 的响应构造）保持按值，不做跨接口的所有权改造。

### 共同纪律

7. 每项改动随同落地行为等价用例与一次可转红的注入变异（见验收判据第 2 条），不接受「看起来更快」作为证据。

8. 每项改动同步更新 `CHANGELOG.md` 的 `### Performance` 段落，并按「回写落点」更新对应规格；新增公共接口须重生成 `aurora_api.json` 并同批通过 API 门禁。

9. **实施记录**：三项全部落地。① 订阅登记：`RunRecord` 增加预登记标记，`replace_deps` 跳过同一来源的重复挂接，`cancel` 与「首跑未读到预登记来源」两条路径补摘除；`add_observer` 删去线性扫描，唯一性改由登记侧保证、Debug 断言复核。落地时确认该扫描并非纯冗余——`subscribe` 先挂接一次、首跑 `read()` 又在 `replace_deps` 登记一次，正是它吸收了这次重复，故先修上游再删扫描，而不是直接在入口加哈希索引。② 数组差分：`diff_arrays` 与 `diff_objects` 同形复用缓冲，相等子树先判后拼。③ schema：缓存改持 `shared_ptr<const Json>`，新增三个只读入口，只读消费端切换；按值 API 实现改为对视图解引用，输出不变。变异自证：去掉预登记跳过令「一次提交一次交付 / 观察者数 == 1」用例转红；把下标路径改成父路径令三条路径断言转红；把视图命中改回拷贝令「两次取视图同一指针」转红；三处均验后文本还原。性能（同机 Debug、新增 `bench_state_serial`）：数组差分 2.491 → 0.916 ms、分配 12299 → 4107 次；schema 单次按值约 49–63 µs / 231 次分配 vs 视图 0.07–0.26 µs / 0 次分配（耗时含同码抖动，分配数是稳定判据）；全量列表 1.63–1.73 → 0.087–0.093 ms。订阅登记 10.02 → 9.90 µs/订阅（噪声内），故只申报结构收益、不申报性能数字。移出项仍为 `as_terminal()` 的交付所有权（见第 5 条）。`aurora_api.json` 按第 8 条用 `gen_api_tools` 重生成后无差异——该生成器只收录 widget 类型、枚举与顶层段，不收录顶层自由函数；`check_api_schema_sync` / `check_gen_api_merge` / `check_api_budget` 同批绿。

## 验收判据

1. **行为等价**：① 订阅侧——同一来源上 N 个订阅者提交一次各收到恰好一次回调，`Subscription` 析构后不再收到，重复建立/撤销后观察者数不残留（`utest_reactor.cpp` / `utest_state.cpp` 补充用例）；② 差分侧——嵌套数组、长度不等、元素相等与不等的组合，`diff()` 输出的 `op` / `path` / `value` 与改动前逐字段相同（`utest_serialization.cpp` / `utest_snapshot_diff.cpp` 既有用例须全绿，另补一条深度嵌套数组用例）；③ schema 侧——视图与按值结果深比较相等，同一类型两次取视图为同一指针，失效后按值查询反映新内容而旧视图保持原快照（`utest_serialization.cpp`）。

2. **反向自证**：每项至少注入一次会改变行为的变异，转红才算证据。订阅：去掉 `replace_deps` 对预登记项的跳过，首跑重复登记使「提交一次、回调一次」用例转红；差分：把下标路径拼成父路径（漏掉索引段），嵌套数组用例的补丁路径断言转红；视图：把视图入口改回返回新构造的拷贝，同一性断言（两次调用指针相同）转红。每项验完即文本还原，不用版本控制回退。

3. **基准**：新增 `tools/bench` 下的状态与序列化基准（`bench_state_serial`），三个场景——单来源 N 订阅者的建立耗时与分配数、大数组（含等长相等/不等混合）的 `diff()` 耗时与分配数、`describe_component` 与全量 schema 的取值耗时与分配字节数（按值 vs 视图）。基线用暂存改动取同机同构建对照，记录均值与分配数；分配数是稳定判据，耗时只作参考。工具按既有 `aurora::bench` 约定输出，不接入 CTest。

4. **门禁与跨平台**：`check_change_proposals --root .`、`check_codespec_xref .`、`check_code_doc_sync .` 全绿；`check_api_schema_sync` 与 `check_gen_api_merge` 在重生成 `aurora_api.json` 后同批绿；增量 clang-tidy 只扫改动 TU；`clang-format` 全量合规。触及公共头（`serialization.h` 新增声明）另跑 `build-wasm` 与 WSL 的 `build-x11`。

5. **范围红线**：`as_terminal()` 的所有权模型、`Json` 的表示、按值 API 的签名均不在本案授权范围内，需另案。

## 回写落点

- `codespec/ARCHITECTURE.md` §4.1 基础层与响应式核心：补「观察者登记的唯一边界（调用方保证 + Debug 断言）」与首跑预登记的生命周期；§9 序列化与元信息：补 schema 缓存视图的所有权与失效语义；§10.7 基准读数契约与持续看护：补本案三类微基准的读数口径。
- `codespec/specification/02-state.md` §3.4 固定来源订阅：补「手动预登记 → 首跑读依赖」不重复挂接的契约；附录 A.3 依赖剪枝与注册表：补登记唯一性与取消时的观察者摘除闭环。
- `codespec/specification/08-tooling.md` §2.2 API：补 schema 只读视图入口与失效语义；§2.4 差分补丁协议：补数组分支的路径复用与相等先判（输出不变）；§6 自描述发现 API：补视图入口与按值入口的分工。
- `codespec/CODING_STANDARDS.md` §6.14 热路径分配纪律：补「缓存命中返回只读视图」与「登记侧保证唯一、热路径不做防御性线性扫描」两条写法。
