# CHG-026 HTTP 交换钩子：请求视图与头改写通道、双闸次序、四腿落点、触点差异与短路交付

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-026 |
| 提出日期 | 2026-10-20 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.NET.HTTP-CLIENT.001 |
| 影响面 | 一枚新公共头（四枚新公共类型：`ExchangeLeg` / `ExchangeRequestView` / `ExchangeHook` / `ExchangeHookRegistration`）加 `Client` 上一枚 `use` 方法；库内新增一份链内核、一只触点差异比较器与一枚短路交付记账口，并在 `send` / `open_stream` / `open_sse` / `connect_websocket` 四条腿上落点。**无破坏性签名变更、无新增错误码、`Request` / `Response` 零改动、不新造构建宏、不受 `AURORA_ENABLE_DEBUG` 裁切**；`Client` 多一枚私有共享内核成员（与 `cache_` / `pool_` / `cookie_jar_` 同格局，拷贝的 `Client` 共用同一条链）。`before_request` 的改写面**收窄为请求头**（方法、目标、体、协议旋钮、超时一律不可达），这一收窄是对伞形提案 §13 原句「改请求」的裁决性收紧，同批回写。**短路交付是终点：不进缓存、不更新 Cookie Jar、不跟随重定向、不进重试环。** |

## 动机

1. **这是伞形提案里最后一件没有实现承担的条目。** `codespec/changes/xplat-network-stack/proposal.md` §13 写着「管道 hook：`Client::use(hook)` 提供『改请求 / 看响应 / 短路返回』三能力，供认证注入、日志与用例断言」，并在同一句里划出非目标——「刻意不照抄 OkHttp `Chain` 的全链可替换语义（那等于把协议正确性交给第三方），拦截点只做观察与最小改写」。实测 `include/aurora/net/http_client.h` 对 `use(` / `Interceptor` / `Hook` 全域零命中，即原句今日没有任何落点；可观测性已随 §8.8.36—§8.8.39 收口，§17 的落地序里只剩本件。本提案就是那句「后续」。

2. **客户端级默认头今天无处可放，只能逐调用点手写。** 实测 `Client` 的全部旋钮（`max_body_bytes` / `max_decompressed_bytes` / `tls_context` / `progress_frame_source` / `retry_policy` / `authenticator` / `request_id` / `proxy` / `policy` / `cache` / `pool` / `sse`）里**没有**任何一枚「对本客户端发出的每个请求补一组字段」的入口。现实形态是一枚 API key 或一个租户标识要挂在几十处调用点上：要么每处手写（漏一处即现场 401，且漏写与对端拒写在线上是同一条应答），要么宿主在业务代码外面包一层自己的构造函数（那层不在库里，`RetryPolicy` / `NetPolicy` / 缓存键全都看不见它改写出的字段）。这不是假想需求——本库的读者是 AI 生成的代码，「把同一份配置散写多处」正是本栈其它切片一路在消灭的那类形态（`Authenticator` 之于凭据、`ProxyEnvironment` 之于代理、`NetPolicy` 之于信道口径，都是把散写收成一处）。

3. **测试替身 today 只有引擎级与对端级两把武器，缺第三把「不造网络也不造时钟」的那把。** 实测仓内替身设施是 `FakeEngine`（管时间与脚本化收发）与 `ScriptedPeer`（管对端字节），二者都要求交换**真的走那条路**——只是路上的对端与引擎被换掉。而「这个函数在拿到 503 时会不会退避重试」这类断言，今天要么排 `FakeEngine` 的脚本、要么起一枚回环对端，两把武器都比「钩子里直接交出一枚 `Response`」贵，且都不易表达「这次交换根本不该发生」。短路交付把「不触网」从需要构造的对象降为一枚返回值，而它与 §13 原句里的「用例断言」正对得上。需要说清边界：**引擎级替身管的是传输事实**（半关闭、乱序、TLS 失败、超大帧），那类形态一枚钩子造不出来，本件不是它的替代。

4. **日志这一用途今天由可观测性切片承担，钩子与之不是重复设闸——两面管的事正好互补。** 那件落的是**只读推模型**：边界上记一次原始事实，进程级存储与事件总线把呈现后的条目推给订阅者，且按它自己的影响面行所述整体受 `AURORA_ENABLE_DEBUG` 裁切。钩子落的是**在链上的拉模型**：同步跑在发起侧的交换路径上，能改写、能决定这次不触网、不受宏裁切（生产遥测与静态凭据注入是它的本职）。因此「一次界面异常反查一次请求」用那件、「这次交换带这组字段出去 / 这次交换不要真的发出去」用本件。这条分工必须写进规格并同批写进头注释，否则后来者会把其中一面当冗余摘掉——它与 §16 第 31 项那处「起手闸与逐跳闸不是重复设闸」、第 33 项那两处「相反形态的 206 双闸」是同一类记账。

5. **拦截点的形态不能按口号定，要按库里已有的真值源定；初稿的「钩子在闸前」是建立在一句未实测的假设上，本稿把它纠正。** 实测 `send` 的起手次序是：取消判定 → `RetryPolicy` 形态校验 → 复制 `origin` → `stamp_request_id` → `log_exchange_identity` → `attach_cookies_from_jar` → `effective_policy` + `gate_origin` → fetch 腿体形态判定 → `open_from_cache` → 重试环（内含认证环与逐跳 `plan_route`）。初稿把钩子放在 `gate_origin` **之前**，理由是「调度闸读的是改写后的头部，钩子注入凭据能被明文白名单拦住」。逐行核对 `gate_origin` / `plan_route` / `NetPolicy::evaluate` 后，这个理由不成立：**它只读目标、端口、代理决策、`TlsContext` 与明文主机白名单，完全不读头部**。钩子放在闸前既换不到任何策略覆盖，又让一个注定被拒的目标白跑一遍钩子。故本稿改为**双闸**：闸 #1 在钩子之前（目标不合规矩则链上一次都不跑），钩子跑完后、上线前再过一次闸 #2（结构不变量与未来扩展的防线）。钩子拿不到目标、方法、体与 `TlsContext` 的改子，因此它**不可能**把 `https://` 改成 `http://` 或改出一个闸 #1 没见过的目标；「钩子注入凭据绕开明文白名单」这条威胁本来就不存在，初稿那条验收判据（往 `http://` 目标注入 `Authorization` 期望 `net-policy-violated`）因此删除，替换为「被策略拒的目标 → 钩子零调用」。双闸的诚实边界见 §变更内容 第 3 项，本件不伪造一条能抓住闸 #2 的用例。

6. **不做全链替换不等于可以不说清「为什么不做」。** OkHttp 的 `Interceptor.Chain` 把「谁接下一环」交给第三方，代价是拦截器可以决定不 `proceed()`、可以换掉 `Request` 的目标、可以改写应答——即协议正确性的最终裁决权在宿主手里。本库的立场（§13 原句 + §15 非目标「不做全量 socket 选项封装与拦截器全链语义」）是裁决权留在库：钩子拿不到 `proceed()`、拿不到连接与 TLS 事实、拿不到改写应答的能力（`after_response` 的入参是 `const Response &`，返回 `void`）。而为了让「不触网」这一维仍可表达，钩子拿得到一枚**只此一次**的短路资格：`before_request` 的返回值携 `Result<Response>`，即一次交换的结局可以完全由钩子定，但**交换一旦进入环里，钩子就再无话语权**。这条取舍同时给出了 `Response` 可公开构造这一既有事实的用武之地（`include/aurora/net/http.h` 里 `Response(std::uint16_t, std::string, Headers, std::vector<char>, bool)` 在 public 段，本件无需为替身另造一枚应答构建器）。

7. **`Request &` 不是一条可执行的契约，只读视图才是。** 初稿让 `before_request` 拿 `Request &`，再用注释声明「只许改头」。这个声明在类型上不成立：`Request` 的公共面同时开着 `body()`、`body_source()`、`range()`、`if_range()`、`expect_continue()`、`timeout()` 等改子。钩子只要调一次 `body_source()->restart()` 就能把下一次重放/重定向的体游标挪位；写一枚 `range()` 就能绕过缓存的「带范围整段不参与」闸；写 `expect_continue()` 就能绕过 100-continue 的现地流程。这些都不是「钩子作者不小心」，而是参数类型把路铺好了。本稿改为**只读视图 + 独立 `Headers &` 写通道**：视图只有 `method()` / `url()` / `headers()` / `leg()` 四枚读取口，体与 `BodySource` 根本不在视图上；写通道只有 `Headers &`。于是「只改头」由编译器兑现，而不是由注释兑现。

