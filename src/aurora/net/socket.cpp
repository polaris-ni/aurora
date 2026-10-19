// Aurora net — 跨平台 socket 操作层实现（本仓唯一的 Winsock2 / BSD socket 适配落点）。
//
// 本文件之外的一切 TU 都不应出现平台 socket 头/裸调用（grep 判据见
//  验收判据 2）。两平台差异在此一次消化：
//   - 句柄形态：Winsock `SOCKET`（UINT_PTR，无效 = INVALID_SOCKET）vs POSIX `int`（无效 < 0），
//     对外统一为 `std::intptr_t`（-1 = 无效）。
//   - 运行时：Windows 须 WSAStartup/WSACleanup（引用计数，见本文件的 `runtime_pinned`），POSIX 无此概念。
//   - 超时选项：SO_RCVTIMEO/SO_SNDTIMEO 在 Winsock 收 DWORD 毫秒、POSIX 收 struct timeval；
//     误传形态在 Windows 上会把「5 秒」执行成「5 毫秒」（历史踩坑，见 动机）。
//   - select 第一参：Windows 忽略、POSIX 须 max_fd+1。
//   - recv/send 长度与返回：POSIX 为 ssize_t（64 位）、Winsock 为 int；按块上限（≤1MiB）
//     使正返回值必落 int 区间，窄化在实现侧一次解决。
//   - 本机 IPC 地址形态：Windows 的 SDK 头不提供 `sockaddr_un`（其 AF_UNIX 腿仍按同一布局解析传入
//     地址），POSIX 有真实的 `<sys/un.h>` 且 macOS 的 `sun_path` 比 Linux 窄；故路径容量在装配
//     `sockaddr` 时按本腿实际宽度再判一次，而不是只靠值类型里那个最宽档的上限。

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/net/blocking.h"
#include "aurora/net/detail/async_io.h"
#include "aurora/net/detail/lookup.h"
#include "aurora/net/detail/runtime.h"
#include "aurora/net/poll.h"
#include "aurora/net/transport.h"

#ifdef AURORA_PLATFORM_WINDOWS
// Winsock 引用计数会话状态需要互斥；POSIX 腿无此状态，不拉入 <mutex>。
#include <mutex>
// Windows: Winsock2（winsock2.h 须在 windows.h 之前——本文件不引入 windows.h）。
// `WSAPoll` / `WSAPOLLFD` 亦声明于此头（wait_ready 的 Windows 腿）。
// clang-format off
// 顺序是硬约束而非风格：`mstcpip.h` 里的 `tcp_keepalive` 与 `SIO_KEEPALIVE_VALS` 建在 Winsock 基础类型
// （`u_long`、`_WSAIOW`、`IOC_IN`）之上，头自身不做包含保护性回补，故必须排在 winsock2.h 之后；
// clang-format 的 SortIncludes 会把它们按字母序换过来，因此这一段单独关掉格式化。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
// clang-format on
#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
#pragma comment(lib, "ws2_32.lib")
#endif
#else
// POSIX（含 macOS / Linux / Android / BSD / Emscripten 的 BSD socket 腿）：BSD sockets。
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
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

/// @brief 「本腿不认这个地址族」判据（本机 IPC 在旧版 Windows 上即落此处）。
/// @param os_error `native_last_error()` 返回值。
/// @return 等于 `WSAEAFNOSUPPORT` 或 `WSAEINVAL` 时为 true。
/// @note `WSAEINVAL` 一并收下：AF_UNIX 尚未随系统更新到位时，Winsock 对未知地址族的回报在这两种
///       形态之间摇摆，而创建句柄这一步的 `WSAEINVAL` 除地址族外没有别的来源。
auto native_family_unsupported(int os_error) -> bool { return os_error == WSAEAFNOSUPPORT || os_error == WSAEINVAL; }

/// @brief send 附加标志：Winsock 无 SIGPIPE 概念（默认行为即不产信号），标志为 0。
constexpr int AURORA_SEND_FLAGS = 0;

/// @brief Windows 的 SDK 头不提供 `sockaddr_un`，而其 AF_UNIX 腿解析传入地址时认的正是这一布局
/// （前两字节是地址族，其后是路径文本），故在此按内核侧形态补齐声明并冠以库内类型名（仓内命名
/// 纪律不允许自定义类型直接占平台标识符）。POSIX 腿一律用 `<sys/un.h>` 里的真实类型——macOS 的
/// `sockaddr_un` 在族字段前还有一字节 `sun_len`、路径段也比这里窄，用自定义结构去拼一条 BSD 地址
/// 会得到错位的命名键。
struct UnixSocketAddr {
    unsigned short sun_family;  ///< 地址族，填 `AF_UNIX`
    char sun_path[108];  ///< 路径文本（含结尾 NUL 的本腿容量）
};
#else
using NativeSocket = int;

/// @brief POSIX 腿的本机 IPC 地址形态直接用内核头里的那一枚，库内不复制布局（两腿字段宽度不同）。
using UnixSocketAddr = sockaddr_un;

auto native_close(NativeSocket s) -> void { (void)::close(s); }

auto native_last_error() -> int { return errno; }

// POSIX：阻塞套接字挂上 SO_RCVTIMEO 后，超时以 EAGAIN/EWOULDBLOCK 形态回报（两者是否同值随平台，
// 故双判）。
auto native_recv_timed_out(int os_error) -> bool { return os_error == EAGAIN || os_error == EWOULDBLOCK; }

// POSIX：地址族不支持只有一个回报码（本机 IPC 通路在裸内核上没有对应物时的形态）。
auto native_family_unsupported(int os_error) -> bool { return os_error == EAFNOSUPPORT; }

/// @brief send 附加标志：POSIX 对「对端已断开仍继续写」默认回 SIGPIPE 直接杀进程，而本层错误语义
/// 全走返回值路径（`EPIPE` 经 `net-send-failed` 回报），信号纯属多余杀伤，故显式抑制。
/// 重试路径尤其依赖这一点：连接被对端先行关闭时，写入须落成一次可重试的失败而非进程死亡。
constexpr int AURORA_SEND_FLAGS = MSG_NOSIGNAL;
#endif

