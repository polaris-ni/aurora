#pragma once

/// @brief 网络端点值类型（CHG-018 底座切片：地址文本的解析、规范化与校验）。
/// @file net/address.h
///
/// 一个 `Endpoint` 表达「一条字节流要去往或来自何处」，是纯值类型：不含句柄、不含平台头，
/// 可在任意线程拷贝与比较。三条设计约束值得在写代码前知道：
///
/// - **解析由本库手写，不经 `inet_pton`**：同一输入在 Win32 / POSIX / wasm 上必须得到同一
///   结果与同一条诊断（`SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001`），交给 libc 就等于让
///   「哪些畸形串被谁容忍」随平台漂移。
/// - **地址族显式**：`is_loopback()` 按解析后的地址族与二进制值判定，不按字符串猜——
///   `"localhost"` 是名字不是地址，走 `Resolver` 解析后才成为 `Endpoint`。
/// - **端口不内嵌在主机文本里**：`from_ip()` 的主机参数只表达地址；需要解析
///   `"host:port"` 文本形态请等 URL 值类型（CHG-018 §12），否则同一段文本会有两种解释。
///
/// 端口 `0` 在值类型里合法（它表达「由系统分配」），语境合法性由使用方判定：`bind()` 接受
/// `0`，`connect()` 拒绝 `0`。
///
/// @code
/// auto ep = aurora::net::Endpoint::from_ip("[fe80::1%eth0]", 22);   // IPv6 + zone id
/// if (ep) { (void)(*ep).to_string(); }                              // "[fe80::1%eth0]:22"
/// auto bad = aurora::net::Endpoint::from_ip("1.2.3.4.5", 80);       // 失败：net-endpoint-invalid
/// @endcode
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "aurora/core/result.h"

namespace aurora::net {

/// @brief 端点地址族：由解析结果决定，不由字符串形态猜测。
/// @note Thread: thread-safe
/// @note Side-effects: none
enum class AddressFamily : std::uint8_t {
    IPv4,  ///< 32 位 IPv4 地址（点分十进制）
    IPv6,  ///< 128 位 IPv6 地址（可含 zone id）
    LocalPath,  ///< 本机 IPC 路径（POSIX 与 Windows 的 AF_UNIX 腿共用同一段路径文本作为命名键）
};

/// @brief 单个网络地址 + 端口（或本机 IPC 路径）的值类型。
/// @note Thread: thread-safe（拷贝与比较无共享状态）
/// @note Side-effects: none
class Endpoint {
  public:
    /// @brief 由数字地址文本构造端点。
    ///
    /// 接受 IPv4 点分十进制、IPv6 冒分十六进制（含 `::` 压缩与 `::ffff:1.2.3.4` 内嵌 IPv4
    /// 尾段）、带方括号的 IPv6 字面量，以及 link-local IPv6 上的 `%zone` 作用域标识。
    /// 明确拒绝：带前导零的 IPv4 段（`01.2.3.4`，与八进制歧义）、段数不足或过多、
    /// 带方括号的 IPv4、非 link-local 地址上挂 zone id、主机文本内嵌端口。
    /// @param address 数字地址文本；非空，且不得含端口（`:` 后跟十进制端口的形态视为 IPv6 解析失败）。
    /// @param port 主机序端口；`0` 合法并表达「由系统分配」，语境由 `bind()` / `connect()` 判。
    /// @return 成功为规范化后的端点；失败为 `net-endpoint-invalid`（message 指出被拒的原因）。
    [[nodiscard]] static auto from_ip(std::string_view address, std::uint16_t port) -> Result<Endpoint>;

    /// @brief 构造 IPv4 回环端点（`127.0.0.1:port`），本机服务与测试桩的常用形态。
    /// @param port 主机序端口；`0` 表示由系统分配（配合 `bind`）。
    /// @return 恒成功的端点（输入无失败分支）。
    [[nodiscard]] static auto loopback(std::uint16_t port) -> Endpoint;

    /// @brief 构造 IPv6 回环端点（`[::1]:port`）。
    /// @param port 主机序端口；`0` 表示由系统分配。
    /// @return 恒成功的端点。
    [[nodiscard]] static auto loopback_v6(std::uint16_t port) -> Endpoint;

    /// @brief 构造「监听所有本机地址」的通配端点（IPv4 `0.0.0.0` 或 IPv6 `::`）。
    /// @param any_v6 为真时取 IPv6 通配地址。
    /// @param port 主机序端口；`0` 表示由系统分配。
    /// @return 恒成功的端点。
    [[nodiscard]] static auto any(bool any_v6, std::uint16_t port) -> Endpoint;

    /// @brief 构造本机 IPC 端点。
    ///
    /// 入参是一段**路径文本**，两型形态都接受：文件系统路径（`/tmp/x.sock`、相对路径）与命名管道样式
    /// 的名字（裸名与 `\\.\pipe\` 全形态）。本层不据形态猜测用途，只按长度与 NUL 约束校验并原样保存
    /// （`host()` 不做规范化）；它最终落到哪条腿、以何种方式命名，由传输层的地址族决定并写在
    /// `aurora/net/transport.h` 与 `aurora/net/blocking.h` 的对应说明里。
    /// @param path 路径或管道名；非空、不含 NUL、长度在上限内。
    /// @return 成功为端点；失败为 `net-endpoint-invalid`。
    [[nodiscard]] static auto local(std::string_view path) -> Result<Endpoint>;

