#pragma once

/// @brief HTTP/1.1 客户端的请求 / 应答值类型、方法令牌与失败分型。
/// @file net/http.h
///
/// 本头只回答一个问题：**「一次 HTTP 交换的输入、输出与失败长什么样」**——方法是哪个、目标 URL、
/// 请求字段与消息体、一份已物化的应答、以及一枚失败该往哪儿归因。它同样是纯值类型：不含句柄、不含
/// socket、不含任何平台头，可在任意线程拷贝与比较。真正的「连上去、把 `render_head()` 写出去、把应答
/// 读回来」属发起侧（`net::http::Client`，建在传输与 TLS 之上），不在这里。
///
/// 三条设计约束值得在写代码前知道：
///
/// - **字符串只能经显式解析进入**：`Request` 的工厂收一段 URL 文本并当场 `Url::parse`，解析
///   失败不静默退化成「空 URL 直接发」——它被记在请求对象里，到 `render_head()` 才成为错误。
///   这一条与 §12「手拼 URL 恰是 AI 生成代码的头号错误源」同源：让错误在最早可确定的地方确定。
/// - **方法令牌与线上文本一一对应**：`method_text()` 给出大写线上形态（`GET` / `POST` …），
///   `method_from_text()` 是其逆且只认规范拼写（大小写敏感，`RFC 9110 §9`）；未知拼写报错而不是
///   当成「某种自定义方法」——自定义方法本批次不提供，静默接受只会让 `Content-Length` 的取舍失据。
/// - **`HEAD` 的请求恒不携带消息体、其应答恒无体**：`body()` 对 `HEAD` 请求无效，应答分帧由
///   `wire::framing_of(head, request_is_head)` 判定，本类型只提供 `is_head()` 这一枚事实来源。
/// - **消息体有两种交法，三枚协议旋钮不进字段表**：体可以是一份已在内存的字节
///   （`body(std::vector<char>)`），也可以是一枚按需推进的 `BodySource`——大文件与 multipart 惰性部件
///   靠后者才不必先整份读进内存。`Range` / `If-Range` / `Expect` 由专用旋钮表达，只在 `render_head()`
///   现地补进线上字段，故 `headers()` 恒看不到它们：发起侧要据此判定线上形态、重放资格与是否先等一个
///   `1xx`，把这些判定输入留在字段表之外才会多出「读 `headers()` 以为这次没分段」的第二份真值源。
///
/// @code
/// auto req = aurora::net::http::Request::get("https://example.com/api?q=1");
/// req.header("Accept", "application/json").timeout(std::chrono::seconds{5});
/// const auto head = req.render_head();  // Result<std::string>：请求行 + 字段（不含体）
/// @endcode
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"

namespace aurora::net::http {

/// @brief HTTP 请求方法；首批只列本库发起侧支持的同名工厂对应的六枚。
/// @note Thread: thread-safe
/// @note Side-effects: none
/// @note 豁免 performance-enum-size：公共 API 枚举，落在 `Request` 的单个字段而非密集数组；
/// 底层类型属公共 API 形态的一部分。
/// NOLINTNEXTLINE(performance-enum-size)
enum class Method {
    Get,  ///< `GET`：读取资源，应答有体
    Head,  ///< `HEAD`：只取头部块，应答恒无体
    Post,  ///< `POST`：提交，请求可带体
    Put,  ///< `PUT`：整体替换，请求可带体
    Patch,  ///< `PATCH`：部分修改，请求可带体
    Delete,  ///< `DELETE`：删除，请求一般无体但允许带
};

/// @brief 方法的线上文本（大写、`RFC 9110 §9` 的规范拼写）。
/// @param method 方法枚举。
/// @return 恒为非空、纯大写的短令牌（`"GET"` / `"HEAD"` / `"POST"` / `"PUT"` / `"PATCH"` / `"DELETE"`）。
[[nodiscard]] auto method_text(Method method) -> std::string_view;

/// @brief 由线上文本解析方法（`method_text()` 的逆）。
///
/// 只认上表六枚的规范大写拼写；其余（含小写与自定义方法）一律失败，错误码 `http-request-invalid`。
/// @param text 方法文本。
/// @return 成功为对应枚举；失败为 `http-request-invalid`。
[[nodiscard]] auto method_from_text(std::string_view text) -> Result<Method>;

/// @brief HTTP 请求的三档超时；各档独立，`nullopt` 或非正值 = 该档不设时限。
///
/// 单一时限无法分别表达「连不上」（`connect`）与「Body 慢」（`read_idle`），故拆三档：`connect` 覆盖建连
/// （含 TLS 握手），`read_idle` 是相邻两次交付之间的空闲上限（滑动，每读到字节即重置），`total` 是整请求
/// 时限（跨连接 / 读写 / 全部重定向）。任一档到点，发起侧一律落 `http-timeout`，`message` 的 detail 标明
/// 是哪一档，使「查服务起没起」与「查 Body 卡在哪」不至混为一谈。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct Timeouts {
    std::optional<std::chrono::milliseconds> connect;  ///< 建连（含 TLS 握手）时限
    std::optional<std::chrono::milliseconds> read_idle;  ///< 相邻两次读之间的空闲时限
    std::optional<std::chrono::milliseconds> total;  ///< 整请求时限
};