/// @brief Winsock `SOCKET` 宏常量各工具链形态不一，经 uint64 中转落 intptr_t 保证
/// `INVALID_SOCKET`（全 1）恰落 -1，与 `blocking::StreamSocket::AURORA_INVALID_HANDLE` 对齐。
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
/// @param h `blocking::StreamSocket`/`blocking::Listener` 内部承载值。
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

/// @brief 本机 IPC 路径能否装进本腿的 Unix 域地址（`sun_path` 的宽度是编译期事实，两腿并不相同）。
///
/// 值类型里的 `AURORA_NET_MAX_LOCAL_PATH` 取的是最宽一条腿，故这里必须再判一次：路径放不下时静默截断
/// 会让两个进程各自绑到不同的名字上，现场表现是「连不上」而不是「名字太长」，那是最难查的一类缺陷。
/// @param path 端点携带的路径文本。
/// @return 留出结尾 NUL 后仍装得下为 OK；否则 `net-endpoint-invalid`，`detail` 写明本腿上限。
auto check_unix_path(std::string_view path) -> Result<void> {
    constexpr std::size_t capacity = sizeof(UnixSocketAddr::sun_path);
    if (path.size() + 1 > capacity) {
        return make_error(ErrorCode::NetEndpointInvalid,
                          ErrorParams{{"detail", "local IPC path needs " + std::to_string(path.size() + 1) +
                                                     " bytes but this platform holds at most " +
                                                     std::to_string(capacity) + " including the terminator"}});
    }
    return Result<void>{};
}

/// @brief 由本机 IPC 端点组装 Unix 域地址。
/// @param endpoint 地址族为 `LocalPath` 的有效端点。
/// @param storage 出参：按 `AF_UNIX` 清零后填族与路径。
/// @return 成功为地址有效长度（到路径结尾的 NUL 为止，其余字节不参与命名）；路径超出本腿容量为
///         `net-endpoint-invalid`。
auto make_unix_address(const Endpoint &endpoint, sockaddr_storage &storage) -> Result<AddrLen> {
    static_assert(sizeof(UnixSocketAddr) <= sizeof(sockaddr_storage),
                  "a unix domain address must fit in the generic socket storage");
    if (auto fits = check_unix_path(endpoint.host()); !fits) {
        return fits.error();
    }
    // `sockaddr_storage` 是「足够大以容纳任一族」的通用容器，按族擦回具体类型是标准用法。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    auto *const address = reinterpret_cast<UnixSocketAddr *>(&storage);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    *address = UnixSocketAddr{};
    address->sun_family = AF_UNIX;
    std::memcpy(address->sun_path, endpoint.host().data(), endpoint.host().size());
    return static_cast<AddrLen>(offsetof(UnixSocketAddr, sun_path) + endpoint.host().size() + 1);
}

/// @brief 由端点组装平台 `sockaddr`（地址族取自端点：IPv4 / IPv6 走 TCP 形态，本机 IPC 走 `AF_UNIX`）。
/// @param endpoint 有效端点；本机 IPC 形态的路径长度在本函数内按本腿容量再校验。
/// @param storage 出参：按族清零后填好地址、端口与作用域号。
/// @return 成功为 `sockaddr` 的有效长度；端点不合用或路径过长为 `net-endpoint-invalid`。
auto make_address(const Endpoint &endpoint, sockaddr_storage &storage) -> Result<AddrLen> {
    if (!endpoint.is_valid()) {
        return make_error(ErrorCode::NetEndpointInvalid, "endpoint carries no address");
    }
    if (endpoint.is_local_path()) {
        return make_unix_address(endpoint, storage);
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
    if (storage.ss_family == AF_UNIX) {
        // 本机 IPC 连接没有「对端 IP 地址」可还原：这一族的 `getpeername` 只回报地址族，路径段是空的
        // （内核不在双向流上登记命名键）。公共面据此把问题答成 `net-option-conflict`，此处只作兜底。
        return make_error(ErrorCode::NetSocketFailed, "a local IPC handle has no IP address form");
    }
    return make_error(ErrorCode::NetSocketFailed, "unknown address family in socket address (family=" +
                                                      std::to_string(static_cast<int>(storage.ss_family)) + ")");
}

// ---- 平台运行时（Windows 的 Winsock 引用计数；POSIX 无此概念）---------------------------------

#ifdef AURORA_PLATFORM_WINDOWS
// Winsock 引用计数状态：首个持有者触发 WSAStartup、末个持有者执行 WSACleanup。
// mutex + 裸计数即可：运行时申请不是热路径，且 POSIX 腿整体不参与编译。
auto session_state_mutex() -> std::mutex & {
    static std::mutex m;
    return m;
}

auto session_refs() -> int & {
    static int refs = 0;
    return refs;
}

/// @brief 申请 Winsock 运行时：首个活跃持有者触发 `WSAStartup`，成功后引用计数递增。
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
#endif

/// @brief 钉住平台运行时：本文件创建任何句柄前都先过一次。
///
/// 「只申请不还」是刻意的取舍而非疏漏：公共面已改由库侧持有运行时（`aurora::net::blocking` 不再要求
/// 调用方自备会话），而退出路径上的静态析构顺序不可控——在途句柄的关闭可能晚于任何作用域会话对象的
/// 析构，`WSACleanup` 之后再关句柄属平台层面的未定义行为（与 `app::EventSourceRegistry` 单例刻意不
/// 析构同一条裁决）。`detail::Session` 的作用域引用仍照常计数并与本枚叠加，末次清理因此不再发生。
/// @return 运行时可用为 true；Windows 侧 `WSAStartup` 被拒为 false；POSIX 恒 true。
auto runtime_pinned() -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
    static const bool PINNED = session_acquire();
    return PINNED;
#else
    return true;
#endif
}

