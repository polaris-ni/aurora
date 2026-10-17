// tools/verify/tls_live_probe.cpp — TLS 通道对真实公网端点的探针（非 CTest，本仓唯一出站段）。
//
// 覆盖：本腿后端装配（`AURORA_ENABLE_NET_TLS_*` 决定注册表里在案的那几枚）下与真实公网端点握手的整条
// L2 通路——系统信任库里的**真实证书链**（叶子 + 中间 + 根；仓内自签桩造不出「部分链」这一形态，也证
// 不了系统根库那条通路）、SNI、ALPN 广告与对端回选、记录级收发、SPKI pin 的命中判定。「真实公网证书
// 的验证」这一条只能由本探针兑现：出站流量不可能进 CI（runner 禁网 + 全部用例的零出站纪律），故本探针
// 不进 CTest。
//
// 自动段（无需人工）：
//   1. 能力位：`Engine::capabilities().tls` 为真（本腿有 TLS 后端；为假即退出码 2，属合法降级）。
//   2. 握手：按缺省口径（校验开、只用系统根、TLS 1.2 下界）连到 host:port 并完成握手——这是
//      「真实证书链被本腿系统栈判为可信」的直接证据，仓内桩一律替代不了。`--backend <id>` 把这份
//      口径交给指定那一枚后端（`TlsContext::backend`，指定即决定、绝不换腿），并在握手后核对
//      `backend()` 回报的 id 与指定值逐字相同；不给该参数时核对回报的 id 确实在注册表表面。
//   3. 证书摘要：`peer_certificate()` 的主体 / 签发者 / 有效期 / SPKI 摘要四项皆非空，且 SPKI
//      摘要恒为 base64(SHA-256(SPKI DER)) 的 44 字符形态。
//   4. ALPN：广告 `--alpn` 时候选回报只能是该值或对端未选（`net-option-conflict` 形态，记 SKIP）；
//      不广告时候选回报**必须**是 `net-option-conflict` 而不是空串（「没有 ALPN」与「协商出一个空
//      协议」是两件事，后者的正确处置是关连接）。
//   5. 数据往返：写明文 GET、读到对端关闭写向，响应首行以 `HTTP/1.` 开头且总字节非零——证明加解密
//      与底层流收发在真实往返上接通，而不只是握手成功。
//   6. pin 反向：把上一条取到的真实 SPKI 摘要改一个字符再握手，必须以 `net-tls-pin-mismatch` 收尾
//      （pin 判定始终执行，且它不放松也不冒充链校验的结论）。
// 人工段（--interactive，肉眼比对）：
//   a. 打印的主体 / 签发者 / 有效期与浏览器同一站点的证书查看器逐项对照——只有人能判断「这张证书
//      确实是这个站点的那张」，程序在此只能证明它非空且可解析。
// 退出码：0 全过；1 自动段断言失败；2 环境不可用（本腿无 TLS 后端，或 DNS / 出站通路不通）。
//
// 运行：build 目录下 ./aurora_verify_tls_live [--host <dns-name>] [--port 443] [--alpn <id>]
//       [--backend <id>] [--interactive]
// 缺省目标是 www.example.com:443（IANA 保留的文档域名，持有效公网证书）。

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/engine.h"
#include "aurora/net/tls.h"
#include "aurora/net/tls_backend.h"
#include "aurora/net/transport.h"
#include "verify_args.h"