/// @brief 一次交换失败后要不要再打一遍、隔多久打；**缺省关闭**。
///
/// 本类型只表达「策略」，不表达「这次到底重不重试」——后者由三件既有事实共同决定，本库不自造第四份判据：
/// - **方法是否幂等**：只有 `GET` / `HEAD` / `PUT` / `DELETE` 会被重放。重放一条非幂等请求可能把一次
///   提交变成两次，而这个代价在失败现场无从判定（响应没收到，恰说明服务端可能已经做完了），故不由策略
///   旋钮放开。
/// - **失败的归因档**：`error_class_of()` 落在 `Dns` / `Unreachable` / `Timeout` 三档之一，且该码在
///   `codespec/errors.toml` 里 `retryable` 为真，才允许重放。`net-cancelled` 虽标可重试，但它落在
///   `Cancelled` 档、且重放用户主动取消的请求等于违抗那次取消，故被同一道闸挡住；`Protocol` / `Status` /
///   `Invalid` 档同理不重放（报文畸形与用法的修复动作都不在「再打一遍」上）。
/// - **应答状态码**：非 2xx 是成功的 `Result`（状态是数据不是错误），故这条分岔不看错误码，只看
///   `error_class_of_status()` 给出的 `Status` 档里的**瞬时**那一族：408 / 429 / 500 / 502 / 503 / 504。
///   这类重试耗尽时把最后那一次应答原样交出，绝不折成错误——调用方要看的是服务器怎么说。
///
/// 两枚时间旋钮各管一段：`total` 档（`Timeouts`）是**每一次尝试**的墙，重试的每次尝试重新拿一份；跨全部
/// 尝试（含退避等待）的墙由本类型的 `budget` 表达，到点即停、剩余尝试不再发起。二者分开是因为「这条连接
/// 慢」与「这次重试序列拖太久」是两件事，合成一枚会让 attempts 与 base 的乘积关系隐式决定总时长。
///
/// 越界形态（`base` 为负、`max_backoff` 小于 `base`、`jitter` 不在 `[0, 1]`）在发起起手时即落
/// `http-request-invalid` 并在 detail 点名是哪一枚，不静默折算——「以为开了抖动其实没开」要等到线上重试
/// 风暴才现形，那时现场已经没有配置信息。`budget` 非正值与不设等价（与 `Timeouts` 各档同口径）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct RetryPolicy {
    /// @brief 总尝试数上限，**含首次**；`1`（缺省）即关闭重试，`0` 与 `1` 同义。
    ///
    /// 本库不给它设安全阀：真正的墙是 `budget` 与每尝试的 `total`，而「打多少次」是使用方的决定。
    std::uint32_t attempts{1};
    /// @brief 首次退避的基准时长（第 n 次退避以 `base * 2^(n-1)` 增长，再取抖动与上限）。
    ///
    /// 非正值合法，意为「不等待，立刻再打」（用于本地回环一类的即时重试场景）。
    std::chrono::milliseconds base{std::chrono::milliseconds{200}};
    /// @brief 单次等待的上限，**同时**充当指数退避的封顶与 `Retry-After` 的封顶。
    ///
    /// 合为一枚是有意的：两条路都在回答「这次最多愿意等多久」，分成两枚旋钮会让「对端要求等 10 分钟」与
    /// 「我方退避上限」互相矛盾却无人裁决。越上限即按上限等待，随后照常发起下一次尝试。
    std::chrono::milliseconds max_backoff{std::chrono::milliseconds{30000}};
    /// @brief 抖动幅度，合法域 `[0, 1]`：本次等待在 `[算得值, 算得值 * (1 + jitter)]` 内取值。
    ///
    /// `0` = 完全确定（测试与可复现诊断用），缺省 `0.2` 足以打散同一时刻整批发起的重试，又不至于让最坏
    /// 等待翻倍。抖动只负责打散节奏，故取值来自库内置的非密码学序列——它不需要也不可预测性可言。
    double jitter{0.2};
    /// @brief 跨全部尝试（含退避等待）的总墙；`nullopt` 或非正值 = 不设，仅由 `attempts` 收敛。
    std::optional<std::chrono::milliseconds> budget;
};

/// @brief 内存缓存的开关与容量；**缺省关闭**。
///
/// 缺省关是刻意的：「同一 URL 的第二次请求不触网」改变的是可观测时序，挂着同一 `Client` 的调用方若不知道
/// 缓存存在，会以为每次拿到的都是对端此刻的说法。开启后兑现的是 `RFC 9111` 的最小集——新鲜副本直接交付、
/// 陈旧副本挂 `ETag` / `If-None-Match` 或 `Last-Modified` / `If-Modified-Since` 做条件请求、304 用新头更新
/// 已存应答再交出、`no-store` 强制 bypass、`must-revalidate` 过期后绝不交付。
///
/// `max_entries` 管的是**条数**而非单条应答的大小：体量上限另有 `Client::max_body_bytes()` 与
/// `Client::max_decompressed_bytes()` 两枚旋钮管字节，本库不用一份容量口径管两件事。越界时逐出最久未使用的
/// 那条；`0` 与「不存任何东西」同义（保留 `enabled` 作可读开关，语义上两者都关）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct CacheOptions {
    bool enabled{false};  ///< 开关；假 = 既不查也不写
    std::uint32_t max_entries{64};  ///< 表的条数上限；越界逐出最久未使用的那条
};

/// @brief 连接池的开关与治理口径；**缺省关闭**。
///
/// 缺省关是刻意的：连接复用改变的是「一次 `send` = 一条连接」这条已写进发起侧头注释的生命周期口径，开着时
/// 第二次同名请求可能不再建连、而是坐上上一条还温着的连接——不知道池存在的调用方会把时序差异当成对端行为
/// 变化。开启后兑现的是 keep-alive 的最小治理集：按本表五判据复用（消息完整读完、无 leftover、对端未发
/// `Connection: close`、分帧非 `UntilClose`、HTTP/1.1），空闲超时与最长寿命到期即关，每主机并发上限满时
/// 排队（等空闲连接的时间计入 `Timeouts::connect` 档，到点在 detail 点名「排队等不到」）。
///
/// `max_connections_per_host` 管的是**同一池键**（方案 + 对端主机 + 端口，含经代理的形态差异）上**同时
/// 存在**的连接数（在途 + 空闲合计）；`0` 与「不设上限」同义。`idle_timeout` 与 `max_age` 各管一段：
/// 前者管「空闲多久就关」（对端随时可能关掉它那边，闲置连接是消耗品），后者管「这条连接出生多久就关」
/// （防 DNS 漂移之后仍在连旧地址）。二者的淘汰在每次取用与归还时顺手执行，不为它单起定时器。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct PoolOptions {
    bool enabled{false};  ///< 开关；假 = 一次 `send` / `open_stream` 一条连接，用完即关（既有口径）
    std::uint32_t max_connections_per_host{6};  ///< 同一池键上同时存在的连接上限（在途 + 空闲）；`0` = 不设上限
    std::chrono::milliseconds idle_timeout{std::chrono::milliseconds{30000}};  ///< 空闲多久即关；非正值 = 不淘汰
    std::chrono::milliseconds max_age{std::chrono::milliseconds{300000}};  ///< 连接最长寿命；非正值 = 不淘汰
};

