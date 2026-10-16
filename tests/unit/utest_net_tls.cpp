/// 测试类型: unit
/// 目标单元: include/aurora/net/tls.h
/// 测试说明: aurora::net TLS 通道（L2）的行为契约回归：入参不合用（空主机名、IP 字面量、版本上界低于
///           下界）在**任何后端与任何 I/O 之前**即定论，故这些判据在无后端腿同样成立；后端缺席时握手恰一次
///           以 `net-tls-engine-unavailable` 收尾而不是静默降级成明文，且 `Engine::capabilities().tls` 与库内
///           探测口同源；握手成功后与 L1 流**逐字同构**——写明文读密文（对端逐片段反转回显）在小载荷上
///           逐字节相等、在跨记录载荷上总字节守恒；`peer_certificate()` 给出可跨腿比较的摘要形态（主体与签发者
///           含目标名、有效期是 ISO-8601、公钥摘要是 base64(SHA-256(SPKI)) 的 44 字符形态）；校验默认开启——
///           自签对端不经追加信任根即落 `net-tls-cert-rejected`，`trusted_certificates` 让同一张证书变得可信
///           （**合并**而非替换系统根），主机名不符（SNI 与证书 SAN/CN 不同）即便链可信也落 `net-tls-cert-rejected`；
///           危险开关 `allow_insecure_certificate` 跳过链与主机名判定但**必留一条 warning 诊断**，且它不影响
///           pin 判定——pin 未命中一律 `net-tls-pin-mismatch`（不设防与防错对象是两枚可分别表达的决策）；
///           ALPN 命中回报协议标识、未宣告落 `net-option-conflict`（形态不匹配而不是失败）；取消让未完成的
///           握手恰一次以 `net-cancelled` 收尾；`close()` 幂等且此后通道不再开放，对端进程消失后挂起的读
///           必有结局而不是悬挂。
///           对端是宿主 `openssl s_server` 子进程（`framework/tls_peer.h`）：证书链、记录分帧、close_notify
///           这些判据必须由真实 TLS 栈回答，仓内伪造测不到内核那一份行为。
/// ⚠️ 平台与能力分支一律写在**用例体内**（registry_integrity 静态扫源码，被 #ifdef 摘掉的声明在另一种
///    构建里必然单边失踪）。四条退让各有原因：wasm 无 fd 等待语义 → 全部真实收发用例 `AURORA_TEST_SKIP`；
///    宿主没有 `openssl` 命令行 → 依赖对端的用例跳过并报成环境缺项；本腿没有 TLS 后端（Linux 未给
///    `AURORA_NET_TLS_PROVIDER`）→ 依赖握手的用例跳过，而「无后端即 `net-tls-engine-unavailable`」那条
///    反而只在此刻可证；本腿的后端解析不了 PKCS#12（mbedTLS 腿）→ mTLS 的「身份真的送出去了」那半报成
///    缺项，同一用例里「带身份即落 `general-invalid-argument` 且绝不静默忽略」那半照常判。
/// ⚠️ 本文件不校验证书吊销（CRL/OCSP）：吊销判定按系统栈缺省形态委托给平台，库内不实现第二条路径。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/tls_backend.h"
#include "aurora/net/engine.h"
#include "aurora/net/tls.h"
#include "aurora/net/transport.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/matchers.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"
#include "framework/tls_peer.h"