namespace {

using aurora::net::Cancellation;
using aurora::net::Engine;
using aurora::net::StreamOptions;
using aurora::net::StreamTransport;
using aurora::net::TlsContext;
using aurora::net::TlsTransport;

/// @brief 出站操作的等待预算：真网络的握手往返按秒计，比仓内用例的 3 s 宽一档。
constexpr auto AURORA_PROBE_BUDGET = std::chrono::milliseconds{20000};

/// @brief 单次响应累计的字节上限：只用于防止异常对端把探针撑爆，不是协议约束。
constexpr std::size_t AURORA_RESPONSE_CAP_BYTES = std::size_t{4} * 1024 * 1024;

// 探针的失败计数：匿名命名空间内的进程私有记账（无跨 TU 可见性、无初始化顺序问题），就地豁免非 const
// 全局（同 `x11_csd_live_probe` 的计数口径）。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int g_failures = 0;

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

auto check(bool condition, const std::string &label) -> void {
    if (condition) {
        emit("[PASS] " + label);
    } else {
        ++g_failures;
        emit("[FAIL] " + label);
    }
}

auto note(const std::string &text) -> void { emit("       " + text); }

/// @brief 常驻引擎的作用域收敛：探针没有宿主帧循环，每条退出路径都显式把线程 join 掉。
///
/// 探针跑完就 `return`，引擎线程仍在 `poll`；进程级单例刻意不析构（理由见 `net/engine.h` 的退出收敛
/// 契约与 `06-app-platform.md` §8.8.4），所以停机只能是调用方的动作——本探针就是三类显式调用方之一。
/// 少了这一步，结局要等操作系统强行终止线程，退出码不由判据决定。
/// @note Thread: 构造/拆除均在主线程
/// @note Side-effects: 拆除时停机常驻引擎线程
class EngineScope {
  public:
    EngineScope() = default;
    ~EngineScope() { Engine::instance().shutdown(); }
    EngineScope(const EngineScope &) = delete;
    auto operator=(const EngineScope &) -> EngineScope & = delete;
    EngineScope(EngineScope &&) = delete;
    auto operator=(EngineScope &&) -> EngineScope & = delete;
};

/// @brief 一条失败原因的对外形态：冻结 slug 打头，便于与 `codespec/ERROR_CATALOG.md` 对照。
template <typename T>
auto reason(const aurora::Result<T> &outcome) -> std::string {
    if (outcome.ok()) {
        return "<unexpected success>";
    }
    return outcome.error().code + " (" + outcome.error().message + ")";
}

/// @brief 一次异步结局的跨线程落点：完成回调可能在引擎线程抵达，探针主体按预算等它。
/// @tparam T 结局载荷类型。
template <typename T>
class Landing {
  public:
    /// @brief 存入结局；同一枚落点只认第一次投递（本层的「恰一次交付」判据在此体现）。
    /// @param outcome 完成回调携来的结果。
    auto arrive(aurora::Result<T> outcome) -> void {
        {
            const std::scoped_lock guard(mutex_);
            if (delivered_) {
                return;
            }
            slot_.emplace(std::move(outcome));
            delivered_ = true;
        }
        notice_.notify_all();
    }

    /// @return 结局是否在预算内抵达（假 = 挂死，探针按失败处理而不是继续等）。
    [[nodiscard]] auto wait() -> bool {
        std::unique_lock guard(mutex_);
        return notice_.wait_for(guard, AURORA_PROBE_BUDGET, [this] { return delivered_; });
    }

    /// @brief 取走结局。
    /// @return `wait()` 为真时必有值。
    [[nodiscard]] auto take() -> std::optional<aurora::Result<T>> {
        const std::scoped_lock guard(mutex_);
        return std::move(slot_);
    }

