# CHG-025 网络可观测性：事件总线与消费者订阅、Inspector 路由注册表、请求时间线与活动表、凭据分级脱敏、network 端点与 MCP 工具

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-025 |
| 提出日期 | 2026-10-20 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.NET.OBSERVABILITY.001 |
| 影响面 | 三个可独立评审的切片：① Inspector 路由注册表化（文件内静态表 + 可取单测的匹配器，零公共面）；② 网络记账与脱敏（两枚新公共头、三枚举 / 五结构体 / 两抽象类 / 十二自由函数，进程级活动存储与事件总线，两段式 `ExportPolicy`）；③ 暴露面（`GET /api/network`、`GET /api/network/request`、MCP 22 → 24）。**无破坏性签名变更、无新增错误码、`Client` / `Response` 零数据成员改动**；`AURORA_ENABLE_DEBUG=OFF` 时总线与三处 emit 点从编译面消失，脱敏保留（Inspector 在 Release 也读树）。REST 面只有一处有意的行为纠回：`DELETE /api/widget/` 一类「动词不支持 **且** 路径退化」的交集由 400 变 405 |

## 动机

1. **本库的立身之处要求这件事是核心交付面，而不是附加项。** `codespec/SPECIFICATIONS.md` §5 已把「AI 能从一次界面异常反查到一次请求的耗时与失败阶段，且排障信息不会泄漏凭据」写成 `SPEC.FEAT.NET.OBSERVABILITY.001` 的判据；网络又是最难事后排查的一类 I/O（现场不可复现、对端不可控、失败形态跨九档归因）。而这条需求今天**没有任何实现承担**——跨平台网络栈那伞形提案（`codespec/changes/xplat-network-stack/proposal.md`）的 §17 落地序里，底座、传输、TLS、HTTP、绑定、SSE、WebSocket 七件都已收口，`codespec/specification/06-app-platform.md` §8.8 现状段记明此刻仍未落地的是**可观测性**与 §6 发起侧的 `Client::use(hook)` 两件；本切片只取可观测性一件，`use(hook)` 不在本提案范围、留待后续提案收口。

2. **「埋点已经全部就位、缺的只是消费者」这个前提不成立，必须先纠正。** 发起侧确有边界挂点：`StageTimer` 挂在 connect / read_idle / total 三档上，`detail::await_connect_via` 是全库唯一建连出口，`stamp_request_id` 与 `log_exchange_identity` 两枚起手行在位。但 `StageTimer` 是**超时看门狗**——它只向 `Engine::post_after` 登记一枚到点动作并在到点时取消令牌，**从不读时钟、不存偏移、不产生任何阶段时刻**；全库也没有 begin/end 配对、没有一次交换的记录实体。换句话说，边界挂点是**可插入记账的位置**，不是已经算好的数据源；本次要补的正是「谁在什么时刻记下哪一笔」。另一处事实纠正：DNS 解析发生在 `connect_via` 内部（解析器 → 双栈竞速 → 建连，代理隧道也在同一出口内），HTTP 层拿不到独立的 DNS 边界，故阶段模型**不设** `Dns`；DNS 失败仍是可区分的——`error_class` 落 `Dns`，`failed_stage` 落 `Connect`。这条与网络栈提案 §13 的「DNS / connect」措辞冲突，按 §16 追加裁决回写。

3. **每加一枚端点必改一枚千行巨函数。** `src/aurora/inspector/inspector_server.cpp` 的 `route_request` 是一条贯穿全文件的 if 链，**精确匹配与前缀匹配交错并存**（`route == "/api/debug/state"` 一类与 `route.starts_with("/api/widget/")`、`starts_with("/api/input/")` 两类穿插）。本切片要挂两枚端点，先做注册表化才能无侵入落地——这条前置改造已由网络栈提案 §16 第 8 项裁决并入范围，本提案把它连同判据一起定形。匹配器本身抽成计划/待建的内部分头 src/aurora/inspector/route_match.h 的模板；`cmake/AuroraTests.cmake` 已把 `src/` 加入测试包含面，故「精确项整体优先、前缀项按登记次序兜后」可以拿合成表直接单测，而不必去戳 Inspector 的私有表。

4. **凭据泄漏面要按「今天真的在漏什么」重画，而不是按最响的口号画。** 实测全域（`src/`、`include/`、`tools/`）对 `redact` / `脱敏` / `打码` 空命中（`mask` 仅命中 WebSocket 帧掩码，协议特性而非脱敏）；`include/aurora/core/log.h` 的 `Logger` 只有级别阈值、sink 重定向、raw 通道重定向与开关四类旋钮，**没有任何脱敏接缝**；`codespec/CODING_STANDARDS.md` 内「凭据 / Authorization / Cookie」零命中。但要说清楚：今天的日志行只有标识、方法与主机，**并没有**在把头部或 URL 写进日志；真正在漏的是第二条路径——Inspector 的 `/api/tree`、`/api/debug/tree`、`/api/yaml`、`/api/widget/{path}` 会把控件属性全量导出（含 TextInput 的文本），宿主塞进输入框的 token 直接进 JSON。而本切片新增的活动表会**第一次**把带查询串的 origin-form 目标带进库侧导出面，所以脱敏必须与它同批落地，而不是等泄漏出现再补。由此定成两轨：协议事实（头部、URL、消息体）只有库知情，由库强制裁决；控件属性只有宿主知情，经进程级 Policy 在 `InspectorJson` 面交给宿主。四枚出口里 `Timeline` 与 `InspectorJson` 是**活出口**，`Log` 与 `ErrorDetail` 是**守门出口**（今天没有凭据位，但未来任何一枚 detail 都必须走同一闸），不把「守门」冒充成「已发生的泄漏」。

5. **一处已定案的执行强度在仓内无法落地，须纠回。** 网络栈提案 §16 第 9 项定的是「条款 + 单测 + lint 规则」。实测仓内没有任何既有 `check_*` 扫描器能自然承载「凭据不得入日志」这条规则，clang-tidy 亦无自定义正则检查能力；硬造一枚 `check_*` 只会多一个需要维护豁免的扫描器，而且**静态扫描抓不到运行时凭据值**。故本提案纠回为「条款 + 四出口同一断言集 + `codespec/CODING_STANDARDS.md` §15 一条反例」，旧编号与旧文字保留、追加裁决取代其后半（理由同批写进裁决）。

6. **「关闭时零开销」与「公共头维持零 `#ifdef`」两条口径不必二选一——只要把观测数据搬出应答对象，并且不往 `Client` 里塞成员。** 此前拟给 `Response` 加一枚按宏裁切的数据成员、给每个 `Client` 加一张环形表 + 一张进程级 weak 名册；前者让 `Response` 的尺寸随宏变化、须登记 `BUILD_OPTIONS.md` 并长期背 `sizeof` 判据，后者让 `timeline_of(request_id)` 与 Inspector 面对「问哪一个 Client」的歧义，还得靠名册逐个 `lock()`。本提案两件都不继承：时间线与活动条目改由**一枚进程级存储**供给，`Client` / `Response` 零成员改动，公共头零 `#ifdef`；Release（`AURORA_ENABLE_DEBUG=OFF`）下总线、存储与三处 emit 点一并编译裁切，两枚查询返回 `available:false` / `nullopt`。代价从「公共类型随构建变形状」降为「总线自身在 Release 不存在」，而代价的受益面是整个进程只有一份答案。

