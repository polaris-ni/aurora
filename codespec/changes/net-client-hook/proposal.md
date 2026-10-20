# CHG-026 HTTP 交换钩子：客户端级登记链、请求头改写与短路交付、四腿落点次序与递归闸

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-026 |
| 提出日期 | 2026-10-20 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.FEAT.NET.HTTP-CLIENT.001 |
| 影响面 | 一枚新公共头（含两枚新公共类型与一枚新公共枚举）加 `Client` 上一枚登记方法；库内新增一份链内核与 `send` / `open_stream` / `open_sse` / `connect_websocket` 四条腿上的落点。**无破坏性签名变更、无新增错误码、`Request` / `Response` 零改动、不新造构建宏、不受 `AURORA_ENABLE_DEBUG` 裁切**；`Client` 多一枚私有共享内核成员（与 `cache_` / `pool_` / `cookie_jar_` 同格局，拷贝的 `Client` 共用同一条链）。`before_request` 的改写面**收窄为请求头**（方法、目标、体、协议旋钮、超时一律不动），这一收窄是对伞形提案 §13 原句「改请求」的裁决性收紧，同批回写 |

## 动机

1. **这是伞形提案里最后一件没有实现承担的条目。** `codespec/changes/xplat-network-stack/proposal.md` §13 写着「管道 hook：`Client::use(hook)` 提供『改请求 / 看响应 / 短路返回』三能力，供认证注入、日志与用例断言」，并在同一句里划出非目标——「刻意不照抄 OkHttp `Chain` 的全链可替换语义（那等于把协议正确性交给第三方），拦截点只做观察与最小改写」。实测 `include/aurora/net/http_client.h` 对 `use(` / `Interceptor` / `Hook` 全域零命中，即原句今日没有任何落点；§17 的落地序里其余切片均已收口，本件与可观测性两件并列剩余，而可观测性另有独立提案（`codespec/changes/net-observability/proposal.md` 的 影响面 行明写「`use(hook)` 不在本提案范围、留待后续提案收口」）。本提案就是那句「后续」。

2. **客户端级默认头今天无处可放，只能逐调用点手写。** 实测 `Client` 的全部旋钮是 `max_body_bytes` / `max_decompressed_bytes` / `tls_context` / `progress_frame_source` / `retry_policy` / `authenticator` / `request_id` / `proxy` / `policy` / `cache` / `pool` / `sse` 十二枚，**没有**任何一枚「对本客户端发出的每个请求补一组字段」的入口。现实形态是一枚 API key 或一个租户标识要挂在几十处调用点上：要么每处手写（漏一处即现场 401，且漏写与对端拒写在线上是同一条应答），要么宿主在业务代码外面包一层自己的构造函数（那层不在库里，`RetryPolicy` / `NetPolicy` / 缓存键全都看不见它改写出的字段）。这不是假想需求——本库的读者是 AI 生成的代码，「把同一份配置散写多处」正是本栈其它切片一路在消灭的那类形态（`Authenticator` 之于凭据、`ProxyEnvironment` 之于代理、`NetPolicy` 之于信道口径，都是把散写收成一处）。

3. **测试替身 today 只有引擎级与对端级两把武器，缺第三把「不造网络也不造时钟」的那把。** 实测仓内替身设施是 `FakeEngine`（管时间与脚本化收发）与 `ScriptedPeer`（管对端字节），二者都要求交换**真的走那条路**——只是路上的对端与引擎被换掉。而「这个函数在拿到 503 时会不会退避重试」这类断言，今天要么排 `FakeEngine` 的脚本、要么起一枚回环对端，两把武器都比「钩子里直接交出一枚 `Response`」贵，且都不易表达「这次交换根本不该发生」。短路交付把「不触网」从需要构造的对象降为一枚返回值，而它与 §13 原句里的「用例断言」正对得上。需要说清边界：**引擎级替身管的是传输事实**（半关闭、乱序、TLS 失败、超大帧），那类形态一枚钩子造不出来，本件不是它的替代。

