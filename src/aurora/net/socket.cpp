// Aurora net — 跨平台 socket 操作层实现（CHG-006：本仓唯一的 Winsock2 / BSD socket 适配落点）。
//
// 本文件之外的一切 TU 都不应出现平台 socket 头/裸调用（grep 判据见
// codespec/changes/winsock-bsd-socket-layer/proposal.md 验收判据 2）。两平台差异在此一次消化：
//   - 句柄形态：Winsock `SOCKET`（UINT_PTR，无效 = INVALID_SOCKET）vs POSIX `int`（无效 < 0），
//     对外统一为 `std::intptr_t`（-1 = 无效）。
//   - 运行时：Windows 须 WSAStartup/WSACleanup（`Session` 引用计数），POSIX 无此概念。
//   - 超时选项：SO_RCVTIMEO/SO_SNDTIMEO 在 Winsock 收 DWORD 毫秒、POSIX 收 struct timeval；
//     误传形态在 Windows 上会把「5 秒」执行成「5 毫秒」（历史踩坑，见 CHG-006 动机）。
//   - select 第一参：Windows 忽略、POSIX 须 max_fd+1。
//   - recv/send 长度与返回：POSIX 为 ssize_t（64 位）、Winsock 为 int；按块上限（≤1MiB）
//     使正返回值必落 int 区间，窄化在实现侧一次解决。

#include "aurora/net/socket.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/net/detail/async_io.h"
#include "aurora/net/detail/lookup.h"
#include "aurora/net/poll.h"
#include "aurora/net/transport.h"

#ifdef AURORA_PLATFORM_WINDOWS
// Winsock 引用计数会话状态需要互斥；POSIX 腿无此状态，不拉入 <mutex>。
#include <mutex>
// Windows: Winsock2（winsock2.h 须在 windows.h 之前——本文件不引入 windows.h）。
// `WSAPoll` / `WSAPOLLFD` 亦声明于此头（wait_ready 的 Windows 腿）。
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
#pragma comment(lib, "ws2_32.lib")
#endif
#else
// POSIX（含 macOS / Linux / Android / BSD / Emscripten 的 BSD socket 腿）：BSD sockets。
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace aurora::net {

namespace {

#ifdef AURORA_PLATFORM_WINDOWS
/// @brief 平台原生句柄形态（仅本文件可见）。
using NativeSocket = SOCKET;

/// @brief 关闭原生句柄（Winsock 专用拼写）。
/// @param s 待关闭的原生句柄。
auto native_close(NativeSocket s) -> void { (void)::closesocket(s); }

/// @brief 平台原生最近错误码（诊断文本用）。
/// @return `WSAGetLastError()` 的 int 化值。
auto native_last_error() -> int { return ::WSAGetLastError(); }

/// @brief recv 超时判据：Winsock 显式回报 WSAETIMEDOUT。
/// @param os_error `native_last_error()` 返回值。
/// @return 等于 `WSAETIMEDOUT` 为 true。
auto native_recv_timed_out(int os_error) -> bool { return os_error == WSAETIMEDOUT; }
#else
using NativeSocket = int;

auto native_close(NativeSocket s) -> void { (void)::close(s); }

auto native_last_error() -> int { return errno; }

// POSIX：阻塞套接字挂上 SO_RCVTIMEO 后，超时以 EAGAIN/EWOULDBLOCK 形态回报（两者是否同值随平台，
// 故双判）。
auto native_recv_timed_out(int os_error) -> bool { return os_error == EAGAIN || os_error == EWOULDBLOCK; }
#endif

/// @brief Winsock `SOCKET` 宏常量各工具链形态不一，经 uint64 中转落 intptr_t 保证
/// `INVALID_SOCKET`（全 1）恰落 -1，与 `StreamSocket::AURORA_INVALID_HANDLE` 对齐。
/// @param s 平台原生句柄。
/// @return `intptr_t` 承载值。
auto to_handle(NativeSocket s) -> std::intptr_t {
#ifdef AURORA_PLATFORM_WINDOWS
    return static_cast<std::intptr_t>(static_cast<std::uint64_t>(s));
#else
    return static_cast<std::intptr_t>(s);
#endif
}

/// @brief 对外 `intptr_t` 形态 → 原生句柄（`to_handle` 的逆）。
/// @param h `StreamSocket`/`Listener` 内部承载值。
/// @return 平台原生句柄；-1 还原为各平台无效值。
auto from_handle(std::intptr_t h) -> NativeSocket {
#ifdef AURORA_PLATFORM_WINDOWS
    return static_cast<NativeSocket>(static_cast<std::uint64_t>(h));
#else
    return static_cast<NativeSocket>(h);
#endif
}

/// @brief 句柄是否原生无效。
/// @param s 平台原生句柄。
/// @return Winsock `INVALID_SOCKET` 或 POSIX 负值时为 true。
auto native_is_invalid(NativeSocket s) -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
    return s == static_cast<NativeSocket>(INVALID_SOCKET);
#else
    return s < 0;
#endif
}

/// @brief 构造带 OS 原生码的 net 错误。
/// @param code `net-*` 错误码（表驱动元数据）。
/// @param op 失败的调用描述（如 "bind(127.0.0.1:6280)"），并入 message 供跨平台比对。
/// @return `Error`，message 形如 "op failed (os_error=N)"。
auto net_error(ErrorCode code, const std::string &op) -> Error {
    const int os_error = native_last_error();
    return make_error(code, op + " failed (os_error=" + std::to_string(os_error) + ")");
}

/// @brief `getpeername`/`getsockname` 的 addrlen 出参在两平台的声明类型（Windows `int`、
/// POSIX `socklen_t`）；按平台形态声明后直接取址传参，无需任何 cast 垫片。
#ifdef AURORA_PLATFORM_WINDOWS
using AddrLen = int;
#else
using AddrLen = socklen_t;
#endif

/// @brief 把 `Endpoint` 的 zone 文本换算成 IPv6 作用域号（`sin6_scope_id`）。
///
/// 只接受**十进制作用域号**：接口名→索引的查表在 POSIX 是 `if_nametoindex`、在 Winsock 属
/// iphlpapi，两平台的可达性不同，与其让「同名输入在一条腿上可用」漂移，一律按数字口径判。
/// @param zone zone 文本；空 = 无作用域。
/// @return 成功为作用域号（空文本为 0）；非数字或超 `std::uint32_t` 为 `net-endpoint-invalid`。
auto zone_to_scope_id(std::string_view zone) -> Result<std::uint32_t> {
    if (zone.empty()) {
        return std::uint32_t{0};
    }
    std::uint64_t value = 0;
    for (const char digit : zone) {
        if (digit < '0' || digit > '9') {
            return make_error(ErrorCode::NetEndpointInvalid,
                              "IPv6 zone must be a decimal scope id (interface names are not resolved)");
        }
        value = (value * 10U) + static_cast<std::uint64_t>(digit - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return make_error(ErrorCode::NetEndpointInvalid, "IPv6 zone scope id is out of range");
        }
    }
    return static_cast<std::uint32_t>(value);
}

/// @brief 写入 IPv6 作用域号。
///
/// Winsock 的 `sockaddr_in6::sin6_scope_id` 位于一枚匿名 union 里（与内核侧 `SCOPE_ID` 同形，
/// 高位字节是接口 LUID），POSIX 则是普通成员；两平台都写这一个名字，故把 union 访问收在
/// 本函数里，豁免只在这一处生效而不散落。
/// @param address 已按族清零的 IPv6 地址。
/// @param scope 作用域号。
auto set_scope_id(sockaddr_in6 &address, std::uint32_t scope) -> void {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): 平台结构体形态如此，POSIX 腿同名普通成员
    address.sin6_scope_id = static_cast<decltype(address.sin6_scope_id)>(scope);
}