7. **「每多一路读者就在各对象里多存一份」撑不住多路消费，业界通行的是推模型的一次记账。** OkHttp 的 `EventListener` 不在 `Response` 上存任何东西，只在生命周期边界回调宿主注册的那枚 listener，并给出三条硬契约：**每个 begin 事件最终必有一个配对的 end 事件**（成功或失败必居其一）、**所有回调必须快、不得抛异常、不得持外部锁、不得回调时重入 client**（写盘与网络 I/O 一律异步）、**复用连接的那次通话不发 DNS 与建连类事件**。它的 `Factory` 同时也是官方给出的采样形态（随机 10% 给真 listener，其余给 `EventListener.NONE` 这枚缺省空实现）。Chromium 同构：全局一枚 `NetLog`，宿主注册观察者，参数经回调惰性取值——没人观察时连字符串都不必拼。本提案据此把「总线 + 存储」定成两层：**边界上记一次原始事实**（由一枚协程帧内的 RAII 守卫持有），总线把呈现后的条目推给订阅者，进程级存储另存一份原始目标供不同导出面按需呈现。两者都是消费者，谁被淘汰都不影响另一条的配对；日后要加指标计数、落盘、外部追踪格式，都不必改 net 一行。

8. **安全边界的最后写字人不能是宿主。** Chromium 把最重的一档做成中央 allowlist：`net_log_capture_mode.h` 明写「individual netlog entry producer callsites do not have any say in this. This is by design to make it harder for a given producer to accidentally log sensitive data」。此前那版的形态是「库算出原值 → 宿主决定呈现 → 宿主返回的字符串就是最终落出去的文本」：宿主实现里一处疏忽、或一枚「原样返回」的调试桩忘了摘，都能把原值写出去。更实际的是，单段的 `present()` 对 `Sensitive` **不可实现**——值一旦被折叠，宿主再没有原值可放行；要支持「按 (导出面 + 键) 放行」，策略必须有两段。故本提案定成：**`allows()` 先答「这一类能不能给」，`present()` 再答「给成什么样」**；`Secret` 不问 `allows()`、拿到手的恒是折叠形态，宿主即使原样返回也放不出去；`Sensitive` 默认折叠，按 `(导出面 + 键)` 显式放行；`Public` 原样给出但宿主可再收窄。URL 的用户信息段与令牌查询参数、以及消息体，按 `Secret` 处理——`OAuth` 回调把 `access_token` 放在查询串里正是常见形态，只盯四枚头部等于承认这条后门。

9. **有界性必须是预算，而不是仁慈。** 此前那版的淘汰口径是「写满时只淘汰最旧的已完成条目，in-flight 永不淘汰」，它隐含假设在途条目终会收摊。真实形态是「对端不收摊 + 应用持续发起」这一组合会把环形表全部换成 in-flight，随后**新事实再也记不进去**——观测恰好在它最被需要的那一刻哑掉。业界给出现成口径：OkHttp 的「每个 begin 必有配对 end」把「一定会收摊」写成 API 契约，`Client` 析构与业务取消是它必须结清的场合；Chromium 的 NetLog 有 `maxFileSize`（到量自动停记）；OpenTelemetry 给每 span 的属性数与事件数设了默认 128 的上限，并要求超限采取截断与丢弃而不是无限增长。本提案据此定两条：**容量淘汰一律从最旧开始，在途条目不豁免；被淘汰的记录只记 `dropped`，不伪造 end，存储此后忽略它的 end，总线仍把真实 end 交出去**。文本侧只有一处上限（`max_value_chars`），超限的原始目标存占位符而不是前缀——截断一个秘密仍是泄漏它的前缀；阶段数由枚举固定为六枚，不另设「单跳 mark 上限」这种永远咬不到的旋钮。

## 变更内容

### 1 Inspector 路由注册表化（前置件，零公共面）

`route_request` 的 if 链改为文件内静态数组，条目字段 `pattern / kind(Exact|Prefix) / methods / label / handler`；handler 取**指向 `InspectorServer::Impl` 成员函数的指针**而非 `std::function`（仓内 `.clang-tidy` 已记录「任何转入 `std::function` 的可调用对象一律判为不应抛出」这条系统性假告警面，不值得为一张表去招它）。查找逻辑不写在 `.cpp` 里，而是抽到计划/待建的内部分头 src/aurora/inspector/route_match.h 的模板 `match_route(std::span<const Entry>, std::string_view)`：条目只需暴露 `pattern` 与 `kind`，测试用合成条目即可覆盖「精确项整体优先、前缀项按登记次序兜后」；生产表与测试共用同一份匹配代码，不存在两套次序。`display` 侧另用一枚 `RouteCall {method, path, route, body}` 形参把四枚事实交入，`path` 仍为 `const std::string &`（`query_param` 签名如此）。

方法闸**统一收在派发处**：先匹配路由，再判该方法是否在条目的位掩码里；不在即 `405`，`label` 由条目在登记时说出（精确项就是 pattern，前缀项不带尾斜杠）。通过方法闸后才进 handler，handler 里的「路径退化」检查（如 `/api/widget/` 的空余段）继续回 `400`。行为差异面只有「动词本就不被支持 **且** 路径恰好退化」这一个交集（`DELETE /api/widget/`、`/api/widget//` 一类）由 400 变 405——`405` 说的是「这个端点不接受这个动词」，而路径形状对与错都改变不了动词不被支持这一更基本的事实。该交集现状无任何用例覆盖，新增用例是**补判据**而非改判据。404 兜底仍用含查询串的原样 path，405 文案仍为 `Method not allowed for <label>`，其余状态码与响应体逐值不变。

匹配器单测落计划/待建的 tests/unit/utest_route_match.cpp：合成表里同时放一条精确 `/api/widget/fixed` 与一条前缀 `/api/widget/`，断言前者命中精确项、后者只在精确项不命中时兜底；另以「把两趟查找顺序颠倒」的变异确认该例真的转红（这是对旧稿「次序断言」空转的纠正——现表无重叠时颠倒顺序不会改变任何分派，必须用合成重叠才能自证）。

### 2 事件总线与进程级活动存储（取代「每 Client 一张表 + weak 名册」）

新增抽象消费者 `NetEventSink`，三枚虚方法 `on_exchange_begin` / `on_stage` / `on_exchange_end` **皆默认空实现**——照 OkHttp `EventListener` 的形态：宿主只覆盖关心的那一枚，「没人听」就是什么都不算，而不是空转一枚什么都不做的 `std::function`。订阅口 `subscribe_events()` 收 `std::weak_ptr<NetEventSink>`，**宿主放手即自动退订**：不必另设注销 API，也不会出现「总线里挂着一枚已析构消费者」这种要靠开发伦理去避免的形态。库侧 emit 只有三处（起手、阶段到达、收摊），三处全部 `noexcept`、不得抛。

