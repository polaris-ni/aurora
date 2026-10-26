# CHG-015 最近文档应用内 store 立骨（RecentDocuments）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-015 |
| 提出日期 | 2026-10-13 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.RECENT-DOCUMENTS.001 |
| 影响面 | 新增 include/aurora/app/recent_documents.h 与 src/aurora/app/recent_documents.cpp、`codespec` 平台 Shell 相关章节、`tests/unit/`（新增 utest_recent_documents，既有用例零改动） |

## 动机

1. **零实现**：最近文档列表无任何对应 API；对标框架全平台内置（Qt 最近文档菜单 / GTK `GtkRecentManager` / Electron JumpList 与 macOS `NSDocumentController`）。「文件 → 最近打开」是桌面应用的基本肌肉记忆，缺失它应用内菜单与 OS 级最近列表都无从谈起。
2. **两层设计、本提案只落第一层**：最近文档拆「应用内列表」（`Preferences` 持久化、全平台一致行为——对齐 SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001）与「OS 级投影」（Windows JumpList / macOS `NSDocumentController`；Linux 无 OS 级机制 ⇒ 能力位 false + 文档申报）两层。本提案交付应用内 store；OS 级投影由平台补齐程提案落地，消费本 store 的数据。
3. **复用既有范式**：`app::ScrollStorage`（`app/scroll_storage.h`）已立「进程级单例 + `Preferences` attach / detach + 分组持久化 + 内存优先懒回读」范式（守护需求 SPEC.FEAT.WIDGET.LAYOUT.SCROLL-RESTORE.001）。最近文档是「有序集合 + 容量上限 + 同路径去重」语义，比 ScrollStorage 的键值映射更结构化，但单例 / 持久化 / 注入范式原样复用，不另造机制。
4. **无新错误码**：store 是进程内纯逻辑 + `Preferences` 持久化，失败口径沿用 `Preferences` 既有结构化错误；OS 级投影缺位时的 `recent-documents-unsupported` 由平台补齐程落。

## 变更内容

1. 新增 include/aurora/app/recent_documents.h：`class RecentDocuments`，仿 `ScrollStorage` 形态——`instance()` 进程级单例（懒构造）；`static constexpr std::size_t AURORA_MAX_ENTRIES = 16`（容量上限，声明处可见；取常见桌面应用的默认档，调用方按语义需要改常量即可，不提供运行期 setter 以免出现「设置与容量不一致」的第二真源）；`static constexpr const char *AURORA_GROUP_NAME = "recent_documents"`（持久化分组名）。**不引入作用域**：最近文档是应用级全局语义（多窗口共享同一列表），与 `ScrollStorage` 的窗口作用域（`Scope` RAII）不同，复用单例与 attach / sync 范式但不复刻 `Scope`。
2. 会话内读写：`add(path)`（同路径去重并置顶、超容量淘汰最旧；空路径为 no-op）；`list() -> std::vector<std::string>`（新 → 旧序值拷贝）；`clear()`（清内存并打墓碑，下次 `sync` 落盘）；`size()`（诊断 / 测试用）。**路径口径：store 只做字符串级存取**——不校验文件是否存在、不做路径规范化（`..` / 相对路径 / 大小写差异按原样字符串比较去重），规范化与死链剔除由调用方在 UI 侧决定。理由：store 在主线程且不触碰文件系统，读存在性会引入不可测的 IO 副作用与 races；「最近文档里留着已删文件」是应用该不该显示的决策，不是 store 该替应用做的。
3. 持久化（可选，仿 `ScrollStorage`）：`attach(preferences::Preferences &)` / `detach()` / `sync()`；`sync()` 只写 `Preferences` 内存并通知订阅者，**落盘由 `App` 的 `flush()` 决定**（与 `ScrollStorage` 同口径）；`Preferences::Group::set` 已支持 `std::vector`（值类型注释明示 `bool` / 整数 / 浮点 / `std::string` / `std::vector` / JSON 对象），故信封为分组下单键 JSON 数组 `{"documents":["..."]}`。**与 `ScrollStorage` 的差异及理由**：ScrollStorage 是「每键一条目」的键值映射（键 = 恢复键），最近文档是**有序集合、顺序即语义**，用索引当键会让顺序编码进键名、墓碑与重排逻辑都不必要地复杂，故取单键数组；这是复用范式后的有意分歧，不是跟随时的疏漏。读取懒回读（首次 `list` 时从 `Preferences` 填充内存缓存），不做全量预热；读缺失或 JSON 损坏 = 空列表（宽容降级，不抛错、不清除用户数据）。
4. 不内置变更通知：store 只管存取（同 `ScrollStorage`——「何时恢复 / 何时写回由调用方接线」）；菜单 UI 刷新由应用在 `add` / `clear` 后自行驱动，文档注明该口径，避免引入第二套通知机制。
5. OS 级投影不落本提案：平台补齐程落 JumpList / `NSDocumentController` 时消费 `list()`；Linux 无 OS 级机制的能力位申报同属该程。
6. 新增 tests/unit/utest_recent_documents.cpp：去重置顶 / 容量淘汰 / 顺序 / 持久化往返 / 空口径全覆盖。

## 验收判据

1. 集合语义单测：同路径 `add` 三次仅一条且置顶；超 `AURORA_MAX_ENTRIES` 淘汰最旧；`list` 新 → 旧序；`clear` 后 `list` 空、`size` 为 0；空路径 `add` 为 no-op（`size` 不变）。路径口径单测：`add` 一个磁盘上不存在的路径正常入列且不报错（store 不触碰文件系统）；仅大小写或相对 / 绝对写法不同的两条路径视为不同条目（原样字符串去重，不隐式规范化）。**变异自证**：去掉去重逻辑或调换置顶 / 淘汰方向，相关用例必须转红。
2. 持久化往返单测：`attach`（测试用 `Preferences` 实例或临时目录）后 `add` → `sync` → 以新 `RecentDocuments` 实例 `attach` → `list` 读回逐条一致；`clear` + `sync` 后读回为空；损坏 JSON（手写非法内容进分组）= 空列表且不抛。
3. 空口径：未 `attach` 时 `list` 空（纯内存会话）；无记录时 `list` 空而非错误。
4. 既有零回归：`utest_preferences` / `utest_scroll_storage` 全绿且不改动（store 不触碰 `Preferences` 既有分组语义）。
5. API 面治理：`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移；`check_api_budget` 增量评估通过（一个类、范式与 `ScrollStorage` 同构）。
6. 门禁与测试：`check_naming_conventions` / `check_doc_comments` / `check_platform_macros` / `check_core_layer_boundary` / `check_no_hardcoded_paths` 全绿；`lint`（仅改动相关 TU）与 `format-check` 绿；全量 `ctest -j16` 绿。

## 回写落点

- `codespec/specification/06-app-platform.md`（§8 平台 Shell：新增最近文档节——应用内 store 契约、持久化信封、空口径、无内置通知口径；OS 级投影留平台补齐程）
- `codespec/SPECIFICATIONS.md`（需求行已于本提案同批落定）
