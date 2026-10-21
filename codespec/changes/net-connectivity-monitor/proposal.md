# CHG-028 网络链路态监听：缺省路由采集腿与不对称状态机、连接旁证插队、订阅投递与 network 端点的 connectivity 字段

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-028 |
| 提出日期 | 2026-10-21 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | 一枚新公共头 include/aurora/net/connectivity.h（三枚举 `NetworkState` / `ConnectivitySource` / `ConnectivityChangeCause`、三结构体 `ConnectivityFact` / `ConnectivityChange` / `ConnectivityOptions`、两类 `ConnectivityMonitor` / `ConnectivityRegistration`、三枚令牌换算自由函数，合计八个类型）。**零新增错误码**（不可得一律复用既有 `net-platform-unavailable`）、**零新增 feature 宏**（`BUILD_OPTIONS.md` §1.2 的「net 面无开关」口径不变）、`Client` / `Response` / `Engine` / `Capabilities` / `ExchangeHook` **零改动**、**零新线程**、**零出站 I/O**；`/api/network` 的**正常形状与 Release 早退形状**顶层同时追加同一枚 `connectivity` 对象（既有字段逐值不变），MCP 工具数 24 不变；Windows 采集腿所需的 `<winsock2.h>` 只出现在 `socket.cpp` 内，平台 socket 头唯一落点不变量不放宽；伞形提案 §15 那句「不做网络状态监听与自动重连编排」由 §16 追加裁决**改判前半句、保留后半句**，旧编号与旧文字一律在位（防伪死链） |

## 动机

1. **§15 那一句话捆住了两件本不该捆在一起的事。**原文是「不做网络状态监听与自动重连编排（`RetryPolicy` 只覆盖单请求重试，链路级重连留给使用方与 `Scheduler` 组合）」——后半句至今成立且必须继续成立：编排要的是策略（退避多久、试几条、给不给重试按钮），那是宿主与 `Scheduler` 的地盘。前半句被捆着否掉的理由今天已经不成立：链路态本身是一枚**事实**，不是一条策略，而需要这枚事实的地方一个都不缺（界面的离线徽标、宿主的退避决策、AI 排障的第一问）。本提案只把前半句拆出来，后半句逐字保留。

2. **「这一批失败是不是同一场链路故障」今天没有任何一处理由能回答。**`error_class` 九档能给单次归因，但它给的是**这次连接为什么没成**，不是**本机现在有没有链路**。库自己的错误表已经把这件事写死了：`net-connect-timeout` 的 hint 原文是「a black-holed peer or an unreachable route looks identical to a slow one from here」——从库里看，黑洞的对端与断掉的路由是同一件事。§5 的 `SPEC.FEAT.NET.OBSERVABILITY.001` 要 AI 能反查到失败阶段与归因，而**归因链条的第一环（当时到底有没有网）今天缺位**，AI 只能从十条失败记录里猜。

3. **不提供它，宿主只能自己起一条线程轮，而每次手写都会漏掉同一条。**这与 `net/http_binding.h` 的立项论证同构（那节写的是「每次由宿主手写就各写一份，且几乎每次都漏掉同一条：取消只丢结果、不掐连接」）。链路态这边漏的是：轮询线程没有退出路径、状态更新发生在非主线程（正面撞「引擎线程绝不触碰 widget / `State`」那条不变量）、以及把「读不到」当成「离线」。库出一枚，这三条就有一次收口的位置。

4. **它不能成为发起侧的闸门——这是本提案的边界，也是最容易被做错的地方。**探测口径与真实路径本来就不等价：服务可以只在链路本地可达、VPN 的路由表在探测面里看不见、容器里读不到任何有效拓扑。拿它当闸等于把可交付的请求变成**库侧自己发起的失败**，而那类失败对端不留痕、时间线上看不出根因，恰是最难排查的一种。闸的位置已经在 `NetPolicy` 与能力闸那一层，且本地收场只有既成的三档形态（见 `codespec/specification/06-app-platform.md` §8.8.36 的打标分档口径），不能再开第四种隐式失败。本件的产物只是读者可查询的事实 + 变更通知。

5. **采集形态之所以定为「同步读事实 + 失败插队」而不是逐平台原生事件，是三条实测阻塞换来的。**其一，Win32 的连通性通知给出的是 waitable `HANDLE`，而自带引擎的常驻循环建立在 `WSAPoll` 之上（`socket.cpp` 是唯一落点），内核对象进不了句柄数组——§16 第 12 项记过完全同一类冲突（`WSAEventSelect` 会把套接字置入事件态、与同步收发面互斥）。其二，Linux 的 `rtnetlink` 是一枚可等的 fd，看似能直接搭上 `EventSourceRegistry`，但它要求平台 socket 头出现在 `socket.cpp` 之外，正面破那条不变量。其三，Windows 上真正给出「有无外网」的 `INetworkListManager` 是 COM 接口，要引一套 apartment 生命周期与消息泵依赖。三条的合计代价是「至少一条自有线程或一套 COM」，而 §15 明写「不引入 GPU / 线程池语义变更」。同步采集把这件事压成一枚无句柄、无回调、可在引擎线程上调用的查询：**零新线程、跨平台同形、可在库内注入替身做逐值单测**；感知延迟的下限则由「失败插队」补回来——一次真失败的往返就是一次最诚实的探测。三平台的原语因此统一到同一句话：**是否存在一条经非回环、operational 接口的缺省路由**（Windows 路由/接口表、macOS 动态存储的全局 IPv4/IPv6 键、Linux 路由伪文件 + sysfs `operstate`）。

6. **刷新必须复用引擎自己的定时器，且必须承认它没有撤销口。**`Engine::post_after` 的既定契约是「到期时刻按 `steady_clock` 计算，与 `wait_ready` 的等待窗口共用同一条循环，**不额外起线程**」，且引擎惰性启动、`shutdown()` 时未到期的定时器**被丢弃**。前三条对本件恰好都是想要的；第四条在本件不需要任何例外——本件没有需要结清的续体。真正要处理的是它**没有撤销口**：插队一次不能靠取消旧定时器，只能用 epoch + 共享 live 标志让旧闭包到期时 no-op（`http_client.cpp` 的 `StageTimer` 同法）。同一刻最多两枚存活登记（一枚作废的周期拍 + 一枚证据拍），采样结束后只登记下一枚周期拍；失败风暴因此不会堆定时器。

