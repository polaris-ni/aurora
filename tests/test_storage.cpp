// test_storage.cpp — 存储抽象层契约测试（Memory + Filesystem 后端、类型化、二进制、
// 异步、变更通知、事务回滚）。接入 CTest（tests/*.cpp 经 GLOB 自动收集）。
// ── API 覆盖映射 ─────────────────────────────
// storage/storage_backend.h(StorageBackend 抽象契约)、storage/fs_backend.h(FilesystemBackend)、
// storage/memory_backend.h(MemoryBackend)、storage/serializable.h(概念与钩子)、storage/storage_types.h(值模型)。

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/storage/memory_backend.h"
#include "test_harness.h"

using aurora::ErrorCode;
using aurora::Json;
using aurora::Result;
using aurora::storage::FilesystemOptions;
using aurora::storage::MemoryBackend;
using aurora::storage::Storage;
using aurora::storage::storage_type_name;
using aurora::storage::StorageBytes;
using aurora::storage::StorageChange;

using std::chrono_literals::operator""ms;

// ---------------------------------------------------------------------------
// 测试用可序列化类型（ADL 自由函数，对标 serialization 的 to_json 风格）
// ---------------------------------------------------------------------------
namespace {

struct WidgetState {
    int id_ = 0;
    std::string name_;
    double value_ = 0.0;
};

[[maybe_unused]] auto to_storage_json(const WidgetState &w) -> Json {
    Json j = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["id"] = w.id_;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["name"] = w.name_;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["value"] = w.value_;
    return j;
}
[[maybe_unused]] auto from_storage_json(WidgetState &out, const Json &j) -> Result<void> {
    if (!j.is_object()) {
        return Result<void>{make_error(ErrorCode::StorageRecordCorrupt, "WidgetState 期望对象")};
    }
    out.id_ = j.value("id", 0);
    out.name_ = j.value("name", "");
    out.value_ = j.value("value", 0.0);
    return Result<void>{};
}

// 另一种类型，用于验证「类型名不匹配」错误（storage_type_name 默认取 typeid，二者不同）。
struct OtherState {
    int x_ = 0;
};
[[maybe_unused]] auto to_storage_json(const OtherState &o) -> Json {
    Json j = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["x"] = o.x_;
    return j;
}
[[maybe_unused]] auto from_storage_json(OtherState &out, const Json &j) -> Result<void> {
    out.x_ = j.value("x", 0);
    return Result<void>{};
}

// 二进制可序列化类型（绕过 JSON）。
struct BlobState {
    std::vector<std::byte> data_;
};
[[maybe_unused]] auto to_storage_bytes(const BlobState &b) -> StorageBytes { return b.data_; }
[[maybe_unused]] auto from_storage_bytes(BlobState &out, const StorageBytes &b) -> Result<void> {
    out.data_ = b;
    return Result<void>{};
}

void wait_until(const std::atomic<bool> &flag, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!flag.load() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(5ms);
    }
}

}  // namespace