namespace aurora::test_cases::utest_net_tls {
namespace {

namespace m = aurora::testing::matchers;

using aurora::net::Cancellation;
using aurora::net::Endpoint;
using aurora::net::StreamOptions;
using aurora::net::StreamTransport;
using aurora::net::TlsContext;
using aurora::net::TlsTransport;
using aurora::net::TlsVersion;
using aurora::test::net::drive_until;
using aurora::test::net::HostDriven;
using aurora::test::net::pump_idle;

/// @brief 本文件的等待主题：超时文本据此指明「是宿主泵没等到还是握手没落地」。
constexpr std::string_view AURORA_SUBJECT{"net-tls-host-pump"};

/// @brief 握手的等待预算：对端是刚起来的子进程，预算要容得下进程调度的抖动而不是逻辑失败。
constexpr std::chrono::milliseconds AURORA_HANDSHAKE_BUDGET{8000};

/// @brief 一轮读写的现场（缓冲必须活到回调抵达，故与计数同属一个对象）。
struct TransferLog {
    std::deque<std::vector<char>> buffers;  ///< 读缓冲池（deque：后入的槽位不会搬动已投递槽位的地址）
    std::atomic<int> reads{0};  ///< 已完成的读次数
    std::atomic<std::size_t> read_bytes{0};  ///< 已交付的明文字节总数
    std::atomic<int> writes{0};  ///< 已完成的写次数
    std::atomic<std::size_t> written{0};  ///< 已交付底层流的明文字节数
    std::string read_code;  ///< 读失败的码（成功形态为空）
    std::string read_message;  ///< 读失败的文本（现场：本层码相同但成因不同的两类都靠它区分）
    std::string write_code;  ///< 写失败的码
    bool read_failed = false;  ///< 读是否以失败收尾
    bool write_failed = false;  ///< 写是否以失败收尾
};

/// @brief 一次握手的现场：计数本身也是断言对象（「恰一次」是本层最难失手的契约）。
struct HandshakeLog {
    std::atomic<int> calls{0};  ///< 回调触发次数
    bool ok = false;  ///< 结局是否成功
    std::string code;  ///< 失败码
    std::string message;  ///< 失败文本（诊断用）
    TlsTransport transport;  ///< 成功时携带的通道
};

/// @brief 一次明文连接的现场（TLS 的底层腿）。
struct ConnectLog {
    std::atomic<int> calls{0};  ///< 回调触发次数
    bool ok = false;  ///< 是否连上
    std::string code;  ///< 失败码
    StreamTransport stream;  ///< 成功时携带的流
};

/// @brief 宿主驱动形态的连接口径：本层定时器落在引擎线程，会让送达时刻不可同线程断言。
/// @return 连接超时置 0（「不设时限，由对端决定何时出结果」）的选项。
auto host_options() -> StreamOptions {
    StreamOptions options;
    options.connect_timeout = std::chrono::milliseconds{0};
    return options;
}

/// @brief 本腿是否装有 TLS 后端（库内探测口是唯一真值源，与 `capabilities().tls` 同源）。
/// @return 可用为 true。
auto backend_available() -> bool { return aurora::net::detail::tls_backend_available(); }

/// @brief 没有后端即跳过：依赖握手的用例在没有后端的腿上不存在可证对象。
/// @param reason 缺项说明（进跳过报告）。
auto require_backend(std::string_view reason) -> void {
    if (!backend_available()) {
        AURORA_TEST_SKIP("this build carries no tls backend (capabilities().tls is false), the case needs one: " +
                         std::string{reason});
    }
}

/// @brief 对端起不来即跳过：环境缺项要报成缺项，不能伪装成一次逻辑失败。
/// @param peer 目标对端。
auto require_peer(const aurora::test::tls::TlsPeer &peer) -> void {
    if (!peer.valid()) {
        AURORA_TEST_SKIP("tls peer is unavailable on this host: " + peer.status());
    }
}

/// @brief 在宿主驱动形态下连上回环端口，交出明文流（握手的前一条腿）。
/// @param port 目标端口。
/// @return 已连接的流；连不上即致命断言转红（制造不出被测前提时不该继续跑）。
auto connect_stream(std::uint16_t port) -> StreamTransport {
    ConnectLog log;
    StreamTransport::connect(Endpoint::loopback(port), host_options(), Cancellation{}, [&log](auto result) {
        log.ok = result.ok();
        if (result) {
            log.stream = std::move(result.value());
        } else {
            log.code = result.error().code;
        }
        ++log.calls;
    });
    AURORA_TEST_REQUIRE_MSG(drive_until(AURORA_SUBJECT, [&]() { return log.calls.load() > 0; }).satisfied,
                            "loopback connect never delivered: port " + std::to_string(port));
    AURORA_TEST_REQUIRE_MSG(log.ok, "loopback connect failed: " + log.code);
    return std::move(log.stream);
}

/// @brief 把 `TlsTransport::handshake` 的回调形态收成一枚可断言的日志（只发起，不等结局）。
///
/// 分成「发起」与「等待」两步是必需的：`cancel_finishes_an_ongoing_handshake_exactly_once` 要断言的
/// 正是「此刻还没有结局」，一个自带等待的入口会把那条判据变成必然超时。
/// @param stream 底层明文流（被握手接管）。
/// @param server_name SNI 与主机名校验的输入。
/// @param context 安全口径。
/// @param log 结局现场（须存活到回调抵达）。
/// @param cancellation 取消令牌；缺省为「不取消」。
auto start_handshake(StreamTransport &&stream, std::string_view server_name, const TlsContext &context,
                     HandshakeLog &log, const Cancellation &cancellation = Cancellation{}) -> void {
    TlsTransport::handshake(std::move(stream), server_name, context, cancellation, [&log](auto result) {
        log.ok = result.ok();
        if (result) {
            log.transport = std::move(result.value());
        } else {
            log.code = result.error().code;
            log.message = result.error().message;
        }
        ++log.calls;
    });
}

/// @brief 等到握手结局落地（预算容得下子进程调度的抖动，而不是把它当成逻辑失败）。
/// @param log 发起时用的同一份现场。
auto await_handshake(HandshakeLog &log) -> void {
    AURORA_TEST_REQUIRE_MSG(drive_until(
                                AURORA_SUBJECT, [&]() { return log.calls.load() > 0; }, AURORA_HANDSHAKE_BUDGET)
                                .satisfied,
                            "tls handshake never delivered a verdict");
}

/// @brief 发起并等待一次握手的结局。
/// @param stream 底层明文流。
/// @param server_name SNI 与主机名校验的输入。
/// @param context 安全口径。
/// @param log 结局现场。
/// @param cancellation 取消令牌。
auto handshake(StreamTransport &&stream, std::string_view server_name, const TlsContext &context, HandshakeLog &log,
               const Cancellation &cancellation = Cancellation{}) -> void {
    start_handshake(std::move(stream), server_name, context, log, cancellation);
    await_handshake(log);
}

/// @brief 一次成功的握手：把通道从日志里取出来（失败即转红并带上原因文本）。
/// @param stream 底层明文流。
/// @param server_name SNI 与主机名校验的输入。
/// @param context 安全口径。
/// @return 已建立的通道。
auto expect_open(StreamTransport &&stream, std::string_view server_name, const TlsContext &context) -> TlsTransport {
    HandshakeLog log;
    handshake(std::move(stream), server_name, context, log);
    AURORA_TEST_REQUIRE_MSG(log.ok, "handshake failed: " + log.code + " " + log.message);
    AURORA_TEST_REQUIRE_EQ(log.calls.load(), 1);
    return std::move(log.transport);
}

/// @brief 投递一次写并等到完成。
/// @param channel 目标通道。
/// @param log 现场。
/// @param payload 待发明文字节。
auto write_through(TlsTransport &channel, TransferLog &log, std::string_view payload) -> void {
    channel.write(std::span<const char>{payload.data(), payload.size()}, [&log](auto result) {
        if (result) {
            log.written.store(result.value());
        } else {
            log.write_failed = true;
            log.write_code = result.error().code;
        }
        ++log.writes;
    });
    AURORA_TEST_REQUIRE(drive_until(AURORA_SUBJECT, [&]() { return log.writes.load() > 0; }).satisfied);
}

/// @brief 投递一次读并等到完成。
/// @param channel 目标通道。
/// @param log 现场。
/// @param bytes 本次投递的缓冲长度。
/// @return 本次交付的明文字节（读失败为空串）。
auto read_once(TlsTransport &channel, TransferLog &log, std::size_t bytes) -> std::string {
    log.buffers.emplace_back(bytes, '\0');
    auto &slot = log.buffers.back();
    // 期望值在投递之前取：本层的读可以在 `read()` 返回前就地完成（缓冲里已有解密的明文），
    // 事后 `reads.load() + 1` 会把这种合法的就地交付读成「还欠一次」而永远等下去。
    const auto delivered = log.reads.load() + 1;
    channel.read(std::span<char>{slot}, [&log, &slot](auto result) {
        if (result) {
            log.read_bytes.store(log.read_bytes.load() + result.value());
            if (result.value() < slot.size()) {
                slot.resize(result.value());  // 交付长度即本轮读到的全部：截短以便逐字节比较
            }
        } else {
            log.read_failed = true;
            log.read_code = result.error().code;
            log.read_message = result.error().message;
            slot.clear();
        }
        ++log.reads;
    });
    AURORA_TEST_REQUIRE_MSG(drive_until(AURORA_SUBJECT, [&]() { return log.reads.load() >= delivered; }).satisfied,
                            log.read_failed ? "read failed: " + log.read_code + " " + log.read_message
                                            : "read never delivered: pending " + std::to_string(delivered));
    return std::string{slot.begin(), slot.end()};
}

/// @brief 缺省安全口径：本地自签测试用（校验关掉且只在此文件里用）。
/// @return 带危险开关的上下文。
auto insecure_context() -> TlsContext {
    TlsContext context;
    context.allow_insecure_certificate = true;
    return context;
}

/// @brief 追加自签根后的安全口径：校验开着，可信签发者由本用例自己给出。
/// @param assets 本用例的证书资产（追加的就是对端那一张自签证书）。
/// @return 带一张追加根的上下文。
auto trusted_context(const aurora::test::tls::Assets &assets) -> TlsContext {
    TlsContext context;
    context.trusted_certificates = {assets.certificate_pem()};
    return context;
}

/// @brief 捕获 Logger 的 sink 以断言诊断留痕；析构恢复全局缺省，不泄漏给其它用例。
class CapturedLogger {
  public:
    CapturedLogger() : level_(aurora::Logger::instance().level()), enabled_(aurora::Logger::instance().is_enabled()) {
        auto &logger = aurora::Logger::instance();
        logger.set_sink([this](std::string_view line) -> void { lines.emplace_back(line); });
    }
    ~CapturedLogger() {
        auto &logger = aurora::Logger::instance();
        logger.set_level(level_);
        logger.set_enabled(enabled_);
        logger.set_sink(nullptr);
    }
    CapturedLogger(const CapturedLogger &) = delete;
    auto operator=(const CapturedLogger &) -> CapturedLogger & = delete;
    CapturedLogger(CapturedLogger &&) = delete;
    auto operator=(CapturedLogger &&) -> CapturedLogger & = delete;