4. **日志这一用途今天由可观测性切片承担，钩子与之不是重复设闸——两面管的事正好互补。** 那件落的是**只读推模型**：边界上记一次原始事实，进程级存储与事件总线把呈现后的条目推给订阅者，且按它自己的 影响面 行所述整体受 `AURORA_ENABLE_DEBUG` 裁切。钩子落的是**在链上的拉模型**：同步跑在发起侧的交换路径上，能改写、能决定这次不触网、不受宏裁切（生产遥测与静态凭据注入是它的本职）。因此「一次界面异常反查一次请求」用那件、 「这次交换带这组字段出去 / 这次交换不要真的发出去」用本件。这条分工必须写进规格并同批写进头注释，否则后来者会把其中一面当冗余摘掉——它与 §16 第 31 项那处「起手闸与逐跳闸不是重复设闸」、第 33 项那两处「相反形态的 206 双闸」是同一类记账。

5. **拦截点的形态不能按口号定，要按库里已有的四个真值源定。** 实测 `send` 的起手次序（HEAD 逐行读过）是：取消判定 → `RetryPolicy` 形态校验 → 复制 `origin` → `stamp_request_id` → `log_exchange_identity` → `attach_cookies_from_jar` → `effective_policy` + `gate_origin` → fetch 腿体形态判定 → `open_from_cache` → 重试环（内含认证环与逐跳 `plan_route`）。这串次序里已经写死了三条已裁决的不变量：策略闸先于缓存（§16 第 31 项）、标识一次交换一枚且落在 `origin` 副本上（第 32 项）、请求体的线上形态只有 `plan_request_body()` 一枚决策点（第 33 项）。钩子若插在任两条之间，就会有一处不变量得改口径——**本提案的落点是按这三条挑出来的，不是按「拦截器该在开头」挑出来的**，逐处理由见 §变更内容 第 4 项。

6. **不做全链替换不等于可以不说清「为什么不做」。** OkHttp 的 `Interceptor.Chain` 把「谁接下一环」交给第三方，代价是拦截器可以决定不 `proceed()`、可以换掉 `Request` 的目标、可以改写应答——即协议正确性的最终裁决权在宿主手里。本库的立场（§13 原句 + §15 非目标「不做全量 socket 选项封装与拦截器全链语义」）是裁决权留在库：钩子拿不到 `proceed()`、拿不到连接与 TLS 事实、拿不到改写应答的能力（`after_response` 的入参是 `const Response &`，返回 `void`）。而为了让「不触网」这一维仍可表达，钩子拿得到一枚**只此一次**的短路资格：`before_request` 的返回值携 `Result<Response>`，即一次交换的结局可以完全由钩子定，但**交换一旦进入环里，钩子就再无话语权**。这条取舍同时给出了 `Response` 可公开构造这一既有事实的用武之地（`include/aurora/net/http.h` 里 `Response(std::uint16_t, std::string, Headers, std::vector<char>, bool)` 在 public 段，本件无需为替身另造一枚应答构建器）。

7. **递归是本件唯一的隐性故障面，必须当场定形而不是留给注释。** 典型现场：宿主挂一枚遥测钩子，钩子里用**同一枚 `Client`** 把日志 POST 到采集端。链上的第二枚交换又会问这枚钩子，于是每次交换在运行时多出一条交换、那条交换又多出一条——无界递归，结局是栈溢出而非一条错误。本库缺省的完成投递在主线程，这条链一旦跑起来整个界面一起没。§16 第 34 项对 `Authenticator` 没有这个问题（换取 token 用的是宿主另一枚客户端，且 `asked` 闸把一次交换的问答封在「至多一次」里），钩子没有等价物可封，故本件自带一枚精确对症的闸，形态与取舍见 §变更内容 第 7 项。

## 变更内容

### 1 公共面：一枚抽象类、一枚 RAII 句柄、一枚枚举、一枚登记方法

新增一枚公共头（计划/待建的 include/aurora/net/http_hook.h，命名空间 `aurora::net::http`，与 include/aurora/net/http_auth.h 同层同格局），伞头 `[direct]` 收录、tools/check/umbrella_manifest.txt 随批刷新：

