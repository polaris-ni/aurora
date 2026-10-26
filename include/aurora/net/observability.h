#pragma once

/// @brief 凭据分级与进程级导出策略：库裁定敏感度，宿主只能在放行与收窄之间选择。
/// @file net/observability.h
///
/// 本头回答一个问题：**「一段可能含凭据的文本，允许以什么形态落到哪里」**。它与 HTTP 无关，
/// 只按「数据将落到哪个导出面」与「字段属于哪一档灵敏度」裁决；网络活动、事件总线与 Inspector
/// 导出都消费这里的口径，故四类出口不会各说各话。
///
/// 三档灵敏度由库裁定，宿主只能收窄、不能放大：
/// - `Public`：方法、主机、状态码、字节数、时刻、`request_id` 与路径本身——给出原值，宿主仍可在
///   `present()` 里再收窄（例如把用户放进路径段的令牌打码）。
/// - `Sensitive`：其余头部、非令牌查询参数、树属性值——缺省折叠为 `"***"`，按
///   `(导出面 + 键)` 经 `ExportPolicy::allows()` 显式放行后才把原值交给 `present()`。
/// - `Secret`：认证 / 代理认证 / Cookie / 名字含 `token`、`secret`、`password`、`api-key` 的头部，
///   URL 用户信息段，令牌查询参数，以及消息体——**从不询问 `allows()`**，`present()` 拿到的恒是
///   折叠形态；宿主实现即便原样返回也放不出去。消息体不是折叠，是根本不进入导出。
///
/// 策略是两段式的，因为单段 `present()` 对 `Sensitive` 不可实现：值一旦被折叠，宿主再没有原值
/// 可放行。`allows()` 先答「这一类能不能给」，`present()` 再答「给成什么样」。两段都按调用时刻
/// 取进程级策略，不构成跨整份导出的事务——需要整份一致时先安装好策略再导出。
///
/// 无策略时：`Public` 原样、`Sensitive` 与 `Secret` 折叠。`present()` 抛出时回退到该字段的折叠
/// 形态，不中断调用方的导出，也不把异常带进网络路径。
///
/// @code
/// class AuditPolicy final : public aurora::net::ExportPolicy {
///   public:
///     [[nodiscard]] auto allows(aurora::net::ExportSurface surface, std::string_view key,
///                               aurora::net::FieldClass) const -> bool override {
///         return surface == aurora::net::ExportSurface::InspectorJson && key == "tree.title";
///     }
///     [[nodiscard]] auto present(aurora::net::ExportSurface, std::string_view key, aurora::net::FieldClass,
///                                std::string_view value) const -> std::string override {
///         return key == "path" ? "<hidden>" : std::string{value};
///     }
/// };
/// aurora::net::install_export_policy(std::make_shared<AuditPolicy>());
/// @endcode
/// @note Thread: thread-safe（进程级策略的读写内部加锁；策略实现自身须可被并发调用）
/// @note Side-effects: none
/// @note Rebuildable: no

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace aurora::net {

/// @brief 导出面：策略按「数据将落到哪里」区分，而不是按调用点区分。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class ExportSurface : std::uint8_t {
    Log,  ///< 库的日志行
    Timeline,  ///< 程序化查询与事件消费者（缺省面）
    InspectorJson,  ///< Inspector 端点（MCP 经 Inspector 代理，继承本面）
    ErrorDetail,  ///< 错误 message 中的补充事实
};

/// @brief 字段灵敏度：由库裁决；宿主只能收窄，不能放大。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class FieldClass : std::uint8_t {
    Public,  ///< 本就可给：方法、主机、状态、字节、时刻、路径与请求标识
    Sensitive,  ///< 缺省折叠，按 `(导出面 + 键)` 可放行：其余头部、非令牌查询值、树属性
    Secret,  ///< 恒折叠、不可放行：凭据、Cookie、用户信息、令牌查询值、消息体
};

/// @brief 宿主侧的导出策略：两段式，先问「能不能给」，再问「给成什么样」。
/// @note Thread: 可在任意发起线程与主线程被调用；实现须线程安全、不得持外部锁。
/// @note Side-effects: none；present() 内不得做 I/O；抛出时库回退到折叠形态。
class ExportPolicy {
  public:
    ExportPolicy() = default;
    ExportPolicy(const ExportPolicy &) = delete;
    auto operator=(const ExportPolicy &) -> ExportPolicy & = delete;
    ExportPolicy(ExportPolicy &&) = delete;
    auto operator=(ExportPolicy &&) -> ExportPolicy & = delete;
    virtual ~ExportPolicy() = default;

    /// @brief 仅对 Sensitive 询问；Secret 恒不问（无权放行），Public 恒不问（本就可给）。
    /// @param surface 本次导出面。
    /// @param key 键词汇（`header.<名>`、`query.<名>`、`detail.<字段>`、`tree.<属性键>`、`path` 等）。
    /// @param field_class 恒为 Sensitive。
    /// @return 允许把原值交给 present() 则为 true；缺省拒绝。
    [[nodiscard]] virtual auto allows(ExportSurface surface, std::string_view key, FieldClass field_class) const
        -> bool {
        (void)surface;
        (void)key;
        (void)field_class;
        return false;
    }

    /// @brief 把库愿意给出的值呈现为最终文本；Secret 收到的恒是折叠形态。
    /// @param surface 本次导出面。
    /// @param key 键词汇。
    /// @param field_class 该字段的分级（库裁定后的最终档）。
    /// @param value 已按分级决定的值（Secret 恒折叠、Sensitive 未放行恒为 `"***"`、Public 为原值）。
    /// @return 最终落出去的文本；库不对其再做展开。
    [[nodiscard]] virtual auto present(ExportSurface surface, std::string_view key, FieldClass field_class,
                                       std::string_view value) const -> std::string = 0;
};

/// @brief 安装进程级策略；传空即清回「未设」。
/// @param policy 策略的共享持有；`nullptr` = 清除。
/// @note Thread: thread-safe。
/// @note Side-effects: 替换此后新发起的呈现取用；已在途的导出不撤回。
auto install_export_policy(std::shared_ptr<const ExportPolicy> policy) -> void;

/// @brief 取当前策略的共享持有；未设时为空。
/// @return 当前策略；未安装为 `nullptr`。
[[nodiscard]] auto export_policy() -> std::shared_ptr<const ExportPolicy>;

/// @brief 呈现单个字段：Public 原样、Sensitive 未放行折叠、Secret 恒折叠，再交给 present()。
/// @param surface 导出面。
/// @param key 键词汇；库按名字识别凭据类键并升级其分级（声明为 Sensitive 也不会被放大）。
/// @param field_class 调用方声明的分级；库只上调、不下调。
/// @param value 原始值（未折叠）。
/// @return 最终文本；无策略时 Public 为原值、其余为折叠形态；present() 抛出时回退到折叠形态。
[[nodiscard]] auto present_field(ExportSurface surface, std::string_view key, FieldClass field_class,
                                 std::string_view value) -> std::string;

/// @brief 呈现绝对或 origin-form URL：去 userinfo、折叠令牌查询参数；解析失败只回占位符。
/// @param surface 导出面。
/// @param url 绝对 URL（`scheme://userinfo@host/path?query`）或 origin-form（`/path?query`）。
/// @return 去掉用户信息段并逐参数折叠后的 URL；解析失败回 `"<unparsable-url>"`，绝不回显输入。
[[nodiscard]] auto present_url(ExportSurface surface, std::string_view url) -> std::string;

}  // namespace aurora::net
