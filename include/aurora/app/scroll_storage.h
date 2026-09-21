#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "aurora/preferences/preferences.h"

namespace aurora {

/**
 * @brief 滚动位置注册表：`restore_key → offset`（会话内），可选经 `Preferences` 写穿（跨进程）。
 *
 * 用途：滚动类控件声明一个键，重建（标签页切换 / 列表重挂载 / 热重载）后据此恢复滚动位置；
 * 对标 Flutter `PageStorage`（会话内）与 Android `rememberSaveable`（跨进程）。控件侧接线见
 * `specification/03-layout-render.md` 的滚动控件 props 表。
 *
 * 设计要点：
 * - **内存为准**：`write` 只更新内存 map（滚动热路径零 JSON / 零字符串序列化开销）；
 *   `Preferences` 侧仅在 `attach` 后由显式 `sync()` **批量**写穿，落盘时机由 App 的
 *   `Preferences::flush()` 决定（与 `Preferences` 的「显式提交」哲学一致）。
 * - **懒回读**：`read` 在内存缺失且已 `attach` 时才查 `Preferences`（不枚举全量键集）。
 * - **多窗口隔离**：`BuildContext` 不携带窗口标识，故由窗口宿主在布局入口用 `Scope` RAII
 *   设置「当前作用域」，内部键为 `scope + 0x1f + key`；无作用域（`TestController` /
 *   golden / 单窗口）时退化为全局单桶，行为与不隔离时一致。
 * - **键争用**：同一键被多个控件认领时按「后写覆盖（最后活跃者为准）」工作，并在 `claim`
 *   时提示一次（提示不在滚动热路径上）。
 *
 * 定位分工：本类只管「键 → 偏移」的存取与持久化，不负责何时恢复 / 何时写回——那是滚动控件的
 * `restore_key` 接线（`on_layout` 首次可滚动时读、滚动变化时写）。
 *
 * @note Thread: main-thread only（单线程 UI；与 `Preferences` 的内部加锁策略不同，本类不加锁）
 * @note Side-effects: `attach`/`sync` 触碰 `Preferences`
 * @note Rebuildable: no（进程级会话状态）
 */
class ScrollStorage {
  public:
    /// @brief 进程级单例（懒构造；仿 `preferences::Preferences::instance`）。
    [[nodiscard]] static auto instance() -> ScrollStorage &;

    /// @brief 持久化分组名：`attach` 后数据位于 `Preferences::group("scroll_positions")` 下。
    static constexpr const char *AURORA_GROUP_NAME = "scroll_positions";

    /// @brief 作用域 RAII：构造时设置当前作用域（空串 = 无作用域 / 全局单桶），析构恢复前值。
    ///
    /// 窗口宿主在渲染入口构造本对象即可让该帧内的读写落在本窗口的键空间里；支持嵌套
    /// （内层析构后恢复外层值）。不可拷贝/移动（避免作用域被意外延长或提前结束）。
    ///
    /// **同值零分配**：宿主每帧都会以同一 id 构造一次，故值与当前作用域相同时直接早退
    /// （比较字符串、不保存旧值、不重分配），使每帧开销退化为一次比较。
    class Scope {
      public:
        /// @param scope 目标作用域（空串 = 无作用域 / 全局单桶）
        explicit Scope(std::string_view scope);
        ~Scope();
        Scope(const Scope &) = delete;
        auto operator=(const Scope &) -> Scope & = delete;
        Scope(Scope &&) = delete;
        auto operator=(Scope &&) -> Scope & = delete;

      private:
        std::string saved_;  ///< 被替换掉的前一个作用域（仅 `changed_` 为真时有效）
        bool changed_ = false;  ///< 本次构造是否真正改变了作用域（未改变则析构无需恢复）
    };

    // ---------- 会话内读写 ----------

    /// @brief 写入偏移（滚动热路径：仅内存 map，不触碰 `Preferences`）。
    auto write(std::string_view key, float offset) -> void;