/// @brief 事件流订阅的重连口径；**缺省开启自动重连**。
///
/// 缺省开与 `PoolOptions` / `CacheOptions` 的「缺省关」相反，这是刻意的：那两件改变的是**已存在**的交换何时
/// 不再发生（不触网、复用连接），调用方不知道也能拿到正确的结果；而事件流是一条**没有结束**的流——关掉
/// 重连等于把「对端重启、网络抖一下」这一类最常见的现实变成宿主必须自己接的活，而续传标识（`Last-Event-ID`）
/// 恰恰只有库知道该在什么时刻带上。
///
/// **窗口只有一个来源**：服务端写的 `retry:` 字段原样采用，不封顶、不指数退避、不加抖动。理由是把
/// 「这条流多久之后该重连」这件事交给服务端裁决——它是唯一知道自己为什么断了的一方；而「本库最多替宿主
/// 试几次」是本地决定，由 `max_reconnects` 收敛，不用时间旋钮拐弯表达。服务端从未写 `retry:` 时取
/// `retry_default` 作初值。
///
/// `Timeouts` 的三档在这里管的是**单条流**而不是整个订阅（每（重）建一条流各拿一份），会话的收口靠
/// `Cancellation` 与 `max_reconnects`：一个永不结束的订阅需要一个能被取消的句柄，而不是一个迟早会误伤长连接的
/// 总时长墙。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct SseOptions {
    bool reconnect{true};  ///< 开关；假 = 一条流收摊即结束本次订阅（成功结局，不重连）
    /// @brief 重连次数上限，**不含首次**；缺省 8，`0` = 不设上限（靠取消令牌收摊）。
    ///
    /// 额度用尽时交出的是**最后一次交换的真实失败**（detail 点名额度已尽），而不是另立一枚「额度码」：
    /// 「对端 502」与「对端 502 且本库不再尝试」的修复动作完全相同，拆开只会多一枚无人读的码。最后一次交换
    /// 本身是干净收摊的那种（服务端每次都把流写完然后关掉）算成功结局，由 `SseOutcome::reconnect_exhausted`
    /// 表达「我停了，不是因为服务端说完了」。
    std::uint32_t max_reconnects{8};
    /// @brief 服务端从未写 `retry:` 时的重连窗口初值；非正值 = 不等待，立刻再打。
    ///
    /// 一旦某条流写过 `retry:`，此后各条流都用那个值（与 DOM 的 `EventSource` 同口径），本字段不再参与。
    std::chrono::milliseconds retry_default{std::chrono::milliseconds{3000}};
};

/// @brief 进度所属方向；一次交换里两侧各报一段，回调需据此分流。
///
/// 上传在请求体被传输层接受时推进，下载在应答体到达时推进。二者共用一枚回调是刻意的：调用方通常只关心
/// 「这件事进行到哪了」，而方向是载荷自带的属性，不该逼调用方挂两个口。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class ProgressPhase : std::uint8_t {
    Upload,  ///< 请求侧：已交出的消息体字节（含请求头部之外的部分）
    Download,  ///< 应答侧：已到达的消息体线上字节（透明解压的还原量不计入，见 `Progress`）
};

/// @brief 一次进度发射所携带的事实（合流口径见 `Request::on_progress()`）。
///
/// 字节口径只有一处需要留意：下载侧计的是**分帧解出后的线上字节**，不是透明解压之后的字节。透明解压在
/// 物化路径上发生在收完之后，中途无法给出还原量；流式路径若要拿「实际交出去的还原字节」，看该路径自己的
/// 完成事实而非进度值。
/// @note Thread: 回调内只读（值随每次发射新建）
/// @note Side-effects: none
struct Progress {
    ProgressPhase phase{ProgressPhase::Upload};  ///< 方向
    std::uint64_t transferred_bytes{0};  ///< 该方向已累计交付的字节数；重定向的每一跳各自重新计数
    std::uint64_t total_bytes{0};  ///< 该方向总量；`total_known` 为假时无意义
    bool total_known{false};  ///< 总量是否已知（上传恒真；下载取决于分帧是否给出长度）
    bool finished{false};  ///< 该方向是否已收摊（成功与失败都算收摊；成败由发起侧的出口值表达，不由本字段表达）
};

/// @brief 进度回调：在发起侧的发射时机被调用（线程与频次约束见 `Request::on_progress()`）。
/// @note Thread: 恒在主线程同步调用（无主线程投递器时即在承接线程）
/// @note Side-effects: 由回调实现决定
using ProgressHandler = std::function<void(const Progress &)>;

/// @brief 一段字节范围请求（`RFC 9110 §14.2` 的 `Range` 字段），三型之一；由 `Request::range()` 设定。
///
/// 三型对应线上三种写法，`first` / `last` 的取值组合本身就是形态：
/// - `between(f, l)` → `bytes=f-l`，两端都含；
/// - `from(f)` → `bytes=f-`，到资源当前长度为止；
/// - `suffix(n)` → `bytes=-n`，取末尾 `n` 字节——此时 `last` 装的是**长度**而不是偏移，这是本类型唯一一处
///   字段名与语义不同名的地方，故换算交给工厂与 `to_header_value()`，调用方不必自己记偏移。
///
/// 只表达一个区间：`RFC 9110 §14.2` 允许多区间，但多区间的应答是多部分体（`multipart/byteranges`），
/// 本库不装配那种形态，故也不给出「静默丢掉第二个区间」的口子。越界一律不裁剪（`Range: bytes=900-999`
/// 打一条 500 字节的资源拿到 416 是协议的正确说法）；形态非法（两端皆空、后缀长度为零、`last < first`）
/// 由 `render_head()` 落 `http-request-invalid`，因为那是一句要上线的话，不是一次查询。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
struct ByteRange {
    std::optional<std::uint64_t> first;  ///< 段起点（含）；`nullopt` = 后缀形态
    std::optional<std::uint64_t> last;  ///< 段终点（含）；后缀形态下装「末尾多少字节」

    /// @brief 从某偏移直到资源末尾（`bytes=first-`）。
    /// @param first 段起点（含），零基偏移。
    /// @return 开放尾端的范围。
    [[nodiscard]] static auto from(std::uint64_t first) -> ByteRange {
        return ByteRange{.first = first, .last = std::nullopt};
    }

    /// @brief 闭合区间（`bytes=first-last`）。
    /// @param first 段起点（含），零基偏移。
    /// @param last 段终点（含），须不小于 `first`。
    /// @return 两端都定的范围。
    [[nodiscard]] static auto between(std::uint64_t first, std::uint64_t last) -> ByteRange {
        return ByteRange{.first = first, .last = last};
    }