```cpp
/// @brief 一次交换发起于哪条腿——钩子据此判断自己的短路资格与字段口径是否适用。
enum class ExchangeLeg : std::uint8_t { Send, Stream, ServerSentEvents, WebSocket };

/// @brief 挂在 Client 上的交换钩子：同步、只改请求头、可在 send 上短路整次交换。
class ExchangeHook {
  public:
    virtual ~ExchangeHook() = default;
    /// 起手问：就地改写 request 的头部；交 nullopt = 照常发起，交有值 = 本次交换以此收摊。
    /// @note 仅 ExchangeLeg::Send 承认短路值；其余三腿交出短路值即整次落 http-request-invalid。
    [[nodiscard]] virtual auto before_request(Request &request, ExchangeLeg leg) const
        -> std::optional<Result<Response>> = 0;
    /// 收摊问：只在成功交出一枚应答时问（线上应答 / 新鲜缓存副本 / 本钩子链的短路成功）。
    virtual auto after_response(const Response &response) const -> void;
};

/// @brief 登记句柄：move-only RAII，析构即从链上摘除本次登记（三条契约同 EventSourceRegistration）。
class ExchangeHookRegistration {
  public:
    ExchangeHookRegistration() = default;
    ~ExchangeHookRegistration();
    ExchangeHookRegistration(const ExchangeHookRegistration &) = delete;
    auto operator=(const ExchangeHookRegistration &) -> ExchangeHookRegistration & = delete;
    ExchangeHookRegistration(ExchangeHookRegistration &&other) noexcept;
    auto operator=(ExchangeHookRegistration &&other) noexcept -> ExchangeHookRegistration &;
    auto clear() -> void;  ///< 提前摘除；重复调用与析构并存不双重摘除
};

class Client {
  public:
    /// @brief 登记一枚钩子并返回其句柄；链按登记次序跑，拷贝的 Client 共用同一条链。
    /// @note const：注册进入的是共享内核而不是本对象，签名做成 non-const 会暗示「这条链属于这份 Client」的假语义。
    auto use(std::shared_ptr<ExchangeHook> hook) const -> ExchangeHookRegistration;
};
```

三条取舍记在案上。**其一，不引入 `std::function` 旋钮而用抽象类**：两枚 `std::function` 装不下「短路时交出什么」这一维（要么牺牲错误分支、要么再叠一枚可选返回值，反而更长），而仓内 `.clang-tidy` 已记录「任何转入 `std::function` 的可调用对象一律判为不应抛出」这条系统性假告警面——本件正好有一条「钩子不得抛」的契约要写，不去招那枚假阳。同批的先例是 `Authenticator` / `BodySource` / `DataSink` 全是抽象类。**其二，`after_response` 给默认实现**（空体），使「只加头的钩子」不必写第二问；`before_request` 纯虚，因为它必须表态一次。**其三，链上没有查询口**（无 `hooks()` 计数、无按 id 摘除）：句柄即真值，多一枚查询口就多一处「它与句柄不一致时以谁为准」。

新公共枚数：一枚抽象类 + 一枚 RAII 句柄 + 一枚枚举 + 一枚方法。`ExchangeLeg` 登记进 tools/include/known_enums.h 并同步 tests/integration/itest_known_enums.cpp 的编译期锚点。

### 2 改写边界收窄为请求头，且不新增任何改子

`before_request` 拿到的 `Request &` 只用得上既有公共改子：`header` / `set_header`（头）、`request_id` / `body` / `range` / `if_range` / `expect_continue` / `timeout`（其余维）。本件的契约是**只允许动头部**，方法、目标 URL、请求体、三枚协议旋钮与超时一律不改；理由三条，且每条都以已裁决的不变量为据：