7. **三档而不是四档：`Unknown` / `Online` / `Offline`，第四档的语义各平台给不齐。**「有链路、无外网」这一档在三个平台上是三件不同的事：macOS 的全局 IPv4/IPv6 键只说明缺省服务，不含互联网判定；Windows 的连通性提示与 wininet 的连接态分处两枚 API；Linux 只能靠旁证推断。在公共面上开一档就等于承诺一份只有个别平台说得清的语义，而 AI 读到它时无法据此分支——这比没有这一档更糟。`Online` 的定义因此收窄为「**存在可用的缺省链路**」，**不等于互联网可达**：captive portal、VPN 的策略路由、DNS 是否可用都不在三态语义内。需要细节的场合由附带事实承担：`source`（编译期采集腿令牌，不是「上次读成功」）、`cause`（这次变更由哪一路证据促成）、`generation`、`since`、`stale`、`last_failure`（冻结错误码 slug，**不含主机名与 URL**）。

8. **`Unknown` 是一条独立的态，既不向 `Online` 也不向 `Offline` 折算。**这条纪律仓内已有先例且写法完全一致：系统代理侧「通道存在而被拒绝报 `net-proxy-config-unreadable`，只有本腿根本没有这个概念时才回空值」、「PAC / WPAD 一律报 `net-proxy-unsupported`，绝不回空集合」。链路态同构：读不到就是读不到，把它读成「离线」会让界面显示一个假故障、把它读成「在线」会让宿主停止退避。无采集源要分两类写死：wasm 既无采集腿、其 fetch 腿又不经 `await_connect_via`，**无任何证据通路**，`subscribe()` 直接回 `net-platform-unavailable`；BSD / Android 一类无腿但有建连旁证，允许状态随成功建连推进，`refresh()` 仍回 `net-platform-unavailable`。Headless 不是平台：Windows / Linux / macOS 的无头进程照样有采集腿，`source` 是 `IpHelper` / `DynamicStore` / `LinkFiles` 而不是 `None`。

9. **状态机的阈值必须不对称：判断快、判通慢；并且只有探测腿能判离线。**误判离线的代价是界面多一枚不准确的提示，而宿主的请求照样发得出去、失败照样按九档归因，事实没有丢失；误判在线的代价是宿主据此**停止退避并立刻重试**，在真断链的窗口里把一次故障放大成一片刷屏的失败。两侧代价不等，故阈值不等：转入 `Offline` 需 `offline_evidence` 枚合格的**探测**读数（缺省 1，即一次采样明确读到无可用缺省路由），转入 `Online` 需 `online_evidence` 枚一致正证（缺省 2：连续两次采样在线，或一次采样在线加一次成功建连）。**连接失败一律不作负证**：`Dns` 同时覆盖 NXDOMAIN 与解析器故障，`Unreachable` 的 `NetConnectFailed` 同时覆盖 ECONNREFUSED 与 ENETUNREACH，`Proxy` 覆盖 407、协议错误与配置读不到；把任一类当链路负证都会造出假离线。失败只触发一次合并采样并把码写进展示字段；`Timeout` 同样只触发不判决——触发条件比证据条件宽，这件事只关乎何时多问一次，不关乎拿它下结论。

10. **「静止期状态冻结」必须显式承认，所以载荷里要有 `stale`。**没有订阅者且没有交换时，本件既不采样也不推进状态——这是刻意的（见动机 12 的预算段），但代价是恢复流量后第一次读数可能是几分钟前的旧事实。两条补法一起做：`stale` 位（距上次成功采样或合格正证超过 3× 采样周期即置真，阈值由周期派生、不留第二枚旋钮，否则两枚旋钮能配出自相矛盾的组合），以及**订阅成功即插队一次采样**（新读者永远拿到当下的事实而不是历史）。`stale` 在 `snapshot()` 读取时按同一公式重算，**读取本身不触发 I/O、不发布通知**；`StalenessMarked` 只在采样循环观察到 false→true 时发布一次。

11. **旁证通道不能走事件总线，尽管总线看起来是现成的。**总线在 `AURORA_ENABLE_DEBUG=OFF` 下整类退化为零成本空壳（`net/detail/observability_trace.h` 的编译期裁切），而 `BUILD_OPTIONS.md` §1.2 定的是「`aurora::net` 的事件引擎、事件源注册口与传输族**没有开关**：公共 API 恒在」。二者共用一条通道会让 Release 构建下的链路态**变钝**——一条没人想要、且判据要写两遍的宏行为差异。故本件另开一枚库内单点入口，由全库唯一建连出口 `detail::await_connect_via` 的结局处调用（四条腿都从这一个函数经过，不在 HTTP 各处重复插桩）：`detail::note_connection_outcome(bool connected, bool loopback_peer, std::optional<ErrorCode> code)`。证据表只有四行——成功且非回环计一枚正证；成功但回环不计（本机自连不证明外链）；任何非 `Cancelled` / `Invalid` 失败只触发合并采样并更新 `last_failure`；`Cancelled` / `Invalid` 无动作。分类仍复用 `error_class_of`（只用来筛触发与排除取消），不新造第二套归因。wasm 的 fetch 腿不经过该出口，因此它对链路态恒无贡献——这条缺口如实留在无证据通路上，不假装补上。