    /// @brief 读取偏移：内存优先，缺失且已 `attach` 时回读 `Preferences` 并填充内存缓存。
    [[nodiscard]] auto read(std::string_view key) -> std::optional<float>;

    /// @brief 清除键（含 `Preferences` 侧：下次 `sync` 打墓碑删除）。
    auto clear(std::string_view key) -> void;

    /// @brief 清空全部状态（内存值 / 待落盘项 / 认领记录 / 作用域 / 持久化绑定）。测试与重置用。
    auto clear_all() -> void;

    /// @brief 内存中记录的键数（诊断/测试用）。
    [[nodiscard]] auto size() const -> std::size_t { return offsets_.size(); }

    /// @brief 当前作用域（空串 = 无作用域）。
    [[nodiscard]] auto current_scope() const -> const std::string & { return scope_; }

    // ---------- 持久化（可选） ----------

    /// @brief 注入偏好存储作为持久化后端（非拥有）；此后 `sync()` 可批量写穿。
    /// @note 不做全量预热枚举——读取走 `read` 的懒回读，避免大文件在注入时被整体搬运。
    auto attach(preferences::Preferences &prefs) -> void;

    /// @brief 解除持久化绑定（内存值保留；未 `sync` 的待落盘项一并保留，供再次 `attach` 后提交）。
    auto detach() -> void;

    /// @brief 是否已绑定持久化后端。
    [[nodiscard]] auto attached() const -> bool { return prefs_ != nullptr; }

    /// @brief 把待落盘改动批量写入 `Preferences` 内存（值 + 墓碑），并清空待落盘队列。
    ///
    /// 未 `attach` 时为空操作（不丢内存值、不清队列）。**不做文件落盘**——由 App 调
    /// `Preferences::flush()` 决定时机，与既有窗口几何持久化一致。
    auto sync() -> void;

    /// @brief 待落盘条目数（值 + 墓碑）。同一键重复写只占 1 条（节流语义）。
    [[nodiscard]] auto pending_writes() const -> std::size_t { return dirty_.size() + removed_.size(); }

    // ---------- 键认领（重复键诊断） ----------

    /// @brief 认领键（控件首次布局时调用一次）。同一键被**不同持有者**认领时提示一次，
    ///        运行语义为「后写覆盖、最后活跃者为准」。
    /// @param owner 持有者身份（控件实例地址）：同一实例重复认领（重布局 / 重建同实例）不提示。
    auto claim(std::string_view key, const void *owner) -> void;

    /// @brief 释放键认领（控件卸载时调用；缺省不调用也安全——认领集合随实例生命周期增长，
    ///        但键数有界，且提示已按「每键一次」去重）。
    auto release(std::string_view key, const void *owner) -> void;

    /// @brief 当前键的认领者数量（测试 / 诊断用）。
    [[nodiscard]] auto owner_count(std::string_view key) const -> std::size_t;

  private:
    ScrollStorage() = default;

    /// @brief 组合作用域前缀：`scope + 0x1f + key`（分隔符取不可打印控制符，避免与用户键冲突）。
    [[nodiscard]] auto scoped_key(std::string_view key) const -> std::string;

    std::unordered_map<std::string, float> offsets_;  ///< 会话内值（含从 Preferences 回读的缓存）
    std::unordered_map<std::string, float> dirty_;  ///< 待落盘值（attach 后由 sync 批量写穿）
    std::unordered_set<std::string> removed_;  ///< 待落盘墓碑（clear 过的键）
    std::unordered_map<std::string, std::unordered_set<const void *>> owners_;  ///< 键 → 认领者集合
    std::unordered_set<std::string> warned_;  ///< 已提示过重复认领的键（每键一次）
    preferences::Preferences *prefs_ = nullptr;  ///< 可选持久化后端（非拥有）
    std::string scope_;  ///< 当前作用域（空 = 无作用域 / 全局单桶；见 Scope）
};

}  // namespace aurora
