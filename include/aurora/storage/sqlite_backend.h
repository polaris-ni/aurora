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
    /// @brief 打开（或创建）单文件库/`:memory:` 库并就绪 schema；失败不抛异常。
    /// @param opts 库路径与 WAL/内存模式选择；path 为空回退默认配置目录。
    explicit SqliteBackend(const SqliteOptions &opts = {});

    /// @brief 析构：释放连接（与 `close()` 走同一条实体路径，但**不经虚派发**——析构期派生
    ///        子对象已销毁，调虚 `close()` 会静默绕过派生侧收尾）。
    ~SqliteBackend() override;

    /// @brief 后端是否成功打开（库可开、schema 就绪）。`Storage::create(SqliteOptions)` 据它返回 Result。
    /// @return 构造成功且连接可用为 true；否则 false（所有操作返回错误）。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 写入信封：INSERT OR REPLACE 单语句；Binary 载荷内联 BLOB（无 sidecar）。
    /// @param id 记录主键。
    /// @param rec 完整信封（blob_ref 恒空）。
    /// @return 成功返回空值；未打开/SQL 失败返回错误。
    [[nodiscard]] auto put_record(const std::string &id, const StorageRecord &rec) -> Result<void> override;

    /// @brief 读取信封：SELECT 单行并还原字段。
    /// @param id 记录主键。
    /// @return 命中携带信封；未命中返回 StorageRecordNotFound。
    [[nodiscard]] auto get_record(const std::string &id) -> Result<StorageRecord> override;

    /// @brief 删除记录：DELETE 单语句；不存在视为成功（幂等）。
    /// @param id 记录主键。
    /// @return 成功（含不存在）返回空值；SQL 失败返回错误。
    [[nodiscard]] auto remove(const std::string &id) -> Result<void> override;

    /// @brief 枚举全部 id：SELECT id（不读载荷）。
    /// @return 恒成功（未打开为错误）；顺序不保证。
    [[nodiscard]] auto list() -> Result<std::vector<std::string>> override;

    /// @brief 真事务：外层 BEGIN IMMEDIATE，体返回错误即 ROLLBACK；嵌套调用加入同一事务（计深度）。
    /// @param body 事务体；异常/错误触发整体回滚。
    /// @return body 的结果原样返回。
    [[nodiscard]] auto transaction(const std::function<Result<void>(StorageBackend &)> &body) -> Result<void> override;

    /// @brief 存在性检查走 SELECT EXISTS（不读载荷）；不存在的默认 get_record 全量读太贵。
    /// @param id 记录主键。
    /// @return 存在为 true；否则 false；SQL 失败返回错误。
    [[nodiscard]] auto contains(const std::string &id) -> Result<bool> override;

    /// @brief 清空为单语句 DELETE FROM（默认实现是事务内逐条 remove）。
    /// @return 成功返回空值；SQL 失败返回错误。
    [[nodiscard]] auto clear() -> Result<void> override;

    /// @brief WAL 下执行 wal_checkpoint(TRUNCATE) 收缩 -wal 文件；非 WAL / 内存库为 no-op。
    /// @return 恒成功（no-op 亦成功）。
    [[nodiscard]] auto flush() -> Result<void> override;

    /// @brief 关闭连接（幂等；析构自动调用）。
    /// @return 恒成功（重复 close 亦成功）。关闭后 `is_open()` 为 false，其余操作按「未打开」返回错误。
    [[nodiscard]] auto close() -> Result<void> override;

    /// @brief 数据库连接是唯一资源属主：显式删除拷贝/移动，补齐五件套口径
    ///        （误拷贝会在编译期报错，而非落到 unique_ptr 成员的隐式行为）。
    ///        删除的特-member 函数按惯例放 public：私有删除只让友元/成员的误用
    ///        变成「私有成员不可访问」这种误导性报错。
    SqliteBackend(const SqliteBackend &) = delete;
    auto operator=(const SqliteBackend &) -> SqliteBackend & = delete;
    SqliteBackend(SqliteBackend &&) = delete;
    auto operator=(SqliteBackend &&) -> SqliteBackend & = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool open_ = false;

    /// @brief 关连接的实体工作（非虚）：`close()` 与析构共用同一路径。
    /// @details 析构内不得调虚 `close()`——那一刻派生类子对象已销毁，虚派发静默落到本类
    ///          槽位，「派生重写的 close()」被绕过（本类的连接释放仍会执行，但派生侧的
    ///          收尾不会）。故析构只调本函数，`close()` 转调它，幂等语义不变。
    /// @return 恒成功（连接已关或未开均为成功）。
    [[nodiscard]] auto close_connection() -> Result<void>;
};

}  // namespace aurora::storage

#endif  // AURORA_ENABLE_STORAGE_SQLITE
