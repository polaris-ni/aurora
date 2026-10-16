#pragma once

/// @brief HTTP 认证（CHG-018 §12 语义件的认证切片：401 / 407 挑战驱动的补凭据与重发）。
/// @file net/http_auth.h
///
/// 本头回答一个问题：**「对端要凭据的时候，凭据从哪儿来」**。它不是一条新的发送路径，而是发起侧在拿到
/// 401 / 407 之后插入的一环：把对端说的那句挑战交回给宿主的凭据提供方，据其答复重发一次同一请求。形态
/// 是可继承抽象类 + 协程出口，而不是「先把 username / password 填进某个池子、由库自行组装」——后者能覆盖
/// 的只有 Basic，而现实里的凭据十有八九要去另一台机器换（OAuth2 的 token 端点、STS、企业单点登录），一次
/// 换取本身就是一次 `send`。取 token 的动作要么能写成 `co_await`，要么就得逼调用方在回调里手搓状态机；
/// 后者不作数，故出口是协程。
///
/// 五条形态裁决（规范性，落码与文档回写以此为准）：
/// - **挂载只有一级：`Client::authenticator()`**。与 `retry_policy()` 的「请求覆盖 → 客户端缺省」两级格局
///   刻意不同：一次换取的凭据天然跨请求共享（一份 token 服务整段时间内的所有交换），而单条请求的一次性
///   凭据早已由 `Request` 上直填 `Authorization` 覆盖——再加一枚逐请求旋钮只会多出「两处都能设、以谁为准」
///   这类无从检查的冲突。
/// - **三环次序：策略环 → 认证环 → 重试环之内层**。一次尝试拿到 401 / 407，认证环先问一次提供方，问到
///   凭据就把这次尝试**原地重跑**，不消耗 `RetryPolicy::attempts` 名额——那枚旋钮管的是「服务暂时不行，
///   再打几次」，把「没带凭据」算成一次失败会让开了重试的调用方在四次尝试里永远学不会怎么带凭据。
///   **一次交换至多注入一次**：带着新凭据重跑后又吃一次 401，说明这份凭据不对，交出的就是那一份 401，
///   不再问第二遍（挑战-重发的死循环是这一类实现最典型的故障形态）。提供方自身失败（换 token 没换成）
///   即把那份错误原样上抛，本库不拿一份注定被拒的凭据去重发。
/// - **只内置 `Basic` 与 `Bearer`**：`Digest` 与 `NTLM` 维持非目标（要引入哈希链与会话状态机，其正确性
///   无法在零真实域控的条件下证明）。但挑战的**原文完整交给实现方**（方案名、`realm` 与全部参数、被挑战
///   的方法与目标），故一枚自定义 `Authenticator` 可以自己实现任何一种方案——本库放弃的是内置实现，不是
///   扩展口。只给 `Digest` 挑战时内置件仍被问一次，答空即原样交出那份 401。
/// - **代理挑战与源站挑战同一条路、以 `AuthChallenge::proxy` 区分**：HTTP 代理的两条腿都进本环——`CONNECT`
///   被 407 拒（隧道未成）与被转发的明文请求拿回 407 都算。答复里的字段按挑战类型落位：源站挑战补
///   `Authorization`，代理挑战补 `Proxy-Authorization`，且**代理答复只写到面向代理的那一侧**（隧道里的请求
///   绝不带它，否则代理的凭据会被送到源站）。SOCKS5 不在本环内：它的子协商没有「挑战-答复」这套报文形态
///   （只有用户名 / 密码与一个状态字节），凭据只能预防性地给出，被拒仍落 `net-proxy-auth-required`。
/// - **凭据纪律**：明文（`http://`）目标带上 `Authorization` / `Cookie` 时，除非该主机已在
///   `NetPolicy::allow_cleartext_hosts` 的例外清单里，一律在起手就落 `net-policy-violated`、一个字节都不写出
///   （那一维的判据见 `net/policy.h`，本切片把它钉成用例而不是另加一条永远走不到的分支）；重定向换了 origin
///   （方案 / 主机 / 有效端口任一不同）时这两枚字段**剥掉不带**；同一份凭据在缓存表里恒参与建键（不依赖
///   应答的 `Vary`），不同凭据读不到彼此的副本。
///
/// 答复的落地形态是「一组要补进请求的字段」而不是「一枚凭据字符串」：字段名由实现方决定，本库只按挑战
/// 类型校验落位（代理答复只认 `Proxy-Authorization`，其余字段在代理挑战上视为没答复），同名以答复为准覆盖
/// 请求上已有的那枚。空集合就是「放弃」——不需要额外的布尔旋钮，交出对端说的那份 401 / 407 是唯一动作。
///
/// `Basic` 的字段值由 `basic_credentials()` 组装：`user ":" password` 按 **UTF-8** 组串后 base64
/// （`RFC 7617 §2.1` 的缺省字符集），不做百分号编码也不做 Unicode 归一——那两步任一做了都会与对端的
/// 逐字节比对分岔，且现场表现为「口令没错却被反复拒」。
///
/// @note 协程出口的推进节奏：`CoroTask` 不是 awaitable（`state/coroutine.h`：promise 永不首挂也不终挂，
///       结果只经 `is_done()` / `result()` 轮询），故发起侧以宿主对外层任务同一套轮询节奏推进这一环，
///       并在两次轮询之间让出协程（`Timeouts` 各档与取消令牌在此期间照常生效）。内置件从不挂起，
///       调用点当场即已完成，一次轮询都不多花。
/// @note Thread: 实现方须自身线程安全——同一条 `Client` 的并发交换共用一个实例（`Client` 的拷贝共享同一
///       提供方，与缓存表同一格局），`authenticate()` 可能在同一时刻被多次调用
/// @note Side-effects: 由实现方决定（一次 token 换取即一次网络 I/O）；本头声明的类型本身无副作用

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/net/http.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::http {

/// @brief 挑战点名的认证方案档位：内置件据此选择答复，未列名的方案统一归 `Unsupported`。
/// @note Thread: thread-safe
/// @note Side-effects: none
/// @note 豁免 performance-enum-size：公共 API 枚举，落在 `AuthMechanism` 的单个字段而非密集数组；
/// 底层类型属公共 API 形态的一部分。
/// NOLINTNEXTLINE(performance-enum-size)
enum class AuthScheme {
    Basic,  ///< `Basic`（`RFC 7617`）：base64 的用户名 / 口令
    Bearer,  ///< `Bearer`（`RFC 6750 §3`）：不透明令牌
    Unsupported,  ///< 本库不实现的方案；原文拼写见 `AuthMechanism::name`，自定义实现方可据它自行处理
};

/// @brief 挑战里的一条方案及其认证参数（`WWW-Authenticate` / `Proxy-Authenticate` 的一项）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct AuthMechanism {
    AuthScheme scheme = AuthScheme::Unsupported;  ///< 归一后的档位；未列名者为 `Unsupported`
    std::string name;  ///< 线上原文的方案名（保留对端的拼写大小写，仅 `Unsupported` 时用得上）
    /// @brief `realm` 参数原文；对端没给即空串。
    ///
    /// 单列而不留在参数表里：它是「这份凭据管不管用」的唯一线索，每条挑战的实现方都要读它。
    std::string realm;
    /// @brief 其余认证参数：值保留原文，按线上次序排列。
    ///
    /// 键由文法层归成 ASCII 小写，手工装配时可不归一（`parameter()` 两侧都做折叠）。
    std::vector<std::pair<std::string, std::string>> parameters;

    /// @brief 按名取一枚认证参数（两侧都做 ASCII 大小写折叠，与字段名的比较口径一致）。
    ///
    /// 折叠查询侧是因为文法层已把登记键归成小写；折叠登记侧是因为自定义实现方会手工装配机制，那时尚不能
    /// 要求它记得归一——只折一侧的现场表现是「参数明明在列表里，`parameter()` 却说没有」。
    /// @param key 参数名。
    /// @return 首个同名参数的值；没有该参数时 `nullopt`（`realm` 不在这里，看 `realm` 字段）。
    [[nodiscard]] auto parameter(std::string_view key) const -> std::optional<std::string>;
};

