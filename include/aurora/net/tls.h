#pragma once

/// @brief 安全通道（TLS 切片：L1 字节流之上的 TLS 客户端）。
///
/// 本头把「加密」做成 L1 流的一层叠加，而不是一条新的传输形态：握手成功之后，读写、背压、暂停、
/// 取消、半关闭与 `StreamTransport` **逐字同构**，因此 L3 协议引擎（HTTP / SSE / WebSocket）不需要
/// 知道底下是否加密——这正是 `net/transport.h` 里「本机 IPC 与 TCP 同型」那条裁决的推论。
///
/// 四条形态裁决（规范性，落码与文档回写以此为准）：
/// - **只有客户端**：本层不提供服务端 TLS（监听腿的加密归宿主服务自己负责，Aurora 的监听口是本机
///   IPC 与 Inspector 的回路，不构成公网服务端场景），因此没有 `TlsListener` 这一类形态。
/// - **后端可插拔，运行期以能力位回答**：TLS 引擎由 `net/tls_backend.h` 的注册表提供——构建配置决定
///   哪几枚内置腿在案（系统栈与 provider 腿**可以并存**，不是二选一），应用还能注入自研实现；
///   `Engine::capabilities().tls` 回答「注册表是否非空」，是唯一真值源。本头**不含任何平台宏**
///   （`SPEC.PLATFORM.ZERO-IFDEF.001`），也没有第二个「TLS 是否可用」的查询入口——两个真相源会诱导
///   调用方去分支一条本腿并不存在的路径。选哪一枚由 `TlsContext::backend` 与全局候选链决定；没有可用
///   后端时 `handshake()` 恰一次以 `net-tls-engine-unavailable` 收尾，而不是静默降级成明文。
/// - **校验默认开启且失败即断连**：证书链、有效期、主机名任一项不过即以 `net-tls-cert-rejected` 收尾
///   并关闭底层流。绕过校验只有 `allow_insecure_certificate` 一枚显式命名的危险开关，且启用它必然
///   产生一条 `warning` 级诊断（用途仅限本地自签测试）。
/// - **握手失败即流作废**：`handshake()` 接管传入的流，失败时不留「还能接着用」的错觉——TLS 语义上
///   握手未完成的连接已无可恢复状态，留着它只会让调用方在一条对端已判死的连接上继续等。
///
/// 信任根与 pin 的分工：`trusted_certificates` 是**追加**可信根（本地自签、企业内 CA），它改变「谁算
/// 可信签发者」；`pinned_public_keys` 是**叠加**约束（对端公钥必须命中集合之一），它不放松校验、只
/// 加严，命中失败落 `net-tls-pin-mismatch`。二者不可互相替代：把 pin 当校验开关用会让「pin 没命中但
/// 链可信」的连接静默通过，而链可信恰恰是中间人最擅长伪造的那一半。
///
/// @code
/// aurora::net::TlsContext tls;
/// tls.alpn_protocols = {"h2", "http/1.1"};
/// aurora::net::TlsTransport::handshake(
///     std::move(stream), "example.com", tls, aurora::net::Cancellation{},
///     [](auto result) {
///         if (!result) { return; }
///         auto secure = std::move(result.value());
///         secure.write(request, [](auto written) { /* ... */ });
///     });
/// @endcode
/// @note Thread: 见各类型；句柄本身单所有权，跨线程移交用 std::move
/// @note Side-effects: 后端凭据与上下文句柄的创建、TLS 记录级收发（不新增操作系统句柄类型）

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/transport.h"

