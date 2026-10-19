# CHG-023 网络可观测性：Inspector 路由注册表、请求时间线与活动表、凭据强制脱敏与宿主过滤口、network 端点与 MCP 工具

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-023 |
| 提出日期 | 2026-10-20 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.FEAT.NET.OBSERVABILITY.001 |
| 影响面 | 跨三层的一次增量交付，**无破坏性签名变更、无新增错误码**。（1）Inspector：`src/aurora/inspector/inspector_server.cpp` 里 `route_request` 的 if 链（实测 16 枚精确路由 + 2 枚前缀族交错，函数体 L327–L1053）改为文件内静态注册表，公共面零变化；随之有一处 REST 行为纠回——「动词本就不被支持 **且** 路径恰好退化」这一交集（`DELETE /api/widget/` 一类）由 400 变 405。（2）HTTP 值层：新增公共枚举 `HttpStage`（五枚阶段）与结构体 `HttpRequestTimeline`，`Response` 新增只读口 `timeline()` 与一枚按 `AURORA_ENABLE_DEBUG` 裁切的数据成员——**该宏自此改变 `Response` 的内存布局**（此前只改 `Widget`），须登记 `codespec/BUILD_OPTIONS.md`；宏关闭时函数恒声明、恒回 `nullopt`，调用侧零 `#ifdef`。（3）发起侧：`Client` 新增 `ActivityOptions{enabled = true, capacity = 64}` 与一枚共享内核（与既有 `cache_` / `pool_` / `cookie_jar_` 同格局，拷贝共享同一张表），条目为 in-flight + 最近 N 条已完成的环形表。（4）脱敏：新增公共头 include/aurora/net/export_filter.h（`ExportSurface` 枚举 + `ExportFilter` 抽象类 + `set_export_filter()` / `export_filter()` 进程级两枚），库内对 `Authorization` / `Proxy-Authorization` / `Cookie` / `Set-Cookie` 四枚字段强制打码且**宿主无权关闭**。（5）暴露面：`aurora::debug` 门面新增两枚 JSON 能力，Inspector 新增 `GET /api/network` 与 `GET /api/network/request?id=` 两枚端点，MCP 工具 13 → 15。（6）`aurora_api.json` 随 `--target aurora_api_json` 重建（enums 段 +2、方法计数随之）、`tools/check/umbrella_manifest.txt` 收录新头、`codespec/manual-test/23-net.md` 新增一枚用例 |

## 动机

1. **本库的立身之处要求这件事是核心交付面，而不是附加项。** `codespec/SPECIFICATIONS.md` §5 已把「AI 能从一次界面异常反查到一次请求的耗时与失败阶段，且排障信息不会泄漏凭据」写成 `SPEC.FEAT.NET.OBSERVABILITY.001` 的判据；网络又是最难事后排查的一类 I/O（现场不可复现、对端不可控、失败形态跨六档归因）。而这条需求今天**没有任何实现承担**——跨平台网络栈那伞形提案（`codespec/changes/xplat-network-stack/proposal.md`）的 §17 落地序里，底座、传输、TLS、HTTP、绑定、SSE、WebSocket 七件都已收口，剩下的只有可观测性这一件。
2. **既有埋点已经全部就位，缺的只是消费者。** 发起侧 `StageTimer` 的 connect / read_idle / total 三档已挂在阶段边界，`detail::await_connect_via` 是全库唯一建连出口，`stamp_request_id` 与 `log_exchange_identity` 那两枚起手行在位；一次交换一枚、跨重定向与重试恒同一枚的 `X-Request-Id` 也已随链路标识件落地。也就是说「耗时与失败阶段」的数据源只差「到点记一笔」这一步。不接这层，链路标识那件的「反查」承诺只到对端 access log 为止——本端什么都问不出来。
3. **每加一枚端点必改一枚千行巨函数。** 实测 `src/aurora/inspector/inspector_server.cpp` 全文 1306 行，`route_request` 一条 if 链占了 L327–L1053，其中**精确匹配与前缀匹配交错并存**（`route == "/api/debug/state"` 一类与 `route.starts_with("/api/widget/")`、`starts_with("/api/input/")` 两类穿插）。本切片要挂两枚端点，先做注册表化才能无侵入落地——这条前置改造已由那份提案的 §16 第 8 项裁决并入网络栈范围，本提案把它连同判据一起定形。
4. **凭据入日志今天零防线，且泄漏面比提案设想的更宽。** 实测全域（`src/`、`include/`、`tools/`）对 `redact` / `脱敏` / `打码` / `mask` 空命中；`include/aurora/core/log.h` 的 `Logger` 只有级别阈值、sink 重定向、开关三枚旋钮，**没有任何脱敏接缝**；`codespec/CODING_STANDARDS.md` 内「凭据 / Authorization / Cookie」零命中。更关键的是第二条泄漏路径不在日志里：Inspector 的 `/api/tree` 与 `/api/widget/{path}` 会把控件属性全量导出（含 TextInput 的文本），宿主塞进输入框的 token 直接进 JSON。因此本提案把脱敏定成**两轨**——库内四枚凭据字段强制打码（安全边界，不可关），其余键值经一枚进程级 Filter 由宿主裁决（库不猜哪些键敏感）。
5. **一处已定案的执行强度在仓内无法落地，须纠回。** 那份提案的 §16 第 9 项定的是「条款 + 四出口单测断言 + lint 规则」。实测仓内没有任何既有 `check_*` 扫描器能自然承载「凭据不得入日志」这条规则，clang-tidy 亦无自定义正则检查能力；硬造一枚 `check_*` 只会多一个需要维护豁免的扫描器。故本提案纠回为「条款 + 四出口同一断言集 + `codespec/CODING_STANDARDS.md` §15 一条反例」，旧编号与旧文字保留、追加裁决取代其后半（静态扫描抓不到运行时凭据值，这条理由同批写进裁决）。
6. **「关闭时零开销」与「公共头维持零 `#ifdef`」两条既有口径相撞，需要一个定案。** 前者的原文承诺是「数据成员不占位」，后者是网络栈公共头的既有纪律。实测本仓早有先例：`include/aurora/widget/widget.h` 在公共头内以 `#ifdef AURORA_ENABLE_DEBUG` 裁切数据成员，`cmake/AuroraFeatures.cmake` 与 `cmake/AuroraInstall.cmake` 明写该宏「改变类内存布局，故由 PUBLIC 导出、消费端须同值」。本提案按这条例外走，并如实登记新增的那处后果（`Response` 尺寸随宏变化）——比之为了零 `#ifdef` 而让 Release 也背着环形表，前者是唯一能兑现原文承诺的形态。

