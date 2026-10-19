/// 测试类型: unit
/// 目标单元: include/aurora/core/url.h
/// 测试说明: `Url` / `QueryParams` 值类型行为契约回归：解析即规范化
///           （scheme 与主机小写、主机尾点去除、百分号三元组大写化且还原无保留字符、绝对 URL
///           点段消除、裸非 ASCII 字节转义）、RFC 3986 §5.4 相对解析表与解析规范化用例集由
///           `tests/fixtures/url_vectors/*.tsv` 黄金语料驱动（来源与口径见该目录 README）、四套百分号
///           字符集与「`+` 不等于空格」解码口径、查询参数同名保序且键大小写敏感、主机三型判定
///           （IPv4 禁前导零且非四段数字按注册名、IPv6 必须方括号且守 `::` 与内嵌 IPv4 语法、
///           注册名走 IDNA punycode 且含多码点插入向量）、`from_parts` 的裸 IPv6 补括号与
///           序列化幂等、畸形输入一律以 `net-url-invalid` 抵达。全程纯内存、
///           无平台分支，故同一断言集在 Win32 / MinGW / WSL / wasm 上必须逐值同色。

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/url.h"
#include "framework/aurora_test.h"
#include "paths.h"

namespace aurora::test_cases::utest_core_url {

namespace {

/// @brief 失败诊断是否为 `net-url-invalid`（错误码 slug 是冻结对外契约）。
/// @param err 结构化错误。
/// @return slug 与枚举码同时匹配时为 true。
auto is_url_invalid(const aurora::Error &err) -> bool {
    return err.code == "net-url-invalid" && err.code_enum == aurora::ErrorCode::NetUrlInvalid;
}

/// @brief 黄金语料的一行：行号只用于失败诊断，列按制表符切分。
struct GoldenRow {
    std::size_t line = 0U;
    std::vector<std::string> cols;
};

/// @brief 读取制表符分隔的黄金语料：`#` 开头与空行跳过，其余按字节原样保留
///        （语料含 UTF-8 与 `%` 三元组，任何文本模式转换都会破坏判据）。
/// @param relative 仓库根相对路径。
/// @return 语料行；列数是否合法留给用例断言（文件缺失时返回空表，由行数判据兜住）。
auto read_golden(std::string_view relative) -> std::vector<GoldenRow> {
    std::ifstream in{aurora::testing::paths::under_repo(relative), std::ios::binary};
    std::vector<GoldenRow> rows;
    std::string line;
    std::size_t number = 0U;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        GoldenRow row;
        row.line = number;
        std::size_t begin = 0U;
        while (true) {
            const std::size_t tab = line.find('\t', begin);
            row.cols.emplace_back(line.substr(begin, tab == std::string::npos ? std::string::npos : tab - begin));
            if (tab == std::string::npos) {
                break;
            }
            begin = tab + 1U;
        }
        rows.emplace_back(std::move(row));
    }
    return rows;
}

/// @brief 失败诊断前缀：行号 + 转义后的原始输入，保证终端里逐字节可读（非 ASCII 与控制字节转 `\xNN`）。
/// @param line 语料文件内的行号。
/// @param input 原始输入列。
/// @return 形如 `line 7 input=http://h/a\x20b`。
auto golden_locate(std::size_t line, std::string_view input) -> std::string {
    static constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";
    std::string out = "line " + std::to_string(line) + " input=";
    for (const char ch : input) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 0x20U && byte < 0x7FU && ch != '\\' && ch != '\t') {
            out += ch;
        } else {
            out += "\\x";
            out += HEX_DIGITS[static_cast<std::size_t>(byte >> 4U)];
            out += HEX_DIGITS[static_cast<std::size_t>(byte & 0x0FU)];
        }
    }
    return out;
}

/// @brief 语料的期望值：`ERR` 表示该输入必须以 `net-url-invalid` 拒绝。
constexpr std::string_view AURORA_GOLDEN_REJECT = "ERR";

/// @brief 一行的形状判据：恰好「输入 + 期望」两列，多一列意味着有人误用了分隔符。
/// @param row 语料行。
/// @return 列数为 2 时为 true。
auto is_well_formed(const GoldenRow &row) -> bool { return row.cols.size() == 2U; }

