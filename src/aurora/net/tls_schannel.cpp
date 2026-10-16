// Aurora net — Schannel 系统栈后端。
//
// 选系统栈而不是自带 TLS 源码，理由有三条，都不可让：
//   - Windows 的系统栈零依赖、随 servicing 打补丁，本仓库「断网可构建」的不变量因此保持不变。
//   - 记录保护、密钥处理与版本降级策略由系统栈承担，这些恰是自研 TLS 最容易写错的部分。
//   - 信任判定由本层承担（`SCH_CRED_MANUAL_CRED_VALIDATION` + CryptoAPI 链引擎），理由见下方
//     `verify_chain`：追加信任根这件事在 Schannel 的自动校验通路里做不到。
//   - 追加根是**合并**而非替换系统根：给自定义根时把系统 ROOT 库与用例给的根一并装进内存存储，不给时
//     干脆走系统缺省链引擎——静默做成替换会让所有对接公网的用例突然不认合法证书。
//
// SNI 与版本界在凭据层表达（`AcquireCredentialsHandle` + `TLS_PARAMETERS`），不是逐连接补丁：Schannel
// 把这两项在凭据创建时固化，之后再改不会影响已建凭据。ALPN 则**不在**凭据层：`TLS_PARAMETERS` 的
// `rgstrAlpnIds` 按文档是给「该组参数所适用的 ALPN 标识」划定范围，实测（自签对端 + `openssl s_server -alpn`）
// 无论宽窄字符编码都协商不出任何结果（`SECPKG_ATTR_APPLICATION_PROTOCOL` 恒回 status=None）。客户端宣告
// ALPN 的唯一通路是首枚 `InitializeSecurityContext` 随附的 `SECBUFFER_APPLICATION_PROTOCOLS` 输入缓冲，
// 本层按该布局装载（见 `build_alpn_advertisement`）；`ISC_REQ_ALPN` 请求位实测非必要，故不带。
//
// 危险开关（`allow_insecure_certificate`）的落点是「本层跳过链与主机名判定」，并必留一条 warning 诊断
// （在 `net/tls.cpp` 发起处）；pin 判定不受它影响（同在 `net/tls.cpp` 里执行）。
//
// 本工具链的头文件缺少量已发布的 ABI 常量（协议位掩码、关闭通知载荷），在下方按官方值
// 就地补齐——补齐的是声明不是实现，值取自 Windows SDK 头，不是猜测。

#ifdef AURORA_NET_TLS_SCHANNEL

#include <array>
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

#include "aurora/core/platform.h"

// clang-format off
// SDK 可见性旋钮：MinGW 的 `sspi.h` 要求三选一地点明握手 API 的调用侧（本腿是用户态出站，故
// `SECURITY_WIN32`），而 `SCH_CREDENTIALS` / `TLS_PARAMETERS` 这一整套凭据形态在 `_WIN32_WINNT` 低于
// Win8 时根本不被声明。缺省值（0x0502）会让本 TU 编译不出可用形态，故在纳入任何平台头之前先钉住。
// 这些是 SDK 旋钮而非本库 feature 宏：只影响系统头声明了哪些东西，不改变本库任何语义。
// 名字与取值逐字取自 Windows SDK：宏由 SDK 头自身消费（改名即不再被任何头看见），而 `#ifndef` 守卫
// 让系统已给的定义优先——改成 constexpr 常量既守住不了「不覆盖上游定义」这条，也会让后续 SDK 头
// 重复定义。名单即删掉本区间后 clang-tidy 实测报出的检查集。
// NOLINTBEGIN(readability-identifier-naming, cppcoreguidelines-macro-usage, bugprone-reserved-identifier)
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif
// `SCH_CREDENTIALS` / `TLS_PARAMETERS`（版本界、ALPN、追加信任根、客户端证书的唯一载体）在 MinGW 与
// Windows SDK 里都包在 `SCHANNEL_USE_BLACKLISTS` 条件内：不定义它，头就把这套凭据形态藏起来。
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS
#endif
// NOLINTEND(readability-identifier-naming, cppcoreguidelines-macro-usage, bugprone-reserved-identifier)

// 顺序是硬约束：`schannel.h` / `security.h` / `bcrypt.h` 建立在 windows.h 的基础类型之上，头自身不做
// 回补；clang-format 的 SortIncludes 会把它们按字母序换到 windows.h 之前，故这一段关掉格式化。
#include <windows.h>
// `schannel.h` 里的 `TLS_PARAMETERS` 用 `UNICODE_STRING` 表达 ALPN，而 MinGW 把该类型的声明放在
// `ntdef.h`，`windows.h` 并不转含它（SDK 的转含链不同）——不先纳入就报「does not name a type」。
// ⚠ 两工具链拿它的入口不同：MinGW 在 `ntdef.h`；Windows SDK 的 `winnt.h` **已转含** `ntdef.h`
// （再直接包含会整组重定义 `_PROCESSOR_NUMBER` / `_GROUP_AFFINITY` / `_LIST_ENTRY` / …），
// SDK 侧的 `UNICODE_STRING` 由 `winternl.h` 提供。
#ifdef AURORA_COMPILER_MINGW
#include <ntdef.h>
#else
#include <winternl.h>
#endif
#include <schannel.h>
#include <security.h>
#include <wincrypt.h>
// clang-format on

#include "aurora/core/result.h"
#include "aurora/net/detail/tls_backend.h"
#include "aurora/net/tls.h"

namespace aurora::net::detail {
namespace {

// ---- 本工具链头文件缺失的已发布常量（值取自 Windows SDK 头）----
// 补齐的是**声明**而不是实现：这些宏名就是 SDK 头里的名字，本层按 `#ifndef` 让位给更新后的头，
// 因此既不能改名（改名即与上游对不上号），也不能按建议换成 constexpr 常量（换成后同名宏仍可能由
// 头文件给出，两份定义谁生效不可控）。名单即删掉本区间后实测报出的检查集。
// NOLINTBEGIN(readability-identifier-naming, cppcoreguidelines-macro-usage)
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0x00000000L
#endif
#ifndef SSL3_PROTOCOL
#define SSL3_PROTOCOL 0x0008U
#endif
#ifndef TLS1_0_PROTOCOL
#define TLS1_0_PROTOCOL 0x0080U
#endif
#ifndef TLS1_1_PROTOCOL
#define TLS1_1_PROTOCOL 0x0200U
#endif
#ifndef TLS1_2_PROTOCOL
#define TLS1_2_PROTOCOL 0x0800U
#endif
#ifndef TLS1_3_PROTOCOL
#define TLS1_3_PROTOCOL 0x8000U
#endif
// NOLINTEND(readability-identifier-naming, cppcoreguidelines-macro-usage)

/// 关闭通知的 `SECBUFFER_TOKEN` 载荷（`ApplyControlToken` 的入参形态）。
struct SchannelShutdownInfo {
    [[maybe_unused]] ULONG shutdown_type = 0;  ///< 0 = SchannelShutdownUsedSecurity：本会话已建立安全上下文
};

constexpr std::size_t AURORA_RECORD_HEADER_BYTES = 5;
constexpr std::size_t AURORA_MAX_RECORD_BODY_BYTES = 16384;
constexpr std::size_t AURORA_MAX_ALPN_PROTOCOL_ID_BYTES = 255;  ///< 单条标识的长度前缀是 u8（`MAX_PROTOCOL_ID_SIZE`）
constexpr std::size_t AURORA_MAX_ALPN_TABLE_BYTES = 65535;  ///< 整表字节数的长度前缀是 u16

auto tls_error(ErrorCode code, std::string text) -> Error {
    return make_error(code, ErrorParams{{"detail", std::move(text)}});
}

auto to_wide(std::string_view text) -> std::wstring {
    if (text.empty()) {
        return {};
    }
    const auto count = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

auto to_utf8(std::wstring_view text) -> std::string {
    if (text.empty()) {
        return {};
    }
    const auto count =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr,
                          nullptr);
    return result;
}

// FILETIME（UTC，100 ns 精度）→ ISO-8601 文本：证书有效期在公共面上是可比对的文本，不是平台结构。
auto to_iso8601(const FILETIME &time) -> std::string {
    SYSTEMTIME utc{};
    if (::FileTimeToSystemTime(&time, &utc) == FALSE) {
        return {};
    }
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02dT%02u:%02u:%02uZ", static_cast<unsigned>(utc.wYear),
                  static_cast<unsigned>(utc.wMonth), static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
                  static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond));
    return {buffer};
}

