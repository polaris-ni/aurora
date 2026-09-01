#pragma once

// ============================================================================
// backend.h — 存储后端抽象接口（对标 Surface）
// ----------------------------------------------------------------------------
// 所有后端（Memory / Filesystem / 未来的 Sqlite）实现此接口。纯虚方法为最小契约，
// 其余给默认实现（经 `Storage` 门面转发），减少各后端样板。后端只认 `StorageRecord`
// 信封，不关心值语义。见 codespec/architecture/ARCHITECTURE_RUNTIME.md §4.11。
// ============================================================================

#include <functional>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/storage/storage_types.h"

namespace aurora::storage {

/// @brief 持久化后端抽象（对标 Surface）。每条记录 = `StorageRecord` 信封。
class StorageBackend {
  public:
    StorageBackend() = default;
    virtual ~StorageBackend() = default;

    StorageBackend(const StorageBackend &) = delete;
    auto operator=(const StorageBackend &) -> StorageBackend & = delete;
    StorageBackend(StorageBackend &&) = delete;
    auto operator=(StorageBackend &&) -> StorageBackend & = delete;

    /// @brief 写入/覆写一条记录信封（id 不存在则创建，存在则整体替换）。
    [[nodiscard]] virtual auto put_record(const std::string &id, const StorageRecord &rec) -> Result<void> = 0;

    /// @brief 读取一条记录信封；不存在返回 ErrorCode::StorageRecordNotFound。
    [[nodiscard]] virtual auto get_record(const std::string &id) -> Result<StorageRecord> = 0;

    /// @brief 删除一条记录；不存在视为成功（幂等）。
    [[nodiscard]] virtual auto remove(const std::string &id) -> Result<void> = 0;

    /// @brief 列出全部记录 id（顺序不保证）。
    [[nodiscard]] virtual auto list() -> Result<std::vector<std::string>> = 0;

    /// @brief 跨记录事务/批量原子。默认实现：顺序执行 body（尽力而为，无原子回滚）。
    ///        支持真事务的后端（SqliteBackend）应覆写为 BEGIN/COMMIT/ROLLBACK；
    ///        MemoryBackend 覆写为快照回滚。不覆写事务的后端（如 FilesystemBackend）
    ///        仅获得顺序执行语义——单条失败不会自动撤销已完成的写，属已知限制。
    [[nodiscard]] virtual auto transaction(const std::function<Result<void>(StorageBackend &)> &body) -> Result<void> {
        return body(*this);
    }

    /// @brief 是否存在某 id；默认经 get_record 实现。
    [[nodiscard]] virtual auto contains(const std::string &id) -> Result<bool> {
        auto r = get_record(id);
        if (r) {
            return Result<bool>{ true };
        }
        if (r.error().code_enum == ErrorCode::StorageRecordNotFound) {
            return Result<bool>{ false };
        }
        return Result<bool>{ r.error() };
    }

    /// @brief 清空全部记录；经 transaction + remove 实现（后端可覆写为单语句）。
    [[nodiscard]] virtual auto clear() -> Result<void> {
        return transaction([this](StorageBackend &) -> Result<void> {
            auto ids = list();
            if (!ids) {
                return Result<void>{ ids.error() };
            }
            for (const auto &id : ids.value()) {
                if (auto e = remove(id); !e) {
                    return e;
                }
            }
            return Result<void>{};
        });
    }

    /// @brief 主动落盘（缓冲型后端覆写；默认 no-op）。
    virtual auto flush() -> Result<void> { return Result<void>{}; }

    /// @brief 关闭并释放资源（默认 no-op）。
    virtual auto close() -> Result<void> { return Result<void>{}; }
};

} // namespace aurora::storage