8. **递归是本件唯一的隐性故障面，必须当场定形而不是留给注释。** 典型现场：宿主挂一枚遥测钩子，钩子里用**同一枚 `Client`** 把日志 POST 到采集端。链上的第二枚交换又会问这枚钩子，于是每次交换在运行时多出一条交换、那条交换又多出一条——无界递归，结局是栈溢出而非一条错误。本库缺省的完成投递在主线程，这条链一旦跑起来整个界面一起没。§16 第 34 项对 `Authenticator` 没有这个问题（换取 token 用的是宿主另一枚客户端，且 `asked` 闸把一次交换的问答封在「至多一次」里），钩子没有等价物可封，故本件自带一枚精确对症的闸，形态与取舍见 §变更内容 第 7 项。闸管**同步重入**；钩子把嵌套交换推迟到回调返回之后，闸看不到，那仍是宿主的责任——这条也写进规格，不让闸冒充一个它给不出的保证。

## 变更内容

### 1 公共面：一枚只读视图、一枚抽象类、一枚 RAII 句柄、一枚枚举、一枚登记方法

新增一枚公共头（计划/待建的 include/aurora/net/http_hook.h，命名空间 `aurora::net::http`，与 include/aurora/net/http_auth.h 同层同格局），伞头 `[direct]` 收录、tools/check/umbrella_manifest.txt 随批刷新；`include/aurora/net/http_client.h` 包含它以声明 `Client::use`，两枚头之间无环（`http_hook.h` 只含值层，不含 `Client`）：

```cpp
namespace detail {
class ExchangeHookChain;  ///< 库内链内核：视图的唯一构造点
}

/// @brief 一次交换发起于哪条腿——钩子据此判断自己的短路资格与字段口径是否适用。
enum class ExchangeLeg : std::uint8_t { Send, Stream, ServerSentEvents, WebSocket };

/// @brief 一次交换的只读请求视图：方法、目标、当前头与腿。
///
/// 视图是**回调入口时刻的快照**：本枚钩子经 `headers` 写下的字段不会出现在它自己的视图里，但会出现在
/// 后续钩子的视图里。视图只在回调期间有效，不得留存、不得跨线程传递。
/// 方法、目标、体、协议旋钮与超时都不在视图上——本件只允许改写请求头。
class ExchangeRequestView {
  public:
    ExchangeRequestView(const ExchangeRequestView &) = delete;
    auto operator=(const ExchangeRequestView &) -> ExchangeRequestView & = delete;
    ExchangeRequestView(ExchangeRequestView &&) = delete;
    auto operator=(ExchangeRequestView &&) -> ExchangeRequestView & = delete;

    /// @brief 请求方法。
    /// @return 方法枚举。
    [[nodiscard]] auto method() const noexcept -> Method;

    /// @brief 目标 URL。
    /// @return 目标；当前四条腿都在目标闸之后才问钩子，故恒有值。类型仍与 `Request::url()` 同形。
    [[nodiscard]] auto url() const noexcept -> const std::optional<Url> &;

    /// @brief 当前请求头（只读）。
    /// @return 字段表的只读引用（含调用方自写、链路标识与已附着的 Cookie；不含库内稍后补的条件头、
    ///         `Accept-Encoding`、`Host` 与分帧字段）。
    [[nodiscard]] auto headers() const noexcept -> const Headers &;

    /// @brief 本腿。
    /// @return 腿枚举。
    [[nodiscard]] auto leg() const noexcept -> ExchangeLeg;

  private:
    friend class detail::ExchangeHookChain;  ///< 库内链内核：视图的唯一构造点
    ExchangeRequestView(Method method, std::optional<Url> url, Headers headers, ExchangeLeg leg);
    Method method_;
    std::optional<Url> url_;
    Headers headers_;
    ExchangeLeg leg_;
};

/// @brief 挂在 Client 上的交换钩子：同步、只改请求头、可在 send 上短路整次交换。
///
/// 两问都同步跑在推进本次交换的协程所在线程上，必须不阻塞、不做 I/O、不抛异常、不持有 Client
/// （会形成引用环）。不同交换可能并发问同一枚钩子，实现须自行保证线程安全。
class ExchangeHook {
  public:
    virtual ~ExchangeHook() = default;

    /// @brief 起手问：经 headers 就地改写请求头；nullopt = 照常发起，有值 = 本次交换以此收摊。
    /// @param request 本次交换的只读视图（入口时刻快照）。
    /// @param headers 请求头的写通道；只可改头，且不得触碰保留头（头注释与规格逐条列出）。
    /// @return 空 = 继续；有值 = 短路。仅 `ExchangeLeg::Send` 承认短路值：其余三腿交出有值即整次落
    ///         `http-request-invalid`。短路成功应答的 `from_cache()` 必须为假，否则同样落该码；
    ///         短路是终点，不进缓存、不更新 Cookie Jar、不跟随重定向、不进重试环。
    /// @note 抛异常即整次落 `http-request-invalid`，链上其余各枚的 `after_response` 一概不问。
    [[nodiscard]] virtual auto before_request(const ExchangeRequestView &request, Headers &headers) const
        -> std::optional<Result<Response>> = 0;

    /// @brief 收摊问：只在成功交出一枚应答时问（线上应答 / 新鲜缓存副本 / 本链短路成功）。
    /// @param request 同一个逻辑请求的收摊视图（钩子改写后的头；不含库内补的条件头、`Accept-Encoding`、
    ///                `Host` 与分帧字段；重定向跨主机时仍是原始目标，不是最后一跳）。
    /// @param response 已物化、已解压的应答；短路成功时是钩子交出的那一份。
    /// @note 失败、取消、流式三腿一律不问；本回调的异常被捕获并记一行 WARN，应答照常交付。
    virtual auto after_response(const ExchangeRequestView &request, const Response &response) const -> void;
};

/// @brief 登记句柄：move-only RAII，析构即从链上摘除本次登记。
/// @note Thread: 句柄本身一次只由一个线程操作；链上的登记/摘除可来自任意线程。
class ExchangeHookRegistration {
  public:
    ExchangeHookRegistration() = default;
    ~ExchangeHookRegistration();
    ExchangeHookRegistration(const ExchangeHookRegistration &) = delete;
    auto operator=(const ExchangeHookRegistration &) -> ExchangeHookRegistration & = delete;
    ExchangeHookRegistration(ExchangeHookRegistration &&other) noexcept;
    auto operator=(ExchangeHookRegistration &&other) noexcept -> ExchangeHookRegistration &;
    /// @brief 提前摘除；重复调用与析构并存不双重摘除。
    auto clear() -> void;
};

class Client {
  public:
    /// @brief 登记一枚钩子并返回其句柄；链按登记次序跑，拷贝的 Client 共用同一条链。
    /// @param hook 钩子；空指针即一次空登记（句柄可析构，链上无条目）。
    /// @return 登记句柄；可早于 Client 销毁，析构不触碰已亡的链。
    /// @note const：登记进入的是共享内核而不是本对象；登记与注销可在任意线程调用，对在途交换只在下一次
    ///       交换生效（每次交换起手取一份快照）。
    [[nodiscard]] auto use(std::shared_ptr<ExchangeHook> hook) const -> ExchangeHookRegistration;
};
```

四条取舍记在案上。**其一，不引入 `std::function` 旋钮而用抽象类**：两枚 `std::function` 装不下「短路时交出什么」这一维（要么牺牲错误分支、要么再叠一枚可选返回值，反而更长），而仓内 `.clang-tidy` 已记录「任何转入 `std::function` 的可调用对象一律判为不应抛出」这条系统性假告警面——本件正好有一条「钩子不得抛」的契约要写，不去招那枚假阳。同批的先例是 `Authenticator` / `BodySource` / `DataSink` 全是抽象类。**其二，`after_response` 给默认实现**（空体），使「只加头的钩子」不必写第二问；`before_request` 纯虚，因为它必须表态一次。**其三，链上没有查询口**（无 `hooks()` 计数、无按 id 摘除）：句柄即真值，多一枚查询口就多一处「它与句柄不一致时以谁为准」。**其四，视图删去拷贝与移动**：它不是一份可以带走的资料；删掉两对操作比在注释里写「别留着」更接近契约。

新公共枚数：一枚抽象类 + 一枚只读视图 + 一枚 RAII 句柄 + 一枚枚举 + 一枚方法。`ExchangeLeg` 登记进 tools/include/known_enums.h 并同步 tests/integration/itest_known_enums.cpp 的编译期锚点。

### 2 改写边界：只读视图 + 头写入通道，保留头集合与触点差异

`before_request` 拿到的写通道只有 `Headers &`。视图与写通道的分工是**入口时刻的只读事实**与**要上线的可变字段**两件事；本轮钩子写下的字段不会回写自己的视图，后续钩子的视图会重新取一份快照，因此链上每一枚看到的都是「前一枚改完之后的那一刻」。方法、目标、请求体、`BodySource`、`range` / `if_range` / `expect_continue` / `timeout` 一律不可达——它们不在视图上，也没有第二枚写通道。三条理由：

