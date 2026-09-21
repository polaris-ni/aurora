// ============================================================================
// sqlite_backend.cpp — SQLite 后端实现（opt-in：AURORA_ENABLE_STORAGE_SQLITE）
// ----------------------------------------------------------------------------
// 表 `aurora_records` 一记录一行：信封元数据分列（id 主键 / type / version / encoding /
// mtime），载荷按编码存 TEXT（JSON dump）或 BLOB（二进制内联，无 sidecar）。schema 版本经
// `PRAGMA user_version` 前滚。所有语句参数绑定（无字符串拼接 SQL）。见 ARCHITECTURE.md §4.8。
// ============================================================================

#include "aurora/storage/sqlite_backend.h"

#ifdef AURORA_ENABLE_STORAGE_SQLITE

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/preferences/preferences.h"

namespace aurora::storage {

namespace {

constexpr int AURORA_SCHEMA_VERSION = 1;

[[nodiscard]] auto mtime_to_ms(const std::chrono::system_clock::time_point &tp) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] auto ms_to_mtime(std::int64_t ms) -> std::chrono::system_clock::time_point {
    if (ms <= 0) {
        return {};  // 缺失/未知 → epoch
    }
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

/// @brief sqlite 返回码 → Result 错误（非 SQLITE_OK / SQLITE_DONE 均视为 IO 错）。
template <typename T>
[[nodiscard]] auto sqlite_err(std::string_view op, int rc, sqlite3 *db) -> Result<T> {
    const char *msg = db ? sqlite3_errmsg(db) : nullptr;
    return Result<T>{make_error(
        ErrorCode::StorageIoError,
        std::string(op) + " failed: sqlite rc=" + std::to_string(rc) + (msg ? std::string(" (") + msg + ")" : ""))};
}

void exec_simple(sqlite3 *db, const char *sql, int *rc_out) {
    char *err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) {
        sqlite3_free(err);
    }
    if (rc_out) {
        *rc_out = rc;
    }
}

}  // namespace

struct SqliteBackend::Impl {
    sqlite3 *db = nullptr;
    std::recursive_mutex mu;  ///< 串行化逻辑操作与事务边界（体经门面回调重入同一后端）
    int tx_depth = 0;
    bool wal = false;
};

SqliteBackend::SqliteBackend(SqliteOptions opts) : impl_(std::make_unique<Impl>()) {
    std::filesystem::path path;
    if (!opts.in_memory) {
        path = opts.path.empty() ? aurora::preferences::Preferences::default_config_dir() / "aurora_storage.db"
                                 : opts.path;
        std::error_code ec;
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);  // 已存在/交由 open 报错，均不在此中断
        }
    }

    // C++20 u8string → char*：Windows 宽字符路径经 UTF-8 触库，避免 ANSI 代码页丢字。
    const std::u8string open_target = opts.in_memory ? std::u8string{u8":memory:"} : path.u8string();
    const int rc = sqlite3_open(reinterpret_cast<const char *>(open_target.c_str()), &impl_->db);
    if (rc != SQLITE_OK) {
        if (impl_->db) {
            sqlite3_close_v2(impl_->db);
            impl_->db = nullptr;
        }
        return;  // open_ 保持 false
    }

    int prc = SQLITE_OK;
    exec_simple(impl_->db, "PRAGMA busy_timeout=5000;", &prc);
    if (!opts.in_memory && opts.wal) {
        exec_simple(impl_->db, "PRAGMA journal_mode=WAL;", &prc);
        impl_->wal = prc == SQLITE_OK;
    }
    exec_simple(impl_->db,
                "CREATE TABLE IF NOT EXISTS aurora_records("
                "  id            TEXT PRIMARY KEY,"
                "  type          TEXT NOT NULL DEFAULT '',"
                "  version       INTEGER NOT NULL DEFAULT 1,"
                "  encoding      INTEGER NOT NULL DEFAULT 0,"
                "  mtime_ms      INTEGER NOT NULL DEFAULT 0,"
                "  payload_json  TEXT,"
                "  payload_blob  BLOB,"
                "  blob_ref      TEXT NOT NULL DEFAULT ''"
                ");",
                &prc);
    if (prc != SQLITE_OK) {
        sqlite3_close_v2(impl_->db);
        impl_->db = nullptr;
        return;
    }
    exec_simple(impl_->db, "PRAGMA user_version=1;", &prc);
    open_ = true;
}

SqliteBackend::~SqliteBackend() { (void)close(); }

auto SqliteBackend::close() -> Result<void> {
    std::lock_guard lock(impl_->mu);
    if (impl_->db) {
        sqlite3_close_v2(impl_->db);
        impl_->db = nullptr;
    }
    return Result<void>{};
}

