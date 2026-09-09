/// 测试类型: unit
/// 目标单元: include/aurora/storage/fs_backend.h
/// 测试说明: FilesystemBackend 打开语义（显式 root / 自动建目录 / 缺目录不建 / 跨进程锁）、JSON 与二进制
///           信封落盘往返（mtime 毫秒精度、sidecar blob_ref）、覆盖写与 sidecar 清理、删除幂等、list/get、
///           坏文件（JSON 损坏、sidecar 缺失）错误码

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

#include "aurora/storage/fs_backend.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_fs_backend {

namespace aus = aurora::storage;
namespace m = aurora::testing::matchers;

/// @brief 本用例的临时目录：框架每用例接管 TMP/TEMP/TMPDIR，temp_directory_path() 已是
/// 用例唯一目录，再挂固定子目录并先行清场，保证幂等与跨用例/跨进程并行安全。
[[nodiscard]] auto make_case_dir(std::string_view tag) -> std::filesystem::path {
    return std::filesystem::temp_directory_path() / "aurora_utest_storage_fs" / std::filesystem::path{tag};
}

/// @brief 清场后重建空目录（幂等），并保证用例结束清理。
[[nodiscard]] auto fresh_dir(std::string_view tag) -> std::filesystem::path {
    const auto dir = make_case_dir(tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

/// @brief 构造一条 JSON 载荷的记录信封。
[[nodiscard]] auto make_json_record(std::string id, aus::Json payload) -> aus::StorageRecord {
    aus::StorageRecord rec;
    rec.id = std::move(id);
    rec.type = "__raw__";
    rec.version = 1;
    rec.encoding = aus::StorageEncoding::Json;
    rec.payload = std::move(payload);
    rec.mtime = std::chrono::system_clock::now();
    return rec;
}

/// @brief 构造一条二进制载荷的记录信封。
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

/// @brief mtime 的毫秒精度表示（落盘即毫秒，比较亦按毫秒）。
[[nodiscard]] auto epoch_ms(const std::chrono::system_clock::time_point& tp) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

/// @brief 列出目录下以指定后缀结尾的常规文件（用于定位信封/sidecar 而不依赖内部编码细节）。
[[nodiscard]] auto files_with_suffix(const std::filesystem::path& dir, std::string_view suffix)
    -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().filename().string().ends_with(suffix)) {
            out.push_back(entry.path());
        }
    }
    return out;
}

AURORA_TEST_CASE(open_explicit_root_reports_open) {
    // 已存在的目录 + 默认选项：后端打开成功且可写。
    const auto dir = fresh_dir("open_ok");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_CHECK(be.is_open());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(auto_create_dir_creates_missing_root) {
    // auto_create_dir=true：多层缺失目录在构造时自动创建并打开成功。
    const auto dir = make_case_dir("auto_create") / "l1" / "l2";
    std::error_code ec;
    std::filesystem::remove_all(make_case_dir("auto_create"), ec);

    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir, .auto_create_dir = true}};
    AURORA_TEST_CHECK(be.is_open());
    AURORA_TEST_CHECK(std::filesystem::is_directory(dir));
    std::filesystem::remove_all(make_case_dir("auto_create"), ec);
}

AURORA_TEST_CASE(closed_backend_when_root_missing_without_autocreate) {
    // auto_create_dir=false 且目录不存在：打开失败，全部操作返回 StorageBackendUnavailable。
    const auto dir = make_case_dir("closed") / "never_created";
    std::error_code ec;
    std::filesystem::remove_all(make_case_dir("closed"), ec);

    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir, .auto_create_dir = false}};
    AURORA_TEST_CHECK(!be.is_open());

    const auto put = be.put_record("x", make_json_record("x", aus::Json{{"v", 1}}));
    AURORA_TEST_CHECK(!put.ok());
    AURORA_TEST_CHECK_EQ(put.error().code_enum, ErrorCode::StorageBackendUnavailable);

    const auto got = be.get_record("x");
    AURORA_TEST_CHECK(!got.ok());
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageBackendUnavailable);

    const auto ids = be.list();
    AURORA_TEST_CHECK(!ids.ok());
    AURORA_TEST_CHECK_EQ(ids.error().code_enum, ErrorCode::StorageBackendUnavailable);

    const auto removed = be.remove("x");
    AURORA_TEST_CHECK(!removed.ok());
    AURORA_TEST_CHECK_EQ(removed.error().code_enum, ErrorCode::StorageBackendUnavailable);
    std::filesystem::remove_all(make_case_dir("closed"), ec);
}