    /// @brief 末尾若干字节（`bytes=-length`）。
    /// @param length 长度，须为正数；长度超过资源本身时对端交出整份（`RFC 9110 §14.2` 的口径）。
    /// @return 后缀范围。
    [[nodiscard]] static auto suffix(std::uint64_t length) -> ByteRange {
        return ByteRange{.first = std::nullopt, .last = length};
    }

    /// @brief 形态是否可作为一句 `Range` 字段上线。
    /// @return 三型之一且数字关系成立为 true。
    [[nodiscard]] auto is_valid() const -> bool;

    /// @brief 折算为字段值文本（不含字段名）。
    /// @return `bytes=` 开头且带单位令牌的字段值；`is_valid()` 为假时为空串。
    [[nodiscard]] auto to_header_value() const -> std::string;
};

/// @brief 流式请求体的推送口：被逐段问出一段明文，由发起侧的体泵按线上形态写出。
///
/// 它存在的原因是「先把整份文件读进 `std::vector<char>` 再交出去」不是上传大文件的形态：体要在写出那一刻
/// 才被读，读过的段不再回来。三条纪律：
/// - **空段即结束**：`next_chunk()` 交出成功且长度为零的段表示体已发完，终止块由发起侧补齐。这里不用
///   `std::optional`，因为「本段为空但后面还有段」在线上根本没有对应形态，留着只会多一条测不到的分支。
/// - **同步、被逐段问、不阻塞**：本接口没有回调也没有协程出口，段长上限由实现自定（本库内置件取一节的
///   写块上限）；一次 `next_chunk()` 不得等待外部事件，它跑在续体恢复所在的那条线程上（缺省即主线程）。
/// - **`restart()` 是重放资格的唯一判据**：`RetryPolicy` 的白名单与「重定向还带不带体」都只问这一句。
///   回到不了原点的体被自动挡在重试之外——重放半截体等于交付一份数据损坏的请求，且这个代价在失败现场
///   无从判定，故本库不为此再开一枚旋钮。
///
/// 拷贝与移动一律删除：`Request` 持的是引用，搬动一枚已挂上的源会让那次搬运无人知晓。
/// @note Thread: 由发起侧在单条协程帧内串行调用；实现自身若无共享状态即随该帧线程约束
/// @note Side-effects: 由实现决定（内置件读文件）
class BodySource {
  public:
    BodySource() = default;
    BodySource(const BodySource &) = delete;
    auto operator=(const BodySource &) -> BodySource & = delete;
    BodySource(BodySource &&) = delete;
    auto operator=(BodySource &&) -> BodySource & = delete;
    virtual ~BodySource() = default;

    /// @brief 交出下一段明文并推进内部游标。
    /// @return 成功且非空 = 本段体字节；成功且为空 = 体已发完；失败 = 读取失败，发起侧当场中止本次交换。
    [[nodiscard]] virtual auto next_chunk() -> Result<std::vector<char>> = 0;

    /// @brief 体的总长度，供发起侧决定线上形态并补齐 `Content-Length`。
    /// @return 已知则返回字节数；`nullopt` = 未知，本次走 `Transfer-Encoding: chunked`。
    [[nodiscard]] virtual auto total_size() const -> std::optional<std::uint64_t> = 0;

    /// @brief 把游标放回原点（重放前的准备动作）。
    /// @return 回到原点为 true；不可回退（管道、一次性流、已被丢掉的段）为 false，此时本次交换不进重试环、
    ///         重定向也不带体重发。
    virtual auto restart() -> bool = 0;
};

/// @brief 一次 HTTP 请求：方法 + 目标 + 字段 + 消息体 + 发起侧选项。
/// @note Thread: thread-safe（值类型，无共享状态；`render_head()` 只读）
/// @note Side-effects: none
class Request {
  public:
    /// @brief 生成 `GET` 请求。
    /// @param url 目标 URL 文本；当场 `Url::parse`，失败记录在对象内、到 `render_head()` 才报错。
    /// @return 请求值（方法 `Get`）。
    [[nodiscard]] static auto get(std::string_view url) -> Request;
    /// @brief 生成 `HEAD` 请求（应答恒无体）。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Head`）。
    [[nodiscard]] static auto head(std::string_view url) -> Request;
    /// @brief 生成 `POST` 请求。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Post`）。
    [[nodiscard]] static auto post(std::string_view url) -> Request;
    /// @brief 生成 `PUT` 请求。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Put`）。
    [[nodiscard]] static auto put(std::string_view url) -> Request;
    /// @brief 生成 `PATCH` 请求。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Patch`）。
    [[nodiscard]] static auto patch(std::string_view url) -> Request;
    /// @brief 生成 `DELETE` 请求。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Delete`）。
    /// @note 名字尾下划线是因为 `delete` 是 C++ 关键字，无法直接作为工厂名。
    [[nodiscard]] static auto delete_(std::string_view url) -> Request;  // NOLINT(readability-identifier-naming)

    /// @brief 方法。
    /// @return 构造时确定的方法枚举。
    [[nodiscard]] auto method() const -> Method { return method_; }

    /// @brief 是否为 `HEAD` 请求（应答分帧判定的唯一输入）。
    /// @return 方法为 `Head` 时为 true。
    [[nodiscard]] auto is_head() const -> bool { return method_ == Method::Head; }

    /// @brief 目标 URL（仅当解析成功时可见）。
    /// @return 解析成功的 URL；解析失败时为 `nullopt`（错误在 `render_head()` 返回）。
    [[nodiscard]] auto url() const -> const std::optional<Url> & { return url_; }

    /// @brief 追加一枚请求字段（保留调用方书写次序；同名可多条）。
    /// @param name 字段名；原样存储。
    /// @param value 字段值。
    /// @return 自身引用，便于链式调用。
    auto header(std::string_view name, std::string_view value) -> Request &;

    /// @brief 置入单值请求字段：先移除同名字段再追加（`Host` / `Content-Type` 一类应唯一的字段）。
    /// @param name 字段名。
    /// @param value 字段值。
    /// @return 自身引用，便于链式调用。
    auto set_header(std::string_view name, std::string_view value) -> Request &;

    /// @brief 查看请求字段（不含 `render_head()` 补齐的 `Host` 与体形态字段，也不含 `Range` / `If-Range` /
    /// `Expect` 三枚由专用旋钮表达的字段）。
    /// @return 字段表的只读引用。
    [[nodiscard]] auto headers() const -> const Headers & { return headers_; }