12. **有界性这条老纪律在这里的正确落点：唯一一枚有效节拍，插队必须合并，探测本身有成本上限。**§15 里那条「有界性必须是预算，而不是仁慈」在本件只有一处真实压力：连接风暴。断链瞬间可能有几十次交换同时失败，每次都要插队采样的话，几十枚定时器加几十次同步查询会挤在引擎线程上。裁决是**单一待采样标记（pending flag）加 epoch 作废旧拍**：已有未到期定时器时后续插队一律合并，同一轮内无论多少次失败只采一次。订阅表本身不设上限（与 `EventSourceRegistry` 的线性表同口径：规模由宿主句柄生命周期决定，析构即回收），因为订阅不是洪水面——真正的洪水面在触发侧，就在触发侧收口。探测成本同样入预算：只读 OS 表与伪文件，成本随接口数线性，无 DNS、无 `connect`、无包；桌面量级低于 1ms，且只在引擎 poll 线程执行，绝不进入交换热路径。周期采样只在有订阅者时运行；证据拍可在无订阅者时运行一次（那正是「有交换」的场合），末枚订阅注销后不再有周期拍。

13. **响应式一侧只到「回调恒回主线程」为止，不再往上长成 `Resource` 或 `Signal`。**三条既有裁决一起把这条路堵死：分层方向裁决禁止 L1 公共头反向依赖 `Task` / 协程（`resolver.h` 那条写得很明白）；`state/resource.h` 的纪律是「不认识网络」，接线件住在 net 侧且把**一次交换**折算成 `Task`，而链路态是持续态、没有「一次落定」可折算，`reload()` / `failed` 的语义套不上去；进程级单例持有 `Signal<>` 会让引擎线程直接写响应式状态，正面撞「引擎线程绝不触碰 widget / `State`」。所以交付形态与 `Resolver` 同构：回调 + 经 `aurora::detail::post_to_main` 回投（与 `Task<T>` 同一条通道，不新增第三种排队），宿主把值写进自己的 store 只需要一行。

14. **本件最高频的消费者已经建好了，顺手接上比留在库里更划算。**可观测切片刚落地 `/api/network` 与两枚 MCP 工具，而 AI 排障的第一问恰恰是「这九条红是不是一场链路故障」。今天它要跨两次工具调用从失败档位里反推。裁决是在 `/api/network` 顶层追加一枚 `connectivity` 对象（既有字段与既有用例逐值不变，MCP 消费的就是同一份 Inspector 文档、**不新增工具**、不在两处登记面继续膨胀），把这一问答成一次读数。**正常形状与 Release 早退形状必须同时有它**（后者原本只有 `available` / `reason`），且由同一枚构造器产出：否则 `AURORA_ENABLE_DEBUG=OFF` 下 AI 的第一问恰好读不到，而本件的公共面按 §1.2 恰恰不随 DEBUG 裁切。

15. **导出面里不放拓扑事实。**链路态三档与 `source` 令牌属于凭据分级里的 `Public`（库只上调不下调，宿主策略也折叠不了它，因为它不含凭据）；而接口名、网关地址、SSID、链路本地地址**一律不进入任何导出面**，连内部快照都不留——内部探测快照只含 `bool` / 枚举 / `optional<bool>`，接口名只在采集 TU 内部用于拼路径，函数返回即丢弃。它们是拓扑信息，不是本件需要的事实，也不该顺手成为一个新的泄漏面。这条沿用可观测切片「协议事实只有库知情，由库强制裁决」的那一侧边界。

## 变更内容

### 1 公共面：一枚新头、八个类型、三枚令牌换算

新头 include/aurora/net/connectivity.h（计划/待建，伞头收录、不依赖 `state/`、不含任何平台头、零 `#ifdef`），落在 `aurora::net` 而**不在** `aurora::net::http`——链路态属于传输层事实，wasm fetch 腿与 SSE/WS 同样受它约束，放进 HTTP 命名空间会误导读者以为它只描述一次交换。

