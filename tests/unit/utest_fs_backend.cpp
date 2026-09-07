/// 测试类型: unit
/// 目标单元: include/aurora/storage/fs_backend.h
/// 测试说明: FilesystemBackend 落盘后端（原子写 / 信封往返 / 列举 / 清退）单元测试

#include <filesystem>
#include <string>
#include <vector>

#include "aurora/storage/fs_backend.h"
#include "aurora/storage/storage_types.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_fs_backend {

namespace st = aurora::storage;

namespace {

auto tmp_root() -> std::filesystem::path {
    static const std::filesystem::path DIR = []() -> std::filesystem::path {
        const auto d = std::filesystem::temp_directory_path() / "aurora_fs_backend_test";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return DIR;
}

auto fresh_backend(const std::string &sub) -> st::FilesystemBackend {
    const auto dir = tmp_root() / sub;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    st::FilesystemOptions opts;
    opts.root = dir;
    return st::FilesystemBackend{opts};
}

auto make_record(const std::string &id, int v) -> st::StorageRecord {
    st::StorageRecord rec;
    rec.id = id;
    rec.type = "sample";
    rec.payload = st::Json{{"v", v}};
    return rec;
}

}  // namespace

AURORA_TEST() {
    // ---- 1. 目录可写时后端打开成功 ----
    {
        auto backend = fresh_backend("open");
        AURORA_TEST_CHECK(backend.is_open());
    }

    // ---- 2. put → get 信封往返（版本号/编码/载荷保持） ----
    {
        auto backend = fresh_backend("roundtrip");
        AURORA_TEST_CHECK(static_cast<bool>(backend.put_record("rec1", make_record("rec1", 7))));

        const auto got = backend.get_record("rec1");
        AURORA_TEST_CHECK(static_cast<bool>(got));
        AURORA_TEST_CHECK(got.value().id == "rec1");
        AURORA_TEST_CHECK(got.value().type == "sample");
        AURORA_TEST_CHECK(got.value().version == 1U);
        AURORA_TEST_CHECK(got.value().encoding == st::StorageEncoding::Json);
        AURORA_TEST_CHECK(std::holds_alternative<st::Json>(got.value().payload));
        AURORA_TEST_CHECK(std::get<st::Json>(got.value().payload)["v"] == 7);
    }

    // ---- 3. 覆写同一 id 后读到新值 ----
    {
        auto backend = fresh_backend("overwrite");
        (void)backend.put_record("k", make_record("k", 1));
        (void)backend.put_record("k", make_record("k", 2));
        AURORA_TEST_CHECK(std::get<st::Json>(backend.get_record("k").value().payload)["v"] == 2);
    }

    // ---- 4. 读取不存在的 id 返回 StorageRecordNotFound ----
    {
        auto backend = fresh_backend("missing");
        const auto r = backend.get_record("nope");
        AURORA_TEST_CHECK(!static_cast<bool>(r));
        AURORA_TEST_CHECK(r.error().code_enum == au::ErrorCode::StorageRecordNotFound);
    }

    // ---- 5. list 反映已写入记录数 ----
    {
        auto backend = fresh_backend("list");
        (void)backend.put_record("a", make_record("a", 1));
        (void)backend.put_record("b", make_record("b", 2));
        const auto ids = backend.list();
        AURORA_TEST_CHECK(static_cast<bool>(ids));
        AURORA_TEST_CHECK(ids.value().size() == 2);
    }

    // ---- 6. remove 生效且幂等 ----
    {
        auto backend = fresh_backend("remove");
        (void)backend.put_record("a", make_record("a", 1));
        AURORA_TEST_CHECK(static_cast<bool>(backend.remove("a")));
        AURORA_TEST_CHECK(!static_cast<bool>(backend.get_record("a")));
        AURORA_TEST_CHECK(static_cast<bool>(backend.remove("a")));  // 重复删除仍成功
    }

    // ---- 7. clear 清空全部记录 ----
    {
        auto backend = fresh_backend("clear");
        (void)backend.put_record("a", make_record("a", 1));
        (void)backend.put_record("b", make_record("b", 2));
        AURORA_TEST_CHECK(static_cast<bool>(backend.clear()));
        AURORA_TEST_CHECK(backend.list().value().empty());
    }

    // ---- 8. 写入时间按毫秒精度往返保持 ----
    {
        auto backend = fresh_backend("mtime");
        const auto stamp = std::chrono::system_clock::now();
        st::StorageRecord rec = make_record("t", 1);
        rec.mtime = stamp;
        AURORA_TEST_CHECK(static_cast<bool>(backend.put_record("t", rec)));

        const auto got = backend.get_record("t");
        AURORA_TEST_CHECK(got.value().mtime != std::chrono::system_clock::time_point{});
        const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(got.value().mtime - stamp).count();
        AURORA_TEST_CHECK(delta == 0);  // 毫秒截断后应完全一致
    }
}

}  // namespace aurora::test_cases::utest_fs_backend
