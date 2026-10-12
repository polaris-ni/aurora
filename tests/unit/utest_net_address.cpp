/// 测试类型: unit
/// 目标单元: include/aurora/net/address.h
/// 测试说明: aurora::net::Endpoint 值类型的解析与校验判据（CHG-007 底座切片）：IPv4 四段与
///           前导零 / 越界 / 段数畸形拒绝、IPv6 的 `::` 压缩与 RFC 5952 规范化（小写、去前导零、
///           最左最长游程）、内嵌 IPv4 尾段落位、方括号形态与「端口不得内嵌」拒绝、zone id 只在
///           link-local 上合法、回环与通配判定按地址族而非字符串、规范化等价性（不同写法判等）、
///           本机 IPC 路径的长度与 NUL 约束、默认构造的无效占位语义、`to_string()` 三族形态。
///           解析器为仓内手写、不依赖 libc（`inet_pton`），故三平台必须逐值同色，用例不设平台分支。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_address {
namespace {

using aurora::net::AddressFamily;
using aurora::net::Endpoint;

/// @brief 解析数字地址；失败即 REQUIRE 转红（这是「本应成功」的前置步骤，不是被测行为）。
/// @param text 地址文本。
/// @param port 端口。
/// @return 规范化后的端点。
auto must_parse(std::string_view text, std::uint16_t port) -> Endpoint {
    auto built = Endpoint::from_ip(text, port);
    AURORA_TEST_REQUIRE(built.ok());
    return built.value();
}

/// @brief 断言一段地址文本被拒，且落在 `net-endpoint-invalid` 上（按 code 判，不依赖措辞）。
/// @param text 待拒的地址文本。
void expect_rejected(std::string_view text) {
    auto built = Endpoint::from_ip(text, 80);
    AURORA_TEST_REQUIRE_FALSE(built.ok());
    AURORA_TEST_CHECK_EQ(built.error().code, std::string("net-endpoint-invalid"));
}

}  // namespace

// IPv4 正例：四段点分十进制、端口原样保留、回环与通配按地址段判定（127/8 整段皆回环）。
AURORA_TEST_CASE(ipv4_accepts_canonical_forms) {
    const auto loop = must_parse("127.0.0.1", 8080);
    AURORA_TEST_CHECK_EQ(loop.family(), AddressFamily::IPv4);
    AURORA_TEST_CHECK_EQ(loop.host(), std::string("127.0.0.1"));
    AURORA_TEST_CHECK_EQ(loop.port(), std::uint16_t{8080});
    AURORA_TEST_CHECK_TRUE(loop.is_valid());
    AURORA_TEST_CHECK_TRUE(loop.is_loopback());
    AURORA_TEST_CHECK_FALSE(loop.is_unspecified());
    AURORA_TEST_CHECK_EQ(loop.to_string(), std::string("127.0.0.1:8080"));
    AURORA_TEST_CHECK_TRUE(must_parse("127.9.9.9", 1).is_loopback());
    AURORA_TEST_CHECK_FALSE(must_parse("10.0.0.1", 22).is_loopback());

    const auto wildcard = must_parse("0.0.0.0", 0);
    AURORA_TEST_CHECK_TRUE(wildcard.is_unspecified());
    AURORA_TEST_CHECK_FALSE(wildcard.is_loopback());
    AURORA_TEST_CHECK_EQ(wildcard.port(), std::uint16_t{0});
}

// IPv4 畸形一律拒：段数、逐段上限、前导零（与八进制歧义）、空白与内嵌端口。
AURORA_TEST_CASE(ipv4_rejects_malformed_text) {
    expect_rejected("1.2.3");
    expect_rejected("1.2.3.4.5");
    expect_rejected("256.1.1.1");
    expect_rejected("01.2.3.4");
    expect_rejected("1.2.3.");
    expect_rejected(".1.2.3");
    expect_rejected("1.2.3.a");
    expect_rejected("1.2.3.4:8080");
    expect_rejected(" 127.0.0.1");
    expect_rejected("127.0.0.1 ");
    expect_rejected("");
}