- `enum class NetworkState : std::uint8_t { Unknown, Online, Offline }`。`Online` 的注释必须写明「存在可用缺省链路，不承诺互联网可达」；`Offline` 必须写明「仅由探测腿明确读到无可用缺省路由时成立」；`Unknown` 必须写明「无源 / 读不到 / 尚未采样」。
- `enum class ConnectivitySource : std::uint8_t { None, IpHelper, DynamicStore, LinkFiles }`——编译期采集腿令牌，构造后不随读数成败变化。`None` 表示该构建没有采集腿（wasm、BSD / Android 一类）；`IpHelper` / `DynamicStore` / `LinkFiles` 分别对应 Windows 路由/接口表、macOS 动态存储、Linux 路由伪文件 + sysfs 三条腿。**不设 `Injected`**：替身只经库内钩子安装，公共面不出现「正在替身下」这类实现态；用例断言的是行为与读数，不靠枚举自证。
- `enum class ConnectivityChangeCause : std::uint8_t { SubscriptionStart, PeriodicSample, FailureEvidence, SuccessfulConnect, HostRefresh, StalenessMarked }`——一次发布只带一枚 cause；多个触发合并时**首因保留**（先登记的那一轮决定 cause）。六枚依次对应：订阅触发的首采（未被合并时）、周期拍、失败触发的插队拍、成功建连的正证或它触发的插队拍、`refresh()` / `set_options()` 的宿主动作、`stale` 的 false→true 标记。
- `struct ConnectivityFact { NetworkState state; ConnectivitySource source; std::chrono::steady_clock::time_point since; std::uint64_t generation; bool stale; std::optional<std::string> last_failure; }`——值类型。`generation` 只在对外发布的变更上单调 +1（状态转换或 `stale` 翻转）；`snapshot()` 重算出的 `stale` 是派生读数，不推进代际，代际只标识已发布的通知序列。`last_failure` 是冻结错误码 slug（如 `net-connect-failed`），只做展示，**静默更新、不推进代际、不单独通知**，转入 `Online` 时清空。`since` 只在 `state` 变化时更新，初值取 monitor 建立时刻，**不是** epoch 零点。
- `struct ConnectivityChange { ConnectivityFact previous; ConnectivityFact current; ConnectivityChangeCause cause; }`——`previous` 是**该订阅者**上次收到的 `current`（首条为订阅时刻的 `snapshot()`），不是全局上一枚事实；合并待投递变更时以最后一次为准、`previous` 仍锚在订阅者已见值上。
- `struct ConnectivityOptions { std::chrono::milliseconds sample_interval; std::uint32_t offline_evidence; std::uint32_t online_evidence; }`，缺省 `5000ms / 1 / 2`，经 `ConnectivityMonitor::defaults()` 在声明处可见（`SPEC.API.SENSIBLE-DEFAULTS.001`）；`sample_interval` 钳到 `[100ms, 1h]`，两枚证据数钳到 `[1, 16]`。`stale` 阈值 = 3× `sample_interval` 写定、不设旋钮。
- `class ConnectivityMonitor`：`instance()`（进程级单例，函数内静态构造并刻意不析构，避免静态拆除阶段与引擎/订阅表的次序问题，与 `shared_engine()` 同口径）、`snapshot()`（恒可读，只做一次加锁拷贝与 `stale` 重算，绝不触发 I/O 或采样）、`options()` / `set_options()`（自下一轮采样起效，不重置在途证据计数、不立即转换；有腿时安排一次 `HostRefresh` 采样（已有待采样轮次则合并；按引擎惰性启动口径，可能拉起线程），无腿时只存值；`set_options` 无返回值，登记失败只清 pending、不报错）、`defaults()`、`refresh()`（返回 `Result<void>`，在引擎线程安排**恰好一次**采样，已有待采样轮次则合并、首因保留；本腿无采集源回 `net-platform-unavailable`；返回值只表示「请求已受理」，结果经 `snapshot()` / 通知抵达）、`subscribe(std::function<void(ConnectivityChange)> callback)` → `Result<ConnectivityRegistration>`（回调按值收取；空回调回 `net-option-failed`；无采集腿且无证据通路回 `net-platform-unavailable`；有采集腿时登记成功即安排一次 `SubscriptionStart` 采样，无腿但有证据通路的平台只登记、等证据驱动；**返回前绝不回调**）。
- `class ConnectivityRegistration`：move-only RAII 句柄，逐字沿用 `EventSourceRegistration` 已定案的三条契约（析构即注销且幂等、移动后源句柄不再二次注销、已入队的一次尾批仍抵达且 `cancel_pending()` 与析构互不隐含），并补三枚成员说明：`is_registered()`（抛出型订阅者被摘除后转假）、`token()`、`cancel_pending()`（只作废已排队的投递，不注销订阅）。
- 三枚自由函数 `network_state_text(NetworkState)` / `connectivity_source_text(ConnectivitySource)` / `connectivity_change_cause_text(ConnectivityChangeCause)` → `std::string_view`，交出 ASCII 令牌（`unknown` / `online` / `offline`；`none` / `ip_helper` / `dynamic_store` / `link_files`；`subscription_start` / `periodic_sample` / `failure_evidence` / `successful_connect` / `host_refresh` / `staleness_marked`）。存在理由同 `http_stage_text()`：REST 与 MCP 侧不得自己拼字面量，令牌只有一处权威。

### 2 采集腿：一处平台头、一处纯换算、一处注入点

严格照 `system_proxy` 那对的形状拆成三段，一段都不许合并：

- 计划/待建的 src/aurora/net/connectivity_probe.cpp（采集腿）：`#if defined(AURORA_PLATFORM_*)` 分派，**只出同步查询、只出一枚内部读数**（`struct ProbeReading { std::optional<bool> online; }`，`nullopt` = 读不到），平台头不外泄（同 `system_proxy.cpp` 的三条落码纪律：采集到的东西要能被纯换算层判死、平台头不出现在任何其它 TU、读不到不等于没配）。三腿统一语义为「**存在经非回环、operational 接口的缺省路由**」：
  - **Linux**：读 `/proc/net/route`（IPv4 缺省：Destination 全零、Flags 含 RTF_UP 且不含 RTF_REJECT）与 `/proc/net/ipv6_route`（IPv6 缺省：目的前缀长 0、Flags 含 RTF_UP），再用接口名读 `/sys/class/net/<if>/operstate` 与 `flags`（IFF_UP）。`operstate` 聚合规则写死：`up` 与 `unknown` 视为 operational，`down` / `dormant` / `lowerlayerdown` / `notpresent` / `testing` / 读不到均不视为；`lo` 一律排除。两族文件都读得到但都无可用缺省路由 → offline；**有缺省路由但其接口状态读不到 → unreadable**（不能拿「读不到」当「接口是 down」）；接口状态读得到但非 operational 的路由不算，若再无可用路由且两族文件都可读 → offline；任一读不到且其余可读文件均无路由 → unreadable；任一读得到且有可用路由 → online。
  - **macOS**：`SCDynamicStoreCopyValue` 读 `State:/Network/Global/IPv4` 与 `.../IPv6`，取 `PrimaryInterface` 并排除 `lo0`；键存在且接口非回环 → online；两键都不存在 → offline；动态存储创建失败或键存在但取不到 `PrimaryInterface` → unreadable。**不用** `SCNetworkReachability`：它已弃用，且取 flags 需要 socket 头。`SystemConfiguration` / `CoreFoundation` 已由 CMake 顶层链接（`CMakeLists.txt` 的 Apple 块），无需新增框架。
  - **Windows**：调用 `detail::probe_default_route_win32()`，该函数**实现在 `socket.cpp` 内**（`<winsock2.h>` → `<netioapi.h>` → `<iphlpapi.h>` 的既有唯一落点），对 `connectivity_probe.cpp` 只暴露「返回 `optional<bool>`」的无平台类型签名。实现用 `GetIfTable2` + `GetIpForwardTable2`：只认目的前缀为 `0.0.0.0/0` 或 `::/0`、接口 `OperStatus == IfOperStatusUp`、`IF_TYPE` 非 `IF_TYPE_SOFTWARE_LOOPBACK` 且非回环的行；API 失败 → unreadable。**不用** `GetNetworkConnectivityHint`：它要求 build 19041+，且本机 MinGW 头没有声明；路由/接口表自 Vista 起可用。链接面统一由 CMake 在 Windows 顶层补 `iphlpapi`（MSVC 与 MinGW 同路），**不依赖** `#pragma comment(lib, ...)`——MinGW 不认，单靠它会漏链。
  - 其余平台与 wasm 回 `None` / 无读数。三条腿都在接口内部消化拓扑名，返回值只有 `optional<bool>`。