auto SqliteBackend::put_record(const std::string &id, const StorageRecord &rec) -> Result<void> {
    std::lock_guard lock(impl_->mu);
    if (!open_) {
        return Result<void>{make_error(ErrorCode::StorageBackendUnavailable, "SQLite backend not opened: " + id)};
    }
    const std::string json_text = rec.encoding == StorageEncoding::Json ? std::get<Json>(rec.payload).dump() : "";
    static const std::vector<std::byte> AURORA_EMPTY_BYTES;
    const std::vector<std::byte> &bytes =
        rec.encoding == StorageEncoding::Binary ? std::get<StorageBytes>(rec.payload) : AURORA_EMPTY_BYTES;

    sqlite3_stmt *stmt = nullptr;
    if (const int rc = sqlite3_prepare_v2(impl_->db,
                                          "INSERT OR REPLACE INTO aurora_records"
                                          "(id,type,version,encoding,mtime_ms,payload_json,payload_blob,blob_ref)"
                                          " VALUES(?1,?2,?3,?4,?5, CASE WHEN ?4=0 THEN ?6 ELSE NULL END,"
                                          " CASE WHEN ?4=1 THEN ?7 ELSE NULL END, ?8);",
                                          -1, &stmt, nullptr);
        rc != SQLITE_OK) {
        return sqlite_err<void>("prepare put", rc, impl_->db);
    }
    sqlite3_bind_text(stmt, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, rec.type.data(), static_cast<int>(rec.type.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(rec.version));
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(rec.encoding == StorageEncoding::Binary ? 1 : 0));
    sqlite3_bind_int64(stmt, 5, mtime_to_ms(rec.mtime));
    sqlite3_bind_text(stmt, 6, json_text.data(), static_cast<int>(json_text.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 7, bytes.empty() ? nullptr : reinterpret_cast<const void *>(bytes.data()),
                      static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, rec.blob_ref.data(), static_cast<int>(rec.blob_ref.size()), SQLITE_TRANSIENT);

    const int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        return sqlite_err<void>("put_record", step, impl_->db);
    }
    return Result<void>{};
}

