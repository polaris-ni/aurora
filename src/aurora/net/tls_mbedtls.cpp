// Aurora net — mbedTLS provider 后端。
//
// 本 TU 只在构建配置把 `AURORA_NET_TLS_PROVIDERS=<static|shared>:<prefix>` 指到一枚 mbedTLS 安装根时被编译
// （`cmake/AuroraNetTls.cmake` 由 `include/mbedtls/ssl.h` 的存在性推导宏，不接受手工设置），Linux 等「没有
// 缺省系统栈」的平台因此由它回答 `capabilities().tls`。
//
// 与 Schannel 腿（`net/tls_schannel.cpp`）的分工是**同一套接缝语义、两套平台事实**，故下面每一条差异都
// 落到接缝的同一处契约上，不新造第二种语义：
//   - **密文缓冲归本后端**：接缝是字节级的，本层用 mbedTLS 的 memory BIO 形态自持两条缓冲——`incoming_`
//     是上层 `push_incoming()` 交来的对端字节（bio_recv 按需取，取空即回 `WANT_READ`），`produced_` 是
//     bio_send 追加的待外发记录（每次接缝调用返回前倒进调用方的 `outgoing`）。两枚 bio 回调都不看 socket、
//     不等待、不部分交付，因此 mbedTLS 内部那些「阻塞直到有数据」的循环在这里一律以 WANT_* 立即返回，
//     会话状态机不会卡在本层（这是本腿存在的全部理由：mbedTLS 默认的 `net_sockets` bio 会阻塞）。
//   - **信任判定交回库内**（与 Schannel 相反）：`mbedtls_ssl_set_hostname()` 一次同时表达 SNI 与主机名
//     核对，`mbedtls_ssl_conf_ca_chain()` 的根列表天然是「系统根 ∪ 追加根」的合并形态，故不需要 Schannel
//     那套手工链引擎——那边做不到合并，这边不需要分开。校验结论仍按同一口径归 `net-tls-cert-rejected`。
//   - **本腿没有 OS 信任库**：mbedTLS 不读任何平台证书存储，也没有把「系统根」做成 API。缺省只读一枚
//     该平台的常规 CA bundle 文件（`AURORA_SYSTEM_TRUST_BUNDLES`）——读不到即**非致命**：追加根照样可用，只是没有
//     公有 CA 的缺省信任。把 `trusted_certificates` 做成「替换」是本层最不该犯的那种静默语义漂移。
//     另：本机实测 Debian 的这枚构建**开着** `MBEDTLS_FS_IO`（`mbedtls_x509_crt_parse[_path]()` 在导出面上），
//     本层却仍自己把 bundle 读进内存再交 `parse`——理由不是「库没有文件通路」，而是「provider 由调用方提供」：
//     换一枚重编译的 mbedTLS 就可能关掉它，而读文件是标准库的能力、不随引擎配置浮动（细节见 `read_text_file`）。
//     PEM 文本交 `parse` 时要求缓冲以 NUL 收尾且长度含它。
//   - **熵源走 PSA 而非 legacy trng**：3.6 把熵收进 PSA core，但 `mbedtls_ssl_conf_rng()` 仍要一枚 f_rng
//     回调。这里用 `psa_generate_random()` 适配（进程内一次性 `psa_crypto_init()`），从而**不链接**
//     `mbedtls_entropy`/`ctr_drbg` 那套读 `/dev/urandom` 的 legacy 通路。注意 PSA 的 `psa_generate_random`
//     无返回码，失败不可见——这也是选它而非 `mbedtls_psa_get_random()` 之外任何自研兜底的原因（有返回码
//     的等价物是 `psa_get_random` 内部那条，本腿不重复实现）。
//   - **TLS 1.0/1.1 在本腿不存在**：mbedTLS 3.x 只实现 1.2/1.3，`mbedtls_ssl_protocol_version` 没有更低
//     的枚举。下界落到 1.2，是「本引擎不支持更旧协议」而不是「无视调用方的下界」；上界低于 1.2 时允许集
//     为空，`make_tls_backend_mbedtls()` 直接以 `general-invalid-argument` 定论，比在握手里报一个含糊的 alert 诚实。
//   - **线程约定**：本机这枚构建开着 `MBEDTLS_THREADING_C`，但库里那把互斥只有应用侧用
//     `mbedtls_threading_set_alt()` 装了实现才真正生效，本层不装（那是全进程的一次性决定，越出单条接缝的
//     边界）。于是串行化完全落在接缝的既有口径上：一枚 `mbedtls_ssl_context` 只由 `net/tls.cpp` 的每连接
//     互斥驱动，本层的库结构与两条缓冲都按「一会话一对象」持有，跨会话不共享任何状态。
//   - **PKCS#12 客户端身份在本腿做不到，PEM 形态可以**：mbedTLS 3.6.5 的 `pkcs12.h` 只导出三枚 PBE/密钥
//     派生原语（`mbedtls_pkcs12_pbe`、`mbedtls_pkcs12_pbe_ext`、`mbedtls_pkcs12_derivation`——实测即
//     `libmbedcrypto.a` 的全部 PKCS#12 导出面），既没有 PFX 容器的读侧也没有写侧，而
//     `TlsContext::client_identity` 的公共口径是 PKCS#12 **字节**。自己补一套 AuthSafe/SafeBag 的 ASN.1
//     解析（含 MAC 校验）不在本切片范围内，故给了 PFX 就明确失败（`general-invalid-argument` + 指明缺哪项
//     能力），而不是静默忽略——静默忽略会让调用方以为 mTLS 已生效，而现场只是对端拒绝了握手。
//     同一份身份的 PEM 形态（`client_certificate_pem` + `client_private_key_pem`）走的是库里现成的
//     `x509_crt_parse` / `pk_parse_key` 通路，本腿完整支持；两条形态在公共面上互斥，见 `net/tls.h`。
//
// TLS 1.3 的后握手 NewSessionTicket 由库内部处理：3.6 缺省**关闭**信号形态
// （`mbedtls_ssl_conf_tls13_enable_signal_new_session_tickets()` 未开），票据记录在 `mbedtls_ssl_read()`
// 里被就地吞掉，不会伪装成应用数据、也不会让读停在 `WANT_READ` 上；本层仍把该错误码当作「继续」处理，
// 以免将来配置改变时这里变成静默丢字节。
//
// 危险开关（`allow_insecure_certificate`）落点是 `MBEDTLS_SSL_VERIFY_NONE`（链与主机名一并交给库跳过），
// warning 诊断与 pin 判定都在 `net/tls.cpp`，本层不重复。