/// @brief 创建流式套接字（TCP 与本机 IPC 共用一条出口，只差地址族与协议号）。
/// @param family 地址族（`AF_INET` / `AF_INET6` / `AF_UNIX`）。
/// @return 成功为原生句柄；失败为 `net-runtime-failed`（运行时起不来）/
///         `net-platform-unavailable`（本机 IPC 通路在本腿不存在）/ `net-socket-failed`（含 OS 码）。
auto open_socket(int family) -> Result<NativeSocket> {
    if (!runtime_pinned()) {
        // 先报「运行时没起来」而不是让 `socket()` 以 `WSAENETDOWN` 形态报成句柄表故障：两者排查方向
        // 完全不同（winsock DLL / 版本请求 vs 地址族与资源上限）。
        return make_error(ErrorCode::NetRuntimeFailed, "socket runtime unavailable");
    }
    // AF_UNIX 没有传输协议号：填 `IPPROTO_TCP` 在 Linux 上直接 EINVAL，而两族的流式语义一致，
    // 故这里只换协议号而不换出口。
    const int protocol = family == AF_UNIX ? 0 : IPPROTO_TCP;
    const NativeSocket s = ::socket(family, SOCK_STREAM, protocol);
    if (native_is_invalid(s)) {
        const int os_error = native_last_error();
        if (family == AF_UNIX && native_family_unsupported(os_error)) {
            // 「本机 IPC 这条路不存在」与「socket 调用失败」是两件事：前者由 `Capabilities` 提前回答，
            // 落在这一分支才不会被当成句柄表故障去查。
            return make_error(
                ErrorCode::NetPlatformUnavailable,
                "local IPC needs a socket runtime that provides AF_UNIX (os_error=" + std::to_string(os_error) + ")");
        }
        return make_error(ErrorCode::NetSocketFailed,
                          "socket(SOCK_STREAM) failed (os_error=" + std::to_string(os_error) + ")");
    }
    return s;
}

/// @brief 流式套接字的地址族判据。
/// @param endpoint 目标端点。
/// @param allow_local 本机 IPC 形态是否可用：异步传输面为真（同一套 `socket` / `connect` / `accept`
///        序列换成 `AF_UNIX` 即可，状态机不变），同步阻塞面按既有裁决保持 TCP-only（口径见
///        `blocking::StreamSocket::connect` 与 `blocking::Listener::bind` 的说明）。
/// @return 成功为 `AF_INET` / `AF_INET6` / `AF_UNIX`；不合用为 `net-endpoint-invalid`。
auto stream_family_of(const Endpoint &endpoint, bool allow_local) -> Result<int> {
    if (!endpoint.is_valid()) {
        return make_error(ErrorCode::NetEndpointInvalid, "stream sockets need an IPv4 or IPv6 endpoint");
    }
    if (endpoint.is_local_path()) {
        if (!allow_local) {
            return make_error(ErrorCode::NetEndpointInvalid,
                              "the blocking socket face carries TCP streams only; use StreamTransport for local IPC");
        }
        return AF_UNIX;
    }
    return endpoint.family() == AddressFamily::IPv4 ? AF_INET : AF_INET6;
}

/// @brief 按端点地址族创建 TCP 流式套接字（同步阻塞面入口，本机 IPC 形态在此即拒）。
/// @param endpoint 决定 `AF_INET` 还是 `AF_INET6` 的端点。
/// @return 成功为原生句柄；失败为 `net-endpoint-invalid` / `net-socket-failed`。
auto open_socket_for(const Endpoint &endpoint) -> Result<NativeSocket> {
    auto family = stream_family_of(endpoint, false);
    if (!family) {
        return family.error();
    }
    return open_socket(family.value());
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
        const auto n = static_cast<std::ptrdiff_t>(::send(s, p, len, AURORA_SEND_FLAGS));
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

// ---- detail::Session（运行时的作用域引用；公共面已不再暴露本类型）------------------------------

#ifdef AURORA_PLATFORM_WINDOWS
detail::Session::Session() : ok_(session_acquire()) {}
#else
detail::Session::Session() : ok_(true) {}  // POSIX 无运行时初始化概念，会话恒可用
#endif

detail::Session::~Session() {
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

// ---- blocking::StreamSocket -------------------------------------------------

namespace blocking {

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
    const int rcv_rc =
        ::setsockopt(from_handle(handle_), SOL_SOCKET, SO_RCVTIMEO, static_cast<const char *>(option), option_size);
    const int snd_rc =
        ::setsockopt(from_handle(handle_), SOL_SOCKET, SO_SNDTIMEO, static_cast<const char *>(option), option_size);
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

// ---- blocking::Listener -----------------------------------------------------

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

}  // namespace blocking

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
    // WSAPoll 无 EINTR 语义（Windows 不被 POSIX 信号打断），单次调用即可；活跃的 Winsock 运行时随
    // 本文件里创建句柄的每一次调用钉住（见 `runtime_pinned`），因此能拿到句柄就一定能等它。
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
    const detail::Session session;  // Windows：`getaddrinfo` 要求已初始化的 Winsock 运行时
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

/// @brief 组装「选项与套接字当前形态不合用」的冲突原因。
/// @param detail 冲突的现场描述（进 message 模板的 `{detail}`）。
/// @return `net-option-conflict` 错误。
auto option_conflict(std::string_view detail) -> Error {
    return make_error(ErrorCode::NetOptionConflict, ErrorParams{{"detail", std::string{detail}}});
}

/// @brief 下发一枚整数形态的 socket 选项（两平台的 `optval` 宽度同名，故只有一处需要按字节豁免）。
/// @param s 原生句柄。
/// @param level 选项层（`SOL_SOCKET` 或 `IPPROTO_TCP`）。
/// @param option 选项名。
/// @param label 写进诊断文本的选项拼写。
/// @param value 选项值。
/// @return 成功为 OK；失败为 `net-option-failed`（含 OS 原生码）。
auto set_int_option(NativeSocket s, int level, int option, std::string_view label, int value) -> Result<void> {
    const char *const bytes =
        reinterpret_cast<const char *>(&value);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::setsockopt(s, level, option, bytes, static_cast<int>(sizeof(value))) != 0) {
        return net_error(ErrorCode::NetOptionFailed, "setsockopt(" + std::string{label} + ")");
    }
    return Result<void>{};
}

/// @brief 下发 TCP 层的一枚整数选项。
/// @param s 原生句柄。
/// @param option 选项名。
/// @param label 写进诊断文本的选项拼写。
/// @param value 选项值。
/// @return 成功为 OK；失败为 `net-option-failed`（含 OS 原生码）。
auto set_tcp_option(NativeSocket s, int option, std::string_view label, int value) -> Result<void> {
    return set_int_option(s, IPPROTO_TCP, option, label, value);
}

/// @brief 把毫秒意图换算成一枚至少为 1 的秒数（内核的保活与时限类选项都以整秒计）。
/// @param value 调用方置了值的毫秒数。
/// @return 向上取整后的秒数；非正值按 1 秒处理（0 秒的「立即探活」在内核侧没有对应形态）。
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID) || defined(AURORA_PLATFORM_MACOS)
auto seconds_at_least_one(std::chrono::milliseconds value) -> int {
    const auto ms = std::max<long long>(value.count(), 1);
    return static_cast<int>((ms + 999) / 1000);
}
#endif

