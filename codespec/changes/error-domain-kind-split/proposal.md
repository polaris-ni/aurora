# CHG-024 错误码模型重构：`ErrorCategory` 拆为 `ErrorDomain` + `ErrorKind`，移除 `FixSuggestion`

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-024 |
| 提出日期 | 2026-10-28 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.QUALITY.CORE.STRUCTURED-ERROR.001 |
| 影响面 | 数据源 `codespec/errors.toml`（157 条目由单列 `category` 改为显式声明 `domain` + `kind` 两列，并按域序重排）；生成器 `tools/gen/gen_error_codes.cpp`（`ErrorEntry` 两字段、`AURORA_DOMAINS`/`AURORA_KINDS` 与 DOC 数组、`ErrorDomain`/`ErrorKind` 两枚举、`ErrorMeta` 两字段、`domain()`/`kind()` 两查表、`to_string` 两个重载、目录与 `aurora_api.json` 各增一列、取值不在表内一律 `return 1`、校验首行恒 `GeneralUnknown`）；三件生成物 `include/aurora/core/error_codes.gen.h` / `codespec/ERROR_CATALOG.md` / `aurora_api.json`（error_codes 段）；`include/aurora/core/result.h` 与 `src/aurora/core/result.cpp`（`Error::category` → `domain` + `kind`，`make_error_from_table` 与 `to_json()` 同步）；`include/aurora/core/diagnostics.h` 与 `src/aurora/core/diagnostics.cpp`（删 `FixSuggestion`/`Diagnostic::fix`/`collect_fixes()`/废弃 `DiagnosticCategory`、`category` → `domain` + `kind`、`category_str()` → `domain_str()` + `kind_str()`、`report` 去第 5 形参）；`src/aurora/debug/debug_runtime.cpp` 与其头 `include/aurora/debug/debug_runtime.h`（`/api/diagnostics` 输出键与 `fix` 块、快照 JSON 字段注释）；`src/aurora/inspector/inspector_api.cpp`（`d.category` 初值）；头注释 `include/aurora/app/clipboard.h` / `include/aurora/net/http.h` / `include/aurora/webview/web_view.h` / `src/aurora/net/tls_backend.cpp`；测试 `tests/unit/utest_error_codes.cpp` / `utest_result.cpp` / `utest_diagnostics.cpp` / `utest_debug_runtime.cpp`（`/api/diagnostics` 逐条键面补测）/ `utest_cli.cpp`（原断言 `error.category == ErrorCategory::Validation`，须改判 `domain == ErrorDomain::Cli`）/ `utest_cli_format.cpp`（原 `result.error().category == ErrorCategory::Validation`，同理）/ `examples/demos/demo_core.cpp`（`item.category_str()` → `domain_str()`+`kind_str()`；「category 前缀」是日志前缀表述，豁免）；文档 `ARCHITECTURE.md` §12.3、`specification/01-core.md` §3.1/§4.1/§4.2、`specification/06-app-platform.md` §8.8.22 及若干「category io」表述、`specification/08-tooling.md`、`specification/09-cli.md`、`SPECIFICATIONS.md`、`manual-test/01-core.md`、`manual-test/19-app.md`、`CHANGELOG.md` [Unreleased] 的 `Breaking`/`Migration`。无新增文件（除本提案）。**不在本次范围**：`Error::suggestion`（CLI did-you-mean 与 `validate_ui` 的调用点特化建议，与 `hint` 是「调用点特化 vs 表驱动默认」的分工，保留）、`Logger` 的 `category` 形参（与错误元数据无关，属日志前缀）、`HttpErrorClass`（答「哪一跳」与「状态码即失败」，与 `domain × kind` 的「域 vs 故障性质」正交，保留）、错误链 / 根因字段（新增能力，另案）。 |

## 动机

1. **`ErrorCategory` 语义错位（精确命名原则）**：一枚枚举同时承载「模块归属」与「故障性质」两个正交概念。重构后模块归属迁往 `ErrorDomain`，`category` 只剩超泛的「某个分组」空壳——既不再贴「模块」、也不拥有独立语义，按精确命名原则应改名换义为 `ErrorKind`（故障性质），与库内 `PlatformKind`/`RouteKind`/`WsMessageKind`/`LengthKind`/`AccessibilityEventKind` 等 `XxxKind` 惯例统一。
2. **判别力失效（巨桶实证）**：157 条里 `io`(67)/`platform`(34)/`validation`(29) 三桶合计 130 条、占 83%。`io` 下混入 net(34)/http(9)/ws(4)/json(4)/compression(3)/storage(3)；`platform` 下混入 webview(7)/system(6)/clipboard(5)/prefs(4)；`validation` 下混入 cli(12)/audio(4)。一旦模块归属独立成 `domain`，`io` 桶自然拆解，`domain` 成为判别力强的路由信号。重构后最大性质桶 `io`(35)/`invalid`(33) 各约 21%，对比现状 43%。
3. **文档已自认分类不足**：`06-app-platform.md` §8.8.22 明写「之所以不直接用生成表的 `ErrorCategory`：那是全库粗分类，本族 47 枚全在 `io` 下，分不出『名字解析不了』与『对端把字节写坏了』」——库自己在 net 域另造 `HttpErrorClass` 归因档来绕开这个粗分类。把「业务域」上升为一等元数据后，net 体能按 `domain=net` 精确聚合，`HttpErrorClass` 与 `ErrorDomain::Net` 各司其职（域 vs 归因档），不再需要靠分类越权。
4. **同域跨类暴露指派随意**：`audio` 7 条被拆进 io/platform/validation/runtime 四类、`storage` 拆 io+validation 两类、`icon` 拆 render/widget/validation 三类。`category` 连「内部一致」都做不到，说明它是加条目时顺手贴的标签而非设计。本提案的核心产物是下方 **157 行逐条映射表，每行带「判型依据」列**（点名该枚错误该归哪一档、为何不归相邻档）——不给依据的表，仍然只是「顺手贴的标签」。判型按固定优先级次序「命中即止」（见 `ErrorKind` 清单），从机制上保证内部一致。
5. **业务域无机器可读载体**：真实业务域（net/http/ws/webview/audio/cli/…约 30 个）只以 slug 前缀的隐形形态存在，消费方要按域聚合只能退化为字符串操作。而 slug 前缀推导不可靠：`widget-not-restorable` 前缀是 `not`、`recent-documents` 前缀是 `recent`、`system-drag` 前缀是 `system`——字符级规则推不出语义域。故 `domain` 必须**显式声明**于 `errors.toml`，不做推导（单一权威源）。
6. **`FixSuggestion` 是 CHG-021 遗留的展示壳**：CHG-021 已删全部可执行机制并写明「建议不再要求可执行，上报即可见」，剩下 `{code, description}` 两枚字符串挂 `Diagnostic::fix` 随 JSON 上报。其作用只剩两件事：(a) 一句补救建议——与 `ErrorMeta::hint`（`explain_diagnostic()` 已表驱动出码）职责完全重叠；(b) 一枚展示标签 `fix.code`——与 `Diagnostic::code`（错误 slug）同名撞语义。既然不执行、又无独立信息，删除后「只向用户/AI 输出错误信息」，修复指引单一由 `hint` 承担，JSON 行里的 `fix_code`/`fix_desc` 一并下线。
7. **AI-first 路由信号失效**：经验库 `ai-first-error-diagnostics-layers` 要求路由信号机器可读、判别力强。现状 `category` 判别力≈0 时，AI 只能退化为读 `message_tpl` 的 `{detail}` 猜域；`domain × kind` 二维后路由信号可精确化（如 `domain=net && kind=Timeout`→建议重试/查防火墙，`domain=net && kind=Corrupt`→查对端形态），全程不碰文本。
8. **破坏性窗口与先例**：alpha 预览版不构成稳定性承诺（`SPECIFICATIONS.md` §1），`ErrorCategory` 重定义、`Error` 输出键改名、`FixSuggestion` 移除均按「直接删除/重定义」而非先 `[[deprecated]]` 过渡（CHG-021 同口径）。数值是进程内行号而非外部契约（冻结契约是 `slug`），枚举值随按 domain 重排允许变化，跨进程零破坏。