auto name_to_text(const CERT_NAME_BLOB &name) -> std::string {
    // CryptoAPI 的固定两步形态：先以空缓冲问长度（返回值含结尾空字符），再填入。
    auto blob = name;
    const auto count = ::CertNameToStrW(X509_ASN_ENCODING, &blob, CERT_X500_NAME_STR, nullptr, 0);
    if (count <= 1) {
        return {};
    }
    std::wstring text(count, L'\0');
    if (::CertNameToStrW(X509_ASN_ENCODING, &blob, CERT_X500_NAME_STR, text.data(), count) == 0) {
        return {};
    }
    return to_utf8(std::wstring_view{text.data(), count - 1});
}

// BCrypt 的长度出参在头里声明成按字节的 `PUCHAR`，只能把 `DWORD` 的地址原样递过去；摘要入参声明成
// 非 const 的 `PUCHAR`，而本层持有的是 `span<const uint8_t>`（系统不改写入参正文）。两处强转是 ABI
// 要求，收在这两枚小函数里，让紧邻式豁免各只生效一处而不散落在每个调用点。
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
auto bcrypt_out(DWORD &value) -> PUCHAR { return reinterpret_cast<PUCHAR>(&value); }

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
auto bcrypt_in(const std::uint8_t *bytes) -> PUCHAR { return const_cast<PUCHAR>(bytes); }

/// SPKI 摘要的哈希通路：BCrypt 的哈希对象缓冲由系统决定尺寸，写死一枚定长 scratch 会让
/// `BCryptCreateHash` 直接失败——而失败若被读成「返回一枚全零摘要」，pin 就会与任何一枚同样全零的
/// 配置值相等（本函数的第一版正是这样把「pin 不命中」吞掉的）。因此长度一律现问，失败一律回空。
auto sha256(std::span<const std::uint8_t> bytes) -> std::vector<std::uint8_t> {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != STATUS_SUCCESS) {
        return {};
    }
    auto queried = DWORD{0};
    auto object_length = DWORD{0};
    auto digest_length = DWORD{0};
    std::vector<std::uint8_t> digest;
    if (::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, bcrypt_out(object_length), sizeof(object_length), &queried,
                            0) == STATUS_SUCCESS &&
        ::BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, bcrypt_out(digest_length), sizeof(digest_length), &queried,
                            0) == STATUS_SUCCESS &&
        object_length != 0 && digest_length != 0) {
        auto scratch = std::vector<std::uint8_t>(object_length, std::uint8_t{0});
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (::BCryptCreateHash(algorithm, &hash, scratch.data(), object_length, nullptr, 0, 0) == STATUS_SUCCESS) {
            digest.assign(digest_length, std::uint8_t{0});
            if (::BCryptHashData(hash, bcrypt_in(bytes.data()), static_cast<ULONG>(bytes.size()), 0) !=
                    STATUS_SUCCESS ||
                ::BCryptFinishHash(hash, digest.data(), digest_length, 0) != STATUS_SUCCESS) {
                digest.clear();
            }
            ::BCryptDestroyHash(hash);
        }
    }
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

auto to_base64(std::span<const std::uint8_t> bytes) -> std::string {
    if (bytes.empty()) {
        return {};
    }
    constexpr auto flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD count = 0;
    if (::CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), flags, nullptr, &count) == FALSE) {
        return {};
    }
    std::string text(count, '\0');
    if (::CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), flags, text.data(), &count) == FALSE) {
        return {};
    }
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

auto to_lower_ascii(std::string_view text) -> std::string {
    std::string result(text);
    for (auto &character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

/// 一条证书名与目标主机的匹配（RFC 6125 的收紧形态）：通配符只允许作为最左标签整体出现，且不跨点。
///
/// 收紧的两处理由：`*` 在中间（`a*b.example.com`）按本函数一律不匹配——真实服务端不会签发这种名字，
/// 而把它当通配处理会让匹配规则变成不可判定的字符串游戏；`*.example.com` 不匹配 `a.b.example.com`——
/// 多级通配会让一张泛证书覆盖整个后缀树，那是发证方的权限而不是连接方的期待。
auto name_matches(std::string_view pattern, std::string_view host) -> bool {
    if (pattern.empty() || host.empty()) {
        return false;
    }
    if (pattern.front() != '*') {
        return pattern == host;
    }
    const auto suffix = pattern.substr(1);
    if (suffix.size() < 2 || suffix.front() != '.' || suffix.find('*') != std::string_view::npos) {
        return false;
    }
    const auto dot = host.find('.');
    if (dot == std::string_view::npos || dot == 0) {
        return false;  // 没有可替换的非空最左标签
    }
    return host.substr(dot) == suffix;
}

/// 证书里可核对的 dNSName 集合（SAN 形态）；SAN 缺失或解码失败即返回空集，由调用方回落到 CN。
auto certificate_dns_names(PCCERT_CONTEXT cert) -> std::vector<std::string> {
    std::vector<std::string> names;
    if (cert == nullptr || cert->pCertInfo == nullptr) {
        return names;
    }
    // 系统给出的两张数组都是「裸指针 + 计数」形态，收成 span 即不再做指针算术。
    const auto extensions = std::span<const CERT_EXTENSION>(cert->pCertInfo->rgExtension, cert->pCertInfo->cExtension);
    for (const auto &extension : extensions) {
        if (extension.pszObjId == nullptr || ::lstrcmpA(extension.pszObjId, szOID_SUBJECT_ALT_NAME2) != 0) {
            continue;
        }
        auto *decoded = static_cast<PCERT_ALT_NAME_INFO>(nullptr);
        DWORD size = 0;
        // 结构类型常量与扩展 OID 同串（SDK 里 `X509_ALT_NAME` 就是 `szOID_SUBJECT_ALT_NAME2`）。
        // `pvStructInfo` 在头里就声明成 `void *`（出参按地址交付），取址即二级指针 → void*，写成显式转换。
        if (::CryptDecodeObjectEx(cert->dwCertEncodingType, szOID_SUBJECT_ALT_NAME2, extension.Value.pbData,
                                  extension.Value.cbData, CRYPT_DECODE_ALLOC_FLAG, nullptr,
                                  static_cast<void *>(&decoded), &size) == FALSE ||
            decoded == nullptr) {
            continue;
        }
        const auto entries = std::span<const CERT_ALT_NAME_ENTRY>(decoded->rgAltEntry, decoded->cAltEntry);
        for (const auto &candidate : entries) {
            // `CERT_ALT_NAME_ENTRY` 的「选择子 + 匿名 union」是 CryptoAPI 的固定形态：按 dwAltNameChoice
            // 取对应成员是唯一读法，换不成 variant。
            // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
            if (candidate.dwAltNameChoice == CERT_ALT_NAME_DNS_NAME && candidate.pwszDNSName != nullptr) {
                names.push_back(to_lower_ascii(to_utf8(candidate.pwszDNSName)));
            }
            // NOLINTEND(cppcoreguidelines-pro-type-union-access)
        }
        ::LocalFree(decoded);
    }
    return names;
}

/// 证书主体 CN（缺 SAN 时的传统核对对象）；`CertGetNameStringW` 不给类型参数即取 CN。
auto certificate_common_name(PCCERT_CONTEXT cert) -> std::string {
    if (cert == nullptr) {
        return {};
    }
    std::wstring buffer(256, L'\0');
    const auto count =
        ::CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0, nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count <= 1) {
        return {};
    }
    return to_lower_ascii(to_utf8(std::wstring_view{buffer.data(), count - 1}));
}

/// 证书类系统状态码：命中即归 `net-tls-cert-rejected`，否则算握手层失败。
///
/// 为什么要自己列：Schannel 把校验错误散在 `SEC_E_*` 与 `CERT_E_*` 两个命名空间里返回，笼统归成
/// 「握手失败」会让「证书过期」与「对端根本不会说 TLS」变成同一条诊断——而这两者的处置完全相反。
auto is_certificate_status(LONG status) -> bool {
    switch (status) {
        case SEC_E_UNTRUSTED_ROOT:
        case SEC_E_CERT_EXPIRED:
        case SEC_E_WRONG_PRINCIPAL:
        case SEC_E_CERT_UNKNOWN:
        case CERT_E_EXPIRED:
        case CERT_E_UNTRUSTEDROOT:
        case CERT_E_CN_NO_MATCH:
        case CERT_E_INVALID_NAME:
        case CERT_E_ROLE:
        case CRYPT_E_REVOKED:
            return true;
        default:
            return false;
    }
}

auto hex_status(LONG status) -> std::string {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(status));
    return {buffer};
}