/// @brief 一次认证挑战：谁在问、问的是哪一跳、给了哪几种方案。
///
/// 挑战的来源有三处，本类型把它们收敛成同一份事实：源站的 401、被转发请求拿回的 407、以及 `CONNECT`
/// 被拒时的 407。前三枚字段就是「谁在问」，`mechanisms` 是「怎么答」的全部依据。
/// @note Thread: thread-safe（值类型；实现方在 `authenticate()` 期间只读）
/// @note Side-effects: none
struct AuthChallenge {
    bool proxy{false};  ///< 真是代理在问（407：`CONNECT` 被拒或被转发的请求拿回 407）；假是目标源站（401）
    std::uint16_t status{0};  ///< 触发本次挑战的状态码（401 / 407），原样给出以免实现方再猜
    std::string host;  ///< 被挑战那一侧的主机文本：源站挑战是目标主机，代理挑战是**代理**主机
    Method method = Method::Get;  ///< 被拒的那次请求的方法（自定义方案常要按方法算摘要）
    std::string url;  ///< 被拒的那次请求的目标 URL 文本（同上：算摘要与选凭据都要它）
    std::vector<AuthMechanism> mechanisms;  ///< 对端给出的方案，按线上次序；一条都没给出时为空

    /// @brief 按档位取对端给出的那一枚方案。
    /// @param scheme 目标档位。
    /// @return 命中时指向 `mechanisms` 里那一条（借用，存活期即本次 `authenticate()` 调用）；否则 `nullptr`。
    [[nodiscard]] auto mechanism(AuthScheme scheme) const -> const AuthMechanism *;
};