## 目标模型

一枚枚举拆两枚正交枚举，第三条（`FixSuggestion`）直接删除：

| 维度 | 目标 | 回答的问题 | 值的域 |
|:---|:---|:---|:---|
| 模块归属 | `ErrorDomain`（新） | 「该去哪个子系统找」 | 29 个业务域 |
| 故障性质 | `ErrorKind`（由 `category` 改名换义） | 「坏在哪一层、该做什么动作」 | 12 档 |
| 补救指引 | `ErrorMeta::hint`（唯一） | 「怎么修」 | 自由文本 |

`FixSuggestion` 的 `{code, description}` 与 `hint` 职责重叠、`fix.code` 又与 `Diagnostic::code` 撞语义，删除后 JSON 行的 `fix_code` / `fix_desc` 一并下线。

## `ErrorDomain` 清单（29，显式声明于 `errors.toml`）

- **`general`** (3) — 跨模块兜底域（未归入具体子系统）
- **`layout`** (4) — 布局测量与约束
- **`widget`** (6) — 控件契约与属性
- **`render`** (6) — 绘制、字体与位图
- **`navigation`** (2) — 页面栈与路由
- **`validation`** (5) — 输入与配置校验
- **`generation`** (1) — UI 生成（generate_ui）
- **`runtime`** (3) — 运行时环境与生命周期状态
- **`io`** (5) — 文件与字节流读写（通用）
- **`json`** (4) — JSON 解析/序列化
- **`compression`** (3) — 压缩编解码
- **`storage`** (6) — 键值存储后端与记录
- **`i18n`** (8) — 国际化消息目录
- **`prefs`** (4) — 配置持久化
- **`icon`** (3) — 图标资源（名/字形/SVG）
- **`platform`** (4) — 平台后端与 OS 集成（通用）
- **`window`** (1) — 窗口生命周期（Surface/设备）
- **`dialog`** (1) — 平台对话框创建
- **`clipboard`** (5) — 系统剪贴板
- **`drag`** (6) — 系统拖放
- **`menu`** (2) — 原生菜单栏
- **`notification`** (1) — 系统通知
- **`recent-documents`** (1) — 系统最近文档投影
- **`webview`** (7) — 原生 Web 视图（交互/快照）
- **`audio`** (7) — 音频图与设备
- **`cli`** (12) — 命令行参数解析
- **`net`** (34) — 网络 socket 统一层
- **`http`** (9) — HTTP 客户端
- **`ws`** (4) — WebSocket

## `ErrorKind` 清单（12，含判型优先级）

判型按**命中即止**的固定次序（即下方编号顺序，写入 `errors.toml` 头注）：先判「输入/声明本身是否非法」，再判「名字合法但目标不存在」「本环境无此能力」「依赖此刻不可用」「被权限/策略拒绝」「两份合法状态互斥」「触顶限额」「超时」「被取消」「对端/输入字节形态不符」「底层动作失败」，最后兜底 `internal`。该次序是动机 4「内部一致」的机制性解法——凡判型边界条目（如 `ws-handshake-rejected` 究属 `denied` 还是 `corrupt`）一律落到此固定次序，不再由加条目的人随手定档。

1. **`invalid`** — 实参/声明不合语法或越界
2. **`not-found`** — 名字合法但注册表/目录/路由无此条目
3. **`unsupported`** — 本构建/平台/实现无该能力或格式
4. **`unavailable`** — 依赖存在但此刻不可用（未就绪/已关闭/被移除/降级兜底）
5. **`denied`** — 权限/策略/凭据被拒
6. **`conflict`** — 两份各自合法的声明或状态互斥（环/重名/重放/体形态组合）
7. **`exhausted`** — 限额/容量/深度/队列/链长触顶
8. **`timeout`** — 操作未在时限内完成
9. **`cancelled`** — 调用方/属主取消操作
10. **`corrupt`** — 不可信输入或对端字节形态不符（解析失败/magic/版本/截断/协议违约）
11. **`io`** — 底层原语动作失败（落盘/send/recv/建连/监听/绑定/系统调用）
12. **`internal`** — 不变量破裂/未预期异常/无法归类

> **命名注记**：「底层原语动作失败」这一档的 kebab 值取 `io`、C++ 枚举名 `ErrorKind::IO`（用户于 2026-10-28 拍板，覆盖原拟的 `IoFailed`）。`ErrorDomain` 另有 `io` 域（`ErrorDomain::Io`），二者分属两个枚举、JSON 键分别为 `kind` 与 `domain`，字符串同形不撞语义：`domain="io"` 读作「通用文件/字节流域」，`kind="io"` 读作「底层原语动作失败」，如 `storage-io-error` 应读作「storage 域 + io 性质」，不是「storage ⊆ io」。

## 逐条映射表（157 行，含判型依据）

下表是动机 4 的正解：每行点名该枚错误归属的 `(domain, kind)` 与判型依据。生成器对 `domain`/`kind` 取值做白名单硬校验（越界即 `return 1`、不写产物），故本表即单一权威源。

