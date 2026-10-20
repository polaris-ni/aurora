# CHG-025 网络可观测性：事件总线与消费者订阅、Inspector 路由注册表、请求时间线与活动表、凭据分级脱敏、network 端点与 MCP 工具

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-025 |
| 提出日期 | 2026-10-20 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.FEAT.NET.OBSERVABILITY.001 |
| 影响面 | 跨三层的一次增量交付，**无破坏性签名变更、无新增错误码、且不改动任何既有类型的内存布局**（此前拟给 `Response` 加一枚按宏裁切数据成员的形态一并废弃，理由见动机第 6 项）。（1）Inspector：`src/aurora/inspector/inspector_server.cpp` 里 `route_request` 的 if 链（实测 16 枚精确路由 + 2 枚前缀族交错，函数体 L327–L1053）改为文件内静态注册表，公共面零变化；随之有一处 REST 行为纠回——「动词本就不被支持 **且** 路径恰好退化」这一交集（`DELETE /api/widget/` 一类）由 400 变 405。（2）记账一次、消费多路：新增公共头 include/aurora/net/observability.h（`ExportSurface` / `FieldClass` 两枚枚举、`ExportPolicy` 抽象类、`NetEventSink` 消费者接口、`subscribe_net_events()` / `install_export_policy()` 两枚进程级注入口），库侧三处 emit 点（起手 / 阶段到达 / 收摊），`AURORA_ENABLE_DEBUG=OFF` 时总线与三处 emit 点一并编译裁切。（3）HTTP 值层：新增公共枚举 `HttpStage` 与 `StageMark` / `HttpRequestTimeline` / `HttpActivityEntry` 三枚结构体，`Client` 新增 `ActivityOptions` 旋钮，另加两枚自由查询 `timeline_of()` / `activity_snapshot()`；`Response` **零改动**。（4）脱敏：从「四枚头部写死」升级为「三类灵敏度分级」（`Public` / `Sensitive` / `Secret`）——分级由库侧裁决、宿主只能收窄不能放大，覆盖面从四枚头部扩到 URL 的用户信息段、查询串令牌与消息体；第二条泄漏路径（`/api/tree` 与 `/api/widget/{path}` 全量导出控件属性）仍经 `InspectorJson` 面交给宿主裁决。（5）暴露面：`aurora::debug` 门面新增两枚 JSON 能力，Inspector 新增 `GET /api/network` 与 `GET /api/network/request?id=` 两枚端点，MCP 工具 22 → 24。（6）`aurora_api.json` 随 `--target aurora_api_json` 重建（enums 段 +3）、`tools/check/umbrella_manifest.txt` 收录新头、`codespec/manual-test/23-net.md` 新增一枚用例 |

## 动机

