#pragma once

/// @brief 代理配置值类型与 `NO_PROXY` 例外匹配（CHG-010 §11 代理族的配置侧）。
/// @file net/proxy.h
///
/// 本头只回答一个问题：**「这个目标该走哪个代理，还是直连」**。它是纯值类型——不含句柄、
/// 不含 socket、不含任何平台头，可在任意线程拷贝与比较。真正的 `CONNECT` / SOCKS5 握手是
/// 字节编解码，落在传输实现侧（`src/aurora/net/`），不在这里。
///
/// 四条设计约束值得在写代码前知道：
///
/// - **畸形配置一律报错，绝不静默直连**：`HTTP_PROXY=http//host` 这类写错的值如果被当作
///   「没配代理」处理，症状是「在办公网里能通、在客户网络里不通」，是企业网里最难排查的一类
///   故障。故 `parse()` 与 `from_environment()` 的返回类型是 `Result`，失败即终止请求，
///   由调用方把错误原样报给用户。
/// - **scheme 决定「怎么连代理」，不决定「目标是否 TLS」**：`HTTPS_PROXY=http://proxy:3128`
///   是合法且最常见的形态——明文连代理、代理再隧道到 https 目标。反过来，代理地址本身写
///   `https://`（与代理建立 TLS）本批次不支持，明确报 `net-proxy-unsupported`，
///   而不是退化成明文连接（那会把「代理链路上的凭据」暴露在错误信道上）。
/// - **安全目标绝不退到 `HTTP_PROXY`**：选择次序是「`HTTPS_PROXY` → `ALL_PROXY` → 直连」，
///   明文目标是「`HTTP_PROXY` → `ALL_PROXY` → 直连」。把 https 目标交给一个只为 http 配置的
///   代理，等于让运维在两个变量间靠猜来隔离流量。
/// - **密码不进可打印形态**：`to_string()` 恒把密码段替换为 `***`，凭据只能通过
///   `credentials()` 显式取用。日志里直接打印配置是凭据泄漏的主要途径。
///
/// 端口缺省沿用 curl / Go 的口径：HTTP CONNECT 代理默认 `80`，SOCKS5 默认 `1080`。
/// 无 scheme 的裸 `host:port`（`HTTP_PROXY=10.0.0.1:3128`）按 HTTP CONNECT 处理。
///
/// `NO_PROXY` 的例外文法是封闭的，只接受这四型（分隔符 `,` `;` 与空白均可，空项忽略）：
/// `*`（全部例外）、`example.com` / `.example.com` / `*.example.com`（本体加任意深层子域，
/// 三种写法等价）、`10.0.0.0/8` 与 `fd00::/8`（CIDR，只对数字字面量目标生效）、
/// 以及上述任一形态后跟 `:端口` 的端口限定。数字字面量项（`127.0.0.1`、`[::1]`）走**全等**
/// 匹配而非后缀匹配——否则 `.0.0.1` 这种写法会把无关主机卷进例外；IPv6 在列表里必须带括号，
/// 因为不带括号时 `:` 无法与端口限定区分。写不进去的形态一律报错。
///
/// @code
/// const auto env = aurora::net::ProxyEnvironment::from_environment();
/// if (env) {
///     const auto proxy = env.value().for_target("api.example.com", 443, true);
///     if (proxy.is_direct()) { /* 直连 */ }
///     (void)proxy.to_string();  // "http://user:***@proxy.internal:3128"（密码已掩码）
/// }
/// const auto manual = aurora::net::ProxyConfig::parse("socks5h://bob@socks.internal");
/// @endcode
/// @note Thread: thread-safe（值类型，无共享状态；`from_environment()` 的默认读取器在调用线程执行）
/// @note Side-effects: none（`from_environment()` 的默认形态读取进程环境变量，除此之外无副作用）

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/address.h"

namespace aurora::net {

/// @brief 代理的传输形态：决定与代理之间要说哪一种握手。
/// @note Thread: thread-safe
/// @note Side-effects: none
enum class ProxyKind : std::uint8_t {
    Direct,  ///< 不经代理，直连目标
    HttpConnect,  ///< HTTP/1.1 `CONNECT` 隧道（明文连代理）
    Socks5,  ///< SOCKS5（`socks5://` 交 IP、`socks5h://` 交主机名给代理解析）
};

/// @brief 代理认证凭据（明文形态，仅在实现侧组装握手时使用）。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
/// @note 恒由 URL 的 userinfo 段百分号解码而来，因此这里的文本是**解码态**；组装
/// `Proxy-Authorization` 时按解码态参与 `user:password` 拼接，与 RFC 7617 一致。
struct ProxyCredentials {
    std::string username;  ///< 用户名；空表示无凭据
    std::string password;  ///< 密码；可为空串（`user@host` 形态即用户名有、密码空）

