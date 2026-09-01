#pragma once

// ============================================================================
// storage_types.h — 存储抽象层核心类型（值模型 / 信封 / 变更事件）
// ----------------------------------------------------------------------------
// 这些是存储子系统对「不变量」的声明：后端只认 `StorageRecord` 信封，门面负责在
// 用户视角的 `id → value`（Json 或原生二进制）与内部信封之间转换，使后端零语义负担。
// 见 codespec/architecture/ARCHITECTURE_RUNTIME.md §4.11。
// ============================================================================

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace aurora::storage {

/// @brief 二进制载荷类型（对标 Room BLOB / Realm data / Hive 二进制）。
using StorageBytes = std::vector<std::byte>;

/// @brief 库内 JSON 别名（不依赖 widget 头，保持存储层低耦合）。
using Json = nlohmann::json;

/// @brief 载荷线格式：决定后端如何落盘与反序列化路由。
enum class StorageEncoding : std::uint8_t { Json = 0, Binary = 1 };

/// @brief 值模型放宽（对标 Room/Core Data/Realm/Hive 的一等公民二进制）：
///         要么 Json（默认、人类可读、可迁移），要么原生二进制（零 base64 膨胀）。
using StorageValue = std::variant<Json, StorageBytes>;

/// @brief 持久化记录信封。后端只认信封，不认裸 value。
/// 字段按对齐重排并去 optional（mtime 缺失=epoch、blob_ref 空=无）：本机 sizeof 160B → 144B。
struct StorageRecord {
    std::string id;   ///< 记录主键
    std::string type; ///< ""/"__raw__" = 无类型（裸 value）；否则为 StorageSerializable 的类型标签
    std::uint32_t version = 1;
    StorageEncoding encoding = StorageEncoding::Json; ///< 载荷线格式
    std::chrono::system_clock::time_point mtime;      ///< 最后写入时间；缺失/未知 = epoch（非 optional，省 8B/记录）
    StorageValue payload;                             ///< 用户实际 value（Json 或原生二进制）
    std::string blob_ref; ///< 二进制 sidecar 相对路径（如 `<enc>.bin`）；空 = 无（非 optional，省 8B/记录）
};

/// @brief 变更通知事件（始终在主线程发射，便于 UI 订阅）。
struct StorageChange {
    enum class Operation : std::uint8_t { Put, Remove, Clear, Batch };
    Operation op;
    std::string id; ///< Put/Remove：受影响的 id；Clear/Batch：空
};

/// @brief 变更回调签名。
using StorageChangeCallback = std::function<void(const StorageChange &)>;

/// @brief `FilesystemBackend` 构造选项（默认文件系统后端）。
struct FilesystemOptions {
    std::filesystem::path root;      ///< 记录目录；空 → preferences::default_config_dir()/"aurora_storage"
    bool auto_create_dir = true;     ///< 父目录不存在时自动创建
    bool cross_process_lock = false; ///< 跨进程 advisory 文件锁（LockFileEx / flock）
};

} // namespace aurora::storage
