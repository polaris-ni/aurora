/// 测试类型: unit
/// 目标单元: include/aurora/storage/storage_backend.h
/// 测试说明: StorageBackend 抽象契约——派生类最小四虚函数实现、基类默认 contains/clear/flush/close 行为与
///           错误码归一（NotFound→false、其它错误透传）、默认 transaction 顺序执行与结果透传（无回滚的已知限制）、
///           不可拷贝不可移动的句柄语义

#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/storage/memory_backend.h"
#include "aurora/storage/storage_backend.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_storage_backend {

namespace aus = aurora::storage;
namespace m = aurora::testing::matchers;

/// @brief 测试用最小后端：只实现四个纯虚函数，其余全部走基类默认实现，用于验证默认契约。
class ProbeBackend final : public aus::StorageBackend {
  public:
    std::map<std::string, aus::StorageRecord> store;
    bool broken = false;  // 模拟持久层故障（非 NotFound 的 IO 错误）

    auto put_record(const std::string& id, const aus::StorageRecord& rec) -> Result<void> override {
        if (broken) {
            return Result<void>{make_error(ErrorCode::StorageIoError, "probe broken")};
        }
        store[id] = rec;
        return Result<void>{};
    }

    auto get_record(const std::string& id) -> Result<aus::StorageRecord> override {
        if (broken) {
            return Result<aus::StorageRecord>{make_error(ErrorCode::StorageIoError, "probe broken")};
        }
        const auto it = store.find(id);
        if (it == store.end()) {
            return Result<aus::StorageRecord>{make_error(ErrorCode::StorageRecordNotFound, "probe missing: " + id)};
        }
        return Result<aus::StorageRecord>{it->second};
    }

    auto remove(const std::string& id) -> Result<void> override {
        if (broken) {
            return Result<void>{make_error(ErrorCode::StorageIoError, "probe broken")};
        }
        store.erase(id);
        return Result<void>{};
    }

    auto list() -> Result<std::vector<std::string>> override {
        if (broken) {
            return Result<std::vector<std::string>>{make_error(ErrorCode::StorageIoError, "probe broken")};
        }
        std::vector<std::string> ids;
        ids.reserve(store.size());
        for (const auto& kv : store) {
            ids.push_back(kv.first);
        }
        return Result<std::vector<std::string>>{std::move(ids)};
    }
};

/// @brief 构造一条 JSON 载荷的记录信封。
[[nodiscard]] auto make_json_record(std::string id, aus::Json payload) -> aus::StorageRecord {
    aus::StorageRecord rec;
    rec.id = id;
    rec.type = "__raw__";
    rec.version = 1;
    rec.encoding = aus::StorageEncoding::Json;
    rec.payload = std::move(payload);
    return rec;
}

// 抽象接口句柄语义：禁止拷贝与移动（对标 Surface 的句柄纪律）。
static_assert(!std::is_copy_constructible_v<aus::StorageBackend>);
static_assert(!std::is_copy_assignable_v<aus::StorageBackend>);
static_assert(!std::is_move_constructible_v<aus::StorageBackend>);
static_assert(!std::is_move_assignable_v<aus::StorageBackend>);

AURORA_TEST_CASE(derived_backend_minimal_contract_roundtrip) {
    // 派生类只需实现四虚函数即可获得完整后端：put/get/remove/list 往返一致。
    ProbeBackend be;
    aus::StorageBackend& base = be;

    auto rec = make_json_record("k", aus::Json{{"v", 7}});
    AURORA_TEST_REQUIRE(base.put_record("k", rec));

    const auto got = base.get_record("k");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value().id, std::string("k"));
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(got.value().payload), aus::Json{{"v", 7}});

    const auto ids = base.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::size_is(1));

    AURORA_TEST_REQUIRE(base.remove("k"));
    AURORA_TEST_CHECK_EQ(base.get_record("k").error().code_enum, ErrorCode::StorageRecordNotFound);
}

AURORA_TEST_CASE(default_contains_maps_notfound_to_false) {
    // 默认 contains 契约：存在 → true；缺失（NotFound）归一为 false 且不视为错误。
    ProbeBackend be;
    aus::StorageBackend& base = be;
    AURORA_TEST_REQUIRE(be.put_record("hit", make_json_record("hit", aus::Json{{"v", 1}})));

    const auto hit = base.contains("hit");
    AURORA_TEST_REQUIRE(hit.ok());
    AURORA_TEST_CHECK(hit.value());

    const auto miss = base.contains("miss");
    AURORA_TEST_REQUIRE(miss.ok());
    AURORA_TEST_CHECK(!miss.value());
}

