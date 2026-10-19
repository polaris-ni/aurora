#pragma once

/// @brief HTTP/1.1 发起侧客户端。
/// @file net/http_client.h
///
/// 本头回答一个问题：**「把一份 `Request` 送出去、把应答拿回来」**——连接（`http://` 直连、
/// `https://` 先握手再套流）、写出请求头部与消息体、逐段喂接收侧装配器 `wire::ResponseAssembler` 装配应答、
/// 按 `Location` 跟随重定向、并施加整请求时限。应答有两条交付路：`send` 物化进 `Response::body()` 才交出，
/// `open_stream` 逐段交给宿主的 `DataSink`（形态差别见 `net/http_stream.h`）。它是纯逻辑之上的收发编排，本身
/// 不持套接字（每次交换起一条连接、收尾即关），因此可自由拷贝、可并发发起。
///
/// 三条形态裁决（规范性，落码与文档回写以此为准）：
/// - **协程出口 `CoroTask<Result<Response>>`**：传输层（L1）刻意只给回调出口（`net/transport.h`：把 `Task`
///   写进 L1 公共头会让下层反向依赖 L4），协程形态归上层给出——本发起侧就是那「上层」。续体经主线程投递器
///   恢复，故调用方（含 UI 线程）在 `co_await` 之后可安全触碰响应式状态；桥接细节藏在库内 `net/detail/`。
/// - **连接生命周期由池口径裁决**：`PoolOptions{}` 缺省关——一次 `send` = 一条连接、用完即关的既有口径
///   逐字不变；开着时同池键的请求坐上一条判干净的连接（复用资格五判据见 `PoolOptions` 头注释），判不干净
///   当场关闭，绝不为「复用」预先牺牲正确性。`Session`（`net/http_session.h`）是身份层：持一枚 Cookie Jar
///   并装进本客户端（拷贝共享池、身份只随 `Session`），裸 `Client` 不收不发 Cookie。
/// - **超时以 `Request::timeout()` 为准**：它是整请求时限（跨连接、TLS、读写、重定向全程），到点落
///   `http-timeout`；分段超时（`connect` / `read_idle`）已随之落地，重试（`RetryPolicy`）的每一次尝试各拿
///   一份 `total`、跨全部尝试的墙另有 `RetryPolicy::budget`。认证（`Client::authenticator()`）是重试环内层的
///   又一环，一次交换至多注入一次且不吃 `attempts` 名额，形态与判据见 `net/http_auth.h`。
/// - **取消经 `Cancellation` 参数进来**：`send` / `open_stream` 各收一枚令牌（缺省即一份独立令牌 = 无人取消）。
///   四条口径：**取消先于一切判定**——令牌到手时已取消，策略闸、缓存表、连接一概不问，直接落 `net-cancelled`；
///   **掐的是在途那一次**——令牌接到交换内建的「跨尝试序列」上，取消即放行当前挂起点，本环不再发起新的尝试，
///   而已经打过的那一次留下的事实（含其它错误码）不被改写；**令牌是共享状态**——拷贝只增引用，宿主与适配器
///   看到同一个取消位（契约见 `net/cancellation.h`）；**不等钩子跑完**——`cancel()` 在调用线程上同步放行，
///   不等结果落地，在途交换由会话自己收尾（声明式接线的「宿主析构即掐连接」正以此为前提）。
///
/// 内容编码（透明解压）在发起侧管线内完成，`Response` 交出的恒是还原后的字节：
/// - **宣告与支持集同源**：core 注册表的 gzip / zlib / deflate 三槽均已注册时，`send` 替调用方补
///   `Accept-Encoding: gzip, deflate`；调用方
///   已自带该字段（任意大小写拼写）则**原样保留、不覆盖也不重复追加**——协商是调用方的表达，本件只在缺省时
///   补一个与自己解码能力等长的宣告。三槽未全部注册（内置解码器仅在 `AURORA_ENABLE_COMPRESSION=ON`
///   的构建编入；应用可自注册补位）时**连宣告都不发**，此时
///   对端强行压缩落 `http-content-encoding-unsupported`，绝不把压缩字节当明文交出。
/// - **解码在物化之后、值层之前**：分帧（`Content-Length` / chunked）解出的仍是编码字节，本件按
///   `Content-Encoding` 链反序逐层还原，随后才交给 `Response`。`Response::text()` 的 charset 解码发生在
///   这一层之外，两条解码各自的失败互不掩盖。无体应答（HEAD / 1xx / 204 / 304）不校验编码名。
/// - **解压产物另有上限**：`max_decompressed_bytes()` 缺省跟随 `max_body_bytes()`，越限落
///   `http-body-too-large` 且在 detail 点名越的是哪一枚旋钮（炸弹防护护的是解压产物，不是线上字节）。
///
/// 进度观测（`Request::on_progress()`）同样在本件源头合流，三条口径与内容编码并列成立：
/// - **每方向在一次可发射机会里至多一发**：字节到达只更新槽值，发射发生在协程恢复出的宿主回合上；装了
///   `progress_frame_source()` 即以帧号为闸，严格「每帧至多一次」。按 chunk 发射会把响应式绑定放大成重绘风暴。
/// - **每方向的末值恒发一次**：合流不能把「收了摊」这一事实合掉，否则调用方只能靠比字节数猜完成。
/// - **挂了回调才分块写请求体**：上传进度要有可观测的节拍，故带体的请求按块写出、每块一次完成回调；未挂
///   回调时恒一次写完，线上字节序列两型完全相同（分块只影响本库内部的完成次数，不影响对端所见）。
///
/// 内存缓存（`Client::cache()`，缺省关）只在开关开着时参与交换，四条口径与上面的形态裁决同级：
/// - **查在重试环之外、写在环之外**：一次交换至多查表一次、至多写表一次。陈旧副本挂上的两枚条件头进的是
///   原始请求，故每一次尝试都带着同一份校验子重跑；重放不会把一个 304 变成 200，也不会漏挂头部。
/// - **新鲜即零交换**：命中新鲜副本时一条连接都不发起，交出的 `Response::from_cache()` 为真。§12 承诺的
///   「同一 URL 二次请求不触网」就是这一条，代价是缓存里的说法可能落后于对端——那是开启者的选择。
/// - **从不交付陈旧副本**：过期条目一律先向对端验证，验证失败（含重试耗尽）就把那次失败原样交出，绝不拿
///   旧数据兜场。因此 `must-revalidate` 在本库里不需要额外分支（它的禁令正是本库的缺省），而 `no-cache`
///   响应指令的效果等价于「条目即刻过期」，走同一条重验证路。
/// - **进表的门槛由库判、不由调用方判**：只存 `GET` / `HEAD`、只存可缓存状态码那一族、`no-store` 与
///   `Vary: *` 直接不存、无显式有效期又无校验子的不存（本库不做 `RFC 9111 §4.2.2` 的启发式新鲜度，
///   那个算法随本机时钟与历史响应漂移，判据写不成定值；有校验子而无有效期则照存，条目即刻过期、每次都
///   走条件请求，省的是体字节而不是那一次往返）。
///
/// 代理（`Client::proxy()`，缺省 `ProxyEnvironment::disabled()` = 恒直连）有五条口径：
/// - **一条建连路、两种交付**：每一跳由 `ProxyEnvironment::for_target(host, port, secure)` 定出 `ProxyConfig`，
///   建连一律经 `StreamTransport::connect_via` 发出（`Direct` 按该入口的契约等价于直连），库里不留第二条
///   建连入口，「这次到底有没有经代理」因此只有一个答案可问。经代理之后分两种交付形态：**隧道**（`https://`
///   经 HTTP 代理、以及一切 SOCKS5）里对端仍是目标、请求行照写 origin-form；**转发**（`http://` 经 HTTP
///   代理）里对端就是代理本身、请求行改写成绝对形态 `http://host:port/path?query`（`RFC 9112 §3.2.2`）。
///   明文不为代理发 `CONNECT`：转发本就是转发代理的本职，而 `CONNECT` 到 80 端口常被拒止，那会把「代理不
///   支持这种用法」伪装成连通性故障。两种形态下 `Host` 都按目标 URL 补齐，代理不改写这条。
/// - **缺省绝不读环境变量**：代理集合只能由调用方显式装配（`ProxyEnvironment::from_environment()` /
///   `from_environment_or_system()` 都是调用方的动作）。同一段代码在不同机器上因环境而异，是本提案要消灭
///   的那类无痕故障，也让「连不上」的现场无法复现。
/// - **隧道不成即失败，不回退直连**：代理不可达、拒绝建隧道、不会说话一律以对应 `net-proxy-*` / 连接类错误
///   收尾（`connect_timeout` 覆盖直到隧道可用的整段），绝不改用直连再试一次——绕过代理往往正是策略禁止的
///   那条路。凭据只出现在写出的字节里，诊断文本一律只带 `host:port`。
/// - **`https://` 在隧道之上再握手**：SNI、证书校验与 `TlsContext` 口径的目标始终是原主机，代理只见 CONNECT
///   与密文流。
/// - **逐跳重算**：重定向把目标换成另一主机（乃至另一方案）时，本跳的代理决策按新目标重新定出，沿用的是
///   同一份环境集合而不是上一跳的结论。
///
/// 声明式安全策略（`Client::policy()` → 进程级 `net::default_policy()` → `NetPolicy{}` 那组保守缺省，三级
/// 取定见 `net/policy.h`）在发起侧问三条：
/// - **起手先于缓存**：目标不合规矩时连缓存表都不查——策略先于「少打一次网络」这条优化，命中缓存照样是
///   一次交付，不能因为它就没被问过。
/// - **逐跳重问**：每一跳（含重定向带来的新主机、新方案）都按本跳的代理决策重新判定，上一跳放行不代表这一跳。
/// - **拒在发出之前**：违规一律落 `net-policy-violated`，一条连接都不发起、一个字节都不写出。
///
/// 流式接收（`open_stream()` + `net/http_stream.h` 的 `DataSink`）与物化路径共用上述全部协议语义，四条形态
/// 差别单列在那里：线上字节不设限（`max_body_bytes()` 管的是物化路径的内存）、`deflate` 两型按包装头判定、
/// 重定向跳的消息体不读、本腿无流式能力时直接失败而不悄悄物化。
///
/// @code
/// aurora::net::http::Client client;
/// auto task = client.send(aurora::net::http::Request::get("https://example.com/api"));
/// // 宿主帧内泵至 task.is_done()，再读 task.result() 得 Result<Response>
/// @endcode
/// @note Thread: thread-safe（值语义客户端；`send` 的续体恒回主线程）
/// @note Side-effects: `send` / `open_stream` 发起网络 I/O；构造与配置无副作用

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/http.h"
#include "aurora/net/http_auth.h"
#include "aurora/net/http_sse.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/policy.h"
#include "aurora/net/proxy.h"
#include "aurora/net/tls.h"
#include "aurora/net/websocket.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::detail {
class HttpResponseCache;  ///< 实现侧缓存表：字段全部留在 `net/detail/http_cache.h`，公共头不暴露
class HttpConnectionPool;  ///< 实现侧连接池：字段全部留在 `net/detail/http_pool.h`，公共头不暴露
class HttpCookieJar;  ///< 实现侧 Cookie Jar：字段全部留在 `net/detail/http_cookie_jar.h`，公共头不暴露
}  // namespace aurora::net::detail