| # | domain | kind | enum | slug | severity | retryable | 判型依据 |
|---|---|---|------|------|----------|-----------|----------|
| 1 | `general` | `internal` | `GeneralUnknown` | `general-unknown` | error | false | 兜底哨兵，不命中任何具体档，归 internal。 |
| 2 | `general` | `invalid` | `GeneralInvalidArgument` | `general-invalid-argument` | error | false | 实参整体不合法。 |
| 3 | `general` | `unsupported` | `GeneralNotSupported` | `general-not-supported` | error | false | 本库/本平台不支持该操作。 |
| 4 | `layout` | `invalid` | `LayoutNullChild` | `layout-null-child` | warning | false | 布局子节点为空，输入非法。 |
| 5 | `layout` | `exhausted` | `LayoutDepthExceeded` | `layout-depth-exceeded` | warning | false | 树深触顶默认上限。 |
| 6 | `layout` | `invalid` | `LayoutInvalidConstraints` | `layout-invalid-constraints` | warning | false | 约束自相矛盾（min>max）。 |
| 7 | `layout` | `invalid` | `LayoutSizeOutOfConstraints` | `layout-size-out-of-constraints` | warning | false | 算出尺寸越界，输入/约束非法。 |
| 8 | `widget` | `not-found` | `WidgetUnknownType` | `widget-unknown-type` | warning | false | 反序列化遇到未注册 widget 类型（注册表无此名）。 |
| 9 | `widget` | `invalid` | `WidgetInvalidProp` | `widget-invalid-prop` | warning | false | 属性值类型/范围非法。 |
| 10 | `widget` | `conflict` | `WidgetPropConstraintViolated` | `widget-prop-constraint-violated` | warning | false | 互斥属性同时设置。 |
| 11 | `widget` | `invalid` | `WidgetMissingRequiredProp` | `widget-missing-required-prop` | warning | false | 缺必填属性，输入不完整（非法）。 |
| 12 | `widget` | `exhausted` | `WidgetDepthExceeded` | `widget-depth-exceeded` | warning | false | widget 树深触顶 64。 |
| 13 | `widget` | `unsupported` | `NotRestorable` | `not-restorable` | warning | false | 该 widget 类型不支持序列化恢复。 |
| 14 | `render` | `io` | `RenderFontLoadFailed` | `render-font-load-failed` | error | false | 字体文件读取/加载原语失败。 |
| 15 | `render` | `io` | `RenderImageLoadFailed` | `render-image-load-failed` | error | false | 图像文件读取/加载原语失败。 |
| 16 | `render` | `invalid` | `RenderInvalidColor` | `render-invalid-color` | error | false | 颜色分量越界，输入非法。 |
| 17 | `render` | `unavailable` | `RendererUnavailable` | `renderer-unavailable` | error | false | 请求渲染后端不可用。 |
| 18 | `render` | `unavailable` | `RenderDegraded` | `render-degraded` | warning | false | 缺资源降级（字体/图像兜底），资源当前不可用。 |
| 19 | `render` | `unavailable` | `FontMissing` | `font-missing` | warning | false | 请求字体缺失，回退内置位图（资源不可用）。 |
| 20 | `navigation` | `exhausted` | `NavDepthExceeded` | `nav-depth-exceeded` | error | false | 导航栈深触顶。 |
| 21 | `navigation` | `not-found` | `NavRouteNotFound` | `nav-route-not-found` | error | false | 路由未注册（表中无）。 |
| 22 | `validation` | `invalid` | `ValidationFailed` | `validation-failed` | warning | false | 通用校验不通过，输入非法。 |
| 23 | `validation` | `invalid` | `ValidationSchemaMismatch` | `validation-schema-mismatch` | warning | false | 结构与 Schema 不符，输入非法。 |
| 24 | `validation` | `exhausted` | `ValidationTreeTooDeep` | `validation-tree-too-deep` | warning | false | UI 树深触顶默认上限。 |
| 25 | `validation` | `not-found` | `ValidationUnknownWidget` | `validation-unknown-widget` | warning | false | 未知/不支持的 widget 类型名（注册表无）。 |
| 26 | `validation` | `invalid` | `ValidationNullChild` | `validation-null-child` | warning | false | 子节点为空，输入非法。 |
| 27 | `generation` | `invalid` | `GenerateUiEmpty` | `generation-ui-empty` | error | false | 生成结果为空（无有效 widget），产出非法。 |
| 28 | `runtime` | `internal` | `RuntimeCoroutineException` | `runtime-coroutine-exception` | error | true | 协程体抛未捕获异常（不变量破裂）。 |
| 29 | `runtime` | `internal` | `RuntimeAsyncException` | `runtime-async-exception` | error | true | 异步回调抛未捕获异常。 |
| 30 | `runtime` | `timeout` | `RuntimeAsyncTimeout` | `async-timeout` | error | true | 异步任务未在时限内完成。 |
| 31 | `io` | `not-found` | `IOFileNotFound` | `io-file-not-found` | error | false | 路径在文件系统中不存在。 |
| 32 | `io` | `corrupt` | `IOParseFailed` | `io-parse-failed` | error | false | 文件内容不符合格式（不可信输入）。 |
| 33 | `io` | `corrupt` | `IOImageDecodeFailed` | `io-image-decode-failed` | error | false | 图像字节形态不符（解码失败）。 |
| 34 | `io` | `invalid` | `IOImageInvalidDimensions` | `io-image-invalid-dimensions` | error | false | 宽高为 0 或超界，输入非法。 |
| 35 | `io` | `io` | `IOImageEncodeFailed` | `io-image-encode-failed` | error | false | 编码原语动作失败。 |
| 36 | `json` | `corrupt` | `JsonParseError` | `json-parse-error` | error | false | JSON 字节形态不符（解析失败）。 |
| 37 | `json` | `exhausted` | `JsonDepthExceeded` | `json-depth-exceeded` | error | false | 嵌套深触顶 max_depth。 |
| 38 | `json` | `invalid` | `JsonTypeMismatch` | `json-type-mismatch` | error | false | 期望类型与实际不符，输入非法。 |
| 39 | `json` | `invalid` | `JsonValueNotSerializable` | `json-value-not-serializable` | error | false | 值无法序列化为 JSON（NaN/Inf），非法。 |
| 40 | `compression` | `corrupt` | `CompressionDataCorrupt` | `compression-data-corrupt` | error | false | 压缩流损坏/截断（不可信字节）。 |
| 41 | `compression` | `exhausted` | `CompressionOutputTooLarge` | `compression-output-too-large` | error | false | 解压输出触顶 max_output_bytes（容量护栏）。 |
| 42 | `compression` | `unavailable` | `CompressionUnavailable` | `compression-unavailable` | error | false | 无对应编解码器或构建未开压缩。 |
| 43 | `storage` | `unavailable` | `StorageBackendUnavailable` | `storage-backend-unavailable` | error | false | 存储后端（目录/介质）不可用。 |
| 44 | `storage` | `not-found` | `StorageRecordNotFound` | `storage-record-not-found` | error | false | 存储记录 id 不存在。 |
| 45 | `storage` | `corrupt` | `StorageRecordCorrupt` | `storage-record-corrupt` | error | false | 记录 JSON 解析失败或结构非法（不可信输入）。 |
| 46 | `storage` | `invalid` | `StorageTypeMismatch` | `storage-type-mismatch` | error | false | 类型化读写类型不一致，输入非法。 |
| 47 | `storage` | `invalid` | `StorageEncodingMismatch` | `storage-encoding-mismatch` | error | false | 序列化线格式不一致，输入非法。 |
| 48 | `storage` | `io` | `StorageIoError` | `storage-io-error` | error | false | 存储底层 I/O 原语失败。 |
| 49 | `i18n` | `corrupt` | `I18nBinMagicFailed` | `i18n-bin-magic` | error | false | 文件 magic 不符 AUM1（损坏/错文件）。 |
| 50 | `i18n` | `unsupported` | `I18nBinVersionUnsupported` | `i18n-bin-version` | error | false | 目录版本高于库支持的 1。 |
| 51 | `i18n` | `corrupt` | `I18nBinEndiannessInvalid` | `i18n-bin-endianness` | error | false | 非小端（字节错位）。 |
| 52 | `i18n` | `corrupt` | `I18nBinTruncated` | `i18n-bin-truncated` | error | false | 文件短于自身头部声明（截断）。 |
| 53 | `i18n` | `invalid` | `I18nIdOutOfRange` | `i18n-id-out-of-range` | error | false | id 空间撞哨兵或溢出，声明非法。 |
| 54 | `i18n` | `conflict` | `I18nVocabularyOverlap` | `i18n-vocabulary-overlap` | error | false | 两目录 id 区间重叠（互斥）。 |
| 55 | `i18n` | `conflict` | `I18nNameDuplicate` | `i18n-name-duplicate` | error | false | 全局名被另一目录占用（互斥）。 |
| 56 | `i18n` | `not-found` | `I18nUnknownName` | `i18n-unknown-name` | error | false | 名不在任何已注册词表中（表中无）。 |
| 57 | `prefs` | `unsupported` | `PrefsNotPersistent` | `prefs-not-persistent` | error | false | 内存模式不支持持久化（能力缺失）。 |
| 58 | `prefs` | `io` | `PrefsOpenFailed` | `prefs-open-failed` | error | false | 配置文件打开原语失败。 |
| 59 | `prefs` | `corrupt` | `PrefsParseFailed` | `prefs-parse-failed` | error | false | 配置 JSON 解析失败（不可信输入）。 |
| 60 | `prefs` | `io` | `PrefsWriteFailed` | `prefs-write-failed` | error | false | 配置文件写入原语失败。 |
| 61 | `icon` | `not-found` | `IconNameNotRegistered` | `icon-name-not-registered` | error | false | 语义名未在 IconRegistry 注册（表中无）。 |
| 62 | `icon` | `unavailable` | `IconGlyphUnavailable` | `icon-glyph-unavailable` | error | false | 字体未注册或码点不存在（字形资源不可用）。 |
| 63 | `icon` | `unsupported` | `IconSvgUnsupported` | `icon-svg-unsupported` | warning | false | 内置光栅器只覆盖 SVG 子集。 |
| 64 | `platform` | `unavailable` | `PlatformUnavailable` | `platform-unavailable` | error | false | 平台特性当前不可用。 |
| 65 | `platform` | `denied` | `PlatformPermissionDenied` | `platform-permission-denied` | error | false | 权限/策略不足。 |
| 66 | `platform` | `io` | `PlatformComInitFailed` | `platform-com-init-failed` | error | false | COM 初始化系统调用失败。 |
| 67 | `platform` | `io` | `OsHotkeyRegisterFailed` | `os-hotkey-register-failed` | error | false | 全局热键注册系统调用失败。 |
| 68 | `window` | `unavailable` | `SurfaceLost` | `surface-lost` | warning | false | 窗口销毁/设备重置，Surface 当前不可用。 |
| 69 | `dialog` | `io` | `PlatformDialogCreateFailed` | `platform-dialog-create-failed` | error | false | 平台对话框创建原语失败。 |
| 70 | `clipboard` | `unavailable` | `ClipboardAccessFailed` | `clipboard-access-failed` | error | true | 剪贴板当前不可访问（被占用/缺 helper）。 |
| 71 | `clipboard` | `io` | `ClipboardWriteFailed` | `clipboard-write-failed` | error | false | 剪贴板提交原语失败。 |
| 72 | `clipboard` | `unsupported` | `ClipboardFormatUnsupported` | `clipboard-format-unsupported` | error | false | 本平台不暴露该 MIME 机制。 |
| 73 | `clipboard` | `io` | `ClipboardProviderFailed` | `clipboard-provider-failed` | error | false | 惰性 provider 回调失败（渲染原语失败）。 |
| 74 | `clipboard` | `unsupported` | `ClipboardListenUnsupported` | `clipboard-listen-unsupported` | error | false | 本平台无剪贴板变更监听机制。 |
| 75 | `drag` | `unsupported` | `SystemDragNotSupported` | `system-drag-not-supported` | error | false | 本平台/后端无系统拖放机制。 |
| 76 | `drag` | `io` | `SystemDragSessionFailed` | `system-drag-session-failed` | error | false | 拖放会话协商/取数原语失败。 |
| 77 | `drag` | `io` | `SystemDragReadFailed` | `system-drag-read-failed` | error | true | 跨进程拖放读取原语失败。 |
| 78 | `drag` | `corrupt` | `SystemDragDecodeFailed` | `system-drag-decode-failed` | error | false | 字节已到但不符合声明 MIME（数据缺陷）。 |
| 79 | `drag` | `cancelled` | `SystemDragCancelled` | `system-drag-cancelled` | info | false | 用户/系统取消拖放会话。 |
| 80 | `drag` | `io` | `SystemDragMoveRollback` | `system-drag-move-rollback` | error | true | 落点已存但源删除原语失败（回滚）。 |
| 81 | `menu` | `unsupported` | `NativeMenuUnsupported` | `native-menu-unsupported` | error | false | 本平台无 OS 级菜单栏机制。 |
| 82 | `menu` | `io` | `NativeMenuInstallFailed` | `native-menu-install-failed` | error | false | 菜单树挂接窗口原语失败。 |
| 83 | `notification` | `io` | `NotificationPostFailed` | `notification-post-failed` | error | false | 系统通知投递原语失败。 |
| 84 | `recent-documents` | `unsupported` | `RecentDocumentsProjectionUnsupported` | `recent-documents-projection-unsupported` | error | false | 本平台无最近文档投影机制。 |
| 85 | `webview` | `unavailable` | `WebViewRuntimeMissing` | `webview-runtime-missing` | warning | false | 无原生 Web 引擎运行时。 |
| 86 | `webview` | `io` | `WebViewEngineInitFailed` | `webview-engine-init-failed` | error | false | 引擎启动原语失败。 |
| 87 | `webview` | `io` | `WebViewNavigationFailed` | `webview-navigation-failed` | error | true | 导航动作失败（URI/可达性）。 |
| 88 | `webview` | `io` | `WebViewScriptEvalFailed` | `webview-script-eval-failed` | error | true | 脚本求值原语失败。 |
| 89 | `webview` | `unsupported` | `WebViewSnapshotUnsupported` | `webview-snapshot-unsupported` | warning | false | 快照模式本后端不支持。 |
| 90 | `webview` | `unavailable` | `WebViewHostUnbound` | `webview-host-unbound` | warning | false | 控件未绑定原生宿主（离屏/未装槽）。 |
| 91 | `webview` | `unsupported` | `WebViewUnsupportedMode` | `webview-unsupported-mode` | warning | false | 该模式本后端（Wayland/headless）不支持。 |
| 92 | `audio` | `conflict` | `AudioGraphCycle` | `audio-graph-cycle` | error | false | 连接会产生环（两条边互斥 DAG）。 |
| 93 | `audio` | `not-found` | `AudioEdgeNotFound` | `audio-edge-not-found` | error | false | 连接不存在（注册表/图中无）。 |
| 94 | `audio` | `unavailable` | `AudioContextClosed` | `audio-context-closed` | error | false | AudioContext 已关闭，操作不再可用。 |
| 95 | `audio` | `invalid` | `AudioParamInvalid` | `audio-param-invalid` | error | false | 参数自动化非法（时间/符号约束）。 |
| 96 | `audio` | `invalid` | `AudioBufferInvalid` | `audio-buffer-invalid` | error | false | 缓冲非法（采样率/通道/空）。 |
| 97 | `audio` | `unavailable` | `AudioDeviceUnavailable` | `audio-device-unavailable` | warning | true | 音频设备不可用，静默模式。 |
| 98 | `audio` | `io` | `AudioRecordingFailed` | `audio-recording-failed` | error | true | 录音原语动作失败。 |
| 99 | `cli` | `invalid` | `CliSpecInvalid` | `cli-spec-invalid` | error | false | 命令/选项 Schema 声明非法。 |
| 100 | `cli` | `not-found` | `CliUnknownOption` | `cli-unknown-option` | error | false | 选项名在 accepted 表中不存在。 |
| 101 | `cli` | `not-found` | `CliUnknownSubcommand` | `cli-unknown-subcommand` | error | false | 子命令名在表中不存在。 |
| 102 | `cli` | `invalid` | `CliMissingSubcommand` | `cli-missing-subcommand` | error | false | 缺必需子命令，输入不完整（非法）。 |
| 103 | `cli` | `invalid` | `CliMissingValue` | `cli-missing-value` | error | false | 选项缺值，输入不完整。 |
| 104 | `cli` | `invalid` | `CliInvalidValue` | `cli-invalid-value` | error | false | 值不符合 kind 语法，输入非法。 |
| 105 | `cli` | `invalid` | `CliChoiceInvalid` | `cli-choice-invalid` | error | false | 值不在允许集合，输入非法。 |
| 106 | `cli` | `invalid` | `CliRangeViolated` | `cli-range-violated` | error | false | 值越界，输入非法。 |
| 107 | `cli` | `invalid` | `CliArityViolated` | `cli-arity-violated` | error | false | 重复/令牌数不符声明，输入非法。 |
| 108 | `cli` | `invalid` | `CliMissingRequired` | `cli-missing-required` | error | false | 缺必需选项，输入不完整。 |
| 109 | `cli` | `invalid` | `CliTooManyPositionals` | `cli-too-many-positionals` | error | false | 位置参数过多，输入非法。 |
| 110 | `cli` | `conflict` | `CliConflictViolated` | `cli-conflict-violated` | error | false | 互斥选项同时给出。 |
| 111 | `net` | `io` | `NetRuntimeFailed` | `net-runtime-failed` | error | true | Winsock 启动原语失败。 |
| 112 | `net` | `io` | `NetSocketFailed` | `net-socket-failed` | error | true | socket() 原语失败。 |
| 113 | `net` | `io` | `NetBindFailed` | `net-bind-failed` | error | true | bind() 原语失败。 |
| 114 | `net` | `io` | `NetListenFailed` | `net-listen-failed` | error | true | listen() 原语失败。 |
| 115 | `net` | `io` | `NetAcceptFailed` | `net-accept-failed` | error | true | accept() 原语失败。 |
| 116 | `net` | `io` | `NetConnectFailed` | `net-connect-failed` | error | true | connect() 原语失败。 |
| 117 | `net` | `io` | `NetTimeoutOptionFailed` | `net-timeout-option-failed` | error | false | setsockopt(SO_RCVTIMEO/SNDTIMEO) 原语失败。 |
| 118 | `net` | `io` | `NetWaitFailed` | `net-wait-failed` | error | true | select() 就绪等待原语失败。 |
| 119 | `net` | `io` | `NetSendFailed` | `net-send-failed` | error | false | send() 原语失败（对端关闭/超时）。 |
| 120 | `net` | `io` | `NetRecvFailed` | `net-recv-failed` | error | false | recv() 原语失败（对端重置/关闭）。 |
| 121 | `net` | `timeout` | `NetRecvTimeout` | `net-recv-timeout` | warning | true | SO_RCVTIMEO 在收到字节前耗尽。 |
| 122 | `net` | `invalid` | `NetEndpointInvalid` | `net-endpoint-invalid` | error | false | 端点地址语法非法。 |
| 123 | `net` | `cancelled` | `NetCancelled` | `net-cancelled` | info | true | 调用方/属主取消操作。 |
| 124 | `net` | `unavailable` | `NetPlatformUnavailable` | `net-platform-unavailable` | error | false | 当前平台无该网络能力。 |
| 125 | `net` | `invalid` | `NetUrlInvalid` | `net-url-invalid` | error | false | URL 语法/可解析性非法。 |
| 126 | `net` | `invalid` | `NetProxyConfigInvalid` | `net-proxy-config-invalid` | error | false | 代理配置语法非法。 |
| 127 | `net` | `unsupported` | `NetProxyUnsupported` | `net-proxy-unsupported` | error | false | 当前构建不支持该代理协议。 |
| 128 | `net` | `io` | `NetProxyUnreachable` | `net-proxy-unreachable` | error | true | 代理隧道建立原语失败。 |
| 129 | `net` | `denied` | `NetProxyAuthRequired` | `net-proxy-auth-required` | error | false | 代理拒绝凭证（407/SOCKS5 子协商）。 |
| 130 | `net` | `corrupt` | `NetProxyProtocol` | `net-proxy-protocol` | error | false | 代理握手字节不符 CONNECT/SOCKS5 文法。 |
| 131 | `net` | `io` | `NetDnsFailed` | `net-dns-failed` | error | true | 解析器返回错误或空答案（解析动作失败）。 |
| 132 | `net` | `io` | `NetProxyConfigUnreadable` | `net-proxy-config-unreadable` | error | true | 系统代理设置读取原语被拒。 |
| 133 | `net` | `denied` | `NetPolicyViolated` | `net-policy-violated` | error | false | 声明式 NetPolicy 拒绝请求（策略/凭据）。 |
| 134 | `net` | `io` | `NetOptionFailed` | `net-option-failed` | error | false | setsockopt/getsockopt/shutdown 原语失败。 |
| 135 | `net` | `timeout` | `NetConnectTimeout` | `net-connect-timeout` | warning | true | 非阻塞 connect 未在 deadline 内完成。 |
| 136 | `net` | `exhausted` | `NetBufferLimit` | `net-buffer-limit` | warning | true | 传输缓冲触顶高水位（背压护栏）。 |
| 137 | `net` | `exhausted` | `NetFramerOverflow` | `net-framer-overflow` | error | false | 单帧触顶 max_frame_bytes（容量护栏）。 |
| 138 | `net` | `invalid` | `NetFramerConfigInvalid` | `net-framer-config-invalid` | error | false | Framer 配置非法（limit=0/分隔符/宽度）。 |
| 139 | `net` | `conflict` | `NetOptionConflict` | `net-option-conflict` | error | false | 两设置不能在同 socket 共存（如已连 UDP 加组播）。 |
| 140 | `net` | `io` | `NetTlsHandshakeFailed` | `net-tls-handshake-failed` | error | false | TLS 握手原语失败（对端中止/拒绝）。 |
| 141 | `net` | `denied` | `NetTlsCertRejected` | `net-tls-cert-rejected` | error | false | 证书链校验失败（未知签发/过期/主机名不符）。 |
| 142 | `net` | `conflict` | `NetTlsVersionMismatch` | `net-tls-version-mismatch` | error | false | 本端 floor 与对端能力互斥（无共同版本）。 |
| 143 | `net` | `unavailable` | `NetTlsEngineUnavailable` | `net-tls-engine-unavailable` | error | false | 无可用 TLS 后端（构建未编/候选链落空）。 |
| 144 | `net` | `denied` | `NetTlsPinMismatch` | `net-tls-pin-mismatch` | error | false | 证书不含任一钉住公钥（身份被拒）。 |
| 145 | `http` | `invalid` | `HttpRequestInvalid` | `http-request-invalid` | error | false | 请求违反消息文法（方法/头/目标非法）。 |
| 146 | `http` | `timeout` | `HttpTimeout` | `http-timeout` | error | true | HTTP 三阶段之一超时。 |
| 147 | `http` | `exhausted` | `HttpRedirectLimit` | `http-redirect-limit` | error | false | 重定向链触顶上限。 |
| 148 | `http` | `exhausted` | `HttpBodyTooLarge` | `http-body-too-large` | error | false | 响应体触顶 max_body/decompressed（容量护栏）。 |
| 149 | `http` | `unsupported` | `HttpCharsetUnsupported` | `http-charset-unsupported` | error | false | 本批只解码 UTF-8/Latin-1。 |
| 150 | `http` | `corrupt` | `HttpProtocolError` | `http-protocol-error` | error | false | 对端违反 HTTP/1.1 文法（字节错位）。 |
| 151 | `http` | `unsupported` | `HttpContentEncodingUnsupported` | `http-content-encoding-unsupported` | error | false | 对端用了本客户端解不开的编码。 |
| 152 | `http` | `corrupt` | `HttpBodyCorrupt` | `http-body-corrupt` | error | false | 消息框完整但载荷解码失败（损坏/截断）。 |
| 153 | `http` | `unsupported` | `HttpBodyNotReplayable` | `http-body-not-replayable` | error | false | 流式体不可回到起点重发（本类型不支持重放）。 |
| 154 | `ws` | `unavailable` | `WsClosed` | `ws-closed` | info | true | 连接已关闭，通道当前不可用。 |
| 155 | `ws` | `denied` | `WsHandshakeRejected` | `ws-handshake-rejected` | error | false | 对端拒绝升级（101/Accept/子协议）。 |
| 156 | `ws` | `corrupt` | `WsProtocolError` | `ws-protocol-error` | error | false | 对端违反 WS 帧文法（字节错位）。 |
| 157 | `ws` | `exhausted` | `WsSendQueueFull` | `ws-send-queue-full` | warning | true | 发送队列触顶字节/条目上限（背压护栏）。 |

