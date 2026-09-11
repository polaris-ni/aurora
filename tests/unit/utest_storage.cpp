/// 测试类型: unit
/// 目标单元: include/aurora/storage/storage.h
/// 测试说明: Storage 门面——双工厂（内存注入 / 文件系统 Result）、原始 JSON 与二进制双通道读写及编码不匹配、
///           信封级 API、类型化 put/get（含 __raw__ 透读与 TypeMismatch）、版本迁移钩子、事务提交/回滚与
///           批量事件、on_change 订阅与退订、async_* 异步往返、跨实例落盘持久化与打开失败、默认实例

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "aurora/storage/memory_backend.h"
#include "aurora/storage/storage.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_storage {

namespace aus = aurora::storage;
namespace m = aurora::testing::matchers;

// ============================================================================
// 测试用可存储类型（置于独立子命名空间，避免 ADL 互相误中定制点）。
// ============================================================================

namespace {
/// @brief JSON 线格式的玩家状态。
struct PlayerState {
    std::string name;
    int score = 0;
};

inline auto to_storage_json(const PlayerState& p) -> aus::Json {
    return aus::Json{{"name", p.name}, {"score", p.score}};
}

inline auto from_storage_json(PlayerState& p, const aus::Json& j) -> Result<void> {
    if (!j.contains("name") || !j.contains("score")) {
        return Result<void>{make_error(ErrorCode::StorageRecordCorrupt, "player fields missing")};
    }
    p.name = j.at("name").get<std::string>();
    p.score = j.at("score").get<int>();
    return Result<void>{};
}

/// @brief 二进制线格式的数据块（玩具编码：data 原样字节 + 末字节 size）。
struct Blob {
    std::vector<std::byte> data;
};

inline auto to_storage_bytes(const Blob& b) -> aus::StorageBytes {
    aus::StorageBytes out = b.data;
    out.push_back(static_cast<std::byte>(b.data.size()));
    return out;
}

inline auto from_storage_bytes(Blob& b, const aus::StorageBytes& bytes) -> Result<void> {
    if (bytes.empty()) {
        return Result<void>{make_error(ErrorCode::StorageRecordCorrupt, "blob payload empty")};
    }
    b.data.assign(bytes.begin(), bytes.end() - 1);
    return Result<void>{};
}

/// @brief 带版本迁移的文档类型（当前版本 2：level 为 v2 新增字段）。
struct MigratingDoc {
    std::string name;
    int level = 0;
};

inline auto to_storage_json(const MigratingDoc& d) -> aus::Json {
    return aus::Json{{"name", d.name}, {"level", d.level}};
}

inline auto from_storage_json(MigratingDoc& d, const aus::Json& j) -> Result<void> {
    if (!j.contains("name")) {
        return Result<void>{make_error(ErrorCode::StorageRecordCorrupt, "doc missing name")};
    }
    d.name = j.at("name").get<std::string>();
    d.level = j.value("level", 0);
    return Result<void>{};
}

/// @brief 用户命名空间覆盖版本号：带 tag 指针实参（ADL 定制点约定），使 MigratingDoc 当前版本为 2。
inline auto storage_version(const MigratingDoc* /*doc*/) -> std::uint32_t { return 2; }

/// @brief 迁移钩子：v1 记录缺 level 字段，迁移时补默认值 1。
inline auto migrate_storage(std::uint32_t old_version, const MigratingDoc* /*doc*/, aus::Json j) -> Result<aus::Json> {
    if (old_version < 2 && !j.contains("level")) {
        j["level"] = 1;
    }
    return Result<aus::Json>{std::move(j)};
}

/// @brief 用例临时目录（fs 门面用例专用）。
[[nodiscard]] auto make_case_dir(std::string_view tag) -> std::filesystem::path {
    return std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_storage" / std::filesystem::path{tag};
}

/// @brief 创建注入 MemoryBackend 的门面（绝大多数用例的底座）。
[[nodiscard]] auto make_mem_storage() -> aus::Storage {
    return aus::Storage::create(std::make_unique<aus::MemoryBackend>());
}

}  // namespace