namespace aurora::net {

class TlsTransport;

namespace detail {
struct TlsState;  ///< 一次 TLS 会话的状态（字段与后端句柄全部留在实现侧，公共头不暴露）
class TlsFactory;  ///< 实现侧工厂：唯一能以状态指针构造 `TlsTransport` 的类型
}  // namespace detail

/// @brief TLS 协议版本档位（`TlsContext` 的下界与上界入参）。
/// @note 档位是**允许集**的表达而非「精确指定某一版」：`min_version` 之下的版本一律不参与协商，
///       上界未置值时由后端给出其最高支持版本。
enum class TlsVersion : std::uint8_t {
    Tls10,  ///< TLS 1.0（仅为对接遗留服务端保留；多数后端已默认停用）
    Tls11,  ///< TLS 1.1
    Tls12,  ///< TLS 1.2（缺省下界）
    Tls13,  ///< TLS 1.3
};

/// @brief 对端证书的可核验摘要（不含平台证书句柄，因此可在任何腿上复制与比较）。
///
/// `public_key_sha256` 是 SPKI pin 的标准形态：对证书 `SubjectPublicKeyInfo` 的 DER 编码取 SHA-256
/// 再 base64（不是整证摘要，也不是十六进制指纹）。这一条是 pin 集合唯一可互通的写法——按整证摘要
/// pin 会在证书续签（公钥不变）时无预警失配，而那是运维最常见的「换证不换钥匙」。
/// `subject` 与 `issuer` 按证书 DER 里的属性次序原样渲染成 `短名=值`、条目之间以 `, ` 连接，四腿同一
/// 形态；但**跨腿逐字可比的是上面那枚摘要**，文本只用于展示与日志——罕见 DN（值内含逗号、引号或
/// 非 ASCII）的转义表随各腿的证书库而异，本层不归一。
/// @note Thread: thread-safe（值类型）
struct PeerCertificate {
    std::string subject;  ///< 主体可分辨名文本
    std::string issuer;  ///< 签发者可分辨名文本
    std::string not_before;  ///< 有效期起点（UTC ISO-8601）
    std::string not_after;  ///< 有效期终点（UTC ISO-8601）
    std::string public_key_sha256;  ///< SPKI pin 形态的公钥摘要（base64(SHA-256(SPKI DER))）
};

/// @brief 一条 TLS 通道的安全口径（值类型；一次配置可供多次连接复用）。
///
/// 未置值的字段一律取「更安全的缺省」：校验开、pin 空、信任根只用系统库。危险开关成对出现但语义
/// 不同——`allow_insecure_certificate` 关掉的是**链与主机名**的判定，它不影响 pin（pin 命中判定
/// 始终执行），因为「不设防」与「防错对象」是两个必须能被分别表达的决策。
/// @note Thread: thread-safe（值类型；握手期间只读）
struct TlsContext {
    /// @brief 协商版本的下界；缺省 TLS 1.2。调低它是显式的安全基线决策，不是兼容性默认。
    TlsVersion min_version = TlsVersion::Tls12;
    /// @brief 协商版本的上界；未置值=后端最高支持版本。
    std::optional<TlsVersion> max_version;
    /// @brief ALPN 协议标识，按偏好次序排列；空集=不宣告 ALPN（协商结果随之为空）。
    std::vector<std::string> alpn_protocols;
    /// @brief 危险开关：**仅限本地自签测试**。置真即跳过对端证书校验（链、有效期、主机名）并产生
    ///        一条 warning 级诊断；用它对接生产端点等于放弃本层的全部安全属性。校验没有第二个开关，
    ///        缺省（置假）即开启——两枚字段各说一半会造出「关掉校验却没授权」这类无法表达的中间态。
    /// @note 它不影响 `pinned_public_keys` 的命中判定：pin 判定始终执行，「不设防」与「防错对象」是
    ///       两个必须能被分别表达的决策。
    bool allow_insecure_certificate = false;
    /// @brief 追加可信根（PEM 文本，可多张）；与系统信任库**合并**而非替换。
    std::vector<std::string> trusted_certificates;
    /// @brief SPKI pin 集合（`PeerCertificate::public_key_sha256` 同形态）；空集=不设 pin。
    std::vector<std::string> pinned_public_keys;
    /// @brief 客户端身份（mTLS）：PKCS#12 档案的**字节内容**；未置值=不发送客户端证书。
    ///        与下面两枚 PEM 字段互斥（同时置值即 `net-option-conflict`，本层不猜以谁为准）。
    std::optional<std::string> client_identity;
    /// @brief `client_identity` 的保护口令；档案无口令时留空。
    std::optional<std::string> client_identity_passphrase;
    /// @brief 客户端身份的 PEM 形态（mTLS）：**证书链**文本，叶证书在前、其后依次是中间 CA，一段
    ///        `-----BEGIN CERTIFICATE-----` 到 `-----END CERTIFICATE-----` 里可串多张。与
    ///        `client_private_key_pem` 成对置值，且与上面的 PKCS#12 形态互斥。
    /// @note 支持腿：OpenSSL 与 mbedTLS。走系统证书库的两腿（Windows Schannel、Apple
    ///       SecureTransport）没有「从内存里的 PEM 造一枚可导出的身份」这条公开通路，置值即在起手落
    ///       `net-platform-unavailable`——那是显式拒绝，不是回落成不带证书的连接。
    std::optional<std::string> client_certificate_pem;
    /// @brief 客户端身份的 PEM 形态（mTLS）：**私钥**文本（PKCS#8 或各后端的传统形态；加密口令本层不解，
    ///        请先行解密后传入）。置了证书就必须同时置它，缺一即 `net-option-conflict`；两者都读得开时
    ///        后端在装进会话**之前**先核对私钥与叶证书配不配对（OpenSSL 的 `X509_check_private_key`、
    ///        mbedTLS 的 `mbedtls_pk_check_pair`），配错即 `general-invalid-argument` 且不放行。
    /// @note 支持腿同 `client_certificate_pem`。
    std::optional<std::string> client_private_key_pem;
    /// @brief 本次握手指定后端（`available_tls_backends()` 里的 id）；未置值即按全局缺省候选链选择。
    ///        置值即**指名即用、绝不回落**：显式指定后端常常含合规或兼容性口径（「这条连接必须走系统栈，
    ///        信任根归操作系统管」），悄悄换成另一枚等于推翻调用方的决定。
    /// @note 指名不存在的 id 落 `net-tls-engine-unavailable`（detail 点名实际在案的 id）；指名那枚后端
    ///       表达不了本口径落 `net-platform-unavailable`。两种都是明确失败，不会被别的后端「凑成功」。
    ///       缺省链形态相反：后端不在案或明确表态不可用时继续下一枚，理由见 `net/tls_backend.h`。
    std::optional<std::string> backend;
};

/// @brief 一次握手的结果：成功携已建立的加密流，失败携原因（含 `net-tls-*` 系列与底层流的原因）。
using TlsHandshakeDone = std::function<void(Result<TlsTransport>)>;

/// @brief 一条已握手的 TLS 通道（move-only RAII 句柄；析构即关闭底层流）。
///
/// 与 `StreamTransport` 的分工：本类型只多做两件事——握手，以及把读写换成记录级收发。其余语义
/// （读队列按登记次序满足、0 字节=对端关闭写向、写高水位背压、暂停读、取消）逐字沿用 L1，
/// 因为「同构」的全部价值在于上层只有一套读写纪律可学。
/// @note Thread: 状态 thread-safe（随底层流串行化）；句柄单所有权，移动即移交
/// @note Side-effects: TLS 记录的加解密与底层流的收发
class TlsTransport {
  public:
    /// @brief 构造空句柄（占位 / 移动后的落点）。
    TlsTransport() = default;