1. **本库的立身之处要求这件事是核心交付面，而不是附加项。** `codespec/SPECIFICATIONS.md` §5 已把「AI 能从一次界面异常反查到一次请求的耗时与失败阶段，且排障信息不会泄漏凭据」写成 `SPEC.FEAT.NET.OBSERVABILITY.001` 的判据；网络又是最难事后排查的一类 I/O（现场不可复现、对端不可控、失败形态跨六档归因）。而这条需求今天**没有任何实现承担**——跨平台网络栈那伞形提案（`codespec/changes/xplat-network-stack/proposal.md`）的 §17 落地序里，底座、传输、TLS、HTTP、绑定、SSE、WebSocket 七件都已收口，`codespec/specification/06-app-platform.md` §8.8 现状段记明此刻仍未落地的是**可观测性**与 §6 发起侧的 `Client::use(hook)` 两件；本切片只取可观测性一件，`use(hook)` 不在本提案范围、留待后续提案收口。
2. **既有埋点已经全部就位，缺的只是消费者。** 发起侧 `StageTimer` 的 connect / read_idle / total 三档已挂在阶段边界，`detail::await_connect_via` 是全库唯一建连出口，`stamp_request_id` 与 `log_exchange_identity` 那两枚起手行在位；一次交换一枚、跨重定向与重试恒同一枚的 `X-Request-Id` 也已随链路标识件落地。也就是说「耗时与失败阶段」的数据源只差「到点记一笔」这一步。不接这层，链路标识那件的「反查」承诺只到对端 access log 为止——本端什么都问不出来。
3. **每加一枚端点必改一枚千行巨函数。** 实测 `src/aurora/inspector/inspector_server.cpp` 全文 1306 行，`route_request` 一条 if 链占了 L327–L1053，其中**精确匹配与前缀匹配交错并存**（`route == "/api/debug/state"` 一类与 `route.starts_with("/api/widget/")`、`starts_with("/api/input/")` 两类穿插）。本切片要挂两枚端点，先做注册表化才能无侵入落地——这条前置改造已由那份提案的 §16 第 8 项裁决并入网络栈范围，本提案把它连同判据一起定形。
4. **凭据入日志今天零防线，且泄漏面比提案设想的更宽。** 实测全域（`src/`、`include/`、`tools/`）对 `redact` / `脱敏` / `打码` 空命中（`mask` 仅命中 WebSocket 帧掩码，协议特性而非脱敏）；`include/aurora/core/log.h` 的 `Logger` 只有级别阈值、sink 重定向、raw 通道重定向与开关四类旋钮，**没有任何脱敏接缝**；`codespec/CODING_STANDARDS.md` 内「凭据 / Authorization / Cookie」零命中。更关键的是第二条泄漏路径不在日志里：Inspector 的 `/api/tree` 与 `/api/widget/{path}` 会把控件属性全量导出（含 TextInput 的文本），宿主塞进输入框的 token 直接进 JSON。因此本提案把脱敏定成**两轨**——自己知情的那一半由库强制裁决，宿主知情的那一半（控件属性）经一枚进程级 Policy 交给宿主。分工理由写进变更内容第 4 项：协议事实（头部、URL、消息体、凭据）只有库知情，控件属性只有宿主知情。
5. **一处已定案的执行强度在仓内无法落地，须纠回。** 那份提案的 §16 第 9 项定的是「条款 + 四出口单测断言 + lint 规则」。实测仓内没有任何既有 `check_*` 扫描器能自然承载「凭据不得入日志」这条规则，clang-tidy 亦无自定义正则检查能力；硬造一枚 `check_*` 只会多一个需要维护豁免的扫描器。故本提案纠回为「条款 + 四出口同一断言集 + `codespec/CODING_STANDARDS.md` §15 一条反例」，旧编号与旧文字保留、追加裁决取代其后半（静态扫描抓不到运行时凭据值，这条理由同批写进裁决）。
6. **「关闭时零开销」与「公共头维持零 `#ifdef`」两条口径不必再二选一——只要把观测数据搬出应答对象。** 前者的原文承诺是「数据成员不占位」，后者是网络栈公共头的既有纪律。此前几稿沿仓内先例（`include/aurora/widget/widget.h` 在公共头内以 `#ifdef AURORA_ENABLE_DEBUG` 裁切数据成员，`cmake/AuroraFeatures.cmake` 与 `cmake/AuroraInstall.cmake` 明写该宏改变类布局、故由 PUBLIC 导出、消费端须同值）决定继承该例外，代价是 `Response` 的尺寸自此随宏变化、须登记 `codespec/BUILD_OPTIONS.md` 并长期背一条 `sizeof` 判据。**本提案不继承这个例外**：撤掉拟增的 `Response::timeline()` 与那枚数据成员，时间线改由 `http::timeline_of(request_id)` 向总线索取。于是 Release 照样零开销，而代价从「一个公共类型的形状随构建而变」降为「总线自身在 Release 不存在」——后者只是自有实现里一处可裁切的东西，不必向消费端解释同一个结构体为什么有两种尺寸。
7. **「每多一路读者就在各对象里多存一份」撑不住多路消费，业界通行的是推模型的一次记账。** OkHttp 的 `EventListener` 不在 `Response` 上存任何东西，只在生命周期边界回调宿主注册的那枚 listener，并给出三条硬契约：**每个 begin 事件最终必有一个配对的 end 事件**（成功或失败必居其一）、**所有回调必须快、不得抛异常、不得持外部锁、不得回调时重入 client**（写盘与网络 I/O 一律异步）、**复用连接的那次通话不发 DNS 与建连类事件**；它的 `Factory` 同时也是官方给出的采样形态（随机 10% 给真 listener，其余给 `EventListener.NONE` 这枚缺省空实现）。Chromium 同构：全局一枚 `NetLog`，宿主注册观察者，参数经回调惰性取值——没人观察时连字符串都不必拼。两家的形态不同但取向一致：**事实只在边界上被记一次，保留与折算交给愿意听的人**。本提案据此把此前那版「每个 `Client` 一张环形表 + 一张进程级 weak 名册由门面逐个 `lock()` 取样」两层併成一层——**总线本身就是名单**，`Client` 只是它的一枚自带订阅者；日后要加指标计数、落盘、外部追踪格式，都不必改 net 一行。
8. **安全边界的最后写字人不能是宿主。** Chromium 把最重的一档做成中央 allowlist：`net_log_capture_mode.h` 明写「individual netlog entry producer callsites do not have any say in this. This is by design to make it harder for a given producer to accidentally log sensitive data」。此前那版的形态是「库算出原值 → 宿主决定呈现 → 宿主返回的字符串就是最终落出去的文本」：宿主实现里一处疏忽、或一枚「原样返回」的调试桩忘了摘，都能把原值写出去，而这条下游就是 AI 会话。更实际的是泄漏面：实测 `include/aurora/core/url.h` 的 `Url` 导出 `userinfo()` 与 `query()` 两枚只读口，`https://user:pass@host/` 与 `?access_token=...` 都不落在任何头部里——只盯四枚头部等于承认「URL 里夹带的令牌可以明文直行」这条后门（OAuth 回调里带 `access_token` 正是它的常见形态）。故本提案把「四枚字段写死」升级为「三类灵敏度分级」：灵敏度由库侧裁决且宿主恒拿不到原值，宿主自定义的那一轨只能收窄（补进需要隐瞒的键），不能放大（无权放行库侧认定的 `Secret`）。
9. **有界性必须是预算，而不是仁慈。** 此前那版的淘汰口径是「写满时只淘汰最旧的已完成条目，in-flight 永不淘汰」，它隐含假设在途条目终会收摊。真实形态是「对端不收摊 + 应用持续发起」这一组合会把环形表全部换成 in-flight，随后**新事实再也记不进去**——观测恰好在它最被需要的那一刻哑掉。业界给出现成口径：OkHttp 的「每个 begin 必有配对 end」把「一定会收摊」写成 API 契约，`Client` 析构与业务取消是它必须结清的场合；Chromium 的 NetLog 有 `maxFileSize`（到量自动停记）；OpenTelemetry 给每 span 的属性数与事件数设了默认 128 的上限，并要求超限采取截断与丢弃而不是无限增长。本提案据此改两条不变量：**在途条目至多 N 条，超出则以 `abandoned` 强制收摊**，以及**单字段文本与单跳阶段数各有一枚上限**。

