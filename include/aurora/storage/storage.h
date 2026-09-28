#pragma once

/// @brief 存储门面（Facade，用户唯一直接持有的句柄）：命名记录仓储 + 信封 + 二进制 + 异步 + 事务 + 变更通知。
/// @file storage.h
///
/// 对标 Application 持有 Surface：门面负责信封封装、类型化、异步卸载与变更通知，
/// 后端经 `create` 注入。见 ARCHITECTURE.md §4.8。

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
#include "aurora/storage/serializable.h"
#include "aurora/storage/storage_backend.h"
#include "aurora/storage/storage_types.h"

namespace aurora::storage {

/// @brief 存储门面：用户视角的 `id → value` 仓储，内部转换为 `StorageRecord` 信封下发后端。
///
/// 值模型三形态：裸 Json（put/get）、原生二进制（put(bytes)/get_bytes）、类型化（put\<T\>/get\<T\>，
/// 经 serializable.h 的 ADL 定制点）。可拷贝（shared_ptr 语义），可入 Result<Storage>。
class Storage {
  public:
    /// @brief 默认文件系统后端（零额外依赖，始终可用）。打开失败返回错误（StorageBackendUnavailable）。
    /// @param opts 根目录与锁策略；root 为空回退默认配置目录。
    /// @return 成功携带就绪门面；打开失败携带错误。
    [[nodiscard]] static auto create(FilesystemOptions opts = {}) -> Result<Storage>;

#ifdef AURORA_ENABLE_STORAGE_SQLITE
    /// @brief SQLite 后端（需 `AURORA_ENABLE_STORAGE_SQLITE`，默认 OFF）。打开失败返回
    ///        StorageBackendUnavailable；真事务语义见 `SqliteBackend::transaction`。
    /// @param opts 库路径与 WAL/内存模式选择。
    /// @return 成功携带就绪门面；打开失败携带错误。
    [[nodiscard]] static auto create(SqliteOptions opts) -> Result<Storage>;
#endif

    /// @brief 注入任意后端（自定义 / SQLite / 测试 Memory）—— 对标 Application(Scene, unique_ptr<Surface>)。
    /// @param backend 后端实例（所有权转入，永不为空由调用方保证）。
    /// @return 就绪门面（无失败面：构造不触库）。
    [[nodiscard]] static auto create(std::unique_ptr<StorageBackend> backend) -> Storage;

    /// @brief 原始 JSON 记录 API（用户视角 id → `json::Value`；内部自动信封化）。
    /// @param id 记录主键。
    /// @param value JSON 载荷（以 `__raw__` 无类型信封落盘）。
    /// @return 成功返回空值；后端 IO/落盘失败返回错误。
    [[nodiscard]] auto put(const std::string &id, const json::Value &value) const -> Result<void>;

    /// @brief 读取 JSON 载荷（裸 value；类型化记录亦可读时返回其 payload）。
    /// @param id 记录主键。
    /// @return 命中携带 `json::Value`；不存在返回 StorageRecordNotFound；载荷非 JSON 返回编码错误。
    [[nodiscard]] auto get(const std::string &id) const -> Result<json::Value>;

    /// @brief 删除记录（含二进制 sidecar）；不存在视为成功（幂等）。
    /// @param id 记录主键。
    /// @return 成功（含不存在）返回空值；IO 失败返回错误。
    [[nodiscard]] auto remove(const std::string &id) const -> Result<void>;

    /// @brief 列出全部记录 id。
    /// @return 恒成功（后端 IO 错误时返回错误）；顺序不保证。
    [[nodiscard]] auto list() const -> Result<std::vector<std::string>>;

    /// @brief 是否存在某 id。
    /// @param id 记录主键。
    /// @return 存在为 true；不存在为 false；后端错误原样返回。
    [[nodiscard]] auto contains(const std::string &id) const -> Result<bool>;

    /// @brief 清空全部记录（经后端事务语义）。
    /// @return 成功返回空值；中途失败返回错误。
    [[nodiscard]] auto clear() const -> Result<void>;

    /// @brief 主动落盘（缓冲型后端有效；其余 no-op 成功）。
    /// @return 成功返回空值；刷盘 IO 失败返回错误。
    [[nodiscard]] auto flush() const -> Result<void>;

    /// @brief 二进制载荷 API（variant 放宽，对标 Room BLOB / Realm data / Hive 二进制）。
    /// @param id 记录主键。
    /// @param value 原始字节载荷（落盘走 sidecar/BLOB，零 base64 膨胀）。
    /// @return 成功返回空值；IO 失败返回错误。
    [[nodiscard]] auto put(const std::string &id, const StorageBytes &value) const -> Result<void>;

    /// @brief 仅取二进制载荷。
    /// @param id 记录主键。
    /// @return 命中携带字节；不存在或载荷为 JSON 时返回编码错误。
    [[nodiscard]] auto get_bytes(const std::string &id) const -> Result<StorageBytes>;

    /// @brief 返回原始 variant 载荷（JSON 或 bytes，不做类型校验）。
    /// @param id 记录主键。
    /// @return 命中携带 StorageValue；不存在返回 StorageRecordNotFound。
    [[nodiscard]] auto get_value(const std::string &id) const -> Result<StorageValue>;

