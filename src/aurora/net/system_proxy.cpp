// system_proxy.cpp —— 系统代理设置的平台采集腿。
//
// 本文件把「操作系统里配了什么」压成 `net::detail::SystemProxySnapshot` 的几个文本字段，
// 判据一律不在这里：文法、槽位、PAC/WPAD 分岔全部由 `net/detail/system_proxy.cpp` 的纯换算承担，
// 于是本文件可以做到的最小——两条腿各一段采集 + 一次编码转换，其余平台的腿直接回空值。
//
// 三条落码纪律：
//   - **只取地址与例外**：本腿绝不执行自动配置脚本，也不发起 WPAD 查询；采集到的脚本 URL 只是
//     一段文本，交由换算层报 `net-proxy-unsupported`。
//   - **「读不到」不等于「没配」**：通道存在而被拒绝（注册表访问失败、动态存储读取失败）报
//     `net-proxy-config-unreadable`；只有本腿根本没有系统代理概念时才回空值。
//   - **平台头不外泄**：`<winhttp.h>` 与 `<SystemConfiguration/…>` 只出现在本 TU，快照是唯一的出口类型。

#include "aurora/net/detail/system_proxy.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/core/result.h"

#ifdef AURORA_PLATFORM_WINDOWS

#ifndef NOMINMAX  // NOLINT(readability-identifier-naming)
#define NOMINMAX  // NOLINT(readability-identifier-naming)
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
// `winhttp.h` 依赖 `windows.h` 先行的基础类型定义，顺序不可交换。
#include <winhttp.h>
// clang-format on

#include <cwchar>

#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
#pragma comment(lib, "winhttp.lib")
#endif

namespace aurora::net::detail {
namespace {

/// @brief 构造 `net-proxy-config-unreadable`（通道在、读取被拒）。
/// @param detail 平台侧原因文本（ASCII）。
/// @return 表驱动 Error。
auto unreadable(std::string detail) -> Error {
    return make_error(ErrorCode::NetProxyConfigUnreadable, "detail=system proxy: " + std::move(detail));
}

/// @brief 宽字符配置文本转 UTF-8（WinINet / WinHTTP 的设置一律以 UTF-16 存放）。
/// @param text 以 NUL 结尾的宽字符串；空指针按空串处理。
/// @return 转换结果；转换失败回空串（等价于该项未配置，比塞进乱码文本更可判定）。
auto utf16_to_utf8(const wchar_t *text) -> std::string {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const auto length = static_cast<int>(::wcslen(text));
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        out.clear();
    } else {
        out.resize(static_cast<std::size_t>(written));
    }
    return out;
}

/// @brief 释放 `WinHttpGetIEProxyConfigForCurrentUser` 分配的四个缓冲区。
/// @param config 已由系统填充（或半填充）的结构；空指针项跳过。
/// @note 口径来自 MSDN：这四段字符串由系统分配，必须以 `GlobalFree` 归还。
auto free_strings(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG &config) -> void {
    for (LPWSTR *slot : {&config.lpszAutoConfigUrl, &config.lpszProxy, &config.lpszProxyBypass}) {
        if (*slot != nullptr) {
            ::GlobalFree(*slot);
            *slot = nullptr;
        }
    }
}

}  // namespace

auto platform_system_proxy() -> Result<std::optional<SystemProxySnapshot>> {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (::WinHttpGetIEProxyConfigForCurrentUser(&config) == FALSE) {
        const DWORD last_error = ::GetLastError();
        free_strings(config);
        // 读失败与「用户分区里没有 IE 代理设置」在 WinHTTP 的返回值上是同一件事，但代价不同：
        // 猜成「没配」会让策略封锁注册表的机器上所有流量直连出去，故按可重试的读取失败回报。
        return unreadable("WinHttpGetIEProxyConfigForCurrentUser failed with system error code " +
                          std::to_string(static_cast<std::uint32_t>(last_error)));
    }
    SystemProxySnapshot snapshot;
    snapshot.pac_url = utf16_to_utf8(config.lpszAutoConfigUrl);
    snapshot.server_list = utf16_to_utf8(config.lpszProxy);
    snapshot.bypass_list = utf16_to_utf8(config.lpszProxyBypass);
    // `fAutoDetect` 有三态：`TRUE` = 开了自动探测，`FALSE` = 明确关闭，`-1` = 用户没表达过偏好。
    // 只有前者才构成「意图须经代理」；把三态压成布尔会让默认镜像上的机器全被判成 WPAD 配置。
    snapshot.auto_detect = config.fAutoDetect == TRUE;
    snapshot.manual_enabled = !snapshot.server_list.empty();
    free_strings(config);
    return std::optional<SystemProxySnapshot>{std::move(snapshot)};
}

}  // namespace aurora::net::detail