/// @brief 形状不合法时的诊断（不猜哪一列是期望值，直接报列数）。
/// @param row 语料行。
/// @return 含行号与实际列数的说明。
auto column_error(const GoldenRow &row) -> std::string {
    return "line " + std::to_string(row.line) + " must hold exactly 2 tab-separated columns, got " +
           std::to_string(row.cols.size());
}

}  // namespace

// 规范化的五条口径同批断言：小写化、三元组大写化、无保留还原、点段消除、主机尾点去除；
// 其中 `%2F` 必须**不**还原成 `/`，否则「a/b」与「a%2Fb」这两个不同资源会被悄悄合并。
AURORA_TEST_CASE(parse_normalizes_case_percent_and_dot_segments) {
    const auto parsed = aurora::Url::parse("HTTPS://Example.COM:8443/a/../b%2Fx%41?q=1&q=%2b#frag%7e");
    AURORA_TEST_REQUIRE(parsed.ok());
    const aurora::Url &url = parsed.value();

    AURORA_TEST_CHECK_EQ(url.scheme(), std::string("https"));
    AURORA_TEST_CHECK_EQ(url.host(), std::string("example.com"));
    AURORA_TEST_CHECK_TRUE(url.host_kind() == aurora::UrlHostKind::Name);
    // ⚠️ `port()` 每次调用都返回一枚新的临时 optional，判空与解包若各调一次，路径分析认不出是同一枚
    // （判为未检查访问）⇒ 先落一个具名局部再判空、再解包。
    const auto port = url.port();
    AURORA_TEST_REQUIRE(port.has_value());
    AURORA_TEST_CHECK_EQ(*port, std::uint16_t{8443});
    AURORA_TEST_CHECK_EQ(url.path(), std::string("/b%2FxA"));
    AURORA_TEST_CHECK_EQ(url.query(), std::string("q=1&q=%2B"));
    AURORA_TEST_CHECK_EQ(url.fragment(), std::string("frag~"));

    AURORA_TEST_CHECK_EQ(url.to_string(), std::string("https://example.com:8443/b%2FxA?q=1&q=%2B#frag~"));
    // 序列化再解析必须逐分量同色（幂等是「规范化」的定义，不是巧合）。
    const auto again = aurora::Url::parse(url.to_string());
    AURORA_TEST_REQUIRE(again.ok());
    AURORA_TEST_CHECK_TRUE(again.value() == url);

    // 主机尾点（DNS 根标签）是同一个名字的另一种拼写，去点后才判等——否则缓存键随写法分裂。
    const auto rooted = aurora::Url::parse("http://example.com./x");
    const auto plain = aurora::Url::parse("http://example.com/x");
    AURORA_TEST_REQUIRE(rooted.ok());
    AURORA_TEST_REQUIRE(plain.ok());
    AURORA_TEST_CHECK_EQ(rooted.value().host(), std::string("example.com"));
    AURORA_TEST_CHECK_TRUE(rooted.value() == plain.value());
}

