/// 测试类型: unit
/// 目标单元: include/aurora/storage/serializable.h
/// 测试说明: storage_version / storage_type_name 默认定制点与 ADL 用户覆盖、migrate_storage 默认恒等迁移、
///           StorageSerializable / StorageBinarySerializable / StorageStorable 概念判定、JSON 与二进制两条线格式往返

#include <cstddef>
#include <cstdint>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "aurora/core/result.h"  // serializable.h 使用 Result 但未自带该 include，测试侧显式引入
#include "aurora/storage/serializable.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_serializable {

namespace aus = aurora::storage;

// ============================================================================
// 测试用类型：各自置于独立子命名空间，使 ADL 关联命名空间互不干扰。
// ============================================================================

namespace json_line {

/// @brief 仅支持 JSON 线格式的可序列化类型。
struct Note {
    std::string title;
    int priority = 0;
};

inline auto to_storage_json(const Note& n) -> aus::Json {
    return aus::Json{{"title", n.title}, {"priority", n.priority}};
}

inline auto from_storage_json(Note& n, const aus::Json& j) -> Result<void> {
    if (!j.contains("title")) {
        return Result<void>{make_error(ErrorCode::StorageRecordCorrupt, "note missing title")};
    }
    n.title = j.at("title").get<std::string>();
    n.priority = j.value("priority", 0);
    return Result<void>{};
}

}  // namespace json_line
using Note = json_line::Note;

namespace binary_line {

/// @brief 仅支持原生二进制线格式的可序列化类型（玩具编码：tag 字节序列 + 末字节 size）。
struct Chunk {
    std::string tag;
    int size = 0;
};

inline auto to_storage_bytes(const Chunk& c) -> aus::StorageBytes {
    aus::StorageBytes out;
    for (const char ch : c.tag) {
        out.push_back(static_cast<std::byte>(ch));
    }
    out.push_back(static_cast<std::byte>(c.size));
    return out;
}

inline auto from_storage_bytes(Chunk& c, const aus::StorageBytes& b) -> Result<void> {
    if (b.empty()) {
        return Result<void>{make_error(ErrorCode::StorageRecordCorrupt, "chunk payload empty")};
    }
    std::string tag;
    for (std::size_t i = 0; i + 1 < b.size(); ++i) {
        tag.push_back(static_cast<char>(b[i]));
    }
    c.tag = std::move(tag);
    c.size = static_cast<int>(std::to_integer<unsigned char>(b.back()));
    return Result<void>{};
}

}  // namespace binary_line
using Chunk = binary_line::Chunk;

namespace versioned {

/// @brief 放在独立子命名空间，避免本 TU 其它类型的 ADL 误中本覆盖。
struct Tag {};

/// @brief 用户命名空间覆盖：带 `const Tag*` tag 实参（ADL 定制点约定）。
inline auto storage_version(const Tag*) -> std::uint32_t { return 7; }

}  // namespace versioned

/// @brief 无任何序列化定制点的普通类型。
struct Plain {
    int x = 0;
};

/// @brief 可序列化但不可默认构造的类型（不满足 StorageStorable）。
struct NoDefaultCtor {
    explicit NoDefaultCtor(int) {}

    std::string title;
};

inline auto to_storage_json(const NoDefaultCtor& n) -> aus::Json {
    return aus::Json{{"title", n.title}};
}

inline auto from_storage_json(NoDefaultCtor& n, const aus::Json& j) -> Result<void> {
    n.title = j.value("title", "");
    return Result<void>{};
}

// 概念判定：编译期即冻结「哪些类型能被门面接受」的契约。
static_assert(aus::StorageSerializable<Note>, "Note 应满足 JSON 序列化概念");
static_assert(!aus::StorageSerializable<Chunk>, "Chunk 未提供 JSON 定制点");
static_assert(!aus::StorageSerializable<Plain>, "Plain 无任何定制点");
static_assert(aus::StorageBinarySerializable<Chunk>, "Chunk 应满足二进制序列化概念");
static_assert(!aus::StorageBinarySerializable<Note>, "Note 未提供二进制定制点");
static_assert(aus::StorageStorable<Note>, "Note 应满足门面存储概念");
static_assert(aus::StorageStorable<Chunk>, "Chunk 应满足门面存储概念");
static_assert(!aus::StorageStorable<Plain>, "Plain 不可经门面存储");
static_assert(!aus::StorageStorable<NoDefaultCtor>, "不可默认构造的类型不满足 StorageStorable");
static_assert(aus::StorageSerializable<NoDefaultCtor>, "NoDefaultCtor 的 JSON 定制点仍成立");
static_assert(aus::storage_version(static_cast<const Note*>(nullptr)) == 1, "默认版本号恒为 1");

// ============================================================================
// 测试专用分发器（仅本 TU 可见，不进库头文件）：在库命名空间内复现
// storage.h put<T>/get<T> 的查找环境，验证「模板默认 + ADL 用户覆盖」的真实决议路径。
// 注意：必须在全局层级重开 aurora::storage——在测试命名空间内写 namespace aurora::storage
// 会按 C++20 规则创建嵌套的 test_cases::utest_serializable::aurora::storage。
// ============================================================================

}  // namespace aurora::test_cases::utest_serializable

namespace aurora::storage {
template <typename T>
auto utest_dispatch_storage_version() -> std::uint32_t {
    // 与门面同款调用：`const T*` 指针实参触发 ADL（零参模板在 GCC/MSVC 下不会查用户命名空间）。
    return storage_version(static_cast<const T*>(nullptr));
}
}  // namespace aurora::storage