**一枚进程级 `ActivityStore`，不是每个 Client 一张表。** 理由有三：其一，`timeline_of()` 与 Inspector 要问的是「这个进程里的这一次交换」，按 Client 存会让同一枚 `X-Request-Id` 在拷贝、池共享与多窗口之间出现多个答案；其二，进程级存储与进程级 `ExportPolicy`、进程级 Inspector 生命周期一致，不需要弱名册去枚举「还活着的 Client」；其三，`Client` 不新增成员，拷贝与并发语义原样保留。存储由库内实现持有，配置是进程级的 `ActivityOptions`：`record` 缺省 **开**（与 `PoolOptions` / `CacheOptions` 的缺省关相反且刻意：那两件改变的是已存在的交换何时不再发生，而本件是唯一「出问题当场问不出来就永久问不出来」的那件，缺省关掉等于「AI 可观测运行时」要配置才成立），`capacity` 缺省 64，`max_value_chars` 缺省 256。宿主可 `set_activity_options({.record = false})` 整体停记；`record = false` 只停新记录，已在途的条目继续走到收摊，避免配对断裂；`capacity` 缩小时立即从最旧开始淘汰并累加 `dropped`。

存储**不经过订阅名单**，而是在与总线相同的三处边界直接喂入：总线推给订阅者的是**呈现后的条目**，存储要留存**原始目标**才能在取快照时按 `Timeline` / `InspectorJson` 两个面分别呈现。两者共享同一枚 RAII 守卫（见第 3 项），守卫自己持有这次交换的原始事实，故存储淘汰、`clear`、`record=false` 都不会让总线的 begin/end 失去配对；被淘汰条目的真实 end 仍会推给订阅者，存储按 `exchange_id` 查不到就忽略，不复活、不补造。

**线程口径**：`Client` 可拷贝、可并发发起，`send` 的续体恒回主线程，但协程体的前半段可能在发起线程上就地执行，故 `NetEventSink` 的三枚回调**可能来自发起线程，也可能来自主线程调度器，且不同交换之间可以并发**。总线自身在订阅/发射时持锁，但**持锁只做名单快照，回调一律在锁外调用**；`NetEventSink` 与 `ExportPolicy` 的实现必须自己线程安全。回调收到的 `entry` 指向一枚仅在本次调用期间有效的副本，需要留存请复制；回调必须快、不得抛、不得做 I/O、不得重入 net（`noexcept` 是契约，抛异常即终止进程）。Release 时总线与三处 emit 点一并编译裁切，进程内无总线全局状态；订阅名单为空时 emit 只是一次空名单快照后返回，不建环、不记账的只是总线这一路（存储按自己的 `record` 决定）。

### 3 请求时间线与活动条目（值层；`Response` 不参与）

新增公共枚举 `HttpStage` 与 `http_stage_text()`，以及 `StageMark` / `HttpRequestTimeline` / `HttpActivityEntry` / `ActivityOptions` / `ActivitySnapshot` 五枚结构体。**阶段划分不新造**——埋点只加在既有三档与建连出口上，「到点记一笔」；但语义要按真实控制流定死：

| 阶段 | 到达点 | 缺席/边界 |
|:---|:---|:---|
| `Connect` | TCP（或代理隧道）建立 | 含 DNS 解析与双栈竞速；DNS 失败记本阶段，`error_class = Dns` |
| `TlsHandshake` | TLS 握手完成 | 明文目标与复用连接不出现 |
| `ConnectionAcquired` | 传输就绪 | 新连接在 Connect/TLS 之后；复用连接的首枚 mark（offset 从 0 起） |
| `RequestSent` | 头与体全部交给传输层 | 流式体以最后一笔写入为界 |
| `FirstByte` | 读到应答头首字节 | 重定向跳也标；握手失败则无 |
| `BodyDone` | 最终一跳的线上体读完 | 中间重定向跳、`sink` 主动停读、WebSocket 握手不标 |

`StageMark` 只留 `{stage, offset}`，**不带 `bytes`**：把「截至本阶段累计」与「本阶段增量」混在一枚字段里，消费者无法判断该做差还是直接读，而字节事实在活动条目的 `sent` / `received` 上已有唯一位置。偏移一律取自 `std::chrono::steady_clock`（墙钟会被 NTP 校时推着走，拿它做差值会得出负数或突跳，那是 bug 而不是精度问题），单位微秒——毫秒会把池化复用与本地回环那一类亚毫秒交换整片量化成 0，而它们恰恰是性能排障最常问的那一段。

**序标三枚**：`exchange_id`（进程内唯一、递增，配对与排序的主键）、`hop`（重定向跳序号，0 起）、`pass`（物理尝试序号，0 起，重试与凭据重放都递增）。凭据重放复用重试环的 `attempt` 配额，**不能**拿配额当尝试序号，否则「第几次物理往来」在认证重放里会重复；`pass` 独立计数，`hop` 只在一次尝试内部随 3xx 递增。`timeline_of()` 取同一 `request_id` 下 `exchange_id` 最大的那一跳（含在途），`timelines_of()` 按 `exchange_id` 升序交全部；空 `request_id` 或未知 id 分别回 `nullopt` / 空数组。`X-Request-Id` 被调用方显式关掉或熵不可得时，条目仍在，`request_id` 为空，只能从 `/api/network` 看见、不能被 `/api/network/request` 寻址——库不替调用方伪造标识。

**失败归因**复用既有纯函数 `error_class_of()`（实测九档制，且有唯一真值源），`error_class` 用 `std::optional`：成功 2xx 为空，≥400 为 `Status`（`error_class_of_status()`），传输失败为对应档。`failed_stage` 是**失败时正在进行的阶段**，由守卫在进入每个阶段时记下当前阶段、失败收摊时读取；建连之外的本地拒绝（策略闸、能力闸、不可重放判定）在阶段开始前就失败，故为 `nullopt`。`status` 是「已收到应答头则为其状态码」——200 的体在中途断了仍是 200 + `net-recv-failed`，不是 0；只有根本没收到应答头才是 0。

**覆盖面**由不变量决定而非按后端列举：判据是「一次会完成的交换 = 一条条目」——`send` 与 `open_stream` 都有 Body 完成终点，全量收；`open_sse` 的每条流（含每一次重连）本身就是一次有始有终的 `open_stream`，按每条流一条收、继承同一批埋点；`connect_websocket` 只收**握手**那一条，升级后的会话不进请求活动表（会话的收摊事实已在 `WsCloseInfo` 里，属会话观测而非请求观测，另登待办）。新鲜缓存命中记 `from_cache = true`、零 mark、`sent = 0`、`received` 为交付体字节；陈旧副本的 304 重验证是一次真实线上交换，记 `from_cache = false`，其最终体来自缓存这一层不另分叉。

