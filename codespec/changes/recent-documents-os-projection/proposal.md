# CHG-038 最近文档 OS 级投影（Windows `SHAddToRecentDocs` / macOS `NSDocumentController`，Linux 能力位申报）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-038 |
| 提出日期 | 2026-10-26 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.RECENT-DOCUMENTS.001 |
| 影响面 | `include/aurora/window/platform.h`（`PlatformCapabilities` 增 `recent_documents_os_projection` 位）；`include/aurora/app/recent_documents.h` 同址扩充投影入口；`src/aurora/window/detail/` 新增零平台头折算层与 Win32 / macOS 两枚平台桥；`codespec/errors.toml` 及生成物扩一枚码；`tests/unit/` 新增折算层与能力位用例；`tools/verify/` 新增一枚真机探针；`codespec/` 两处回写 |

## 动机

1. **需求只兑现了第一层。** 「应用内 store 持久化 + OS 级投影」里的前半已落（`RecentDocuments`：进程级单例、`Preferences` 持久化、去重置顶、容量淘汰、宽容降级，配套单测齐备），后半是零——全仓 `SHAddToRecentDocs` / JumpList / `NSDocumentController` 零命中，头注与规格都明写「OS 级投影不在本类职责内」。结果是**应用内有一份最近文档，操作系统那边一份都没有**：任务栏跳转列表、文件资源管理器的「最近使用」、macOS 的「文件 → 打开最近使用」、Dock 右键菜单，这些用户已经形成肌肉记忆的入口全是空的。应用自己造一个菜单能补上一部分，但补不上 Dock 右键与跳转列表。
2. **`add` 之后不自动投影，是有意的，不是漏的。** 三条理由：其一，投影是**有外部副作用的动作**（改 OS 状态、可能被系统审计或同步），而 store 的既有哲学是「显式提交」——`sync()` 只写内存、落盘由 `App` 的 `flush()` 决定，不引入隐式写穿。其二，时机不由 store 决定：文档「打开成功」才该进最近列表，打开失败（权限不足、格式不对、解析一半报错）也常常已经走过了 `add` 的调用点，而撤销一条已推给 OS 的记录比不推麻烦得多。其三，平台语义本就是显式的——Qt 与 GTK 的对应设施都是应用显式调用，`SHAddToRecentDocs` 与 `noteNewRecentDocumentURL:` 同样如此。故本批只给显式推送入口，不把副作用藏进 `add`。
3. **Linux 不落 OS 级投影，需求本身已为这个缺位留了口径。** 需求原文写着「OS 级缺位时应用内列表全平台行为一致」，即缺位是设计允许的形态。Linux 侧唯一沾边的是 XDG `recently-used.xbel`：它是 GTK 生态共读的一个用户态 XML 文件，**不是操作系统提供的机制**——写它要自己实现跨进程合并语义与文件锁（别的应用随时在写同一个文件），写坏会污染用户其他应用的最近列表；而收益只是「GTK 应用的文件选择器里多显示几条」。以这个代价换这个收益不成立，且与既有「Linux 无 OS 级机制 ⇒ 能力位 `false` + 文档申报」的口径一致。定案：Linux 侧零代码、能力位 `false`、应用内列表照常工作。
4. **macOS 端有与菜单栏同源的前置阻塞。** macOS 后端窗口壳未实写、事件泵未接，`NSDocumentController` 属 AppKit 且需要 `NSApplication` 存在。桥代码可以先落并被机制层证到，但真机验收依赖 macOS 后端实写，判据同样拆「机制层无头可证」与「真机段留白」两档。
5. **不因为桥接而放宽路径口径。** store 只做字符串级存取（不校验存在性、不规范化相对路径），投影沿用同一口径：桥只透传，**不替应用判断文件在不在、也不把相对路径猜成绝对路径**。相对路径推给 OS 没有意义但也不报错（与 store 的「不抛错、不清除用户数据」同款），由应用在 `add` 前自行规范化——store 一旦替应用猜，猜错的代价是一条错记录留在系统列表里且无从撤销。

## 变更内容