namespace aurora::net::http {

class Session;  ///< 会话面（身份层）：持有 Jar 并装进 Client，见 `net/http_session.h`

/// @brief HTTP 发起侧客户端：把 `Request` 送出去、把物化的 `Response` 拿回来。
/// @note Thread: thread-safe（可拷贝、可并发发起；每次 `send` / `open_stream` 自持一条连接）
/// @note Side-effects: `send` / `open_stream` 发起网络 I/O
class Client {
  public:
    /// @brief 以缺省口径构造：不设物化体上限、TLS 采安全缺省（校验开、无 pin、系统信任库）、缓存关。
    ///
    /// 定义在实现侧：缓存表是一只指向库内类型的共享指针，公共头只知道它的名字，构造点必须看得见全貌。
    Client();

    /// @brief 设定物化应答体的字节上限（`std::nullopt` = 不设限）。
    /// @param value 上限字节数；应答体越限落 `http-body-too-large`。
    /// @return 自身引用，便于链式调用。
    auto max_body_bytes(std::optional<std::uint64_t> value) -> Client & {
        max_body_bytes_ = value;
        return *this;
    }

    /// @brief 查看物化体上限。
    /// @return 已设则返回之；否则 `nullopt`（不限）。
    [[nodiscard]] auto max_body_bytes() const -> std::optional<std::uint64_t> { return max_body_bytes_; }

