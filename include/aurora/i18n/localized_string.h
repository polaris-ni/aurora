#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "aurora/i18n/format.h"
#include "aurora/i18n/locale.h"
#include "aurora/i18n/message_id.h"

namespace aurora {

class MessageCatalog;  // 前向声明：resolve 的定义在本头尾部（避免循环包含）

/// @brief 消息具名参数（i18n 运行时，specification/07-environment-modifier.md §6 国际化）。
///
/// 参数按名字匹配模板占位（`{name}`），与位置占位相比译文可自由调序而不静默错位。
/// 值是纯标量：需要嵌套本地化时先 `resolve` 再以文本传入（v2 起不再支持嵌套参数）。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct MessageArg {
    std::string name;  ///< 参数名（与模板占位名一致，ASCII snake_case 约定）
    std::variant<std::string, std::int64_t, double, CivilDate> value;  ///< 参数值（文本 / 整数 / 浮点 / 日期）

    /// @brief 文本参数。
    /// @param arg_name 参数名。
    /// @param v 文本值。
    MessageArg(std::string arg_name, std::string v) : name(std::move(arg_name)), value(std::move(v)) {}
    /// @brief 整数参数。
    /// @param arg_name 参数名。
    /// @param v 整数值。
    MessageArg(std::string arg_name, std::int64_t v) : name(std::move(arg_name)), value(v) {}
    /// @brief 浮点参数。
    /// @param arg_name 参数名。
    /// @param v 浮点值。
    MessageArg(std::string arg_name, double v) : name(std::move(arg_name)), value(v) {}
    /// @brief 日期参数（供 `{name, date}` 占位；复数选择器不接受日期值）。
    /// @param arg_name 参数名。
    /// @param v 日期值。
    MessageArg(std::string arg_name, CivilDate v) : name(std::move(arg_name)), value(v) {}
};

/// @brief 可本地化字符串（i18n 运行时，specification/07-environment-modifier.md §6 国际化）。
///
/// - 非本地化：直接持文本（`text`）
/// - 本地化：以 `id` 查 `MessageCatalog`，按 `Locale` 从当前回退视图取模板，并用具名 `args`
/// 经消息引擎格式化（命名占位 `{name}`、函数化 `{count, number}`、CLDR 选择器
/// `{count, plural, =0{…} one{…} other{…}}`，见 plural.h）
/// 提供从文本的隐式构造，便于 AI 直接写 `Text{ .content = "Hi" }`。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: yes, via from_json（props 序列化见 widget 层 $msg 形态）
struct LocalizedString {
    std::string text;  ///< 字面文本（非本地化值 / 本地化查表失败时的回退）
    MessageId id = AURORA_INVALID_MESSAGE_ID;  ///< 本地化词条 Id（localize 为 true 时使用）
    std::vector<MessageArg> args;  ///< 具名模板参数
    bool localize = false;  ///< 是否走查表解析

    LocalizedString() = default;
    /// @brief 由 C 串构造非本地化字面值。
    /// @param t 文本内容（拷入 text）。
    LocalizedString(const char *t) : text(t) {}  // NOLINT(google-explicit-constructor)：字面量直写的便利通道
    /// @brief 由 std::string 构造非本地化字面值。
    /// @param t 文本内容（移入 text）。
    LocalizedString(std::string t) : text(std::move(t)) {}  // NOLINT(google-explicit-constructor)：同上
    /// @brief 由 std::string_view 构造非本地化字面值。
    /// @param t 文本内容（拷贝为 std::string 存入 text）。
    LocalizedString(std::string_view t) : text(t) {}  // NOLINT(google-explicit-constructor)：同上

    /// @brief 构造一个待本地化的字符串（按 Id 查目录 + 具名参数格式化）。
    /// @param msg_id 词表内词条 Id（生成头产出的 `constexpr MessageId` 常量）。
    /// @param a 具名参数列表，缺省为空。
    /// @return localize 为 true、text 为空的实例（text 仅作查表失败时的回退）。
    static auto tr(MessageId msg_id, std::vector<MessageArg> a = {}) -> LocalizedString {
        LocalizedString s;
        s.id = msg_id;
        s.args = std::move(a);
        s.localize = true;
        return s;
    }

    /// @brief 取字面文本的 C 串指针。
    /// @return text.c_str()；注意不是 resolve 之后的显示文本。
    [[nodiscard]] auto c_str() const -> const char * { return text.c_str(); }

    /// @brief 相等性比较：比较 text 与 id 两字段（不含 args 与 localize，与旧版口径一致）。
    /// @param o 待比较的另一实例。
    /// @return text 与 id 均相等时为 true。
    auto operator==(const LocalizedString &o) const -> bool { return text == o.text && id == o.id; }

    /// @brief 解析为最终显示字符串：localize 且目录命中 → 引擎格式化；否则回退 `text`。
    ///
    /// @param catalog 消息目录；nullptr 时直接回退 text（定义见本头尾部）。
    /// @param loc 查表与选择器判定使用的区域设置。
    /// @return 格式化后的显示字符串。
    [[nodiscard]] auto resolve(const MessageCatalog *catalog, const Locale &loc) const -> std::string;
};

}  // namespace aurora