#ifdef AURORA_ENABLE_NET_TLS_MBEDTLS

#include "aurora/net/tls.h"

#include <mbedtls/base64.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/detail/tls_backend.h"

namespace aurora::net::detail {
namespace {

constexpr std::size_t AURORA_MAX_RECORD_BODY_BYTES = 16384;  ///< 一条应用记录的最大正文（协议上限）
constexpr std::size_t AURORA_DN_TEXT_BYTES = 512;  ///< 可分辨名文本缓冲（远超真实 DN 长度）
constexpr std::size_t AURORA_SPKI_DER_BYTES = 2048;  ///< SPKI DER 缓冲（RSA-4096 的 SPKI 约 560 字节）
constexpr std::size_t AURORA_VERIFY_INFO_BYTES = 512;  ///< 校验标志位的可读文本
constexpr std::size_t AURORA_STRERROR_BYTES = 100;  ///< `mbedtls_strerror` 建议的最小缓冲
constexpr int AURORA_CONTROL_ONLY_ROUNDS = 16;  ///< 一轮读里「只出控制消息不出正文」的上限

/// 本平台的常规系统 CA bundle（按发行版惯例排序；读到第一枚即停）。
///
/// 为什么是文件而不是「系统信任库」：mbedTLS 不带 OS 存储读取，这是该引擎的固有缺口，提案 §5 的
/// provider 行按「诚实降级」处理——读不到就只用追加根，并把观察结果留在文档与报告里，而不是假装
/// 本腿认公有 CA。
constexpr std::array<const char *, 3> AURORA_SYSTEM_TRUST_BUNDLES = {
    "/etc/ssl/certs/ca-certificates.crt",  ///< Debian / Ubuntu（ca-certificates 包的合并 bundle）
    "/etc/pki/tls/certs/ca-bundle.crt",  ///< Fedora / RHEL 系
    "/etc/ssl/cert.pem",  ///< 其它 BSD 派生的单文件形态
};

/// @brief 客户端身份那几处失败的构造口。
///
/// 它们走「调用方覆盖 message」那一枚 `make_error` 重载而不是上面的 `tls_error`：后者把文本交给表模板的
/// `{detail}` 槽，而 `general-invalid-argument` 的模板没有那一槽（`codespec/errors.toml` 里它渲染成一句
/// 「Invalid argument supplied」），于是「PEM 的哪一步、缺什么」会在现场被整段吃掉——本节目的全部是替调用方
/// 指认他给错了哪一份材料，指认丢了就等于没指认。同文件其余各码（`net-tls-*` / `net-platform-unavailable`）
/// 的模板带槽，仍走 `tls_error`。
auto identity_error(const std::string &text) -> Error { return make_error(ErrorCode::GeneralInvalidArgument, text); }

auto tls_error(ErrorCode code, std::string text) -> Error {
    return make_error(code, ErrorParams{{"detail", std::move(text)}});
}

auto hex_code(int code) -> std::string {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%04x", static_cast<unsigned>(code));
    return {buffer};
}

/// mbedTLS 错误码 → 一行可诊断文本（库内 strerror 的说法比裸数字有用：同一枚 -0x6E00 背后是
/// 「对端回了 handshake_failure」还是「本端密码套件为空」，现场处置完全相反）。
auto mbedtls_text(int code) -> std::string {
    char buffer[AURORA_STRERROR_BYTES] = {};
    ::mbedtls_strerror(code, buffer, sizeof(buffer));
    auto text = std::string{buffer};
    return text.empty() ? std::string{"no description"} : text;
}

/// PSA 熵源的一次性装配：3.6 的 PSA core 要求先 `psa_crypto_init()`，之后 `psa_generate_random()` 才可调用。
///
/// 函数内静态量让它在整个进程里只跑一次（C++11 起线程安全初始化），因此多个会话并发建后端也不需要
/// 本层另加锁；`psa_crypto_init()` 本身幂等。
auto psa_entropy_ready() -> bool {
    static const bool INITIALIZED = [] { return ::psa_crypto_init() == PSA_SUCCESS; }();
    return INITIALIZED;
}

/// `mbedtls_ssl_conf_rng()` 的适配：随机字节直接取 PSA，不引入 legacy entropy/ctr_drbg 通路。
///
/// `psa_generate_random()` 是无返回码的形态（失败在 PSA 侧不可见），故本回调恒返回 0：这不是「假装成功」，
/// 而是 mbedTLS 与 PSA 之间唯一被官方支持的接缝（`mbedtls_psa_get_random()` 做的正是同一件事，但它在
/// `psa_util.h` 里没有被 `extern "C"` 包住，C++ 侧取它的地址会拿到限定名而链接失败）。
auto psa_random(void *context, unsigned char *output, std::size_t length) -> int {
    (void)context;
    if (length != 0) {
        ::psa_generate_random(output, length);
    }
    return 0;
}

/// SHA-256（SPKI pin 摘要）：走 mbedTLS 的 MD 层，而不是 PSA 的哈希算子——本层只需要一次性摘要。
auto sha256(std::span<const std::uint8_t> bytes) -> std::vector<std::uint8_t> {
    const auto *info = ::mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr) {
        return {};
    }
    const auto size = ::mbedtls_md_get_size(info);
    if (size == 0) {
        return {};
    }
    auto digest = std::vector<std::uint8_t>(size, std::uint8_t{0});
    if (::mbedtls_md(info, bytes.data(), bytes.size(), digest.data()) != 0) {
        digest.clear();
    }
    return digest;
}

/// 标准 base64（无换行）：pin 集合的跨腿可比形态，与 Schannel 腿的 `CRYPT_STRING_BASE64|NOCRLF` 同形。
auto to_base64(std::span<const std::uint8_t> bytes) -> std::string {
    // 一次算足容量（4*ceil(n/3) 再加库写入的结尾 NUL），省掉「先问长度」那一步：base64 的长度是纯算术。
    const auto whole_groups = (bytes.size() / 3) + 1;  // 恒上取整（余数并给最后一段），多出的空间无害
    const auto capacity = (whole_groups * 4) + 1;
    auto text = std::string(capacity, '\0');
    auto written = std::size_t{0};
    auto *const out = reinterpret_cast<unsigned char *>(
        text.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) mbedTLS C API 收 `unsigned char *`
    if (::mbedtls_base64_encode(out, capacity, &written, bytes.data(), bytes.size()) != 0) {
        return {};
    }
    text.resize(written);  // `olen` 不含写入的 NUL
    return text;
}

/// 证书有效期 → ISO-8601（UTC，秒级）：公共面上是可比对的文本，不是平台结构（与 Schannel 腿同形）。
///
/// 不用 `mbedtls_x509_btimestr`：它的形态是 `YYYY-MM-DD HH:MM:SS`（空格分隔、无 `Z`），与接缝约定的
/// 20 字符 ISO-8601 不同形；`mbedtls_x509_time` 的六个整数字段本身就是一年里的时刻，直接格式化最稳。
auto to_iso8601(const mbedtls_x509_time &time) -> std::string {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", time.year, time.mon, time.day, time.hour,
                  time.min, time.sec);
    return {buffer};
}