- **目标本来就没有改子。** 实测 `Request` 的公共面里 `url()` 只是只读访问器（`const std::optional<Url> &`），没有 `url(...)` 设值口，`method()` 同理。要放开「钩子改目标」就得先新增一枚公共改子，而那是另一件事的形状。
- **不改目标换来一条硬收益：策略闸与路由不需要第二次判定。** 三条已定案口径因此原样成立——「本跳有没有经代理只有一个答案」（§16 第 30 项的 `plan_route` 单入口）、策略三维读的是目标与 `TlsContext`（第 31 项）、钩子不可能把 `https://` 改成 `http://` 从而绕过明文白名单。若允许改目标，`gate_origin` 与 `plan_route` 各要重跑一遍，库里当场长出第二个路由答案，时间线与活动表（可观测性切片）也须跟着改口径。
- **不改体与协议旋钮是为了不拆 `plan_request_body()` 那一枚唯一决策点**（第 33 项）。钩子在决策之后改体即「声明 `Content-Length: 5` 而发出分块」那一类错位；`range` / `if_range` / `expect_continue` 是 `render_head()` 现地补进线上的字段（第 33 项第 ⑦ 条：它们不进 `headers()` 字段表），钩子若经 `headers()` 手写一枚裸 `Range`，前一道缓存闸失效——本件不给这条路留任何侥幸，头注释直接写「钩子只改头，且改的是要上线的字段」。

**保留头集合：改了就整次落 `http-request-invalid`。** 「只改头」仍是一条很宽的通道，下面这些字段各自把着一条库内不变量，钩子不得触碰：

| 组 | 保留头 | 理由 |
|:---|:---|:---|
| 目标与身份 | `Host`、`X-Request-Id` | `Host` 改变虚拟主机而 URL 不变，策略闸与缓存键都按 URL 判；`X-Request-Id` 在钩子之前已落定并由日志行读定，钩子后覆写会让日志、活动条目与应答侧看到两个标识 |
| 分帧与体 | `Content-Length`、`Transfer-Encoding`、`Content-Encoding` | 体形态只有 `plan_request_body()` 一枚决策点；请求体恒是原始字节，钩子写 `Content-Encoding` 等于对端声明一份不存在的编码 |
| 连接与逐跳 | `Connection`、`TE`、`Trailer`、`Upgrade` | 连接复用、逐跳字段与升级都由库定；钩子改这些等于在通用交换上开一条本库不认识的协议 |
| 范围与预检 | `Range`、`If-Range`、`Expect` | 缓存范围闸读的是 `Request::range()` 而不是头；`Expect` 有自己的 100-continue 流程。裸写头会绕过各自的闸 |
| 代理 | `Proxy-Authorization` | 代理凭据由代理协商写定；钩子改写会把「谁在回答」变成两个答案 |
| WebSocket（仅该腿） | `Sec-WebSocket-Key`、`Sec-WebSocket-Version`、`Sec-WebSocket-Protocol`、`Sec-WebSocket-Extensions` | 握手四字段由库写定；子协议与扩展走 `WebSocketOptions`，钩子不得绕开那条入口 |

保留头检查是**逐钩子的前后差分**，不是「存在即拒」：调用方本来就可以在 `Request` 上自带 `Host` / `Content-Length` / `Range`（既有边界，本件不修），钩子只要**没有改动**它们就合法。每枚钩子跑完，按大小写不敏感的字段名与值序列比较前后两份字段表；某枚保留头被新增、删除或改值，即判 `http-request-invalid`，detail 点名钩子序号、腿与那枚保留头的名字，**不带值**。大小写是统一的：写 `content-length` 与写 `Content-Length` 是同一枚保留头。

`X-Request-Id` 是初稿与本稿之间的一处**反转**，记在此：初稿允许钩子覆写它，理由是「钩子看到的已经是落好的标识，覆写就是一次显式覆写」。实测不成立——`stamp_request_id` 与 `log_exchange_identity` 都在钩子之前，而每一跳的 `ExchangeTrace` 是在重试环内按 `origin` 副本建的；钩子在两者之间改标识，日志行、活动条目与缓存/重定向副本就会读到两个值。故本稿把它收进保留头。**调用方仍可在 `Request` 上自带 `X-Request-Id`**，`stamp_request_id` 会原样尊重它；只是钩子不能改。这是一条可预防的限制，代价是「钩子想换标识」这件本就不该由观测面做的事少了一条路。

`Authorization` / `Cookie` **不在**保留集合里：钩子可以写它们。`Cookie` 的附着在钩子之前（`attach_cookies_from_jar`），故钩子写下的值赢过 Jar；`Authorization` 若被认证环在一次 401 / 407 后重写，以认证环那一份上线（见 §变更内容 第 6 项的同名冲突裁决）。`Origin` / `Accept` / `Content-Type` / `User-Agent` 与一切自定义头照常可写。

**触点差异集：钩子改过的头，跨 origin 要剥、进缓存要变。** 这是本件与重定向链、缓存表之间唯一的新耦合，必须精确：

- **怎么算**：每枚钩子跑完，把前后两份字段表按「大小写不敏感的字段名 → 值序列」逐名比较；某名被新增、删除或改值即记为触点。整条链取**并集**，不回退——一枚钩子改了又改回去，名字仍算触点（回退与否不该改变「这枚头曾被钩子经手」这一事实）。
- **跨 origin**：`follow()` 在换 origin 时除既有的 `Authorization` / `Cookie` 外，再剥掉触点集里的全部名字。理由：库不知道哪一枚自定义头是凭据，而钩子改过的那一组是库唯一能知道的「这可能是钩子注入的身份」；保守剥掉可能误伤一枚无害头，但不剥就可能把租户标识、API key 或签名带到陌生对端。**调用方自写且钩子没碰过的自定义头维持今天的跨 origin 行为**——这条不对称必须写进规格：库只对「它看见被改过」的头负责，猜测凭据名的代价比这更大。
- **进缓存**：`open_from_cache` / `settle_cache` 与 `detail::HttpResponseCache` 的 `lookup` / `store` / `revalidated` 各收一组额外 `Vary` 名（与应答 `Vary`、`AURORA_CREDENTIAL_VARY_FIELDS` 去重，口径沿用表内既有的 `(name, value_or(""))` 形态）。**旧条目缺少当前触点名的，一律不命中**：一枚钩子这次加了 `X-Tenant`，上一次存下的条目没有把这枚头算进键，若照常命中，等于让新租户读到旧租户的副本。查找侧必须先检查「条目 `vary` 覆盖当前触点集」，再做既有的逐值比较；写入侧把触点名与当时的取值一并存进 `vary`；重验证侧走同一套匹配。
- **诚实代价**：一枚在每次请求里写唯一值（nonce、时间戳）的钩子会让缓存键逐请求不同，缓存退化成逐请求存储/不命中。本件不猜「哪些头是凭据」来避免这个代价——猜错的代价是私密内容串租户，比缓存不命中贵得多。这条写进规格，作为开启钩子 + 缓存时的已知口径。
- **不算触点**：库内稍后补的 `If-None-Match` / `If-Modified-Since`、`Accept-Encoding`、`Host` 与分帧字段。触点差分在链跑完之后、这些字段补上之前算，故它们不会混进来；钩子也看不到它们（视图只给入口时刻的字段表）。

钩子写出的头与调用方自写的头走同一条 `Headers` → `render_head()` 序列化路径；本件不为钩子另造一层字段合法性校验——字段合法性是 `Headers` 的既有边界，钩子是宿主进程内的可信代码，不因为多一枚回调就获得比调用方更大的能力。调用方裸写 `Range` 绕过缓存闸的既有缺口也不在本件修（那是 `Request` 侧的旧边界），本件只保证钩子不新增一条同样的绕行路。

### 3 钩子的两问在 `send` 上的落点：双闸、短路、记账

按 §动机 第 5 项实测出的起手串，落点定死如下，逐条附理由：