// `port()` 与 `effective_port()` 是两件事：前者回答「文本里写了没有」，后者回答「连到哪」。
// 显式写出的默认端口原样保留，不折叠成 nullopt——折叠会让「用户显式指定 80」这一事实在序列化时消失。
AURORA_TEST_CASE(effective_port_follows_scheme_but_keeps_explicit_value) {
    struct Row {
        std::string_view text;
        std::string_view expected;
    };
    const std::vector<Row> rows{{.text = "http://h/", .expected = "80"},
                                {.text = "ws://h/", .expected = "80"},
                                {.text = "https://h/", .expected = "443"},
                                {.text = "wss://h/", .expected = "443"}};
    for (const auto &row : rows) {
        const auto parsed = aurora::Url::parse(row.text);
        AURORA_TEST_REQUIRE(parsed.ok());
        AURORA_TEST_CHECK_FALSE(parsed.value().port().has_value());
        const auto effective = parsed.value().effective_port();
        AURORA_TEST_REQUIRE(effective.has_value());
        AURORA_TEST_CHECK_EQ(std::to_string(*effective), std::string(row.expected));
    }

    const auto ftp = aurora::Url::parse("ftp://h/x");
    AURORA_TEST_REQUIRE(ftp.ok());
    AURORA_TEST_CHECK_FALSE(ftp.value().effective_port().has_value());
    AURORA_TEST_CHECK_FALSE(ftp.value().is_secure());

    const auto explicit_default = aurora::Url::parse("http://h:80/");
    AURORA_TEST_REQUIRE(explicit_default.ok());
    const auto explicit_port = explicit_default.value().port();
    AURORA_TEST_REQUIRE(explicit_port.has_value());
    AURORA_TEST_CHECK_EQ(*explicit_port, std::uint16_t{80});
    AURORA_TEST_CHECK_EQ(explicit_default.value().to_string(), std::string("http://h:80/"));

    const auto secure = aurora::Url::parse("https://h/");
    AURORA_TEST_REQUIRE(secure.ok());
    AURORA_TEST_CHECK_TRUE(secure.value().is_secure());
    // `is_secure()` 只看 scheme：本机明文仍是明文，策略层据此拒绝，而不是靠「看起来无害」放行。
    const auto loopback_plain = aurora::Url::parse("http://127.0.0.1/");
    AURORA_TEST_REQUIRE(loopback_plain.ok());
    AURORA_TEST_CHECK_FALSE(loopback_plain.value().is_secure());
}

// RFC 3986 §5.4 的完整引用解析表（基址 http://a/b/c/d;p?q）改由黄金语料驱动：
// 表本身就是判据，写死在代码里会让「加一条用例」变成「改测试源码」，语料文件把两者解耦。
AURORA_TEST_CASE(golden_resolve_vectors_match_rfc3986_table) {
    const auto base = aurora::Url::parse("http://a/b/c/d;p?q");
    AURORA_TEST_REQUIRE(base.ok());

    const auto rows = read_golden("tests/fixtures/url_vectors/resolve.tsv");
    AURORA_TEST_REQUIRE_MSG(rows.size() >= 40U, "resolve corpus looks empty or truncated");
    for (const auto &row : rows) {
        if (!is_well_formed(row)) {
            AURORA_TEST_CHECK_MSG(false, column_error(row));
            continue;
        }
        const auto what = golden_locate(row.line, row.cols[0]);
        const auto resolved = base.value().resolve(row.cols[0]);
        // 逐行降级为非致命：一条失配不该遮住同表其余各条的判据。
        if (!resolved.ok()) {
            AURORA_TEST_CHECK_MSG(false, what + " rejected: " + resolved.error().code);
            continue;
        }
        AURORA_TEST_CHECK_MSG(resolved.value().to_string() == row.cols[1],
                              what + " expected=" + row.cols[1] + " actual=" + resolved.value().to_string());
    }
}

// `Url::parse` 的黄金用例集：输入 → 规范化后的 `to_string()`（或 `ERR` 表示必须拒绝）。
// 每行还要求「序列化结果再解析」逐分量同色——幂等是规范化的定义，由整张表兜住而非单条断言。
AURORA_TEST_CASE(golden_parse_vectors_match_normalized_form) {
    const auto rows = read_golden("tests/fixtures/url_vectors/parse.tsv");
    AURORA_TEST_REQUIRE_MSG(rows.size() >= 70U, "parse corpus looks empty or truncated");
    for (const auto &row : rows) {
        if (!is_well_formed(row)) {
            AURORA_TEST_CHECK_MSG(false, column_error(row));
            continue;
        }
        const auto what = golden_locate(row.line, row.cols[0]);
        const auto parsed = aurora::Url::parse(row.cols[0]);
        if (row.cols[1] == AURORA_GOLDEN_REJECT) {
            AURORA_TEST_CHECK_MSG(!parsed.ok(), what + " must be rejected");
            if (!parsed.ok()) {
                AURORA_TEST_CHECK_MSG(is_url_invalid(parsed.error()), what + " rejected with wrong error code");
            }
            continue;
        }
        if (!parsed.ok()) {
            AURORA_TEST_CHECK_MSG(false, what + " rejected: " + parsed.error().code);
            continue;
        }
        AURORA_TEST_CHECK_MSG(parsed.value().to_string() == row.cols[1],
                              what + " expected=" + row.cols[1] + " actual=" + parsed.value().to_string());
        const auto again = aurora::Url::parse(parsed.value().to_string());
        AURORA_TEST_CHECK_MSG(again.ok() && again.value() == parsed.value(), what + " is not idempotent");
    }
}
// 相对解析需要绝对基址；基址自身只是引用时无从推断 scheme / host，必须报错而非猜一个。
AURORA_TEST_CASE(resolve_needs_absolute_base_and_rejects_relative_base) {
    const auto base = aurora::Url::parse("/b/c/");
    AURORA_TEST_REQUIRE(base.ok());
    AURORA_TEST_CHECK_FALSE(base.value().is_absolute());
    const auto resolved = base.value().resolve("g");
    AURORA_TEST_REQUIRE(!resolved.ok());
    AURORA_TEST_CHECK_TRUE(is_url_invalid(resolved.error()));
}