AURORA_TEST_CASE(create_with_backend_json_roundtrip) {
    // 注入内存后端：原始 JSON 通道 put/get 往返，信封自动封装为 __raw__ / version 1 / Json 编码。
    auto s = make_mem_storage();
    AURORA_TEST_REQUIRE(s.put("cfg", aus::Json{{"theme", "dark"}, {"volume", 3}}));

    const auto got = s.get("cfg");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value(), aus::Json{{"theme", "dark"}, {"volume", 3}});

    const auto rec = s.get_record("cfg");
    AURORA_TEST_REQUIRE(rec.ok());
    AURORA_TEST_CHECK_EQ(rec.value().type, std::string("__raw__"));
    AURORA_TEST_CHECK_EQ(rec.value().version, 1U);
    AURORA_TEST_CHECK(rec.value().encoding == aus::StorageEncoding::Json);
}

AURORA_TEST_CASE(get_missing_json_returns_not_found) {
    // 读缺失键：透传后端 StorageRecordNotFound；contains 归一为 false；remove 幂等成功。
    auto s = make_mem_storage();

    const auto got = s.get("absent");
    AURORA_TEST_CHECK(!got.ok());
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageRecordNotFound);

    const auto contains = s.contains("absent");
    AURORA_TEST_REQUIRE(contains.ok());
    AURORA_TEST_CHECK(!contains.value());

    AURORA_TEST_CHECK(s.remove("absent").ok());
}

AURORA_TEST_CASE(bytes_channel_and_encoding_mismatch) {
    // 二进制通道往返；跨通道读取（JSON 记录走二进制通道、反之亦然）返回 StorageEncodingMismatch。
    auto s = make_mem_storage();

    const aus::StorageBytes raw{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    AURORA_TEST_REQUIRE(s.put("bin", raw));
    const auto bytes = s.get_bytes("bin");
    AURORA_TEST_REQUIRE(bytes.ok());
    AURORA_TEST_CHECK(bytes.value() == raw);

    const auto as_json = s.get("bin");
    AURORA_TEST_CHECK(!as_json.ok());
    AURORA_TEST_CHECK_EQ(as_json.error().code_enum, ErrorCode::StorageEncodingMismatch);

    AURORA_TEST_REQUIRE(s.put("js", aus::Json{{"a", 1}}));
    const auto as_bytes = s.get_bytes("js");
    AURORA_TEST_CHECK(!as_bytes.ok());
    AURORA_TEST_CHECK_EQ(as_bytes.error().code_enum, ErrorCode::StorageEncodingMismatch);
}

AURORA_TEST_CASE(get_value_returns_variant_payload) {
    // get_value 返回原始 variant：JSON 记录含 Json、二进制记录含 StorageBytes。
    auto s = make_mem_storage();
    AURORA_TEST_REQUIRE(s.put("j", aus::Json{{"a", 1}}));
    AURORA_TEST_REQUIRE(s.put("b", aus::StorageBytes{std::byte{0x01}}));

    const auto vj = s.get_value("j");
    AURORA_TEST_REQUIRE(vj.ok());
    AURORA_TEST_CHECK(std::holds_alternative<aus::Json>(vj.value()));

    const auto vb = s.get_value("b");
    AURORA_TEST_REQUIRE(vb.ok());
    AURORA_TEST_CHECK(std::holds_alternative<aus::StorageBytes>(vb.value()));
}

AURORA_TEST_CASE(typed_json_struct_roundtrip) {
    // 类型化 JSON 结构：put<T> 写入类型标签与版本，get<T> 往返还原。
    auto s = make_mem_storage();
    const PlayerState in{.name = "ada", .score = 99};
    AURORA_TEST_REQUIRE(s.put("player", in));

    const auto rec = s.get_record("player");
    AURORA_TEST_REQUIRE(rec.ok());
    AURORA_TEST_CHECK(rec.value().type == aus::storage_type_name(static_cast<const PlayerState*>(nullptr)));
    AURORA_TEST_CHECK_EQ(rec.value().version, aus::storage_version(static_cast<const PlayerState*>(nullptr)));

    const auto out = s.get<PlayerState>("player");
    AURORA_TEST_REQUIRE(out.ok());
    AURORA_TEST_CHECK_EQ(out.value().name, std::string("ada"));
    AURORA_TEST_CHECK_EQ(out.value().score, 99);
}

AURORA_TEST_CASE(typed_binary_struct_roundtrip) {
    // 类型化二进制结构：put<T> 走 Binary 编码（优先二进制定制点），get<T> 往返还原。
    auto s = make_mem_storage();
    const Blob in{.data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}};
    AURORA_TEST_REQUIRE(s.put("blob", in));

    const auto rec = s.get_record("blob");
    AURORA_TEST_REQUIRE(rec.ok());
    AURORA_TEST_CHECK(rec.value().encoding == aus::StorageEncoding::Binary);

    const auto out = s.get<Blob>("blob");
    AURORA_TEST_REQUIRE(out.ok());
    AURORA_TEST_CHECK(out.value().data == in.data);
}