    /// @brief 是否没有任何用户名。
    /// @return `username` 为空时为 true（此时密码一并忽略）。
    [[nodiscard]] auto empty() const -> bool { return username.empty(); }

    /// @brief 逐字段相等比较。
    /// @param other 参与比较的凭据。
    /// @return 用户名与密码都相等时为 true。
    [[nodiscard]] auto operator==(const ProxyCredentials &other) const -> bool;
};

/// @brief 单个代理的配置值类型（代理地址 + 形态 + 凭据）。
/// @note Thread: thread-safe（拷贝、比较与访问均无共享状态）
/// @note Side-effects: none
class ProxyConfig {
  public:
    /// @brief 构造「显式关闭代理」的配置。
    /// @return `kind()` 为 `Direct`、无主机与凭据的配置（与「没配代理」在决策上是同一结果）。
    [[nodiscard]] static auto direct() -> ProxyConfig;

    /// @brief 解析一段代理文本（URL 形态或裸 `host[:port]`）。
    ///
    /// 接受的 scheme：`http`（→ `HttpConnect`）、`socks5` / `socks5h`（→ `Socks5`）。
    /// `socks5h` 与 `socks5` 的差别只在握手阶段由谁解析主机名（见 `resolves_via_proxy()`）。
    /// 无 scheme 时按 `http` 补齐；userinfo 段百分号解码为凭据。
    /// @param text 代理地址文本，如 `http://user:pass@proxy:3128`、`10.0.0.1:3128`、
    ///             `socks5h://[fd00::2]:1080`。不得含路径、查询段或片段段。
    /// @param source 填 `source()` 的诊断字段（通常是来源环境变量名）；显式配置时留空。
    /// @return 成功为规范化后的配置；畸形为 `net-proxy-config-invalid`，
    ///         scheme 是已知但不支持的（`https` / `socks4`）为 `net-proxy-unsupported`。
    [[nodiscard]] static auto parse(std::string_view text, std::string_view source = {}) -> Result<ProxyConfig>;

    /// @brief 由结构化字段构造 HTTP/1.1 `CONNECT` 代理配置（程序内手工配置的路径）。
    /// @param host 代理主机：注册名、IPv4 或 IPv6 字面量（规范化口径与 `Url` 一致）。
    /// @param port 代理端口；`0` 表示取该形态的默认端口（`HttpConnect` 为 `80`）。
    /// @param credentials 凭据；默认空 = 不认证。
    /// @return 成功为配置；主机文本畸形或端口非法为 `net-proxy-config-invalid`。
    [[nodiscard]] static auto http_connect(std::string_view host, std::uint16_t port,
                                           const ProxyCredentials &credentials = {}) -> Result<ProxyConfig>;

    /// @brief 由结构化字段构造 SOCKS5 代理配置。
    /// @param host 代理主机；规范化口径同 `http_connect()`。
    /// @param port 代理端口；`0` 表示取默认端口 `1080`。
    /// @param resolve_via_proxy 为真时把目标主机名交给代理解析（`socks5h` 语义），
    ///                          为假时本端解析后只交 IP 地址。
    /// @param credentials 凭据；默认空 = 不认证（SOCKS5 无认证方法协商）。
    /// @return 成功为配置；失败口径同 `http_connect()`。
    [[nodiscard]] static auto socks5(std::string_view host, std::uint16_t port, bool resolve_via_proxy,
                                     const ProxyCredentials &credentials = {}) -> Result<ProxyConfig>;

    /// @brief 代理形态。
    /// @return 构造时确定的形态。
    [[nodiscard]] auto kind() const -> ProxyKind { return kind_; }

    /// @brief 是否直连（不建任何代理连接）。
    /// @return `kind()` 为 `Direct` 时为 true。
    [[nodiscard]] auto is_direct() const -> bool { return kind_ == ProxyKind::Direct; }

    /// @brief 代理主机（规范化后：ASCII 小写、IPv6 不含方括号）。
    /// @return 主机文本；`Direct` 时为空串。
    [[nodiscard]] auto host() const -> const std::string & { return host_; }

