// Aurora net — SecureTransport 系统栈后端（CHG-018 TLS 切片：macOS 缺省装配，提案 §5 平台矩阵）。
//
// 与 Schannel 腿同一套取舍：用系统栈而不是自带 TLS 源码，零依赖、随系统 servicing 打补丁，记录保护
// 与版本降级策略由系统承担；本层只做三件系统做不了或做不对的事——把字节流换成内存推拉、把信任判定
// 上收到本层（追加信任根在系统通路里做不到）、把 SPKI 摘要算成跨腿可比的形态。
//
// 形态要点（每一条都对应一处不可省略的代码）：
//   - **内存 I/O**：`SSLSetIOFuncs` + `SSLSetConnection` 让 SecureTransport 从本层的缓冲取字节、把
//     记录写进本层的缓冲，因此本后端与其余两腿共用同一条「上层负责交付底层流」的接缝（见
//     `detail/tls_backend.h`）。接缝要求「握手未完成即不得自旋等待」，故取不到字节时回
//     `errSSLWouldBlock`——文档原话：它表示「少于请求的量已传输，只是所请求的 I/O 还没完成」。
//   - **信任判定上收**：`kSSLSessionOptionBreakOnServerAuth` 让 `SSLHandshake` 在服务端认证完成处返回
//     `errSSLServerAuthCompleted` 交回控制权；同一页文档写明设了它即关闭 SecureTransport 对服务端证书
//     的自动校验，所以追加根只能自己接：`SecTrustSetAnchorCertificates` +
//     `SecTrustSetAnchorCertificatesOnly(false)`（false = 与系统根**合并**，不是替换），判定用
//     `SecTrustEvaluateWithError`。校验完**必须再调一次** `SSLHandshake` 续跑——那是文档规定的动作。
//   - **主机名交系统核**：`SecPolicyCreateSSL(true, hostname)` 的文档写明「指定该值即要求它与叶子证书里的
//     名字匹配」，这一条就是平台的 RFC 6125 通路，本层不再自己写一份匹配规则——与 OpenSSL 腿把名字判定交给
//     `SSL_set1_host` 是同一取舍。与 Schannel 腿因此有一处**已知不重合**：那腿的手工校验通路拿不到名字判定
//     （`InitializeSecurityContext` 的目标名只用于 SNI 与票据），只能自己实现，于是它对「只有 CN、没有 SAN」
//     的证书回落到 CN，而本平台按自己的现行策略裁决。这一处差异是平台策略而非本层规则，跨腿对账按此读。
//     失败诊断仍带上目标名（公共面 `net-tls-cert-rejected` 的可归因要求），并把系统的 CFError 描述一并带出。
//   - **读链之前必须先评估**：`SecTrustGetCertificateAtIndex` 与 `SecTrustGetCertificateCount` 的文档都
//     写明「调用之前必须先 `SecTrustEvaluateWithError`」。因此危险开关 `allow_insecure_certificate` 在
//     本后端跳过的是**结论**而不是**动作**：仍然评估一次，只是不采纳它的结果。这样两种档位下拿到的
//     证书摘要形态完全一致，不会出现「设了开关反而读不到对端证书」。
//   - **SPKI 摘要自己切 DER**：`SecKeyCopyExternalRepresentation` 的文档写明 RSA 返回 PKCS#1、EC 返回
//     ANSI X9.63 的 `04 || X || Y`——两者都不是 `SubjectPublicKeyInfo` 的 DER，取它算 pin 会与
//     Schannel/OpenSSL 腿永久失配。故本层自己走一遍证书 DER（按 RFC 5280 的 TBSCertificate 字段次序），
//     切出 SPKI 的整段 TLV 再取 SHA-256 + base64（RFC 7469 对 pin 的定义就是这整段）。
//
// 版本与可用性边界（写下来是为了让「为什么是这套调用」可被复查）：
//   - 本 TU 只用 `<Security/Security.h>` 与 `<CoreFoundation/CoreFoundation.h>`：APPLE 腿只链接这两枚
//     framework（见 `cmake/AuroraNetTls.cmake`）。Network.framework 虽是指定的替代者，但它是回调驱动的
//     连接抽象（自带 I/O 与队列），无法表达「本层持有密文缓冲、上层交付底层流」这套接缝，还要多链一枚
//     framework，因此本切片不采用。
//   - 整套 SecureTransport 自 macOS 10.15 起弃用（文档原话「No longer supported. Use Network
//     framework.」），而提案 §5 指定 macOS 走系统栈，故本 TU 需要整段关闭弃用告警，见下方 pragma 处。
//   - `SSLSetALPNProtocols` / `SSLCopyALPNProtocols` 自 macOS 10.13 起存在；`SecTrustEvaluateWithError`
//     自 macOS 10.14 起存在；取链的两个访问器自 macOS 10.7 起存在（`SecTrustGetCertificateAtIndex` 在
//     macOS 12 起弃用，而 12 之前没有任何非弃用的取链口——`SecTrustCopyCertificateChain` 要 macOS 12）。
//     本仓不设 `CMAKE_OSX_DEPLOYMENT_TARGET`，故选可用性下限最低的那一组，把运行时下限留在 10.14。
//   - SHA-256 用 CommonCrypto 的 `CC_SHA256`（`<CommonCrypto/CommonDigest.h>`，属 libSystem，不需额外
//     链接）。曾考虑的 `SecTransform` + `kSecDigestSHA2` 已被文档判死（"SecTransform is no longer
//     supported"），故不用。

#ifdef AURORA_NET_TLS_SECURE_TRANSPORT

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/detail/tls_backend.h"
#include "aurora/net/tls.h"

// 整套 SecureTransport 自 macOS 10.15 起弃用（文件头给了不改用 Network.framework 的理由），本 TU 的
// 每一次 SSL*/SecTrust 调用都会撞上 `-Wdeprecated-declarations`。用区间而不是逐点：本文件里除 CF 与
// CommonCrypto 之外的系统调用**全部**是弃用 API，逐点抑制会把四十处噪声写成四十行豁免，反而淹没真正
// 需要留痕的那两处 const_cast。CF 与 CommonCrypto 自身未弃用，因此本区间对它们不构成遮蔽。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

namespace aurora::net::detail {
namespace {

constexpr std::size_t kMaxRecordBodyBytes = 16384;  ///< 一条 TLS 记录的明文上限（RFC 5246 §6.2.1）
constexpr std::size_t kMaxPlainReadBytes = 16384;  ///< 单次 `SSLRead` 的取字节上限（一记录的量）
constexpr int kMaxHandshakeRounds = 8;  ///< 一次 `handshake()` 调用内的续跑轮次上限
constexpr std::size_t kMaxAlpnProtocolIdBytes = 255;  ///< RFC 7301 的协议标识上限（列表长度前缀是 u8）

/// DER 通用标签（X.680 标签号；本层只用得到这些）。
constexpr std::uint8_t kTagSequence = 0x30;
constexpr std::uint8_t kTagOid = 0x06;
constexpr std::uint8_t kTagUtcTime = 0x17;
constexpr std::uint8_t kTagGeneralizedTime = 0x18;
constexpr std::uint8_t kTagUniversalString = 0x1C;
constexpr std::uint8_t kTagBmpString = 0x1E;

auto tls_error(ErrorCode code, std::string text) -> Error {
    return make_error(code, ErrorParams{{"detail", std::move(text)}});
}

/// OSStatus 的十六进制文本（与 Schannel 腿的 `hex_status` 同形）。
///
/// 为什么只带数字而不带符号名：SecureTransport 的结果码有百余个，符号名要再引一张表，而现场需要的只是
/// 「哪一次调用的哪个码」这一枚可对账的标识；数值也正是 Apple 文档与 bug 报告里用来指认结果码的形态。
auto hex_status(OSStatus status) -> std::string {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(status));
    return {buffer};
}

auto hex_number(long long value) -> std::string {
    char buffer[24] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08llx", static_cast<unsigned long long>(value));
    return {buffer};
}

/// 证书类结果码：命中即归 `net-tls-cert-rejected`，否则算握手层失败。
///
/// 名单只列 Apple 文档里确实存在的符号（`SSLHandshake` 的返回值说明与「Secure Transport 结果代码」两页
/// 各给一半），不猜同族命名——例如文档里没有 `errSSLProtocolVersion`，存在的是
/// `errSSLPeerProtocolVersion`，写错的那一枚连编译都过不去。
auto is_certificate_status(OSStatus status) -> bool {
    switch (status) {
        case errSSLBadCert:
        case errSSLCertExpired:
        case errSSLCertNotYetValid:
        case errSSLHostNameMismatch:
        case errSSLNoRootCert:
        case errSSLUnknownRootCert:
        case errSSLXCertChainInvalid:
        case errSSLPeerCertExpired:
        case errSSLPeerCertRevoked:
        case errSSLPeerCertUnknown:
        case errSSLPeerUnsupportedCert:
        case errSSLPeerUnknownCA:
            return true;
        default:
            return false;
    }
}

auto status_error(OSStatus status, std::string_view phase) -> Error {
    const auto text = std::string{phase} + ", status=" + hex_status(status);
    if (is_certificate_status(status)) {
        return tls_error(ErrorCode::NetTlsCertRejected, text);
    }
    return tls_error(ErrorCode::NetTlsHandshakeFailed, text);
}

/// CF 引用的一次性接管。
///
/// `CFRelease` 对 NULL 是运行时崩溃（文档在 Special Considerations 里明写「if cf is NULL, this will
/// cause a runtime error」），故判空收在一处，比在每个释放点都写一遍 `if` 可靠。
class CfReleaser {
  public:
    /// @brief 接管一个 Create/Copy 得来的引用。
    /// @param object 对象引用；空指针按「无可释放」处理。
    explicit CfReleaser(CFTypeRef object) : object_(object) {}
    CfReleaser(const CfReleaser &) = delete;
    auto operator=(const CfReleaser &) -> CfReleaser & = delete;
    CfReleaser(CfReleaser &&) = delete;
    auto operator=(CfReleaser &&) -> CfReleaser & = delete;
    /// @brief 归还接管时那份引用。
    ~CfReleaser() {
        if (object_ != nullptr) {
            ::CFRelease(object_);
        }
    }