AURORA_TEST_CASE(typed_read_type_mismatch_and_raw_allowed) {
    // 类型化读取契约：__raw__ 记录可透读；type 标签不符 → StorageTypeMismatch。
    auto s = make_mem_storage();

    AURORA_TEST_REQUIRE(s.put("raw", aus::Json{{"name", "r"}, {"score", 1}}));
    const auto via_raw = s.get<PlayerState>("raw");
    AURORA_TEST_REQUIRE(via_raw.ok());
    AURORA_TEST_CHECK_EQ(via_raw.value().name, std::string("r"));
    AURORA_TEST_CHECK_EQ(via_raw.value().score, 1);

    aus::StorageRecord foreign;
    foreign.id = "foreign";
    foreign.type = "definitely_other_type";
    foreign.version = 1;
    foreign.encoding = aus::StorageEncoding::Json;
    foreign.payload = aus::Json{{"name", "x"}, {"score", 2}};
    AURORA_TEST_REQUIRE(s.put_record("foreign", foreign));

    const auto mismatch = s.get<PlayerState>("foreign");
    AURORA_TEST_CHECK(!mismatch.ok());
    AURORA_TEST_CHECK_EQ(mismatch.error().code_enum, ErrorCode::StorageTypeMismatch);
}

AURORA_TEST_CASE(typed_migration_hook_upgrades_old_version) {
    // 迁移契约：put<T> 写当前版本 2；手写 version=1 旧信封后 get<T> 触发 ADL 迁移钩子补 level。
    auto s = make_mem_storage();

    AURORA_TEST_REQUIRE(s.put("doc", MigratingDoc{.name = "hero", .level = 5}));
    const auto fresh = s.get_record("doc");
    AURORA_TEST_REQUIRE(fresh.ok());
    AURORA_TEST_CHECK_EQ(fresh.value().version, 2U);  // 覆盖后的当前版本
    const auto fresh_out = s.get<MigratingDoc>("doc");
    AURORA_TEST_REQUIRE(fresh_out.ok());
    AURORA_TEST_CHECK_EQ(fresh_out.value().level, 5);

    aus::StorageRecord legacy;
    legacy.id = "legacy";
    legacy.type = aus::storage_type_name(static_cast<const MigratingDoc*>(nullptr));
    legacy.version = 1;  // 旧版本记录
    legacy.encoding = aus::StorageEncoding::Json;
    legacy.payload = aus::Json{{"name", "old"}};  // v1 格式：无 level 字段
    legacy.mtime = std::chrono::system_clock::now();
    AURORA_TEST_REQUIRE(s.put_record("legacy", legacy));

    const auto migrated = s.get<MigratingDoc>("legacy");
    AURORA_TEST_REQUIRE(migrated.ok());
    AURORA_TEST_CHECK_EQ(migrated.value().name, std::string("old"));
    AURORA_TEST_CHECK_EQ(migrated.value().level, 1);  // 迁移钩子补的字段
}