    /// @brief 为本次交换指定链路标识（写进 `X-Request-Id` 请求字段）。
    ///
    /// 三级取定与 `Accept-Encoding` 的缺省补齐同一纪律：**调用方自己写过的 `X-Request-Id` 永远优先**（本旋钮
    /// 与 `Client::request_id()` 都不覆盖它）；其次才是本旋钮给定的值；两者都没设而客户端缺省开着时，由发起侧
    /// 现取一枚 `RFC 9562` UUIDv4 小写文本形态的标识。一次交换只取一枚：重定向的每一跳与重试的每一次尝试都带
    /// 同一串，对端与中间层的日志才据此把一条逻辑交换的若干次物理往来串起来。
    /// @param value 标识文本，原样写上线（本库不校验形状）；空串 = 明确声明本次交换不带链路标识（不是「跟随缺省」）。
    /// @return 自身引用，便于链式调用。
    auto request_id(std::string value) -> Request &;

    /// @brief 查看本次交换的链路标识设定。
    /// @return 已设定则返回之（空串 = 明确不带）；`nullopt` = 跟随 `Client::request_id()` 的缺省。
    [[nodiscard]] auto request_id() const -> const std::optional<std::string> & { return request_id_; }

    /// @brief 设定内存消息体（覆盖既有，含已挂上的流式体）。`HEAD` 请求忽略之（应答无体、请求不得带体）。
    /// @param body 体字节。
    /// @return 自身引用，便于链式调用。
    auto body(std::vector<char> body) -> Request &;

    /// @brief 查看消息体。
    /// @return 体字节的只读引用；无体时为空。流式体（`body(BodySource &)`）恒不体现在此返回值上，看
    ///         `body_source()`。
    [[nodiscard]] auto body() const -> const std::vector<char> & { return body_; }

    /// @brief 设定流式消息体（覆盖既有；与 `body(std::vector<char>)` 互斥，后设者生效）。
    ///
    /// `Request` 只持引用，不接管所有权：源的存活期须覆盖整次交换，含重定向的每一跳与重试的每一次尝试——
    /// 体泵在协程下一次恢复时还要问它要第二段。`HEAD` 请求忽略之（与 `body(std::vector<char>)` 同口径，
    /// 请求不得带体）。线上形态、分段与终止块都由发起侧的体泵负责，语义约束见 `BodySource`。
    /// @param source 体的推送口。
    /// @return 自身引用，便于链式调用。
    auto body(BodySource &source) -> Request &;

    /// @brief 查看流式消息体。
    /// @return 已挂上的源；未挂为 `nullptr`。
    [[nodiscard]] auto body_source() const -> BodySource * { return body_source_; }

    /// @brief 设定本次请求的字节范围（现地补进 `Range` 请求字段，`headers()` 里看不到它）。
    ///
    /// 本库是唯一会发出范围请求的一方，故只存一型、由本方法负责字段拼写；越界不裁剪（对端用 416 说是它
    /// 的事）。形态非法在 `render_head()` 落 `http-request-invalid`。带范围的请求不参与内存缓存——同一
    /// URL 的整份副本绝不能被拿来充当一个分段应答。
    /// @param value 字节范围。
    /// @return 自身引用，便于链式调用。
    auto range(ByteRange value) -> Request &;

    /// @brief 查看字节范围。
    /// @return 已设定则返回之；`nullopt` = 本次请求整份资源。
    [[nodiscard]] auto range() const -> const std::optional<ByteRange> & { return range_; }

    /// @brief 设定 `If-Range` 校验子（续传用；现地补进请求字段，`headers()` 里看不到它）。
    ///
    /// 取值为已持有副本的校验子（`ETag`）或其 `Last-Modified` 文本，两形态都由 `RFC 9110 §13.1.5` 承认，
    /// 本库不校验形状、原样写出。断点续传的正确写法是「带 `range()` + 带本校验子」：对端确认资源自持副本
    /// 以来未变才回 206，否则回 200 交整份，调用方据此丢弃已落盘的半截。只在同时设有 `range()` 时才上线
    /// ——没有可修饰的分段请求时，这条字段只是一次无谓的往返。
    /// @param value 校验子文本；空串与未设同义（不发该字段）。
    /// @return 自身引用，便于链式调用。
    auto if_range(std::string value) -> Request &;

    /// @brief 查看 `If-Range` 校验子。
    /// @return 已设定则返回之；`nullopt` = 未设。
    [[nodiscard]] auto if_range() const -> const std::optional<std::string> & { return if_range_; }

    /// @brief 设定是否「先问再发体」（`Expect: 100-continue`）；**缺省关闭**。
    ///
    /// 开着且本次确实有体时，头部写完后先等一小段（长度是库内置常量，不占 `Timeouts` 任何一档）：对端或
    /// 中间层回 `1xx` 就立刻发体，窗口到点没人应就盲发——后者是 `RFC 9110 §10.1.1` 明确允许的形态，且是
    /// 现实里的常见路径（多数中间层压根不回 `100`）。关掉时逐字节与「本功能从未存在」同型：头部与体连着发。
    /// 无体的请求设了也不写 `Expect` 字段：问了也没有待发的体，只会让对端多等一个判定。
    ///
    /// 窗口里那条 `1xx` 不是最终应答，它排在最终应答之前被装配器收下，不改写最终应答的分帧与判定；
    /// 一条 `101` 落 `http-protocol-error`（协议升级不属本层）。
    /// @param value 开启为 true。
    /// @return 自身引用，便于链式调用。
    auto expect_continue(bool value) -> Request &;

    /// @brief 查看是否「先问再发体」。
    /// @return 开启为 true；缺省假。
    [[nodiscard]] auto expect_continue() const -> bool { return expect_continue_; }

    /// @brief 设定整请求时限（`total` 档的便捷入口；要分档控制用 `timeouts()`）。
    /// @param value 时限。
    /// @return 自身引用，便于链式调用。
    auto timeout(std::chrono::milliseconds value) -> Request &;

    /// @brief 查看整请求时限（等价 `timeouts().total`）。
    /// @return 已设定则返回之；未设定为 `nullopt`。
    [[nodiscard]] auto timeout() const -> std::optional<std::chrono::milliseconds> { return timeouts_.total; }