  private:
    CFTypeRef object_{nullptr};
};

/// UTF-8 文本 → CFString（Create 规则：调用方释放）。
///
/// 先落地一份副本而不是直接交 `string_view.data()`：`CFStringCreateWithCString` 要求 NUL 结尾的串，
/// 而 `string_view` 不保证——少一个终止符就是越界读。
auto to_cfstring(std::string_view text) -> CFStringRef {
    const std::string terminated(text);
    return ::CFStringCreateWithCString(kCFAllocatorDefault, terminated.c_str(), kCFStringEncodingUTF8);
}

/// CFString → UTF-8 文本。
///
/// 不用 `CFStringGetCStringPtr`：文档允许它在非连续内部表示时返回 NULL，而「返回 NULL 就当没有值」会
/// 把真的协商结果读成没协商。固定两步形态（先按 UTF-16 单元数问上界，再填入）不受内部表示影响。
[[nodiscard]] auto from_cfstring(CFStringRef string) -> std::string {
    if (string == nullptr) {
        return {};
    }
    const CFIndex length = ::CFStringGetLength(string);
    if (length <= 0) {
        return {};
    }
    const CFIndex capacity = ::CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
    if (capacity <= 0) {
        return {};
    }
    std::vector<char> buffer(static_cast<std::size_t>(capacity) + 1U, '\0');
    if (::CFStringGetCString(string, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8) == 0) {
        return {};
    }
    return {buffer.data()};
}

/// CFError → 「系统原话 + 码」的现场文本（信任判定失败时唯一能带出来的证据）。
[[nodiscard]] auto describe_cferror(CFErrorRef error) -> std::string {
    if (error == nullptr) {
        return {};
    }
    std::string text = " (" + hex_number(static_cast<long long>(::CFErrorGetCode(error))) + ")";
    const CFStringRef copy = ::CFErrorCopyDescription(error);  // Copy 规则：调用方释放（文档保证非 NULL）
    if (copy != nullptr) {
        const CfReleaser guard{copy};
        if (auto described = from_cfstring(copy); !described.empty()) {
            text = ": " + described + text;
        }
    }
    return text;
}

/// 版本档位 → SecureTransport 的 `SSLProtocol` 值。
///
/// 不给 SSL 3.0 及更早的档位：公共面 `TlsVersion` 从 TLS 1.0 起算，本层的下界映射也只到
/// `kTLSProtocol1`——留一个能调到 SSL 的口子等于没有下界。
auto protocol_value(TlsVersion version) -> SSLProtocol {
    switch (version) {
        case TlsVersion::Tls10:
            return kTLSProtocol1;
        case TlsVersion::Tls11:
            return kTLSProtocol11;
        case TlsVersion::Tls12:
            return kTLSProtocol12;
        case TlsVersion::Tls13:
            return kTLSProtocol13;
    }
    return kTLSProtocol12;
}

/// 档位的可比较次序：只用于「上界低于下界」这条入参自检，不参与任何协商。
///
/// 为什么不用 `protocol_value` 的返回值直接比：`SSLProtocol` 的数值是 Apple 的内部编号（TLS 1.3 是 402
/// 而不是 4），拿它当次序来比较是把巧合当契约。
auto version_rank(TlsVersion version) -> int {
    switch (version) {
        case TlsVersion::Tls10:
            return 0;
        case TlsVersion::Tls11:
            return 1;
        case TlsVersion::Tls12:
            return 2;
        case TlsVersion::Tls13:
            return 3;
    }
    return 2;
}

/// base64 编码（SPKI 摘要的公共面形态：标准字母表 + `=` 填充，无换行）。
///
/// 自己写而不用 `SecTransform`：那条通路已被文档判死；系统侧也没有可链接的 base64 编码口在本腿允许的
/// 两枚 framework 之内。表与仓内既有实现（`net/detail/proxy_wire.cpp`）逐字一致。
auto to_base64(std::span<const std::uint8_t> bytes) -> std::string {
    constexpr std::string_view table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
        const auto remaining = bytes.size() - offset;
        const auto first = static_cast<unsigned>(bytes[offset]);
        const auto second = remaining > 1U ? static_cast<unsigned>(bytes[offset + 1U]) : 0U;
        const auto third = remaining > 2U ? static_cast<unsigned>(bytes[offset + 2U]) : 0U;
        const auto group = (first << 16U) | (second << 8U) | third;
        text.push_back(table[(group >> 18U) & 0x3FU]);
        text.push_back(table[(group >> 12U) & 0x3FU]);
        text.push_back(remaining > 1U ? table[(group >> 6U) & 0x3FU] : '=');
        text.push_back(remaining > 2U ? table[group & 0x3FU] : '=');
    }
    return text;
}

auto base64_value(char character) -> int {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

/// base64 解码：跳过空白，遇填充即收，遇表外字符即整块作废。
///
/// 「作废而不是跳过」是刻意的：PEM 正文里出现表外字符只可能是资产坏了，跳过它会解出一段看似合理实则
/// 错位的 DER，而错位的 DER 会被后续解析读成一张名字完全不同的证书——那时现场指向的是证书，不是资产。
[[nodiscard]] auto from_base64(std::string_view text) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> bytes;
    bytes.reserve((text.size() / 4U) * 3U + 3U);
    unsigned accumulator = 0;
    unsigned bits = 0;
    for (const char character : text) {
        if (character == '=') {
            return bytes;  // 填充之后不再有正文
        }
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            continue;
        }
        const auto value = base64_value(character);
        if (value < 0) {
            return {};
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6U;
        if (bits >= 24U) {
            bits -= 24U;
            bytes.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFFU));
            accumulator &= (1U << bits) - 1U;
        }
    }
    return bytes;
}

/// SHA-256：CommonCrypto 的一步式接口（手册页给的形态：`md` 由调用方备好 32 字节）。
///
/// 长度入参是 `CC_LONG`（u32）。SPKI 的 DER 远小于 4 GiB，但仍显式判一次边界：把截断读成「算完了」会让
/// pin 与另一枚同样截断的配置值相等，那是本层最难发现的错法。
[[nodiscard]] auto sha256(std::span<const std::uint8_t> bytes) -> std::vector<std::uint8_t> {
    constexpr std::size_t kDigestBytes = static_cast<std::size_t>(CC_SHA256_DIGEST_LENGTH);
    if (bytes.size() > static_cast<std::size_t>(0xFFFFFFFFUL)) {
        return {};
    }
    std::vector<std::uint8_t> digest(kDigestBytes, std::uint8_t{0});
    (void)::CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    return digest;
}