| 次序 | 既有环节 | 与本件的关系 |
|:---|:---|:---|
| 1 | 取消判定（令牌到手即已取消） | **恒先于钩子**。取消那侧一次钩子都不问，与「策略闸、缓存表、连接一概不问」是同一句承诺，判据可断 |
| 2 | `RetryPolicy` 形态校验 | 先于钩子：形态非法的交换不配被改写 |
| 3 | 复制 `origin` → `stamp_request_id` → `log_exchange_identity` → `attach_cookies_from_jar` | 先于钩子，故钩子看到的是带标识、Cookie 已附着、与日志行同源的请求；`X-Request-Id` 因此必须是保留头 |
| 4 | **策略闸 #1**：`effective_policy` + `gate_origin` | **先于钩子**。`gate_origin` 只读目标 / 端口 / 代理 / `TlsContext` 与明文主机白名单，不读头部；被拒即 `net-policy-violated`，链上一次都不跑。这是初稿次序的纠正，不是偏好 |
| 5 | **`before_request` 链** | 按登记次序跑；每枚取一份视图快照、跑完检查保留头与触点差异；一枚短路即停，后面的不再问 |
| 6 | **钩子后取消复检** | 取消先于短路：钩子在回调里取消了令牌，结局仍是 `net-cancelled`，短路值丢弃 |
| 7 | **策略闸 #2**：`gate_origin` 重跑 + 保留头终检 | 双闸的第二道。`gate_origin` 的入参与钩子可改面不相交（目标 / 端口 / 代理 / `TlsContext` 都不可达），故它今天是**结构不变量**、不是可独立观测的新判据；保留头终检是逐钩子检查之后的**冗余防线**。两者都保留，诚实边界见下 |
| 8 | 短路处理 | 仅 `Send` 承认；成功 → `record_hook_delivery`、问前缀钩子的 `after_response`、直接收摊；携错 → `record_local_outcome`、直接收摊；`from_cache=true` → `http-request-invalid` |
| 9 | fetch 腿体形态判定 | 后于短路：替身不必为 fetch 腿物化体，流式体请求也能被 mock |
| 10 | `open_from_cache` | 短路不查表；未短路才查。命中新鲜副本 → 问 `after_response` 后返回 |
| 11 | 重试环 / 认证环 / 逐跳 `plan_route` | 钩子一律不再问。一次交换 `before_request` 恰好一次、`after_response` 至多一次；逐跳与逐尝试的细节属可观测性切片的时间线，不在本件重复设闸（第 32 项「逐跳重算的是决策，不是身份」的同一条分界） |
| 12 | `settle_cache` | 触点集作为额外 `Vary` 名写进/匹配缓存条目 |
| 13 | 网络收摊 | 问 `after_response`；入参是最终那一条应答的明文体形态（含非 2xx），以及原始逻辑请求的视图 |

**双闸的诚实边界。** 闸 #1 是今天唯一能因目标而拒绝的闸，且它在钩子之前，故「被拒目标 → 钩子零调用」是可观测、可断言的。闸 #2 的 `gate_origin` 重跑在钩子不能改其入参的前提下**不可能**得出不同结论；它存在的意义是把「上线前最后一刻再判一次」写成结构不变量，将来若有人给钩子开了目标改子，第二道闸已经在那里。保留头终检同理：逐钩子检查已经是 fail-fast 的守卫，终检只是再扫一遍。**本件不伪造一条能抓住闸 #2 移除的用例**——那种用例只能靠测试专用后门，等于把「判据」变成「证明实现调用了某个函数」。规格与验收表逐字写明这一点；变异矩阵里闸 #2 与终检列为**不可独立观测项**，不计入「必转红」。

**短路语义（终点，不是旁路）。** `send` 上的短路值一经承认，本次交换立即以该 `Result<Response>` 收摊：不查缓存、不进缓存、不更新 Cookie Jar、不跟随 `Location`、不进重试环、不发进度、不再解压。四条推论写死：

- **mock 一个 HTTP 错误状态是成功交换**：钩子交 `Response{503}` 时调用方拿到 503，`after_response` 照问，观测记一条交付条目；要让调用方看到「传输失败」就交 `Result` 的错误分支，两者不是一件事。
- **mock 的 `Set-Cookie` 不落 Jar、`Location` 不跟随、503 不重试**：替身的产物不得变成会话身份、第二次真实交换或重试环的燃料。钩子想要真实的这三件事就交 `nullopt`，让库照常走。
- **短路赢过缓存**：短路在 `open_from_cache` 之前，所以即使表里有新鲜副本，钩子仍能决定这次不触网、交自己的应答。这是替身该有的优先级；反过来「缓存命中让 mock 失效」才会让用例随缓存状态漂移。
- **`from_cache=true` 的短路应答拒收**（`http-request-invalid`）：`from_cache` 是库对「这份来自表」的事实声明，钩子不能伪造一枚缓存命中；要 mock 就交 `from_cache=false`。同理，短路应答的体按原样交付，库不再解压——钩子交的是已物化、已解压的最终值。

**配对规则。** 只有 `before_request` **实际跑过**的钩子才在 `after_response` 的前缀里，短路那一枚也算在内；被短路截断的后继、被递归闸跳过的、因异常而未问到的，一概不问。失败、取消、流式三腿、策略拒绝也一概不问。一次交换对一枚钩子至多问一次 `after_response`。收摊视图是「钩子改写后的原始逻辑请求」：方法、目标、字段表（不含库内补的条件头 / `Accept-Encoding` / `Host` / 分帧字段），跨 origin 重定向后仍是原始目标而不是最后一跳——`after_response` 问的是「这次交换起手时是什么」，逐跳细节归时间线。异常口径见 §变更内容 第 8 项。

### 4 四条腿的覆盖面与短路在流式腿上的诚实失败

- `send`：`before_request` + `after_response` 成对，短路生效（成功与携错两种都生效——携错即「模拟一次失败」那条断言需求的落点，无需新码）。
- `open_stream`：抽出私有入口 `open_stream_impl(Request, DataSink &, Cancellation, ExchangeLeg)`，公共 `open_stream` 传 `ExchangeLeg::Stream`；钩子在闸 #1 之后、缓存/重试之前跑，只问 `before_request`，不问 `after_response`（本腿的收摊值是 `StreamOutcome` 而非 `Response`）。
- `open_sse`：每条（重）连都经同一枚 `open_stream_impl`，传 `ExchangeLeg::ServerSentEvents`；§16 第 38 项已定「一条订阅环在一枚帧里 `co_await` 一次 `open_stream`」，一次重连本身就是一次有始有终的调用，故**逐连重问一次** `before_request`。这不是「逐跳」——重定向链内部的跳不重问，重连才重问。
- `connect_websocket`：只问 `before_request`（`ExchangeLeg::WebSocket`），问的是握手那一跳；升级后的会话不属一次 HTTP 交换，与可观测性切片把会话排除出请求活动表是同一条分界。本腿不问 `after_response`（收摊值是 `WebSocket`）。
- **非 `Send` 腿上钩子交出短路值**：如实落 `http-request-invalid`，detail 点名是哪枚钩子、哪条腿、以及「短路只在 `send` 生效」，绝不静默忽略。理由是静默丢弃等于让宿主的替身「以为自己在 mock，实际打到了真网」——那正是本栈要消灭的那类无痕故障；写错误码而非忽略的代价（一枚钩子同时服务 `send` 与流式腿时会被打断）由 `ExchangeLeg` 入参抵掉：钩子问得到自己在哪条腿上，因此这条错误是可预防的，而「预防所需的知情」正是那枚枚举存在的全部理由。`open_sse` 上这条错误还有一个硬收益：`http_sse.cpp` 的 `refused_before_the_wire()` 已含 `HttpRequestInvalid`，故订阅**不会**带着同一个短路值重连八次——错误一次说清，不变成重连风暴。
- **流式三腿为什么没有 `after_response`**：收摊值分别是 `StreamOutcome` / `SseOutcome` / `WebSocket`，为钩子另造一枚「头部已定、体未读完」的应答快照，等于在库里长出第二份应答真值（`Response` 与 `StreamOutcome` 谁说的是最终事实将无从陈述）。本件不做，随裁决写进规格并登记为**刻意限制**（不是待办）；将来若要，另批提案。
- **落点次序（流式腿）**：`open_stream_impl` 为取消 → 重试形态校验 → 可重放 / 环设置 → 序列与墙 → 复制 / 打标 / 日志 → 能力闸 → Cookie 附着 → 闸 #1 → 钩子链 → 钩子后取消 → 闸 #2 → 非 `Send` 短路拒绝 → 缓存 / 重试。能力闸保留在钩子之前：本腿没有流式能力时连一条流都建不起来，钩子的短路也不会被承认，先拒可少问一遍。`connect_websocket` 为取消 → 方法闸 → 复制 / 打标 / 日志 / Cookie → 能力闸 → 闸 #1 → 方案 / 端口校验 → 钩子链 → 钩子后取消 → 闸 #2 → 非 `Send` 短路拒绝 → `plan_route` → 握手。
- **缓存**：`open_stream` 的缓存是只读的（命中新鲜副本照常交付），触点集同样进它的 `lookup`；`open_sse` 不查不写；WebSocket 不查不写。`send` 的 `settle_cache` 用触点集写入/重验证。

