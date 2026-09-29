#pragma once

/// @brief 内存存储后端（零依赖，始终编译）：全部记录存于 std::map，不落盘。
/// @file memory_backend.h
///
/// 对标 HeadlessSurface：用于单元测试与「临时/易失」存储场景。见 ARCHITECTURE.md §4.8。

// ============================================================================
// memory_backend.h — 内存后端（零依赖，始终编译）
// ----------------------------------------------------------------------------
// 对标 HeadlessSurface：不落盘，全部存于内存。用于单元测试与「临时/易失」存储场景。
// 见 ARCHITECTURE.md §4.8。
// ============================================================================

#include <map>
#include <string>

#include "aurora/storage/storage_backend.h"

namespace aurora::storage {

/// @brief 内存后端：StorageBackend 的 map 实现（进程内，无持久化）。
///
/// @note Thread: not thread-safe (callers must external-sync; Storage 门面负责加锁)
class MemoryBackend : public StorageBackend {
  public:
    /// @brief 写入/覆写记录：整体替换 store_ 中同 id 信封。
    /// @param id 记录主键。
    /// @param rec 完整信封（按值拷入）。
    /// @return 恒成功（内存写入无 IO 失败面）。
    [[nodiscard]] auto put_record(const std::string &id, const StorageRecord &rec) -> Result<void> override;

    /// @brief 读取记录信封。
    /// @param id 记录主键。
    /// @return 命中携带信封；未命中返回 StorageRecordNotFound。
    [[nodiscard]] auto get_record(const std::string &id) -> Result<StorageRecord> override;

    /// @brief 删除记录；不存在视为成功（幂等）。
    /// @param id 记录主键。
    /// @return 恒成功。
    [[nodiscard]] auto remove(const std::string &id) -> Result<void> override;

    /// @brief 列出全部 id。
    /// @return 恒成功；id 按 map 字典序（接口层面顺序不保证）。
    [[nodiscard]] auto list() -> Result<std::vector<std::string>> override;

    /// @brief 覆写 transaction：快照回滚（Memory 可精确回滚，对标 Sqlite 真事务）。
    /// @param body 事务体；失败时 store_ 恢复为执行前快照。
    /// @return body 的结果原样返回。
    [[nodiscard]] auto transaction(const std::function<Result<void>(StorageBackend &)> &body) -> Result<void> override;

  private:
    std::map<std::string, StorageRecord> store_;
};

}  // namespace aurora::storage