## 变更内容

1. **Inspector 路由注册表化（前置件，零公共面）。** `route_request` 的 if 链改为文件内静态数组，条目字段 `pattern / kind(Exact|Prefix) / methods / label / handler`；handler 取**指向 `InspectorServer::Impl` 成员函数的指针**而非 `std::function`（仓内 `.clang-tidy` 已记录「任何转入 `std::function` 的可调用对象一律判为不应抛出」这条系统性假告警面，不值得为一张表去招它）。查找做**两趟**：先整表扫精确项，再按登记次序扫前缀项——这就是那份提案 §18 第 5 项要求的「显式优先级字段而非依赖插入顺序」。方法闸**统一收在派发处**：`405` 说的是「这个端点不接受这个动词」，而 `DELETE /api/widget/` 现在先回 400「路径为空」——路径形状对与错都改变不了动词不被支持这一更基本的事实；收在派发处后，方法闸成为端点表**说出一次**的属性，每个 handler 可假定进到手里的方法就是它声明过的那枚。行为差异面只有那一个交集，故**记为纠回**：追加裁决写明差异、理由与判据用例，旧编号旧文字保留；该交集现状无任何用例覆盖，新增用例是**补判据**而非改判据。handler 体内逻辑逐字搬移、一行不改，`path` / `route` / `method` / `body` 四枚经一枚 `RouteCall` 形参交入（`path` 仍为 `const std::string &`，因 `query_param` 签名如此）。405 文案与 404 兜底逐值不变（前缀族的 label 不带尾斜杠、404 用含查询串的原样 path）。
2. **事件总线与消费者订阅（取代「每 Client 一张表 + weak 名册」两层结构）。** 新增抽象消费者 `NetEventSink`，三枚虚方法 `on_exchange_begin` / `on_stage` / `on_exchange_end` **皆默认空实现**——照 OkHttp `EventListener` 的形态：宿主只覆盖关心的那一枚，「没人听」就是什么都不算，而不是空转一枚什么都不做的 `std::function`。订阅口 `subscribe_net_events()` 收 `std::weak_ptr<NetEventSink>`，**宿主放手即自动退订**：不必另设注销 API，也不会出现「总线里挂着一枚已析构消费者」这种要靠开发伦理去避免的形态——此前那版用 weak 名册解决的问题，在这里由订阅名单本身解决，概念从两个降到一个。库侧 emit 只有三处（起手、阶段到达、收摊），三处全部 `noexcept`、不得抛。三条契约照抄进头注释（**快、不得抛、不得回调时重入 net 或做 I/O**），它们不是礼貌而是稳定性口径：任何一枚消费者回调都运行在发起侧的栈上，一次重入或一次磁盘 / 网络 I/O 会把整条发起链路拖在它身后。Release（`AURORA_ENABLE_DEBUG=OFF`）时总线与三处 emit 点一并编译裁切，进程内无全局可变状态；一名消费者都没订阅时，emit 只是一次 relaxed 原子读后返回。`Client` 缺省持有一枚库内的环形消费者（`RecentActivitySink`，`ActivityOptions` 给定容量与上限），**缺省开**（与 `PoolOptions` / `CacheOptions` 的缺省关相反且刻意：那两件改变的是已存在的交换何时不再发生，而本件是唯一「出问题当场问不出来就永久问不出来」的那件，缺省关掉等于「AI 可观测运行时」要配置才成立）。由此派生的两枚自由查询都落在总线之上：`timeline_of(request_id)` 取代此前拟加的 `Response::timeline()`；`activity_snapshot(limit)` 取代此前那张由门面逐个 `lock()` 扫描的名册快照——两者向总线问同一件事，环形消费者只是默认挂上的那枚订阅者。
3. **请求时间线与活动条目（值层；`Response` 不参与）。** 新增公共枚举 `HttpStage`（`Dns` / `Connect` / `TlsHandshake` / `FirstByte` / `BodyDone`，登记 `tools/include/known_enums.h` + `tests/integration/itest_known_enums.cpp` 编译期锚点）与三枚结构体 `StageMark` / `HttpRequestTimeline` / `HttpActivityEntry`；**阶段划分不新造**——埋点只加在既有三档与建连出口上，「到点记一笔」。粒度：**一跳一条**，`timeline_of()` 交出本次收摊那一跳（查无此标识时回 `nullopt`）；完整跳序列由 `activity_snapshot()` 与 `GET /api/network/request?id=` 供远端展开——重定向的每一跳、重试的每一次尝试各一条，由同一枚 `X-Request-Id` 串起，并以 `hop` / `attempt` 两枚序标标出层级。 **阶段允许缺席**：走池内复用连接的那一跳本就没有 `Dns` / `Connect` / `TlsHandshake` 三枚 mark（OkHttp 明确如此：复用那一跳从 `callStart` 直接到 `connectionAcquired`），故时间线带一枚 `reused_connection` 标志把「缺席」与「记漏」区分开，而不是让消费者对着一枚 0 去猜。时刻一律取自 `std::chrono::steady_clock`（墙钟会被 NTP 校时推着走，拿它做差值会得出负数或突跳，那是 bug 而不是精度问题），单位微秒——毫秒会把池化复用与本地回环那一类亚毫秒交换整片量化成 0，而它们恰恰是性能排障最常问的那一段。失败归因复用既有纯函数 `error_class_of()` 得出 `HttpErrorClass`（实测该档已是九档制，且复用既有那枚有着唯一真值源的纯函数，本提案不另造一套失败分类），并额外记 `failed_stage`（最后到达的阶段），使「失败在哪一跳的哪一阶段」一处即可读出。覆盖面由不变量决定而非按后端列举：判据是「一次会完成的交换 = 一条条目」——`send` 与 `open_stream` 都有 Body 完成终点，全量收；`open_sse` 的每条流（含每一次重连）本身就是一次有始有终的 `open_stream`，按每条流一条收、继承同一批埋点；`connect_websocket` 只收**握手**那一条，升级后的会话**不进请求活动表**（会话的收摊事实已在 `WsCloseInfo` 里，属会话观测而非请求观测）。会话级观测（连接时长、消息计数）另登待办，不在本提案范围。
4. **凭据脱敏：三类灵敏度，库只对自己知情的那半做主。** 分级 `FieldClass{ Public, Sensitive, Secret }`（同批登记 `tools/include/known_enums.h` 与 `tests/integration/itest_known_enums.cpp` 锚点，故 enums 段 +3）。归属：**库对自己知情的那一半做主**（头部、URL 的用户信息段、查询串令牌、消息体），**宿主对自己知情的那一半做主**（控件属性）。宿主这一轨的形态是单向的：`Secret` 类由库侧折叠之后才交给 `ExportPolicy`，宿主恒看不到原值——`present()` 收到的就是已折叠形态，于是宿主实现即使写成「原样返回」，落出去的也只能是折叠形态；`Sensitive`（其余头部、`Referer`、查询串剩余部分、URL 剩余分量）默认折叠，须经 Policy 按「导出面 + 键」显式放行；`Public`（方法、主机、状态码、字节数、时刻、请求标识）原样导出。`Secret` 的最小集：**四枚既有字段**（`Authorization` / `Proxy-Authorization` / `Cookie` / `Set-Cookie`）**加上此前漏掉的三处**——`Url::userinfo()` 带出的 `user:pass@`、查询串中名字命中令牌名单的参数（`token` / `access_token` / `refresh_token` / `id_token` / `api_key` / `key` / `secret` / `signature` / `sig` / `session` / `password` / `code`）、以及**恒不导出的消息体**；折叠形态只留认证方案与长度（如 `Bearer len=40`）。泄漏的第二条轨迹（`/api/tree` 与 `/api/widget/{path}` 全量导出控件属性）走 `InspectorJson` 导出面交宿主裁决：这不是库推卸责任，而是那些属性的敏感性只有宿主知道（库若把整棵树都当作可疑对象一律折叠，等于把 Inspector 的首要功能——高保真导出界面状态——给废掉）；未注册 Policy 时这两枚端点输出逐值不变，判据第 1 项的对照组天然覆盖。注入形态照 `net::set_default_policy()` / `default_policy()` 的进程级三级取定与内部加锁（按值往返、锁外拿不到引用、传空即清回「未设」）。归属层选 `aurora::net` 而非 `aurora::core` 的理由写进头注释：Inspector 已依赖 net（实测 `src/aurora/inspector/inspector_server.cpp` 已 include `include/aurora/net/blocking.h`），依赖方向本就成立；挂进 core 会让 L0 长出「导出面」这一非 core 概念，挂进 `aurora::debug` 则违反分层方向（**net 不得反向依赖到依赖 widget 的门面层**，这条沿用网络栈既有纪律，本提案不改）。同批落 `codespec/CODING_STANDARDS.md`：§4.1 双通道段补一条硬规则（凭据一律折叠、四出口同口径、库侧 `Secret` 不可由宿主放大），§15 反例速查加一条反例。判据形态是**四出口同一断言集**——同一份带凭据的交换在四条路上各问一遍，逐出口断言原值不出现、折叠形态出现。
5. **端点与工具面。** 注册表加行即挂两枚端点：`GET /api/network`（活动快照，经 `FieldClass` 分级与强制折叠）与 `GET /api/network/request?id=<X-Request-Id>`（单条请求的多跳展开 + 各跳的 `StageMark`），两枚都以精确项登记。MCP 侧 `tools/servers/aurora_mcp.cpp` 有**两处登记点**——`tool_definitions()` 的声明表与 `execute_tool()` 的派发链（实测各 22 条）——两枚新工具须同改两处：`list_network_activity{limit?, host?, status?}` 与 `describe_network_request{request_id}`，22 → **24**；本切片不改造 MCP 的登记形态（「加一枚工具要动两处」与动机第 3 项批判的 Inspector 旧形态同款，收口留待后续提案），落地按同一份清单同批改毕。`codespec/specification/08-tooling.md` §7.1 工具清单表与 §10.6 那句「暴露 13 个 MCP tools」的数字同批改：§7.1 现存 13 行，较代码少九行存量漂移（`live_patch` / `live_simulate` / `live_tree` / `live_widget_get` / `live_widget_set` 五枚与 `build_ui_prompt` / `compare_snapshot` / `generate_ui` / `repair_tree` 四枚），借此同批补齐，否则新表与代码继续对不上；§10.2 补网络可观测细则（那行现状仍是纯 UI Inspector 条目）。CLI 侧不同步——提案未要求，`aurora_cli` 子命令不在本件范围。
6. **交付批次与每批门禁（按依赖切三批，每批自带单测与文档回写，不跨批留红灯）。** 批一 = 本条第 1 项（零公共面，判据是既有 27 例逐端点响应逐值对照 + 新增两重次序断言 + 变异自证）；批二 = 第 2 / 3 / 4 项（新增公共头、枚举与旋钮，判据是一次记账多路消费、生命周期配对、有界预算、阶段单调与缺席、四出口同一断言集，门禁含 `check_api_schema_sync` / `check_api_budget` / `check_umbrella_header`；第 4 项带入的两枚既有 Inspector 导出端点接入 `InspectorJson` 面亦在本批落地）；批三 = 第 5 项（工具面在 `tools/` 与 Inspector，判据是端点回环夹具 + MCP stdio 协议断言 + `check_codespec_xref` / `check_manual_test_format`）。三批各自的 `codespec/` 回写随批落地，末批补齐 §16 追加裁决与 `CHANGELOG.md`。