    /// @brief 代理端口（主机序）。
    /// @return 恒为非零值——显式写出的端口，或该形态的默认端口；`Direct` 时为 `0`。
    [[nodiscard]] auto port() const -> std::uint16_t { return port_; }

    /// @brief 认证凭据。
    /// @return 解码态凭据；无凭据时 `username` 为空串。
    [[nodiscard]] auto credentials() const -> const ProxyCredentials & { return credentials_; }

    /// @brief 是否携带凭据。
    /// @return 用户名非空时为 true。
    [[nodiscard]] auto has_credentials() const -> bool { return !credentials_.username.empty(); }

    /// @brief 目标主机名是否交给代理解析。
    /// @return `Socks5` 且来自 `socks5h` 时为 true；其余形态恒为 false（域名解析在 `CONNECT`
    ///         请求行里以文本携带，等价于交代理解析，故 `HttpConnect` 不需要这个开关）。
    [[nodiscard]] auto resolves_via_proxy() const -> bool { return resolve_via_proxy_; }

    /// @brief 配置来源的环境变量名（诊断用）。
    /// @return 由 `from_environment()` 经 `parse()` 填充时为实际命中的变量名（如 `HTTPS_PROXY`）；
    ///         其余构造路径为空串。不参与相等比较。
    [[nodiscard]] auto source() const -> const std::string & { return source_; }

    /// @brief 渲染为可打印文本，**密码恒以 `***` 掩码**。
    /// @return 形如 `http://proxy.internal:3128`、`socks5h://bob:***@10.0.0.1:1080`；
    ///         `Direct` 返回 `"direct"`。绝不含明文密码。
    [[nodiscard]] auto to_string() const -> std::string;

    /// @brief 语义相等：形态、主机、端口、凭据与代理解析开关逐项比较，`source()` 不参与。
    /// @param other 参与比较的配置。
    /// @return 全部语义字段相等时为 true。
    [[nodiscard]] auto operator==(const ProxyConfig &other) const -> bool;

  private:
    /// @brief 两枚结构化工厂的共同实现：规范化主机、代入缺省端口、校验端口非零。
    /// @param kind 代理形态。
    /// @param resolve_via_proxy SOCKS5 是否交代理解析主机名。
    /// @param host 候选代理主机文本。
    /// @param port 候选端口；`0` 代入该形态的缺省端口。
    /// @param credentials 凭据（原样保存，不做编码变换）。
    /// @return 成功为配置；主机或端口畸形为 `net-proxy-config-invalid`。
    [[nodiscard]] static auto build(ProxyKind kind, bool resolve_via_proxy, std::string_view host, std::uint16_t port,
                                    const ProxyCredentials &credentials) -> Result<ProxyConfig>;

    ProxyKind kind_{ProxyKind::Direct};  ///< 代理形态
    bool resolve_via_proxy_{false};  ///< SOCKS5 是否交代理解析主机名
    std::uint16_t port_{0};  ///< 主机序端口（已代入默认值）
    std::string host_;  ///< 规范化代理主机
    ProxyCredentials credentials_;  ///< 解码态凭据
    std::string source_;  ///< 来源环境变量名（诊断用）
};

/// @brief `NO_PROXY` 例外集合：命中即直连，优先于任何代理选择。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
class NoProxy {
  public:
    /// @brief 一条已解析的例外规则。
    struct Rule {
        /// @brief 规则类型。
        enum class Kind : std::uint8_t {
            Any,  ///< `*`：匹配一切目标
            Domain,  ///< 域名本体及其任意深层子域（可带端口限定）
            Literal,  ///< 数字字面量主机的全等匹配（可带端口限定）
            Cidr,  ///< 地址前缀（只对数字字面量目标生效，不带端口限定）
            SingleLabel,  ///< `<local>`：无限定域主机名（名字里无 `.`）
        };

        Kind kind{Kind::Any};  ///< 规则类型
        std::string domain;  ///< `Domain` / `Literal` 用：规范化主机文本（域名去前导点与尾点，IPv6 去括号）
        AddressFamily family{AddressFamily::IPv4};  ///< `Cidr` 用：前缀地址族
        std::array<std::uint8_t, 16> network{};  ///< `Cidr` 用：网络地址（网络序字节，非前缀位须为零）
        std::uint8_t prefix_bits{0};  ///< `Cidr` 用：前缀长度（IPv4 ≤ 32、IPv6 ≤ 128）
        std::uint16_t port{0};  ///< `Domain` 用：端口限定；`0` = 不限端口
    };