auto status_error(LONG status, std::string_view phase) -> Error {
    const auto text = std::string{phase} + ", status=" + hex_status(status);
    if (is_certificate_status(status)) {
        return tls_error(ErrorCode::NetTlsCertRejected, text);
    }
    return tls_error(ErrorCode::NetTlsHandshakeFailed, text);
}

auto version_bits(TlsVersion version) -> DWORD {
    switch (version) {
        case TlsVersion::Tls10:
            return TLS1_0_PROTOCOL;
        case TlsVersion::Tls11:
            return TLS1_1_PROTOCOL;
        case TlsVersion::Tls12:
            return TLS1_2_PROTOCOL;
        case TlsVersion::Tls13:
            return TLS1_3_PROTOCOL;
    }
    return TLS1_2_PROTOCOL;
}

/// 版本界 → 禁用位掩码：低于下界与高于上界的一律关掉（上界未置值时不禁高版本）。
///
/// SSL 2/3 恒禁：它们不是「旧一点」而是已判死，留一个能把下界调到 SSL3 的口子等于没有下界。
auto disabled_protocol_bits(const TlsContext &context) -> DWORD {
    const auto rank = [](TlsVersion version) -> int {
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
    };
    const auto floor = rank(context.min_version);
    const auto ceiling = context.max_version ? rank(*context.max_version) : 3;
    DWORD disabled = SSL3_PROTOCOL;
    for (auto index = 0; index < 4; ++index) {
        if (index < floor || index > ceiling) {
            disabled |= version_bits(static_cast<TlsVersion>(index));
        }
    }
    return disabled;
}

/// PEM → 每张证书的 DER：一张字符串里串多个 `-----BEGIN CERTIFICATE-----` 块是合法输入。
auto pem_blocks(std::string_view pem) -> std::vector<std::vector<std::uint8_t>> {
    constexpr std::string_view header = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view footer = "-----END CERTIFICATE-----";
    std::vector<std::vector<std::uint8_t>> blocks;
    // 以视图窗口前移代替游标指针算术：substr 本身就表达「跳过已消费的前缀」，无需豁免指针推进。
    auto rest = pem;
    while (!rest.empty()) {
        const auto start = rest.find(header);
        if (start == std::string_view::npos) {
            break;
        }
        rest = rest.substr(start + header.size());
        const auto body = rest;
        const auto stop = body.find(footer);
        if (stop == std::string_view::npos) {
            break;
        }
        std::vector<std::uint8_t> der(body.size(), std::uint8_t{0});
        auto count = static_cast<DWORD>(der.size());
        // 长度已作为第二个实参显式交给系统：`CryptStringToBinaryA` 按 `cbData` 取数据、不依赖结尾符，
        // 故此处 `body.data()` 未以 NUL 收尾不构成越界风险（本检查认不出显式长度实参）。
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
        if (::CryptStringToBinaryA(body.data(), static_cast<DWORD>(body.size()), CRYPT_STRING_BASE64, der.data(),
                                   &count, nullptr, nullptr) != FALSE &&
            count > 0) {
            der.resize(count);
            blocks.push_back(std::move(der));
        }
        rest = rest.substr(stop);
    }
    return blocks;
}

/// ALPN 宣告缓冲（`SECBUFFER_APPLICATION_PROTOCOLS` 的载荷形态）。
///
/// 布局（自 `SecPkgContext_ApplicationProtocol` 的对侧读取实测确认，见文件头注释）：
///   `u32 后续字节数 | u32 扩展标识(=ALPN) | u16 协议表字节数 | 逐条 u8 长度 + 标识字节`。
/// 系统只登记指针不复制正文，故产物必须由会话对象持有到首枚握手调用返回。
/// @param protocols 按偏好排序的协议标识；超出单条 255 字节或整表 65535 字节者被丢弃。
/// @return 载荷字节；未宣告（空输入）时为空串。
auto build_alpn_advertisement(const std::vector<std::string> &protocols) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> table;
    for (const auto &protocol : protocols) {
        if (protocol.empty() || protocol.size() > AURORA_MAX_ALPN_PROTOCOL_ID_BYTES) {
            continue;
        }
        table.push_back(static_cast<std::uint8_t>(protocol.size()));
        table.insert(table.end(), protocol.begin(), protocol.end());
    }
    if (table.empty() || table.size() > AURORA_MAX_ALPN_TABLE_BYTES) {
        return {};
    }
    constexpr auto extension = static_cast<std::uint32_t>(SecApplicationProtocolNegotiationExt_ALPN);
    const auto table_bytes = static_cast<std::uint16_t>(table.size());
    const auto payload_bytes = static_cast<std::uint32_t>(sizeof(extension) + sizeof(table_bytes) + table.size());
    auto bytes = std::vector<std::uint8_t>(
        sizeof(payload_bytes) + sizeof(extension) + sizeof(table_bytes) + table.size(), std::uint8_t{0});
    // 逐字段写入紧凑布局：用 span 的 subspan 前移窗口，而不是对裸指针做算术。
    auto tail = std::span<std::uint8_t>{bytes};
    std::memcpy(tail.data(), &payload_bytes, sizeof(payload_bytes));
    tail = tail.subspan(sizeof(payload_bytes));
    std::memcpy(tail.data(), &extension, sizeof(extension));
    tail = tail.subspan(sizeof(extension));
    std::memcpy(tail.data(), &table_bytes, sizeof(table_bytes));
    tail = tail.subspan(sizeof(table_bytes));
    std::memcpy(tail.data(), table.data(), table.size());
    return bytes;
}