公共面形态（本提案定形，落码逐名照此）：

```cpp
// 以下均属命名空间 aurora::net::http；Response 与 Client 是既有类，块内只列本提案新增面。
// 值层四枚（HttpStage / StageMark / HttpRequestTimeline / HttpActivityEntry）与 ActivityOptions
// 落 include/aurora/net/http.h，Client 的三枚新口落 include/aurora/net/http_client.h。
// 两枚自由查询亦落 include/aurora/net/http.h，与既有 error_class_of() 一族同位。
// HttpErrorClass 是既有枚举，与本块新增面同在该头内，声明次序按该头既有格局安排。
namespace aurora::net::http {

enum class HttpStage : std::uint8_t { Dns, Connect, TlsHandshake, FirstByte, BodyDone };

struct StageMark {
    HttpStage stage;                              ///< 阶段
    std::chrono::microseconds offset{0};          ///< 相对起手的偏移（steady_clock）
    std::uint64_t bytes{0};                       ///< 截至本阶段累计收发字节
};

struct HttpRequestTimeline {
    std::string request_id;                       ///< 与 X-Request-Id 同源
    std::uint16_t hop{0};                         ///< 第几跳（重定向逐个递增）
    std::chrono::microseconds total{0};           ///< 本跳总耗时
    std::vector<StageMark> marks;                 ///< 已到达的阶段（缺席表示本跳未经历该阶段）
    bool reused_connection{false};                ///< 是否复用池内连接：为真时无 Dns/Connect/Tls 三枚 mark
};

struct HttpActivityEntry {
    std::string method;                           ///< 请求方法
    std::string host;                             ///< 目标主机（含端口；永不含用户信息段）
    std::uint16_t hop{0};                         ///< 第几跳
    std::uint16_t attempt{0};                     ///< 本跳第几次尝试
    std::uint16_t status{0};                      ///< 状态码；未完成与失败置 0
    std::string error_code;                       ///< 失败时的错误码；成功为空
    HttpErrorClass error_class{HttpErrorClass::Invalid};  ///< 失败归因档（由既有 error_class_of() 得出）
    std::optional<HttpStage> failed_stage;        ///< 失败时最后到达的阶段；成功为 nullopt
    std::chrono::microseconds elapsed{0};         ///< 耗时；进行中为起手至今
    std::uint64_t sent{0};                        ///< 发送字节数
    std::uint64_t received{0};                    ///< 接收字节数
    std::string request_id;                       ///< 与 X-Request-Id 同源
    bool in_flight{false};                        ///< 是否仍在途
    bool abandoned{false};                        ///< 是否因宿主析构/取消/容量压力被动收摊
    bool reused_connection{false};                ///< 本跳是否复用池内连接
};

struct ActivityOptions {
    bool enabled = true;
    std::uint32_t capacity = 64;        ///< 环形容量（在途 + 已完成共用）
    std::uint32_t max_value_chars = 256;  ///< 单字段文本上限；超出按尾截断并只记一次丢弃
    std::uint8_t max_marks_per_hop = 8;   ///< 单跳阶段数上限；超出丢弃后续并只记一次
};

class Client {
public:
    auto activity(ActivityOptions options) -> Client &;
    [[nodiscard]] auto activity_options() const -> const ActivityOptions &;
    [[nodiscard]] auto activity_entries() const -> std::vector<HttpActivityEntry>;
};

// 两枚自由查询取代此前拟加的 Response::timeline()：答案由总线的订阅者供给，无人订阅即为空。
[[nodiscard]] auto timeline_of(std::string_view request_id) -> std::optional<HttpRequestTimeline>;
[[nodiscard]] auto activity_snapshot(std::uint32_t limit) -> std::vector<HttpActivityEntry>;

}  // namespace aurora::net::http

// 新公共头 include/aurora/net/observability.h，命名空间 aurora::net。
namespace aurora::net {

enum class ExportSurface : std::uint8_t { Log, Timeline, InspectorJson, McpJson, ErrorDetail };
enum class FieldClass : std::uint8_t { Public, Sensitive, Secret };

/// @note Thread: present() 可在任意调用 net 的线程上被调用；实现须可重入且不得持有外部锁。
/// @note Side-effects: none（返回值即最终呈现；实现内不得做 I/O）
class ExportPolicy {
public:
    virtual ~ExportPolicy() = default;
    /// @param value 已按 field_class 折叠后的取值；field_class == Secret 时宿主恒拿不到原值。
    [[nodiscard]] virtual auto present(ExportSurface surface, std::string_view key, FieldClass field_class,
                                       std::string_view value) const -> std::string = 0;
};

auto install_export_policy(std::shared_ptr<const ExportPolicy> policy) -> void;  ///< 传空 = 清回未设
[[nodiscard]] auto export_policy() -> std::shared_ptr<const ExportPolicy>;

/// @note Thread: 三枚回调在发起侧所在线程上被调用；NetEventSink 自身不加锁。
/// @note Side-effects: none（回调须快、不得抛、不得回调时重入 Client 或做 I/O）
class NetEventSink {
public:
    virtual ~NetEventSink() = default;
    virtual auto on_exchange_begin(const http::HttpActivityEntry &entry) noexcept -> void {}
    virtual auto on_stage(std::string_view request_id, std::uint16_t hop,
                          const http::StageMark &mark) noexcept -> void {}
    virtual auto on_exchange_end(const http::HttpActivityEntry &entry) noexcept -> void {}
};

auto subscribe_net_events(std::weak_ptr<NetEventSink> sink) -> void;  ///< 宿主放手即自动退订

}  // namespace aurora::net
```