- **目标本来就没有改子。** 实测 `Request` 的公共面里 `url()` 只是只读访问器（`const std::optional<Url> &`），没有 `url(...)` 设值口，`method()` 同理。要放开「钩子改目标」就得先新增一枚公共改子，而那是另一件事的形状。
- **不改目标换来一条硬收益：策略闸与路由不需要第二次判定。** 三条已定案口径因此原样成立——「本跳有没有经代理只有一个答案」（§16 第 30 项的 `plan_route` 单入口）、策略三维读的是目标与 `TlsContext`（第 31 项）、钩子不可能把 `https://` 改成 `http://` 从而绕过明文白名单。若允许改目标，`gate_origin` 与 `plan_route` 各要重跑一遍，库里当场长出第二个路由答案，时间线与活动表（可观测性切片）也须跟着改口径。
- **不改体与协议旋钮是为了不拆 `plan_request_body()` 那一枚唯一决策点**（第 33 项）。钩子在决策之后改体即「声明 `Content-Length: 5` 而发出分块」那一类错位；`range` / `if_range` / `expect_continue` 是 `render_head()` 现地补进线上的字段（第 33 项第 ⑦ 条：它们不进 `headers()` 字段表），钩子若经 `headers()` 手写一枚裸 `Range`，前一道缓存闸失效——本件不给这条路留任何侥幸，头注释直接写「钩子只改头，且改的是要上线的字段」。

`X-Request-Id` 是唯一需要就地说明的头部：钩子覆写它**允许**，且语义等价于「调用方手写同名头」。这不破坏第 32 项，因为 `stamp_request_id` 跑在钩子之前——钩子看到的已经是落好的标识，覆写就是一次显式覆写；此后缓存键、`log_exchange_identity` 那行、逐跳复制出的 `origin` 副本三者同读一份 `origin.headers()`，不存在第二处真值。判据把「三份同源」钉成一条可执行断言（见 §验收判据 第 10 项）。

### 3 钩子的两问在 `send` 上的落点（次序是本件的主要交付）

按 §动机 第 5 项实测出的起手串，两处插入点定死如下，逐条附理由：

| 既有环节 | 与本件的关系 |
|:---|:---|
| 取消判定（令牌到手即已取消） | **恒先于钩子**。取消那侧一次钩子都不问，与「策略闸、缓存表、连接一概不问」是同一句承诺，判据可断 |
| `RetryPolicy` 形态校验 | 先于钩子：形态非法的交换不配被改写 |
| 复制 `origin` → `stamp_request_id` → `log_exchange_identity` | 先于钩子，故钩子看到的是带标识、与日志行同源的请求 |
| `attach_cookies_from_jar` | 先于钩子。理由：观察的定义是看**要上线的那份**；钩子覆写 `Cookie` 属宿主的显式动作，与「调用方自写的 `Cookie` 优先，Jar 不覆盖」同一条优先级（钩子在调用方之后表态） |
| **`before_request` 链** | 在此跑完，并按登记次序传递改写 |
| `effective_policy` + `gate_origin` | **后于钩子**，判的是改写后的那一份。收益具体：第 34 项「明文目标带 `Authorization` / `Cookie` 起手硬拒」那一条读的就是头部，钩子在闸前跑正是让这条纪律覆盖钩子写出的字节——否则「钩子注入凭据」是白名单外的一条绕行 |
| fetch 腿体形态判定 | 后于钩子（本件不改体，两处无冲突） |
| 短路资格判定 | 排在闸门**之后**：被策略拒的那一次，钩子交出的短路值一律丢弃并如实落 `net-policy-violated`。策略先于「少打一次网络」这条优化，与「起手闸先于缓存」是同一条次序纪律的第 N 个消费者 |
| `open_from_cache` | 短路成立即不查表；未短路时照常查表，命中新鲜副本仍问 `after_response` |
| 重试环 / 认证环 / 逐跳 `plan_route` | **钩子一律不再问**。一次交换 `before_request` 恰好一次、`after_response` 至多一次；逐跳与逐尝试的细节属可观测性切片的时间线，不在本件重复设闸（第 32 项「逐跳重算的是决策，不是身份」的同一条分界） |
| 收摊（重定向链之后、体物化与解压完成之后） | 在此问 `after_response`，入参是最终那一条应答的明文体形态 |
| 失败收摊（任何 `http-*` / `net-*` 错误） | **不问** `after_response`。那枚入参的类型就是 `Response`，库里不为钩子造一枚「伪应答」去承载失败；要观测失败读返回值与错误码 |

「一次交换至多一对钩子」是本件的核心不变量，`send` 与三条流式腿都按它收口。

### 4 四条腿的覆盖面与短路在流式腿上的诚实失败

