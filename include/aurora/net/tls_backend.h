#pragma once

/// @brief TLS 后端契约与后端注册表（可插拔实现层）。
///
/// 本头把「TLS 状态机」与「哪个 TLS 栈在跑它」之间的接缝公开出来。接缝的**形态**早已由四腿实现
/// 验证过（Schannel / SecureTransport / OpenSSL / mbedTLS 各自实现同一组七个虚函数就能工作），本次
/// 公开改的是「谁能提供一条腿」，而不是这条腿长什么样：
///   - `net/tls.cpp` 只认本接口——握手循环、记录级读写、背压与取消这些与平台无关的纪律写一遍，
///     任何后端都不得各写一份（各写一份的结局是各条腿的失败语义在细节上分叉，而分叉只会在使用方
///     那边暴露）。
///   - 内置后端由构建配置决定编译哪几枚，它们在运行期**同时**在注册表里，由候选链决定这一次握手
///     用哪一枚。系统栈与 provider 因此是并存关系而不是互斥关系。
///   - 应用与第三方经 `register_tls_backend()` 注入自研实现，注入后 `Engine::capabilities().tls`
///     即真，且缺省候选链可以选中它。
///
/// 分工边界（规范性）：**后端持有密文缓冲**。握手的分帧、记录的边界与 leftover 的归位只有后端
/// 自己清楚（Schannel 按 `SECBUFFER_STREAM` 就地消费并回写剩余长度，OpenSSL 按 memory BIO 推拉），
/// 若把分帧放到上层就得在公共侧再表达一次「一条记录」的概念——那是本层最不该泄漏的细节。上层
/// 只负责：把从底层流读到的字节 `push_incoming()`、催后端推进、把 `outgoing` 交付底层流。
///
/// 回落只发生在握手**开始前**：后端还没造出来之前，「这一枚表达不了这个口径」是可以换一枚的
/// 事实；后端一旦开始协商，换腿就等于换了一次证书校验的口径（信任根在各腿各自实现里），那是把
/// 安全判定交给调度器。因此 `create` 只可能因「不可用」被跳过，因配置错误失败则终止整条链。
///
/// 插件的二进制兼容边界如实交代：本契约是**源码级 C++ 抽象接口**，不是 C ABI。后端实例由
/// `unique_ptr<TlsBackend>` 经基类指针跨越调用点，故插件必须与宿主同工具链、同标准库、同 CRT
/// 装配（静态链接，或由宿主在装载后自行构造）；跨编译器承诺不存在，也不被暗示。
///
/// @code
/// class MyBackend final : public aurora::net::TlsBackend {
///   public:
///     auto push_incoming(std::span<const char> bytes) -> void override;
///     // ... 其余六个纯虚函数
/// };
///
/// aurora::net::TlsBackendRegistration registration;
/// registration.id = "my-tls";
/// registration.priority = 120;
/// registration.create = [](const aurora::net::TlsContext &,
///                           std::string_view) -> aurora::Result<std::unique_ptr<aurora::net::TlsBackend>> {
///     return std::make_unique<MyBackend>();
/// };
/// (void)aurora::net::register_tls_backend(registration);
///
/// aurora::net::TlsContext context;
/// context.backend = "openssl";  // 这一次握手指定后端，不回落
/// @endcode
/// @note Thread: 注册表 thread-safe（内部串行化，读取即拷出快照）；`TlsBackend` 实例**不**承诺
///       thread-safe——调用方以每连接一枚互斥量串行化全部调用，后端因此可以用裸成员而不必自己加锁
/// @note Side-effects: 进程级注册表状态变更（注册 / 注销 / 缺省链），不新增操作系统句柄类型

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
#include "aurora/net/tls.h"

namespace aurora::net {

/// @brief 数据阶段一次推进的结果。
enum class TlsDataState : std::uint8_t {
    NeedMore,  ///< 缓冲里凑不出完整记录（或记录只有握手/控制信息），需要更多字节
    Delivered,  ///< 至少解出一条应用记录，已追加到 plaintext
    PeerClosed,  ///< 收到对端 close_notify：此后不再会有明文字节
};

/// @brief TLS 后端的字节级状态机（不含 socket、线程与调度）。
///
/// 实现方只需要回答「字节进、字节出」：它不知道底层是 TCP 还是命名管道，也不登记读请求、不管
/// 回调恰一次、不做取消——那些是 `net/tls.cpp` 会话状态机的职责，写一遍归所有后端。
/// @note Thread: 单实例由调用方串行化（见本头 @note）
class TlsBackend {
  public:
    TlsBackend(const TlsBackend &) = delete;
    auto operator=(const TlsBackend &) -> TlsBackend & = delete;
    TlsBackend(TlsBackend &&) = delete;
    auto operator=(TlsBackend &&) -> TlsBackend & = delete;
    virtual ~TlsBackend() = default;

    /// @brief 追加对端字节到后端缓冲（在调用线程就地复制，上层保留自己缓冲的所有权）。
    /// @param bytes 本次从底层流读到的字节。
    virtual auto push_incoming(std::span<const char> bytes) -> void = 0;

    /// @brief 推进握手。
    /// @param outgoing 待交付底层流的记录（追加语义；上层负责发完再回来）。
    /// @return 成功携「是否已完成握手」；失败携 `net-tls-*` 系列原因（握手期间不产出明文，故不回报
    ///         数据：本函数只在握手阶段调用）。
    virtual auto handshake(std::vector<char> &outgoing) -> Result<bool> = 0;

