#pragma once

// ============================================================================
// storage.h — 存储门面（Facade，用户唯一直接持有的句柄）
// ----------------------------------------------------------------------------
// 对标 Application 持有 Surface：门面负责信封封装、类型化、异步卸载与变更通知，
// 后端经 `create` 注入。API 形态：命名记录仓储（put/get/remove/list）+ 信封级 +
// 二进制 + 异步 + 事务 + 类型化 + 变更通知。见 ARCHITECTURE.md §4.8。
// ============================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/state/async.h"
#include "aurora/state/subscription.h"
#include "aurora/storage/backend.h"
#include "aurora/storage/serializable.h"
#include "aurora/storage/storage_types.h"

namespace aurora::storage {

class Storage {
  public:
    /// @brief 默认文件系统后端（零额外依赖，始终可用）。打开失败返回错误（StorageBackendUnavailable）。
    [[nodiscard]] static auto create(FilesystemOptions opts = {}) -> Result<Storage>;

    /// @brief 注入任意后端（自定义 / SQLite / 测试 Memory）—— 对标 Application(Scene, unique_ptr<Surface>)。
    [[nodiscard]] static auto create(std::unique_ptr<StorageBackend> backend) -> Storage;

    // ---------- 原始 JSON 记录 API（用户视角 id → Json value；内部自动信封化） ----------
    [[nodiscard]] auto put(const std::string &id, const Json &value) const -> Result<void>;
    [[nodiscard]] auto get(const std::string &id) const -> Result<Json>; ///< 返回 JSON payload（裸 value）
    [[nodiscard]] auto remove(const std::string &id) const -> Result<void>;
    [[nodiscard]] auto list() const -> Result<std::vector<std::string>>;
    [[nodiscard]] auto contains(const std::string &id) const -> Result<bool>;
    [[nodiscard]] auto clear() const -> Result<void>;
    auto flush() const -> Result<void>;

    // ---------- 二进制载荷 API（variant 放宽，对标 Room BLOB / Realm data / Hive 二进制） ----------
    [[nodiscard]] auto put(const std::string &id, const StorageBytes &value) const -> Result<void>;
    [[nodiscard]] auto get_bytes(const std::string &id) const -> Result<StorageBytes>; ///< 仅取二进制载荷
    [[nodiscard]] auto get_value(const std::string &id) const
        -> Result<StorageValue>; ///< 返回原始 variant（Json 或 bytes）

    // ---------- 信封级 API（含元数据/迁移时使用） ----------
    [[nodiscard]] auto put_record(const std::string &id, const StorageRecord &rec) const -> Result<void>;
    [[nodiscard]] auto get_record(const std::string &id) const -> Result<StorageRecord>;

    // ---------- 异步 API（门面 async_* 重载，内部经 au::async 卸载到 worker） ----------
    // 注意：au::async 会把 Result<T> 解包为 Task<T>，故回调收到 Result<T>（非 Task<Result<T>>）。
    [[nodiscard]] auto async_put(const std::string &id, const Json &value) const -> aurora::Task<void>;
    [[nodiscard]] auto async_get(const std::string &id) const -> aurora::Task<Json>;
    [[nodiscard]] auto async_put(const std::string &id, const StorageBytes &value) const -> aurora::Task<void>;
    [[nodiscard]] auto async_get_value(const std::string &id) const -> aurora::Task<StorageValue>;
    [[nodiscard]] auto async_remove(const std::string &id) const -> aurora::Task<void>;
    [[nodiscard]] auto async_list() const -> aurora::Task<std::vector<std::string>>;

    // ---------- 跨记录事务（后端契约，见 specification/06-app-platform.md §9.2） ----------
    [[nodiscard]] auto transaction(std::function<Result<void>(Storage &)> body) -> Result<void>;

    // ---------- 类型化便捷层（核心抽象接入点） ----------
    template<StorageStorable T> [[nodiscard]] auto put(const std::string &id, const T &obj) -> Result<void> {
        StorageRecord rec;
        rec.id = id;
        rec.type = storage_type_name<T>();
        rec.version = storage_version<T>();
        rec.mtime = std::chrono::system_clock::now();
        if constexpr (StorageBinarySerializable<T>) {
            rec.encoding = StorageEncoding::Binary;
            rec.payload = to_storage_bytes(obj);
        } else {
            rec.encoding = StorageEncoding::Json;
            rec.payload = to_storage_json(obj);
        }
        return put_record(id, rec);
    }