AURORA_TEST_CASE(json_record_roundtrip_preserves_envelope) {
    // JSON 信封落盘往返：id/type/version/encoding/mtime（毫秒精度）/payload 逐字段保真，blob_ref 为空。
    const auto dir = fresh_dir("json_roundtrip");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_REQUIRE(be.is_open());

    auto rec = make_json_record("user/profile", aus::Json{{"name", "ada"}, {"score", 42}});
    const auto mtime_ms = epoch_ms(rec.mtime);
    AURORA_TEST_REQUIRE(be.put_record("user/profile", rec));

    const auto got = be.get_record("user/profile");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value().id, std::string("user/profile"));
    AURORA_TEST_CHECK_EQ(got.value().type, std::string("__raw__"));
    AURORA_TEST_CHECK_EQ(got.value().version, 1U);
    AURORA_TEST_CHECK(got.value().encoding == aus::StorageEncoding::Json);
    AURORA_TEST_CHECK_EQ(epoch_ms(got.value().mtime), mtime_ms);
    AURORA_TEST_CHECK(std::get<aus::Json>(got.value().payload) == std::get<aus::Json>(rec.payload));
    AURORA_TEST_CHECK(got.value().blob_ref.empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(binary_record_roundtrip_via_sidecar) {
    // 二进制信封：载荷写入 sidecar 文件，信封携带 blob_ref，读回字节逐一相等。
    const auto dir = fresh_dir("bin_roundtrip");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_REQUIRE(be.is_open());

    const aus::StorageBytes payload{std::byte{0x00}, std::byte{0x7F}, std::byte{0x80}, std::byte{0xFF}};
    auto rec = make_binary_record("blob1", payload);
    AURORA_TEST_REQUIRE(be.put_record("blob1", rec));

    const auto got = be.get_record("blob1");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK(got.value().encoding == aus::StorageEncoding::Binary);
    AURORA_TEST_CHECK(!got.value().blob_ref.empty());
    AURORA_TEST_CHECK(got.value().blob_ref.ends_with(".bin"));
    AURORA_TEST_CHECK(std::get<aus::StorageBytes>(got.value().payload) == payload);
    AURORA_TEST_CHECK(std::filesystem::exists(dir / got.value().blob_ref));  // sidecar 确在 blob_ref 指向处
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(overwrite_switches_encoding_and_cleans_sidecar) {
    // 覆盖写契约：JSON→Binary 生成 sidecar；Binary→JSON 重写时清理残留 sidecar（磁盘不泄漏）。
    const auto dir = fresh_dir("overwrite");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_REQUIRE(be.is_open());

    AURORA_TEST_REQUIRE(be.put_record("swap", make_json_record("swap", aus::Json{{"v", 1}})));
    AURORA_TEST_CHECK(files_with_suffix(dir, ".bin").empty());  // JSON 记录无 sidecar

    AURORA_TEST_REQUIRE(be.put_record("swap", make_binary_record("swap", aus::StorageBytes{std::byte{0x01}})));
    AURORA_TEST_CHECK_EQ(files_with_suffix(dir, ".bin").size(), std::size_t{1});
    const auto as_binary = be.get_record("swap");
    AURORA_TEST_REQUIRE(as_binary.ok());
    AURORA_TEST_CHECK(as_binary.value().encoding == aus::StorageEncoding::Binary);

    AURORA_TEST_REQUIRE(be.put_record("swap", make_json_record("swap", aus::Json{{"v", 2}})));
    AURORA_TEST_CHECK(files_with_suffix(dir, ".bin").empty());  // 改回 JSON 后 sidecar 被清理
    const auto as_json = be.get_record("swap");
    AURORA_TEST_REQUIRE(as_json.ok());
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(as_json.value().payload), aus::Json{{"v", 2}});
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(remove_record_and_sidecar_is_idempotent) {
    // remove：删除 JSON 信封与二进制 sidecar；重复删除与删除不存在的键均幂等成功。
    const auto dir = fresh_dir("remove");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_REQUIRE(be.is_open());

    AURORA_TEST_REQUIRE(be.put_record("gone", make_json_record("gone", aus::Json{{"v", 1}})));
    AURORA_TEST_REQUIRE(be.put_record("gblob", make_binary_record("gblob", aus::StorageBytes{std::byte{0x09}})));
    AURORA_TEST_REQUIRE(be.put_record("kept", make_json_record("kept", aus::Json{{"v", 2}})));

    AURORA_TEST_REQUIRE(be.remove("gone"));
    AURORA_TEST_CHECK_EQ(be.get_record("gone").error().code_enum, ErrorCode::StorageRecordNotFound);
    AURORA_TEST_REQUIRE(be.remove("gblob"));
    AURORA_TEST_CHECK_EQ(be.get_record("gblob").error().code_enum, ErrorCode::StorageRecordNotFound);
    AURORA_TEST_CHECK(files_with_suffix(dir, ".bin").empty());  // sidecar 随记录删除

    AURORA_TEST_REQUIRE(be.remove("gone"));  // 重复删除幂等
    AURORA_TEST_REQUIRE(be.remove("never-was"));  // 从未存在的键幂等

    const auto ids = be.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::size_is(1));
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"kept"}));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(list_semantics_empty_and_populated) {
    // list：空目录返回空表；写入后返回全部 id（含路径不友好字符的 id）。
    const auto dir = fresh_dir("list");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_REQUIRE(be.is_open());

    const auto empty = be.list();
    AURORA_TEST_REQUIRE(empty.ok());
    AURORA_TEST_CHECK_THAT(empty.value(), m::is_empty());

    AURORA_TEST_REQUIRE(be.put_record("a/b", make_json_record("a/b", aus::Json{{"v", 1}})));
    AURORA_TEST_REQUIRE(be.put_record("c d", make_json_record("c d", aus::Json{{"v", 2}})));
    const auto ids = be.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::size_is(2));
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"a/b"}));
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"c d"}));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(get_missing_record_not_found) {
    // 读不存在的记录：StorageRecordNotFound，而非 IO 错误。
    const auto dir = fresh_dir("missing");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_REQUIRE(be.is_open());

    const auto got = be.get_record("absent");
    AURORA_TEST_CHECK(!got.ok());
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageRecordNotFound);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(corrupt_record_file_reports_corrupt) {
    // 错误路径：信封 JSON 被外部改坏 → StorageRecordCorrupt（可解析性防御）。
    const auto dir = fresh_dir("corrupt");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_REQUIRE(be.is_open());
    AURORA_TEST_REQUIRE(be.put_record("crash", make_json_record("crash", aus::Json{{"v", 1}})));

    auto jsons = files_with_suffix(dir, ".json");
    AURORA_TEST_REQUIRE_THAT(jsons, m::size_is(1));
    {
        std::ofstream out{jsons[0], std::ios::binary};  // 模拟外部写坏
        out << "{definitely not valid json";
    }

    const auto got = be.get_record("crash");
    AURORA_TEST_CHECK(!got.ok());
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageRecordCorrupt);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(missing_binary_sidecar_reports_corrupt) {
    // 错误路径：信封声明 Binary 但 sidecar 缺失 → StorageRecordCorrupt（不静默返回空载荷）。
    const auto dir = fresh_dir("sidecar_lost");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir}};
    AURORA_TEST_REQUIRE(be.is_open());
    AURORA_TEST_REQUIRE(be.put_record("lost", make_binary_record("lost", aus::StorageBytes{std::byte{0x42}})));

    const auto bins = files_with_suffix(dir, ".bin");
    AURORA_TEST_REQUIRE_THAT(bins, m::size_is(1));
    std::error_code ec;
    std::filesystem::remove(bins[0], ec);

    const auto got = be.get_record("lost");
    AURORA_TEST_CHECK(!got.ok());
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageRecordCorrupt);
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(cross_process_lock_option_opens) {
    // cross_process_lock=true：advisory 锁获取成功、后端打开、锁文件落在 root 下。
    const auto dir = fresh_dir("lock");
    aus::FilesystemBackend be{aus::FilesystemOptions{.root = dir, .auto_create_dir = true, .cross_process_lock = true}};
    AURORA_TEST_REQUIRE(be.is_open());
    AURORA_TEST_CHECK(std::filesystem::exists(dir / "aurora_storage.lock"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace aurora::test_cases::utest_fs_backend