/// @brief 下发保活调参（空闲时长 / 探测间隔 / 失败次数）。
///
/// 各腿的拼写不同，故按腿各写一条：Linux 与 Android 是 `TCP_KEEPIDLE` / `TCP_KEEPINTVL` /
/// `TCP_KEEPCNT`；BSD 系（macOS）三档都有，只是空闲档叫 `TCP_KEEPALIVE`；Winsock 只有一枚
/// `SIO_KEEPALIVE_VALS`，一次下发前两档，且**没有**失败次数的入口。既不支持的字段不在这里被静默
/// 吞掉——调用方要的是「这条连接多久判死」，缺一档答案就得说缺一档。
/// @param s 原生句柄（`SO_KEEPALIVE` 已置）。
/// @param options 置了值的调参字段。
/// @return 成功为 OK；本腿没有对应入口或系统调用失败为 `net-option-failed`（`detail` 写明缺哪一枚）。
auto apply_keepalive_tuning([[maybe_unused]] NativeSocket s, const NativeOptions &options) -> Result<void> {
#ifdef AURORA_PLATFORM_WINDOWS
    if (options.keep_alive_probes) {
        return make_error(ErrorCode::NetOptionFailed,
                          ErrorParams{{"detail",
                                       "Winsock exposes no per-connection probe-count knob beside "
                                       "SIO_KEEPALIVE_VALS (the retry limit is a system-wide one)"}});
    }
    // 两档都没置时不发起这次 Ioctl：只想要保活本身已由 SO_KEEPALIVE 表达，参数留给内核缺省
    // （空闲 2 小时、间隔 1 秒），把它填成显式值等于替调用方做了一个看不见的决定。
    if (!options.keep_alive_idle && !options.keep_alive_interval) {
        return Result<void>{};
    }
    auto millis = [](const std::optional<std::chrono::milliseconds> &value, long long fallback) {
        return static_cast<u_long>(value ? std::max<long long>(value->count(), 1) : fallback);
    };
    tcp_keepalive request{};
    request.onoff = 1;
    request.keepalivetime = millis(options.keep_alive_idle, 7200000);
    request.keepaliveinterval = millis(options.keep_alive_interval, 1000);
    unsigned long returned = 0;
    // `WSAIoctl` 在不需要传回数据时按 0 长度给出空出参缓冲，这是该接线的固定形态。
    if (::WSAIoctl(s, static_cast<DWORD>(SIO_KEEPALIVE_VALS), &request, static_cast<DWORD>(sizeof(request)), nullptr, 0,
                   &returned, nullptr, nullptr) != 0) {
        return net_error(ErrorCode::NetOptionFailed, "WSAIoctl(SIO_KEEPALIVE_VALS)");
    }
    return Result<void>{};
#elif defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
    if (options.keep_alive_idle) {
        if (auto set = set_tcp_option(s, TCP_KEEPIDLE, "TCP_KEEPIDLE", seconds_at_least_one(*options.keep_alive_idle));
            !set) {
            return set.error();
        }
    }
    if (options.keep_alive_interval) {
        if (auto set =
                set_tcp_option(s, TCP_KEEPINTVL, "TCP_KEEPINTVL", seconds_at_least_one(*options.keep_alive_interval));
            !set) {
            return set.error();
        }
    }
    if (options.keep_alive_probes) {
        if (*options.keep_alive_probes <= 0) {
            return option_conflict("keep_alive_probes needs a positive retry count");
        }
        if (auto set = set_tcp_option(s, TCP_KEEPCNT, "TCP_KEEPCNT", *options.keep_alive_probes); !set) {
            return set.error();
        }
    }
    return Result<void>{};
#elif defined(AURORA_PLATFORM_MACOS)
    // BSD 系的三档都在，只是空闲档不叫 `TCP_KEEPIDLE` 而叫 `TCP_KEEPALIVE`（Linux 那个名字在这里
    // 不存在）；另两档与 Linux 同名。因此本腿与 Linux 腿同样能回答「这条连接多久判死」，不该落到
    // 下面「本平台没有入口」的分支。
    if (options.keep_alive_idle) {
        if (auto set =
                set_tcp_option(s, TCP_KEEPALIVE, "TCP_KEEPALIVE", seconds_at_least_one(*options.keep_alive_idle));
            !set) {
            return set.error();
        }
    }
    if (options.keep_alive_interval) {
        if (auto set =
                set_tcp_option(s, TCP_KEEPINTVL, "TCP_KEEPINTVL", seconds_at_least_one(*options.keep_alive_interval));
            !set) {
            return set.error();
        }
    }
    if (options.keep_alive_probes) {
        if (*options.keep_alive_probes <= 0) {
            return option_conflict("keep_alive_probes needs a positive retry count");
        }
        if (auto set = set_tcp_option(s, TCP_KEEPCNT, "TCP_KEEPCNT", *options.keep_alive_probes); !set) {
            return set.error();
        }
    }
    return Result<void>{};
#else
    // 本腿确实没有逐连接保活调参的入口。但「没有入口」只在**真的要求调参**时才算失败：只开
    // `SO_KEEPALIVE` 的调用方要的就是系统缺省行为，上面已经置好了，此处再报错等于把一项平台缺省
    // 能力说成不支持。
    if (!options.keep_alive_idle && !options.keep_alive_interval && !options.keep_alive_probes) {
        return Result<void>{};
    }
    return make_error(ErrorCode::NetOptionFailed,
                      ErrorParams{{"detail", "this platform leg exposes no per-connection keepalive tuning"}});
#endif
}