AURORA_TEST_CASE(envelope_record_roundtrip) {
    // 信封级 API：自定义 type/version/blob_ref 的信封原样存取（供元数据与迁移场景）。
    auto s = make_mem_storage();
    aus::StorageRecord rec;
    rec.id = "env";
    rec.type = "custom_type";
    rec.version = 9;
    rec.encoding = aus::StorageEncoding::Json;
    rec.payload = aus::Json{{"k", "v"}};
    rec.mtime = std::chrono::system_clock::now();
    rec.blob_ref = "env.bin";
    AURORA_TEST_REQUIRE(s.put_record("env", rec));

    const auto got = s.get_record("env");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value().id, std::string("env"));
    AURORA_TEST_CHECK_EQ(got.value().type, std::string("custom_type"));
    AURORA_TEST_CHECK_EQ(got.value().version, 9U);
    AURORA_TEST_CHECK_EQ(got.value().blob_ref, std::string("env.bin"));
    AURORA_TEST_CHECK_EQ(std::get<aus::Json>(got.value().payload), aus::Json{{"k", "v"}});
}

AURORA_TEST_CASE(crud_list_contains_clear_flush) {
    // 门面 CRUD 全链路：put/list/contains/remove/clear/flush 的可见性变化。
    auto s = make_mem_storage();
    AURORA_TEST_REQUIRE(s.put("k1", aus::Json{{"v", 1}}));
    AURORA_TEST_REQUIRE(s.put("k2", aus::Json{{"v", 2}}));
    AURORA_TEST_REQUIRE(s.put("k3", aus::Json{{"v", 3}}));

    auto ids = s.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::size_is(3));
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"k2"}));

    AURORA_TEST_REQUIRE(s.remove("k2"));
    const auto gone = s.contains("k2");
    AURORA_TEST_REQUIRE(gone.ok());
    AURORA_TEST_CHECK(!gone.value());
    ids = s.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::size_is(2));

    AURORA_TEST_REQUIRE(s.flush().ok());  // 内存后端 flush 为 no-op 成功
    AURORA_TEST_REQUIRE(s.clear());
    ids = s.list();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::is_empty());
}

AURORA_TEST_CASE(change_events_put_remove_clear_and_batch) {
    // 变更通知：逐操作事件 Put/Remove/Clear；事务内逐操作被抑制，成功后统一发一条 Batch。
    auto s = make_mem_storage();
    std::vector<aus::StorageChange> events;
    const auto sub = s.on_change([&events](const aus::StorageChange& ch) -> void { events.push_back(ch); });
    AURORA_TEST_REQUIRE(sub.active());

    AURORA_TEST_REQUIRE(s.put("a", aus::Json{{"v", 1}}));
    AURORA_TEST_REQUIRE(s.put("b", aus::Json{{"v", 2}}));
    AURORA_TEST_REQUIRE(s.remove("a"));

    const auto failed = s.transaction([](const aus::Storage& inner) -> Result<void> {
        (void)inner.put("t1", aus::Json{{"v", 9}});
        (void)inner.remove("b");
        return Result<void>{make_error(ErrorCode::GeneralUnknown, "abort")};
    });
    AURORA_TEST_CHECK(!failed.ok());
    AURORA_TEST_CHECK(events.size() == 3);  // 失败事务不补发任何事件

    // Memory 后端快照回滚：事务内写入被撤销、删除被恢复。
    const auto b_alive = s.contains("b");
    AURORA_TEST_REQUIRE(b_alive.ok());
    AURORA_TEST_CHECK(b_alive.value());
    const auto t1_gone = s.contains("t1");
    AURORA_TEST_REQUIRE(t1_gone.ok());
    AURORA_TEST_CHECK(!t1_gone.value());

    const auto committed =
        s.transaction([](const aus::Storage& inner) -> Result<void> { return inner.put("c", aus::Json{{"v", 3}}); });
    AURORA_TEST_REQUIRE(committed.ok());

    AURORA_TEST_REQUIRE(s.clear());

    AURORA_TEST_REQUIRE_EQ(events.size(), std::size_t{5});
    AURORA_TEST_CHECK(events[0].op == aus::StorageChange::Operation::Put);
    AURORA_TEST_CHECK_EQ(events[0].id, std::string("a"));
    AURORA_TEST_CHECK(events[1].op == aus::StorageChange::Operation::Put);
    AURORA_TEST_CHECK_EQ(events[1].id, std::string("b"));
    AURORA_TEST_CHECK(events[2].op == aus::StorageChange::Operation::Remove);
    AURORA_TEST_CHECK_EQ(events[2].id, std::string("a"));
    AURORA_TEST_CHECK(events[3].op == aus::StorageChange::Operation::Batch);  // 事务统一批量事件
    AURORA_TEST_CHECK(events[3].id.empty());
    AURORA_TEST_CHECK(events[4].op == aus::StorageChange::Operation::Clear);
}