### 4 凭据脱敏：三类灵敏度，两段式策略，库只对自己知情的那半做主

分级 `FieldClass{Public, Sensitive, Secret}`（登记 `tools/include/known_enums.h` 与 `tests/integration/itest_known_enums.cpp` 锚点）。归属：**库对自己知情的那一半做主**（头部、URL 的用户信息段、查询串令牌、消息体），**宿主对自己知情的那一半做主**（控件属性）。策略是两段式：

- `allows(surface, key, field_class)` 只对 `Sensitive` 询问，缺省 `false`；`Secret` 恒不问（宿主无权放行），`Public` 恒不问（本就可给）。
- `present(surface, key, field_class, value)` 对每个导出字段调用一次，拿到的是库**已经决定愿意给出**的值：`Secret` 恒为折叠形态，`Sensitive` 未放行时为 `"***"`、放行时为原值，`Public` 为原值。宿主返回什么就是最终文本——但它拿不到 `Secret` 原值，故「原样返回」的调试桩也放大不了。
- 无策略时：`Public` 原样、`Sensitive` 折叠、`Secret` 折叠。`present()` 抛出时库回退到该字段的折叠形态，不中断交换、不把异常带进 net。

`ExportSurface` 定四枚：`Log`、`Timeline`、`InspectorJson`、`ErrorDetail`；**没有 `McpJson`**——MCP 的两枚工具经 Inspector 取 JSON，若另开一面，同一份数据可能在 MCP 面被放行而在 Inspector 面被折叠，等于给「换个端口看」留后门。键词汇固定为 `header.<小写名>`、`query.<原名>`、`detail.<字段>`、`tree.<属性键>`、`userinfo`、`path`、`host`、`method`、`request_id`；查询参数名按百分号解码后、ASCII 大小写不敏感地匹配令牌名单，避免 `access%5Ftoken` 这类绕过。折叠形态：认证类头部保留方案与长度（如 `Bearer len=40`），`Cookie` / `Set-Cookie` 与一般 `Sensitive` 为 `"***"`，查询令牌保留 `name=***`，用户信息段 `***`，消息体恒不导出（不是折叠，是不出现）。

`Secret` 最小集：`Authorization` / `Proxy-Authorization` / `Cookie` / `Set-Cookie`，另加 `x-api-key` / `x-auth-token` / `x-csrf-token` 一类名字含 `token` / `secret` / `password` / `api-key` 的头部；URL 的 `userinfo`；查询串中名字命中令牌名单的参数（`token` / `access_token` / `refresh_token` / `id_token` / `api_key` / `key` / `secret` / `signature` / `sig` / `session` / `password` / `code`）；请求体与应答体。其余头部与非令牌查询值为 `Sensitive`；方法、方案、主机（永不含用户信息段）、状态码、字节数、时刻、`exchange_id` / `request_id`、以及 `path` 为 `Public`。**`path` 定为 `Public` 是刻意的取舍**：把整条路径默认折叠会把 Inspector 的首要功能（高保真导出界面状态）废掉，而路径段里的令牌没有名字可供库侧推断；库侧能可靠覆盖的是 userinfo 与令牌查询参数，宿主若把令牌放进路径段，应在 `present(InspectorJson, "path", ...)` 里收窄——`Public` 允许收窄，这条正是为此留的。

`present_url()` 接受绝对或 origin-form：解析失败只回 `"<unparsable-url>"`，绝不回显输入；绝对形态去掉 userinfo、按上述键规则折叠查询；origin-form 只处理路径与查询。`present_field()` 是单字段版本，供头部、detail、树属性使用。两枚 helper 都按调用时刻取进程级策略，不构成跨整份导出的事务；需要整份一致时先安装好策略再导出。

**Inspector 这一轨**覆盖 `/api/tree`（含 `?window=`）、`/api/debug/tree`、`/api/yaml`、`/api/widget/{path}` 的 GET，以及新增的两枚 `/api/network*`。前四者的 `props` / `values` 字符串叶子按 `tree.<属性键>` 走 `Sensitive`；`type`、`descriptor.name`、属性名等结构键**不参与**折叠（否则类型名会被打成 `***`，Inspector 直接失去可读性）。走法是「先取一份策略快照；未安装策略时整条 walk 不执行、输出逐字节不变」——这就是未注册策略时既有端点保持原样的原因，也是判据第 1 项对照组的天然覆盖。`/api/to_code` 不在此列：它把调用方自己送进来的树转成代码，是纯输入变换而非状态导出；`/api/find` 与 `/api/debug/pick` 只回路径、类型、id、边界，不回属性值；`/api/patch` 只回状态。MCP 的 `live_tree` / `live_widget_get` 经同一端点，自动继承。

`Log` 与 `ErrorDetail` 是守门出口：今天 `log_exchange_identity` 只写标识 / 方法 / 主机，错误 message 也不含 URL 或头部（建连类 detail 只有 `host:port`，属 `Public`），所以这两条路上没有凭据可折；本提案不为了「看起来有活」去改日志格式，而是把 `present_field` 的头部/`detail.*` 单测与「未来任何 detail 必须走此闸」写进 `CODING_STANDARDS.md`。同批在 §4.1 补硬规则（凭据一律折叠、四出口同口径、库侧 `Secret` 不可由宿主放大），§15 加一条反例（把 `Authorization` 原值直接拼进日志）；判据形态是**四出口同一断言集**，但如实区分活出口与守门出口。

脱敏归属层选 `aurora::net` 而非 `aurora::core`：Inspector 已依赖 net（`inspector_server.cpp` 已 include `include/aurora/net/blocking.h`），依赖方向成立；挂进 core 会让 L0 长出「导出面」这一非 core 概念，挂进 `aurora::debug` 则违反分层方向（net 不得反向依赖到依赖 widget 的门面层）。脱敏实现**不经 `AURORA_ENABLE_DEBUG` 裁切**——它在 Release 也保护 Inspector 的树导出；Release 裁掉的只有总线、存储与 emit。

### 5 端点与工具面

注册表加两行即挂两枚精确端点：`GET /api/network`（活动快照，按 `InspectorJson` 面呈现）与 `GET /api/network/request?id=<X-Request-Id>`（单条请求的多跳展开 + 各跳 `StageMark`）。`/api/network` 支持 `limit`（0 = 全部）、`host`（精确、大小写不敏感）、`status`（整数）三枚查询参数；先过滤再取 `limit`，非法参数回 400。`/api/network/request` 缺 `id` 或空串回 400，未知 id 回 404（不把调用方给的 id 回显进文案，避免把任意输入带进响应），命中时回 `{available, request_id, entry, timelines}`——`entry` 是该 id 最新一条活动条目（含 path/host/status），`timelines` 按 `exchange_id` 升序。Release 下两枚端点都回 `{available:false, reason:"AURORA_ENABLE_DEBUG not enabled"}`，形状与既有调试门面的 unavailable JSON 一致。