    /// @brief 设定三档超时（连接 / 读空闲 / 总耗时）。
    /// @param value 超时集合。
    /// @return 自身引用，便于链式调用。
    auto timeouts(const Timeouts &value) -> Request &;

    /// @brief 查看三档超时。
    /// @return 超时集合的只读引用。
    [[nodiscard]] auto timeouts() const -> const Timeouts & { return timeouts_; }

    /// @brief 为本次请求单独设定重试策略（覆盖 `Client::retry_policy()` 的缺省）。
    ///
    /// 只在「这一条请求跟别的请求不一样」时用（例如某个已知可安全重放的 PUT 想多打两次）。要关掉单条请求
    /// 的缺省重试，设 `RetryPolicy{}` 即可（`attempts` 缺省 `1` = 关闭），不是传空值语义。
    /// @param value 重试策略。
    /// @return 自身引用，便于链式调用。
    auto retry_policy(RetryPolicy value) -> Request &;

    /// @brief 查看本请求的重试策略覆盖。
    /// @return 已设定则返回之；`nullopt` = 跟随 `Client::retry_policy()`。
    [[nodiscard]] auto retry_policy() const -> const std::optional<RetryPolicy> & { return retry_policy_; }

    /// @brief 设定本次交换完全绕过缓存：既不查表，也不把这次的结果写进表。
    ///
    /// 语义比 `RFC 9111 §5.2.1.5` 的 `no-cache` 请求指令强（那条只要求「交付前先向对端验证」，仍允许存），
    /// 取的是 `no-cache` ∧ `no-store` 那一面：调用方按「这次要此刻的真相」用它，而弱形态留下的那次写入照样
    /// 会让下一请求看见本次的副本，那不算把缓存「越」过去。只在 `Client::cache()` 开着时才有意义。
    /// @param value 绕过为 true。
    /// @return 自身引用，便于链式调用。
    auto no_cache(bool value) -> Request &;

    /// @brief 查看本次交换是否绕过缓存。
    /// @return 绕过为 true；缺省假（跟随客户端的缓存开关）。
    [[nodiscard]] auto no_cache() const -> bool { return no_cache_; }

    /// @brief 设定本请求的事件流重连口径（覆盖 `Client::sse()` 的缺省）。
    ///
    /// 只在 `Client::open_sse()` 这一条路上被读取：`send` 与 `open_stream` 不关心重连窗口，物化路径也没有
    /// 「一条流」的概念。典型用法是某一端点已知会瞬断、想多试几次，而别处保持缺省。
    /// @param value 重连口径。
    /// @return 自身引用，便于链式调用。
    auto sse(SseOptions value) -> Request &;

    /// @brief 查看本请求的事件流重连口径覆盖。
    /// @return 已设定则返回之；`nullopt` = 跟随 `Client::sse()`。
    [[nodiscard]] auto sse() const -> const std::optional<SseOptions> & { return sse_; }

    /// @brief 设定重定向跟随上限（`0` 表示不跟随）。缺省 10。
    /// @param value 上限次数。
    /// @return 自身引用，便于链式调用。
    auto max_redirects(int value) -> Request &;

    /// @brief 查看重定向跟随上限。
    /// @return 上限次数。
    [[nodiscard]] auto max_redirects() const -> int { return max_redirects_; }

    /// @brief 设定进度回调（覆盖既有；传空回调即取消）。
    ///
    /// 发起侧把两侧进度**在源头合流**后再发射：字节到达只更新一枚「最新值」槽，发射发生在每一次可发射
    /// 的机会（宿主回合）上，且**同一机会至多一次**——挂了回调的下载不会因为对端切得碎就每包写一次状态。
    /// 本库是响应式框架，每次回调都可能落到一个信号写入上，按 chunk 发射会把它放大成重绘风暴。装可注入的
    /// 帧号源（`Client::progress_frame_source()`）时改用帧号作闸，严格「每帧至多一次」；未装即以宿主回合
    /// 为闸（一次可发射机会即一回合）。两相都有一个例外：**每个方向的末值恒发一次**，否则调用方无法靠回调
    /// 判定「收了摊」。
    ///
    /// 回调恒在主线程同步调用（续体经主线程投递器恢复）；它只用于观测，其返回值与耗时都不参与协议判定。
    /// @param handler 回调；空回调 = 不设。
    /// @return 自身引用，便于链式调用。
    auto on_progress(ProgressHandler handler) -> Request &;

    /// @brief 查看进度回调。
    /// @return 已设则返回之；否则空回调。
    [[nodiscard]] auto on_progress() const -> const ProgressHandler & { return on_progress_; }

    /// @brief 序列化请求行与请求字段（不含消息体）。
    ///
    /// 补齐四处：无 `Host` 时按目标 URL 补；体形态按 `Content-Length` / `Transfer-Encoding: chunked` 二者
    /// 之一补（判据与发起侧体泵共用同一份计划，见 `detail::plan_request_body()`，故线上声明与实际写出的
    /// 字节不会分岔）；设有 `range()` 时补 `Range`，并连带设有 `if_range()` 时补 `If-Range`；开着
    /// `expect_continue()` 且有待发的体时补 `Expect: 100-continue`。四处的通例都是**调用方自己写过的同名
    /// 字段永远优先**。校验（方法令牌、字段名 token、字段值无 CR / LF、URL 含主机、范围形态）由本函数与
    /// 字节层执行，越界与歧义一律落 `http-request-invalid`。
    ///
    /// 请求目标默认写成 origin-form（`/path?query`），即「这条请求发往目标本身」。`absolute_form` 为真时
    /// 改写成绝对形态（`http://host:port/path?query`）——那是**明文经转发代理**时代理要的写法（`RFC 9112
    /// §3.2.2`），它据此决定把请求递给谁；`Host` 仍按目标 URL 补，代理不改写这条。发起侧只在路由判出
    /// 「本跳的对端是转发代理而非目标」时置真，调用方一般不必自己碰它。
    /// @param absolute_form 请求目标是否写成绝对形态；假 = origin-form（缺省）。
    /// @return 请求头部字节（以空行收尾）；URL 解析失败、范围形态非法或字段文法不合时为错误
    ///         （`http-request-invalid`）。
    [[nodiscard]] auto render_head(bool absolute_form = false) const -> Result<std::string>;