/// 可分辨名 → 文本：mbedTLS 按 DER 次序输出 `C=US, O=..., CN=...`，与 Schannel 腿的
/// `CertNameToStr(CERT_X500_NAME_STR)` 实测逐字相同（本腿因此在 DN 文本上与其余腿天然同形，不必自己
/// 渲染一遍）；分隔与空格属库内固定形态，改不了也不需要改。
auto distinguished_name(const mbedtls_x509_name &name) -> std::string {
    auto buffer = std::vector<char>(AURORA_DN_TEXT_BYTES, '\0');
    const auto length = ::mbedtls_x509_dn_gets(buffer.data(), buffer.size(), &name);
    if (length <= 0) {
        return {};
    }
    return {buffer.data(), static_cast<std::size_t>(length)};
}

/// 校验标志位 → 文本（`MBEDTLS_X509_BADCERT_EXPIRED` 之类的说法直接进诊断，省掉一次「现场再解一遍」）。
auto verify_flags_text(std::uint32_t flags) -> std::string {
    auto buffer = std::vector<char>(AURORA_VERIFY_INFO_BYTES, '\0');
    const auto length = ::mbedtls_x509_crt_verify_info(buffer.data(), buffer.size(), " ", flags);
    if (length <= 0) {
        return {};
    }
    auto text = std::string{buffer.data(), static_cast<std::size_t>(length)};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

/// SPKI 摘要：`base64(SHA-256(SubjectPublicKeyInfo DER))`，与 Schannel 腿逐字节同形（pin 跨腿一致的前提）。
///
/// 首选 `pk_raw`：那是解析证书时就留下的 SPKI DER 原文，与证书里那一段逐字节相同，不必再「重新编码一遍」
/// （本机实测：与 `openssl x509 -pubkey | openssl pkey -pubin -outform der | dgst -sha256 -binary` 的
/// 摘要完全一致，而 Schannel 腿走的正是同一份 DER 的编码形态）。
/// 回落到 `mbedtls_pk_write_pubkey_der`：该函数从缓冲**末尾**向前写并返回长度，故有效区间是
/// `[size-length, size)`；反过来按 `[0, length)` 取会拿到一截全零前缀，摘要于是与任何其它腿都不相等
/// （这类错不会崩，只会让 pin 永远不命中，正是最难查的那一种）。
auto spki_sha256(const mbedtls_x509_crt &cert) -> std::string {
    if (cert.pk_raw.p != nullptr && cert.pk_raw.len != 0) {
        return to_base64(sha256(std::span<const std::uint8_t>{cert.pk_raw.p, cert.pk_raw.len}));
    }
    auto buffer = std::vector<unsigned char>(AURORA_SPKI_DER_BYTES, std::uint8_t{0});
    const auto length = ::mbedtls_pk_write_pubkey_der(&cert.pk, buffer.data(), buffer.size());
    if (length <= 0 || static_cast<std::size_t>(length) > buffer.size()) {
        return {};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) DER 缓冲尾部回溯一段裸指针
    const auto *first = buffer.data() + buffer.size() - static_cast<std::size_t>(length);
    return to_base64(sha256(std::span<const std::uint8_t>{first, static_cast<std::size_t>(length)}));
}

/// 读一枚 bundle 文件的全文（信任根的唯一取回通路）。
///
/// 为什么不用 `mbedtls_x509_crt_parse(path)` 的「传文件名」形态，哪怕本机这枚构建开着 `MBEDTLS_FS_IO`：
/// provider 引擎由调用方提供，换一枚重编译的就可能关掉它，而读文件是标准库能力、不随引擎配置浮动；自己读
/// 还顺带把「文件不存在」与「内容不是 PEM」两种失败分开——前者非致命（本腿就是没有系统根），后者才需要说清
/// 是哪一枚 bundle 出的问题。追加根本来就是以内存文本进来的，两条路共用同一枚 `parse`，少一条分支。
/// @param path bundle 路径。
/// @return 文本内容；读不到为空串。
auto read_text_file(std::string_view path) -> std::string {
    auto stream = std::ifstream{std::string{path}, std::ios::binary};
    if (!stream) {
        return {};
    }
    auto collector = std::ostringstream{};
    collector << stream.rdbuf();
    return collector.str();
}

/// 版本档位 → mbedTLS 的版本枚举（本引擎只有 1.2/1.3，低于 1.2 的档位没有对应枚举）。
/// @param floor 下界档位（0=1.0 … 3=1.3）。
/// @param ceiling 上界档位（未置上界时传 3）。
/// @return 成功携「下界枚举 + 上界枚举」；允许集为空携原因文本。
auto version_bounds(int floor, int ceiling)
    -> Result<std::pair<mbedtls_ssl_protocol_version, mbedtls_ssl_protocol_version>> {
    if (ceiling < 2 || floor > ceiling) {
        return tls_error(ErrorCode::GeneralInvalidArgument,
                         "mbedTLS speaks TLS 1.2 and TLS 1.3 only: the requested version bounds leave no "
                         "acceptable protocol version");
    }
    const auto low = floor >= 3 ? MBEDTLS_SSL_VERSION_TLS1_3 : MBEDTLS_SSL_VERSION_TLS1_2;
    const auto high = ceiling >= 3 ? MBEDTLS_SSL_VERSION_TLS1_3 : MBEDTLS_SSL_VERSION_TLS1_2;
    return std::pair<mbedtls_ssl_protocol_version, mbedtls_ssl_protocol_version>{low, high};
}

/// 一条 TLS 会话的 mbedTLS 实现（线程约定见 `detail/tls_backend.h`：调用方串行化，本类不自持锁）。
class MbedTlsBackend final : public TlsBackend {
  public:
    [[nodiscard]] static auto create(const TlsContext &context, std::string_view server_name)
        -> Result<std::unique_ptr<MbedTlsBackend>>;

    MbedTlsBackend(const MbedTlsBackend &) = delete;
    auto operator=(const MbedTlsBackend &) -> MbedTlsBackend & = delete;
    MbedTlsBackend(MbedTlsBackend &&) = delete;
    auto operator=(MbedTlsBackend &&) -> MbedTlsBackend & = delete;

    ~MbedTlsBackend() override { release(); }

    auto push_incoming(std::span<const char> bytes) -> void override {
        incoming_.insert(incoming_.end(), bytes.begin(), bytes.end());
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
    MbedTlsBackend() = default;

    /// memory BIO 的发送侧：只追加、不交付、永不部分写，因此 mbedTLS 眼里「socket 总是立即可写」。
    static auto bio_send(void *context, const unsigned char *data, std::size_t length) -> int {
        auto *self = static_cast<MbedTlsBackend *>(context);
        self->produced_.insert(
            self->produced_.end(), data,
            data + length);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) memory BIO 发送侧：裸指针区间
        return static_cast<int>(length);
    }

    /// memory BIO 的接收侧：缓冲空即 `WANT_READ`（这就是「本层不阻塞」的落点）。
    static auto bio_recv(void *context, unsigned char *data, std::size_t length) -> int {
        auto *self = static_cast<MbedTlsBackend *>(context);
        const auto available = self->incoming_.size() - self->incoming_read_;
        if (available == 0) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        const auto taken = length < available ? length : available;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) memory BIO 接收侧：从已读偏移取一段裸指针
        const auto *first = self->incoming_.data() + static_cast<std::ptrdiff_t>(self->incoming_read_);
        std::copy_n(first, static_cast<std::ptrdiff_t>(taken), data);
        self->incoming_read_ += taken;
        if (self->incoming_read_ == self->incoming_.size()) {
            // 消费到底即整段丢弃：留着一段永不复用的前缀会让每条记录的搬运都从 0 开始。
            self->incoming_.clear();
            self->incoming_read_ = 0;
        }
        return static_cast<int>(taken);
    }

    /// 把 bio_send 攒下的记录交给调用方（追加语义：本层不保留已外发的字节）。
    auto take_produced(std::vector<char> &outgoing) -> void {
        outgoing.insert(outgoing.end(), produced_.begin(), produced_.end());
        produced_.clear();
    }

    /// 装载信任根：常规 bundle（可有可无）+ `trusted_certificates` 的追加根（必须有意义）。
    /// @return 成功；追加根本身解不开即 `general-invalid-argument`。
    [[nodiscard]] auto load_trust_roots(const TlsContext &context) -> Result<void>;

    /// 把 PEM 文本交进任何 `mbedtls_*_parse` 前的统一形态：长度含结尾 NUL，分块才认得出结束。
    [[nodiscard]] static auto nul_terminated(std::string_view text) -> std::string {
        std::string buffer{text};
        buffer.push_back('\0');
        return buffer;
    }

    /// 装载客户端身份（PEM 形态）：证书链 + 私钥，并当场核对两者配不是一对。
    ///
    /// 装在 `config_` 之前完成的指针登记（`conf_own_cert` 只存指针、不复制正文），故这两枚库结构由本对象
    /// 持有到连接结束；配错一对却在握手半途以 alert 收场，比在这里说一句「钥匙不是这张证书的」难查得多。
    /// @return 成功；文本解不开、或私钥与叶证书不匹配即 `general-invalid-argument`。
    [[nodiscard]] auto install_client_identity(const TlsContext &context) -> Result<void>;

    /// 握手完成那一刻的取证：对端证书摘要与协商出的 ALPN。
    ///
    /// 必须在 `mbedtls_ssl_handshake()` 返回 0 之后、任何后续 SSL 调用之前读：`mbedtls_ssl_get_peer_cert()`
    /// 交回的指针归会话所有，重协商即换（库头明示「不得跨 SSL API 调用使用」），而接缝要求摘要在握手
    /// 那一刻固化。
    auto capture_negotiated() -> void;

    /// 校验结论 → 拒绝原因：标志位文本与目标主机名一并带上。
    ///
    /// 主机名恒在文本里，是因为 mbedTLS 把「链不可信」与「名字不符」都收敛成同一枚
    /// `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`；现场需要的恰好是「它到底在拒哪一半」。
    [[nodiscard]] auto certificate_rejected(std::uint32_t flags) -> Error;

    /// 释放库内结构：与构造反向，`ssl` 先于 `conf`（它持 conf 的指针），`conf` 先于客户端身份材料
    /// （`conf_own_cert` 登记的正是那两枚指针），身份材料先于根链（各自独立，按装配次序倒放）。
    void release() noexcept {
        if (ssl_ready_) {
            ::mbedtls_ssl_free(&ssl_);
            ssl_ready_ = false;
        }
        if (config_ready_) {
            ::mbedtls_ssl_config_free(&config_);
            config_ready_ = false;
        }
        if (client_key_ready_) {
            ::mbedtls_pk_free(&client_key_);
            client_key_ready_ = false;
        }
        if (client_chain_ready_) {
            ::mbedtls_x509_crt_free(&client_chain_);
            client_chain_ready_ = false;
        }
        if (roots_ready_) {
            ::mbedtls_x509_crt_free(&roots_);
            roots_ready_ = false;
        }
    }

    mbedtls_ssl_config config_{};  ///< 会话级 TLS 配置（版本界、authmode、ALPN、信任根都装在这里）
    mbedtls_ssl_context ssl_{};  ///< 状态机本体（bio 已绑到本对象）
    mbedtls_x509_crt roots_{};  ///< 信任根链：系统 bundle ∪ 追加根，必须活过 config_/ssl_
    mbedtls_x509_crt client_chain_{};  ///< 客户端证书链（PEM 装进来的那份；`conf_own_cert` 只登记指针）
    mbedtls_pk_context client_key_{};  ///< 客户端私钥（同上，必须活过 config_/ssl_）
    bool config_ready_ = false;  ///< config 是否已 init（决定 release 与失败路径）
    bool ssl_ready_ = false;  ///< ssl 是否已 init
    bool roots_ready_ = false;  ///< 根链是否已 init
    bool client_chain_ready_ = false;  ///< 客户端证书链是否已 init
    bool client_key_ready_ = false;  ///< 客户端私钥是否已 init
    bool handshaked_ = false;  ///< 握手是否完成
    bool peer_closed_ = false;  ///< 已收到 close_notify（或读端被就地关闭）
    bool insecure_ = false;  ///< 危险开关：置真即校验整体交给 VERIFY_NONE 跳过
    std::vector<std::string> alpn_ids_;  ///< ALPN 标识正文（库不复制，须活到连接结束）
    std::vector<const char *> alpn_list_;  ///< 以 nullptr 结尾的指针数组（`conf_alpn_protocols` 的入参形态）
    std::vector<char> incoming_;  ///< 上层交来的对端字节
    std::size_t incoming_read_ = 0;  ///< `incoming_` 的消费游标
    std::vector<char> produced_;  ///< bio_send 攒下的待外发记录
    std::optional<PeerCertificate> cert_;  ///< 对端证书摘要
    std::optional<std::string> alpn_;  ///< 协商出的 ALPN
    std::string host_;  ///< SNI 与主机名校验目标
};