**不新增 `aurora::debug` 门面函数**：那两枚查询自身在 Release 已有确定形态，Inspector 直接调 `aurora::net::http` 即可；加门面只会多一条同义的 Release 分支，还要改 `debug_api.toml` 与生成器。Inspector 已经依赖 net，分层方向不变。MCP 侧 `tools/servers/aurora_mcp.cpp` 有**两处登记点**——`tool_definitions()` 的声明表与 `execute_tool()` 的派发链（实测各 22 条）——两枚新工具须同改两处：`list_network_activity{limit?, host?, status?}` 与 `describe_network_request{request_id}`，22 → **24**；工具返回 Inspector 的 JSON 原文，不另开一面。本切片不改造 MCP 的登记形态（「加一枚工具要动两处」与 Inspector 旧形态同款，收口留待后续提案）。

`codespec/specification/08-tooling.md` §7.1 工具清单表与 §10.6 那句「暴露 13 个 MCP tools」同批改：§7.1 现存 13 行，较代码少九行存量漂移（`live_patch` / `live_simulate` / `live_tree` / `live_widget_get` / `live_widget_set` 五枚与 `build_ui_prompt` / `compare_snapshot` / `generate_ui` / `repair_tree` 四枚），借此同批补齐，否则新表与代码继续对不上；§10.2 补网络可观测细则。CLI 侧不同步——提案未要求，`aurora_cli` 子命令不在本件范围。

### 6 交付切片与每片门禁（按依赖切三片，每片自带单测与文档回写，不跨片留红灯）

- **注册表切片**：第 1 项。零公共面；判据是既有端点逐值对照 + 合成重叠的精确优先 + 三处变异自证。门禁含 `check_naming_conventions`、`format-check`、`lint`（改动 TU）。
- **记账与脱敏切片**：第 2 / 3 / 4 项。新增两枚公共头、三枚举、五结构体、两抽象类、十二自由函数；判据是一次记账多路消费、生命周期配对、有界预算、阶段语义与缺席、四出口同一断言集；门禁含 `check_api_schema_sync`（先 `--target aurora_api_json`）、`check_api_budget`、`check_umbrella_header`、`check_core_layer_boundary`、`check_doc_comments`、`check_naming_conventions`、`lint`、`format-check`。第 4 项带入的四枚既有 Inspector 导出端点接入 `InspectorJson` 面亦在本片落地。
- **暴露面切片**：第 5 项。Inspector 两枚端点、MCP 两枚工具、手工用例；判据是端点回环夹具 + MCP stdio 往返 + `check_codespec_xref` / `check_manual_test_format`。

三片各自的 `codespec/` 回写随片落地；末片补齐网络栈提案的追加裁决与 `CHANGELOG.md`，并在 `git add` 之后复跑 `format-check`（该门禁只枚举已跟踪文件）。

## 公共面形态（本提案定形，落码逐名照此）

