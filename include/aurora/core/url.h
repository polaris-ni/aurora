#pragma once

/// @brief URL 值类型。
/// @file core/url.h
///
/// 一个 `Url` 表达「一个已解析并规范化过的定位符」，是纯值类型：不含句柄、无共享状态、
/// 无平台头，可在任意线程拷贝与比较。它存在的理由是提案 §12 的一句话——**手拼 URL 恰是
/// AI 生成代码的头号错误源**，所以 `Request` 与 `WebSocket::connect` 一律收 `Url`，
/// 字符串只能经 `Url::parse` 显式进入。
///
/// 四条设计约束值得在写代码前知道：
///
/// - **文法与相对解析按 RFC 3986 手写，不经平台 API**：同一输入在 Win32 / POSIX / wasm 上
///   必须给出同一结果与同一条诊断（`SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001`）。仓内既无
///   Windows 的 `UrlEscape/UrlUnescape` 可用（那是 IE 依赖），也不该让「哪些畸形串被谁容忍」
///   随 libc 漂移。
/// - **存的是规范化后的分量，不是原文**：scheme 与非 `IPv6` 主机名转小写、主机尾点去除、
///   百分号三元组的大写十六进制化、可解无保留字符还原（`%41` → `A`）、路径点段消除
///   （RFC 3986 §6.2.2）。两个 `Url` 相等即「同一资源」，`to_string()` 是规范形态而非输入回声。
/// - **分量各自带编码态**：`path()` / `query()` / `fragment()` 返回的是**已百分号编码**的线上
///   形态（`to_string` 直接拼接它们），要拿到可读文本用 `percent_decode()` 出口或
///   `query_params()`。这样「`a%2Fb`」与「`a/b`」在路径里是两个不同资源，不会被悄悄合并。
/// - **主机名三型显式**：`host_kind()` 区分注册名 / IPv4 字面量 / IPv6 字面量（IPv6 在 URL 里
///   必须带方括号，RFC 3986 §3.2.2；本类型存去括号的形式，`to_string()` 再加回）。注册名允许
///   非 ASCII（UTF-8 输入）并在解析时转成 punycode 的 ASCII 形态，`to_unicode_host()` 是显示
///   出口。IPv4 只认点分四段且禁前导零（与 `net::Endpoint` 同口径）；`1.2.3` 一类不是四段的
///   数字主机按**注册名**处理——本层不做 `inet_aton` 式缩写，免得同一地址有两种拼写却判不等。
///
/// @code
/// const auto url = aurora::Url::parse("HTTPS://Example.COM:443/a/../b%2Fx?q=1&q=2#frag");
/// if (url) {
///     const aurora::Url &parsed = url.value();
///     (void)parsed.scheme();            // "https"（小写）
///     (void)parsed.effective_port();    // 443（显式端口等于默认端口，仍照原值保留）
///     (void)parsed.path();              // "/b%2Fx"（点段消除；%2F 不还原成 '/'）
///     (void)parsed.query_params();      // 两条 q，按序，值已解码
///     (void)parsed.is_secure();         // true
/// }
/// const auto base = aurora::Url::parse("https://example.com/a/b");
/// const auto resolved = base.value().resolve("../c?d=1#e");  // https://example.com/a/c?d=1#e
/// @endcode
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {

/// @brief 查询参数：有序、允许同键重复的多值映射（键**大小写敏感**，与 `Headers` 相反）。
///
/// 与 `Headers` 同构但口径不同：URL 查询串没有「同键折叠」的惯例，`?a=1&A=2` 是两个不同键；
/// 且键与值都处在百分号编码态，所以本类持有的是**已解码**的可读文本，序列化回查询串时重新
/// 编码。存储同样是有序条目表，`entries()` 为遍历与序列化的唯一出口。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
class QueryParams {
  public:
    /// @brief 单个参数条目（解码态）。
    struct Entry {
        std::string key;  ///< 参数名（解码态；比较时大小写敏感）
        std::string value;  ///< 参数值（解码态；`a=` 形态为空串而非缺失）
    };

    /// @brief 追加参数：不去重，新条目置于末尾（`?a=1&a=2` 的正确形态）。
    /// @param key 参数名；原样存储，不做编解码。
    /// @param value 参数值。
    auto append(std::string_view key, std::string_view value) -> void;

    /// @brief 置入参数：移除全部同名条目后追加到末尾。
    /// @param key 参数名。
    /// @param value 参数值。
    auto set(std::string_view key, std::string_view value) -> void;

    /// @brief 取首个匹配参数的值。
    /// @param key 参数名；大小写敏感。
    /// @return 首个匹配条目的值；无匹配为 `nullopt`。
    [[nodiscard]] auto get(std::string_view key) const -> std::optional<std::string>;

    /// @brief 取同名参数的全部值，按插入顺序。
    /// @param key 参数名；大小写敏感。
    /// @return 匹配到的值序列（可能为空）。
    [[nodiscard]] auto values(std::string_view key) const -> std::vector<std::string>;

    /// @brief 是否存在某参数。
    /// @param key 参数名；大小写敏感。
    /// @return 存在至少一个同名条目时为 true。
    [[nodiscard]] auto contains(std::string_view key) const -> bool;

    /// @brief 移除某参数的全部条目。
    /// @param key 参数名；大小写敏感。
    /// @return 实际移除的条目数（0 表示不存在，操作幂等）。
    auto remove(std::string_view key) -> std::size_t;

    /// @brief 清空全部条目。
    auto clear() -> void { entries_.clear(); }

    /// @brief 条目总数（同键重复按多条计）。
    /// @return 当前条目数。
    [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

    /// @brief 是否无参数。
    /// @return 无任何条目时为 true。
    [[nodiscard]] auto empty() const -> bool { return entries_.empty(); }

    /// @brief 按插入顺序查看全部条目（序列化唯一出口）。
    /// @return 条目序列的只读引用。
    [[nodiscard]] auto entries() const -> const std::vector<Entry> & { return entries_; }

    /// @brief 序列化为查询串（不含前导 `?`）：键与值各自按查询字符集百分号编码，`&` 连接。
    /// @return 编码后的查询串；无条目时为空串。
    [[nodiscard]] auto to_string() const -> std::string;

    /// @brief 逐条目相等：键与值都严格比较，顺序敏感。
    /// @param other 参与比较的参数集合。
    /// @return 条目数相同且逐项相等时为 true。
    [[nodiscard]] auto operator==(const QueryParams &other) const -> bool;

  private:
    std::vector<Entry> entries_;  ///< 有序条目表：插入顺序即查询串顺序，同键不去重
};

/// @brief 百分号编码的「按分量」字符集：决定哪些字符在该分量里可直接出现、哪些须转义。
/// @note Thread: thread-safe
/// @note Side-effects: none
/// @note 豁免 performance-enum-size：公共 API 枚举，只作 `percent_encode` 的按值形参，不落在任何
/// 结构体/容器字段里；底层类型属公共 API 形态的一部分，改窄仅省 3 字节。
/// NOLINTNEXTLINE(performance-enum-size)
enum class PercentSet {
    Unreserved,  ///< 只留字母数字与 `-` `.` `_` `~`，其余全部转义（生成不可歧义的片段）
    PathSegment,  ///< 额外保留子界分隔符与 `:` `@` `/`（RFC 3986 §3.3 路径段可直接含 `/`）
    QueryParam,  ///< 查询串里的**单个键或值**：额外保留 `:` `@` `/` `?`，但 `&` 与 `=` 是定界符故转义
    Fragment,  ///< 额外保留 `:` `@` `/` `?` `#`（片段里的 `#` 不再是定界符）
};

/// @brief 按分量字符集做百分号编码。
///
/// 与「一律转义」的差别只在定界符：同一段文本编码成 `PathSegment` 与 `Unreserved` 结果不同，
/// 选错集合会让路径里的 `/` 变成 `%2F`（语义改变：从「目录分隔」变成「文件名的一部分」）。
/// 非 ASCII 按 UTF-8 逐字节转义，不做任何 Unicode 归一化。
/// @param raw 原始文本（解码态，UTF-8）。
/// @param set 目标分量的保留字符集。
/// @return 编码后的文本（纯 ASCII，十六进制大写形态 `%2F` 而非 `%2f`）。
[[nodiscard]] auto percent_encode(std::string_view raw, PercentSet set) -> std::string;

/// @brief 百分号解码。
///
/// `+` **不**当作空格：那是 `application/x-www-form-urlencoded` 的历史约定，URL 分量里 `+` 就是
/// `+`。把两者混为一谈会把「搜索词 1+2」解码成「搜索词 1 2」。
/// @param encoded 编码态文本。
/// @return 解码后的文本；出现截断的三元组（`%` 后不足两个十六进制位）时失败，
///         错误码 `net-url-invalid`，`message` 携 `detail=` 指明位置。
[[nodiscard]] auto percent_decode(std::string_view encoded) -> Result<std::string>;

/// @brief URL 主机名的类型；由解析结果决定，不按字符串形态猜。
/// @note Thread: thread-safe
/// @note Side-effects: none
/// @note 豁免 performance-enum-size：公共 API 枚举，作 `host_kind()` 返回值与 `Url` 的私有字段，
/// 单实例 4 字节不参与任何密集数组；底层类型属公共 API 形态，改窄仅省 3 字节。
/// NOLINTNEXTLINE(performance-enum-size)
enum class UrlHostKind {
    Empty,  ///< 无主机（`file:`、`mailto:`、`urn:`、不含授权段的引用）
    Name,  ///< 注册名（可含 punycode 标签，解析后恒为 ASCII）
    Ipv4,  ///< IPv4 点分十进制字面量
    Ipv6,  ///< IPv6 字面量（URL 内带方括号，本类型存去括号形态）
};

/// @brief 已解析、已规范化的 URL 值类型。
/// @note Thread: thread-safe（拷贝、比较与访问均无共享状态）
/// @note Side-effects: none
class Url {
  public:
    /// @brief 默认构造：空 URL（无 scheme、无授权段、路径为空）。
    Url() = default;

    /// @brief 解析一段 URL 文本（绝对 URL 或相对引用）。
    ///
    /// 解析即规范化（见文件头第三条约束）。非 ASCII 主机名在此转成 punycode ASCII 形态。
    /// @param text URL 文本；不含前导空白与百分号编码错误。
    /// @return 成功为规范化后的 `Url`（引用形态时 `is_absolute()` 为 false）；失败为
    ///         `net-url-invalid`，`message` 携 `detail=` 指明被拒的原因。
    [[nodiscard]] static auto parse(std::string_view text) -> Result<Url>;

    /// @brief 由分量构造（各分量传**编码态**，与 `path()` / `query()` 出口同口径）。
    ///
    /// 与 `parse` 的分工：`parse` 处理「一段文本」，`from_parts` 处理「程序里已有的结构化
    /// 输入」，两者产出同一规范化形态。解码态文本要先转成编码态，出口是 `percent_encode()`
    /// 与 `QueryParams::to_string()`；缺授权段的 URL（`mailto:` 一类）请用 `parse`。
    /// @param scheme 协议名；小写化后须为合法 token。
    /// @param host 主机名；注册名（允许 UTF-8）、IPv4 或 IPv6 字面量；裸 IPv6（`::1`）自动补方括号。
    /// @param port 端口；`std::nullopt` 表示不写端口（`0` 是合法值并原样保留）。
    /// @param path 编码态路径；非空且不以 `/` 开头时补 `/`（授权段后的路径必须以 `/` 起始）。
    /// @param query 编码态查询段（不含 `?`）；`std::nullopt` 表示无查询串（与「空查询串 `?`」区分）。
    /// @param fragment 编码态片段段（不含 `#`）；`std::nullopt` 表示无片段。
    /// @return 成功为规范化后的 `Url`；失败为 `net-url-invalid`。
    [[nodiscard]] static auto from_parts(std::string_view scheme, std::string_view host,
                                         std::optional<std::uint16_t> port, std::string_view path,
                                         std::optional<std::string_view> query = std::nullopt,
                                         std::optional<std::string_view> fragment = std::nullopt) -> Result<Url>;

    /// @brief 以 `*this` 为基址解析一段引用（RFC 3986 §5）。
    ///
    /// 引用本身是绝对 URL 时直接返回它（基址不参与）；基址无 scheme 时无法解析
    /// 相对引用，报 `net-url-invalid`。
    /// @param reference 引用文本（可为 `//host/path`、`/path`、`path?x#y`、`?x`、`#y`、空串）。
    /// @return 解析后的绝对 URL；失败为 `net-url-invalid`。
    [[nodiscard]] auto resolve(std::string_view reference) const -> Result<Url>;

    /// @brief 协议名（小写，不含 `:`）。
    /// @return 协议名；引用形态为空串。
    [[nodiscard]] auto scheme() const -> std::string_view { return scheme_; }

    /// @brief 用户信息段（编码态，不含尾部 `@`）。
    /// @return 编码态文本；无授权段时为空串。
    [[nodiscard]] auto userinfo() const -> std::string_view { return userinfo_; }

    /// @brief 主机名（规范化后的编码态；IPv6 不含方括号）。
    /// @return ASCII 主机名；无主机时为空串。
    [[nodiscard]] auto host() const -> std::string_view { return host_; }

    /// @brief 主机名类型。
    /// @return `host_kind()` 对应的三型之一或 `Empty`。
    [[nodiscard]] auto host_kind() const -> UrlHostKind { return host_kind_; }

    /// @brief 显式写出的端口。
    /// @return 文本里出现 `:port` 时为其值；未写端口为 `nullopt`（与「默认端口」区分）。
    [[nodiscard]] auto port() const -> std::optional<std::uint16_t> { return port_; }

    /// @brief 有效端口：显式端口，或该 scheme 的注册默认端口。
    /// @return `http` 80 / `https` 443 / `ws` 80 / `wss` 443；其余 scheme 无默认值时 `nullopt`。
    [[nodiscard]] auto effective_port() const -> std::optional<std::uint16_t>;

    /// @brief 路径（编码态；绝对 URL 的点段已消除）。
    /// @return 含授权段的 URL 路径恒以 `/` 起始；无授权段时可为空串。相对引用（无 scheme）
    ///         保留 `.` / `..` 原文，因为点段消除定义在合并之后（RFC 3986 §5.2.4），提前消除
    ///         会抹掉引用携带的层级信息。
    [[nodiscard]] auto path() const -> std::string_view { return path_; }

    /// @brief 是否存在查询段（即使为空，对应文本里的 `?`）。
    /// @return 文本含 `?` 时为 true。
    [[nodiscard]] auto has_query() const -> bool { return query_.has_value(); }

    /// @brief 查询串（编码态，不含 `?`）。
    /// @return 编码态文本；无查询段时为空串。
    [[nodiscard]] auto query() const -> std::string_view {
        return query_.has_value() ? std::string_view{*query_} : std::string_view{};
    }

    /// @brief 是否存在片段段（即使为空，对应文本里的 `#`）。
    /// @return 文本含 `#` 时为 true。
    [[nodiscard]] auto has_fragment() const -> bool { return fragment_.has_value(); }

    /// @brief 片段（编码态，不含 `#`）。
    /// @return 编码态文本；无片段时为空串。
    [[nodiscard]] auto fragment() const -> std::string_view {
        return fragment_.has_value() ? std::string_view{*fragment_} : std::string_view{};
    }

    /// @brief 把查询串解成多值参数表（键与值均解码态）。
    /// @return 解析后的参数表；查询段缺失时为空表；出现畸形编码时失败（`net-url-invalid`）。
    [[nodiscard]] auto query_params() const -> Result<QueryParams>;

    /// @brief 是否为绝对 URL（含 scheme）。
    /// @return 有 scheme 时为 true。
    [[nodiscard]] auto is_absolute() const -> bool { return !scheme_.empty(); }

    /// @brief 该 scheme 是否表达「传输层安全」（`https` / `wss`）。
    ///
    /// 只看 scheme，不看主机：`http://127.0.0.1` 依然是明文，策略层（`NetPolicy`）据此
    /// 拒非白名单明文，而不是靠「本机地址看起来无害」放行。
    /// @return `https` 或 `wss` 时为 true。
    [[nodiscard]] auto is_secure() const -> bool { return scheme_ == "https" || scheme_ == "wss"; }

    /// @brief 序列化为规范化后的完整文本。
    /// @return 与 `parse` 再解析本结果所得文本一致（幂等）。
    [[nodiscard]] auto to_string() const -> std::string;

    /// @brief 主机名的 Unicode 显示形态（punycode 标签解码回 UTF-8）。
    /// @return 显示用主机名；IPv4 / IPv6 字面量原样返回；punycode 解码失败为 `net-url-invalid`。
    [[nodiscard]] auto to_unicode_host() const -> Result<std::string>;

    /// @brief 规范化形态的整体相等比较（分量逐一比对）。
    /// @param other 参与比较的 URL。
    /// @return 各分量全等时为 true。
    [[nodiscard]] auto operator==(const Url &other) const -> bool;

  private:
    std::string scheme_;  ///< 小写协议名（不含 `:`）
    std::string userinfo_;  ///< 编码态用户信息（不含 `@`）
    std::string host_;  ///< 规范化主机名（ASCII；IPv6 不含方括号）
    std::string path_;  ///< 编码态路径（点段已消除）
    std::optional<std::string> query_;  ///< 编码态查询（不含 `?`）；nullopt = 无该段
    std::optional<std::string> fragment_;  ///< 编码态片段（不含 `#`）；nullopt = 无该段
    std::optional<std::uint16_t> port_;  ///< 显式端口；nullopt = 未写
    UrlHostKind host_kind_{UrlHostKind::Empty};  ///< 主机名类型
    bool has_authority_{false};  ///< 文本是否含 `//` 授权段（决定路径可否为空与序列化形态）
};

}  // namespace aurora