namespace aurora::test_cases::utest_serializable {


AURORA_TEST_CASE(storage_version_defaults_to_one) {
    // 未覆盖时所有类型默认版本号 1（编译期常量，同时以运行期断言复核）。
    static_assert(aus::storage_version(static_cast<const Plain*>(nullptr)) == 1);
    AURORA_TEST_CHECK_EQ(aus::storage_version(static_cast<const Note*>(nullptr)), 1U);
    AURORA_TEST_CHECK_EQ(aus::storage_version(static_cast<const Chunk*>(nullptr)), 1U);
}

AURORA_TEST_CASE(storage_version_user_override_wins_via_adl) {
    // 库命名空间内的非限定调用（与 put<T>/get<T> 同环境）：定制点以 `const Tag*` 实参触发 ADL，
    // 用户命名空间的非模板同名函数进入重载集且优先于模板 → 覆盖生效。
    AURORA_TEST_CHECK_EQ(aurora::storage::utest_dispatch_storage_version<versioned::Tag>(), 7U);
    // 未提供覆盖的类型仍走默认模板。
    AURORA_TEST_CHECK_EQ(aurora::storage::utest_dispatch_storage_version<Note>(), 1U);
    AURORA_TEST_CHECK_EQ(aurora::storage::utest_dispatch_storage_version<Plain>(), 1U);
}

AURORA_TEST_CASE(storage_type_name_stable_and_distinct) {
    // 默认类型标签：同类型返回同一静态缓存引用（零分配比较），异类型标签不同，取值为 typeid 短名。
    const auto& note_name = aus::storage_type_name(static_cast<const Note*>(nullptr));
    const auto& note_name_again = aus::storage_type_name(static_cast<const Note*>(nullptr));
    AURORA_TEST_CHECK(&note_name == &note_name_again);  // 同一 static 缓存
    AURORA_TEST_CHECK(!note_name.empty());
    AURORA_TEST_CHECK_EQ(note_name, std::string(typeid(Note).name()));

    const auto& chunk_name = aus::storage_type_name(static_cast<const Chunk*>(nullptr));
    AURORA_TEST_CHECK_NE(note_name, chunk_name);

    const auto& plain_name = aus::storage_type_name(static_cast<const Plain*>(nullptr));
    AURORA_TEST_CHECK_NE(note_name, plain_name);
    AURORA_TEST_CHECK_NE(chunk_name, plain_name);
}

AURORA_TEST_CASE(migrate_storage_default_is_identity) {
    // 默认迁移钩子对 JSON 与二进制两条线格式均恒等返回（不丢数据、不报错）。
    const aus::Json payload = aus::Json{{"k", 1}, {"s", "v"}};
    const auto migrated_json = aus::migrate_storage(1, static_cast<const Note*>(nullptr), payload);
    AURORA_TEST_REQUIRE(migrated_json.ok());
    AURORA_TEST_CHECK_EQ(migrated_json.value(), payload);

    const aus::StorageBytes bytes{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
    const auto migrated_bytes = aus::migrate_storage(1, static_cast<const Chunk*>(nullptr), bytes);
    AURORA_TEST_REQUIRE(migrated_bytes.ok());
    AURORA_TEST_CHECK(migrated_bytes.value() == bytes);
}

AURORA_TEST_CASE(json_line_roundtrip_and_parse_failure) {
    // JSON 线格式：to → from 往返还原；形状不符时返回结构化错误。
    const Note in{.title = "todo", .priority = 3};
    const aus::Json encoded = json_line::to_storage_json(in);
    AURORA_TEST_CHECK_EQ(encoded, aus::Json{{"title", "todo"}, {"priority", 3}});

    Note out;
    const auto ok = json_line::from_storage_json(out, encoded);
    AURORA_TEST_REQUIRE(ok.ok());
    AURORA_TEST_CHECK_EQ(out.title, std::string("todo"));
    AURORA_TEST_CHECK_EQ(out.priority, 3);

    Note bad;
    const auto failed = json_line::from_storage_json(bad, aus::Json{{"other", 1}});
    AURORA_TEST_CHECK(!failed.ok());
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, ErrorCode::StorageRecordCorrupt);
}

AURORA_TEST_CASE(binary_line_roundtrip_and_decode_failure) {
    // 二进制线格式：to → from 往返还原；空载荷返回结构化错误。
    const Chunk in{.tag = "ab", .size = 9};
    const aus::StorageBytes encoded = binary_line::to_storage_bytes(in);
    AURORA_TEST_CHECK_EQ(encoded.size(), std::size_t{3});

    Chunk out;
    const auto ok = binary_line::from_storage_bytes(out, encoded);
    AURORA_TEST_REQUIRE(ok.ok());
    AURORA_TEST_CHECK_EQ(out.tag, std::string("ab"));
    AURORA_TEST_CHECK_EQ(out.size, 9);

    Chunk bad;
    const auto failed = binary_line::from_storage_bytes(bad, aus::StorageBytes{});
    AURORA_TEST_CHECK(!failed.ok());
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, ErrorCode::StorageRecordCorrupt);
}

AURORA_TEST_CASE(storable_concept_requires_default_construction) {
    // StorageStorable = 可序列化（JSON 或二进制）且可默认构造；仅缺默认构造即被门面拒绝。
    static_assert(aus::StorageStorable<Note> && !aus::StorageStorable<NoDefaultCtor>);
    AURORA_TEST_CHECK(true);  // 全部判定已在编译期完成
}

}  // namespace aurora::test_cases::utest_serializable