/// @brief 读出 IPv6 作用域号（与 `set_scope_id` 对称，union 访问同样收在此处）。
/// @param address 由系统填好的 IPv6 地址。
/// @return 作用域号；0 表示无作用域。
auto scope_id_of(const sockaddr_in6 &address) -> std::uint32_t {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): 平台结构体形态如此，POSIX 腿同名普通成员
    return static_cast<std::uint32_t>(address.sin6_scope_id);
}

/// @brief 由端点组装平台 `sockaddr`（地址族取自端点，本层只承载 TCP 流）。
/// @param endpoint 有效且为 IPv4 / IPv6 的端点。
/// @param storage 出参：按族清零后填好地址、端口与作用域号。
/// @return 成功为 `sockaddr` 的有效长度；端点不合用时为 `net-endpoint-invalid`。
auto make_address(const Endpoint &endpoint, sockaddr_storage &storage) -> Result<AddrLen> {
    if (!endpoint.is_valid()) {
        return make_error(ErrorCode::NetEndpointInvalid, "endpoint carries no address");
    }
    if (endpoint.is_local_path()) {
        return make_error(ErrorCode::NetEndpointInvalid, "local IPC endpoint is not a TCP address");
    }
    // sockaddr_storage 是「足够大以容纳任一族」的通用容器，按族擦回具体类型是标准用法。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-type-cstyle-cast)
    if (endpoint.family() == AddressFamily::IPv4) {
        auto *const address = reinterpret_cast<sockaddr_in *>(&storage);
        *address = sockaddr_in{};
        address->sin_family = AF_INET;
        address->sin_port = htons(endpoint.port());
        std::memcpy(&address->sin_addr.s_addr, endpoint.address_bytes().data(), sizeof(address->sin_addr.s_addr));
        return static_cast<AddrLen>(sizeof(sockaddr_in));
    }
    auto scope = zone_to_scope_id(endpoint.zone());
    if (!scope) {
        return scope.error();
    }
    auto *const address = reinterpret_cast<sockaddr_in6 *>(&storage);
    *address = sockaddr_in6{};
    address->sin6_family = AF_INET6;
    address->sin6_port = htons(endpoint.port());
    set_scope_id(*address, scope.value());
    std::memcpy(&address->sin6_addr, endpoint.address_bytes().data(), sizeof(address->sin6_addr));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-type-cstyle-cast)
    return static_cast<AddrLen>(sizeof(sockaddr_in6));
}

/// @brief 把平台 `sockaddr` 还原为端点值类型（与 `make_address` 互逆）。
/// @param storage 由 `getpeername` / `getsockname` 填好的地址。
/// @return 成功为端点；不认识的地址族为 `net-socket-failed`。
auto endpoint_from_address(const sockaddr_storage &storage) -> Result<Endpoint> {
    std::array<std::uint8_t, 16> bytes{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto *const base = reinterpret_cast<const sockaddr *>(&storage);
    if (storage.ss_family == AF_INET) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto *const address = reinterpret_cast<const sockaddr_in *>(base);
        std::memcpy(bytes.data(), &address->sin_addr.s_addr, sizeof(address->sin_addr.s_addr));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
        return Endpoint::from_bytes(AddressFamily::IPv4, bytes, ntohs(address->sin_port));
    }
    if (storage.ss_family == AF_INET6) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto *const address = reinterpret_cast<const sockaddr_in6 *>(base);
        std::memcpy(bytes.data(), &address->sin6_addr, sizeof(address->sin6_addr));
        std::string zone;
        const std::uint32_t scope = scope_id_of(*address);
        if (scope != 0) {
            zone = std::to_string(static_cast<unsigned long>(scope));
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
        return Endpoint::from_bytes(AddressFamily::IPv6, bytes, ntohs(address->sin6_port), zone);
    }
    return make_error(ErrorCode::NetSocketFailed, "unknown address family in socket address (family=" +
                                                      std::to_string(static_cast<int>(storage.ss_family)) + ")");
}

