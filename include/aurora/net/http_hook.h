#pragma once

/// @brief HTTP 交换钩子：客户端级登记链、只读请求视图、头改写通道与 `send` 上的短路交付。
/// @file net/http_hook.h
///
/// 本头回答一个问题：**「一次交换起手时，宿主还有没有话要说；应答落定时，它还想不想看」**。钩子是同步的链上
/// 回调，不是 OkHttp 那种可替换整条链的 `Interceptor`——裁决权留在库内，钩子只拿得到两件事：一枚**只读**的
/// 请求视图，和一枚**只改请求头**的写通道。方法、目标、消息体、`BodySource`、`range` / `if_range` /
/// `expect_continue` / `timeout` 与 TLS / 代理口径都不在它够得着的地方；这一收窄由类型兑现，而不是由注释兑现。
///
/// 四个用途与分工写在案上（完整表见 `net/http_client.h` 的「交换钩子」段与规格）：
/// - **每请求补同一组静态字段**（API key、租户头）——本件的本职；
/// - **挑战-答复型凭据**（401 / 407）——`Authenticator`，钩子写下的 `Authorization` 会被认证环的答复覆盖；
/// - **能不能谈**（明文白名单、TLS 下限、必须经代理）——`NetPolicy`，闸在钩子**之前**判且钩子改不动它读的
///   任何事实；
/// - **逐跳耗时与失败归因**——可观测性切片，那面是只读推模型；本件是链上的拉模型，能改写、能决定这次不触网。
///
/// 三条硬约束（规范性）：
/// - **两问都同步跑在推进本次交换的协程所在线程上**：不得阻塞、不得做 I/O、不得在非主线程直接触碰 UI，也不得
///   持有 `Client`（链 → 钩子 → `Client` → 链是引用环；需要发起交换时用另一枚不挂同一枚钩子的客户端）。
///   不同交换可能并发问同一枚钩子，实现须自行保证线程安全。
/// - **短路是终点**：`before_request` 交出的应答一经承认，本次交换立即收摊——不查缓存、不进缓存、不更新
///   Cookie Jar、不跟随重定向、不进重试环、不发进度、不再解压。短路只在 `ExchangeLeg::Send` 上成立；其余
///   三条腿交出短路值即整次落 `http-request-invalid`（静默忽略会让宿主的替身「以为 mock 了，实际打到了真网」）。
/// - **保留头不可触碰**：`Host` / `X-Request-Id` / 分帧与体形态 / 逐跳 / 范围与预检 / `Proxy-Authorization`
///   以及 WebSocket 握手四字段各自把着一条库内不变量，钩子新增、删除或改值即整次落 `http-request-invalid`；
///   调用方自带的同名值只要钩子没碰就照常上线。
///
/// @code
/// class ApiKeyHook final : public aurora::net::http::ExchangeHook {
///   public:
///     auto before_request(const aurora::net::http::ExchangeRequestView &,
///                         aurora::Headers &headers) const
///         -> std::optional<aurora::Result<aurora::net::http::Response>> override {
///         headers.set("X-Api-Key", key_);
///         return std::nullopt;  // 照常发起
///     }
///   private:
///     std::string key_;
/// };
/// auto handle = client.use(std::make_shared<ApiKeyHook>(key));
/// @endcode
/// @note Thread: 实现方的两问恒在发起侧续体所在线程同步调用；登记的增删可在任意线程
/// @note Side-effects: 由实现方决定（本头只定义契约；交换在网络上的收发仍由 `Client` 发起）

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/http.h"

namespace aurora::net::detail {
class ExchangeHookChain;  ///< 库内链内核：视图的唯一构造点、登记句柄的摘除口
}  // namespace aurora::net::detail