- `send`：`before_request` + `after_response` 成对，短路生效（成功与携错两种都生效——携错即「模拟一次失败」那条断言需求的落点，无需新码）。
- `open_stream` / `connect_websocket`：只问 `before_request`（`ExchangeLeg::Stream` / `WebSocket`），WebSocket 问的是握手那一跳（升级后的会话不属一次 HTTP 交换，与可观测性切片把会话排除出请求活动表是同一条分界）。
- `open_sse`：只问 `before_request`（`ExchangeLeg::ServerSentEvents`），且**每一次重连各问一次**——§16 第 38 项已定「每条订阅环在一枚帧里 `co_await` 一次 `open_stream`」，一次重连本身就是一次有始有终的调用，逐连重问正是那条裁决在本件上的推论（也满足第 30 项的逐跳重算精神：改写要跟上换了主机的重连）。
- 非 `Send` 腿上钩子交出短路值：**如实落 `http-request-invalid`，detail 点名是哪枚钩子、哪条腿、以及「短路只在 `send` 生效」**，绝不静默忽略。理由是静默丢弃等于让宿主的替身「以为自己在 mock，实际打到了真网」——那正是本栈要消灭的那类无痕故障；写错误码而非忽略的代价（一枚钩子同时服务 `send` 与流式腿时会被打断）由 `ExchangeLeg` 入参抵掉：钩子问得到自己在哪条腿上，因此这条错误是可预防的，而「预防所需的知情」正是那枚枚举存在的全部理由。
- 三条流式腿的收摊值分别是 `StreamOutcome` / `SseOutcome` / `WebSocket` 而非 `Response`。为钩子另造一枚「头部已定、体未读完」的应答快照，等于在库里长出第二份应答真值（`Response` 与 `StreamOutcome` 谁说的是最终事实将无从陈述），故本件不做，随裁决写进规格并登记为待办形态。

### 5 链内核、快照与并发口径

链住在一枚新增的库内共享内核（计划/待建的 src/aurora/net/detail/http_hook_chain.h 与 .cpp），`Client` 以 `std::shared_ptr` 持有，与 `cache_` / `pool_` / `cookie_jar_` / `authenticator_` 完全同格局：拷贝的 `Client` 共用同一条链，登记与注销在任一份拷贝上做都是给整条链做。公共头只做前向声明（照 `detail::HttpResponseCache` 的既有形态），`Client` 因此多一枚私有成员、不改任何既有签名。

- **次序**：按登记次序跑（第一枚最先看到未改写的请求，也最先获得短路资格；后一枚看到前一枚改写后的那份）。不提供优先级字段——「登记次序即执行次序」是可陈述、可断言的唯一口径，加一枚优先级旋钮就多一处「同优先级时以谁为准」。
- **快照**：一次交换在起手取一份链快照（拷出 `shared_ptr` 数组），此后 `use` 与句柄析构都不影响在途交换。与 §16 第 31 项「生效时刻是发起那一刻」同律，也让「钩子在回调里注销自己」这种形态有确定结局（本次交换照跑完，下一次不跑）。
- **并发**：内核一枚互斥量护住链本体；登记 / 注销可在任意线程调用，快照在协程帧所在线程取。`@note Thread` 逐处写明（登记口：任意线程；两问：**同步跑在发起侧续体所在线程**，缺省即主线程投递器那条，不得阻塞、不得重入 UI）。不沿用「单线程 UI」的笼统说法。
- **快路径**：链空时只读一枚原子计数即返回，不加锁、不分配、不构造临时量。判据断「空链时线上字节与从未挂载逐字节相同」。
- **重复登记不去重**：同一枚 `shared_ptr` 登记两次即在链上跑两遍。去重需要比较实现方身份，而钩子实现可以持可变态（同一实例挂两处是常态写法），库无从陈述「这两次是同一件事」。句柄仍各自管各自的摘除。

### 6 与既有注入点的分工（一张表写进头注释与规格）