两处取舍注明在此，供评审专盯：其一，`NetEventSink` 的回调参数取**所有权形态的字符串**而非借用的 `std::string_view`（Chromium 的惰性取值把这枚拷贝推迟到有人真要留存为止）。一次交换只有三次回调、每份文本都被 `max_value_chars` 截在上百字节内，代价是一次短字符串构造而不是一次堆跨越；但借用视图会把「想留存就得自己拷」的负担推给每一个消费者，而绝大多数消费者都要留存——这里宁肯不那么极致的 emit 成本，也不把陷阱写进接口。其二，`subscribe_net_events()` 只收一枚 `weak_ptr` 而没有对称的注销口：整套退订依赖持有者析构，这是刻意的——对称 API 会引出一类「忘记注销」的运行期缺陷，而这个接口的调用方恰恰都是与进程同寿的东西（门面、环形消费者、将来的指标收集器）。

## 验收判据

1. **REST 逐值不变（批一）**：以 `tests/unit/utest_inspector_server.cpp` 的 `http_roundtrip` 一族为底座，全部现存端点（16 枚精确 + 2 枚前缀族的子路径）跑同一组请求/响应对，响应体、状态码、405 与 404 文案逐值对照；两枚前缀族的 label 不带尾斜杠这一形态另有断言。
2. **精确优先不变量（批一）**：断言不存在「恰等于某精确路由又落在两枚前缀族下」的路径，故两趟查找不改变任何现存路径的分派——这条写成用例钉住，不只写进注释。
3. **方法闸交集纠回（批一）**：`DELETE /api/widget/`、`/api/widget//` 一类「动词不支持且路径退化」的交集回 405，其余路径的 400 / 405 / 404 分流与改造前逐值相同；该差异与理由同批登记为追加裁决。
4. **变异自证非空转（批一，至少三处）**：① 两趟查找顺序颠倒 ⇒ 次序断言例转红；② label 取错形态（前缀族带上尾斜杠）⇒ 405 文案断言转红；③ 把某枚前缀项误登记成精确项 ⇒ 该族全部子路径转 404。
5. **一次记账、多路消费（批二）**：同进程内挂两枚测试消费者（一枚只累计计数、一枚留存全量），一次交换后两者收到同一组 `on_exchange_begin` / `on_stage` / `on_exchange_end` 且各自计数相等 ⇒ 事实只在边界记了一次；消费者析构后总线不再向它回调且此后发射不崩 ⇒ 弱引用退订生效；订阅名单为空时不建环、不记账 ⇒ 断言该状态下 `activity_snapshot()` 恒为空。本项用例落 tests/unit/utest_net_observability.cpp（本批新增文件，随 CMake 的 GLOB 入套件，无需手工登记）。
6. **生命周期配对（批二）**：正常、失败、取消三条路径各自恰好一枚 `on_exchange_end`，每条 `on_exchange_begin` 必有配对（以计数钉住：收摊次数等于起手次数，不许出现无配对的一端）；`Client` 析构时仍在途的条目就地以 `abandoned = true` 收摊且**不清空已登记的已完成条目**（共享形态：调用方持有的另一份拷贝仍见同一张表）；饿死不变量的构造方式：把 `capacity` 设为 2、连发三条永不收摊的请求，断言最旧的在途条目被强制收摊且标 `abandoned`，随后**新条目仍能进表**（饿死不变量）。
7. **有界预算（批二）**：超过 `max_value_chars` 的字段被截断而不是原样带走，且整张表只报**一次**丢弃（重复超限不再刷屏——照 OpenTelemetry 那条「同一记录上不得反复打 Log」的纪律）；超过 `max_marks_per_hop` 的多余 mark 被丢弃且同样只报一次；两枚上限改小后，同一份交换的呈现随之变短、各自的丢弃记录随之出现——证明上限生效而非摆设。
8. **阶段可选、缺勤与单调（批二）**：出现的若干枚 mark 偏移单调不减且均 ≤ `total`；走池内复用连接的那一跳断言没有 `Dns` / `Connect` / `TlsHandshake` 三枚 mark 且 `reused_connection` 为真（缺席必须有解释，而不是留一个 0 让消费者猜）；带一次重定向 + 一次重试的交换按 `hop` / `attempt` 展开、条数恰等于跳数 × 尝试数且同一枚 `request_id` 串起；`timeline_of()` 交出收摊那一跳、未知 id 回 `nullopt`。
9. **时钟与单位（批二）**：以 `static_assert` 钉住 `HttpRequestTimeline::total` 与 `HttpActivityEntry::elapsed` 的单位是 `std::chrono::microseconds`（避免毫秒把亚毫秒交换整片量化成 0）；实现口径——它们取自 `std::chrono::steady_clock`，net 与那枚新头的实现里不得出现 `system_clock`。
10. **Release 零全局状态（批二）**：`AURORA_ENABLE_DEBUG=OFF` 构建以 `static_assert` 断 `sizeof(Response)` 回到改造前的字节数（`Response` 零改动，这条判定应当自然成立；把它钉住正是为了预防日后把手再伸回应答对象），并断 `timeline_of()` / `activity_snapshot()` 恒回空、进程总线不产生可观测符号；`ON` 构建断两者有值——两种构建各一枚同名用例，跳过形态按 `AURORA_TEST_SKIP` 纪律登记。
11. **四出口同一断言集（批二）**：同一份带 `Authorization` / `Cookie` / URL 用户信息段 / 查询串令牌的交换，在日志行、`timeline_of()`、Inspector JSON、错误 `message` 四条路上各断言一遍：原值不出现、折叠形态出现；`Sensitive` 类键在未放行时折叠、按 `(导出面 + 键)` 放行后才出原值；**`Secret` 类在宿主注册「原样返回」的 Policy 下仍折叠**（证明不可放大）；**消息体恒不出现在任何出口**。Inspector 这一出口覆盖四处端点、同一口径：新增的 `GET /api/network` 两枚与既有的 `/api/tree`、`/api/widget/{path}`（全量导出控件属性，属宿主知情那一半）同标 `ExportSurface::InspectorJson`——同一份塞了 token 的 TextInput 树，未注册 Policy 时在两枚既有端点上原样导出（库不猜宿主数据的敏感性），注册后与两枚新端点的 JSON 按同一呈现一致折叠。net 侧三出口的用例落 tests/unit/utest_net_observability.cpp，Inspector 出口的用例落 `tests/unit/utest_inspector_server.cpp` 的 `http_roundtrip` 夹具。
12. **端点与工具往返（批三）**：`GET /api/network` 与 `GET /api/network/request?id=` 在 `tests/unit/utest_inspector_server.cpp` 里走 `http_roundtrip` 回环夹具逐值断言（含 `available:false` 形态、非法 id 与多跳展开；该文件现 27 例，本批新增两枚端点用例）；MCP 侧 `tool_definitions()` 报 24 枚、`execute_tool()` 的两枚新派发臂各一条 stdio 往返断言（含缺省 limit 与按 host / status 过滤），用例落 `tests/integration/itest_mcp.cpp`（现 16 例，本批新增两枚工具往返与 22 → 24 计数断言）。
13. **手工用例（批三）**：`codespec/manual-test/23-net.md` 新增 TC-NET-008，按 §1.1 第二类准入（真实 Inspector 进程 + MCP stdio 往返）人工核对端点 JSON、时间线各阶段与两枚工具的实际返回；用例的前置条件照 TC-NET-007 先例点名载体与端口发现方式（起哪个宿主目标、Inspector 监听端口如何取得、`aurora_mcp` 以何命令行接入），执行记录同批填入该文档 §3。
14. **门禁全绿**：`ctest --preset ninja-test` 全量（含全部 `check_*`），其中 `check_api_schema_sync`（先 `--target aurora_api_json`）、`check_api_budget`、`check_naming_conventions`、`check_doc_comments`、`check_umbrella_header`、`check_core_layer_boundary`、`check_arch_module_map`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`check_no_cjk_literals`、`check_no_hardcoded_paths`、`check_version_consistency`；改动 TU 逐个过 `lint`，`git add` 之后复跑 `format-check`。