/// 一条 TLS 会话的 Schannel 实现（线程约定见 `detail/tls_backend.h`：调用方串行化，本类不自持锁）。
class SchannelBackend final : public TlsBackend {
  public:
    [[nodiscard]] static auto create(const TlsContext &context, std::string_view server_name)
        -> Result<std::unique_ptr<SchannelBackend>>;

    SchannelBackend(const SchannelBackend &) = delete;
    auto operator=(const SchannelBackend &) -> SchannelBackend & = delete;
    SchannelBackend(SchannelBackend &&) = delete;
    auto operator=(SchannelBackend &&) -> SchannelBackend & = delete;
    ~SchannelBackend() override { release(); }

    auto push_incoming(std::span<const char> bytes) -> void override {
        pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    }

    auto handshake(std::vector<char> &outgoing) -> Result<bool> override;

    auto next_records(std::vector<char> &plaintext, std::vector<char> &outgoing) -> Result<TlsDataState> override;

    auto protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> override;

    auto shutdown_records(std::vector<char> &outgoing) -> Result<void> override;

    [[nodiscard]] auto peer_certificate() const -> Result<PeerCertificate> override {
        if (!cert_) {
            return tls_error(ErrorCode::NetSocketFailed, "no peer certificate is bound to this session");
        }
        return *cert_;
    }

    [[nodiscard]] auto negotiated_alpn() const -> std::optional<std::string> override { return alpn_; }

  private:
    SchannelBackend() = default;

    void release() {
        if (peer_cert_ != nullptr) {
            ::CertFreeCertificateContext(peer_cert_);
            peer_cert_ = nullptr;
        }
        if (have_context_) {
            ::DeleteSecurityContext(&context_);
            have_context_ = false;
        }
        if (have_credential_) {
            ::FreeCredentialsHandle(&credential_);
            have_credential_ = false;
        }
        for (const auto *certificate : client_certificates_) {
            ::CertFreeCertificateContext(certificate);
        }
        client_certificates_.clear();
        // PKCS#12 存储必须晚于其证书释放：私钥句柄由该存储持有，先关存储会让客户端认证静默失效。
        if (identity_store_ != nullptr) {
            ::CertCloseStore(identity_store_, 0);
            identity_store_ = nullptr;
        }
        if (root_store_ != nullptr) {
            ::CertCloseStore(root_store_, 0);
            root_store_ = nullptr;
        }
    }

    /// 缓冲里「连续完整记录」的总字节数（记录头 5 字节：类型 1 + 版本 2 + 长度 2，长度不含头）。
    ///
    /// 只交完整记录是给 Schannel 的确定性输入：`SECBUFFER_TOKEN` 在头部块不完整时的返回形态因系统版本
    /// 而异，而自己按记录边界切分后，本层永远知道还剩多少字节。
    [[nodiscard]] auto complete_records_prefix() const -> std::size_t {
        std::size_t offset = 0;
        while (offset + AURORA_RECORD_HEADER_BYTES <= pending_.size()) {
            const auto length = (static_cast<std::size_t>(static_cast<std::uint8_t>(pending_[offset + 3])) << 8U) |
                                static_cast<std::size_t>(static_cast<std::uint8_t>(pending_[offset + 4]));
            const auto total = length + AURORA_RECORD_HEADER_BYTES;
            if (total > pending_.size() - offset) {
                break;
            }
            offset += total;
        }
        return offset;
    }

    /// 缓冲里第一条完整记录的字节数；凑不出即 0。
    [[nodiscard]] auto first_record_bytes() const -> std::size_t {
        if (pending_.size() < AURORA_RECORD_HEADER_BYTES) {
            return 0;
        }
        const auto length = (static_cast<std::size_t>(static_cast<std::uint8_t>(pending_[3])) << 8U) |
                            static_cast<std::size_t>(static_cast<std::uint8_t>(pending_[4]));
        const auto total = length + AURORA_RECORD_HEADER_BYTES;
        return total <= pending_.size() ? total : 0;
    }

    auto capture_negotiated() -> void;

    /// 链可信判定：由本层经 CryptoAPI 链引擎做，而不是交给 Schannel 的自动校验。
    ///
    /// 为什么不能交系统（本机实测口径，非推断）：`SCH_CREDENTIALS.hRootStore` 装进「系统 ROOT ∪ 追加根」
    /// 的内存存储后，自动校验照旧回 `SEC_E_UNTRUSTED_ROOT`；同一条凭据换成 `SCH_CREDENTIALS_VERSION` +
    /// 手工校验则握手直接通过。也就是说追加信任根在 Schannel 的校验通路里做不到，而它正是本切片的验收
    /// 判据之一（自签对端默认证住、给了根即可连）。判定因此上收到本层，与 OpenSSL / mbedTLS 适配器同一
    /// 口径：链可建 + 无信任错误 + 服务端身份 EKU，主机名再单独核对。
    ///
    /// 吊销（CRL/OCSP）不在这里做：不给 `CERT_CHAIN_REVOCATION_CHECK_*` 即沿用系统缺省的「离线不判吊销」，
    /// 与两平台适配器一致；把它做成在线检查需要 AIA 网络取回，属另一件事。
    [[nodiscard]] auto verify_chain() const -> Result<void>;

    /// 主机名核对：SAN 的 dNSName 优先，无 SAN 才看 CN。
    ///
    /// 为什么系统栈的校验不够：Schannel 的自动校验只管「链可信 + 未过期 + 未被吊销」，它并不知道本连接
    /// 要去的那个名字（`InitializeSecurityContext` 的目标名只用于 SNI 与客户端认证票据）。缺了这一步，
    /// 一张合法签给别人的证书就能通过握手——而「校验过了」的全部意义是「连到了我信任的那台」。
    [[nodiscard]] auto verify_hostname() const -> Result<void>;

    /// 数据阶段的重协商轮次：以「无输入」的一次握手调用把系统内部的协商状态推进，产出需外发的字节。
    ///
    /// 为什么不能直接忽略 `SEC_I_RENEGOTIATE`：实测跳过这一轮次之后，同一上下文里的下一条记录一律以
    /// `SEC_E_DECODE_BITS` 失败——系统把那条记录当作协商材料消费了，只有本轮次能对账。
    /// @param outgoing 需交付底层流的握手字节（追加语义；TLS 1.3 的票据分支下通常为空）。
    /// @return 成功携「本轮无需外发或外发内容已备」；失败携原因（上下文已失效）。
    [[nodiscard]] auto renegotiate(std::vector<char> &outgoing) -> Result<void>;

    /// 每次握手调用复用的请求位（分配内存由系统负责，本层不碰 `SecBuffer` 的释放协议）。
    [[nodiscard]] static auto context_request() -> ULONG {
        return ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
               ISC_REQ_EXTENDED_ERROR | ISC_REQ_STREAM;
    }