/// @brief 逐条应用原生选项——只在套接字创建时调用（bind / connect 之前，`SO_REUSEADDR` 与源地址绑定
///        过了这个时点就无效，故本层不提供「建立后再改」的第二入口）。
///
/// 未置值的字段一条 `setsockopt` 都不发起，内核缺省原样保留。与地址族的配合：`no_delay` 与保活各档
/// 是 TCP 专属，在本机 IPC 流上落 `net-option-conflict` 而**不是静默忽略**——静默忽略会让「以为开了
/// 保活的长连接」等到下一次中间设备超时才现形，那时现场已经没有配置信息可查。
/// @param s 原生句柄。
/// @param family 本句柄的地址族。
/// @param options 调用方置了值的字段。
/// @param allow_source_bind 是否允许 `source`：只有尚未绑定的客户端套接字允许——监听口的绑定目标已由
///        端点给出，已接受的连接则对端已定，两者上的 `source` 都是用不上的字段。
/// @return 成功为 OK；不合用为 `net-option-conflict`，系统调用失败为 `net-option-failed` /
///         `net-bind-failed`（源地址绑定）。
auto apply_socket_options(NativeSocket s, AddressFamily family, const NativeOptions &options, bool allow_source_bind)
    -> Result<void> {
    const bool local_ipc = family == AddressFamily::LocalPath;
    if (options.receive_buffer_bytes) {
        if (*options.receive_buffer_bytes <= 0) {
            return option_conflict("receive_buffer_bytes needs a positive byte count");
        }
        if (auto set = set_int_option(s, SOL_SOCKET, SO_RCVBUF, "SO_RCVBUF", *options.receive_buffer_bytes); !set) {
            return set.error();
        }
    }
    if (options.send_buffer_bytes) {
        if (*options.send_buffer_bytes <= 0) {
            return option_conflict("send_buffer_bytes needs a positive byte count");
        }
        if (auto set = set_int_option(s, SOL_SOCKET, SO_SNDBUF, "SO_SNDBUF", *options.send_buffer_bytes); !set) {
            return set.error();
        }
    }
    if (options.reuse_address) {
        if (auto set = set_int_option(s, SOL_SOCKET, SO_REUSEADDR, "SO_REUSEADDR", *options.reuse_address ? 1 : 0);
            !set) {
            return set.error();
        }
    }
    if (options.linger) {
        if (options.linger->count() < 0) {
            return option_conflict("linger needs a non-negative duration");
        }
        // `SO_LINGER` 的时限单位是整秒，故向上取整：请求 1 ms 的收尾窗口却按 0 秒下发，等于把「等一等」
        // 执行成「立即 RST」，那是与调用方意图相反的两种关闭形态。置值即启用（0 是合法的「立即丢弃」）。
        const auto seconds = static_cast<unsigned int>((std::max<long long>(options.linger->count(), 0) + 999) / 1000);
        // 上限按最窄一条腿（Winsock 的 `unsigned short`）夹：被夹到的口径是 18 小时以上的收尾窗口，
        // 两腿对此都没有一致行为，夹住并报同一枚结构比让某条腿溢出更可信。
        const auto capped = static_cast<unsigned short>(
            std::min(seconds, static_cast<unsigned int>(std::numeric_limits<unsigned short>::max())));
        const ::linger value{.l_onoff = 1, .l_linger = capped};
        // 选项值按结构体整体传字节，这是 `SO_LINGER` 唯一的形态。
        const char *const bytes =
            reinterpret_cast<const char *>(&value);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::setsockopt(s, SOL_SOCKET, SO_LINGER, bytes, static_cast<int>(sizeof(value))) != 0) {
            return net_error(ErrorCode::NetOptionFailed, "setsockopt(SO_LINGER)");
        }
    }
    if (options.no_delay) {
        if (local_ipc) {
            return option_conflict("TCP_NODELAY has no counterpart on a local IPC stream");
        }
        if (auto set = set_tcp_option(s, TCP_NODELAY, "TCP_NODELAY", *options.no_delay ? 1 : 0); !set) {
            return set.error();
        }
    }
    const bool keepalive_tuned = options.keep_alive_idle || options.keep_alive_interval || options.keep_alive_probes;
    if (keepalive_tuned && options.keep_alive_enabled == false) {
        return option_conflict("keepalive tuning while keep_alive_enabled is explicitly false");
    }
    const bool keepalive_wanted = options.keep_alive_enabled.value_or(false) || keepalive_tuned;
    if (keepalive_wanted || options.keep_alive_enabled == false) {
        if (local_ipc) {
            return option_conflict("SO_KEEPALIVE is a TCP option and has no counterpart on a local IPC stream");
        }
        if (auto set = set_int_option(s, SOL_SOCKET, SO_KEEPALIVE, "SO_KEEPALIVE", keepalive_wanted ? 1 : 0); !set) {
            return set.error();
        }
    }
    if (keepalive_tuned) {
        if (auto tuned = apply_keepalive_tuning(s, options); !tuned) {
            return tuned.error();
        }
    }
    if (options.source) {
        if (!allow_source_bind) {
            return option_conflict("a source address needs an unbound client socket; this handle already has one");
        }
        if (local_ipc) {
            return option_conflict("a local IPC stream has no IP source address to bind");
        }
        if (!options.source->is_valid() || options.source->is_local_path()) {
            return make_error(ErrorCode::NetEndpointInvalid, "a source address needs an IPv4 or IPv6 endpoint");
        }
        if (options.source->family() != family) {
            return option_conflict("source address family differs from the connection address family");
        }
        sockaddr_storage storage{};
        auto built = make_address(*options.source, storage);
        if (!built) {
            return built.error();
        }
        const AddrLen length = built.value();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::bind(s, reinterpret_cast<const sockaddr *>(&storage), length) != 0) {
            return net_error(ErrorCode::NetBindFailed, "bind(source=" + options.source->to_string() + ")");
        }
    }
    return Result<void>{};
}