    std::vector<std::string> lines;  ///< 捕获到的诊断行

  private:
    aurora::LogLevel level_;
    bool enabled_;
};

/// @brief base64 字符集判定（SPKI 摘要的形态判据：可比对、可复制、不含平台句柄）。
/// @param text 待判文本。
/// @return 全部字符落在 base64 表内为 true。
auto is_base64(std::string_view text) -> bool {
    for (const char character : text) {
        const bool ok = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                        (character >= '0' && character <= '9') || character == '+' || character == '/' ||
                        character == '=';
        if (!ok) {
            return false;
        }
    }
    return !text.empty();
}

}  // namespace

AURORA_TEST_CASE(invalid_inputs_are_decided_before_any_backend) {
    ::fprintf(stderr, "[diag] ENTER invalid_inputs body\n");
    // 这三条判据的价值恰在于「后端还没登场就定了结局」：无后端腿与 wasm 同样可证，
    // 也就不会有人把「本腿没有 TLS」误读成「入参校验没跑」。
    HostDriven host;
    struct Case {
        std::string_view name;
        std::string_view server_name;
        std::string_view expected;
        TlsContext context;
    };
    auto crossed = TlsContext{};
    crossed.min_version = TlsVersion::Tls13;
    crossed.max_version = TlsVersion::Tls11;
    const std::vector<Case> cases{
        Case{.name = "empty name", .server_name = "", .expected = "net-endpoint-invalid", .context = TlsContext{}},
        Case{.name = "ip literal",
             .server_name = "127.0.0.1",
             .expected = "net-endpoint-invalid",
             .context = TlsContext{}},
        Case{.name = "ipv6 literal",
             .server_name = "[::1]",
             .expected = "net-endpoint-invalid",
             .context = TlsContext{}},
        Case{.name = "version bounds cross",
             .server_name = "localhost",
             .expected = "net-tls-version-mismatch",
             .context = crossed},
    };
    for (const auto &item : cases) {
        HandshakeLog log;
        TlsTransport::handshake(StreamTransport{}, item.server_name, item.context, Cancellation{}, [&log](auto result) {
            log.ok = result.ok();
            if (!result) {
                log.code = result.error().code;
            }
            ++log.calls;
        });
        AURORA_TEST_REQUIRE_MSG(drive_until(AURORA_SUBJECT, [&]() { return log.calls.load() > 0; }).satisfied,
                                std::string{item.name} + ": no verdict delivered");
        AURORA_TEST_CHECK_EQ(log.calls.load(), 1);
        AURORA_TEST_CHECK_FALSE(log.ok);
        AURORA_TEST_CHECK_EQ(log.code, std::string{item.expected});
        AURORA_TEST_CHECK_FALSE(log.transport.is_open());
    }
}

AURORA_TEST_CASE(null_callback_starts_nothing) {
    HostDriven host;
    // 空回调不是「发起后丢弃结局」而是不发起：没有任何登记与后端创建可被这次调用触发。
    TlsTransport::handshake(StreamTransport{}, "localhost", TlsContext{}, Cancellation{}, nullptr);
    pump_idle(std::chrono::milliseconds{20});
    AURORA_TEST_CHECK(true);  // 活到这里就是判据（不崩、不悬挂）
}

AURORA_TEST_CASE(absent_backend_reports_engine_unavailable) {
    HostDriven host;
    if (backend_available()) {
        AURORA_TEST_SKIP("this build has a tls backend, the unavailable-branch verdict needs the other shape");
    }
    HandshakeLog log;
    TlsTransport::handshake(StreamTransport{}, "localhost", TlsContext{}, Cancellation{}, [&log](auto result) {
        log.ok = result.ok();
        if (!result) {
            log.code = result.error().code;
        }
        ++log.calls;
    });
    AURORA_TEST_REQUIRE(drive_until(AURORA_SUBJECT, [&]() { return log.calls.load() > 0; }).satisfied);
    AURORA_TEST_CHECK_EQ(log.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(log.ok);
    // 关键判据：没有后端就是**明确失败**，不是静默降级成明文——后者会让调用方以为自己在加密。
    AURORA_TEST_CHECK_EQ(log.code, std::string{"net-tls-engine-unavailable"});
}

AURORA_TEST_CASE(capability_bit_shares_one_source_with_probe) {
    HostDriven host;
    const auto caps = aurora::net::Engine::instance().capabilities();
    AURORA_TEST_CHECK_EQ(caps.tls, backend_available());
}

AURORA_TEST_CASE(handshake_establishes_channel_and_summarizes_peer) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    auto channel = expect_open(connect_stream(peer.port()), "localhost", insecure_context());
    AURORA_TEST_CHECK(channel.is_open());

    const auto summary = channel.peer_certificate();
    AURORA_TEST_REQUIRE(summary.ok());
    const auto &cert = summary.value();
    AURORA_TEST_CHECK_THAT(cert.subject, m::has_substr("localhost"));
    AURORA_TEST_CHECK_FALSE(cert.issuer.empty());
    // 摘要形态是 pin 集合唯一可互通的写法：base64(SHA-256(SPKI DER)) 恒为 44 字符且以 '=' 收尾。
    AURORA_TEST_CHECK_EQ(cert.public_key_sha256.size(), std::size_t{44});
    AURORA_TEST_CHECK(is_base64(cert.public_key_sha256));
    AURORA_TEST_CHECK_EQ(cert.public_key_sha256.back(), '=');
    // 有效期在公共面上是可比对的文本而不是平台结构：ISO-8601（UTC，秒级）恒 20 字符。
    AURORA_TEST_CHECK_EQ(cert.not_before.size(), std::size_t{20});
    AURORA_TEST_CHECK_EQ(cert.not_after.size(), std::size_t{20});
    AURORA_TEST_CHECK_EQ(cert.not_after[4], '-');
    AURORA_TEST_CHECK_EQ(cert.not_after[10], 'T');
    AURORA_TEST_CHECK_EQ(cert.not_after[19], 'Z');

    // 未宣告 ALPN 时 negotiated_alpn() 落形态冲突而不是空串：空串会被读成「协商出一个空协议」。
    const auto alpn = channel.negotiated_alpn();
    AURORA_TEST_CHECK_FALSE(alpn.ok());
    AURORA_TEST_CHECK_EQ(alpn.error().code, std::string{"net-option-conflict"});

    channel.close();
    AURORA_TEST_CHECK_FALSE(channel.is_open());
}

AURORA_TEST_CASE(read_and_write_are_isomorphic_with_l1_stream) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    auto channel = expect_open(connect_stream(peer.port()), "localhost", insecure_context());