    template<StorageStorable T> [[nodiscard]] auto get(const std::string &id) -> Result<T> { // NOLINT
        auto rec = m_backend->get_record(id);
        if (!rec) {
            return Result<T>{ rec.error() };
        }
        if (rec.value().type != storage_type_name<T>() && rec.value().type != "__raw__") {
            return Result<T>{ make_error(ErrorCode::StorageTypeMismatch, "Typed read type mismatch") };
        }
        T out{};
        if (rec.value().encoding == StorageEncoding::Binary) {
            if constexpr (StorageBinarySerializable<T>) {
                auto bytes = std::get<StorageBytes>(rec.value().payload);
                if (rec.value().version < storage_version<T>()) {
                    auto migrated = migrate_storage<T>(rec.value().version, std::move(bytes));
                    if (!migrated) {
                        return Result<T>{ migrated.error() };
                    }
                    bytes = std::move(migrated.value());
                }
                auto r = from_storage_bytes(out, bytes);
                if (!r) {
                    return Result<T>{ r.error() };
                }
            } else {
                return Result<T>{ make_error(ErrorCode::StorageEncodingMismatch,
                                             "Type T only supports JSON serialization") };
            }
        } else {
            if constexpr (StorageSerializable<T>) {
                auto j = std::get<Json>(rec.value().payload);
                if (rec.value().version < storage_version<T>()) {
                    auto migrated = migrate_storage<T>(rec.value().version, std::move(j));
                    if (!migrated) {
                        return Result<T>{ migrated.error() };
                    }
                    j = std::move(migrated.value());
                }
                auto r = from_storage_json(out, j);
                if (!r) {
                    return Result<T>{ r.error() };
                }
            } else {
                return Result<T>{ make_error(ErrorCode::StorageEncodingMismatch,
                                             "Type T only supports binary serialization") };
            }
        }
        return Result<T>{ std::move(out) };
    }

    template<StorageStorable T>
    [[nodiscard]] auto async_put(const std::string &id, const T &obj) -> aurora::Task<void> {
        StorageRecord rec;
        rec.id = id;
        rec.type = storage_type_name<T>();
        rec.version = storage_version<T>();
        rec.mtime = std::chrono::system_clock::now();
        if constexpr (StorageBinarySerializable<T>) {
            rec.encoding = StorageEncoding::Binary;
            rec.payload = to_storage_bytes(obj);
        } else {
            rec.encoding = StorageEncoding::Json;
            rec.payload = to_storage_json(obj);
        }
        auto *be = m_backend.get();
        const std::string &idc = id;
        auto task = aurora::async([be, idc, rec]() -> Result<void> { return be->put_record(idc, rec); });
        task.then([this, idc](const Result<void> &r) -> auto {
            if (r.ok()) {
                emit_change({ .op = StorageChange::Operation::Put, .id = idc });
            }
        });
        return task;
    }

    // ---------- 响应式变更通知（v1；始终在主线程发射，便于 UI 订阅） ----------
    [[nodiscard]] auto on_change(StorageChangeCallback cb) -> aurora::Subscription;

    // ---------- 可选进程级默认实例（对标 preferences::instance） ----------
    static auto set_default(Storage s) -> void;
    [[nodiscard]] static auto default_instance() -> Storage &;

  private:
    explicit Storage(std::unique_ptr<StorageBackend> b) : m_backend(std::move(b)) {}

    void emit_change(const StorageChange &ch) const;

    std::unique_ptr<StorageBackend> m_backend;

    /// 事务内抑制逐操作通知，提交后统一发 Batch。经 m_listener_mutex 保护：异步 API
    /// 的回调可能在 worker 线程发射变更，与主线程事务并发访问此标志（atomic 会使
    /// Storage 失去移动性、破坏 Result<Storage>，故用锁）。
    bool m_notify_suppressed{ false };

    // 变更监听器注册表（RAII via aurora::Subscription）
    struct Listener {
        std::uint64_t id;
        StorageChangeCallback cb;
    };
    std::vector<Listener> m_listeners;
    std::uint64_t m_listener_seq = 1;
    std::unique_ptr<std::mutex> m_listener_mutex = std::make_unique<std::mutex>();
};

} // namespace aurora::storage