AURORA_TEST_CASE(on_change_unsubscribe_stops_delivery) {
    // RAII 订阅：reset() 后不再投递，active() 反映订阅状态。
    auto s = make_mem_storage();
    int calls = 0;
    auto sub = s.on_change([&calls](const aus::StorageChange&) -> void { ++calls; });
    AURORA_TEST_REQUIRE(sub.active());

    AURORA_TEST_REQUIRE(s.put("u1", aus::Json{{"v", 1}}));
    AURORA_TEST_CHECK_EQ(calls, 1);

    sub.reset();
    AURORA_TEST_CHECK(!sub.active());
    AURORA_TEST_REQUIRE(s.put("u2", aus::Json{{"v", 2}}));
    AURORA_TEST_CHECK_EQ(calls, 1);  // 退订后不再计数
}

AURORA_TEST_CASE(async_roundtrip_put_get_value_remove_list) {
    AURORA_TEST_REQUIRE_THREADS();
    // 异步通道：async_put/async_get/async_get_value/async_remove/async_list 经线程池执行，
    // 回调（无主线程投递器时在 worker 直接调用）以 promise/future 同步等待。
    auto s = make_mem_storage();

    std::promise<Result<void>> put_p;
    auto put_f = put_p.get_future();
    s.async_put("cfg", aus::Json{{"n", 7}}).then([&put_p](const Result<void>& r) -> void { put_p.set_value(r); });
    AURORA_TEST_REQUIRE_EQ(put_f.wait_for(std::chrono::seconds{10}), std::future_status::ready);
    AURORA_TEST_CHECK(put_f.get().ok());

    std::promise<Result<aus::Json>> get_p;
    auto get_f = get_p.get_future();
    s.async_get("cfg").then([&get_p](const Result<aus::Json>& r) -> void { get_p.set_value(r); });
    AURORA_TEST_REQUIRE_EQ(get_f.wait_for(std::chrono::seconds{10}), std::future_status::ready);
    const auto got = get_f.get();
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value(), aus::Json{{"n", 7}});

    std::promise<bool> val_p;
    auto val_f = val_p.get_future();
    s.async_get_value("cfg").then([&val_p](const Result<aus::StorageValue>& r) -> void {
        val_p.set_value(r.ok() && std::holds_alternative<aus::Json>(r.value()));
    });
    AURORA_TEST_REQUIRE_EQ(val_f.wait_for(std::chrono::seconds{10}), std::future_status::ready);
    AURORA_TEST_CHECK(val_f.get());

    AURORA_TEST_REQUIRE(s.put("lone", aus::Json{{"v", 1}}));  // 供 async_list 核对
    std::promise<Result<std::vector<std::string>>> list_p;
    auto list_f = list_p.get_future();
    s.async_list().then([&list_p](const Result<std::vector<std::string>>& r) -> void { list_p.set_value(r); });
    AURORA_TEST_REQUIRE_EQ(list_f.wait_for(std::chrono::seconds{10}), std::future_status::ready);
    const auto ids = list_f.get();
    AURORA_TEST_REQUIRE(ids.ok());
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"cfg"}));
    AURORA_TEST_CHECK_THAT(ids.value(), m::contains(std::string{"lone"}));

    std::promise<Result<void>> rm_p;
    auto rm_f = rm_p.get_future();
    s.async_remove("cfg").then([&rm_p](const Result<void>& r) -> void { rm_p.set_value(r); });
    AURORA_TEST_REQUIRE_EQ(rm_f.wait_for(std::chrono::seconds{10}), std::future_status::ready);
    AURORA_TEST_CHECK(rm_f.get().ok());
    const auto gone = s.contains("cfg");
    AURORA_TEST_REQUIRE(gone.ok());
    AURORA_TEST_CHECK(!gone.value());
}