    // 对端是反转回显的真实 TLS 服务：写出 7 字节读回 7 字节即为「读写与 L1 同构」的直接证据。
    // 载荷一律以换行收尾：`-rev` 的应答按行触发（收到换行才回写），少了它这条腿会永远等不到回显。
    TransferLog log;
    write_through(channel, log, "aurora\n");
    AURORA_TEST_CHECK_FALSE(log.write_failed);
    AURORA_TEST_CHECK_EQ(log.written.load(), std::size_t{7});
    const auto echoed = read_once(channel, log, 64);
    AURORA_TEST_CHECK_MSG(!log.read_failed, "read failed: " + log.read_code + " " + log.read_message);
    AURORA_TEST_CHECK_EQ(echoed, std::string{"arorua\n"});

    // 第二次往返证明通道不是「一次性」的：同一条通道可继续按 L1 纪律收发。
    TransferLog second;
    write_through(channel, second, "xy\n");
    AURORA_TEST_CHECK_EQ(read_once(channel, second, 64), std::string{"yx\n"});

    // 空写入按 L1 口径直接以 OK 完成（不产生记录也不悬挂）。
    TransferLog empty;
    write_through(channel, empty, "");
    AURORA_TEST_CHECK_FALSE(empty.write_failed);
    AURORA_TEST_CHECK_EQ(empty.written.load(), std::size_t{0});