    CredHandle credential_{};  ///< 系统栈凭据
    CtxtHandle context_{};  ///< 安全上下文
    bool have_credential_ = false;  ///< 凭据是否已登记
    bool have_context_ = false;  ///< 上下文是否已建立
    bool handshaked_ = false;  ///< 握手是否完成
    std::vector<std::uint8_t> alpn_advertisement_;  ///< ALPN 宣告载荷（随首枚握手调用交付；空即未宣告）
    DWORD attrs_ = 0;  ///< 最近一次握手返回的属性位（未消费输入长度由本层自己按记录边界算，故仅作记录）
    SecPkgContext_StreamSizes sizes_{};  ///< 记录级头/尾/上限
    std::vector<char> pending_;  ///< 尚未交给系统的对端字节
    std::optional<PeerCertificate> cert_;  ///< 对端证书摘要
    std::optional<std::string> alpn_;  ///< 协商出的 ALPN
    std::wstring target_;  ///< SNI 与主机名校验目标
    std::string host_;  ///< 同一目标的主机名文本（小写，主机名核对的比较侧）
    bool insecure_ = false;  ///< 危险开关：置真即跳过主机名核对（链校验由系统在同一开关下跳过）
    PCCERT_CONTEXT peer_cert_ = nullptr;  ///< 对端证书上下文（持有到会话结束：摘要与名字核对都读它）
    std::vector<PCCERT_CONTEXT> client_certificates_;  ///< mTLS 客户端证书（本对象持有）
    HCERTSTORE identity_store_ = nullptr;  ///< PKCS#12 导入出的存储（持有私钥句柄）
    HCERTSTORE root_store_ = nullptr;  ///< 排他根库（系统 ROOT ∪ 追加根）；未追加即空，链引擎据此二选一
    bool peer_closed_ = false;  ///< 已收到 close_notify
};

auto SchannelBackend::create(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<SchannelBackend>> {
    std::unique_ptr<SchannelBackend> backend{new SchannelBackend()};
    backend->target_ = to_wide(server_name);
    backend->host_ = to_lower_ascii(server_name);
    backend->insecure_ = context.allow_insecure_certificate;

    // 追加信任根：系统 ROOT 库 + 给的根，合并进一枚内存存储，交给链引擎作**排他**根库（`hExclusiveRoot`）。
    if (!context.trusted_certificates.empty()) {
        auto *store = ::CertOpenStore(CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, 0, 0, nullptr);
        if (store == nullptr) {
            return tls_error(ErrorCode::NetTlsEngineUnavailable, "cannot open an in-memory certificate store");
        }
        WCHAR root_name[] = L"ROOT";
        if (auto *system = ::CertOpenSystemStoreW(0, root_name); system != nullptr) {
            for (const auto *item = ::CertEnumCertificatesInStore(system, nullptr); item != nullptr;
                 item = ::CertEnumCertificatesInStore(system, item)) {
                ::CertAddCertificateContextToStore(store, item, CERT_STORE_ADD_ALWAYS, nullptr);
            }
            ::CertCloseStore(system, 0);
        }
        for (const auto &pem : context.trusted_certificates) {
            const auto blocks = pem_blocks(pem);
            if (blocks.empty()) {
                ::CertCloseStore(store, 0);
                return tls_error(ErrorCode::GeneralInvalidArgument,
                                 "trusted certificate holds no PEM X.509 certificate block");
            }
            for (const auto &der : blocks) {
                if (::CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, der.data(),
                                                       static_cast<DWORD>(der.size()), CERT_STORE_ADD_NEW,
                                                       nullptr) == FALSE) {
                    ::CertCloseStore(store, 0);
                    return tls_error(ErrorCode::GeneralInvalidArgument,
                                     "trusted certificate is not a decodable X.509 certificate");
                }
            }
        }
        backend->root_store_ = store;
    }

    // 客户端身份（mTLS）：PKCS#12 导入出的存储保持打开，私钥句柄随它存活。
    if (context.client_identity) {
        auto blob = CRYPT_DATA_BLOB{};
        blob.cbData = static_cast<DWORD>(context.client_identity->size());
        // `CRYPT_DATA_BLOB::pbData` 是 `PUCHAR`（系统不改写入参正文），而公共面把档案字节交给我们的是
        // `std::optional<std::string>` 的 const 视图：按 ABI 只能先去 const 再把地址按字节指针登记。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-type-const-cast)
        blob.pbData = reinterpret_cast<PUCHAR>(const_cast<char *>(context.client_identity->data()));
        const auto password = to_wide(context.client_identity_passphrase.value_or(""));
        auto *store =
            ::PFXImportCertStore(&blob, password.empty() ? nullptr : password.c_str(), CERT_SYSTEM_STORE_CURRENT_USER);
        if (store == nullptr) {
            return tls_error(ErrorCode::GeneralInvalidArgument, "client identity is not a readable PKCS#12 archive: " +
                                                                    hex_status(static_cast<LONG>(::GetLastError())));
        }
        backend->identity_store_ = store;
        for (const auto *item =
                 ::CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_ANY, nullptr, nullptr);
             item != nullptr;
             item = ::CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_ANY, nullptr, item)) {
            DWORD size = 0;
            // 只认带私钥登记的那一张：没有私钥的证书交给 Schannel 会在需要客户端认证时才失败，
            // 而那次失败看起来像「对端拒了我」，现场没有留下任何配置信息。
            if (::CertGetCertificateContextProperty(item, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) != FALSE &&
                size > 0) {
                backend->client_certificates_.push_back(::CertDuplicateCertificateContext(item));
                break;
            }
        }
        if (backend->client_certificates_.empty()) {
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "client identity carries no certificate with a usable private key");
        }
    }

    auto parameters = TLS_PARAMETERS{};
    parameters.grbitDisabledProtocols = disabled_protocol_bits(context);
    backend->alpn_advertisement_ = build_alpn_advertisement(context.alpn_protocols);

    auto credentials = SCH_CREDENTIALS{};
    // `dwVersion` 必须是 `SCH_CREDENTIALS_VERSION`：填 `SCH_CRED_V3` 时系统按旧版 `SCHANNEL_CRED` 的字段
    // 布局解析本结构，结果是 `dwFlags` 与 `pTlsParameters` 被静默丢弃（实测：同一份凭据下手工校验不生效，
    // 未受信根照旧回 `SEC_E_UNTRUSTED_ROOT`，版本界也不再受控）。这一枚是形态旋钮，不是可选项。
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.cTlsParameters = 1;
    credentials.pTlsParameters = &parameters;
    // 手工校验恒定开启：信任判定由本层 `verify_chain` 产出，系统不再给出「半个结论」。
    credentials.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION;
    if (!backend->client_certificates_.empty()) {
        credentials.cCreds = static_cast<DWORD>(backend->client_certificates_.size());
        credentials.paCred = backend->client_certificates_.data();
    }

    TimeStamp expiry{};
    // `AcquireCredentialsHandleW` 的 pszPrincipal 形参历史上签成 LPWSTR（非常量），而 UNISP_NAME_W
    // 是字面量：该形参只读（凭据包名，系统不经它回写），复制一份可写缓冲喂给 API——MSVC 的
    // C2664 正是卡在字面量到非常量指针的转换上，去常量则把「只读」这一点藏起来。
    wchar_t principal[] = UNISP_NAME_W;
    const auto status = ::AcquireCredentialsHandleW(nullptr, principal, SECPKG_CRED_OUTBOUND, nullptr, &credentials,
                                                    nullptr, nullptr, &backend->credential_, &expiry);
    if (status != SEC_E_OK) {
        return status_error(static_cast<LONG>(status), "tls credential");
    }
    backend->have_credential_ = true;
    return backend;
}

