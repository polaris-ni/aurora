#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/preferences/preferences.h"

namespace aurora {

/// @brief 最近文档列表：应用级「新 → 旧」有序集合（会话内），可选经 `Preferences` 持久化。
///
/// 用途：应用把它接到「文件 → 最近打开」菜单——打开 / 保存文档后调 `add`，菜单刷新时取 `list`；
/// 要让用户在**系统入口**（任务栏跳转列表、资源管理器「最近使用」）也看到同一批文档，显式调
/// `project_to_os()`。
///
/// 设计要点：
/// - **内存优先**：`add` / `clear` 只改内存列表；`Preferences` 侧仅在 `attach` 后由显式 `sync()`
///   **整体**写穿，落盘时机由 App 的 `Preferences::flush()` 决定（与 `Preferences` 的「显式提交」
///   哲学一致）。
/// - **懒回读**：`list` / `add` / `size` 首次触达且已 `attach` 时从 `Preferences` 填充内存缓存，
///   不做全量预热；回读不产生待落盘标记（值已在后端）。
/// - **容量与去重**：`add` 对同路径「先摘后插」实现去重并置顶；超过 `AURORA_MAX_ENTRIES` 时淘汰
///   最旧的一条（列表尾部）。
/// - **路径口径**：只做字符串级存取——不校验文件是否存在、不做路径规范化（`..` / 相对路径 /
///   大小写差异一律按原样字符串比较）。理由：本类在主线程且不触碰文件系统，读存在性会引入不可测
///   的 IO 副作用与竞态；「最近文档里留着已删文件要不要显示」是应用侧决策，不是 store 该替应用做的。
///   OS 投影**沿用**这一口径：相对路径与不存在的路径照常进入投影计划，由应用在 `add` 前自行规范化。
/// - **持久化信封**：分组下单键 JSON 数组 `{"documents":[...]}`。与 `ScrollStorage` 的「每键一条目」
///   是有意分歧：最近文档是**有序集合、顺序即语义**，用索引当键会把顺序编码进键名，墓碑与重排逻辑
///   都不必要地复杂。
/// - **宽容降级**：读缺失或信封损坏（值非数组、元素非字符串）时得到空列表，不抛错、不清除用户数据。
/// - **无内置变更通知**：本类只管存取，菜单刷新由调用方在 `add` / `clear` 后自行驱动（同
///   `ScrollStorage` 的口径，避免引入第二套通知机制）。
/// - **OS 投影是显式动作**：`add` / `clear` **不**触碰 OS 侧，推送只由 `project_to_os()` 发起。
///   理由：投影改的是 OS 状态（可能被系统审计或同步），而撤销一条已推给 OS 的记录比不推麻烦得多
///   ——文档「打开成功」才该进最近列表，而打开失败也常常已经走过 `add` 的调用点。时机归应用。
///
/// 定位分工：本类管「有哪些最近文档」的存取、持久化与向 OS 的显式投影，不负责何时记录、何时展示。
/// OS 级投影的能力归属见 `PlatformCapabilities::recent_documents_os_projection`（缺位平台返回
/// `recent-documents-projection-unsupported`，应用内列表照常工作，不因此降级）。
///
/// @note Thread: main-thread only（单线程 UI；与 `Preferences` 的内部加锁策略不同，本类不加锁）
/// @note Side-effects: `attach`/`sync` 触碰 `Preferences`；`project_to_os`/`clear_os_projection` 触碰 OS 状态
/// @note Rebuildable: no（进程级会话状态）
class RecentDocuments {
  public:
    /// @brief 进程级单例（懒构造；仿 `preferences::Preferences::instance`）。
    /// @return 单例引用（首次调用时构造）。
    [[nodiscard]] static auto instance() -> RecentDocuments &;

    static constexpr std::size_t AURORA_MAX_ENTRIES = 16;  ///< 容量上限（条）；无运行期 setter，避免出现第二真源
    static constexpr const char *AURORA_GROUP_NAME =
        "recent_documents";  ///< 持久化分组名：`attach` 后数据位于 `Preferences` 的该分组下

    // ---------- 会话内读写 ----------

    /// @brief 记录一次文档打开：同路径去重并置顶，超容量淘汰最旧；空路径为 no-op。
    /// @param path 文档路径（原样保存，不做存在性校验与规范化）；空串为 no-op。
    auto add(std::string_view path) -> void;

    /// @brief 最近文档列表（新 → 旧序的值拷贝）。
    /// @return 当前条目的拷贝；无记录、未 `attach` 或信封损坏时为空列表（不是错误）。
    [[nodiscard]] auto list() -> std::vector<std::string>;

    /// @brief 清空列表：清内存并打墓碑，下次 `sync` 落盘为「无记录」。
    auto clear() -> void;

    /// @brief 当前条目数（诊断 / 测试用；与 `list` 同口径，会触发懒回读）。
    /// @return 内存列表的长度。
    [[nodiscard]] auto size() -> std::size_t;

