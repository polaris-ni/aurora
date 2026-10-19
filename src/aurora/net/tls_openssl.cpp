// Aurora net — OpenSSL 后端。
//
// 本腿存在的理由是「Linux 没有可供本库驱动的系统 TLS 栈」，而自带一份 TLS 源码会同时破坏「断网可构建」与
// 「仓库不 vendored 三方」两条不变量，故库由使用方以 `AURORA_NET_TLS_PROVIDERS=<linkage>:<prefix>`
// 传入本地安装；宏 `AURORA_ENABLE_NET_TLS_OPENSSL` 由 `cmake/AuroraNetTls.cmake` 的头探测推导，本 TU 只在
// 该宏下产出符号。它与系统栈腿、mbedTLS 腿**可以同时在案**：多枚后端共存由 `net/tls_backend.cpp` 的
// 注册表在运行期分派，一次握手用哪一枚由候选链或 `TlsContext::backend` 决定。
//
// 字节进出**全部**走 memory BIO，本层一次都不碰 fd（无 `SSL_get_fd`、无 `select`/`poll`、无
// `BIO_new_connect`）：接缝（`net/detail/tls_backend.h`）的形态是「上层交付字节、后端回吐字节」，
// 一旦本层持有 socket 就会与上层 L1 的读队列抢同一份数据，且「不阻塞」这条纪律就从本地不变量变成
// 由对端与网络状况决定。落码后的三条推论都是必需的，不是顺手写法：
//   - 入向：`push_incoming()` 把字节写进读 BIO；空 BIO 以 retry 形态返回，OpenSSL 据此给出
//     `SSL_ERROR_WANT_READ`——这正是接缝的 NeedMore 语义，于是不需要任何 fd 等待就能表达「还缺字节」。
//   - 出向：写 BIO 没有容量上限，因此 `SSL_connect`/`SSL_read`/`SSL_write`/`SSL_shutdown` 都不会部分写；
//     但也因此**每次调用之后必须把它排空并追加进 `outgoing`**，漏一次就等于把半截记录私存在本层，
//     而对端已经在等上一条。
//   - 入向 BIO 写不进去只有「内存耗尽」一种成因。此处不留「静默吞字节」的通路：置一枚毒标记，
//     此后任何推进都以失败收尾。吞掉的后果是上层无限等一次永远不够的读。
//
// 信任判定交给 OpenSSL 的内置校验器（与 Schannel 腿**相反**的分工，且是有理由的相反）：本平台的
// 校验器接受一枚可增删的 `X509_STORE`，「追加可信根」这件事在这里做得到，于是没有理由在本层重写
// 一份链引擎——链构建与名字核对是最容易两条腿分叉的地方。追加是**合并**而非替换：
// `SSL_CTX_set_default_verify_paths` 先把系统缺省根装进 ctx 自己的 store，PEM 再逐张加进同一枚
// store；因此对接公网端点的证书照旧可信。主机名走 `SSL_set1_host`（校验器内的 RFC 6125 通路：
// SAN 的 dNSName 优先、有 SAN 就不再回看 CN、通配只认最左标签且不跨点），与 Schannel 腿手写的
// `name_matches` 是同一套规则。
//
// 危险开关（`allow_insecure_certificate`）在本层的落点是 `SSL_VERIFY_NONE`——链、有效期、主机名
// 一次全跳过；那条 warning 诊断由 `net/tls.cpp` 在发起处统一打（两腿同一条，本层重复打就是把同
// 一件事写成两种说法）。pin 判定不受它影响，同样在 `net/tls.cpp` 里执行。
//
// 与 Schannel 腿的四处**已知不重合**，都记录在此以免后来者当成 bug：
//   - 本层不实现「握手期不产出明文」以外的例外：TLS 1.3 的 NewSessionTicket 装在
//     `application_data` 记录里，OpenSSL 在 `SSL_read`/`SSL_connect` 内部就地消费并登记进会话
//     缓存，既不产出明文也不外发回执，因此接缝那条「数据阶段可外发握手字节」的通路在本腿只被
//     服务端发起的重协商用到（TLS 1.2 及以下；TLS 1.3 里 OpenSSL 直接按 RFC 8446 回一条
//     `no_renegotiation` 告警后继续用原会话密钥，同样不需要外发）。
//   - 版本下界只到 TLS 1.0：`TlsVersion` 没有 SSL 3 这一档，OpenSSL 3.x 也已把 SSL 3/早期版本
//     按构建配置关死，`SSL_OP_NO_SSLv3` 一类补丁因此不必设。
//   - 吊销（CRL/OCSP）不做：不置 `X509_V_FLAG_CRL_CHECK*` 即沿用 OpenSSL 缺省的「离线不判吊销」，
//     与 Schannel 腿同口径；在线取回 AIA 属另一件事。
//   - 「服务端要求客户端证书」的**拒绝时机**不在本层抹平：TLS 1.2 及以下是握手内事件（服务端等得到客户端
//     证书才发 Finished，缺证书的致命告警因此先于 `SSL_connect` 返回），TLS 1.3 是**握手后**事件（服务端的
//     Finished 已在自己的首飞里发完，客户端 `SSL_connect` 先成功，告警在第一次记录交换时抵达——见
//     `RFC 8446` §4.3.2 与 §4.4.2 的时序）。据此公共语义是「握手成功不蕴含服务端已接受客户端证书」，
//     实测覆盖两条 provider 腿（OpenSSL 与 mbedTLS 都先成功后落败）与 Windows 腿（Schannel 在握手内即拒）。

#ifdef AURORA_ENABLE_NET_TLS_OPENSSL

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