1. **能力位**：`PlatformCapabilities` 增 `recent_documents_os_projection`，机制存在性语义（与 `native_menu_bar`、`system_drag_in` 同款：只答机制在不在，运行期不可用走结构化错误）。`Win32` 与 `D3D11` 为 `true`、`MacOS` 为 `true`、`X11` / `Wayland` / `Glfw` / `Wasm` / `Headless` 为 `false`。
2. **公共入口（同址扩充，不新开门面）**：`include/aurora/app/recent_documents.h` 的 `RecentDocuments` 增 `project_to_os()`（返回 `Result<void>`，把当前 `list()` 推送给 OS）与 `clear_os_projection()`（清 OS 侧由本应用贡献的记录，返回 `Result<void>`）。二者均与 `sync()` 同款显式语义，不在 `add` / `clear` 内隐式触发。
3. **折算层（零平台头）**：`src/aurora/window/detail/` 新增 `recent_documents_translate.{h,cpp}`，把 `list()` 摊成平台无关的**投影计划**：丢弃空串条目、按 capacities 给出的平台上限截断（新 → 旧优先）、去重保持首次出现序。这一层不引任何平台头、可无头逐值单测，两枚平台桥共用同一份计划，不存在两套次序。**不在这层做路径规范化**（见动机第 5 条）。
4. **Win32 桥**：同目录新增 `win32_recent_documents.{h,cpp}`——经 `SHAddToRecentDocs`（`SHARD_PATHW`）逐条推送；清空走同 API 的空参形态。头文件经平台桥内部包含、**不泄漏到公共面**；`shell32` 已在本库 Windows 链接面内，无新增依赖。桥实例由 `Win32Host::Impl` 唯一持有，与既有四座桥同款纪律。
5. **macOS 桥**：同目录新增 `macos_recent_documents.{h,cpp}`（沿用 `macos_drag` 的混编形态）——经 `NSDocumentController` 的 `noteNewRecentDocumentURL:` 逐条推送。AppKit 不可用或应用对象缺失时返回错误并记 WARN 降级，**不谎报成功**。
6. **Linux**：零代码。能力位 `false`，规格申报「无 OS 级机制，应用内列表全平台一致」。
7. **错误码**：`codespec/errors.toml` 新增 `recent-documents-projection-unsupported`（平台无此机制，能力缺位而非故障），`retryable = false`。平台侧真实失败复用既有结构化错误通道，不新增一枚只会被命中一次的码。
8. **失败不影响应用内列表**：投影失败时 store 的内存与 `Preferences` 状态**已按 `add` 的语义成立**，投影只是没推送出去；返回错误并由调用方决定是否重试。这条要写进文档——否则调用方会误以为 `project_to_os()` 失败等于 `add` 没生效。
9. **测试与真机接缝**：`tests/unit/` 新增 `utest_recent_documents_translate.cpp`（折算层，无头）与能力位矩阵用例；`tools/verify/` 新增 `win32_recent_documents_live_probe.cpp`（`AURORA_BUILD_VERIFY_TOOLS` 口径、`EXCLUDE_FROM_ALL`、不进 CTest），显式登记到 `cmake/AuroraVerify.cmake`。macOS 真机段随窗口壳实写后补，本批不留占位文件。
10. **不落本批**：Windows 自定义 JumpList（`ICustomDestinationList` 的 Tasks 类别与自定义目标，属应用自定义内容而非「最近文档」语义）、macOS 的 Dock 菜单定制、Linux 的 `recently-used.xbel` 写入。

## 验收判据