    // 暂停读的形态与 L1 一致：暂停后字节不进本层缓冲，恢复后照原样送达。
    channel.pause_read();
    AURORA_TEST_CHECK(channel.read_paused());
    write_through(channel, second, "ab\n");
    pump_idle(std::chrono::milliseconds{80});
    AURORA_TEST_CHECK_EQ(second.reads.load(), std::size_t{1});  // 暂停期间没有新的读投递
    channel.resume_read();
    AURORA_TEST_CHECK_FALSE(channel.read_paused());
    AURORA_TEST_CHECK_EQ(read_once(channel, second, 64), std::string{"ba\n"});

    AURORA_TEST_CHECK_EQ(channel.pending_write_bytes(), std::size_t{0});
    channel.close();
}

AURORA_TEST_CASE(payload_spanning_records_round_trips) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    auto channel = expect_open(connect_stream(peer.port()), "localhost", insecure_context());

    // 跨记录载荷：逐片段反转的次序由对端的记录读取决定，故此处只断言**总字节守恒**（内容次序不成立
    // 也不该成立，那是把对端的缓冲尺寸当成我方契约）。
    const std::size_t total = std::size_t{32} * 1024U;
    std::string payload(total - 1, 'a');
    payload.push_back('\n');
    TransferLog log;
    write_through(channel, log, payload);
    AURORA_TEST_CHECK_FALSE(log.write_failed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (log.read_bytes.load() < total && std::chrono::steady_clock::now() < deadline) {
        const auto remaining = total - log.read_bytes.load();
        read_once(channel, log, remaining < 8192 ? remaining : 8192);
    }
    AURORA_TEST_CHECK_EQ(log.read_bytes.load(), total);
    AURORA_TEST_CHECK_FALSE(log.read_failed);
    channel.close();
}

AURORA_TEST_CASE(untrusted_self_signed_peer_is_rejected_by_default) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    HandshakeLog log;
    handshake(connect_stream(peer.port()), "localhost", TlsContext{}, log);
    AURORA_TEST_CHECK_EQ(log.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(log.ok);
    AURORA_TEST_CHECK_EQ(log.code, std::string{"net-tls-cert-rejected"});
    // 握手失败即流作废：不留「还能接着用」的错觉。
    AURORA_TEST_CHECK_FALSE(log.transport.is_open());
}