### 5 链内核、快照、句柄与并发口径

链住在一枚新增的库内共享内核（计划/待建的 src/aurora/net/detail/http_hook_chain.h 与 .cpp），`Client` 以 `std::shared_ptr` 持有，与 `cache_` / `pool_` / `cookie_jar_` / `authenticator_` 完全同格局：拷贝的 `Client` 共用同一条链，登记与注销在任一份拷贝上做都是给整条链做。公共头只做前向声明（照 `detail::HttpConnectionPool` 的既有形态），`Client` 因此多一枚私有成员、不改任何既有签名。

- **次序**：按登记次序跑（第一枚最先看到未改写的请求，也最先获得短路资格；后一枚看到前一枚改写后的那份）。不提供优先级字段——「登记次序即执行次序」是可陈述、可断言的唯一口径，加一枚优先级旋钮就多一处「同优先级时以谁为准」。
- **快照**：一次交换在起手取一份链快照（拷出 `shared_ptr` 数组），此后 `use` 与句柄析构都不影响在途交换。与 §16 第 31 项「生效时刻是发起那一刻」同律，也让「钩子在回调里注销自己」这种形态有确定结局（本次交换照跑完，下一次不跑）。快照中的 `shared_ptr` 同时保证「回调里注销另一枚钩子」不会把那枚的实现对象在本次交换中途析构。
- **句柄**：持链的 `weak_ptr` + 本次登记的编号；`clear()` 与析构都走同一条幂等摘除；移动把编号转走、源对象变空。句柄**不延长链的生命期**：所有 `Client` 都没了，链即销毁，句柄析构为空操作。`use(nullptr)` 返回空句柄，链上无条目。忽略 `use` 的返回值等于登记后立刻析构句柄、立刻摘除——头注释把这条写清。
- **并发**：内核一枚互斥量护住链本体；登记 / 注销可在任意线程调用，快照在协程帧所在线程取。锁只在取快照与增删条目时持有，**调用钩子时不持锁**——否则钩子在回调里注销自己或另一枚会自锁。两问同步跑在推进本次交换的协程所在线程上，缺省即主线程投递器那条；不同交换可能并发问同一枚钩子，钩子实现必须自行保证线程安全，且不得阻塞、不得做 I/O、不得在非主线程直接触碰 UI。
- **快路径**：链空时只读一枚原子计数即返回，不加锁、不分配、不构造视图；`send` 也据此跳过闸 #2 与全部差分。判据断「空链时线上字节与从未挂载逐字节相同」。
- **重复登记不去重**：同一枚 `shared_ptr` 登记两次即在链上跑两遍。去重需要比较实现方身份，而钩子实现可以持可变态（同一实例挂两处是常态写法），库无从陈述「这两次是同一件事」。句柄仍各自管各自的摘除。
- **所有权**：链持有钩子的 `shared_ptr`，故钩子在登记期间不会被销毁；钩子**不得持有 `Client`**（值、引用成员、`shared_ptr` 都算）——链 → 钩子 → `Client` → 链是引用环。钩子需要发起交换时用另一枚不挂同一枚钩子的 `Client`，或持弱引用/在回调外取。规格与头注释都写这一条，句柄的 `weak_ptr` 救不了环。

### 6 与既有注入点的分工（一张表写进头注释与规格）

| 要表达的事 | 落点 | 本件为什么不是它的替代 |
|:---|:---|:---|
| 每请求补同一组静态字段（API key、租户头） | 本件的 `before_request` | — |
| 挑战-答复型凭据（401 / 407） | `Authenticator` | 协程、可再去一次网络换 token、一次交换至多问一次、不吃 `attempts` 名额，钩子这三条都不该有；两处都写 `Authorization` 时以认证环那一份上线 |
| 能不能谈（明文白名单、TLS 下限、必须经代理） | `NetPolicy` + `gate_origin` | 闸在钩子**之前**判，且钩子改不动它的入参（目标、端口、代理、`TlsContext`） |
| 逐跳耗时、阶段、活动条目、失败归因 | 可观测性切片（时间线 / 活动表 / 事件总线） | 只读推模型、受 `AURORA_ENABLE_DEBUG` 裁切；钩子在链上、可改写、生产可用。短路交付经新增的库内 `record_hook_delivery` 记一条，见 §变更内容 第 9 项 |
| 退避与重试 | `RetryPolicy` | 钩子不参与逐尝试，重试环的次数不由钩子决定；短路是终点，不进环 |
| 上传/下载进度 | `Request::on_progress` + 合流闸 | 逐段回调不属交换路径的起点与终点；短路不发进度 |
| 传输事实的替身（半关闭、乱序、TLS 失败） | `FakeEngine` / `ScriptedPeer` | 钩子造不出线上字节 |
| Cookie 身份 | `Session` 的 Cookie Jar | Jar 在钩子之前附着，钩子可覆盖本次请求的 `Cookie`；短路交付不更新 Jar |
| 链路标识 | `Request::request_id()` / `Client::request_id()` | 两者都在钩子之前落定；`X-Request-Id` 是保留头，钩子只能读 |

**同名冲突定案**：钩子与认证环都写 `Authorization`（或 `Proxy-Authorization`）时，**以认证环那一份上线**。挑战-答复的语义就是「上一份不被接受」，钩子不该有推翻对端裁决的特权；`place_auth_decision` 的 `set_header` 覆盖行为因此是正确形态，本件不改它，只把头注释与规格写成「两处都写时以问答结果为准」。这条也解释了为什么钩子不是认证注入的替代口：它的注入会被一次 401 正当覆盖。钩子与 Jar 都写 `Cookie` 时以钩子为准（钩子在附着之后表态，与「调用方自写的 `Cookie` 优先，Jar 不覆盖」同一条优先级）；钩子与调用方写同一枚自定义头时以钩子为准（后者是更晚的显式动作）；保留头不在此列，钩子碰即错。

### 7 递归闸：线程局部「本枚在栈上」即跳过该枚并记一行 WARN

一枚钩子在自身的 `before_request` 或 `after_response` 尚未返回时，经由同一枚 `Client` 又发起一次交换，导致链上会再问这枚自己——这是遥测钩子的典型形态，结局是无界递归与栈溢出。定案：内核以一枚**线程局部**的「正在栈上的钩子」集合护住每一次回调（两问都过同一道闸）；某枚钩子被问时若自己已在栈上，则**跳过该枚、链上其余各枚照常跑**，并经 `AURORA_LOG_WARN("net", ...)` 记一行（只带可公开事实：钩子序号、腿、本次交换的标识，**不带** URL、头部名、头值与异常 `what()`，AGENTS.md 规则 8 与脱敏纪律同源）。被跳过的那一次交换照常完成、照常交付、照常记活动条目。闸用 RAII 在回调入口压栈、出口（含异常）弹栈；同一枚钩子在一次交换里被跳过多层也只记一行。

三条取舍写在此：**为什么不是「嵌套照常跑链」**——那把宿主的一次写错变成 `0xC0000005`，而本库缺省的完成投递在主线程，一次递归带走整个界面；**为什么不是「嵌套落一条错误」**——遥测钩子不该有能力让业务请求失败，把观测面写成故障源是反向的优先级；**为什么不是「整条链跳过」而是「只跳在栈上那一枚」**——链上其余各枚与被这枚钩子发起的那条日志请求毫无关系，整链哑掉会顺带吃掉别人的静态头注入，那才是真的无痕故障。闸管的是崩溃边界而非安全边界，故 WARN 而不是错误。

两条边界必须一起写进规格：**其一，闸只管同一线程的同步重入**；钩子若把嵌套交换投到另一条线程、或在回调返回之后才让它跑，栈上的集合已经看不到它，闸不生效——宿主的无界自反馈仍是宿主的责任。**其二，被跳过的钩子不在 `after_response` 前缀里**：它没跑 `before_request`，就没有「收摊问」的资格；后备交换里的其余钩子照常配对。

### 8 异常、取消、失败面与「无新码」