#elif defined(AURORA_PLATFORM_MACOS)

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

namespace aurora::net::detail {
namespace {

/// @brief 构造 `net-proxy-config-unreadable`（通道在、读取被拒）。
/// @param detail 平台侧原因文本（ASCII）。
/// @return 表驱动 Error。
auto unreadable(std::string detail) -> Error {
    return make_error(ErrorCode::NetProxyConfigUnreadable, "detail=system proxy: " + std::move(detail));
}

/// @brief CF 对象的手动释放器（本文件按非 ARC 的 C++ TU 编译，`Copy`/`Create` 规则须自己收尾）。
class CfHandle {
  public:
    /// @brief 接管一个引用计数对象。
    /// @param object 对象引用；空指针按「无需释放」处理。
    explicit CfHandle(CFTypeRef object) : object_(object) {}
    CfHandle(const CfHandle &) = delete;
    auto operator=(const CfHandle &) -> CfHandle & = delete;
    CfHandle(CfHandle &&) = delete;
    auto operator=(CfHandle &&) -> CfHandle & = delete;
    /// @brief 归还接管时的引用。
    ~CfHandle() {
        if (object_ != nullptr) {
            ::CFRelease(object_);
        }
    }

    /// @brief 取回原始引用（仅本类用途：交给 CF 的读取函数）。
    /// @return 构造时传入的对象引用。
    auto get() const -> CFTypeRef { return object_; }

  private:
    CFTypeRef object_{nullptr};
};

/// @brief 读一个字符串键。
/// @param dictionary 代理设置字典。
/// @param key 键（schema 常量）。
/// @param out 输出 UTF-8 文本；键缺失或类型不符时不写。
/// @return 读到为 true。
auto read_string(CFDictionaryRef dictionary, CFStringRef key, std::string &out) -> bool {
    const void *raw = dictionary == nullptr ? nullptr : ::CFDictionaryGetValue(dictionary, key);
    if (raw == nullptr || ::CFGetTypeID(static_cast<CFTypeRef>(raw)) != ::CFStringGetTypeID()) {
        return false;
    }
    const auto value = static_cast<CFStringRef>(raw);
    const CFIndex length = ::CFStringGetLength(value);
    if (length <= 0) {
        out.clear();
        return true;
    }
    const CFIndex capacity = ::CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
    if (capacity <= 0) {
        return false;
    }
    std::vector<char> buffer(static_cast<std::size_t>(capacity) + 1U, '\0');
    if (::CFStringGetCString(value, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8) == 0) {
        return false;
    }
    out.assign(buffer.data());
    return true;
}

/// @brief 读一个整数键（enable 标志与端口都是 CFNumber）。
/// @param dictionary 代理设置字典。
/// @param key 键。
/// @param out 输出整数值。
/// @return 存在且为数字时为 true。
auto read_number(CFDictionaryRef dictionary, CFStringRef key, std::int32_t &out) -> bool {
    const void *raw = dictionary == nullptr ? nullptr : ::CFDictionaryGetValue(dictionary, key);
    if (raw == nullptr || ::CFGetTypeID(static_cast<CFTypeRef>(raw)) != ::CFNumberGetTypeID()) {
        return false;
    }
    return ::CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberSInt32Type, &out) != 0;
}

/// @brief 读 `ExceptionsList` 数组并拼成 `;` 分隔的例外清单。
/// @param dictionary 代理设置字典。
/// @return 拼接文本；键缺失或类型不符时为空。
auto read_exceptions(CFDictionaryRef dictionary) -> std::string {
    const void *raw =
        dictionary == nullptr ? nullptr : ::CFDictionaryGetValue(dictionary, kSCPropNetProxiesExceptionsList);
    if (raw == nullptr || ::CFGetTypeID(static_cast<CFTypeRef>(raw)) != ::CFArrayGetTypeID()) {
        return {};
    }
    const auto list = static_cast<CFArrayRef>(raw);
    std::string out;
    for (CFIndex index = 0; index < ::CFArrayGetCount(list); ++index) {
        const void *element = ::CFArrayGetValueAtIndex(list, index);
        if (element == nullptr || ::CFGetTypeID(static_cast<CFTypeRef>(element)) != ::CFStringGetTypeID()) {
            continue;  // 数组项由系统持有且非所有权移交，按索引取即可
        }
        const auto value = static_cast<CFStringRef>(element);
        const CFIndex capacity = ::CFStringGetMaximumSizeForEncoding(::CFStringGetLength(value), kCFStringEncodingUTF8);
        if (capacity <= 0) {
            continue;
        }
        std::vector<char> buffer(static_cast<std::size_t>(capacity) + 1U, '\0');
        if (::CFStringGetCString(value, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8) ==
            0) {
            continue;
        }
        if (!out.empty()) {
            out += ';';
        }
        out += buffer.data();
    }
    return out;
}

/// @brief 追加一个 `scheme=host[:port]` 项。
/// @param out 服务器表文本（就地累加）。
/// @param scheme 槽位令牌。
/// @param host 主机文本。
/// @param port 端口；`0` = 系统里没写端口，交给配置侧的缺省口径。
auto append_entry(std::string &out, std::string_view scheme, const std::string &host, std::int32_t port) -> void {
    if (!out.empty()) {
        out += ';';
    }
    out += scheme;
    out += '=';
    out += host;
    if (port > 0) {
        out += ':';
        out += std::to_string(port);
    }
}

}  // namespace