    /// @brief 设定透明解压产物的字节上限（解压炸弹防护；`std::nullopt` = 跟随 `max_body_bytes()`）。
    ///
    /// 两枚旋钮管的是不同的字节：`max_body_bytes()` 管线上到达的字节，本旋钮管还原之后的字节。缺省跟随
    /// 前者是「压缩不该让调用方拿到比不设限更多」的最小口径；需要收得更紧（或更松）时显式设定。
    /// 越限落 `http-body-too-large`，其 detail 点名越的是本旋钮。
    /// @param value 上限字节数。
    /// @return 自身引用，便于链式调用。
    auto max_decompressed_bytes(std::optional<std::uint64_t> value) -> Client & {
        max_decompressed_bytes_ = value;
        return *this;
    }

    /// @brief 查看解压产物上限的**已设值**（未设即跟随 `max_body_bytes()`）。
    /// @return 已设则返回之；否则 `nullopt`（表示跟随，不表示不限）。
    [[nodiscard]] auto max_decompressed_bytes() const -> std::optional<std::uint64_t> {
        return max_decompressed_bytes_;
    }

    /// @brief 设定 `https://` 握手的安全口径（版本界、ALPN、校验、信任根、pin、客户端身份）。
    ///
    /// 每次改写都推进一枚**配置代次**（拷贝共享的计数器）：连接池开着时，池条目记着「当初按哪一代口径
    /// 握手」，代次对不上即不复用——改了信任根还坐上一条按旧口径握手的连接，等于新配置静默不生效。
    /// @param context TLS 上下文。
    /// @return 自身引用，便于链式调用。
    auto tls_context(TlsContext context) -> Client & {
        tls_ = std::move(context);
        if (tls_generation_ != nullptr) {
            ++(*tls_generation_);
        }
        return *this;
    }

    /// @brief 查看 TLS 口径。
    /// @return 上下文的只读引用。
    [[nodiscard]] auto tls_context() const -> const TlsContext & { return tls_; }

    /// @brief 装帧号源，把进度发射的闸门从「宿主回合」改判为「帧」（配合 `Request::on_progress()`）。
    ///
    /// 发起侧的进度恒是合流后的（每方向在一次可发射的机会里至多一发）。缺省的机会是宿主回合——每次协程经
    /// 主线程投递器恢复算一次；本应用有帧循环时，宿主在此给出单调递增的帧号，同一帧内的多次恢复就只发第一
    /// 发，语义严格等于「每帧至多一次」。传空函数即退回回合闸。
    ///
    /// 帧号源在每次发射判定时被调用一次，故它应当是廉价的读计数器而非计算式；它不触发任何网络动作。
    /// @param source 返回当前帧号的可调用体；空 = 用回合闸（缺省）。
    /// @return 自身引用，便于链式调用。
    auto progress_frame_source(std::function<std::uint64_t()> source) -> Client & {
        progress_frame_source_ = std::move(source);
        return *this;
    }

    /// @brief 查看已装的帧号源。
    /// @return 已装则返回之；否则空函数（表示用回合闸，不表示关掉进度）。
    [[nodiscard]] auto progress_frame_source() const -> const std::function<std::uint64_t()> & {
        return progress_frame_source_;
    }

    /// @brief 装本客户端的缺省重试策略（`Request::retry_policy()` 可逐请求覆盖）。
    ///
    /// 缺省即 `RetryPolicy{}`——`attempts` 为 `1`，**不重试**。策略开着时，每一次尝试都是从原始请求重新
    /// 发起（含重新建连与重跑重定向链），不是「接着上次断的地方再读」；请求体由 `Request` 自持，故可重放。
    /// 判据与两枚时间旋钮的分工见 `RetryPolicy` 的头注释。
    /// @param value 重试策略。
    /// @return 自身引用，便于链式调用。
    auto retry_policy(const RetryPolicy &value) -> Client & {
        retry_ = value;
        return *this;
    }

