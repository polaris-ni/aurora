// system_proxy.cpp —— 系统代理快照到决策集合的纯换算（本文件不含任何平台 API 头）。
//
// 口径全部写进 `net/detail/system_proxy.h` 的头注释与 `ProxyEnvironment::from_system()` 的声明处：
// PAC / WPAD 一律报 unsupported 而不回空集合（配了自动配置的机器意图就是「须经代理」，静默当成
// 「没配」会让办公网流量直连出去且无痕）；主机文法只有 `ProxyConfig::parse` 与 `NoProxy::parse`
// 一套，本文件不做第二套解析。

#include "aurora/net/detail/system_proxy.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora::net::detail {
namespace {

/// @brief 生成 `net-proxy-config-invalid`（与配置侧同码同形态：`detail=` 打头）。
/// @param detail 语境文本，须指到第几项。
/// @return 构造好的错误。
auto config_failed(std::string_view detail) -> Error {
    return make_error(ErrorCode::NetProxyConfigInvalid, "detail=system proxy: " + std::string{detail});
}

/// @brief 生成 `net-proxy-unsupported`（本库认得但不实现的形态）。
/// @param detail 说明为什么本库不实现该项。
/// @return 构造好的错误。
auto unsupported(std::string_view detail) -> Error {
    return make_error(ErrorCode::NetProxyUnsupported, "detail=system proxy: " + std::string{detail});
}

/// @brief 去掉内层错误 message 的 `detail=` 前缀，便于拼上层语境（与配置侧同形态）。
/// @param message 内层错误 message。
/// @return 去掉前缀后的原因文本。
auto strip_detail(std::string_view message) -> std::string {
    constexpr std::string_view prefix = "detail=";
    return std::string(message.starts_with(prefix) ? message.substr(prefix.size()) : message);
}

/// @brief 去掉首尾空白（含 `\r`，Windows 配置文本常带）。
/// @param text 原文。
/// @return 空白收敛后的视图。
auto trim(std::string_view text) -> std::string_view {
    const auto blank = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    while (!text.empty() && blank(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief ASCII 小写化（scheme 令牌与槽位判定用；主机规范化不归本层管）。
/// @param text 原文。
/// @return 小写副本。
auto lower(std::string_view text) -> std::string {
    std::string out(text);
    for (auto &ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

/// @brief 按 `;` 与 `,` 切分（两者都在真实系统配置里出现过，空白由 `trim` 收敛）。
/// @param list 原文。
/// @return 项列表（未去空白，交给调用方）。
auto split_items(std::string_view list) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto next = list.find_first_of(";,", start);
        out.push_back(next == std::string_view::npos ? list.substr(start) : list.substr(start, next - start));
        if (next == std::string_view::npos) {
            break;
        }
        start = next + 1;
    }
    return out;
}

/// @brief 一个槽位的落点。
enum class Slot : std::uint8_t {
    Http,  ///< 明文目标首选
    Https,  ///< 安全目标首选
    All,  ///< 两者的共同回退
    Skip,  ///< 本库不说该协议（`ftp=` / `gopher=`），整项略过
};

/// @brief 认一个 scheme 令牌；`socks` 与 `socks4` 是「认得但不实现」，单独回错。
/// @param scheme 已小写的令牌（不含 `=`）。
/// @param slot 输出槽位。
/// @return 令牌合法为 true；未知令牌为 false（`socks` 类由调用方在此之前拦下）。
auto slot_of(std::string_view scheme, Slot &slot) -> bool {
    if (scheme == "http") {
        slot = Slot::Http;
        return true;
    }
    if (scheme == "https") {
        slot = Slot::Https;
        return true;
    }
    if (scheme == "socks5") {
        slot = Slot::All;
        return true;
    }
    if (scheme == "all" || scheme == "other" || scheme == "*") {
        slot = Slot::All;
        return true;
    }
    if (scheme == "ftp" || scheme == "gopher") {
        slot = Slot::Skip;
        return true;
    }
    return false;
}

/// @brief 判断 `=` 之前确实是 scheme 令牌而非畸形文本（令牌字符集有限且非空）。
/// @param head `=` 之前的片段。
/// @return 全部由字母、数字、`-`、`_`、`.`、`*` 组成且非空时为 true。
auto looks_like_scheme(std::string_view head) -> bool {
    if (head.empty()) {
        return false;
    }
    return std::ranges::all_of(head, [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '*';
    });
}

/// @brief 解析例外清单并补上层语境。
///
/// `NoProxy::parse` 的失败码本就是 `net-proxy-config-invalid`，折算不损失诊断力；加语境是为了
/// 让「系统设置里的例外列表写错」与「`NO_PROXY` 写错」在日志里分得开。
/// @param list 例外清单原文。
/// @return 成功为规则集合；畸形为带 `detail=system proxy: ` 语境的错误。
auto parse_bypass(std::string_view list) -> Result<NoProxy> {
    auto parsed = NoProxy::parse(list);
    if (!parsed) {
        return config_failed("exception list: " + strip_detail(parsed.error().message));
    }
    return parsed;
}

/// @brief 把一个 `host[:port]` 文本变成配置（端口缺省口径与配置侧一致：CONNECT 80、SOCKS5 1080）。
///
/// 内层错误码原样保留：`https://proxy:8443` 这类写法是「本库不实现」，折算成配置畸形会把排查方向
/// 带偏（改配置没用，得换形态）；而原因文本必须带上是第几项，否则运维面对一整行服务器表无从定位。
/// @param authority 主机与端口的原文。
/// @param socks5 为真时按 `socks5h` 解析（代理解析目标主机名）。
/// @param source 写进 `ProxyConfig::source()` 的诊断文本。
/// @param context 条目语境（第几项与原文）。
/// @return 成功为配置；失败为同码错误并带条目语境。
auto build_config(std::string_view authority, bool socks5, std::string_view source, const std::string &context)
    -> Result<ProxyConfig> {
    const std::string text = socks5 ? "socks5h://" + std::string{authority} : std::string{authority};
    auto parsed = ProxyConfig::parse(text, source);
    if (parsed) {
        return parsed;
    }
    return make_error(parsed.error().code_enum,
                      "detail=system proxy: " + context + ": " + strip_detail(parsed.error().message));
}

}  // namespace

auto system_proxy_from_snapshot(const SystemProxySnapshot &snapshot) -> Result<ProxyEnvironment> {
    // 两条「配了但本库不执行」的形态先于一切判定：回空集合等于替用户决定「不走了」，而那正是
    // 企业网里最难归因的故障形态。
    if (!snapshot.pac_url.empty()) {
        return unsupported("an auto-configuration script (PAC) is configured and this library never executes it: " +
                           snapshot.pac_url);
    }
    if (snapshot.auto_detect) {
        return unsupported(
            "auto-detection (WPAD) is enabled, which resolves a PAC script; this library never "
            "executes one, so the proxy set cannot be derived");
    }
    if (!snapshot.manual_enabled) {
        ProxyEnvironment empty;  // 系统里没配代理是正常状态：一切直连
        if (!snapshot.bypass_list.empty()) {
            auto exceptions = parse_bypass(snapshot.bypass_list);
            if (!exceptions) {
                return exceptions.error();
            }
            empty.exceptions = std::move(exceptions.value());
        }
        return empty;
    }
    if (snapshot.server_list.empty()) {
        return config_failed("manual proxy is enabled but no proxy server is listed");
    }

    ProxyEnvironment env;
    std::size_t ordinal = 0;
    for (const auto &raw_item : split_items(snapshot.server_list)) {
        const auto item = trim(raw_item);
        if (item.empty()) {
            continue;  // 空项忽略：`;` 收尾与 `A;;B` 都是真实书写
        }
        ++ordinal;
        const std::string context = "entry #" + std::to_string(ordinal) + " '" + std::string{item} + "'";

        std::string scheme;  // 副本而非视图：`lower()` 返回临时量，视图会当场悬空
        std::string_view authority = item;
        const auto equals = item.find('=');
        if (equals != std::string_view::npos) {
            const auto head = item.substr(0, equals);
            if (!looks_like_scheme(head)) {
                return config_failed(context + " has a scheme prefix that is not a scheme token");
            }
            scheme = lower(head);
            authority = trim(item.substr(equals + 1));
            if (authority.empty()) {
                return config_failed(context + " has an empty proxy server part");
            }
        }
        if (scheme == "socks" || scheme == "socks4") {
            // Windows 与 IE 口径里的 `socks` 是 SOCKS4，版本本身在配置里没有表达；猜成 SOCKS5
            // 会让同一份配置在不同机器上连到不同协议的端口上，故报「认得但不实现」。
            return unsupported(context +
                               " is a SOCKS4-era 'socks' entry; only SOCKS5 is implemented, use an "
                               "explicit 'socks5=' entry when the server speaks SOCKS5");
        }
        auto slot = Slot::All;
        if (!scheme.empty() && !slot_of(scheme, slot)) {
            std::string rejected = context + " carries unrecognized scheme token '";
            rejected += scheme;
            rejected += '\'';
            return config_failed(std::move(rejected));
        }
        if (slot == Slot::Skip) {
            continue;  // 本库不发起 FTP / Gopher 连接，该槽位与直连与否无关
        }
        const bool use_socks5 = scheme == "socks5";
        std::string config_source("system:");
        config_source += scheme.empty() ? std::string{"all"} : scheme;
        auto config = build_config(authority, use_socks5, config_source, context);
        if (!config) {
            return config.error();
        }
        auto &target = slot == Slot::Http ? env.http : (slot == Slot::Https ? env.https : env.all);
        if (target.has_value()) {
            return config_failed(context + " duplicates an earlier entry for the same slot");
        }
        target = std::move(config.value());
    }

    if (!snapshot.bypass_list.empty()) {
        auto exceptions = parse_bypass(snapshot.bypass_list);
        if (!exceptions) {
            return exceptions.error();
        }
        env.exceptions = std::move(exceptions.value());
    }
    return env;
}

namespace {

/// @brief 采集钩子的唯一存储点（装入与读取共用同一槽，口径同 `net::detail::lookup_hook`）。
/// @return 槽的可变引用。
auto system_hook_slot() -> SystemProxyFn & {
    static SystemProxyFn slot;
    return slot;
}

}  // namespace

auto set_system_proxy_hook(SystemProxyFn hook) -> void { system_hook_slot() = std::move(hook); }

auto system_proxy_hook() -> const SystemProxyFn & { return system_hook_slot(); }

}  // namespace aurora::net::detail