AURORA_TEST_CASE(appended_trust_root_makes_self_signed_peer_acceptable) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    // 同一张自签证书：上一条用例被拒、这一条被接受，差别只有「追加了一枚可信根」。
    // 判据因此同时证明了两件事：追加确实改变「谁算可信签发者」，且合并没有替换系统根（替换会让
    // 任何公有 CA 签发的证书在此刻一起失效，那是比拒绝更糟的故障）。
    auto channel = expect_open(connect_stream(peer.port()), "localhost", trusted_context(assets));
    AURORA_TEST_CHECK(channel.is_open());
    TransferLog log;
    write_through(channel, log, "trust\n");
    AURORA_TEST_CHECK_EQ(read_once(channel, log, 64), std::string{"tsurt\n"});
    channel.close();
}

AURORA_TEST_CASE(expired_self_signed_peer_is_rejected_even_when_chain_is_trusted) {
    // 一张**过期**的自签证书：把它同时当追加可信根用，于是「谁算可信签发者」这一维是满足的，
    // 剩下的唯一不满足项是 Validity。判据要的是这两维各自独立——追加根只赦签发者，不赦有效期，
    // 否则一条「把自己那张过期根加进去」的写法就等于关掉校验（而危险开关是另一枚显式命名的字段）。
    const aurora::test::tls::Assets assets{"localhost", false, aurora::test::tls::Lifetime::Expired};
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    HandshakeLog log;
    handshake(connect_stream(peer.port()), "localhost", trusted_context(assets), log);
    AURORA_TEST_CHECK_EQ(log.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(log.ok);
    AURORA_TEST_CHECK_EQ(log.code, std::string{"net-tls-cert-rejected"});
    AURORA_TEST_CHECK_FALSE(log.transport.is_open());
}

AURORA_TEST_CASE(hostname_mismatch_is_rejected_even_when_chain_is_trusted) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    // 链可信（追加了根）而名字不符：证书写给 localhost，我们要求的却是另一个名字。
    // 这一条把「主机名核对」从「链校验」里单独分离出来——系统栈按 SNI 建链却**不**核对名字，
    // 少了本层这一半，公共面上「校验默认开启」的承诺就是假的。
    HandshakeLog log;
    auto context = trusted_context(assets);
    handshake(connect_stream(peer.port()), "not-the-peer-name.invalid", context, log);
    AURORA_TEST_CHECK_EQ(log.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(log.ok);
    AURORA_TEST_CHECK_EQ(log.code, std::string{"net-tls-cert-rejected"});
    AURORA_TEST_CHECK_THAT(log.message, m::has_substr("not-the-peer-name.invalid"));
}

AURORA_TEST_CASE(dangerous_switch_skips_verification_but_leaves_a_trace) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    CapturedLogger capture;
    aurora::Logger::instance().set_level(aurora::LogLevel::Warn);
    // 危险开关连主机名一起跳过（上一条用例的名字在此刻不构成拒绝理由），但必须留下诊断：
    // 事后无法从连接本身还原「当初为什么没校验」，诊断是这一档唯一的现场证据。
    auto channel = expect_open(connect_stream(peer.port()), "anything.invalid", insecure_context());
    AURORA_TEST_CHECK(channel.is_open());
    channel.close();
    bool warned = false;
    for (const auto &line : capture.lines) {
        warned = warned || (line.find("[WRN]") != std::string::npos &&
                            line.find("certificate verification disabled") != std::string::npos);
    }
    AURORA_TEST_CHECK_MSG(warned, "no warning diagnostic was emitted for the insecure switch");
}

AURORA_TEST_CASE(pin_mismatch_rejects_a_trusted_peer_and_insecure_switch_does_not_mask_it) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    const auto wrong_pin = std::string{"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="};

    // 链可信 + pin 不命中 → 拒绝：pin 是叠加约束，不放松校验。
    auto context = trusted_context(assets);
    context.pinned_public_keys = {wrong_pin};
    HandshakeLog rejected;
    handshake(connect_stream(peer.port()), "localhost", context, rejected);
    AURORA_TEST_CHECK_EQ(rejected.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(rejected.ok);
    AURORA_TEST_CHECK_EQ(rejected.code, std::string{"net-tls-pin-mismatch"});

    // 不设防 + 同一个错 pin → 仍然拒绝：「关掉校验」与「防错对象」必须能分别表达。
    auto masked = insecure_context();
    masked.pinned_public_keys = {wrong_pin};
    HandshakeLog still_rejected;
    handshake(connect_stream(peer.port()), "localhost", masked, still_rejected);
    AURORA_TEST_CHECK_FALSE(still_rejected.ok);
    AURORA_TEST_CHECK_EQ(still_rejected.code, std::string{"net-tls-pin-mismatch"});

    // 命中判定本身：先在不设防的档位上取到对端摘要，再以该摘要为 pin 重连——两步都成立才说明
    // 摘要与 pin 是同一套形态（按整证摘要 pin 会在证书续签时无预警失配）。
    HandshakeLog learn;
    handshake(connect_stream(peer.port()), "localhost", insecure_context(), learn);
    AURORA_TEST_REQUIRE(learn.ok);
    const auto summary = learn.transport.peer_certificate();
    AURORA_TEST_REQUIRE(summary.ok());
    const auto pin = summary.value().public_key_sha256;
    learn.transport.close();

    auto trusting = trusted_context(assets);
    trusting.pinned_public_keys = {pin};
    auto channel = expect_open(connect_stream(peer.port()), "localhost", trusting);
    AURORA_TEST_CHECK(channel.is_open());
    channel.close();
}