/// PEM → 每张证书的 DER：一张字符串里串多个 `-----BEGIN CERTIFICATE-----` 块是合法输入。
[[nodiscard]] auto pem_blocks(std::string_view pem) -> std::vector<std::vector<std::uint8_t>> {
    constexpr std::string_view header = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view footer = "-----END CERTIFICATE-----";
    std::vector<std::vector<std::uint8_t>> blocks;
    for (std::size_t cursor = 0; cursor < pem.size();) {
        const std::string_view rest = pem.substr(cursor);
        const auto start = rest.find(header);
        if (start == std::string_view::npos) {
            break;
        }
        cursor += start + header.size();
        const std::string_view body = pem.substr(cursor);
        const auto stop = body.find(footer);
        if (stop == std::string_view::npos) {
            break;
        }
        if (auto der = from_base64(body.substr(0, stop)); !der.empty()) {
            blocks.push_back(std::move(der));
        }
        cursor += stop;
    }
    return blocks;
}

// ---- DER 读取（X.690 确定长度子集；RFC 5280 的证书一律是 DER）----

/// 一个 TLV：标签、正文，以及**含标签与长度字节的整段编码**（SPKI pin 取的是整段）。
struct DerElement {
    std::uint8_t tag = 0;
    std::span<const std::uint8_t> value;
    std::span<const std::uint8_t> encoded;
};

/// 从 `offset` 起读一个 TLV；非规范长度、越界一律判坏数据（返回未置值）。
///
/// 严格性是有意的：接受「长度不最小编码」就等于接受一份能被两种解释的证书，而 SPKI 摘要只对其中一种
/// 成立——那时 pin 会在一侧生成、另一侧校验，现场表现成「证书没变却 pin 不命中」。
[[nodiscard]] auto der_read(std::span<const std::uint8_t> bytes, std::size_t &offset) -> std::optional<DerElement> {
    if (offset + 2U > bytes.size()) {
        return std::nullopt;
    }
    const auto tag = bytes[offset];
    std::size_t cursor = offset + 1U;
    if ((tag & 0x1FU) == 0x1FU) {  // 高位标签形式：后续字节的最高位是延续位
        while (cursor < bytes.size() && (bytes[cursor] & 0x80U) != 0U) {
            ++cursor;
        }
        if (cursor >= bytes.size()) {
            return std::nullopt;
        }
        ++cursor;
    }
    if (cursor >= bytes.size()) {
        return std::nullopt;
    }
    const auto first = bytes[cursor++];
    std::size_t length = 0;
    if ((first & 0x80U) == 0U) {
        length = first;
    } else {
        const auto count = static_cast<std::size_t>(first & 0x7FU);
        if (count == 0U || count > 4U || cursor + count > bytes.size()) {
            return std::nullopt;
        }
        if (bytes[cursor] == 0U) {
            return std::nullopt;  // 长形式的首字节不得为 0（最小编码）
        }
        for (std::size_t index = 0; index < count; ++index) {
            length = (length << 8U) | bytes[cursor++];
        }
        if (length < 0x80U) {
            return std::nullopt;  // 能用短形式时不得用长形式
        }
    }
    if (length > bytes.size() - cursor) {
        return std::nullopt;
    }
    DerElement element;
    element.tag = tag;
    element.value = bytes.subspan(cursor, length);
    element.encoded = bytes.subspan(offset, (cursor - offset) + length);
    offset = cursor + length;
    return element;
}

/// 一个构造值的子元素列表；正文里有一处读不出即整份判坏。
[[nodiscard]] auto der_children(std::span<const std::uint8_t> value) -> std::optional<std::vector<DerElement>> {
    std::vector<DerElement> children;
    std::size_t offset = 0;
    while (offset < value.size()) {
        auto child = der_read(value, offset);
        if (!child) {
            return std::nullopt;
        }
        children.push_back(*child);
    }
    return children;
}

/// OBJECT IDENTIFIER → 点分文本（首字节 = 40*X + Y，其余是 base-128 的弧）。
[[nodiscard]] auto der_oid_text(std::span<const std::uint8_t> value) -> std::string {
    if (value.empty()) {
        return {};
    }
    const auto first = static_cast<unsigned>(value[0]);
    std::string text = std::to_string(first / 40U) + "." + std::to_string(first % 40U);
    for (std::size_t index = 1; index < value.size();) {
        unsigned arc = 0;
        std::size_t consumed = 0;
        while (index + consumed < value.size()) {
            const auto byte = static_cast<unsigned>(value[index + consumed]);
            arc = (arc << 7U) | (byte & 0x7FU);
            ++consumed;
            if ((byte & 0x80U) == 0U) {
                break;
            }
            if (consumed > 4U) {
                return {};  // 异常长的弧：不认为是合法 OID
            }
        }
        if (consumed == 0U) {
            return {};
        }
        index += consumed;
        text += "." + std::to_string(arc);
    }
    return text;
}

/// 属性类型 OID → 简名（RFC 5280 表 1 里常见的几枚）；未知名即回点分 OID 本身。
[[nodiscard]] auto attribute_type_name(std::string_view oid) -> std::string {
    if (oid == "2.5.4.3") {
        return "CN";
    }
    if (oid == "2.5.4.4") {
        return "SN";
    }
    if (oid == "2.5.4.5") {
        return "serialNumber";
    }
    if (oid == "2.5.4.6") {
        return "C";
    }
    if (oid == "2.5.4.7") {
        return "L";
    }
    if (oid == "2.5.4.8") {
        return "ST";
    }
    if (oid == "2.5.4.9") {
        return "STREET";
    }
    if (oid == "2.5.4.10") {
        return "O";
    }
    if (oid == "2.5.4.11") {
        return "OU";
    }
    if (oid == "2.5.4.12") {
        return "T";
    }
    if (oid == "0.9.2342.19200300.100.1.1") {
        return "DC";
    }
    if (oid == "0.9.2342.19200300.100.1.3") {
        return "E";
    }
    return std::string{oid};
}

/// 名字里的一个值 → 文本。
///
/// `Name` 的属性值是一组可选字符串类型（X.680 的 character string 家族），不是只有一种 UTF-8。8 位类型
/// 按字节原样交出去（现代证书一律 UTF8String）；BMPString/UniversalString 是定宽大端码元，不转换就等于
/// 把国际化主体名读成一堆空格——而那是唯一能解释「连到了错误的机器」的现场证据。
[[nodiscard]] auto der_string_value(const DerElement &element) -> std::string {
    if (element.tag == kTagBmpString) {  // UTF-16BE
        std::string text;
        for (std::size_t index = 0; index + 1U < element.value.size(); index += 2U) {
            const auto unit =
                (static_cast<unsigned>(element.value[index]) << 8U) | static_cast<unsigned>(element.value[index + 1U]);
            if (unit < 0x80U) {
                text.push_back(static_cast<char>(unit));
            } else if (unit < 0x800U) {
                text.push_back(static_cast<char>(0xC0U | (unit >> 6U)));
                text.push_back(static_cast<char>(0x80U | (unit & 0x3FU)));
            } else {
                text.push_back(static_cast<char>(0xE0U | (unit >> 12U)));
                text.push_back(static_cast<char>(0x80U | ((unit >> 6U) & 0x3FU)));
                text.push_back(static_cast<char>(0x80U | (unit & 0x3FU)));
            }
        }
        return text;
    }
    if (element.tag == kTagUniversalString) {  // UCS-4BE：只保 ASCII，其余以 `?` 呈现
        std::string text;
        for (std::size_t index = 0; index + 3U < element.value.size(); index += 4U) {
            const auto unit = (static_cast<unsigned>(element.value[index]) << 24U) |
                              (static_cast<unsigned>(element.value[index + 1U]) << 16U) |
                              (static_cast<unsigned>(element.value[index + 2U]) << 8U) |
                              static_cast<unsigned>(element.value[index + 3U]);
            text.push_back(unit < 0x80U ? static_cast<char>(unit) : '?');
        }
        return text;
    }
    return {element.value.begin(), element.value.end()};
}

