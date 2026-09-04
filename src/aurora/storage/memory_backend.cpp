// ============================================================================
// memory_backend.cpp — 内存后端实现（零依赖，始终编译）
// ----------------------------------------------------------------------------
// 全部记录存于内存 `std::map`，不落盘。transaction 覆写为快照回滚，使 Memory 后端
// 拥有与 Sqlite 真事务同等的精确回滚语义。见 ARCHITECTURE.md §4.8。
// ============================================================================

#include "aurora/storage/memory_backend.h"

#include "aurora/core/result.h"

namespace aurora::storage {

auto MemoryBackend::put_record(const std::string &id, const StorageRecord &rec) -> Result<void> {
    store_[id] = rec;
    return Result<void>{};
}

auto MemoryBackend::get_record(const std::string &id) -> Result<StorageRecord> {
    const auto it = store_.find(id);
    if (it == store_.end()) {
        return Result<StorageRecord>{
            make_error(ErrorCode::StorageRecordNotFound, "In-memory record does not exist: " + id)};
    }
    return Result{it->second};
}

auto MemoryBackend::remove(const std::string &id) -> Result<void> {
    store_.erase(id);  // 幂等：缺失不视为错误
    return Result<void>{};
}

auto MemoryBackend::list() -> Result<std::vector<std::string>> {
    std::vector<std::string> ids;
    ids.reserve(store_.size());
    for (const auto &kv : store_ | std::views::keys) {
        ids.push_back(kv);
    }
    return Result{std::move(ids)};
}

auto MemoryBackend::transaction(const std::function<Result<void>(StorageBackend &)> &body) -> Result<void> {
    const auto snapshot = store_;  // 复制快照，供失败回滚
    auto r = body(*this);
    if (!r) {
        store_ = snapshot;  // 精确回滚
    }
    return r;
}

}  // namespace aurora::storage