/// @brief 创建 TCP 流式套接字。
/// @param family 地址族（`AF_INET` / `AF_INET6`）。
/// @return 成功为原生句柄；失败为 `net-socket-failed`（含 OS 码）。
auto open_socket(int family) -> Result<NativeSocket> {
    const NativeSocket s = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (native_is_invalid(s)) {
        return net_error(ErrorCode::NetSocketFailed, "socket(SOCK_STREAM, IPPROTO_TCP)");
    }
    return s;
}

/// @brief 按端点地址族创建 TCP 流式套接字。
/// @param endpoint 决定 `AF_INET` 还是 `AF_INET6` 的端点。
/// @return 成功为原生句柄；失败为 `net-endpoint-invalid` / `net-socket-failed`。
auto open_socket_for(const Endpoint &endpoint) -> Result<NativeSocket> {
    if (!endpoint.is_valid() || endpoint.is_local_path()) {
        return make_error(ErrorCode::NetEndpointInvalid, "stream sockets need an IPv4 or IPv6 endpoint");
    }
    return open_socket(endpoint.family() == AddressFamily::IPv4 ? AF_INET : AF_INET6);
}

/// @brief 写完全部字节：≤1MiB 分块循环（单轮请求字节数必落 int 可表示区间，两平台返回值
/// 窄化在承接处一次解决），对端断开/出错即止。
/// @param s 目标原生句柄。
/// @param data 待发送字节（非空）。
/// @return 全部写入为 OK；中途失败为 `net-send-failed`（含 OS 码）。
auto write_all_native(NativeSocket s, std::span<const char> data) -> Result<void> {
    std::size_t left = data.size();
    const char *p = data.data();
    while (left > 0) {
        const int len = static_cast<int>(std::min<std::size_t>(left, 1U << 20U));
        // 返回值用 ptrdiff_t 承接：POSIX send 返回 ssize_t（64 位）、Winsock 返回 int。
        const auto n = static_cast<std::ptrdiff_t>(::send(s, p, len, 0));
        if (n <= 0) {
            return net_error(ErrorCode::NetSendFailed, "send");
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 套接字发送按字节推进指针是有意设计
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return Result<void>{};
}

/// @brief 依据关注方向组装 poll / WSAPoll 的事件位（两平台同名常量、同为 short）。
/// @param descriptor 待等待句柄的关注方向。
/// @return 事件位；未关注任何方向返回 0（该句柄只会被动回报 error/hangup）。
auto poll_events(const PollDescriptor &descriptor) -> short {
    short events = 0;
    if (descriptor.want_read) {
        events |= POLLIN;
    }
    if (descriptor.want_write) {
        events |= POLLOUT;
    }
    return events;
}

/// @brief 把 poll / WSAPoll 的 revents 位映射成四向就绪结果。
/// @param revents 平台 pollfd 的 revents（两平台同名常量、同为 short）。
/// @return 就绪方向；POLLNVAL 折入 error（无效句柄是错误态，不是「什么都没等到」）。
auto poll_outcome(short revents) -> PollOutcome {
    PollOutcome outcome;
    outcome.readable = (revents & POLLIN) != 0;
    outcome.writable = (revents & POLLOUT) != 0;
    outcome.error = (revents & (POLLERR | POLLNVAL)) != 0;
    outcome.hangup = (revents & POLLHUP) != 0;
    return outcome;
}

/// @brief 把毫秒超时归一到系统调用形态：非正值一律按「立即回报」（0），正值夹进 int 域。
/// @param timeout 调用方意图的等待上限。
/// @return 传给 poll / WSAPoll 的 int 毫秒值。
auto poll_timeout_ms(std::chrono::milliseconds timeout) -> int {
    const auto ms = timeout.count();
    if (ms <= 0) {
        return 0;
    }
    return static_cast<int>(std::min<decltype(ms)>(ms, std::numeric_limits<int>::max()));
}

}  // namespace

// ---- Session ----------------------------------------------------------------

#ifdef AURORA_PLATFORM_WINDOWS
namespace {
// Winsock 引用计数状态：第一个构造的会话触发 WSAStartup、最后一个析构的会话执行 WSACleanup。
// mutex + 裸计数即可：会话构造/析构不是热路径，且 POSIX 腿整体不参与编译。
auto session_state_mutex() -> std::mutex & {
    static std::mutex m;
    return m;
}

auto session_refs() -> int & {
    static int refs = 0;
    return refs;
}

/// @brief 申请 Winsock 运行时：首个活跃会话触发 `WSAStartup`，成功后引用计数递增。
/// @return 可用状态；初始化失败 = false（计数不递增，配对清理不欠账）。
auto session_acquire() -> bool {
    std::scoped_lock lock(session_state_mutex());
    if (session_refs() == 0) {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
    }
    ++session_refs();
    return true;
}
}  // namespace
#endif

#ifdef AURORA_PLATFORM_WINDOWS
Session::Session() : ok_(session_acquire()) {}
#else
Session::Session() : ok_(true) {}  // POSIX 无运行时初始化概念，会话恒可用
#endif

Session::~Session() {
#ifdef AURORA_PLATFORM_WINDOWS
    if (!ok_) {
        return;
    }
    std::scoped_lock lock(session_state_mutex());
    --session_refs();
    if (session_refs() == 0) {
        ::WSACleanup();
    }
#endif
}

// ---- StreamSocket -----------------------------------------------------------

auto StreamSocket::connect_loopback(std::uint16_t port) -> Result<StreamSocket> {
    return connect(Endpoint::loopback(port));
}

auto StreamSocket::connect(const Endpoint &endpoint) -> Result<StreamSocket> {
    if (endpoint.port() == 0) {
        return make_error(ErrorCode::NetEndpointInvalid, "connect needs a non-zero port");
    }
    auto opened = open_socket_for(endpoint);
    if (!opened) {
        return opened.error();
    }
    const NativeSocket s = opened.value();
    sockaddr_storage storage{};
    auto built = make_address(endpoint, storage);
    if (!built) {
        native_close(s);
        return built.error();
    }
    const AddrLen address_length = built.value();
    // socket API 契约：具体族的 sockaddr 须擦除为通用 sockaddr 指针（族字段即运行期判别依据）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::connect(s, reinterpret_cast<const sockaddr *>(&storage), address_length) != 0) {
        auto err = net_error(ErrorCode::NetConnectFailed, "connect(" + endpoint.to_string() + ")");
        native_close(s);
        return err;
    }
    return StreamSocket{to_handle(s)};
}