  private:
    std::mutex mutex_;
    std::condition_variable notice_;
    bool delivered_ = false;
    std::optional<aurora::Result<T>> slot_;
};

/// @brief 按主机名连到 port，返回已建成的流；失败打印原因并回空。
auto connect_stream(const std::string &host, std::uint16_t port) -> std::optional<StreamTransport> {
    Landing<StreamTransport> landing;
    const Cancellation token;
    auto options = StreamOptions{};
    options.connect_timeout = AURORA_PROBE_BUDGET;
    StreamTransport::connect(host, port, options, token, [&landing](aurora::Result<StreamTransport> outcome) {
        landing.arrive(std::move(outcome));
    });
    if (!landing.wait()) {
        emit("[FAIL] connect never delivered within budget");
        return std::nullopt;
    }
    auto outcome = landing.take();
    if (!outcome || !outcome->ok()) {
        emit("[FAIL] connect to " + host + ":" + std::to_string(port) + " -> " + reason(*outcome));
        return std::nullopt;
    }
    return std::move(outcome->value());
}

/// @brief 在已建成的流上握手；回空表示「连握手请求都没发出」。
auto perform_handshake(StreamTransport &&stream, const std::string &server_name, const TlsContext &context)
    -> std::optional<aurora::Result<TlsTransport>> {
    Landing<TlsTransport> landing;
    const Cancellation token;
    TlsTransport::handshake(std::move(stream), server_name, context, token,
                            [&landing](aurora::Result<TlsTransport> outcome) { landing.arrive(std::move(outcome)); });
    if (!landing.wait()) {
        emit("[FAIL] handshake never delivered within budget");
        return std::nullopt;
    }
    return landing.take();
}

/// @brief 在已握手的通道上发一条 GET 并读到对端关闭写向。
/// @return 累计的响应字节；空表示一条也没读到。
auto round_trip(TlsTransport &channel, const std::string &host) -> std::optional<std::string> {
    const auto request = "GET / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    Landing<std::size_t> written;
    channel.write(std::span<const char>(request),
                  [&written](aurora::Result<std::size_t> outcome) { written.arrive(std::move(outcome)); });
    if (!written.wait()) {
        emit("[FAIL] write never delivered within budget");
        return std::nullopt;
    }
    const auto write_outcome = written.take();
    if (!write_outcome || !write_outcome->ok()) {
        emit("[FAIL] request write -> " + reason(*write_outcome));
        return std::nullopt;
    }

    auto response = std::string{};
    auto buffer = std::vector<char>(16384);
    while (response.size() < AURORA_RESPONSE_CAP_BYTES) {
        Landing<std::size_t> read;
        channel.read(std::span<char>(buffer),
                     [&read](aurora::Result<std::size_t> outcome) { read.arrive(std::move(outcome)); });
        if (!read.wait()) {
            emit("[FAIL] read never delivered within budget");
            break;
        }
        const auto outcome = read.take();
        if (!outcome) {
            break;
        }
        if (!outcome->ok()) {
            // 读侧失败在真实公网上有两条合法成因（对端硬断、本层把记录解坏了），前者不影响已读到的
            // 字节，故只在「一条都没读到」时判失败；后者由后面的首行判据兜住。
            if (response.empty()) {
                emit("[FAIL] first read -> " + reason(*outcome));
                return std::nullopt;
            }
            note("read ended with " + reason(*outcome));
            break;
        }
        const auto delivered = outcome->value();
        if (delivered == 0) {
            break;  // 对端关闭写向：0 字节是流末，不是错误
        }
        response.append(buffer.data(), delivered);
    }
    return response.empty() ? std::nullopt : std::optional<std::string>{std::move(response)};
}

/// @brief base64(SHA-256(SPKI DER)) 的形态判据：44 字符且末位是 `=`。
auto looks_like_spki_pin(const std::string &pin) -> bool { return pin.size() == 44 && pin.back() == '='; }

/// @brief 改一个字符（末位填充符之前的首个可变字符），保持形态仍是一枚合法 base64 串。
[[nodiscard]] auto corrupted_pin(const std::string &pin) -> std::string {
    auto altered = pin;
    auto &slot = altered[0];
    slot = slot == 'A' ? 'B' : 'A';
    return altered;
}

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现，捕获反而把失败压成 0。
// 口径与 tools/verify/ 其余探针同。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    EngineScope engine_scope;
    const auto
        cli =
            aurora_verify::parse_command_line(
                aurora::cli::CommandSpec{.name = "aurora_verify",
                                         .about = "TLS live probe against a real public endpoint",
                                         .options =
                                             {
                                                 aurora::cli::OptionSchema{
                                                     .long_name = "interactive",
                                                     .kind = aurora::cli::ValueKind::Bool,
                                                     .arity = aurora::cli::Arity::flag(),
                                                     .help = "Compare the printed certificate fields with a browser by "
                                                             "hand before continuing"},
                                                 aurora::cli::OptionSchema{
                                                     .long_name = "host",
                                                     .kind = aurora::cli::ValueKind::String,
                                                     .help = "DNS name to reach (SNI and hostname verification input)",
                                                     .default_text = "www.example.com"},
                                                 aurora::cli::OptionSchema{
                                                     .long_name = "port",
                                                     .kind = aurora::cli::ValueKind::Int,
                                                     .help = "TCP port of the TLS endpoint",
                                                     .default_text = "443",
                                                     .minimum = 1,
                                                     .maximum = 65535},
                                                 aurora::cli::OptionSchema{
                                                     .long_name = "alpn",
                                                     .kind = aurora::cli::ValueKind::String,
                                                     .help = "Advertise this ALPN id (omit to send no ALPN at all)"},
                                                 aurora::cli::OptionSchema{
                                                     .long_name = "backend",
                                                     .kind = aurora::cli::ValueKind::String,
                                                     .help = "Designate one backend id from available_tls_backends() "
                                                             "for every "
                                                             "handshake instead of taking the default candidate chain"},
                                             }},
                argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const auto &arguments = *cli.arguments;
    const bool interactive = arguments.flag("interactive");
    const auto host = arguments.get<std::string>("host");
    const auto port = arguments.get<int>("port");
    if (!host || !port) {
        return aurora_verify::CommandLine::AURORA_EXIT_USAGE;
    }
    auto context = TlsContext{};
    if (arguments.explicitly_given("alpn")) {
        const auto advertised = arguments.get<std::string>("alpn");
        if (!advertised) {
            return aurora_verify::CommandLine::AURORA_EXIT_USAGE;
        }
        context.alpn_protocols = {advertised.value()};
    }
    std::optional<std::string> designated_backend;
    if (arguments.explicitly_given("backend")) {
        const auto requested = arguments.get<std::string>("backend");
        if (!requested) {
            return aurora_verify::CommandLine::AURORA_EXIT_USAGE;
        }
        context.backend = requested.value();
        designated_backend = requested.value();
    }

    emit("== Aurora TLS live probe ==");
    emit("target: " + host.value() + ":" + std::to_string(port.value()));
    // 注册表表面先打出来：共存装配（系统栈与 provider 同时在案）的唯一直接证据就是这一行有多枚 id。
    auto surface = aurora::net::available_tls_backends();
    std::string surface_text;
    for (const auto &id : surface) {
        surface_text += surface_text.empty() ? id : ", " + id;
    }
    emit("backends on this leg: " + (surface_text.empty() ? "(none)" : surface_text));

    // 1. 能力位：本腿有没有 TLS 后端决定后面每一段是否有意义。
    const auto capabilities = Engine::instance().capabilities();
    if (!capabilities.tls) {
        emit("[SKIP] capabilities().tls == false: no TLS backend in the registry on this leg");
        emit("       (POSIX legs need -DAURORA_NET_TLS_PROVIDERS=<static|shared>:<prefix>)");
        emit("result: ENV-UNAVAILABLE (exit 2)");
        return 2;
    }
    check(capabilities.tls, "capabilities().tls reports the assembled backend");

    // 2. 缺省口径握手：校验开、只用系统信任库。
    const auto endpoint_port = static_cast<std::uint16_t>(port.value());
    auto stream = connect_stream(host.value(), endpoint_port);
    if (!stream) {
        emit("result: ENV-UNAVAILABLE (exit 2) — DNS or the outbound path is not usable on this machine");
        return 2;
    }
    auto established = perform_handshake(std::move(*stream), host.value(), context);
    if (!established) {
        return 1;
    }
    if (!established->ok()) {
        emit("[FAIL] default-policy handshake against a real certificate -> " + reason(*established));
        emit("result: FAIL (exit 1)");
        return 1;
    }
    check(true, "handshake against the real chain in the system trust store succeeds");
    auto channel = std::move(established->value());

    // 2b. 实际生效的后端：指定即逐字兑现，缺省则回报必须是注册表表面上的 id。
    const auto served = channel.backend();
    if (designated_backend) {
        check(served.ok() && served.value() == *designated_backend,
              "the designated backend served the handshake (a designation never falls back)");
    } else {
        const auto on_surface =
            served.ok() && std::find(surface.begin(), surface.end(), served.value()) != surface.end();
        check(on_surface, "backend() reports an id that is on the registry surface");
    }
    note("backend : " + (served.ok() ? served.value() : reason(served)));

    // 3. 证书摘要四项。
    const auto summary = channel.peer_certificate();
    if (!summary.ok()) {
        emit("[FAIL] peer_certificate() after a successful handshake -> " + reason(summary));
        return 1;
    }
    const auto &peer = summary.value();
    note("subject : " + peer.subject);
    note("issuer  : " + peer.issuer);
    note("valid   : " + peer.not_before + " .. " + peer.not_after);
    note("spki    : " + peer.public_key_sha256);
    check(!peer.subject.empty() && !peer.issuer.empty() && !peer.not_before.empty() && !peer.not_after.empty(),
          "peer_certificate() fills all four fields from a real certificate");
    check(looks_like_spki_pin(peer.public_key_sha256), "SPKI pin is the base64(SHA-256(SPKI)) shape");

    if (interactive) {
        emit("--interactive: compare the four fields above with the browser's certificate viewer for this");
        emit("               host (subject, issuer and the validity minute), then continue. Only a human");
        emit("               can judge that this is *that* certificate for this site.");
    }

    // 4. ALPN 的两态回报。
    const auto negotiated = channel.negotiated_alpn();
    if (context.alpn_protocols.empty()) {
        check(!negotiated.ok() && negotiated.error().code == "net-option-conflict",
              "no ALPN advertised -> negotiated_alpn() reports the shape mismatch, not an empty id");
    } else if (negotiated.ok()) {
        check(negotiated.value() == context.alpn_protocols.front(),
              "negotiated ALPN is the advertised id (a peer may only pick from the advertisement)");
    } else {
        check(negotiated.error().code == "net-option-conflict", "peer selected no ALPN id");
        note("peer declined the advertisement; the channel still carries the requested protocol");
    }

    // 5. 真实往返。
    const auto response = round_trip(channel, host.value());
    if (!response) {
        emit("[FAIL] no response bytes over the established channel");
    } else {
        const auto head = response->substr(0, 9);
        check(head.starts_with("HTTP/1."), "response over the encrypted channel starts with HTTP/1.");
        note("received " + std::to_string(response->size()) + " bytes");
    }

    // 6. pin 反向：改一个字符的真实摘要必须被拒。
    auto pinned = context;
    pinned.pinned_public_keys = {corrupted_pin(peer.public_key_sha256)};
    auto retry = connect_stream(host.value(), endpoint_port);
    if (!retry) {
        check(false, "second connection for the pin check");
    } else {
        const auto verdict = perform_handshake(std::move(*retry), host.value(), pinned);
        const bool rejected = verdict && !verdict->ok() && verdict->error().code == "net-tls-pin-mismatch";
        check(rejected, "a public key outside the pin set is rejected with net-tls-pin-mismatch");
        if (verdict && !verdict->ok()) {
            note("observed: " + reason(*verdict));
        }
    }

    emit(g_failures == 0 ? "result: PASS (exit 0)" : "result: FAIL (exit 1)");
    return g_failures == 0 ? 0 : 1;
}