| 要表达的事 | 落点 | 本件为什么不是它的替代 |
|:---|:---|:---|
| 每请求补同一组静态字段（API key、租户头） | 本件的 `before_request` | — |
| 挑战-答复型凭据（401 / 407） | `Authenticator` | 协程、可再去一次网络换 token、一次交换至多问一次、不吃 `attempts` 名额，钩子这三条都不该有 |
| 能不能谈（明文白名单、TLS 下限、必须经代理） | `NetPolicy` + `gate_origin` | 它在钩子之后判，且钩子改不动它的三维入参（目标与 `TlsContext`） |
| 逐跳耗时、阶段、活动条目、失败归因 | 可观测性切片（时间线 / 活动表 / 事件总线） | 只读推模型、受 `AURORA_ENABLE_DEBUG` 裁切；钩子在链上、可改写、生产可用 |
| 退避与重试 | `RetryPolicy` | 钩子不参与逐尝试，重试环的次数不由钩子决定 |
| 上传/下载进度 | `Request::on_progress` + 合流闸 | 逐段回调不属交换路径的起点与终点 |
| 传输事实的替身（半关闭、乱序、TLS 失败） | `FakeEngine` / `ScriptedPeer` | 钩子造不出线上字节 |

**同名冲突定案**：钩子与认证环都写 `Authorization`（或 `Proxy-Authorization`）时，**以认证环那一份上线**。挑战-答复的语义就是「上一份不被接受」，钩子不该有推翻对端裁决的特权；`place_auth_decision` 的 `set_header` 覆盖行为因此是正确形态，本件不改它，只把头注释与规格写成「两处都写时以问答结果为准」。这条也解释了为什么钩子不是认证注入的替代口：它的注入会被一次 401 正当覆盖。

### 7 递归闸：线程局部「本枚在栈上」即跳过该枚并记一行 WARN

一枚钩子在自身的 `before_request` 尚未返回时，经由同一枚 `Client` 又发起一次交换，导致链上会再问这枚自己——这是遥测钩子的典型形态，结局是无界递归与栈溢出。定案：内核以一枚**线程局部**的「正在栈上的钩子」集合护住每一次回调；某枚钩子被问时若自己已在栈上，则**跳过该枚、链上其余各枚照常跑**，并经 `AURORA_LOG_WARN("net", ...)` 记一行（只带可公开事实：嵌套这一维与本次交换的标识，**不带** URL、头部名与任何头值，AGENTS.md 规则 8 与脱敏纪律同源）。被跳过的那一次交换照常完成、照常交付、照常记活动条目。

三条取舍写在此：**为什么不是「嵌套照常跑链」**——那把宿主的一次写错变成 `0xC0000005`，而本库缺省的完成投递在主线程，一次递归带走整个界面；**为什么不是「嵌套落一条错误」**——遥测钩子不该有能力让业务请求失败，把观测面写成故障源是反向的优先级；**为什么不是「整条链跳过」而是「只跳在栈上那一枚」**——链上其余各枚与被这枚钩子发起的那条日志请求毫无关系，整链哑掉会顺带吃掉别人的静态头注入，那才是真的无痕故障。闸管的是崩溃边界而非安全边界，故 WARN 而不是错误。判据：一枚自反馈钩子对同一 `Client` 再 `send`，断言链上只跑一层、两条交换都成功交付、WARN 恰一行（经 `Logger` 的 sink 重定向捕获）。

### 8 无门控、无新码、无宏、无破坏性签名

钩子不受 `AURORA_ENABLE_DEBUG` 裁切，也不新造构建宏（静态凭据注入与生产遥测是它的本职，那类用途与「调试构建」无关）。`Client` 的公共面因此多一枚 `use`，`Request` / `Response` 零改动，四条腿签名零改动。**无新增错误码**：短路在流式腿上的落点用 `http-request-invalid`（§16 第 34 项已定它是「调用点写错」这一族出口的落码处，与本件同一形态），递归闸不产错误。codespec/errors.toml 与 codespec/ERROR_CATALOG.md 因此不动，本件对该判定负举证责任（见 §回写落点 第 7 项）。

### 9 交付批次