/// `Name`（RDNSequence）→ 可分辨名文本。
///
/// 渲染顺序按 DER 里的次序原样输出、条目之间以 `, ` 连接、`=` 两侧不留空格。这与另三腿是同一形态：
/// Schannel 腿的 `CertNameToStr(CERT_X500_NAME_STR)` 与 mbedTLS 腿的 `mbedtls_x509_dn_gets` 实测对同一
/// 签发者给出 `C=US, O=SSL Corporation, CN=Cloudflare TLS Issuing ECC CA 3`（两条都是 DER 正序），
/// OpenSSL 腿因此把自己改成同一形态而不是沿用 `XN_FLAG_RFC2253` 的逆序。本腿无本机运行条件，
/// 这条一致性属**代码形态层面的一致**，跨腿逐字判据由 CI 的 `macos-securetransport-system` 腿补
/// （见 `codespec/manual-test/23-net.md` 的 `TC-NET-006` 与提案 §16 第 21 项）。
[[nodiscard]] auto der_read_name(std::span<const std::uint8_t> name) -> std::string {
    std::string result;
    const auto rdns = der_children(name);
    if (!rdns) {
        return result;
    }
    for (const auto &rdn : *rdns) {
        const auto pairs = der_children(rdn.value);
        if (!pairs) {
            continue;
        }
        std::size_t position = 0;
        for (const auto &pair : *pairs) {  // AttributeTypeAndValue ::= SEQUENCE { type, value }
            const auto parts = der_children(pair.value);
            if (!parts || parts->size() < 2U || (*parts)[0].tag != kTagOid) {
                continue;
            }
            const auto type = der_oid_text((*parts)[0].value);
            const auto value = der_string_value((*parts)[1]);
            if (!result.empty()) {
                result += position == 0U ? ", " : " + ";  // 同一 RDN 内的多值按 RFC 4514 用 ` + `
            }
            result += attribute_type_name(type) + "=" + value;
            ++position;
        }
    }
    return result;
}

/// `Validity` 的一个 `Time` → ISO-8601（UTC，秒级）文本。
///
/// 为什么不用 `SecCertificateCopyNotValidBeforeDate`：文档给的引入版本是 macOS 15.0，而本仓不设
/// `CMAKE_OSX_DEPLOYMENT_TARGET`——把公共面的摘要挂在只有最新系统才有的接口上不可接受。DER 里的时间按
/// RFC 5280 §4.1.2.5 一律是 GMT，因此解析不需要时区数学；只接受以 `Z` 收尾的形态，非 GMT 即判坏。
[[nodiscard]] auto der_time_text(const DerElement &element) -> std::string {
    const std::span<const std::uint8_t> &value = element.value;
    const std::string_view text{reinterpret_cast<const char *>(value.data()), value.size()};
    std::size_t digits = 0;
    if (element.tag == kTagUtcTime) {
        digits = 12U;  // YYMMDDHHMMSS
    } else if (element.tag == kTagGeneralizedTime) {
        digits = 14U;  // YYYYMMDDHHMMSS
    } else {
        return {};
    }
    if (text.size() < digits + 1U || text[digits] != 'Z') {
        return {};  // 带偏移或小数位的形态不采纳：判据要的是确定性，不是尽力解析
    }
    for (std::size_t index = 0; index < digits; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return {};
        }
    }
    std::size_t cursor = 0;
    const auto take = [&text, &cursor](std::size_t count) {
        unsigned number = 0;
        for (std::size_t index = 0; index < count; ++index) {
            number = number * 10U + static_cast<unsigned>(text[cursor++] - '0');
        }
        return number;
    };
    auto year = take(2U);
    if (element.tag == kTagUtcTime) {
        year += year >= 50U ? 1900U : 2000U;  // RFC 5280 的两位年窗口
    } else {
        year = year * 100U + take(2U);
    }
    const auto month = take(2U);
    const auto day = take(2U);
    const auto hour = take(2U);
    const auto minute = take(2U);
    const auto second = take(2U);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02dT%02u:%02u:%02uZ", year, month, day, hour, minute, second);
    return {buffer};
}

/// 证书 DER → 公共面摘要。
///
/// 定位按 RFC 5280 的字段次序：`TBSCertificate` 里的 SEQUENCE 子元素依次是 signature / issuer /
/// validity / subject / subjectPublicKeyInfo——`version [0]`、`uniqueID [1]`、`subjectUniqueID [2]`、
/// `extensions [3]` 都是上下文标签而非 SEQUENCE，所以它们有或没有都不影响这个序号。
[[nodiscard]] auto parse_certificate(std::span<const std::uint8_t> der) -> std::optional<PeerCertificate> {
    std::size_t offset = 0;
    const auto certificate = der_read(der, offset);
    if (!certificate || certificate->tag != kTagSequence) {
        return std::nullopt;
    }
    const auto outer = der_children(certificate->value);
    if (!outer || outer->empty() || outer->front().tag != kTagSequence) {
        return std::nullopt;
    }
    const auto fields = der_children(outer->front().value);
    if (!fields) {
        return std::nullopt;
    }
    std::vector<DerElement> sequences;
    for (const auto &field : *fields) {
        if (field.tag == kTagSequence) {
            sequences.push_back(field);
        }
    }
    if (sequences.size() < 5U) {
        return std::nullopt;
    }
    PeerCertificate parsed;
    parsed.subject = der_read_name(sequences[3].value);
    parsed.issuer = der_read_name(sequences[1].value);
    const auto validity = der_children(sequences[2].value);
    if (validity && validity->size() >= 2U) {
        parsed.not_before = der_time_text((*validity)[0]);
        parsed.not_after = der_time_text((*validity)[1]);
    }
    // SPKI pin 的口径：整段 subjectPublicKeyInfo 的 DER（含标签与长度）取 SHA-256 再 base64，与
    // Schannel 腿的 `CryptEncodeObject(X509_PUBLIC_KEY_INFO)` 和 OpenSSL 腿的 `i2d_PUBKEY` 同源。
    // 名字核对不在这里：它由系统的 `SecPolicyCreateSSL` 通路给结论（文件头有取舍理由）。
    parsed.public_key_sha256 = to_base64(sha256(sequences[4].encoded));
    return parsed;
}

class SecureTransportBackend;

/// 内存读回调：SecureTransport 从本层缓冲取对端字节。
/// 长度入参按 `SSLReadFunc` 的声明用 `size_t`（文档的 Swift 形态即 `UnsafeMutablePointer<Int>`）；写成
/// 别的整型在 LP64 上侥幸同宽，但那是函数指针类型的硬匹配，不是「差不多就行」。
auto memory_read(const void *connection, void *buffer, size_t *processed) -> int;

/// 内存写回调：SecureTransport 把封装好的记录写进本层缓冲。
auto memory_write(const void *connection, const void *data, size_t *processed) -> int;

/// 一条 TLS 会话的 SecureTransport 实现（线程约定见 `detail/tls_backend.h`：调用方串行化，本类不自持锁）。
class SecureTransportBackend final : public TlsBackend {
  public:
    [[nodiscard]] static auto create(const TlsContext &context, std::string_view server_name)
        -> Result<std::unique_ptr<SecureTransportBackend>>;

    /// @brief 归还本对象持有的 CF 引用（不调 `SSLClose`，理由见 `release`）。
    ~SecureTransportBackend() override { release(); }

    auto push_incoming(std::span<const char> bytes) -> void override {
        // 已喂给系统的部分就地丢掉：这是密文缓冲唯一的收缩点，否则一条长连接上的字节会单调增长。
        if (pending_taken_ >= pending_.size()) {
            pending_.clear();
            pending_taken_ = 0;
        }
        pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    }

    auto handshake(std::vector<char> &outgoing) -> Result<bool> override;

    auto next_records(std::vector<char> &plaintext, std::vector<char> &outgoing) -> Result<TlsDataState> override;

    auto protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> override;

    auto shutdown_records(std::vector<char> &outgoing) -> Result<void> override;

    auto peer_certificate() const -> Result<PeerCertificate> override {
        if (!cert_) {
            return tls_error(ErrorCode::NetSocketFailed, "no peer certificate is bound to this session");
        }
        return *cert_;
    }

    auto negotiated_alpn() const -> std::optional<std::string> override { return alpn_; }