auto MbedTlsBackend::certificate_rejected(std::uint32_t flags) -> Error {
    auto text = std::string{"certificate is not accepted for '"} + host_ + "'";
    const auto flags_text = verify_flags_text(flags);
    if (!flags_text.empty()) {
        text += ": " + flags_text;
    }
    return tls_error(ErrorCode::NetTlsCertRejected, text);
}

auto MbedTlsBackend::load_trust_roots(const TlsContext &context) -> Result<void> {
    ::mbedtls_x509_crt_init(&roots_);
    roots_ready_ = true;

    for (const auto *path : AURORA_SYSTEM_TRUST_BUNDLES) {
        const auto bundle = read_text_file(path);
        if (bundle.empty()) {
            continue;  // 本发行版没有这个位置：换下一枚候选，不是错误
        }
        auto buffer = std::string{bundle};
        buffer.push_back('\0');  // mbedTLS 的 PEM 通路要求以 NUL 收尾的缓冲
        const auto *const bundle_bytes = reinterpret_cast<const unsigned char *>(
            buffer.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) mbedTLS 收 `const unsigned char *`
        const auto failures = ::mbedtls_x509_crt_parse(&roots_, bundle_bytes, buffer.size());
        if (failures < 0) {
            // 整枚 bundle 读不通（格式不认识）：这不是致命，但「没有系统根」得说清是哪枚文件出的场。
            return tls_error(ErrorCode::GeneralInvalidArgument, std::string{"the system trust bundle at "} + path +
                                                                    " is not parsable: " + mbedtls_text(failures));
        }
        // failures > 0 是「其中若干张解不开」：bundle 是发行版拼出来的，个别条目不合本引擎的口味不该断连。
        break;
    }

    for (const auto &pem : context.trusted_certificates) {
        if (pem.empty()) {
            return tls_error(ErrorCode::GeneralInvalidArgument, "trusted certificate holds no PEM X.509 block");
        }
        auto buffer = std::string{pem};
        buffer.push_back('\0');  // 同上：长度含结尾 NUL，PEM 分块才认得出结束
        const auto *const pem_bytes = reinterpret_cast<const unsigned char *>(
            buffer.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) mbedTLS 收 `const unsigned char *`
        const auto failures = ::mbedtls_x509_crt_parse(&roots_, pem_bytes, buffer.size());
        if (failures < 0) {
            return tls_error(
                ErrorCode::GeneralInvalidArgument,
                std::string{"trusted certificate is not a decodable X.509 certificate: "} + mbedtls_text(failures));
        }
        if (failures > 0) {
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "trusted certificate holds " + std::to_string(failures) +
                                 " block(s) that are not decodable X.509 certificates");
        }
    }

    ::mbedtls_ssl_conf_ca_chain(&config_, &roots_, nullptr);
    return Result<void>{};
}