    /// @brief 信封级 API（含元数据/迁移时使用）：直接写入完整信封。
    /// @param id 记录主键。
    /// @param rec 完整信封（type/version/encoding 由调用方自定）。
    /// @return 成功返回空值；IO 失败返回错误。
    [[nodiscard]] auto put_record(const std::string &id, const StorageRecord &rec) const -> Result<void>;

    /// @brief 读取完整信封（含 type/version/mtime/blob_ref 元数据）。
    /// @param id 记录主键。
    /// @return 命中携带信封；不存在返回 StorageRecordNotFound。
    [[nodiscard]] auto get_record(const std::string &id) const -> Result<StorageRecord>;

    /// @brief 异步 API（门面 `async_*` 重载，内部经 `au::async` 卸载到 worker）。
    /// 注意：`au::async` 会把 `Result<T>` 解包为 `Task<T>`，故回调收到 `Result<T>`（非 `Task<Result<T>>`）。
    /// @param id 记录主键。
    /// @param value JSON 载荷。
    /// @return Task<void>：落盘结果经 then 回调的 Result 传达，成功后发射 Put 通知。
    [[nodiscard]] auto async_put(const std::string &id, const json::Value &value) const -> aurora::Task<void>;

    /// @brief 异步读取 JSON 载荷。
    /// @param id 记录主键。
    /// @return Task<json::Value>：worker 上执行 get，错误同样经 Result 解包语义传达。
    [[nodiscard]] auto async_get(const std::string &id) const -> aurora::Task<json::Value>;

    /// @brief 异步写入二进制载荷。
    /// @param id 记录主键。
    /// @param value 原始字节载荷。
    /// @return Task<void>：成功后发射 Put 通知。
    [[nodiscard]] auto async_put(const std::string &id, const StorageBytes &value) const -> aurora::Task<void>;

    /// @brief 异步读取原始 variant 载荷。
    /// @param id 记录主键。
    /// @return Task<StorageValue>：Json 或 bytes 原样返回。
    [[nodiscard]] auto async_get_value(const std::string &id) const -> aurora::Task<StorageValue>;

    /// @brief 异步删除记录（幂等）。
    /// @param id 记录主键。
    /// @return Task<void>：成功后发射 Remove 通知。
    [[nodiscard]] auto async_remove(const std::string &id) const -> aurora::Task<void>;

    /// @brief 异步列出全部 id。
    /// @return Task<vector<string>>：顺序不保证。
    [[nodiscard]] auto async_list() const -> aurora::Task<std::vector<std::string>>;

    /// @brief 跨记录事务（后端契约，见 specification/06-app-platform.md §9.2）。
    /// 事务体内抑制逐操作通知，提交后统一发 Batch。
    /// @param body 事务体：以门面为参数执行一串操作，返回首个错误或成功。
    /// @return body 的结果（后端支持时失败即回滚）。
    [[nodiscard]] auto transaction(std::function<Result<void>(Storage &)> body) -> Result<void>;

