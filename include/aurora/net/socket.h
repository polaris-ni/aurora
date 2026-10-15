#pragma once

/// @brief 迁移期兼容层：`aurora::net` 同步阻塞面的旧名字（内容已迁至 `aurora/net/blocking.h`）。
/// @file net/socket.h
///
/// 本头不再定义任何类型，只做两件事：把 `aurora::net::StreamSocket` / `aurora::net::Listener`
/// 转发到 `aurora::net::blocking` 的同名类型，并把迁移动机写在一处。CHG-018 §4 的处置把同步阻塞面
/// 收缩进子命名空间，是为了让「阻塞语义」出现在头名与命名空间里——面向使用者的一侧应走
/// `aurora/net/transport.h` 的事件驱动传输族，把阻塞类型留在 `aurora::net` 顶层会让它看起来像推荐用法。
///
/// 转发是**同一枚类型**，不是包装：成员、错误码、超时口径、句柄借用规则逐名不变，因此不存在
/// 「旧名字还在、新面已改语义」的中间态。时间表（CHG-018 §20）：本别名随 `1.0.0-alpha.10` 落地，
/// 随下一个 alpha（`1.0.0-alpha.11`）连同本头一起删除。
///
/// 逐符号对应（迁移时只需改名字前缀，不需改任何调用形态）：
/// - `net::StreamSocket` → `net::blocking::StreamSocket`：`connect_loopback` `connect` `is_valid`
///   `native_handle` `set_io_timeout` `read_some` `write_all` `is_loopback_peer` `peer_endpoint`
///   `close` 全部同名同义。
/// - `net::Listener` → `net::blocking::Listener`：`listen_loopback` `bind` `is_valid` `port`
///   `native_handle` `wait_readable` `accept` `close` 全部同名同义。
/// - `net::Session` → **无替代符号**。Windows 的 Winsock 运行时改由实现侧随句柄钉住（该类型降为
///   `src/aurora/net/detail/runtime.h` 的库内细节），迁移动作是**删掉**那枚局部变量：留着它同样
///   正确（引用计数与钉住共用一份计数），但它不再是任何用法的前置条件。
///
/// 范围、失败形态与 `Result<T>` / `net-*` 错误码的整体契约见 `aurora/net/blocking.h`。
///
/// @deprecated 改用 `aurora/net/blocking.h` 里的 `aurora::net::blocking` 同名类型（成员逐名不变），自 1.0.0-alpha.10
/// 起弃用、1.0.0-alpha.11 移除。

#include "aurora/net/blocking.h"

namespace aurora::net {

using StreamSocket [[deprecated(
    "aurora::net::StreamSocket is deprecated since 1.0.0-alpha.10; use aurora::net::blocking::StreamSocket "
    "instead (this alias is removed in 1.0.0-alpha.11)")]] =
    blocking::StreamSocket;  ///< 迁移期别名：与 `blocking::StreamSocket` 同一类型（新代码直接用后者）。

using Listener [[deprecated(
    "aurora::net::Listener is deprecated since 1.0.0-alpha.10; use aurora::net::blocking::Listener instead "
    "(this alias is removed in 1.0.0-alpha.11)")]] =
    blocking::Listener;  ///< 迁移期别名：与 `blocking::Listener` 同一类型（新代码直接用后者）。

}  // namespace aurora::net