- **`before_request` 抛异常**：整次落 `http-request-invalid`，一条连接都不发起，链上后续各枚不再问，`after_response` 对谁都不问。理由：一次没跑完的改写让字段表停在半截，继续把这份半截请求发给对端比失败更糟。detail 只带钩子序号与腿，不带 `what()`、URL 或头值。
- **`after_response` 抛异常**：就地捕获、记一行 WARN（同样只带序号 / 腿 / 交换标识），应答照常交付，链上其余钩子照常问。收摊问是观测面，不得反向失败一次已经成功的交换。
- **钩子后取消**：`before_request` 链跑完后复检令牌；已取消即 `net-cancelled`，短路值丢弃，`after_response` 不问。取消先于短路，与「取消先于一切」同一条次序。
- **策略拒绝**：闸 #1 被拒即 `net-policy-violated`，钩子零调用；闸 #2 若在将来因扩展而拒绝，同样按此收口。
- **保留头 / `from_cache=true` / 非 `Send` 短路**：一律 `http-request-invalid`，detail 点名钩子序号、腿与成因；保留头另点名那枚头（固定集合里的名字，不是任意头值）。
- **不新增错误码**：全表仍沿用既有码（`http-request-invalid` / `net-cancelled` / `net-policy-violated`），`codespec/errors.toml` 与生成的 `codespec/ERROR_CATALOG.md` 不动。钩子写错是一次调用点错误，与 §16 第 34 项把调用点错误收进 `http-request-invalid` 同族；递归闸不产错误，只记 WARN。
- **无门控、无新宏**：钩子不受 `AURORA_ENABLE_DEBUG` 裁切，也不新造构建宏（静态凭据注入与生产遥测是它的本职，那类用途与「调试构建」无关）。`Client` 的公共面因此多一枚 `use`，`Request` / `Response` 零改动，四条腿的公共签名零改动（只多一枚私有 `open_stream_impl`）。

### 9 可观测性：新增一枚库内 `record_hook_delivery`，不复用缓存命中口

短路成功是一条**没有线上往来的成功交付**，与既有三类记录都不同：真实交换有阶段 mark；新鲜缓存命中是 `from_cache=true`；本地拒绝有错误码、没有状态码。初稿写「复用 `record_cache_hit`」，那会把短路伪装成缓存命中（`from_cache=true`），与事实相反。定案：在 `src/aurora/net/http_client.cpp` 的记账区新增一枚库内 helper（与 `record_local_outcome` / `record_cache_hit` 同层，不占公共 API）：

- **形态**：`record_hook_delivery(const Request &, const Response &)`——建一枚 `ExchangeTrace`（`hop=0`、`pass=0`、`reused_connection=false`、`from_cache=false`），记 `set_status(response.status())`、`note_received(response.body().size())`、`finish()`；**一枚 mark 都不记**，`sent=0`。于是条目可被 Inspector 与时间线唯一辨认：零 mark + 有状态码 + 无错误码 + `from_cache=false` = 钩子短路交付。真实交换有 mark，缓存命中 `from_cache=true`，本地拒绝无状态码。
- **失败侧**：短路携错、保留头拒绝、`from_cache=true` 拒绝、`before_request` 异常、钩子后取消，一律走既有的 `record_local_outcome(origin, code, 0, 0)`——零 mark、`failed_stage=nullopt`、带错误码，与既有的「起手后本地拒绝」同一形态。
- **不进公共结构**：`HttpActivityEntry` / `HttpRequestTimeline` 零字段改动；Release 下 `ExchangeTrace` 是零成本空壳，helper 随之无声。`AURORA_ENABLE_DEBUG` 关时没有条目，这是可观测性切片既有的裁切口径，不是本件的特例。
- **顺序**：`record_hook_delivery` 先于 `after_response` 发布，故收摊回调里查活动表已经能看到这次交付；网络应答与缓存命中的既有记账点也都在各自 `after_response` 之前，三条路同序。
- **规格回写**：`06-app-platform.md` §8.8.36 的那条「打标前后的拒绝分两档」补第三档——钩子短路交付；§8.8.37 的条目覆盖补「钩子交付」一类。两处都不新造字段。

### 10 交付批次

一批可完成：公共面（一枚头 + 四枚类型 + 一枚方法）→ 库内链内核、触点差异比较器、四腿落点与 `record_hook_delivery` → 单测与变异自证 → codespec 回写与数据面刷新 → 全量门禁。无跨批次红灯。可观测性已在位（§8.8.36—§8.8.39），故 `record_hook_delivery` 直接复用 `ExchangeTrace` 与活动存储，不再保留初稿「若观测切片先落地则……」的分支。本件落地后 §12 与 §17 的 `use(hook)` 一项即收口。

## 验收判据