namespace aurora::net::http {

/// @brief 一次交换发起于哪条腿——钩子据此判断自己的短路资格与字段口径是否适用。
/// @note Thread: thread-safe
/// @note Side-effects: none
/// @note 豁免 performance-enum-size：公共 API 枚举，落在视图的单个字段而非密集数组；底层类型属公共 API 形态。
/// NOLINTNEXTLINE(performance-enum-size)
enum class ExchangeLeg : std::uint8_t {
    Send,  ///< `Client::send`：物化应答；唯一承认短路的腿
    Stream,  ///< `Client::open_stream`：逐段交给 `DataSink`
    ServerSentEvents,  ///< `Client::open_sse`：每次（重）连各问一次
    WebSocket,  ///< `Client::connect_websocket`：只问握手那一跳
};

/// @brief 一次交换的只读请求视图：方法、目标、当前头与腿。
///
/// 视图是**本枚钩子回调入口时刻的快照**：本枚钩子经 `headers` 写通道落下的字段不会出现在它自己的视图里，
/// 但会出现在后续钩子的视图里（每一枚各取一份）。视图只在回调期间有效，不得留存、不得跨线程传递；它删去
/// 拷贝与移动正是为了让这条约束在类型上成立。
///
/// 方法、目标、体、协议旋钮与超时都不在视图上——本件只允许改写请求头。字段表里也看不到库内稍后补的条件头
/// （`If-None-Match` / `If-Modified-Since`）、`Accept-Encoding`、`Host` 与分帧字段：它们要么在钩子链之后才
/// 补上，要么由 `render_head()` 现地写定。
/// @note Thread: 回调期间只读；不得跨线程传递
/// @note Side-effects: none
class ExchangeRequestView {
  public:
    ExchangeRequestView(const ExchangeRequestView &) = delete;
    auto operator=(const ExchangeRequestView &) -> ExchangeRequestView & = delete;
    ExchangeRequestView(ExchangeRequestView &&) = delete;
    auto operator=(ExchangeRequestView &&) -> ExchangeRequestView & = delete;
    /// @brief 析构：视图不持有链上任何对象，默认析构即足。
    ~ExchangeRequestView() = default;

    /// @brief 请求方法。
    /// @return 方法枚举。
    [[nodiscard]] auto method() const noexcept -> Method;

    /// @brief 目标 URL。
    /// @return 目标；四条腿都在目标闸之后才问钩子，故解析成功时恒有值。类型与 `Request::url()` 同形，
    ///         使「解析失败由 `render_head()` 报」这条既有口径在视图上不需要第二份表达。
    [[nodiscard]] auto url() const noexcept -> const std::optional<Url> &;

    /// @brief 本枚钩子入口时刻的请求头（只读）。
    /// @return 字段表的只读引用（含调用方自写、链路标识与已附着的 Cookie；不含库内稍后补的条件头、
    ///         `Accept-Encoding`、`Host` 与分帧字段）。
    [[nodiscard]] auto headers() const noexcept -> const Headers &;

    /// @brief 本腿。
    /// @return 腿枚举。
    [[nodiscard]] auto leg() const noexcept -> ExchangeLeg;

  private:
    friend class aurora::net::detail::ExchangeHookChain;  ///< 库内链内核：视图的唯一构造点
    ExchangeRequestView(Method method, std::optional<Url> url, Headers headers, ExchangeLeg leg);
    Method method_;  ///< 方法
    std::optional<Url> url_;  ///< 目标
    Headers headers_;  ///< 入口时刻的字段表快照
    ExchangeLeg leg_;  ///< 腿
};

/// @brief 挂在 `Client` 上的交换钩子：同步、只改请求头、可在 `send` 上短路整次交换。
///
/// 两问的调用时机与配对规则见 `Client::use()` 与规格；实现方须**不阻塞、不做 I/O、不抛异常、不持有
/// `Client`**，并在并发交换下自行保证线程安全。
/// @note Thread: 两问由发起侧在续体所在线程同步调用；同一实例可能被不同交换并发问
/// @note Side-effects: 由实现方决定
class ExchangeHook {
  public:
    ExchangeHook() = default;
    ExchangeHook(const ExchangeHook &) = delete;
    auto operator=(const ExchangeHook &) -> ExchangeHook & = delete;
    ExchangeHook(ExchangeHook &&) = delete;
    auto operator=(ExchangeHook &&) -> ExchangeHook & = delete;
    virtual ~ExchangeHook() = default;