// 四套字符集的差别只在定界符：选错集合会改变资源身份（路径里的 `/` 变成 `%2F` 即「文件名的一部分」）。
AURORA_TEST_CASE(percent_encode_honours_per_component_sets) {
    const std::string_view sample = "a b/c?d#e~f+g=h%25i";

    AURORA_TEST_CHECK_EQ(aurora::percent_encode(sample, aurora::PercentSet::Unreserved),
                         std::string("a%20b%2Fc%3Fd%23e~f%2Bg%3Dh%2525i"));
    AURORA_TEST_CHECK_EQ(aurora::percent_encode(sample, aurora::PercentSet::PathSegment),
                         std::string("a%20b/c%3Fd%23e~f+g=h%2525i"));
    AURORA_TEST_CHECK_EQ(aurora::percent_encode(sample, aurora::PercentSet::QueryParam),
                         std::string("a%20b/c?d%23e~f+g%3Dh%2525i"));
    AURORA_TEST_CHECK_EQ(aurora::percent_encode(sample, aurora::PercentSet::Fragment),
                         std::string("a%20b/c?d#e~f+g=h%2525i"));

    // 非 ASCII 按 UTF-8 逐字节转义，不做归一化。
    AURORA_TEST_CHECK_EQ(aurora::percent_encode("\xC3\xBC", aurora::PercentSet::Unreserved), std::string("%C3%BC"));
}

// 解码侧的三条判据：三元组还原、`+` 保持字面量（那是 form-urlencoding 的历史约定，不是 URL 的）、
// 截断三元组必须报错而不是「保留原样」——静默保留会让 `%2` 一路走到对端再变成不可预期字节。
AURORA_TEST_CASE(percent_decode_keeps_plus_and_rejects_truncated_escapes) {
    const auto plain = aurora::percent_decode("a%20b%2Fc%3Fd");
    AURORA_TEST_REQUIRE(plain.ok());
    AURORA_TEST_CHECK_EQ(plain.value(), std::string("a b/c?d"));

    const auto plus = aurora::percent_decode("search+term%2B1");
    AURORA_TEST_REQUIRE(plus.ok());
    AURORA_TEST_CHECK_EQ(plus.value(), std::string("search+term+1"));

    for (const std::string_view bad :
         {std::string_view{"100%"}, std::string_view{"%2"}, std::string_view{"%zz"}, std::string_view{"ok%41%"}}) {
        const auto decoded = aurora::percent_decode(bad);
        AURORA_TEST_REQUIRE(!decoded.ok());
        AURORA_TEST_CHECK_TRUE(is_url_invalid(decoded.error()));
    }
}

