#pragma once

// Win32 最近文档投影桥：**内部头**（与 `win32_drag.h` / `win32_ime.h` 同列于 src/）。
//
// 机制：`SHAddToRecentDocs(SHARD_PATHW, path)` 把一条文件路径投进用户的「最近使用的项目」——
// 资源管理器「快速访问 → 最近使用」与任务栏跳转列表的 Recent 类别都从这里取数。实测（本机
// Windows 11 + MinGW SDK 的 `shlobj.h`）该 API 只有 `SHARD_PIDL` / `SHARD_PATHA` / `SHARD_PATHW`
// 三个 flag、返回 `void`：**没有**「空参即清空」形态，也**没有**逐条撤销 API，单条投递成败无从回读。
// 故本桥的口径是：
// - 返回成功的确切含义是**「已投递给 OS」**，不是「OS 已落记录」——API 无回执，`GetLastError` 也不是
//   它的约定失败通道（壳内部的无关调用会置位），据它判成败是假信号。落没落记录由真机探针直查
//   Recent 目录验收；实测该落盘还**晚于返回**（投递与写出解耦，毫秒级），故任何「调用后立即读 OS」
//   的判据都要有界轮询，撤销侧也据此只销「确认删掉了」的账（见 `win32_recent_documents_clear`）；
// - **可证的失败才报错**，共三处：路径 UTF-8→UTF-16 转换失败、用户级 Recent 目录取不到 / 枚举失败、
//   以及下条的用户策略；
// - **用户在系统设置里关掉「在跳转列表中显示最近打开的项目」（`HKCU\...\Explorer\Advanced` 的
//   `Start_TrackDocs=0`）时，`SHAddToRecentDocs` 静默无操作**。这条是注册表可证的旁证，故如实返回
//   `platform-unavailable` 错误（不谎报成功），真机探针据此判 SKIP 而非 FAIL——它是用户策略，不是库的缺陷。
//
// 撤销（`clear_os_projection()` 的落地）：OS 侧最近文档是**用户级全局集合**（不是本应用私有），
// 逐条撤销没有 API，唯一可证的形态是删除壳自己写出的快捷方式——Recent 目录（`%APPDATA%` 下的
// `Microsoft\Windows\Recent`）里的 `<文件名>.lnk`。本桥因此维护一份**本进程投影记录**，清空时
// 枚举该目录、经 `IShellLinkW` 解析每条 `.lnk` 的真实目标、只删目标命中记录的那些。两条如实申报的
// 边界：
// ① 同一文件若也被别的应用打开过，那条快捷方式是**共享**记录，删除对它同样生效；
// ② 注册表侧的按扩展名 MRU（`HKCU\...\Explorer\RecentDocs`）是用户全局的二进制 MRU，
//    库改写它需自备 `MRUListEx` 重排、写坏会污染用户其它应用的记录，**本桥不碰**。
//
// 门控与 `win32_drag.h` 同款（平台宏 ∧ 后端宏析取）：Win32 / D3D11 / WgpuWin32 三类 Surface 共用
// 同一宿主，能力位三者同翻（见 `window/platform.h`）。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

#include <cstddef>
#include <string>
#include <vector>

#include "aurora/core/result.h"

namespace aurora::detail {

/// @brief Win32 侧单次投影的条目上限（平台侧事实，由桥申报给折算层）。
///
/// 取「开始菜单隐私设置 → 最近使用的项目数」的系统默认值 20；用户可调（注册表
/// `HKCU\...\Explorer\Advanced\MaxDocumentHistory`）。store 自身容量 16 恒小于它，故实际投递
/// 条数由 store 侧决定，本上限是「不与 OS 已知的显示上限打架」的护栏。
inline constexpr std::size_t AURORA_WIN32_RECENT_DOCUMENTS_MAX_ENTRIES = 20U;

/// @brief Win32 最近文档投影桥的**宿主引用守卫**。
///
/// 构造时把投影通路登记进折算层的进程级单槽（引用计数 +1），析构时计数 -1、归零才注销——
/// 多窗口场景下每个宿主各持一份守卫，先关的窗口不会把仍在世的窗口的通路一起拆掉。
///
/// 通路本身是无 HWND 的：`SHAddToRecentDocs` 是进程级调用，与具体窗口句柄无关。宿主持有本守卫
/// 的意义是**生命周期接线**（窗口在 ⇒ 通路在，窗口销毁期先散守卫再放窗口，与 IME / 拖放桥同款
/// 纪律），不是「投影必须有窗口」——无宿主进程由 `win32_recent_documents_ensure_registered()`
/// 自举同一条通路。
/// @note Thread: main-thread only（投影记录是无锁进程态，与单线程 UI 契约一致）
class Win32RecentDocumentsBridge {
  public:
    Win32RecentDocumentsBridge();
    ~Win32RecentDocumentsBridge();

    Win32RecentDocumentsBridge(const Win32RecentDocumentsBridge &) = delete;
    auto operator=(const Win32RecentDocumentsBridge &) -> Win32RecentDocumentsBridge & = delete;
    Win32RecentDocumentsBridge(Win32RecentDocumentsBridge &&) = delete;
    auto operator=(Win32RecentDocumentsBridge &&) -> Win32RecentDocumentsBridge & = delete;

    /// @brief 本守卫是否已计入引用（即通路是否由本实例登记过）。
    /// @return 构造成功后恒为 true；显式 `detach()` 后为 false。
    [[nodiscard]] auto installed() const -> bool { return installed_; }

    /// @brief 提前释放本守卫的引用（窗口销毁路径显式调用；析构会再调一次，幂等）。
    auto detach() -> void;

  private:
    bool installed_ = false;  ///< 本实例是否持有一份进程级引用
};

/// @brief 幂等自举：确保进程级投影通路已登记（供**无窗口宿主**的调用点使用）。
///
/// 由 `RecentDocuments::project_to_os()` 在 Win32/D3D11 上调用：投影记录一旦建立即进程级长存
/// （不随窗口销毁消失），故「先记文档、后建窗口」的应用同样能投影。
/// @return 恒为 true（登记是无失败路径的进程态操作）。
[[nodiscard]] auto win32_recent_documents_ensure_registered() -> bool;

/// @brief 投影 UTF-8 路径清单给 OS（折算层后端回调的实现面；也供真机探针直连）。
/// @param plan 投影计划（已由折算层去过空串 / 去重 / 截断）。
/// @return 成功（含 OS 因用户策略未落盘的情形）或结构化错误（路径形态转换失败）。
[[nodiscard]] auto win32_recent_documents_project(const std::vector<std::string> &plan) -> Result<void>;

/// @brief 撤销本进程投影记录内的条目（删除 Recent 目录里目标命中的 `.lnk`）。
///
/// 记账只销「确认删掉了」的那几条：实测壳在 `SHAddToRecentDocs` 返回**之后**才写出快捷方式，
/// 撤销时找不到不等于永远不会出现，当场销账会把迟到的记录变成无主残留（真机探针复现过）。
/// @return 成功（含记录为空、条目尚未落盘的情形）或结构化错误（COM / 目录枚举失败）。
[[nodiscard]] auto win32_recent_documents_clear() -> Result<void>;

/// @brief 本进程投影记录的条目数（真机探针与诊断观测点，非公共 API）。
/// @return 已投影且尚未撤销的路径条数。
[[nodiscard]] auto win32_recent_documents_projected_count() -> std::size_t;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11 || GPU_WGPU)