1. **公共面与 API 数据面**：新头进伞头 `[direct]` 与 manifest；四枚类型 + `use` 出现在 `aurora_api.json`；`ExchangeLeg` 进 known_enums.h 且有 `itest_known_enums` 编译期锚点；`Client` 拷贝共享同一条链（在拷贝上登记，原对象发出的下一次交换被问到）。
2. **视图是编译期边界**：以 `std::void_t` 检测 `ExchangeRequestView` 上不存在 `body()` / `body_source()` / `range()` / `if_range()` / `expect_continue()` / `timeout()` / `url(...)` / `method(...)`；`headers()` 返回 `const Headers &`；`after_response` 的第二形参是 `const Response &`、返回 `void`。检测到任一存在即编译期红灯。
3. **闸先于钩子**：一个被 `NetPolicy` 拒绝的 `http://` 目标（明文白名单不含它）→ `net-policy-violated`、`before_request` 零调用、`sent_bytes == 0`。**这条替换初稿那条「注入 Authorization 期望策略拒」的伪判据**；把钩子挪到闸 #1 之前必须让本条转红。
4. **双闸的诚实记录**：把闸 #2 的 `gate_origin` 重跑或保留头终检改成 no-op，**不要求**任何既有判据转红——它们是结构不变量 / 冗余防线；规格与验收表逐字写明。保留头的行为由逐钩子检查的用例（第 5 项）覆盖，不由终检的移除覆盖。
5. **保留头逐钩子拒绝**：表驱动的用例把每一枚保留头各试一次「新增 / 删除 / 改值 / 改大小写拼写」，每次 `http-request-invalid`、零连接、detail 点名钩子序号、腿与那枚保留头的名字且不含头值；调用方自带的保留头在钩子不碰时照常上线；一枚钩子改 `content-length` 与 `Content-Length` 等价被拒。
6. **`X-Request-Id` 保留**：钩子覆写它 → `http-request-invalid`；调用方在 `Request` 上自带的值原样上线，日志行、活动条目与 `after_response` 视图读到同一串；未自带时由 `stamp_request_id` 补的值同样三处同源。
7. **只改头**：钩子新增 / 删除 / 覆盖自定义头与 `Authorization` / `Cookie` 后，方法、目标、体字节、`range` / `if_range` / `expect_continue` / `timeout` 与挂载前逐值相同；线上头部块反映钩子改动，请求体逐字节不变。
8. **链上次序与改写传递**：三枚钩子依次登记，断言第二枚的视图读到第一枚写的头、第三枚读前两枚，且链上跑出的最终线上头集与逐个改写的并集逐值相同；把次序改成逆序的变异必须转红。
9. **触点差异跨 origin**：钩子加 `X-Tenant`，对端回 302 到另一 origin → 下一跳不带 `X-Tenant`，同 origin 的 302 仍带；调用方自写且钩子未碰的 `X-Caller` 跨 origin 仍按今天的行为保留；`Authorization` / `Cookie` 照旧被剥。把 `follow()` 的触点剥离摘掉的变异必须转红。
10. **触点差异进缓存**：钩子加 `X-Tenant: A` 取回并存入；同 URL 的 `X-Tenant: B` 请求不命中、不带该头的裸打请求不命中、旧条目（未把 `X-Tenant` 算进 `vary`）不命中；同值第二次命中。把触点集从 `lookup` / `store` / `revalidated` 摘掉任一处的变异必须转红。另加一例：钩子注入 `Authorization` 换回的副本存入后，一枚不带该头的裸打请求读不到它（`Authorization` 恒在凭据 `Vary` 名里，这条是既有纪律在钩子通道上的复核）。再加一例：每次写唯一值的钩子让缓存逐请求不命中（诚实代价的判据，不是缺陷）。
11. **短路成功**：`send` 钩子交 `Response{200}` → 零连接（对端 accept 恒 0）、调用方拿到该应答、`from_cache=false`、`after_response` 对前缀钩子各一次；第二次同名请求照常触网（未进缓存）；mock 里的 `Set-Cookie` 不影响下一次请求的 Cookie 头（未进 Jar）；mock 302 + `Location` 不产生第二次交换；mock 503 在 `RetryPolicy` 开着时也不重试。
12. **短路携错**：钩子交 `Result` 错误 → 调用方看到该错误、零连接、`record_local_outcome` 记该码、`after_response` 零次。
13. **`from_cache=true` 拒收**：钩子交 `from_cache=true` 的应答 → `http-request-invalid`、零连接、无 `record_hook_delivery`。
14. **钩子后取消先于短路**：钩子在 `before_request` 里取消令牌并交成功短路 → 结局 `net-cancelled`、零连接、`after_response` 零次；把取消复检挪到短路之后的变异必须转红。
15. **起手已取消**：令牌到手即已取消 → 两问各零次、零连接。
16. **非 `Send` 短路如实失败**：`open_stream` 与 `connect_websocket` 各一例，detail 同时点名腿、钩子序号与「短路只在 `send` 生效」；零连接、`after_response` 零次。`open_sse` 另加一例：同一短路值只问一次、订阅以 `http-request-invalid` 收摊且**不重连**（`SseOptions::max_reconnects` 开着也不重连）。
17. **SSE 逐连重问**：脚本化对端首次发事件后断流一次，钩子 `before_request` 恰两次（初连 + 一次重连），两次 `leg` 都是 `ServerSentEvents`，两次改写都出现在各自的线上头里。
18. **流式腿不问收摊**：`open_stream` / `open_sse` / `connect_websocket` 各一例断言 `after_response` 零次、`before_request` 的 `leg` 逐字相符。
19. **收摊配对**：网络 200、新鲜缓存命中、短路成功三例各断言参与前缀的钩子 `after_response` 恰一次；短路点之后的钩子零次；被递归闸跳过的钩子零次；失败 / 取消 / 策略拒绝零次；把 `after_response` 也问在失败侧的变异必须转红。
20. **收摊视图**：网络应答与缓存命中的 `after_response` 视图为钩子改写后的逻辑原始请求；不含**库内补的** `If-None-Match` / `Accept-Encoding` / `Host` / 分帧字段（调用方自写的同名字段仍在）；跨 origin 重定向后 `url()` 仍是原始目标而非最后一跳；`leg()` 为 `Send`。
21. **`after_response` 异常隔离**：一枚钩子在收摊里抛异常 → 记一行 WARN（捕获 sink 断言不含 `what()` / URL / 头值）、应答照常交付、其后钩子的 `after_response` 仍被问到；调用方看到成功。
22. **`before_request` 异常**：抛异常 → `http-request-invalid`、零连接、链上后续钩子零调用、`after_response` 对谁都不问；detail 不含 `what()` / URL / 头值。
23. **递归闸（起手侧）**：一枚自反馈钩子在同一 `Client` 上再 `send` → 链上该钩子只跑一层、两条交换都成功交付、其余钩子两条都跑、WARN 恰一行；摘掉闸的变异必须以可见的深递归转红（而非静默通过）。
24. **递归闸（收摊侧）**：同一枚钩子在 `after_response` 里再 `send` → 嵌套交换跳过该钩子的两问、照常完成、WARN 恰一行；摘掉闸的变异同样转红。
25. **快照与句柄**：`before_request` 里注销本次链上另一枚钩子的句柄 → 本次仍把那枚问完、下一次不问；回调里新登记一枚 → 本次不问、下一次问；同一 `shared_ptr` 登记两次跑两遍；`use(nullptr)` 不产条目；句柄 `clear()` 后再析构不双重摘除、移动后源对象析构不摘除、链上其余各枚不受影响；所有 `Client` 先亡、句柄后析构不崩。把「快照」改成「每次现读链」的变异必须转红。
26. **空链恒等**：未登记任何钩子时，既有 `utest_net_http_client` / `utest_net_http_stream` / `utest_net_http_sse` / `utest_net_websocket` 全量用例逐值不变；请求字节与「挂载一枚恒答 `nullopt` 的钩子」的字节逐字节相同；空链路径不取锁、不分配（以链内计数与测试钩子零调用断言）。
27. **观测可辨**：`AURORA_ENABLE_DEBUG` 构建下，短路交付的条目零 mark、`from_cache=false`、状态码与 `received == body.size()`、`sent == 0`；真实交换有 mark；新鲜缓存命中 `from_cache=true`；本地拒绝有错误码且无状态码。四类互不混淆；公共结构体零字段变化。
28. **同名冲突以认证环为准**：钩子先注入 `Authorization: Bearer wrong`，对端回 401，`Authenticator` 交出 `Bearer right` → 上线的是 right 那一份、交换成功、调用方拿到 200；钩子写 `Authorization` 不在保留集合、不阻断认证环。把认证环的 `set_header` 改成「遇既有值即跳过」的变异必须转红。
29. **门禁全绿**：`check_api_schema_sync`（先 `--target aurora_api_json`）、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`check_umbrella_header`（新头入 `[direct]`）、`check_core_layer_boundary`、`check_arch_module_map`、`check_platform_macros`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_no_cjk_literals`、`check_no_hardcoded_paths`、`check_manual_test_format`、`check_version_consistency`、`format-check`；改动文件逐个过 `lint`；`ExchangeLeg` 入 known_enums.h 后 `itest_known_enums` 锚点随批。

**判据非空转（变异矩阵）**：至少八处变异自证并逐条记入规格小节末段——① 钩子挪到闸 #1 之前（第 3 项红）；② 摘掉逐钩子保留头检查（第 5 项红）；③ 流式腿静默忽略短路（第 16 项红）；④ `follow()` 不剥触点（第 9 项红）；⑤ 缓存三处不传触点（第 10 项红）；⑥ 摘掉递归闸（第 23、24 项红）；⑦ 短路赢过钩子后取消（第 14 项红）；⑧ 失败侧也问 `after_response`（第 19 项红）。再加三处：⑨ 去掉 `from_cache=true` 拒收（第 13 项红）；⑩ 快照改成现读链（第 25 项红）；⑪ 认证环不再覆盖钩子写入的 `Authorization`（第 28 项红）。闸 #2 与保留头终检**不在矩阵里**：它们是不可独立观测项，见第 4 项。

## 回写落点

1. `codespec/specification/06-app-platform.md`：新增 **§8.8.40**（§8.8.39 之后的下一枚空闲号）承载本件契约——公共面逐符号契约、四条腿落点次序表、保留头集合与逐钩子差分、触点差异跨 origin / 缓存口径、双闸的诚实边界、短路终点语义、递归闸的两条边界、异常 / 取消 / 错误码归属、`record_hook_delivery` 的观测形态，以及 §验收判据 末段的变异自证。同批改三处既有文字：§8.8.36 的「打标前后的拒绝分两档」补第三档「钩子短路交付」；§8.8.37 的条目覆盖补「钩子交付」；§8.8.19 末尾那段「待建」（它同时写着「`use(hook)` 仍未落地」与「§12 与 §17 此刻仍未落地的只剩 `use(hook)`」）改为全部落地并指向 §8.8.40。
2. `codespec/SPECIFICATIONS.md`：§5 表 `SPEC.FEAT.NET.HTTP-CLIENT.001` 行的能力清单补「交换钩子（客户端级登记链：只读请求视图 + 头改写 / `send` 上短路交付）」。
3. `codespec/changes/xplat-network-stack/proposal.md`：§13 那条「管道 hook」原句就地补一条 §16 追加裁决（第 47 项），写死七处定形（只读视图 + 独立头通道、双闸次序与初稿假设的纠正、保留头含 `X-Request-Id` 的反转、触点差异跨 origin 与缓存、四腿落点含 `open_stream_impl` 与非 `Send` 拒绝、短路终点与 `record_hook_delivery`、两问递归闸），并记明「改请求」收窄为「改请求头」；**原句编号与文字一律保留**（防伪死链）。同批把 §17 落地序与现状段里「仍未落地的只剩 `use(hook)`」收口为无剩余。
4. `codespec/CODING_STANDARDS.md`：§4 的日志 / 回调纪律补一条钩子契约（两问同步跑在发起侧续体所在线程、不得阻塞、不得做 I/O、不得抛、不得持有 `Client`、不得在非主线程直接触碰 UI），§15 反例速查补一条形态「钩子里做无界阻塞 I/O 或用同一枚 `Client` 自反馈上报」并给正解（换一枚不挂钩子的客户端、或把上报推迟到回调之外）。
5. `codespec/ARCHITECTURE.md`：§4 模块映射的 net 层随批补新头、库内链内核、触点差异比较器与 `record_hook_delivery` 各一行；§14 若涉及测试替身口径（`FakeEngine` / `ScriptedPeer` 那两处），补一句第三把武器与其边界（引擎级替身管传输事实，本件管是否触网与头部注入；短路不进缓存、不更新 Jar）。
6. `codespec/BUILD_OPTIONS.md`：判定**本件不改该文档**并在回写说明里记理由——无新宏、无新缓存变量、无按宏裁切的公共成员（`Client` 尺寸不随构建面变化）。
7. `codespec/errors.toml`：判定**无改动**（复用 `http-request-invalid` / `net-cancelled` / `net-policy-violated`），故生成的 `codespec/ERROR_CATALOG.md` 亦不动；本件对该判定负责——若落码时发现某一形态需要专属码，按规则 14 另批走并回到本文件补裁决。
8. `codespec/manual-test/23-net.md`：判定**不落手工用例**并登记理由——本件四腿落点、双闸、保留头与触点差异在零出站的回环夹具（`ScriptedPeer` / 假对端 / 纯内存 `Response`）上全覆盖，没有「必须出站」或「必须有宿主帧循环」的现场；若可观测性切片的既有 `TC-NET-0xx` 覆盖到钩子交付的 Inspector 面，由那件记。
9. 代码与数据面（不属 codespec 树，此处只记必做动作）：根目录 CHANGELOG.md 记新增公共面（一枚头 / 四枚类型（含枚举）/ 一枚方法）与 `Client` 多一枚私有共享内核、迁移面为零（纯新增）；根目录 aurora_api.json 经 `--target aurora_api_json` 重建；tools/include/known_enums.h 登记 `ExchangeLeg` 并同步 tests/integration/itest_known_enums.cpp 锚点；tools/check/umbrella_manifest.txt 经 `--write-manifest` 刷新。

## 落地收口

### 一 四处决策（由需求方定，规范性）

1. **钩子形态 = 只读视图 + 独立写通道**：`before_request` 拿 `const ExchangeRequestView &` 与 `Headers &` 两枚入参，方法、目标、体、`BodySource`、协议旋钮与超时一律不可达；`after_response` 拿视图 + `const Response &`。本稿 §变更内容 第 1 项的形态即按此定案落码，伞形提案 §13 的「改请求」自此收窄为「改请求头」。
2. **闸 / 钩次序 = 双闸**：闸 #1 先于链，链跑完先复检取消、再对最终请求复核 `gate_origin`。§动机 第 5 项对初稿假设的纠正按此兑现，取消复检排在短路采纳之前（判据第 14 项）。
3. **凭据头口径 = 触点差异集**：跨 origin 重定向剥离的不只 `Authorization` / `Cookie`，而是「钩子改过的头名」全集，`Authorization` / `Cookie` 恒在集合内；同一集合进缓存 `Vary`。
4. **短路记账 = 内部新增 delivery 记账**：`record_hook_delivery` 落在库内，公共结构体零字段变化；形态为零 mark、`from_cache=false`、状态码 + 实收体字节、`sent=0`。

### 二 落码面

新增 `include/aurora/net/http_hook.h`（185 行，四枚公共类型 + `ExchangeLeg`）、`src/aurora/net/detail/http_hook_chain.h`（122 行）与 `.cpp`（379 行，链内核 + 保留头表 + 触点差异比较器）、`src/aurora/net/http_hook.cpp`（61 行，视图与 `void_t` 检测面的落地）；`include/aurora/net/http_client.h` +47 行（`use` 与一枚私有共享成员）、`src/aurora/net/http_client.cpp` +213 行（四腿落点、`engage_exchange_hooks`、`record_hook_delivery`）、`src/aurora/net/detail/http_cache.{h,cpp}` +61/-… 行（三处 `extra_vary`）、`src/aurora/net/http_sse.cpp` 5 行（`open_stream_impl` 的逐连重问）。`Request` / `Response` 零改动、`include/aurora/net/http.h` 零改动。**无新增错误码、无新增构建宏、无按宏裁切的公共成员。**

### 三 判据兑现与三处偏差

判据 1–29 全部有对应用例，套件 `tests/unit/utest_net_http_hook.cpp`（2006 行 / 39 例，基线 39/39 通过，零出站：回环 `ScriptedPeer` + 假对端 + 纯内存 `Response`）。三处与本稿文字不同，均已回写规格、不留伪死链：

1. **判据 26 的「不取锁、不分配（以链内计数断言）」改为不为此伪造断言**。链内确实有一枚原子 `registered_` 作空链快路径，但那是实现事实而非可观测不变量——本件没有一枚「可数的锁计数」可断言，摘掉快路径后也没有一条用例能单点转红。规格 §8.8.40 按此口径改写，本件承担的判据是前半段：空链与「恒答 `nullopt` 的一枚钩子」请求字节逐字节相同、四套既有用例（`utest_net_http_client` / `_stream` / `_sse` / `utest_net_websocket`）全量复跑逐值不变。
2. **判据 28 的 `Authorization` 归属**：落码确认 `Authorization` **不在**保留头集合（保留集合是 13 枚共享 + 4 枚 WebSocket，见 §8.8.40），认证环以 `set_header` 覆盖钩子写入的同名值——与本稿一致，只是「反转」发生在 `X-Request-Id` 那枚（它进保留集合）。伞形提案 §16 第 47 项③ 已按此记。
3. **判据 29 的 `lint` / `format-check` 不在 `ctest --preset ninja-test` 覆盖内**，本批单独跑并单独记（见本节末「门禁」）。另：新头与新源文件须先 `git add` 才进 `format-check` 的文件枚举，本批已按此处理。

### 四 变异自证（11 行，逐行落 §8.8.40 末段）

| 变异 | 结果 |
|:---|:---|
| ① 钩子挪到闸 #1 之前 | 只让 `a_policy_refused_target_is_never_offered_to_the_hooks` 转红 |
| ② 摘掉逐钩子保留头检查 | 只让 `a_hook_cannot_touch_a_reserved_header_on_the_send_leg` 转红 |
| ③ 流式腿静默忽略短路 | 流式 + 订阅两条转红；`open_sse` 那条单跑亦红，全量跑时进程在变异体自身的重连环里崩溃 |
| ④ `follow()` 不剥触点 | 只让 `a_hook_touched_header_does_not_cross_the_origin_boundary` 转红 |
| ⑤ 缓存三处不传触点 | 只让 `a_hook_touched_header_is_part_of_the_cache_key` 转红 |
| ⑥ 摘掉递归闸 | 两枚嵌套用例以 `STATUS_STACK_OVERFLOW`（`0xC00000FD`）中止、邻例照常绿 |
| ⑦ 短路赢过钩子后取消 | 只让 `cancelling_from_a_request_hook_beats_the_short_circuit` 转红 |
| ⑧ 失败侧也问收摊 | 只让 `the_response_hook_is_asked_once_per_delivered_response` 转红 |
| ⑨ 去掉伪造命中拒收 | 只让 `a_hook_may_not_forge_a_cache_hit` 转红 |
| ⑩ 快照改成现读链 | 只让 `deregistering_another_hook_mid_exchange_does_not_cut_it_out` 转红 |
| ⑪ 认证环不再覆盖 | 只让 `the_authenticator_answers_override_a_hook_written_credential` 转红 |

三处如实记录：③ 的崩溃是变异产物（摘掉拒绝后那条腿真的开始触网并自连），不是判据的空转；⑥ 的转红形态是「看得见的深递归」而非一条干净的红，正是判据 23 / 24 所要求的形态（静默通过才是问题）；⑧ 起初写作 `Response{}` 默认构造导致编译失败——`Response` 无默认构造，改为四参显式构造后该行成立。闸 #2 与链尾保留头终检按判据 4 排除在矩阵之外（不可独立观测项），不冒充成一行绿。

### 五 门禁（判据 29 的实跑口径）

- `ctest --preset ninja-test`：**438/438 全绿**（含全部 `check_*` 静态门禁与本件新登记的 `utest_net_http_hook`）。
- `format-check`：1173 枚第一方源文件全部合规（首轮 4 枚非合规，为本批四份文件，已就地格式化；`git add` 之后复跑确认新文件进得了枚举）。
- `lint`（clang-tidy，按改动面 `--include` 覆盖 10 枚相关 TU）：**首轮 31 条 finding 全在本批新增文件内，逐条清零后复跑 0 条**。清零形态记此以免后来者误读：保留头表改指派初始化列表（17 处）、两处数组迭代器补 `*const`（`readability-qualified-auto`）、`ExchangeRequestView` 补默认析构以满足 rule-of-five 一致性、`run_after` 的 `noexcept` 按库侧既有口径逐点豁免（该规格是「收摊问不得把已成功的交换反转为失败」这一契约，体内唯一逃逸是内存耗尽，与流式传输派发边界同族）、`http_hook.cpp` 的空缺省实现按 `/*request*/` 形态命名形参、测试替身的八枚公开配置字段去掉私有成员后缀（公开字段带 `_` 反而违例，就地写明理由）、一枚 `AURORA_TEST_REQUIRE` 之后的 `optional::value()` 补跨不过宏体的豁免。
- `check_doc_comments` 首轮为红：`ExchangeHookRegistration` 的析构、移动构造与移动赋值三枚特殊成员缺注释；补齐后即绿（`= default` 与 `= delete` 两枚不在该门禁的检出口径内，未伪补）。

### 六 状态

判据 1–29 全部兑现、变异矩阵十一行与门禁三项均有实跑数字，本件自此进入 **已归档**：代码、测试、数据面与 `codespec/` 回写同批，无预留项、无「计划/待建」残留。
