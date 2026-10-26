// remote_connect.cpp —— 解析之后的连接编排：双栈竞速与代理隧道（仅库实现可见）。
#include "aurora/net/detail/remote_connect.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.gen.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/proxy_wire.h"
#include "aurora/net/engine.h"
#include "aurora/net/resolver.h"
#include "aurora/state/async.h"

namespace aurora::net::detail {

namespace {

/// @brief 握手缓冲的分轮增量（字节）：按此粒度长到头部块上限为止，不一次性预留 16 KiB。
constexpr std::size_t AURORA_HANDSHAKE_CHUNK = std::size_t{2} * 1024;

/// @brief SOCKS5 两段定长回报的字节数：方法协商 `VER METHOD` 与子协商 `VER STATUS` 恒为 2 字节。
constexpr std::size_t AURORA_SOCKS_SHORT_REPLY = 2;

/// @brief 需要在发起线程上执行的一段编排动作。
using Work = std::function<void()>;

/// @brief 把动作归队到发起线程：已在该线程就就地执行，否则走主线程投递器。
///
/// 与 `transport.cpp` 的 `deliver` 同一条通道、同一处局限：宿主从工作线程发起时以投递器的落点为准。
/// @param origin 发起线程标识。
/// @param work 待执行动作（接管所有权）。
auto run_on_origin(std::thread::id origin, Work work) -> void {
    if (std::this_thread::get_id() == origin) {
        work();
        return;
    }
    aurora::detail::post_to_main(std::move(work));
}

/// @brief 构造「入参不合用」形态的失败（端口 0、空主机名）。
/// @param detail 现场细节。
/// @return `net-endpoint-invalid` 错误。
auto invalid_reason(const std::string &detail) -> Error { return make_error(ErrorCode::NetEndpointInvalid, detail); }

/// @brief 构造「隧道没能 usable」形态的失败（对端提前关闭、头部块越上限）。
/// @param detail 现场细节。
/// @return `net-proxy-protocol` 错误。
auto protocol_reason(const std::string &detail) -> Error { return make_error(ErrorCode::NetProxyProtocol, detail); }

/// @brief 组装诊断文本里的 `host:port`（绝不含凭据，与 `wire` 层同口径）。
/// @param host 主机文本。
/// @param port 端口。
/// @return `host:port` 形态文本。
auto host_port(std::string_view host, std::uint16_t port) -> std::string {
    return std::string{host} + ":" + std::to_string(port);
}

/// @brief 一次竞速的全部编排状态。
///
/// 只在发起线程上转换（所有外来入口先经 `run_on_origin` 归队），故无互斥量：交错定时器与到期时限
/// 都从引擎线程归队后才动这张表，同一条线程上的动作天然串行。
struct Race {
    std::vector<Endpoint> candidates;  ///< 竞速次序的候选（已剔除本机 IPC 形态）
    StreamOptions options;  ///< 缓冲、超时与交错窗口
    Cancellation cancellation;  ///< 调用方的取消令牌，逐条传给每个候选
    ConnectDone on_done;  ///< 唯一一次交付的出口（claimed 后移空）
    std::thread::id origin = std::this_thread::get_id();  ///< 发起线程
    std::size_t started = 0;  ///< 已发起的候选条数（也等于下一条的序号）
    std::size_t outstanding = 0;  ///< 已发起且尚未出结果的条数
    bool claimed = false;  ///< `on_done` 已被某次成功或最终失败占用
    std::optional<Error> first_error;  ///< 最早那条失败原因（全败时据此收尾）
};

/// @brief 竞速状态的共享指针（每个在途候选各持一份强引用，故最后一条结果落地才释放）。
using RacePointer = std::shared_ptr<Race>;

/// @brief 交付竞速结果：先到先得，迟到的那份由局部对象析构关闭（不产生第二次交付）。
/// @param race 竞速状态。
/// @param outcome 待交付结果。
auto finish_race(const RacePointer &race, Result<StreamTransport> outcome) -> void {
    if (race->claimed) {
        return;
    }
    race->claimed = true;
    const auto done = std::move(race->on_done);
    if (done) {
        done(std::move(outcome));
    }
}

/// @brief 全部候选出结果且无人建成时收尾（携最早那条失败原因，保留现场的第一手信息）。
/// @param race 竞速状态。
auto settle_race(const RacePointer &race) -> void {
    const auto reason = race->first_error.value_or(invalid_reason("no connectable endpoint for the resolved name"));
    finish_race(race, Result<StreamTransport>{reason});
}

auto on_attempt(const RacePointer &race, Result<StreamTransport> outcome) -> void;

/// @brief 发起第 `index` 条候选（须恰等于已发起条数，否则视为重复发起直接返回）。
/// @param race 竞速状态。
/// @param index 候选序号。
auto launch(const RacePointer &race, std::size_t index) -> void {
    if (index != race->started || index >= race->candidates.size()) {
        return;
    }
    ++race->started;
    // 交错窗口只在还剩候选时有意义。定时器落不到引擎就退化为「不交错」：后续候选仍由失败快跟发起，
    // 竞速的时限判据本来由每条尝试自带的 connect_timeout 负责，故这里不额外报错。
    if (index + 1 < race->candidates.size() && race->options.race_stagger.count() > 0) {
        const auto origin = race->origin;
        std::weak_ptr<Race> witness = race;
        const auto ignored =
            Engine::instance().post_after(race->options.race_stagger, [witness, index, origin]() -> void {
                const auto held = witness.lock();
                if (!held) {
                    return;
                }
                run_on_origin(origin, [held, index]() -> void {
                    // 快跟可能已把这条发出去了：序号不吻合即说明窗口已被抢占，本定时器无事可做。
                    if (held->claimed || held->started != index + 1) {
                        return;
                    }
                    launch(held, held->started);
                });
            });
        (void)ignored;
    }
    ++race->outstanding;
    StreamTransport::connect(race->candidates[index], race->options, race->cancellation,
                             [race](Result<StreamTransport> outcome) { on_attempt(race, std::move(outcome)); });
}

/// @brief 一条候选出结果（成功、失败、超时、取消都走此处，恰一次）。
/// @param race 竞速状态。
/// @param outcome 该候选结果。
auto on_attempt(const RacePointer &race, Result<StreamTransport> outcome) -> void {
    if (race->outstanding > 0) {
        --race->outstanding;
    }
    if (outcome) {
        finish_race(race, std::move(outcome));
        return;
    }
    if (!race->first_error) {
        race->first_error = outcome.error();
    }
    if (race->claimed) {
        return;
    }
    // 一族失败不阻塞另一族：还有没发起的候选就立刻补上，不等交错窗口。
    if (race->started < race->candidates.size()) {
        launch(race, race->started);
        return;
    }
    if (race->outstanding == 0) {
        settle_race(race);
    }
}

/// @brief 一次代理隧道的编排状态（连上代理后的握手状态机）。
struct Tunnel {
    /// @brief 握手阶段：读阶段名即「当前缓冲该交给哪个解析口」。
    enum class Stage : std::uint8_t {
        ProxyLink,  ///< 代理连接尚未建立
        HttpHeadRead,  ///< 等 CONNECT 应答的头部块
        SocksMethodRead,  ///< 等 SOCKS5 方法协商回报
        SocksAuthRead,  ///< 等用户名 / 密码子协商回报
        SocksReplyRead,  ///< 等 SOCKS5 CONNECT 应答
        Done,  ///< 已收尾（成功或失败）
    };