StreamSocket::StreamSocket(StreamSocket &&other) noexcept : handle_(other.handle_) {
    other.handle_ = AURORA_INVALID_HANDLE;
}

auto StreamSocket::operator=(StreamSocket &&other) noexcept -> StreamSocket & {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = AURORA_INVALID_HANDLE;
    }
    return *this;
}

StreamSocket::~StreamSocket() { close(); }

auto StreamSocket::set_io_timeout(std::chrono::milliseconds timeout) const -> Result<void> {
    const auto ms = static_cast<long long>(std::max<std::int64_t>(timeout.count(), 1));
#ifdef AURORA_PLATFORM_WINDOWS
    // Winsock 形态：DWORD 毫秒数。
    const auto timeout_value = static_cast<DWORD>(ms);
    const void *const option = &timeout_value;
    constexpr int option_size = static_cast<int>(sizeof(timeout_value));
#else
    // POSIX 形态：timeval（秒 + 微秒）；微秒字段先在目标类型内乘，避免 int 域算完再隐式拓宽。
    timeval timeout_value{};
    timeout_value.tv_sec = static_cast<decltype(timeval::tv_sec)>(ms / 1000);
    timeout_value.tv_usec = static_cast<decltype(timeval::tv_usec)>(ms % 1000) * 1000;
    const void *const option = &timeout_value;
    const int option_size = static_cast<int>(sizeof(timeout_value));
#endif
    // `optval` 形参两平台都只认按字节传参（POSIX `const void *`、Winsock `const char *`），
    // 取选项变量地址过界是唯一方式。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const int rcv_rc = ::setsockopt(from_handle(handle_), SOL_SOCKET, SO_RCVTIMEO,
                                    reinterpret_cast<const char *>(option), option_size);
    const int snd_rc = ::setsockopt(from_handle(handle_), SOL_SOCKET, SO_SNDTIMEO,
                                    reinterpret_cast<const char *>(option), option_size);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (rcv_rc != 0 || snd_rc != 0) {
        return net_error(ErrorCode::NetTimeoutOptionFailed, "setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)");
    }
    return Result<void>{};
}

auto StreamSocket::read_some(std::span<char> buffer) const -> Result<std::size_t> {
    if (buffer.empty()) {
        return std::size_t{0};
    }
    // 长度参数夹进 ≤1MiB（返回值承接同 send：ptrdiff_t 防 ssize_t 窄化）。
    const int len = static_cast<int>(std::min<std::size_t>(buffer.size(), 1U << 20U));
    const auto n = static_cast<std::ptrdiff_t>(::recv(from_handle(handle_), buffer.data(), len, 0));
    if (n == 0) {
        return std::size_t{0};  // 对端有序关闭（FIN）
    }
    if (n < 0) {
        if (native_recv_timed_out(native_last_error())) {
            return net_error(ErrorCode::NetRecvTimeout, "recv");
        }
        return net_error(ErrorCode::NetRecvFailed, "recv");
    }
    return static_cast<std::size_t>(n);
}

auto StreamSocket::write_all(std::span<const char> data) const -> Result<void> {
    if (data.empty()) {
        return Result<void>{};
    }
    return write_all_native(from_handle(handle_), data);
}

auto StreamSocket::peer_endpoint() const -> Result<Endpoint> {
    if (!is_valid()) {
        return make_error(ErrorCode::NetSocketFailed, "getpeername on an invalid handle");
    }
    sockaddr_storage peer{};
    AddrLen name_len = sizeof(peer);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getpeername(from_handle(handle_), reinterpret_cast<sockaddr *>(&peer), &name_len) != 0) {
        return net_error(ErrorCode::NetSocketFailed, "getpeername");
    }
    return endpoint_from_address(peer);
}

auto StreamSocket::is_loopback_peer() const -> bool {
    auto peer = peer_endpoint();
    return peer && peer.value().is_loopback();
}

auto StreamSocket::close() -> void {
    if (is_valid()) {
        native_close(from_handle(handle_));
        handle_ = AURORA_INVALID_HANDLE;
    }
}