AURORA_TEST_CASE(default_contains_propagates_other_errors) {
    // 默认 contains 契约：非 NotFound 的底层错误原样透传（不吞错）。
    ProbeBackend be;
    be.broken = true;
    aus::StorageBackend& base = be;

    const auto r = base.contains("any");
    AURORA_TEST_CHECK(!r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, ErrorCode::StorageIoError);
}

AURORA_TEST_CASE(default_clear_removes_all_records) {
    // 默认 clear（transaction 内逐条 remove）：清空 Probe 与 Memory 两个具体后端。
    ProbeBackend probe;
    AURORA_TEST_REQUIRE(probe.put_record("a", make_json_record("a", aus::Json{{"v", 1}})));
    AURORA_TEST_REQUIRE(probe.put_record("b", make_json_record("b", aus::Json{{"v", 2}})));
    aus::StorageBackend& probe_base = probe;
    AURORA_TEST_REQUIRE(probe_base.clear());
    AURORA_TEST_CHECK(probe.store.empty());

    aus::MemoryBackend memory;  // Memory 未覆写 clear，同样走默认实现
    AURORA_TEST_REQUIRE(memory.put_record("m", make_json_record("m", aus::Json{{"v", 3}})));
    aus::StorageBackend& memory_base = memory;
    AURORA_TEST_REQUIRE(memory_base.clear());
    const auto ids = memory_base.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::is_empty());
}

AURORA_TEST_CASE(default_flush_and_close_are_noop_success) {
    // 未覆写 flush/close 的后端获得 no-op 默认实现，恒成功。
    ProbeBackend be;
    aus::StorageBackend& base = be;
    AURORA_TEST_CHECK(base.flush().ok());
    AURORA_TEST_CHECK(base.close().ok());
}

AURORA_TEST_CASE(default_transaction_executes_body_and_propagates) {
    // 默认 transaction：顺序执行 body 并透传结果；成功时体内写入提交生效。
    ProbeBackend be;
    aus::StorageBackend& base = be;

    const auto ok = base.transaction([](aus::StorageBackend& b) -> Result<void> {
        auto r = b.put_record("txn", make_json_record("txn", aus::Json{{"v", 1}}));
        if (!r) {
            return r;
        }
        // body 收到的即本后端：体内写入立即可见。
        return b.get_record("txn").ok() ? Result<void>{} : Result<void>{make_error(ErrorCode::GeneralUnknown, "?")};
    });
    AURORA_TEST_REQUIRE(ok.ok());
    AURORA_TEST_REQUIRE(be.store.contains("txn"));

    // 失败透传；默认实现无回滚——体内已完成写入保留（接口注明的已知限制）。
    const auto failed = base.transaction([](aus::StorageBackend& b) -> Result<void> {
        (void)b.put_record("kept", make_json_record("kept", aus::Json{{"v", 2}}));
        return Result<void>{make_error(ErrorCode::GeneralUnknown, "abort")};
    });
    AURORA_TEST_CHECK(!failed.ok());
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, ErrorCode::GeneralUnknown);
    AURORA_TEST_CHECK(be.store.contains("kept"));  // 无原子回滚（文档化限制）
}

AURORA_TEST_CASE(derived_backend_polymorphic_through_base) {
    // 基类指针统一驱动不同具体后端（对标 Surface 多态使用方式）。
    ProbeBackend probe;
    aus::MemoryBackend memory;
    std::vector<aus::StorageBackend*> backends{&probe, &memory};

    for (aus::StorageBackend* be : backends) {
        AURORA_TEST_REQUIRE(be->put_record("poly", make_json_record("poly", aus::Json{{"v", 9}})));
        const auto got = be->get_record("poly");
        AURORA_TEST_REQUIRE(got.ok());
        AURORA_TEST_CHECK_EQ(std::get<aus::Json>(got.value().payload), aus::Json{{"v", 9}});
        AURORA_TEST_REQUIRE(be->remove("poly"));
        AURORA_TEST_CHECK_EQ(be->get_record("poly").error().code_enum, ErrorCode::StorageRecordNotFound);
    }
}

}  // namespace aurora::test_cases::utest_storage_backend