    // 两个内存 I/O 回调是自由函数（`SSLSetIOFuncs` 要的是 C 函数指针，成员函数指针要 reinterpret 才是
    // 同一形态，那是不必要的未定义行为），因此以友元取得本类的缓冲所有权。
    friend auto memory_read(const void *connection, void *buffer, size_t *processed) -> int;
    friend auto memory_write(const void *connection, const void *data, size_t *processed) -> int;

  private:
    SecureTransportBackend() = default;

    /// 归还本对象持有的 CF 引用。
    ///
    /// 不调 `SSLClose`：文档没有承诺它的幂等性，而关闭通知的产出已由 `shutdown_records()` 承担；
    /// `SSLCreateContext` 的文档明写「用 `CFRelease` 释放本对象」，内存 I/O 之下没有 fd 需要额外收尾。
    void release() {
        if (certificate_refs_ != nullptr) {
            ::CFRelease(certificate_refs_);
            certificate_refs_ = nullptr;
        }
        if (anchors_ != nullptr) {
            ::CFRelease(anchors_);
            anchors_ = nullptr;
        }
        if (context_ != nullptr) {
            ::CFRelease(context_);
            context_ = nullptr;
        }
    }

    /// 把系统产出的记录交给调用方（追加语义），取完即清空。
    auto drain_produced(std::vector<char> &outgoing) -> void {
        if (produced_.empty()) {
            return;
        }
        const std::span<const char> taken{produced_};
        outgoing.insert(outgoing.end(), taken.begin(), taken.end());
        produced_.clear();
    }

    /// 本层缓冲里是否已无可喂的字节。
    [[nodiscard]] auto incoming_exhausted() const -> bool { return pending_taken_ >= pending_.size(); }

    /// 信任判定：链（含追加根）与主机名由同一次系统评估给出，随后固化对端证书摘要。
    ///
    /// 判定与取证的先后是有约束的：证书链只有在评估之后才保证可读（文件头引的文档原话），所以危险开关
    /// 也只可能跳结论、不可能跳动作。名字判定并进同一次评估（策略里带了 hostname），于是「链可信但身份
    /// 不符」由系统的 CFError 说清，本层不再自己排两次序。
    [[nodiscard]] auto verify_peer_trust() -> Result<void>;

    /// 从已评估的 trust 里读叶子证书，固化公共面摘要（含 SPKI 的 pin 形态）。
    [[nodiscard]] auto capture_summary(SecTrustRef trust) -> Result<void>;

    /// 握手完成时固化协商出的 ALPN（`SSLCopyALPNProtocols`：入参必须为 NULL，产出的数组由调用方释放）。
    auto capture_alpn() -> void;

    SSLContextRef context_ = nullptr;  ///< SecureTransport 会话上下文（CF 引用，本对象持有）
    std::string host_display_;  ///< 调用方给的原样名字（SNI 与诊断文本用它）
    bool insecure_ = false;  ///< 危险开关：置真即不采纳信任结论
    bool handshaked_ = false;  ///< 握手是否完成
    bool closing_ = false;  ///< 是否已发出关闭通知（此后不再复用上下文）
    bool peer_closed_ = false;  ///< 连接已终止：此后不再有明文字节
    std::vector<char> pending_;  ///< 对端字节（未喂给系统的部分自 `pending_taken_` 起）
    std::size_t pending_taken_ = 0;  ///< 已交给系统的字节数
    std::vector<char> produced_;  ///< 系统产出、尚未交付的记录
    CFArrayRef anchors_ = nullptr;  ///< 追加信任根（SecCertificateRef 数组，本对象持有）
    CFArrayRef certificate_refs_ = nullptr;  ///< mTLS 身份数组（文档要求保活到会话结束）
    std::optional<PeerCertificate> cert_;  ///< 对端证书摘要
    std::optional<std::string> alpn_;  ///< 协商出的 ALPN
};