auto SchannelBackend::handshake(std::vector<char> &outgoing) -> Result<bool> {
    if (handshaked_) {
        return Result<bool>{true};
    }
    const auto usable = complete_records_prefix();
    if (have_context_ && usable == 0) {
        return Result<bool>{false};  // 还没有一条完整记录可交：等更多字节
    }

    auto input = SecBuffer{.cbBuffer = static_cast<ULONG>(usable),
                           .BufferType = SECBUFFER_TOKEN,
                           .pvBuffer = usable != 0 ? pending_.data() : nullptr};
    auto input_desc = SecBufferDesc{.ulVersion = SECBUFFER_VERSION, .cBuffers = 1, .pBuffers = &input};
    auto output = SecBuffer{.cbBuffer = 0, .BufferType = SECBUFFER_TOKEN, .pvBuffer = nullptr};
    auto output_desc = SecBufferDesc{.ulVersion = SECBUFFER_VERSION, .cBuffers = 1, .pBuffers = &output};

    const auto request = context_request();
    // 首枚调用带 ALPN 宣告（若有）：系统只在 ClientHello 生成那一次读它，之后的调用给了也不生效。
    auto advertise = SecBuffer{.cbBuffer = static_cast<ULONG>(alpn_advertisement_.size()),
                               .BufferType = SECBUFFER_APPLICATION_PROTOCOLS,
                               .pvBuffer = alpn_advertisement_.data()};
    auto advertise_desc = SecBufferDesc{.ulVersion = SECBUFFER_VERSION, .cBuffers = 1, .pBuffers = &advertise};
    auto status = have_context_
                      ? ::InitializeSecurityContextW(&credential_, &context_, nullptr, request, 0, 0, &input_desc, 0,
                                                     &context_, &output_desc, &attrs_, nullptr)
                      : ::InitializeSecurityContextW(&credential_, nullptr, target_.data(), request, 0, 0,
                                                     alpn_advertisement_.empty() ? nullptr : &advertise_desc, 0,
                                                     &context_, &output_desc, &attrs_, nullptr);
    if (output.cbBuffer != 0 && output.pvBuffer != nullptr) {
        // 系统的握手产物按「基址 + 字节数」交付（内存由 `ISC_REQ_ALLOCATE_MEMORY` 分配），搬走它只能按
        // 字节推进指针——这是 Schannel 的 ABI 边界，没有可改的容器访问形态。
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto *first = static_cast<const char *>(output.pvBuffer);
        outgoing.insert(outgoing.end(), first, first + output.cbBuffer);
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ::FreeContextBuffer(output.pvBuffer);
    }
    if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_COMPLETE_NEEDED ||
        status == SEC_I_COMPLETE_AND_CONTINUE) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(usable));
        have_context_ = true;
        if (status == SEC_E_OK) {
            capture_negotiated();
            // 结论的先后顺序是有意的：先看链可不可信，再看它是不是发给这个名字的。反过来会让「连到了
            // 一台证书可信但身份不符的机器」的现场写成身份问题，而它其实是信任问题。
            const auto chained = verify_chain();
            if (!chained) {
                return chained.error();
            }
            const auto verified = verify_hostname();
            if (!verified) {
                return verified.error();
            }
            handshaked_ = true;
        }
        return Result<bool>{handshaked_};
    }
    return status_error(static_cast<LONG>(status), "tls handshake");
}

auto SchannelBackend::capture_negotiated() -> void {
    ::QueryContextAttributes(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_);

    PCCERT_CONTEXT peer = nullptr;
    // `pvBuffer` 在头里声明成 `void *`（出参按地址交付），取址即二级指针 → void*，写成显式转换。
    if (::QueryContextAttributes(&context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, static_cast<void *>(&peer)) == SEC_E_OK &&
        peer != nullptr) {
        // 上下文持有到会话结束：证书摘要在握手那一刻固化，而主机名核对读的是同一份证书，
        // 两头都指向它才不会「摘要说没问题、核对用的是另一张」。
        peer_cert_ = peer;
        PeerCertificate info;
        info.subject = name_to_text(peer->pCertInfo->Subject);
        info.issuer = name_to_text(peer->pCertInfo->Issuer);
        info.not_before = to_iso8601(peer->pCertInfo->NotBefore);
        info.not_after = to_iso8601(peer->pCertInfo->NotAfter);
        // SPKI pin 的口径：对 SubjectPublicKeyInfo 的 DER 取 SHA-256 再 base64（不是整证摘要，也不是
        // 十六进制指纹），这样证书续签不换钥匙时 pin 不失配。
        auto spki = DWORD{0};
        if (::CryptEncodeObject(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &peer->pCertInfo->SubjectPublicKeyInfo,
                                nullptr, &spki) != FALSE &&
            spki > 0) {
            std::vector<std::uint8_t> der(spki, std::uint8_t{0});
            if (::CryptEncodeObject(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &peer->pCertInfo->SubjectPublicKeyInfo,
                                    der.data(), &spki) != FALSE) {
                der.resize(spki);
                info.public_key_sha256 = to_base64(sha256(std::span<const std::uint8_t>{der}));
            }
        }
        cert_ = std::move(info);
    }

    auto negotiated = SecPkgContext_ApplicationProtocol{};
    if (::QueryContextAttributes(&context_, SECPKG_ATTR_APPLICATION_PROTOCOL, &negotiated) == SEC_E_OK &&
        negotiated.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success &&
        negotiated.ProtocolIdSize > 0) {
        // `ProtocolId` 在系统结构里是 `BYTE[255]`（协议标识按字节给出，没有 char 形态），把它读成字符串
        // 必须按字节 reinterpret；长度已显式取 `ProtocolIdSize`，不依赖任何终止符。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        alpn_ = std::string{reinterpret_cast<const char *>(negotiated.ProtocolId),
                            static_cast<std::size_t>(negotiated.ProtocolIdSize)};
    }
}