    /// @brief 查看缺省重试策略。
    /// @return 策略的只读引用。
    [[nodiscard]] auto retry_policy() const -> const RetryPolicy & { return retry_; }

    /// @brief 装本客户端的凭据提供方（缺省空 = 认证环整环不存在）。
    ///
    /// 拿到 401 / 407 时用它答一次挑战、据答复原地重跑那一次尝试；判据、三环次序与「至多注入一次」见
    /// `net/http_auth.h`。这里**只有客户端一级**、没有逐请求旋钮，与 `retry_policy()` 的两级格局刻意不同：
    /// 换取的凭据天然跨请求共享，而单条请求的一次性凭据早已由 `Request` 上直填 `Authorization` 覆盖。
    ///
    /// 提供方是**共享状态**：`Client` 的拷贝共用同一个实例（与缓存表同一格局）。一份 token 服务整段时间内
    /// 的所有交换，拷一次就换一份凭据池既没有道理，也会让并发交换各自去换一份 token。实现方因此须自身线程
    /// 安全——同一实例可被并发调用。
    /// @param value 凭据提供方；空指针 = 关掉本环，401 / 407 一律原样交出。
    /// @return 自身引用，便于链式调用。
    auto authenticator(std::shared_ptr<Authenticator> value) -> Client & {
        authenticator_ = std::move(value);
        return *this;
    }

    /// @brief 查看已装的凭据提供方。
    /// @return 已装则返回之；否则空指针（缺省，表示不答任何挑战）。
    [[nodiscard]] auto authenticator() const -> const std::shared_ptr<Authenticator> & { return authenticator_; }

    /// @brief 装本客户端的链路标识缺省：是否自动补写 `X-Request-Id` 请求字段（缺省**开**）。
    ///
    /// 一次交换取一枚标识，重定向的每一跳与重试的每一次尝试都带同一串——它是「这条请求在对端日志里是哪
    /// 一条」的唯一线索，也是本库把一次逻辑交换与若干次物理往来对上的锚点。取值形态是 `RFC 9562` UUIDv4
    /// 的小写文本形态，熵取库内置的非密码学序列（进程内不重复、跨进程不相关，但**不是安全边界**：需要
    /// 不可预测的关联值请由 `Request::request_id()` 自带）。
    ///
    /// 补齐只发生在「调用方没写过这枚字段」的时候：自己写了 `X-Request-Id` 的请求，本旋钮与请求级旋钮都
    /// 不覆盖它（与 `Accept-Encoding` 的缺省补齐同一纪律）。关掉本旋钮即一个字节都不多发。
    /// @param value 自动补齐为 true；假 = 除非请求自带，否则不发链路标识。
    /// @return 自身引用，便于链式调用。
    auto request_id(bool value) -> Client & {
        request_id_ = value;
        return *this;
    }

    /// @brief 查看链路标识缺省是否开着。
    /// @return 开着为 true（缺省即 true）。
    [[nodiscard]] auto request_id() const -> bool { return request_id_; }

    /// @brief 装本客户端的代理集合（缺省 `ProxyEnvironment::disabled()`：一切目标直连）。
    ///
    /// 每一跳按 `ProxyEnvironment::for_target(host, port, secure)` 现算代理，之后按本头「代理」条的两种交付
    /// 形态走：`https://` 与一切 SOCKS5 经该代理建隧道、在隧道之上按原主机握手与校验；`http://` 经 HTTP 代理
    /// 时把请求交给它转发（绝对形态请求行、连到代理本身）。任何形态都**绝不回退直连**，失败以对应错误收尾。
    /// 集合的装配恒是调用方的显式动作——本库不会自己去读环境变量或系统偏好（那会让同一段代码在不同机器上走
    /// 不同路径），需要时由调用方取 `ProxyEnvironment::from_environment()` 或 `from_environment_or_system()`
    /// 的结果交进来。`NO_PROXY` 例外照常生效：命中例外的目标本次就是 `Direct`，与声明式安全策略的交叉判定见
    /// `net/policy.h`。
    /// @param value 代理集合（含例外清单）。
    /// @return 自身引用，便于链式调用。
    auto proxy(ProxyEnvironment value) -> Client & {
        proxy_ = std::move(value);
        return *this;
    }

    /// @brief 查看已装的代理集合。
    /// @return 集合的只读引用（缺省为空集合 = 一切直连）。
    [[nodiscard]] auto proxy() const -> const ProxyEnvironment & { return proxy_; }

    /// @brief 为本客户端显式声明一份安全策略（覆盖进程级默认）。
    ///
    /// 三级取定：本旋钮设过用它，否则用 `net::default_policy()`，两者都没设即 `NetPolicy{}` 那组保守缺省
    /// （拒一切明文、TLS 下限 1.2、不强求代理）。判据与主机匹配口径见 `net/policy.h`；发起侧在起手与每一跳
    /// 各问一次，被拒的那一跳一条连接都不发起。生效时刻是「交换发起那一刻」，之后改本旋钮或进程级默认都不
    /// 影响在途交换。
    /// @param value 安全策略。
    /// @return 自身引用，便于链式调用。
    auto policy(NetPolicy value) -> Client & {
        policy_ = std::move(value);
        return *this;
    }