// 查询串是多值有序表：同键不折叠、键大小写敏感（与 `Headers` 刻意相反）、空槽不产幽灵参数。
AURORA_TEST_CASE(query_params_keep_duplicates_ordered_and_keys_case_sensitive) {
    const auto parsed = aurora::Url::parse("http://h/?a=1&b=&A=2&a=3%20x&&c&tail=%2B");
    AURORA_TEST_REQUIRE(parsed.ok());
    const auto params = parsed.value().query_params();
    AURORA_TEST_REQUIRE(params.ok());

    const aurora::QueryParams &q = params.value();
    AURORA_TEST_CHECK_EQ(q.size(), std::size_t{6});
    AURORA_TEST_CHECK_EQ(q.get("a").value_or(std::string{}), std::string("1"));
    AURORA_TEST_CHECK_EQ(q.values("a"), std::vector<std::string>{"1", "3 x"});
    AURORA_TEST_CHECK_EQ(q.get("A").value_or(std::string{}), std::string("2"));  // 键大小写敏感
    AURORA_TEST_CHECK_EQ(q.get("b").value_or(std::string{"unset"}), std::string{});  // `b=` 是空值不是缺失
    AURORA_TEST_CHECK_EQ(q.get("c").value_or(std::string{"unset"}), std::string{});  // 无 `=` 同样落空值
    AURORA_TEST_CHECK_EQ(q.get("tail").value_or(std::string{}), std::string("+"));
    AURORA_TEST_CHECK_TRUE(q.contains("b"));
    AURORA_TEST_CHECK_FALSE(q.contains("B"));  // 键大小写敏感，与 `Headers` 刻意相反
    AURORA_TEST_CHECK_FALSE(q.contains("missing"));

    // 写回序列化后再解析应与原表逐条目相等（编码态与解码态的往返不丢信息）：
    // 键/值里的 `&` `=` `%` 都被转义，故不会被误读成定界符。
    aurora::QueryParams rebuilt;
    rebuilt.append("k", "v 1");
    rebuilt.append("k", "v&2");
    rebuilt.append("q%", "x=y");
    const auto url = aurora::Url::from_parts("https", "example.com", std::nullopt, "/api", rebuilt.to_string());
    AURORA_TEST_REQUIRE(url.ok());
    const auto round = url.value().query_params();
    AURORA_TEST_REQUIRE(round.ok());
    AURORA_TEST_CHECK_TRUE(round.value() == rebuilt);

    AURORA_TEST_CHECK_EQ(rebuilt.remove("k"), std::size_t{2});
    AURORA_TEST_CHECK_EQ(rebuilt.size(), std::size_t{1});

    const auto without_query = aurora::Url::parse("http://h/x");
    AURORA_TEST_REQUIRE(without_query.ok());
    AURORA_TEST_CHECK_FALSE(without_query.value().has_query());
    const auto empty_params = without_query.value().query_params();
    AURORA_TEST_REQUIRE(empty_params.ok());
    AURORA_TEST_CHECK_TRUE(empty_params.value().empty());
}

// 主机三型由解析结果决定，且 IPv4 与 `net::Endpoint` 同口径禁前导零（同一地址只允许一种写法）。
AURORA_TEST_CASE(host_kinds_are_typed_and_strict) {
    const auto v4 = aurora::Url::parse("http://192.168.0.1:8080/");
    AURORA_TEST_REQUIRE(v4.ok());
    AURORA_TEST_CHECK_TRUE(v4.value().host_kind() == aurora::UrlHostKind::Ipv4);
    AURORA_TEST_CHECK_EQ(v4.value().host(), std::string("192.168.0.1"));

    const auto v6 = aurora::Url::parse("http://[2001:DB8::1]:8080/x");
    AURORA_TEST_REQUIRE(v6.ok());
    AURORA_TEST_CHECK_TRUE(v6.value().host_kind() == aurora::UrlHostKind::Ipv6);
    AURORA_TEST_CHECK_EQ(v6.value().host(), std::string("2001:db8::1"));  // 存去括号形态且小写
    AURORA_TEST_CHECK_EQ(v6.value().to_string(), std::string("http://[2001:db8::1]:8080/x"));

    // 内嵌 IPv4 尾段与 RFC 6874 的 `%25` 作用域标识。
    for (const std::string_view text :
         {std::string_view{"http://[::ffff:192.0.2.1]/"}, std::string_view{"http://[fe80::1%25eth0]/"},
          std::string_view{"http://[1:2:3:4:5:6:7:8]/"}, std::string_view{"http://[::]/"}}) {
        const auto parsed = aurora::Url::parse(text);
        AURORA_TEST_REQUIRE(parsed.ok());
        AURORA_TEST_CHECK_TRUE(parsed.value().host_kind() == aurora::UrlHostKind::Ipv6);
    }

    // 非四段点分的纯数字主机按注册名处理（`1.2.3` 是一枚三个标签的名字，不是缩写 IPv4）：
    // core 只做语法分层，inet_aton 式缩写留给解析器去分歧，URL 层一律按名匹配。
    for (const std::string_view numeric : {std::string_view{"1.2.3"}, std::string_view{"1.2.3.4.5"}}) {
        const auto parsed = aurora::Url::parse("http://" + std::string(numeric) + "/");
        AURORA_TEST_REQUIRE(parsed.ok());
        AURORA_TEST_CHECK_TRUE(parsed.value().host_kind() == aurora::UrlHostKind::Name);
        AURORA_TEST_CHECK_EQ(parsed.value().host(), std::string(numeric));
    }

    for (const std::string_view bad : {std::string_view{"http://01.2.3.4/"}, std::string_view{"http://256.1.1.1/"},
                                       std::string_view{"http://example..com/"}, std::string_view{"http://[1::2::3]/"},
                                       std::string_view{"http://[1:2:3]/"}, std::string_view{"http://[::1"},
                                       std::string_view{"http://::1/"}, std::string_view{"http://[12345::1]/"},
                                       std::string_view{"http://[fe80::1%25]/"}, std::string_view{"http://h]/"}}) {
        const auto parsed = aurora::Url::parse(bad);
        AURORA_TEST_REQUIRE(!parsed.ok());
        AURORA_TEST_CHECK_TRUE(is_url_invalid(parsed.error()));
    }
}

