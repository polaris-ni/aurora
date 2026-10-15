#pragma once

/// @brief HTTP/1.1 发起侧客户端（CHG-010 §6 的 HTTP 发起侧、§12 语义的地基件）。
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
/// - **一次 `send` = 一条连接的生命周期**：连接池、keep-alive 复用、`Session` 的 Cookie Jar 属后续切片，本
///   件每次为（每一跳）新建连接、应答物化后关闭，绝不为「复用」预先牺牲正确性。
/// - **超时以 `Request::timeout()` 为准**：它是整请求时限（跨连接、TLS、读写、重定向全程），到点落
///   `http-timeout`；分段超时（`connect` / `read_idle`）与 `RetryPolicy` / 缓存 / 认证属 §12 其余语义件。
///
/// 内容编码（透明解压）在发起侧管线内完成，`Response` 交出的恒是还原后的字节：
/// - **宣告与支持集同源**：构建编入解压能力时，`send` 替调用方补 `Accept-Encoding: gzip, deflate`；调用方
///   已自带该字段（任意大小写拼写）则**原样保留、不覆盖也不重复追加**——协商是调用方的表达，本件只在缺省时
///   补一个与自己解码能力等长的宣告。未编入（`AURORA_ENABLE_COMPRESSION=OFF`）时**连宣告都不发**，此时
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

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "aurora/core/result.h"
#include "aurora/net/http.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/tls.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::http {

/// @brief HTTP 发起侧客户端：把 `Request` 送出去、把物化的 `Response` 拿回来。
/// @note Thread: thread-safe（可拷贝、可并发发起；每次 `send` / `open_stream` 自持一条连接）
/// @note Side-effects: `send` / `open_stream` 发起网络 I/O
class Client {
  public:
    /// @brief 以缺省口径构造：不设物化体上限、TLS 采安全缺省（校验开、无 pin、系统信任库）。
    Client() = default;

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
    /// @param context TLS 上下文。
    /// @return 自身引用，便于链式调用。
    auto tls_context(TlsContext context) -> Client & {
        tls_ = std::move(context);
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

    /// @brief 发送一次请求并物化应答：连接 → 写出 → 逐段装配 → 透明解压 → 按 `Location` 跟随重定向。
    ///
    /// 全程受 `Request::timeout()` 的整请求时限约束（到点落 `http-timeout`）；重定向次数超过
    /// `Request::max_redirects()` 落 `http-redirect-limit`；应答声明了本件解不了的 `Content-Encoding` 落
    /// `http-content-encoding-unsupported`、声明得了但字节过不了该编码落 `http-body-corrupt`。协程出口，
    /// 续体恒回主线程。
    /// @param request 待发送的请求（其 `render_head()` 的 URL 解析失败、字段畸形在此以错误结局交出；
    ///                未自带 `Accept-Encoding` 时由本件补齐）；按值接管以随协程帧存活，调用后可立即销毁源对象。
    /// @return 可 `co_await` 的协程任务：成功携物化的 `Response`，失败携对应 `http-*` / `net-*` 错误码。
    [[nodiscard]] auto send(Request request) const -> CoroTask<Result<Response>>;

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
    /// @param request 待发送的请求（按值接管以随协程帧存活，调用后可立即销毁源对象）。
    /// @param sink 接收侧宿主；**按引用**交付，其生命周期必须覆盖到协程收尾（`is_done()` 之后）。
    /// @return 可 `co_await` 的协程任务：成功携 `StreamOutcome`，失败携对应 `http-*` / `net-*` 错误码。
    /// @note Thread: `sink` 的三个入口恒在主线程同步调用，由本协程串行驱动
    /// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): Sink 由调用方持有，帧只借用
    [[nodiscard]] auto open_stream(Request request, DataSink &sink) const -> CoroTask<Result<StreamOutcome>>;

  private:
    std::optional<std::uint64_t> max_body_bytes_;  ///< 物化体上限（nullopt = 不限）
    std::optional<std::uint64_t> max_decompressed_bytes_;  ///< 解压产物上限（nullopt = 跟随上一行）
    TlsContext tls_;  ///< https 握手口径
    std::function<std::uint64_t()> progress_frame_source_;  ///< 进度发射的帧号源（空 = 回合闸）
};

}  // namespace aurora::net::http
