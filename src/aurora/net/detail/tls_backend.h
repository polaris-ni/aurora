#pragma once

// Aurora net — TLS 后端接缝（CHG-018 TLS 切片，内部头）。
//
// 存在的理由是把「TLS 状态机」与「哪个平台的 TLS 栈在跑它」彻底分开：
//   - `net/tls.cpp` 只认本接口——握手循环、记录级读写、背压与取消这些与平台无关的纪律写一遍，
//     任何后端都不得各写一份（各写一份的结局是三条腿的失败语义在细节上分叉，而分叉只会在使用方
//     那边暴露）。
//   - 每个后端一个 TU（`net/tls_schannel.cpp` 等），由构建配置选定编译哪一个；没有可用后端时
//     编译 `net/tls_unavailable.cpp`，它让 `make_tls_backend()` 恒落 `net-tls-engine-unavailable`。
//
// 分工边界（规范性）：**后端持有密文缓冲**。握手的分帧、记录的边界与 leftover 的归位只有后端
// 自己清楚（Schannel 按 `SECBUFFER_STREAM` 就地消费并回写剩余长度，OpenSSL 按 memory BIO 推拉），
// 若把分帧放到上层就得在公共侧再表达一次「一条记录」的概念——那是本层最不该泄漏的细节。上层
// 只负责：把从底层流读到的字节 `push_incoming()`、催后端推进、把 `outgoing` 交付底层流。
//
// 线程约定：本接口不承诺 thread-safe——调用方（`net/tls.cpp` 的会话状态机）以每连接一枚互斥量
// 串行化全部调用，后端因此可以用裸成员而不必自己加锁。

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/tls.h"

namespace aurora::net::detail {

/// @brief 数据阶段一次推进的结果。
enum class TlsDataState : std::uint8_t {
    NeedMore,  ///< 缓冲里凑不出完整记录（或记录只有握手/控制信息），需要更多字节
    Delivered,  ///< 至少解出一条应用记录，已追加到 plaintext
    PeerClosed,  ///< 收到对端 close_notify：此后不再会有明文字节
};

/// @brief TLS 后端的字节级状态机（不含 socket、线程与调度）。
class TlsBackend {
  public:
    TlsBackend(const TlsBackend &) = delete;
    auto operator=(const TlsBackend &) -> TlsBackend & = delete;
    TlsBackend(TlsBackend &&) = delete;
    auto operator=(TlsBackend &&) -> TlsBackend & = delete;
    virtual ~TlsBackend() = default;

    /// @brief 追加对端字节到后端缓冲（在调用线程就地复制，上层保留自己缓冲的所有权）。
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
    ///
    /// `outgoing` 非空意味着后端在数据阶段产出了握手消息（服务端发起的重协商、TLS 1.3 的后握手消息
    /// 回执）：调用方必须先把它交付底层流再回来继续，否则会停在半截协商上。没有这条例外通路时，
    /// 「数据阶段只产明文」的简化会把系统的合法状态机切成两半。
    virtual auto next_records(std::vector<char> &plaintext, std::vector<char> &outgoing) -> Result<TlsDataState> = 0;

    /// @brief 把一段明文封装成记录。
    /// @param plaintext 待发明文。
    /// @param outgoing 记录追加目标。
    /// @return 失败携原因（会话已失效、缓冲越界）。
    virtual auto protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> = 0;

    /// @brief 生成 close_notify 记录（尽力而为：握手未完成或会话已失效时允许返回空产出）。
    /// @param outgoing 记录追加目标。
    virtual auto shutdown_records(std::vector<char> &outgoing) -> Result<void> = 0;

    /// @brief 对端证书摘要（握手完成那一刻固化）。
    [[nodiscard]] virtual auto peer_certificate() const -> Result<PeerCertificate> = 0;

    /// @brief 协商出的 ALPN 协议标识；未宣告或未协商出时为未置值。
    [[nodiscard]] virtual auto negotiated_alpn() const -> std::optional<std::string> = 0;

  protected:
    /// @brief 构造：后端由 `make_tls_backend()` 造出并交由会话持有，无外部构造形态。
    TlsBackend() = default;
};

/// @brief 按构建配置造出当时唯一可用的后端。
/// @param context 安全口径（版本界、ALPN、危险开关、信任根、pin、客户端身份）。
/// @param server_name SNI 与主机名校验的目标名。
/// @return 成功携后端；本腿无后端为 `net-tls-engine-unavailable`，口径不合为
///         `general-invalid-argument`（如版本上界低于下界）。
auto make_tls_backend(const TlsContext &context, std::string_view server_name) -> Result<std::unique_ptr<TlsBackend>>;

/// @brief 本腿是否编入了 TLS 后端（`Engine::capabilities().tls` 的唯一真值来源）。
///
/// 由后端 TU 各自定义（有后端者为 true、`tls_unavailable.cpp` 为 false），因此能力位与「调用真的
/// 能成功」严格同源：能力位先于实现点亮会诱导调用方去分支一条今天不存在的路径。
[[nodiscard]] auto tls_backend_available() -> bool;

}  // namespace aurora::net::detail