// ---- Listener ---------------------------------------------------------------

auto Listener::listen_loopback(std::uint16_t port, int backlog) -> Result<Listener> {
    return bind(Endpoint::loopback(port), backlog);
}

auto Listener::bind(const Endpoint &endpoint, int backlog) -> Result<Listener> {
    auto opened = open_socket_for(endpoint);
    if (!opened) {
        return opened.error();
    }
    const NativeSocket s = opened.value();
    sockaddr_storage storage{};
    auto built = make_address(endpoint, storage);
    if (!built) {
        native_close(s);
        return built.error();
    }
    const AddrLen address_length = built.value();
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(s, reinterpret_cast<const sockaddr *>(&storage), address_length) != 0) {
        auto err = net_error(ErrorCode::NetBindFailed, "bind(" + endpoint.to_string() + ")");
        native_close(s);
        return err;
    }
    if (::listen(s, backlog) != 0) {
        auto err = net_error(ErrorCode::NetListenFailed, "listen(backlog=" + std::to_string(backlog) + ")");
        native_close(s);
        return err;
    }
    // 实际端口回报（port=0 时为系统分配值）；查询失败按入参回落（与旧 server 行为一致）。
    std::uint16_t actual_port = endpoint.port();
    sockaddr_storage bound{};
    AddrLen name_len = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr *>(&bound), &name_len) == 0) {
        if (auto resolved = endpoint_from_address(bound)) {
            actual_port = resolved.value().port();
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    Listener listener;
    listener.handle_ = to_handle(s);
    listener.port_ = actual_port;
    return listener;
}

Listener::Listener(Listener &&other) noexcept : handle_(other.handle_), port_(other.port_) {
    other.handle_ = AURORA_INVALID_HANDLE;
    other.port_ = 0;
}

auto Listener::operator=(Listener &&other) noexcept -> Listener & {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = AURORA_INVALID_HANDLE;
        other.port_ = 0;
    }
    return *this;
}

Listener::~Listener() { close(); }

auto Listener::wait_readable(std::chrono::milliseconds timeout) const -> Result<bool> {
    const NativeSocket s = from_handle(handle_);
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(s, &read_fds);
    timeval tv{};
    const auto total_ms = std::max<std::int64_t>(timeout.count(), 0);
    tv.tv_sec = static_cast<decltype(timeval::tv_sec)>(total_ms / 1000);
    tv.tv_usec = static_cast<decltype(timeval::tv_usec)>(total_ms % 1000) * 1000;
#ifdef AURORA_PLATFORM_WINDOWS
    // Windows 忽略 nfds（句柄不是描述符索引），传 0；传 s+1 反而可能越出 fd_set。
    const int ready = ::select(0, &read_fds, nullptr, nullptr, &tv);
#else
    // POSIX 要求 nfds = 最高 fd + 1。
    const int ready = ::select(static_cast<int>(s) + 1, &read_fds, nullptr, nullptr, &tv);
#endif
    if (ready < 0) {
        return net_error(ErrorCode::NetWaitFailed, "select");
    }
    return ready > 0;
}

auto Listener::accept() const -> Result<StreamSocket> {
    sockaddr_storage peer{};
    AddrLen name_len = sizeof(peer);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const NativeSocket client = ::accept(from_handle(handle_), reinterpret_cast<sockaddr *>(&peer), &name_len);
    if (native_is_invalid(client)) {
        return net_error(ErrorCode::NetAcceptFailed, "accept");
    }
    return StreamSocket{to_handle(client)};
}

auto Listener::close() -> void {
    if (!is_valid()) {
        return;
    }
    // 先 shutdown 再 close：唤醒另一线程可能阻塞其中的 wait_readable/accept（InspectorServer
    // 停机协议）。
#ifdef AURORA_PLATFORM_WINDOWS
    (void)::shutdown(from_handle(handle_), SD_BOTH);
#else
    (void)::shutdown(from_handle(handle_), SHUT_RDWR);
#endif
    native_close(from_handle(handle_));
    handle_ = AURORA_INVALID_HANDLE;
    port_ = 0;
}

// ---- wait_ready -------------------------------------------------------------

auto wait_ready(const std::vector<PollDescriptor> &descriptors, std::chrono::milliseconds timeout)
    -> Result<std::vector<PollOutcome>> {
    // 默认全 false：无效句柄槽位、纯超时槽位都落在此初值上（「没等到」不是错误）。
    std::vector<PollOutcome> outcomes(descriptors.size());
    if (descriptors.empty()) {
        return outcomes;
    }

    // 只把有效句柄并入系统调用：一个坏句柄不该毁掉整批等待（POSIX 会因负 fd 之外的错误整体失败，
    // Windows 传 INVALID_SOCKET 更可能直接 WSAENOTSOCK 失败）。紧凑数组下标经 slot_of 映射回原序。
    std::vector<int> slot_of;
    slot_of.reserve(descriptors.size());
#ifdef AURORA_PLATFORM_WINDOWS
    std::vector<WSAPOLLFD> fds;
#else
    std::vector<pollfd> fds;
#endif
    fds.reserve(descriptors.size());
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const PollDescriptor &descriptor = descriptors[i];
        if (descriptor.handle < 0) {
            continue;  // -1（含任何负值）恒无效，保持全 false
        }
        slot_of.push_back(static_cast<int>(i));
#ifdef AURORA_PLATFORM_WINDOWS
        WSAPOLLFD entry{};
        entry.fd = from_handle(descriptor.handle);
#else
        pollfd entry{};
        entry.fd = static_cast<int>(descriptor.handle);
#endif
        entry.events = poll_events(descriptor);
        entry.revents = 0;
        fds.push_back(entry);
    }

    if (fds.empty()) {
        return outcomes;  // 全为无效句柄：非错误，等价「什么都没等到」
    }

    const int timeout_ms = poll_timeout_ms(timeout);