auto SecureTransportBackend::create(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<SecureTransportBackend>> {
    std::unique_ptr<SecureTransportBackend> backend{new SecureTransportBackend()};
    backend->host_display_ = std::string{server_name};
    backend->insecure_ = context.allow_insecure_certificate;

    // 版本界的自洽性先于任何会话对象：上界低于下界时 `SSLSetProtocolVersionMin` 与 `Max` 会各自成功，
    // 然后系统按自己的次序裁决，结论文本里没有任何一处指向这条配置错误（口径同 OpenSSL 腿）。
    // `net/tls.cpp` 早在此之前就落 `net-tls-version-mismatch`，本层被直接调用时仍然要有同一结论。
    if (context.max_version && version_rank(*context.max_version) < version_rank(context.min_version)) {
        return tls_error(ErrorCode::GeneralInvalidArgument,
                         "the requested TLS version upper bound is below the lower bound");
    }

    // 客户端上下文 + 流类型：`kSSLStreamType` 才是本接缝的形态（记录是连续字节流，不是数据报）。
    // `SSLCreateContext` 直接**返回**自引用的上下文（分配失败回 nullptr）：旧 SDK 的 `SSLNewContext`
    // 出参形态已废除，新签名没有 OSStatus 出口，失败只有空指针一种观测。产出的上下文由 `release()`
    // 交回 CFRelease（文档：用 CFRelease 释放该对象）。
    backend->context_ = ::SSLCreateContext(kCFAllocatorDefault, kSSLClientSide, kSSLStreamType);
    if (backend->context_ == nullptr) {
        return tls_error(ErrorCode::NetTlsEngineUnavailable, "SecureTransport created no SSL context");
    }

    // 内存 I/O 必须在任何握手调用之前装好：`SSLSetIOFuncs` 的文档写明它先于 `SSLHandshake`。
    if (const auto status = ::SSLSetIOFuncs(backend->context_, &memory_read, &memory_write); status != noErr) {
        return status_error(status, "tls memory io");
    }
    // 连接上下文即本对象：它由会话持有、析构时上下文一并释放，因此回调的整个生命周期内指针始终有效。
    if (const auto status = ::SSLSetConnection(backend->context_, static_cast<void *>(backend.get()));
        status != noErr) {
        return status_error(status, "tls connection binding");
    }

    // 信任判定上收：拿不到这个选项就宁可不连——系统的自动通路不认追加根，静默继续会让「给了根却连不上」
    // 变成一条永远解释不清的失败。
    if (const auto status = ::SSLSetSessionOption(backend->context_, kSSLSessionOptionBreakOnServerAuth, true);
        status != noErr) {
        return status_error(status, "tls manual verification option");
    }

    // 版本界：系统拒绝某一档位是**配置与本腿能力不合**，不是握手失败，故与 OpenSSL 腿同码
    // （`general-invalid-argument`），现场指向调用方的口径而不是对端。
    if (const auto status = ::SSLSetProtocolVersionMin(backend->context_, protocol_value(context.min_version));
        status != noErr) {
        return tls_error(ErrorCode::GeneralInvalidArgument,
                         "this SecureTransport build does not support the requested minimum TLS version, status=" +
                             hex_status(status));
    }
    if (context.max_version) {
        if (const auto status = ::SSLSetProtocolVersionMax(backend->context_, protocol_value(*context.max_version));
            status != noErr) {
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "this SecureTransport build does not support the requested maximum TLS version, status=" +
                                 hex_status(status));
        }
    }

    // SNI：`SSLSetPeerDomainName` 只把名字送出去，不做任何校验（校验在 `verify_peer_trust` 的策略里）。
    if (!server_name.empty()) {
        if (const auto status = ::SSLSetPeerDomainName(backend->context_, server_name.data(), server_name.size());
            status != noErr) {
            return status_error(status, "tls server name indication");
        }
    }

    // 追加信任根：PEM → DER → SecCertificateRef 数组。`kCFTypeArrayCallBacks` 的语义是「加入即 retain」，
    // 故证书在追加后由各数组持有，本层不再单独释放。
    if (!context.trusted_certificates.empty()) {
        const CFMutableArrayRef anchors = ::CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        if (anchors == nullptr) {
            return tls_error(ErrorCode::NetTlsEngineUnavailable, "cannot allocate the anchor certificate array");
        }
        const CfReleaser guard{anchors};
        for (const auto &pem : context.trusted_certificates) {
            const auto blocks = pem_blocks(pem);
            if (blocks.empty()) {
                return tls_error(ErrorCode::GeneralInvalidArgument,
                                 "trusted certificate holds no PEM X.509 certificate block");
            }
            for (const auto &der : blocks) {
                const CFDataRef data =
                    ::CFDataCreate(kCFAllocatorDefault, der.data(), static_cast<CFIndex>(der.size()));
                if (data == nullptr) {
                    return tls_error(ErrorCode::GeneralInvalidArgument, "trusted certificate cannot be buffered");
                }
                const CfReleaser data_guard{data};
                // `SecCertificateCreateWithData` 对非 DER 输入返回 NULL（文档明写），这就是「根坏了」的唯一
                // 可观测形态——把它读成「没有根」会让校验默默按系统根走，现场指向完全错误的方向。
                const SecCertificateRef certificate = ::SecCertificateCreateWithData(kCFAllocatorDefault, data);
                if (certificate == nullptr) {
                    return tls_error(ErrorCode::GeneralInvalidArgument,
                                     "trusted certificate is not a decodable X.509 certificate");
                }
                ::CFArrayAppendValue(anchors, certificate);
                ::CFRelease(certificate);
            }
        }
        backend->anchors_ = static_cast<CFArrayRef>(::CFRetain(anchors));
    }

    // 客户端身份（mTLS）：PKCS#12 就地导入（不落钥匙串），取 item 0 的 `SecIdentityRef` 装进数组交给
    // `SSLSetCertificate`——文档要求数组第 0 位必须是身份，且必须在 `SSLHandshake` 之前设好。
    if (context.client_identity) {
        const CFDataRef archive =
            ::CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(context.client_identity->data()),
                           static_cast<CFIndex>(context.client_identity->size()));
        if (archive == nullptr) {
            return tls_error(ErrorCode::GeneralInvalidArgument, "client identity cannot be buffered");
        }
        const CfReleaser archive_guard{archive};
        const std::string password = context.client_identity_passphrase.value_or(std::string{});
        const CFStringRef passphrase = to_cfstring(password);
        if (passphrase == nullptr) {
            return tls_error(ErrorCode::GeneralInvalidArgument, "client identity passphrase is not representable");
        }
        const CfReleaser passphrase_guard{passphrase};
        const void *keys[] = {kSecImportExportPassphrase};
        const void *values[] = {passphrase};
        const CFDictionaryRef options = ::CFDictionaryCreate(
            kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (options == nullptr) {
            return tls_error(ErrorCode::GeneralInvalidArgument, "client identity options could not be built");
        }
        const CfReleaser options_guard{options};
        CFArrayRef items = nullptr;  // Copy 规则：产出的数组由调用方释放
        if (const auto status = ::SecPKCS12Import(archive, options, &items); status != noErr) {
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "client identity is not a readable PKCS#12 archive, status=" + hex_status(status));
        }
        const CfReleaser items_guard{items};
        const CFDictionaryRef item = (items != nullptr && ::CFArrayGetCount(items) > 0)
                                         ? static_cast<CFDictionaryRef>(::CFArrayGetValueAtIndex(items, 0))
                                         : nullptr;
        const void *raw_identity = item == nullptr ? nullptr : ::CFDictionaryGetValue(item, kSecImportItemIdentity);
        // 类型判据不能省：没有私钥的 PKCS#12 一样能解出证书，把证书当身份交给 SecureTransport 只会在
        // 需要客户端认证那一刻才失败，而那次失败看起来像「对端拒了我」，现场不留任何配置信息。
        if (raw_identity == nullptr || ::CFGetTypeID(raw_identity) != ::SecIdentityGetTypeID()) {
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "client identity carries no certificate with a usable private key");
        }
        const CFMutableArrayRef chain = ::CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        if (chain == nullptr) {
            return tls_error(ErrorCode::NetTlsEngineUnavailable, "cannot allocate the client certificate array");
        }
        const CfReleaser chain_guard{chain};
        ::CFArrayAppendValue(chain, raw_identity);
        if (const auto status = ::SSLSetCertificate(backend->context_, chain); status != noErr) {
            return status_error(status, "tls client identity");
        }
        // 文档明写「Secure Transport 假定这些引用在整个会话期内有效」，故本对象自己 retain 一份数组，
        // 而不是把只活到本函数结束的局部数组交出去。
        backend->certificate_refs_ = static_cast<CFArrayRef>(::CFRetain(chain));
    }

    // ALPN：宣告形态按文档给的是「ASCII 编码的协议标识」数组（RFC 7301），本腿装 CFString。元素类型文档
    // 没写死 CFString 还是 CFData（见 `capture_alpn` 的同一处说明），而 setter 只能选一型：CFString 是
    // 「ASCII 编码的字符串」最直接的读法，也是系统把协商结果交回来时的形态。这条是本腿**未实测**的假设之一。
    if (!context.alpn_protocols.empty()) {
        const CFMutableArrayRef protocols = ::CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        if (protocols == nullptr) {
            return tls_error(ErrorCode::NetTlsEngineUnavailable, "cannot allocate the ALPN protocol array");
        }
        const CfReleaser guard{protocols};
        for (const auto &protocol : context.alpn_protocols) {
            if (protocol.empty() || protocol.size() > kMaxAlpnProtocolIdBytes) {
                continue;  // 空标识不是合法的 RFC 7301 协议名：丢掉它，而不是宣告一个空协议。超长的同样丢掉
                           // ——协议名在 wire 上带 u8 长度前缀，本腿交出去只会让平台把整条连接判死，而 Schannel
                           // 腿对同一份配置是「跳过这一条、其余照宣」（见其 `build_alpn_advertisement`）。
            }
            const CFStringRef identifier = to_cfstring(protocol);
            if (identifier == nullptr) {
                return tls_error(ErrorCode::GeneralInvalidArgument,
                                 "ALPN protocol identifier is not representable: " + protocol);
            }
            ::CFArrayAppendValue(protocols, identifier);
            ::CFRelease(identifier);  // 数组已 retain（kCFTypeArrayCallBacks），本层的这份即可归还
        }
        if (::CFArrayGetCount(protocols) > 0) {
            // 宣告失败即整条连接失败：ALPN 决定调用方认为自己在说哪种协议，静默丢掉会让它按错误的协议
            // 解析对端字节。公共面对「没协商出来」另有 `net-option-conflict` 口径，那是另一件事。
            if (const auto status = ::SSLSetALPNProtocols(backend->context_, protocols); status != noErr) {
                return status_error(status, "tls ALPN advertisement");
            }
        }
    }

    return backend;
}

auto SecureTransportBackend::handshake(std::vector<char> &outgoing) -> Result<bool> {
    if (handshaked_) {
        return Result<bool>{true};
    }
    // 一次调用内允许若干轮：`errSSLServerAuthCompleted` 之后文档要求「再调 SSLHandshake 续跑」，而续跑的
    // 那一轮才产出 ClientKeyExchange/Finished。若在这里就返回 false，调用方会去看需要更多字节，而对端正在
    // 等我们——握手僵死。轮次上限是防自旋的兜底，正常握手用不到两轮以上。
    for (int round = 0; round < kMaxHandshakeRounds; ++round) {
        const auto status = ::SSLHandshake(context_);
        drain_produced(outgoing);
        if (status == errSSLServerAuthCompleted || status == errSSLPeerAuthCompleted) {
            // 前者是本层设定 `kSSLSessionOptionBreakOnServerAuth` 所要拿的那一次交权。后者按文档属于服务端
            // 侧（`kSSLSessionOptionBreakOnClientAuth`）的形态，客户端理论上不该遇到，但它同样是「认证阶段
            // 结束、可再调 SSLHandshake 续跑」的非致命结果：把它落进下面的 status_error 会把一次合法的续跑
            // 判成握手失败，而两条通路做的判定本来完全一致（服务端证书可信与否）。
            if (auto verdict = verify_peer_trust(); !verdict) {
                return verdict.error();
            }
            continue;
        }
        if (status == noErr) {
            capture_alpn();
            handshaked_ = true;
            return Result<bool>{true};
        }
        if (status == errSSLWouldBlock) {
            return Result<bool>{false};  // 需要更多对端字节（outgoing 里可能已带本轮产出）
        }
        return status_error(status, "tls handshake");
    }
    return tls_error(ErrorCode::NetTlsHandshakeFailed,
                     "tls handshake did not settle within " + std::to_string(kMaxHandshakeRounds) + " rounds");
}

