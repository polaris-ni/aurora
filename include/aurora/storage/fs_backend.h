#pragma once

/// @brief 文件系统存储后端（默认，零依赖，始终编译）：每记录一个 JSON 信封文件。
/// @file fs_backend.h
///
/// 二进制载荷走 sidecar `<id>.bin` + 信封内 `blob_ref`；原子写（临时文件 + rename）。

// ============================================================================
// fs_backend.h — 文件系统后端（默认，零依赖，始终编译）
// ----------------------------------------------------------------------------
// 每记录一个 JSON 文件（信封），二进制载荷走 sidecar `<id>.bin` + 信封内 `blob_ref`
// （对齐 Core Data「>1MB 二进制落盘 + 存路径」策略，零 base64 膨胀）。原子写（临时文件 +
// rename），可选跨进程 advisory 锁。见 ARCHITECTURE.md §4.8。
// ============================================================================

#include <memory>

#include "aurora/storage/storage_backend.h"
#include "aurora/storage/storage_types.h"

namespace aurora::storage {

/// @brief 默认后端：把 `StorageRecord` 信封持久化为 root 目录下的文件。
///
/// 文件名对 id 做 base64url 编码以规避非法路径字符；写为临时文件 + rename 原子替换；
/// 二进制载荷另存 `<enc>.bin` sidecar，信封只记 blob_ref。打开失败不抛异常，
/// 经 is_open() 汇报，由 `Storage::create` 转成 Result。
///
/// @note Thread: not thread-safe (Storage 门面加锁串行化；跨进程锁为 advisory)
class FilesystemBackend : public StorageBackend {
  public:
    /// @brief 打开/创建存储根目录（可选 auto_create_dir、cross_process_lock）。
    /// @param opts 根目录路径与锁策略；root 为空回退默认配置目录。
    explicit FilesystemBackend(FilesystemOptions opts = {});

    /// @brief 后端是否成功打开（目录可写、锁可获取）。`Storage::create` 据它返回 Result。
    /// @return 构造成功且资源可用为 true；否则 false（所有操作返回错误）。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 写入信封：JSON 原子落盘（临时文件 + rename）；Binary 载荷另写 sidecar。
    /// @param id 记录主键（落盘文件名取其 base64url 编码）。
    /// @param rec 完整信封。
    /// @return 成功返回空值；未打开/IO 失败返回错误（rename 前失败不留半成品）。
    [[nodiscard]] auto put_record(const std::string &id, const StorageRecord &rec) -> Result<void> override;

    /// @brief 读取信封：解析 JSON 文件，Binary 载荷从 sidecar 还原。
    /// @param id 记录主键。
    /// @return 命中携带信封；文件不存在返回 StorageRecordNotFound；解析失败返回错误。
    [[nodiscard]] auto get_record(const std::string &id) -> Result<StorageRecord> override;

    /// @brief 删除记录：信封与 sidecar 一并删除；不存在视为成功（幂等）。
    /// @param id 记录主键。
    /// @return 成功（含不存在）返回空值；IO 失败返回错误。
    [[nodiscard]] auto remove(const std::string &id) -> Result<void> override;

    /// @brief 枚举 root 下全部信封文件对应的 id。
    /// @return 恒成功（目录不可读时为错误）；顺序不保证（文件系统枚举序）。
    [[nodiscard]] auto list() -> Result<std::vector<std::string>> override;

  private:
    /// @brief 获取跨进程 advisory 锁（Windows LockFileEx / POSIX flock）；失败返回 false。
    [[nodiscard]] auto acquire_lock() -> bool;

    FilesystemOptions opts_;
    std::filesystem::path root_;
    bool open_ = false;
    std::shared_ptr<void> lock_ = nullptr;  ///< 跨进程锁句柄（RAII），无锁时为 nullptr
};

}  // namespace aurora::storage