// IPv6 规范化逐条对上 RFC 5952：小写、去前导零、最长最左零游程、单段零不压缩。
AURORA_TEST_CASE(ipv6_normalizes_to_rfc5952_text) {
    AURORA_TEST_CHECK_EQ(must_parse("::", 0).host(), std::string("::"));
    AURORA_TEST_CHECK_EQ(must_parse("::1", 22).host(), std::string("::1"));
    AURORA_TEST_CHECK_EQ(must_parse("2001:0db8:0000:0000:0000:0000:0000:0001", 80).host(), std::string("2001:db8::1"));
    AURORA_TEST_CHECK_EQ(must_parse("0:0:1:0:0:2:0:0", 1).host(), std::string("::1:0:0:2:0:0"));
    AURORA_TEST_CHECK_EQ(must_parse("2001:db8:0:1:1:1:1:1", 1).host(), std::string("2001:db8:0:1:1:1:1:1"));
    AURORA_TEST_CHECK_EQ(must_parse("FE80::0001", 1).host(), std::string("fe80::1"));
    AURORA_TEST_CHECK_EQ(must_parse("2001:db8::", 1).host(), std::string("2001:db8::"));
    // 内嵌 IPv4 尾段按两个 16 段落位（192.0.2.1 → c000:201）。
    AURORA_TEST_CHECK_EQ(must_parse("::ffff:192.0.2.1", 1).host(), std::string("::ffff:c000:201"));
}

// IPv6 畸形一律拒：两处压缩、段数溢出、单段过长、悬空冒号、括号误用与端口内嵌。
AURORA_TEST_CASE(ipv6_rejects_malformed_text) {
    expect_rejected("1::2::3");
    expect_rejected("1:2:3:4:5:6:7:8:9");
    expect_rejected("12345::1");
    expect_rejected("2001:db8:");
    expect_rejected(":2001:db8:0:1");
    expect_rejected("1:2:3:4:5:6:7:8g");
    expect_rejected("[127.0.0.1]");
    expect_rejected("[::1]:8080");
    expect_rejected("[::1");
    expect_rejected("::ffff:192.0.2");
    expect_rejected("1:2:3:4:5:6:7:192.0.2.1");
}

// zone id：link-local 上合法（括号与否同解），别处一律拒；渲染时留在方括号内。
AURORA_TEST_CASE(ipv6_zone_only_on_link_local) {
    const auto scoped = must_parse("[fe80::1%eth0]", 22);
    AURORA_TEST_CHECK_EQ(scoped.host(), std::string("fe80::1"));
    AURORA_TEST_CHECK_EQ(scoped.zone(), std::string("eth0"));
    AURORA_TEST_CHECK_EQ(scoped.to_string(), std::string("[fe80::1%eth0]:22"));
    AURORA_TEST_CHECK_EQ(must_parse("fe80::1%1", 22).zone(), std::string("1"));
    AURORA_TEST_CHECK_FALSE(scoped.is_loopback());

    expect_rejected("::1%eth0");
    expect_rejected("2001:db8::1%1");
    expect_rejected("127.0.0.1%eth0");
    expect_rejected("fe80::1%");
    expect_rejected("fe80::1%et h0");
}

// IPv6 回环与通配：`::1` 判回环、`::` 判通配，写法相似度不参与判定。
AURORA_TEST_CASE(ipv6_loopback_and_unspecified) {
    const auto loop = must_parse("0:0:0:0:0:0:0:1", 1);
    AURORA_TEST_CHECK_EQ(loop.family(), AddressFamily::IPv6);
    AURORA_TEST_CHECK_TRUE(loop.is_loopback());
    AURORA_TEST_CHECK_FALSE(loop.is_unspecified());
    AURORA_TEST_CHECK_EQ(loop.to_string(), std::string("[::1]:1"));

    const auto wildcard = must_parse("::", 0);
    AURORA_TEST_CHECK_TRUE(wildcard.is_unspecified());
    AURORA_TEST_CHECK_FALSE(wildcard.is_loopback());
    AURORA_TEST_CHECK_EQ(wildcard.to_string(), std::string("[::]:0"));
}

// 规范化等价性：同一地址的多种写法必须判等，否则缓存键与连接池键会分裂。
AURORA_TEST_CASE(equal_after_normalization) {
    AURORA_TEST_CHECK_TRUE(must_parse("::1", 80) == must_parse("0:0:0:0:0:0:0:1", 80));
    AURORA_TEST_CHECK_TRUE(must_parse("[fe80::0001%eth0]", 80) == must_parse("fe80::1%eth0", 80));
    AURORA_TEST_CHECK_FALSE(must_parse("::1", 80) == must_parse("::1", 81));
    AURORA_TEST_CHECK_FALSE(must_parse("fe80::1%eth0", 80) == must_parse("fe80::1%eth1", 80));
    AURORA_TEST_CHECK_FALSE(must_parse("127.0.0.1", 80) == must_parse("::1", 80));
}

