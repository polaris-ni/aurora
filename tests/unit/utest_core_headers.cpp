/// 测试类型: unit
/// 目标单元: include/aurora/core/headers.h
/// 测试说明: `Headers` 值类型行为契约回归：字段名 ASCII 大小写不敏感
///           存取（`get`/`contains`/`remove` 三处同口径）、同名字段保序不折叠（`Set-Cookie`
///           多条独立存活，`values` 按写入顺序返回）、`set` 覆盖全部同名并追加到末尾
///           （线上次序与调用时序一致）、`remove` 计数与幂等、`entries` 保留首次写入的原始
///           拼写、逐条目相等比较（名字折叠 / 值严格 / 顺序敏感）。全程纯内存、无平台分支。

#include <string>
#include <vector>

#include "aurora/core/headers.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_core_headers {

// 字段名大小写不敏感是 RFC 9110 §5.1 的硬性语义：写入拼写与读取拼写可以不同，
// 但原始拼写必须原样保留（HTTP/1.1 线上形态按书写者意图发送）。
AURORA_TEST_CASE(name_access_is_case_insensitive_and_preserves_spelling) {
    aurora::Headers headers;
    headers.set("Content-Type", "application/json");

    AURORA_TEST_CHECK_TRUE(headers.contains("content-type"));
    AURORA_TEST_CHECK_TRUE(headers.contains("CONTENT-TYPE"));
    AURORA_TEST_CHECK_FALSE(headers.contains("content-length"));

    auto got = headers.get("CoNtEnT-tYpE");
    AURORA_TEST_REQUIRE(got.has_value());
    AURORA_TEST_CHECK_EQ(*got, std::string("application/json"));

    // 遍历出口给的是原始拼写，不是折叠后的形态。
    AURORA_TEST_REQUIRE_EQ(headers.entries().size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(headers.entries()[0].name, std::string("Content-Type"));
}

// 同名多值是常态而非异常：`append` 不去重、`values` 保序，`get` 只取首个——
// 三者共同保证 `Set-Cookie` 不会被悄悄折叠成一条（历史缺陷形态）。
AURORA_TEST_CASE(append_keeps_duplicates_in_insertion_order) {
    aurora::Headers headers;
    headers.append("Set-Cookie", "a=1");
    headers.append("X-Trace", "mid");
    headers.append("set-cookie", "b=2");  // 大小写不同的同名写法仍属同一字段

    AURORA_TEST_CHECK_EQ(headers.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(headers.values("Set-Cookie"), std::vector<std::string>{"a=1", "b=2"});

    auto first = headers.get("SET-COOKIE");
    AURORA_TEST_REQUIRE(first.has_value());
    AURORA_TEST_CHECK_EQ(*first, std::string("a=1"));

    // 未命中的字段返回空序列而非「不存在」的错误形态。
    AURORA_TEST_CHECK_TRUE(headers.values("X-Missing").empty());
}

// `set` = 清空同名 + 追加到末尾：既覆盖多值残留，又让整体次序与调用时序一致。
AURORA_TEST_CASE(set_replaces_all_matches_and_moves_to_tail) {
    aurora::Headers headers;
    headers.append("Set-Cookie", "stale=1");
    headers.append("Set-Cookie", "stale=2");
    headers.append("Accept", "*/*");

    headers.set("Set-Cookie", "fresh=1");

    AURORA_TEST_CHECK_EQ(headers.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(headers.values("set-cookie"), std::vector<std::string>{"fresh=1"});
    // 覆盖动作发生在末尾：线上次序随调用时序，而非原位替换。
    AURORA_TEST_CHECK_EQ(headers.entries()[1].name, std::string("Set-Cookie"));
}

// 移除按同名整批走且回报真实条数；重复移除是 no-op（幂等，便于条件清理路径）。
AURORA_TEST_CASE(remove_reports_count_and_is_idempotent) {
    aurora::Headers headers;
    headers.append("X-Dup", "1");
    headers.append("Keep", "y");
    headers.append("x-dup", "2");

    AURORA_TEST_CHECK_EQ(headers.remove("X-DUP"), std::size_t{2});
    AURORA_TEST_CHECK_FALSE(headers.contains("X-Dup"));
    AURORA_TEST_CHECK_EQ(headers.remove("X-Dup"), std::size_t{0});
    AURORA_TEST_CHECK_EQ(headers.size(), std::size_t{1});

    headers.clear();
    AURORA_TEST_CHECK_TRUE(headers.empty());
}

// 相等比较逐条目进行：名字折叠大小写、值严格比较、顺序敏感——
// 顺序敏感是刻意的（折叠后的两条同值头在线上仍是不同报文）。
AURORA_TEST_CASE(equality_folds_names_but_not_values_or_order) {
    aurora::Headers a;
    a.set("Host", "Example.COM");
    a.append("Set-Cookie", "a=1");

    aurora::Headers b;
    b.set("HOST", "Example.COM");
    b.append("set-cookie", "a=1");

    AURORA_TEST_CHECK_TRUE(a == b);

    aurora::Headers c;  // 同内容、反次序
    c.append("Set-Cookie", "a=1");
    c.set("Host", "Example.COM");
    AURORA_TEST_CHECK_FALSE(a == c);

    aurora::Headers d;  // 值大小写敏感
    d.set("host", "example.com");
    AURORA_TEST_CHECK_FALSE(a == d);

    AURORA_TEST_CHECK_TRUE(a != d);

    aurora::Headers e;  // 折叠名字不等于「另一个字段」
    e.set("HostX", "Example.COM");
    AURORA_TEST_CHECK_FALSE(a == e);
}

}  // namespace aurora::test_cases::utest_core_headers