    TlsTransport(const TlsTransport &) = delete;
    auto operator=(const TlsTransport &) -> TlsTransport & = delete;
    /// @brief 移动构造：所有权移交，源转为空态。
    /// @param other 被移交的源句柄。
    TlsTransport(TlsTransport &&other) noexcept;
    /// @brief 移动赋值：先关闭自有通道再接管。
    /// @param other 被移交的源句柄。
    /// @return 本对象引用。
    auto operator=(TlsTransport &&other) noexcept -> TlsTransport &;
    /// @brief 析构：通道仍开着则关闭（等价 `close()`）。
    ~TlsTransport();

    /// @brief 在已建立的流上发起握手（非阻塞；结果恰一次经 `on_done` 抵达）。
    ///
    /// 传入的流被本函数接管：无论成败都不再归调用方（失败即底层流关闭，理由见本头开头的「握手失败
    /// 即流作废」）。`server_name` 同时用于 SNI 与主机名校验，因此**必须是 DNS 名而不是 IP 字面量**
    /// ——IP 目标按证书校验语义没有可核对的名字，本层对它落 `net-endpoint-invalid` 而不是跳过校验。
    /// @param stream 已连接、尚未握手的底层流。
    /// @param server_name 目标主机名（SNI + 证书主机名校验的输入）；空串落 `net-endpoint-invalid`。
    /// @param context 安全口径（版本界、ALPN、校验、信任根、pin、客户端身份）。
    /// @param cancellation 本次握手的取消令牌；`cancel()` 后回调恰一次以 `net-cancelled` 抵达。
    /// @param on_done 完成回调；成功携已握手的通道。
    static auto handshake(StreamTransport &&stream, std::string_view server_name, const TlsContext &context,
                          const Cancellation &cancellation, TlsHandshakeDone on_done) -> void;

