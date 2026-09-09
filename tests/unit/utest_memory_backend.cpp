/// 测试类型: unit
/// 目标单元: include/aurora/storage/memory_backend.h
/// 测试说明: MemoryBackend 信封级 put/get/remove/list 基本行为、键缺失与覆盖写语义、经基类默认实现的
///           contains/clear、Memory 特有的快照事务提交/回滚，以及默认 flush/close 空实现

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "aurora/storage/memory_backend.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_memory_backend {

namespace aus = aurora::storage;
namespace m = aurora::testing::matchers;

/// @brief 构造一条 JSON 载荷的记录信封。
[[nodiscard]] auto make_json_record(std::string id, aus::Json payload) -> aus::StorageRecord {
    aus::StorageRecord rec;
    rec.id = id;
    rec.type = "__raw__";
    rec.version = 1;
    rec.encoding = aus::StorageEncoding::Json;
    rec.payload = std::move(payload);
    rec.mtime = std::chrono::system_clock::now();
    return rec;
}

AURORA_TEST_CASE(put_get_record_roundtrip) {
    // 写入信封 → 读回：id/type/version/encoding/mtime/payload 逐字段保真。
    aus::MemoryBackend be;
    auto rec = make_json_record("k1", aus::Json{{"name", "ada"}, {"score", 42}});
    const auto mtime_before = rec.mtime;
    AURORA_TEST_REQUIRE(be.put_record("k1", rec));

    const auto got = be.get_record("k1");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value().id, std::string("k1"));
    AURORA_TEST_CHECK_EQ(got.value().type, std::string("__raw__"));
    AURORA_TEST_CHECK_EQ(got.value().version, 1U);
    AURORA_TEST_CHECK(got.value().encoding == aus::StorageEncoding::Json);
    AURORA_TEST_CHECK(got.value().mtime == mtime_before);
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(got.value().payload), aus::Json{{"name", "ada"}, {"score", 42}});
}

AURORA_TEST_CASE(get_missing_record_not_found) {
    // 键不存在：返回 StorageRecordNotFound 结构化错误，而非抛异常或空值。
    aus::MemoryBackend be;
    const auto got = be.get_record("nope");
    AURORA_TEST_CHECK(!got.ok());
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageRecordNotFound);
}

AURORA_TEST_CASE(put_overwrites_existing_record) {
    // 同 id 覆盖写：整体替换信封，读回为新内容。
    aus::MemoryBackend be;
    AURORA_TEST_REQUIRE(be.put_record("k", make_json_record("k", aus::Json{{"v", 1}})));
    AURORA_TEST_CHECK_EQ(be.list().value().size(), std::size_t{1});  // 覆盖不新增键

    auto rec2 = make_json_record("k", aus::Json{{"v", 2}});
    rec2.version = 5;
    AURORA_TEST_REQUIRE(be.put_record("k", rec2));

    const auto got = be.get_record("k");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value().version, 5U);
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(got.value().payload), aus::Json{{"v", 2}});
}

AURORA_TEST_CASE(remove_is_idempotent) {
    // remove 契约：删除存在的记录；删除不存在的记录同样返回成功（幂等）。
    aus::MemoryBackend be;
    AURORA_TEST_REQUIRE(be.put_record("gone", make_json_record("gone", aus::Json{{"v", 1}})));
    AURORA_TEST_REQUIRE(be.remove("gone"));
    const auto got = be.get_record("gone");
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageRecordNotFound);

    AURORA_TEST_REQUIRE(be.remove("gone"));      // 重复删除仍成功
    AURORA_TEST_REQUIRE(be.remove("never-was")); // 从未存在的键亦成功
}

AURORA_TEST_CASE(list_returns_all_ids) {
    // list：返回全部已写入 id；空库返回空表。
    aus::MemoryBackend be;
    AURORA_TEST_REQUIRE_THAT(be.list().value(), m::is_empty());

    AURORA_TEST_REQUIRE(be.put_record("a", make_json_record("a", aus::Json{{"v", 1}})));
    AURORA_TEST_REQUIRE(be.put_record("b", make_json_record("b", aus::Json{{"v", 2}})));
    AURORA_TEST_REQUIRE(be.put_record("c", make_json_record("c", aus::Json{{"v", 3}})));

    const auto ids = be.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::size_is(3));
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"a"}));
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"b"}));
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"c"}));
}