    /// @brief 查看本客户端的**已设**策略。
    /// @return 已设则返回之；否则 `nullopt`（表示跟随进程级默认与保守缺省，不表示关掉策略）。
    [[nodiscard]] auto policy() const -> const std::optional<NetPolicy> & { return policy_; }

    /// @brief 装本客户端的内存缓存口径（`CacheOptions{}` 即关掉，缺省就是它）。
    ///
    /// 开着时兑现 `RFC 9111` 的最小集：`GET` / `HEAD` 的应答按「方法 + 目标 URL + `Vary` 点名的请求头取值」
    /// 建键存进表，新鲜副本直接交付且**一条连接都不发起**，陈旧副本挂上 `If-None-Match` / `If-Modified-Since`
    /// 走条件请求，拿到 304 就把新头并进已存应答再交出（`Response::from_cache()` 为真）。`no-store`、
    /// `Vary: *`、不可存的状态码集合、非幂等方法一律不进表。
    ///
    /// 表是**共享状态**：`Client` 的拷贝共用同一张表（各拿一份空表的话，拷贝一次就等于把已付的代价扔掉、
    /// 下一次照样触网，那不是值语义想要的）。关掉开关不清空已有条目——开关管的是「查不查、写不写」，
    /// 要清表请另建一个 `Client`。逐请求绕过用 `Request::no_cache()`。
    /// @param value 缓存口径。
    /// @return 自身引用，便于链式调用。
    auto cache(CacheOptions value) -> Client &;

    /// @brief 查看缓存口径。
    /// @return 口径的只读引用（`enabled` 缺省假 = 不查也不写）。
    [[nodiscard]] auto cache() const -> const CacheOptions & { return cache_options_; }

    /// @brief 装本客户端的连接池口径（`PoolOptions{}` 即关掉，缺省就是它）。
    ///
    /// 开着时兑现 keep-alive 的最小治理集：同池键的请求坐上一条判干净的连接（复用资格五判据：消息完整
    /// 读完、无 leftover、对端未发 `Connection: close`、分帧非 `UntilClose`、HTTP/1.1——缺一即当场关闭），
    /// 空闲超时与最长寿命到期即关，每键并发上限满时排队（等空闲连接的时间计入 `Timeouts::connect` 档，
    /// 到点在 detail 点名「排队等不到」而非「连不上」）。任何一条连接被对端在空闲期关掉时，下一次坐上去
    /// 的失败按普通传输失败交出（幂等方法的同一跳会先换一条新连接重试一次，非幂等不重试）。
    ///
    /// 池是**共享状态**：`Client` 的拷贝共用同一张池（与缓存表同一格局）。`tls_context()` 的每次改写推进
    /// 一枚配置代次，池条目对不上代次即不复用——改了信任根不该悄悄坐旧连接。关掉开关即清空现有空闲连接。
    /// @param value 池口径。
    /// @return 自身引用，便于链式调用。
    auto pool(const PoolOptions &value) -> Client &;

    /// @brief 查看连接池口径。
    /// @return 口径的只读引用（`enabled` 缺省假 = 一次交换一条连接、用完即关）。
    [[nodiscard]] auto pool() const -> const PoolOptions & { return pool_options_; }

    /// @brief 装本客户端的事件流重连口径（`open_sse` 专用；缺省开重连、额度 8 次、窗口初值 3 秒）。
    ///
    /// 与 `pool()` / `cache()` 相反，这里的缺省是**开**：事件流是一条没有结束的流，关掉重连等于把「对端重启、
    /// 网络抖一下」这一类最常见的现实变成宿主自己接的活，而续传标识恰恰只有库知道该在什么时刻带上。
    /// 逐请求由 `Request::sse()` 覆盖。
    /// @param value 重连口径。
    /// @return 自身引用，便于链式调用。
    auto sse(SseOptions value) -> Client &;

    /// @brief 查看事件流重连口径。
    /// @return 口径的只读引用（`reconnect` 缺省真 = 一条流断了就再接一条，直到额度用尽或宿主收摊）。
    [[nodiscard]] auto sse() const -> const SseOptions & { return sse_options_; }

    /// @brief 发送一次请求并物化应答：连接 → 写出 → 逐段装配 → 透明解压 → 按 `Location` 跟随重定向。
    ///
    /// 全程受 `Request::timeout()` 的整请求时限约束（到点落 `http-timeout`）；重定向次数超过
    /// `Request::max_redirects()` 落 `http-redirect-limit`；应答声明了本件解不了的 `Content-Encoding` 落
    /// `http-content-encoding-unsupported`、声明得了但字节过不了该编码落 `http-body-corrupt`。协程出口，
    /// 续体恒回主线程。
    ///
    /// 重试按 `Request::retry_policy()`（缺省取本客户端的 `retry_policy()`，即不重试）逐尝试发起：每一次尝试
    /// 都从原始请求重跑「建连 + 重定向链」，`Timeouts::total` 因此逐尝试重新计时，跨全部尝试的墙是
    /// `RetryPolicy::budget`（到点落 `http-timeout` 并在 detail 点名 budget）。尝试耗尽后交出的仍是最后一次
    /// 那一份事实——非 2xx 是成功的 `Response`，传输失败是最后一枚底层错误码，本件不新造「重试失败」码。
    /// @param request 待发送的请求（其 `render_head()` 的 URL 解析失败、字段畸形在此以错误结局交出；
    ///                未自带 `Accept-Encoding` 时由本件补齐）；按值接管以随协程帧存活，调用后可立即销毁源对象。
    /// @param cancellation 本次交换的取消令牌（共享状态句柄，缺省 = 独立令牌、无人可取消）。
    /// @return 可 `co_await` 的协程任务：成功携物化的 `Response`，失败携对应 `http-*` / `net-*` 错误码。
    /// @note 缓存开着且本请求未 `no_cache()` 时，命中新鲜副本直接返回（`from_cache()` 为真、零连接），
    ///       陈旧副本自动挂条件头重验证，304 用新头更新已存应答后交出。
    /// @note 起手先于查表、其后逐跳各过一遍 `NetPolicy` 三维判定（三级取定见本头「声明式安全策略」段）：
    ///       被拒时落 `net-policy-violated`，一条连接都不发起、一个字节都不写出，缓存命中也不例外。
    /// @note 取消口径见本头「取消经 `Cancellation` 参数进来」条：令牌到手时已取消即一个字节都不发。
    [[nodiscard]] auto send(Request request, Cancellation cancellation = {}) const -> CoroTask<Result<Response>>;