auto platform_system_proxy() -> Result<std::optional<SystemProxySnapshot>> {
    const CfHandle handle(::SCDynamicStoreCopyProxies(nullptr));
    if (handle.get() == nullptr) {
        // 动态存储不可达（网络配置服务没起来、沙箱拒绝）：通道在但读不到，不是「没配」。
        return unreadable("SCDynamicStoreCopyProxies returned no dictionary");
    }
    const auto dictionary = static_cast<CFDictionaryRef>(handle.get());

    SystemProxySnapshot snapshot;
    snapshot.bypass_list = read_exceptions(dictionary);
    std::string pac_url;
    std::int32_t pac_enable = 0;
    std::int32_t wpad_enable = 0;
    if (read_number(dictionary, kSCPropNetProxiesProxyAutoConfigEnable, pac_enable) && pac_enable != 0) {
        (void)read_string(dictionary, kSCPropNetProxiesProxyAutoConfigURLString, pac_url);
    }
    if (read_number(dictionary, kSCPropNetProxiesProxyAutoDiscoveryEnable, wpad_enable) && wpad_enable != 0) {
        snapshot.auto_detect = true;
    }
    snapshot.pac_url = std::move(pac_url);

    std::int32_t http_enable = 0;
    std::int32_t https_enable = 0;
    std::int32_t socks_enable = 0;
    std::string http_host;
    std::string https_host;
    std::string socks_host;
    std::int32_t http_port = 0;
    std::int32_t https_port = 0;
    std::int32_t socks_port = 0;
    (void)read_number(dictionary, kSCPropNetProxiesHTTPEnable, http_enable);
    (void)read_number(dictionary, kSCPropNetProxiesHTTPSEnable, https_enable);
    (void)read_number(dictionary, kSCPropNetProxiesSOCKSEnable, socks_enable);
    const bool has_http = http_enable != 0 && read_string(dictionary, kSCPropNetProxiesHTTPProxy, http_host);
    const bool has_https = https_enable != 0 && read_string(dictionary, kSCPropNetProxiesHTTPSProxy, https_host);
    const bool has_socks = socks_enable != 0 && read_string(dictionary, kSCPropNetProxiesSOCKSProxy, socks_host);
    (void)read_number(dictionary, kSCPropNetProxiesHTTPPort, http_port);
    (void)read_number(dictionary, kSCPropNetProxiesHTTPSPort, https_port);
    (void)read_number(dictionary, kSCPropNetProxiesSOCKSPort, socks_port);
    if (has_http) {
        append_entry(snapshot.server_list, "http", http_host, http_port);
    }
    if (has_https) {
        append_entry(snapshot.server_list, "https", https_host, https_port);
    }
    // macOS 的 SOCKS 代理由系统在服务端做名字解析（本地根本不查 DNS），故映射为「代理解析」形态。
    if (has_socks) {
        append_entry(snapshot.server_list, "socks5", socks_host, socks_port);
    }
    snapshot.manual_enabled = has_http || has_https || has_socks;
    return std::optional<SystemProxySnapshot>{std::move(snapshot)};
}

}  // namespace aurora::net::detail

#else

namespace aurora::net::detail {

auto platform_system_proxy() -> Result<std::optional<SystemProxySnapshot>> {
    // Linux 与浏览器腿没有「系统代理」这件事：桌面环境的代理项本身就是那几个环境变量，
    // 回空值即「本腿没有该通道」，由 `from_environment_or_system()` 按环境变量口径继续。
    return std::optional<SystemProxySnapshot>{};
}

}  // namespace aurora::net::detail

#endif