1. **折算层纯逻辑单测**：空串条目被丢弃；超上限时按新 → 旧截断且尾部先丢；重复路径保持首次出现序；空列表产出空计划（不是错误）。**变异自证**：把截断方向改成保留最旧，或去掉空串过滤，相关用例必须转红。
2. **显式性单测**：`add` / `clear` 之后 OS 侧推送计数为 0（未隐式触发）；`project_to_os()` 之后计数与计划条目数一致。**变异自证**：把 `add` 改成自动投影，用例必须转红。
3. **能力位矩阵单测**：逐 `SurfaceKind` 断言 `recent_documents_os_projection` 的期望值（Win32 / D3D11 / MacOS 为 true，其余为 false）。**变异自证**：为未落地方向翻位，对应用例转红。
4. **失败隔离单测**：桥返回失败时，`list()` 仍返回 `add` 进去的条目、`size()` 不变；错误码为 `recent-documents-projection-unsupported` 或平台侧结构化错误，二者不混淆。
5. **路径口径单测**：相对路径与磁盘上不存在的路径照常进入计划（不做规范化、不校验存在性）；空串被过滤——两条是不同的处置，用例须分别断言而不是合并成一条。
6. **Win32 真机探针**：真实推送后系统侧最近文档出现该条目；再次 `project_to_os()` 幂等（不重复堆积）；`clear_os_projection()` 后本应用贡献的记录消失。
7. **macOS 机制层**：URL 构造与 `NSDocumentController` 调用序列在无窗口环境下逐值可证；真机段在窗口壳实写前按留白登记，不写推断当事实。
8. **回归与门禁**：`utest_recent_documents` / `utest_preferences` / `utest_scroll_storage` 零改动全绿；`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移、`check_api_budget` 增量通过；`check_naming_conventions` / `check_doc_comments` / `check_platform_macros` / `check_core_layer_boundary` / `check_no_hardcoded_paths` 全绿；`lint`（仅改动相关 TU）与 `format-check` 绿；全量 `ctest -j16` 绿。

## 回写落点

- `codespec/specification/06-app-platform.md`（§5 能力位表 + §8.10 最近文档：OS 级投影的能力位、显式推送契约、折算层与计划语义、Win32 桥与撤销形态、失败隔离口径、Linux / macOS 能力位申报）
- `codespec/SPECIFICATIONS.md`（需求行补记：OS 级投影的落位与 Linux / macOS 缺位时的应用内一致口径）
- `codespec/BUILD_OPTIONS.md` §2.4（新增真机探针的产物登记）

## 实施记录与偏离（落码期回写）

1. **macOS 桥本批未交付 ⇒ 能力位 `MacOS` 落为 `false`**（与「变更内容」第 1 条的 `true` 有意偏离，交付范围经用户拍板收在 Win32）。理由即本提案自己的口径：未接线的方向不预先声称，否则 `project_to_os()` 在 macOS 上静默失败且调用方无从降级。「变更内容」第 5 条（macOS 桥）与「验收判据」第 7 条（macOS 机制层）随之留待后续批次，本批不留占位文件。
2. **「清空走同 API 的空参形态」实测不成立**：本机 MinGW SDK 的 `SHAddToRecentDocs` 只有 `SHARD_PIDL` / `SHARD_PATHA` / `SHARD_PATHW` 三个 flag、返回 `void`，既无清空标志也无逐条撤销 API。撤销的落地形态改为：桥维护**本进程投影记录**，清空时枚举 `%APPDATA%\Microsoft\Windows\Recent\*.lnk`、经 `IShellLinkW` 解析目标后只删命中记录的那些。两条边界如实写进桥头注与规格（共享记录、注册表 MRU 不碰）。
3. **宿主持有形态由「唯一持有」调整为「引用计数守卫 + 无宿主按需自举」**（经用户拍板）：通路本身与 `HWND` 无关（进程级调用），每个宿主各持一份守卫、先关的窗口不拆仍在世窗口的通路；无窗口进程（纯命令行应用）也能投影，判据由真机探针（不建窗口）覆盖。
4. **三条实测平台事实**（都写进桥头注与规格，不藏在代码里）：投递无回执（成功只表「已投给 OS」）；快捷方式的实际写出**晚于** API 返回（毫秒级迟到，故撤销只销「确认删掉了」的账，真机判据须有界轮询）；`Start_TrackDocs=0` 时 API 静默无操作，桥读该键作为可证旁证、返回 `platform-unavailable` 而非谎报成功。附带一条取证事实：壳把 `%TEMP%` 排除在最近文档之外，探针文件因此建在 `%LOCALAPPDATA%\aurora_verify`。
5. **验收判据兑现情况**：第 1—6 条与第 8 条已兑现（无头单测 12 例 + 真机探针自动段全绿，第 6 条的「条目出现 / 幂等 / 撤销后消失」由探针自带的独立取证证明）；第 7 条随 macOS 桥一并留待后续批次。
6. **变异自证已实跑两条**（判据第 6 项要求的正是这两条）：把折算层的截断方向改成「保留最旧」⇒ `plan_truncates_from_the_oldest_tail` 等 5 例转红；摘掉空串过滤 ⇒ `plan_drops_empty_entries` 转红。两次改坏后均已还原并复跑回绿（11 通过 / 1 如实 SKIP），用例不是空转。
7. **后续批次已补上 macOS 桥并归档（覆盖第 1 条的 macOS 部分）**：`src/aurora/window/detail/macos_recent_documents.{h,cpp}` 把折算后的计划逐条交给 `NSDocumentController noteNewRecentDocumentURL:`，撤销走 `clearRecentDocuments:`；`PlatformCapabilities::recent_documents_os_projection` 的 `MacOS` 由 `false` 翻为 `true`（机制在且桥已接）。三条实测口径写进规格与桥头注：**① 只在 `.app` 包内生效**——裸可执行文件调该 API 不落记录，桥逐条回读 `recentDocumentURLs` 核对，未记录即报 `platform-unavailable` 并点名需要 app bundle，不谎报成功；**② 投递无回执**，成功的确切含义是「已交给 `NSDocumentController` 且回读可见」；**③ 撤销的作用域是整个应用**（AppKit 无逐条移除 API），与 Win32 桥只删本进程记录的 `.lnk` 的窄语义不同。判据第 7 条（macOS 机制层）由 `aurora_verify_macos_recent_documents` 在本机经临时 `.app` 包跑通兑现：17/17 项断言全过（投影后 OS 列表出现、重复投影幂等、撤销后清空、store 不被触碰）。