## 已核实的实现约束与非落点

以下六条经逐处核查确认，用于排除疑似落点、并把实现期最容易踩的约束前置：

1. **`IO` 双命名门禁放行**：`tools/check/check_naming_conventions.py` 的 `PASCAL_RE = [A-Z][A-Za-z0-9]*` 对 `IO` 判_PASS_；`.clang-tidy` 的 `readability-identifier-naming.EnumConstantCase = CamelCase` 只禁下划线、不禁全大写，`IO` 同样放行。故 `ErrorKind::IO` 不需要任何 NOLINT / 豁免。
2. **生成头会被 clang-tidy 扫描**：`.clang-tidy` 的 `HeaderFilterRegex` 含 `include[/\\]aurora`，而 `error_codes.gen.h` 正在 `include/aurora/core/` 下 ⇒ 新增的 29 + 12 = 41 个枚举项**必须各自带 `///<` 文档**（否则 `check_doc_comments` 与 `lint` 判红），且生成物必须经 clang-format（`generate_error_codes` 目标已内建该步骤，不得绕过其路径默认值）。这是把两个 DOC 数组列为必做项、而非可选项的原因。
3. **按域重排不影响任何 golden 或 JSON 断言**：`Error::to_json()` 的 `code_enum` 输出的是 `to_string(code_enum)` 即**标识符字符串**（如 `"LayoutNullChild"`），不是数值；`Diagnostic::to_json_line()` 只输出 `code`（slug）与 `severity`，同样无数值。故 `ErrorCode` 数值随重排变化对进程外零影响——这是「允许重排」这条决策的实证支撑，而非仅凭「数值是进程内行号」的论断。
4. **`aurora_api.json` 的 enums 段不含 `ErrorCategory`**：现有 46 枚枚举里与错误相关的只有 `HttpErrorClass`；生成器 `gen_api_tools` 不把 `error_codes.gen.h` 的枚举收进 enums 段。故新增 `ErrorDomain`/`ErrorKind` **不进** enums 段、不占枚举预算、也不受 `check_naming_conventions` 检查；`error_codes` 段每条只是把 `category` 一个短串换成 `domain`/`kind` 两个短串，增量可忽略。
5. **API 预算充足**：现状 `~49567 / 75000` tokens（余量 25433），新增内容远低于余量，`check_api_budget` 无风险。
6. **两处疑似落点经核实无需改动**：`codespec/debug_api.toml` 对 `diagnostics` 只声明签名 `diagnostics() -> Json` 与一句摘要，**不枚举响应键**，故 `/api/diagnostics` 改键名无需同步该 toml；`tools/check/check_api_schema_sync.py` 对 `error_codes` 段只区分「是否参与合并」，**不校验键集合**，故键名增删无需改门禁。

