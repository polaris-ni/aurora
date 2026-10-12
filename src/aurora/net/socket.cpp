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
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/net/poll.h"

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

/// @brief 组装 IPv4 回环地址（127.0.0.1:port）。
/// @param port 主机序端口。
/// @return 填好的 `sockaddr_in`（族 = AF_INET，地址 = htonl(INADDR_LOOPBACK)）。
auto loopback_addr(std::uint16_t port) -> sockaddr_in {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

/// @brief 创建 TCP 流式套接字。
/// @return 成功为原生句柄；失败为 `net-socket-failed`（含 OS 码）。
auto open_socket() -> Result<NativeSocket> {
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (native_is_invalid(s)) {
        return net_error(ErrorCode::NetSocketFailed, "socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)");
    }
    return s;
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

/// @brief `getpeername`/`getsockname` 的 addrlen 出参在两平台的声明类型（Windows `int`、
/// POSIX `socklen_t`）；按平台形态声明后直接取址传参，无需任何 cast 垫片。
#ifdef AURORA_PLATFORM_WINDOWS
using AddrLen = int;
#else
using AddrLen = socklen_t;
#endif

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
    auto opened = open_socket();
    if (!opened) {
        return opened.error();
    }
    const NativeSocket s = opened.value();
    const sockaddr_in addr = loopback_addr(port);
    // socket API 契约：sockaddr_in 须擦除为通用 sockaddr 指针（协议族字段即运行期判别依据）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::connect(s, reinterpret_cast<const sockaddr *>(&addr), static_cast<int>(sizeof(addr))) != 0) {
        auto err = net_error(ErrorCode::NetConnectFailed, "connect(127.0.0.1:" + std::to_string(port) + ")");
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

auto StreamSocket::is_loopback_peer() const -> bool {
    if (!is_valid()) {
        return false;
    }
    sockaddr_in peer{};
    AddrLen name_len = sizeof(peer);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getpeername(from_handle(handle_), reinterpret_cast<sockaddr *>(&peer), &name_len) != 0) {
        return false;
    }
    // 127.0.0.0/8 全段视为回环（htonl 后首字节 0x7F）。
    return (ntohl(peer.sin_addr.s_addr) & UINT32_C(0xFF000000)) == UINT32_C(0x7F000000);
}

auto StreamSocket::close() -> void {
    if (is_valid()) {
        native_close(from_handle(handle_));
        handle_ = AURORA_INVALID_HANDLE;
    }
}

// ---- Listener ---------------------------------------------------------------

auto Listener::listen_loopback(std::uint16_t port, int backlog) -> Result<Listener> {
    auto opened = open_socket();
    if (!opened) {
        return opened.error();
    }
    const NativeSocket s = opened.value();
    const sockaddr_in addr = loopback_addr(port);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(s, reinterpret_cast<const sockaddr *>(&addr), static_cast<int>(sizeof(addr))) != 0) {
        auto err = net_error(ErrorCode::NetBindFailed, "bind(127.0.0.1:" + std::to_string(port) + ")");
        native_close(s);
        return err;
    }
    if (::listen(s, backlog) != 0) {
        auto err = net_error(ErrorCode::NetListenFailed, "listen(backlog=" + std::to_string(backlog) + ")");
        native_close(s);
        return err;
    }
    // 实际端口回报（port=0 时为系统分配值）；查询失败按入参回落（与旧 server 行为一致）。
    sockaddr_in bound{};
    AddrLen name_len = sizeof(bound);
    std::uint16_t actual_port = port;
    if (::getsockname(s, reinterpret_cast<sockaddr *>(&bound), &name_len) == 0) {
        actual_port = ntohs(bound.sin_port);
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
    sockaddr_in peer{};
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

}  // namespace aurora::net