AURORA_TEST_CASE(contains_via_base_true_false) {
    // 经基类默认 contains：存在 → true；缺失 → false（将 NotFound 归一为布尔，而非错误）。
    aus::MemoryBackend be;
    aus::StorageBackend& base = be;
    AURORA_TEST_REQUIRE(be.put_record("hit", make_json_record("hit", aus::Json{{"v", 1}})));

    const auto hit = base.contains("hit");
    AURORA_TEST_REQUIRE(hit.ok());
    AURORA_TEST_CHECK(hit.value());

    const auto miss = base.contains("miss");
    AURORA_TEST_REQUIRE(miss.ok());
    AURORA_TEST_CHECK(!miss.value());
}

AURORA_TEST_CASE(clear_via_base_empties_store) {
    // 经基类默认 clear（transaction + 逐条 remove）：清空后 list 为空。
    aus::MemoryBackend be;
    aus::StorageBackend& base = be;
    AURORA_TEST_REQUIRE(be.put_record("a", make_json_record("a", aus::Json{{"v", 1}})));
    AURORA_TEST_REQUIRE(be.put_record("b", make_json_record("b", aus::Json{{"v", 2}})));

    AURORA_TEST_REQUIRE(base.clear());
    const auto ids = be.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::is_empty());
}

AURORA_TEST_CASE(transaction_commits_on_success) {
    // 事务成功：体内全部写入提交生效。
    aus::MemoryBackend be;
    const auto r = be.transaction([](aus::StorageBackend& b) -> Result<void> {
        auto r1 = b.put_record("t1", aus::StorageRecord{.id = "t1", .payload = aus::Json{{"v", 1}}});
        auto r2 = b.put_record("t2", aus::StorageRecord{.id = "t2", .payload = aus::Json{{"v", 2}}});
        if (!r1 || !r2) {
            return Result<void>{make_error(ErrorCode::GeneralUnknown, "unexpected put failure")};
        }
        return Result<void>{};
    });
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_REQUIRE(be.get_record("t1").ok());
    AURORA_TEST_REQUIRE(be.get_record("t2").ok());
}

AURORA_TEST_CASE(transaction_rolls_back_on_failure) {
    // Memory 特有快照事务：体内失败 → 精确回滚到事务前快照（对标 Sqlite 真事务）。
    aus::MemoryBackend be;
    AURORA_TEST_REQUIRE(be.put_record("keep", make_json_record("keep", aus::Json{{"v", 1}})));

    const auto r = be.transaction([](aus::StorageBackend& b) -> Result<void> {
        (void)b.put_record("txn", aus::StorageRecord{.id = "txn", .payload = aus::Json{{"v", 2}}});
        (void)b.remove("keep");
        return Result<void>{make_error(ErrorCode::GeneralUnknown, "abort on purpose")};
    });
    AURORA_TEST_CHECK(!r.ok());

    // 回滚后：事务内新增消失、被删记录复活。
    const auto added = be.get_record("txn");
    AURORA_TEST_CHECK(!added.ok());
    AURORA_TEST_CHECK_EQ(added.error().code_enum, ErrorCode::StorageRecordNotFound);
    const auto kept = be.get_record("keep");
    AURORA_TEST_REQUIRE(kept.ok());
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(kept.value().payload), aus::Json{{"v", 1}});
}

AURORA_TEST_CASE(binary_payload_roundtrip) {
    // 二进制载荷信封在内存后端原样存取。
    aus::MemoryBackend be;
    auto rec = make_json_record("bin", aus::Json{});
    rec.encoding = aus::StorageEncoding::Binary;
    rec.payload = aus::StorageBytes{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    rec.blob_ref = "bin.bin";
    AURORA_TEST_REQUIRE(be.put_record("bin", rec));

    const auto got = be.get_record("bin");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK(got.value().encoding == aus::StorageEncoding::Binary);
    AURORA_TEST_CHECK_EQ(got.value().blob_ref, std::string("bin.bin"));
    AURORA_TEST_CHECK(std::get<aus::StorageBytes>(got.value().payload) ==
                      aus::StorageBytes{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}});
}

AURORA_TEST_CASE(flush_and_close_default_success) {
    // 内存后端未覆写 flush/close：走基类默认 no-op，恒返回成功。
    aus::MemoryBackend be;
    aus::StorageBackend& base = be;
    AURORA_TEST_CHECK(base.flush().ok());
    AURORA_TEST_CHECK(base.close().ok());
}

}  // namespace aurora::test_cases::utest_memory_backend