AURORA_TEST_CASE(fs_create_persists_across_instances_and_reports_open_failure) {
    // 文件系统工厂：成功返回 Result<Storage> 且数据落盘（新实例重开同目录可读）；目录不存在且
    // 不自动创建时返回 StorageBackendUnavailable。
    const auto dir = make_case_dir("facade_fs");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    auto opened = aus::Storage::create(aus::FilesystemOptions{.root = dir, .auto_create_dir = true});
    AURORA_TEST_REQUIRE(opened.ok());
    AURORA_TEST_REQUIRE(opened.value().put("fk", aus::Json{{"v", 11}}));

    auto reopened = aus::Storage::create(aus::FilesystemOptions{.root = dir, .auto_create_dir = true});
    AURORA_TEST_REQUIRE(reopened.ok());
    const auto got = reopened.value().get("fk");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value(), aus::Json{{"v", 11}});

    const auto refused =
        aus::Storage::create(aus::FilesystemOptions{.root = dir / "no" / "such", .auto_create_dir = false});
    AURORA_TEST_CHECK(!refused.ok());
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::StorageBackendUnavailable);

    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(fs_binary_sidecar_is_directory_reports_io_error) {
    // 守护点：POSIX 下 ifstream 打开目录会成功、读取时才报 EISDIR，而 libstdc++ 的
    // basic_filebuf::underflow 对此无条件抛 ios_base::failure（不看流的异常掩码）。
    // 后端必须把这类读失败降级为 Result 错误，绝不能让异常逃出 get_record()。
    // （Windows 上 ifstream 打不开目录，本用例恒绿；只有 Linux/WSL 才真正暴露该缺陷。）
    const auto dir = make_case_dir("fs_sidecar_dir");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    auto opened = aus::Storage::create(aus::FilesystemOptions{.root = dir, .auto_create_dir = true});
    AURORA_TEST_REQUIRE(opened.ok());
    auto& s = opened.value();
    AURORA_TEST_REQUIRE(s.put("blob", aus::StorageBytes{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}));

    // 定位刚落盘的 .bin sidecar（文件名是 id 的 base64url，不便反推，故按扩展名找）。
    std::filesystem::path sidecar;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".bin") {
            sidecar = entry.path();
            break;
        }
    }
    AURORA_TEST_REQUIRE(!sidecar.empty());  // 找不到说明二进制落盘布局变了，用例需同步

    // 换成同名目录：模拟 sidecar 被损坏或被同名目录占用。
    std::filesystem::remove(sidecar, ec);
    AURORA_TEST_REQUIRE(std::filesystem::create_directory(sidecar, ec));

    const auto got = s.get_bytes("blob");
    AURORA_TEST_CHECK(!got.ok());
    AURORA_TEST_CHECK_EQ(got.error().code_enum, ErrorCode::StorageIoError);

    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(default_instance_returns_set_instance) {
    // 进程级默认实例：set_default 后 default_instance 恒返回同一对象。
    // （set_default 先行，避免触发默认文件系统后端创建真实用户目录。）
    auto mem = make_mem_storage();
    AURORA_TEST_REQUIRE(mem.put("dk", aus::Json{{"d", 1}}));
    aus::Storage::set_default(std::move(mem));

    auto& first = aus::Storage::default_instance();
    auto& second = aus::Storage::default_instance();
    AURORA_TEST_CHECK(&first == &second);
    const auto contains = first.contains("dk");
    AURORA_TEST_REQUIRE(contains.ok());
    AURORA_TEST_CHECK(contains.value());
}

}  // namespace aurora::test_cases::utest_storage