    /// @brief 发送一次请求并**流式**收下应答：字节到手一段就交一段，整条消息不必同时存在于内存。
    ///
    /// 协议语义与 `send` 同源（缺省 `Accept-Encoding` 宣告、编码链反序逐层还原、按 `Location` 跟随重定向、
    /// `Request::timeout()` 的整请求时限、进度节流同判据），四条形态差别另有：
    /// - **完成事实是 `StreamOutcome`**：交出的恒是最终一跳的头部块 + 两枚口径的字节数 + 停读事实，
    ///   `response.body()` 恒为空——体的字节已经逐段交给 `sink`，库内不再留存。
    /// - **线上字节不设限**：`max_body_bytes()` 管的是物化路径的内存，本路径不物化，故它对本入口没有任何
    ///   约束（也不会因为设过它而提前失败）。解压产物照 `max_decompressed_bytes()` 限，越限落
    ///   `http-body-too-large` 且在 detail 点名越的是这一枚旋钮。
    /// - **`deflate` 的两型按包装头判定**：物化路径可以先试 zlib、解不开再回落裸流；本路径的字节一旦交给
    ///   宿主就收不回来，重播等于让宿主重写已写下那段。故只看该环节最前两枚输入字节能否构成合法 zlib 头
    ///   （`RFC 1950 §2.2`），据此把格式锁死到整条流。
    /// - **重定向跳的消息体不读**：头部一判定为重定向即弃该跳载荷、直接发起下一跳——为一注定被丢弃的体继续
    ///   读字节没有意义。`send` 逐跳物化，本入口只看最终一跳；`StreamOutcome::wire_bytes` 仍累计全部跳。
    ///
    /// `sink` 回 `SinkFlow::Stop` 时本次交换以**成功**结局返回（`stopped_by_sink` 为真），剩余字节不再读取
    /// 也不再交付；此时库内不宣告流「完整」，故解压环节的中途收摊不校验尾块。失败结局不调 `on_end`。本腿无
    /// 流式能力（`Engine::capabilities().streaming` 为假，如 wasm）时直接落 `net-platform-unavailable`，
    /// 绝不悄悄退化成物化路径。协程出口，续体恒回主线程。
    ///
    /// 重试同样成立，但资格比物化路径窄（判据见 `Request::retry_policy()`）：**`on_head` 交出即耗尽**——
    /// 宿主一旦见过头部，后续尝试会把已写下那段再写一遍，本件绝不替宿主做那种重播；`stopped_by_sink` 是宿主
    /// 自己收的摊，同样不重放。未交出头部的失败（建连、写出、装配、解压）与可重试的非 2xx 才进退避环。
    ///
    /// 缓存命中时三个入口照常走：`on_head` 交已存的头部块，`on_data` 把已存的（已解压）体逐段交出去，
    /// `on_end` 收尾——宿主看到的交付次序与真从对端取回时一致，区别只在 `StreamOutcome::wire_bytes` 为 0
    /// （本次没有线上字节）而 `delivered_bytes` 是已存体的长度。304 重验证成功同理：线上只有头部块。
    /// @param request 待发送的请求（按值接管以随协程帧存活，调用后可立即销毁源对象）。
    /// @param sink 接收侧宿主；**按引用**交付，其生命周期必须覆盖到协程收尾（`is_done()` 之后）。
    /// @param cancellation 本次交换的取消令牌（共享状态句柄，缺省 = 独立令牌、无人可取消）。
    /// @return 可 `co_await` 的协程任务：成功携 `StreamOutcome`，失败携对应 `http-*` / `net-*` 错误码。
    /// @note 策略判定与 `send` 同一条路：起手先于查表、其后逐跳，被拒落 `net-policy-violated` 且不发起连接。
    /// @note 取消到点在已交付给宿主的那一段之后不产生任何回滚：宿主见到过的字节仍由宿主处置，本件只保证
    ///       不再交付后续字节，且 `on_end` 不被调用（失败结局不收尾，口径与 `sink` 主动停读不同）。
    /// @note Thread: `sink` 的三个入口恒在主线程同步调用，由本协程串行驱动
    /// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): Sink 由调用方持有，帧只借用
    [[nodiscard]] auto open_stream(Request request, DataSink &sink, Cancellation cancellation = {}) const
        -> CoroTask<Result<StreamOutcome>>;