auto SchannelBackend::verify_chain() const -> Result<void> {
    if (insecure_) {
        return Result<void>{};  // 危险开关：链判定与主机名核对一并跳过，发起处已留 warning 诊断
    }
    if (peer_cert_ == nullptr) {
        return tls_error(ErrorCode::NetTlsCertRejected, "peer presents no certificate to verify");
    }

    // 对端送来的中间证书不必自己搬运：Schannel 把整条链装进叶子证书所属的内存存储（实测 `entries` 随
    // 对端发送的张数增长），把它作为附加存储交给链引擎即可。
    //
    // 为什么不用 `SECPKG_ATTR_REMOTE_CERT_CHAIN`：本机实测它返回的结构 `cbSize=1`（不等于
    // `sizeof(CERT_CHAIN_CONTEXT)`），按链上下文解引用即访问违例——那份内存的真实形态无人担保。
    // 只认叶子会让「叶子 + 中间 + 根」的公网形态建成部分链（`CERT_TRUST_IS_PARTIAL_CHAIN`），
    // 而那会被读成「证书不可信」，现场指向完全错误的方向。
    HCERTSTORE supplied = peer_cert_->hCertStore;

    // 链引擎：给了追加根就用「系统 ROOT ∪ 追加根」作**排他**根库（`hExclusiveRoot`，Win7 起的形态），
    // 没给则传空引擎 = 完全按系统缺省信任策略走，两条通路互不 shadow。
    HCERTCHAINENGINE engine = nullptr;
    if (root_store_ != nullptr) {
        auto config = CERT_CHAIN_ENGINE_CONFIG{};
        config.cbSize = static_cast<DWORD>(sizeof(config));
        config.dwFlags = CERT_CHAIN_CACHE_END_CERT;
        config.hExclusiveRoot = root_store_;
        if (::CertCreateCertificateChainEngine(&config, &engine) == FALSE) {
            return tls_error(ErrorCode::NetTlsHandshakeFailed, "certificate chain engine could not be created: " +
                                                                   hex_status(static_cast<LONG>(::GetLastError())));
        }
    }

    char server_auth[] = szOID_PKIX_KP_SERVER_AUTH;
    auto identifiers = std::array<char *, 1>{server_auth};
    auto usage = CERT_ENHKEY_USAGE{.cUsageIdentifier = 1, .rgpszUsageIdentifier = identifiers.data()};
    auto parameters = CERT_CHAIN_PARA{};
    parameters.cbSize = static_cast<DWORD>(sizeof(parameters));
    parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage = usage;

    const auto *built = static_cast<PCCERT_CHAIN_CONTEXT>(nullptr);
    const auto built_ok =
        ::CertGetCertificateChain(engine, peer_cert_, nullptr, supplied, &parameters, 0, nullptr, &built);
    auto verdict = Result<void>{};
    if (built_ok == FALSE || built == nullptr) {
        verdict = tls_error(ErrorCode::NetTlsCertRejected,
                            "certificate chain could not be built: " + hex_status(static_cast<LONG>(::GetLastError())));
    } else if (const auto errors = built->TrustStatus.dwErrorStatus; errors != CERT_TRUST_NO_ERROR) {
        verdict = tls_error(ErrorCode::NetTlsCertRejected,
                            "certificate chain is not trusted, status=" + hex_status(static_cast<LONG>(errors)));
    }
    if (built != nullptr) {
        ::CertFreeCertificateChain(built);
    }
    if (engine != nullptr) {
        ::CertFreeCertificateChainEngine(engine);
    }
    return verdict;
}

auto SchannelBackend::verify_hostname() const -> Result<void> {
    if (insecure_) {
        return Result<void>{};  // 危险开关的语义就是「本连接不做名字核对」，诊断已在发起处打过
    }
    if (peer_cert_ == nullptr) {
        return tls_error(ErrorCode::NetTlsCertRejected,
                         "peer presents no certificate to match against '" + host_ + "'");
    }
    const auto names = certificate_dns_names(peer_cert_);
    if (!names.empty()) {
        for (const auto &name : names) {
            if (name_matches(name, host_)) {
                return Result<void>{};
            }
        }
        return tls_error(ErrorCode::NetTlsCertRejected,
                         "certificate is issued for a different name than '" + host_ + "'");
    }
    // 只有 SAN 完全没给出 dNSName 才回落到 CN：SAN 存在却没有 dNSName 时，证书已经明确表达了
    // 「可核对的名字不在这张证书里」，再拿 CN 兜底等于把现代证书的规则退回 2000 年代。
    const auto common = certificate_common_name(peer_cert_);
    if (!common.empty() && name_matches(common, host_)) {
        return Result<void>{};
    }
    return tls_error(ErrorCode::NetTlsCertRejected, "certificate carries no name matching '" + host_ + "' (subject: " +
                                                        (common.empty() ? std::string{"<none>"} : common) + ")");
}

auto SchannelBackend::next_records(std::vector<char> &plaintext, std::vector<char> &outgoing) -> Result<TlsDataState> {
    if (peer_closed_) {
        return Result<TlsDataState>{TlsDataState::PeerClosed};
    }
    while (!pending_.empty()) {
        // 流模式口径（本机实测，非推断）：整段缓冲作为一枚 `SECBUFFER_DATA` 交出，由系统就地标注
        // 消费到哪里、还剩多少（`SECBUFFER_EXTRA`）。自己按记录边界切一条喂进去并不等价——重协商信号
        // 一次只消费一条外层记录，而「系统已经吞进多少字节」只有系统自己知道。
        auto buffers = std::array<SecBuffer, 4>{
            SecBuffer{.cbBuffer = static_cast<ULONG>(pending_.size()),
                      .BufferType = SECBUFFER_DATA,
                      .pvBuffer = pending_.data()},
            SecBuffer{.cbBuffer = 0, .BufferType = SECBUFFER_EMPTY, .pvBuffer = nullptr},
            SecBuffer{.cbBuffer = 0, .BufferType = SECBUFFER_EMPTY, .pvBuffer = nullptr},
            SecBuffer{.cbBuffer = 0, .BufferType = SECBUFFER_EMPTY, .pvBuffer = nullptr},
        };
        auto message = SecBufferDesc{
            .ulVersion = SECBUFFER_VERSION, .cBuffers = static_cast<DWORD>(buffers.size()), .pBuffers = buffers.data()};
        ULONG flag = 0;
        const auto status = ::DecryptMessage(&context_, &message, 0, &flag);
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            return Result<TlsDataState>{plaintext.empty() ? TlsDataState::NeedMore : TlsDataState::Delivered};
        }
        if (status == SEC_I_CONTEXT_EXPIRED) {
            pending_.clear();
            peer_closed_ = true;
            return Result<TlsDataState>{TlsDataState::PeerClosed};
        }
        if (status == SEC_I_RENEGOTIATE) {
            // 系统就地吃掉一条外层记录并回下重协商信号：TLS 1.3 的 NewSessionTicket 装在
            // `application_data` 记录里，Schannel 据此把它当握手消息消费（实测：握手后第一笔读必然命中
            // 这条分支两次，跳过它则后续记录全部 `SEC_E_DECODE_BITS`）。它回填的 EXTRA 是整段长度、
            // 不能用来前移，因此按记录头自己算这一条的边界。
            const auto boundary = first_record_bytes();
            const auto eaten = boundary == 0 ? pending_.size() : boundary;
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(eaten));
            if (auto round = renegotiate(outgoing); !round) {
                return round.error();
            }
            if (!outgoing.empty()) {
                // 系统要求把握手应答发出去（客户端重协商）：交付权在调用方，本轮到此为止。
                return Result<TlsDataState>{plaintext.empty() ? TlsDataState::NeedMore : TlsDataState::Delivered};
            }
            continue;
        }
        if (status != SEC_E_OK) {
            for (const auto &buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_ALERT) {
                    return tls_error(ErrorCode::NetTlsHandshakeFailed,
                                     "peer sent a TLS alert, status=" + hex_status(static_cast<LONG>(status)));
                }
            }
            return status_error(static_cast<LONG>(status), "tls record");
        }
        auto consumed = pending_.size();  // 没有 EXTRA 标注即整段都属于这一条消息
        for (const auto &buffer : buffers) {
            if (buffer.BufferType == SECBUFFER_EXTRA && static_cast<std::size_t>(buffer.cbBuffer) < consumed) {
                consumed = pending_.size() - static_cast<std::size_t>(buffer.cbBuffer);
            }
        }
        if (consumed == 0) {
            consumed = first_record_bytes();  // EXTRA 报成整段：按记录头切，否则同一批字节会原地重解
        }
        for (const auto &buffer : buffers) {
            if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer != 0 && buffer.pvBuffer != nullptr) {
                // 明文就落在 pending_ 的这一段里（系统就地改写），必须在搬走字节之前取出来。系统只给
                // 「基址 + 字节数」，按字节推进指针是 Schannel 流模式的 ABI 边界。
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                const auto *first = static_cast<const char *>(buffer.pvBuffer);
                plaintext.insert(plaintext.end(), first, first + buffer.cbBuffer);
                // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            }
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (!plaintext.empty()) {
            return Result<TlsDataState>{TlsDataState::Delivered};
        }
        // 解出一条不含应用正文的记录：继续排空缓冲。
    }
    return Result<TlsDataState>{plaintext.empty() ? TlsDataState::NeedMore : TlsDataState::Delivered};
}