auto SecureTransportBackend::verify_peer_trust() -> Result<void> {
    SecTrustRef trust = nullptr;  // Copy 规则：`SSLCopyPeerTrust` 的产出由调用方释放
    const auto copied = ::SSLCopyPeerTrust(context_, &trust);
    if (copied != noErr) {
        if (trust != nullptr) {
            ::CFRelease(trust);
        }
        return status_error(copied, "tls peer trust");
    }
    if (trust == nullptr) {
        return tls_error(ErrorCode::NetTlsCertRejected, "peer presents no certificate to verify");
    }
    const CfReleaser trust_guard{trust};

    if (!insecure_) {
        if (anchors_ != nullptr) {
            if (const auto status = ::SecTrustSetAnchorCertificates(trust, anchors_); status != noErr) {
                return tls_error(ErrorCode::NetTlsHandshakeFailed,
                                 "appended trust roots were not accepted, status=" + hex_status(status));
            }
            // false = 除给定根之外**也**认系统信任库：追加而非替换。做成替换会让所有对接公网的用例在给出
            // 自签根的那一刻突然不认合法证书，那是比拒绝更糟的故障。
            if (const auto status = ::SecTrustSetAnchorCertificatesOnly(trust, false); status != noErr) {
                return tls_error(ErrorCode::NetTlsHandshakeFailed,
                                 "trust root merging was not accepted, status=" + hex_status(status));
            }
        }
        // 名字核对交给平台：`SecPolicyCreateSSL` 的文档写明「指定 hostname 即要求它与叶子证书里的名字匹配」，
        // 这一条就是系统的 RFC 6125 通路，与 OpenSSL 腿把名字判定交给 `SSL_set1_host` 同一取舍。本层**不**
        // 自己再实现一份匹配：链构建与名字匹配正是跨腿最容易分叉的两处，自己写一份就多得一处「只有本腿这么
        // 判」的现场。与 Schannel 腿的已知差异（那腿的系统手工校验通路拿不到名字判定，只能自己写）记在文件头。
        const CFStringRef expected = to_cfstring(host_display_);
        if (expected == nullptr) {
            return tls_error(ErrorCode::NetTlsHandshakeFailed, "server name is not representable as a policy host");
        }
        const CfReleaser expected_guard{expected};
        const SecPolicyRef policy = ::SecPolicyCreateSSL(true, expected);
        if (policy == nullptr) {
            return tls_error(ErrorCode::NetTlsHandshakeFailed, "server authentication policy is unavailable");
        }
        const CfReleaser policy_guard{policy};
        if (const auto status = ::SecTrustSetPolicies(trust, policy); status != noErr) {
            return tls_error(ErrorCode::NetTlsHandshakeFailed,
                             "server authentication policy was not accepted, status=" + hex_status(status));
        }
    }

    CFErrorRef error = nullptr;
    const bool trusted = ::SecTrustEvaluateWithError(trust, &error);
    // 系统的失败描述只说「哪张证书出了什么问题」，不含我们本来要连的那个名字；而「连到了给别人签发的证书」
    // 是本层最常见的一类现场，名字必须在诊断里（公共面对 `net-tls-cert-rejected` 的期望就是可归因）。
    const std::string detail = describe_cferror(error);
    if (error != nullptr) {
        ::CFRelease(error);
    }
    if (!insecure_ && !trusted) {
        // 危险开关跳的是结论不是动作：取证书链的前提条件就是「先评估」（文件头引的文档原话），所以这里
        // 评估照做，只是不采纳它。
        return tls_error(ErrorCode::NetTlsCertRejected,
                         "certificate chain is not trusted for '" + host_display_ + "'" + detail);
    }
    // 吊销（OCSP/CRL）不在这里加做：本层不置任何吊销策略，判定按系统缺省给出（其文档同时写明评估过程可能
    // 联网取中间证书或吊销信息），与 Schannel/OpenSSL 两腿的「不因取不到吊销信息而拒连」同口径；把它做成
    // 强制在线检查是另一件事。

    // 名字与链的结论到此都由系统给出，剩下的只是把叶子证书摘成公共面摘要。
    return capture_summary(trust);
}

auto SecureTransportBackend::capture_summary(SecTrustRef trust) -> Result<void> {
    // 两个访问器都要求「先评估」，这正是本函数只在评估之后被调用的原因（文件头有文档原话）。
    if (::SecTrustGetCertificateCount(trust) <= 0) {
        return tls_error(ErrorCode::NetTlsCertRejected, "peer presents no certificate to verify");
    }
    const SecCertificateRef leaf = ::SecTrustGetCertificateAtIndex(trust, 0);  // Get 规则：不拥有该引用
    if (leaf == nullptr) {
        return tls_error(ErrorCode::NetTlsCertRejected, "peer certificate chain has no leaf certificate");
    }
    const CFDataRef der = ::SecCertificateCopyData(leaf);  // Copy 规则：本函数负责释放
    if (der == nullptr) {
        return tls_error(ErrorCode::NetTlsHandshakeFailed, "peer certificate could not be exported as DER");
    }
    const CfReleaser der_guard{der};
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t *>(::CFDataGetBytePtr(der)),
                                              static_cast<std::size_t>(::CFDataGetLength(der))};
    const auto parsed = parse_certificate(bytes);
    if (!parsed) {
        return tls_error(ErrorCode::NetTlsCertRejected, "peer certificate is not a decodable X.509 structure");
    }
    if (parsed->public_key_sha256.empty()) {
        // 摘要算不出即 pin 永不可能命中；把它当「一张正常证书」交给上层会让 pin 判定静默偏向放行。
        return tls_error(ErrorCode::NetTlsCertRejected, "peer certificate has no reproducible public key digest");
    }
    cert_ = std::move(*parsed);
    return Result<void>{};
}

auto SecureTransportBackend::capture_alpn() -> void {
    CFArrayRef protocols = nullptr;  // 文档：入参必须为 NULL，否则调用失败；产出的数组由调用方释放
    if (::SSLCopyALPNProtocols(context_, &protocols) != noErr || protocols == nullptr) {
        return;
    }
    const CfReleaser guard{protocols};
    if (::CFArrayGetCount(protocols) <= 0) {
        return;  // 对端未选择协议：公共面据「未置值」落 net-option-conflict，而不是空串
    }
    const void *raw = ::CFArrayGetValueAtIndex(protocols, 0);  // Get 规则：元素由数组持有
    if (raw == nullptr) {
        return;
    }
    // 文档对元素类型只写到「ASCII 编码的协议标识」，没写死 CFString 还是 CFData，而两枚 ALPN 接口都停在
    // macOS 10.15 的弃用线上、无从复核。于是两种形态都读：只认一种再把另一种读成未置值，会让真的协商结果
    // 在公共面表现成「对端没选协议」——而调用方对后者的处置是关掉连接。
    if (::CFGetTypeID(raw) == ::CFStringGetTypeID()) {
        if (auto negotiated = from_cfstring(static_cast<CFStringRef>(raw)); !negotiated.empty()) {
            alpn_ = std::move(negotiated);
        }
        return;
    }
    if (::CFGetTypeID(raw) == ::CFDataGetTypeID()) {
        const auto *data = static_cast<CFDataRef>(raw);
        const CFIndex length = ::CFDataGetLength(data);
        const std::span<const char> text{reinterpret_cast<const char *>(::CFDataGetBytePtr(data)),
                                         length > 0 ? static_cast<std::size_t>(length) : std::size_t{0}};
        if (!text.empty()) {
            alpn_ = std::string{text.data(), text.size()};
        }
    }
}