    /// @brief 通道是否仍开着（底层流关闭或收到对端 close_notify 后为 false）。
    /// @return 已握手且未关闭为真；空态、已关闭或已落败为假。
    [[nodiscard]] auto is_open() const -> bool;

    /// @brief 握手后读出的对端证书摘要。
    /// @return 成功为摘要；空态或已关闭为 `net-socket-failed`。
    /// @note 摘要在握手完成那一刻固化，此后 `close()` 之前都可读；它不是实时查询，因此不受对端后续
    ///       状态影响，也不引入「证书对象生命周期」这一层公共语义。
    [[nodiscard]] auto peer_certificate() const -> Result<PeerCertificate>;

    /// @brief 协商出的 ALPN 协议标识。
    /// @return 命中值；未宣告 ALPN、对端未选择或本腿后端不回报时为 `net-option-conflict`
    ///         （**形态不匹配而不是失败**：把「没有 ALPN」报成空串会让调用方把「未协商」误读成「协商
    ///         出一个空协议」，而后者的正确处置是关掉连接）。
    [[nodiscard]] auto negotiated_alpn() const -> Result<std::string>;

    /// @brief 实际生效的后端标识（`TlsContext::backend` 的指名结果或候选链选中的那一枚）。
    /// @return 成功携注册表里的 id；空态或已关闭为 `net-socket-failed`。
    /// @note id 在握手起手那一刻固化，与 `peer_certificate()` 同理：通道关掉后再报「当前后端」会诱导
    ///       调用方拿它去复用一条已不属于任何口径的连接。可观测性靠它——同一份配置在不同构建里可能
    ///       落在不同腿上，而出错的那条腿正是排查的起点。
    [[nodiscard]] auto backend() const -> Result<std::string>;

    /// @brief 登记一次解密读请求（缓冲必须存活到本次回调抵达；语义同 `StreamTransport::read`）。
    /// @param buffer 目标缓冲；空 span 的用法错误以 `net-buffer-limit` 形态经回调抵达。
    /// @param on_done 完成回调；携实际交付的明文字节数，0 = 对端已关闭写向。
    auto read(std::span<char> buffer, ReadDone on_done) const -> void;

    /// @brief 登记一次加密写请求（语义同 `StreamTransport::write`，背压判据落在密文待发字节上）。
    /// @param data 待发送明文；空 span 直接以 OK 完成（不产生记录）。
    /// @param on_done 完成回调；携已交付底层流的明文字节数。
    auto write(std::span<const char> data, WriteDone on_done) -> void;

    /// @brief 暂停读投递：不再从底层流取字节，密文留在内核与对端缓冲里。
    auto pause_read() const -> void;

    /// @brief 恢复读投递（未暂停时为空操作）。
    auto resume_read() const -> void;

    /// @brief 当前是否处于暂停读状态。
    /// @return 上一次 `pause_read()` 生效且尚未 `resume_read()` 时为真。
    [[nodiscard]] auto read_paused() const -> bool;

    /// @brief 待发明文字节数（背压断言点，口径同 `StreamTransport::pending_write_bytes`）。
    /// @return 已排队但尚未交付底层流的明文字节数；空态为 0。
    [[nodiscard]] auto pending_write_bytes() const -> std::size_t;

    /// @brief 关闭通道：发出 close_notify（尽力而为）、丢弃未完成的读写请求并关闭底层流。幂等。
    auto close() const -> void;

    /// @brief 本通道的取消令牌（`cancel()` 会中止未完成的握手与读写并关闭通道）。
    /// @return 与本对象共享状态的令牌；空态返回一枚独立令牌（取消它不产生任何效果）。
    [[nodiscard]] auto cancellation() const -> Cancellation;

  private:
    friend class detail::TlsFactory;

    /// @brief 以既有状态接管（仅实现侧工厂使用）。
    /// @param state 已握手的会话状态；空指针即空态。
    explicit TlsTransport(std::shared_ptr<detail::TlsState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::TlsState> state_;  ///< 会话状态（空态为 nullptr）
};

}  // namespace aurora::net