/// @brief 认证方的答复：要补进重发请求的字段；空集合即「放弃」。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct AuthDecision {
    /// @brief 追加进重发请求的字段；同名以本集合为准（覆盖请求上已有的那枚）。
    ///
    /// 源站挑战上全部字段都写；代理挑战上只有 `Proxy-Authorization` 生效，其余字段视为没答复。
    aurora::Headers headers;

    /// @brief 是否是「放弃」这一次挑战。
    /// @return 没有任何字段时为 true。
    [[nodiscard]] auto empty() const -> bool { return headers.empty(); }
};

/// @brief 凭据提供方：拿到挑战、给出要补的字段（可继承抽象类，协程出口）。
///
/// 内置件覆盖 `Basic` / `Bearer` 两枚方案与「只答被点名的那一枚」这条纪律；要换 token、要按 `realm`
/// 分池、要实现 `Digest`，都派生本类型自行决定答复。派生类的协程体里可以直接 `co_await` 一次 `send`
/// 去取凭据——这正是出口取协程形态的理由。
/// @note Thread: 实现须自身线程安全（同一条 `Client` 的并发交换共用一个实例）
/// @note Side-effects: 由实现决定，常见形态是一次网络 I/O
class Authenticator {
  public:
    Authenticator() = default;
    Authenticator(const Authenticator &) = delete;
    auto operator=(const Authenticator &) -> Authenticator & = delete;
    Authenticator(Authenticator &&) = delete;
    auto operator=(Authenticator &&) -> Authenticator & = delete;
    virtual ~Authenticator() = default;