- 计划/待建的 src/aurora/net/detail/connectivity_probe.h（内部接缝）：只含 `ProbeReading`、`compiled_source()` 与 Windows 查询 `probe_default_route_win32()` 的声明，不含任何平台类型、不进公共面；`socket.cpp` 与采集腿都包含它，Windows 分支的实现仍只在 socket.cpp。
- 计划/待建的 `src/aurora/net/detail/connectivity_state.{h,cpp}`（判据层，纯逻辑、可逐值单测）：输入 `ProbeReading`、正证、读失败与注入时钟，输出状态机转换与计数变化；`stale` 派生、`generation` 推进、`since` 更新、`last_failure` 清理都在这里裁决。这一层不碰时钟之外的任何进程状态，也不认识任何平台类型。
- 计划/待建的 src/aurora/net/connectivity.cpp（公共面）：`instance()`、订阅表（线性表 + 一把互斥量）、epoch 定时、pending 合并、经 `post_to_main` 回投。锁内只做事实更新与闭包快照；探测 I/O、发布与回调一律锁外。
- 库内注入点 `detail::set_connectivity_probe_hook(std::function<ProbeReading()>)` 与 `detail::reset_connectivity_for_test()`：与 `set_lookup_hook()` / `set_system_proxy_hook()` / `set_engine_override()` 同一条思路（能改变运行期事实的开关一律留在库内、不进公共面、不占 API 预算），装/拆成对由用例侧 RAII 包装负责。钩子只在引擎线程调用；用例据此断言线程与调用次数。

### 3 旁证插桩：一个函数、一枚入口、四行证据表

`detail::note_connection_outcome(bool connected, bool loopback_peer, std::optional<ErrorCode> code)` 只在 `detail::await_connect_via` 的结局处调用一处（四条腿都经它，HTTP 侧不再各插一遍）。成功分支在交还传输前用 `StreamTransport::is_loopback_peer()` 取值，不保留传输；失败分支取 `err.code_enum`。证据表：

| 结局 | 动作 |
|:---|:---|
| 成功且 `!loopback_peer` | 计一枚正证（`Unknown→Online` 一枚即可；`Offline→Online` 需 `online_evidence` 枚）；计票后仍未到 `Online` 时再触发一次合并采样，求第二枚一致读数；已到 `Online` 则不采样 |
| 成功但回环 | 不计、不采样（本机自连不证明外链） |
| 非 `Cancelled` / `Invalid` 的失败 | 只触发一次合并采样，并把 `ErrorMeta::slug` 写进 `last_failure`；**不计负证** |
| `Cancelled` / `Invalid` | 无动作 |

`error_class_of` 只用于筛「是否属于网络交换的失败」与排除取消，不再用它把失败折成链路负证。无采集腿的平台没有可读的事实，失败只写 `last_failure`、成功只计正证，都不登记采样。本入口不阻塞、不做 I/O、不改任何交换结局，且 monitor 自身永不发起连接，故不存在自激回环。`last_failure` 是展示字段：它可能停在一次与当前链路无关的失败上（例如代理 407），状态仍由探测腿裁决，读者不得据此反推离线。

### 4 线程、定时与投递纪律

- **一把互斥量**保护事实、两侧证据计数、pending、epoch 与订阅表。探测在锁外执行，读数回锁内应用；发布与订阅者回调也在锁外。`snapshot()` 只加锁拷贝并重算 `stale`。
- **单例**用函数内静态构造并刻意不析构；订阅表与定时闭包持有的共享状态随之活到进程结束，避开静态拆除次序。
- **定时**：周期拍只在至少一枚订阅存在时登记；失败拍无条件触发（用于发现掉线），正证拍只在计票后仍未到 `Online` 时触发（用于求第二枚一致读数，避免每次成功请求都探一次），两者都无订阅者也可运行一次。无腿平台的订阅只接收证据驱动的变更，不跑周期拍，`stale` 只经 `snapshot()` 派生可见、不发布 `StalenessMarked`——没有可采样的事实，伪造一次采样没有意义。每次登记先递增 epoch，旧闭包到期见 epoch 不符或 live 为假即 no-op（`post_after` 无撤销口，这是既有的 `StageTimer` 写法）。同一刻最多两枚存活登记（一枚已作废的旧拍 + 一枚待采样拍）；所有触发共用同一枚 pending 标记，已有待采样轮次时只合并、不另登记，采样后只登记下一枚周期拍。`post_after` 回 `net-platform-unavailable`（本腿无引擎语义，如 wasm）时清 pending；**证据在 `Engine::instance().running()` 为假时不登记**（不把停机引擎拉起来），只更新事实；`refresh()` 与 `set_options()` 是显式宿主动作，按引擎既有惰性启动口径办事。
- **投递**：每订阅者最多一条待投递变更，按「最后一次 `current` + 订阅者已见 `previous` + 最新 `cause`」合并；投递闭包只持订阅者共享状态，执行时才在锁内取最新待投递值，因此 `cancel_pending()` 能作废尚未执行的批次，同一订阅者也不会因合并而排队多条闭包。装了主线程投递器就回主线程，未装（无头 / 单测）就地同步执行；新订阅在登记后先处于「投递关闭」态，返回前一瞬才在锁内开启，故并发发布也不会抢跑——`subscribe()` 返回前不回调。注销后最多一次在途投递，`cancel_pending()` 可将其作废（不注销订阅）。订阅者回调抛异常不得穿破采样循环与任何在途交换——投递处逐一 try/catch，抛出者摘除（`is_registered()` 转假）、留一条 WARN 诊断（`AURORA_LOG_*`，不直触标准输出），其余订阅者不受影响；投递器本身抛出按同一路径处理。