## 回写落点

1. `codespec/specification/06-app-platform.md`：新增四枚小节（§8.8.36 网络事件总线与消费者订阅、§8.8.37 请求时间线与活动条目、§8.8.38 凭据分级脱敏与进程级 ExportPolicy、§8.8.39 Inspector 路由注册表与 network 端点），各含契约、消费者三条回调契约、线程口径、收摊与预算纪律、判据与变异自证记录。
2. `codespec/specification/08-tooling.md`：§10.2 补网络可观测端点与脱敏细则；§7.1 工具清单表补齐九行存量漂移（`live_*` 五枚与 `build_ui_prompt` / `compare_snapshot` / `generate_ui` / `repair_tree` 四枚）并加两枚新工具，§10.6 的 MCP 工具数按补齐后的 24 改写（现状「13」同批纠回）。
3. `codespec/SPECIFICATIONS.md`：§5 特性表 `SPEC.FEAT.NET.OBSERVABILITY.001` 行的「规格落点」列补新小节指针，判据措辞随实现形态收口。
4. `codespec/CODING_STANDARDS.md`：§4.1 双通道段补「凭据一律折叠、四出口同口径、库侧 `Secret` 类不可由宿主放大」硬规则，§15 反例速查加一条对应反例。
5. `codespec/BUILD_OPTIONS.md`：`AURORA_ENABLE_DEBUG` 条目登记本提案造成的那一处后果——该宏决定事件总线与其三处 emit 点是否存在（宏关时 `timeline_of()` / `activity_snapshot()` 恒回空），**不再涉及任何公共类型的布局**（此前拟改动 `Response` 的形态已废弃）；门面新增两枚能力的可见性随条说明。
6. `codespec/ARCHITECTURE.md`：§4 的 net 行补事件总线与活动订阅的归属，§14 的测试与 CI 段登记 Inspector 路由注册表与两枚端点的回归口径。
7. `codespec/manual-test/23-net.md`：§1.1 被测模块表补新公共头一行，§1.2 执行载体表补一行（Inspector 宿主 + `aurora_mcp` stdio 是一类新载体，端口发现方式随行写明），§2 用例清单加 TC-NET-008，§3 执行记录表随批填写。
8. `codespec/changes/xplat-network-stack/proposal.md`：§13 四条承诺各补「落地现状」段；§16 追加裁决条目——本提案定案取代第 9 项后半（取消 lint 规则那条，旧编号与旧文字保留）、给第 8 项的注册表化补方法闸统一收派发处的纠回与 400→405 差异、给 §18 第 5 项的两级查找补两趟实现的定形；§17 的可观测性条目随批收口（`use(hook)` 不在本切片，该条不动）。
9. `codespec/errors.toml`：本件不新增码；若落码中出现「时间线 / 活动表自身的失败形态」一律走既有码并在 detail 点名，届时按实际分支决定是否需要条目 hint 改写（默认：无改动）。
10. 代码与数据面（不属 codespec 树，此处只记必做动作）：根目录 CHANGELOG.md 记 Added 段（新公共头 / 枚举 / 结构体 / 旋钮 / 两枚自由查询 / 两枚端点 / MCP 24 枚）与 Changed 段的 Inspector 405 差异；根目录 aurora_api.json 经 `--target aurora_api_json` 重建（enums 段 +3 = `HttpStage` / `ExportSurface` / `FieldClass`）；tools/check/umbrella_manifest.txt 收录新公共头，include/aurora/aurora.h 同步加入该头的直接包含（`check_umbrella_header` 的 R7 / R8 同查伞头与清单两处）；tools/include/known_enums.h 与 tests/integration/itest_known_enums.cpp 各加三枚枚举锚点。