auto MbedTlsBackend::install_client_identity(const TlsContext &context) -> Result<void> {
    if (!context.client_certificate_pem.has_value() || !context.client_private_key_pem.has_value()) {
        return identity_error("client identity is incomplete: certificate or private key PEM is missing");
    }
    const std::string chain = nul_terminated(*context.client_certificate_pem);
    const std::string key = nul_terminated(*context.client_private_key_pem);
    const auto *const chain_bytes = reinterpret_cast<const unsigned char *>(
        chain.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) mbedTLS 收 `const unsigned char *`
    const auto *const key_bytes = reinterpret_cast<const unsigned char *>(
        key.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) mbedTLS 收 `const unsigned char *`

    ::mbedtls_x509_crt_init(&client_chain_);
    client_chain_ready_ = true;
    const auto parsed_chain = ::mbedtls_x509_crt_parse(&client_chain_, chain_bytes, chain.size());
    // 「链里到底有没有一张可用证书」是一条事实，两型证据合在一处判：负码说整段不是可解码的 X.509，
    // 而 `raw.p` 为空说结构里根本没填进证书。判据取 `raw.p` 而非 `next`：对着刚 `init` 的空结构，第一张证书是
    // **就地填进结构体本身**的，`next` 从第二张起才挂——实测 mbedTLS 3.6.5 里单张链解析成功时 `raw.p` 非空而
    // `next` 恒为 null，拿 `next` 判空会把一份合法身份读成空链（本轮就是这么栽的）。合成一支而不是两支的另一个
    // 理由是变异自证：拆成两支时，摘掉负码那一半后空链那一半仍交出同一句诊断，用例照绿、两处判据谁也没被证明在
    // 起作用（实测即如此），合起来才是「摘掉就没得说」的那一处。
    if (parsed_chain < 0 || client_chain_.raw.p == nullptr) {
        auto text = std::string{"the client certificate chain holds no usable X.509 certificate"};
        if (parsed_chain < 0) {
            text += ": " + mbedtls_text(parsed_chain);  // 库的原话只对负码那一型有意义，空链那型没有码可引
        }
        return identity_error(std::move(text));
    }
    if (parsed_chain > 0) {
        // 与追加根同一口径：调用方自己给的材料没有「个别条目不合口味」这一说，半条链送出去只会在对端换来
        // 一次无法解释的拒绝。
        return identity_error("the client certificate chain holds " + std::to_string(parsed_chain) +
                              " block(s) that are not decodable X.509 certificates");
    }

    ::mbedtls_pk_init(&client_key_);
    client_key_ready_ = true;
    // 口令位给空：加密过的 PEM 私钥（`Proc-Type: 4,ENCRYPTED`）本层不解，那需要一个口令旋钮，而它一出现
    // 就会与 PKCS#12 那枚 `client_identity_passphrase` 长成第二个「以谁为准」的冲突。
    const auto parsed_key =
        ::mbedtls_pk_parse_key(&client_key_, key_bytes, key.size(), nullptr, 0, &psa_random, nullptr);
    if (parsed_key != 0) {
        return identity_error("the client private key is not a readable unencrypted PEM key: " +
                              mbedtls_text(parsed_key));
    }
    // 配对当场判定：链首那张就是叶证书（公共面把「叶在前」写成入参契约），它的公钥与这枚私钥对不上，
    // 握手里就是一句来路不明的 alert。证书公钥住在 `pk` 字段（3.x 的 `mbedtls_x509_crt` 没有 `key`，
    // 而 `pk_raw` 是它的 DER 原文），由 `mbedtls_x509_crt_parse_der` 在解析证书时一并填好。
    const auto pair = ::mbedtls_pk_check_pair(&client_chain_.pk, &client_key_, &psa_random, nullptr);
    if (pair != 0) {
        return identity_error("the client private key does not match the client certificate: " + mbedtls_text(pair));
    }
    const auto installed = ::mbedtls_ssl_conf_own_cert(&config_, &client_chain_, &client_key_);
    if (installed != 0) {
        return identity_error("the client identity could not be installed on this session: " + mbedtls_text(installed));
    }
    return Result<void>{};
}