### 5 与既有件的分工（不重叠、不互改）

| 既有件 | 本件与它的边界 |
|:---|:---|
| `RetryPolicy` | 它管一次交换的重放；本件不参与，也不改它的任何判定 |
| `NetPolicy` | 放行判定的唯一所在；本件**不是**闸（动机 4），故 `NetPolicy` 不读 `ConnectivityMonitor` |
| SSE 自动重连 | 它自己那圈重连环不动；本件只是可选读者，重连环可据链路态决定是否继续，但那是宿主的策略 |
| WebSocket 会话 | 升级后的会话仍不进请求活动表；本件不新增会话观测，也不替代 `WsCloseInfo` |
| 事件总线 / 活动存储 | 总线是面向宿主与工具的推模型出口、Release 裁切；本件是运行期事实源、不裁切。二者不共用通道（动机 11），本件也不向总线写任何东西 |
| `Resource` / `StreamResource` | 不给三态绑定形态（动机 13）；宿主在回调里写自己的 store |
| 交换钩子 | 钩子在发起侧、只改请求头；本件不参与发起，二者无交集 |
| `Capabilities` | **不加字段**。那枚结构写定「唯一真值源是 `Engine::capabilities()`」且表达的是引擎腿的传输形态；链路态不是引擎形态，硬塞会污染它的语义。本腿有没有采集源由 `ConnectivityFact::source` 自己说 |
| 单次错误归因 | `error_class_of` 仍回答「这次为什么没成」；本件回答「当时有没有链路」。失败只作采样触发与展示，绝不作链路负证 |

### 6 Inspector / MCP 暴露形态

`/api/network` 顶层追加一枚 `connectivity` 对象：`state` / `source` / `generation` / `stale` / `since_ms`（相对值，非墙上时钟）/ `last_failure`（冻结 slug 或 `null`）。**正常形状与 Release 早退形状都由同一构造器产出**：前者保留既有字段，后者保留 `available:false` 与 `reason`，两者都带 `connectivity`；既有字段、既有活动表条目、既有用例逐值不变；`?id=` 单条查询面不动。MCP **不新增工具**，`list_network_activity` 交出的仍是同一份 Inspector 文档。脱敏档位按动机 15：状态与源令牌为 `Public`，拓扑事实一律不导出，四出口断言集里追加一条「`connectivity` 段内不出现接口名 / 网关 / SSID / 链路本地地址」的源码文本与快照双检。

### 7 明确不做

不做发起侧闸门、不做链路级重连编排（§15 后半句原样保留）、不做第四档状态、不给 `Resource` / `Signal` 形态、不加 `Capabilities` 字段、不新增错误码（不可得复用 `net-platform-unavailable`）、不新增 feature 宏、不引 netlink / COM / 任何 vendored 依赖、不起新线程、不在探测里主动打一个包（**零出站**是本件的硬判据，见验收 5）、不把任何连接失败当链路负证、不用已弃用的 `SCNetworkReachability` 与受 build 19041+ 限制的 `GetNetworkConnectivityHint`。

### 8 交付批次

三批一次收，不分批发版。第一批：公共面 + 判据层 + 旁证插桩 + 注入钩子 + 纯逻辑单测与变异自证（平台腿尚未接入，源恒 `None` 或替身读数）。第二批：三枚平台采集腿（含 `socket.cpp` 内的 Windows 查询、CMake 的 `iphlpapi` 链接面）+ 逐平台真机探针 + 无源腿的降级判据。第三批：`/api/network` 两种形状的字段 + 脱敏断言 + 文档回写与全量门禁。

## 验收判据