// 授权段的 user-info 与端口各自的归一形态：空端口（写了 `:` 没写数字）归一为「无端口」并去掉 `:`。
AURORA_TEST_CASE(authority_splits_userinfo_host_and_port) {
    const auto auth = aurora::Url::parse("http://user:pa%20ss@example.com:8443/x");
    AURORA_TEST_REQUIRE(auth.ok());
    AURORA_TEST_CHECK_EQ(auth.value().userinfo(), std::string("user:pa%20ss"));
    AURORA_TEST_CHECK_EQ(auth.value().host(), std::string("example.com"));
    AURORA_TEST_CHECK_EQ(auth.value().to_string(), std::string("http://user:pa%20ss@example.com:8443/x"));

    const auto empty_port = aurora::Url::parse("http://example.com:/x");
    AURORA_TEST_REQUIRE(empty_port.ok());
    AURORA_TEST_CHECK_FALSE(empty_port.value().port().has_value());
    AURORA_TEST_CHECK_EQ(empty_port.value().to_string(), std::string("http://example.com/x"));

    const auto no_authority = aurora::Url::parse("mailto:someone@example.com");
    AURORA_TEST_REQUIRE(no_authority.ok());
    AURORA_TEST_CHECK_TRUE(no_authority.value().is_absolute());
    AURORA_TEST_CHECK_TRUE(no_authority.value().host().empty());
    AURORA_TEST_CHECK_TRUE(no_authority.value().host_kind() == aurora::UrlHostKind::Empty);
    AURORA_TEST_CHECK_EQ(no_authority.value().path(), std::string("someone@example.com"));

    for (const std::string_view bad :
         {std::string_view{"http://h:70000/"}, std::string_view{"http://h:80x/"},
          std::string_view{"http://user name@h/"}, std::string_view{"http://user%xx@h/"}}) {
        const auto parsed = aurora::Url::parse(bad);
        AURORA_TEST_REQUIRE(!parsed.ok());
        AURORA_TEST_CHECK_TRUE(is_url_invalid(parsed.error()));
    }
}

