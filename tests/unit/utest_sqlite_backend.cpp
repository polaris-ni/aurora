/// 测试类型: unit
/// 目标单元: include/aurora/storage/sqlite_backend.h
/// 测试说明: SqliteBackend 打开语义（内存库 / 文件库 / 坏路径不可用）、JSON 与二进制信封
/// 往返（mtime 毫秒、BLOB 内联无 sidecar、空载荷）、覆盖写、删除幂等、list/contains/clear、
/// 真事务提交与回滚、嵌套事务加入同一事务、文件库重开持久化、Storage::create 门面往返。
/// 注：后端整体被 AURORA_ENABLE_STORAGE_SQLITE（默认 OFF）门控，但**用例恒注册**
/// （TEST-R6：--list 用例集须与源字面量一致），未开启时各用例体内 SKIP。

#include "framework/aurora_test.h"

#ifdef AURORA_ENABLE_STORAGE_SQLITE

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "aurora/storage/sqlite_backend.h"
#include "aurora/storage/storage.h"

namespace aurora::test_cases::utest_sqlite_backend {

namespace aus = aurora::storage;

/// @brief 本用例的文件库路径：框架接管 TMP 后的用例唯一目录 + 固定名，先清场保幂等。
[[nodiscard]] auto fresh_db(std::string_view tag) -> std::filesystem::path {
    const auto path =
        std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_sqlite" /
        (std::string{tag} + ".db");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return path;
}

/// @brief 构造一条 JSON 载荷的记录信封（与 fs 后端测试同形态）。
[[nodiscard]] auto make_json_record(std::string id, aus::Json payload) -> aus::StorageRecord {
    aus::StorageRecord rec;
    rec.id = std::move(id);
    rec.type = "__raw__";
    rec.version = 2;
    rec.encoding = aus::StorageEncoding::Json;
    rec.payload = std::move(payload);
    rec.mtime = std::chrono::system_clock::now();
    return rec;
}

[[nodiscard]] auto make_binary_record(std::string id, aus::StorageBytes payload) -> aus::StorageRecord {
    aus::StorageRecord rec;
    rec.id = std::move(id);
    rec.type = "__raw__";
    rec.version = 1;
    rec.encoding = aus::StorageEncoding::Binary;
    rec.payload = std::move(payload);
    rec.mtime = std::chrono::system_clock::now();
    return rec;
}

[[nodiscard]] auto memory_backend() -> aus::SqliteBackend {
    // prvalue 返回（C++17 保证消除）：后端不可移动，具名返回会触发被删的 move ctor。
    return aus::SqliteBackend{aus::SqliteOptions{.in_memory = true}};
}

[[nodiscard]] auto epoch_ms(const std::chrono::system_clock::time_point &tp) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

AURORA_TEST_CASE(sqlite_backend_type_contract) {
    static_assert(std::is_base_of_v<aus::StorageBackend, aus::SqliteBackend>);
    static_assert(!std::is_copy_constructible_v<aus::SqliteBackend>);
    static_assert(!std::is_move_constructible_v<aus::SqliteBackend>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aus::StorageBackend, aus::SqliteBackend>);
}

AURORA_TEST_CASE(in_memory_backend_opens_and_roundtrips_json) {
    auto be = memory_backend();
    AURORA_TEST_REQUIRE(be.is_open());
    auto rec = make_json_record("user/profile", aus::Json{{"name", "ada"}, {"score", 42}});
    const auto want_ms = epoch_ms(rec.mtime);
    AURORA_TEST_REQUIRE(be.put_record("user/profile", rec));

    const auto got = be.get_record("user/profile");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value().id, "user/profile");
    AURORA_TEST_CHECK_EQ(got.value().type, "__raw__");
    AURORA_TEST_CHECK_EQ(got.value().version, 2U);
    AURORA_TEST_CHECK(got.value().encoding == aus::StorageEncoding::Json);
    AURORA_TEST_CHECK_EQ(epoch_ms(got.value().mtime), want_ms);  // 毫秒精度保真
    AURORA_TEST_CHECK(got.value().blob_ref.empty());             // 无 sidecar 语义
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(got.value().payload), (aus::Json{{"name", "ada"}, {"score", 42}}));
}

