// policy.cpp —— 声明式安全策略的判定与进程级默认值（CHG-018 §12 策略件）。
//
// 三条维度在这里一次问完，问的是「这次交换允不允许发生」而不是「怎么发生」：判据只看目标 scheme、主机清单、
// TLS 口径的版本界与本跳的代理决策四份已经定好的事实，因此零套接字、零解析、零时钟，可在任意线程逐值断言。
// 策略一律**只判不改**：它从不改写 `TlsContext`、也从不替调用方补一条白名单——「看起来生效了其实被改了」
// 是这类声明最坏的失效形态。
//
// 进程级默认值是本仓第一枚公共可变全局，取舍记此：安全策略天然属「宿主启动时声明一次、之后各处新建的
// 客户端都该服从」的那类配置，逐客户端设置会让「忘记设」成为默认放行路径。它用互斥量护住一份按值返回的
// `std::optional<NetPolicy>`，读写都在锁内完成、交出的是副本，因此调用方拿到的策略不会被后续 `set` 半途改写。
#include "aurora/net/policy.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/proxy.h"
#include "aurora/net/tls.h"

namespace aurora::net {
namespace {

/// @brief ASCII 小写化（与 `Url` / `Headers` 同一套不受本机 locale 影响的口径）。
/// @param text 原文本。
/// @return 逐字符 `tolower` 后的副本。
std::string ascii_lower(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

/// @brief 主机文本的比较形态：小写 + 去一枚根区尾点。
///
/// 尾点按 DNS 口径与不带点同义（`example.com.` == `example.com`），`Url` 的规范化早已去掉目标侧的尾点，
/// 这里再剥一次是为了让**清单条目**也接受带点写法。只剥一枚，`a..b` 这类畸形不在本层修。
/// @param host 主机文本（注册名或数字字面量）。
/// @return 参与比较的文本。
std::string comparable_host(std::string_view host) {
    std::string out = ascii_lower(host);
    if (out.ends_with('.')) {
        out.pop_back();
    }
    return out;
}

/// @brief TLS 版本的可读文本（只用于错误 detail，不参与任何判定）。
/// @param version 版本档位。
/// @return 形如 `TLS 1.2`；越界档位给出 `TLS (unknown)` 而不是编一个可能误导排查的号。
std::string_view version_text(TlsVersion version) {
    switch (version) {
        case TlsVersion::Tls10:
            return "TLS 1.0";
        case TlsVersion::Tls11:
            return "TLS 1.1";
        case TlsVersion::Tls12:
            return "TLS 1.2";
        case TlsVersion::Tls13:
            return "TLS 1.3";
    }
    return "TLS (unknown)";
}

/// @brief 进程级默认策略的存储：一份值 + 一枚护住它的锁。
///
/// 只在本文件内可见，出口是 `default_policy()` / `set_default_policy()` 两枚按值往返的函数，锁外拿不到引用。
struct DefaultPolicySlot {
    std::mutex mutex;  ///< 护住 `value`；读写都在锁内完成
    std::optional<NetPolicy> value;  ///< 未设 = 各客户端回到 `NetPolicy{}`
};

/// @brief 单条清单条目是否放行某个主机（两侧文本都已归一）。
///
/// 拆成谓词是为了让「逐条试到第一条命中」这件事保持为一个纯判定的量词，而不是带四段提前退出的循环。
/// @param target 归一后的目标主机文本。
/// @param rule 归一后的条目文本。
/// @return 命中为真；空条目恒假（漏配一行不该变成放行一切）。
auto entry_matches(std::string_view target, std::string_view rule) -> bool {
    if (rule.empty()) {
        return false;
    }
    if (rule == "*") {
        return true;  // 唯一通配项：这一维不设限，是显式写下的放弃而不是漏配
    }
    if (rule.front() == '.') {
        // 后缀形态：域本体与任意深层子域都算命中。比尾部时带上那枚点，故 `example.com` 不会被
        // `.xample.com` 这种半截后缀命中。
        return target == rule.substr(1) || target.ends_with(rule);
    }
    return target == rule;
}

/// @brief 唯一的进程级存储点（函数内静态：首次用到才构造，避开静态初始化期）。
/// @return 存储对象的引用。
auto default_slot() -> DefaultPolicySlot & {
    static DefaultPolicySlot slot;
    return slot;
}

}  // namespace

auto NetPolicy::allows_cleartext_host(std::string_view host) const -> bool {
    const std::string target = comparable_host(host);
    return std::ranges::any_of(allow_cleartext_hosts,
                               [&](const std::string &entry) { return entry_matches(target, comparable_host(entry)); });
}

auto NetPolicy::evaluate(const Url &target, const ProxyConfig &proxy, const TlsContext &tls) const -> Result<void> {
    const std::string host = comparable_host(target.host());
    if (!target.is_secure() && !allows_cleartext_host(target.host())) {
        // detail 里只放目标本身的文本：清单可能很长，整份抄进错误消息既不现实也可能泄露内部主机名。
        return Result<void>{make_error(ErrorCode::NetPolicyViolated, "cleartext target " +
                                                                         std::string{target.scheme()} + "://" + host +
                                                                         " is not listed in allow_cleartext_hosts")};
    }
    if (target.is_secure() && (tls.min_version < min_tls_version ||
                               (tls.max_version.has_value() && tls.max_version.value() < min_tls_version))) {
        // 只判不改：上下文允许的版本集合整个必须落在下限之上，否则「这次握手谈到的版本」取决于对端，
        // 策略就无法在发起前给出承诺。两枚配置都点名，冲突现场一眼可读。
        return Result<void>{make_error(
            ErrorCode::NetPolicyViolated,
            "TlsContext permits " + std::string{version_text(tls.min_version)} +
                (tls.max_version.has_value() ? " to " + std::string{version_text(tls.max_version.value())} : "") +
                " while the policy floor is " + std::string{version_text(min_tls_version)})};
    }
    if (require_proxy && proxy.is_direct()) {
        return Result<void>{make_error(ErrorCode::NetPolicyViolated, "policy requires a proxy but the hop to " +
                                                                         std::string{target.scheme()} + "://" + host +
                                                                         " resolves direct")};
    }
    return Result<void>{};
}

auto default_policy() -> std::optional<NetPolicy> {
    DefaultPolicySlot &slot = default_slot();
    const std::scoped_lock lock{slot.mutex};
    return slot.value;
}

auto set_default_policy(std::optional<NetPolicy> policy) -> void {
    DefaultPolicySlot &slot = default_slot();
    const std::scoped_lock lock{slot.mutex};
    slot.value = std::move(policy);
}

}  // namespace aurora::net