auto MbedTlsBackend::create(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<MbedTlsBackend>> {
    // mbedTLS 3.6.5 没有 PFX 容器的解析入口（`pkcs12.h` 只给 PBE/密钥派生原语），而公共口径给的就是
    // PKCS#12 字节：本腿做不到，就给一个说明缺口的失败，而不是收下字段后静默不带客户端证书。同一份身份的
    // PEM 形态在下面 `install_client_identity` 里是支持的（互斥口径由 `net/tls.cpp` 的起手闸保证）。
    if (context.client_identity) {
        return identity_error(
            "this build's mbedTLS engine has no PKCS#12 reader, so a client identity cannot be "
            "installed on a TLS session; use client_certificate_pem / client_private_key_pem instead");
    }

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
    const auto bounds = version_bounds(rank(context.min_version), context.max_version ? rank(*context.max_version) : 3);
    if (!bounds) {
        return bounds.error();
    }

    std::unique_ptr<MbedTlsBackend> backend{new MbedTlsBackend()};
    backend->host_ = std::string{server_name};
    backend->insecure_ = context.allow_insecure_certificate;

    if (!psa_entropy_ready()) {
        return tls_error(ErrorCode::NetTlsEngineUnavailable, "the PSA entropy pool could not be initialized");
    }

    ::mbedtls_ssl_config_init(&backend->config_);
    backend->config_ready_ = true;
    // 缺省项一次装齐（含密码套件集、超时、协议预设）：本层只在其上覆盖接缝要表达的口径。
    if (const auto defaults = ::mbedtls_ssl_config_defaults(&backend->config_, MBEDTLS_SSL_IS_CLIENT,
                                                            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        defaults != 0) {
        return tls_error(ErrorCode::NetTlsEngineUnavailable,
                         "tls defaults could not be installed: " + mbedtls_text(defaults));
    }
    ::mbedtls_ssl_conf_rng(&backend->config_, &psa_random, nullptr);
    ::mbedtls_ssl_conf_min_tls_version(&backend->config_, bounds.value().first);
    ::mbedtls_ssl_conf_max_tls_version(&backend->config_, bounds.value().second);

    if (auto roots = backend->load_trust_roots(context); !roots) {
        return roots.error();
    }
    // 危险开关的落点：VERIFY_NONE 让库把链与主机名一并跳过（诊断与 pin 判定在 `net/tls.cpp`）。
    ::mbedtls_ssl_conf_authmode(&backend->config_,
                                backend->insecure_ ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);

    // 客户端身份装在配置上（`conf_own_cert` 只登记指针，正文由本对象持有），故排在信任根之后、`ssl_setup`
    // 之前：与 ALPN 同一类「配置期材料」，都要活过整条连接。
    if (context.client_certificate_pem) {
        if (auto identity = backend->install_client_identity(context); !identity) {
            return identity.error();
        }
    }

    // ALPN：库只登记指针数组且不复制正文，故正文与指针都由本对象持有到连接结束。
    // 单条 255 字节是协议上限（长度前缀是 u8），空标识是非法输入而不是「未宣告」，两者都在这里剔掉，
    // 否则整枚列表被库判为 BAD_INPUT_DATA、连不宣告 ALPN 的连接也一起失败。
    for (const auto &protocol : context.alpn_protocols) {
        if (protocol.empty() || protocol.size() > 255) {
            continue;
        }
        backend->alpn_ids_.push_back(protocol);
    }
    if (!backend->alpn_ids_.empty()) {
        for (const auto &protocol : backend->alpn_ids_) {
            backend->alpn_list_.push_back(protocol.c_str());
        }
        backend->alpn_list_.push_back(nullptr);
        if (const auto offered = ::mbedtls_ssl_conf_alpn_protocols(&backend->config_, backend->alpn_list_.data());
            offered != 0) {
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "the ALPN advertisement is not acceptable: " + mbedtls_text(offered));
        }
    }

    ::mbedtls_ssl_init(&backend->ssl_);
    backend->ssl_ready_ = true;
    if (const auto setup = ::mbedtls_ssl_setup(&backend->ssl_, &backend->config_); setup != 0) {
        return tls_error(ErrorCode::NetTlsEngineUnavailable, "tls context could not be set up: " + mbedtls_text(setup));
    }
    if (!backend->host_.empty()) {
        // 一枚调用同时给出 SNI 与主机名核对目标：这正是本切片把校验做成默认开启的那一半（库头明示
        // 「不设 hostname 即不认证对端身份」）。名字过长时库回 BAD_INPUT_DATA，此处按配置不合用定论。
        if (const auto named = ::mbedtls_ssl_set_hostname(&backend->ssl_, backend->host_.c_str()); named != 0) {
            return tls_error(ErrorCode::GeneralInvalidArgument,
                             "the server name cannot be used for SNI and verification: " + mbedtls_text(named));
        }
    }
    ::mbedtls_ssl_set_bio(&backend->ssl_, backend.get(), &MbedTlsBackend::bio_send, &MbedTlsBackend::bio_recv, nullptr);
    return backend;
}

auto MbedTlsBackend::capture_negotiated() -> void {
    const auto *peer = ::mbedtls_ssl_get_peer_cert(&ssl_);
    if (peer != nullptr) {
        PeerCertificate info;
        info.subject = distinguished_name(peer->subject);
        info.issuer = distinguished_name(peer->issuer);
        info.not_before = to_iso8601(peer->valid_from);
        info.not_after = to_iso8601(peer->valid_to);
        info.public_key_sha256 = spki_sha256(*peer);
        cert_ = std::move(info);
    }
    const auto *protocol = ::mbedtls_ssl_get_alpn_protocol(&ssl_);
    if (protocol != nullptr && *protocol != '\0') {
        alpn_ = std::string{protocol};
    }
}

auto MbedTlsBackend::handshake(std::vector<char> &outgoing) -> Result<bool> {
    if (handshaked_) {
        return Result<bool>{true};
    }
    // 一次调用把库推进到「要么完成、要么明确要更多字节」：bio 是内存型的，库内部的循环不会停在那里等
    // socket，因此这里不需要自己按记录边界分帧（与 Schannel 腿相反，那一边必须由本层切完整记录）。
    const auto outcome = ::mbedtls_ssl_handshake(&ssl_);
    take_produced(outgoing);
    if (outcome == 0) {
        // VERIFY_REQUIRED 让库在握手内完成链与主机名判定；VERIFY_NONE 时结论本来就无用（危险开关）。
        const auto flags = ::mbedtls_ssl_get_verify_result(&ssl_);
        if (!insecure_ && flags != 0) {
            return certificate_rejected(flags);
        }
        capture_negotiated();
        handshaked_ = true;
        return Result<bool>{true};
    }
    if (outcome == MBEDTLS_ERR_SSL_WANT_READ || outcome == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return Result<bool>{false};
    }
    if (outcome == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
        // 握手期间收到票据（只在信号形态被显式开启时出现）：不是失败，等上层再催一轮。
        return Result<bool>{false};
    }
    if (outcome == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        // 库在 REQUIRED 下校验不过即中止握手；标志位已写进会话，据此给出「拒的是哪一半」。
        return certificate_rejected(::mbedtls_ssl_get_verify_result(&ssl_));
    }
    return tls_error(ErrorCode::NetTlsHandshakeFailed,
                     "tls handshake failed, code=" + hex_code(outcome) + " (" + mbedtls_text(outcome) + ")");
}

auto MbedTlsBackend::next_records(std::vector<char> &plaintext, std::vector<char> &outgoing) -> Result<TlsDataState> {
    if (peer_closed_) {
        return Result<TlsDataState>{TlsDataState::PeerClosed};
    }
    auto scratch = std::vector<char>(AURORA_MAX_RECORD_BODY_BYTES, '\0');
    auto rounds = 0;
    while (rounds < AURORA_CONTROL_ONLY_ROUNDS) {
        ++rounds;
        auto *const read_buf = reinterpret_cast<unsigned char *>(
            scratch.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) mbedTLS 收 `unsigned char *`
        const auto count = ::mbedtls_ssl_read(&ssl_, read_buf, scratch.size());
        if (count > 0) {
            plaintext.insert(plaintext.end(), scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(count));
            continue;
        }
        if (count == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // 库在数据阶段产出了握手字节（服务端 HelloRequest 触发的重协商）：交出去，本轮到此为止，
            // 由调用方发完再回来——这正是接缝给 `outgoing` 第二枚出参的那条例外通路。
            take_produced(outgoing);
            return Result<TlsDataState>{plaintext.empty() ? TlsDataState::NeedMore : TlsDataState::Delivered};
        }
        if (count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            // WANT_READ 即「缓冲里凑不出完整记录」；票据被就地消化后也算这一档（缺省配置下库不信号，
            // 走到这支只是为将来开关变了以后这里不静默丢字节）。
            take_produced(outgoing);
            if (!plaintext.empty()) {
                return Result<TlsDataState>{TlsDataState::Delivered};
            }
            return Result<TlsDataState>{TlsDataState::NeedMore};
        }
        if (count == 0 || count == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            // 0 = 读端被关掉而对端没发 close_notify，PEER_CLOSE_NOTIFY = 正式告退：两者之后都不再有明文。
            take_produced(outgoing);
            peer_closed_ = true;
            return Result<TlsDataState>{plaintext.empty() ? TlsDataState::PeerClosed : TlsDataState::Delivered};
        }
        take_produced(outgoing);
        return tls_error(ErrorCode::NetTlsHandshakeFailed,
                         "tls record failed, code=" + hex_code(count) + " (" + mbedtls_text(count) + ")");
    }
    // 只出控制消息的轮次用完：交回已有结论（有正文算 Delivered，否则 NeedMore），下一轮由上层再催。
    take_produced(outgoing);
    return Result<TlsDataState>{plaintext.empty() ? TlsDataState::NeedMore : TlsDataState::Delivered};
}

auto MbedTlsBackend::protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> {
    if (!handshaked_) {
        return tls_error(ErrorCode::NetTlsHandshakeFailed, "tls session is not established");
    }
    if (plaintext.empty()) {
        return Result<void>{};  // 空写不产生记录（上层已按 L1 口径就地完成，这里只是不把 0 长度交给库）
    }
    auto cursor = std::size_t{0};
    while (cursor < plaintext.size()) {
        const auto remaining = plaintext.size() - cursor;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) 明文缓冲按游标取下一段裸指针
        const auto *const first = plaintext.data() + static_cast<std::ptrdiff_t>(cursor);
        const auto *const write_buf = reinterpret_cast<const unsigned char *>(
            first);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) mbedTLS 收 `const unsigned char *`
        const auto written = ::mbedtls_ssl_write(&ssl_, write_buf, remaining);
        if (written > 0) {
            cursor += static_cast<std::size_t>(written);
            continue;
        }
        if (written == MBEDTLS_ERR_SSL_WANT_WRITE || written == MBEDTLS_ERR_SSL_WANT_READ) {
            // 内存型 bio 让这两支都只可能是「库攒了一条记录正要外发」或「需要重协商材料」：先把已产出的
            // 记录交出去再原地重试，绝不在这里返回「成功但字节还没成型」的中间态。
            take_produced(outgoing);
            if (written == MBEDTLS_ERR_SSL_WANT_READ && incoming_.size() == incoming_read_) {
                return tls_error(ErrorCode::NetTlsHandshakeFailed,
                                 "the record could not be wrapped until a renegotiation response is read");
            }
            continue;
        }
        take_produced(outgoing);
        return tls_error(ErrorCode::NetTlsHandshakeFailed,
                         "tls record wrap failed, code=" + hex_code(written) + " (" + mbedtls_text(written) + ")");
    }
    take_produced(outgoing);
    return Result<void>{};
}

auto MbedTlsBackend::shutdown_records(std::vector<char> &outgoing) -> Result<void> {
    if (!handshaked_) {
        return Result<void>{};  // 没有会话状态可关：空产出即「没发」而不是「失败」
    }
    // 尽力而为：close_notify 的成败不可能有结论（对端可能已经不再读），产出多少交多少，错误一律吞掉。
    (void)::mbedtls_ssl_close_notify(&ssl_);
    take_produced(outgoing);
    return Result<void>{};
}

}  // namespace

auto make_tls_backend_mbedtls(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<TlsBackend>> {
    auto backend = MbedTlsBackend::create(context, server_name);
    if (!backend) {
        return backend.error();
    }
    // 派生 → 基类的所有权移交必须写明一次：两级隐式转换（unique_ptr 与 Result 各一级）不被语言允许。
    return Result<std::unique_ptr<TlsBackend>>{std::unique_ptr<TlsBackend>{std::move(backend.value())}};
}

}  // namespace aurora::net::detail

#endif  // AURORA_ENABLE_NET_TLS_MBEDTLS