auto SecureTransportBackend::next_records(std::vector<char> &plaintext, std::vector<char> &outgoing)
    -> Result<TlsDataState> {
    if (peer_closed_) {
        return Result<TlsDataState>{TlsDataState::PeerClosed};
    }
    std::vector<char> scratch(kMaxPlainReadBytes, '\0');
    // `SSLGetBufferedReadSize` 报的是「本轮能保证取到的已解密字节数」，且文档明写它不阻塞、不触发任何底层
    // 读——有了它就不必靠反复空读试探缓冲，接缝里的 NeedMore 才有确定判据。
    std::size_t buffered = 0;
    if (::SSLGetBufferedReadSize(context_, &buffered) != noErr) {
        buffered = 0;
    }
    while (true) {
        drain_produced(outgoing);
        if (!outgoing.empty()) {
            // 数据阶段产出了握手字节（系统要求的重协商、TLS 1.3 的后握手消息）：交付权在调用方。
            return Result<TlsDataState>{plaintext.empty() ? TlsDataState::NeedMore : TlsDataState::Delivered};
        }
        if (buffered == 0U && incoming_exhausted()) {
            break;  // 既无已解密字节，也无待喂字节
        }
        const auto wanted = buffered != 0U ? std::min(buffered, kMaxPlainReadBytes) : kMaxPlainReadBytes;
        std::size_t processed = 0;
        const auto status = ::SSLRead(context_, scratch.data(), wanted, &processed);
        if (processed != 0U) {
            const std::span<const char> taken{scratch.data(), processed};
            plaintext.insert(plaintext.end(), taken.begin(), taken.end());
            buffered = processed >= buffered ? 0U : buffered - processed;
        }
        if (status == noErr || status == errSSLWouldBlock) {
            if (processed != 0U) {
                continue;  // 拿到字节就继续排空，直到缓冲空或凑不出完整记录
            }
            break;  // 半截记录：等更多字节（已喂出的部分由系统持有，下次从缺口续读）
        }
        if (status == errSSLClosedGraceful || status == errSSLClosedNoNotify || status == errSSLClosedAbort) {
            // 三种「连接已终止」的结果码都归 PeerClosed：接缝里能表达「此后不再有明文字节」的只有这一枚，
            // 而对端进程被强杀（RST 即 ClosedAbort）与未告而退（ClosedNoNotify）都不该被上层读成「我方
            // 解密失败」。挂起的读据 PeerClosed 落 0 字节结局，而不是永久悬挂。
            pending_.clear();
            pending_taken_ = 0;
            peer_closed_ = true;
            return Result<TlsDataState>{TlsDataState::PeerClosed};
        }
        if (status == errSSLPeerAuthCompleted) {
            // 重协商时服务端认证又来一次：先把结论做完，本轮已解出的明文照原样交付。
            if (auto verdict = verify_peer_trust(); !verdict) {
                return verdict.error();
            }
            continue;
        }
        return status_error(status, "tls record");
    }
    return Result<TlsDataState>{plaintext.empty() ? TlsDataState::NeedMore : TlsDataState::Delivered};
}

auto SecureTransportBackend::protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> {
    if (!handshaked_) {
        return tls_error(ErrorCode::NetTlsHandshakeFailed, "tls session is not established");
    }
    const auto produced_before = outgoing.size();
    std::size_t offset = 0;
    while (offset < plaintext.size()) {
        // 每次只喂一条记录的上限：`SSLWrite` 的文档对入参长度不作任何承诺，把分片行为留给系统决定就是赌
        // 运气；本层按 16 KiB 切开后，每次调用产出的记录数与内容都是确定的。
        const auto chunk = std::min(plaintext.size() - offset, kMaxRecordBodyBytes);
        const auto body = plaintext.subspan(offset, chunk);
        std::size_t processed = 0;
        const auto status = ::SSLWrite(context_, body.data(), static_cast<size_t>(chunk), &processed);
        drain_produced(outgoing);
        if (status == noErr) {
            offset += chunk;
            continue;
        }
        if (status == errSSLWouldBlock) {
            // 文档规定「少于请求的量已传输时重复调用直到出现其他结果」。本层的写回调永远全收，走到这里
            // 只能是系统内部没推进：0 字节的推进即卡死，不做无界重试。
            if (processed == 0U) {
                return tls_error(ErrorCode::NetTlsHandshakeFailed,
                                 "tls record wrap is blocked, status=" + hex_status(status));
            }
            offset += processed;
            continue;
        }
        return status_error(status, "tls record wrap");
    }
    if (!plaintext.empty() && outgoing.size() == produced_before) {
        // 系统声称写完却没产出任何记录：本层无法把它和「已交付底层流」区分开，只能显式失败——静默返回空
        // 产出的话，上层会把这报成「明文已发出」，那是数据丢失而不是错误。
        return tls_error(ErrorCode::NetTlsHandshakeFailed, "tls record wrap produced no wire bytes");
    }
    return Result<void>{};
}

auto SecureTransportBackend::shutdown_records(std::vector<char> &outgoing) -> Result<void> {
    if (!handshaked_ || closing_ || context_ == nullptr) {
        return Result<void>{};  // 没有会话状态可关：空产出即「没发」而不是「失败」
    }
    closing_ = true;
    // 接缝在这里要的是 close_notify，而 SecureTransport 并没有独立的「发送关闭通知」入口——
    // `SSLShutdown` 这个名字在本框架里不存在（与 OpenSSL 的 `SSL_shutdown` 串味），`SSLClose`
    // 终止会话即是本腿能拿到的最接近形态。产出照旧走写回调，因此取回字节即可交给调用方交付底层流。
    // 等对端回 close_notify 这一步在本层不可能有结论（`incoming_exhausted()` 时读回调一律回
    // `errSSLWouldBlock`），所以成败一律忽略、空产出照算成功。
    const auto status = ::SSLClose(context_);
    drain_produced(outgoing);
    (void)status;  // 关闭通知的成败不可能有结论（对端已不再读），空产出与已产出都算尽力而为
    return Result<void>{};
}

auto memory_read(const void *connection, void *buffer, size_t *processed) -> int {
    // 连接上下文是本类自己交给 `SSLSetConnection` 的指针，不存在真正的常量性；协议却把回调入参声明成
    // `const void *`，因此这里必须去 const。本文件只有这两处 const_cast。
    const auto *owner = static_cast<const SecureTransportBackend *>(connection);
    auto *self = const_cast<SecureTransportBackend *>(owner);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    if (self == nullptr || buffer == nullptr || processed == nullptr) {
        return errSSLClosedAbort;
    }
    const std::size_t requested = static_cast<std::size_t>(*processed);
    const std::size_t available = self->pending_.size() - self->pending_taken_;
    const std::size_t taken = std::min(available, requested);
    if (taken != 0U) {
        const std::span<const char> window{self->pending_.data() + self->pending_taken_, taken};
        std::memcpy(buffer, window.data(), taken);
        self->pending_taken_ += taken;
    }
    *processed = taken;
    // 短读必须回 errSSLWouldBlock（文档：表示「少于请求量已传输，只是 I/O 未完成」）；回成 noErr 会让
    // 系统把缺的字节当成流尾，把半截记录判成协议错误。
    // 两个值取自不同的匿名枚举（`noErr` 属于 OSStatus，`errSSLWouldBlock` 属于 SSLResult），因此不用三元
    // 表达式而是写成两条 return，避免枚举混用。
    if (taken == requested) {
        return noErr;
    }
    return errSSLWouldBlock;
}

auto memory_write(const void *connection, const void *data, size_t *processed) -> int {
    const auto *owner = static_cast<const SecureTransportBackend *>(connection);
    auto *self = const_cast<SecureTransportBackend *>(owner);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    if (self == nullptr || data == nullptr || processed == nullptr) {
        return errSSLClosedAbort;
    }
    const auto requested = static_cast<std::size_t>(*processed);
    const std::span<const char> record{static_cast<const char *>(data), requested};
    self->produced_.insert(self->produced_.end(), record.begin(), record.end());
    // 本层的产出缓冲无限增长，故永远全收：写回调若报短写，系统就会以 errSSLWouldBlock 中断本轮记录输出，
    // 把一条完整记录留成半截——那正是本接缝最不该出现的状态。
    *processed = requested;
    return noErr;
}

}  // namespace

auto make_tls_backend(const TlsContext &context, std::string_view server_name) -> Result<std::unique_ptr<TlsBackend>> {
    auto backend = SecureTransportBackend::create(context, server_name);
    if (!backend) {
        return backend.error();
    }
    // 派生 → 基类的所有权移交必须写明一次：两级隐式转换（unique_ptr 与 Result 各一级）不被语言允许。
    return Result<std::unique_ptr<TlsBackend>>{std::unique_ptr<TlsBackend>{std::move(backend.value())}};
}

auto tls_backend_available() -> bool { return true; }

}  // namespace aurora::net::detail

#pragma GCC diagnostic pop

#endif  // AURORA_NET_TLS_SECURE_TRANSPORT