## 变更内容

1. **Inspector 路由注册表化（前置件，零公共面）。** `route_request` 的 if 链改为文件内静态数组，条目字段 `pattern / kind(Exact|Prefix) / methods / label / handler`；handler 取**指向 `InspectorServer::Impl` 成员函数的指针**而非 `std::function`（仓内 `.clang-tidy` 已记录「任何转入 `std::function` 的可调用对象一律判为不应抛出」这条系统性假告警面，不值得为一张表去招它）。查找做**两趟**：先整表扫精确项，再按登记次序扫前缀项——这就是那份提案 §18 第 5 项要求的「显式优先级字段而非依赖插入顺序」。方法闸**统一收在派发处**：`405` 说的是「这个端点不接受这个动词」，而 `DELETE /api/widget/` 现在先回 400「路径为空」——路径形状对与错都改变不了动词不被支持这一更基本的事实；收在派发处后，方法闸成为端点表**说出一次**的属性，每个 handler 可假定进到手里的方法就是它声明过的那枚。行为差异面只有那一个交集，故**记为纠回**：追加裁决写明差异、理由与判据用例，旧编号旧文字保留；该交集现状无任何用例覆盖，新增用例是**补判据**而非改判据。handler 体内逻辑逐字搬移、一行不改，`path` / `route` / `method` / `body` 四枚经一枚 `RouteCall` 形参交入（`path` 仍为 `const std::string &`，因 `query_param` 签名如此）。405 文案与 404 兜底逐值不变（前缀族的 label 不带尾斜杠、404 用含查询串的原样 path）。
2. **请求时间线（值层 + 门控）。** 新增公共枚举 `HttpStage`（`Dns` / `Connect` / `TlsHandshake` / `FirstByte` / `BodyDone`，登记 `tools/include/known_enums.h` + `tests/integration/itest_known_enums.cpp` 编译期锚点），与结构体 `HttpRequestTimeline`（`request_id` + 总耗时 + 每阶段「相对起手偏移 / 累计收发字节」）；`Response` 新增 `timeline()`，数据成员按 `AURORA_ENABLE_DEBUG` 裁切、宏关时恒回 `nullopt`。**阶段划分不新造**——埋点只加在既有三档与建连出口上，「到点记一笔」。**粒度：一跳一条**，`timeline()` 交出本次收摊那一跳，完整跳序列进活动表供远端展开（重定向的每一跳、重试的每一次尝试各一条，同一次调用按同一枚 `X-Request-Id` 串成序列）。**覆盖腿面由不变量决定而非按腿列举**：判据是「一次会完成的交换 = 一条条目」——`send` 与 `open_stream` 都有 Body 完成终点，全量收；`open_sse` 的每条流（含每一次重连）本身就是一次有始有终的 `open_stream`，按每条流一条收、继承同一批埋点；`connect_websocket` 只收**握手**那一条，升级后的会话**不进请求活动表**（会话的收摊事实已在 `WsCloseInfo` 里，属会话观测而非请求观测）。守住这条分界的理由是「in-flight 恒有界」——长驻订阅若占 in-flight 名额，一条永不结束的流会把环形表挤满、把真正要排障的读数淘汰掉。会话级观测（连接时长、消息计数）另登待办，不在本提案范围。
3. **活动表（`Client` 持有 + 门面汇总）。** 新增共享内核（`std::shared_ptr<detail::HttpActivityLog>`）与既有 `cache_` / `pool_` / `cookie_jar_` 同格局，故拷贝的 `Client` 共用同一张表；旋钮为 `ActivityOptions{enabled = true, capacity = 64}`，**缺省开**（与 `PoolOptions` / `CacheOptions` 的缺省关相反且刻意：那两件改变的是已存在的交换何时不再发生，而本件是唯一「出问题当场问不出来就永久问不出来」的那件，缺省关掉等于「AI 可观测运行时」要配置才成立）。条目字段：method、host、状态码、错误码、耗时、收发字节数、`X-Request-Id`、in-flight 标志。淘汰口径：环形表满只淘汰最旧的一条**已完成**条目，in-flight 恒不被淘汰（淘汰它等于抹掉唯一还在的事实）。名册形态：内核构造时把自己**自动**登进进程级 `weak` 名册、析构时摘除，门面取快照时逐个 `lock()`、失败的当场剔除；名册本身按 `AURORA_ENABLE_DEBUG` 裁切 ⇒ Release 零全局可变状态。线程口径逐处写进 `@note Thread`：写入发生在发起侧（主线程投递器恢复的续体上），读取发生在门面对象被问的那一刻，名册与表各自加锁，不沿用「单线程 UI」的笼统说法。导出经 `aurora::debug` 门面新增两枚能力（`network_activity(limit)` 与 `network_request(request_id)`），沿用既有五项能力的格局：薄封装、子系统留原地、函数体按宏裁切、Release 回 `{"available": false, ...}`；JSON 折算留在门面层，net 侧只交出结构体——**net 不得 upward-depend 到依赖 widget 的门面层**，这条分层方向是本件的硬约束。
4. **凭据脱敏：两轨分明。** 第一轨**库内强制、不可关**：`Authorization` / `Proxy-Authorization` / `Cookie` / `Set-Cookie` 在日志、时间线、Inspector 响应、错误 `message` 四出口一律打码，形态只留认证方案与长度（如 `Bearer len=40`）。这是安全边界，宿主无权关闭。第二轨是**进程级 Filter 注入口**（新公共头 include/aurora/net/export_filter.h，命名空间 `aurora::net`）：`ExportFilter` 抽象类的取值口收「导出面 + 键 + 原值」三枚、交出呈现后的文本，导出面由公共枚举 `ExportSurface` 表达（`Log` / `Timeline` / `InspectorJson` / `McpJson` / `ErrorDetail`），宿主注册后覆盖四枚之外的任意键值（典型现场即 §1.1 的 TextInput 文本里的 token）。注入形态照 `net::set_default_policy()` / `default_policy()` 的进程级三级取定与内部加锁（按值往返、锁外拿不到引用、传空即清回「未设」）。归属层选 `aurora::net` 而非 `aurora::core` 的理由写进头注释：Inspector 已依赖 net（实测 `src/aurora/inspector/inspector_server.cpp` 已 include `include/aurora/net/blocking.h`），依赖方向本就成立；挂进 core 会让 L0 长出「导出面」这一非 core 概念，挂进 `aurora::debug` 则违反上一条分层方向。同批落 `codespec/CODING_STANDARDS.md`：§4.1 双通道段补一条硬规则（凭据一律打码、四出口同口径），§15 反例速查加一条反例。判据形态是**四出口同一断言集**——同一份带凭据的交换在四条路上各问一遍，逐出口断言原值不出现、打码形态出现。
5. **端点与工具面。** 注册表加行即挂两枚端点：`GET /api/network`（活动表快照，经 Filter 与强制脱敏）与 `GET /api/network/request?id=<X-Request-Id>`（单条请求的时间线展开），两枚都以精确项登记。MCP 侧 `tools/servers/aurora_mcp.cpp` 的唯一登记点 `tool_definitions()`（实测现 13 枚）加两枚：`list_network_activity{limit?, host?, status?}` 与 `describe_network_request{request_id}`，13 → **15**；`codespec/specification/08-tooling.md` §10.6 那句「暴露 13 个 MCP tools」的数字与清单同批改，§10.2 补网络可观测细则（那行现状仍是纯 UI Inspector 条目）。CLI 侧不同步——提案未要求，`aurora_cli` 子命令不在本件范围。
6. **交付批次与每批门禁（按依赖切三批，每批自带单测与文档回写，不跨批留红灯）。** 批一 = 本条第 1 项（零公共面，判据是既有 27 例逐端点响应逐值对照 + 新增两重次序断言 + 变异自证）；批二 = 第 2 / 3 / 4 项（新增公共枚举与旋钮，判据是阶段单调、容量淘汰、四出口同一断言集，门禁含 `check_api_schema_sync` / `check_api_budget` / `check_umbrella_header`）；批三 = 第 5 项（工具面在 `tools/` 与 Inspector，判据是端点回环夹具 + MCP stdio 协议断言 + `check_codespec_xref` / `check_manual_test_format`）。三批各自的 `codespec/` 回写随批落地，末批补齐 §16 追加裁决与 `CHANGELOG.md`。

