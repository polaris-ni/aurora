#pragma once

// macOS 最近文档投影桥：**内部头**（与 `win32_recent_documents.h` / `macos_drag.h` 同列于 src/）。
//
// 机制：`NSDocumentController` 的 `noteNewRecentDocumentURL:` —— macOS 上「最近打开」的唯一
// 一等入口。系统菜单（Dock 的「最近使用的文稿」、应用「文件 → 打开最近使用」）从这份集合取数。
//
// 口径与如实申报的边界：
// - 投递**没有回执**：`noteNewRecentDocumentURL:` 返回 void。返回成功的含义是「已交给
//   NSDocumentController」，不是「OS 已落盘」。可证的失败只有形态转换失败（UTF-8 路径无法
//   变成 NSString / NSURL）与 controller 不可用（AppKit 未初始化）。
// - 撤销调 `clearRecentDocuments:`，它清的是**本应用**的整份最近文档集合，不只是本进程投影过
//   的那几条。这是 AppKit 的唯一形态（无逐条移除 API）；与 Win32 桥「只删自己记账的 .lnk」
//   的窄语义不同，调用方在 `clear_os_projection()` 前应知悉该更宽的作用域。
// - 上限取 AppKit 的默认值 `NSRecentDocumentsLimit`（10）；`RecentDocuments` store 自身容量
//   16 更大，故实际由本上限截断（折算层统一执行新 → 旧优先）。
// - 无窗口宿主同样可用：`NSDocumentController` 是应用级对象，`ensure_registered()` 负责自举。
//
// 门控与 `macos_surface.cpp` 相同：macOS 平台宏 ∧ 后端宏；其它平台本文件编译为空 TU。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include <cstddef>
#include <string>
#include <vector>

#include "aurora/core/result.h"

namespace aurora::detail {

/// @brief macOS 侧单次投影的条目上限（`NSRecentDocumentsLimit` 的系统默认值 10）。
inline constexpr std::size_t AURORA_MACOS_RECENT_DOCUMENTS_MAX_ENTRIES = 10U;

/// @brief macOS 最近文档投影桥的**宿主引用守卫**（与 Win32 桥同款）。
///
/// 构造时向折算层登记通路（引用计数 +1），析构时 -1、归零才注销；多窗口各持一份，
/// 先关的窗口不会拆掉仍在世窗口的通路。通路本身与窗口无关，宿主守卫只负责生命周期接线。
/// @note Thread: main-thread only（`NSDocumentController` 与投影记录都按单线程 UI 契约使用）
class MacOSRecentDocumentsBridge {
  public:
    MacOSRecentDocumentsBridge();
    ~MacOSRecentDocumentsBridge();

    MacOSRecentDocumentsBridge(const MacOSRecentDocumentsBridge &) = delete;
    auto operator=(const MacOSRecentDocumentsBridge &) -> MacOSRecentDocumentsBridge & = delete;
    MacOSRecentDocumentsBridge(MacOSRecentDocumentsBridge &&) = delete;
    auto operator=(MacOSRecentDocumentsBridge &&) -> MacOSRecentDocumentsBridge & = delete;

    /// @brief 本守卫是否已计入引用。
    /// @return 构造成功后恒为 true；`detach()` 后为 false。
    [[nodiscard]] auto installed() const -> bool { return installed_; }

    /// @brief 提前释放本守卫的引用（窗口销毁路径显式调用；析构再调一次，幂等）。
    auto detach() -> void;

  private:
    bool installed_ = false;  ///< 本实例是否持有一份进程级引用
};

/// @brief 幂等自举：确保进程级投影通路已登记（供无窗口宿主使用）。
/// @return 恒为 true（登记是无失败路径的进程态操作）。
[[nodiscard]] auto macos_recent_documents_ensure_registered() -> bool;

/// @brief 把折算后的路径计划投递给 `NSDocumentController`（桥后端实现面；探针可直连）。
/// @param plan 投影计划（已由折算层去过空串 / 去重 / 截断）。
/// @return 成功（含 API 无回执、记录稍后落盘的情形）或结构化错误（形态转换 / controller 不可用）。
[[nodiscard]] auto macos_recent_documents_project(const std::vector<std::string> &plan) -> Result<void>;

/// @brief 撤销本应用的最近文档集合（`clearRecentDocuments:`）。
/// @note 作用域是**整个应用**，不只是本进程投影过的条目（见文件头注）。
/// @return 成功或结构化错误（controller 不可用）。
[[nodiscard]] auto macos_recent_documents_clear() -> Result<void>;

/// @brief 本进程投影记账的条目数（真机探针与诊断观测点，非公共 API）。
/// @return 已投影且尚未撤销的路径条数。
[[nodiscard]] auto macos_recent_documents_projected_count() -> std::size_t;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