    // ---------- 类型化便捷层（核心抽象接入点） ----------
    /// @brief 类型化写入：按 serializable.h 定制点封装 type/version/encoding 信封后走 put_record。
    /// @tparam T 满足 StorageStorable（JSON 或二进制可序列化 + 可默认构造）的类型。
    /// @param id 记录主键。
    /// @param obj 待持久化对象。
    /// @return put_record 的结果。
    template <StorageStorable T>
    [[nodiscard]] auto put(const std::string &id, const T &obj) -> Result<void> {
        StorageRecord rec;
        rec.id = id;
        rec.type = storage_type_name(static_cast<const T *>(nullptr));
        rec.version = storage_version(static_cast<const T *>(nullptr));
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

    /// @brief 类型化读取：校验 type 标签与 encoding，必要时先经 migrate_storage 升版再反序列化。
    /// @tparam T 满足 StorageStorable 的目标类型。
    /// @param id 记录主键。
    /// @return 命中携带 T；不存在返回 StorageRecordNotFound；type 不符返回 StorageTypeMismatch；
    ///         记录 encoding 与 T 支持的序列化线不符返回 StorageEncodingMismatch。
    template <StorageStorable T>
    [[nodiscard]] auto get(const std::string &id) -> Result<T> {  // NOLINT
        auto rec = backend_->get_record(id);
        if (!rec) {
            return Result<T>{rec.error()};
        }
        if (rec.value().type != storage_type_name(static_cast<const T *>(nullptr)) && rec.value().type != "__raw__") {
            return Result<T>{make_error(ErrorCode::StorageTypeMismatch, "Typed read type mismatch")};
        }
        T out{};
        if (rec.value().encoding == StorageEncoding::Binary) {
            if constexpr (StorageBinarySerializable<T>) {
                auto bytes = std::get<StorageBytes>(rec.value().payload);
                if (rec.value().version < storage_version(static_cast<const T *>(nullptr))) {
                    auto migrated =
                        migrate_storage(rec.value().version, static_cast<const T *>(nullptr), std::move(bytes));
                    if (!migrated) {
                        return Result<T>{migrated.error()};
                    }
                    bytes = std::move(migrated.value());
                }
                auto r = from_storage_bytes(out, bytes);
                if (!r) {
                    return Result<T>{r.error()};
                }
            } else {
                return Result<T>{
                    make_error(ErrorCode::StorageEncodingMismatch, "Type T only supports JSON serialization")};
            }
        } else {
            if constexpr (StorageSerializable<T>) {
                auto j = std::get<json::Value>(rec.value().payload);
                if (rec.value().version < storage_version(static_cast<const T *>(nullptr))) {
                    auto migrated = migrate_storage(rec.value().version, static_cast<const T *>(nullptr), std::move(j));
                    if (!migrated) {
                        return Result<T>{migrated.error()};
                    }
                    j = std::move(migrated.value());
                }
                auto r = from_storage_json(out, j);
                if (!r) {
                    return Result<T>{r.error()};
                }
            } else {
                return Result<T>{
                    make_error(ErrorCode::StorageEncodingMismatch, "Type T only supports binary serialization")};
            }
        }
        return Result<T>{std::move(out)};
    }

    /// @brief 类型化异步写入：信封封装同 put<T>，落盘卸载到 worker，成功后发射 Put 通知。
    /// @tparam T 满足 StorageStorable 的类型。
    /// @param id 记录主键。
    /// @param obj 待持久化对象。
    /// @return Task<void>：落盘结果经 then 回调的 Result 传达。
    template <StorageStorable T>
    [[nodiscard]] auto async_put(const std::string &id, const T &obj) -> aurora::Task<void> {
        StorageRecord rec;
        rec.id = id;
        rec.type = storage_type_name(static_cast<const T *>(nullptr));
        rec.version = storage_version(static_cast<const T *>(nullptr));
        rec.mtime = std::chrono::system_clock::now();
        if constexpr (StorageBinarySerializable<T>) {
            rec.encoding = StorageEncoding::Binary;
            rec.payload = to_storage_bytes(obj);
        } else {
            rec.encoding = StorageEncoding::Json;
            rec.payload = to_storage_json(obj);
        }
        auto *be = backend_.get();
        const std::string &idc = id;
        auto task = aurora::async([be, idc, rec]() -> Result<void> { return be->put_record(idc, rec); });
        task.then([this, idc](const Result<void> &r) -> auto {
            if (r.ok()) {
                emit_change({.op = StorageChange::Operation::Put, .id = idc});
            }
        });
        return task;
    }

    // ---------- 响应式变更通知（v1；未设主线程投递器时可能在 worker 线程发射——`async_*` 的回调在 worker 线程经
    // `emit_change` 直接派发，见下方 `listener_mutex_`；设投递器后转主线程） ----------
    /// @brief 订阅变更通知：Put/Remove/Clear/Batch 后发射。
    /// @param cb 回调签名 `void(const StorageChange&)`。
    /// @return RAII 订阅句柄（Subscription），销毁即自动退订。
    [[nodiscard]] auto on_change(StorageChangeCallback cb) -> aurora::Subscription;

    // ---------- 可选进程级默认实例（对标 preferences::instance） ----------
    /// @brief 设置进程级默认实例（通常在 main 初始化时调用一次）。
    /// @param s 就绪门面（接管所有权）。
    static auto set_default(Storage s) -> void;

    /// @brief 进程级默认实例；未设置时回退为 MemoryBackend 支撑的懒单例。
    /// @return 门面引用（生命周期 = 进程）。
    [[nodiscard]] static auto default_instance() -> Storage &;

  private:
    /// @brief 门面私有构造：仅 create* 使用（后端所有权转入）。
    /// @param b 后端实例。
    explicit Storage(std::unique_ptr<StorageBackend> b) : backend_(std::move(b)) {}

    /// @brief 向全部监听器派发变更事件（listener_mutex_ 下快照后锁外回调）。
    /// @param ch 变更事件。
    void emit_change(const StorageChange &ch) const;

    std::unique_ptr<StorageBackend> backend_;  ///< 注入的持久化后端（所有权在门面）

    /// @brief 事务内抑制逐操作通知，提交后统一发 Batch。经 listener_mutex_ 保护：异步 API
    /// 的回调可能在 worker 线程发射变更，与主线程事务并发访问此标志（atomic 会使
    /// Storage 失去移动性、破坏 Result<Storage>，故用锁）。
    bool notify_suppressed_{false};

    // 变更监听器注册表（RAII via aurora::Subscription）
    struct Listener {
        std::uint64_t id;  ///< 注册序号（退订时据此摘除）
        StorageChangeCallback cb;  ///< 回调本体
    };
    std::vector<Listener> listeners_;  ///< 已注册监听器（id 升序无保证，按注册先后遍历）
    std::uint64_t listener_seq_ = 1;  ///< 下一个监听器 id（单调递增）
    /// @brief 监听器注册表互斥锁（注册/退订/派发共用）。
    std::unique_ptr<std::mutex> listener_mutex_ = std::make_unique<std::mutex>();
};

}  // namespace aurora::storage