    /// @brief 推进数据阶段：解出缓冲里所有完整应用记录。
    /// @param plaintext 明文追加目标。
    /// @param outgoing 待交付底层流的握手字节（追加语义；仅在系统要求重协商时非空，见下）。
    /// @return 成功携本轮状态；失败携原因（记录越界、解密错误、对端致命告警）。
    /// @note `outgoing` 非空意味着后端在数据阶段产出了握手消息（服务端发起的重协商、TLS 1.3 的后握手
    ///       消息回执）：调用方必须先把它交付底层流再回来继续，否则会停在半截协商上。没有这条例外通路
    ///       时，「数据阶段只产明文」的简化会把系统的合法状态机切成两半。
    virtual auto next_records(std::vector<char> &plaintext, std::vector<char> &outgoing) -> Result<TlsDataState> = 0;

    /// @brief 把一段明文封装成记录。
    /// @param plaintext 待发明文。
    /// @param outgoing 记录追加目标。
    /// @return 失败携原因（会话已失效、缓冲越界）。
    virtual auto protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> = 0;

    /// @brief 生成 close_notify 记录（尽力而为：握手未完成或会话已失效时允许返回空产出）。
    /// @param outgoing 记录追加目标。
    /// @return 失败携原因（会话状态已不可用）；产出经 `outgoing` 表达，空产出即「发不出也不算错」。
    virtual auto shutdown_records(std::vector<char> &outgoing) -> Result<void> = 0;

    /// @brief 对端证书摘要（握手完成那一刻固化）。
    /// @return 成功携摘要；会话未成立或摘要不可读携原因。
    [[nodiscard]] virtual auto peer_certificate() const -> Result<PeerCertificate> = 0;

    /// @brief 协商出的 ALPN 协议标识；未宣告或未协商出时为未置值。
    /// @return 命中值或未置值（不携错误：「没有 ALPN」是合法状态而不是失败）。
    [[nodiscard]] virtual auto negotiated_alpn() const -> std::optional<std::string> = 0;

  protected:
    /// @brief 构造：后端由注册表里的工厂造出并交由会话持有，无外部直接构造形态。
    TlsBackend() = default;
};

/// @brief 后端工厂：按本次握手的口径造出一枚可用后端。
/// @details 不可置空（注册时即校验）。返回的错误码里只有 `net-tls-engine-unavailable` 与
///          `net-platform-unavailable` 会让候选链继续下一枚——它们说的是「这一枚不行」，其余错误
///          说的是「这份配置不行」，后者换一枚后端只会把同一个错再犯一遍。
using TlsBackendCreator =
    std::function<Result<std::unique_ptr<TlsBackend>>(const TlsContext &context, std::string_view server_name)>;

/// @brief 一次后端注册的全部入参（值类型；注册即拷贝所需字段）。
struct TlsBackendRegistration {
    /// @brief 稳定标识，用于候选链与 `TlsContext::backend` 的指名，也用于诊断点名的那段文本。
    ///        小写字母 + 数字 + `-`（如 `schannel`、`openssl`、`my-tls`）；空串即非法。
    std::string id;
    /// @brief 缺省候选链里的次序，大者在前；内置系统栈为 100、OpenSSL 为 60、mbedTLS 为 40，
    ///        自研后端取 0 时排在内置之后，取更大值即优先于内置。
    int priority = 0;
    /// @brief 工厂；置空即非法注册。
    TlsBackendCreator create;
};

/// @brief 登记一枚 TLS 后端。
/// @param registration 标识、优先级与工厂。
/// @return 成功为 void；`id` 为空或 `create` 为空携 `general-invalid-argument`；`id` 已在案携
///         `net-option-conflict`（**不覆盖既有实现**：静默替换别人注册的后端是比失败更糟的结果）。
/// @note 注册对已在途的会话无影响——它们持有自己的后端实例。
auto register_tls_backend(const TlsBackendRegistration &registration) -> Result<void>;

/// @brief 注销一枚后端（内置后端同样可注销：「这台机器上我不想让这条腿参与」是调用方的合法决定）。
/// @param id 注册时使用的标识。
/// @return 命中并移除为 true；该 id 不在案为 false。
/// @note 注销只影响**之后**起手的握手；在途会话持有的实例不被回收。
auto unregister_tls_backend(std::string_view id) -> bool;

/// @brief 当前在案的后端标识。
/// @return 按 `priority` 降序排列的 id 副本（同优先级按注册次序）；空表即本进程没有任何 TLS 后端，
///         此时 `Engine::capabilities().tls` 亦为 false。
[[nodiscard]] auto available_tls_backends() -> std::vector<std::string>;

/// @brief 置全局缺省候选链（握手未指名后端时按此序尝试）。
/// @param chain 后端 id 序列；空表是合法值，语义为「起手不选任何后端，除非每次握手显式指定」。
/// @return 成功为 void；链内含空串或重复项携 `general-invalid-argument`。
/// @note 置入时**不**校验 id 是否已在案：第三方后端的注册次序与配置次序没有先后保证，置入即拒会把
///       「先配链、后加载插件」这一常见启动顺序变成不可用形态。未知 id 在解析时跳过并留 warning，
///       整链解析不出可用后端时落 `net-tls-engine-unavailable`。
auto set_default_tls_backends(std::vector<std::string> chain) -> Result<void>;

/// @brief 读当前全局缺省候选链。
/// @return 置入值的副本；未置过即内置后端按 `priority` 降序的缺省链。
[[nodiscard]] auto default_tls_backends() -> std::vector<std::string>;

}  // namespace aurora::net