namespace aurora::net::detail {
namespace {

constexpr std::size_t AURORA_MAX_RECORD_BODY_BYTES = 16384;  ///< 一条 TLS 记录的正文上限（RFC 8446 §5.2）
constexpr std::size_t AURORA_MAX_PUSH_BYTES = 1U << 20U;  ///< 一次写进入向 BIO 的上界（只为长度参数切块）
constexpr std::size_t AURORA_SHA256_DIGEST_BYTES = 32;  ///< SHA-256 摘要长度（base64 后恒 44 字符）
constexpr std::size_t AURORA_MAX_ALPN_PROTOCOL_ID_BYTES = 255;  ///< 单条标识的长度前缀是 u8
constexpr std::size_t AURORA_MAX_ALPN_TABLE_BYTES = 65535;  ///< 整表字节数的长度前缀是 u16

auto tls_error(ErrorCode code, std::string text) -> Error {
    return make_error(code, ErrorParams{{"detail", std::move(text)}});
}

/// @brief 客户端身份那几处失败的构造口。
///
/// 它们走「调用方覆盖 message」那一枚 `make_error` 重载而不是上面的 `tls_error`：后者把文本交给表模板的
/// `{detail}` 槽，而 `general-invalid-argument` 的模板没有那一槽（`codespec/errors.toml` 里它渲染成一句
/// 「Invalid argument supplied」），于是「PEM 的哪一步、缺什么」会在现场被整段吃掉——本节目的全部是替调用方
/// 指认他给错了哪一份材料，指认丢了就等于没指认。同文件其余各码（`net-tls-*` / `net-platform-unavailable`）
/// 的模板带槽，仍走 `tls_error`。
auto identity_error(const std::string &text) -> Error { return make_error(ErrorCode::GeneralInvalidArgument, text); }

/// 取 OpenSSL 错误队列的栈顶原因文本。
///
/// 只取栈顶是有取舍的：OpenSSL 一次失败常留下三四层调用栈，而本层的诊断面（`detail`）只有一条
/// 句子的位置；栈顶恰是最具体的那一条 reason。队列在此被清空，故调用方**不得**指望第二次读到同
/// 一条——要留现场就当场拼进文本。
auto openssl_reason() -> std::string {
    const auto code = ::ERR_get_error();
    if (code == 0) {
        return {"no library error was recorded"};
    }
    const auto *text = ::ERR_reason_error_string(code);
    return text != nullptr ? std::string{text} : std::string{"library error without a reason string"};
}

/// 属性类型的名字：用 OpenSSL 的 NID 短名（`CN` / `O` / `DC` 一类），不认识的 OID 回点分十进制而不是
/// 干脆省掉这一项——省掉会让「这张证书有一个非标准属性」这件事在文本里彻底消失。
auto attribute_name(const ASN1_OBJECT *object) -> std::string {
    if (object == nullptr) {
        return {};
    }
    const int nid = ::OBJ_obj2nid(object);
    if (const char *short_name = nid != NID_undef ? ::OBJ_nid2sn(nid) : nullptr; short_name != nullptr) {
        return {short_name};
    }
    std::array<char, 128> buffer{};
    const int written = ::OBJ_obj2txt(buffer.data(), buffer.size(), object, 1);
    return written > 0 ? std::string{buffer.data(), static_cast<std::size_t>(written)} : std::string{};
}

/// 可分辨名 → 文本：按 DER 的条目**正序**展开成 `短名=值`，条目之间以 `, ` 连接。
///
/// 不用 `X509_NAME_print_ex`：`XN_FLAG_RFC2253` 按 RFC 2253 的规定**逆序**展开（主体在前），
/// `XN_FLAG_ONELINE` 又带 `XN_FLAG_SPC_EQ` 产出 `CN = localhost`，而其余三腿给的都是正序无空格形态
/// ——Schannel 腿走 CryptoAPI `CERT_X500_NAME_STR`、mbedTLS 腿走 `mbedtls_x509_dn_gets`、
/// SecureTransport 腿自己按 DER 走一遍。实测（同一张 `CN=example.com` 叶子的签发者）三腿逐字相同为
/// `C=US, O=SSL Corporation, CN=Cloudflare TLS Issuing ECC CA 3`，OpenSSL 的两个现成旗标都与之不同。
/// `PeerCertificate` 的 subject / issuer 在公共头里承诺为「不含平台句柄、可在任何腿上复制与比较」的
/// 摘要，文本形态因此不能按各库缺省各写一份（跨腿逐值判据见 `codespec/manual-test/23-net.md` 的
/// `TC-NET-006`，它比的就是这两行）。
///
/// 残余差异如实记账，不做归一：值里含逗号、等号、引号或非 ASCII 时的**转义表**四腿各不相同
/// （CryptoAPI 加双引号、mbedTLS 用反斜杠、本腿原样交付），属性类型的**短名表**也各自一套。归一要
/// 把三家文档的规则都自己实现一遍，换来的只是罕见 DN 看起来一样，而真正跨腿可比的是 SPKI 摘要那一行。
auto name_text(const X509_NAME *name) -> std::string {
    if (name == nullptr) {
        return {};
    }
    auto text = std::string{};
    const int count = ::X509_NAME_entry_count(name);
    for (int index = 0; index < count; ++index) {
        const X509_NAME_ENTRY *entry = ::X509_NAME_get_entry(name, index);
        if (entry == nullptr) {
            continue;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += attribute_name(X509_NAME_ENTRY_get_object(entry));
        text += '=';
        const ASN1_STRING *value = ::X509_NAME_ENTRY_get_data(entry);
        if (value != nullptr) {
            // 长度以 `ASN1_STRING_length` 为权威（DER 允许值里含 NUL），不能用 C 字符串长度截断。
            const unsigned char *bytes = ::ASN1_STRING_get0_data(value);
            const int length = ::ASN1_STRING_length(value);
            if (bytes != nullptr && length > 0) {
                // ASN1_STRING 的载荷就是 UTF-8（X.500 目录串），按字节接进文本而不是逐字符转码。
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                text.append(reinterpret_cast<const char *>(bytes), static_cast<std::size_t>(length));
            }
        }
    }
    return text;
}

/// `ASN1_TIME` → ISO-8601 UTC（`YYYY-MM-DDTHH:MM:SSZ`，恒 20 字符）。
///
/// 为什么不把库的文本直接交出去：RFC 5280 的有效期既可能是 UTCTime（两位年）也可能是
/// GeneralizedTime，按缺省文本交付会让同一枚公共字段在两张证书上是两种形态。
/// `ASN1_TIME_to_tm` 已把两者归一到 UTC 分解时间，本层只补一次固定宽度拼接。
auto time_text(const ASN1_TIME *value) -> std::string {
    if (value == nullptr) {
        return {};
    }
    auto broken = std::tm{};
    if (::ASN1_TIME_to_tm(value, &broken) != 1) {
        return {};
    }
    // 缓冲按最坏情形给：`%04d` 面对的是 `int`，编译器无法排除「11 个字符」的极端值，
    // `-Wformat-truncation` 因此判定任何刚好容纳 20 字符的缓冲都可能截断。真实证书永远落在四位年内，
    // 多给的这一段只是让「不会截断」成为可证明的事实，而不是对取值再叠一层假设。
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", broken.tm_year + 1900, broken.tm_mon + 1,
                  broken.tm_mday, broken.tm_hour, broken.tm_min, broken.tm_sec);
    return {buffer};
}

/// SPKI 摘要：`base64(SHA-256(SubjectPublicKeyInfo 的 DER))`。
///
/// 口径必须与 Schannel 腿逐字节相同——`pinned_public_keys` 是被承诺「跨腿可互换」的配置值，而
/// pin 判定的失败原因（对端换了钥匙）与被静默改写的 pin 集合完全无法区分。这里的对应关系是：
/// `i2d_X509_PUBKEY` 导出的是完整 SPKI `SEQUENCE`（算法标识 + 公钥位串），与 CryptoAPI 对
/// `CERT_PUBLIC_KEY_INFO` 做 `CryptEncodeObject(X509_PUBLIC_KEY_INFO)` 得到的是同一段字节。
/// 取裸公钥位串、或按整证摘要，都会让两腿的 pin 各算各的。
auto spki_digest(const X509 *certificate) -> std::string {
    if (certificate == nullptr) {
        return {};
    }
    auto *public_key = ::X509_get_X509_PUBKEY(certificate);  // 不额外加引用：随证书本体存活
    if (public_key == nullptr) {
        return {};
    }
    unsigned char *der = nullptr;
    const auto encoded = ::i2d_X509_PUBKEY(public_key, &der);  // 由 OpenSSL 分配，必须 OPENSSL_free
    if (encoded <= 0 || der == nullptr) {
        return {};
    }
    auto digest = std::array<unsigned char, AURORA_SHA256_DIGEST_BYTES>{};
    unsigned int size = 0;
    auto outcome = std::string{};
    if (::EVP_Digest(der, static_cast<std::size_t>(encoded), digest.data(), &size, ::EVP_sha256(), nullptr) == 1 &&
        size == AURORA_SHA256_DIGEST_BYTES) {
        // `EVP_EncodeBlock`：标准 base64 表 + 填充，不插换行、返回值不含结尾空字符——32 字节恒 44 字符，
        // 正是 `PeerCertificate::public_key_sha256` 承诺的形态。
        char encoded_text[64] = {};
        // OpenSSL 的字节缓冲在类型上恒为 `unsigned char *`，与 `char *` 之间没有合法的 `static_cast`
        // 桥（两者指针域不同），而 EVP 的编码接口只收前者——C API 边界的固定拼法，与仓内 Win32/COM
        // 出参同形，故只在该调用上区间豁免，不外扩到别的检查。
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto written = ::EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded_text), digest.data(),
                                               static_cast<int>(AURORA_SHA256_DIGEST_BYTES));
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        if (written > 0) {
            outcome.assign(encoded_text, static_cast<std::size_t>(written));
        }
    }
    ::OPENSSL_free(der);
    return outcome;
}

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