1. 采集原语**先实测再进判据**：计划/待建的 tools/verify/net_connectivity_live_probe.cpp 逐平台跑通并落 `codespec/manual-test/23-net.md` 一条新用例（拔线/关 Wi-Fi/恢复三段各有可观察读数）。探针目标必须显式登记到 `cmake/AuroraVerify.cmake`（`aurora_add_verify_probe`；`CONFIGURE_DEPENDS` 只覆盖 `src/` 源列表，不覆盖 verify 目标）。Windows 走路由/接口表而非 `GetNetworkConnectivityHint`（后者要求 19041+ 且 MinGW 头无声明），macOS 走动态存储而非已弃用的 reachability；未经真机命中的腿一律不得进入公共判据，直接按「无源 → `Unknown`」落地——不在没有实测的地方写推断当事实。
2. `snapshot()` 在从未采样时的首快照回 `Unknown` + 编译期腿令牌（无腿平台为 `None`）+ `stale=true` + `generation=0`；`since` 取 monitor 建立时刻；这一枚读数不依赖引擎线程是否启动，不触发任何采样或 I/O。
3. 不对称阈值逐值断言：`offline_evidence` 枚合格的探测读数（缺省 1，明确读到无可用缺省路由）即转 `Offline`；`Unknown→Online` 一枚正证即可；`Offline→Online` 需 `online_evidence` 枚一致正证（缺省 2；连续两次采样在线、一次采样在线加一次成功建连、两次成功建连，三种组合各自成立且各有一例；「两次成功建连」这一组合在探测腿读不到的替身下跑，避免插队采样的第二票提前满足）。另有两例：回环对端成功建连不计正证也不采样；计票后已到 `Online`（含本次正证促成）不触发采样。计数规则本身被用例固定：反向证据清零对方计数、状态转换清零两侧、`stale` 翻转清零两侧、读失败不计数也不清零；该规则不得由实现自由解释。
4. `Timeout` 类失败**不产生任何状态转换**，只让待采样标记置真并写 `last_failure`；断言「一次 `Timeout` → 状态不变 + 恰好一次额外采样」；`Cancelled` / `Invalid` 既不采样也不写 `last_failure`；`last_failure` 的静默更新不推进 `generation`、不单独通知。
5. 零出站判据：以 `FakeEngine` / `ScriptedPeer` 双替身跑完整用例集，断言出站连接计数恒 0、注册口 `live_count()` 不因本件增加；探针 TU 不含 `connect` / `sendto` / `getaddrinfo` 调用（源码文本面断言），只读 OS 路由/接口表、伪文件或动态存储——「探测」必须是读事实，不是打一个包。
6. 零新线程判据：采样体只在引擎线程执行（`FakeEngine` 推进并断言线程标识；从外部线程投递旁证时，探针钩子记录的线程标识仍是引擎线程）；在引擎已启动的前提下，用例前后进程线程数不变——首次使用只允许既有引擎线程的惰性启动，不允许本件另起线程。
7. 插队合并判据：在引擎推进到期拍之前连投 50 次**失败旁证**，**有效探测调用恰为 1 次、证据触发登记恰为 1 次、存活登记 ≤ 2 枚**；订阅者收到的变更不超过实际状态变化次数（不因 50 次失败刷 50 条通知）。变异自证：摘掉 pending 标记后该断言必须转红。
8. 惰性启停判据：无订阅者、无交换、未调 `refresh()` / `set_options()` 时周期拍登记为 0；有采集腿时首枚订阅成功即安排一次插队采样（若已有待采样轮次则合并、不另登记），无腿平台只接受订阅、不登记定时器；末枚句柄析构后不再有下一次周期采样；失败拍可在无订阅者时跑一次，结束后不再登记；`Engine::shutdown()` 之后，在没有显式 `refresh()` / 新订阅的情况下不再登记任何定时器。
9. 通道报错不推进判据：探测腿返回「读不到」时状态维持、`generation` 不变、两侧证据计数都不 +1；在**先有一次成功采样使 `stale=false`** 之后，连续报错越过 3× 周期只允许 `stale` 位翻转，`cause=StalenessMarked` 恰好一次通知，且两侧计数被清零。
10. `Engine::shutdown()` 之后：`snapshot()` 仍可读、不再有转换、越过阈值后 `stale` 置真；再次 `refresh()` 若需要引擎则按既有惰性重启口径办事，不报新码；停机后到达的证据只更新事实、不重启引擎、不抛异常；`post_after` 回 `net-platform-unavailable` 时 pending 清零，下一条证据可重新尝试登记（不因一次登记失败永久卡死）。
11. 句柄三条契约复用注册口既有写法：析构注销幂等（双析构不双重摘除）、移动后源句柄不再触发注销、已入队尾批抵达一次且 `cancel_pending()` 与析构互不隐含；乱序尾批被 `generation` 丢弃的用法在示例注释里可见；首条 `previous` 等于订阅时刻的 `snapshot()`，合并待投递变更时 `previous` 仍是订阅者已见值；抛出型订阅者被摘除后 `is_registered()` 转假。
12. 抛出型订阅者判据：装一枚必抛的回调，断言（a）其余订阅者照常收到、（b）采样循环不中断、（c）一次在途交换的结局逐值不变、（d）抛出者被摘除且 WARN 恰好一条、后续不再调用它。
13. 无源腿判据分两类：wasm 无腿且无证据通路，`subscribe()` 与 `refresh()` 都回 `net-platform-unavailable`，**不得**回 OK 后静默永不通知；BSD / Android 一类无腿但有旁证，`subscribe()` 可成功、`refresh()` 回 `net-platform-unavailable`，状态可经非回环成功建连 `Unknown→Online`，但**永不可能转 `Offline`**。两类下 `snapshot()` 都恒可读（错误只发生在要求动作的口上）。
14. 宏行为不变判据：`AURORA_ENABLE_DEBUG=OFF` 与 `ON` 两侧，本件公共面、状态转换序列与通知次数逐值相同（与总线的裁切行为对照，证明二者不共用通道这条落到了实现）；公共头全文零 `#ifdef`。
15. 分层判据：新公共头不含 `Task` / 协程 / `state/` 依赖、不含平台头；`check_core_layer_boundary` 与 `check_arch_module_map` 绿；本件新增的平台宏只出现在采集腿一个 TU（`check_platform_macros` 绿）；`<winsock2.h>` 仍只在 `socket.cpp`，本件没有新开平台 socket 头落点。
16. `/api/network` 判据：无源时 `connectivity.state == "unknown"` 且 `source == "none"`；既有字段与全部既有用例逐值不变；**正常形状与 Release 早退形状都含 `connectivity`，且两处由同一构造器产出**（补一条构造器/构建形态的单测）；两枚 MCP 工具返回的文档里同一对象同源（不另开一面）；三枚令牌换算只经 `network_state_text()` / `connectivity_source_text()` / `connectivity_change_cause_text()`。
17. 脱敏四出口既有断言集全绿且不回退；新增「拓扑事实不导出」一条以源码文本面与导出快照双检命中；内部 `ProbeReading` 只含 `optional<bool>`，结构体无字符串与地址字段（静态断言 + 源码文本双检）。
18. 变异自证覆盖四处：阈值改对称（判通只需一枚）→ 期望转红；把 `Unknown` 当 `Offline` 消费 → 转红；把连接失败计为链路负证 → 「NXDOMAIN / ECONNREFUSED / 407 不得产生离线」用例转红；把旁证入口改挂到事件总线上 → Release 侧判据转红。每条都要以「摘掉即红」自证非空转。
19. 公共 API 门禁全套：`--target aurora_api_json` 刷新后 `check_api_schema_sync`、`check_naming_conventions`、`check_api_budget`（当前实测 48.3k / 75k 令牌，本件新增八个类型、约 23 枚成员与三枚自由函数，余量充足）、`check_doc_comments`、`lint`（改动文件）、`format-check` 全绿；`check_umbrella_header` 以 `--write-manifest` 收录新头后 R7/R8 绿。
20. 文档回写全套：`check_change_proposals`、`check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format` 绿；§16 追加裁决与 §15 改写见下条。
21. 口径冲突按裁决条目回写：伞形提案 §15 那句的**前半句**由 §16 追加的第 48 项改判（后半句「不做链路级重连编排」原样有效），§15 原句与旧编号一律保留在位并就地标注「前半句由 §16 第 48 项改判」，§16 标题的「四十七项」改「四十八项」，全仓不再出现未加限定的「不做网络状态监听」表述。
22. 选项判据：`sample_interval` 越界钳到 `[100ms, 1h]` 两端、两枚证据数越界钳到 `[1, 16]` 两端，各有一例；`set_options()` 不重置在途计数、不立即转换、安排一次 `HostRefresh` 采样（有腿时；可经引擎惰性启动拉起线程，登记失败只清 pending、不报错），无腿时只存值；`options()` 回钳后的新值。
23. 并发判据：一边从外部线程连投旁证（至少 1000 次）、一边在主线程循环 `snapshot()` / `subscribe()` / 注销 / `cancel_pending()`，无崩溃、无死锁、无双重注销，注销后最多一次在途投递；同批审计锁纪律——本件只有一把互斥量，持锁期间不调 `post_to_main`、不调探针、不调用户回调。仓内当前没有 TSan 预设，本件不承诺新增；若落码时工具链已支持，同批复跑该用例并附报告，未支持则如实记为人工审计项。

