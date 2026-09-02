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
    m_store[id] = rec;
    return Result<void>{};
}

auto MemoryBackend::get_record(const std::string &id) -> Result<StorageRecord> {
    const auto it = m_store.find(id);
    if (it == m_store.end()) {
        return Result<StorageRecord>{ make_error(ErrorCode::StorageRecordNotFound,
                                                 "In-memory record does not exist: " + id) };
    }
    return Result{ it->second };
}

auto MemoryBackend::remove(const std::string &id) -> Result<void> {
    m_store.erase(id); // 幂等：缺失不视为错误
    return Result<void>{};
}

auto MemoryBackend::list() -> Result<std::vector<std::string>> {
    std::vector<std::string> ids;
    ids.reserve(m_store.size());
    for (const auto &kv : m_store | std::views::keys) {
        ids.push_back(kv);
    }
    return Result{ std::move(ids) };
}

auto MemoryBackend::transaction(const std::function<Result<void>(StorageBackend &)> &body) -> Result<void> {
    const auto snapshot = m_store; // 复制快照，供失败回滚
    auto r = body(*this);
    if (!r) {
        m_store = snapshot; // 精确回滚
    }
    return r;
}

} // namespace aurora::storage