    /// @brief 订阅一次 `text/event-stream`：逐条事件交付，一条流断了就带着续传标识再接一条。
    ///
    /// 本入口住在 `open_stream()` **之上**而不是旁边：分帧、透明解压、重定向、三档超时、连接池、Cookie 与
    /// 策略闸全都由那条腿原样兑现，这里只加两件事——行与字段的语义（判据见 `net/http_sse.h` 与库内
    /// `net/detail/http_sse.h`）与「重连之后还是同一次订阅」这层会话状态。七条形态差别另有：
    /// - **一次调用 = 一个订阅，不是一条交换**：`SseOutcome::exchanges` 随重连增长，`SseSink::on_open` 每条流
    ///   各叫一次（那是宿主唯一能知道「流断过」的出口）。
    /// - **重连窗口只有一个来源**：服务端写的 `retry:` 原样采用，**不封顶、不指数退避、不加抖动**——「这条流
    ///   多久之后该重连」是服务端的事，它是唯一知道自己为什么断了的一方；「本库最多替宿主试几次」是本地
    ///   决定，由 `SseOptions::max_reconnects` 收敛。服务端从未写 `retry:` 才用 `retry_default` 作初值。
    /// - **只有「这条流断了」才重连**：非 2xx 应答、`Content-Type` 不是 `text/event-stream`（允许带参数）、
    ///   单行或单条事件越限三族都是**终止失败**——重连一万次拿到的也是同一个答案，落
    ///   `http-protocol-error` / `net-framer-overflow` / `net-buffer-limit` 且不重连。取消同理（落
    ///   `net-cancelled`，已交付给宿主的事件不回滚）。传输类失败与对端干净收摊算「断了」。
    ///   这条判定**不看 `Error::retryable`**：那枚旗位说的是「同一条请求原样重发有没有意义」（`net-recv-failed`
    ///   因此标假——读了一半的字节流不能续），而订阅问的是另一件事；按旗位筛会把对端砸断连接这种最常见的断流
    ///   形态判成「别再试了」，于是自动重连在最该生效的地方不生效。
    /// - **额度用尽不新造码**：交出的是最后一次交换的真实失败并在 detail 点名额度已尽；最后一次交换本身干净
    ///   时仍是成功结局，由 `SseOutcome::reconnect_exhausted` 表达「我停了，不是因为服务端说完了」。
    /// - **方法只认 GET**：事件流的语义就是一条 GET 出来的长应答，其余方法起手即落 `http-request-invalid`——
    ///   既不静默改成 GET（那会让调用方以为发出去了体），也不照发（那会打出一个本库不认识的协议）。
    /// - **`Last-Event-ID` 只在非空时带**：从未有过 `id:` 字段就没必要发一枚内容为空的头；服务端用空值 id
    ///   表达「清空续传标识」，其线上形态同样是不带。
    /// - **不查也不写缓存表**：`open_stream` 那侧「只读不写」的理由在这里更进一步——一次订阅要的是**此刻之后**
    ///   的事件，一份新鲜的历史副本对它没有任何意义，照着交付反而会让宿主以为收到了新事件。
    ///
    /// `Timeouts` 的三档管的是**单条流**（每（重）建一条流各拿一份），不是整个订阅；会话的收口靠
    /// `cancellation` 与额度。本腿无流式能力（`Engine::capabilities().streaming` 为假，如 wasm）时直接落
    /// `net-platform-unavailable`，绝不退化成「物化一份应答再慢慢切行」——那等于把一条不会结束的流等成一次
    /// 也不会结束的挂死。协程出口，续体恒回主线程。
    /// @param request 待订阅的请求（只认 GET；按值接管以随协程帧存活，调用后可立即销毁源对象）。
    /// @param sink 订阅侧宿主；**按引用**交付，其生命周期必须覆盖到协程收尾（`is_done()` 之后）。
    /// @param cancellation 本次订阅的取消令牌（共享状态句柄，缺省 = 独立令牌、无人可取消）。
    /// @return 可 `co_await` 的协程任务：成功携 `SseOutcome`，失败携对应 `http-*` / `net-*` 错误码。
    /// @note Thread: `sink` 的四个入口恒在主线程同步调用，由本协程串行驱动
    /// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): Sink 由调用方持有，帧只借用
    [[nodiscard]] auto open_sse(Request request, SseSink &sink, Cancellation cancellation = {}) const
        -> CoroTask<Result<SseOutcome>>;