AURORA_TEST() {
    // 1) Memory 后端基础契约：put/get/remove/list/contains。
    {
        auto mem = Storage::create(std::make_unique<MemoryBackend>());
        AURORA_TEST_CHECK(mem.put("a", Json(1)).ok());
        AURORA_TEST_CHECK(mem.put("b", Json("two")).ok());

        auto a = mem.get("a");
        AURORA_TEST_CHECK(a.ok() && a.value() == 1);
        auto missing = mem.get("nope");
        AURORA_TEST_CHECK(!missing.ok() && missing.error().code == "storage-record-not-found");

        auto has = mem.contains("b");
        AURORA_TEST_CHECK(has.ok() && has.value());
        auto has_no = mem.contains("nope");
        AURORA_TEST_CHECK(has_no.ok() && !has_no.value());

        auto ids = mem.list();
        AURORA_TEST_CHECK(ids.ok() && ids.value().size() == 2);

        AURORA_TEST_CHECK(mem.remove("a").ok());
        auto after_remove = mem.contains("a");
        AURORA_TEST_CHECK(after_remove.ok() && !after_remove.value());
    }

    // 2) 类型化 JSON 往返：put<T>/get<T>。
    {
        auto mem = Storage::create(std::make_unique<MemoryBackend>());
        WidgetState in;
        in.id_ = 5;
        in.name_ = "foo";
        in.value_ = 3.5;
        AURORA_TEST_CHECK(mem.put("w", in).ok());

        auto out = mem.get<WidgetState>("w");
        AURORA_TEST_CHECK(out.ok());
        if (out.ok()) {
            AURORA_TEST_CHECK(out.value().id_ == 5);
            AURORA_TEST_CHECK(out.value().name_ == "foo");
            AURORA_TEST_CHECK(out.value().value_ == 3.5);
        }

        // 类型名不匹配 → StorageTypeMismatch。
        auto wrong = mem.get<OtherState>("w");
        AURORA_TEST_CHECK(!wrong.ok() && wrong.error().code == "storage-type-mismatch");

        // 优化 A：storage_type_name 为函数内 static 缓存——同 T 跨调用返回同一对象（零重建/零分配）。
        AURORA_TEST_CHECK(&storage_type_name<WidgetState>() == &storage_type_name<WidgetState>());
    }

    // 3) 二进制载荷通道：put(id,bytes)/get_bytes + 类型化二进制。
    {
        auto mem = Storage::create(std::make_unique<MemoryBackend>());
        StorageBytes bytes = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{0xFF}};
        AURORA_TEST_CHECK(mem.put("bin", bytes).ok());
        auto got = mem.get_bytes("bin");
        AURORA_TEST_CHECK(got.ok() && got.value() == bytes);

        // 以 JSON 通道读二进制记录 → StorageEncodingMismatch。
        auto as_json = mem.get("bin");
        AURORA_TEST_CHECK(!as_json.ok() && as_json.error().code == "storage-encoding-mismatch");

        // 类型化二进制。
        BlobState in;
        in.data_ = {std::byte{'a'}, std::byte{'b'}};
        AURORA_TEST_CHECK(mem.put("blob", in).ok());
        auto out = mem.get<BlobState>("blob");
        AURORA_TEST_CHECK(out.ok() && out.value().data_ == in.data_);
    }

    // 4) 变更通知：on_change 收到 Put/Remove/Clear；退订后停止。
    {
        auto mem = Storage::create(std::make_unique<MemoryBackend>());
        std::vector<StorageChange> events;
        auto sub = mem.on_change([&](const StorageChange &ch) -> void { events.push_back(ch); });

        (void)mem.put("x", Json(42));
        (void)mem.put("y", Json(7));
        (void)mem.remove("x");
        (void)mem.clear();

        AURORA_TEST_CHECK(events.size() == 4);
        if (events.size() == 4) {
            AURORA_TEST_CHECK(events.at(0).op == StorageChange::Operation::Put && events.at(0).id == "x");
            AURORA_TEST_CHECK(events.at(1).op == StorageChange::Operation::Put && events.at(1).id == "y");
            AURORA_TEST_CHECK(events.at(2).op == StorageChange::Operation::Remove && events.at(2).id == "x");
            AURORA_TEST_CHECK(events.at(3).op == StorageChange::Operation::Clear && events.at(3).id.empty());
        }

        sub.reset();
        events.clear();
        (void)mem.put("z", Json(1));
        AURORA_TEST_CHECK(events.empty());
    }

    // 5) 事务：失败回滚（Memory 精确快照回滚）。
    {
        auto mem = Storage::create(std::make_unique<MemoryBackend>());
        AURORA_TEST_CHECK(mem.put("a", Json(1)).ok());
        AURORA_TEST_CHECK(mem.put("b", Json(2)).ok());

        auto r = mem.transaction([&](Storage const &st) -> Result<void> {
            (void)st.put("c", Json(3));
            return Result<void>{make_error(ErrorCode::GeneralUnknown, "boom")};
        });
        AURORA_TEST_CHECK(!r.ok());

        auto has_c = mem.contains("c");
        AURORA_TEST_CHECK(has_c.ok() && !has_c.value());  // 回滚：c 不存在
        auto a = mem.get("a");
        AURORA_TEST_CHECK(a.ok() && a.value() == 1);  // a/b 不受影响

        // 成功事务提交。
        auto ok = mem.transaction([&](Storage const &st) -> Result<void> {
            (void)st.put("d", Json(4));
            return Result<void>{};
        });
        AURORA_TEST_CHECK(ok.ok());
        auto has_d = mem.contains("d");
        AURORA_TEST_CHECK(has_d.ok() && has_d.value());
    }

    // 6) 异步 API：async_put/async_get 经线程池，回调在主线程。
    {
        auto mem = Storage::create(std::make_unique<MemoryBackend>());

        std::atomic put_done{false};
        mem.async_put("k", Json(99)).then([&](const Result<void> &) -> void { put_done = true; });
        wait_until(put_done, 2000ms);
        AURORA_TEST_CHECK(put_done.load());

        std::atomic get_done{false};
        Result<Json> got{0};
        mem.async_get("k").then([&](const Result<Json> &r) -> void {
            got = r;
            get_done = true;
        });
        wait_until(get_done, 2000ms);
        AURORA_TEST_CHECK(get_done.load() && got.ok() && got.value() == 99);

        std::atomic list_done{false};
        Result lst{std::vector<std::string>{}};
        mem.async_list().then([&](const Result<std::vector<std::string>> &r) -> void {
            lst = r;
            list_done = true;
        });
        wait_until(list_done, 2000ms);
        AURORA_TEST_CHECK(list_done.load() && lst.ok() && lst.value().size() == 1);
    }

    // 7) 文件系统后端：契约 + 重开持久化（默认后端路径）；退出清理。
    {
        auto dir =
            std::filesystem::temp_directory_path() /
            ("aurora_storage_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);

        auto st = Storage::create(FilesystemOptions{.root = dir, .auto_create_dir = true, .cross_process_lock = false});
        AURORA_TEST_CHECK(st.ok());
        auto &s = st.value();
        AURORA_TEST_CHECK(s.put("fs1", Json{{"x", 1}}).ok());
        AURORA_TEST_CHECK(s.put("fs2", Json("hello")).ok());

        auto v = s.get("fs1");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(v.ok() && v.value()["x"] == 1);
        auto ids = s.list();
        AURORA_TEST_CHECK(ids.ok() && ids.value().size() == 2);

        // 重开同一目录 → 数据持久化。
        auto st2 = Storage::create(FilesystemOptions{.root = dir});
        AURORA_TEST_CHECK(st2.ok());
        auto &s2 = st2.value();
        auto v2 = s2.get("fs1");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(v2.ok() && v2.value()["x"] == 1);

        // 二进制在文件系统后端走 sidecar，往返无损。
        StorageBytes bytes = {std::byte{10}, std::byte{20}, std::byte{30}};
        AURORA_TEST_CHECK(s2.put("fb", bytes).ok());
        auto fb = s2.get_bytes("fb");
        AURORA_TEST_CHECK(fb.ok() && fb.value() == bytes);

        // 优化 B 去 optional 后语义不变：mtime 经文件系统往返仍非 epoch；二进制记录带 sidecar 引用。
        auto rec1 = s2.get_record("fs1");
        AURORA_TEST_CHECK(rec1.ok() && rec1.value().mtime != std::chrono::system_clock::time_point{});
        auto rec_bin = s2.get_record("fb");
        AURORA_TEST_CHECK(rec_bin.ok() && !rec_bin.value().blob_ref.empty());

        // 回归：记录由 Binary 改写为 Json 时，旧 <id>.bin sidecar 必须被清理
        // （此前残留为磁盘泄漏 + 已删载荷继续躺在盘上）。目录中另有 fb 记录的
        // 合法 sidecar，故用计数差断言而非存在性。
        auto count_bins = [&dir]() -> int {
            int n = 0;
            for (const auto &p : std::filesystem::directory_iterator(dir)) {
                if (p.path().extension() == ".bin") {
                    ++n;
                }
            }
            return n;
        };
        AURORA_TEST_CHECK(s2.put("mix", bytes).ok());
        const int bins_before = count_bins();
        AURORA_TEST_CHECK(bins_before >= 1);  // 二进制写入后存在 sidecar（fb + mix）
        AURORA_TEST_CHECK(s2.put("mix", Json{{"now", "json"}}).ok());
        AURORA_TEST_CHECK_EQ(count_bins(), bins_before - 1);  // mix 改写为 Json 后其 sidecar 被清理，fb 保留
        auto mixv = s2.get("mix");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(mixv.ok() && mixv.value()["now"] == "json");

        std::filesystem::remove_all(dir, ec);
    }

    // 8) 默认实例可用（文件系统或退化为内存，永不 fatal）。
    {
        auto &def = Storage::default_instance();
        AURORA_TEST_CHECK(def.put("default-key", Json(true)).ok());
        auto v = def.get("default-key");
        AURORA_TEST_CHECK(v.ok() && v.value() == true);
    }

    // 9) 事务异常安全：body 抛异常后通知抑制标志必须复位（后续变更仍可通知）。
    {
        auto s = Storage::create(std::make_unique<MemoryBackend>());
        std::atomic notified{false};
        auto sub = s.on_change([&](const StorageChange &) -> void { notified.store(true); });
        bool caught = false;
        try {
            (void)s.transaction([](Storage &) -> Result<void> { throw std::runtime_error("boom"); });
        } catch (...) {
            caught = true;
        }
        AURORA_TEST_CHECK(caught);
        AURORA_TEST_CHECK(s.put("after", Json(1)).ok());
        AURORA_TEST_CHECK(notified.load());  // RAII 守卫复位抑制标志，通知正常发出
    }
}