公共面形态（本提案定形，落码逐名照此）：

```cpp
enum class HttpStage : std::uint8_t { Dns, Connect, TlsHandshake, FirstByte, BodyDone };

struct HttpRequestTimeline {
    std::string request_id;                                    ///< 与 X-Request-Id 同源
    std::chrono::milliseconds total{0};                        ///< 本跳总耗时
    std::vector<StageMark> marks;                              ///< 每阶段：偏移 + 累计收发字节
};

class Response {
public:
    [[nodiscard]] auto timeline() const -> std::optional<HttpRequestTimeline>;  ///< 宏关时恒 nullopt
};

struct ActivityOptions {
    bool enabled = true;
    std::size_t capacity = 64;        ///< in-flight + 已完成共用的环形容量
};

class Client {
public:
    auto activity(ActivityOptions options) -> Client &;
    [[nodiscard]] auto activity_options() const -> ActivityOptions;
    [[nodiscard]] auto activity_entries() const -> std::vector<HttpActivityEntry>;
};

enum class ExportSurface : std::uint8_t { Log, Timeline, InspectorJson, McpJson, ErrorDetail };

class ExportFilter {
public:
    virtual ~ExportFilter() = default;
    [[nodiscard]] virtual auto present(ExportSurface surface, std::string_view key,
                                       std::string_view value) const -> std::string = 0;
};

auto set_export_filter(std::shared_ptr<const ExportFilter> filter) -> void;  ///< 传空 = 清回未设
[[nodiscard]] auto export_filter() -> std::shared_ptr<const ExportFilter>;
```

