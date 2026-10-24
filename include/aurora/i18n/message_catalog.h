#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/i18n/locale.h"
#include "aurora/i18n/localized_string.h"

namespace aurora {

/// @brief 消息目录（i18n 运行时，specification/07-environment-modifier.md §6 国际化）。
///
/// 取代旧 `StringTable`：词条 Id 全局直排（`MessageId`），二进制词表一次载入常驻内存，
/// 各 locale 的查表视图在载入期沿回退链物化——`resolve` 纯 O(1)，locale 切换只换视图。
///
/// 数据通道：
/// - 批量：`load_binary`（构建期 `gen_i18n` 产出的 `AUM1` 词表，全语言合一）
/// - 稀疏：`add`（按规范名，供单测与内置词条登记；规范名须已由词表/内置词表登记）
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class MessageCatalog {
  public:
    /// @brief 构造空目录。
    MessageCatalog();
    /// @brief 析构（pimpl，实现见 src/aurora/i18n/message_catalog.cpp）。
    ~MessageCatalog();
    /// @brief 拷贝构造：词条与词表的深快照（AST 解析缓存随拷贝丢弃，按需重解析；进程级单例的
    /// 保存/还原守卫依赖本语义）。
    /// @param other 被拷贝的目录。
    MessageCatalog(const MessageCatalog &other);
    /// @brief 拷贝赋值：同拷贝构造口径。
    /// @param other 被拷贝的目录。
    /// @return 赋值后的自身引用。
    auto operator=(const MessageCatalog &other) -> MessageCatalog &;
    /// @brief 移动构造（转移内部实现，源目录回到空态）。
    MessageCatalog(MessageCatalog &&) noexcept;
    /// @brief 移动赋值（同上）。
    /// @return 赋值后的自身引用。
    auto operator=(MessageCatalog &&) noexcept -> MessageCatalog &;

    /// @brief 登记一枚词表的规范名区间（Id ↔ 规范名映射的来源之一）。
    ///
    /// Id 区间为 `[id_base, id_base + names.size())`；与**其他**已登记区间重叠、名字映射到
    /// 不同 Id、或触雷 `AURORA_INVALID_MESSAGE_ID_VALUE` 哨兵 → 结构化错误。同基址重载是
    /// 允许的（词表只增不减：既有名单须为新名单的前缀），据此支撑 `load_binary` 的
    /// 「同 locale 后到覆盖」。空名槽是保留区空洞占位（内置区与应用区并入同一区间时
    /// 的未分配 Id），不进名字索引、不可按名登记译文。载入的 `AUM1` 词表自带名字表
    /// （`load_binary` 内部走同一登记路径）。
    ///
    /// @param id_base 区间基址。
    /// @param names 规范名列表（下标 k ↔ Id id_base + k）。
    /// @return 成功返回 Ok；区间重叠 / 名字冲突 / 越界返回 Error。
    auto register_vocabulary(std::uint32_t id_base, std::vector<std::string> names) -> Result<void>;

    /// @brief 载入二进制词表（小端 `AUM1` v1，全语言合一；数据常驻，视图在载入期物化）。
    ///
    /// 同 locale 重复载入按后到覆盖并重建受影响视图；magic / version / 字节序 /
    /// 截断 / Id 越界 / 词表区间重叠均返回结构化错误（错误码见 errors.toml i18n 组）。
    ///
    /// @param bytes 词表字节（调用后目录持有自己的拷贝，span 生命周期无关）。
    /// @return 成功返回 Ok；各类结构化错误见上。
    auto load_binary(std::span<const std::byte> bytes) -> Result<void>;

    /// @brief 稀疏登记一条模板（按规范名查 Id；供单测与内置词条翻译）。
    ///
    /// 规范名未登记（不在任何词表名字表内）→ 结构化错误；同 locale 同 Id 重复
    /// 登记覆盖旧值并重建受影响视图。
    ///
    /// @param loc 区域设置（按其完整 tag 归档）。
    /// @param canonical_name 规范名（如 "command_palette.placeholder"）。
    /// @param tmpl 模板串（命名占位与选择器语法见 §6）。
    /// @return 成功返回 Ok；未知规范名返回 Error。
    auto add(const Locale &loc, std::string_view canonical_name, std::string tmpl) -> Result<void>;

    /// @brief 设置缺省区域（回退链末端）。
    /// @param loc 缺省区域；随后物化的视图以其为末端档。
    auto set_default_locale(const Locale &loc) -> void;

    /// @brief 解析一个本地化字符串（视图 O(1) 取模板 + 引擎格式化）。
    /// @param ls 待解析字符串：localize 为 false 或 Id 无效时直接返回其 text。
    /// @param loc 区域设置（决定取哪个物化视图与选择器判定）。
    /// @return 格式化后的显示串；查表失败回退 ls.text。
    [[nodiscard]] auto resolve(const LocalizedString &ls, const Locale &loc) const -> std::string;

    /// @brief 查词条规范名（序列化与诊断用）。
    /// @param id 词条 Id。
    /// @return 已登记词表的规范名；未登记（空目录 / 区间外）返回空 view。
    [[nodiscard]] auto canonical_name(MessageId id) const -> std::string_view;

    /// @brief 按规范名反查词条 Id（props JSON `$msg` 形态的解析入口）。
    /// @param canonical_name 规范名。
    /// @return 命中的 Id；未登记返回 `AURORA_INVALID_MESSAGE_ID`。
    [[nodiscard]] auto find_by_name(std::string_view canonical_name) const -> MessageId;

    /// @brief 缺译诊断：loc 无自有模板、而缺省区域（或任一其他已载 locale）有译文的 Id 集合。
    /// @param loc 待检区域。
    /// @return 按 Id 升序排列的缺译清单（载入期聚合 WARN 的明细查询口）。
    [[nodiscard]] auto missing_translations(const Locale &loc) const -> std::vector<MessageId>;

  private:
    struct Impl;  ///< 实现细节（存储 / 词表 / 视图物化），见 message_catalog.cpp
    std::unique_ptr<Impl> impl_;  ///< pimpl：实现下沉单 TU，减少全库重编
};

/// @brief 进程级默认消息目录（供 widget 渲染时就地查表）。
/// @return 函数内 static 目录的引用（首次调用惰性构造，生命周期至进程结束；内置词表在其中预登记）。
[[nodiscard]] auto default_catalog() -> MessageCatalog &;

inline auto LocalizedString::resolve(const MessageCatalog *catalog, const Locale &loc) const -> std::string {
    if (!localize || catalog == nullptr) {
        return text;
    }
    return catalog->resolve(*this, loc);
}

}  // namespace aurora