#ifdef AURORA_PLATFORM_WINDOWS
    // WSAPoll 无 EINTR 语义（Windows 不被 POSIX 信号打断），单次调用即可；需要活跃的 Winsock
    // 运行时（与所有 net 用法一致：调用方持 `Session`），否则以 WSAENETDOWN 落 net-wait-failed。
    const int ready = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), static_cast<LONG>(timeout_ms));
    if (ready == SOCKET_ERROR) {
        return net_error(ErrorCode::NetWaitFailed, "WSAPoll");
    }
#else
    int ready = 0;
    do {
        ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
    } while (ready < 0 && errno == EINTR);  // 被信号打断：同预算重新武装，不吞错误、不提前返回
    if (ready < 0) {
        return net_error(ErrorCode::NetWaitFailed, "poll");
    }
#endif

    for (std::size_t k = 0; k < fds.size(); ++k) {
        outcomes[static_cast<std::size_t>(slot_of[k])] = poll_outcome(fds[k].revents);
    }
    return outcomes;
}

// ---- 平台查表腿（`getaddrinfo` 的仓内唯一调用点） -----------------------------------------

namespace {

/// @brief 组装 `net-dns-failed`：只带主机名与平台回报的数字码。
///
/// 不带 `gai_strerror` 文本：那串消息随 locale 与代码页漂移（Windows 上还会给出非 ASCII 形态），
/// 而数字码在 man 页与 MSDN 里各有确定含义——诊断文本要能跨机器比对，不能由现场语言决定。
/// @param host 被查询的主机名文本。
/// @param code 平台回报码。
/// @return `net-dns-failed` 错误。
auto dns_failed(std::string_view host, int code) -> Error {
    return make_error(ErrorCode::NetDnsFailed, "host=" + std::string{host} + ", resolver code=" + std::to_string(code));
}

}  // namespace

auto detail::platform_lookup(std::string_view host, std::uint16_t port) -> Result<std::vector<Endpoint>> {
    const Session session;  // Windows：`getaddrinfo` 要求已初始化的 Winsock 运行时
    if (!session.ok()) {
        return make_error(ErrorCode::NetPlatformUnavailable, "socket runtime could not be started for name resolution");
    }
    const std::string name(host);
    const std::string service = std::to_string(port);
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    // AI_NUMERICSERV：端口按数字给。服务名查表会让 `/etc/services` 参与结果（同一端口在两台机器
    // 上给出两个服务名），而本层只需要族与地址；填回的端口本就是入参那个。
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo *records = nullptr;
    const int code = ::getaddrinfo(name.c_str(), service.c_str(), &hints, &records);
    if (code != 0 || records == nullptr) {
        return dns_failed(name, code);
    }
    std::vector<Endpoint> out;
    for (const addrinfo *node = records; node != nullptr; node = node->ai_next) {
        if (node->ai_family != AF_INET && node->ai_family != AF_INET6) {
            continue;  // 其余族（链路层等）本库不承载
        }
        if (node->ai_addr == nullptr || node->ai_addrlen == 0 || node->ai_addrlen > sizeof(sockaddr_storage)) {
            continue;  // 长度不合结构即不可信：宁可少一条，也不从越界形态里读字节
        }
        sockaddr_storage storage{};
        std::memset(&storage, 0, sizeof(storage));
        std::memcpy(&storage, node->ai_addr, node->ai_addrlen);
        // 地址族的原点在两型结构里都是首字段，故上面的整块拷贝已把 `ss_family` 带到位。
        auto built = endpoint_from_address(storage);
        if (built) {
            out.push_back(std::move(built.value()));
        }
    }
    ::freeaddrinfo(records);
    if (out.empty()) {
        // 「查表成功但零条可用记录」等同不可用：空列表会让调用方去猜这是 DNS 空段还是本库筛掉了。
        return make_error(ErrorCode::NetDnsFailed, "host=" + name + ", resolver returned no usable address record");
    }
    return out;
}

// ---- 非阻塞出口（`net/detail/async_io.h`；传输层的平台腿） ----------------------------------

namespace {

/// @brief 把原生句柄置为非阻塞模式。
///
/// 两平台拼写完全不同：Winsock 走 `ioctlsocket(FIONBIO)`、POSIX 走 `fcntl(F_SETFL, O_NONBLOCK)`。
/// POSIX 侧必须先读回现有标志再按位或——直接 `F_SETFL(O_NONBLOCK)` 会把 `O_APPEND` 之类既有标志清掉。
/// @param s 目标原生句柄。
/// @return 成功为 OK；失败为 `net-option-failed`（含 OS 原生码）。
auto make_nonblocking(NativeSocket s) -> Result<void> {
#ifdef AURORA_PLATFORM_WINDOWS
    u_long mode = 1;
    if (::ioctlsocket(s, FIONBIO, &mode) != 0) {
        return net_error(ErrorCode::NetOptionFailed, "ioctlsocket(FIONBIO)");
    }
#else
    const int current = ::fcntl(s, F_GETFL, 0);
    if (current < 0) {
        return net_error(ErrorCode::NetOptionFailed, "fcntl(F_GETFL)");
    }
    if (::fcntl(s, F_SETFL, current | O_NONBLOCK) < 0) {
        return net_error(ErrorCode::NetOptionFailed, "fcntl(F_SETFL, O_NONBLOCK)");
    }
#endif
    return Result<void>{};
}

/// @brief 最近错误是否「内核暂无进展」（读空 / 写满）。
/// @param os_error `native_last_error()` 返回值。
/// @return Winsock `WSAEWOULDBLOCK`、POSIX `EAGAIN`/`EWOULDBLOCK` 时为 true（两者是否同值随平台）。
auto native_would_block(int os_error) -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
    return os_error == WSAEWOULDBLOCK;
#else
    return os_error == EAGAIN || os_error == EWOULDBLOCK;
#endif
}