auto SqliteBackend::get_record(const std::string &id) -> Result<StorageRecord> {
    std::lock_guard lock(impl_->mu);
    if (!open_) {
        return Result<StorageRecord>{
            make_error(ErrorCode::StorageBackendUnavailable, "SQLite backend not opened: " + id)};
    }
    sqlite3_stmt *stmt = nullptr;
    if (const int rc = sqlite3_prepare_v2(impl_->db,
                                          "SELECT id,type,version,encoding,mtime_ms,payload_json,payload_blob,"
                                          " blob_ref FROM aurora_records WHERE id=?1;",
                                          -1, &stmt, nullptr);
        rc != SQLITE_OK) {
        return sqlite_err<StorageRecord>("prepare get", rc, impl_->db);
    }
    sqlite3_bind_text(stmt, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);

    const int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return Result<StorageRecord>{make_error(ErrorCode::StorageRecordNotFound, "Record does not exist: " + id)};
    }
    if (step != SQLITE_ROW) {
        auto err = sqlite_err<StorageRecord>("get_record", step, impl_->db);
        sqlite3_finalize(stmt);
        return err;
    }

    StorageRecord rec;
    auto text_at = [stmt](int col) -> std::string {
        const auto *p = sqlite3_column_text(stmt, col);
        return p ? std::string(reinterpret_cast<const char *>(p),
                               static_cast<std::size_t>(sqlite3_column_bytes(stmt, col)))
                 : std::string{};
    };
    rec.id = text_at(0);
    rec.type = text_at(1);
    rec.version = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 2));
    rec.encoding = sqlite3_column_int64(stmt, 3) == 1 ? StorageEncoding::Binary : StorageEncoding::Json;
    rec.mtime = ms_to_mtime(sqlite3_column_int64(stmt, 4));
    rec.blob_ref = text_at(7);

    Result<StorageRecord> out = Result<StorageRecord>{std::move(rec)};
    if (out.value().encoding == StorageEncoding::Binary) {
        const void *blob = sqlite3_column_blob(stmt, 6);
        const int n = sqlite3_column_bytes(stmt, 6);
        StorageBytes bytes(static_cast<std::size_t>(n));
        if (blob && n > 0) {
            const auto *src = static_cast<const std::uint8_t *>(blob);
            for (int i = 0; i < n; ++i) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                bytes[static_cast<std::size_t>(i)] = static_cast<std::byte>(src[i]);
            }
        }
        out.value().payload = std::move(bytes);
    } else {
        const std::string json_text = text_at(5);
        try {
            out.value().payload = Json::parse(json_text);
        } catch (...) {
            sqlite3_finalize(stmt);
            return Result<StorageRecord>{
                make_error(ErrorCode::StorageRecordCorrupt, "Record JSON parse failed: " + id)};
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

auto SqliteBackend::remove(const std::string &id) -> Result<void> {
    std::lock_guard lock(impl_->mu);
    if (!open_) {
        return Result<void>{make_error(ErrorCode::StorageBackendUnavailable, "SQLite backend not opened: " + id)};
    }
    sqlite3_stmt *stmt = nullptr;
    if (const int rc = sqlite3_prepare_v2(impl_->db, "DELETE FROM aurora_records WHERE id=?1;", -1, &stmt, nullptr);
        rc != SQLITE_OK) {
        return sqlite_err<void>("prepare remove", rc, impl_->db);
    }
    sqlite3_bind_text(stmt, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
    const int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        return sqlite_err<void>("remove", step, impl_->db);
    }
    return Result<void>{};  // 不存在亦成功（幂等）
}

auto SqliteBackend::list() -> Result<std::vector<std::string>> {
    std::lock_guard lock(impl_->mu);
    if (!open_) {
        return Result<std::vector<std::string>>{
            make_error(ErrorCode::StorageBackendUnavailable, "SQLite backend not opened")};
    }
    sqlite3_stmt *stmt = nullptr;
    if (const int rc = sqlite3_prepare_v2(impl_->db, "SELECT id FROM aurora_records;", -1, &stmt, nullptr);
        rc != SQLITE_OK) {
        return sqlite_err<std::vector<std::string>>("prepare list", rc, impl_->db);
    }
    std::vector<std::string> ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto *p = sqlite3_column_text(stmt, 0);
        if (p) {
            ids.emplace_back(reinterpret_cast<const char *>(p),
                             static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
        }
    }
    sqlite3_finalize(stmt);
    return Result<std::vector<std::string>>{std::move(ids)};
}

auto SqliteBackend::contains(const std::string &id) -> Result<bool> {
    std::lock_guard lock(impl_->mu);
    if (!open_) {
        return Result<bool>{make_error(ErrorCode::StorageBackendUnavailable, "SQLite backend not opened: " + id)};
    }
    sqlite3_stmt *stmt = nullptr;
    if (const int rc =
            sqlite3_prepare_v2(impl_->db, "SELECT 1 FROM aurora_records WHERE id=?1 LIMIT 1;", -1, &stmt, nullptr);
        rc != SQLITE_OK) {
        return sqlite_err<bool>("prepare contains", rc, impl_->db);
    }
    sqlite3_bind_text(stmt, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return Result<bool>{found};
}

auto SqliteBackend::clear() -> Result<void> {
    std::lock_guard lock(impl_->mu);
    if (!open_) {
        return Result<void>{make_error(ErrorCode::StorageBackendUnavailable, "SQLite backend not opened")};
    }
    int rc = SQLITE_OK;
    exec_simple(impl_->db, "DELETE FROM aurora_records;", &rc);
    if (rc != SQLITE_OK) {
        return sqlite_err<void>("clear", rc, impl_->db);
    }
    return Result<void>{};
}

auto SqliteBackend::transaction(const std::function<Result<void>(StorageBackend &)> &body) -> Result<void> {
    std::lock_guard lock(impl_->mu);
    if (!open_) {
        return Result<void>{make_error(ErrorCode::StorageBackendUnavailable, "SQLite backend not opened")};
    }
    int rc = SQLITE_OK;
    if (impl_->tx_depth == 0) {
        exec_simple(impl_->db, "BEGIN IMMEDIATE;", &rc);
        if (rc != SQLITE_OK) {
            return sqlite_err<void>("BEGIN", rc, impl_->db);
        }
    }
    ++impl_->tx_depth;

    auto r = body(*this);

    if (--impl_->tx_depth == 0) {
        if (r.ok()) {
            exec_simple(impl_->db, "COMMIT;", &rc);
            if (rc != SQLITE_OK) {
                exec_simple(impl_->db, "ROLLBACK;", nullptr);
                return sqlite_err<void>("COMMIT", rc, impl_->db);
            }
            return Result<void>{};
        }
        exec_simple(impl_->db, "ROLLBACK;", nullptr);
    }
    return r;
}

auto SqliteBackend::flush() -> Result<void> {
    std::lock_guard lock(impl_->mu);
    if (!open_ || !impl_->wal) {
        return Result<void>{};
    }
    int rc = SQLITE_OK;
    exec_simple(impl_->db, "PRAGMA wal_checkpoint(TRUNCATE);", &rc);
    if (rc != SQLITE_OK) {
        return sqlite_err<void>("wal_checkpoint", rc, impl_->db);
    }
    return Result<void>{};
}

}  // namespace aurora::storage

#endif  // AURORA_ENABLE_STORAGE_SQLITE