    ProxyConfig proxy;  ///< 代理配置
    std::string target;  ///< 目标的 `host:port` 文本（诊断用，不含凭据）
    std::string host;  ///< 目标主机文本（交给代理的那份）
    std::uint16_t port = 0;  ///< 目标端口
    ConnectDone on_done;  ///< 唯一一次交付的出口
    std::thread::id origin = std::this_thread::get_id();  ///< 发起线程
    std::optional<StreamTransport> stream;  ///< 代理连接（建成后填入，即隧道本体）
    std::vector<char> buffer;  ///< 握手字节缓冲；扩容只在无在途读时发生，故读请求的 span 恒有效
    std::size_t filled = 0;  ///< 缓冲里已读入的字节数
    std::size_t offset = 0;  ///< 已交出的握手字节数：SOCKS5 是多段报文，不前移就会把上一段当本段重读
    Stage stage = Stage::ProxyLink;  ///< 当前阶段
    bool claimed = false;  ///< `on_done` 已占用
    /// @brief 握手两侧的补充事实（借用：调用方持有它到 `on_done` 落地；空 = 不参与，绝大多数调用的形态）。
    ProxyNegotiation *negotiation = nullptr;
};

/// @brief 隧道状态的共享指针（每个在途读写回调各持一份强引用）。
using TunnelPointer = std::shared_ptr<Tunnel>;

/// @brief 交付隧道结果：先到先得，迟到的成功那份随局部对象析构关闭。
/// @param tunnel 隧道状态。
/// @param outcome 待交付结果。
auto finish_tunnel(const TunnelPointer &tunnel, Result<StreamTransport> outcome) -> void {
    if (tunnel->claimed) {
        return;
    }
    tunnel->claimed = true;
    tunnel->stage = Tunnel::Stage::Done;
    const auto done = std::move(tunnel->on_done);
    if (done) {
        done(std::move(outcome));
    }
}

auto parse_handshake(const TunnelPointer &tunnel) -> void;

/// @brief 交回头部块之后的字节并把隧道本体交付出去。
/// @param tunnel 隧道状态。
/// @param message_bytes 本段握手报文占用的字节数（从已交出的偏移起算，不含更早阶段的字节）。
auto complete_handshake(const TunnelPointer &tunnel, std::size_t message_bytes) -> void {
    if (!tunnel->stream) {
        finish_tunnel(tunnel, Result<StreamTransport>{protocol_reason("proxy handshake finished without a stream")});
        return;
    }
    const std::size_t consumed = tunnel->offset + message_bytes;
    if (tunnel->filled > consumed) {
        // 应答解析只吃掉自身长度；多读到的那段属于隧道，接回读队列前端才不会凭空丢字节。
        seed_read(*tunnel->stream, std::span<const char>(tunnel->buffer).subspan(consumed, tunnel->filled - consumed));
    }
    finish_tunnel(tunnel, Result<StreamTransport>{std::move(*tunnel->stream)});
}

/// @brief 发起一轮握手读（缓冲不足先按分轮增量扩容，到上限仍不完整即按畸形收尾）。
/// @param tunnel 隧道状态。
auto start_read(const TunnelPointer &tunnel) -> void {
    if (!tunnel->stream) {
        finish_tunnel(tunnel, Result<StreamTransport>{protocol_reason("proxy handshake has no stream to read")});
        return;
    }
    if (tunnel->buffer.size() == tunnel->filled) {
        if (tunnel->buffer.size() >= wire::AURORA_PROXY_MAX_HEAD_BYTES) {
            finish_tunnel(tunnel, Result<StreamTransport>{protocol_reason(
                                      "proxy handshake exceeded " + std::to_string(wire::AURORA_PROXY_MAX_HEAD_BYTES) +
                                      " bytes without completing: " + tunnel->target)});
            return;
        }
        tunnel->buffer.resize(
            std::min(tunnel->buffer.size() + AURORA_HANDSHAKE_CHUNK, wire::AURORA_PROXY_MAX_HEAD_BYTES));
    }
    tunnel->stream->read(
        std::span<char>(tunnel->buffer).subspan(tunnel->filled), [tunnel](Result<std::size_t> outcome) {
            if (!outcome) {
                finish_tunnel(tunnel, Result<StreamTransport>{outcome.error()});
                return;
            }
            if (outcome.value() == 0) {
                finish_tunnel(tunnel,
                              Result<StreamTransport>{protocol_reason(
                                  "proxy closed the connection before completing the handshake: " + tunnel->target)});
                return;
            }
            tunnel->filled += outcome.value();
            parse_handshake(tunnel);
        });
}

/// @brief 写出一段握手字节并转入下一阶段。
/// @param tunnel 隧道状态。
/// @param bytes 待写字节（写入即复制进待发队列，本函数返回后可销毁）。
/// @param next 写成功后的读阶段。
auto send_handshake(const TunnelPointer &tunnel, std::vector<char> bytes, Tunnel::Stage next) -> void {
    if (!tunnel->stream) {
        finish_tunnel(tunnel, Result<StreamTransport>{protocol_reason("proxy handshake has no stream to write")});
        return;
    }
    tunnel->stage = next;
    tunnel->stream->write(std::span<const char>(bytes), [tunnel](const Result<std::size_t> &outcome) {
        if (!outcome) {
            finish_tunnel(tunnel, Result<StreamTransport>{outcome.error()});
            return;
        }
        start_read(tunnel);
    });
}

/// @brief 组装并写出 SOCKS5 的 CONNECT 请求（方法协商与子协商都可能落到这一步）。
/// @param tunnel 隧道状态。
auto send_socks_connect(const TunnelPointer &tunnel) -> void {
    auto request = wire::make_socks5_connect(tunnel->host, tunnel->port, tunnel->proxy.resolves_via_proxy());
    if (!request) {
        finish_tunnel(tunnel, Result<StreamTransport>{request.error()});
        return;
    }
    send_handshake(tunnel, std::vector<char>(request.value().begin(), request.value().end()),
                   Tunnel::Stage::SocksReplyRead);
}

/// @brief 组装并写出 SOCKS5 用户名 / 密码子协商请求。
/// @param tunnel 隧道状态。
auto send_socks_auth(const TunnelPointer &tunnel) -> void {
    auto request = wire::make_socks5_auth_request(tunnel->proxy.credentials());
    if (!request) {
        finish_tunnel(tunnel, Result<StreamTransport>{request.error()});
        return;
    }
    send_handshake(tunnel, std::vector<char>(request.value().begin(), request.value().end()),
                   Tunnel::Stage::SocksAuthRead);
}

/// @brief 按当前阶段解析已读入的握手字节（只看尚未交出的那段窗口）。
/// @param tunnel 隧道状态。
auto parse_handshake(const TunnelPointer &tunnel) -> void {
    const auto window = std::span<const char>(tunnel->buffer).subspan(tunnel->offset, tunnel->filled - tunnel->offset);
    const std::string_view bytes{window.data(), window.size()};
    switch (tunnel->stage) {
        case Tunnel::Stage::HttpHeadRead: {
            auto parsed = wire::parse_connect_response(bytes);
            if (!parsed) {
                finish_tunnel(tunnel, Result<StreamTransport>{parsed.error()});
                return;
            }
            // ⚠️ 必须把 `parsed.value()` 绑成一个具名引用再守卫：连写两次 `.value()` 时静态分析无法
            // 把前一行的 `has_value()` 判空与后一行的解包关联到同一个 optional 上（视为未检查访问）。
            auto &maybe = parsed.value();
            if (!maybe.has_value()) {
                start_read(tunnel);  // 还没读到终止空行
                return;
            }
            const auto head = *maybe;
            if (head.status < 200 || head.status >= 300) {
                // 非 2xx 一律不回退直连：407 落认证码，其余落不可达码，分岔由 wire 层负责。
                // 代理在这一句里说的是「怎么才能过」，故把状态码与 `Proxy-Authenticate` 原文一并交回上层：
                // 只留状态码就把挑战读成了一次纯粹的连通性故障，认证环无从答复。
                if (tunnel->negotiation != nullptr) {
                    tunnel->negotiation->status = head.status;
                    tunnel->negotiation->authenticate = head.headers.values("Proxy-Authenticate");
                }
                finish_tunnel(tunnel, Result<StreamTransport>{wire::connect_tunnel_error(
                                          head.status, tunnel->proxy.to_string(), tunnel->target)});
                return;
            }
            complete_handshake(tunnel, head.header_bytes);
            return;
        }
        case Tunnel::Stage::SocksMethodRead: {
            auto parsed = wire::parse_socks5_method(bytes);
            if (!parsed) {
                finish_tunnel(tunnel, Result<StreamTransport>{parsed.error()});
                return;
            }
            auto &maybe = parsed.value();
            if (!maybe.has_value()) {
                start_read(tunnel);
                return;
            }
            const auto method = *maybe;
            if (method == 0 || method == 2) {
                // 本段 2 字节回报交出去后才发下一段：不前移偏移，下一段会把这段当自己的开头重读。
                tunnel->offset += AURORA_SOCKS_SHORT_REPLY;
                if (method == 0) {
                    send_socks_connect(tunnel);
                    return;
                }
                send_socks_auth(tunnel);
                return;
            }
            finish_tunnel(tunnel, Result<StreamTransport>{wire::socks5_error_for(
                                      method, wire::Socks5Phase::MethodSelection, tunnel->proxy.to_string())});
            return;
        }
        case Tunnel::Stage::SocksAuthRead: {
            auto parsed = wire::parse_socks5_auth(bytes);
            if (!parsed) {
                finish_tunnel(tunnel, Result<StreamTransport>{parsed.error()});
                return;
            }
            auto &maybe = parsed.value();
            if (!maybe.has_value()) {
                start_read(tunnel);
                return;
            }
            const auto status = *maybe;
            if (status == 0) {
                tunnel->offset += AURORA_SOCKS_SHORT_REPLY;  // 同方法协商：这 2 字节已交出，CONNECT 应答才是本段
                send_socks_connect(tunnel);
                return;
            }
            finish_tunnel(tunnel, Result<StreamTransport>{wire::socks5_error_for(
                                      status, wire::Socks5Phase::Authentication, tunnel->proxy.to_string())});
            return;
        }
        case Tunnel::Stage::SocksReplyRead: {
            auto parsed = wire::parse_socks5_reply(bytes);
            if (!parsed) {
                finish_tunnel(tunnel, Result<StreamTransport>{parsed.error()});
                return;
            }
            auto &maybe = parsed.value();
            if (!maybe.has_value()) {
                start_read(tunnel);
                return;
            }
            const auto reply = *maybe;
            if (reply.status == 0) {
                complete_handshake(tunnel, reply.consumed);
                return;
            }
            finish_tunnel(tunnel, Result<StreamTransport>{wire::socks5_error_for(
                                      reply.status, wire::Socks5Phase::Tunnel, tunnel->proxy.to_string())});
            return;
        }
        case Tunnel::Stage::ProxyLink:
        case Tunnel::Stage::Done:
            finish_tunnel(tunnel, Result<StreamTransport>{protocol_reason("proxy handshake is in an unusable stage")});
            return;
    }
}

/// @brief 代理连接建成：装上隧道并写出首个握手报文。
/// @param tunnel 隧道状态。
/// @param outcome 代理连接结果。
auto on_proxy_connected(const TunnelPointer &tunnel, Result<StreamTransport> outcome) -> void {
    if (!outcome) {
        // 代理连不上就是连不上：换算与分岔留给上层，但**绝不改道直连目标**。
        finish_tunnel(tunnel, Result<StreamTransport>{outcome.error()});
        return;
    }
    tunnel->stream = std::move(outcome.value());
    if (tunnel->proxy.kind() == ProxyKind::HttpConnect) {
        // 认证环答出来的那份 `Proxy-Authorization` 优先于配置里的凭据：前者是对端这一轮点名要的方案，
        // 后者只是预防性的一枚。没答过（或本调用不参与握手事实）就照旧走预防性形态。
        std::optional<std::string> answer;
        if (tunnel->negotiation != nullptr && !tunnel->negotiation->proxy_authorization.empty()) {
            answer = tunnel->negotiation->proxy_authorization;
        }
        const auto request =
            wire::make_connect_request(tunnel->host, tunnel->port, tunnel->proxy.credentials(), answer);
        send_handshake(tunnel, std::vector<char>(request.begin(), request.end()), Tunnel::Stage::HttpHeadRead);
        return;
    }
    const auto greeting = wire::make_socks5_greeting(tunnel->proxy.has_credentials());
    send_handshake(tunnel, std::vector<char>(greeting.begin(), greeting.end()), Tunnel::Stage::SocksMethodRead);
}

}  // namespace

auto race_preference(const std::vector<Endpoint> &endpoints) -> std::vector<Endpoint> {
    std::vector<Endpoint> preferred;  // IPv6 候选（保持解析给出的相对次序）
    std::vector<Endpoint> other;  // IPv4 候选
    for (const auto &endpoint : endpoints) {
        if (endpoint.family() == AddressFamily::IPv6) {
            preferred.push_back(endpoint);
        } else if (endpoint.family() == AddressFamily::IPv4) {
            other.push_back(endpoint);
        }
        // 本机 IPC 形态在此剔除：它不属于 TCP 竞速，随本机 IPC 切片另立发起口。
    }
    // RFC 8305 §5.4 的次序：偏好族首条居首，其后两族交替；一族耗尽即由另一族尾部自然补齐。
    std::vector<Endpoint> ordered;
    ordered.reserve(preferred.size() + other.size());
    std::size_t taken = 0;
    if (!preferred.empty()) {
        ordered.push_back(preferred[taken++]);
    }
    while (taken < preferred.size() || !other.empty()) {
        if (!other.empty()) {
            ordered.push_back(other.front());
            other.erase(other.begin());
        }
        if (taken < preferred.size()) {
            ordered.push_back(preferred[taken++]);
        }
    }
    return ordered;
}

auto connect_by_name(std::string_view host, std::uint16_t port, const StreamOptions &options,
                     const Cancellation &cancellation, ConnectDone on_done) -> void {
    if (!on_done) {
        return;  // 没有回调就没有承诺：不解析、不建句柄
    }
    if (host.empty()) {
        on_done(Result<StreamTransport>{invalid_reason("connect target host is empty")});
        return;
    }
    if (port == 0) {
        on_done(Result<StreamTransport>{invalid_reason("connect target port is 0")});
        return;
    }
    const auto origin = std::this_thread::get_id();
    // 下面两枚 lambda 都转入 std::function（Resolver 回调与 `run_on_origin` 的归队体）：本检查对任何
    // 可调用对象一律判「不应抛出」（std::function::operator() 无 noexcept 规格，分析器无法证明），
    // 属 .clang-tidy 已记录的系统性假告警面 —— 同 `commands.cpp` 的处置口径，区间豁免并写明理由。
    // NOLINTBEGIN(bugprone-exception-escape)
    Resolver::resolve(
        host, port,
        [origin, options, cancellation, on_done = std::move(on_done)](Result<std::vector<Endpoint>> resolved) mutable {
            // 解析在引擎线程完成，先归队再建竞速状态：此后这张表的所有转换都在同一条线程上。
            run_on_origin(origin, [resolved = std::move(resolved), options, cancellation,
                                   on_done = std::move(on_done)]() mutable {
                if (!resolved) {
                    on_done(Result<StreamTransport>{resolved.error()});
                    return;
                }
                const auto race = std::make_shared<Race>();
                race->candidates = race_preference(resolved.value());
                race->options = options;
                race->cancellation = cancellation;
                race->on_done = std::move(on_done);
                race->origin = std::this_thread::get_id();
                if (race->candidates.empty()) {
                    // 解析回报里一条可连接的 TCP 端点都没有（全是本机 IPC 形态）：按「查不到可用地址」收尾。
                    finish_race(race, Result<StreamTransport>{make_error(
                                          ErrorCode::NetDnsFailed, "resolved name has no connectable TCP endpoint")});
                    return;
                }
                launch(race, 0);
            });
        });
    // NOLINTEND(bugprone-exception-escape)
}

auto connect_via_proxy(const ProxyConfig &proxy, std::string_view host, std::uint16_t port,
                       const StreamOptions &options, const Cancellation &cancellation, ConnectDone on_done,
                       ProxyNegotiation *negotiation) -> void {
    if (!on_done) {
        return;
    }
    if (proxy.is_direct()) {
        connect_by_name(host, port, options, cancellation, std::move(on_done));
        return;
    }
    if (host.empty()) {
        on_done(Result<StreamTransport>{invalid_reason("proxy tunnel target host is empty")});
        return;
    }
    if (port == 0) {
        on_done(Result<StreamTransport>{invalid_reason("proxy tunnel target port is 0")});
        return;
    }
    auto tunnel = std::make_shared<Tunnel>();
    tunnel->proxy = proxy;
    tunnel->host = std::string{host};
    tunnel->port = port;
    tunnel->target = host_port(host, port);
    tunnel->on_done = std::move(on_done);
    tunnel->origin = std::this_thread::get_id();
    tunnel->negotiation = negotiation;

    if (options.connect_timeout.count() > 0) {
        // 一枚时限覆盖「直到隧道可用」的整段：代理连上了却不应答握手同样要有结局，否则静默悬挂。
        const auto target = tunnel->target;
        std::weak_ptr<Tunnel> witness = tunnel;
        // 定时器回调与归队动作都经 `std::function` 派发，本检查对无抛异常规格的可调用对象一律判为
        // 「不应抛出」；可抛处只有错误文本拼接与调用方回调，派发边界不吞它（口径同 transport.cpp）。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        const auto armed = Engine::instance().post_after(options.connect_timeout, [witness, target]() -> void {
            const auto held = witness.lock();
            if (!held) {
                return;
            }
            // 同上：这一段跑在发起线程的归队入口里，抛出只可能来自调用方回调。
            // NOLINTNEXTLINE(bugprone-exception-escape)
            run_on_origin(held->origin, [held, target]() -> void {
                if (held->claimed) {
                    return;
                }
                // 超时裁决先落，再关腿：`close()` 会让在途的握手读以「已取消」再回一次，若让它先占住
                // 结局，调用方看到的失败原因就是取消而不是超时——同一件事实的两种说法必须留下成因那一种。
                finish_tunnel(
                    held, Result<StreamTransport>{make_error(ErrorCode::NetConnectTimeout,
                                                             ErrorParams{{"detail", "proxy tunnel to " + target}})});
                if (held->stream) {
                    held->stream->close();
                }
            });
        });
        if (!armed) {
            finish_tunnel(tunnel, Result<StreamTransport>{armed.error()});
            return;
        }
    }
    connect_by_name(proxy.host(), proxy.port(), options, cancellation,
                    [tunnel](Result<StreamTransport> outcome) { on_proxy_connected(tunnel, std::move(outcome)); });
}

}  // namespace aurora::net::detail
