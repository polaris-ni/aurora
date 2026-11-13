#pragma once

// ============================================================
// 测试框架 —— 进程内 memory 剪贴板后端
// ------------------------------------------------------------
// 经内部接缝 `aurora/app/detail/clipboard_backend.h` 注册到与平台后端**同一张表**，
// 因此被测的始终是产品代码本身：门面的每个入口只做「表里有该字段则委托」的一次判断，
// 不存在与平台路径并行的第二实现。
//
// 填 `set_change_listener` 即模拟「本平台有变更监听机制」；不填即模拟「无机制」，
// 与平台路径同一条能力判据（字段存在即有能力）。
// ============================================================

namespace aurora::testing::clipboard_memory {

/// @brief 安装 memory 后端（清空内容与惰性要约），并模拟「有变更监听机制」。
/// @return 恒为 true（后端注册在测试进程内无失败路径）。
[[nodiscard]] auto install() -> bool;

/// @brief 仅清空内容（文本 / 图像 / 富格式载荷 / 惰性要约及其渲染缓存），保留安装态。
auto reset() -> void;

/// @brief 卸载 memory 后端（仅在此前确有安装时移除后端表），并清空内容。
/// @return 此前是否处于安装态。
[[nodiscard]] auto remove() -> bool;

/// @brief 当前是否安装。
[[nodiscard]] auto installed() -> bool;

/// @brief 模拟「本平台有无变更监听机制」：false 时后端表不填 `set_change_listener`。
/// @param supported true = 模拟有监听机制；false = 模拟无机制。
auto set_listen_supported(bool supported) -> void;

/// @brief 模拟一次「剪贴板内容变化」，与平台桥共用 `clipboard::detail::fire_change()`。
auto emit_change() -> void;

/// @brief 模拟一次「所有权被其它进程夺走」：未安装或未持有惰性要约时为 no-op；
/// 否则清空要约与其渲染缓存，再经 `clipboard::detail::fire_ownership_lost()` 派发。
auto emit_ownership_lost() -> void;

}  // namespace aurora::testing::clipboard_memory