```cpp
// ── include/aurora/net/observability.h — 命名空间 aurora::net；只做脱敏，不依赖 HTTP ──
namespace aurora::net {

/// @brief 导出面：策略按「数据将落到哪里」区分，而不是按调用点区分。
enum class ExportSurface : std::uint8_t {
    Log,            ///< 库的日志行
    Timeline,       ///< 程序化查询与事件消费者（缺省面）
    InspectorJson,  ///< Inspector 端点（MCP 经 Inspector 代理，继承本面）
    ErrorDetail,    ///< 错误 message 中的补充事实
};

/// @brief 字段灵敏度：由库裁决；宿主只能收窄，不能放大。
enum class FieldClass : std::uint8_t { Public, Sensitive, Secret };

/// @brief 宿主侧的导出策略：两段式，先问「能不能给」，再问「给成什么样」。
/// @note Thread: 可在任意发起线程与主线程被调用；实现须线程安全、不得持外部锁。
/// @note Side-effects: none；present() 内不得做 I/O；抛出时库回退到折叠形态。
class ExportPolicy {
  public:
    virtual ~ExportPolicy() = default;
    /// @brief 仅对 Sensitive 询问；Secret 恒不问（无权放行），Public 恒不问（本就可给）。
    /// @param surface 本次导出面；@param key 键词汇；@param field_class 恒为 Sensitive。
    /// @return 允许把原值交给 present() 则为 true。
    [[nodiscard]] virtual auto allows(ExportSurface surface, std::string_view key,
                                      FieldClass field_class) const -> bool { return false; }
    /// @brief 把库愿意给出的值呈现为最终文本；Secret 收到的恒是折叠形态。
    /// @param value 已按分级决定的值；@param field_class 该字段的分级。
    /// @return 最终落出去的文本；库不对其再做展开。
    [[nodiscard]] virtual auto present(ExportSurface surface, std::string_view key, FieldClass field_class,
                                       std::string_view value) const -> std::string = 0;
};

/// @brief 安装进程级策略；传空即清回「未设」。
/// @note Thread: thread-safe；@note Side-effects: 替换此后新发起的呈现取用。
auto install_export_policy(std::shared_ptr<const ExportPolicy> policy) -> void;
/// @brief 取当前策略的共享持有；未设时为空。
[[nodiscard]] auto export_policy() -> std::shared_ptr<const ExportPolicy>;

/// @brief 呈现单个字段：Public 原样、Sensitive 未放行折叠、Secret 恒折叠，再交给 present()。
[[nodiscard]] auto present_field(ExportSurface surface, std::string_view key, FieldClass field_class,
                                 std::string_view value) -> std::string;
/// @brief 呈现绝对或 origin-form URL：去 userinfo、折叠令牌查询参数；解析失败只回占位符。
[[nodiscard]] auto present_url(ExportSurface surface, std::string_view url) -> std::string;

}  // namespace aurora::net

// ── include/aurora/net/http_observability.h — 命名空间 aurora::net::http；包含 http.h ──
namespace aurora::net::http {

/// @brief 一次交换的阶段。DNS 折在 Connect 内；复用连接没有前三枚。
enum class HttpStage : std::uint8_t {
    Connect,             ///< DNS + TCP + 代理隧道；DNS 失败也记本阶段
    TlsHandshake,        ///< TLS 握手完成
    ConnectionAcquired,  ///< 传输就绪：新连接在 Connect/TLS 之后，复用连接的首枚 mark
    RequestSent,         ///< 头与体全部交给传输层
    FirstByte,           ///< 读到应答头首字节
    BodyDone,            ///< 最终一跳的线上体读完；重定向跳/主动停读/WS 握手不标
};

/// @brief 阶段名的稳定小写令牌（日志与 JSON 用，跨进程只认这枚文本）。
[[nodiscard]] auto http_stage_text(HttpStage stage) -> std::string_view;

struct StageMark {
    HttpStage stage{HttpStage::Connect};  ///< 阶段
    std::chrono::microseconds offset{0};  ///< 相对本跳起手的偏移（steady_clock）
};

struct HttpRequestTimeline {
    std::uint64_t exchange_id{0};        ///< 进程内唯一、递增；配对与排序的主键
    std::string request_id;              ///< X-Request-Id 的值；未带标识时为空
    std::uint16_t hop{0};                ///< 重定向跳序号，0 起
    std::uint16_t pass{0};               ///< 物理尝试序号，0 起（重试与凭据重放都递增）
    std::chrono::microseconds total{0};  ///< 本跳耗时；在途时为取快照时的已耗时
    bool in_flight{false};               ///< 是否仍在途
    bool reused_connection{false};       ///< 是否复用池内连接
    std::vector<StageMark> marks;        ///< 已到达阶段；缺席即本跳未经历
};

struct HttpActivityEntry {
    std::uint64_t exchange_id{0};               ///< 进程内唯一、递增
    std::string request_id;                     ///< X-Request-Id 的值；未带标识时为空
    std::string method;                         ///< 请求方法
    std::string path;                           ///< 呈现后的 origin-form（含查询串）
    std::string host;                           ///< 目标主机（含端口；永不含用户信息段）
    std::uint16_t hop{0};                       ///< 重定向跳序号，0 起
    std::uint16_t pass{0};                      ///< 物理尝试序号，0 起
    std::uint16_t status{0};                    ///< 收到应答头则为其状态码；未收到为 0（体失败仍保留）
    std::string error_code;                     ///< 失败码；成功为空
    std::optional<HttpErrorClass> error_class;  ///< 失败归因；2xx 为空，≥400 为 Status
    std::optional<HttpStage> failed_stage;      ///< 失败时正在进行的阶段；本地预检拒绝为空
    std::chrono::microseconds elapsed{0};       ///< 本跳耗时；在途为取快照时已耗时
    std::uint64_t sent{0};                      ///< 本跳交给传输层的字节
    std::uint64_t received{0};                  ///< 本跳读回的线上字节；缓存命中为交付体字节
    bool in_flight{false};                      ///< 是否仍在途
    bool abandoned{false};                      ///< 协程帧未收摊即被销毁
    bool reused_connection{false};              ///< 本跳是否复用池内连接
    bool from_cache{false};                     ///< 新鲜缓存命中；304 重验证为 false
    bool truncated{false};                      ///< 某个文本字段因上限被替换或截断
};

struct ActivityOptions {
    bool record = true;                   ///< 缺省开：可观测性不配置也成立
    std::uint32_t capacity = 64;          ///< 环形容量（在途 + 已完成共用），钳到 [1, 1024]
    std::uint32_t max_value_chars = 256;  ///< 单字段文本上限，钳到 [32, 4096]
};

struct ActivitySnapshot {
    bool available{true};                    ///< Release 下为 false
    std::string reason;                      ///< available == false 的原因
    std::vector<HttpActivityEntry> entries;  ///< 最新在前
    std::uint64_t dropped{0};                ///< 因容量/缩容被淘汰的记录数
    std::uint64_t truncated{0};              ///< 至少一个字段被截断的记录数
};

/// @brief 事件消费者；三枚回调缺省空实现，宿主只覆盖关心的那一枚。
/// @note Thread: 可能来自发起线程或主线程，且不同交换可并发；实现须线程安全。
/// @note 回调须快、不得抛、不得做 I/O、不得重入 net；entry 仅在本次调用期间有效，留存请复制。
class NetEventSink {
  public:
    virtual ~NetEventSink() = default;
    virtual auto on_exchange_begin(const HttpActivityEntry &entry) noexcept -> void {}
    virtual auto on_stage(std::uint64_t exchange_id, const StageMark &mark) noexcept -> void {}
    virtual auto on_exchange_end(const HttpActivityEntry &entry) noexcept -> void {}
};

/// @brief 订阅进程级事件；宿主放手（weak 失效）即自动退订。
auto subscribe_events(std::weak_ptr<NetEventSink> sink) -> void;
/// @brief 读进程级活动配置（拷贝，线程安全）。
[[nodiscard]] auto activity_options() -> ActivityOptions;
/// @brief 写进程级活动配置；缩容立即从最旧开始淘汰并累加 dropped，record=false 只停新记录。
auto set_activity_options(ActivityOptions options) -> void;
/// @brief 清空全部记录（含在途）并归零 dropped / truncated；此后迟到的 end 被存储忽略。
auto clear_network_activity() -> void;
/// @brief 活动快照；surface 选呈现面，limit=0 为全部，>0 为最新 N 条。
[[nodiscard]] auto activity_snapshot(ExportSurface surface = ExportSurface::Timeline, std::uint32_t limit = 0)
    -> ActivitySnapshot;
/// @brief 同一 request_id 下 exchange_id 最大的一跳（含在途）；空/未知回 nullopt。
[[nodiscard]] auto timeline_of(std::string_view request_id) -> std::optional<HttpRequestTimeline>;
/// @brief 同一 request_id 下的全部跳，按 exchange_id 升序；空/未知回空数组。
[[nodiscard]] auto timelines_of(std::string_view request_id) -> std::vector<HttpRequestTimeline>;

}  // namespace aurora::net::http
```

## 关键口径

### 生命周期与容量

| 场合 | 存储 | 总线 | 结局字段 |
|:---|:---|:---|:---|
| 正常完成 | begin + marks + end | 同左 | `status` 为最终状态；`error_code` 空 |
| 传输失败 | begin + marks + end | 同左 | `error_code` / `error_class` / `failed_stage` |
| 取消 | begin + marks + end | 同左 | `net-cancelled`；**不**标 `abandoned` |
| 协程帧销毁 | 若仍在环内则 end | 同左 | `abandoned = true`，无伪造错误码 |
| 新鲜缓存命中 | begin + end（零 mark） | 同左 | `from_cache = true`，`sent = 0` |
| 打标前拒绝（取消、非法重试策略） | 不建记录 | 不发射 | —— |
| 打标后本地拒绝（策略/能力/可重放） | begin + end（零 mark） | 同左 | `failed_stage = nullopt` |
| 容量淘汰 | 丢弃最旧（含在途）+ `dropped` | 无 end 补造；真实 end 照常推 | 存储忽略迟到 end |
| `clear` | 全丢 + 计数器归零 | 不补造 | 存储忽略迟到 end |
| `record = false` | 不再收新；在途继续 | 照常 | —— |

### 脱敏键与呈现

| 键 | 分级 | 无策略时 | 备注 |
|:---|:---|:---|:---|
| `header.authorization` / `proxy-authorization` | Secret | 保留方案 + `len=N` | 名字大小写不敏感 |
| `header.cookie` / `set-cookie` | Secret | `"***"` | —— |
| `header.<其他>` | Sensitive | `"***"` | 需 `allows` 放行 |
| `query.<令牌名>` | Secret | `name=***` | 名字百分号解码后判 |
| `query.<其他>` | Sensitive | `"***"` | 需 `allows` 放行 |
| `userinfo` | Secret | `"***"` | 绝对 URL 独有 |
| `path` | Public | 原样 | 宿主可在 `present` 里收窄 |
| `host` / `method` / `request_id` / 状态 / 字节 / 时刻 | Public | 原样 | 主机永不含 userinfo |
| `tree.<属性键>` | Sensitive | 不 walk（无策略时逐字节不变） | 有策略时默认折叠 |
| `detail.<字段>` | 按字段 | 按字段 | 未来 detail 必须走此闸 |
| 请求体 / 应答体 | Secret | 永不出现 | 不折叠，直接不导出 |

