/// 测试类型: unit
/// 目标单元: include/aurora/storage/storage_types.h
/// 测试说明: 存储层值模型（StorageRecord 信封 / StorageValue / 变更事件 / 后端选项）单元测试

#include <chrono>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "aurora/storage/storage_types.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_storage_types {

namespace st = aurora::storage;

AURORA_TEST() {
    // ---- 1. StorageRecord 默认不变量：version=1 / Json 编码 / 无类型 / 无 sidecar ----
    {
        const st::StorageRecord rec;
        AURORA_TEST_CHECK(rec.id.empty());
        AURORA_TEST_CHECK(rec.type.empty());
        AURORA_TEST_CHECK(rec.version == 1U);
        AURORA_TEST_CHECK(rec.encoding == st::StorageEncoding::Json);
        AURORA_TEST_CHECK(rec.blob_ref.empty());
        AURORA_TEST_CHECK(rec.mtime == std::chrono::system_clock::time_point{});  // epoch = 未知
    }

    // ---- 2. StorageValue：Json 载荷路由 ----
    {
        st::StorageValue v = st::Json{{"k", 1}};
        AURORA_TEST_CHECK(std::holds_alternative<st::Json>(v));
        AURORA_TEST_CHECK(!std::holds_alternative<st::StorageBytes>(v));
        AURORA_TEST_CHECK(std::get<st::Json>(v)["k"] == 1);
    }

    // ---- 3. StorageValue：二进制载荷路由（零 base64 膨胀） ----
    {
        st::StorageBytes bytes{};
        bytes.push_back(std::byte{0xDE});
        bytes.push_back(std::byte{0xAD});
        st::StorageValue v = bytes;
        AURORA_TEST_CHECK(std::holds_alternative<st::StorageBytes>(v));
        AURORA_TEST_CHECK(std::get<st::StorageBytes>(v).size() == 2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK(std::get<st::StorageBytes>(v)[0] == std::byte{0xDE});
    }

    // ---- 4. 信封携带二进制载荷时以 blob_ref 指向 sidecar ----
    {
        st::StorageRecord rec;
        rec.id = "avatar";
        rec.type = "__raw__";
        rec.encoding = st::StorageEncoding::Binary;
        rec.blob_ref = "avatar.bin";
        rec.payload = st::StorageBytes{std::byte{1}};

        AURORA_TEST_CHECK(rec.encoding == st::StorageEncoding::Binary);
        AURORA_TEST_CHECK(rec.blob_ref == "avatar.bin");
        AURORA_TEST_CHECK(std::holds_alternative<st::StorageBytes>(rec.payload));
    }

    // ---- 5. StorageChange 操作枚举与 id 语义 ----
    {
        const st::StorageChange put{.op = st::StorageChange::Operation::Put, .id = "user:1"};
        AURORA_TEST_CHECK(put.op == st::StorageChange::Operation::Put);
        AURORA_TEST_CHECK(put.id == "user:1");

        const st::StorageChange cleared{.op = st::StorageChange::Operation::Clear, .id = ""};
        AURORA_TEST_CHECK(cleared.id.empty());  // Clear/Batch 不带 id
        AURORA_TEST_CHECK(cleared.op == st::StorageChange::Operation::Clear);
    }

    // ---- 6. FilesystemOptions 默认不变量 ----
    {
        const st::FilesystemOptions opts;
        AURORA_TEST_CHECK(opts.root.empty());  // 空 → 默认配置目录
        AURORA_TEST_CHECK(opts.auto_create_dir);
        AURORA_TEST_CHECK(!opts.cross_process_lock);  // 默认不跨进程加锁
    }
}

}  // namespace aurora::test_cases::utest_storage_types