一批可完成：公共面（一枚头 + 三枚类型 + 一枚方法）→ 库内链内核与四腿落点 → 单测与变异自证 → codespec 回写与数据面刷新 → 全量门禁。无跨批次红灯，无依赖可观测性切片（两者只在「短路那一跳要不要进活动表」这一处相遇：本件复用已在位的 `record_local_outcome` 出口形态，若观测切片先落地则短路那一跳按它的 `record_cache_hit` 先例记一条**零 mark** 条目并点名未触网；两处协调点在同批实现说明里记明，不各写一套）。

## 验收判据

1. **一次调用一对钩子**：`send` 的三种交付来源各一例——走网络拿到 200、命中新鲜缓存（`after_response` 的入参 `from_cache == true`）、钩子短路（`from_cache == false` 且零连接发起）——三例都断言 `before_request` 恰一次、`after_response` 恰一次。
2. **闸门判的是改写后的那份**：钩子往 `http://` 目标注入 `Authorization` 起手落 `net-policy-violated`，`sent_bytes == 0`；摘掉这条注入即交换成功。
3. **策略先于短路**：同一枚钩子在 `http://` 目标上既注入凭据又交短路值，结局必须是策略拒而非短路交付（丢弃短路值这条次序的唯一可执行形态）。
4. **取消那侧不问钩子**：起手即已取消的令牌，两问各零次（与「策略闸、缓存表、连接一概不问」同句承诺）。
5. **链上次序与改写传递**：三枚钩子依次登记，断言第二枚读到第一枚写的头、第三枚读前两枚，且链上跑出的最终线上头集与逐个改写的并集逐值相同；把次序改成逆序的变异必须转红。
6. **快照先于中途注销**：`before_request` 里注销本次链上另一枚钩子的句柄，断言本次交换仍把那枚问完、下一次不问；这条同时是「析构与在途交换并发不产生半截链」的自证。
7. **句柄三条契约**：`clear()` 后再析构不双重摘除、句柄移动后原对象析构不摘除、链上其余各枚不受影响（照 `EventSourceRegistration` 的既有判据形态逐条写）。
8. **流式腿只问前钩**：三条腿各一例断言 `after_response` 零次、`before_request` 恰一次且 `leg` 逐字相符；`open_sse` 另加一例断「每次重连各问一次」（初连 + 一次重连 = 两问）。
9. **非 `Send` 腿的短路如实失败**：在 `open_stream` 上交短路值落 `http-request-invalid`，detail 同时点名腿与短路只在 `send` 生效；同例断一条连接都不发起。
10. **标识三份同源**：钩子覆写 `X-Request-Id` 后，断言日志行、缓存键、`after_response` 入参三者读到的是同一串，且第二次同名请求仍命中缓存（对端 accept 恒 1）。
11. **凭据进缓存键**：钩子注入 `Authorization` 取回的副本存进表后，一枚不带凭据的裸打请求**读不到**它（与 §16 第 34 项那条同律，防「钩子注入的凭据换回的私密内容被下一位裸打读到」）。
12. **同名冲突以认证环为准**：钩子先注入 `Authorization: Bearer wrong`，对端回 401，`Authenticator` 给出 `Bearer right`，断言上线的是 right 那份且最终应答成功。
13. **短路不进缓存表**：短路交付后第二次同名请求仍照常触网（`open_from_cache` 未被写入任何条目），防「替身的产物被缓存成线上事实」。
14. **递归闸**：§变更内容 第 7 项那条用例——链上只跑一层、两条交换均成功、WARN 恰一行；把闸摘掉的变异必须以可见的深递归转红（而非静默通过）。
15. **空链恒等**：未登记任何钩子时，既有 `utest_net_http_client` / `utest_net_http_stream` 全量用例逐值不变，且请求字节与挂载钩子但恒答 `nullopt` 的字节逐字节相同。
16. **改写边界**：钩子改完 `request` 后 `url()` 与 `method()` 与挂载前逐字段相同，且渲染出的请求行与目标一致；`Response` 侧断言 `after_response` 拿不到任何改写口（编译期判据：形参为 `const Response &`、返回 `void`）。
17. **门禁全绿**：`check_api_schema_sync`（先 `--target aurora_api_json`）、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`check_umbrella_header`（新头入 `[direct]`）、`check_core_layer_boundary`、`check_arch_module_map`、`check_platform_macros`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_no_cjk_literals`、`check_no_hardcoded_paths`、`check_manual_test_format`、`check_version_consistency`、`format-check`；改动文件逐个过 `lint`；`ExchangeLeg` 入 known_enums.h 后 `itest_known_enums` 锚点随批。
18. **判据非空转**：至少六处变异自证并逐条记入规格小节末段——把钩子挪到 `gate_origin` 之后（第 2、3 项各一转红）；把短路判定挪到闸门之前（第 3 项）；把 `after_response` 也问在失败侧（第 1 项）；把流式腿的短路静默忽略（第 9 项）；摘掉递归闸（第 14 项）；把快照改成每次现读链（第 6 项）。

## 回写落点

1. `codespec/specification/06-app-platform.md`：在 §8.8.35 之后新增一节承载本件契约（编号取同批实际落地时的下一枚空闲号——并行的可观测性切片占用的编号以它先落地为准，两处避撞），内容为公共面逐符号契约、四腿落点次序表、改写边界与三条理由、同名冲突裁决、递归闸的三条取舍、以及 §验收判据 第 18 项的变异自证末段。
2. `codespec/SPECIFICATIONS.md`：§5 表 `SPEC.FEAT.NET.HTTP-CLIENT.001` 行的能力清单补「交换钩子（客户端级登记链：改请求头 / 看应答 / `send` 上短路交付）」。
3. `codespec/changes/xplat-network-stack/proposal.md`：§13 那条「管道 hook」原句就地补一条 §16 追加裁决，写死五处定形（级数与句柄、同步抽象类、改写边界收窄为请求头、腿面覆盖与流式腿短路落错、递归闸），并记明「改请求」在本件收窄为「改请求头」这一收紧与其三条理由；**原句编号与文字一律保留**（防伪死链）。同批把 §17 落地序与现状段里「仍未落地的两件」收口其一。
4. `codespec/CODING_STANDARDS.md`：§4 的日志/回调纪律补一条钩子契约（两问同步跑在发起侧续体所在线程、不得阻塞、不得抛、不得改可观测面之外的字段），§15 反例速查补一条形态「钩子里做无界阻塞 I/O 或用同一枚 Client 自反馈上报」并给正解（换一枚不挂钩子的客户端、或用库内既有异步出口）。
5. `codespec/ARCHITECTURE.md`：§4 模块映射的 net 层随批补新头与库内链内核一行；§14 若涉及测试替身口径（`FakeEngine` / `ScriptedPeer` 那两处），补一句第三把武器与其边界（引擎级替身管传输事实，本件管是否触网与头部注入）。
6. `codespec/BUILD_OPTIONS.md`：判定**本件不改该文档**并在回写说明里记理由——无新宏、无新缓存变量、无按宏裁切的公共成员（`Client` 尺寸不随构建面变化，与 `AURORA_ENABLE_DEBUG` 那条布局例外无关）。
7. `codespec/errors.toml`：判定**无改动**（复用 `http-request-invalid`），故生成的 `codespec/ERROR_CATALOG.md` 亦不动；本件对该判定负责——若落码时发现流式腿短路需要一枚专属码，按规则 14 另批走并回到本文件补裁决。
8. `codespec/manual-test/23-net.md`：判定**不落手工用例**并登记理由——本件四腿落点与链上次序在零出站的回环夹具（`ScriptedPeer` / 假对端 / 纯内存 `Response`）上全覆盖，没有「必须出站」或「必须有宿主帧循环」的现场；若可观测性切片的 `TC-NET-0xx` 覆盖到钩子交付的 Inspector 面，由那件记。
9. 代码与数据面（不属 codespec 树，此处只记必做动作）：根目录 CHANGELOG.md 记新增公共面（一枚头 / 三枚类型 / 一枚方法 / 一枚枚举）与 `Client` 多一枚私有共享内核、迁移面为零（纯新增）；根目录 aurora_api.json 经 `--target aurora_api_json` 重建；tools/include/known_enums.h 登记 `ExchangeLeg` 并同步 tests/integration/itest_known_enums.cpp 锚点；tools/check/umbrella_manifest.txt 经 `--write-manifest` 刷新。
