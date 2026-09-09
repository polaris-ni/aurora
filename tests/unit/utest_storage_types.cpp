/// 测试类型: unit
/// 目标单元: include/aurora/storage/storage_types.h
/// 测试说明: 存储域值类型默认值与构造不变量（StorageRecord/StorageChange/FilesystemOptions）、
///           StorageEncoding/StorageChange::Operation 枚举取值、StorageValue variant 双形态、StorageBytes 基本行为

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "aurora/storage/storage_types.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_storage_types {

namespace aus = aurora::storage;
namespace m = aurora::testing::matchers;  // 匹配器工厂别名

AURORA_TEST_CASE(storage_encoding_enum_has_documented_values) {
    // StorageEncoding 的取值是落盘线格式的序列化契约，须与文档值一致。
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(aus::StorageEncoding::Json), 0);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(aus::StorageEncoding::Binary), 1);
}

AURORA_TEST_CASE(storage_change_operation_values_distinct) {
    // 变更事件的四种操作必须两两可区分（UI 订阅按 op 分派）。
    const std::vector<std::uint8_t> ops{
        static_cast<std::uint8_t>(aus::StorageChange::Operation::Put),
        static_cast<std::uint8_t>(aus::StorageChange::Operation::Remove),
        static_cast<std::uint8_t>(aus::StorageChange::Operation::Clear),
        static_cast<std::uint8_t>(aus::StorageChange::Operation::Batch),
    };
    AURORA_TEST_CHECK_THAT(ops, m::size_is(4));
    AURORA_TEST_CHECK_NE(ops[0], ops[1]);
    AURORA_TEST_CHECK_NE(ops[0], ops[2]);
    AURORA_TEST_CHECK_NE(ops[0], ops[3]);
    AURORA_TEST_CHECK_NE(ops[1], ops[2]);
    AURORA_TEST_CHECK_NE(ops[1], ops[3]);
    AURORA_TEST_CHECK_NE(ops[2], ops[3]);
}

AURORA_TEST_CASE(storage_record_default_construction_invariants) {
    // 默认信封不变量：version=1、Json 线格式、mtime=epoch、blob_ref/type/id 为空、payload 为 null Json。
    const aus::StorageRecord rec;
    AURORA_TEST_CHECK(rec.id.empty());
    AURORA_TEST_CHECK(rec.type.empty());
    AURORA_TEST_CHECK_EQ(rec.version, 1U);
    AURORA_TEST_CHECK(rec.encoding == aus::StorageEncoding::Json);
    AURORA_TEST_CHECK(rec.mtime == std::chrono::system_clock::time_point{});  // 缺失/未知 = epoch
    AURORA_TEST_CHECK(rec.blob_ref.empty());
    AURORA_TEST_CHECK(std::holds_alternative<aus::Json>(rec.payload));
    AURORA_TEST_CHECK(std::get<aus::Json>(rec.payload).is_null());
}

AURORA_TEST_CASE(storage_record_aggregate_fields_preserved) {
    // 聚合构造按声明序逐字段初始化，各字段原样保留。
    const aus::StorageRecord rec{
        .id = "rec1",
        .type = "note",
        .version = 3U,
        .encoding = aus::StorageEncoding::Binary,
        .mtime = std::chrono::system_clock::time_point{std::chrono::milliseconds{123456}},
        .payload = aus::StorageBytes{std::byte{0xAA}},
        .blob_ref = "rec1.bin",
    };
    AURORA_TEST_CHECK_EQ(rec.id, std::string("rec1"));
    AURORA_TEST_CHECK_EQ(rec.type, std::string("note"));
    AURORA_TEST_CHECK_EQ(rec.version, 3U);
    AURORA_TEST_CHECK(rec.encoding == aus::StorageEncoding::Binary);
    AURORA_TEST_CHECK_EQ(std::chrono::duration_cast<std::chrono::milliseconds>(rec.mtime.time_since_epoch()).count(),
                         123456LL);
    AURORA_TEST_CHECK(std::holds_alternative<aus::StorageBytes>(rec.payload));
    AURORA_TEST_CHECK_EQ(std::get<aus::StorageBytes>(rec.payload).size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(rec.blob_ref, std::string("rec1.bin"));
}

AURORA_TEST_CASE(storage_value_variant_dispatch) {
    // StorageValue = variant<Json, StorageBytes>：两种形态可写入、可判别、可取出。
    aus::StorageValue value = aus::Json{{"a", 1}};
    AURORA_TEST_CHECK(std::holds_alternative<aus::Json>(value));
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(value), aus::Json{{"a", 1}});

    value = aus::StorageBytes{std::byte{0x01}, std::byte{0x02}};
    AURORA_TEST_CHECK(std::holds_alternative<aus::StorageBytes>(value));
    AURORA_TEST_CHECK(std::get<aus::StorageBytes>(value) == aus::StorageBytes{std::byte{0x01}, std::byte{0x02}});
}

AURORA_TEST_CASE(storage_bytes_value_semantics) {
    // StorageBytes 是 std::vector<std::byte> 的别名：可拷贝、可比较、可为空。
    const aus::StorageBytes bytes{std::byte{0x00}, std::byte{0xFF}};
    AURORA_TEST_CHECK_EQ(bytes.size(), std::size_t{2});

    const aus::StorageBytes& copy = bytes;  // 值语义拷贝
    AURORA_TEST_CHECK(copy == bytes);

    const aus::StorageBytes empty;
    AURORA_TEST_CHECK(empty.empty());
}

AURORA_TEST_CASE(storage_change_callback_receives_event) {
    // StorageChangeCallback 签名：以 const 引用接收事件，字段完整传递。
    aus::StorageChange sent{.op = aus::StorageChange::Operation::Remove, .id = "k1"};
    aus::StorageChange received{.op = aus::StorageChange::Operation::Put, .id = ""};

    const aus::StorageChangeCallback cb = [&received](const aus::StorageChange& ch) -> void { received = ch; };
    cb(sent);

    AURORA_TEST_CHECK(received.op == aus::StorageChange::Operation::Remove);
    AURORA_TEST_CHECK_EQ(received.id, std::string("k1"));
}

AURORA_TEST_CASE(filesystem_options_defaults_and_overrides) {
    // FilesystemOptions 默认值：root 空（回退默认配置目录）、自动建目录开、跨进程锁关。
    const aus::FilesystemOptions opts;
    AURORA_TEST_CHECK(opts.root.empty());
    AURORA_TEST_CHECK(opts.auto_create_dir);
    AURORA_TEST_CHECK(!opts.cross_process_lock);

    const aus::FilesystemOptions custom{
        .root = "aurora_utest_root", .auto_create_dir = false, .cross_process_lock = true};
    AURORA_TEST_CHECK_EQ(custom.root, std::filesystem::path{"aurora_utest_root"});
    AURORA_TEST_CHECK(!custom.auto_create_dir);
    AURORA_TEST_CHECK(custom.cross_process_lock);
}

}  // namespace aurora::test_cases::utest_storage_types