## 验收判据

1. **注册表逐值不变（注册表切片）**：`tests/unit/utest_inspector_server.cpp` 的 `http_roundtrip` 一族对全部现存端点（十六枚精确 + 两枚前缀族的子路径）跑同一组请求/响应对，响应体、状态码、404 与 405 文案逐值对照；两枚前缀族的 label 不带尾斜杠另有断言。
2. **精确优先与变异自证（注册表切片）**：`utest_route_match.cpp` 用合成重叠表断言精确项整体优先、前缀按登记次序兜后；至少三处变异：① 两趟查找顺序颠倒 ⇒ 合成重叠例转红；② 前缀 label 带上尾斜杠 ⇒ 405 文案断言转红；③ 把某枚前缀项误登记成精确项 ⇒ 该族全部子路径转 404。生产表的等价性由第 1 项的全量对照承担，不另称一条无法暴露私有表的「不变量用例」。
3. **方法闸交集纠回（注册表切片）**：`DELETE /api/widget/`、`/api/widget//` 一类「动词不支持且路径退化」的交集回 405，其余路径的 400 / 405 / 404 分流与改造前逐值相同；差异与理由同批登记为网络栈提案的追加裁决。
4. **一次记账、多路消费（记账与脱敏切片）**：同进程挂两枚测试消费者（一枚只累计计数、一枚留存全量），一次交换后两者收到同一组 `on_exchange_begin` / `on_stage` / `on_exchange_end` 且计数相等；消费者析构后总线不再回调且此后发射不崩；无订阅者时存储仍按 `record` 记账（Inspector 不依赖订阅）；`record=false` 后新交换不产生条目、在途条目仍收摊。用例落计划/待建的 tests/unit/utest_net_observability.cpp，每例前后重置进程级存储与策略。
5. **生命周期配对（记账与脱敏切片）**：正常、失败、取消三条路径各自恰好一枚 `on_exchange_end`，收摊次数等于起手次数；取消的结局是 `net-cancelled` 且 `abandoned=false`；协程帧在收摊前销毁 ⇒ 恰一枚 `abandoned=true` 的 end；新鲜缓存命中 ⇒ begin+end 紧邻、零 mark、`from_cache=true`；打标前取消与非法重试策略 ⇒ 零记录；打标后策略/能力拒绝 ⇒ 有记录、零 mark、`failed_stage=nullopt`。
6. **容量与饥饿（记账与脱敏切片）**：`capacity=2` 连发三条永不收摊的交换，断言最旧的在途条目被淘汰、`dropped=1`、新条目仍能进表；被淘汰者随后真实收摊时存储不复活、不补造 end，而订阅者仍收到它的真实 end；`clear` 清空并归零计数，迟到 end 被忽略；缩容立即淘汰并累加 `dropped`；`record=false` 不清旧记录。
7. **有界预算（记账与脱敏切片）**：超过 `max_value_chars` 的原始目标存占位符而非前缀（断言原值前若干字节不出现在快照中），`truncated` 计一次；呈现值超限按上限截断；`limit` 只影响返回条数、不影响计数器。
8. **阶段语义、缺席与单调（记账与脱敏切片）**：出现的 mark 偏移单调不减且 ≤ `total`；`Connect` 覆盖 DNS/TCP/代理隧道，DNS 失败给 `error_class=Dns` 且 `failed_stage=Connect`；复用连接的那一跳没有 `Connect` / `TlsHandshake`，首枚是 offset 0 的 `ConnectionAcquired` 且 `reused_connection=true`；`RequestSent` 在 `FirstByte` 之前，`BodyDone` 在最后；中间重定向跳、`sink` 主动停读、WebSocket 握手没有 `BodyDone`；200 的体中途失败仍保留 `status=200` 且 `failed_stage=BodyDone`；本地预检拒绝 `failed_stage` 为空。
9. **序标与查询（记账与脱敏切片）**：`exchange_id` 进程内唯一递增；一次重定向 + 一次重试 + 一次凭据重放的交换按 `hop` / `pass` 展开，条数等于物理往来数，`pass` 不因凭据重放重复；`timeline_of()` 取最新一跳（含在途）、未知 id 回 `nullopt`、空 id 回 `nullopt`；`timelines_of()` 按 `exchange_id` 升序；`X-Request-Id` 被显式关闭的交换仍进快照但 `request_id` 为空、不可被 id 查询寻址。
10. **四出口同一断言集（记账与脱敏切片）**：同一份带 `Authorization` / `Cookie` / URL userinfo / 令牌查询参数的交换，在 `Timeline` 与 `InspectorJson` 两条活出口各断言原值不出现、折叠形态出现；`Sensitive` 键未放行时折叠、按 `(导出面 + 键)` 放行后出原值；`Secret` 在宿主注册「原样返回」策略下仍折叠（证明不可放大）；`present()` 抛出时回退折叠且交换不受影响；消息体不出现在任何出口。`Log` 与 `ErrorDetail` 以 helper 单测钉住头部/`detail.*` 的折叠，并断言当前日志行与错误 message 不含原值（守门，不冒充活出口）。
11. **Inspector 脱敏范围（记账与脱敏切片）**：`/api/tree`、`/api/debug/tree`、`/api/yaml`、`/api/widget/{path}` 在未安装策略时输出与改造前逐字节相同；安装策略后，`props` / `values` 字符串按 `tree.<键>` 折叠，而 `type`、结构键与 `descriptor.name` 不被折叠；`/api/to_code` 不 walk（纯输入变换）；`/api/find`、`/api/debug/pick`、`/api/patch` 无属性值导出；MCP 的 `live_tree` / `live_widget_get` 经同一端点继承。
12. **Release 与布局（记账与脱敏切片）**：`AURORA_ENABLE_DEBUG=OFF` 构建下 `activity_snapshot()` 回 `{available:false, reason}`、`timeline_of()` 回 `nullopt`、`timelines_of()` 回空、`subscribe_events()` 为 no-op；Inspector 的树脱敏在该构建下仍生效（证明脱敏未被裁切）；`Client` / `Response` 公共头无本提案新增数据成员（以头文件 diff 审查记录）；`ON` 构建断查询有值。两种构建各一枚同名用例，跳过形态按 `AURORA_TEST_SKIP` 纪律登记。
13. **端点与工具往返（暴露面切片）**：`GET /api/network` 的 `limit` / `host` / `status` 过滤、`GET /api/network/request?id=` 的多跳展开与 400 / 404 分流，在 `utest_inspector_server.cpp` 走 `http_roundtrip` 逐值断言，含 unavailable 形态；MCP 的 `tool_definitions()` 报 24 枚、`execute_tool()` 两枚新派发臂各一条 stdio 往返（含缺省 limit 与按 host / status 过滤），落 `tests/integration/itest_mcp.cpp`（现 16 例，本片新增两枚工具往返与 22 → 24 计数断言）。
14. **手工用例与门禁（收口）**：`codespec/manual-test/23-net.md` 新增 TC-NET-008，按 §1.1 第二类准入（真实 Inspector 进程 + MCP stdio 往返）人工核对端点 JSON、时间线各阶段与两枚工具的实际返回；前置条件照 TC-NET-007 先例点名载体、端口发现方式与 `aurora_mcp` 接入命令行。`ctest --preset ninja-test` 全量（含全部 `check_*`），其中 `check_api_schema_sync`（先 `--target aurora_api_json`）、`check_api_budget`、`check_naming_conventions`、`check_doc_comments`、`check_umbrella_header`、`check_core_layer_boundary`、`check_arch_module_map`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`check_no_cjk_literals`、`check_no_hardcoded_paths`、`check_version_consistency`；改动 TU 逐个过 `lint`，`git add` 之后复跑 `format-check`。