    /// @brief 答一次挑战（纯虚：一次交换里没有默认答复这回事，「什么都没准备」与「看过之后决定不答」
    ///        必须由实现方自己说清）。
    ///
    /// 成功携要补的字段（空集合 = 放弃，本库原样交出那份 401 / 407）；失败携实现方自己的错误——那次错误
    /// 就是本次交换的结局，本库不拿一份没到手的凭据去重发。
    ///
    /// **引用的存活期契约**（规范性）：`challenge` 及其内部的 `mechanism()` 借用只活到本函数返回的那个
    /// `CoroTask` 收尾为止——即「实现方的协程体跑完」，而不是「`authenticate()` 返回」。协程体若在
    /// `co_await` 之后还要读挑战，必须先把需要的字段**复制**进自己的局部量（一次 token 换取通常正是这样：
    /// 换取前记下 `realm` 与目标主机，回来后凭这两份副本决定往哪投）。本库一侧确实把挑战放在交换作用域上、
    /// 活到答复落地为止，但那只兜住「提供方被放弃时帧仍引用它」这一种形态，不构成实现方可以跨挂起点
    /// 直接引用挑战的承诺。
    /// @param challenge 对端说出的挑战（发起侧在本次交换的现场装配，调用期间只读；存活期见上）。
    /// @return 协程任务：成功携 `AuthDecision`，失败携对应错误码。
    [[nodiscard]] virtual auto authenticate(const AuthChallenge &challenge) const -> CoroTask<Result<AuthDecision>> = 0;
};

/// @brief 用户名 / 口令的 `Basic` 提供方：只在挑战点名 `Basic` 时答复。
///
/// 凭据以 `RFC 7617 §2.1` 的 UTF-8 形态组装（见 `basic_credentials()`）。`realm` 不参与选择——一份口令
/// 对多个 realm 复用是真实存在的用法，要按 `realm` 分池请派生 `Authenticator` 自行判定。
/// @note Thread: thread-safe（构造后只读）
/// @note Side-effects: none
class BasicAuthenticator final : public Authenticator {
  public:
    /// @brief 以一组凭据构造。
    /// @param user 用户名（UTF-8 文本，原样参与组串，不做百分号编码）。
    /// @param password 口令（UTF-8 文本，同上）。
    BasicAuthenticator(std::string user, std::string password);

    /// @brief 挑战给了 `Basic` 时答一枚 `Authorization` / `Proxy-Authorization`，否则答空。
    /// @param challenge 对端说出的挑战。
    /// @return 协程任务：恒成功，载荷是答复或空的「放弃」。
    [[nodiscard]] auto authenticate(const AuthChallenge &challenge) const -> CoroTask<Result<AuthDecision>> override;

  private:
    std::string user_;  ///< 用户名
    std::string password_;  ///< 口令
};

/// @brief 令牌的 `Bearer` 提供方：只在挑战点名 `Bearer` 时答复。
///
/// 令牌是构造时给定的静态值——会过期、要刷新的令牌属于「取凭据本身是一次交换」那类场景，请派生
/// `Authenticator` 在协程体里 `co_await` 换取口。
/// @note Thread: thread-safe（构造后只读）
/// @note Side-effects: none
class BearerAuthenticator final : public Authenticator {
  public:
    /// @brief 以一枚令牌构造。
    /// @param token 令牌文本（`RFC 6750 §2.1` 的 `b64token` 形态；本库不校验其形状，原样写上线）。
    explicit BearerAuthenticator(std::string token);

    /// @brief 挑战给了 `Bearer` 时答一枚 `Authorization` / `Proxy-Authorization`，否则答空。
    /// @param challenge 对端说出的挑战。
    /// @return 协程任务：恒成功，载荷是答复或空的「放弃」。
    [[nodiscard]] auto authenticate(const AuthChallenge &challenge) const -> CoroTask<Result<AuthDecision>> override;

  private:
    std::string token_;  ///< 令牌文本
};

/// @brief 组装 `Basic` 方案的字段值：`"Basic " + base64(user ":" password)`。
///
/// 组串按 UTF-8 逐字节，不做百分号编码、不做归一（`RFC 7617 §2.1` 的缺省字符集即 UTF-8）。它是内置件与
/// 自定义实现方共用的唯一组装口：自己拼这串时最容易悄悄丢掉「先连串再编码」那一步，而现场只是一句
/// 「口令不对」。
/// @param user 用户名。
/// @param password 口令。
/// @return 可直接写进 `Authorization` / `Proxy-Authorization` 的字段值。
[[nodiscard]] auto basic_credentials(std::string_view user, std::string_view password) -> std::string;

}  // namespace aurora::net::http