## 验收判据

1. **REST 逐值不变（批一）**：以 `tests/unit/utest_inspector_server.cpp` 的 `http_roundtrip` 一族为底座，全部现存端点（16 枚精确 + 2 枚前缀族的子路径）跑同一组请求/响应对，响应体、状态码、405 与 404 文案逐值对照；两枚前缀族的 label 不带尾斜杠这一形态另有断言。
2. **精确优先不变量（批一）**：断言不存在「恰等于某精确路由又落在两枚前缀族下」的路径，故两趟查找不改变任何现存路径的分派——这条写成用例钉住，不只写进注释。
3. **方法闸交集纠回（批一）**：`DELETE /api/widget/`、`/api/widget//` 一类「动词不支持且路径退化」的交集回 405，其余路径的 400 / 405 / 404 分流与改造前逐值相同；该差异与理由同批登记为追加裁决。
4. **变异自证非空转（批一，至少三处）**：① 两趟查找顺序颠倒 ⇒ 次序断言例转红；② label 取错形态（前缀族带上尾斜杠）⇒ 405 文案断言转红；③ 把某枚前缀项误登记成精确项 ⇒ 该族全部子路径转 404。
5. **阶段单调与一跳一条（批二）**：任一 `HttpRequestTimeline` 的 `marks` 偏移单调不减且均 ≤ `total`；带一次重定向 + 一次重试的交换产出多条条目、同一枚 `request_id` 串起、条数恰等于跳数 × 尝试数；`send` / `open_stream` / `open_sse`（含每次重连）各产出条目，`connect_websocket` 只产出握手那一条且会话存续期间 in-flight 计数不因它增长。
6. **容量与淘汰（批二）**：写满 `capacity` 后淘汰的是最旧的**已完成**条目，in-flight 条目恒在场；`enabled = false` 时表空且 `Client` 行为与改造前逐值相同。
7. **Release 零占位（批二）**：`AURORA_ENABLE_DEBUG=OFF` 腿以 `static_assert` 断 `sizeof(Response)` 回到改造前的字节数、进程名册符号不存在；`ON` 腿断 `timeline()` 有值——两腿各一枚同名用例，跳过形态按 `AURORA_TEST_SKIP` 纪律登记。
8. **四出口同一断言集（批二）**：同一份带 `Authorization` / `Cookie` 的交换，在日志行、`timeline()`、Inspector JSON、错误 `message` 四条路上各断言一遍：原值不出现、打码形态出现；Filter 注册后额外键值按其呈现、未注册时库不猜；四枚凭据字段在宿主注册「原样返回」的 Filter 下**仍打码**（证明不可关）。
9. **端点与工具往返（批三）**：`GET /api/network` 与 `GET /api/network/request?id=` 走 `http_roundtrip` 回环夹具逐值断言（含 `available:false` 形态与非法 id）；MCP 侧 `tool_definitions()` 报 15 枚、两枚新工具的 stdio 往返各一条断言（含缺省 limit 与按 host / status 过滤）。
10. **手工用例（批三）**：`codespec/manual-test/23-net.md` 新增 TC-NET-008，按 §1.1 第二类准入（真实 Inspector 进程 + MCP stdio 往返）人工核对端点 JSON、时间线各阶段与两枚工具的实际返回，执行记录同批填入该文档 §3。
11. **门禁全绿**：`ctest --preset ninja-test` 全量（含全部 `check_*`），其中 `check_api_schema_sync`（先 `--target aurora_api_json`）、`check_api_budget`、`check_naming_conventions`、`check_doc_comments`、`check_umbrella_header`、`check_core_layer_boundary`、`check_arch_module_map`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`check_no_cjk_literals`、`check_no_hardcoded_paths`、`check_version_consistency`；改动 TU 逐个过 `lint`，`git add` 之后复跑 `format-check`。