## 破坏性口径与先例

- `SPECIFICATIONS.md` §1 明定 alpha 预览版不构成稳定性承诺；`ErrorCategory` 重定义、`Error` 输出键改名、`FixSuggestion` 移除均按「直接删除/重定义」而非先 `[[deprecated]]` 过渡——与 CHG-021 同口径（被删符号零消费端、随生成器重建，先标废弃再删只让两批变更各背一半状态，收益为零）。
- 数值是进程内行号而非外部契约；冻结契约是 `slug`。枚举值随按 domain 重排允许变化（首次出现 `domain=net` 即 net 域内首枚定新值），跨进程零破坏。
- `Error::suggestion` 保留（调用点特化建议，与 `hint` 分工明确），仅删第三份 `fix` 展示壳。

## 变更内容

1. **`codespec/errors.toml` 改两列 + 重排 + 头注**：157 条目的 `category = "..."` 行全部替换为 `domain = "..."` 与 `kind = "..."`（机械替换，按上方映射表逐条；`domain`/`kind` 取值严格取自两张清单）。条目整体按 `ErrorDomain` 枚举值顺序重排（域内保持原 toml 相对次序），使 `ErrorCode` 数值、`AURORA_ERROR_TABLE`、目录、`aurora_api.json` 均按域聚集；首行恒为 `GeneralUnknown`。头部注释改写：删「category 取值枚举」段，改述 `domain`/`kind` 两列的语义，并把 `ErrorKind` 的判型优先级次序写入头注。
2. **生成器 `tools/gen/gen_error_codes.cpp`**：`ErrorEntry` 的 `category` 字段改为 `domain` + `kind`；新增 `AURORA_DOMAINS`(29)/`AURORA_KINDS`(12) 与并行 `AURORA_DOMAIN_DOC`/`AURORA_KIND_DOC` 两个说明数组（满足 CODING_STANDARDS §13.5.2 枚举一描述）；`apply_field` 增加 `domain`/`kind` 分支；`parse_toml` 校验改为「`domain`/`kind` 任一为空，或取值不在 `AURORA_DOMAINS`/`AURORA_KINDS` 内」即 `err()` 并 `return false`（触发 `main` 返回 1、不写产物）；枚举生成由单一 `ErrorCategory` 改为 `ErrorDomain` + `ErrorKind` 两枚；`ErrorMeta` 结构字段由 `category` 改为 `domain` + `kind`；表初始化与结构化绑定同步；新增 `domain(ErrorCode)`/`kind(ErrorCode)` 两查表与 `to_string(ErrorDomain)`/`to_string(ErrorKind)` 两重载，删 `category(ErrorCode)`/`to_string(ErrorCategory)`；目录表头/单元格与 `aurora_api.json` 输出各增 `domain`/`kind` 两列（替代原 `category` 单列）。**枚举 C++ 名不用规则推导，改显式并行数组**：除 `AURORA_DOMAINS`/`AURORA_KINDS` 与两个 DOC 数组外，再加 `AURORA_DOMAIN_NAMES`/`AURORA_KIND_NAMES` 两个并行数组，显式给出每档的 C++ 枚举名（`io`→`Io`、`i18n`→`I18n`、`recent-documents`→`RecentDocuments`、`not-found`→`NotFound`，以及性质 `io`→`IO`）。理由：一是「kebab ↔ C++ 名」的映射需要可 grep、可评审，写成规则函数则 `IO` 这一个全大写特例变成藏在代码里的隐含知识；二是现有 DOC 数组本就是并行数组惯例，加两个同构数组成本最低。既有的 `capitalize()` 仍只服务于 `ErrorSeverity`（单词档名，无需分情况）。**新增首行硬校验**：`entries[0]` 必须同时是 `GeneralUnknown` / `general-unknown`，否则 `err()` 并 `return 1`——这是「`ErrorCode{}` 恒等于 `GeneralUnknown`」零值不变量在重排后唯一的机械保障。**新增取值白名单**：`domain`/`kind` 任一为空或不在各自数组内即 `return 1`，绝不静默兜底为 `general`（现状 `category` 空值会被兜底成 `general`，正是「随手贴标签」的温床）。
3. **三件生成物重建**：经 `generate_error_codes` 目标重刷，不手改（`error_codes.gen.h` 增 `ErrorDomain`/`ErrorKind` 两枚举与 `domain()`/`kind()`；`ERROR_CATALOG.md` 列改为 `domain`/`kind`；`aurora_api.json` 的 `error_codes` 段对象键 `category`→`domain`+`kind`）。
4. **`Error` 结构体（result.h / result.cpp）**：字段 `category` 改为 `domain` + `kind`（`make_error_from_table` 填入 `m.domain`/`m.kind`）；`to_json()` 输出键由 `category` 改为 `domain` + `kind`（恒输出），`suggestion`/`docs`/`where`/`hint` 条件输出不变。**同步四处注释**：字段受众说明里的 `category`、序列化字段清单 `code/message/code_enum/severity/category/retryable`、查表说明、以及 legacy 重载上「slug/severity/category/fix 元数据仍来自表」那句——该处含 `fix` 字样，须一并改写为 `domain`/`kind`，否则「fix 零残留」不成立。
5. **`Diagnostics` 删 `FixSuggestion`（diagnostics.h / diagnostics.cpp）**：删 `struct FixSuggestion`、`Diagnostic::fix`、`Diagnostics::collect_fixes()`、废弃别名 `DiagnosticCategory`、`report` 的第 5 形参（`fix`）；`Diagnostic` 字段 `category` 改为 `domain` + `kind` 并增 `domain_str()`/`kind_str()`（删 `category_str()`）；`to_json_line()` 输出 `domain`/`kind` 两键、删 `fix_code`/`fix_desc` 分支；`report()` 表驱动注入 `domain`/`kind`（保持既有无码退化逻辑）；`warn`/`degraded` 去掉多余 `FixSuggestion{}` 实参。
6. **`/api/diagnostics`（debug_runtime.cpp）**：该行 `e.set("category", ...)` 改为 `e.set("domain", ...)` 与 `e.set("kind", ...)`；删 `if (d.fix) { ... fix_code/fix_desc ... }` 块。
7. **`inspector_api.cpp`**：`d.category = ErrorCategory::General;` 改为 `d.domain = ErrorDomain::General; d.kind = ErrorKind::Internal;`。
8. **头注释收敛**：`include/aurora/app/clipboard.h`（3 处「category platform」→「domain clipboard」语义表述）、`include/aurora/net/http.h`、`include/aurora/webview/web_view.h`、`src/aurora/net/tls_backend.cpp` 中涉及 `ErrorCategory`/「category」的注释改写。
9. **测试迁移**：`utest_error_codes.cpp` 增 29 域/12 性质枚举完备性断言、两查表 `domain()`/`kind()` 抽查、`GeneralUnknown == ErrorCode{}` 的 `static_assert`（零值哨兵不变量）、按新表抽取若干条目核对 `domain`/`kind`；`utest_result.cpp` 的 JSON 键断言 `category`→`domain`+`kind`；`utest_diagnostics.cpp` 删 `fix` 三例、`category_str` 断言改 `domain_str`/`kind_str`、删 `collect_fixes` 相关；`examples/demos/demo_core.cpp` 同步 `category`→`domain`+`kind` 用法。**补测 `/api/diagnostics` 的逐条键面**（`utest_debug_runtime.cpp` 的 `diagnostics_snapshot_shape` 原先只查 `count` 与数组形状，两轴改名对它不可见）：现按 `code` 定位本例自己发出的那条，断言 `severity`/`domain`/`kind` 三键取表内值且 `category`/`fix` 两键不出现；开关两侧的分支都由既有 `feature_flags().debug` 运行时探测承担（关闭态 SKIP，开启态实跑）。
10. **文档回写**（见「回写落点」）。
11. **`CHANGELOG.md` [Unreleased]**：补 `Breaking`（`Error::category`→`domain`+`kind`、`Diagnostic`/`Error` JSON 输出 `category` 键→`domain`+`kind`、`FixSuggestion`/`Diagnostic::fix`/`collect_fixes()`/`DiagnosticCategory` 移除）与 `Migration`（消费端改读 `hint` 作唯一修复指引、JSON 键改名、枚举 `ErrorCategory`→`ErrorDomain`+`ErrorKind` 且值随重排变化但 `slug` 冻结不动）。