  private:
    /// @brief 由工厂共用：绑定方法与已解析的目标。
    /// @param method 方法枚举。
    /// @param url 目标 URL 文本；当场解析。
    Request(Method method, std::string_view url);

    Method method_{Method::Get};  ///< 方法
    std::optional<Url> url_;  ///< 已解析目标；nullopt = 解析失败（错误暂存于 url_error_）
    std::optional<Error> url_error_;  ///< 解析失败的载荷；仅在 url_ 为空时有意义
    Headers headers_;  ///< 请求字段（不含补齐项）
    std::optional<std::string> request_id_;  ///< 链路标识设定；nullopt = 跟随 Client 缺省，空串 = 明确不发
    std::vector<char> body_;  ///< 内存消息体；与 body_source_ 互斥（后设者清前者）
    BodySource *body_source_{nullptr};  ///< 流式消息体（借用，不接管所有权）；nullptr = 无
    std::optional<ByteRange> range_;  ///< 字节范围；有值即在 render_head() 现地补 `Range`
    std::optional<std::string> if_range_;  ///< `If-Range` 校验子；只在同时设有 range_ 时上线
    bool expect_continue_{false};  ///< 是否先问再发体（`Expect: 100-continue`），缺省关
    Timeouts timeouts_;  ///< 三档超时（connect / read_idle / total），缺省全不设
    std::optional<RetryPolicy> retry_policy_;  ///< 本请求的重试策略覆盖；nullopt = 跟随 Client 的缺省
    bool no_cache_{false};  ///< 本次交换是否绕过缓存（既不读也不写）
    std::optional<SseOptions> sse_;  ///< 本请求的事件流重连口径覆盖；nullopt = 跟随 Client 的缺省
    int max_redirects_{10};  ///< 重定向跟随上限
    ProgressHandler on_progress_;  ///< 进度回调（空 = 不报）；发起侧负责合流后再调用
};

/// @brief 一段部分应答所覆盖的范围（`RFC 9110 §14.4` 的 `Content-Range` 字段）。
///
/// 三个字段都可缺：对端用 `*` 表达的「范围未知」（`bytes */1234`）折成 `first` / `last` 皆空、`total` 有值，
/// 总长未知则 `total` 为空。它们是同一枚字段的不同缺项，故用 `nullopt` 而非零值——把「未知」写成 `0` 会让
/// 续传逻辑以为资源是空的。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
struct ContentRange {
    std::optional<std::uint64_t> first;  ///< 本段起点（含）；`*` 形态为空
    std::optional<std::uint64_t> last;  ///< 本段终点（含）；`*` 形态为空
    std::optional<std::uint64_t> total;  ///< 完整表示体的总长；对端未知时为空
};

/// @brief 一次 HTTP 应答：状态行 + 字段 + 已物化（解压完毕）的消息体。
///
/// `body()` 交出的是未解码的原始字节，`text()` 才按 `Content-Type` 的 `charset` 现地解码为文本；
/// 二者对同一份体各表一层，故 charset 不支持时仍可回退到 `body()` 自处。分段应答由 `is_partial()` 与
/// `content_range()` 两枚说出：本类型不裁剪、不合并、不缓存拼接，把对端说的那段范围原样交回调用方。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
class Response {
  public:
    /// @brief 由已物化的分量构造（发起侧把体读全后交进来）。
    /// @param status 三位状态码。
    /// @param reason 原因短语（原样，可为空）。
    /// @param headers 应答字段。
    /// @param body 已解压的消息体字节（`Content-Encoding` 的透明解压在物化之前完成；`charset` 解码留给
    /// `text()` 按 `Content-Type` 现场执行，故 `body()` 恒给出未解码的原始字节）。
    /// @param from_cache 这份应答是否出自本客户端的内存缓存（真 = 未参与本次线上交换，或只是 304 的另一半）。
    ///                   发起侧交付新鲜副本、以及把 304 与已存副本合并时置真；从对端取回的完整应答置假。
    Response(std::uint16_t status, std::string reason, Headers headers, std::vector<char> body,
             bool from_cache = false);

    /// @brief 状态码。
    /// @return 三位整数。
    [[nodiscard]] auto status() const -> std::uint16_t { return status_; }

    /// @brief 原因短语。
    /// @return 原样文本；对端未给时为空串。
    [[nodiscard]] auto reason() const -> std::string_view { return reason_; }

    /// @brief 应答字段表。
    /// @return 字段表的只读引用。
    [[nodiscard]] auto headers() const -> const Headers & { return headers_; }

    /// @brief 取首个同名应答字段的值。
    /// @param name 字段名；ASCII 大小写不敏感。
    /// @return 首个匹配值；无匹配为 `nullopt`。
    [[nodiscard]] auto header(std::string_view name) const -> std::optional<std::string>;

    /// @brief 解析应答的 `Content-Range` 字段（`RFC 9110 §14.4`）。
    ///
    /// 每次调用现解，不给 `Response` 加字段：它是纯值类型，且绝大多数应答永远不会被问到这一枚。只认
    /// `bytes` 单位的三种形态（`0-499/1234`、`500-1234/1234`、`*/1234`）——本库是唯一会发出范围请求的一方，
    /// 其余单位走不到这层。字段缺失、文法不合、数字超出可表示范围一律回 `nullopt`：与 `body()` 一样，
    /// 本函数不替对端的畸形报文编造事实，也不报错（一条坏 `Content-Range` 附着的仍是一份要交出的应答）。
    /// @return 解析出的范围；无可解字段为 `nullopt`。
    [[nodiscard]] auto content_range() const -> std::optional<ContentRange>;

    /// @brief 本次应答是否为部分应答。
    /// @return 状态码 `206` 为 true。判定只看状态码而不看 `content_range()` 是否可解：一条带着坏字段的 206
    ///         仍然是「对端说了部分」这一事实，把字段解析失败折成「完整应答」会把它抹掉；续传要落盘的调用方
    ///         两枚都该问一遍，缺任何一枚都不该往既有文件尾部接。
    [[nodiscard]] auto is_partial() const -> bool { return status_ == 206; }

    /// @brief 消息体原始字节。
    /// @return 体字节的只读引用。
    [[nodiscard]] auto body() const -> const std::vector<char> & { return body_; }