/// @brief 非阻塞 connect 的「正在进行中」判据。
///
/// Winsock 在一次未完成的异步连接上重复调用 `connect` 会回报 `WSAEALREADY`，首次则多为
/// `WSAEWOULDBLOCK`；POSIX 是 `EINPROGRESS`/`EALREADY`，另加 `EINTR`——被信号打断的 connect
/// 连接仍在后台推进，按「进行中」处理才不会把一次合法的异步连接误报成失败。
/// @param os_error `native_last_error()` 返回值。
/// @return 属「进行中」集合时为 true。
auto native_connect_in_progress(int os_error) -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
    return os_error == WSAEALREADY || os_error == WSAEWOULDBLOCK || os_error == WSAEINPROGRESS;
#else
    return os_error == EINPROGRESS || os_error == EALREADY || os_error == EINTR;
#endif
}

/// @brief 「其实早已连接」判据（重复发起 connect 或竞态下可出现，等价于成功）。
/// @param os_error `native_last_error()` 返回值。
/// @return Winsock `WSAEISCONN`、POSIX `EISCONN` 时为 true。
auto native_already_connected(int os_error) -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
    return os_error == WSAEISCONN;
#else
    return os_error == EISCONN;
#endif
}

/// @brief `accept` 的瞬态空回报判据：无新连接，或挂起连接在被接受前已被对端撤掉/被信号打断。
///
/// `ECONNABORTED` 在 POSIX 与 Winsock 都是「完成三次握手后连接被中止」，是 backlog 的正常损耗，
/// 不是监听口的错误——把它报成失败会让一个客户端刷新页面就打挂服务端。
/// @param os_error `native_last_error()` 返回值。
/// @return 属瞬态空回报集合时为 true。
auto native_accept_empty(int os_error) -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
    return os_error == WSAEWOULDBLOCK || os_error == WSAECONNABORTED || os_error == WSAEINTR;
#else
    return os_error == EAGAIN || os_error == EWOULDBLOCK || os_error == ECONNABORTED || os_error == EINTR;
#endif
}

/// @brief 组装 `IoAttempt` 的失败分支（带 OS 原生码与操作名）。
/// @param code 失败落点码。
/// @param op 失败的调用描述。
/// @return 只填了 `failure` 的三态结果。
auto io_failed(ErrorCode code, const std::string &op) -> detail::IoAttempt {
    detail::IoAttempt attempt;
    attempt.failure = net_error(code, op);
    return attempt;
}

}  // namespace

auto detail::open_nonblocking_stream(const Endpoint &endpoint) -> Result<std::intptr_t> {
    if (endpoint.port() == 0) {
        return make_error(ErrorCode::NetEndpointInvalid, "a stream client endpoint needs a non-zero port");
    }
    auto opened = open_socket_for(endpoint);
    if (!opened) {
        return opened.error();
    }
    const NativeSocket s = opened.value();
    if (auto set = make_nonblocking(s); !set) {
        native_close(s);
        return set.error();
    }
    return to_handle(s);
}

auto detail::open_nonblocking_listener(const Endpoint &endpoint, int backlog) -> Result<std::intptr_t> {
    auto opened = open_socket_for(endpoint);
    if (!opened) {
        return opened.error();
    }
    const NativeSocket s = opened.value();
    sockaddr_storage storage{};
    auto built = make_address(endpoint, storage);
    if (!built) {
        native_close(s);
        return built.error();
    }
    const AddrLen address_length = built.value();
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(s, reinterpret_cast<const sockaddr *>(&storage), address_length) != 0) {
        auto err = net_error(ErrorCode::NetBindFailed, "bind(" + endpoint.to_string() + ")");
        native_close(s);
        return err;
    }
    if (::listen(s, backlog) != 0) {
        auto err = net_error(ErrorCode::NetListenFailed, "listen(backlog=" + std::to_string(backlog) + ")");
        native_close(s);
        return err;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (auto set = make_nonblocking(s); !set) {
        native_close(s);
        return set.error();
    }
    return to_handle(s);
}

auto detail::close_handle(std::intptr_t handle) -> void {
    if (handle != static_cast<std::intptr_t>(-1)) {
        native_close(from_handle(handle));
    }
}

auto detail::start_connect(std::intptr_t handle, const Endpoint &endpoint) -> Result<void> {
    const NativeSocket s = from_handle(handle);
    sockaddr_storage storage{};
    auto built = make_address(endpoint, storage);
    if (!built) {
        return built.error();
    }
    const AddrLen address_length = built.value();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::connect(s, reinterpret_cast<const sockaddr *>(&storage), address_length) == 0) {
        return Result<void>{};  // 即时建立（回环上可发生）；结果仍由 take_connect_error 复核
    }
    const int os_error = native_last_error();
    if (native_connect_in_progress(os_error) || native_already_connected(os_error)) {
        return Result<void>{};  // 进行中与「已连接」都由可写就绪后的取错口裁决
    }
    return make_error(ErrorCode::NetConnectFailed,
                      "connect(" + endpoint.to_string() + ") failed (os_error=" + std::to_string(os_error) + ")");
}

