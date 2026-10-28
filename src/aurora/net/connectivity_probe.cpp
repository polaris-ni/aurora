// connectivity_probe.cpp — 链路态采集腿：平台分派与统一读数。
//
// 本文件是全仓**唯一**为链路态引入平台宏的地方。但 Windows 那条腿的 socket 头依赖不在此展开：
// 查询实现写在既有的 `src/aurora/net/socket.cpp` 内（平台 socket 头唯一落点），本文件只见
// `optional<bool>` 的签名——「平台 socket 头唯一落点」这条不变量因此不为链路态开口子。
//
// 两条硬约束：① 只出**同步**查询，不注册回调、不建线程；② **零出站 I/O**，只读 OS 表 / 伪文件 /
// 动态存储，绝不 `connect` / `sendto` / `getaddrinfo`。
#include "aurora/net/detail/connectivity_probe.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#endif

namespace aurora::net::detail {
namespace {

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
/// @brief 路由标志取自 Linux `linux/rtnetlink.h`（`RTF_UP` = 0x0001、`RTF_REJECT` = 0x0200）。
///
/// 不引内核头：那会把平台 socket 头带进采集腿之外的文件。值在此就地写定并注明出处。
constexpr std::uint32_t k_rtf_up = 0x0001U;
constexpr std::uint32_t k_rtf_reject = 0x0200U;
/// @brief `IFF_UP` 取自 Linux `linux/if.h`；`sysfs` 的 `flags` 以十六进制文本给出。
constexpr std::uint32_t k_iff_up = 0x0001U;
/// @brief `/proc/net/ipv6_route` 里目的地址的十六进制字符数（128 位）。
constexpr std::size_t k_ipv6_destination_hex = 32U;
/// @brief sysfs 接口目录前缀（路径常量，非本机特定）。
constexpr std::string_view k_sysfs_net_dir = "/sys/class/net/";

/// @brief 接口状态三值：可用 / 明确不可用 / 读不到。
enum class InterfaceStatus { Operational, NotOperational, Unreadable };

/// @brief 路由表读取结果：缺省路由所经的接口名 + 本文件是否读得到。
struct RouteCandidates {
    std::vector<std::string> interfaces{};  ///< 候选接口名（去重）
    bool readable = false;  ///< 本文件是否读得到（空文件 = 读得到但无缺省路由）
};

/// @brief 去掉首尾空白（`/proc` 与 `/sys` 的值尾部常带换行）。
/// @param text 原始文本。
/// @return 去空白后的子视图。
[[nodiscard]] auto trim(std::string_view text) -> std::string_view {
    const auto is_space = [](const char c) -> bool {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    std::size_t begin = 0;
    while (begin < text.size() && is_space(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && is_space(text[end - 1U])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// @brief 把一段十六进制文本译成整数。
///
/// `sysfs` 的 `flags` 以 `0x` 前缀给出（如 `0x1003`），`/proc/net/*` 的字段不带前缀，
/// 故前缀可有可无。
/// @param text 待解析文本（可含前后空白与可选 `0x` / `0X` 前缀）。
/// @return 成功为值；空 / 非法字符 / 溢出为空值。
[[nodiscard]] auto parse_hex(std::string_view text) -> std::optional<std::uint32_t> {
    const auto body = trim(text);
    if (body.size() > 2U && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
        return parse_hex(body.substr(2U));
    }
    if (body.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char unit : body) {
        std::uint32_t digit = 0;
        if (unit >= '0' && unit <= '9') {
            digit = static_cast<std::uint32_t>(unit - '0');
        } else if (unit >= 'a' && unit <= 'f') {
            digit = static_cast<std::uint32_t>((unit - 'a') + 10);
        } else if (unit >= 'A' && unit <= 'F') {
            digit = static_cast<std::uint32_t>((unit - 'A') + 10);
        } else {
            return std::nullopt;  // 不是十六进制 ⇒ 不猜，交回「读不到」
        }
        value = (value * 16U) + digit;
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

/// @brief 读一枚小文本文件的全部内容。
/// @param path 待读取路径。
/// @return 读得到为内容；打不开为空值。
[[nodiscard]] auto read_text_file(const std::string &path) -> std::optional<std::string> {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string out;
    std::array<char, 512> buffer{};
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0) {
        out.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    return out;
}

/// @brief 按接口名问 sysfs：这条链路是否 operational。
///
/// `operstate` 聚合规则写死：`up` 与 `unknown` 视为 operational；`down` / `dormant` /
/// `lowerlayerdown` / `notpresent` / `testing` 一律不视为。`flags` 须含 `IFF_UP`。
/// 任一文件读不到 ⇒ 判「读不到」，**绝不**把读不到当成 down。
/// @param name 接口名（如 `eth0`）。
/// @return 可用 / 明确不可用 / 读不到。
[[nodiscard]] auto interface_is_operational(const std::string &name) -> InterfaceStatus {
    if (name.empty() || name == "lo") {
        return InterfaceStatus::NotOperational;  // 回环一律排除（确定不可用，非读不到）
    }
    const std::string base = std::string{k_sysfs_net_dir} + name + "/";
    const auto operstate = read_text_file(base + "operstate");
    if (!operstate) {
        return InterfaceStatus::Unreadable;
    }
    const auto state = trim(*operstate);
    if (state != "up" && state != "unknown") {
        return InterfaceStatus::NotOperational;
    }
    const auto raw_flags = read_text_file(base + "flags");
    if (!raw_flags) {
        return InterfaceStatus::Unreadable;
    }
    const auto flags = parse_hex(*raw_flags);
    if (!flags) {
        return InterfaceStatus::Unreadable;
    }
    return (*flags & k_iff_up) != 0U ? InterfaceStatus::Operational : InterfaceStatus::NotOperational;
}

/// @brief 读 `/proc/net/route`，收 IPv4 缺省路由所经的接口名。
///
/// 判据：目的地址全零、标志含 `RTF_UP` 且不含 `RTF_REJECT`；首行为表头，跳过。
/// @return 候选接口名与「本文件是否读得到」。
[[nodiscard]] auto ipv4_default_route_interfaces() -> RouteCandidates {
    RouteCandidates out;
    const auto text = read_text_file("/proc/net/route");
    if (!text) {
        return out;  // 文件不可读 ⇒ readable 保持 false
    }
    out.readable = true;
    std::istringstream stream{*text};
    std::string line;
    std::getline(stream, line);  // 跳表头
    while (std::getline(stream, line)) {
        std::istringstream row{line};
        std::string iface;
        std::string destination;
        std::string gateway;
        std::string flags_text;
        if (!(row >> iface >> destination >> gateway >> flags_text)) {
            continue;  // 行格式不合 ⇒ 跳过，不猜
        }
        if (trim(destination) != "00000000") {
            continue;  // 非缺省路由
        }
        const auto flags = parse_hex(flags_text);
        if (!flags || (*flags & k_rtf_up) == 0U || (*flags & k_rtf_reject) != 0U) {
            continue;  // 未启用或被黑洞 ⇒ 不算能出去
        }
        if (std::ranges::find(out.interfaces, iface) == out.interfaces.end()) {
            out.interfaces.emplace_back(iface);
        }
    }
    return out;
}

/// @brief 读 `/proc/net/ipv6_route`，收 IPv6 缺省路由所经的接口名。
///
/// 判据：目的地址全零、前缀长为 0、标志含 `RTF_UP`。该文件无表头行。
/// @return 候选接口名与「本文件是否读得到」。
[[nodiscard]] auto ipv6_default_route_interfaces() -> RouteCandidates {
    RouteCandidates out;
    const auto text = read_text_file("/proc/net/ipv6_route");
    if (!text) {
        return out;
    }
    out.readable = true;
    std::istringstream stream{*text};
    std::string line;
    std::vector<std::string> fields;
    while (std::getline(stream, line)) {
        fields.clear();
        std::istringstream row{line};
        for (std::string field; row >> field;) {
            fields.push_back(field);
        }
        // 列序：目的地址 / 前缀长 / 源地址 / 源前缀长 / 下一跳 / metric / ref / use / flags / 设备名
        if (fields.size() < 10U) {
            continue;
        }
        const auto destination = trim(fields.at(0));
        if (destination.size() != k_ipv6_destination_hex) {
            continue;  // 列数 / 列宽对不上 ⇒ 跳过
        }
        bool all_zero = true;
        for (const char unit : destination) {
            if (unit != '0') {
                all_zero = false;
                break;
            }
        }
        if (!all_zero) {
            continue;  // 目的地址非全零 ⇒ 非缺省路由
        }
        const auto prefix_length = parse_hex(fields.at(1));
        if (!prefix_length || *prefix_length != 0U) {
            continue;  // 前缀长非 0 ⇒ 非缺省路由
        }
        const auto flags = parse_hex(fields.at(8));
        if (!flags || (*flags & k_rtf_up) == 0U) {
            continue;  // 未启用 ⇒ 不算能出去
        }
        if (std::ranges::find(out.interfaces, fields.at(9)) == out.interfaces.end()) {
            out.interfaces.emplace_back(fields.at(9));
        }
    }
    return out;
}

/// @brief Linux 腿的统一判据：存在经非回环、operational 接口的缺省路由。
///
/// 两族文件都读得到但都无可用缺省路由 ⇒ offline；有缺省路由而其接口状态读不到 ⇒ 读不到；
/// 接口状态读得到但非 operational 的路由不算，若再无可用路由且两族文件都可读 ⇒ offline。
/// @return 有 / 没有 / 读不到。
[[nodiscard]] auto linux_default_route() -> std::optional<bool> {
    const auto ipv4 = ipv4_default_route_interfaces();
    const auto ipv6 = ipv6_default_route_interfaces();
    // 任一文件不可读不算终局：另一族读得到且有可用路由时仍判 online（下面的循环会命中）。
    bool saw_unreadable = false;
    for (const auto &name : ipv4.interfaces) {
        const auto status = interface_is_operational(name);
        if (status == InterfaceStatus::Operational) {
            return true;
        }
        if (status == InterfaceStatus::Unreadable) {
            saw_unreadable = true;
        }
    }
    for (const auto &name : ipv6.interfaces) {
        const auto status = interface_is_operational(name);
        if (status == InterfaceStatus::Operational) {
            return true;
        }
        if (status == InterfaceStatus::Unreadable) {
            saw_unreadable = true;
        }
    }
    if (saw_unreadable) {
        // 有候选路由但其接口状态读不到 ⇒ 无从判决；先于「两族都可读」返回，绝不折算成离线。
        return std::nullopt;
    }
    if (ipv4.readable && ipv6.readable) {
        return false;  // 两族都可读且都无可用缺省路由 ⇒ 明确离线
    }
    return std::nullopt;  // 文件不可读 ⇒ 无从判决
}
#endif

}  // namespace

auto compiled_source() -> ConnectivitySource {
#ifdef AURORA_PLATFORM_WINDOWS
    return ConnectivitySource::IpHelper;
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    return ConnectivitySource::LinkFiles;
#else
    // macOS 的 `SCDynamicStore` 腿归另一位 Agent 落地，桥落地前如实缺位；wasm 无 BSD socket 语义。
    return ConnectivitySource::None;
#endif
}

auto has_evidence_path() -> bool {
#ifdef AURORA_PLATFORM_WASM
    // wasm 的 fetch 腿不经建连出口 ⇒ 对链路态恒无贡献，也没有可采的路由事实。
    return false;
#else
    return true;
#endif
}

auto probe_default_route() -> ProbeReading {
    ProbeReading reading;
#ifdef AURORA_PLATFORM_WINDOWS
    reading.online = probe_default_route_win32();
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    reading.online = linux_default_route();
#else
    reading.online = std::nullopt;
#endif
    return reading;
}

}  // namespace aurora::net::detail