    /// @brief 解析一段例外列表。
    ///
    /// 分隔符 `,`、`;` 与空白等价，空项忽略（`,` 收尾与 `A,,B` 都合法）。大小写不敏感，
    /// 尾点视作根区限定符并去除。除 `*` 外另认两枚系统代理写法的令牌：`<local>`（无限定域的
    /// 主机名例外，即名字里没有 `.` 的目标）与 `<-loopback>`（回环地址例外）——二者是 Windows
    /// 与 macOS 系统设置里「不为本地地址使用代理」那条勾选的实际存储形态，按不认识的项报错会让
    /// 默认配置直接读不出代理，而静默忽略又会让内网流量绕过代理，故在文法里如实表达。
    /// @param list 例外列表原文，如 `localhost, .internal, 10.0.0.0/8, api.example.com:8080`。
    /// @return 成功为规则集合（空文本得空集合，匹配不到任何目标）；
    ///         某条形如规则但不合法（`10.0.0.0/33`、`[::1]x`、CIDR 带端口）为
    ///         `net-proxy-config-invalid`，`message` 携 `detail=` 指明是第几条。
    [[nodiscard]] static auto parse(std::string_view list) -> Result<NoProxy>;

    /// @brief 构造 `*`（全部目标例外 = 整体关闭代理的例外形态）。
    /// @return 恒含单条 `Any` 规则的集合。
    [[nodiscard]] static auto match_all() -> NoProxy;

    /// @brief 判断某目标是否命中例外。
    ///
    /// 域名规则匹配主机本体与任意深层子域；数字字面量规则要求全等；CIDR 规则只把**数字字面量**
    /// 目标与之比较——名字型目标在本层不做解析，故不会被 CIDR 命中（解析后再判需要回到
    /// `Resolver`，本批次不做，口径与 Go / curl 一致）。
    /// @param host 目标主机（URL 规范化后的形态：ASCII 小写、IPv6 不含方括号）。
    /// @param port 目标端口（主机序）。
    /// @return 任一规则同时满足主机与端口条件时为 true。
    [[nodiscard]] auto matches(std::string_view host, std::uint16_t port) const -> bool;

    /// @brief 是否没有任何规则。
    /// @return 规则数为零时为 true（此时 `matches()` 恒为 false）。
    [[nodiscard]] auto empty() const -> bool { return rules_.empty(); }

    /// @brief 规则条数。
    /// @return 解析出的规则数。
    [[nodiscard]] auto size() const -> std::size_t { return rules_.size(); }

    /// @brief 只读遍历规则（诊断与测试出口）。
    /// @return 按输入顺序排列的规则序列。
    [[nodiscard]] auto rules() const -> const std::vector<Rule> & { return rules_; }

    /// @brief 逐规则相等比较。
    /// @param other 参与比较的集合。
    /// @return 规则数相同且逐项相等时为 true。
    [[nodiscard]] auto operator==(const NoProxy &other) const -> bool;

  private:
    std::vector<Rule> rules_;  ///< 规则序列，输入顺序即匹配顺序
};

/// @brief 从环境变量得到的代理配置集合，以及「目标 → 代理」的唯一决策入口。
/// @note Thread: thread-safe（值类型；读取动作由 `from_environment()` 的读取器完成）
/// @note Side-effects: none（默认读取器只读环境变量）
struct ProxyEnvironment {
    /// @brief 环境变量读取器：返回该变量的值，未设置或空值返回 `nullopt`。
    ///
    /// 注入这条接缝是为了让单测在无真实环境变量的情况下覆盖全部选择次序与例外形态，
    /// 也让调用方能把 `setenv` 之外的来源（配置文件、命令行参数）接进同一决策路径。
    using Reader = std::function<std::optional<std::string>(std::string_view)>;

    std::optional<ProxyConfig> http;  ///< `HTTP_PROXY`：明文目标首选
    std::optional<ProxyConfig> https;  ///< `HTTPS_PROXY`：安全目标首选
    std::optional<ProxyConfig> all;  ///< `ALL_PROXY`：两者的共同回退
    NoProxy exceptions;  ///< `NO_PROXY` 例外集合（空集合 = 无例外）