/// @brief 创建一个非阻塞流式套接字并配好原生选项（两条异步入口共用的前半段）。
/// @param endpoint 目标端点（IPv4 / IPv6 或本机 IPC 路径形态）。
/// @param options 原生选项。
/// @param allow_source_bind 是否允许 `source`（见 `apply_native_options`）。
/// @param needs_port 是否要求端口非 0（客户端发起连接要，监听口按 `port == 0` = 系统分配）。
/// @return 成功为已置非阻塞的句柄；失败为上述各码（失败路径已关闭句柄）。
auto open_configured_stream(const Endpoint &endpoint, const NativeOptions &options, bool allow_source_bind,
                            bool needs_port) -> Result<std::intptr_t> {
    auto family = stream_family_of(endpoint, true);
    if (!family) {
        return family.error();
    }
    if (needs_port && !endpoint.is_local_path() && endpoint.port() == 0) {
        return make_error(ErrorCode::NetEndpointInvalid, "a stream client endpoint needs a non-zero port");
    }
    auto opened = open_socket(family.value());
    if (!opened) {
        return opened.error();
    }
    const NativeSocket s = opened.value();
    if (auto applied = apply_socket_options(s, endpoint.family(), options, allow_source_bind); !applied) {
        native_close(s);
        return applied.error();
    }
    if (auto set = make_nonblocking(s); !set) {
        native_close(s);
        return set.error();
    }
    return to_handle(s);
}

}  // namespace

auto detail::open_nonblocking_stream(const Endpoint &endpoint, const NativeOptions &options) -> Result<std::intptr_t> {
    return open_configured_stream(endpoint, options, true, true);
}

auto detail::open_nonblocking_listener(const Endpoint &endpoint, int backlog, const NativeOptions &options)
    -> Result<std::intptr_t> {
    // 监听口的选项必须在 bind 之前下发（`SO_REUSEADDR` 过了这个时点就无效），故这里先建句柄应用选项，
    // 再绑定与挂起。本机 IPC 形态绑到已存在的路径文件上会以 EADDRINUSE 落 `net-bind-failed`：本层
    // 不自动 `unlink()`，删掉别人留下的命名键是一种抢占，而抢占在并发启动的两个进程之间无解。
    auto opened = open_configured_stream(endpoint, options, false, false);
    if (!opened) {
        return opened.error();
    }
    // `open_configured_stream` 交回的是 `intptr_t` 形态句柄（`to_handle` 的出口），还原经 `from_handle`：
    // POSIX 上 `NativeSocket` 窄于 `intptr_t`，直接初始化是实现定义收窄，显式走既定逆转换。
    const NativeSocket s = from_handle(opened.value());
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
    return opened;
}

auto detail::apply_native_options(std::intptr_t handle, const NativeOptions &options) -> Result<void> {
    auto family = handle_family(handle);
    if (!family) {
        return family.error();
    }
    // 已接受的连接没有 `source` 可言（对端已由内核定下），故这里的口径与监听口一致。
    return apply_socket_options(from_handle(handle), family.value(), options, false);
}

auto detail::handle_family(std::intptr_t handle) -> Result<AddressFamily> {
    sockaddr_storage local{};
    AddrLen name_len = sizeof(local);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(from_handle(handle), reinterpret_cast<sockaddr *>(&local), &name_len) != 0) {
        return net_error(ErrorCode::NetSocketFailed, "getsockname");
    }
    if (local.ss_family == AF_UNIX) {
        return AddressFamily::LocalPath;
    }
    if (local.ss_family == AF_INET) {
        return AddressFamily::IPv4;
    }
    if (local.ss_family == AF_INET6) {
        return AddressFamily::IPv6;
    }
    return make_error(ErrorCode::NetSocketFailed, "unknown address family on this handle (family=" +
                                                      std::to_string(static_cast<int>(local.ss_family)) + ")");
}

auto detail::peer_identity(std::intptr_t handle) -> Result<PeerCredentials> {
    PeerCredentials credentials;
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
    // 内核登记的对端凭据是三个 32 位整数（pid、uid、gid）。这里按 ABI 就地声明同形结构而不用 glibc 的
    // `struct ucred`：那个名字只在 `_GNU_SOURCE` 下可见，而本层不能假设消费方的宏。
    struct KernelPeerCred {
        std::uint32_t process_id;
        std::uint32_t user_id;
        std::uint32_t group_id;
    };
    KernelPeerCred kernel{};
    AddrLen length = sizeof(kernel);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockopt(from_handle(handle), SOL_SOCKET, SO_PEERCRED, reinterpret_cast<char *>(&kernel), &length) != 0) {
        return net_error(ErrorCode::NetOptionFailed, "getsockopt(SO_PEERCRED)");
    }
    credentials.anonymous = false;
    credentials.process_id = kernel.process_id;
    credentials.user_id = kernel.user_id;
    credentials.group_id = kernel.group_id;
    return credentials;
#else
    // 其余腿如实回报匿名：Windows 的 AF_UNIX 腿没有向用户态登记对端身份的内核通路，macOS / BSD 的
    // `LOCAL_PEERCRED` 出参结构本轮无验证腿，按记忆猜布局会得到错位的小整数。「匿名」是可判读的事实，
    // 调用方据此走匿名策略分支；把它报成查询失败反而掩盖了「这条腿本来就不给身份」。
    (void)handle;
    return credentials;
#endif
}