    /// @brief 发起一次 `ws://` / `wss://` 升级握手，成功交出一条自持读环的会话句柄。
    ///
    /// 本入口是 `Client` 三条发起腿里的第三条（`send` 物化、`open_stream` / `open_sse` 交付、本件升级），它
    /// 复用前两条腿的地基而不复用它们的骨架：解析与双栈竞速、代理隧道或转发、TLS 信任根、策略闸、链路标识、
    /// Cookie 附着与收取、`Timeouts` 全部照旧兑现；而应答侧的装配器、重定向环、重试环、缓存表与分帧解码**一律
    /// 不进来**——101 之后这条连接不再是「一次交换」，把 `ResponseAssembler` 套上去它会把 101 当场判成协议违例
    /// （那是 HTTP 侧正确的判断，只是不适用于升级）。握手字节层与判定在 `net/detail/ws_handshake.h`，会话状态
    /// 在 `net/detail/ws_session.h`，形态约束的完整口径见 `net/websocket.h` 的文件头。
    ///
    /// 六条与另两腿不同的落点（规范性）：
    /// - **方法只认 `GET`**：其余方法起手即落 `http-request-invalid`，既不静默改成 `GET`（那会让调用方以为
    ///     发出去了体），也不照发（那会打出一个本库不认识的协议）。
    /// - **不重试、不跟随重定向**：一条已经升级的连接没法「换个地址再升一次」，而 `RetryPolicy` 的「原样重发」
    ///     在这里意味着再打一枚 `Sec-WebSocket-Key` 去问同一件事——服务端把它当成两次独立握手是常态。3xx 与非
    ///     2xx 同样由 `validate_ws_handshake()` 点名状态码后落 `ws-handshake-rejected`。
    /// - **不进连接池**：句柄是这条连接的唯一持有者，池既拿不到它也收不回它；`PoolOptions` 在本腿上无事可管，
    ///     握手一律起新连接。复用与「同一条长连接」在两处说的是相反的事。
    /// - **不查也不写缓存表**：一次升级要的是此刻之后的帧，一份历史副本对它没有意义。
    /// - **超时与取消管到握手结算为止**：`Timeouts::connect` 照旧管建连（含 TLS 握手），`Timeouts::total` 管
    ///     整段握手（写出请求到读出 `101`）；`Timeouts::read_idle` 在本腿上无事可管——升级后的读环属于会话而不
    ///     属于这次交换，它的时限由 `WebSocketOptions` 的两枚心跳旋钮表达。令牌到点或被取消只影响握手，不会
    ///     掐一条宿主正在使用的会话（口径同 `net/websocket.h`）。
    /// - **`101` 之后读多的字节属于帧流**：一次读不会刚好停在头部块的边界上，那一段原样交给会话先喂分帧器，
    ///     既不丢半条消息，也不把它当成「上一条消息的余货」判成畸形。
    ///
    /// 本腿无裸流式能力（`Engine::capabilities().streaming` 为假，如 wasm）时起手即落 `net-platform-unavailable`
    /// （裁决见提案 §16 第 3 项：浏览器 `fetch` 那侧连可读的 socket 都不给，要接原生 `WebSocket` 是另一件事）。
    /// 协程出口，续体恒回主线程。
    /// @param request 待升级的请求（只认 `GET`；按值接管以随协程帧存活，调用后可立即销毁源对象）。
    /// @param sink 会话侧宿主；**按引用**交付，其生命周期必须覆盖到会话收尾——即句柄析构或 `on_closed` 之后。
    ///             握手未成时本入口**不调用它的任何一口**（含 `on_closed`）：那次会话从未存在过，失败事实全部
    ///             由返回的 `Result` 表达，两条通知路若各报一次，宿主就会在同一次调用里收到两个终态。
    /// @param options 三枚上限、子协议与心跳两档（读定一次，此后改旋钮不影响已交出的会话）。
    /// @param cancellation 握手段的取消令牌（缺省 = 独立令牌、无人可取消；结算后不再驱动会话）。
    /// @return 可 `co_await` 的协程任务：成功携 `WebSocket` 句柄，失败携对应 `ws-*` / `http-*` / `net-*` 错误码。
    /// @note Thread: `sink` 的四个入口恒在主线程同步调用，由会话的读环串行驱动
    /// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): Sink 由调用方持有，帧只借用
    [[nodiscard]] auto connect_websocket(Request request, WebSocketSink &sink, WebSocketOptions options = {},
                                         Cancellation cancellation = {}) const -> CoroTask<Result<WebSocket>>;

  private:
    std::optional<std::uint64_t> max_body_bytes_;  ///< 物化体上限（nullopt = 不限）
    std::optional<std::uint64_t> max_decompressed_bytes_;  ///< 解压产物上限（nullopt = 跟随上一行）
    TlsContext tls_;  ///< https 握手口径
    ProxyEnvironment proxy_;  ///< 代理集合（缺省空 = 一切直连；逐跳由 `for_target` 现算）
    std::optional<NetPolicy> policy_;  ///< 已设的安全策略（未设 = 跟随进程级默认与保守缺省）
    std::function<std::uint64_t()> progress_frame_source_;  ///< 进度发射的帧号源（空 = 回合闸）
    RetryPolicy retry_;  ///< 缺省重试策略（attempts=1 即关闭）；逐请求由 `Request::retry_policy()` 覆盖
    std::shared_ptr<Authenticator> authenticator_;  ///< 凭据提供方（空 = 认证环不存在）；拷贝的 Client 共用同一个

    bool request_id_{true};  ///< 链路标识缺省：自动补 `X-Request-Id`（逐请求由 `Request::request_id()` 定）
    CacheOptions cache_options_;  ///< 缓存口径（缺省关）；逐请求由 `Request::no_cache()` 越
    std::shared_ptr<detail::HttpResponseCache> cache_;  ///< 共享的缓存表；拷贝的 Client 共用同一张
    PoolOptions pool_options_;  ///< 连接池口径（缺省关）
    SseOptions sse_options_;  ///< 事件流重连口径（缺省开）；逐请求由 `Request::sse()` 覆盖
    std::shared_ptr<detail::HttpConnectionPool> pool_;  ///< 共享的连接池；拷贝的 Client 共用同一张
    std::shared_ptr<std::uint64_t> tls_generation_;  ///< TLS 配置代次（拷贝共享计数；`tls_context()` 每次改写自增）
    std::shared_ptr<detail::HttpCookieJar> cookie_jar_;  ///< Cookie Jar（空 = 裸 Client 不收不发；只有 Session 会装）

    // 会话面是唯一给本客户端装 Cookie Jar 的地方（`Session` 持有 Jar，`client()` 交出的引用挂它）。
    friend class Session;
};

}  // namespace aurora::net::http