    /// @brief 由二进制地址还原端点（`sockaddr` 出参与 `getaddrinfo` 结果的统一入口）。
    ///
    /// 与 `from_ip()` 共用同一套规范化渲染，因此二进制形态与文本形态得到同一个 `Endpoint`
    /// （`from_bytes(... ) == from_ip(host(), port())` 对无 zone 地址成立），平台 DNS 返回的
    /// 字节序结果不会另产生一种写法。
    /// @param family 地址族；只接受 `IPv4` / `IPv6`（本机 IPC 没有二进制地址形态）。
    /// @param bytes 网络序地址字节：`IPv4` 取前 4 字节（其余忽略），`IPv6` 取满 16 字节。
    /// @param port 主机序端口；`0` 的语境合法性由使用方判。
    /// @param zone IPv6 作用域文本；空 = 无作用域。非 link-local 地址或 `IPv4` 上带 zone 一律拒。
    /// @return 成功为规范化后的端点；失败为 `net-endpoint-invalid`。
    [[nodiscard]] static auto from_bytes(AddressFamily family, const std::array<std::uint8_t, 16> &bytes,
                                         std::uint16_t port, std::string_view zone = {}) -> Result<Endpoint>;

    /// @brief 地址族。
    /// @return 构造时解析确定的族；默认构造为 `IPv4`（配 `is_valid()` 为 false）。
    [[nodiscard]] auto family() const -> AddressFamily { return family_; }
    /// @brief 是否携带有效地址。
    /// @return 经任一工厂成功构造时为 true；默认构造为 false。
    [[nodiscard]] auto is_valid() const -> bool { return valid_; }

    /// @brief 规范化后的主机文本或本机路径。
    /// @return IPv4 为点分十进制；IPv6 为小写冒分形态（不含方括号，含 `%zone` 尾巴时一并保留）；
    ///         本机 IPC 为原样路径。
    [[nodiscard]] auto host() const -> const std::string & { return host_; }

    /// @brief 端口。
    /// @return 主机序端口；本机 IPC 端点恒为 0。
    [[nodiscard]] auto port() const -> std::uint16_t { return port_; }

    /// @brief IPv6 zone id（作用域标识）。
    /// @return link-local 地址上的 `%` 后文本；无 zone 或非 IPv6 时为空。
    [[nodiscard]] auto zone() const -> const std::string & { return zone_; }

    /// @brief 二进制地址字节（供实现侧组装 `sockaddr`，不暴露平台类型）。
    /// @return IPv4 取前 4 字节（网络序、其余为零），IPv6 取满 16 字节（网络序），本机 IPC 全零。
    [[nodiscard]] auto address_bytes() const -> const std::array<std::uint8_t, 16> & { return bytes_; }

    /// @brief 是否为回环地址。
    /// @return IPv4 落在 `127.0.0.0/8`、或 IPv6 恰为 `::1` 时为 true；本机 IPC、无效占位与
    ///         其它地址为 false（无效端点的全零字节不参与判定）。
    [[nodiscard]] auto is_loopback() const -> bool;

    /// @brief 是否为通配（任一本机地址）地址。
    /// @return 有效的 IPv4 `0.0.0.0` 或 IPv6 `::` 时为 true；无效占位为 false。
    [[nodiscard]] auto is_unspecified() const -> bool;

    /// @brief 是否为组播地址。
    /// @return IPv4 落在 `224.0.0.0/4`（`RFC 5771` §2）、或 IPv6 首字节为 `0xFF`（`RFC 4291` §2.7）
    ///         时为 true；本机 IPC、无效占位与单播 / 广播地址为 false。
    /// @note 判据取解析后的地址族与二进制值，与 `is_loopback()` 同源；广播地址（如
    ///       `255.255.255.255`）**不是**组播，本函数对它返回 false。
    [[nodiscard]] auto is_multicast() const -> bool;

    /// @brief 是否为本机 IPC 端点。
    /// @return 地址族为 `LocalPath` 时为 true。
    [[nodiscard]] auto is_local_path() const -> bool { return family_ == AddressFamily::LocalPath; }

    /// @brief 渲染为可打印文本（日志与诊断用，IPv6 按惯例加方括号）。
    /// @return 形如 `"127.0.0.1:8080"`、`"[::1]:22"`、`"[fe80::1%eth0]:22"`、`"unix:/tmp/x.sock"`；
    ///         无效端点返回空串。
    [[nodiscard]] auto to_string() const -> std::string;

    /// @brief 逐字段相等比较（地址族、有效性、规范化主机、zone、端口）。
    /// @param other 被比较的端点。
    /// @return 全部字段相同为 true。
    [[nodiscard]] auto operator==(const Endpoint &other) const -> bool;

  private:
    AddressFamily family_ = AddressFamily::IPv4;  ///< 地址族
    bool valid_ = false;  ///< 是否携带有效地址
    std::string host_;  ///< 规范化主机文本或本机路径
    std::string zone_;  ///< IPv6 zone id（不含 `%`）
    std::uint16_t port_ = 0;  ///< 主机序端口
    std::array<std::uint8_t, 16> bytes_{};  ///< 网络序地址字节
};

/// @brief 本机 IPC 路径的长度上限（最宽一条腿的 `sockaddr_un::sun_path` 容量）。
/// @note 超出即 `Endpoint::local()` 拒绝，不等 bind 时才报路径过长。这只是**值类型**的上限：各条腿的
///         `sun_path` 宽度并不相同（108 与 104 两档），故发起系统调用前还会按本平台实际容量再校验一次，
///         超出时在 bind / connect 之前落 `net-endpoint-invalid` 并在 `detail` 里写明本腿上限——
///         静默截断路径会让两个进程各自绑到不同的名字上，表现为「连不上」而不是「名字太长」。
inline constexpr std::size_t AURORA_NET_MAX_LOCAL_PATH = 108;

}  // namespace aurora::net