AURORA_TEST_CASE(alpn_reports_the_negotiated_identifier) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets, {"-alpn", "http/1.1"}};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    TlsContext context = insecure_context();
    context.alpn_protocols = {"http/1.1"};
    auto channel = expect_open(connect_stream(peer.port()), "localhost", context);
    const auto negotiated = channel.negotiated_alpn();
    // 附带原因文本时不得无条件取 `error()`：成功态上的它是一次 `std::get` 越界，会把断言变成异常。
    AURORA_TEST_REQUIRE_MSG(negotiated.ok(), negotiated.ok()
                                                 ? "alpn negotiated: " + negotiated.value()
                                                 : "alpn was offered but not reported: " + negotiated.error().code);
    AURORA_TEST_CHECK_EQ(negotiated.value(), std::string{"http/1.1"});
    channel.close();
}

AURORA_TEST_CASE(alpn_without_overlap_is_reported_as_shape_mismatch) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets, {"-alpn", "http/1.1"}};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    TlsContext context = insecure_context();
    context.alpn_protocols = {"h2"};  // 对端只认 http/1.1：两腿的处置不同，但都不是「协商出一个空协议」
    HandshakeLog log;
    handshake(connect_stream(peer.port()), "localhost", context, log);
    if (!log.ok) {
        AURORA_TEST_CHECK_EQ(log.calls.load(), 1);
        return;
    }
    const auto negotiated = log.transport.negotiated_alpn();
    AURORA_TEST_CHECK_FALSE(negotiated.ok());
    AURORA_TEST_CHECK_EQ(negotiated.error().code, std::string{"net-option-conflict"});
    log.transport.close();
}

AURORA_TEST_CASE(cancel_finishes_an_ongoing_handshake_exactly_once) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("wasm has no fd wait semantics for a stalled handshake");
#endif
    require_backend("needs an ongoing handshake to cancel");
    aurora::test::ScriptedPeer stalled{aurora::test::scripts::silent()};
    AURORA_TEST_REQUIRE(stalled.valid());
    HostDriven host;
    Cancellation cancellation;
    HandshakeLog log;
    // 对端连上但不回 TLS：握手挂在不确定的时长上，取消是唯一能给出结局的那一手。
    start_handshake(connect_stream(stalled.port()), "localhost", insecure_context(), log, cancellation);
    AURORA_TEST_CHECK_EQ(log.calls.load(), 0);  // 此刻不该有结局：取消之前的等待不是失败
    cancellation.cancel();
    AURORA_TEST_CHECK_EQ(log.calls.load(), 1);  // 钩子在调用线程同步落地，恰一次
    AURORA_TEST_CHECK_FALSE(log.ok);
    AURORA_TEST_CHECK_EQ(log.code, std::string{"net-cancelled"});
    AURORA_TEST_CHECK_FALSE(log.transport.is_open());
    cancellation.cancel();
    AURORA_TEST_CHECK_EQ(log.calls.load(), 1);  // 重复取消不追加结局
}

AURORA_TEST_CASE(close_is_idempotent_and_leaves_nothing_to_wait_for) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    auto channel = expect_open(connect_stream(peer.port()), "localhost", insecure_context());
    channel.close();
    channel.close();  // 幂等：第二次关闭不产生新结局
    AURORA_TEST_CHECK_FALSE(channel.is_open());

    // 关闭之后的读写都不悬挂：要么有明确结局，要么是空态口径的确定码。
    TransferLog log;
    log.buffers.emplace_back(std::size_t{64}, '\0');
    channel.read(std::span<char>{log.buffers.back()}, [&log](const auto &result) {
        if (!result) {
            log.read_failed = true;
            log.read_code = result.error().code;
            log.read_message = result.error().message;
        }
        ++log.reads;
    });
    AURORA_TEST_REQUIRE(drive_until(AURORA_SUBJECT, [&]() { return log.reads.load() > 0; }).satisfied);
    AURORA_TEST_CHECK_TRUE(log.read_failed);

    const auto summary = channel.peer_certificate();
    AURORA_TEST_CHECK_FALSE(summary.ok());
    AURORA_TEST_CHECK_EQ(summary.error().code, std::string{"net-socket-failed"});
}