/// 档位 → OpenSSL 的版本常量（`SSL_CTX_set_min_proto_version` 的入参形态）。
auto openssl_version(TlsVersion version) -> int {
    switch (version) {
        case TlsVersion::Tls10:
            return TLS1_VERSION;
        case TlsVersion::Tls11:
            return TLS1_1_VERSION;
        case TlsVersion::Tls12:
            return TLS1_2_VERSION;
        case TlsVersion::Tls13:
            return TLS1_3_VERSION;
    }
    return TLS1_2_VERSION;
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

/// ALPN 宣告（wire 形态：整表由 `SSL_CTX_set_alpn_protos` 的长度参数表达，逐条 `u8 长度 + 标识`）。
///
/// OpenSSL 要求调用方自己排好这张表，且单条标识的长度前缀必须是 u8，故超出 255 字节者一律丢弃
/// （口径同 Schannel 腿：宁可不宣告一条非法标识，也不让整次握手因配置里的一条超长标识而失败）。
/// @param protocols 按偏好排序的协议标识。
/// @return 编码后的表；未宣告（空输入或空表）时为空串。
auto build_alpn_advertisement(const std::vector<std::string> &protocols) -> std::vector<unsigned char> {
    std::vector<unsigned char> table;
    for (const auto &protocol : protocols) {
        if (protocol.empty() || protocol.size() > AURORA_MAX_ALPN_PROTOCOL_ID_BYTES ||
            table.size() + protocol.size() + 1 > AURORA_MAX_ALPN_TABLE_BYTES) {
            continue;
        }
        table.push_back(static_cast<unsigned char>(protocol.size()));
        table.insert(table.end(), protocol.begin(), protocol.end());
    }
    return table;
}

/// PKCS#12 档案（内存字节）→ 客户端凭据。
///
/// 档案不落盘是公共面定的形态（`client_identity` 是**字节内容**），故这里用一枚只读 memory BIO 喂
/// `d2i_PKCS12_bio`。链的处理是这函数里唯一容易被略过的一处：先 `SSL_CTX_clear_chain_certs` 再逐张
/// `SSL_CTX_add1_chain_cert`——不清就会把上一次配好的链留在 ctx 身上，而「发出去的那条链到底是谁」
/// 直接决定服务端认不认这个客户端；档案里没有中间证书时产出就只剩叶子，这也对。
///
/// 口令错的落点是 `general-invalid-argument`（配置不合用）而不是 `net-tls-*`：此刻还没有任何对端
/// 参与，把它写成连接类错误会把排查方向推向日志里的服务端。
/// @param ctx 待装凭据的上下文（所有权仍归调用方）。
/// @param archive PKCS#12 字节。
/// @param passphrase 保护口令；档案无口令时为空串。
/// @return 成功为空；失败携 `general-invalid-argument` 与成因文本。
[[nodiscard]] auto apply_client_identity(SSL_CTX *ctx, std::string_view archive, std::string_view passphrase)
    -> Result<void> {
    // 只读 memory BIO 不复制正文，故 `archive`（即 `TlsContext::client_identity`）须存活到本函数返回——
    // 它由调用方的上下文快照持有，跨过 `SSL_CTX_use_*` 那几次复制即安全。
    auto *source = ::BIO_new_mem_buf(archive.data(), static_cast<int>(archive.size()));
    if (source == nullptr) {
        return tls_error(ErrorCode::GeneralInvalidArgument, "client identity could not be opened for reading");
    }
    auto *archive_handle = static_cast<PKCS12 *>(nullptr);
    if (::d2i_PKCS12_bio(source, &archive_handle) == nullptr || archive_handle == nullptr) {
        const auto reason = openssl_reason();
        ::BIO_free(source);
        return tls_error(ErrorCode::GeneralInvalidArgument,
                         "client identity is not a readable PKCS#12 archive: " + reason);
    }
    auto *key = static_cast<EVP_PKEY *>(nullptr);
    auto *leaf = static_cast<X509 *>(nullptr);
    auto *chain = static_cast<STACK_OF(X509) *>(nullptr);
    auto secret = std::string{passphrase};
    if (::PKCS12_parse(archive_handle, secret.c_str(), &key, &leaf, &chain) != 1) {
        const auto reason = openssl_reason();
        ::PKCS12_free(archive_handle);
        ::BIO_free(source);
        return tls_error(ErrorCode::GeneralInvalidArgument,
                         "client identity could not be unlocked, check client_identity_passphrase: " + reason);
    }
    auto outcome = Result<void>{};
    if (leaf == nullptr || key == nullptr) {
        outcome = tls_error(ErrorCode::GeneralInvalidArgument,
                            "client identity carries no certificate with a usable private key");
    } else if (::SSL_CTX_use_certificate(ctx, leaf) != 1) {
        outcome = tls_error(ErrorCode::GeneralInvalidArgument,
                            "the client certificate could not be installed: " + openssl_reason());
    } else if (::SSL_CTX_use_PrivateKey(ctx, key) != 1) {
        outcome = tls_error(ErrorCode::GeneralInvalidArgument,
                            "the client private key could not be installed: " + openssl_reason());
    } else if (::SSL_CTX_check_private_key(ctx) != 1) {
        outcome = tls_error(ErrorCode::GeneralInvalidArgument,
                            "the client identity holds a private key that does not match its certificate");
    } else {
        ::SSL_CTX_clear_chain_certs(ctx);
        // `sk_X509_*` 在这一族安全栈宏里展开（不是可调用的函数名），因此都不能加 `::` 限定。
        for (int index = 0; chain != nullptr && index < sk_X509_num(chain); ++index) {
            auto *item = sk_X509_value(chain, index);
            if (item == nullptr) {
                continue;
            }
            if (::SSL_CTX_add1_chain_cert(ctx, item) != 1) {
                outcome = tls_error(ErrorCode::GeneralInvalidArgument,
                                    "the client certificate chain could not be installed: " + openssl_reason());
                break;
            }
        }
    }
    ::X509_free(leaf);
    ::EVP_PKEY_free(key);
    if (chain != nullptr) {
        sk_X509_pop_free(chain, X509_free);
    }
    ::PKCS12_free(archive_handle);
    ::BIO_free(source);
    return outcome;
}

/// PEM 文本（证书链 + 私钥）→ 客户端凭据。
///
/// 与上面那条 PKCS#12 通路共用同一套落点与次序：先叶、再中间证书、最后 `SSL_CTX_check_private_key`；
/// 链的清理口径也一致——`SSL_CTX_clear_chain_certs` 先行，否则上一次配好的链留在 ctx 身上，而「发出去的
/// 那条链到底是谁」直接决定服务端认不认这个客户端。区别只在材料：这里没有容器要解，也不接口令
/// （加密过的 PEM 私钥本层不解，入参约定见 `net/tls.h`）。
/// @param ctx 待装凭据的上下文（所有权仍归调用方）。
/// @param chain_pem 证书链 PEM 文本，叶证书在前（契约如此，这里不做重排）。
/// @param key_pem 私钥 PEM 文本（未加密）。
/// @return 成功为空；失败携 `general-invalid-argument` 与成因文本。
[[nodiscard]] auto apply_client_identity_pem(SSL_CTX *ctx, std::string_view chain_pem, std::string_view key_pem)
    -> Result<void> {
    auto *chain_source = ::BIO_new_mem_buf(chain_pem.data(), static_cast<int>(chain_pem.size()));
    if (chain_source == nullptr) {
        return identity_error("the client certificate chain could not be opened for reading");
    }
    // `sk_X509_*` 在这一族安全栈宏里展开（不是可调用的函数名），因此都不能加 `::` 限定。
    auto *extras = sk_X509_new_null();
    auto *leaf = ::PEM_read_bio_X509(chain_source, nullptr, nullptr, nullptr);
    if (leaf != nullptr) {
        // 其余条目是中间证书：读到末尾返回空指针，那不是失败，只是 PEM 到头了。
        for (;;) {
            auto *item = ::PEM_read_bio_X509(chain_source, nullptr, nullptr, nullptr);
            if (item == nullptr) {
                break;
            }
            if (sk_X509_push(extras, item) <= 0) {
                ::X509_free(item);
                break;
            }
        }
    }
    ::BIO_free(chain_source);
    if (leaf != nullptr) {
        // 读循环每次「没有下一块」都会往错误队列塞一条 `no start line`；叶证书已到手时那不是失败，留着
        // 只会盖掉后面真正的成因——本层的诊断取的是栈顶。
        ::ERR_clear_error();
    }

    auto *key_source = ::BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
    auto *key = static_cast<EVP_PKEY *>(nullptr);
    if (key_source != nullptr) {
        key = ::PEM_read_bio_PrivateKey(key_source, nullptr, nullptr, nullptr);
        ::BIO_free(key_source);
    }

    auto outcome = Result<void>{};
    if (leaf == nullptr) {
        // 这句话与 mbedTLS 腿逐字相同（那边把「整段不是可解码的 X.509」与「结构里没填进证书」合成一支判）：
        // 对调用方而言这就是同一件事——链这一份材料里没有可用证书。
        outcome = identity_error("the client certificate chain holds no usable X.509 certificate: " + openssl_reason());
    } else if (key == nullptr) {
        outcome = identity_error("the client private key is not a readable unencrypted PEM key: " + openssl_reason());
    } else if (::X509_check_private_key(leaf, key) != 1) {
        // 配对判据走在装入**之前**。OpenSSL 3 会顺手在 `SSL_CTX_use_PrivateKey` 里比对公钥，于是同一件事在
        // 版本间会说成「could not be installed: key values mismatch」——那是引擎自己的步骤名，不是调用方要听的
        // 诊断。这里直接问「这枚私钥配不配这张叶证书」，本层与 mbedTLS 腿的 `mbedtls_pk_check_pair` 因此对同一件
        // 事给同一句话，且失败时 ctx 上还没落下半份凭据。
        outcome = identity_error("the client private key does not match the client certificate");
    } else if (::SSL_CTX_use_certificate(ctx, leaf) != 1) {
        outcome = identity_error("the client certificate could not be installed: " + openssl_reason());
    } else if (::SSL_CTX_use_PrivateKey(ctx, key) != 1) {
        outcome = identity_error("the client private key could not be installed: " + openssl_reason());
    } else if (::SSL_CTX_check_private_key(ctx) != 1) {
        // 上面判的是两份入参，这一步判的是已经落到 ctx 上的那一对。预检通过后它不该再触发，留作最后一道复核：
        // 「本层返回成功」从此恒等于「ctx 自己确认过这条链配这把私钥」。
        outcome = identity_error(
            "the client private key does not match the client certificate (verified after installation)");
    } else {
        ::SSL_CTX_clear_chain_certs(ctx);
        for (int index = 0; index < sk_X509_num(extras); ++index) {
            auto *item = sk_X509_value(extras, index);
            if (item == nullptr) {
                continue;
            }
            if (::SSL_CTX_add1_chain_cert(ctx, item) != 1) {
                outcome = identity_error("the client certificate chain could not be installed: " + openssl_reason());
                break;
            }
        }
    }
    ::X509_free(leaf);
    ::EVP_PKEY_free(key);
    if (extras != nullptr) {
        sk_X509_pop_free(extras, X509_free);
    }
    return outcome;
}

/// 一条 TLS 会话的 OpenSSL 实现（线程约定见 `detail/tls_backend.h`：调用方串行化，本类不自持锁）。
class OpenSslBackend final : public TlsBackend {
  public:
    [[nodiscard]] static auto create(const TlsContext &context, std::string_view server_name)
        -> Result<std::unique_ptr<OpenSslBackend>>;

    ~OpenSslBackend() override;

    /// 会话即「一份 SSL 句柄 + 一对 memory BIO」的所有权载体，复制会让两份析构各自归还一次底层资源；
    /// 接缝只经 `make_tls_backend_openssl` 交出 `unique_ptr`，因此显式禁掉复制与移动（资源本身不可复制）。
    OpenSslBackend(const OpenSslBackend &) = delete;
    OpenSslBackend(OpenSslBackend &&) = delete;
    auto operator=(const OpenSslBackend &) -> OpenSslBackend & = delete;
    auto operator=(OpenSslBackend &&) -> OpenSslBackend & = delete;

    auto push_incoming(std::span<const char> bytes) -> void override;

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
    OpenSslBackend() = default;

    /// 排空写 BIO 并追加进 `outgoing`（见文件头：每次进入 OpenSSL 之后都必须调一次）。
    auto drain_outgoing(std::vector<char> &outgoing) -> void;

    /// 握手完成那一刻的取证：对端证书摘要与协商出的 ALPN。
    auto capture_negotiated() -> void;

    /// `SSL_connect`/`SSL_read`/`SSL_write` 返回 ≤0 之后的分类。
    /// @param ret 该次调用的返回值（`SSL_get_error` 的必需入参）。
    /// @param phase 阶段文本（进诊断，用于区分「同一句 OpenSSL 原因出现在哪一步」）。
    /// @return 未置值 = 没有结论，还能推进（缺字节 / 已收到 close_notify，后者由 `peer_closed_` 表达）；
    ///         置值 = 本层的失败原因（证书判定不过优先于「握手失败」）。
    auto stall_reason(int ret, std::string_view phase) -> std::optional<Error>;

    /// 校验器结论 → 本层的拒绝理由（危险开关下恒为未置值）。
    [[nodiscard]] auto verification_verdict() const -> std::optional<Error>;

    /// 入向 BIO 写不进去（唯一成因是内存耗尽）之后的失败：让每次推进都立刻给出结论。
    [[nodiscard]] auto poisoned() const -> std::optional<Error> {
        if (!input_rejected_) {
            return std::nullopt;
        }
        return tls_error(ErrorCode::NetTlsHandshakeFailed,
                         "the incoming record buffer could not be filled, the session is unrecoverable");
    }

    SSL_CTX *ctx_ = nullptr;  ///< 配置面（版本界、信任根、凭据、ALPN、校验档位）；本对象持有
    SSL *ssl_ = nullptr;  ///< 会话状态机；持有两枚 memory BIO（由 `SSL_set_bio` 移交，故不单独释放）
    bool handshaked_ = false;  ///< 握手是否完成
    bool peer_closed_ = false;  ///< 已收到 close_notify（此后不再产出明文）
    bool input_rejected_ = false;  ///< 入向缓冲拒绝写入
    bool insecure_ = false;  ///< 危险开关：链、有效期、主机名判定一并跳过
    std::string host_;  ///< SNI 与主机名校验的目标名（原样文本，OpenSSL 自身不区分大小写）
    std::optional<PeerCertificate> cert_;  ///< 对端证书摘要（握手完成那一刻固化）
    std::optional<std::string> alpn_;  ///< 协商出的 ALPN
    std::array<char, AURORA_MAX_RECORD_BODY_BYTES> scratch_{};  ///< 记录级暂存（读写两向复用，同一会话内串行使用）
};

auto OpenSslBackend::create(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<OpenSslBackend>> {
    // 版本界先判：接缝文档把「上界低于下界」列为本层的 `general-invalid-argument`，而 `net/tls.cpp`
    // 早在后端登场前就以 `net-tls-version-mismatch` 拦下了同一条。两处都要——本层被直接调用时
    // （测试与工具）不该静默把空允许集交给 OpenSSL 去猜。
    if (context.max_version && version_rank(*context.max_version) < version_rank(context.min_version)) {
        return tls_error(ErrorCode::GeneralInvalidArgument,
                         "the requested TLS version upper bound is below the lower bound");
    }

    std::unique_ptr<OpenSslBackend> backend{new OpenSslBackend()};
    backend->host_ = to_lower_ascii(server_name);
    backend->insecure_ = context.allow_insecure_certificate;

    auto *ctx = ::SSL_CTX_new(::TLS_client_method());
    if (ctx == nullptr) {
        return tls_error(ErrorCode::NetTlsEngineUnavailable,
                         "a TLS client context could not be created: " + openssl_reason());
    }
    if (::SSL_CTX_set_min_proto_version(ctx, openssl_version(context.min_version)) != 1) {
        ::SSL_CTX_free(ctx);
        return tls_error(ErrorCode::GeneralInvalidArgument,
                         "this OpenSSL build does not support the requested minimum TLS version");
    }
    // 上界未置值即不调：让 OpenSSL 用其编译期最高版本，正是公共面「未置值=后端最高支持版本」的形态。
    if (context.max_version && ::SSL_CTX_set_max_proto_version(ctx, openssl_version(*context.max_version)) != 1) {
        ::SSL_CTX_free(ctx);
        return tls_error(ErrorCode::GeneralInvalidArgument,
                         "this OpenSSL build does not support the requested maximum TLS version");
    }

    // 信任根：系统缺省根 + 追加根，装进同一枚 store（合并而非替换，见文件头）。
    // `SSL_CTX_set_default_verify_paths` 的返回值在这里不作判据：它报告的是「这一族路径能不能
    // 打开」，而容器化文件系统里 `dir` 哈希目录缺失、`file` 照旧有效是常态——把非零返回当失败就会
    // 让「有系统根可用」的现场报错。真正的可用性由校验器在对端送证时给出结论。
    (void)::SSL_CTX_set_default_verify_paths(ctx);
    auto *store = ::SSL_CTX_get_cert_store(ctx);  // 不转移所有权：store 随 ctx 存活
    if (store == nullptr) {
        ::SSL_CTX_free(ctx);
        return tls_error(ErrorCode::NetTlsEngineUnavailable, "the trust store is not reachable on this context");
    }
    for (const auto &pem : context.trusted_certificates) {
        auto *source = ::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        if (source == nullptr) {
            ::SSL_CTX_free(ctx);
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "a trusted certificate could not be opened for reading");
        }
        auto loaded = 0;
        for (;;) {
            auto *certificate = ::PEM_read_bio_X509(source, nullptr, nullptr, nullptr);
            if (certificate == nullptr) {
                break;  // 读到末尾或非证书块：一张字符串里串多块 PEM 是合法输入，故由这里收口
            }
            if (::X509_STORE_add_cert(store, certificate) != 1) {
                ::X509_free(certificate);
                ::BIO_free(source);
                ::SSL_CTX_free(ctx);
                return tls_error(ErrorCode::GeneralInvalidArgument,
                                 "a trusted certificate could not be added to the trust store: " + openssl_reason());
            }
            ::X509_free(certificate);
            ++loaded;
        }
        ::BIO_free(source);
        // PEM 读循环每次「没有下一块」都会往错误队列塞一条 `no start line`；不清掉就会盖掉后面
        // 真正的失败原因——本层的诊断取的是栈顶。
        ::ERR_clear_error();
        if (loaded == 0) {
            ::SSL_CTX_free(ctx);
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "trusted certificate holds no PEM X.509 certificate block");
        }
    }

    // 校验档位：危险开关一次跳过链、有效期与主机名；否则交校验器（追加根在这里生效）。
    // 没有第二枚开关：`allow_insecure_certificate` 的语义在公共面写死了「链与主机名一并跳过」。
    if (backend->insecure_) {
        ::SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        ::SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    }

    const auto advertisement = build_alpn_advertisement(context.alpn_protocols);
    if (!advertisement.empty() &&
        ::SSL_CTX_set_alpn_protos(ctx, advertisement.data(), static_cast<unsigned int>(advertisement.size())) != 0) {
        // 这一枚接口的约定是「0 才算成功」（与多数 OpenSSL 接口相反），非 0 只有长度越界与内存不足。
        ::SSL_CTX_free(ctx);
        return tls_error(ErrorCode::GeneralInvalidArgument, "the ALPN advertisement was rejected: " + openssl_reason());
    }

    if (context.client_identity) {
        if (auto identity =
                apply_client_identity(ctx, *context.client_identity, context.client_identity_passphrase.value_or(""));
            !identity) {
            ::SSL_CTX_free(ctx);
            return identity.error();
        }
    }
    // 起手闸已保证这两种形态不会同时出现（`net/tls.cpp` 的 `validate_handshake_inputs`），故这里各判各的。
    if (context.client_certificate_pem) {
        if (auto identity = apply_client_identity_pem(ctx, *context.client_certificate_pem,
                                                      context.client_private_key_pem.value_or(""));
            !identity) {
            ::SSL_CTX_free(ctx);
            return identity.error();
        }
    }

    auto *ssl = ::SSL_new(ctx);
    if (ssl == nullptr) {
        ::SSL_CTX_free(ctx);
        return tls_error(ErrorCode::NetTlsEngineUnavailable,
                         "a TLS session object could not be created: " + openssl_reason());
    }
    backend->ctx_ = ctx;
    backend->ssl_ = ssl;

    // 收发各一枚 memory BIO。所有权在 `SSL_set_bio` 那一刻移交（`SSL_free` 负责释放两枚），此后
    // 一律不再单独 free——重复释放是这一层最容易写错的一处，因为两枚 BIO 在代码里看起来像本地变量。
    auto *incoming = ::BIO_new(::BIO_s_mem());
    auto *outgoing_bio = ::BIO_new(::BIO_s_mem());
    if (incoming == nullptr || outgoing_bio == nullptr) {
        if (incoming != nullptr) {
            ::BIO_free(incoming);
        }
        if (outgoing_bio != nullptr) {
            ::BIO_free(outgoing_bio);
        }
        return tls_error(ErrorCode::NetTlsEngineUnavailable,
                         "the record buffers for this session could not be created: " + openssl_reason());
    }
    ::SSL_set_bio(ssl, incoming, outgoing_bio);

    if (!backend->host_.empty()) {
        // SNI：公共面把 `server_name` 同时用作 SNI 与主机名校验目标（见 `net/tls.h`），两者因此
        // 必须来自同一枚字符串，不然「校验过了」与「问的机器」就是两件事。宏内部复制正文，
        // 故这里传入的缓冲不需要活过本次调用。
        (void)::SSL_set_tlsext_host_name(ssl, backend->host_.c_str());
        // 主机名核对：登记进校验器的 `X509_VERIFY_PARAM`，与链校验同一条通路出结论（因此同归于
        // `net-tls-cert-rejected`）。危险开关下不登记——那一档的语义就是「本连接不做名字核对」。
        if (!backend->insecure_ && ::SSL_set1_host(ssl, backend->host_.c_str()) != 1) {
            return tls_error(
                ErrorCode::GeneralInvalidArgument,
                "the peer host name could not be registered for certificate verification: " + openssl_reason());
        }
    }
    return Result<std::unique_ptr<OpenSslBackend>>{std::move(backend)};
}