auto SchannelBackend::renegotiate(std::vector<char> &outgoing) -> Result<void> {
    auto output = SecBuffer{.cbBuffer = 0, .BufferType = SECBUFFER_TOKEN, .pvBuffer = nullptr};
    auto output_desc = SecBufferDesc{.ulVersion = SECBUFFER_VERSION, .cBuffers = 1, .pBuffers = &output};
    auto attrs = DWORD{0};
    const auto status = ::InitializeSecurityContextW(&credential_, &context_, nullptr, context_request(), 0, 0, nullptr,
                                                     0, &context_, &output_desc, &attrs, nullptr);
    if (output.cbBuffer != 0 && output.pvBuffer != nullptr) {
        // 系统的握手产物按「基址 + 字节数」交付（内存由 `ISC_REQ_ALLOCATE_MEMORY` 分配），搬走它只能按
        // 字节推进指针——这是 Schannel 的 ABI 边界，没有可改的容器访问形态。
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto *first = static_cast<const char *>(output.pvBuffer);
        outgoing.insert(outgoing.end(), first, first + output.cbBuffer);
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ::FreeContextBuffer(output.pvBuffer);
    }
    if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_COMPLETE_NEEDED ||
        status == SEC_I_COMPLETE_AND_CONTINUE) {
        return Result<void>{};
    }
    return status_error(static_cast<LONG>(status), "tls renegotiation");
}

auto SchannelBackend::protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> {
    if (!handshaked_) {
        return tls_error(ErrorCode::NetTlsHandshakeFailed, "tls session is not established");
    }
    const auto limit = sizes_.cbMaximumMessage != 0 ? sizes_.cbMaximumMessage : AURORA_MAX_RECORD_BODY_BYTES;
    auto cursor = std::size_t{0};
    while (cursor < plaintext.size()) {
        const auto chunk = std::min(plaintext.size() - cursor, static_cast<std::size_t>(limit));
        std::vector<char> header(sizes_.cbHeader, '\0');
        std::vector<char> body(plaintext.begin() + static_cast<std::ptrdiff_t>(cursor),
                               plaintext.begin() + static_cast<std::ptrdiff_t>(cursor + chunk));
        std::vector<char> trailer(sizes_.cbTrailer, '\0');
        auto buffers = std::array<SecBuffer, 4>{
            SecBuffer{.cbBuffer = sizes_.cbHeader,
                      .BufferType = SECBUFFER_STREAM_HEADER,
                      .pvBuffer = header.empty() ? nullptr : header.data()},
            SecBuffer{
                .cbBuffer = static_cast<ULONG>(body.size()), .BufferType = SECBUFFER_DATA, .pvBuffer = body.data()},
            SecBuffer{.cbBuffer = sizes_.cbTrailer,
                      .BufferType = SECBUFFER_STREAM_TRAILER,
                      .pvBuffer = trailer.empty() ? nullptr : trailer.data()},
            SecBuffer{.cbBuffer = 0, .BufferType = SECBUFFER_EMPTY, .pvBuffer = nullptr},
        };
        auto message = SecBufferDesc{
            .ulVersion = SECBUFFER_VERSION, .cBuffers = static_cast<DWORD>(buffers.size()), .pBuffers = buffers.data()};
        const auto status = ::EncryptMessage(&context_, 0, &message, 0);
        if (status != SEC_E_OK) {
            return status_error(static_cast<LONG>(status), "tls record wrap");
        }
        // 系统就地写好头尾并把正文收回到实际长度：三段顺序拼接即线上记录。
        for (const auto &buffer : buffers) {
            if (buffer.cbBuffer != 0 && buffer.pvBuffer != nullptr) {
                // 头/正文/尾三段都由系统按「基址 + 字节数」回填，顺序拼接只能按字节推进指针。
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                const auto *first = static_cast<const char *>(buffer.pvBuffer);
                outgoing.insert(outgoing.end(), first, first + buffer.cbBuffer);
                // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            }
        }
        cursor += chunk;
    }
    return Result<void>{};
}

auto SchannelBackend::shutdown_records(std::vector<char> &outgoing) -> Result<void> {
    if (!handshaked_ || !have_context_) {
        return Result<void>{};  // 没有会话状态可关：空产出即「没发」而不是「失败」
    }
    auto info = SchannelShutdownInfo{};
    auto token = SecBuffer{.cbBuffer = sizeof(info), .BufferType = SECBUFFER_TOKEN, .pvBuffer = &info};
    auto control = SecBufferDesc{.ulVersion = SECBUFFER_VERSION, .cBuffers = 1, .pBuffers = &token};
    if (::ApplyControlToken(&context_, &control) != SEC_E_OK) {
        return Result<void>{};
    }
    auto output = SecBuffer{.cbBuffer = 0, .BufferType = SECBUFFER_TOKEN, .pvBuffer = nullptr};
    auto output_desc = SecBufferDesc{.ulVersion = SECBUFFER_VERSION, .cBuffers = 1, .pBuffers = &output};
    auto attrs = DWORD{0};
    const auto status = ::InitializeSecurityContextW(&credential_, &context_, nullptr, context_request(), 0, 0, nullptr,
                                                     0, &context_, &output_desc, &attrs, nullptr);
    if (output.cbBuffer != 0 && output.pvBuffer != nullptr) {
        // 系统的握手产物按「基址 + 字节数」交付（内存由 `ISC_REQ_ALLOCATE_MEMORY` 分配），搬走它只能按
        // 字节推进指针——这是 Schannel 的 ABI 边界，没有可改的容器访问形态。
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto *first = static_cast<const char *>(output.pvBuffer);
        outgoing.insert(outgoing.end(), first, first + output.cbBuffer);
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ::FreeContextBuffer(output.pvBuffer);
    }
    (void)status;  // 关闭通知的成败不可能有结论（对端已不再读），空产出与已产出都算尽力而为
    return Result<void>{};
}

}  // namespace

auto make_tls_backend(const TlsContext &context, std::string_view server_name) -> Result<std::unique_ptr<TlsBackend>> {
    auto backend = SchannelBackend::create(context, server_name);
    if (!backend) {
        return backend.error();
    }
    // 派生 → 基类的所有权移交必须写明一次：两级隐式转换（unique_ptr 与 Result 各一级）不被语言允许。
    return Result<std::unique_ptr<TlsBackend>>{std::unique_ptr<TlsBackend>{std::move(backend.value())}};
}

auto tls_backend_available() -> bool { return true; }

}  // namespace aurora::net::detail

#endif  // AURORA_NET_TLS_SCHANNEL
