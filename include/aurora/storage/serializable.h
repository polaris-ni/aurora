#pragma once

// ============================================================================
// serializable.h — 类型化持久化定制点（非侵入式 ADL）
// ----------------------------------------------------------------------------
// 存储层只认信封；「具体类型 ⇄ 信封」由调用方经 ADL 自由函数定制（与 `serialization`
// 的 `to_json(const Widget&)` 风格一致，不强制成员函数、不改既有类型可见性）。
// 提供两条线格式：JSON（默认、可读、可迁移）与原生二进制（对标 Hive TypeAdapter /
// Realm data，零 JSON 开销）。见 ARCHITECTURE.md §4.8。
// ============================================================================

#include <concepts>
#include <string>

#include "aurora/storage/storage_types.h"

namespace aurora::storage {

/// @brief 默认版本号（可经 ADL 在用户命名空间覆盖；或特化本项目命名空间下的
///        `storage_version<T>()`）。门面 put<T>/get<T> 据此触发迁移钩子。
template<typename T> constexpr auto storage_version() -> std::uint32_t { return 1; }

/// @brief 默认类型标签（默认 typeid 短名；可覆盖以得到稳定、可读的 type 字符串）。
/// 返回函数内 static 缓存的 const 引用：`get<T>` 的类型比较零分配（此前每次调用都重建
/// 字符串，长命名空间名会堆分配）。覆写者仍可按值返回 `std::string`；推荐返回短标签
/// （≤15 字符，如 "ws"）——走 SSO 零堆分配，且跨版本/重构稳定（typeid mangled 名在移动
/// 命名空间后会变，反会破坏持久化类型检查）。
template<typename T> inline auto storage_type_name() -> const std::string & {
    static const std::string name = typeid(T).name(); // 线程安全静态初始化，跨 TU 唯一
    return name;
}

/// @brief 默认迁移钩子：不迁移（原样返回）。旧版本记录反序列化前会经此钩子升级。
template<typename T> auto migrate_storage(std::uint32_t /*old_version*/, Json j) -> Result<Json> {
    return Result{ std::move(j) };
}
template<typename T>
inline auto migrate_storage(std::uint32_t /*old_version*/, StorageBytes b) -> Result<StorageBytes> {
    return Result<StorageBytes>{ std::move(b) };
}

/// @brief 类型 T 可经 JSON 持久化的充要条件（默认线格式）。
template<typename T>
concept StorageSerializable = requires(const T &t, T &out, const Json &j) {
    { to_storage_json(t) } -> std::convertible_to<Json>;
    { from_storage_json(out, j) } -> std::convertible_to<Result<void>>;
};

/// @brief 类型 T 可经原生二进制持久化的充要条件（绕过 JSON，对标 Hive/Realm）。
template<typename T>
concept StorageBinarySerializable = requires(const T &t, T &out, const StorageBytes &b) {
    { to_storage_bytes(t) } -> std::convertible_to<StorageBytes>;
    { from_storage_bytes(out, b) } -> std::convertible_to<Result<void>>;
};

/// @brief 门面 put<T>/get<T> 接受的充要条件：满足 JSON 或二进制任一 + 可默认构造。
template<typename T>
concept StorageStorable = (StorageSerializable<T> || StorageBinarySerializable<T>) && std::default_initializable<T>;

} // namespace aurora::storage