## 验收判据

1. **全仓零残留**：排除 `build*/`、`cmake-build*/` 与已归档提案正文后，检索 `ErrorCategory`/`DiagnosticCategory`/`category_str`/`FixSuggestion`/`collect_fixes`/`fix_code`/`fix_desc`/`\.fix\b` 零命中。**豁免（已逐处核实为与错误元数据无关的同名概念，改动它们属越界）**：
   - 日志前缀 `Logger::log(category)` 与 `AURORA_LOG_*(category)` 系列宏（`core/log.h` 全篇、`core/log.cpp`、`utest_log.cpp`、`demo_core.cpp` 的「category 前缀」表述）；
   - i18n 复数类别 `plural_category` / `plural_category_ordinal` / `PluralCategory` / CLDR category（`i18n/plural.h`、`i18n/plural.cpp`、`i18n/message_engine.cpp`、`tools/gen/gen_i18n.cpp`、`tools/gen/gen_plural_rules.py`、`tools/check/check_i18n_gen_golden.py`、`utest_plural.cpp`）；
   - LSP 组件 schema 的分组字段 `category`（`tools/include/lsp_schema.h`、`tools/servers/aurora_lsp.cpp`、`tools/include/lsp_features.h`、`itest_aurora_lsp.cpp`）；
   - 命令分组字段 `cmd.category`（`commands.h`、`commands.cpp`、`widget/command_palette.h`、`utest_commands.cpp`、`utest_command_palette.cpp`、`demo_command_palette.cpp`）；
   - XDG 通知 hint 的 `category` 键（`app/notification.cpp` 里按 XDG 规范写入 `a{sv}` 的那段）；
   - 图表分类轴 `category_count` / `category_center` / `draw_category_axis`（`widget/bar_chart.h`、`widget/line_chart.h`、`widget/chart_common.h`、`utest_bar_chart.cpp`、`demo_bar_chart.cpp` 文案）；
   - ObjC 的 `@interface ... (category)` 声明（`tools/verify/macos_cursor_live_probe.mm`）；
   - 保留字段 `Error::suggestion`（调用点特化建议，与 `hint` 分工明确，见「不在本次范围」）；
   - **带日期的既成记录**：`CHANGELOG.md` 中已发布版本段（`[1.0.0-alpha.9]` 及其前）的 Breaking / Migration 条目，与 `codespec/manual-test/*` 第 3 节执行记录里**未重跑**那几例的实际现象（如 `19-app.md` 的 `TC-APP-013`，其**期望值**本就不含 `category`，只有记录行有）。它们是「某日观察到什么」的证据链，改写等于伪造记录；本次改动的口径由 `[Unreleased]` 新条目与其对应的新执行记录行承载。判据只约束**规范性文本**（代码、头注、规格正文、用例期望值）。
   - **`errors.toml` 的 `hint` 文案及其在 `error_codes.gen.h` 中的自然语言中出现的 "fix" 一词**（如 `"...or fix the logic"`、`"Fix the request at the call site"`）：那是修复提示的英文散文，与 `FixSuggestion` 机制无关，**不得为凑「零残留」而改写错误文案**——判据只针对 `FixSuggestion`/`fix_code`/`fix_desc`/`collect_fixes`/`Diagnostic::fix` 这些**符号与键名**。
   - **`CHANGELOG.md` 的 `[Unreleased]` 段**（`Breaking` 叙述与 `Migration` 的旧→新对照表）：迁移契约的左列按定义就是被删掉的标识符与键名，抹掉它等于把迁移指南写成不可执行。与上一条「已发布版本段」同理，判据约束的是规范性文本（代码、头注、规格正文、用例期望值）与三件生成物，不约束「描述这次改名」的发布记录正文。
   - **`third_party/`**（AGENTS.md §2 不许修改）里恰好同名的 `fix_code`（libwebp 的文档与 `lossless_common.h`）：与错误元数据无任何关系。
   - **lint / format 驱动脚本的 `--fix` 开关**（`tools/check/run_clang_format.py`、`tools/check/run_clang_tidy.py` 里的 `args.fix` / `a.fix`）：命令行「改写模式」标志，与 `Diagnostic::fix` 无关。
   - **测试中断言「旧键不再输出」的负向字面量**（`utest_diagnostics.cpp`、`utest_result.cpp` 里的 `R"("category")"` 与 `"fix"`）：它们是判据 4 的证据形态本身——删掉这些断言等于取消判据 4，而不是让判据 1 变绿。检索口径因此只看**标识符的使用与键的写出**，不看「断言某键不存在」的字符串。
   - 检索命令（本轮实跑，命中集为上述豁免项加 `CHANGELOG.md` 的 `[Unreleased]` 与已发布段）：`git grep -n "<模式>" -- ':!build*' ':!cmake-build*' ':!third_party/*' ':!codespec/changes/*'`，七个符号模式在豁免之外**零命中**。