    /// @brief 读取进程环境变量构造配置集合（默认入口）。
    ///
    /// 每个变量先读大写形态再读小写形态（`HTTP_PROXY` 然后 `http_proxy`），大写优先——
    /// 这是 curl 与 Go 的共同口径。值为空串视为未设置；非空但畸形则整体失败。
    /// @return 成功为配置集合（三个变量都没设时为全空集合，即一切直连）；
    ///         某个变量存在但解析失败为 `net-proxy-config-invalid` / `net-proxy-unsupported`，
    ///         `message` 携 `detail=` 指明变量名。
    [[nodiscard]] static auto from_environment() -> Result<ProxyEnvironment>;

    /// @brief 用注入的读取器构造配置集合（测试与非环境变量来源）。
    /// @param reader 变量读取器；对每个候选变量名各调用一次，顺序为「大写、小写」。
    /// @return 口径同 `from_environment()`。
    [[nodiscard]] static auto from_environment(const Reader &reader) -> Result<ProxyEnvironment>;

    /// @brief 构造「整体关闭环境代理」的空集合。
    /// @return 无代理、无例外的集合，`for_target()` 对任何目标都返回直连。
    [[nodiscard]] static auto disabled() -> ProxyEnvironment;

    /// @brief 读取**操作系统**的代理设置（Windows 走 WinHTTP 的用户级配置，macOS 走系统代理偏好）。
    ///
    /// 只取地址与例外两件事，**绝不执行自动配置脚本**（PAC / WPAD 属非目标）。据此有两条必须
    /// 分岔的回报：机器上确实没配代理是正常状态，回空集合（一切直连）；配了 PAC 或开了自动探测
    /// 却回空集合，等于让企业网流量静默绕过代理——那正是本库最难排查的一类故障，故回
    /// `net-proxy-unsupported` 而不降级。本腿没有系统代理通道（Linux 与浏览器腿的通道就是环境变量）
    /// 时回 `net-platform-unavailable`，让「读不到」与「没配」在类型上就是两件事。
    /// @return 成功为集合（各配置的 `source()` 为 `system:<槽位令牌>`，如 `system:socks5`）；
    ///         畸形项为 `net-proxy-config-invalid`（`detail=` 指到第几项），PAC/WPAD/SOCKS4
    ///         形态为 `net-proxy-unsupported`，本腿无通道为 `net-platform-unavailable`，
    ///         通道在而读取被拒为 `net-proxy-config-unreadable`。
    /// @note Side-effects: 读取操作系统配置（Windows 注册表用户分区 / macOS 动态存储），不写不回网
    [[nodiscard]] static auto from_system() -> Result<ProxyEnvironment>;

    /// @brief 按「环境变量优先，缺则系统设置」组装集合（桌面机上的缺省读取口径）。
    ///
    /// 次序是运维契约：显式设过 `HTTP_PROXY` 的机器（容器、CI、代理调试现场）以环境变量为准，
    /// 系统设置只在环境变量一枚都没设时参与。系统腿报 `net-platform-unavailable`（本腿没有该
    /// 通道）时等价于只有环境变量，不报错——Linux 上这就是一次正常的环境变量读取。
    /// @return 成功为集合；任一被采纳的来源里有畸形值为对应 `net-proxy-*` 码（不静默丢弃该来源）。
    [[nodiscard]] static auto from_environment_or_system() -> Result<ProxyEnvironment>;

    /// @brief 用注入的读取器组装两来源集合（测试与非环境变量来源）。
    /// @param reader 变量读取器；口径同 `from_environment(const Reader &)`。
    /// @return 口径同 `from_environment_or_system()`。
    [[nodiscard]] static auto from_environment_or_system(const Reader &reader) -> Result<ProxyEnvironment>;

    /// @brief 唯一的代理决策入口：给定目标，返回该走的代理或直连。
    ///
    /// 次序：先判例外（命中即直连），再按目标是否安全走各自的变量次序。
    /// 安全目标**绝不**回退到 `http`，明文目标可回退到 `all`。
    /// @param host 目标主机（URL 规范化后的形态）。
    /// @param port 目标端口（主机序）。
    /// @param secure 目标是否要求传输层安全（`Url::is_secure()` 的 `https` / `wss`）。
    /// @return 命中的代理配置；无代理或命中例外时为 `ProxyConfig::direct()`。
    [[nodiscard]] auto for_target(std::string_view host, std::uint16_t port, bool secure) const -> ProxyConfig;

    /// @brief 逐字段相等比较（三个可选配置与例外集合）。
    /// @param other 参与比较的集合。
    /// @return 各字段都相等时为 true。
    [[nodiscard]] auto operator==(const ProxyEnvironment &other) const -> bool;
};

}  // namespace aurora::net