## 回写落点

1. `codespec/specification/06-app-platform.md`：新增三枚小节（§8.8.36 请求时间线与活动表、§8.8.37 凭据强制脱敏与进程级 Filter、§8.8.38 Inspector 路由注册表与 network 端点），各含契约、线程口径、淘汰与门控纪律、判据与变异自证记录。
2. `codespec/specification/08-tooling.md`：§10.2 补网络可观测端点与脱敏细则，§10.6 的 MCP 工具数与清单从 13 改 15。
3. `codespec/SPECIFICATIONS.md`：§5 特性表 `SPEC.FEAT.NET.OBSERVABILITY.001` 行的「规格落点」列补新小节指针，判据措辞随实现形态收口。
4. `codespec/CODING_STANDARDS.md`：§4.1 双通道段补「凭据一律打码、四出口同口径」硬规则，§15 反例速查加一条对应反例。
5. `codespec/BUILD_OPTIONS.md`：`AURORA_ENABLE_DEBUG` 条目登记新增的那处后果——该宏自此改变 `Response` 内存布局并决定进程名册是否存在，消费端须同值；门面新增两枚能力的可见性随条说明。
6. `codespec/ARCHITECTURE.md`：§4 的 net 行补时间线与活动表归属，§14 的测试与 CI 段登记 Inspector 路由注册表与两枚端点的回归口径。
7. `codespec/manual-test/23-net.md`：§1.1 被测模块表补新公共头一行，§2 用例清单加 TC-NET-008，§3 执行记录表随批填写。
8. `codespec/changes/xplat-network-stack/proposal.md`：§13 四条承诺各补「落地现状」段；§16 追加裁决条目——本提案定案取代第 9 项后半（取消 lint 规则那条，旧编号与旧文字保留）、给第 8 项的注册表化补方法闸统一收派发处的纠回与 400→405 差异、给第 5 项的两级查找补两趟实现的定形；§17 的可观测性条目随批收口。
9. `codespec/errors.toml`：本件不新增码；若落码中出现「时间线/活动表自身的失败形态」一律走既有码并在 detail 点名，届时按实际分支决定是否需要条目 hint 改写（默认：无改动）。
10. 代码与数据面（不属 codespec 树，此处只记必做动作）：根目录 CHANGELOG.md 记 Added 段（新公共枚举 / 结构体 / 旋钮 / 新头 / 两枚端点 / MCP 15 枚）与 Changed 段的 Inspector 405 差异；根目录 aurora_api.json 经 `--target aurora_api_json` 重建；tools/check/umbrella_manifest.txt 收录新公共头。