OpenSslBackend::~OpenSslBackend() {
    // `SSL_free` 连带释放两枚 memory BIO 与 `SSL_set_bio` 之后被它接管的引用；
    // 上下文单独释放，且在会话之后（`SSL` 持有 ctx 的引用，反序也不至于崩，但顺序写清是纪律）。
    if (ssl_ != nullptr) {
        ::SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_ != nullptr) {
        ::SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

auto OpenSslBackend::push_incoming(std::span<const char> bytes) -> void {
    // `ssl_` 为假只可能发生在构造失败的路上，而构造失败不会交出后端；置真检查是防御，不是分支。
    if (bytes.empty() || ssl_ == nullptr) {
        return;
    }
    auto *incoming = ::SSL_get_rbio(ssl_);
    if (incoming == nullptr) {
        input_rejected_ = true;
        return;
    }
    auto remaining = bytes;
    while (!remaining.empty()) {
        const auto chunk = std::min(remaining.size(), AURORA_MAX_PUSH_BYTES);
        const auto written = ::BIO_write(incoming, remaining.data(), static_cast<int>(chunk));
        if (written <= 0) {
            // memory BIO 无容量上限，走到这里只有内存耗尽一种成因。绝不静默丢字节（见文件头）。
            input_rejected_ = true;
            return;
        }
        remaining = remaining.subspan(static_cast<std::size_t>(written));
    }
}

auto OpenSslBackend::drain_outgoing(std::vector<char> &outgoing) -> void {
    if (ssl_ == nullptr) {
        return;
    }
    auto *source = ::SSL_get_wbio(ssl_);
    if (source == nullptr) {
        return;
    }
    for (;;) {
        const auto count = ::BIO_read(source, scratch_.data(), static_cast<int>(scratch_.size()));
        if (count <= 0) {
            return;  // 空 BIO 以 retry 形态返回 -1：这就是「已排空」
        }
        // 切出「本次读到的那一段」用的是 span 视图而非指针加法：本仓内部缓冲区统一以 `std::span`
        // 传递，而 `count` 由 `BIO_read` 保证不超过 `scratch_` 的长度，切片即在其内。
        const auto slice = std::span<const char>{scratch_}.first(static_cast<std::size_t>(count));
        outgoing.insert(outgoing.end(), slice.begin(), slice.end());
    }
}

auto OpenSslBackend::verification_verdict() const -> std::optional<Error> {
    if (insecure_) {
        return std::nullopt;  // 危险开关：链、有效期、主机名的结论一概不取，诊断已在发起处留痕
    }
    // 校验器的结论是一条 `X509_V_ERR_*`：链建不起来、根不受信、名字不符、有效期已过——公共面把它们
    // 全归成一枚 `net-tls-cert-rejected`（「校验不过即断连」不区分是哪一项），但在 `detail` 里把
    // 校验器给的原文与目标主机名一起带出去：主机名不符时调用方要的是那个名字，而 OpenSSL 自己的
    // reason 串只有 "hostname mismatch"。
    const auto verdict = ::SSL_get_verify_result(ssl_);
    if (verdict == X509_V_OK) {
        return std::nullopt;
    }
    const auto *text = ::X509_verify_cert_error_string(verdict);
    auto detail = std::string{"peer certificate is not accepted for host '"} + host_ + "': ";
    detail += text != nullptr ? text : "unspecified verification error";
    return tls_error(ErrorCode::NetTlsCertRejected, std::move(detail));
}

auto OpenSslBackend::stall_reason(int ret, std::string_view phase) -> std::optional<Error> {
    const auto kind = ::SSL_get_error(ssl_, ret);
    switch (kind) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // 不是失败，是接缝的「还缺字节 / 还得外发」。`SSL_ERROR_WANT_WRITE` 在 memory BIO 上
            // 理论上不该出现（写 BIO 无上限），出现即说明上面漏了一处排空，故与 WANT_READ 同待：
            // 由调用方把已排出的字节发走并再催一次。
            return std::nullopt;
        case SSL_ERROR_ZERO_RETURN:
            peer_closed_ = true;  // 对端的 close_notify：此后不再会有明文字节
            return std::nullopt;
        default:
            break;
    }
    // 证书判定先于「握手失败」：同一刻两条都成立时（校验不过 → 本层中止握手），现场的真问题是信任，
    // 写成握手失败会让人去查对端的 TLS 实现。顺序理由同 Schannel 腿。
    if (const auto rejected = verification_verdict(); rejected) {
        return rejected;
    }
    // 没有 `net-tls-alert-received` 这一枚码可用（见 `codespec/ERROR_CATALOG.md` 的 net-tls 段），
    // 对端致命告警因此与「记录层解不开」同归 `net-tls-handshake-failed`，靠 `detail` 里的 OpenSSL
    // reason 区分（如 `peer did not return a certificate` / `bad record mac` / `unexpected message`）。
    auto detail = std::string{phase} + ", " + openssl_reason() + ", openssl error=" + std::to_string(kind);
    return tls_error(ErrorCode::NetTlsHandshakeFailed, std::move(detail));
}

auto OpenSslBackend::handshake(std::vector<char> &outgoing) -> Result<bool> {
    if (handshaked_) {
        return Result<bool>{true};
    }
    if (const auto broken = poisoned(); broken) {
        return *broken;
    }
    // 与 Schannel 腿不同，这里不需要「先攒够一条完整记录再喂」的判断：memory BIO 的 retry 形态
    // 就是 OpenSSL 自己的「还不够」，`SSL_connect` 据此回 WANT_READ，接缝照原样表达成 NeedMore。
    ::ERR_clear_error();
    const auto progressed = ::SSL_connect(ssl_);
    drain_outgoing(outgoing);
    if (progressed == 1) {
        if (const auto rejected = verification_verdict(); rejected) {
            return *rejected;
        }
        if (!insecure_ && ::SSL_get0_peer_certificate(ssl_) == nullptr) {
            return tls_error(ErrorCode::NetTlsCertRejected, "peer presents no certificate to verify");
        }
        capture_negotiated();
        handshaked_ = true;
        // TLS 1.3 的会话票据可能已随握手的最后几条记录一起被吞进会话缓存（OpenSSL 内部完成，
        // 不产出明文也不外发），故这里只回报「握手完成」，留在本层的明文字节由数据阶段交付。
        return Result<bool>{true};
    }
    if (const auto failure = stall_reason(progressed, "tls handshake"); failure) {
        return *failure;
    }
    return Result<bool>{false};
}

auto OpenSslBackend::capture_negotiated() -> void {
    // 引用计数由 `SSL_get1_peer_certificate` 加一次，摘要取完即还：证书不需要活过本函数，
    // 公共面上交付的是纯文本摘要（`PeerCertificate`），不留任何平台句柄形态。
    auto *peer = ::SSL_get1_peer_certificate(ssl_);
    if (peer != nullptr) {
        PeerCertificate info;
        // 名字与有效期都用 `X509_get_*` / `X509_get0_*` 这一族「返回内部指针、不加引用」的取值口：
        // 证书本身在本函数里存活，摘要又是在握手那一刻固化的纯文本，因此不需要第二份所有权。
        info.subject = name_text(::X509_get_subject_name(peer));
        info.issuer = name_text(::X509_get_issuer_name(peer));
        info.not_before = time_text(::X509_get0_notBefore(peer));
        info.not_after = time_text(::X509_get0_notAfter(peer));
        info.public_key_sha256 = spki_digest(peer);
        cert_ = std::move(info);
        ::X509_free(peer);
    }
    const unsigned char *selected = nullptr;
    unsigned int selected_size = 0;
    ::SSL_get0_alpn_selected(ssl_, &selected, &selected_size);
    if (selected != nullptr && selected_size > 0) {
        // 协商出的 ALPN 在 OpenSSL 侧是 `unsigned char *` 加长度，在公共面是 `std::string`；两个指针域
        // 之间没有 `static_cast` 可走，只能在 C API 边界上 reinterpret，故就这一行豁免该检查。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        alpn_ = std::string{reinterpret_cast<const char *>(selected), static_cast<std::size_t>(selected_size)};
    }
}

auto OpenSslBackend::next_records(std::vector<char> &plaintext, std::vector<char> &outgoing) -> Result<TlsDataState> {
    if (peer_closed_) {
        return Result<TlsDataState>{TlsDataState::PeerClosed};
    }
    if (const auto broken = poisoned(); broken) {
        return Result<TlsDataState>{*broken};
    }
    for (;;) {
        ::ERR_clear_error();
        const auto count = ::SSL_read(ssl_, scratch_.data(), static_cast<int>(scratch_.size()));
        // 排空在回报之前：TLS 1.2 的服务端发起重协商会在这一趟里产出应答字节，漏掉就把会话切半。
        drain_outgoing(outgoing);
        if (count > 0) {
            const auto slice = std::span<const char>{scratch_}.first(static_cast<std::size_t>(count));
            plaintext.insert(plaintext.end(), slice.begin(), slice.end());
            continue;  // 一次推进里把缓冲解到底：接缝承诺「解出所有完整应用记录」
        }
        const auto failure = stall_reason(count, "tls record");
        if (failure) {
            return Result<TlsDataState>{*failure};
        }
        if (peer_closed_) {
            return Result<TlsDataState>{TlsDataState::PeerClosed};
        }
        // 没有失败也没有字节：还缺一次网络交付。已解出的明文优先回报 Delivered（调用方会先把字节
        // 交付完再来催），本轮外发的握手字节走的是同一条通路——两者都非空时以明文的结局优先，
        // 因为 `outgoing` 由调用方无条件交付，而 `NeedMore` 会让它去登记一次新读。
        return Result<TlsDataState>{plaintext.empty() ? TlsDataState::NeedMore : TlsDataState::Delivered};
    }
}

auto OpenSslBackend::protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> {
    if (!handshaked_) {
        return tls_error(ErrorCode::NetTlsHandshakeFailed, "tls session is not established");
    }
    auto cursor = std::size_t{0};
    while (cursor < plaintext.size()) {
        // 本层按记录上限切块，而不是把整段交给 `SSL_write`：OpenSSL 自己会切成多条记录，但一次
        // 调用吞下任意长度会让「一处的记录级失败」与「整段没发出去」这两种现场无法区分。
        const auto chunk = std::min(plaintext.size() - cursor, AURORA_MAX_RECORD_BODY_BYTES);
        // 切片用 span 而非 `data() + cursor`：本仓内部缓冲区一律以 `std::span` 传递，`subspan` 自带
        // 「起点在范围内」的契约（循环条件保证），也不触发指针算术检查。
        const auto pending = plaintext.subspan(cursor);
        ::ERR_clear_error();
        const auto written = ::SSL_write(ssl_, pending.data(), static_cast<int>(chunk));
        drain_outgoing(outgoing);
        if (written <= 0) {
            const auto failure = stall_reason(written, "tls record wrap");
            if (failure) {
                return *failure;
            }
            // 走到这里即「重协商正在途中」：本轮记录封装不出来，而 `outgoing` 里已有握手应答。
            // 判成失败而不是成功：`net/tls.cpp` 会把本次写的完成计数报成明文字节数，静默返回即
            // 「报了已发却没发」。会话因此终止，与上层对一次失败写所做的事一致。
            return tls_error(ErrorCode::NetTlsHandshakeFailed,
                             "the session is renegotiating and cannot wrap records right now");
        }
        if (std::cmp_not_equal(written, chunk)) {
            // 默认档位（未开 `SSL_MODE_ENABLE_PARTIAL_WRITE`）下 `SSL_write` 只会整块接受；
            // 部分接受出现即本层的形态假设被打破，此时已排空的记录是真实写出的长度，照数报错。
            // `std::cmp_not_equal` 按值比较而不做隐式换号（`written` 是有符号的字节数、`chunk` 是
            // `size_t`），比把一侧硬转成另一侧的写法更难写错。
            return tls_error(ErrorCode::NetTlsHandshakeFailed, "the record layer accepted only part of the payload");
        }
        cursor += static_cast<std::size_t>(written);
    }
    return Result<void>{};
}

auto OpenSslBackend::shutdown_records(std::vector<char> &outgoing) -> Result<void> {
    if (!handshaked_ || ssl_ == nullptr) {
        return Result<void>{};  // 没有会话状态可关：空产出即「没发」而不是「失败」
    }
    // quiet shutdown：只发 close_notify，不等对端的回执。缺省形态下 `SSL_shutdown` 要先读到对端的
    // close_notify 才返回 1，而在 memory BIO 上那次读只会是 WANT_READ——一次「尽力而为」的关闭就会
    // 停在半截，还顺手把错误队列搅脏，让下一条诊断读到无关的 reason。
    ::SSL_set_quiet_shutdown(ssl_, 1);
    ::ERR_clear_error();
    (void)::SSL_shutdown(ssl_);
    drain_outgoing(outgoing);
    // 关闭通知的成败不可能有结论（对端已不再读），空产出与已产出都算尽力而为。
    return Result<void>{};
}

}  // namespace

auto make_tls_backend_openssl(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<TlsBackend>> {
    auto backend = OpenSslBackend::create(context, server_name);
    if (!backend) {
        return backend.error();
    }
    // 派生 → 基类的所有权移交必须写明一次：两级隐式转换（unique_ptr 与 Result 各一级）不被语言允许。
    return Result<std::unique_ptr<TlsBackend>>{std::unique_ptr<TlsBackend>{std::move(backend.value())}};
}

}  // namespace aurora::net::detail

#endif  // AURORA_ENABLE_NET_TLS_OPENSSL