// IDNA 侧只到 RFC 3492 那一层：非 ASCII 标签转 punycode，ASCII 侧仅小写，
// **不做** NFC/NFD 归一与 Unicode 大小写折叠——`ß` 因此落 `xn--zca` 而非 IDNA2003 的 `xn--ssa`。
// 该边界与其后果记在 codespec/specification/01-core.md §11.5。
AURORA_TEST_CASE(idn_hosts_round_trip_through_punycode) {
    struct Row {
        std::string host;  ///< UTF-8 原文
        std::string ascii;
    };
    const std::vector<Row> rows = {
        {.host = "m\xC3\xBC"
                 "nchen",
         .ascii = "xn--mnchen-3ya"},
        {.host = "\xC3\xBC"
                 "ber",
         .ascii = "xn--ber-goa"},
        {.host = "\xC3\xA9"
                 "xample",
         .ascii = "xn--xample-9ua"},
        {.host = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", .ascii = "xn--wgv71a119e"},
        {.host = "\xE4\xB8\xAD\xE6\x96\x87", .ascii = "xn--fiq228c"},
        {.host = "\xC3\x9F", .ascii = "xn--zca"},
        // 多次插入（偏差自适应逐次生效）与 ASCII / 非 ASCII 混排：这两类形态最能暴露
        // `adapt` 里 `numpoints` 与阈值常量的口径差，单码点标签测不到。
        {.host = "na\xC3\xAFve-caf\xC3\xA9", .ascii = "xn--nave-caf-i1a7c"},
        {.host = "m\xC3\xBCnchen\xC3\xA4", .ascii = "xn--mnchen-gua0q"},
        {.host = "\xD1\x82\xD0\xB5\xD1\x81\xD1\x82", .ascii = "xn--e1aybc"},
        {.host = "\xE9\x80\x81\xE5\x88\xAB\xE5\x88\xB0\xE6\x9D\xB1\xE4\xBA\xAC", .ascii = "xn--1lqz8fpa878sml0c"},
    };
    for (const auto &row : rows) {
        const auto parsed = aurora::Url::parse("http://" + row.host + ".test/");
        AURORA_TEST_REQUIRE(parsed.ok());
        AURORA_TEST_CHECK_EQ(parsed.value().host(), row.ascii + ".test");

        const auto display = parsed.value().to_unicode_host();
        AURORA_TEST_REQUIRE(display.ok());
        AURORA_TEST_CHECK_EQ(display.value(), row.host + ".test");

        // 已是 punycode 的输入原样保留，且显示出口稳定。
        const auto literal = aurora::Url::parse("http://" + row.ascii + ".test/");
        AURORA_TEST_REQUIRE(literal.ok());
        AURORA_TEST_CHECK_TRUE(literal.value() == parsed.value());
        const auto again = literal.value().to_unicode_host();
        AURORA_TEST_REQUIRE(again.ok());
        AURORA_TEST_CHECK_EQ(again.value(), row.host + ".test");
    }

    // 字面量主机（IPv4 / IPv6）不经 punycode，原样返回。
    const auto v4 = aurora::Url::parse("http://127.0.0.1/");
    AURORA_TEST_REQUIRE(v4.ok());
    const auto v4_display = v4.value().to_unicode_host();
    AURORA_TEST_REQUIRE(v4_display.ok());
    AURORA_TEST_CHECK_EQ(v4_display.value(), std::string("127.0.0.1"));

    for (const std::string_view bad : {std::string_view{"http://xn--/"}, std::string_view{"http://-bad.example/"},
                                       std::string_view{"http://ba%64.example/"},
                                       std::string_view{"http://bad-.example/"}, std::string_view{"http://a..b/"}}) {
        const auto parsed = aurora::Url::parse(bad);
        AURORA_TEST_REQUIRE(!parsed.ok());
        AURORA_TEST_CHECK_TRUE(is_url_invalid(parsed.error()));
    }
}

// 畸形输入一律以结构化错误抵达，且 message 携 `detail=` 指明被拒的那一段——
// 「手拼 URL」最常见的三类错误（未编码空格、截断三元组、把 scheme 写错）都要有可读的诊断。
AURORA_TEST_CASE(parse_rejects_malformed_input_with_readable_detail) {
    for (const std::string_view bad :
         {std::string_view{"http://ex ample.com/"}, std::string_view{"https://example.com/a b"},
          std::string_view{"https://example.com/a%zz"}, std::string_view{"https://example.com/a%2"},
          std::string_view{"1http://x/"}, std::string_view{"https://example.com/a\nb"},
          std::string_view{"http://h/\xC3\x28"}}) {
        const auto parsed = aurora::Url::parse(bad);
        AURORA_TEST_REQUIRE(!parsed.ok());
        AURORA_TEST_CHECK_TRUE(is_url_invalid(parsed.error()));
        AURORA_TEST_CHECK_TRUE(parsed.error().message.find("detail=") != std::string::npos);
    }
}

// `from_parts` 收**编码态**分量（与 `path()` / `query()` 出口同口径），并补齐授权段必需的前导 `/`。
AURORA_TEST_CASE(from_parts_accepts_encoded_components_and_normalizes_shape) {
    const auto built = aurora::Url::from_parts("HTTP", "Example.COM", std::nullopt, "a/b%20c");
    AURORA_TEST_REQUIRE(built.ok());
    AURORA_TEST_CHECK_EQ(built.value().to_string(), std::string("http://example.com/a/b%20c"));

    const auto with_all = aurora::Url::from_parts("wss", "example.com", std::uint16_t{9000}, "/x",
                                                  std::string_view{"a=1"}, std::string_view{"frag"});
    AURORA_TEST_REQUIRE(with_all.ok());
    AURORA_TEST_CHECK_EQ(with_all.value().to_string(), std::string("wss://example.com:9000/x?a=1#frag"));
    AURORA_TEST_CHECK_TRUE(with_all.value().is_secure());

    // 空查询段与「无查询段」是两个不同值：`?` 本身在报文里有意义。
    const auto empty_query = aurora::Url::from_parts("https", "h", std::nullopt, "/", std::string_view{});
    AURORA_TEST_REQUIRE(empty_query.ok());
    AURORA_TEST_CHECK_TRUE(empty_query.value().has_query());
    AURORA_TEST_CHECK_TRUE(empty_query.value().query().empty());
    AURORA_TEST_CHECK_FALSE(empty_query.value() == with_all.value());

    const auto bare_ipv6 = aurora::Url::from_parts("http", "::1", std::uint16_t{80}, "/");
    AURORA_TEST_REQUIRE(bare_ipv6.ok());
    AURORA_TEST_CHECK_TRUE(bare_ipv6.value().host_kind() == aurora::UrlHostKind::Ipv6);
    AURORA_TEST_CHECK_EQ(bare_ipv6.value().to_string(), std::string("http://[::1]:80/"));

    for (const std::string_view path : {std::string_view{"//ambiguous"}, std::string_view{"a b"}}) {
        const auto bad = aurora::Url::from_parts("http", path == "//ambiguous" ? "" : "h", std::nullopt, path);
        AURORA_TEST_REQUIRE(!bad.ok());
        AURORA_TEST_CHECK_TRUE(is_url_invalid(bad.error()));
    }
    const auto bad_scheme = aurora::Url::from_parts("1http", "h", std::nullopt, "/");
    AURORA_TEST_REQUIRE(!bad_scheme.ok());
    AURORA_TEST_CHECK_TRUE(is_url_invalid(bad_scheme.error()));
}

// 相等比较比的是规范化形态：同一资源的两种写法（大小写、点段、三元组形态）必须判等，
// 而「写了 `?`」与「没写 `?`」判不等——比较口径与序列化口径必须同源，否则缓存键会分裂。
AURORA_TEST_CASE(equality_compares_normalized_form) {
    const auto a = aurora::Url::parse("https://Example.COM:443/a/../b%2Fx?q=1#f");
    const auto b = aurora::Url::parse("HTTPS://example.com:443/b/../b%2Fx?q=1#f");
    const auto c = aurora::Url::parse("https://example.com:443/b%2Fx?q=1");
    AURORA_TEST_REQUIRE(a.ok());
    AURORA_TEST_REQUIRE(b.ok());
    AURORA_TEST_REQUIRE(c.ok());

    AURORA_TEST_CHECK_TRUE(a.value() == b.value());
    AURORA_TEST_CHECK_FALSE(a.value() == c.value());
    AURORA_TEST_CHECK_TRUE(a.value() != c.value());

    const auto default_port = aurora::Url::parse("https://example.com/b%2Fx?q=1#f");
    AURORA_TEST_REQUIRE(default_port.ok());
    // 显式默认端口与省略端口是两个不同值（省略走 `effective_port()` 回落，不改动存值）。
    AURORA_TEST_CHECK_FALSE(a.value() == default_port.value());
}

}  // namespace aurora::test_cases::utest_core_url