    /// @brief 这份应答是否出自本客户端的内存缓存。
    ///
    /// 两种形态都算：新鲜副本被直接交付（本次交换根本没连出去），以及条件请求拿到 304 后把已存副本合并交出
    /// （线上只走了头部）。它只回答「这份载荷是不是从表里来的」，不回答「表里那条老不老」——新鲜度判定在
    /// 发起侧完成，走到调用方手里的一律是可交付的。缓存未开启时恒为假。
    /// @return 出自缓存为 true。
    [[nodiscard]] auto from_cache() const -> bool { return from_cache_; }

    /// @brief 消息体的文本形态（按 `Content-Type` 的 `charset` 解码为 UTF-8 文本）。
    ///
    /// 首批只解 `charset` 为 UTF-8（剥前导 BOM）与 Latin-1（`iso-8859-1` / `latin-1`）两族；字段缺失
    /// 或无 `charset` 参数时按 UTF-8 处理（现代缺省）。其余 charset 报 `http-charset-unsupported`——
    /// 原始字节恒可经 `body()` 取回，本函数绝不把非法字节序列静默替换成 U+FFFD 或另作他解。透明
    /// 解压（`Content-Encoding`）不在此处：进入 `body_` 的字节已由发起侧物化管线解压完毕。
    /// @return 解码后的 UTF-8 文本；charset 不支持时为 `http-charset-unsupported`。
    [[nodiscard]] auto text() const -> Result<std::string>;

  private:
    std::uint16_t status_{0};  ///< 状态码
    std::string reason_;  ///< 原因短语
    Headers headers_;  ///< 应答字段
    std::vector<char> body_;  ///< 已物化消息体
    bool from_cache_{false};  ///< 载荷是否出自缓存（真 = 未参与线上交换，或只是 304 的另一半）
};

/// @brief 一次 HTTP 失败的归因档：把「该往哪儿排查」这一件事从错误码里提出来。
///
/// 本档只回答**归因层**，不回答**能不能重试**——后者由 `Error::retryable`（与 `codespec/errors.toml`
/// 逐码同源）给出，两枚正交：`Timeout` 且可重试是「再来一次」，`Proxy` 且可重试
/// （`net-proxy-config-unreadable`）是「读系统配置的时机不对」，压进同一枚枚举只会让调用方靠猜分工。
/// 业务域已是一等元数据（`Error::domain` 取 `Net` / `Http` / `Ws`，归属问题不再需要本档代答），本档因此只管一件
/// 事：失败落在网络栈的哪一跳。生成表的 `ErrorKind` 答的是「什么坏了」而不答「哪一跳」——同一枚 `corrupt` 既可
/// 能是对端把 HTTP 报文写坏，也可能是代理握手不成帧；同一枚 `denied` 既可能是代理拒了凭据，也可能是 TLS 链拒了
/// 证书，而排查方向恰由这一跳决定。
///
/// 九档按「先具体后兜底」归一，同一枚码恒只进一档：`Cancelled` 优先于一切（调用方主动收摊，不再看
/// 链路）；`Timeout` 看的是**时限**而不是阶段（HTTP 三档超时与裸传输空闲超时同归此档）；`Dns` / `Proxy` /
/// `Tls` 按**失败发生在哪一跳**归类；`Unreachable` 是「链路断了，但没有更具体的档可归」；`Protocol` 只指
/// **对端把报文写坏了**；`Status` 指「应答已收到、状态码本身即失败」；`Invalid` 是兜底，涵盖调用点写错、
/// 本构建缺能力、本库旋钮设限这三类「动作在代码或配置里而不在网络上」的情形。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class HttpErrorClass : std::uint8_t {
    Invalid = 0,  ///< 兜底档：调用点 / 本构建能力 / 本库上限（要改的是代码与旋钮，不是网络）
    Dns,  ///< 名字解析不了
    Unreachable,  ///< 链路断了：建连、读写、事件循环侧的失败
    Proxy,  ///< 代理这一跳给的答案：不可达、要认证、握手字节不合、系统配置读不到
    Tls,  ///< 安全通道本身：握手、证书、版本、pin
    Timeout,  ///< 时限到点
    Protocol,  ///< 对端把报文写坏了：状态行、字段、分帧、编码后的字节
    Status,  ///< 应答已收到且状态码本身即失败（4xx / 5xx）
    Cancelled,  ///< 调用方取消
};

/// @brief 把一枚错误码归入 HTTP 失败分型档（全函数：任意 `ErrorCode` 都有档可归）。
///
/// 逐码映射以本函数为唯一真值源，覆盖 `net-*` 与 `http-*` 两族全部 43 枚（含发起侧用不到的服务端面码，
/// 同样给出档）；其余族的码（`general-*` / `widget-*` 之类）不是网络交换的产物，一律落 `Invalid` 而
/// **不**报错——分型是观测口，不是校验口。两处值得点名的归类：`http-redirect-limit` 归 `Protocol`
/// （对端把重定向写成了环，改不了它就得改跟随策略），`http-body-too-large` 归 `Invalid`（越的是本库
/// 上限旋钮，动作在调用方而不在对端）。
/// @param code 错误码（取自 `Error::code_enum`）。
/// @return 该码的归因档。
[[nodiscard]] auto error_class_of(ErrorCode code) -> HttpErrorClass;

/// @brief 由应答状态码折算归因档（状态码是数据不是错误，故这条映射不在错误码表里）。
///
/// `send` 与 `open_stream` 对非 2xx 交出的仍是**成功** `Result`：一份 404 的应答同样是完整的事实，把它
/// 折算成错误就丢掉了状态码本身。于是「这次算不算状态码类失败」由调用方判，本函数给出判完之后的档，
/// 使 `Status` 这一档有唯一产出点而不是枚举里的死项。
/// @param status 应答状态码（`Response::status()`）。
/// @return `>= 400` 恒为 `Status`；`< 400` 回 `Invalid`，此处语义退化为「没有状态码类失败可归因」，
///         不代表这次交换出错。
[[nodiscard]] auto error_class_of_status(std::uint16_t status) -> HttpErrorClass;

/// @brief 分型档的稳定小写令牌（日志与 Inspector 用；跨进程只认这枚文本，改名不影响它）。
/// @param error_class 归因档。
/// @return `"invalid"` / `"dns"` / `"unreachable"` / `"proxy"` / `"tls"` / `"timeout"` / `"protocol"` /
///         `"status"` / `"cancelled"` 之一，恒非空。
[[nodiscard]] auto error_class_text(HttpErrorClass error_class) -> std::string_view;

}  // namespace aurora::net::http
