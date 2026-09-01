#pragma once

// ============================================================================
// fs_backend.h — 文件系统后端（默认，零依赖，始终编译）
// ----------------------------------------------------------------------------
// 每记录一个 JSON 文件（信封），二进制载荷走 sidecar `<id>.bin` + 信封内 `blob_ref`
// （对齐 Core Data「>1MB 二进制落盘 + 存路径」策略，零 base64 膨胀）。原子写（临时文件 +
// rename），可选跨进程 advisory 锁。见 ARCHITECTURE.md §4.8。
// ============================================================================

#include <memory>

#include "aurora/storage/backend.h"
#include "aurora/storage/storage_types.h"

namespace aurora::storage {

class FilesystemBackend : public StorageBackend {
  public:
    explicit FilesystemBackend(FilesystemOptions opts = {});

    /// @brief 后端是否成功打开（目录可写、锁可获取）。`Storage::create` 据它返回 Result。
    [[nodiscard]] auto is_open() const -> bool { return m_open; }

    [[nodiscard]] auto put_record(const std::string &id, const StorageRecord &rec) -> Result<void> override;
    [[nodiscard]] auto get_record(const std::string &id) -> Result<StorageRecord> override;
    [[nodiscard]] auto remove(const std::string &id) -> Result<void> override;
    [[nodiscard]] auto list() -> Result<std::vector<std::string>> override;

  private:
    /// @brief 获取跨进程 advisory 锁（Windows LockFileEx / POSIX flock）；失败返回 false。
    [[nodiscard]] auto acquire_lock() -> bool;

    FilesystemOptions m_opts;
    std::filesystem::path m_root;
    bool m_open = false;
    std::shared_ptr<void> m_lock = nullptr; ///< 跨进程锁句柄（RAII），无锁时为 nullptr
};

} // namespace aurora::storage