auto detail::take_connect_error(std::intptr_t handle) -> Result<void> {
    int option = 0;
    AddrLen option_len = sizeof(option);
    const NativeSocket s = from_handle(handle);
    // `getsockopt` 的 optval 是「按字节写出」的出参，两平台都只接受非 const void*/char*。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&option), &option_len) != 0) {
        return net_error(ErrorCode::NetConnectFailed, "getsockopt(SO_ERROR)");
    }
    if (option == 0) {
        return Result<void>{};
    }
    // 可写就绪却仍带错误码：`EALREADY`/`WSAEALREADY` 一类的「还在进行」在这一刻已是平台缺陷，
    // 报同一枚 connect 失败码而不新造码——调用方的动作（换候选地址）在两种情形下完全一致。
    return make_error(ErrorCode::NetConnectFailed, "connect completed with os_error=" + std::to_string(option));
}

auto detail::try_read(std::intptr_t handle, std::span<char> buffer) -> IoAttempt {
    if (buffer.empty()) {
        IoAttempt attempt;
        attempt.blocked = true;
        return attempt;
    }
    const int len = static_cast<int>(std::min<std::size_t>(buffer.size(), 1U << 20U));
    const auto n = static_cast<std::ptrdiff_t>(::recv(from_handle(handle), buffer.data(), len, 0));
    if (n > 0) {
        IoAttempt attempt;
        attempt.bytes = static_cast<std::size_t>(n);
        return attempt;
    }
    if (n == 0) {
        return IoAttempt{};  // 对端 FIN：零字节且未失败，语义在声明处（读方向的 EOF）
    }
    const int os_error = native_last_error();
    if (native_would_block(os_error)) {
        IoAttempt attempt;
        attempt.blocked = true;
        return attempt;
    }
    return io_failed(ErrorCode::NetRecvFailed, "recv");
}

auto detail::try_write(std::intptr_t handle, std::span<const char> data) -> IoAttempt {
    if (data.empty()) {
        IoAttempt attempt;
        attempt.blocked = true;
        return attempt;
    }
    const int len = static_cast<int>(std::min<std::size_t>(data.size(), 1U << 20U));
    const auto n = static_cast<std::ptrdiff_t>(::send(from_handle(handle), data.data(), len, 0));
    if (n > 0) {
        IoAttempt attempt;
        attempt.bytes = static_cast<std::size_t>(n);
        return attempt;
    }
    const int os_error = native_last_error();
    if (native_would_block(os_error)) {
        IoAttempt attempt;
        attempt.blocked = true;
        return attempt;
    }
    if (n == 0) {
        return io_failed(ErrorCode::NetSendFailed, "send returned zero without would-block");
    }
    return io_failed(ErrorCode::NetSendFailed, "send");
}

auto detail::try_accept(std::intptr_t handle) -> AcceptAttempt {
    const NativeSocket s = from_handle(handle);
    sockaddr_storage peer{};
    AddrLen peer_len = sizeof(peer);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const NativeSocket accepted = ::accept(s, reinterpret_cast<sockaddr *>(&peer), &peer_len);
    if (!native_is_invalid(accepted)) {
        if (auto set = make_nonblocking(accepted); !set) {
            native_close(accepted);
            AcceptAttempt attempt;
            attempt.failure = set.error();
            return attempt;
        }
        AcceptAttempt attempt;
        attempt.peer = to_handle(accepted);
        return attempt;
    }
    const int os_error = native_last_error();
    if (native_accept_empty(os_error)) {
        AcceptAttempt attempt;
        attempt.blocked = true;  // 无新连接 / 挂起连接在 accept 前被对端撤掉，都不是监听口的错误
        return attempt;
    }
    AcceptAttempt attempt;
    attempt.failure = net_error(ErrorCode::NetAcceptFailed, "accept");
    return attempt;
}

auto detail::shutdown_handle(std::intptr_t handle, ShutdownMode mode) -> Result<void> {
    int how = 0;
#ifdef AURORA_PLATFORM_WINDOWS
    how = mode == ShutdownMode::Read ? SD_RECEIVE : (mode == ShutdownMode::Write ? SD_SEND : SD_BOTH);
#else
    how = mode == ShutdownMode::Read ? SHUT_RD : (mode == ShutdownMode::Write ? SHUT_WR : SHUT_RDWR);
#endif
    if (::shutdown(from_handle(handle), how) != 0) {
        return net_error(ErrorCode::NetOptionFailed, "shutdown");
    }
    return Result<void>{};
}

auto detail::bound_port(std::intptr_t handle) -> std::uint16_t {
    sockaddr_storage bound{};
    AddrLen name_len = sizeof(bound);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(from_handle(handle), reinterpret_cast<sockaddr *>(&bound), &name_len) != 0) {
        return 0;
    }
    auto resolved = endpoint_from_address(bound);
    return resolved ? resolved.value().port() : 0;
}

auto detail::peer_of(std::intptr_t handle) -> Result<Endpoint> {
    sockaddr_storage peer{};
    AddrLen name_len = sizeof(peer);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getpeername(from_handle(handle), reinterpret_cast<sockaddr *>(&peer), &name_len) != 0) {
        return net_error(ErrorCode::NetSocketFailed, "getpeername");
    }
    return endpoint_from_address(peer);
}

auto detail::peer_is_loopback(std::intptr_t handle) -> bool {
    auto peer = peer_of(handle);
    return peer && peer.value().is_loopback();
}

}  // namespace aurora::net