AURORA_TEST_CASE(binary_payload_inline_blob_roundtrip) {
    auto be = memory_backend();
    // 含 0x00 与 0xFF 的载荷走 BLOB 内联（base64-free），读回逐字节相等。
    const aus::StorageBytes bytes{std::byte{0x00}, std::byte{0xFF}, std::byte{0x10}, std::byte{0x00}};
    AURORA_TEST_REQUIRE(be.put_record("frame", make_binary_record("frame", bytes)));

    const auto got = be.get_record("frame");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK(got.value().encoding == aus::StorageEncoding::Binary);
    AURORA_TEST_CHECK(got.value().blob_ref.empty());
    AURORA_TEST_CHECK_EQ(std::get<aus::StorageBytes>(got.value().payload), bytes);

    // 空二进制载荷：往返仍为空 bytes（非 Json 回落）。
    AURORA_TEST_REQUIRE(be.put_record("empty", make_binary_record("empty", {})));
    const auto got_empty = be.get_record("empty");
    AURORA_TEST_REQUIRE(got_empty.ok());
    AURORA_TEST_CHECK(got_empty.value().encoding == aus::StorageEncoding::Binary);
    AURORA_TEST_CHECK(std::get<aus::StorageBytes>(got_empty.value().payload).empty());
}

AURORA_TEST_CASE(overwrite_remove_list_contains_clear) {
    auto be = memory_backend();
    AURORA_TEST_REQUIRE(be.put_record("a", make_json_record("a", aus::Json{{"v", 1}})));
    AURORA_TEST_REQUIRE(be.put_record("a", make_json_record("a", aus::Json{{"v", 2}})));  // INSERT OR REPLACE
    const auto got = be.get_record("a");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(got.value().payload)["v"], 2);

    AURORA_TEST_REQUIRE(be.put_record("b", make_json_record("b", aus::Json{{"v", 1}})));
    const auto ids = be.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_EQ(ids.value().size(), 2U);

    const auto missing = be.get_record("zz");
    AURORA_TEST_CHECK(!missing.ok());
    AURORA_TEST_CHECK_EQ(missing.error().code_enum, ErrorCode::StorageRecordNotFound);

    auto has_a = be.contains("a");
    AURORA_TEST_REQUIRE(has_a.ok());
    AURORA_TEST_CHECK(has_a.value());
    auto has_zz = be.contains("zz");
    AURORA_TEST_REQUIRE(has_zz.ok());
    AURORA_TEST_CHECK(!has_zz.value());

    // 删除幂等：不存在亦成功。
    AURORA_TEST_CHECK(be.remove("a").ok());
    AURORA_TEST_CHECK(be.remove("a").ok());

    AURORA_TEST_CHECK(be.clear().ok());
    const auto after = be.list();
    AURORA_TEST_REQUIRE(after.ok());
    AURORA_TEST_CHECK(after.value().empty());
}

AURORA_TEST_CASE(closed_backend_returns_unavailable) {
    // 路径不可达（父级是普通文件）→ 打开失败；全部操作返回 StorageBackendUnavailable。
    const auto blocker = fresh_db("blocked_parent");
    std::ofstream(blocker) << "not a directory";
    aus::SqliteBackend be{aus::SqliteOptions{.path = blocker / "impossible.db"}};
    AURORA_TEST_CHECK(!be.is_open());

    const auto put = be.put_record("x", make_json_record("x", aus::Json{{"v", 1}}));
    AURORA_TEST_CHECK(!put.ok());
    AURORA_TEST_CHECK_EQ(put.error().code_enum, ErrorCode::StorageBackendUnavailable);
    const auto got = be.get_record("x");
    AURORA_TEST_CHECK(!got.ok());
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageBackendUnavailable);

    std::error_code ec;
    std::filesystem::remove(blocker, ec);
}

AURORA_TEST_CASE(transaction_commits_body_writes) {
    auto be = memory_backend();
    const auto r = be.transaction([&](aus::StorageBackend &tx) -> aurora::Result<void> {
        if (auto e = tx.put_record("t1", make_json_record("t1", aus::Json{{"v", 1}})); !e) {
            return e;
        }
        return tx.put_record("t2", make_json_record("t2", aus::Json{{"v", 2}}));
    });
    AURORA_TEST_CHECK(r.ok());
    auto has1 = be.contains("t1");
    auto has2 = be.contains("t2");
    AURORA_TEST_REQUIRE(has1.ok());
    AURORA_TEST_REQUIRE(has2.ok());
    AURORA_TEST_CHECK(has1.value());
    AURORA_TEST_CHECK(has2.value());
}

AURORA_TEST_CASE(transaction_rolls_back_on_error) {
    auto be = memory_backend();
    AURORA_TEST_REQUIRE(be.put_record("keep", make_json_record("keep", aus::Json{{"v", 0}})));

    const aurora::Error boom = aurora::make_error(ErrorCode::StorageIoError, "boom");
    const auto r = be.transaction([&](aus::StorageBackend &tx) -> aurora::Result<void> {
        if (auto e = tx.put_record("dirty", make_json_record("dirty", aus::Json{{"v", 1}})); !e) {
            return e;
        }
        if (auto e = tx.remove("keep"); !e) {
            return e;
        }
        return aurora::Result<void>{boom};
    });
    AURORA_TEST_CHECK(!r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, ErrorCode::StorageIoError);

    // 体内写入与删除全部撤销：keep 仍在、dirty 不存在。
    auto has_keep = be.contains("keep");
    auto has_dirty = be.contains("dirty");
    AURORA_TEST_REQUIRE(has_keep.ok());
    AURORA_TEST_REQUIRE(has_dirty.ok());
    AURORA_TEST_CHECK(has_keep.value());
    AURORA_TEST_CHECK(!has_dirty.value());
}

