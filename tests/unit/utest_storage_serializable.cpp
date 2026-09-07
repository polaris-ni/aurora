/// 测试类型: unit
/// 目标单元: include/aurora/storage/serializable.h
/// 测试说明: 存储层类型化定制点（ADL 序列化 / 版本 / 类型标签 / 迁移钩子）单元测试

#include <cstdint>
#include <string>

#include "aurora/core/result.h"
#include "aurora/storage/serializable.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_storage_serializable {

namespace st = aurora::storage;

/// 满足 JSON 线的样例类型（ADL 自由函数定制，非侵入）。
struct JsonSample {
    int n = 0;
    std::string s;
};

auto to_storage_json(const JsonSample &v) -> st::Json { return st::Json{{"n", v.n}, {"s", v.s}}; }

auto from_storage_json(JsonSample &out, const st::Json &j) -> au::Result<void> {
    out.n = j.value("n", 0);
    out.s = j.value("s", std::string{});
    return au::Result<void>{};
}

/// 满足二进制线的样例类型。
struct BytesSample {
    std::uint32_t v = 0;
};

auto to_storage_bytes(const BytesSample &v) -> st::StorageBytes {
    st::StorageBytes b;
    b.push_back(static_cast<std::byte>(v.v & 0xFFU));
    return b;
}

auto from_storage_bytes(BytesSample &out, const st::StorageBytes &b) -> au::Result<void> {
    if (b.empty()) {
        return au::Result<void>{};
    }
    out.v = static_cast<std::uint32_t>(b.front());
    return au::Result<void>{};
}

/// 未定制任何序列化函数的类型：不应满足任何 storable concept。
struct NotStorable {
    int x = 0;
};

// ---- 编译期契约 ----
static_assert(st::StorageSerializable<JsonSample>, "JsonSample 应满足 JSON 线可存储");
static_assert(!st::StorageBinarySerializable<JsonSample>, "JsonSample 未定制二进制线");
static_assert(st::StorageBinarySerializable<BytesSample>, "BytesSample 应满足二进制线可存储");
static_assert(st::StorageStorable<JsonSample>, "满足任一线格式 + 可默认构造即可存储");
static_assert(st::StorageStorable<BytesSample>, "满足任一线格式 + 可默认构造即可存储");
static_assert(!st::StorageSerializable<NotStorable>, "未定制 ADL 的类型不应满足");
static_assert(!st::StorageStorable<NotStorable>, "未定制 ADL 的类型不应可存储");

}  // namespace aurora::test_cases::utest_storage_serializable

// 版本定制点：在 storage 命名空间内特化（等价于用户侧的 ADL 覆盖）。
namespace aurora::storage {
template <>
constexpr auto storage_version<aurora::test_cases::utest_storage_serializable::JsonSample>() -> std::uint32_t {
    return 7;
}
}  // namespace aurora::storage

namespace aurora::test_cases::utest_storage_serializable {

AURORA_TEST() {
    // ---- 1. 默认版本号为 1，可经特化覆盖 ----
    {
        AURORA_TEST_CHECK(st::storage_version<BytesSample>() == 1U);
        AURORA_TEST_CHECK(st::storage_version<JsonSample>() == 7U);
    }

    // ---- 2. 类型标签返回缓存引用：同一类型两次调用同一对象（零分配） ----
    {
        const std::string &a = st::storage_type_name<JsonSample>();
        const std::string &b = st::storage_type_name<JsonSample>();
        AURORA_TEST_CHECK(&a == &b);
        AURORA_TEST_CHECK(!a.empty());

        const std::string &c = st::storage_type_name<BytesSample>();
        AURORA_TEST_CHECK(a != c);  // 不同类型标签不同
    }

    // ---- 3. JSON 往返：to → from 保持字段 ----
    {
        const JsonSample src{.n = 42, .s = "hello"};
        const st::Json j = to_storage_json(src);
        AURORA_TEST_CHECK(j["n"] == 42);
        AURORA_TEST_CHECK(j["s"] == "hello");

        JsonSample dst{};
        const auto r = from_storage_json(dst, j);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(dst.n == 42);
        AURORA_TEST_CHECK(dst.s == "hello");
    }

    // ---- 4. 二进制往返 ----
    {
        const BytesSample src{.v = 200U};
        const st::StorageBytes b = to_storage_bytes(src);
        AURORA_TEST_CHECK(b.size() == 1);

        BytesSample dst{};
        const auto r = from_storage_bytes(dst, b);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(dst.v == 200U);
    }

    // ---- 5. 默认迁移钩子为恒等变换 ----
    {
        st::Json j{{"n", 5}};
        const auto migrated = st::migrate_storage<JsonSample>(1U, j);
        AURORA_TEST_CHECK(static_cast<bool>(migrated));
        AURORA_TEST_CHECK(migrated.value()["n"] == 5);

        st::StorageBytes b{std::byte{9}};
        const auto mb = st::migrate_storage<BytesSample>(1U, b);
        AURORA_TEST_CHECK(static_cast<bool>(mb));
        AURORA_TEST_CHECK(mb.value().size() == 1);
    }
}

}  // namespace aurora::test_cases::utest_storage_serializable