    /// @brief 起手问：经 `headers` 就地改写请求头；`nullopt` = 照常发起，有值 = 本次交换以此收摊。
    ///
    /// 触发时机是取消判定、重试形态校验、链路标识、Cookie 附着与策略闸**之后**，缓存查表与重试环**之前**。
    /// 被策略拒绝的目标一次都不会问到本问；起手已取消的令牌同理。
    ///
    /// 短路语义是终点：交出的应答照原样成为调用方的结局；不进缓存、不更新 Cookie Jar、不跟随重定向、不进
    /// 重试环、不发进度、不再解压。只有 `ExchangeLeg::Send` 承认短路值，其余三腿交出有值即整次落
    /// `http-request-invalid`。短路成功应答的 `from_cache()` 必须为假（钩子不能伪造一枚缓存命中）。
    /// 抛异常即整次落 `http-request-invalid`，链上其余钩子的 `after_response` 一概不问。
    /// @param request 本次交换的只读视图（本枚钩子入口时刻的快照）。
    /// @param headers 请求字段的写通道；只可改头，且不得触碰保留头（改了就整次落 `http-request-invalid`）。
    /// @return 空 = 继续；有值 = 短路。
    [[nodiscard]] virtual auto before_request(const ExchangeRequestView &request, Headers &headers) const
        -> std::optional<Result<Response>> = 0;

    /// @brief 收摊问：只在成功交出一枚应答时问（线上应答 / 新鲜缓存副本 / 本链短路成功）。
    ///
    /// 失败、取消、策略拒绝与三条流式腿（它们的收摊值不是 `Response`）一律不问。收摊视图是钩子改写后的原始
    /// 逻辑请求：方法、目标与字段表都不含库内补的条件头 / `Accept-Encoding` / `Host` / 分帧字段；跨 origin
    /// 重定向后仍是原始目标而不是最后一跳。本回调抛出的异常被捕获并记一行 WARN，应答照常交付，链上其余钩子
    /// 照常问。
    /// @param request 同一个逻辑请求的收摊视图。
    /// @param response 已物化、已解压的应答；短路成功时是钩子交出的那一份。
    virtual auto after_response(const ExchangeRequestView &request, const Response &response) const -> void;
};

/// @brief 登记句柄：move-only RAII，析构即从链上摘除本次登记。
///
/// 句柄不延长链的生命期：所有 `Client` 都没了，链即销毁，句柄析构为空操作。忽略 `use()` 的返回值等于登记后
/// 立刻析构句柄、立刻摘除——要留住登记就必须留住返回的句柄。
/// @note Thread: 句柄本身一次只由一个线程操作；链上的登记与摘除可在任意线程调用
/// @note Side-effects: 析构与 `clear()` 从链上摘除登记
class ExchangeHookRegistration {
  public:
    /// @brief 默认构造出一枚空句柄：析构与 `clear()` 均为空操作。
    ExchangeHookRegistration() = default;

    /// @brief 析构：从链上摘除本次登记（若句柄非空且链仍在），重复摘除幂等。
    ~ExchangeHookRegistration();
    ExchangeHookRegistration(const ExchangeHookRegistration &) = delete;
    auto operator=(const ExchangeHookRegistration &) -> ExchangeHookRegistration & = delete;

    /// @brief 移动构造：把摘除责任交给目标句柄，源句柄转为空句柄。
    /// @param other 被接管的句柄（移动后为空）。
    ExchangeHookRegistration(ExchangeHookRegistration &&other) noexcept;

    /// @brief 移动赋值：先摘除本句柄原有的登记，再接管 `other` 的责任。
    /// @param other 被接管的句柄（移动后为空）。
    /// @return 本句柄。
    auto operator=(ExchangeHookRegistration &&other) noexcept -> ExchangeHookRegistration &;

    /// @brief 提前摘除；重复调用与析构并存不双重摘除。
    auto clear() -> void;

  private:
    friend class aurora::net::detail::ExchangeHookChain;
    ExchangeHookRegistration(std::weak_ptr<aurora::net::detail::ExchangeHookChain> chain, std::uint64_t id);
    std::weak_ptr<aurora::net::detail::ExchangeHookChain> chain_;  ///< 链（不持有）
    std::uint64_t id_{0};  ///< 本次登记的编号；`0` = 空句柄
};

}  // namespace aurora::net::http