## 风险与取舍

- **`path` 定为 `Public`**：路径段里的令牌不在库侧可推断范围；缓解是 userinfo 与令牌查询参数强制 `Secret`，且 `Public` 允许宿主在 `present()` 里收窄。把整条路径默认折叠会废掉 Inspector 的主功能，不取。
- **进程级单表**：多租户/多会话隔离不在本件范围；将来若要按 `Session` 过滤，在快照口加过滤参数即可，不必改存储形态。进程级配置意味着一个宿主库的 `record=false` 会影响同进程其他发起方——这是「一个进程一份答案」的代价，文档写明。
- **调试构建限定**：Release 下两枚端点恒 unavailable；这是「零开销」的代价，不靠运行期开关假装存在。
- **回调线程**：`NetEventSink` 可能并发回调，宿主必须自己线程安全；总线只保证名单快照与锁外调用，不替宿主串行化。
- **树脱敏只覆盖字符串叶子**：数值型秘密（如把 PIN 存成整数）不在库侧可识别范围；宿主知情面由 `present()` 决定，文档写明这一边界。
- **公共面预算**：三枚举 + 五结构体 + 两抽象类 + 十二自由函数；`check_api_budget` 在落码前先以重建后的 `aurora_api.json` 实测，若逼近 75k 上限按既有预案收敛注释与结构，不删语义。

## 回写落点

1. `codespec/specification/06-app-platform.md`：新增四枚小节——§8.8.36 网络事件总线与进程级活动存储、§8.8.37 请求时间线与活动条目、§8.8.38 凭据分级脱敏与进程级 `ExportPolicy`、§8.8.39 Inspector 路由注册表与 network 端点；各含契约、三枚回调契约、线程口径、收摊与预算纪律、判据与变异自证记录。
2. `codespec/specification/08-tooling.md`：§10.2 补网络可观测端点与脱敏细则；§7.1 工具清单表补齐九行存量漂移（`live_*` 五枚与 `build_ui_prompt` / `compare_snapshot` / `generate_ui` / `repair_tree` 四枚）并加两枚新工具；§10.6 的 MCP 工具数按补齐后的 24 改写（现状「13」同批纠回）。
3. `codespec/SPECIFICATIONS.md`：§5 特性表 `SPEC.FEAT.NET.OBSERVABILITY.001` 行的「规格落点」列补 §8.8.36–§8.8.39 指针，判据措辞随实现形态收口。
4. `codespec/CODING_STANDARDS.md`：§4.1 双通道段补「凭据一律折叠、四出口同口径、库侧 `Secret` 类不可由宿主放大、消息体不导出」硬规则；§15 反例速查加一条「把 `Authorization` 原值直接拼进日志」的反例。
5. `codespec/BUILD_OPTIONS.md`：`AURORA_ENABLE_DEBUG` 条目登记本提案造成的后果——该宏决定事件总线、活动存储与三处 emit 点是否存在（宏关时 `activity_snapshot()` 回 unavailable、`timeline_of()` 回 `nullopt`），**不涉及任何公共类型的布局**；脱敏 helper 与策略不随宏裁切，Release 的 Inspector 树导出仍受保护。
6. `codespec/ARCHITECTURE.md`：§4 的 net 行补事件总线与活动存储的归属，§14 的测试与 CI 段登记 Inspector 路由注册表、两枚端点与 Release 双构建判据。
7. `codespec/manual-test/23-net.md`：§1.1 被测模块表补两枚新公共头，§1.2 执行载体表补一行（Inspector 宿主 + `aurora_mcp` stdio 是一类新载体，端口发现方式随行写明），§2 用例清单加 TC-NET-008，§3 执行记录表随片填写。
8. `codespec/changes/xplat-network-stack/proposal.md`：§13 的可观测性条目补「落地现状」；§16 末尾追加裁决——① 本提案定案取代第 9 项后半（取消 lint 规则那条，旧编号与旧文字保留）；② 给第 8 项的注册表化补「方法闸统一收派发处」与 400→405 差异；③ 阶段模型**不设** `Dns`，DNS 折在 `Connect` 内、DNS 失败记 `failed_stage=Connect` + `error_class=Dns`（纠正 §13 的「DNS / connect」措辞）；④ 活动表改为进程级存储、含在途、容量淘汰不豁免在途、不补造 end；⑤ 活动表不经 `aurora::debug` 门面，Inspector 直接消费 `net::http`（纠正 §13 的「经门面 JSON 导出」）；⑥ 单段 `present()` 改两段式 `allows()` + `present()`（`Sensitive` 可放行、`Secret` 不可放大）；⑦ MCP 工具数按 24 收口；§18 第 5 项的两级查找补两趟实现的定形；§17 的可观测性条目随切片的收口更新（`use(hook)` 不在本切片，该条不动）。
9. `codespec/errors.toml`：本件不新增码；端点的 400 / 404 / 405 是 HTTP 状态而非库错误码。落码中若出现「活动存储自身的失败形态」一律走既有码并在 detail 点名，届时按实际分支决定是否需要条目 hint 改写（默认：无改动）。
10. 代码与数据面（不属 codespec 树，此处只记必做动作）：根目录 CHANGELOG.md 记 Added 段（两枚新公共头 / 三枚举 / 五结构体 / 两抽象类 / 十二自由函数 / 两枚端点 / MCP 24 枚 / `ExportPolicy` 树脱敏）与 Changed 段的 Inspector 405 差异；根目录 aurora_api.json 经 `--target aurora_api_json` 重建（enums 段 +3 = `HttpStage` / `ExportSurface` / `FieldClass`）；tools/check/umbrella_manifest.txt 收录两枚新公共头，include/aurora/aurora.h 同步加入两枚直接包含；tools/include/known_enums.h 与 tests/integration/itest_known_enums.cpp 各加三枚枚举锚点。debug_api.toml 与生成器**不动**（本件不加 `aurora::debug` 门面函数）。
