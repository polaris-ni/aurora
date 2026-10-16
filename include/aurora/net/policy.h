#pragma once

/// @brief 声明式网络安全策略：明文白名单、TLS 版本下限、是否必须经代理。
/// @file net/policy.h
///
/// 本头回答一个问题：**「这次交换允不允许发生」**——它是一个纯判定的值类型，不碰套接字、不记状态，
/// 判据全部在发起之前兑现：不合规矩的那一跳**一条连接都不发起**，也不写出任何字节。形态对齐 iOS 的
/// `NSAppTransportSecurity` 与 Android 的 `network_security_config`（声明一次、由框架逐请求执行），而不是
/// 「调用方自己记得别发 http」那种约定。
///
/// 三维判据与各自的缺省：
/// - **明文（`http://`）默认拒绝**：`allow_cleartext_hosts` 缺省为空 = 一切明文都拒。这不是「功能没做完」
///   的保守开关，而是本库对「凭据与用户数据会不会顺手中转」的默认回答——与 iOS ATS 同一条取舍。
/// - **TLS 版本下限缺省 1.2**：只判不改。策略从不改写 `TlsContext`，它检查那份上下文允许的版本集合是否
///   整个落在下限之上；上下文能把版本谈到下限以下即视为冲突、直接拒，并在 detail 里同时点名两枚配置。
///   「静默把下限抬到策略值」是假安全：调用方以为自己的 `TlsContext` 说了算，实际有一处看不见的改写。
/// - **是否必须经代理**：`require_proxy` 缺省假。为真时任何判定为直连的目标都被拒（`NO_PROXY` 例外命中同样
///   算直连，因此同样被拒——那是该条声明的字面含义，不是例外）。
///
/// 主机匹配只有两种写法，且**与 `NoProxy` 的域名规则刻意不同**：
/// - 不带前导点的条目 = 注册名**全等**匹配（规范化后 ASCII 小写、去根区尾点）。
/// - 带**一枚**前导点的条目 = 该域**及其任意深层子域**（`.example.com` 收 `example.com` 与
///   `a.b.example.com`）。
///
/// 差别是有意的：`NO_PROXY` 里 `.internal` 与 `internal` 同义，因为那是「少打几个字」的运维便利性；而一份
/// 安全白名单里 `example.com` 静默放行 `evil.example.com` 是这一类配置最常见的失误形态，后缀在这里必须是
/// 显式选择的动作。清单只认主机名：不做 CIDR、不带端口限定、不认 `<local>` 那类平台令牌（数字字面量按文本
/// 全等比较即可，IPv6 用 `Url` 规范化后的无括号形态）。
///
/// **没有隐含的回环例外**。`127.0.0.1` / `::1` / `localhost` 与任何其它主机一样受明文维度约束：本机服务
/// 同样是明文信道，把例外藏在库里等于让「策略放行了一切」与「实际放行了一切加本机」这两种配置在调用方
/// 眼里长得一样。需要连本机明文服务就把它列进清单，那行声明就是这条决定的可见记录。
///
/// 生效次序（进程级默认 + 逐客户端覆盖，两级）：`Client` 若经 `Client::policy()` 显式设过策略，用它；否则
/// 取 `default_policy()`；进程级也没设时取 `NetPolicy{}`，即上面那组保守缺省。判定在每一跳各问一次，因此
/// 重定向的下一跳换主机、换 scheme 照样受管；缓存命中路径同样先判后交付（少打一次网络不等于少一道闸）。
///
/// 证书固定（SPKI pin）**不在本类型里**：它随 TLS 切片落在 `TlsContext::pin_spki`（见 `net/tls.h`），
/// 与信任根、校验开关同属一次握手的口径。策略层因此只管「能不能谈、谈的话版本下限多少」，不管谈成之后
/// 认哪张证书。
///
/// @code
/// aurora::net::NetPolicy policy;
/// policy.allow_cleartext_hosts = {"127.0.0.1", ".intra.example.com"};
/// aurora::net::set_default_policy(policy);  // 进程级：此后未显式设策略的 Client 都用它
///
/// aurora::net::http::Client strict;
/// strict.policy(aurora::net::NetPolicy{.min_tls_version = aurora::net::TlsVersion::Tls13});
/// @endcode
/// @note Thread: thread-safe（值类型无共享状态；`default_policy()` / `set_default_policy()` 内部加锁）
/// @note Side-effects: none（判定不发起 I/O、不写日志、不改任何配置）

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/proxy.h"
#include "aurora/net/tls.h"

namespace aurora::net {

/// @brief 一次交换必须满足的声明式约束；缺省构造就是那组保守口径（拒一切明文、下限 TLS 1.2、不强求代理）。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
struct NetPolicy {
    /// @brief 允许明文目标的主机清单；空 = 一切明文都拒。
    ///
    /// 条目写法见本头注释：不带前导点即注册名全等，带一枚前导点即该域及其任意深层子域。`*` 是唯一的通配
    /// 项，意为「这一维不设限」——它是显式写下的放弃，而不是漏配。
    std::vector<std::string> allow_cleartext_hosts;

    /// @brief TLS 版本下限：协商结果不允许低于它，且 `TlsContext` 也不能容许低于它。
    TlsVersion min_tls_version = TlsVersion::Tls12;

    /// @brief 是否要求每一跳都经代理（`NO_PROXY` 例外命中的目标因此被拒）。
    bool require_proxy = false;

    /// @brief 明文主机白名单判定（不做解析、不比端口、不比 CIDR）。
    /// @param host 目标主机文本（`Url` 规范化后的形态：ASCII 小写、IPv6 不含方括号、无根区尾点）。
    /// @return 命中清单任一条目时为真；清单为空时恒为假。
    [[nodiscard]] auto allows_cleartext_host(std::string_view host) const -> bool;

    /// @brief 三维判定：这一跳（目标 + 代理决策 + TLS 口径）能不能发起。
    ///
    /// 三条维度按「明文 → 版本下限 → 是否必须经代理」的次序问，第一个不合的就交出错误；错误 detail 点名
    /// 是哪一维、涉事的取值，版本冲突时同时给出策略下限与上下文允许的下界。判定不看方法、不看路径。
    /// @param target 已解析的目标 URL（scheme 决定是否算明文）。
    /// @param proxy `ProxyEnvironment::for_target` 对本跳定出的代理决策（`Direct` = 不经代理）。
    /// @param tls 本次交换将使用的 TLS 口径（明文那侧不参与判定，因为无版本可谈）。
    /// @return 成功为 `Result<void>{}`；被拒携 `net-policy-violated`。
    [[nodiscard]] auto evaluate(const Url &target, const ProxyConfig &proxy, const TlsContext &tls) const
        -> Result<void>;
};

/// @brief 查看进程级默认策略。
/// @return 已设则返回之；否则 `std::nullopt`（表示用 `NetPolicy{}` 那组保守缺省，不表示关掉策略）。
[[nodiscard]] auto default_policy() -> std::optional<NetPolicy>;

/// @brief 设定进程级默认策略：给没有显式设过 `Client::policy()` 的客户端兜底。
///
/// 这是**进程级共享状态**，写给谁就影响谁——同一进程内后设者覆盖前者，且已发出的交换不受影响（判定用的
/// 是发起那一刻读到的策略）。传 `std::nullopt` 清回「未设」，此后各客户端回到 `NetPolicy{}`。
/// @param policy 新策略；`std::nullopt` = 清除。
auto set_default_policy(std::optional<NetPolicy> policy) -> void;

}  // namespace aurora::net