## 回写落点

1. `codespec/specification/06-app-platform.md`：新增 **§8.8.41**（§8.8.40 之后的下一枚空闲号）承载本件契约——类型逐符号契约、采集腿三段分工与三平台原语（含 Windows 查询落在 socket.cpp 的决定）、`Online` 只表示缺省链路存在、不对称阈值与其代价论证、`Unknown` 纪律与两类无源腿、旁证四行证据表与 `error_class_of` 复用、epoch 定时与 pending 合并、线程与投递纪律、明确不做清单。同批把 **§8.8.19 的收口段**（「交换钩子随 §8.8.40 收口，§12 与 §17 至此无剩余项」那句所在段落）追加本件口径：本件属新增能力，不改该句对既有伞形范围的判定。
2. `codespec/specification/06-app-platform.md` §8.8.39：追加 `connectivity` 对象的字段表、令牌来源（三枚换算自由函数唯一出口）、**正常形状与 Release 早退形状都由同一构造器产出**的口径、「既有字段逐值不变、MCP 不新增工具」。
3. `codespec/changes/xplat-network-stack/proposal.md`：§15 那句就地标注**前半句由 §16 第 48 项改判、后半句原样有效**；§16 标题改「四十八项」，末尾追加第 48 项裁决，写死六处定案（三平台采集原语与 Windows 查询落点、失败只触发不判决的四行证据表、链路态不是发起闸、不走事件总线的宏行为论证、三档而非四档且 `Online` 不等于互联网可达、`post_after` 无撤销口下的 epoch/pending 合并），**原句编号与文字一律保留**（防伪死链）。
4. `codespec/SPECIFICATIONS.md`：§5 特性表增设 `SPEC.FEAT.NET.CONNECTIVITY.001` 一行（判据：AI 能把一批失败归因到同一场链路故障，且库不因此拒发任何请求；规格落点指向 `codespec/specification/06-app-platform.md` §8.8.41），并确认 `SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001` 的跨平台一致读法覆盖本件的「无源即 `Unknown`」。
5. `codespec/ARCHITECTURE.md`：§4 模块映射的 net 行补新公共头一行与三个实现 TU 的分工；补一条不变量表述——同步探测腿不引入平台 socket 头、不引入新线程，Windows 的 `<winsock2.h>` 依赖落在既有 socket.cpp 内，平台 socket 头唯一落点地位不因链路态而放宽。
6. `codespec/CODING_STANDARDS.md`：§15 反例速查新增两条——「把链路态当发起闸」（闸一律在 `NetPolicy` / 能力闸那两处，链路态只作读者可查询的事实与退避提示，理由写足「库侧发起的失败对端不留痕」）与「把单次连接失败当链路负证」（`Dns` / `Unreachable` / `Proxy` 各自覆盖多类互不相干的原因，负证只能来自探测腿）。
7. `codespec/BUILD_OPTIONS.md`：§1.2 那句「事件引擎、事件源注册口与传输族没有开关」的覆盖面追加链路态监听，并写明它同时**不随 `AURORA_ENABLE_DEBUG` 裁切**（与本件不走事件总线那条裁决互为因果）。
8. `codespec/specification/02-state.md` §7.3：在 `Resource` / `StreamResource` 一节补一句边界——持续态事实不进三态绑定容器，宿主在订阅回调里写自己的 store。
9. `codespec/manual-test/23-net.md`：§1.1 被测模块表补新公共头一行，§2 用例清单加一条链路态真机用例（拔线 / 关适配器 / 恢复三段读数），§3 执行记录逐平台填写，未命中的平台如实记「无源 → `Unknown`」。
10. `codespec/errors.toml`：判定**无改动**——不可得与无源一律复用 `net-platform-unavailable`，故生成的错误目录亦不动；本件对该判定负责，若落码时出现需要专属码的形态，按规则 14 另批走并回到本文件补裁决。
11. 代码与数据面（不属 codespec 树，此处只记必做动作）：根目录 CHANGELOG.md 记新增公共面（一枚头 / 三枚举 / 三结构体 / 两类 / 三枚自由函数）与 `/api/network` 两种形状的追加字段；根目录 aurora_api.json 经 `--target aurora_api_json` 重建（enums 段 +3）；tools/include/known_enums.h 登记三枚新枚举并同步 tests/integration/itest_known_enums.cpp 锚点；tools/check/umbrella_manifest.txt 经 `--write-manifest` 收录新头，include/aurora/aurora.h 同步加入该直接包含；CMake 源列表由 `CONFIGURE_DEPENDS` 覆盖，但 Windows 顶层链接面须补 `iphlpapi`、`cmake/AuroraVerify.cmake` 须显式登记链路态真机探针（`aurora_add_verify_probe`）；新增 utest_net_connectivity（公共面与投递）、utest_net_connectivity_state（判据层逐值）与 Inspector 两种形状的构造器断言，按「一个源文件一枚 utest」的口径分文件。