AURORA_TEST_CASE(nested_transaction_joins_outer) {
    auto be = memory_backend();
    // 内层体失败只由外层定生死：外层吞掉内层错误并提交 → 内外写入全部可见。
    const auto r = be.transaction([&](aus::StorageBackend &outer) -> aurora::Result<void> {
        if (auto e = outer.put_record("outer1", make_json_record("outer1", aus::Json{{"v", 1}})); !e) {
            return e;
        }
        (void)outer.transaction([&](aus::StorageBackend &inner) -> aurora::Result<void> {
            if (auto e = inner.put_record("inner1", make_json_record("inner1", aus::Json{{"v", 1}})); !e) {
                return e;
            }
            return aurora::Result<void>{
                aurora::make_error(ErrorCode::StorageIoError, "inner abort")};
        });
        return outer.put_record("outer2", make_json_record("outer2", aus::Json{{"v", 2}}));
    });
    AURORA_TEST_CHECK(r.ok());
    auto has_outer = be.contains("outer2");
    auto has_inner = be.contains("inner1");
    AURORA_TEST_REQUIRE(has_outer.ok());
    AURORA_TEST_REQUIRE(has_inner.ok());
    AURORA_TEST_CHECK(has_outer.value());
    AURORA_TEST_CHECK(has_inner.value());
}

AURORA_TEST_CASE(file_db_persists_across_reopen) {
    const auto db = fresh_db("persist");
    {
        aus::SqliteBackend be{aus::SqliteOptions{.path = db}};
        AURORA_TEST_REQUIRE(be.is_open());
        AURORA_TEST_REQUIRE(be.put_record("persisted", make_json_record("persisted", aus::Json{{"n", 7}})));
        AURORA_TEST_REQUIRE(be.flush().ok());  // WAL checkpoint 收缩 -wal
    }
    aus::SqliteBackend again{aus::SqliteOptions{.path = db}};
    AURORA_TEST_REQUIRE(again.is_open());
    const auto got = again.get_record("persisted");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(got.value().payload)["n"], 7);
    AURORA_TEST_CHECK(again.close().ok());

    std::error_code ec;
    std::filesystem::remove(db, ec);
    std::filesystem::remove(db.string() + "-wal", ec);
    std::filesystem::remove(db.string() + "-shm", ec);
}

AURORA_TEST_CASE(storage_facade_create_sqlite_roundtrip) {
    const auto db = fresh_db("facade");
    auto st = aus::Storage::create(aus::SqliteOptions{.path = db});
    AURORA_TEST_REQUIRE(st.ok());
    AURORA_TEST_CHECK(st.value().put("k", aus::Json{{"x", 1}}).ok());
    const auto got = st.value().get("k");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value()["x"], 1);

    aus::StorageBytes blob{std::byte{0xDE}, std::byte{0x00}, std::byte{0xAD}};
    AURORA_TEST_CHECK(st.value().put("bin", blob).ok());
    const auto back = st.value().get_bytes("bin");
    AURORA_TEST_REQUIRE(back.ok());
    AURORA_TEST_CHECK_EQ(back.value(), blob);

    std::error_code ec;
    std::filesystem::remove(db, ec);
    std::filesystem::remove(db.string() + "-wal", ec);
    std::filesystem::remove(db.string() + "-shm", ec);
}

}  // namespace aurora::test_cases::utest_sqlite_backend

#else  // AURORA_ENABLE_STORAGE_SQLITE 未开启：用例恒注册，体内 SKIP（TEST-R6 纪律）。

namespace aurora::test_cases::utest_sqlite_backend {

AURORA_TEST_CASE(sqlite_backend_type_contract) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(in_memory_backend_opens_and_roundtrips_json) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(binary_payload_inline_blob_roundtrip) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(overwrite_remove_list_contains_clear) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(closed_backend_returns_unavailable) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(transaction_commits_body_writes) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(transaction_rolls_back_on_error) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(nested_transaction_joins_outer) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(file_db_persists_across_reopen) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}
AURORA_TEST_CASE(storage_facade_create_sqlite_roundtrip) {
    AURORA_TEST_SKIP("AURORA_ENABLE_STORAGE_SQLITE 未开启（默认 OFF）");
}

}  // namespace aurora::test_cases::utest_sqlite_backend

#endif  // AURORA_ENABLE_STORAGE_SQLITE