2. **覆盖**：157 条无一缺 `domain`/`kind`（生成器硬校验，缺列/取值越界即 `return 1` 且不写产物）；29 域与 12 性质枚举各自至少有一枚条目使用。
3. **零值不变量**：`static_assert(ErrorCode::GeneralUnknown == ErrorCode{})`（`code_enum{}` 作默认哨兵，重排时最容易踩）；首行恒 `GeneralUnknown`。
4. **形态**：`Error::to_json()` 含 `domain`/`kind`、无 `category`；`/api/diagnostics` 与 `Diagnostic::to_json_line()` 无 `fix` 键。
5. **变异自证**：(a) 改错一条 `(domain, kind)` → 抽查表转红；(b) 删生成器的取值校验 → 单词拼错的 kind 不再报错（用 toml 副本验）。
6. **幂等**：生成器连跑两次零 diff；`check_gen_api_merge` 绿。
7. **门禁**：`check_api_schema_sync`（先 `aurora_api_json`）/`check_api_budget`/`check_doc_comments`/`check_naming_conventions`/`check_code_doc_sync`/`check_codespec_xref`/`check_change_proposals`/`check_core_layer_boundary`/`check_umbrella_header`/`format-check` 全绿；`lint` 仅改动 TU；全量 ctest 最高并行度在 Windows LLVM 构建下绿，并另跑 WSL X11 与 wasm 构建验证（改公共头）。