auto detail::local_ipc_supported() -> bool {
    // 判据是「本进程现在能建出本机 IPC 的流式套接字」而不是「编译期认得这个宏」：Windows 的 AF_UNIX
    // 随系统版本而来，编译通过不代表运行可用，而 `Capabilities` 是唯一真值源。
    static const bool SUPPORTED = [] {
        // 运行时不必在此自备：`open_socket` 会先钉住它，起不来就以 `net-runtime-failed` 落进下面的
        // 失败分支，探测结论一致。
        auto opened = open_socket(AF_UNIX);
        if (!opened) {
            return false;
        }
        native_close(opened.value());
        return true;
    }();
    return SUPPORTED;
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
    const auto n = static_cast<std::ptrdiff_t>(::send(from_handle(handle), data.data(), len, AURORA_SEND_FLAGS));
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

// ---- 数据报（UDP）腿：非阻塞收发与组播口径 -----------------------------------

namespace {

/// @brief 创建 UDP 数据报套接字。
/// @param family 地址族（`AF_INET` / `AF_INET6`）。
/// @return 成功为原生句柄；失败为 `net-runtime-failed`（运行时起不来）/ `net-socket-failed`（含 OS 码）。
auto open_datagram_socket(int family) -> Result<NativeSocket> {
    if (!runtime_pinned()) {
        return make_error(ErrorCode::NetRuntimeFailed, "socket runtime unavailable");
    }
    const NativeSocket s = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (native_is_invalid(s)) {
        return net_error(ErrorCode::NetSocketFailed, "socket(SOCK_DGRAM, IPPROTO_UDP)");
    }
    return s;
}

/// @brief 数据报地址的可用性判据（本机 IPC 没有数据报形态：公共面的 `DatagramTransport` 按裁决只承载
///        IP 数据报，路径形态没有端口，一次投递的边界语义也无从对齐）。
/// @param endpoint 待检端点。
/// @return 成功为 `AF_INET` / `AF_INET6`；不合用为 `net-endpoint-invalid`。
auto datagram_family_of(const Endpoint &endpoint) -> Result<int> {
    if (!endpoint.is_valid() || endpoint.is_local_path()) {
        return make_error(ErrorCode::NetEndpointInvalid, "datagram sockets need an IPv4 or IPv6 endpoint");
    }
    return endpoint.family() == AddressFamily::IPv4 ? AF_INET : AF_INET6;
}

/// @brief 读回套接字自身的地址族（组播选项的两种结构体形态由此分岔，公共面不必带族参数）。
/// @param handle 目标句柄。
/// @return 成功为族；查询失败或地址族不认识为 `net-socket-failed`。
auto socket_family_of(std::intptr_t handle) -> Result<AddressFamily> {
    sockaddr_storage local{};
    AddrLen name_len = sizeof(local);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(from_handle(handle), reinterpret_cast<sockaddr *>(&local), &name_len) != 0) {
        return net_error(ErrorCode::NetSocketFailed, "getsockname");
    }
    auto resolved = endpoint_from_address(local);
    if (!resolved) {
        return resolved.error();
    }
    return resolved.value().family();
}

/// @brief 下发组播回环开关（IPv4 收 1 字节、IPv6 收 4 字节，两平台同名常量）。
/// @param s 原生句柄。
/// @param family 套接字地址族。
/// @param enabled 是否把本端发出的组播报回投给本端。
/// @return 成功为 OK；失败为 `net-option-failed`（含 OS 原生码）。
auto set_multicast_loop_option(NativeSocket s, AddressFamily family, bool enabled) -> Result<void> {
    // 选项值宽度按族分岔是内核口径而非平台口径，故只有 `reinterpret_cast` 需要豁免。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (family == AddressFamily::IPv4) {
        auto value = static_cast<unsigned char>(enabled ? 1 : 0);
        if (::setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char *>(&value),
                         static_cast<int>(sizeof(value))) != 0) {
            return net_error(ErrorCode::NetOptionFailed, "setsockopt(IP_MULTICAST_LOOP)");
        }
        return Result<void>{};
    }
    auto value = static_cast<unsigned int>(enabled ? 1 : 0);
    if (::setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, reinterpret_cast<const char *>(&value),
                     static_cast<int>(sizeof(value))) != 0) {
        return net_error(ErrorCode::NetOptionFailed, "setsockopt(IPV6_MULTICAST_LOOP)");
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return Result<void>{};
}

/// @brief 单条数据报载荷的可读首地址。
///
/// 空载荷是一条合法的零长数据报，照样要发起系统调用，而 `std::span::data()` 对空 span 合法地返回空指针，
/// 长度 0 时把空指针交给 `send` / `sendto` 在 Winsock 上属未定义用法。
/// @param data 待发送载荷。
/// @return 指向载荷首字节；载荷为空时指向静态零值字节。
auto datagram_payload_pointer(std::span<const char> data) -> const char * { return data.empty() ? "" : data.data(); }

/// @brief 数据报发送结果的统一换算：整条接受、暂无进展、失败（两种发送出口共用一段）。
/// @param n `send` / `sendto` 的返回值。
/// @param operation 失败时写进 message 的操作文本。
/// @return 三态结果。
auto datagram_send_attempt(std::ptrdiff_t n, const std::string &operation) -> detail::IoAttempt {
    if (n >= 0) {
        detail::IoAttempt attempt;
        attempt.bytes = static_cast<std::size_t>(n);  // 0 = 一条零长数据报已整条进入内核
        return attempt;
    }
    if (native_would_block(native_last_error())) {
        detail::IoAttempt attempt;
        attempt.blocked = true;  // 内核发送缓冲满：等下一次可写就绪，不是失败
        return attempt;
    }
    return io_failed(ErrorCode::NetSendFailed, operation);
}

/// @brief 越过内核载荷上界的数据报：两种发送出口共用同一条拒收（不截断、不分片）。
/// @return 三态结果，只填了 `failure`。
auto datagram_too_large_attempt() -> detail::IoAttempt {
    detail::IoAttempt attempt;
    attempt.failure = make_error(ErrorCode::NetBufferLimit,
                                 ErrorParams{{"detail", "datagram exceeds the 65535-byte UDP payload bound"}});
    return attempt;
}

}  // namespace