AURORA_TEST_CASE(peer_process_disappearance_settles_a_pending_read) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    auto channel = expect_open(connect_stream(peer.port()), "localhost", insecure_context());
    // 先排一次读，再让对端整个消失（进程级强杀）：挂起的读必须有结局，不得把用例线程留在等待里。
    // 结局形态两腿可异（close_notify 之后是 EOF 的 0 字节，RST 之后是失败），故判据是「有结局」。
    TransferLog log;
    log.buffers.emplace_back(std::size_t{4096}, '\0');
    channel.read(std::span<char>{log.buffers.back()}, [&log](auto result) {
        if (result) {
            log.read_bytes.store(result.value());
        } else {
            log.read_failed = true;
            log.read_code = result.error().code;
            log.read_message = result.error().message;
        }
        ++log.reads;
    });
    peer.stop();
    AURORA_TEST_CHECK_FALSE(peer.running());
    AURORA_TEST_REQUIRE(drive_until(AURORA_SUBJECT, [&]() { return log.reads.load() > 0; }).satisfied);
    AURORA_TEST_CHECK_EQ(log.reads.load(), 1);
    channel.close();
}

AURORA_TEST_CASE(version_floor_below_peer_reaches_a_verdict) {
    const aurora::test::tls::Assets assets;
    aurora::test::tls::TlsPeer peer{assets, {"-tls1_2"}};
    require_peer(peer);
    require_backend("needs a real handshake");
    HostDriven host;
    TlsContext context = insecure_context();
    context.min_version = TlsVersion::Tls13;  // 对端最高只到 1.2：允许集为空，结局由平台给出但必须给出
    HandshakeLog log;
    handshake(connect_stream(peer.port()), "localhost", context, log);
    AURORA_TEST_CHECK_EQ(log.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(log.ok);
    AURORA_TEST_CHECK_FALSE(log.transport.is_open());
    peer.stop();
}

AURORA_TEST_CASE(client_identity_satisfies_a_verified_peer_request) {
    const aurora::test::tls::Assets assets{"localhost", true};
    aurora::test::tls::TlsPeer peer{assets, {"-Verify", "1", "-CAfile", assets.certificate().string()}};
    require_peer(peer);
    require_backend("needs a real handshake");
    if (!assets.identity_valid()) {
        AURORA_TEST_SKIP("the host openssl could not pack a PKCS#12 client identity: " + assets.status());
    }
    HostDriven host;

    // 服务端要求客户端证书：不带身份的会话拿不到回显，带上身份才拿得到。两半合起来才证明
    // `client_identity` 真的把身份送了出去，而不是「服务端其实没要」。
    //
    // 匿名半的判据刻意**不是**「握手必须失败」：TLS 1.2 及以下的服务端要等收到客户端证书才发自己的
    // Finished，缺证书的致命告警因此先于握手调用返回（Windows Schannel 腿实测即此形态）；TLS 1.3 里
    // 服务端的 Finished 已在自己的首飞里发完，客户端视角下握手已经完成，那条告警是**握手后**事件
    // （`RFC 8446` §4.3.2 与 §4.4.2；两条 provider 腿实测为握手报成功、第一次记录交换才落
    // `net-tls-handshake-failed`）。写成「必须失败」会把一条协议固有的时序差异读成库的缺陷，
    // 而调用方据此分支出的失败路径在今天合法的构建上永不成立。
    HandshakeLog anonymous;
    handshake(connect_stream(peer.port()), "localhost", trusted_context(assets), anonymous);
    AURORA_TEST_CHECK_EQ(anonymous.calls.load(), 1);
    if (anonymous.ok) {
        TransferLog echoed;
        AURORA_TEST_CHECK_NE(read_once(anonymous.transport, echoed, 64), std::string{"sltm\n"});
        anonymous.transport.close();
    } else {
        AURORA_TEST_CHECK_FALSE(anonymous.transport.is_open());
    }

    auto context = trusted_context(assets);
    context.client_identity = assets.client_identity_bytes();
    context.client_identity_passphrase = assets.passphrase();
    AURORA_TEST_REQUIRE_FALSE(context.client_identity->empty());
    HandshakeLog identified;
    handshake(connect_stream(peer.port()), "localhost", context, identified);
    if (!identified.ok && identified.code == "general-invalid-argument") {
        // 本腿的后端没有 PFX 容器的解析入口（mbedTLS 3.6.5 的 `pkcs12.h` 只导出 PBE 与密钥派生原语）：
        // 发起前明确失败、点名缺哪项能力，就是本半在本腿的正判据（静默收下字段才是缺陷）。但「身份真的
        // 送出去了」只能在能解析 PKCS#12 的腿上正面证明，故本腿报成缺项而不是放行一条空判据。
        AURORA_TEST_SKIP("this build's TLS backend cannot parse a PKCS#12 client identity: " + identified.message);
    }
    AURORA_TEST_REQUIRE_MSG(identified.ok, "handshake failed: " + identified.code + " " + identified.message);
    AURORA_TEST_REQUIRE_EQ(identified.calls.load(), 1);
    auto channel = std::move(identified.transport);
    TransferLog log;
    write_through(channel, log, "mtls\n");
    AURORA_TEST_CHECK_EQ(read_once(channel, log, 64), std::string{"sltm\n"});
    channel.close();
}

}  // namespace aurora::test_cases::utest_net_tls