## 验收证据

执行面：Windows（Ninja + g++ 16.1.0，Release，`AURORA_ENABLE_DEBUG` 取 `AUTO` 落 OFF，另以 LLVM/clang-tidy 做静态检查）、WSL2 Ubuntu（Unix Makefiles，Debug，X11 与 Wayland 双后端编译，16 核）、Emscripten（Node v26.12.1）。日期 2026-10-29。

| 判据 | 实测证据 |
|:---|:---|
| 1 全仓零残留 | 判据 1 所附 `git grep` 命令实跑：七个符号模式在豁免清单之外零命中；豁免项逐条给出文件与理由（见判据 1 列表），没有为凑绿而改写错误文案或迁移表左列。 |
| 2 覆盖 | 解析 `ERROR_CATALOG.md` 得 157 行、29 个不同 `domain`、12 个不同 `kind`；CLI 族 12 码按 `kind` 分档 2 `not-found` / 1 `conflict` / 9 `invalid`；29 域与 12 性质均至少被一枚条目使用（无空枚举项）。 |
| 3 零值不变量 | `static_assert(ErrorCode::GeneralUnknown == ErrorCode{})` 在 Windows g++、WSL gcc 两套编译面成立；生成器首行硬校验对「首行非 `GeneralUnknown`」实测 `return 1` 且不写产物。 |
| 4 形态 | `Error::to_json()` / `Diagnostic::to_json_line()` 的正向两轴断言与反向断言（`category` 与 `fix` 键不出现）两侧绿；`/api/diagnostics` 的逐条键面断言在 WSL Debug（`AURORA_ENABLE_DEBUG` 生效）实跑通过，在 Windows Release（关闭态）按设计 SKIP——开关两分支各被执行过一次，不是只测了一侧。 |
| 5 变异自证 | (a) 把一条 `(domain, kind)` 改错 → 抽查断言转红并报出该行实际值；复原后与改动前逐字节一致。(b) 删生成器的取值白名单 → 拼错的 `timout` 通过并落成 `ErrorKind::,` 与 `kind= timout`，证明该校验承重；三处硬校验（缺 `domain` / 缺 `kind` / 首行非 `GeneralUnknown`）各实测 `return 1` 且三件产物零写入。 |
| 6 幂等 | 最终内容连跑两次生成器零 diff（`error_codes.gen.h` `00d844dd08c3…`、`ERROR_CATALOG.md` `9be1b90bc413…`、`aurora_api.json` `ef79cf3423…`）；`check_gen_api_merge` 绿；三件产物在 g++、gcc、Emscripten host 三套生成器下逐字节一致。 |
| 7 门禁 | Windows：全量 `ctest --preset ninja-test` **450/450 通过（29.63 s）**，含全部 21 条 `check_*`；改动的 19 个 C++ TU 经 clang-tidy 0 finding、clang-format 19/19 合规。WSL X11：全量 ctest 最高并行度 **449/450**，唯一红项 `etest_a11y_semantics` 因该镜像未装 AT-SPI 总线（`find_atspi` 恒空），属环境项。wasm：738 步构建 EXIT=0，测试 runner 与全部工具链接完成（判据只要求构建面，未跑浏览器内 ctest）。 |

WSL 首轮曾出现 6 项红，逐项定性如下，均为环境项而非本改动引起；处置后得到上表的 449/450：

| 红项 | 机制 | 处置与结果 |
|:---|:---|:---|
| `check_api_schema_sync`、`check_i18n_gen_golden` | 该构建目录的 `gen_api_tools` 二进制停在 2026-09-30（早于 upstream 新增 `Icon`/`IconButton`/`WebView` 与 21 枚枚举），`gen_i18n` 从未构建；前者正是「代码视角」JSON 的产出者 | 重编两枚生成器后均绿 |
| `utest_net_transport`（3 例本机 IPC） | 临时目录落在仓库根下的 `test_temp/`，即 DrvFs 挂载点；AF_UNIX `bind()` 返回 `EOPNOTSUPP`（探针实测：同一路径 `/mnt/d` 失败、`/tmp` 成功） | 以 tmpfs 绑定挂载覆盖 `test_temp/` 后 17/17 全绿 |
| `etest_interaction_flow`、`etest_multi_window`、`etest_a11y_semantics` 的 `wayland` 例 | WSLg 的 Weston 无窗口管理器语义：焦点断言恒假、像素读回为白；仓库口径本身把 Wayland 排除在 CI 外（`specification/08-tooling.md` §8.2） | 以空 `XDG_RUNTIME_DIR` 且不设 `WAYLAND_DISPLAY` 运行，即按策略 skip；同批 `x11` 与 `glfw` 例全绿 |

## 回写落点

- `codespec/errors.toml`（157 条改两列 + 头注 + 域序重排 + 优先级段）
- `codespec/ARCHITECTURE.md`（§12.3 分类维度：删 `category` 单行、改 `domain`+`kind` 两行并各写语义与路由价值；类型模型段 `ErrorCategory`→`ErrorDomain`+`ErrorKind`）
- `codespec/specification/01-core.md`（§3.1 `Error` 字段清单：`category`→`domain`+`kind`；§4.1 `Diagnostic` 与 `FixSuggestion` 描述：删 `FixSuggestion`/`collect_fixes`，写明修复指引唯一由 `hint` 承担；§4.2 `Diagnostics` 方法表：删 `collect_fixes` 一行，`report` 去 `fix` 形参）
- `codespec/specification/06-app-platform.md`（§8.8.22 理由段重写——「之所以不直接用 `ErrorCategory`…」改为「业务域已上升为一等元数据，`domain=net` 即可精确聚合，`HttpErrorClass` 与 `ErrorDomain::Net` 各司其职」；各处 `category = io` / `category = platform` 表述改为对应的 `domain` + 逐码 `kind` 两轴写法，含 `net-*` 五枚错误码、`net-policy-violated` 与 §8.8.28 重放闸的 `http-body-not-replayable`）
- `codespec/specification/08-tooling.md`（§10.1 需求规格的 `Error` 字段清单同步）
- `codespec/specification/09-cli.md`（§6.1 CLI 错误码段：12 条改述为 `domain` 一律 `cli`、`kind` 逐码分档 2/1/9）
- `codespec/SPECIFICATIONS.md`（§1 稳定性口径引用同步，无实质改动）
- `codespec/ERROR_CATALOG.md`（生成物，随生成器重建，不手改）
- `codespec/manual-test/01-core.md`（`TC-CORE-006` 的操作步骤与预期结果改按 `domain=` / `kind=` 双字段读，记录行按重跑输出更新）；`codespec/manual-test/19-app.md` **无需改动**——全篇只在 `TC-APP-013` 的日期记录行里出现过一次 `category`，其期望值本就不引用错误轴（豁免见验收判据 1）
- 发布记录 CHANGELOG.md（`[Unreleased]` 的 `Breaking`/`Migration`；该文档不属 codespec 树，按 AGENTS.md 硬规则 5 不列为回写落点，实际已同步）