    /// @brief 清空全部状态（内存列表 / 待落盘标记 / 持久化绑定）。测试与重置用。
    ///
    /// 进程级单例没有第二个实例，故「模拟新实例（进程重启后重新 attach）」的做法是：先调本方法
    /// 复位，再 `attach` 同一个 `Preferences`，随后 `list` 即走懒回读路径。
    auto clear_all() -> void;

    // ---------- 持久化（可选） ----------

    /// @brief 注入偏好存储作为持久化后端（非拥有）；此后 `sync()` 可写穿。
    /// @param prefs 注入的偏好存储实例；生命周期由调用方保证长于本 store。
    /// @note 不做全量预热枚举——读取走 `list` / `add` 的懒回读。
    auto attach(preferences::Preferences &prefs) -> void;

    /// @brief 解除持久化绑定（内存列表保留；未 `sync` 的待落盘标记一并保留，供再次 `attach` 后提交）。
    auto detach() -> void;

    /// @brief 是否已绑定持久化后端。
    /// @return 已绑定为 true；仅内存模式为 false。
    [[nodiscard]] auto attached() const -> bool { return prefs_ != nullptr; }

    /// @brief 把待落盘改动写入 `Preferences` 内存（值或墓碑），并清除待落盘标记。
    /// 未 `attach` 时为空操作（不丢内存列表、不清标记）。**不做文件落盘**——由 App 调
    /// `Preferences::flush()` 决定时机，与既有窗口几何持久化一致。
    auto sync() -> void;

    // ---------- OS 级投影（显式） ----------

    /// @brief 把当前 `list()` 投影给操作系统（任务栏跳转列表 / 资源管理器「最近使用」等系统入口）。
    ///
    /// 契约：
    /// - **显式**：`add` / `clear` 不触发本方法，投影时机由应用决定（典型落点是「文档打开成功」之后）；
    /// - **幂等**：同一列表重复投影不重复堆积（Win32 侧由壳按路径去重快捷方式）；
    /// - **推送的是折算后的计划**：丢弃空串条目、去重保持首次出现序、按平台上限截断且**新 → 旧优先**
    ///   （尾部最旧先丢）。列表为空时产出空计划并返回成功——空不是错误；
    /// - **不做路径规范化、不校验存在性**：与 `add` 的字符串级口径同源，需要绝对路径请应用在
    ///   `add` 前自行规范化；
    /// - **失败不影响应用内列表**：返回错误只说明「没推出去」，`add` 已经成立的内存列表与
    ///   `Preferences` 状态**不变**，`list()` / `size()` 照旧——重试与否由调用方决定。
    ///
    /// 平台归属：能力位见 `PlatformCapabilities::recent_documents_os_projection`（当前 Win32 /
    /// D3D11 / MacOS 落了桥；Linux 无 OS 级机制）。位为 `false` 的平台返回
    /// `recent-documents-projection-unsupported`，平台侧真实故障走结构化错误（`platform-unavailable` /
    /// `platform-com-init-failed`），两类不混淆。
    /// @note 成功仅表「已投递给 OS」：Win32 的 `SHAddToRecentDocs` 无回执，OS 是否落记录另由
    ///       系统侧策略决定（用户关掉「显示最近打开的项目」时返回 `platform-unavailable`）。
    /// @return 成功（含空计划）或结构化错误。
    [[nodiscard]] auto project_to_os() -> Result<void>;

    /// @brief 撤销本应用贡献给 OS 的最近文档记录（不影响应用内列表与 `Preferences`）。
    ///
    /// 语义边界（各平台自述，见 `specification/06-app-platform.md` §8.10）：OS 侧最近文档是
    /// **用户级全局集合**，别的应用贡献的条目不在本方法的射程内；Win32 侧撤销的是「本进程投影
    /// 记录内的条目」对应的快捷方式——同一文件若也被别的应用打开过，那条是共享记录，删除对它同样生效。
    /// @return 成功（含无记录可撤）或结构化错误（平台无此机制 / 平台侧故障）。
    [[nodiscard]] auto clear_os_projection() -> Result<void>;

  private:
    RecentDocuments() = default;

    /// @brief 懒回读：已 `attach`、尚未回读过、且本会话尚无改动时，从 `Preferences` 填充内存列表
    ///        （只做一次）。本会话已 `add` / `clear` 过时内存为准，不覆盖尚未 `sync` 的结果。
    auto ensure_loaded() -> void;

    std::vector<std::string> documents_;  ///< 会话内列表（新 → 旧；含从 Preferences 回读的缓存）
    bool loaded_ = false;  ///< 是否已回读过后端；未 attach 时保持 false，绑定后仍会回读
    bool dirty_ = false;  ///< 待落盘值标记（`add` 后置位，`sync` 后清除）
    bool removed_ = false;  ///< 待落盘墓碑标记（`clear` 后置位，`sync` 后清除）
    preferences::Preferences *prefs_ = nullptr;  ///< 可选持久化后端（非拥有）
};

}  // namespace aurora
