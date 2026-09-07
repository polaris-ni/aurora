/// 测试类型: unit
/// 目标单元: include/aurora/storage/storage_backend.h
/// 测试说明: StorageBackend 抽象默认实现（contains / clear / transaction / flush / close）契约单元测试

#include <map>
#include <string>
#include <vector>

#include "aurora/storage/storage_backend.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_storage_backend {

namespace st = aurora::storage;

namespace {

/// 最小内存后端：只实现四个纯虚方法，其余沿用基类默认实现（本测试的被测对象）。
class MockBackend final : public st::StorageBackend {
  public:
    [[nodiscard]] auto put_record(const std::string &id, const st::StorageRecord &rec) -> au::Result<void> override {
        records_[id] = rec;
        return au::Result<void>{};
    }

    [[nodiscard]] auto get_record(const std::string &id) -> au::Result<st::StorageRecord> override {
        const auto it = records_.find(id);
        if (it == records_.end()) {
            return au::Result<st::StorageRecord>{au::make_error(au::ErrorCode::StorageRecordNotFound, "missing")};
        }
        return au::Result<st::StorageRecord>{it->second};
    }

    [[nodiscard]] auto remove(const std::string &id) -> au::Result<void> override {
        records_.erase(id);  // 幂等
        return au::Result<void>{};
    }

    [[nodiscard]] auto list() -> au::Result<std::vector<std::string>> override {
        std::vector<std::string> ids;
        ids.reserve(records_.size());
        for (const auto &[id, rec] : records_) {
            (void)rec;
            ids.push_back(id);
        }
        return au::Result<std::vector<std::string>>{std::move(ids)};
    }

  private:
    std::map<std::string, st::StorageRecord> records_;
};

auto make_record(const std::string &id) -> st::StorageRecord {
    st::StorageRecord rec;
    rec.id = id;
    rec.payload = st::Json{{"v", id}};
    return rec;
}

}  // namespace

AURORA_TEST() {
    // ---- 1. put → get 往返 ----
    {
        MockBackend b;
        AURORA_TEST_CHECK(static_cast<bool>(b.put_record("a", make_record("a"))));
        const auto got = b.get_record("a");
        AURORA_TEST_CHECK(static_cast<bool>(got));
        AURORA_TEST_CHECK(got.value().id == "a");
        AURORA_TEST_CHECK(std::get<st::Json>(got.value().payload)["v"] == "a");
    }

    // ---- 2. 默认 contains：命中最 true，未命中 false（NotFound 不算错误） ----
    {
        MockBackend b;
        (void)b.put_record("a", make_record("a"));
        const auto hit = b.contains("a");
        AURORA_TEST_CHECK(static_cast<bool>(hit));
        AURORA_TEST_CHECK(hit.value());

        const auto miss = b.contains("nope");
        AURORA_TEST_CHECK(static_cast<bool>(miss));  // 查询本身成功
        AURORA_TEST_CHECK(!miss.value());
        AURORA_TEST_CHECK(miss.value() == false);
    }

    // ---- 3. remove 幂等：删除不存在的 id 仍成功 ----
    {
        MockBackend b;
        AURORA_TEST_CHECK(static_cast<bool>(b.remove("ghost")));
        AURORA_TEST_CHECK(static_cast<bool>(b.remove("ghost")));
    }

    // ---- 4. 默认 transaction：顺序执行 body，成功则透传 ----
    {
        MockBackend b;
        int steps = 0;
        const auto r = b.transaction([&](st::StorageBackend &tx) -> au::Result<void> {
            ++steps;
            (void)tx.put_record("x", make_record("x"));
            ++steps;
            return au::Result<void>{};
        });
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(steps == 2);
        AURORA_TEST_CHECK(static_cast<bool>(b.get_record("x")));
    }

    // ---- 5. 默认 transaction：body 失败则整体失败（尽力而为，无自动回滚） ----
    {
        MockBackend b;
        const auto r = b.transaction([&](st::StorageBackend &tx) -> au::Result<void> {
            (void)tx.put_record("y", make_record("y"));  // 这一步会留下
            return au::Result<void>{au::make_error(au::ErrorCode::StorageRecordNotFound, "boom")};
        });
        AURORA_TEST_CHECK(!static_cast<bool>(r));
        AURORA_TEST_CHECK(r.error().code_enum == au::ErrorCode::StorageRecordNotFound);
        AURORA_TEST_CHECK(static_cast<bool>(b.get_record("y")));  // 已知限制：不自动撤销
    }

    // ---- 6. 默认 clear：经 transaction + list + remove 清空 ----
    {
        MockBackend b;
        (void)b.put_record("a", make_record("a"));
        (void)b.put_record("b", make_record("b"));
        AURORA_TEST_CHECK(b.list().value().size() == 2);

        AURORA_TEST_CHECK(static_cast<bool>(b.clear()));
        AURORA_TEST_CHECK(b.list().value().empty());
        AURORA_TEST_CHECK(!static_cast<bool>(b.get_record("a")));
    }

    // ---- 7. 默认 flush / close 为 no-op 且成功 ----
    {
        MockBackend b;
        AURORA_TEST_CHECK(static_cast<bool>(b.flush()));
        AURORA_TEST_CHECK(static_cast<bool>(b.close()));
    }

    // ---- 8. 基类指针可多态驱动默认实现 ----
    {
        MockBackend impl;
        st::StorageBackend &base = impl;
        (void)base.put_record("p", make_record("p"));
        AURORA_TEST_CHECK(base.contains("p").value());
        AURORA_TEST_CHECK(static_cast<bool>(base.clear()));
        AURORA_TEST_CHECK(base.list().value().empty());
    }
}

}  // namespace aurora::test_cases::utest_storage_backend
