#pragma once

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

class MemoryBackend : public StorageBackend {
  public:
    [[nodiscard]] auto put_record(const std::string &id, const StorageRecord &rec) -> Result<void> override;
    [[nodiscard]] auto get_record(const std::string &id) -> Result<StorageRecord> override;
    [[nodiscard]] auto remove(const std::string &id) -> Result<void> override;
    [[nodiscard]] auto list() -> Result<std::vector<std::string>> override;

    /// @brief 覆写 transaction：快照回滚（Memory 可精确回滚，对标 Sqlite 真事务）。
    [[nodiscard]] auto transaction(const std::function<Result<void>(StorageBackend &)> &body) -> Result<void> override;

  private:
    std::map<std::string, StorageRecord> store_;
};

}  // namespace aurora::storage
