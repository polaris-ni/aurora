#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "aurora/preferences/preferences.h"

namespace aurora {

/// @brief 最近文档列表：应用级「新 → 旧」有序集合（会话内），可选经 `Preferences` 持久化。
///
/// 用途：应用把它接到「文件 → 最近打开」菜单——打开 / 保存文档后调 `add`，菜单刷新时取 `list`。
/// OS 级投影（Windows JumpList / macOS 的 `NSDocumentController`）消费本类的 `list`，不在本类职责内。
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
/// - **持久化信封**：分组下单键 JSON 数组 `{"documents":[...]}`。与 `ScrollStorage` 的「每键一条目」
///   是有意分歧：最近文档是**有序集合、顺序即语义**，用索引当键会把顺序编码进键名，墓碑与重排逻辑
///   都不必要地复杂。
/// - **宽容降级**：读缺失或信封损坏（值非数组、元素非字符串）时得到空列表，不抛错、不清除用户数据。
/// - **无内置变更通知**：本类只管存取，菜单刷新由调用方在 `add` / `clear` 后自行驱动（同
///   `ScrollStorage` 的口径，避免引入第二套通知机制）。
///
/// 定位分工：本类只管「有哪些最近文档」的存取与持久化，不负责何时记录、何时展示。
///
/// @note Thread: main-thread only（单线程 UI；与 `Preferences` 的内部加锁策略不同，本类不加锁）
/// @note Side-effects: `attach`/`sync` 触碰 `Preferences`
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
