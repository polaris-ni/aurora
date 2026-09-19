#pragma once

// ============================================================================
// sqlite_backend.h — SQLite 后端（记录仓储第三后端，opt-in：AURORA_ENABLE_STORAGE_SQLITE）
// ----------------------------------------------------------------------------
// 单文件数据库（或 `:memory:`）。相对 FilesystemBackend 的差异化能力：
//   - 真事务：`transaction` 覆写为 BEGIN IMMEDIATE / COMMIT / ROLLBACK（体失败整体回滚）；
//   - 二进制载荷 BLOB 内联存储（无 sidecar，`blob_ref` 恒空），免文件名编码与孤儿文件；
//   - `contains` 走 SELECT EXISTS（不读载荷）、`clear` 单语句 DELETE。
// 头仅当 AURORA_ENABLE_STORAGE_SQLITE 定义（CMake 选项，默认 OFF）时编译，避免默认
// 零三方依赖构建引入 sqlite3 amalgamation。见 ARCHITECTURE.md §4.8。
// ============================================================================

#ifdef AURORA_ENABLE_STORAGE_SQLITE

#include <memory>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/storage/storage_backend.h"
#include "aurora/storage/storage_types.h"

namespace aurora::storage {

/// @brief SQLite 持久化后端。单连接；C++ 侧以递归互斥串行化逻辑操作（含事务边界），
///         sqlite3 亦以 serialized 模式构建（Storage 异步 API 可能从 worker 线程触库）。
class SqliteBackend : public StorageBackend {
  public:
    explicit SqliteBackend(SqliteOptions opts = {});
    ~SqliteBackend() override;

    /// @brief 后端是否成功打开（库可开、schema 就绪）。`Storage::create(SqliteOptions)` 据它返回 Result。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    [[nodiscard]] auto put_record(const std::string &id, const StorageRecord &rec) -> Result<void> override;
    [[nodiscard]] auto get_record(const std::string &id) -> Result<StorageRecord> override;
    [[nodiscard]] auto remove(const std::string &id) -> Result<void> override;
    [[nodiscard]] auto list() -> Result<std::vector<std::string>> override;

    /// @brief 真事务：外层 BEGIN IMMEDIATE，体返回错误即 ROLLBACK；嵌套调用加入同一事务（计深度）。
    [[nodiscard]] auto transaction(const std::function<Result<void>(StorageBackend &)> &body) -> Result<void> override;

    /// @brief 存在性检查走 SELECT EXISTS（不读载荷）；不存在的默认 get_record 全量读太贵。
    [[nodiscard]] auto contains(const std::string &id) -> Result<bool> override;

    /// @brief 清空为单语句 DELETE FROM（默认实现是事务内逐条 remove）。
    [[nodiscard]] auto clear() -> Result<void> override;

    /// @brief WAL 下执行 wal_checkpoint(TRUNCATE) 收缩 -wal 文件；非 WAL / 内存库为 no-op。
    [[nodiscard]] auto flush() -> Result<void> override;

    /// @brief 关闭连接（幂等；析构自动调用）。
    [[nodiscard]] auto close() -> Result<void> override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool open_ = false;
};

}  // namespace aurora::storage

#endif  // AURORA_ENABLE_STORAGE_SQLITE