auto detail::open_nonblocking_datagram(const Endpoint &local) -> Result<std::intptr_t> {
    auto family = datagram_family_of(local);
    if (!family) {
        return family.error();
    }
    auto opened = open_datagram_socket(family.value());
    if (!opened) {
        return opened.error();
    }
    const NativeSocket s = opened.value();
    sockaddr_storage storage{};
    auto built = make_address(local, storage);
    if (!built) {
        native_close(s);
        return built.error();
    }
    const AddrLen address_length = built.value();
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(s, reinterpret_cast<const sockaddr *>(&storage), address_length) != 0) {
        auto err = net_error(ErrorCode::NetBindFailed, "bind(" + local.to_string() + ")");
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

auto detail::associate_datagram_peer(std::intptr_t handle, const Endpoint &peer) -> Result<void> {
    if (peer.port() == 0) {
        return make_error(ErrorCode::NetEndpointInvalid, "a datagram peer endpoint needs a non-zero port");
    }
    auto family = datagram_family_of(peer);
    if (!family) {
        return family.error();
    }
    sockaddr_storage storage{};
    auto built = make_address(peer, storage);
    if (!built) {
        return built.error();
    }
    const AddrLen address_length = built.value();
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::connect(from_handle(handle), reinterpret_cast<const sockaddr *>(&storage), address_length) != 0) {
        return net_error(ErrorCode::NetConnectFailed, "connect(" + peer.to_string() + ")");
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return Result<void>{};
}

auto detail::try_send_to(std::intptr_t handle, std::span<const char> data, const Endpoint &target) -> IoAttempt {
    // 一条数据报不可拆分，故越界的长度不做静默截断而是原样拒收（内核上界 65535 字节）。
    if (data.size() > std::size_t{65535}) {
        return datagram_too_large_attempt();
    }
    sockaddr_storage storage{};
    auto built = make_address(target, storage);
    if (!built) {
        IoAttempt attempt;
        attempt.failure = built.error();
        return attempt;
    }
    const AddrLen address_length = built.value();
    const int len = static_cast<int>(data.size());
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto n = static_cast<std::ptrdiff_t>(::sendto(from_handle(handle), datagram_payload_pointer(data), len, 0,
                                                        reinterpret_cast<const sockaddr *>(&storage), address_length));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return datagram_send_attempt(n, "sendto(" + target.to_string() + ")");
}

auto detail::try_send_datagram(std::intptr_t handle, std::span<const char> data) -> IoAttempt {
    if (data.size() > std::size_t{65535}) {
        return datagram_too_large_attempt();
    }
    const int len = static_cast<int>(data.size());
    const auto n = static_cast<std::ptrdiff_t>(
        ::send(from_handle(handle), datagram_payload_pointer(data), len, AURORA_SEND_FLAGS));
    return datagram_send_attempt(n, "send(connected datagram)");
}

auto detail::try_receive_datagram(std::intptr_t handle, std::span<char> buffer) -> DatagramAttempt {
    if (buffer.empty()) {
        DatagramAttempt attempt;
        attempt.blocked = true;
        return attempt;
    }
    sockaddr_storage source{};
    AddrLen source_len = sizeof(source);
    // 收侧按数据报上界给长度：多出来的缓冲永远用不上，而 65535 之内两平台都落 int 域。
    const int len = static_cast<int>(std::min<std::size_t>(buffer.size(), std::size_t{65535}));
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto n = static_cast<std::ptrdiff_t>(
        ::recvfrom(from_handle(handle), buffer.data(), len, 0, reinterpret_cast<sockaddr *>(&source), &source_len));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (n >= 0) {
        auto resolved = endpoint_from_address(source);
        DatagramAttempt attempt;
        if (!resolved) {
            attempt.failure = resolved.error();  // 认不出的来源族不交出一条没有来源的数据报
            return attempt;
        }
        attempt.bytes = static_cast<std::size_t>(n);  // 0 = 一条零长数据报（UDP 合法消息，不是 EOF）
        attempt.source = resolved.value();
        return attempt;
    }
    const int os_error = native_last_error();
    if (native_would_block(os_error)) {
        DatagramAttempt attempt;
        attempt.blocked = true;
        return attempt;
    }
    DatagramAttempt attempt;
    attempt.failure = net_error(ErrorCode::NetRecvFailed, "recvfrom");
    return attempt;
}

auto detail::set_multicast_membership(std::intptr_t handle, const Endpoint &group, bool joining) -> Result<void> {
    if (!group.is_multicast()) {
        return make_error(ErrorCode::NetEndpointInvalid,
                          "multicast membership needs a group address (got " + group.to_string() + ")");
    }
    auto family = socket_family_of(handle);
    if (!family) {
        return family.error();
    }
    if (group.family() != family.value()) {
        return make_error(ErrorCode::NetEndpointInvalid, "multicast group family differs from the socket family");
    }
    const NativeSocket s = from_handle(handle);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (family.value() == AddressFamily::IPv4) {
        const char *const option = joining ? "IP_ADD_MEMBERSHIP" : "IP_DROP_MEMBERSHIP";
        ip_mreq membership{};
        std::memcpy(&membership.imr_multiaddr.s_addr, group.address_bytes().data(),
                    sizeof(membership.imr_multiaddr.s_addr));
        // 接收网卡取通配值，由内核按路由选择：把网卡做成公共选项会让每条腿的默认值都不一样。
        membership.imr_interface.s_addr = INADDR_ANY;
        if (::setsockopt(s, IPPROTO_IP, joining ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                         reinterpret_cast<const char *>(&membership), static_cast<int>(sizeof(membership))) != 0) {
            return net_error(ErrorCode::NetOptionFailed, std::string{"setsockopt("} + option + ")");
        }
        return Result<void>{};
    }
    const char *const option = joining ? "IPV6_JOIN_GROUP" : "IPV6_LEAVE_GROUP";
    ipv6_mreq membership{};
    std::memcpy(&membership.ipv6mr_multiaddr, group.address_bytes().data(), sizeof(membership.ipv6mr_multiaddr));
    // IPv6 组播的作用域号即网卡索引；`Endpoint::zone()` 只认十进制作用域号，故可直接落位而不查表。
    auto scope = zone_to_scope_id(group.zone());
    if (!scope) {
        return scope.error();
    }
    membership.ipv6mr_interface = scope.value();
    if (::setsockopt(s, IPPROTO_IPV6, joining ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                     reinterpret_cast<const char *>(&membership), static_cast<int>(sizeof(membership))) != 0) {
        return net_error(ErrorCode::NetOptionFailed, std::string{"setsockopt("} + option + ")");
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return Result<void>{};
}

auto detail::set_multicast_loopback(std::intptr_t handle, bool enabled) -> Result<void> {
    auto family = socket_family_of(handle);
    if (!family) {
        return family.error();
    }
    return set_multicast_loop_option(from_handle(handle), family.value(), enabled);
}

auto detail::set_broadcast_allowed(std::intptr_t handle, bool enabled) -> Result<void> {
    auto value = enabled ? 1 : 0;
    // 选项值取 int 是两平台同名宽度（Winsock 的 BOOL 即 int），故只需豁免地址的按字节传参。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::setsockopt(from_handle(handle), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&value),
                     static_cast<int>(sizeof(value))) != 0) {
        return net_error(ErrorCode::NetOptionFailed, "setsockopt(SO_BROADCAST)");
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return Result<void>{};
}

}  // namespace aurora::net