// 便捷工厂免解析直接成型，端口 0 语义保留给 bind（connect 侧的拒绝由传输层判）。
AURORA_TEST_CASE(factory_shortcuts) {
    AURORA_TEST_CHECK_TRUE(Endpoint::loopback(0) == must_parse("127.0.0.1", 0));
    AURORA_TEST_CHECK_TRUE(Endpoint::loopback_v6(4) == must_parse("::1", 4));
    AURORA_TEST_CHECK_TRUE(Endpoint::any(false, 0) == must_parse("0.0.0.0", 0));
    AURORA_TEST_CHECK_TRUE(Endpoint::any(true, 0) == must_parse("::", 0));
    AURORA_TEST_CHECK_EQ(Endpoint::loopback(9).port(), std::uint16_t{9});
}

// 默认构造是「无效占位」：不携带地址、渲染为空串，且与任何有效端点都不判等。
AURORA_TEST_CASE(default_endpoint_is_invalid) {
    const Endpoint blank;
    AURORA_TEST_CHECK_FALSE(blank.is_valid());
    AURORA_TEST_CHECK_EQ(blank.to_string(), std::string());
    AURORA_TEST_CHECK_FALSE(blank.is_loopback());
    AURORA_TEST_CHECK_FALSE(blank.is_unspecified());
    AURORA_TEST_CHECK_FALSE(blank == Endpoint::loopback(0));
}

// 本机 IPC：长度与 NUL 约束在值类型层拦住（不等 bind 才发现路径过长），管道名与文件路径同口。
AURORA_TEST_CASE(local_path_endpoint) {
    auto socket_path = Endpoint::local("/run/user/1000/aurora-inspector.sock");
    AURORA_TEST_REQUIRE(socket_path.ok());
    AURORA_TEST_CHECK_TRUE(socket_path.value().is_local_path());
    AURORA_TEST_CHECK_EQ(socket_path.value().family(), AddressFamily::LocalPath);
    AURORA_TEST_CHECK_EQ(socket_path.value().to_string(), std::string("unix:/run/user/1000/aurora-inspector.sock"));
    AURORA_TEST_CHECK_EQ(socket_path.value().port(), std::uint16_t{0});
    AURORA_TEST_CHECK_FALSE(socket_path.value().is_loopback());

    auto pipe = Endpoint::local(R"(\\.\pipe\aurora-inspector)");
    AURORA_TEST_REQUIRE(pipe.ok());
    AURORA_TEST_CHECK_TRUE(pipe.value().is_local_path());

    auto empty = Endpoint::local("");
    AURORA_TEST_REQUIRE_FALSE(empty.ok());
    AURORA_TEST_CHECK_EQ(empty.error().code, std::string("net-endpoint-invalid"));

    const std::string at_limit(aurora::net::AURORA_NET_MAX_LOCAL_PATH, 'x');
    AURORA_TEST_CHECK_TRUE(Endpoint::local(at_limit).ok());
    const std::string over_limit(aurora::net::AURORA_NET_MAX_LOCAL_PATH + 1, 'x');
    AURORA_TEST_CHECK_FALSE(Endpoint::local(over_limit).ok());

    const char with_nul[] = {'a', '\0', 'b'};
    AURORA_TEST_CHECK_FALSE(Endpoint::local(std::string_view(with_nul, sizeof(with_nul))).ok());
}

// 地址字节按网络序暴露（实现侧据此组装 sockaddr，公共头不引平台类型）。
AURORA_TEST_CASE(address_bytes_are_network_order) {
    const auto v4 = must_parse("192.168.1.254", 1);
    const auto &v4_bytes = v4.address_bytes();
    AURORA_TEST_CHECK_EQ(static_cast<int>(v4_bytes[0]), 192);
    AURORA_TEST_CHECK_EQ(static_cast<int>(v4_bytes[1]), 168);
    AURORA_TEST_CHECK_EQ(static_cast<int>(v4_bytes[2]), 1);
    AURORA_TEST_CHECK_EQ(static_cast<int>(v4_bytes[3]), 254);

    const auto v6 = must_parse("2001:db8::1", 1);
    const auto &v6_bytes = v6.address_bytes();
    AURORA_TEST_CHECK_EQ(static_cast<int>(v6_bytes[0]), 0x20);
    AURORA_TEST_CHECK_EQ(static_cast<int>(v6_bytes[1]), 0x01);
    AURORA_TEST_CHECK_EQ(static_cast<int>(v6_bytes[3]), 0xb8);
    AURORA_TEST_CHECK_EQ(static_cast<int>(v6_bytes[14]), 0x00);
    AURORA_TEST_CHECK_EQ(static_cast<int>(v6_bytes[15]), 0x01);
}

}  // namespace aurora::test_cases::utest_net_address
